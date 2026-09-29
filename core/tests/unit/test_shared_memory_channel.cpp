#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cerrno>

#include <gtest/gtest.h>

import coderoast.ipc.core.test;

namespace
{
using Frame = coderoast::ipc::LineFrame<64>;

// refs: DN-103.D29
// invariant: the seal grid these channels declare; nothing here admits by it, so any non-zero one.
constexpr coderoast::ipc::SealGrid kAnyGrid{
    .origin_unix_ns = 0U, .window_length_ns = 1U, .frontier_step_ns = 1U};

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
        coderoast::ipc::ChannelConfig{.name = name, .slot_count = 8, .seal_grid = kAnyGrid})};
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
}

TEST(SharedMemorySpscChannel, DropNewestCountsRejectedFrames)
{
    const auto name{unique_channel("drop")};
    auto producer{
        coderoast::ipc::SharedMemorySpscChannel<Frame>::create(coderoast::ipc::ChannelConfig{
            .name = name,
            .slot_count = 1,
            .backpressure = coderoast::ipc::BackpressurePolicy::DropNewest,
            .seal_grid = kAnyGrid})};

    EXPECT_TRUE(producer.push(make_frame(1, "one")));
    EXPECT_FALSE(producer.push(make_frame(2, "two")));
    EXPECT_EQ(producer.stats().dropped, 1U);
}

// refs: ADR-22.D4
TEST(SharedMemoryChannel, ForwardsTheDeclaredIntentChannelToTheConsumer)
{
    const auto name{unique_channel("intent_channel")};
    auto producer{
        coderoast::ipc::SharedMemorySpscChannel<Frame>::create(coderoast::ipc::ChannelConfig{
            .name = name, .slot_count = 4, .intent_channel = "annotated", .seal_grid = kAnyGrid})};
    EXPECT_EQ(producer.intent_channel(), "annotated");

    auto consumer{coderoast::ipc::SharedMemorySpscChannel<Frame>::open(name)};
    EXPECT_EQ(consumer.intent_channel(), "annotated")
        << "the consumer must recover the producer's DECLARED IntentChannel off the header — "
           "without it the SHM path would have to guess the materialization, which is the one "
           "thing a consumer must never do";

    producer.close();
    consumer.close();
}

// refs: ADR-22.D4, ADR-22.D5
TEST(SharedMemoryChannel, UndeclaredIntentChannelIsUnspecifiedNotAConcreteName)
{
    const auto name{unique_channel("intent_channel_none")};
    auto producer{coderoast::ipc::SharedMemorySpscChannel<Frame>::create(
        coderoast::ipc::ChannelConfig{.name = name, .slot_count = 4, .seal_grid = kAnyGrid})};
    auto consumer{coderoast::ipc::SharedMemorySpscChannel<Frame>::open(name)};

    EXPECT_TRUE(producer.intent_channel().empty());
    EXPECT_TRUE(consumer.intent_channel().empty())
        << "an undeclared ring must read back Unspecified. Defaulting the transport to a concrete "
           "channel would hand the consumer a materialization nobody declared — the fail-open the "
           "coordinate exists to close.";

    producer.close();
    consumer.close();
}

// refs: ADR-22.D4
TEST(SharedMemoryChannel, RefusesAnIntentChannelNameThatWouldNotFit)
{
    const auto name{unique_channel("intent_channel_long")};
    const std::string too_long(coderoast::ipc::kIntentChannelNameCapacity, 'x');
    EXPECT_THROW(
        {
            auto ch{coderoast::ipc::SharedMemorySpscChannel<Frame>::create(
                coderoast::ipc::ChannelConfig{.name = name,
                                              .slot_count = 4,
                                              .intent_channel = too_long,
                                              .seal_grid = kAnyGrid})};
        },
        std::invalid_argument)
        << "a name that does not fit must be refused at create, not silently clipped to " +
               std::to_string(coderoast::ipc::kIntentChannelNameCapacity - 1U) + " bytes";
}

// refs: DN-102.D3
TEST(SharedMemoryChannel, RefusesASlotCountWhoseSegmentSizeIsNotRepresentable)
{
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
    const auto name{unique_channel("slot_count_unrepresentable")};
    const std::size_t unrepresentable{std::numeric_limits<std::size_t>::max() / sizeof(Frame) + 1U};
    EXPECT_FALSE(Channel::segment_bytes(unrepresentable).has_value())
        << unrepresentable << " slots × " << sizeof(Frame) << " B does not fit std::size_t";
    EXPECT_THROW(
        {
            auto ch{Channel::create(coderoast::ipc::ChannelConfig{
                .name = name, .slot_count = unrepresentable, .seal_grid = kAnyGrid})};
        },
        std::invalid_argument)
        << "slot_count " << unrepresentable << " × " << sizeof(Frame)
        << " B wraps std::size_t; a create that proceeds maps a segment far smaller than the ring "
           "its header indexes";
}

