// refs: DN-102.D2
// invariant: the one report of a start-of-run reap, shared by the test environment and the
// benchmark mains; no library compiles it, so it reaches no shipped archive.
// invariant: it prints to stderr, never stdout, which a benchmark may fill with JSON results.
// note: stderr is a C macro, which `import std` cannot carry, so cstdio stays textual.
#include <cstdio>

import std;
import coderoast.ipc.core;

#include "reap_orphaned_segments_at_start.hpp"

namespace coderoast::ipc::testing
{

void reap_orphaned_segments_at_start()
{
    std::size_t removed{0};
    std::size_t alive{0};
    std::size_t unknown{0};
    std::size_t not_removed{0};
    for (const auto& segment : reap_orphaned_segments())
    {
        if (segment.verdict == SegmentVerdict::Alive)
            ++alive;
        else if (segment.verdict == SegmentVerdict::Unknown)
            ++unknown;
        else if (segment.removed)
        {
            ++removed;
            std::println(stderr, "segment reaper: removed /dev/shm/{}, whose owner process is dead",
                         segment.name);
        }
        else
        {
            ++not_removed;
            std::println(stderr, "segment reaper: /dev/shm/{} has a dead owner and was not removed",
                         segment.name);
        }
    }
    std::println(stderr,
                 "segment reaper: at start, removed {} orphaned segment(s); kept {} with a live "
                 "owner, {} it cannot judge and {} with a dead owner it could not remove",
                 removed, alive, unknown, not_removed);
}

} // namespace coderoast::ipc::testing
