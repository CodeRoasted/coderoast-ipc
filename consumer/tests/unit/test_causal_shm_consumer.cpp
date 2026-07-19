// NOLINTBEGIN : Unit tests intentionally favour clarity over style.
//
// Unit coverage for the single-threaded pull-based causal SHM consumer
// pipeline introduced by commit 335f9f6 ("collapse SHM drainer to
// single-threaded pull pipeline"). These tests pin the externally
// observable contract of:
//
//   * ShmTransportDrainer   — EOS absorption, seal surfacing,
//                              transport_complete, channel_stats.
//   * CausalReorderBuffer   — frontier gating, drained predicate,
//                              shard_summaries.
//   * CausalShmConsumer     — end-to-end causal ordering + control-frame
//                              filtering across multiple shards.
//
// The tests use real SHM channels (via SharedMemorySpscChannel) and
// unique PID-suffixed names so they are safe under parallel ctest.

#include <unistd.h>

#include <gtest/gtest.h>

import coderoast.ipc.consumer.test; // std + the facade + core (§11.9.11 aggregate)

namespace
{
using Frame = coderoast::ipc::DefaultLineFrame;
using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
using Drainer = coderoast::ipc::consumer::ShmTransportDrainer<Frame>;
using Buffer = coderoast::ipc::consumer::CausalReorderBuffer<Frame>;
using Emitter = coderoast::ipc::consumer::FrameEmitter<Frame>;
using Consumer = coderoast::ipc::consumer::CausalShmConsumer<Frame>;
using Flags = coderoast::ipc::LineFrameFlags;

[[nodiscard]] std::string unique_channel(const char* suffix)
{
    return std::string{"coderoast_drainer_test_"} + suffix + "_" + std::to_string(::getpid());
}

[[nodiscard]] Frame make_frame(std::uint64_t sequence, std::uint32_t shard_id, const char* payload,
                               std::uint64_t logical_tick = 0, std::uint32_t agent_order = 0,
                               std::uint32_t intra_agent_index = 0,
                               Flags flags = Flags::kLineFrameFlagNone)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.shard_id = shard_id;
    frame.header.shard_sequence = sequence;
    frame.header.logical_tick = logical_tick;
    frame.header.agent_order = agent_order;
    frame.header.intra_agent_index = intra_agent_index;
    frame.header.flags = flags;
    frame.header.payload_size = static_cast<std::uint32_t>(std::strlen(payload));
    std::memcpy(frame.payload.data(), payload, frame.header.payload_size);
    return frame;
}

[[nodiscard]] std::string payload_of(const Frame& frame)
{
    return std::string{// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                       reinterpret_cast<const char*>(frame.payload.data()),
                       frame.header.payload_size};
}

struct ProducerHarness
{
    std::string base;
    std::vector<Channel> producers;

    ProducerHarness(const char* suffix, std::size_t shard_count) : base{unique_channel(suffix)}
    {
        producers.reserve(shard_count);
        for (std::size_t shard_id{0}; shard_id < shard_count; ++shard_id)
        {
            producers.emplace_back(Channel::create(coderoast::ipc::ChannelConfig{
                .name = coderoast::ipc::shard_channel_name(base, shard_id),
                .slot_count = 16,
            }));
        }
    }

    ~ProducerHarness()
    {
        for (std::size_t shard_id{0}; shard_id < producers.size(); ++shard_id)
        {
            Channel::unlink(coderoast::ipc::shard_channel_name(base, shard_id));
        }
    }

    ProducerHarness(const ProducerHarness&) = delete;
    ProducerHarness& operator=(const ProducerHarness&) = delete;
    ProducerHarness(ProducerHarness&&) = delete;
    ProducerHarness& operator=(ProducerHarness&&) = delete;
};
} // namespace

TEST(ShmTransportDrainer, ZeroShardCountThrows)
{
    EXPECT_THROW((Drainer{Drainer::Config{.channel = unique_channel("zero"), .shard_count = 0}}),
                 std::invalid_argument);
}

