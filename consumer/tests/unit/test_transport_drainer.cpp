// NOLINTBEGIN : Unit tests intentionally favour clarity over style.
//
// Unit coverage for ShmTransportDrainer — step 1 of the pull-based causal SHM
// consumer pipeline: EOS absorption, seal surfacing, transport_complete,
// channel_stats — plus its atomic counters and discrete-event observer.
//
// Split by domain from the former test_causal_shm_consumer.cpp (pure moves;
// TEST bodies unchanged). Shared fixtures: causal_pipeline_test_harness.hpp.

#include <gtest/gtest.h>

import coderoast.ipc.consumer.test; // std + the facade + core (§11.9.11 aggregate)

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

// NOLINTEND : Unit tests intentionally favour clarity over style.
