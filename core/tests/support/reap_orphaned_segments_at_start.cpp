// refs: DN-102.D2
// invariant: a test executable that creates shared-memory segments compiles this unit, so each of
// its processes reaps, before its first test, the segments a crashed earlier process left behind.
// invariant: no library compiles it: coderoast_ipc_reap_orphaned_segments_at_start adds it to a
// test target, so googletest reaches no shipped archive.
#include <gtest/gtest.h>

import std;
import coderoast.ipc.core;

namespace
{

// post: every /dev/shm segment whose owner is proven dead is unlinked, one line naming each.
// post: one line counts the segments removed, kept alive, unjudged and dead but not removed.
class ReapOrphanedSegmentsAtStart final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        std::size_t removed{0};
        std::size_t alive{0};
        std::size_t unknown{0};
        std::size_t not_removed{0};
        for (const auto& segment : coderoast::ipc::reap_orphaned_segments())
        {
            if (segment.verdict == coderoast::ipc::SegmentVerdict::Alive)
                ++alive;
            else if (segment.verdict == coderoast::ipc::SegmentVerdict::Unknown)
                ++unknown;
            else if (segment.removed)
            {
                ++removed;
                std::println("segment reaper: removed /dev/shm/{}, whose owner process is dead",
                             segment.name);
            }
            else
            {
                ++not_removed;
                std::println("segment reaper: /dev/shm/{} has a dead owner and was not removed",
                             segment.name);
            }
        }
        std::println(
            "segment reaper: at start, removed {} orphaned segment(s); kept {} with a live "
            "owner, {} it cannot judge and {} with a dead owner it could not remove",
            removed, alive, unknown, not_removed);
    }
};

// invariant: registered at static initialisation, so it is in place before the entry point runs
// the tests, and googletest owns the instance.
const ::testing::Environment* const g_segment_reaper{
    ::testing::AddGlobalTestEnvironment(new ReapOrphanedSegmentsAtStart)};

} // namespace
