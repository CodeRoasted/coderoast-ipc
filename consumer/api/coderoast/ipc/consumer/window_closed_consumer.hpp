#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>

#include "coderoast/ipc/consumer/causal_shm_consumer.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

/// **Opt-in WindowClosed coalescing adapter over `CausalShmConsumer`.**
///
/// Background
/// ----------
/// The sharded SHM transport emits N independent `WindowSeal` frames per
/// closed window (one per shard). Users who only care about "is this window
/// finished" must collect all N seals before acting — the per-shard frames
/// also arrive in non-deterministic order (each shard's seal travels through
/// its own SHM ring and the merge step is pull-based; see Gate 3 in
/// `logcraft/core/tests/determinism/test_determinism_shm_gate.cpp` for the
/// detailed rationale).
///
/// This adapter solves both problems with a single coalescing layer:
///
///   * Per-shard `WindowSeal` frames are **swallowed** — they never reach
///     the caller's `Frame` channel.
///   * After the Nth shard's seal is observed for a given `window_id`, the
///     adapter emits **one synthesized `WindowClosed{window_id, logical_tick}`
///     event** in their place.
///
/// Users who want raw per-shard seals (diagnostics, determinism gates,
/// custom barrier logic) should keep using `CausalShmConsumer` directly with
/// `emit_control_frames=true`.
///
/// Pairing with InSight's MetaLog window
/// -------------------------------------
/// To get exactly one barrier event per MetaLog close, configure the
/// scenario output's `shm_window_seal_interval_seconds` to equal the
/// MetaLog window duration (default 25 s on the server, see
/// `default_insight_pipeline_config()` and `pyramid.window_ns`). The
/// adapter will then emit one `WindowClosed` per 25 s wall window, in
/// lock-step with the engine's window close. Mismatched cadences are
/// permitted — the adapter simply produces one event per "all-shards-sealed"
/// signal regardless of how that maps to upstream windows.
///
/// Ordering & determinism contract
/// -------------------------------
/// * `WindowClosed` events appear in increasing `window_id` order. The
///   Nth-seal-of-window-K event is emitted strictly after the (N-1)th
///   seal of K is observed.
/// * Any data frame returned **before** a `WindowClosed{K}` event has
///   `logical_tick < seal_tick(K)` — the underlying causal merge upholds
///   this invariant.
/// * Two replays of the same scenario emit a byte-identical sequence of
///   `WindowClosed` events (1 per window, with deterministic `(window_id,
///   logical_tick)`). Data-frame order between events still depends on
///   inter-shard SHM scheduling and remains a deterministic multiset.
///
/// Single-thread, allocation-free hot path
/// ---------------------------------------
/// Owned and called by a single thread (the caller of `try_next`).
/// Internal seal-counter map grows at most `concurrent_in_flight_windows`
/// entries — for the standard 1-window-at-a-time flow this is a single
/// bucket the unordered_map allocates lazily. Successful frame emission
/// performs zero allocations.
template <typename Frame = coderoast::ipc::DefaultLineFrame> class WindowClosedConsumer
{
  public:
    using Underlying = CausalShmConsumer<Frame>;

    struct WindowClosed
    {
        std::uint64_t window_id{0};
        std::uint64_t logical_tick{0};
    };

    enum class NextKind : std::uint8_t
    {
        kNone,         ///< Nothing emittable yet — caller should retry / yield.
        kFrame,        ///< `out_frame` is populated (data frame, non-control).
        kWindowClosed, ///< `out_window` is populated; all shards sealed this window.
    };

    struct Config
    {
        typename Underlying::Config underlying{};
    };

    explicit WindowClosedConsumer(Config config)
        : shard_count_{config.underlying.shard_count},
          underlying_{[&config]
                      {
                          // Force the adapter to
                          // observe seals; data-frame
                          // emission stays normal.
                          auto cfg{std::move(config.underlying)};
                          cfg.emit_control_frames = true;
                          return cfg;
                      }()}
    {
    }

    WindowClosedConsumer(const WindowClosedConsumer&) = delete;
    WindowClosedConsumer& operator=(const WindowClosedConsumer&) = delete;
    WindowClosedConsumer(WindowClosedConsumer&&) = delete;
    WindowClosedConsumer& operator=(WindowClosedConsumer&&) = delete;
    ~WindowClosedConsumer() = default;

    /// Try to emit either a data frame or a synthesized `WindowClosed` event.
    ///
    /// Returns `NextKind::kNone` when no frame is ready (caller should yield
    /// or poll again). Returns `kFrame` with `out_frame` populated for data
    /// frames. Returns `kWindowClosed` with `out_window` populated when the
    /// Nth shard's seal has been observed for a window.
    ///
    /// At most one event is produced per call — multiple sealing seals in
    /// the same call still yield exactly one `WindowClosed` per window per
    /// call; the next window's event is emitted on the next call.
    [[nodiscard]] NextKind try_next(Frame& out_frame, WindowClosed& out_window)
    {
        // Drain seals until either a data frame is found or a window completes.
        Frame scratch{};
        for (;;)
        {
            if (!underlying_.try_next(scratch))
            {
                return NextKind::kNone;
            }
            const bool is_seal{coderoast::ipc::has_flag(
                scratch.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagWindowSeal)};
            if (!is_seal)
            {
                out_frame = std::move(scratch);
                ++frames_emitted_;
                return NextKind::kFrame;
            }
            const auto window_id{scratch.header.window_id};
            const auto logical_tick{scratch.header.logical_tick};
            auto [it, inserted] = seal_counts_.try_emplace(window_id, std::size_t{0});
            ++(it->second);
            ++seals_observed_;
            if (it->second >= shard_count_)
            {
                // Final shard has sealed — synthesize the WindowClosed event.
                seal_counts_.erase(it);
                ++windows_closed_;
                out_window = WindowClosed{.window_id = window_id, .logical_tick = logical_tick};
                return NextKind::kWindowClosed;
            }
            // Otherwise loop — swallow the seal and look for the next event.
        }
    }

    [[nodiscard]] bool all_shards_done() const noexcept
    {
        return underlying_.all_shards_done();
    }

    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return shard_count_;
    }

    [[nodiscard]] std::uint64_t frames_emitted() const noexcept
    {
        return frames_emitted_;
    }

    [[nodiscard]] std::uint64_t windows_closed() const noexcept
    {
        return windows_closed_;
    }

    [[nodiscard]] std::uint64_t seals_observed() const noexcept
    {
        return seals_observed_;
    }

    [[nodiscard]] Underlying& underlying() noexcept
    {
        return underlying_;
    }

    void close() noexcept
    {
        underlying_.close();
    }

  private:
    std::size_t shard_count_{1};
    Underlying underlying_;
    std::unordered_map<std::uint64_t, std::size_t> seal_counts_;
    std::uint64_t frames_emitted_{0};
    std::uint64_t windows_closed_{0};
    std::uint64_t seals_observed_{0};
};

} // namespace coderoast::ipc::consumer
