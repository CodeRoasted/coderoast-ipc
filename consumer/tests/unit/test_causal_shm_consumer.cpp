// NOLINTBEGIN : Unit tests intentionally favour clarity over style.
//
// Unit coverage for the single-threaded pull-based causal SHM consumer
// pipeline introduced by commit 335f9f6 ("collapse SHM drainer to
// single-threaded pull pipeline"). These tests pin the externally
// observable contract of:
//
//   * ShmTransportDrainer   — EOS absorption, ever_had_data semantics,
//                              transport_complete, channel_stats.
//   * CausalReorderBuffer   — frontier gating, drained predicate,
//                              shard_summaries.
//   * CausalShmConsumer     — end-to-end causal ordering + control-frame
//                              filtering across multiple shards.
//
// The tests use real SHM channels (via SharedMemorySpscChannel) and
// unique PID-suffixed names so they are safe under parallel ctest.

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

#include <gtest/gtest.h>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/consumer/causal_reorder_buffer.hpp"
#include "coderoast/ipc/consumer/causal_shm_consumer.hpp"
#include "coderoast/ipc/consumer/frame_emitter.hpp"
#include "coderoast/ipc/consumer/scoped_shm_channel_set.hpp"
#include "coderoast/ipc/consumer/shm_transport_drainer.hpp"
#include "coderoast/ipc/frame.hpp"

namespace
{
using Frame = coderoast::ipc::DefaultLineFrame;
using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
using Drainer = coderoast::ipc::consumer::ShmTransportDrainer<Frame>;
using Buffer = coderoast::ipc::consumer::CausalReorderBuffer<Frame>;
using Emitter = coderoast::ipc::consumer::FrameEmitter<Frame>;
using Consumer = coderoast::ipc::consumer::CausalShmConsumer<Frame>;
using Flags = coderoast::ipc::LineFrameFlags;

[[nodiscard]] std::string unique_channel(const char* suffix)
{
    return std::string{"coderoast_drainer_test_"} + suffix + "_" + std::to_string(::getpid());
}

[[nodiscard]] Frame make_frame(std::uint64_t sequence, std::uint32_t shard_id, const char* payload,
                               std::uint64_t logical_tick = 0, std::uint32_t agent_order = 0,
                               std::uint32_t intra_agent_index = 0,
                               Flags flags = Flags::kLineFrameFlagNone)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.shard_id = shard_id;
    frame.header.shard_sequence = sequence;
    frame.header.logical_tick = logical_tick;
    frame.header.agent_order = agent_order;
    frame.header.intra_agent_index = intra_agent_index;
    frame.header.flags = flags;
    frame.header.payload_size = static_cast<std::uint32_t>(std::strlen(payload));
    std::memcpy(frame.payload.data(), payload, frame.header.payload_size);
    return frame;
}

[[nodiscard]] std::string payload_of(const Frame& frame)
{
    return std::string{// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                       reinterpret_cast<const char*>(frame.payload.data()),
                       frame.header.payload_size};
}

struct ProducerHarness
{
    std::string base;
    std::vector<Channel> producers;

    ProducerHarness(const char* suffix, std::size_t shard_count) : base{unique_channel(suffix)}
    {
        producers.reserve(shard_count);
        for (std::size_t shard_id{0}; shard_id < shard_count; ++shard_id)
        {
            producers.emplace_back(Channel::create(coderoast::ipc::ChannelConfig{
                .name = Drainer::shard_channel_name(base, shard_id),
                .slot_count = 16,
            }));
        }
    }

    ~ProducerHarness()
    {
        for (std::size_t shard_id{0}; shard_id < producers.size(); ++shard_id)
        {
            Channel::unlink(Drainer::shard_channel_name(base, shard_id));
        }
    }

    ProducerHarness(const ProducerHarness&) = delete;
    ProducerHarness& operator=(const ProducerHarness&) = delete;
    ProducerHarness(ProducerHarness&&) = delete;
    ProducerHarness& operator=(ProducerHarness&&) = delete;
};
} // namespace

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
    (void)producers.producers[0].push(
        make_frame(2, 0, "", 0, 0, 0, Flags::kLineFrameFlagEndOfStream));

    Frame out{};
    ASSERT_TRUE(drainer.try_pull(0, out));
    EXPECT_EQ(payload_of(out), "data");
    EXPECT_TRUE(drainer.shard_ever_had_data(0));
    EXPECT_FALSE(drainer.shard_eos(0));

    // Second pull pops the EOS marker; try_pull must NOT surface it.
    EXPECT_FALSE(drainer.try_pull(0, out));
    EXPECT_TRUE(drainer.shard_eos(0));
    EXPECT_TRUE(drainer.transport_complete());

    // Further pulls must remain false (idempotent EOS).
    EXPECT_FALSE(drainer.try_pull(0, out));
}

