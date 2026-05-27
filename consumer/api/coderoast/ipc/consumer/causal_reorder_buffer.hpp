#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <queue>
#include <utility>
#include <vector>

#include "coderoast/ipc/consumer/consumer_metrics.hpp"
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
        .shard_id = frame.header.shard_id,
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
    if (lhs.header.sequence != rhs.header.sequence)
    {
        return lhs.header.sequence < rhs.header.sequence;
    }
    // Final determinism tie-breaker: producer shard_id. Per-shard sequence
    // numbers are independent counters and can collide across shards, so
    // sequence alone is not globally deterministic. See CausalKey doc in
    // ordered_line_frame_iterator.hpp for rationale.
    return lhs_key.shard_id < rhs_key.shard_id;
}

/// **Step 2 of the pull-based causal SHM consumer pipeline.**
///
/// Owns a per-shard CausalKey min-heap and runs a k-way merge over the
/// shard heads to emit frames in global causal order.
///
/// ─────────────────────────── Production contracts ───────────────────────────
///
/// **Threading model**
///   * Single owner thread: the same thread that calls `try_select`.
///   * Holds no internal threads. No mutex anywhere.
///   * Reads the drainer's per-shard EOS flag via the public `shard_eos`
///     accessor (acquire load).
///
/// **Backpressure semantics**
///   * Pure pull. Never blocks. `try_select` returns false when the
///     frontier is incomplete; the caller decides whether to poll again
///     or yield.
///   * Per-shard heaps grow with backlog when other shards stall. There
///     is no hard cap — under sustained imbalance, RSS grows. This is
///     intentional: the drainer/producer side already enforces the SHM
///     backpressure policy; throwing it away here would create silent
///     data loss.
///
/// **Determinism contract**
///   * Output is deterministic given identical drainer input. Ties are
///     broken by `(logical_tick, agent_order, intra_agent_index, sequence)`
///     in that order, which is a total order.
///   * The frontier gate guarantees no late frame from any non-EOS shard
///     can be "passed" by a later frame from another shard.
///
/// **Frame lifetime / move semantics**
///   * Frames are moved out of the drainer into the per-shard min-heap
///     (`shards_[i].buffer.push(std::move(frame))`).
///   * `try_select` moves the heap top out to the caller's frame.
///     `priority_queue::top()` returns const-ref; we use a single
///     `const_cast` to enable the move (the next `pop()` invalidates
///     the slot). NO copies on the success path.
///
/// **Error handling strategy**
///   * No exceptions thrown on the hot path. `refill` and `try_select`
///     are `noexcept` in spirit (a `std::bad_alloc` from the heap push
///     can propagate; in steady state allocations come from a small
///     vector pool that pre-grows).
///
/// **Allocation strategy**
///   * `std::priority_queue<Frame, std::vector<Frame>>` re-uses its
///     internal vector. Once steady-state backlog stabilises, no more
///     allocations happen.
///   * `refill` and `try_select` perform zero heap allocations beyond
///     the priority queue's growth.
///
/// **Observability**
///   * `metrics()` returns a `ReorderMetrics` snapshot (relaxed atomics).
///   * `set_observer()` registers a callback for `kFrontierBlock` and
///     `kDrainComplete` discrete events. Off the per-frame hot path.
///
/// **Frontier rule (watermark gate):** the causally-earliest buffered
/// candidate `best` (tick t) may be emitted only once no shard could still
/// deliver a frame earlier than `best`. Each shard settles that in one of
/// three ways: it is EOS, it holds a buffered candidate (so its earliest
/// future frame is >= best), or its *watermark* — the highest tick it has
/// produced — has already reached t. The watermark works because each shard
/// emits a deterministic WindowSeal for *every* window, data-bearing or not,
/// and a seal is a promise that nothing at or before its boundary tick
/// remains on that shard. Only a non-EOS shard with an empty heap AND a
/// watermark below t can still strand an earlier frame, so only that case
/// blocks. Temporal progression is driven by seals, never by data volume;
/// this is the only safe rule when a shard can be idle now and produce data
/// later, and it lets the final same-tick seal batch drain at a PlayToTarget
/// freeze where no EOS follows.
///
/// **Separation of concerns:** this stage knows nothing about transport
/// (SHM rings, EOS frames). It pulls *opaque* frames from a
/// `ShmTransportDrainer` via the narrow `try_pull/shard_eos/...` API.
template <typename Frame = coderoast::ipc::DefaultLineFrame> class CausalReorderBuffer
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
        refills_.fetch_add(1U, std::memory_order_relaxed);
        Frame frame{};
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            while (drainer_->try_pull(shard_id, frame))
            {
                // Track the shard's watermark — the highest tick it has produced.
                // Per-shard frames are causally non-decreasing, so this is the
                // shard's last-pulled tick; it bounds the earliest frame the shard
                // can still deliver, which the frontier gate relies on.
                const auto tick{extract_causal_key(frame).logical_tick};
                if (tick > shards_[shard_id].watermark_tick)
                {
                    shards_[shard_id].watermark_tick = tick;
                }
                shards_[shard_id].buffer.push(std::move(frame));
            }
        }
    }

    /// Attempt to select and pop the globally-earliest CausalKey across
    /// all shard heads. Returns false when the frontier is incomplete
    /// or no shard has a buffered candidate.
    [[nodiscard]] bool try_select(Frame& out)
    {
        selects_attempted_.fetch_add(1U, std::memory_order_relaxed);
        refill();

        // Pick the causally-earliest buffered candidate across all shard heads.
        std::size_t best{kNoIndex};
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            if (shards_[shard_id].buffer.empty())
            {
                continue;
            }
            if (best == kNoIndex ||
                causal_less(shards_[shard_id].buffer.top(), shards_[best].buffer.top()))
            {
                best = shard_id;
            }
        }
        if (best == kNoIndex)
        {
            // Every heap is empty. If every shard is also EOS we are fully
            // drained; otherwise we are simply waiting for the next frame.
            if (!drain_complete_notified_ && drained())
            {
                drain_complete_notified_ = true;
                notify(ConsumerEvent::kDrainComplete, ConsumerEventPayload::kAllShards);
            }
            return false;
        }

        // Watermark frontier gate. `best` may be emitted only once no other shard
        // could still deliver a causally-earlier frame. A shard settles that in one
        // of three ways: it is EOS (no more frames at all), it holds a buffered
        // candidate (its earliest future frame is therefore >= best), or its
        // watermark — the highest tick it has produced — has already reached best's
        // tick, since every WindowSeal promises nothing at or before its boundary
        // tick remains on that shard. Only a non-EOS shard with an empty heap AND a
        // watermark below best's tick can still strand an earlier frame, so only
        // that case blocks. This is what lets the final batch of same-tick window
        // seals drain at a PlayToTarget freeze, where no EOS ever follows.
        const auto best_tick{extract_causal_key(shards_[best].buffer.top()).logical_tick};
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            if (!drainer_->shard_eos(shard_id) && shards_[shard_id].buffer.empty() &&
                shards_[shard_id].watermark_tick < best_tick)
            {
                frontier_blocks_.fetch_add(1U, std::memory_order_relaxed);
                notify(ConsumerEvent::kFrontierBlock, shard_id);
                return false;
            }
        }

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        out = std::move(const_cast<Frame&>(shards_[best].buffer.top()));
        shards_[best].buffer.pop();
        selects_succeeded_.fetch_add(1U, std::memory_order_relaxed);
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
                .eos = drainer_->shard_eos(shard_id),
                .buf_size = shards_[shard_id].buffer.size(),
            });
        }
        return out;
    }

    [[nodiscard]] ReorderMetrics metrics() const noexcept
    {
        return ReorderMetrics{
            .refills = refills_.load(std::memory_order_acquire),
            .selects_attempted = selects_attempted_.load(std::memory_order_acquire),
            .selects_succeeded = selects_succeeded_.load(std::memory_order_acquire),
            .frontier_blocks = frontier_blocks_.load(std::memory_order_acquire),
        };
    }

    /// Register a discrete-event observer. See drainer documentation.
    void set_observer(ConsumerObserver observer)
    {
        observer_ = std::move(observer);
    }

  private:
    void notify(ConsumerEvent event, std::size_t shard_id)
    {
        if (observer_)
        {
            observer_(ConsumerEventPayload{.event = event, .shard_id = shard_id});
        }
    }

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
        // Highest tick pulled from this shard so far. A shard's future frames are
        // causally >= this, so it bounds the earliest frame the shard can still
        // deliver — the basis of the watermark frontier gate in try_select().
        std::uint64_t watermark_tick{0};
    };

    static constexpr std::size_t kNoIndex{std::numeric_limits<std::size_t>::max()};

    ShmTransportDrainer<Frame>* drainer_{nullptr};
    std::vector<ShardState> shards_;
    ConsumerObserver observer_;
    bool drain_complete_notified_{false};

    std::atomic<std::uint64_t> refills_{0};
    std::atomic<std::uint64_t> selects_attempted_{0};
    std::atomic<std::uint64_t> selects_succeeded_{0};
    std::atomic<std::uint64_t> frontier_blocks_{0};
};

} // namespace coderoast::ipc::consumer