TEST(ShmTransportDrainer, AbsorbsEosFrameAndFlipsShardEos)
{
    ProducerHarness producers{"eos", 1};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 1}};

    (void)producers.producers[0].push(make_frame(1, 0, "data"));
    producers.producers[0].close_graceful();

    Frame out{};
    ASSERT_TRUE(drainer.try_pull(0, out));
    EXPECT_EQ(payload_of(out), "data");
    EXPECT_FALSE(drainer.shard_eos(0));

    // Second pull pops the EOS marker; try_pull must NOT surface it.
    EXPECT_FALSE(drainer.try_pull(0, out));
    EXPECT_TRUE(drainer.shard_eos(0));
    EXPECT_TRUE(drainer.transport_complete());

    // Further pulls must remain false (idempotent EOS).
    EXPECT_FALSE(drainer.try_pull(0, out));
}

TEST(ShmTransportDrainer, SealFrameSurfacesToCaller)
{
    ProducerHarness producers{"seal", 1};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 1}};

    (void)producers.producers[0].push(
        make_frame(1, 0, "", 0, 0, 0, Flags::kLineFrameFlagWindowSeal));

    // Seals are surfaced to the caller (the reorder buffer's seal-driven
    // frontier gates on them) — unlike EOS, they do not flip the eos flag.
    Frame out{};
    ASSERT_TRUE(drainer.try_pull(0, out));
    EXPECT_TRUE(coderoast::ipc::has_flag(out.header.flags, Flags::kLineFrameFlagWindowSeal));
    EXPECT_FALSE(drainer.shard_eos(0));
}

TEST(ShmTransportDrainer, ChannelStatsReflectPushAndPop)
{
    ProducerHarness producers{"stats", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};

    (void)producers.producers[0].push(make_frame(1, 0, "a"));
    (void)producers.producers[1].push(make_frame(1, 1, "b"));
    (void)producers.producers[1].push(make_frame(2, 1, "c"));

    Frame tmp{};
    (void)drainer.try_pull(0, tmp);
    (void)drainer.try_pull(1, tmp);

    const auto stats{drainer.channel_stats()};
    ASSERT_EQ(stats.size(), 2U);
    EXPECT_EQ(stats[0].popped, 1U);
    EXPECT_EQ(stats[1].popped, 1U);
}

TEST(ShmTransportDrainer, CloseIsIdempotent)
{
    ProducerHarness producers{"close", 1};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 1}};
    drainer.close();
    EXPECT_NO_THROW(drainer.close());
}

