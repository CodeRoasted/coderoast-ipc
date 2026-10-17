// invariant: the names, the channel type and the reaper readings both segment test executables
// share: core's confined suite and the namespace suite of coderoast_ipc_namespace_tests.
// invariant: every name here carries this process's id, so no arm opens, reserves or removes a
// segment another process created.
// pre: the including unit has imported coderoast.ipc.core and included <gtest/gtest.h>,
// <fcntl.h> and <sys/mman.h> before this header.
// refs: ADR-11.D7, DN-102.D2, DN-142.D29
#pragma once

namespace coderoast::ipc::testing
{

using SegmentFrame = coderoast::ipc::LineFrame<64>;
using SegmentChannel = coderoast::ipc::SharedMemorySpscChannel<SegmentFrame>;

// invariant: the seal grid these channels declare; nothing here admits by it, so any non-zero one.
// refs: DN-103.D29
inline constexpr coderoast::ipc::SealGrid kAnySegmentGrid{
    .origin_unix_ns = 0U, .window_length_ns = 1U, .frontier_step_ns = 1U};

[[nodiscard]] inline std::string unique_channel(const char* suffix)
{
    static std::atomic<std::uint64_t> counter{0};
    return std::string{"coderoast_ipc_lifetime_"} + suffix + "_" + std::to_string(::getpid()) +
           "_" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

// pre: `name` carries no '.', so its segment is `/<name>` exactly as create() normalises it.
// post: whether the name still resolves to a segment; an error other than ENOENT fails the arm.
[[nodiscard]] inline bool name_resolves(const std::string& name)
{
    const auto path{"/" + name};
    const int descriptor{::shm_open(path.c_str(), O_RDONLY, 0)};
    if (descriptor >= 0)
    {
        static_cast<void>(::close(descriptor));
        return true;
    }
    const auto error_number{errno};
    if (error_number != ENOENT)
    {
        ADD_FAILURE() << "shm_open('" << path << "') failed with "
                      << std::error_code(error_number, std::generic_category()).message()
                      << ", neither resolving nor absent";
    }
    return false;
}

[[nodiscard]] inline std::string_view verdict_name(coderoast::ipc::SegmentVerdict verdict) noexcept
{
    switch (verdict)
    {
    case coderoast::ipc::SegmentVerdict::Alive:
        return "Alive";
    case coderoast::ipc::SegmentVerdict::Orphaned:
        return "Orphaned";
    case coderoast::ipc::SegmentVerdict::Unknown:
        return "Unknown";
    }
    return "an undeclared verdict";
}

// post: the reaper's reading of `name` alone, the only entry its prefix admits; an empty or wider
// reading fails the arm and yields nullopt.
[[nodiscard]] inline std::optional<coderoast::ipc::ReapedSegment> reap_only(const std::string& name)
{
    auto reaped{coderoast::ipc::reap_orphaned_segments(name)};
    if (reaped.size() != 1U || reaped.front().name != name)
    {
        std::string listed;
        for (const auto& entry : reaped)
        {
            listed += " '" + entry.name + "'";
        }
        ADD_FAILURE() << "reaping the prefix '" << name << "' listed " << reaped.size()
                      << " entries:" << listed << "; expected exactly that one segment";
        return std::nullopt;
    }
    return reaped.front();
}

} // namespace coderoast::ipc::testing
