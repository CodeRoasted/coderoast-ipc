// refs: ADR-3.D4
// invariant: the libc POSIX headers stay textual in the global module fragment because their macros
// cannot cross a module boundary; std still arrives by import.
module;
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

module coderoast.ipc.core;
import std;

namespace coderoast::ipc
{

namespace
{

    constexpr mode_t kSharedMemoryPermissions{0600};

    constexpr const char* kShmDirectory{"/dev/shm"};
    constexpr const char* kOwnPidNamespace{"/proc/self/ns/pid"};
    // invariant: /proc/<pid>/stat numbers the process's start time 22, and fields 1 and 2 (the pid
    // and the parenthesised command) end at the line's last ')'.
    constexpr std::size_t kStartTimeField{22U};
    constexpr std::size_t kFieldsThroughCommand{2U};

    [[noreturn]] void throw_error(std::string_view action, int error_number)
    {
        throw std::runtime_error(std::string(action) + " failed: " +
                                 std::error_code(error_number, std::generic_category()).message());
    }

    [[noreturn]] void throw_errno(std::string_view action)
    {
        throw_error(action, errno);
    }

    enum class ProcessReading : std::uint8_t
    {
        Found,
        Absent,
        Unreadable,
    };

    struct StartTime
    {
        ProcessReading reading{ProcessReading::Unreadable};
        std::uint64_t ticks{0};
    };

    // post: Absent when no process holds `pid` in this pid namespace; the command field may hold
    // spaces and parentheses, so fields are counted from the line's last ')'.
    [[nodiscard]] StartTime read_start_time(std::int32_t pid)
    {
        const std::filesystem::path process{std::format("/proc/{}", pid)};
        std::error_code status_error;
        if (std::filesystem::symlink_status(process, status_error).type() ==
            std::filesystem::file_type::not_found)
        {
            return {.reading = ProcessReading::Absent};
        }
        // assert: a process that exits between the status and this read is Unreadable, never
        // Absent, so the race keeps a segment and never reclaims one.
        std::string stat_line;
        if (!std::getline(std::ifstream{process / "stat"}, stat_line))
        {
            return {};
        }
        const std::string_view line{stat_line};
        const auto command_end{line.rfind(')')};
        if (command_end == std::string_view::npos)
        {
            return {};
        }
        std::size_t field{kFieldsThroughCommand};
        std::size_t position{command_end + 1U};
        while (position < line.size())
        {
            const auto begin{line.find_first_not_of(' ', position)};
            if (begin == std::string_view::npos)
            {
                break;
            }
            const auto end{std::min(line.find(' ', begin), line.size())};
            if (++field == kStartTimeField)
            {
                const auto token{line.substr(begin, end - begin)};
                std::uint64_t ticks{0};
                const auto parsed{
                    std::from_chars(token.data(), token.data() + token.size(), ticks)};
                if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size())
                {
                    return {};
                }
                return {.reading = ProcessReading::Found, .ticks = ticks};
            }
            position = end;
        }
        return {};
    }

    // post: the inode of this process's pid namespace, or 0 when /proc cannot say.
    [[nodiscard]] std::uint64_t own_pid_namespace() noexcept
    {
        struct stat stats{};
        if (::stat(kOwnPidNamespace, &stats) != 0)
        {
            return 0U;
        }
        return static_cast<std::uint64_t>(stats.st_ino);
    }

    // refs: DN-102.D2
    // post: the entry's verdict; the header is pread through a non-blocking descriptor, and an
    // Orphaned segment is unlinked only while its name still resolves to the file judged.
    [[nodiscard]] ReapedSegment judge_and_reap(std::string name)
    {
        ReapedSegment reaped{.name = std::move(name)};
        const auto segment_name{"/" + reaped.name};
        const int descriptor{
            ::shm_open(segment_name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK, 0)};
        if (descriptor < 0)
        {
            return reaped;
        }
        struct stat stats{};
        std::array<std::byte, sizeof(SharedChannelHeader)> bytes{};
        const bool whole_header{::fstat(descriptor, &stats) == 0 && S_ISREG(stats.st_mode) &&
                                static_cast<std::size_t>(stats.st_size) >= bytes.size() &&
                                ::pread(descriptor, bytes.data(), bytes.size(), 0) ==
                                    static_cast<::ssize_t>(bytes.size())};
        (void)::close(descriptor);
        if (!whole_header)
        {
            return reaped;
        }

        const std::span<const std::byte> header{bytes};
        std::uint64_t magic{0};
        std::uint32_t abi_version{0};
        SegmentOwner owner{};
        std::memcpy(&magic, header.subspan(offsetof(SharedChannelHeader, magic)).data(),
                    sizeof(magic));
        std::memcpy(&abi_version, header.subspan(offsetof(SharedChannelHeader, abi_version)).data(),
                    sizeof(abi_version));
        std::memcpy(&owner, header.subspan(offsetof(SharedChannelHeader, owner)).data(),
                    sizeof(owner));
        if (magic != kSharedChannelMagic || abi_version != kSharedChannelAbiVersion)
        {
            return reaped;
        }

        reaped.verdict = judge_segment_owner(owner);
        if (reaped.verdict == SegmentVerdict::Orphaned)
        {
            reaped.removed = shm_unlink_if_identity(
                segment_name.c_str(),
                SegmentIdentity{.device = static_cast<std::uint64_t>(stats.st_dev),
                                .inode = static_cast<std::uint64_t>(stats.st_ino)});
        }
        return reaped;
    }

} // namespace