TEST(CausalReorderBuffer, FrontierGatesOnceAllShardsProduced)
{
    ProducerHarness producers{"frontier", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    // Both shards produce before we select: refill pulls both, frontier
    // is satisfied, and the causally-earlier (tick=5) wins.
    (void)producers.producers[0].push(make_frame(1, 0, "s0-a", /*tick=*/10));
    (void)producers.producers[1].push(make_frame(2, 1, "s1-a", /*tick=*/5));

    Frame out{};
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s1-a");

    // After the s1-a pop, shard 1's heap is empty and shard 1 is not EOS,
    // so the frontier gate must block emission of s0-a even though shard 0
    // has a buffered candidate.
    EXPECT_FALSE(buffer.try_select(out));

    // Pushing fresh data to shard 0 does not unblock — shard 1 is still silent.
    (void)producers.producers[0].push(make_frame(3, 0, "s0-b", /*tick=*/20));
    EXPECT_FALSE(buffer.try_select(out));

    // EOS on shard 1 releases the gate; shard 0 frames flow in tick order.
    producers.producers[1].close_graceful();
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s0-a");
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s0-b");
}

TEST(CausalReorderBuffer, FrontierGatesOnIdleShardThatNeverProducedData)
{
    // Regression (seal-driven barrier): a shard that has produced no data
    // frames at all MUST still gate the frontier until it buffers a frame
    // (its window seal — the watermark) or reaches EOS. The old data-driven
    // gate excluded never-produced shards, which let the merge run ahead of
    // an idle shard that later produces data — e.g. an agent silent for 60 s
    // that then emits. See the frontier-rule contract in the header.
    ProducerHarness producers{"idle_frontier", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    // Shard 0 produces data; shard 1 is completely silent (no data, no seal,
    // no EOS) — its agents have not fired yet.
    (void)producers.producers[0].push(make_frame(1, 0, "s0-a", /*tick=*/10));

    // Frontier MUST block: emitting s0-a could pass a lower-tick frame that
    // the still-silent shard 1 has yet to produce.
    Frame out{};
    EXPECT_FALSE(buffer.try_select(out));

    // A window seal from shard 1 is a buffered candidate (seals are the
    // watermark): the frontier completes and the causally-earliest frame
    // flows — here the tick=5 seal precedes the tick=10 data.
    (void)producers.producers[1].push(
        make_frame(2, 1, "", /*tick=*/5, 0, 0, Flags::kLineFrameFlagWindowSeal));
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_TRUE(coderoast::ipc::has_flag(out.header.flags, Flags::kLineFrameFlagWindowSeal));
}

TEST(CausalReorderBuffer, WatermarkFrontierDrainsFinalSealBatchWithoutEos)
{
    // Regression (the PlayToTarget freeze stall): at the freeze the last frame on
    // every shard is that window's seal — all sharing one boundary tick — and NO
    // eos follows (the engine is paused, not stopped). A strict "empty heap
    // blocks" frontier emits the first shard's seal, then blocks forever on that
    // now-empty, non-eos shard, stranding the others' seals so the window never
    // closes. The watermark frontier drains them: a shard whose watermark has
    // reached the candidate's tick no longer blocks it.
    constexpr std::uint64_t kSealTick{1000};
    constexpr std::size_t kShards{3};
    ProducerHarness producers{"tail_drain", kShards};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = kShards}};
    Buffer buffer{drainer};

    // Every shard emits exactly one window seal at the same boundary tick, then
    // goes idle — no data after, no eos.
    for (std::uint32_t shard{0}; shard < kShards; ++shard)
    {
        (void)producers.producers[shard].push(make_frame(shard + 1U, shard, "", /*tick=*/kSealTick,
                                                         0, 0, Flags::kLineFrameFlagWindowSeal));
    }

    std::vector<std::uint32_t> drained_shards;
    Frame out{};
    for (int attempt{0}; attempt < 16 && drained_shards.size() < kShards; ++attempt)
    {
        if (buffer.try_select(out))
        {
            drained_shards.push_back(out.header.shard_id);
        }
    }

    // All three seals must surface despite no shard ever reaching eos.
    ASSERT_EQ(drained_shards.size(), kShards)
        << "watermark frontier stranded the final seal batch (drained only "
        << drained_shards.size() << "/" << kShards << ")";
    std::sort(drained_shards.begin(), drained_shards.end());
    EXPECT_EQ(drained_shards, (std::vector<std::uint32_t>{0U, 1U, 2U}));
}

