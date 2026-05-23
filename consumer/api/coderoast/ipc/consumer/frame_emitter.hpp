#pragma once

#include <cstdint>
#include <utility>

#include "coderoast/ipc/consumer/causal_reorder_buffer.hpp"
#include "coderoast/ipc/consumer/consumer_metrics.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

/// **Step 3 of the pull-based causal SHM consumer pipeline.**
///
/// Drives the reorder buffer and surfaces emit-ready frames to the caller.
/// Optionally filters out IPC control frames (window seals; EOS frames
/// are already absorbed by the drainer) so downstream code only sees
/// payload frames.
///
/// ─────────────────────────── Production contracts ───────────────────────────
///
/// **Threading model**
///   * Single owner thread (the caller of `try_next`).
///   * Holds no internal threads, no mutex.
///
/// **Backpressure**
///   * Pure pull. Never blocks. `try_next` returns false when the buffer
///     cannot select a frame.
///
/// **Determinism**
///   * Output order is identical to the reorder buffer's output minus
///     filtered control frames. Filter decision is purely a function of
///     `frame.header.flags` — deterministic.
///
/// **Frame lifetime / move semantics**
///   * Frames are moved out of the reorder buffer's heap into a local
///     `candidate`, then `std::move`d into the caller's `out`. Two moves
///     total per emitted frame, no copies.
///
/// **Error handling**
///   * No exceptions. Counters are best-effort relaxed atomics for fast
///     observability.
///
/// **Allocation**
///   * Zero allocations on the hot path.
///
/// Keeps emission diagnostics (`emitted()`, `control_dropped()`,
/// `last_sequence()`) here, away from the ordering logic, so step 2 stays
/// purely about correctness.
template <typename Frame = coderoast::ipc::DefaultLineFrame> class FrameEmitter
{
  public:
    struct Config
    {
        /// When false (default) `try_next` silently skips window-seal frames.
        bool emit_control_frames{false};
    };

    explicit FrameEmitter(CausalReorderBuffer<Frame>& buffer, Config config = {})
        : buffer_{&buffer}, config_{config}
    {
    }

    FrameEmitter(const FrameEmitter&) = delete;
    FrameEmitter& operator=(const FrameEmitter&) = delete;
    FrameEmitter(FrameEmitter&&) = delete;
    FrameEmitter& operator=(FrameEmitter&&) = delete;
    ~FrameEmitter() = default;

    /// Pop the next emit-ready frame in causal order. Returns false when
    /// no frame is currently emit-ready (frontier incomplete, or fully
    /// drained).
    [[nodiscard]] bool try_next(Frame& out)
    {
        Frame candidate{};
        while (buffer_->try_select(candidate))
        {
            if (!config_.emit_control_frames &&
                coderoast::ipc::is_control_frame(candidate.header.flags))
            {
                ++control_dropped_;
                continue;
            }
            ++emitted_;
            last_sequence_ = candidate.header.sequence;
            out = std::move(candidate);
            return true;
        }
        return false;
    }

    [[nodiscard]] std::uint64_t emitted() const noexcept
    {
        return emitted_;
    }

    [[nodiscard]] std::uint64_t control_dropped() const noexcept
    {
        return control_dropped_;
    }

    [[nodiscard]] std::uint64_t last_sequence() const noexcept
    {
        return last_sequence_;
    }

    [[nodiscard]] EmitterMetrics metrics() const noexcept
    {
        return EmitterMetrics{
            .emitted = emitted_,
            .control_dropped = control_dropped_,
            .last_sequence = last_sequence_,
        };
    }

  private:
    CausalReorderBuffer<Frame>* buffer_{nullptr};
    Config config_{};
    std::uint64_t emitted_{0};
    std::uint64_t control_dropped_{0};
    std::uint64_t last_sequence_{0};
};

} // namespace coderoast::ipc::consumer