TEST(ShmTransportDrainer, SealFrameSurfacesButDoesNotSetEverHadData)
{
    ProducerHarness producers{"seal", 1};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 1}};

    (void)producers.producers[0].push(
        make_frame(1, 0, "", 0, 0, 0, Flags::kLineFrameFlagWindowSeal));

    Frame out{};
    ASSERT_TRUE(drainer.try_pull(0, out));
    EXPECT_FALSE(drainer.shard_ever_had_data(0));
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

TEST(CausalReorderBuffer, FrontierGatesOnceAllShardsProduced)
{
    ProducerHarness producers{"frontier", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    // Both shards produce before we select: refill pulls both, frontier
    // is satisfied, and the causally-earlier (tick=5) wins.
    (void)producers.producers[0].push(make_frame(1, 0, "s0-a", /*tick=*/10));
    (void)producers.producers[1].push(make_frame(2, 1, "s1-a", /*tick=*/5));

    Frame out{};
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s1-a");

    // After the s1-a pop, shard 1's heap is empty and shard 1 is not EOS,
    // so the frontier gate must block emission of s0-a even though shard 0
    // has a buffered candidate.
    EXPECT_FALSE(buffer.try_select(out));

    // Pushing fresh data to shard 0 does not unblock — shard 1 is still silent.
    (void)producers.producers[0].push(make_frame(3, 0, "s0-b", /*tick=*/20));
    EXPECT_FALSE(buffer.try_select(out));

    // EOS on shard 1 releases the gate; shard 0 frames flow in tick order.
    (void)producers.producers[1].push(
        make_frame(99, 1, "", 0, 0, 0, Flags::kLineFrameFlagEndOfStream));
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s0-a");
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "s0-b");
}

TEST(CausalReorderBuffer, DrainedRequiresAllShardsEosAndEmptyHeaps)
{
    ProducerHarness producers{"drained", 2};
    Drainer drainer{Drainer::Config{.channel = producers.base, .shard_count = 2}};
    Buffer buffer{drainer};

    EXPECT_FALSE(buffer.drained());

    (void)producers.producers[0].push(
        make_frame(1, 0, "", 0, 0, 0, Flags::kLineFrameFlagEndOfStream));
    (void)producers.producers[1].push(make_frame(1, 1, "x", /*tick=*/5));
    (void)producers.producers[1].push(
        make_frame(2, 1, "", 0, 0, 0, Flags::kLineFrameFlagEndOfStream));

    // No refill has happened yet: drainer EOS flags are still unset.
    EXPECT_FALSE(buffer.drained());

    Frame out{};
    // Refill via try_select absorbs both EOS frames and the data frame.
    ASSERT_TRUE(buffer.try_select(out));
    EXPECT_EQ(payload_of(out), "x");

    // Both shards EOS, heaps empty \u2192 drained.
    EXPECT_TRUE(buffer.drained());
}

TEST(CausalShmConsumer, EndToEndCausalOrderingAcrossShardsWithEos)
{
    constexpr std::size_t kShardCount{2};
    ProducerHarness producers{"e2e", kShardCount};

    // Producer interleaves causal keys across shards.
    // shard 0: tick=10, tick=30
    // shard 1: tick=20, tick=40, then EOS
    // Expected emit order by logical_tick: 10, 20, 30, 40.
    (void)producers.producers[0].push(make_frame(1, 0, "t10", /*tick=*/10));
    (void)producers.producers[1].push(make_frame(2, 1, "t20", /*tick=*/20));
    (void)producers.producers[0].push(make_frame(3, 0, "t30", /*tick=*/30));
    (void)producers.producers[1].push(make_frame(4, 1, "t40", /*tick=*/40));
    (void)producers.producers[0].push(
        make_frame(5, 0, "", 0, 0, 0, Flags::kLineFrameFlagEndOfStream));
    (void)producers.producers[1].push(
        make_frame(6, 1, "", 0, 0, 0, Flags::kLineFrameFlagEndOfStream));

    Consumer consumer{Consumer::Config{
        .channel = producers.base,
        .shard_count = kShardCount,
        .backpressure = coderoast::ipc::BackpressurePolicy::Block,
        .wait_strategy = coderoast::ipc::WaitStrategy::SpinYieldPark,
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

TEST(CausalShmConsumer, ControlFramesAreFilteredByDefault)
{
    ProducerHarness producers{"ctl", 1};

    (void)producers.producers[0].push(make_frame(1, 0, "data1", /*tick=*/10));
    (void)producers.producers[0].push(
        make_frame(2, 0, "", /*tick=*/15, 0, 0, Flags::kLineFrameFlagWindowSeal));
    (void)producers.producers[0].push(make_frame(3, 0, "data2", /*tick=*/20));
    (void)producers.producers[0].push(
        make_frame(4, 0, "", 0, 0, 0, Flags::kLineFrameFlagEndOfStream));

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

// NOLINTEND : Unit tests intentionally favour clarity over style.
