//
// Unit coverage for the CausalShmConsumer facade: end-to-end causal ordering
// across shards with EOS, and the aggregate ConsumerMetrics snapshot across
// all three sub-stages.
//
// Split by domain from the former test_causal_shm_consumer.cpp (pure moves;
// TEST bodies unchanged). Shared fixtures: causal_pipeline_test_harness.hpp.

#include <gtest/gtest.h>

import coderoast.ipc.consumer.test; // std + the facade + core (the test aggregate)

#include "causal_pipeline_test_harness.hpp"

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
