// refs: DN-103.D29
// invariant: the frontier contract in miniature: one scripted producer thread per shard pushes its
// frames in key order and seals on a frontier thread's horizon, while the merge consumes beside it.
#include <gtest/gtest.h>

import coderoast.ipc.consumer.test;

#include "causal_pipeline_test_harness.hpp"

namespace
{
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using coderoast::ipc::SealGrid;

constexpr std::uint64_t kOrigin{1000U};
constexpr std::uint64_t kStep{4U};
constexpr std::uint64_t kAlignedWindow{16U};
constexpr std::uint64_t kMisalignedWindow{10U};
constexpr std::uint64_t kSteps{40U};
constexpr std::size_t kRingSlots{4U};
constexpr std::uint32_t kDenseShard{1U};
constexpr std::uint32_t kSparseShard{2U};
constexpr std::size_t kShards{3U};
constexpr std::uint64_t kSparseEvery{7U};
// invariant: more dense frames lie between a misaligned window's end and its frontier point than a
// ring and one held frame can take, so an L short of that point leaves the dense shard blocked.
constexpr std::uint32_t kDensePerTick{4U};
constexpr std::uint32_t kBurstAtWindowEnd{3U};
// invariant: every run ends in well under a second; past this it is stalled and the arm reds.
constexpr auto kRunBound{10s};
// invariant: a stall is a deadlock, never a slow run, so the control waits it out briefly.
constexpr auto kStallBound{2s};
// invariant: a pause after each emitted frame, so the producers run ahead of the merge as an
// unpaced start runs ahead of the analysis.
constexpr auto kThrottle{20us};

enum class FrontierCadence : std::uint8_t
{
    PerStep,
    AtEndOnly,
};

struct Record
{
    std::uint64_t tick{0};
    std::uint32_t index{0};
};

struct Script
{
    SealGrid grid{};
    std::array<std::vector<Record>, kShards> records{};

    [[nodiscard]] std::uint64_t horizon() const noexcept
    {
        return grid.origin_unix_ns + (kSteps * grid.frontier_step_ns);
    }

    [[nodiscard]] std::uint64_t window_of(std::uint64_t tick) const noexcept
    {
        return (tick - grid.origin_unix_ns) / grid.window_length_ns;
    }

    [[nodiscard]] std::uint64_t window_end(std::uint64_t window) const noexcept
    {
        return grid.origin_unix_ns + ((window + 1U) * grid.window_length_ns);
    }
};

// post: the dense shard carries kDensePerTick frames at every tick and a burst at each window's end
// tick, the sparse shard one every kSparseEvery ticks, and shard 0, the silent one, none.
[[nodiscard]] Script make_script(std::uint64_t window_length)
{
    Script script{.grid = SealGrid{.origin_unix_ns = kOrigin,
                                   .window_length_ns = window_length,
                                   .frontier_step_ns = kStep},
                  .records = {}};
    std::uint32_t dense_index{0};
    std::uint32_t sparse_index{0};
    for (std::uint64_t tick{kOrigin + 1U}; tick <= script.horizon(); ++tick)
    {
        const bool window_end{(tick - kOrigin) % window_length == 0U};
        const std::uint32_t copies{kDensePerTick + (window_end ? kBurstAtWindowEnd : 0U)};
        for (std::uint32_t copy{0}; copy < copies; ++copy)
            script.records[kDenseShard].push_back(Record{.tick = tick, .index = dense_index++});
        if ((tick - kOrigin) % kSparseEvery == 0U)
            script.records[kSparseShard].push_back(Record{.tick = tick, .index = sparse_index++});
    }
    return script;
}

[[nodiscard]] Frame data_frame(std::uint32_t shard, const Record& record)
{
    return make_frame(record.index + 1U, shard, "d", record.tick, /*agent_order=*/shard + 1U,
                      record.index);
}

[[nodiscard]] Frame seal_frame(std::uint32_t shard, const Script& script, std::uint64_t window)
{
    auto frame{make_frame(window + 1U, shard, "", script.window_end(window), 0U, 0U,
                          Flags::kLineFrameFlagWindowSeal)};
    frame.header.window_id = window;
    return frame;
}

struct Key
{
    std::uint64_t tick{0};
    std::uint32_t agent_order{0};
    std::uint32_t index{0};
    std::uint32_t shard{0};

