// NOLINTBEGIN : Unit tests intentionally favour clarity over style.
//
// Coverage for the WindowClosedConsumer coalescing adapter.
//
// HOMING NOTE (Kleio) — do NOT add a "per-window frame membership" test here.
// It cannot fail at this layer, and a can't-fail gate is worse than no gate.
// WindowClosed(K) is synthesized only on the Nth shard's seal for window K
// (`seal_counts_[K] >= shard_count_`), and each shard's channel is SPSC-FIFO, so a
// shard's window-K data necessarily precedes its own window-K seal, which necessarily
// precedes WindowClosed(K). Membership is implied by the adapter's own structure —
// asserting it here makes the SUT its own oracle.
//
// The invariant IS falsifiable one layer up, on the producer, where the seal watermark
// is applied only once the shard ring has drained (logcraft `sharded_pipeline.cpp`,
// `apply_seal_if_due()` inside the `depth == 0` branch). It is pinned there, end-to-end
// over the real transport under real backpressure, by
// `ShmGate.DataPrecedesSealPerShardUnderBackpressure`
// (logcraft/core/tests/determinism/test_determinism_shm_gate.cpp) — verified by mutation:
// hoisting that call out of the drained branch reddens it. Seal completeness at a
// PlayToTarget freeze is pinned by `ShmGate.WindowSealsCompleteUnderPlayToTargetWithoutStop`;
// the closed-window count for a fixed target by
// `CodeRoastServerTestSuite.InsightBarrierClosesEachWindowExactlyOnce` (coderoast-server).

#include <unistd.h>

#include <gtest/gtest.h>

import coderoast.ipc.consumer.test; // std + the facade + core (§11.9.11 aggregate)

namespace
{
using Frame = coderoast::ipc::DefaultLineFrame;
using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
using Drainer = coderoast::ipc::consumer::ShmTransportDrainer<Frame>;
using WindowClosedConsumer = coderoast::ipc::consumer::WindowClosedConsumer<Frame>;
using Flags = coderoast::ipc::LineFrameFlags;

[[nodiscard]] std::string unique_channel(const char* suffix)
{
    return std::string{"coderoast_window_closed_test_"} + suffix + "_" + std::to_string(::getpid());
}

[[nodiscard]] Frame make_data(std::uint64_t sequence, std::uint32_t shard_id,
                              std::uint64_t logical_tick, const char* payload)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.shard_id = shard_id;
    frame.header.shard_sequence = sequence;
    frame.header.logical_tick = logical_tick;
    frame.header.flags = Flags::kLineFrameFlagNone;
    frame.header.payload_size = static_cast<std::uint32_t>(std::strlen(payload));
    std::memcpy(frame.payload.data(), payload, frame.header.payload_size);
    return frame;
}

[[nodiscard]] Frame make_seal(std::uint64_t sequence, std::uint32_t shard_id,
                              std::uint64_t window_id, std::uint64_t logical_tick)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.shard_id = shard_id;
    frame.header.shard_sequence = sequence;
    frame.header.logical_tick = logical_tick;
    frame.header.window_id = window_id;
    frame.header.flags = Flags::kLineFrameFlagWindowSeal;
    frame.header.payload_size = 0;
    return frame;
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
                .name = coderoast::ipc::shard_channel_name(base, shard_id),
                .slot_count = 16,
            }));
        }
    }

    ~ProducerHarness()
    {
        for (std::size_t shard_id{0}; shard_id < producers.size(); ++shard_id)
        {
            Channel::unlink(coderoast::ipc::shard_channel_name(base, shard_id));
        }
    }

    ProducerHarness(const ProducerHarness&) = delete;
    ProducerHarness& operator=(const ProducerHarness&) = delete;
    ProducerHarness(ProducerHarness&&) = delete;
    ProducerHarness& operator=(ProducerHarness&&) = delete;
};
} // namespace

