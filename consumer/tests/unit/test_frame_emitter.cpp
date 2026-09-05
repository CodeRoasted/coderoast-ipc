
#include <gtest/gtest.h>

import coderoast.ipc.consumer.test;

#include "causal_pipeline_test_harness.hpp"

TEST(CausalShmConsumer, ControlFramesAreFilteredByDefault)
{
    ProducerHarness producers{"ctl", 1};

    (void)producers.producers[0].push(make_frame(1, 0, "data1", /*logical_tick=*/10));
    (void)producers.producers[0].push(
        make_frame(2, 0, "", /*logical_tick=*/15, 0, 0, Flags::kLineFrameFlagWindowSeal));
    (void)producers.producers[0].push(make_frame(3, 0, "data2", /*logical_tick=*/20));
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
