#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

enum class SequenceGapPolicy : std::uint8_t
{
    WaitForMissing,
    SkipMissing,
};

enum class FrameOrdering : std::uint8_t
{
    TransportSequence,
    CausalKey,
};

struct CausalKey
{
    std::uint64_t logical_tick{0};
    std::uint32_t agent_order{0};
    std::uint32_t intra_agent_index{0};
};

template <typename Frame = coderoast::ipc::DefaultLineFrame> struct OrderedLineFrameIteratorConfig
{
    std::string channel{"coderoast.default"};
    std::size_t shard_count{1};
    std::uint64_t first_sequence{1};
    SequenceGapPolicy gap_policy{SequenceGapPolicy::WaitForMissing};
    FrameOrdering ordering{FrameOrdering::CausalKey};
    coderoast::ipc::BackpressurePolicy backpressure{coderoast::ipc::BackpressurePolicy::Block};
    coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::SpinYieldPark};
};

template <typename Frame = coderoast::ipc::DefaultLineFrame> class OrderedLineFrameIterator
{
  public:
    using Config = OrderedLineFrameIteratorConfig<Frame>;
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

    OrderedLineFrameIterator() = default;

    explicit OrderedLineFrameIterator(Config config) : config_{std::move(config)}
    {
        if (config_.shard_count == 0U)
        {
            throw std::invalid_argument(
                "OrderedLineFrameIterator shard_count must be greater than zero");
        }
        channels_.reserve(config_.shard_count);
        transport_eos_.resize(config_.shard_count);
        causal_state_.resize(config_.shard_count);
        for (std::size_t shard_id{0}; shard_id < config_.shard_count; ++shard_id)
        {
            channels_.emplace_back(Channel::open(shard_channel_name(config_.channel, shard_id),
                                                 config_.backpressure, config_.wait_strategy));
        }
        next_sequence_ = config_.first_sequence;
    }

    OrderedLineFrameIterator(const OrderedLineFrameIterator&) = delete;
    OrderedLineFrameIterator& operator=(const OrderedLineFrameIterator&) = delete;
    OrderedLineFrameIterator(OrderedLineFrameIterator&&) noexcept = default;
    OrderedLineFrameIterator& operator=(OrderedLineFrameIterator&&) noexcept = default;
    ~OrderedLineFrameIterator() = default;

    [[nodiscard]] bool try_next(Frame& out)
    {
        if (config_.ordering == FrameOrdering::CausalKey)
        {
            return try_next_causal(out);
        }

        return try_next_transport(out);
    }

    [[nodiscard]] std::uint64_t next_sequence() const noexcept
    {
        return next_sequence_;
    }

    [[nodiscard]] std::uint64_t skipped_sequences() const noexcept
    {
        return skipped_sequences_;
    }

    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return channels_.size();
    }

    // Per-shard SHM ring stats (push/pop sequence numbers, blocked events, …).
    [[nodiscard]] std::vector<coderoast::ipc::ChannelStats> channel_stats() const
    {
        std::vector<coderoast::ipc::ChannelStats> result;
        result.reserve(channels_.size());
        for (const auto& chan : channels_)
        {
            result.push_back(chan.stats());
        }
        return result;
    }

    struct ShardSummary
    {
        bool ever_had_data{false};
        bool eos{false};
        std::size_t buf_size{0};
    };

    // Per-shard state for diagnostics.
    [[nodiscard]] std::vector<ShardSummary> shard_summaries() const
    {
        std::vector<ShardSummary> out;
        out.reserve(channels_.size());
        for (std::size_t index{0}; index < channels_.size(); ++index)
        {
            if (config_.ordering == FrameOrdering::CausalKey)
            {
                const auto& state{causal_state_[index]};
                out.push_back({state.ever_had_data, state.eos, state.buffer.size()});
            }
            else
            {
                out.push_back({false, transport_eos_[index], transport_buffer_.size()});
            }
        }
        return out;
    }

    // Returns true once every shard has both received its EOS frame AND had
    // every buffered frame emitted by try_next().  When this returns true the
    // consumer has seen every frame the producer will ever send; the loop can
    // exit without a timing-dependent idle-timeout.
    //
    // In CausalKey mode EOS frames are consumed by the causal drain. In
    // TransportSequence mode EOS frames are kept in the transport queue and
    // returned by try_next(), so completion waits for both EOS observation and
    // an empty transport queue.
    [[nodiscard]] bool all_shards_done() const noexcept
    {
        if (config_.ordering == FrameOrdering::TransportSequence)
        {
            if (transport_buffer_.empty())
            {
                for (const bool eos : transport_eos_)
                {
                    if (!eos)
                    {
                        return false;
                    }
                }
                return !transport_eos_.empty();
            }
            return false;
        }
        for (const auto& state : causal_state_)
        {
            if (!state.eos || !state.buffer.empty())
            {
                return false;
            }
        }
        return true;
    }

    void close() noexcept
    {
        for (auto& channel : channels_)
        {
            channel.close();
        }
        channels_.clear();
        transport_eos_.clear();
        transport_buffer_ = {};
        causal_state_.clear();
    }

    [[nodiscard]] static std::string shard_channel_name(std::string_view base, std::size_t shard_id)
    {
        return std::string{base} + "_shard_" + std::to_string(shard_id);
    }

    [[nodiscard]] static CausalKey causal_key(const Frame& frame) noexcept
    {
        return CausalKey{.logical_tick = frame.header.logical_tick != 0U
                                             ? frame.header.logical_tick
                                             : frame.header.timestamp_unix_ns,
                         .agent_order = frame.header.agent_order,
                         .intra_agent_index = frame.header.intra_agent_index};
    }

    [[nodiscard]] static bool causal_less(const Frame& lhs, const Frame& rhs) noexcept
    {
        const auto lhs_key{causal_key(lhs)};
        const auto rhs_key{causal_key(rhs)};
        if (lhs_key.logical_tick != rhs_key.logical_tick)
        {
            return lhs_key.logical_tick < rhs_key.logical_tick;
        }
        if (lhs_key.agent_order != rhs_key.agent_order)
        {
            return lhs_key.agent_order < rhs_key.agent_order;
        }
        if (lhs_key.intra_agent_index != rhs_key.intra_agent_index)
        {
            return lhs_key.intra_agent_index < rhs_key.intra_agent_index;
        }
        return lhs.header.sequence < rhs.header.sequence;
    }

  private:
    struct TransportGreater
    {
        bool operator()(const Frame& lhs, const Frame& rhs) const noexcept
        {
            return lhs.header.sequence > rhs.header.sequence;
        }
    };

    // Min-heap comparator: smaller CausalKey has higher priority → top() is always
    // the causally-earliest frame held by this shard, regardless of SHM arrival order.
    struct CausalGreater
    {
        bool operator()(const Frame& lhs, const Frame& rhs) const noexcept
        {
            const auto lhs_key = CausalKey{
                .logical_tick = lhs.header.logical_tick != 0U ? lhs.header.logical_tick
                                                              : lhs.header.timestamp_unix_ns,
                .agent_order = lhs.header.agent_order,
                .intra_agent_index = lhs.header.intra_agent_index,
            };
            const auto rhs_key = CausalKey{
                .logical_tick = rhs.header.logical_tick != 0U ? rhs.header.logical_tick
                                                              : rhs.header.timestamp_unix_ns,
                .agent_order = rhs.header.agent_order,
                .intra_agent_index = rhs.header.intra_agent_index,
            };
            if (lhs_key.logical_tick != rhs_key.logical_tick)
                return lhs_key.logical_tick > rhs_key.logical_tick;
            if (lhs_key.agent_order != rhs_key.agent_order)
                return lhs_key.agent_order > rhs_key.agent_order;
            if (lhs_key.intra_agent_index != rhs_key.intra_agent_index)
                return lhs_key.intra_agent_index > rhs_key.intra_agent_index;
            return lhs.header.sequence > rhs.header.sequence;
        }
    };

    struct ShardCausalState
    {
        std::priority_queue<Frame, std::vector<Frame>, CausalGreater> buffer{};
        bool eos{false};
        // Set on first data frame arrival.  Shards that never receive data
        // (no agents hashed to them) remain false and are excluded from the
        // frontier check, so they never stall the k-way merge.
        bool ever_had_data{false};
    };

    static constexpr std::size_t kNoIndex{std::numeric_limits<std::size_t>::max()};

    using TransportQueue = std::priority_queue<Frame, std::vector<Frame>, TransportGreater>;

    void drain_transport_sequence()
    {
        for (std::size_t shard_id{0}; shard_id < channels_.size(); ++shard_id)
        {
            if (transport_eos_[shard_id])
            {
                continue;
            }
            Frame frame{};
            while (channels_[shard_id].try_pop(frame))
            {
                if (coderoast::ipc::has_flag(
                        frame.header.flags,
                        coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream))
                {
                    transport_eos_[shard_id] = true;
                }
                transport_buffer_.push(std::move(frame));
                if (transport_eos_[shard_id])
                {
                    break;
                }
            }
        }
    }

    [[nodiscard]] bool try_next_transport(Frame& out)
    {
        drain_transport_sequence();

        if (next_sequence_ == 0U)
        {
            if (transport_buffer_.empty())
            {
                return false;
            }
            next_sequence_ = transport_buffer_.top().header.sequence;
        }

        while (!transport_buffer_.empty() && transport_buffer_.top().header.sequence < next_sequence_)
        {
            transport_buffer_.pop();
        }

        if (transport_buffer_.empty())
        {
            return false;
        }

        const auto available_sequence{transport_buffer_.top().header.sequence};
        if (available_sequence == next_sequence_)
        {
            out = std::move(const_cast<Frame&>(transport_buffer_.top()));
            transport_buffer_.pop();
            ++next_sequence_;
            return true;
        }

        // Allow forward progress past missing sequences when either
        //   a) the configured gap policy says so, or
        //   b) every shard has signalled EndOfStream — in which case no
        //      future frame can ever close the gap and blocking forever would
        //      deadlock the consumer (and, transitively, any producer that
        //      eventually blocks on SHM backpressure).  Determinism is
        //      preserved because consumers re-sort the captured frames by
        //      causal key after the stream completes.
        const bool all_eos = !transport_eos_.empty() && [this]() noexcept {
            for (const bool eos : transport_eos_)
            {
                if (!eos)
                {
                    return false;
                }
            }
            return true;
        }();

        if (available_sequence > next_sequence_ &&
            (config_.gap_policy == SequenceGapPolicy::SkipMissing || all_eos))
        {
            skipped_sequences_ += available_sequence - next_sequence_;
            next_sequence_ = available_sequence;
            out = std::move(const_cast<Frame&>(transport_buffer_.top()));
            transport_buffer_.pop();
            ++next_sequence_;
            return true;
        }

        return false;
    }

    // ── Stage 1 ──────────────────────────────────────────────────────────────
    // Eagerly drain every shard's SHM ring into causal_state_[s].buffer.
    // This frees ring slots immediately so the ShardedPipeline consumer thread
    // (the SHM producer) is never stalled waiting for the ordering stage to
    // decide what to emit.  All frame types are moved into the per-shard deque;
    // EOS frames are consumed silently and mark the shard as done so future
    // drain calls skip it.
    void drain_transport()
    {
        for (std::size_t shard_id{0}; shard_id < channels_.size(); ++shard_id)
        {
            if (causal_state_[shard_id].eos)
            {
                continue;
            }
            Frame frame{};
            while (channels_[shard_id].try_pop(frame))
            {
                if (coderoast::ipc::has_flag(
                        frame.header.flags,
                        coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream))
                {
                    causal_state_[shard_id].eos = true;
                    break; // no more frames from this shard
                }
                const bool is_seal{coderoast::ipc::has_flag(
                    frame.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagWindowSeal)};
                causal_state_[shard_id].buffer.push(std::move(frame));
                if (!is_seal)
                    causal_state_[shard_id].ever_had_data = true;
            }
        }
    }

    // ── Stage 2 ──────────────────────────────────────────────────────────────
    // k-way merge over per-shard CausalKey min-heaps.
    //
    // Each shard's buffer is a min-heap (CausalGreater comparator), so top()
    // always exposes that shard's causally-earliest available frame.  The
    // k-way merge selects the global minimum across all non-empty shard tops.
    //
    // Per-shard frontier invariant: before emitting any frame, every shard
    // that has ever produced data must hold at least one buffered candidate
    // (or be EOS).  A non-EOS active shard with an empty buffer may still
    // produce a frame with a lower CausalKey than the current minimum — the
    // inter-shard scheduling lag between ShardedPipeline consumer threads and
    // SHM can expose shard A's higher-tick head while shard B's lower-tick
    // frame is still in-flight.  drain_transport() has already freed every
    // available SHM slot, so returning false here never stalls the engine.
    //
    // Shards with ever_had_data=false (no agents assigned) are excluded from
    // the frontier check so they never delay emission.
    //
    // Returns false when no safe emission is possible yet or all data is done.
    [[nodiscard]] bool try_next_causal(Frame& out)
    {
        drain_transport();

        // Frontier gate: all active non-EOS shards must have a candidate.
        for (std::size_t shard_id{0}; shard_id < causal_state_.size(); ++shard_id)
        {
            const auto& state{causal_state_[shard_id]};
            if (state.ever_had_data && !state.eos && state.buffer.empty())
            {
                return false;
            }
        }

        std::size_t best{kNoIndex};
        for (std::size_t shard_id{0}; shard_id < causal_state_.size(); ++shard_id)
        {
            if (causal_state_[shard_id].buffer.empty())
            {
                continue;
            }
            if (best == kNoIndex ||
                causal_less(causal_state_[shard_id].buffer.top(),
                            causal_state_[best].buffer.top()))
            {
                best = shard_id;
            }
        }
        if (best == kNoIndex)
        {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        out = std::move(const_cast<Frame&>(causal_state_[best].buffer.top()));
        causal_state_[best].buffer.pop();
        return true;
    }

    Config config_{};
    std::vector<Channel> channels_;
    std::vector<bool> transport_eos_;              // TransportSequence mode
    TransportQueue transport_buffer_;              // TransportSequence mode
    std::vector<ShardCausalState> causal_state_{}; // CausalKey mode
    std::uint64_t next_sequence_{1};
    std::uint64_t skipped_sequences_{0};
};

} // namespace coderoast::ipc::consumer
