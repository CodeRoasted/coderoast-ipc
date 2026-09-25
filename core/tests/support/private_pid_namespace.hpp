#pragma once

// refs: DN-102.D2
// pre: included after the importer's `import std` and `import coderoast.ipc.core`.
// invariant: a segment created inside a private pid namespace records that namespace, so a reaper
// in any other namespace on the host judges it Unknown and never removes it.
namespace coderoast::ipc::testing
{

// invariant: exit codes the namespace's own steps end with; an init body returns none of them.
inline constexpr int kPrivateNamespaceRefused{120};
inline constexpr int kPrivateNamespaceInitDied{121};
inline constexpr int kPrivateNamespaceInitThrew{122};

// invariant: what a reaper in the calling process's namespace listed when a run asked it to judge
// the segments under `prefix`.
struct OutsideReap
{
    std::string prefix;
    std::vector<ReapedSegment> segments;
};

struct PrivateNamespaceRun
{
    // invariant: true when the host refuses an unprivileged user, pid or mount namespace; then no
    // init body ran.
    bool refused{false};
    int exit_code{-1};
    std::vector<OutsideReap> outside_reaps;
};

// post: `init` ran as the first process of a new user, pid and mount namespace with its own /proc,
// and its exit code is returned; each let_an_outside_reaper_judge it reached is served here.
[[nodiscard]] PrivateNamespaceRun run_in_a_private_pid_namespace(const std::function<int()>& init);

struct ExecutableRun
{
    std::string output;
    int status{-1};
    bool started{false};
};

// post: this executable run again on `filter` alone with `environment` added, its stdout and stderr
// captured together.
[[nodiscard]] ExecutableRun
run_this_executable(const std::string& filter,
                    const std::vector<std::pair<std::string, std::string>>& environment);

// post: the current test run again, alone and with `environment` added, as the only test of a
// private pid namespace; exit code 0 when it passed, and its output printed when it did not.
[[nodiscard]] PrivateNamespaceRun rerun_this_test_in_a_private_pid_namespace(
    const std::vector<std::pair<std::string, std::string>>& environment);

// post: whether this process is the run rerun_this_test_in_a_private_pid_namespace started for the
// current test, or a death-test child of it.
[[nodiscard]] bool in_a_private_pid_namespace_rerun();

// post: returns once a reaper in the namespace the run started from has judged every segment whose
// name starts with `prefix`; false when no such reaper serves this process.
[[nodiscard]] bool let_an_outside_reaper_judge(std::string_view prefix);

} // namespace coderoast::ipc::testing