    [[nodiscard]] auto operator<=>(const Key&) const = default;
};

[[nodiscard]] Key key_of(const Frame& frame)
{
    return Key{.tick = frame.header.logical_tick,
               .agent_order = frame.header.agent_order,
               .index = frame.header.intra_agent_index,
               .shard = frame.header.shard_id};
}

// post: every frame the script's shards put on the wire, data and seals through the horizon, in
// causal order.
[[nodiscard]] std::vector<Key> sorted_union(const Script& script)
{
    std::vector<Key> keys;
    const std::uint64_t sealed_windows{(script.horizon() - kOrigin) / script.grid.window_length_ns};
    for (std::uint32_t shard{0}; shard < kShards; ++shard)
    {
        for (const Record& record : script.records[shard])
            keys.push_back(key_of(data_frame(shard, record)));
        for (std::uint64_t window{0}; window < sealed_windows; ++window)
            keys.push_back(key_of(seal_frame(shard, script, window)));
    }
    std::ranges::sort(keys);
    return keys;
}

// post: F = W⁺ + shards × (2 + ceil(e / I)), W⁺ counted over the script's own data frames: per
// window, those with a tick from its start through the first frontier point at or past its end.
[[nodiscard]] std::uint64_t frame_bound(const Script& script)
{
    const std::uint64_t window_length{script.grid.window_length_ns};
    const std::uint64_t windows{((script.horizon() - kOrigin) / window_length) + 1U};
    std::uint64_t peak{0};
    for (std::uint64_t window{0}; window < windows; ++window)
    {
        const std::uint64_t start{kOrigin + (window * window_length)};
        std::uint64_t frontier{kOrigin};
        while (frontier < script.window_end(window))
            frontier += kStep;
        std::uint64_t frames{0};
        for (const auto& shard_records : script.records)
            frames += static_cast<std::uint64_t>(
                std::ranges::count_if(shard_records, [&](const Record& record)
                                      { return record.tick >= start && record.tick <= frontier; }));
        peak = std::max(peak, frames);
    }
    const std::uint64_t seals_per_shard{2U + ((kStep + window_length - 1U) / window_length)};
    return peak + (kShards * seals_per_shard);
}

struct Run
{
    bool completed{false};
    std::vector<Key> emitted;
    std::uint64_t held_peak{0};
    std::string stall;
};

// invariant: every thread's wait is bounded by `abandoned`: past kRunBound the merge aborts every
// channel and wakes every waiter, so a stalled run reds instead of hanging the suite.
class MiniatureRun
{
  public:
    MiniatureRun(const Script& script, FrontierCadence cadence)
        : script_{script}, cadence_{cadence},
          producers_{"admission", kShards, script.grid, kRingSlots}
    {
    }

    [[nodiscard]] Run run(Clock::duration bound)
    {
        Drainer drainer{Drainer::Config{.channel = producers_.base, .shard_count = kShards}};
        Buffer buffer{drainer};
        Run result;
        {
            std::vector<std::jthread> threads;
            for (std::uint32_t shard{0}; shard < kShards; ++shard)
                threads.emplace_back([this, shard] { produce(shard); });
            threads.emplace_back([this] { publish_frontier(); });

            const auto deadline{Clock::now() + bound};
            Frame frame{};
            while (!buffer.drained() && Clock::now() < deadline)
            {
                if (buffer.try_select(frame))
                {
                    result.emitted.push_back(key_of(frame));
                    std::this_thread::sleep_for(kThrottle);
                    continue;
                }
                std::this_thread::sleep_for(kThrottle);
            }
            result.completed = buffer.drained();
            if (!result.completed)
            {
                result.stall = describe(buffer);
                abandon();
            }
        }
        result.held_peak = buffer.metrics().held_frames_peak;
        return result;
    }