// The per-shard seals of one window all carry the SAME causal triple — emit_control_frame sets
// neither agent_order nor intra_agent_index, so each is (tick, 0, 0). `shard_id` is therefore a
// REACHABLE tie-break, and it is what makes the reconciled seal order deterministic.
//
// This regression pins that. It is arranged so it can FAIL: the shards are pushed in the order
// 2, 0, 1, so their global `header.sequence` values (1, 2, 3) run OPPOSITE to shard_id. The
// comparator previously consulted `header.sequence` — the cross-shard racing counter named in
// CLAUDE.md's determinism carve-out — before ever reaching shard_id, which made the emitted seal
// order follow the transport race instead of the causal key. Under that comparator this test
// yields {2, 0, 1}; under the current one it yields {0, 1, 2}.
//
// Note this asserts the ORDER, unlike the tail-drain test above, which sorts first because its
// property is "no seal is stranded by the frontier gate", not "the seals come out in one order".
TEST(CausalReorderBuffer, PerWindowSealOrderIsDeterministicByShardNotTransportSequence)
{
    constexpr std::uint64_t kSealTick{1000};
    constexpr std::size_t kShards{3};
    ProducerHarness producers{"seal_order", kShards};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = kShards}};
    Buffer buffer{drainer};

    // Push order (and therefore ascending global sequence) deliberately inverts shard_id.
    constexpr std::array<std::uint32_t, kShards> kPushOrder{2U, 0U, 1U};
    for (std::size_t index{0}; index < kPushOrder.size(); ++index)
    {
        const auto shard{kPushOrder[index]};
        (void)producers.producers[shard].push(
            make_frame(static_cast<std::uint64_t>(index) + 1U, shard, "", /*tick=*/kSealTick, 0, 0,
                       Flags::kLineFrameFlagWindowSeal));
    }

    std::vector<std::uint32_t> emitted_shards;
    Frame out{};
    for (int attempt{0}; attempt < 16 && emitted_shards.size() < kShards; ++attempt)
    {
        if (buffer.try_select(out))
        {
            emitted_shards.push_back(out.header.shard_id);
        }
    }

    ASSERT_EQ(emitted_shards.size(), kShards)
        << "expected every shard's seal to surface; got " << emitted_shards.size() << "/"
        << kShards;
    EXPECT_EQ(emitted_shards, (std::vector<std::uint32_t>{0U, 1U, 2U}))
        << "seal emission order followed the transport sequence, not the causal key — the "
           "reconciled stream is no longer a deterministic byte sequence";
}

TEST(CausalReorderBuffer, DrainedRequiresAllShardsEosAndEmptyHeaps)
{
    ProducerHarness producers{"drained", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    EXPECT_FALSE(buffer.drained());

    producers.producers[0].close_graceful();
    (void)producers.producers[1].push(make_frame(1, 1, "x", /*tick=*/5));
    producers.producers[1].close_graceful();

    // No refill has happened yet: drainer EOS flags are still unset.
    EXPECT_FALSE(buffer.drained());

    Frame out{};
    // Refill via try_select absorbs both EOS frames and the data frame.
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "x");

    // Both shards EOS, heaps empty \u2192 drained.
    EXPECT_TRUE(buffer.drained());
}

TEST(CausalShmConsumer, EndToEndCausalOrderingAcrossShardsWithEos)
{
    constexpr std::size_t kShardCount{2};
    ProducerHarness producers{"e2e", kShardCount};

    // Producer interleaves causal keys across shards.
    // shard 0: tick=10, tick=30
    // shard 1: tick=20, tick=40, then EOS
    // Expected emit order by logical_tick: 10, 20, 30, 40.
    (void)producers.producers[0].push(make_frame(1, 0, "t10", /*tick=*/10));
    (void)producers.producers[1].push(make_frame(2, 1, "t20", /*tick=*/20));
    (void)producers.producers[0].push(make_frame(3, 0, "t30", /*tick=*/30));
    (void)producers.producers[1].push(make_frame(4, 1, "t40", /*tick=*/40));
    producers.producers[0].close_graceful();
    producers.producers[1].close_graceful();

    Consumer consumer{Consumer::Config{
        .channel = producers.base,
        .shard_count = kShardCount,
        .backpressure = coderoast::ipc::BackpressurePolicy::Block,
        .wait_strategy = coderoast::ipc::WaitStrategy::Adaptive,
        .emit_control_frames = false,
    }};

    std::vector<std::string> emitted;
    Frame out{};
    while (!consumer.all_shards_done())
    {
        if (consumer.try_next(out))
        {
            emitted.push_back(payload_of(out));
        }
    }

    ASSERT_EQ(emitted.size(), 4U);
    EXPECT_EQ(emitted[0], "t10");
    EXPECT_EQ(emitted[1], "t20");
    EXPECT_EQ(emitted[2], "t30");
    EXPECT_EQ(emitted[3], "t40");

    EXPECT_EQ(consumer.emitted(), 4U);
    EXPECT_EQ(consumer.shard_count(), kShardCount);
    EXPECT_TRUE(consumer.all_shards_done());
}