CreatedSegment shm_open_create(const char* name)
{
    const int descriptor{::shm_open(name, O_CREAT | O_EXCL | O_RDWR, kSharedMemoryPermissions)};
    if (descriptor < 0)
    {
        throw_errno("shm_open(create)");
    }
    struct stat stats{};
    if (::fstat(descriptor, &stats) != 0)
    {
        const auto error_number{errno};
        // assert: O_EXCL made the name this call's own an instant ago, so no identity is needed.
        (void)::close(descriptor);
        (void)::shm_unlink(name);
        throw_error("fstat(create)", error_number);
    }
    return CreatedSegment{
        .descriptor = descriptor,
        .identity = SegmentIdentity{.device = static_cast<std::uint64_t>(stats.st_dev),
                                    .inode = static_cast<std::uint64_t>(stats.st_ino)},
    };
}

int shm_open_existing(const char* name)
{
    const int descriptor{::shm_open(name, O_RDWR, kSharedMemoryPermissions)};
    if (descriptor < 0)
    {
        throw_errno("shm_open(open)");
    }
    return descriptor;
}

void shm_reserve(int descriptor, std::size_t size, const char* channel)
{
    int error_number{EINTR};
    while (error_number == EINTR)
    {
        error_number = ::posix_fallocate(descriptor, 0, static_cast<off_t>(size));
    }
    if (error_number != 0)
    {
        throw std::runtime_error(
            std::format("posix_fallocate of {} B for shared-memory channel '{}' failed: {}", size,
                        channel, std::error_code(error_number, std::generic_category()).message()));
    }
}

std::size_t shm_fstat_size(int descriptor)
{
    struct stat stats{};
    if (::fstat(descriptor, &stats) != 0)
    {
        throw_errno("fstat");
    }
    return static_cast<std::size_t>(stats.st_size);
}

void* shm_map(int descriptor, std::size_t size)
{
    void* mapping{::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0)};
    if (mapping == MAP_FAILED)
    {
        throw_errno("mmap");
    }
    return mapping;
}

void shm_unmap(void* address, std::size_t size) noexcept
{
    (void)::munmap(address, size);
}

void close_descriptor(int descriptor) noexcept
{
    (void)::close(descriptor);
}

void shm_unlink_name(const char* name) noexcept
{
    (void)::shm_unlink(name);
}

SegmentOwner current_segment_owner()
{
    const auto pid{static_cast<std::int32_t>(::getpid())};
    const auto start{read_start_time(pid)};
    return SegmentOwner{
        .start_time = start.reading == ProcessReading::Found ? start.ticks : 0U,
        .pid_namespace = own_pid_namespace(),
        .pid = pid,
    };
}

SegmentVerdict judge_segment_owner(const SegmentOwner& owner)
{
    const auto self_namespace{own_pid_namespace()};
    if (owner.pid <= 0 || owner.start_time == 0U || owner.pid_namespace == 0U ||
        self_namespace == 0U || owner.pid_namespace != self_namespace)
    {
        return SegmentVerdict::Unknown;
    }
    const auto start{read_start_time(owner.pid)};
    if (start.reading == ProcessReading::Absent)
    {
        return SegmentVerdict::Orphaned;
    }
    if (start.reading == ProcessReading::Unreadable)
    {
        return SegmentVerdict::Unknown;
    }
    return start.ticks == owner.start_time ? SegmentVerdict::Alive : SegmentVerdict::Orphaned;
}

std::vector<ReapedSegment> reap_orphaned_segments(std::string_view name_prefix)
{
    std::vector<ReapedSegment> reaped;
    std::error_code scan_error;
    for (std::filesystem::directory_iterator iter{kShmDirectory, scan_error}, end;
         !scan_error && iter != end; iter.increment(scan_error))
    {
        auto name{iter->path().filename().string()};
        std::error_code type_error;
        if (!name.starts_with(name_prefix) ||
            iter->symlink_status(type_error).type() != std::filesystem::file_type::regular)
        {
            continue;
        }
        reaped.push_back(judge_and_reap(std::move(name)));
    }
    return reaped;
}

bool shm_unlink_if_identity(const char* name, SegmentIdentity identity) noexcept
{
    const int descriptor{::shm_open(name, O_RDONLY, 0)};
    if (descriptor < 0)
    {
        return false;
    }
    struct stat stats{};
    const bool same{::fstat(descriptor, &stats) == 0 &&
                    static_cast<std::uint64_t>(stats.st_dev) == identity.device &&
                    static_cast<std::uint64_t>(stats.st_ino) == identity.inode};
    (void)::close(descriptor);
    return same && ::shm_unlink(name) == 0;
}

} // namespace coderoast::ipc
