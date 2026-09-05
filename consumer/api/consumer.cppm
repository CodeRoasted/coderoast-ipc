export module coderoast.ipc.consumer;
import std;
import coderoast.ipc.core;

export namespace coderoast::ipc::consumer
{

// invariant: monotonic counters written relaxed on the hot path, so a snapshot is internally
// inconsistent under concurrency — cheaper than a lock.
struct DrainerMetrics
{
    std::uint64_t pulls_attempted{0};
    std::uint64_t pulls_succeeded{0};
    std::uint64_t eos_observed{0};
    std::uint64_t seals_observed{0};
};

// invariant: every try_select makes exactly one refill; frontier_blocks counts the selections a
// lagging shard held back.
struct ReorderMetrics
{
    std::uint64_t refills{0};
    std::uint64_t selects_attempted{0};
    std::uint64_t selects_succeeded{0};
    std::uint64_t frontier_blocks{0};
};

// invariant: plain counters, not atomics — the emitter has a single owner thread.
struct EmitterMetrics
{
    std::uint64_t emitted{0};
    std::uint64_t control_dropped{0};
    std::uint64_t last_sequence{0};
};

struct ConsumerMetrics
{
    DrainerMetrics drainer{};
    ReorderMetrics reorder{};
    EmitterMetrics emitter{};
};

// invariant: raised on lifecycle transitions only, never on the per-frame path; per-frame numbers
// come from ConsumerMetrics.
enum class ConsumerEvent : std::uint8_t
{
    kShardEos = 0,
    // invariant: at most one per blocking try_select, so its rate follows the caller's poll loop
    // and not the frame rate.
    kFrontierBlock,
    kDrainComplete,
};

struct ConsumerEventPayload
{
    ConsumerEvent event{ConsumerEvent::kShardEos};
    std::size_t shard_id{0};
    static constexpr std::size_t kAllShards{static_cast<std::size_t>(-1)};
};

using ConsumerObserver = std::function<void(const ConsumerEventPayload&)>;

// refs: ADR-11.D3
// invariant: data frames are unique on (agent_order, intra_agent_index) alone — the agent_order
// namespaces are disjoint and the index is per-producer monotonic.
// invariant: every shard's seal for one window carries the same (tick, 0, 0), so shard_id is a
// reachable tie-break rather than defensive code.
// invariant: header.sequence is the cross-shard race counter and is never in the key.
struct CausalKey
{
    std::uint64_t logical_tick{0};
    std::uint32_t agent_order{0};
    std::uint32_t intra_agent_index{0};
    // invariant: already on the wire, so the tie-break costs no bytes; it is what lifts the merge
    // from a deterministic multiset to a deterministic byte sequence.
    std::uint32_t shard_id{0};
};

// invariant: one key and one comparator for every causal reorder over these frames.
// invariant: logical_tick == 0 means the producer emitted no logical clock, so the fallback to the
// wall stamp is made here and not at each caller.
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

// refs: ADR-11.D3
// post: a strict TOTAL order — no two distinct frames compare equivalent, so the merge leaves no
// residual tie.
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
    return lhs_key.shard_id < rhs_key.shard_id;
}

// post: unlinks every shard on construction and again on destruction, so a crashed run's frames
// cannot leak in and the next run starts clean.
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

