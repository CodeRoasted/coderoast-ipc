#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace coderoast::ipc::consumer
{

/// Snapshot of the SHM transport drainer's hot-path counters.
///
/// All counters are monotonically increasing; deltas between two
/// snapshots are the canonical way to read rates. Counters are updated
/// with `memory_order_relaxed` writes inside the hot path and `acquire`
/// reads here, so a snapshot is internally inconsistent under heavy
/// concurrency — that is intentional and cheaper than a global mutex.
struct DrainerMetrics
{
    /// Number of times `try_pull` was invoked, regardless of outcome.
    std::uint64_t pulls_attempted{0};
    /// Number of `try_pull` calls that returned a payload frame.
    std::uint64_t pulls_succeeded{0};
    /// Number of EOS frames absorbed by the drainer (never surfaced).
    std::uint64_t eos_observed{0};
    /// Number of window-seal frames observed (surfaced to the caller; the
    /// reorder buffer's seal-driven frontier gates on them).
    std::uint64_t seals_observed{0};
};

/// Snapshot of the causal reorder buffer's hot-path counters.
struct ReorderMetrics
{
    /// Number of `refill` calls (one per `try_select`).
    std::uint64_t refills{0};
    /// Number of `try_select` invocations.
    std::uint64_t selects_attempted{0};
    /// Number of successful selections (a frame was popped).
    std::uint64_t selects_succeeded{0};
    /// Number of selections blocked by the watermark frontier gate (a non-EOS
    /// shard had an empty heap and a watermark below the candidate's tick, so it
    /// could still deliver a causally-earlier frame).
    std::uint64_t frontier_blocks{0};
};

/// Snapshot of the frame emitter's counters.
struct EmitterMetrics
{
    std::uint64_t emitted{0};
    std::uint64_t control_dropped{0};
    std::uint64_t last_sequence{0};
};

/// Aggregate snapshot returned by `CausalShmConsumer::metrics()`.
struct ConsumerMetrics
{
    DrainerMetrics drainer{};
    ReorderMetrics reorder{};
    EmitterMetrics emitter{};
};

/// Discrete events surfaced through the optional consumer observer.
/// The observer is invoked on rare lifecycle transitions only; it is NOT
/// invoked on the per-frame hot path. Use `ConsumerMetrics` snapshots
/// for per-frame counters.
enum class ConsumerEvent : std::uint8_t
{
    /// A shard transitioned from "active" to "EOS observed".
    kShardEos = 0,
    /// A `try_select` returned false because the frontier was incomplete.
    /// Fired at most once per try_select that blocks; rate is bounded by
    /// the caller's poll loop, NOT by frame rate.
    kFrontierBlock,
    /// Every shard has reported EOS and the reorder buffer is empty.
    kDrainComplete,
};

/// Payload accompanying a `ConsumerEvent`.
struct ConsumerEventPayload
{
    ConsumerEvent event{ConsumerEvent::kShardEos};
    /// Shard identifier when the event is per-shard (kShardEos,
    /// kFrontierBlock). Set to `kAllShards` for whole-pipeline events.
    std::size_t shard_id{0};
    static constexpr std::size_t kAllShards{static_cast<std::size_t>(-1)};
};

/// Observer callback. Empty by default; injectable per component or via
/// the `CausalShmConsumer` facade. Setting an observer is a control-plane
/// operation; the observer itself is invoked off the per-frame hot path.
using ConsumerObserver = std::function<void(const ConsumerEventPayload&)>;

} // namespace coderoast::ipc::consumer