  private:
    // post: the shard's frames in key order, each epoch's after the horizon it last saw is sealed,
    // then its progress published; then it seals on every horizon through the last and closes.
    void produce(std::uint32_t shard)
    {
        auto& channel{producers_.producers[shard]};
        std::uint64_t next_seal{0};
        const auto seal_below{
            [&](std::uint64_t window)
            {
                while (next_seal < window)
                {
                    static_cast<void>(channel.push(seal_frame(shard, script_, next_seal)));
                    ++next_seal;
                }
            }};
        const auto seal_through{[&](std::uint64_t horizon)
                                {
                                    seal_below((std::min(horizon, script_.horizon()) - kOrigin) /
                                               script_.grid.window_length_ns);
                                }};
        const auto& records{script_.records[shard]};
        std::size_t next{0};
        for (std::uint64_t step{1}; step <= kSteps && !abandoned_.load(); ++step)
        {
            seal_through(horizon_.load(std::memory_order_acquire));
            const std::uint64_t step_end{kOrigin + (step * kStep)};
            while (next < records.size() && records[next].tick <= step_end)
            {
                seal_below(script_.window_of(records[next].tick));
                static_cast<void>(channel.push(data_frame(shard, records[next])));
                ++next;
            }
            progress_[shard].store(step, std::memory_order_release);
            progress_[shard].notify_all();
        }
        for (;;)
        {
            const std::uint64_t horizon{horizon_.load(std::memory_order_acquire)};
            seal_through(horizon);
            if (horizon >= script_.horizon() || abandoned_.load())
                break;
            horizon_.wait(horizon, std::memory_order_acquire);
        }
        channel.close_graceful();
    }

    // post: each frontier point published once every shard pushed its frames through it, or only
    // the last one under AtEndOnly, as the bulk replay published before DN-103.D29 (d).
    void publish_frontier()
    {
        for (std::uint64_t step{1}; step <= kSteps; ++step)
        {
            for (auto& progress : progress_)
            {
                for (auto seen{progress.load(std::memory_order_acquire)};
                     seen < step && !abandoned_.load(); seen = progress.load())
                    progress.wait(seen, std::memory_order_acquire);
            }
            if (abandoned_.load())
                return;
            if (cadence_ == FrontierCadence::PerStep || step == kSteps)
            {
                horizon_.store(kOrigin + (step * kStep), std::memory_order_release);
                horizon_.notify_all();
            }
        }
    }

    void abandon()
    {
        abandoned_.store(true);
        for (auto& channel : producers_.producers)
            channel.close_abort();
        for (auto& progress : progress_)
        {
            progress.fetch_add(kSteps + 1U);
            progress.notify_all();
        }
        horizon_.store(std::numeric_limits<std::uint64_t>::max());
        horizon_.notify_all();
    }

    [[nodiscard]] std::string describe(Buffer& buffer) const
    {
        std::string text{std::format("anchor {} L {} horizon {}", buffer.anchor_tick(),
                                     buffer.admission_horizon(), horizon_.load())};
        const auto summaries{buffer.shard_summaries()};
        for (std::size_t shard{0}; shard < summaries.size(); ++shard)
            text += std::format("; shard {}: held {} last pulled {} progress {} ring {}", shard,
                                summaries[shard].buf_size, summaries[shard].last_pulled_tick,
                                progress_[shard].load(), producers_.producers[shard].size_approx());
        return text;
    }

    const Script& script_;
    FrontierCadence cadence_;
    ProducerHarness producers_;
    std::array<std::atomic<std::uint64_t>, kShards> progress_{};
    // invariant: the origin until the first frontier point is published, so no window seals early.
    std::atomic<std::uint64_t> horizon_{kOrigin};
    std::atomic<bool> abandoned_{false};
};

void expect_complete_within_bound(std::uint64_t window_length)
{
    const auto script{make_script(window_length)};
    // note: the silent shard's progress never lags, since it has nothing to push.
    MiniatureRun miniature{script, FrontierCadence::PerStep};
    const auto run{miniature.run(kRunBound)};
    const auto expected{sorted_union(script)};
    const auto bound{frame_bound(script)};

    ASSERT_TRUE(run.completed) << "the merge stalled on a " << window_length << "-tick window, "
                               << kStep << "-tick step grid after " << run.emitted.size() << " of "
                               << expected.size() << " frames: " << run.stall;
    EXPECT_EQ(run.emitted.size(), expected.size());
    const auto mismatch{std::ranges::mismatch(run.emitted, expected)};
    EXPECT_TRUE(mismatch.in1 == run.emitted.end() && mismatch.in2 == expected.end())
        << "the emitted sequence departs from the sorted union at frame "
        << (mismatch.in1 - run.emitted.begin());
    EXPECT_LE(run.held_peak, bound)
        << "the storage held " << run.held_peak << " frames at its peak, above F = " << bound
        << " for a " << window_length << "-tick window on a " << kStep << "-tick step";
}

} // namespace

TEST(AdmissionHorizon, TheMergeCompletesWithinFOnAnAlignedGrid)
{
    expect_complete_within_bound(kAlignedWindow);
}