TEST(WindowClosedConsumer, EmitsOneEventPerWindowAcrossShards)
{
    constexpr std::size_t kShardCount{4};
    constexpr std::uint64_t kWindow0Tick{1000};
    constexpr std::uint64_t kWindow1Tick{2000};

    ProducerHarness producers{"coalesce", kShardCount};

    // Push one data frame + one seal for window 0 on each shard, then close.
    for (std::uint32_t shard{0}; shard < kShardCount; ++shard)
    {
        (void)producers.producers[shard].push(make_data(1, shard, 100 + shard, "d0"));
        (void)producers.producers[shard].push(make_seal(2, shard, 0, kWindow0Tick));
        (void)producers.producers[shard].push(make_data(3, shard, kWindow0Tick + 10, "d1"));
        (void)producers.producers[shard].push(make_seal(4, shard, 1, kWindow1Tick));
        producers.producers[shard].close_graceful();
    }

    WindowClosedConsumer consumer{WindowClosedConsumer::Config{
        .underlying = {.channel = producers.base, .shard_count = kShardCount}}};

    std::vector<std::uint64_t> closed_windows;
    std::size_t data_frames_seen{0};

    Frame frame{};
    WindowClosedConsumer::WindowClosed wc{};
    for (int spin{0}; spin < 5000; ++spin)
    {
        const auto kind{consumer.try_next(frame, wc)};
        if (kind == WindowClosedConsumer::NextKind::kFrame)
        {
            ++data_frames_seen;
            continue;
        }
        if (kind == WindowClosedConsumer::NextKind::kWindowClosed)
        {
            closed_windows.push_back(wc.window_id);
            continue;
        }
        if (consumer.all_shards_done())
        {
            break;
        }
    }

    // Exactly 2 windows × 4 shards = 8 raw seals were swallowed; 2 events emitted.
    ASSERT_EQ(closed_windows.size(), 2U);
    EXPECT_EQ(closed_windows[0], 0U);
    EXPECT_EQ(closed_windows[1], 1U);
    EXPECT_EQ(consumer.windows_closed(), 2U);
    EXPECT_EQ(consumer.seals_observed(), 2U * kShardCount);

    // Each shard sent 2 data frames → 8 total.
    EXPECT_EQ(data_frames_seen, 2U * kShardCount);
    EXPECT_EQ(consumer.frames_emitted(), data_frames_seen);
}

TEST(WindowClosedConsumer, DataFrameBeforeWindowClosedRespectsCausalOrder)
{
    constexpr std::size_t kShardCount{2};
    constexpr std::uint64_t kSealTick{500};

    ProducerHarness producers{"order", kShardCount};

    // Each shard: one data frame BEFORE the seal, one AFTER.
    for (std::uint32_t shard{0}; shard < kShardCount; ++shard)
    {
        (void)producers.producers[shard].push(make_data(1, shard, kSealTick - 50, "pre"));
        (void)producers.producers[shard].push(make_seal(2, shard, 7, kSealTick));
        (void)producers.producers[shard].push(make_data(3, shard, kSealTick + 50, "post"));
        producers.producers[shard].close_graceful();
    }

    WindowClosedConsumer consumer{WindowClosedConsumer::Config{
        .underlying = {.channel = producers.base, .shard_count = kShardCount}}};

    bool saw_window_close{false};
    std::size_t pre_seal_data{0};
    std::size_t post_seal_data{0};

    Frame frame{};
    WindowClosedConsumer::WindowClosed wc{};
    for (int spin{0}; spin < 5000; ++spin)
    {
        const auto kind{consumer.try_next(frame, wc)};
        if (kind == WindowClosedConsumer::NextKind::kFrame)
        {
            if (saw_window_close)
                ++post_seal_data;
            else
                ++pre_seal_data;
            continue;
        }
        if (kind == WindowClosedConsumer::NextKind::kWindowClosed)
        {
            EXPECT_EQ(wc.window_id, 7U);
            EXPECT_EQ(wc.logical_tick, kSealTick);
            saw_window_close = true;
            continue;
        }
        if (consumer.all_shards_done())
        {
            break;
        }
    }

    EXPECT_TRUE(saw_window_close);
    EXPECT_EQ(pre_seal_data, kShardCount);
    EXPECT_EQ(post_seal_data, kShardCount);
}

// NOLINTEND
