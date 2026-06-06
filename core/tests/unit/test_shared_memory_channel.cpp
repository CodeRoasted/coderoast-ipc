// NOLINTBEGIN : Unit tests may intentionally violate some style rules for clarity or simplicity.
#include <unistd.h>

#include <gtest/gtest.h>

import std;
import coderoast.ipc.core; // §11.9: ipc is a pure module now (headers deleted)

namespace
{
using Frame = coderoast::ipc::LineFrame<64>;

[[nodiscard]] std::string unique_channel(const char* suffix)
{
    return std::string{"coderoast_ipc_test_"} + suffix + "_" + std::to_string(::getpid());
}

[[nodiscard]] Frame make_frame(std::uint64_t sequence, const char* payload)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.payload_size = static_cast<std::uint32_t>(std::strlen(payload));
    std::memcpy(frame.payload.data(), payload, frame.header.payload_size);
    return frame;
}

[[nodiscard]] std::string payload_of(const Frame& frame)
{
    return std::string{reinterpret_cast<const char*>(frame.payload.data()),
                       frame.header.payload_size};
}
} // namespace

TEST(SharedMemorySpscChannel, PushesAndPopsLineFramesInOrder)
{
    const auto name{unique_channel("ordered")};
    auto producer{coderoast::ipc::SharedMemorySpscChannel<Frame>::create(
        coderoast::ipc::ChannelConfig{.name = name, .slot_count = 8})};
    auto consumer{coderoast::ipc::SharedMemorySpscChannel<Frame>::open(name)};

    EXPECT_TRUE(producer.push(make_frame(1, "one")));
    EXPECT_TRUE(producer.push(make_frame(2, "two")));

    Frame out{};
    ASSERT_TRUE(consumer.try_pop(out));
    EXPECT_EQ(out.header.sequence, 1U);
    EXPECT_EQ(payload_of(out), "one");

    ASSERT_TRUE(consumer.try_pop(out));
    EXPECT_EQ(out.header.sequence, 2U);
    EXPECT_EQ(payload_of(out), "two");
    EXPECT_FALSE(consumer.try_pop(out));

    coderoast::ipc::SharedMemorySpscChannel<Frame>::unlink(name);
}

TEST(SharedMemorySpscChannel, DropNewestCountsRejectedFrames)
{
    const auto name{unique_channel("drop")};
    auto producer{
        coderoast::ipc::SharedMemorySpscChannel<Frame>::create(coderoast::ipc::ChannelConfig{
            .name = name,
            .slot_count = 1,
            .backpressure = coderoast::ipc::BackpressurePolicy::DropNewest})};

    EXPECT_TRUE(producer.push(make_frame(1, "one")));
    EXPECT_FALSE(producer.push(make_frame(2, "two")));
    EXPECT_EQ(producer.stats().dropped, 1U);

    coderoast::ipc::SharedMemorySpscChannel<Frame>::unlink(name);
}

TEST(SharedMemorySpscChannel, OverwriteOldestKeepsLatestFrame)
{
    const auto name{unique_channel("overwrite")};
    auto producer{
        coderoast::ipc::SharedMemorySpscChannel<Frame>::create(coderoast::ipc::ChannelConfig{
            .name = name,
            .slot_count = 1,
            .backpressure = coderoast::ipc::BackpressurePolicy::OverwriteOldest})};
    auto consumer{coderoast::ipc::SharedMemorySpscChannel<Frame>::open(name)};

    EXPECT_TRUE(producer.push(make_frame(1, "one")));
    EXPECT_TRUE(producer.push(make_frame(2, "two")));
    EXPECT_EQ(producer.stats().overwritten, 1U);

    Frame out{};
    ASSERT_TRUE(consumer.try_pop(out));
    EXPECT_EQ(out.header.sequence, 2U);
    EXPECT_EQ(payload_of(out), "two");

    coderoast::ipc::SharedMemorySpscChannel<Frame>::unlink(name);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
// NOLINTEND : Unit tests may intentionally violate some style rules for clarity
