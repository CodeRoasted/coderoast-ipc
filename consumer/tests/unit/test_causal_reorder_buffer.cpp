
#include <gtest/gtest.h>

import coderoast.ipc.consumer.test;

#include "causal_pipeline_test_harness.hpp"

TEST(CausalReorderBuffer, FrontierGatesOnceAllShardsProduced)
{
    ProducerHarness producers{"frontier", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    (void)producers.producers[0].push(make_frame(1, 0, "s0-a", /*logical_tick=*/10));
    (void)producers.producers[1].push(make_frame(2, 1, "s1-a", /*logical_tick=*/5));

    Frame out{};
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s1-a");

    EXPECT_FALSE(buffer.try_select(out));

    (void)producers.producers[0].push(make_frame(3, 0, "s0-b", /*logical_tick=*/20));
    EXPECT_FALSE(buffer.try_select(out));

    producers.producers[1].close_graceful();
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s0-a");
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s0-b");
}

TEST(CausalReorderBuffer, FrontierGatesOnIdleShardThatNeverProducedData)
{
    // assert: a shard that has produced nothing at all must still gate the frontier — an agent
    // silent for a minute and then emitting is the case this pins.
    ProducerHarness producers{"idle_frontier", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    (void)producers.producers[0].push(make_frame(1, 0, "s0-a", /*logical_tick=*/10));

    Frame out{};
    EXPECT_FALSE(buffer.try_select(out));

    (void)producers.producers[1].push(
        make_frame(2, 1, "", /*logical_tick=*/5, 0, 0, Flags::kLineFrameFlagWindowSeal));
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_TRUE(coderoast::ipc::has_flag(out.header.flags, Flags::kLineFrameFlagWindowSeal));
}

// refs: F-SRC-logcraft:test_determinism_shm_gate.cpp
TEST(CausalReorderBuffer, WatermarkFrontierDrainsFinalSealBatchWithoutEos)
{
    // assert: at a play-to-target freeze every shard's last frame is that window's seal, all on one
    // boundary tick, and no EOS ever follows because the engine is paused.
    constexpr std::uint64_t kSealTick{1000};
    constexpr std::size_t kShards{3};
    ProducerHarness producers{"tail_drain", kShards};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = kShards}};
    Buffer buffer{drainer};

    for (std::uint32_t shard{0}; shard < kShards; ++shard)
    {
        (void)producers.producers[shard].push(make_frame(shard + 1U, shard, "",
                                                         /*logical_tick=*/kSealTick, 0, 0,
                                                         Flags::kLineFrameFlagWindowSeal));
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

    ASSERT_EQ(drained_shards.size(), kShards)
        << "watermark frontier stranded the final seal batch (drained only "
        << drained_shards.size() << "/" << kShards << ")";
    std::sort(drained_shards.begin(), drained_shards.end());
    EXPECT_EQ(drained_shards, (std::vector<std::uint32_t>{0U, 1U, 2U}));
}

// refs: ADR-11.D3
// assert: the push order 2, 0, 1 makes the global sequence run opposite to shard_id, so a
// comparator consulting it would emit 2, 0, 1 here instead of 0, 1, 2.
// note: the tail-drain test above sorts first; its property is completeness, not order.
TEST(CausalReorderBuffer, PerWindowSealOrderIsDeterministicByShardNotTransportSequence)
{
    constexpr std::uint64_t kSealTick{1000};
    constexpr std::size_t kShards{3};
    ProducerHarness producers{"seal_order", kShards};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = kShards}};
    Buffer buffer{drainer};

    constexpr std::array<std::uint32_t, kShards> kPushOrder{2U, 0U, 1U};
    for (std::size_t index{0}; index < kPushOrder.size(); ++index)
    {
        const auto shard{kPushOrder[index]};
        (void)producers.producers[shard].push(make_frame(static_cast<std::uint64_t>(index) + 1U,
                                                         shard, "", /*logical_tick=*/kSealTick, 0,
                                                         0, Flags::kLineFrameFlagWindowSeal));
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

    ASSERT_EQ(emitted_shards.size(), kShards) << "expected every shard's seal to surface; got "
                                              << emitted_shards.size() << "/" << kShards;
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
    (void)producers.producers[1].push(make_frame(1, 1, "x", /*logical_tick=*/5));
    producers.producers[1].close_graceful();

    EXPECT_FALSE(buffer.drained());

    Frame out{};
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "x");

    EXPECT_TRUE(buffer.drained());
}

TEST(ReorderMetrics, CountsRefillsSelectsAndFrontierBlocks)
{
    ProducerHarness producers{"metrics_reorder", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    // assert: with no candidate anywhere there is nothing for a lagging shard to hold back, so this
    // select is not a frontier block.
    Frame out{};
    EXPECT_FALSE(buffer.try_select(out));

    (void)producers.producers[0].push(make_frame(1, 0, "s0-a", /*logical_tick=*/10));
    (void)producers.producers[1].push(make_frame(2, 1, "s1-a", /*logical_tick=*/5));
    ASSERT_TRUE(buffer.try_select(out));

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

    producers.producers[0].close_graceful();
    EXPECT_FALSE(buffer.try_select(out));
    EXPECT_FALSE(buffer.try_select(out));

    const auto drain_complete{coderoast::ipc::consumer::ConsumerEvent::kDrainComplete};
    const auto count{
        static_cast<std::size_t>(std::count(events.begin(), events.end(), drain_complete))};
    EXPECT_EQ(count, 1U) << "kDrainComplete must fire exactly once";
}