TEST(CausalShmConsumer, ControlFramesAreFilteredByDefault)
{
    ProducerHarness producers{"ctl", 1};

    (void)producers.producers[0].push(make_frame(1, 0, "data1", /*tick=*/10));
    (void)producers.producers[0].push(
        make_frame(2, 0, "", /*tick=*/15, 0, 0, Flags::kLineFrameFlagWindowSeal));
    (void)producers.producers[0].push(make_frame(3, 0, "data2", /*tick=*/20));
    producers.producers[0].close_graceful();

    Consumer consumer{Consumer::Config{
        .channel = producers.base,
        .shard_count = 1,
        .emit_control_frames = false,
    }};

    std::vector<std::string> emitted;
    Frame out{};
    while (!consumer.all_shards_done())
    {
        if (consumer.try_next(out))
        {
            emitted.push_back(payload_of(out));
        }
    }

    ASSERT_EQ(emitted.size(), 2U);
    EXPECT_EQ(emitted[0], "data1");
    EXPECT_EQ(emitted[1], "data2");
    EXPECT_EQ(consumer.emitter().control_dropped(), 1U);
}

// ─────────────────────────────────────────────────────────────────────────────
// Observability — atomic counters + discrete-event observer.
// ─────────────────────────────────────────────────────────────────────────────

TEST(DrainerMetrics, CountsPullsEosAndSeals)
{
    ProducerHarness producers{"metrics_drainer", 1};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 1}};

    EXPECT_EQ(drainer.metrics().pulls_attempted, 0U);

    (void)producers.producers[0].push(make_frame(1, 0, "a"));
    (void)producers.producers[0].push(
        make_frame(2, 0, "", 0, 0, 0, Flags::kLineFrameFlagWindowSeal));
    producers.producers[0].close_graceful();

    Frame out{};
    EXPECT_TRUE(drainer.try_pull(0, out));  // data
    EXPECT_TRUE(drainer.try_pull(0, out));  // seal (surfaced as data=true)
    EXPECT_FALSE(drainer.try_pull(0, out)); // EOS absorbed
    EXPECT_FALSE(drainer.try_pull(0, out)); // already EOS — fast-path

    const auto metrics{drainer.metrics()};
    EXPECT_EQ(metrics.pulls_attempted, 4U);
    // pulls_succeeded counts only frames returned to the caller: data + seal.
    EXPECT_EQ(metrics.pulls_succeeded, 2U);
    EXPECT_EQ(metrics.eos_observed, 1U);
    EXPECT_EQ(metrics.seals_observed, 1U);
}

TEST(DrainerObserver, FiresOnShardEosTransition)
{
    ProducerHarness producers{"obs_eos", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};

    std::vector<std::size_t> eos_shards;
    drainer.set_observer(
        [&eos_shards](const coderoast::ipc::consumer::ConsumerEventPayload& event)
        {
            if (event.event == coderoast::ipc::consumer::ConsumerEvent::kShardEos)
            {
                eos_shards.push_back(event.shard_id);
            }
        });

    producers.producers[1].close_graceful();
    producers.producers[0].close_graceful();

    Frame out{};
    EXPECT_FALSE(drainer.try_pull(1, out));
    EXPECT_FALSE(drainer.try_pull(0, out));

    ASSERT_EQ(eos_shards.size(), 2U);
    EXPECT_EQ(eos_shards[0], 1U);
    EXPECT_EQ(eos_shards[1], 0U);
}