// invariant: one channel per shard; no thread, no mutex and no queue of its own, and try_pull never
// blocks.
// invariant: per-shard order is the ring's FIFO; cross-shard order is step 2's.
// post: construction opens every shard or throws, closing what it already opened.
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

    // invariant: end of stream is the channel's state, not an in-band sentinel.
    // post: true only for a data or seal frame; an EOS is absorbed, flips the shard's eos flag
    // exactly once and returns false.
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

    // post: every shard has seen EOS; the reorder buffer's heaps may still hold frames.
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

    [[nodiscard]] DrainerMetrics metrics() const noexcept
    {
        return DrainerMetrics{
            .pulls_attempted = pulls_attempted_.load(std::memory_order_acquire),
            .pulls_succeeded = pulls_succeeded_.load(std::memory_order_acquire),
            .eos_observed = eos_observed_.load(std::memory_order_acquire),
            .seals_observed = seals_observed_.load(std::memory_order_acquire),
        };
    }

    // pre: not called concurrently with try_pull — this is construction-time wiring.
    void set_observer(ConsumerObserver observer)
    {
        observer_ = std::move(observer);
    }

    // post: idempotent; the destructor calls it.
    void close() noexcept
    {
        for (auto& chan : channels_)
        {
            chan.close();
        }
        channels_.clear();
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

// refs: ADR-11.D4
// invariant: a per-shard CausalKey min-heap merged k-way; single owner thread, no mutex.
// invariant: the per-shard heaps are unbounded by design — capping here would drop frames
// silently, and the ring's slot_count bounds the ring, never these heaps.
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

    // post: moves every frame the shards' rings currently hold into the per-shard heaps and raises
    // each shard's watermark to the highest tick it has produced.
    void refill()
    {
        refills_.fetch_add(1U, std::memory_order_relaxed);
        Frame frame{};
        for (std::size_t shard_id{0}; shard_id < shards_.size(); ++shard_id)
        {
            while (drainer_->try_pull(shard_id, frame))
            {
                const auto tick{extract_causal_key(frame).logical_tick};
                if (tick > shards_[shard_id].watermark_tick)
                {
                    shards_[shard_id].watermark_tick = tick;
                }
                shards_[shard_id].buffer.push(std::move(frame));
            }
        }
    }

    // invariant: the frontier gate — the earliest buffered candidate is emitted only once no
    // non-EOS shard has an empty heap and a watermark below that candidate's tick.
    // post: false when the frontier blocks or no shard holds a candidate.
    [[nodiscard]] bool try_select(Frame& out)
    {
        selects_attempted_.fetch_add(1U, std::memory_order_relaxed);
        refill();

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
            if (!drain_complete_notified_ && drained())
            {
                drain_complete_notified_ = true;
                notify(ConsumerEvent::kDrainComplete, ConsumerEventPayload::kAllShards);
            }
            return false;
        }

        // assert: a shard settles by being EOS, by holding a candidate, or by a watermark that
        // reached best's tick — a WindowSeal promises nothing earlier remains on that shard.
        // note: this is what drains the final same-tick seal batch at a PlayToTarget freeze.
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

        // note: priority_queue::top() is const and the pop right after invalidates the slot.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        out = std::move(const_cast<Frame&>(shards_[best].buffer.top()));
        shards_[best].buffer.pop();
        check_causal_monotonicity(out);
        selects_succeeded_.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }

    // post: every shard's transport signalled EOS and every per-shard heap is empty.
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

    // refs: ADR-11.D3
    // invariant: the emitted keys are strictly increasing; a tie means the key's uniqueness premise
    // broke upstream, an inversion means the frontier released a frame too early.
    // note: one comparison per frame against an O(shards) select — determinism outranks it.
    void check_causal_monotonicity(const Frame& frame)
    {
        const auto key{extract_causal_key(frame)};
        if (has_emitted_)
        {
            const auto as_tuple{[](const CausalKey& causal_key)
                                {
                                    return std::tie(causal_key.logical_tick, causal_key.agent_order,
                                                    causal_key.intra_agent_index,
                                                    causal_key.shard_id);
                                }};
            if (!(as_tuple(key) > as_tuple(last_emitted_key_)))
            {
                std::cerr << "FATAL: causal merge emitted a non-increasing key — the reconciled "
                             "order is not deterministic.\n  previous: tick="
                          << last_emitted_key_.logical_tick
                          << " agent_order=" << last_emitted_key_.agent_order
                          << " intra_agent_index=" << last_emitted_key_.intra_agent_index
                          << " shard_id=" << last_emitted_key_.shard_id
                          << "\n  current:  tick=" << key.logical_tick
                          << " agent_order=" << key.agent_order
                          << " intra_agent_index=" << key.intra_agent_index
                          << " shard_id=" << key.shard_id << '\n';
                std::abort();
            }
        }
        last_emitted_key_ = key;
        has_emitted_ = true;
    }

    struct Greater
    {
        bool operator()(const Frame& lhs, const Frame& rhs) const noexcept
        {
            return causal_less(rhs, lhs);
        }
    };

    struct ShardState
    {
        std::priority_queue<Frame, std::vector<Frame>, Greater> buffer{};
        // invariant: per-shard frames are causally non-decreasing, so the highest tick pulled
        // bounds the earliest frame that shard can still deliver.
        std::uint64_t watermark_tick{0};
    };

    static constexpr std::size_t kNoIndex{std::numeric_limits<std::size_t>::max()};

    ShmTransportDrainer<Frame>* drainer_{nullptr};
    std::vector<ShardState> shards_;
    ConsumerObserver observer_;
    bool drain_complete_notified_{false};

    CausalKey last_emitted_key_{};
    bool has_emitted_{false};

    std::atomic<std::uint64_t> refills_{0};
    std::atomic<std::uint64_t> selects_attempted_{0};
    std::atomic<std::uint64_t> selects_succeeded_{0};
    std::atomic<std::uint64_t> frontier_blocks_{0};
};

