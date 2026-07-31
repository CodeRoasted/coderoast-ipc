// coderoast.ipc.core — implementation unit (ADR-3.D4 cascade rule).
//
// The POSIX shared-memory syscalls touch C MACROS (errno, O_*, PROT_*, MAP_*) that cannot live in
// the BMI-producing module INTERFACE (.cppm) — so they live HERE, in an implementation unit (a
// regular .o, not a BMI), where the GMF's textual POSIX headers make those macros available in the
// purview normally. Refinement over the interface ban: this impl unit CAN `import std` for its std
// needs — only the libc POSIX headers (which carry the macros) stay textual in the GMF. It attaches
// via `module coderoast.ipc.core;` so the definitions match the interface's `shm_*`
// declarations; the interface↔impl boundary crosses primitives only (int/size_t/void*/const char*).
module;
#include <cerrno>
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

    [[noreturn]] void throw_errno(std::string_view action)
    {
        const auto error_number{errno};
        throw std::runtime_error(std::string(action) + " failed: " +
                                 std::error_code(error_number, std::generic_category()).message());
    }

} // namespace

int shm_open_create(const char* name)
{
    const int descriptor{::shm_open(name, O_CREAT | O_EXCL | O_RDWR, kSharedMemoryPermissions)};
    if (descriptor < 0)
    {
        throw_errno("shm_open(create)");
    }
    return descriptor;
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

void shm_truncate(int descriptor, std::size_t size)
{
    if (::ftruncate(descriptor, static_cast<off_t>(size)) != 0)
    {
        throw_errno("ftruncate");
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

} // namespace coderoast::ipc