TEST(ReorderMetrics, CountsRefillsSelectsAndFrontierBlocks)
{
    ProducerHarness producers{"metrics_reorder", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    // No data anywhere → there is no candidate, so try_select returns false
    // without a frontier block (the gate only fires when a candidate exists
    // but a lagging shard holds it back — see the next select below).
    Frame out{};
    EXPECT_FALSE(buffer.try_select(out));

    // Both shards produce before any select → frontier satisfied; the
    // first select pops the lower-tick frame (shard 1, tick=5).
    (void)producers.producers[0].push(make_frame(1, 0, "s0-a", /*tick=*/10));
    (void)producers.producers[1].push(make_frame(2, 1, "s1-a", /*tick=*/5));
    ASSERT_TRUE(buffer.try_select(out));

    // Shard 1 now has an empty heap, is not EOS, and its watermark (tick=5)
    // is below the only candidate s0-a (tick=10) → the frontier gate MUST
    // block: shard 1 could still deliver a frame causally before s0-a.
    EXPECT_FALSE(buffer.try_select(out));

    const auto metrics{buffer.metrics()};
    EXPECT_GE(metrics.refills, 3U);
    EXPECT_EQ(metrics.selects_attempted, 3U);
    EXPECT_EQ(metrics.selects_succeeded, 1U);
    EXPECT_GE(metrics.frontier_blocks, 1U);
}

TEST(ReorderObserver, FiresFrontierBlockAndDrainComplete)
{
    ProducerHarness producers{"obs_reorder", 1};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 1}};
    Buffer buffer{drainer};

    std::vector<coderoast::ipc::consumer::ConsumerEvent> events;
    buffer.set_observer([&events](const coderoast::ipc::consumer::ConsumerEventPayload& event)
                        { events.push_back(event.event); });

    (void)producers.producers[0].push(make_frame(1, 0, "x"));
    Frame out{};
    ASSERT_TRUE(buffer.try_select(out));

    // EOS + empty heap → next try_select should fire kDrainComplete exactly once.
    producers.producers[0].close_graceful();
    EXPECT_FALSE(buffer.try_select(out));
    EXPECT_FALSE(buffer.try_select(out)); // idempotent

    const auto drain_complete{coderoast::ipc::consumer::ConsumerEvent::kDrainComplete};
    const auto count{
        static_cast<std::size_t>(std::count(events.begin(), events.end(), drain_complete))};
    EXPECT_EQ(count, 1U) << "kDrainComplete must fire exactly once";
}

TEST(ConsumerMetrics, FacadeAggregatesAllStages)
{
    ProducerHarness producers{"facade_metrics", 1};
    Consumer consumer{Consumer::Config{.channel = producers.base, .shard_count = 1}};

    // Distinct ticks are load-bearing, not decoration: the causal key must be unique, and a
    // fixture that leaves tick/agent_order/intra_agent_index at 0 has no causal identity at all
    // — it used to be ordered by `header.sequence`, the non-deterministic transport counter.
    (void)producers.producers[0].push(make_frame(1, 0, "alpha", /*tick=*/10));
    (void)producers.producers[0].push(
        make_frame(2, 0, "", /*tick=*/20, 0, 0, Flags::kLineFrameFlagWindowSeal));
    (void)producers.producers[0].push(make_frame(3, 0, "beta", /*tick=*/30));
    producers.producers[0].close_graceful();

    Frame out{};
    std::size_t emitted_count{0};
    while (!consumer.all_shards_done())
    {
        if (consumer.try_next(out))
        {
            ++emitted_count;
        }
    }

    EXPECT_EQ(emitted_count, 2U);
    const auto metrics{consumer.metrics()};
    EXPECT_GE(metrics.drainer.pulls_attempted, 4U);
    EXPECT_EQ(metrics.drainer.eos_observed, 1U);
    EXPECT_EQ(metrics.drainer.seals_observed, 1U);
    EXPECT_EQ(metrics.emitter.emitted, 2U);
    EXPECT_EQ(metrics.emitter.control_dropped, 1U);
    EXPECT_EQ(metrics.emitter.last_sequence, 3U);
    EXPECT_GE(metrics.reorder.selects_succeeded, 3U); // 2 data + 1 seal
}

// NOLINTEND : Unit tests intentionally favour clarity over style.
