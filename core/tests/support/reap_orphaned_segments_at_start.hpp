#pragma once

// refs: DN-102.D2
// invariant: it names no type, so a main includes it before or after any import.
namespace coderoast::ipc::testing
{

// post: every /dev/shm segment whose owner is proven dead is unlinked, one stderr line naming each.
// post: one stderr line counts the segments removed, kept alive, unjudged and dead but kept.
void reap_orphaned_segments_at_start();

} // namespace coderoast::ipc::testing
