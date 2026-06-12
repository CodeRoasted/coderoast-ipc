// coderoast.ipc.consumer — PURE named module (1.5.1 unwrap). Header-only: the former 10
// api/coderoast/ipc/consumer/*.hpp now live here, concatenated in dependency (topo) order. std via
// import std; core frame/channel types via import coderoast.ipc.core (was textual includes). No
// detail namespace / thread_local / POSIX in this package. (ingest.hpp umbrella retired.)
export module coderoast.ipc.consumer;
import std;
import coderoast.ipc.core;

export namespace coderoast::ipc::consumer
{

// ─────────── from consumer_metrics.hpp ───────────
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

// ─────────── from ordered_line_frame_iterator.hpp ───────────
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
    // Final determinism tie-breaker: producer shard_id from the frame header.
    // No extra wire bytes — shard_id is already populated by the producer
    // (see shared_memory_producer.hpp::build). Including it here lifts the
    // consumer contract from "deterministic multiset" to "deterministic byte
    // sequence", so two replays of the same scenario emit byte-identical
    // frame streams even when (logical_tick, agent_order, intra_agent_index)
    // collide across shards.
    std::uint32_t shard_id{0};
};

struct OrderedLineFrameIteratorConfig
{
    std::string channel{"coderoast.default"};
    std::size_t shard_count{1};
    std::uint64_t first_sequence{1};
    SequenceGapPolicy gap_policy{SequenceGapPolicy::WaitForMissing};
    FrameOrdering ordering{FrameOrdering::CausalKey};
    coderoast::ipc::BackpressurePolicy backpressure{coderoast::ipc::BackpressurePolicy::Block};
    coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::Adaptive};
};

template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame>
class OrderedLineFrameIterator
{
  public:
    using Config = OrderedLineFrameIteratorConfig;
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
                         .intra_agent_index = frame.header.intra_agent_index,
                         .shard_id = frame.header.shard_id};
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
        if (lhs.header.sequence != rhs.header.sequence)
        {
            return lhs.header.sequence < rhs.header.sequence;
        }
        // Final tie-break: shard_id. Per-shard sequence numbers can collide
        // across shards (each shard has its own counter), so sequence alone
        // is not globally deterministic. shard_id is monotonic and unique.
        return lhs_key.shard_id < rhs_key.shard_id;
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
            if (lhs.header.sequence != rhs.header.sequence)
                return lhs.header.sequence > rhs.header.sequence;
            // Final determinism tie-breaker: producer shard_id. See CausalKey
            // doc-comment above for rationale.
            return lhs.header.shard_id > rhs.header.shard_id;
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
            for (;;)
            {
                const auto status{channels_[shard_id].try_pop_status(frame)};
                if (status == coderoast::ipc::PopStatus::Ok)
                {
                    transport_buffer_.push(std::move(frame));
                    continue;
                }
                if (status == coderoast::ipc::PopStatus::Closed ||
                    status == coderoast::ipc::PopStatus::Aborted)
                {
                    transport_eos_[shard_id] = true;
                }
                break;
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

        while (!transport_buffer_.empty() &&
               transport_buffer_.top().header.sequence < next_sequence_)
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
        const bool all_eos =
            !transport_eos_.empty() && std::ranges::all_of(transport_eos_, std::identity{});

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
            for (;;)
            {
                const auto status{channels_[shard_id].try_pop_status(frame)};
                if (status == coderoast::ipc::PopStatus::Ok)
                {
                    const bool is_seal{coderoast::ipc::has_flag(
                        frame.header.flags,
                        coderoast::ipc::LineFrameFlags::kLineFrameFlagWindowSeal)};
                    causal_state_[shard_id].buffer.push(std::move(frame));
                    if (!is_seal)
                        causal_state_[shard_id].ever_had_data = true;
                    continue;
                }
                if (status == coderoast::ipc::PopStatus::Closed ||
                    status == coderoast::ipc::PopStatus::Aborted)
                {
                    causal_state_[shard_id].eos = true;
                }
                break;
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
                causal_less(causal_state_[shard_id].buffer.top(), causal_state_[best].buffer.top()))
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

// ─────────── from scoped_shm_channel_set.hpp ───────────
/// RAII guard that unlinks every shard of a named sharded SHM channel set
/// on construction and again on destruction.
///
/// Use this around any consumer that owns the lifetime of its channel set
/// (tests, standalone consumer programs, scenario runners). The pre-unlink
/// ensures stale frames from a previously crashed run cannot leak into the
/// current run; the post-unlink ensures the next run starts clean.
///
/// This type intentionally lives in coderoast-ipc rather than in any
/// downstream test helper so that the convention is owned and tested by
/// the IPC package itself.
template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame>
class ScopedShmChannelSet
{
  public:
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

    ScopedShmChannelSet(std::string channel_name, std::size_t shard_count)
        : channel_name_{std::move(channel_name)}, shard_count_{shard_count}
    {
        unlink_all();
    }

    ScopedShmChannelSet(const ScopedShmChannelSet&) = delete;
    ScopedShmChannelSet& operator=(const ScopedShmChannelSet&) = delete;
    ScopedShmChannelSet(ScopedShmChannelSet&&) = delete;
    ScopedShmChannelSet& operator=(ScopedShmChannelSet&&) = delete;

    ~ScopedShmChannelSet() noexcept
    {
        unlink_all();
    }

    [[nodiscard]] const std::string& channel_name() const noexcept
    {
        return channel_name_;
    }

    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return shard_count_;
    }

    [[nodiscard]] static std::string shard_channel_name(std::string_view base, std::size_t shard_id)
    {
        return std::string{base} + "_shard_" + std::to_string(shard_id);
    }

  private:
    void unlink_all() const noexcept
    {
        for (std::size_t shard_id{0}; shard_id < shard_count_; ++shard_id)
        {
            Channel::unlink(shard_channel_name(channel_name_, shard_id));
        }
    }

    std::string channel_name_;
    std::size_t shard_count_{0};
};

