// refs: DN-102.D2
// invariant: no library compiles it: coderoast_ipc_private_pid_namespace adds it to a test target,
// so googletest reaches no shipped archive.
#include <fcntl.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <gtest/gtest.h>

import std;
import coderoast.ipc.core;

#include "private_pid_namespace.hpp"

namespace coderoast::ipc::testing
{
namespace
{

    // invariant: set only in a run rerun_this_test_in_a_private_pid_namespace starts, to the filter
    // naming the test it reruns.
    constexpr const char* kRerunVariable{"CODEROAST_IPC_PRIVATE_NAMESPACE_RERUN"};
    // invariant: set only inside a private namespace, to the descriptor of the socket whose other
    // end the starting process serves outside reaps on.
    constexpr const char* kOutsideReaperVariable{"CODEROAST_IPC_OUTSIDE_REAPER_SOCKET"};

    constexpr int kExecFailed{127};
    constexpr std::size_t kOutputChunkBytes{4096U};
    constexpr std::size_t kRequestChunkBytes{256U};

    [[nodiscard]] std::string errno_text(int error_number)
    {
        return std::error_code(error_number, std::generic_category()).message();
    }

    // post: whether `text` was written whole to the procfs file at `path`.
    [[nodiscard]] bool write_proc_file(const char* path, std::string_view text) noexcept
    {
        const int descriptor{::open(path, O_WRONLY | O_CLOEXEC)};
        if (descriptor < 0)
        {
            return false;
        }
        const auto written{::write(descriptor, text.data(), text.size())};
        static_cast<void>(::close(descriptor));
        return written == static_cast<::ssize_t>(text.size());
    }

    // pre: runs in a forked child of the test process, kOutsideReaperVariable naming its socket.
    // post: never returns; exits with init's exit code or one of the namespace's reserved codes.
    [[noreturn]] void enter_and_run_init(const std::function<int()>& init) noexcept
    {
        try
        {
            const auto uid{::getuid()};
            const auto gid{::getgid()};
            if (::unshare(CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNS) != 0 ||
                !write_proc_file("/proc/self/setgroups", "deny") ||
                !write_proc_file("/proc/self/uid_map", std::format("0 {} 1", uid)) ||
                !write_proc_file("/proc/self/gid_map", std::format("0 {} 1", gid)))
            {
                std::println(stderr, "[private namespace] no user or pid namespace: {}",
                             errno_text(errno));
                std::_Exit(kPrivateNamespaceRefused);
            }
            const auto first{::fork()};
            if (first == 0)
            {
                if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0 ||
                    ::mount("proc", "/proc", "proc", 0, nullptr) != 0)
                {
                    std::println(stderr, "[private namespace] no private /proc: {}",
                                 errno_text(errno));
                    std::_Exit(kPrivateNamespaceRefused);
                }
                try
                {
                    std::_Exit(init());
                }
                catch (const std::exception& error)
                {
                    std::println(stderr, "[private namespace] the init body threw: {}",
                                 error.what());
                    std::_Exit(kPrivateNamespaceInitThrew);
                }
            }
            int status{0};
            if (first < 0 || ::waitpid(first, &status, 0) != first || !WIFEXITED(status))
            {
                std::println(stderr,
                             "[private namespace] its first process ended without an exit "
                             "code{}",
                             first > 0 && WIFSIGNALED(status)
                                 ? std::format(", by signal {} ({})", WTERMSIG(status),
                                               ::strsignal(WTERMSIG(status)))
                                 : std::string{});
                std::_Exit(kPrivateNamespaceInitDied);
            }
            std::_Exit(WEXITSTATUS(status));
        }
        catch (const std::exception& error)
        {
            std::println(stderr, "[private namespace] unexpected exception: {}", error.what());
            std::_Exit(kPrivateNamespaceInitThrew);
        }
    }

    // post: every request read from `socket` until each namespace process closed it was answered by
    // a reap of its prefix in this process's namespace, recorded in `reaps` in arrival order.
    void serve_outside_reaps(int socket, std::vector<OutsideReap>& reaps)
    {
        std::string pending;
        std::array<char, kRequestChunkBytes> chunk{};
        for (;;)
        {
            const auto received{::read(socket, chunk.data(), chunk.size())};
            if (received < 0 && errno == EINTR)
            {
                continue;
            }
            if (received <= 0)
            {
                return;
            }
            pending.append(chunk.data(), static_cast<std::size_t>(received));
            for (auto end{pending.find('\n')}; end != std::string::npos; end = pending.find('\n'))
            {
                auto prefix{pending.substr(0, end)};
                pending.erase(0, end + 1);
                auto segments{reap_orphaned_segments(prefix)};
                reaps.push_back(
                    OutsideReap{.prefix = std::move(prefix), .segments = std::move(segments)});
                const char served{1};
                static_cast<void>(::send(socket, &served, 1, MSG_NOSIGNAL));
            }
        }
    }

