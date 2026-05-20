#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/consumer/causal_reorder_buffer.hpp"
#include "coderoast/ipc/consumer/frame_emitter.hpp"
#include "coderoast/ipc/consumer/shm_transport_drainer.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

/// Thin facade composing the three-step pull-based causal SHM consumer:
///
///   * Step 1 — `ShmTransportDrainer`  (per-shard background drain,
///                                      never stalls on ordering)
///   * Step 2 — `CausalReorderBuffer`  (global CausalKey min-heap)
///   * Step 3 — `FrameEmitter`         (control-frame filter + diagnostics)
///
/// The facade owns the three sub-objects by value, in the right order,
/// so that destruction joins drain threads before the reorder buffer
/// goes away.
///
/// All callers that only need "give me the next causally-earliest data
/// frame from this sharded SHM channel" should use this facade and
/// never instantiate the sub-objects directly. Advanced callers (e.g. a
/// test that injects a fault between stages) may compose the three
/// classes themselves.
template <typename Frame = coderoast::ipc::DefaultLineFrame>
class CausalShmConsumer
{
  public:
    using Drainer = ShmTransportDrainer<Frame>;
    using Buffer = CausalReorderBuffer<Frame>;
    using Emitter = FrameEmitter<Frame>;
    using ShardSummary = typename Buffer::ShardSummary;

    struct Config
    {
        std::string channel{"coderoast.default"};
        std::size_t shard_count{1};
        coderoast::ipc::BackpressurePolicy backpressure{coderoast::ipc::BackpressurePolicy::Block};
        coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::SpinYieldPark};
        bool emit_control_frames{false};
    };

    explicit CausalShmConsumer(Config config)
        : drainer_{typename Drainer::Config{
              .channel = std::move(config.channel),
              .shard_count = config.shard_count,
              .backpressure = config.backpressure,
              .wait_strategy = config.wait_strategy,
          }},
          buffer_{drainer_},
          emitter_{buffer_,
                   typename Emitter::Config{.emit_control_frames = config.emit_control_frames}}
    {
    }

    // Non-copyable, non-movable: buffer_ holds a raw pointer to drainer_,
    // and emitter_ holds a raw pointer to buffer_.
    CausalShmConsumer(const CausalShmConsumer&) = delete;
    CausalShmConsumer& operator=(const CausalShmConsumer&) = delete;
    CausalShmConsumer(CausalShmConsumer&&) = delete;
    CausalShmConsumer& operator=(CausalShmConsumer&&) = delete;
    ~CausalShmConsumer() = default;

    [[nodiscard]] bool try_next(Frame& out)
    {
        return emitter_.try_next(out);
    }

    /// EOS-based completion predicate. True only after every shard has
    /// observed EOS and the reorder buffer is empty.
    [[nodiscard]] bool all_shards_done() const noexcept
    {
        return buffer_.drained();
    }

    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return drainer_.shard_count();
    }

    [[nodiscard]] std::vector<coderoast::ipc::ChannelStats> channel_stats() const
    {
        return drainer_.channel_stats();
    }

    [[nodiscard]] std::vector<ShardSummary> shard_summaries() const
    {
        return buffer_.shard_summaries();
    }

    [[nodiscard]] std::uint64_t emitted() const noexcept
    {
        return emitter_.emitted();
    }

    [[nodiscard]] std::uint64_t last_sequence() const noexcept
    {
        return emitter_.last_sequence();
    }

    void close() noexcept
    {
        drainer_.close();
    }

    // Sub-component accessors for advanced diagnostics / tests.
    [[nodiscard]] Drainer& drainer() noexcept
    {
        return drainer_;
    }
    [[nodiscard]] Buffer& buffer() noexcept
    {
        return buffer_;
    }
    [[nodiscard]] Emitter& emitter() noexcept
    {
        return emitter_;
    }

  private:
    Drainer drainer_;
    Buffer buffer_;
    Emitter emitter_;
};

} // namespace coderoast::ipc::consumer