// ─────────── from shm_transport_drainer.hpp ───────────
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
///   * Window-seal frames ARE returned. They carry the per-window
///     watermark the reorder buffer's seal-driven frontier gates on.
///   * No causal-ordering / watermark logic lives here — that is step 2's
///     responsibility. This stage only knows about SHM transport state.
template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame>
class ShmTransportDrainer
{
  public:
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

    struct Config
    {
        std::string channel{"coderoast.default"};
        std::size_t shard_count{1};
        coderoast::ipc::BackpressurePolicy backpressure{coderoast::ipc::BackpressurePolicy::Block};
        coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::Adaptive};
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

    ~ShmTransportDrainer() noexcept
    {
        close();
    }

    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return shards_.size();
    }

    /// Non-blocking pop from shard `shard_id`'s SPSC SHM ring.
    ///
    /// End-of-stream is now a *channel state*, not an in-band sentinel.
    /// When the producer transitions the channel to Closing/Closed
    /// (graceful) or Aborted (forced), `try_pop_status` returns
    /// `PopStatus::Closed` / `PopStatus::Aborted` once the ring is
    /// empty.  The drainer flips the per-shard `eos` flag exactly once
    /// on that transition.
    ///
    /// Returns true iff a data/seal frame was popped.
    [[nodiscard]] bool try_pull(std::size_t shard_id, Frame& out)
    {
        auto& shard{*shards_[shard_id]};
        pulls_attempted_.fetch_add(1U, std::memory_order_relaxed);
        if (shard.eos.load(std::memory_order_acquire))
        {
            return false;
        }
        const auto status{channels_[shard_id].try_pop_status(out)};
        if (status == coderoast::ipc::PopStatus::Ok)
        {
            if (coderoast::ipc::has_flag(out.header.flags,
                                         coderoast::ipc::LineFrameFlags::kLineFrameFlagWindowSeal))
            {
                seals_observed_.fetch_add(1U, std::memory_order_relaxed);
            }
            pulls_succeeded_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        if (status == coderoast::ipc::PopStatus::Closed ||
            status == coderoast::ipc::PopStatus::Aborted)
        {
            // Producer has signalled end-of-stream out-of-band.  Flip
            // the per-shard eos flag exactly once and notify observers.
            bool expected{false};
            if (shard.eos.compare_exchange_strong(expected, true, std::memory_order_acq_rel))
            {
                eos_observed_.fetch_add(1U, std::memory_order_relaxed);
                notify(ConsumerEvent::kShardEos, shard_id);
            }
        }
        return false;
    }

    [[nodiscard]] bool shard_eos(std::size_t shard_id) const noexcept
    {
        return shards_[shard_id]->eos.load(std::memory_order_acquire);
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
    };

    Config config_{};
    std::vector<Channel> channels_;
    std::vector<std::unique_ptr<Shard>> shards_;
    ConsumerObserver observer_;

    std::atomic<std::uint64_t> pulls_attempted_{0};
    std::atomic<std::uint64_t> pulls_succeeded_{0};
    std::atomic<std::uint64_t> eos_observed_{0};
    std::atomic<std::uint64_t> seals_observed_{0};
};