// invariant: a truncated segment is the shape a corrupt or hostile header takes: its slot count
// indexes past what the mapping holds.
TEST(SharedMemoryChannel, OpenRefusesAHeaderWhoseSlotCountTheMappedSizeCannotHold)
{
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
    constexpr std::size_t kHeaderSlots{4U};
    const auto name{unique_channel("slot_count_past_mapping")};
    const auto producer{Channel::create(coderoast::ipc::ChannelConfig{
        .name = name, .slot_count = kHeaderSlots, .seal_grid = kAnyGrid})};
    const auto claimed{Channel::segment_bytes(kHeaderSlots).value_or(0U)};
    const auto held{Channel::segment_bytes(1U).value_or(0U)};
    {
        const auto path{"/" + name};
        const int descriptor{::shm_open(path.c_str(), O_RDWR, 0)};
        ASSERT_GE(descriptor, 0) << "shm_open('" << path << "'): "
                                 << std::error_code(errno, std::generic_category()).message();
        const bool truncated{::ftruncate(descriptor, static_cast<off_t>(held)) == 0};
        static_cast<void>(::close(descriptor));
        ASSERT_TRUE(truncated) << "ftruncate to " << held << " B failed";
    }

    EXPECT_THROW(
        { const auto consumer{Channel::open(name)}; }, std::runtime_error)
        << "the header claims " << kHeaderSlots << " slots, " << claimed << " B, over a mapping of "
        << held << " B; an open that accepts it pops and pushes past the end of its mapping";
}

// refs: DN-103.D29
// invariant: a consumer's admission divides by the window length and the frontier step, so a
// channel declaring either as zero is refused before any segment exists.
TEST(SharedMemoryChannel, RefusesASealGridWithAZeroWindowOrStep)
{
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
    const auto name{unique_channel("zero_grid")};
    constexpr coderoast::ipc::SealGrid kNoWindow{
        .origin_unix_ns = 0U, .window_length_ns = 0U, .frontier_step_ns = 1U};
    constexpr coderoast::ipc::SealGrid kNoStep{
        .origin_unix_ns = 0U, .window_length_ns = 1U, .frontier_step_ns = 0U};
    for (const auto& grid : {kNoWindow, kNoStep, coderoast::ipc::SealGrid{}})
    {
        EXPECT_THROW(
            {
                auto channel{Channel::create(coderoast::ipc::ChannelConfig{
                    .name = name, .slot_count = 4, .seal_grid = grid})};
            },
            std::invalid_argument)
            << "window " << grid.window_length_ns << " ns, step " << grid.frontier_step_ns
            << " ns was admitted";
        const auto path{"/" + name};
        const int descriptor{::shm_open(path.c_str(), O_RDONLY, 0)};
        EXPECT_LT(descriptor, 0) << "a refused create left the segment '" << path << "' behind";
        if (descriptor >= 0)
        {
            static_cast<void>(::close(descriptor));
        }
    }
}

// refs: DN-103.D29
TEST(SharedMemoryChannel, TheConsumerReadsTheGridTheProducerDeclared)
{
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
    const auto name{unique_channel("declared_grid")};
    constexpr coderoast::ipc::SealGrid kDeclared{.origin_unix_ns = 1'700'000'000'000'000'000U,
                                                 .window_length_ns = 25'000'000'000U,
                                                 .frontier_step_ns = 1'000'000'000U};
    const auto producer{Channel::create(
        coderoast::ipc::ChannelConfig{.name = name, .slot_count = 4, .seal_grid = kDeclared})};
    const auto consumer{Channel::open(name)};
    EXPECT_EQ(producer.seal_grid(), kDeclared);
    EXPECT_EQ(consumer.seal_grid(), kDeclared)
        << "the consumer read origin " << consumer.seal_grid().origin_unix_ns << " ns, window "
        << consumer.seal_grid().window_length_ns << " ns, step "
        << consumer.seal_grid().frontier_step_ns << " ns off the header";
}

// refs: DN-103.D29
// invariant: version 7 added the seal grid to the header, so a mapping stamped 6 is refused.
TEST(SharedMemoryChannel, OpenRefusesAVersionSixHeader)
{
    using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
    static_assert(coderoast::ipc::kSharedChannelAbiVersion == 7U);
    const auto name{unique_channel("abi_six")};
    const auto producer{Channel::create(
        coderoast::ipc::ChannelConfig{.name = name, .slot_count = 4, .seal_grid = kAnyGrid})};
    {
        const auto path{"/" + name};
        const int descriptor{::shm_open(path.c_str(), O_RDWR, 0)};
        ASSERT_GE(descriptor, 0) << "shm_open('" << path << "'): "
                                 << std::error_code(errno, std::generic_category()).message();
        constexpr std::uint32_t kVersionSix{6U};
        // invariant: the header opens with the 8-byte magic, then the 4-byte version.
        constexpr ::off_t kVersionOffset{sizeof(coderoast::ipc::kSharedChannelMagic)};
        const auto written{::pwrite(descriptor, &kVersionSix, sizeof(kVersionSix), kVersionOffset)};
        static_cast<void>(::close(descriptor));
        ASSERT_EQ(written, static_cast<::ssize_t>(sizeof(kVersionSix)));
    }
    EXPECT_THROW(
        { const auto consumer{Channel::open(name)}; }, std::runtime_error)
        << "a header stamped with ABI version 6 was opened by a version 7 consumer";
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
