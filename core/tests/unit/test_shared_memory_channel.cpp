// NOLINTBEGIN : Unit tests may intentionally violate some style rules for clarity or simplicity.
#include <unistd.h>

#include <gtest/gtest.h>

import coderoast.ipc.core.test; // std + the facade (§11.9.11 aggregate)

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

// ── The transported IntentChannel (ADR-22) ─────────────────────────────────────────────────────
// The ring ENCAPSULATES an IntentChannel: the producer declares it at create(), the consumer reads
// it off the header at open(). This is the seam that lets the SHM path be exactly as informed as
// the real
// `--channel` path — no more (it forwards a declaration, it does not invent one) and no less (the
// consumer never has to be told out-of-band what the stream already carries).
//
// It is CHANNEL-level, not per-frame, on purpose: one IntentChannel per TREE is the contract, and a
// per-frame field would *permit* the multi-channel tree that contract forbids.
TEST(SharedMemoryChannel, ForwardsTheDeclaredIntentChannelToTheConsumer)
{
    const auto name{unique_channel("intent_channel")};
    auto producer{
        coderoast::ipc::SharedMemorySpscChannel<Frame>::create(coderoast::ipc::ChannelConfig{
            .name = name, .slot_count = 4, .intent_channel = "annotated"})};
    EXPECT_EQ(producer.intent_channel(), "annotated");

    auto consumer{coderoast::ipc::SharedMemorySpscChannel<Frame>::open(name)};
    EXPECT_EQ(consumer.intent_channel(), "annotated")
        << "the consumer must recover the producer's DECLARED IntentChannel off the header — "
           "without "
           "it the SHM path would have to guess the materialization, which is the one thing a "
           "consumer must never do (ADR-22)";

    producer.close();
    consumer.close();
    coderoast::ipc::SharedMemorySpscChannel<Frame>::unlink(name);
}

// An undeclared ring reads back EMPTY = Unspecified — never a concrete channel. This is the
// default, and it is what keeps every non-dialect stream (the ~20 pure formats) out of the blast
// radius.
TEST(SharedMemoryChannel, UndeclaredIntentChannelIsUnspecifiedNotAConcreteName)
{
    const auto name{unique_channel("intent_channel_none")};
    auto producer{coderoast::ipc::SharedMemorySpscChannel<Frame>::create(
        coderoast::ipc::ChannelConfig{.name = name, .slot_count = 4})};
    auto consumer{coderoast::ipc::SharedMemorySpscChannel<Frame>::open(name)};

    EXPECT_TRUE(producer.intent_channel().empty());
    EXPECT_TRUE(consumer.intent_channel().empty())
        << "an undeclared ring must read back Unspecified. Defaulting the transport to a concrete "
           "channel would hand the consumer a materialization nobody declared — the fail-open the "
           "coordinate exists to close.";

    producer.close();
    consumer.close();
    coderoast::ipc::SharedMemorySpscChannel<Frame>::unlink(name);
}

// An over-long name is REFUSED, never truncated: a clipped name would reach the consumer as a
// different string — failing its vocabulary check far from the cause, or silently aliasing another
// declared name and mis-gating recognition.
TEST(SharedMemoryChannel, RefusesAnIntentChannelNameThatWouldNotFit)
{
    const auto name{unique_channel("intent_channel_long")};
    const std::string too_long(coderoast::ipc::kIntentChannelNameCapacity, 'x');
    EXPECT_THROW(
        {
            auto ch{coderoast::ipc::SharedMemorySpscChannel<Frame>::create(
                coderoast::ipc::ChannelConfig{
                    .name = name, .slot_count = 4, .intent_channel = too_long})};
        },
        std::invalid_argument)
        << "a name that does not fit must be refused at create, not silently clipped to " +
               std::to_string(coderoast::ipc::kIntentChannelNameCapacity - 1U) + " bytes";
    coderoast::ipc::SharedMemorySpscChannel<Frame>::unlink(name);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
// NOLINTEND : Unit tests may intentionally violate some style rules for clarity