// invariant: single owner thread, no thread and no mutex of its own; two moves per emitted frame
// and no allocation.
template <coderoast::ipc::FrameLike Frame = coderoast::ipc::DefaultLineFrame> class FrameEmitter
{
  public:
    struct Config
    {
        // invariant: false skips window seals; EOS never reaches here, the drainer absorbs it.
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

    // post: false when the frontier is incomplete or the pipeline is fully drained.
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

// invariant: the whole pipeline runs on the caller's thread inside try_next() — no thread, no
// mutex and no queue anywhere in it.
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

    // invariant: buffer_ points at drainer_ and emitter_ at buffer_, so a move would dangle.
    CausalShmConsumer(const CausalShmConsumer&) = delete;
    CausalShmConsumer& operator=(const CausalShmConsumer&) = delete;
    CausalShmConsumer(CausalShmConsumer&&) = delete;
    CausalShmConsumer& operator=(CausalShmConsumer&&) = delete;
    ~CausalShmConsumer() = default;

    [[nodiscard]] bool try_next(Frame& out)
    {
        return emitter_.try_next(out);
    }

    // post: every shard EOS and the reorder buffer empty — the condition on which try_select
    // raises kDrainComplete once.
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

    [[nodiscard]] ConsumerMetrics metrics() const noexcept
    {
        return ConsumerMetrics{
            .drainer = drainer_.metrics(),
            .reorder = buffer_.metrics(),
            .emitter = emitter_.metrics(),
        };
    }

    // post: registers with the drainer and the reorder buffer; the emitter raises no event.
    void set_observer(ConsumerObserver observer)
    {
        drainer_.set_observer(observer);
        buffer_.set_observer(std::move(observer));
    }

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

// invariant: the N per-shard WindowSeals of a window are swallowed and replaced by one synthesized
// WindowClosed after the Nth arrives.
// note: raw per-shard seals stay reachable through CausalShmConsumer directly.
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

    // invariant: kNone means nothing is emittable yet, not that the stream ended.
    enum class NextKind : std::uint8_t
    {
        kNone,
        kFrame,
        kWindowClosed,
    };

    struct Config
    {
        typename Underlying::Config underlying{};
    };

    explicit WindowClosedConsumer(Config config)
        : shard_count_{config.underlying.shard_count},
          underlying_{[&config]
                      {
                          // invariant: the underlying consumer is forced to emit control frames —
                          // this adapter is their only reader.
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

    // post: at most one event per call; several completing seals in one call still yield one
    // WindowClosed, and the next comes on the next call.
    // invariant: a window's entry is erased when its Nth seal lands, so the map holds only
    // partially sealed windows.
    [[nodiscard]] NextKind try_next(Frame& out_frame, WindowClosed& out_window)
    {
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
                seal_counts_.erase(it);
                ++windows_closed_;
                out_window = WindowClosed{.window_id = window_id, .logical_tick = logical_tick};
                return NextKind::kWindowClosed;
            }
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
