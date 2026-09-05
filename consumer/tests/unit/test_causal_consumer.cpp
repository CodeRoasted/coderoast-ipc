
#include <gtest/gtest.h>

import coderoast.ipc.consumer.test;

#include "causal_pipeline_test_harness.hpp"

TEST(CausalShmConsumer, EndToEndCausalOrderingAcrossShardsWithEos)
{
    constexpr std::size_t kShardCount{2};
    ProducerHarness producers{"e2e", kShardCount};

    (void)producers.producers[0].push(make_frame(1, 0, "t10", /*logical_tick=*/10));
    (void)producers.producers[1].push(make_frame(2, 1, "t20", /*logical_tick=*/20));
    (void)producers.producers[0].push(make_frame(3, 0, "t30", /*logical_tick=*/30));
    (void)producers.producers[1].push(make_frame(4, 1, "t40", /*logical_tick=*/40));
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

// refs: ADR-11.D3
TEST(ConsumerMetrics, FacadeAggregatesAllStages)
{
    ProducerHarness producers{"facade_metrics", 1};
    Consumer consumer{Consumer::Config{.channel = producers.base, .shard_count = 1}};

    // assert: the distinct ticks are load-bearing — a fixture leaving tick, agent_order and
    // intra_agent_index at zero gives its frames no causal identity at all.
    (void)producers.producers[0].push(make_frame(1, 0, "alpha", /*logical_tick=*/10));
    (void)producers.producers[0].push(
        make_frame(2, 0, "", /*logical_tick=*/20, 0, 0, Flags::kLineFrameFlagWindowSeal));
    (void)producers.producers[0].push(make_frame(3, 0, "beta", /*logical_tick=*/30));
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
    EXPECT_GE(metrics.reorder.selects_succeeded, 3U);
}
