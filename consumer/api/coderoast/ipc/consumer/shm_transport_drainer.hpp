#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/consumer/consumer_metrics.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

/// **Step 1 of the pull-based causal SHM consumer pipeline.**
///
/// Owns one `SharedMemorySpscChannel` per shard. Holds **no background
/// threads, no mutex, no internal queue.** Each call to `try_pull` is a
/// direct, lock-free pop on the per-shard SPSC ring; the result is moved
/// straight into the caller-provided `Frame`. EOS frames are absorbed
/// (the per-shard `eos` flag is set) so callers never observe them.
///
/// ─────────────────────────── Production contracts ───────────────────────────
///
/// **Threading model**
///   * Single consumer thread per shard (the design also supports a
///     single consumer thread for all shards, which is the tested case).
///   * No threads are spawned by this class. It never blocks.
///   * Producers (one per shard, separate processes) push concurrently
///     into the SPSC rings; that path is `coderoast::ipc::channel`'s
///     responsibility, not ours.
///
/// **Backpressure semantics**
///   * SHM full → producer side handles per `BackpressurePolicy`
///     (Block/DropOldest/DropNewest). The drainer never sees SHM-full;
///     it only sees "ring empty" via `try_pop` returning false.
///   * Consumer slow → ring fills up; producer either spins (Block) or
///     drops frames (Drop*). Either way, the drainer's `try_pull` keeps
///     returning whatever is in the ring at its own pace.
///
/// **Determinism contract**
///   * Per-shard order is preserved (SPSC FIFO).
///   * Cross-shard order is NOT preserved here — that is step 2's job.
///   * Output is deterministic given identical SHM contents and identical
///     `try_pull` interleavings. There is no randomness, no time-based
///     decision, no allocation that could fail differently between runs.
///
/// **Frame lifetime / move semantics**
///   * `try_pop` moves the frame out of the SHM ring slot into the
///     caller-provided `Frame&`. Zero-copy aside from the in-slot move
///     (DefaultLineFrame is trivially relocatable; the payload bytes are
///     copied because they live in the slot).
///   * The caller owns the frame after a successful `try_pull`. The
///     drainer keeps no reference to it.
///
/// **Error handling strategy**
///   * Construction-time invariants (`shard_count == 0`, channel open
///     failure) fail fast via exceptions.
///   * Run-time errors are absent by construction: `try_pop` returning
///     false is "no data" and does not propagate.
///   * Poison frames (corrupted header) are NOT detected here; downstream
///     stages MAY detect them via flag inspection.
///
/// **Allocation strategy**
///   * No allocations on the hot path. `try_pull` is allocation-free.
///   * One-time allocations in the constructor only: channel vector,
///     shard state vector, per-shard atomic block.
///
/// **Observability**
///   * `metrics()` returns a `DrainerMetrics` snapshot with counters
///     updated by `memory_order_relaxed` writes — cost is one atomic
///     increment per `try_pull` regardless of outcome.
///   * `set_observer()` registers a callback invoked on discrete
///     lifecycle events only (EOS observed); never on the per-frame
///     hot path. Default = no observer = exactly one branch per event.
///
/// **Lifecycle / shutdown**
///   * Construction opens every shard channel. Failure throws and rolls
///     back (RAII destructors close already-opened channels).
///   * `close()` is idempotent and may be called before destruction to
///     release SHM handles explicitly.
///   * Destruction calls `close()` automatically.
///
/// **Strict invariants (do NOT relax):**
///   * `try_pull` never blocks. It returns `false` when the shard's SHM
///     ring is empty *or* the shard has already observed EOS.
///   * EOS frames are NOT returned to the caller. They flip the per-shard
///     `eos` flag and `try_pull` returns `false`.
///   * Window-seal frames ARE returned (they carry watermark information
///     downstream stages may want) but they do not set `ever_had_data`.
///   * No causal-ordering / watermark logic lives here — that is step 2's
///     responsibility. This stage only knows about SHM transport state.
template <typename Frame = coderoast::ipc::DefaultLineFrame> class ShmTransportDrainer
{
  public:
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

    struct Config
    {
        std::string channel{"coderoast.default"};
        std::size_t shard_count{1};
        coderoast::ipc::BackpressurePolicy backpressure{coderoast::ipc::BackpressurePolicy::Block};
        coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::SpinYieldPark};
    };

    explicit ShmTransportDrainer(Config config) : config_{std::move(config)}
    {
        if (config_.shard_count == 0U)
        {
            throw std::invalid_argument(
                "ShmTransportDrainer shard_count must be greater than zero");
        }
        channels_.reserve(config_.shard_count);
        shards_.reserve(config_.shard_count);
        for (std::size_t shard_id{0}; shard_id < config_.shard_count; ++shard_id)
        {
            channels_.emplace_back(Channel::open(shard_channel_name(config_.channel, shard_id),
                                                 config_.backpressure, config_.wait_strategy));
            shards_.emplace_back(std::make_unique<Shard>());
        }
    }

    ShmTransportDrainer(const ShmTransportDrainer&) = delete;
    ShmTransportDrainer& operator=(const ShmTransportDrainer&) = delete;
    ShmTransportDrainer(ShmTransportDrainer&&) = delete;
    ShmTransportDrainer& operator=(ShmTransportDrainer&&) = delete;

    ~ShmTransportDrainer()
    {
        close();
    }

    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return shards_.size();
    }

    /// Non-blocking pop from shard `shard_id`'s SPSC SHM ring. Returns
    /// `false` immediately if the ring is empty, the shard has already
    /// observed EOS, or the popped frame WAS the EOS marker (in which
    /// case the shard's `eos` flag is set as a side-effect).
    [[nodiscard]] bool try_pull(std::size_t shard_id, Frame& out)
    {
        auto& shard{*shards_[shard_id]};
        pulls_attempted_.fetch_add(1U, std::memory_order_relaxed);
        if (shard.eos.load(std::memory_order_acquire))
        {
            return false;
        }
        if (!channels_[shard_id].try_pop(out))
        {
            return false;
        }
        const bool is_eos{coderoast::ipc::has_flag(
            out.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream)};
        if (is_eos)
        {
            shard.eos.store(true, std::memory_order_release);
            eos_observed_.fetch_add(1U, std::memory_order_relaxed);
            notify(ConsumerEvent::kShardEos, shard_id);
            return false;
        }
        const bool is_seal{coderoast::ipc::has_flag(
            out.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagWindowSeal)};
        if (is_seal)
        {
            seals_observed_.fetch_add(1U, std::memory_order_relaxed);
        }
        else
        {
            shard.ever_had_data.store(true, std::memory_order_release);
        }
        pulls_succeeded_.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }

    [[nodiscard]] bool shard_eos(std::size_t shard_id) const noexcept
    {
        return shards_[shard_id]->eos.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool shard_ever_had_data(std::size_t shard_id) const noexcept
    {
        return shards_[shard_id]->ever_had_data.load(std::memory_order_acquire);
    }

    /// True once every shard has observed EOS. The reorder buffer must
    /// still drain its own per-shard min-heaps after this returns true.
    [[nodiscard]] bool transport_complete() const noexcept
    {
        for (const auto& shard : shards_)
        {
            if (!shard->eos.load(std::memory_order_acquire))
            {
                return false;
            }
        }
        return !shards_.empty();
    }

    [[nodiscard]] std::vector<coderoast::ipc::ChannelStats> channel_stats() const
    {
        std::vector<coderoast::ipc::ChannelStats> out;
        out.reserve(channels_.size());
        for (const auto& chan : channels_)
        {
            out.push_back(chan.stats());
        }
        return out;
    }

    /// Snapshot of the hot-path counters. Internally inconsistent under
    /// heavy concurrency by design (relaxed atomics, no global lock).
    [[nodiscard]] DrainerMetrics metrics() const noexcept
    {
        return DrainerMetrics{
            .pulls_attempted = pulls_attempted_.load(std::memory_order_acquire),
            .pulls_succeeded = pulls_succeeded_.load(std::memory_order_acquire),
            .eos_observed = eos_observed_.load(std::memory_order_acquire),
            .seals_observed = seals_observed_.load(std::memory_order_acquire),
        };
    }

    /// Register a discrete-event observer (EOS transitions). Replace
    /// with an empty function to detach. Thread-safety: callers must not
    /// race this with `try_pull`; intended for construction-time wiring.
    void set_observer(ConsumerObserver observer)
    {
        observer_ = std::move(observer);
    }

    /// Close every channel. Idempotent.
    void close() noexcept
    {
        for (auto& chan : channels_)
        {
            chan.close();
        }
        channels_.clear();
    }

    [[nodiscard]] static std::string shard_channel_name(std::string_view base, std::size_t shard_id)
    {
        return std::string{base} + "_shard_" + std::to_string(shard_id);
    }

  private:
    void notify(ConsumerEvent event, std::size_t shard_id)
    {
        if (observer_)
        {
            observer_(ConsumerEventPayload{.event = event, .shard_id = shard_id});
        }
    }

    struct Shard
    {
        std::atomic<bool> eos{false};
        std::atomic<bool> ever_had_data{false};
    };

    Config config_{};
    std::vector<Channel> channels_;
    std::vector<std::unique_ptr<Shard>> shards_;
    ConsumerObserver observer_{};

    std::atomic<std::uint64_t> pulls_attempted_{0};
    std::atomic<std::uint64_t> pulls_succeeded_{0};
    std::atomic<std::uint64_t> eos_observed_{0};
    std::atomic<std::uint64_t> seals_observed_{0};
};

} // namespace coderoast::ipc::consumer
