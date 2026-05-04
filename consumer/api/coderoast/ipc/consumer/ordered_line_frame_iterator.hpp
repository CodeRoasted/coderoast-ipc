#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/frame.hpp"

namespace insight::ingest
{

enum class SequenceGapPolicy : std::uint8_t
{
    WaitForMissing,
    SkipMissing,
};

template <typename Frame = coderoast::ipc::DefaultLineFrame> struct OrderedLineFrameIteratorConfig
{
    std::string channel{"coderoast.default"};
    std::size_t shard_count{1};
    std::uint64_t first_sequence{1};
    SequenceGapPolicy gap_policy{SequenceGapPolicy::WaitForMissing};
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
        pending_.resize(config_.shard_count);
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
        fill_pending();
        discard_stale_pending();

        if (next_sequence_ == 0U)
        {
            const auto lowest{lowest_pending_sequence()};
            if (lowest == kNoSequence)
            {
                return false;
            }
            next_sequence_ = lowest;
        }

        if (const auto index{find_pending_sequence(next_sequence_)}; index != kNoIndex)
        {
            out = pending_[index].frame;
            pending_[index].has_value = false;
            ++next_sequence_;
            return true;
        }

        if (config_.gap_policy == SequenceGapPolicy::SkipMissing)
        {
            const auto lowest{lowest_pending_sequence()};
            if (lowest != kNoSequence && lowest > next_sequence_)
            {
                skipped_sequences_ += lowest - next_sequence_;
                next_sequence_ = lowest;
                return try_next(out);
            }
        }
        return false;
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

    void close() noexcept
    {
        for (auto& channel : channels_)
        {
            channel.close();
        }
        channels_.clear();
        pending_.clear();
    }

    [[nodiscard]] static std::string shard_channel_name(std::string_view base, std::size_t shard_id)
    {
        return std::string{base} + "_shard_" + std::to_string(shard_id);
    }

  private:
    struct PendingFrame
    {
        Frame frame{};
        bool has_value{false};
    };

    static constexpr std::size_t kNoIndex{std::numeric_limits<std::size_t>::max()};
    static constexpr std::uint64_t kNoSequence{std::numeric_limits<std::uint64_t>::max()};

    void fill_pending()
    {
        for (std::size_t index{0}; index < channels_.size(); ++index)
        {
            if (pending_[index].has_value)
            {
                continue;
            }
            Frame frame{};
            if (channels_[index].try_pop(frame))
            {
                pending_[index] = PendingFrame{.frame = frame, .has_value = true};
            }
        }
    }

    void discard_stale_pending()
    {
        if (next_sequence_ == 0U)
        {
            return;
        }
        bool discarded{true};
        while (discarded)
        {
            discarded = false;
            for (std::size_t index{0}; index < pending_.size(); ++index)
            {
                if (pending_[index].has_value &&
                    pending_[index].frame.header.sequence < next_sequence_)
                {
                    pending_[index].has_value = false;
                    discarded = true;
                }
            }
            if (discarded)
            {
                fill_pending();
            }
        }
    }

    [[nodiscard]] std::size_t find_pending_sequence(std::uint64_t sequence) const noexcept
    {
        for (std::size_t index{0}; index < pending_.size(); ++index)
        {
            if (pending_[index].has_value && pending_[index].frame.header.sequence == sequence)
            {
                return index;
            }
        }
        return kNoIndex;
    }

    [[nodiscard]] std::uint64_t lowest_pending_sequence() const noexcept
    {
        std::uint64_t lowest{kNoSequence};
        for (const auto& pending : pending_)
        {
            if (pending.has_value)
            {
                lowest = std::min(lowest, pending.frame.header.sequence);
            }
        }
        return lowest;
    }

    Config config_{};
    std::vector<Channel> channels_;
    std::vector<PendingFrame> pending_;
    std::uint64_t next_sequence_{1};
    std::uint64_t skipped_sequences_{0};
};

} // namespace insight::ingest