// invariant: I = 2.5 e: window 0 ends between two frontier points, so its seal on the silent shard
// needs the frontier past the window's end, which L must admit the dense shard's frames up to.
TEST(AdmissionHorizon, TheMergeCompletesWithinFOnAMisalignedGrid)
{
    expect_complete_within_bound(kMisalignedWindow);
}

// invariant: the bulk replay's cadence before DN-103.D29 (d): with a silent shard and a bounded
// merge, nothing publishes a horizon before the producers finish, and they cannot finish.
// assert: the control that makes the two arms above non-vacuous: their bound detects a stall.
TEST(AdmissionHorizon, AFrontierPublishedOnlyAtTheEndStallsASetWithASilentShard)
{
    const auto script{make_script(kAlignedWindow)};
    MiniatureRun miniature{script, FrontierCadence::AtEndOnly};
    const auto run{miniature.run(kStallBound)};
    EXPECT_FALSE(run.completed)
        << "the merge completed with no horizon before the end; the admission did not bind";
}

TEST(AdmissionHorizon, LIsTheFrontierPointAtOrAfterTheEndOfTheAnchorsWindow)
{
    constexpr SealGrid kAligned{
        .origin_unix_ns = kOrigin, .window_length_ns = kAlignedWindow, .frontier_step_ns = kStep};
    constexpr SealGrid kMisaligned{.origin_unix_ns = kOrigin,
                                   .window_length_ns = kMisalignedWindow,
                                   .frontier_step_ns = kStep};
    EXPECT_EQ(coderoast::ipc::admission_horizon(kAligned, 0U), kOrigin + 16U);
    EXPECT_EQ(coderoast::ipc::admission_horizon(kAligned, kOrigin + 15U), kOrigin + 16U);
    EXPECT_EQ(coderoast::ipc::admission_horizon(kAligned, kOrigin + 16U), kOrigin + 32U);
    EXPECT_EQ(coderoast::ipc::admission_horizon(kMisaligned, kOrigin), kOrigin + 12U);
    EXPECT_EQ(coderoast::ipc::admission_horizon(kMisaligned, kOrigin + 10U), kOrigin + 20U);
    EXPECT_EQ(coderoast::ipc::admission_horizon(kMisaligned, kOrigin + 20U), kOrigin + 32U);
    EXPECT_EQ(coderoast::ipc::first_window_open_at(kMisaligned, kOrigin + 12U), 0U);
    EXPECT_EQ(coderoast::ipc::first_window_open_at(kMisaligned, kOrigin + 13U), 1U);
    EXPECT_EQ(coderoast::ipc::first_window_open_at(kAligned, kOrigin + 16U), 0U);
    EXPECT_EQ(coderoast::ipc::first_window_open_at(kAligned, kOrigin + 17U), 1U);
    constexpr SealGrid kHuge{.origin_unix_ns = std::numeric_limits<std::uint64_t>::max() - 1U,
                             .window_length_ns = kStep,
                             .frontier_step_ns = kStep};
    EXPECT_EQ(coderoast::ipc::admission_horizon(kHuge, 0U),
              std::numeric_limits<std::uint64_t>::max())
        << "a frontier point past the uint64 range must saturate, which admits every frame";
}

// invariant: the admission reads one grid for every shard, so a set declaring two is refused.
TEST(AdmissionHorizon, ASetWhoseShardsDeclareTwoGridsIsRefusedAtOpen)
{
    const auto base{unique_channel("two_grids")};
    constexpr SealGrid kFirst{
        .origin_unix_ns = kOrigin, .window_length_ns = kAlignedWindow, .frontier_step_ns = kStep};
    constexpr SealGrid kSecond{
        .origin_unix_ns = kOrigin, .window_length_ns = kAlignedWindow, .frontier_step_ns = 2U};
    std::vector<Channel> channels;
    channels.push_back(Channel::create(
        coderoast::ipc::ChannelConfig{.name = coderoast::ipc::shard_channel_name(base, 0U),
                                      .slot_count = 4U,
                                      .seal_grid = kFirst}));
    channels.push_back(Channel::create(
        coderoast::ipc::ChannelConfig{.name = coderoast::ipc::shard_channel_name(base, 1U),
                                      .slot_count = 4U,
                                      .seal_grid = kSecond}));
    EXPECT_THROW((Drainer{Drainer::Config{.channel = base, .shard_count = 2}}), std::runtime_error)
        << "shard 1 declares a frontier step of 2 ticks against shard 0's 4";
}
