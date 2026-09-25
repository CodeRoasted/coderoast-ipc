// refs: DN-102.D2
// invariant: a test executable that creates shared-memory segments compiles this unit, so each of
// its processes reaps, before its first test, the segments a crashed earlier process left behind.
// invariant: no library compiles it: coderoast_ipc_reap_orphaned_segments_at_start adds it to a
// test target, so googletest reaches no shipped archive.
#include <gtest/gtest.h>

#include "reap_orphaned_segments_at_start.hpp"

namespace
{

class ReapOrphanedSegmentsAtStart final : public ::testing::Environment
{
  public:
    void SetUp() override
    {
        coderoast::ipc::testing::reap_orphaned_segments_at_start();
    }
};

// invariant: registered at static initialisation, so it is in place before the entry point runs
// the tests, and googletest owns the instance.
const ::testing::Environment* const g_segment_reaper{
    ::testing::AddGlobalTestEnvironment(new ReapOrphanedSegmentsAtStart)};

} // namespace
