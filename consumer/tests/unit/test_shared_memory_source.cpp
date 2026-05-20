// NOLINTBEGIN : Unit tests may intentionally violate some style rules for clarity or simplicity.
#include <cstring>
#include <string>
#include <unistd.h>

#include <gtest/gtest.h>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/consumer/ordered_line_frame_iterator.hpp"
#include "coderoast/ipc/consumer/shared_memory_source.hpp"

namespace
{
using Frame = coderoast::ipc::DefaultLineFrame;
using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

[[nodiscard]] std::string unique_channel(const char* suffix)
{
    return std::string{"coderoast_consumer_test_"} + suffix + "_" + std::to_string(::getpid());
}

[[nodiscard]] Frame make_frame(
    std::uint64_t sequence, std::uint32_t shard_id, const char* payload,
    coderoast::ipc::LineFrameFlags flags = coderoast::ipc::LineFrameFlags::kLineFrameFlagNone)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.shard_id = shard_id;
    frame.header.shard_sequence = 1;
    frame.header.payload_size = static_cast<std::uint32_t>(std::strlen(payload));
    frame.header.flags = flags;
    std::memcpy(frame.payload.data(), payload, frame.header.payload_size);
    return frame;
}

[[nodiscard]] std::string payload_of(std::string_view view)
{
    return std::string{view};
}
} // namespace

TEST(SharedMemorySource, PopsFramesAsSingleShardSource)
{
    const auto base_name{unique_channel("single")};
    const auto shard_name{base_name + "_shard_0"};

    auto producer{
        Channel::create(coderoast::ipc::ChannelConfig{.name = shard_name, .slot_count = 8})};

    coderoast::ipc::consumer::SharedMemorySource<> source{
        coderoast::ipc::consumer::SharedMemorySource<>::Config{
            .channel = base_name,
            .shard_count = 1,
        }};

    (void)producer.push(make_frame(1, 0, "hello"));
    (void)producer.push(make_frame(2, 0, "world"));

    std::string_view payload;
    ASSERT_TRUE(source.try_pop(payload));
    EXPECT_EQ(payload_of(payload), "hello");

    ASSERT_TRUE(source.try_pop(payload));
    EXPECT_EQ(payload_of(payload), "world");

    Channel::unlink(shard_name);
}

TEST(OrderedLineFrameIterator, ConsumesOrderedFrames)
{
    const auto base_name{unique_channel("ordered")};
    const auto shard_name{base_name + "_shard_0"};

    auto producer{
        Channel::create(coderoast::ipc::ChannelConfig{.name = shard_name, .slot_count = 8})};

    coderoast::ipc::consumer::OrderedLineFrameIterator<> iterator{
        typename coderoast::ipc::consumer::OrderedLineFrameIterator<>::Config{
            .channel = base_name,
            .shard_count = 1,
            .first_sequence = 1,
        }};

    (void)producer.push(make_frame(1, 0, "first"));
    (void)producer.push(make_frame(2, 0, "second"));

    Frame out{};
    ASSERT_TRUE(iterator.try_next(out));
    EXPECT_EQ(out.header.sequence, 1U);

    ASSERT_TRUE(iterator.try_next(out));
    EXPECT_EQ(out.header.sequence, 2U);

    Channel::unlink(shard_name);
}

TEST(OrderedLineFrameIterator, TransportSequenceDrainsAheadOfMissingFrame)
{
    const auto base_name{unique_channel("transport_drain")};
    const auto shard0_name{base_name + "_shard_0"};
    const auto shard1_name{base_name + "_shard_1"};

    auto producer0{
        Channel::create(coderoast::ipc::ChannelConfig{.name = shard0_name, .slot_count = 8})};
    auto producer1{
        Channel::create(coderoast::ipc::ChannelConfig{.name = shard1_name, .slot_count = 8})};

    coderoast::ipc::consumer::OrderedLineFrameIterator<> iterator{
        typename coderoast::ipc::consumer::OrderedLineFrameIterator<>::Config{
            .channel = base_name,
            .shard_count = 2,
            .first_sequence = 1,
            .ordering = coderoast::ipc::consumer::FrameOrdering::TransportSequence,
        }};

    (void)producer1.push(make_frame(2, 1, "second"));

    Frame out{};
    EXPECT_FALSE(iterator.try_next(out));

    const auto stats_after_gap{iterator.channel_stats()};
    ASSERT_EQ(stats_after_gap.size(), 2U);
    EXPECT_EQ(stats_after_gap[1].popped, 1U);

    (void)producer0.push(make_frame(1, 0, "first"));

    ASSERT_TRUE(iterator.try_next(out));
    EXPECT_EQ(out.header.sequence, 1U);
    ASSERT_TRUE(iterator.try_next(out));
    EXPECT_EQ(out.header.sequence, 2U);

    (void)producer0.push(make_frame(3, 0, "", coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream));
    (void)producer1.push(make_frame(4, 1, "", coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream));

    ASSERT_TRUE(iterator.try_next(out));
    EXPECT_TRUE(coderoast::ipc::has_flag(
        out.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream));
    ASSERT_TRUE(iterator.try_next(out));
    EXPECT_TRUE(coderoast::ipc::has_flag(
        out.header.flags, coderoast::ipc::LineFrameFlags::kLineFrameFlagEndOfStream));
    EXPECT_TRUE(iterator.all_shards_done());

    Channel::unlink(shard0_name);
    Channel::unlink(shard1_name);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
// NOLINTEND : Unit tests may intentionally violate some style rules for clarity