// ─────────── from shared_memory_source.hpp ───────────
template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame>
class SharedMemorySource
{
  public:
    struct Config
    {
        std::string channel{"coderoast.default"};
        std::size_t shard_count{1};
        std::uint64_t first_sequence{1};
        SequenceGapPolicy gap_policy{SequenceGapPolicy::WaitForMissing};
        FrameOrdering ordering{FrameOrdering::CausalKey};
        coderoast::ipc::BackpressurePolicy backpressure{coderoast::ipc::BackpressurePolicy::Block};
        coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::Adaptive};
    };

    SharedMemorySource() = default;

    explicit SharedMemorySource(Config config)
        : iterator_{typename OrderedLineFrameIterator<Frame>::Config{
              .channel = std::move(config.channel),
              .shard_count = config.shard_count,
              .first_sequence = config.first_sequence,
              .gap_policy = config.gap_policy,
              .ordering = config.ordering,
              .backpressure = config.backpressure,
              .wait_strategy = config.wait_strategy,
          }}
    {
    }

    SharedMemorySource(const SharedMemorySource&) = delete;
    SharedMemorySource& operator=(const SharedMemorySource&) = delete;
    SharedMemorySource(SharedMemorySource&&) noexcept = default;
    SharedMemorySource& operator=(SharedMemorySource&&) noexcept = default;
    ~SharedMemorySource() = default;

    // Attempt to consume the next ordered frame.
    //
    // Returns true and populates out_payload with a view into the frame's
    // payload bytes.  The view is valid until the next call to try_pop().
    //
    // Returns false when no in-order frame is currently available (either
    // all channels are empty, or a gap is being waited on under the
    // WaitForMissing policy).
    [[nodiscard]] bool try_pop(std::string_view& out_payload)
    {
        while (iterator_.try_next(current_frame_))
        {
            if (coderoast::ipc::is_control_frame(current_frame_.header.flags))
            {
                continue;
            }
            out_payload =
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                std::string_view{reinterpret_cast<const char*>(current_frame_.payload.data()),
                                 current_frame_.header.payload_size};
            return true;
        }
        return false;
    }

    // Attempt to consume the next ordered frame, including control frames such
    // as WindowSeal and EndOfStream. Callers that need deterministic replay
    // completion should use this API instead of the payload-only try_pop().
    [[nodiscard]] bool try_pop_frame(Frame& out_frame)
    {
        return iterator_.try_next(out_frame);
    }

    // Shard id carried in the frame header of the most recently popped frame.
    // Valid only immediately after a successful try_pop() call.
    [[nodiscard]] std::uint32_t current_shard_id() const noexcept
    {
        return current_frame_.header.shard_id;
    }

    // Number of globally-sequenced frames skipped due to SkipMissing policy.
    [[nodiscard]] std::uint64_t skipped_sequences() const noexcept
    {
        return iterator_.skipped_sequences();
    }

    // Number of shards this source is consuming.
    [[nodiscard]] std::size_t shard_count() const noexcept
    {
        return iterator_.shard_count();
    }

    void close() noexcept
    {
        iterator_.close();
    }

  private:
    OrderedLineFrameIterator<Frame> iterator_{};
    Frame current_frame_{};
};

