
#include <gtest/gtest.h>

import coderoast.ipc.consumer.test;

#include "causal_pipeline_test_harness.hpp"

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

    EXPECT_FALSE(drainer.try_pull(0, out));
    EXPECT_TRUE(drainer.shard_eos(0));
    EXPECT_TRUE(drainer.transport_complete());

    EXPECT_FALSE(drainer.try_pull(0, out));
}

TEST(ShmTransportDrainer, SealFrameSurfacesToCaller)
{
    ProducerHarness producers{"seal", 1};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 1}};

    (void)producers.producers[0].push(
        make_frame(1, 0, "", 0, 0, 0, Flags::kLineFrameFlagWindowSeal));

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
    // assert: the four pulls are data, seal, the absorbed EOS, and the post-EOS fast path.
    EXPECT_TRUE(drainer.try_pull(0, out));
    EXPECT_TRUE(drainer.try_pull(0, out));
    EXPECT_FALSE(drainer.try_pull(0, out));
    EXPECT_FALSE(drainer.try_pull(0, out));

    const auto metrics{drainer.metrics()};
    EXPECT_EQ(metrics.pulls_attempted, 4U);
    // assert: pulls_succeeded counts only what reached the caller — the data and the seal.
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