    [[nodiscard]] std::string current_test_filter()
    {
        const auto* const current{::testing::UnitTest::GetInstance()->current_test_info()};
        return current == nullptr ? std::string{}
                                  : std::string{current->test_suite_name()} + "." + current->name();
    }

} // namespace

PrivateNamespaceRun run_in_a_private_pid_namespace(const std::function<int()>& init)
{
    std::array<int, 2> outside_reaper{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, outside_reaper.data()) != 0)
    {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    const auto child{::fork()};
    if (child == 0)
    {
        static_cast<void>(::close(outside_reaper[0]));
        if (::setenv(kOutsideReaperVariable, std::to_string(outside_reaper[1]).c_str(), 1) != 0)
        {
            std::_Exit(kPrivateNamespaceInitThrew);
        }
        enter_and_run_init(init);
    }
    const int fork_error{errno};
    static_cast<void>(::close(outside_reaper[1]));
    PrivateNamespaceRun run;
    if (child > 0)
    {
        serve_outside_reaps(outside_reaper[0], run.outside_reaps);
    }
    static_cast<void>(::close(outside_reaper[0]));
    if (child < 0)
    {
        throw std::system_error(fork_error, std::generic_category(), "fork");
    }
    int status{0};
    if (::waitpid(child, &status, 0) != child || !WIFEXITED(status))
    {
        run.exit_code = kPrivateNamespaceInitDied;
        return run;
    }
    run.exit_code = WEXITSTATUS(status);
    run.refused = run.exit_code == kPrivateNamespaceRefused;
    return run;
}

ExecutableRun
run_this_executable(const std::string& filter,
                    const std::vector<std::pair<std::string, std::string>>& environment)
{
    std::array<int, 2> output{-1, -1};
    if (::pipe(output.data()) != 0)
    {
        return {};
    }
    const auto child{::fork()};
    if (child == 0)
    {
        static_cast<void>(::dup2(output[1], STDOUT_FILENO));
        static_cast<void>(::dup2(output[1], STDERR_FILENO));
        static_cast<void>(::close(output[0]));
        static_cast<void>(::close(output[1]));
        std::string executable{"/proc/self/exe"};
        std::string filter_flag{"--gtest_filter=" + filter};
        std::array<char*, 3> arguments{executable.data(), filter_flag.data(), nullptr};
        for (const auto& [name, value] : environment)
        {
            if (::setenv(name.c_str(), value.c_str(), 1) != 0)
            {
                std::_Exit(kExecFailed);
            }
        }
        static_cast<void>(::execv(executable.c_str(), arguments.data()));
        std::_Exit(kExecFailed);
    }
    static_cast<void>(::close(output[1]));
    ExecutableRun run{.started = child > 0};
    std::array<char, kOutputChunkBytes> chunk{};
    while (run.started)
    {
        const auto received{::read(output[0], chunk.data(), chunk.size())};
        if (received > 0)
        {
            run.output.append(chunk.data(), static_cast<std::size_t>(received));
        }
        else if (received == 0 || errno != EINTR)
        {
            break;
        }
    }
    static_cast<void>(::close(output[0]));
    if (run.started && ::waitpid(child, &run.status, 0) != child)
    {
        run.started = false;
    }
    return run;
}

PrivateNamespaceRun rerun_this_test_in_a_private_pid_namespace(
    const std::vector<std::pair<std::string, std::string>>& environment)
{
    const auto filter{current_test_filter()};
    auto rerun_environment{environment};
    rerun_environment.emplace_back(kRerunVariable, filter);
    return run_in_a_private_pid_namespace(
        [&filter, &rerun_environment]
        {
            const auto run{run_this_executable(filter, rerun_environment)};
            const bool passed{run.started && WIFEXITED(run.status) && WEXITSTATUS(run.status) == 0};
            if (!passed)
            {
                std::println(stderr, "[private namespace] the run of {} printed:\n{}", filter,
                             run.output);
            }
            return passed ? 0 : 1;
        });
}

bool in_a_private_pid_namespace_rerun()
{
    const char* rerun{std::getenv(kRerunVariable)};
    return rerun != nullptr && current_test_filter() == rerun;
}

bool let_an_outside_reaper_judge(std::string_view prefix)
{
    const char* socket_text{std::getenv(kOutsideReaperVariable)};
    if (socket_text == nullptr)
    {
        return false;
    }
    const std::string_view text{socket_text};
    int socket{-1};
    if (std::from_chars(text.data(), text.data() + text.size(), socket).ec != std::errc{})
    {
        return false;
    }
    std::string request{prefix};
    request += '\n';
    if (::send(socket, request.data(), request.size(), MSG_NOSIGNAL) !=
        static_cast<::ssize_t>(request.size()))
    {
        return false;
    }
    char served{0};
    return ::recv(socket, &served, 1, 0) == 1;
}

} // namespace coderoast::ipc::testing