// ─────────── from causal_reorder_buffer.hpp ───────────
// CausalKey + extract_causal_key + causal_less are defined in
// ordered_line_frame_iterator.hpp and re-used here. The new pull-based
// pipeline is intentionally byte-for-byte compatible with the legacy
// iterator's ordering so callers can migrate without re-computing the
// expected frame order.

template <coderoast::ipc::FrameLike Frame>
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

template <coderoast::ipc::FrameLike Frame>
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
template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame>
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

// ─────────── from frame_emitter.hpp ───────────
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
template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame> class FrameEmitter
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

// ─────────── from causal_shm_consumer.hpp ───────────
/// Thin facade composing the three-step pull-based causal SHM consumer:
///
///   * Step 1 — `ShmTransportDrainer`  (threadless SHM pop, never stalls
///                                      on ordering, never blocks)
///   * Step 2 — `CausalReorderBuffer`  (global CausalKey min-heap)
///   * Step 3 — `FrameEmitter`         (control-frame filter + diagnostics)
///
/// The whole pipeline runs on the caller's thread inside `try_next()`:
/// the emitter asks the reorder buffer for the next causally-earliest
/// frame; the buffer round-robin-polls every shard's SHM ring via the
/// drainer; the drainer pops directly from each `SharedMemorySpscChannel`.
/// There are no background threads, no mutexes, and no internal queues —
/// frames move from SHM ring → per-shard heap → caller via two moves.
///
/// All callers that only need "give me the next causally-earliest data
/// frame from this sharded SHM channel" should use this facade and
/// never instantiate the sub-objects directly. Advanced callers (e.g. a
/// test that injects a fault between stages) may compose the three
/// classes themselves.
///
/// ─────────────────────────── Production contracts ───────────────────────────
///
/// **Threading model.** Single owner thread. The facade spawns no
/// threads and holds no mutex. Backpressure, determinism, frame lifetime
/// and error-handling contracts are inherited from the three sub-stages;
/// see their individual headers.
///
/// **Observability.** Call `metrics()` for a `ConsumerMetrics` snapshot
/// aggregating the three sub-stages' atomic counters. Register a
/// `ConsumerObserver` via `set_observer()` to receive discrete events
/// (shard EOS, frontier block, drain complete). Both are off the
/// per-frame hot path.
///
/// **Lifecycle.** Construction opens every shard channel; failure
/// throws and rolls back via RAII. `close()` releases the SHM handles
/// explicitly; destruction does the same. EOS propagation: producers
/// push an EOS frame to every shard; the drainer absorbs them; once
/// every shard is EOS and every heap is empty, `all_shards_done()`
/// returns true and the observer fires `kDrainComplete` exactly once.
template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame>
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
        coderoast::ipc::WaitStrategy wait_strategy{coderoast::ipc::WaitStrategy::Adaptive};
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

    /// Aggregate snapshot of every sub-stage's counters. See
    /// `ConsumerMetrics` documentation for relaxed-atomic semantics.
    [[nodiscard]] ConsumerMetrics metrics() const noexcept
    {
        return ConsumerMetrics{
            .drainer = drainer_.metrics(),
            .reorder = buffer_.metrics(),
            .emitter = emitter_.metrics(),
        };
    }

    /// Register the same observer with every sub-stage. Pass an empty
    /// `ConsumerObserver` to detach.
    void set_observer(ConsumerObserver observer)
    {
        drainer_.set_observer(observer);
        buffer_.set_observer(std::move(observer));
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

// ─────────── from window_closed_consumer.hpp ───────────
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
template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame>
class WindowClosedConsumer
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
