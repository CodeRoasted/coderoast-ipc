// NOLINTBEGIN : Unit tests may intentionally violate some style rules for clarity or simplicity.
#include <gtest/gtest.h>

import coderoast.ipc.producer.test; // std + the facade + core (§11.9.11 aggregate)

using FrameBuilder = coderoast::ipc::producer::FrameBuilder<>;

TEST(FrameBuilder, BuildsFrameWithIncrementingGlobalSequence)
{
    FrameBuilder builder{FrameBuilder::Config{.shard_count = 1, .first_sequence = 1}};

    auto frame1 = builder.build(0, 1000, 64, 12345);
    EXPECT_EQ(frame1.header.sequence, 1U);
    EXPECT_EQ(frame1.header.shard_sequence, 1U);

    auto frame2 = builder.build(0, 2000, 128, 12345);
    EXPECT_EQ(frame2.header.sequence, 2U);
    EXPECT_EQ(frame2.header.shard_sequence, 2U);
}

TEST(FrameBuilder, TracksSeparatePerShardSequences)
{
    FrameBuilder builder{FrameBuilder::Config{.shard_count = 2, .first_sequence = 1}};

    auto frame_shard0 = builder.build(0, 1000, 64, 12345);
    EXPECT_EQ(frame_shard0.header.shard_id, 0U);
    EXPECT_EQ(frame_shard0.header.shard_sequence, 1U);

    auto frame_shard1 = builder.build(1, 1000, 64, 12345);
    EXPECT_EQ(frame_shard1.header.shard_id, 1U);
    EXPECT_EQ(frame_shard1.header.shard_sequence, 1U);

    auto frame_shard0_2 = builder.build(0, 1000, 64, 12345);
    EXPECT_EQ(frame_shard0_2.header.shard_id, 0U);
    EXPECT_EQ(frame_shard0_2.header.shard_sequence, 2U);
}

TEST(FrameBuilder, ModulosShardIdAgainstShardCount)
{
    FrameBuilder builder{FrameBuilder::Config{.shard_count = 3, .first_sequence = 1}};

    auto frame = builder.build(5, 1000, 64, 12345);
    EXPECT_EQ(frame.header.shard_id, 5 % 3);
}

TEST(StableAgentId, HasConsistentHashForSameName)
{
    const auto id1 = coderoast::ipc::producer::stable_agent_id("my_app");
    const auto id2 = coderoast::ipc::producer::stable_agent_id("my_app");
    EXPECT_EQ(id1, id2);
}

TEST(StableAgentId, DifferentNamesProduceDifferentHashes)
{
    const auto id1 = coderoast::ipc::producer::stable_agent_id("app_a");
    const auto id2 = coderoast::ipc::producer::stable_agent_id("app_b");
    EXPECT_NE(id1, id2);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// NOLINTEND : Unit tests may intentionally violate some style rules for clarity
