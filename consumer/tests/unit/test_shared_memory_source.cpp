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

[[nodiscard]] Frame make_frame(std::uint64_t sequence, std::uint32_t shard_id,
                               const char* payload)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.shard_id = shard_id;
    frame.header.shard_sequence = 1;
    frame.header.payload_size = static_cast<std::uint32_t>(std::strlen(payload));
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

    insight::ingest::SharedMemorySource<> source{insight::ingest::SharedMemorySource<>::Config{
        .channel = base_name,
        .shard_count = 1,
    }};

    producer.push(make_frame(1, 0, "hello"));
    producer.push(make_frame(2, 0, "world"));

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

    insight::ingest::OrderedLineFrameIterator<> iterator{
        typename insight::ingest::OrderedLineFrameIterator<>::Config{
            .channel = base_name,
            .shard_count = 1,
            .first_sequence = 1,
        }};

    producer.push(make_frame(1, 0, "first"));
    producer.push(make_frame(2, 0, "second"));

    Frame out{};
    ASSERT_TRUE(iterator.try_next(out));
    EXPECT_EQ(out.header.sequence, 1U);

    ASSERT_TRUE(iterator.try_next(out));
    EXPECT_EQ(out.header.sequence, 2U);

    Channel::unlink(shard_name);
}
// NOLINTEND : Unit tests may intentionally violate some style rules for clarity
