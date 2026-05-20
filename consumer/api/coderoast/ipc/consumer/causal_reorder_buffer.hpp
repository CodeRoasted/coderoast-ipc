#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include "coderoast/ipc/consumer/ordered_line_frame_iterator.hpp"
#include "coderoast/ipc/consumer/shm_transport_drainer.hpp"
#include "coderoast/ipc/frame.hpp"

namespace coderoast::ipc::consumer
{

// CausalKey + extract_causal_key + causal_less are defined in
// ordered_line_frame_iterator.hpp and re-used here. The new pull-based
// pipeline is intentionally byte-for-byte compatible with the legacy
// iterator's ordering so callers can migrate without re-computing the
// expected frame order.

template <typename Frame>
[[nodiscard]] inline CausalKey extract_causal_key(const Frame& frame) noexcept
{
    return CausalKey{
        .logical_tick = frame.header.logical_tick != 0U ? frame.header.logical_tick
                                                        : frame.header.timestamp_unix_ns,
        .agent_order = frame.header.agent_order,
        .intra_agent_index = frame.header.intra_agent_index,
    };
}

template <typename Frame>
[[nodiscard]] inline bool causal_less(const Frame& lhs, const Frame& rhs) noexcept
{
    const auto lhs_key{extract_causal_key(lhs)};
    const auto rhs_key{extract_causal_key(rhs)};
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

/// **Step 2 of the pull-based causal SHM consumer pipeline.**
///
/// Owns a per-shard CausalKey min-heap and runs a k-way merge over the
/// shard heads to emit frames in global causal order.
///
/// **Frontier rule:** before emitting any frame, every shard that has
/// previously produced data and has not yet observed EOS must hold at
/// least one candidate frame. This prevents emission of a higher-tick
/// frame from shard A while a lower-tick frame from shard B is still
/// in-flight. Shards that have never produced data (no agents hashed to
/// them) are excluded from the frontier check.
///
/// **Separation of concerns:** this stage knows nothing about transport
/// (SHM rings, EOS frames). It pulls *opaque* frames from a
/// `ShmTransportDrainer` via the narrow `try_pull/shard_eos/...` API.
template <typename Frame = coderoast::ipc::DefaultLineFrame>
class CausalReorderBuffer
{
  public:
    explicit CausalReorderBuffer(ShmTransportDrainer<Frame>& drainer)
        : drainer_{&drainer}, shards_(drainer.shard_count())
    {
    }

    CausalReorderBuffer(const CausalReorderBuffer&) = delete;
    CausalReorderBuffer& operator=(const CausalReorderBuffer&) = delete;
    CausalReorderBuffer(CausalReorderBuffer&&) = delete;
    CausalReorderBuffer& operator=(CausalReorderBuffer&&) = delete;
    ~CausalReorderBuffer() = default;

    /// Drain every fresh frame currently held by the transport drainer
    /// into the per-shard min-heaps. Cheap and bounded: it pulls only
    /// what is already buffered in-process; it never reaches into SHM.
    void refill()
    {
        Frame frame{};
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            while (drainer_->try_pull(shard_id, frame))
            {
                shards_[shard_id].buffer.push(std::move(frame));
            }
        }
    }

    /// Attempt to select and pop the globally-earliest CausalKey across
    /// all shard heads. Returns false when the frontier is incomplete
    /// or no shard has a buffered candidate.
    [[nodiscard]] bool try_select(Frame& out)
    {
        refill();

        // Frontier gate.
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            if (drainer_->shard_ever_had_data(shard_id) && !drainer_->shard_eos(shard_id) &&
                shards_[shard_id].buffer.empty())
            {
                return false;
            }
        }

        std::size_t best{kNoIndex};
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            if (shards_[shard_id].buffer.empty())
            {
                continue;
            }
            if (best == kNoIndex || causal_less(shards_[shard_id].buffer.top(),
                                                shards_[best].buffer.top()))
            {
                best = shard_id;
            }
        }
        if (best == kNoIndex)
        {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        out = std::move(const_cast<Frame&>(shards_[best].buffer.top()));
        shards_[best].buffer.pop();
        return true;
    }

    /// True once every shard's transport has signalled EOS *and* every
    /// per-shard min-heap is empty. Equivalent semantics to the legacy
    /// `OrderedLineFrameIterator::all_shards_done()`.
    [[nodiscard]] bool drained() const noexcept
    {
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            if (!drainer_->shard_eos(shard_id))
            {
                return false;
            }
            if (!shards_[shard_id].buffer.empty())
            {
                return false;
            }
        }
        return !shards_.empty();
    }

    struct ShardSummary
    {
        bool ever_had_data{false};
        bool eos{false};
        std::size_t buf_size{0};
    };

    [[nodiscard]] std::vector<ShardSummary> shard_summaries() const
    {
        std::vector<ShardSummary> out;
        out.reserve(shards_.size());
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            out.push_back(ShardSummary{
                .ever_had_data = drainer_->shard_ever_had_data(shard_id),
                .eos = drainer_->shard_eos(shard_id),
                .buf_size = shards_[shard_id].buffer.size(),
            });
        }
        return out;
    }

  private:
    struct Greater
    {
        bool operator()(const Frame& lhs, const Frame& rhs) const noexcept
        {
            // Min-heap: top() is causally earliest.
            return causal_less(rhs, lhs);
        }
    };

    struct ShardState
    {
        std::priority_queue<Frame, std::vector<Frame>, Greater> buffer{};
    };

    static constexpr std::size_t kNoIndex{std::numeric_limits<std::size_t>::max()};

    ShmTransportDrainer<Frame>* drainer_{nullptr};
    std::vector<ShardState> shards_;
};

} // namespace coderoast::ipc::consumer
