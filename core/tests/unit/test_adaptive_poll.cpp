#include <gtest/gtest.h>

import coderoast.ipc.core.test;

namespace
{
using namespace std::chrono_literals;

// invariant: past the progression's paused spins and yields with a wide margin, so every wait
// counted below is a sleep.
constexpr std::size_t kWaitsPastTheYieldPhase{512U};
constexpr auto kIdleWindow{250ms};
// invariant: a caller sleeping 1 us per wait wakes several times this bound in the window, and one
// sleeping toward a millisecond ceiling stays far under it; load only lengthens a sleep.
constexpr std::size_t kIdleWakeBound{1000U};
} // namespace

TEST(AdaptivePoll, AnIdleCallerBacksOffTowardABoundedSleepInsteadOfWakingEveryMicrosecond)
{
    coderoast::ipc::AdaptivePoll poll;
    for (std::size_t warmup{0}; warmup < kWaitsPastTheYieldPhase; ++warmup)
    {
        poll.wait();
    }

    std::size_t wakes{0};
    const auto until{std::chrono::steady_clock::now() + kIdleWindow};
    while (std::chrono::steady_clock::now() < until)
    {
        poll.wait();
        ++wakes;
    }
    EXPECT_LT(wakes, kIdleWakeBound)
        << "an idle AdaptivePoll woke " << wakes << " times in " << kIdleWindow.count()
        << " ms (bound " << kIdleWakeBound
        << "): a caller past the spin and yield phases must sleep toward the ceiling";
}
