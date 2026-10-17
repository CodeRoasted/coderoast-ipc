// refs: ADR-11.D7, DN-102.D2, DN-102.D3, DN-103.D20, DN-142.D29
// invariant: the segment arms that change mount topology: each mounts a private tmpfs in a fresh
// user, pid and mount namespace, which no Landlock domain allows (DN-142.D29 (ix)).
// invariant: this package is declared unconfined in packages.yml and built against the STORED
// coderoast_ipc_core, so the core's own suite runs confined and stays reusable.
// invariant: every name here carries this process's id, so no arm opens, reserves or removes a
// segment another process created.
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

#include <gtest/gtest.h>

import std;
import coderoast.ipc.core;

#include "private_pid_namespace.hpp"
#include "segment_test_helpers.hpp"

namespace
{
using namespace coderoast::ipc::testing;
using Channel = SegmentChannel;
using coderoast::ipc::SegmentVerdict;
constexpr coderoast::ipc::SealGrid kAnyGrid{kAnySegmentGrid};
} // namespace

namespace
{

constexpr int kRefusedAndSurvived{0};
constexpr int kNamespaceUnavailable{2};
constexpr int kCreateReturned{3};
constexpr int kRefusalIncomplete{4};
constexpr int kNameLeftBehind{5};
constexpr int kNotAFullTmpfsError{6};

constexpr std::size_t kSmallTmpfsBytes{std::size_t{1024} * 1024};

// post: the verdict of a create twice the size of a private /dev/shm of kSmallTmpfsBytes.
// pre: runs as the first process of a private namespace, whose mounts die with it; the host's
// /dev/shm is never touched.
[[nodiscard]] int create_past_a_small_tmpfs()
{
    if (::mount("tmpfs", "/dev/shm", "tmpfs", 0,
                std::format("size={}", kSmallTmpfsBytes).c_str()) != 0)
    {
        std::println(stderr, "[namespace init] no private tmpfs: {}",
                     std::error_code(errno, std::generic_category()).message());
        return kNamespaceUnavailable;
    }

    using Wide = coderoast::ipc::SharedMemorySpscChannel<coderoast::ipc::DefaultLineFrame>;
    const std::size_t slots{(2U * kSmallTmpfsBytes) / sizeof(coderoast::ipc::DefaultLineFrame)};
    const auto name{unique_channel("past_tmpfs")};
    const auto requested{Wide::segment_bytes(slots).value_or(0U)};
    try
    {
        const auto channel{Wide::create(coderoast::ipc::ChannelConfig{
            .name = name, .slot_count = slots, .seal_grid = kAnyGrid})};
        std::println(stderr, "[namespace init] a {} B create returned over a {} B tmpfs", requested,
                     kSmallTmpfsBytes);
        return kCreateReturned;
    }
    catch (const std::system_error& refusal)
    {
        const std::string_view message{refusal.what()};
        std::println(stderr, "[namespace init] refused: {}", message);
        const auto no_space{std::make_error_code(std::errc::no_space_on_device)};
        if (refusal.code() != no_space)
        {
            std::println(stderr, "[namespace init] the refusal carries '{}', not '{}'",
                         refusal.code().message(), no_space.message());
            return kNotAFullTmpfsError;
        }
        if (!message.contains(name) || !message.contains(std::to_string(requested)) ||
            !message.contains(no_space.message()))
        {
            std::println(stderr,
                         "[namespace init] the refusal must name the channel '{}', the {} B "
                         "requested and '{}'",
                         name, requested, no_space.message());
            return kRefusalIncomplete;
        }
        if (name_resolves(name))
        {
            std::println(stderr, "[namespace init] the refused create left '/{}' behind", name);
            return kNameLeftBehind;
        }
        return kRefusedAndSurvived;
    }
    catch (const std::runtime_error& refusal)
    {
        std::println(stderr,
                     "[namespace init] refused by a {} that carries no errno, so no caller can "
                     "tell a full tmpfs from another failure: {}",
                     typeid(refusal).name(), refusal.what());
        return kNotAFullTmpfsError;
    }
}

} // namespace

namespace
{

constexpr int kCapacityReadAsSized{0};
constexpr int kCapacityMisread{3};
constexpr int kFillerRefused{4};

// post: whether shared_memory_capacity reads a private /dev/shm of kSmallTmpfsBytes as that size,
// with a quarter of it already taken, so a reading of what is free cannot pass.
// pre: runs as the first process of a private namespace, whose mounts die with it.
[[nodiscard]] int read_the_capacity_of_a_small_tmpfs()
{
    if (::mount("tmpfs", "/dev/shm", "tmpfs", 0,
                std::format("size={}", kSmallTmpfsBytes).c_str()) != 0)
    {
        std::println(stderr, "[namespace init] no private tmpfs: {}",
                     std::error_code(errno, std::generic_category()).message());
        return kNamespaceUnavailable;
    }
    using Wide = coderoast::ipc::SharedMemorySpscChannel<coderoast::ipc::DefaultLineFrame>;
    const std::size_t slots{(kSmallTmpfsBytes / 4U) / sizeof(coderoast::ipc::DefaultLineFrame)};
    try
    {
        const auto filler{
            Wide::create(coderoast::ipc::ChannelConfig{.name = unique_channel("capacity_filler"),
                                                       .slot_count = slots,
                                                       .seal_grid = kAnyGrid})};
        const auto capacity{coderoast::ipc::shared_memory_capacity()};
        if (capacity != std::optional<std::size_t>{kSmallTmpfsBytes})
        {
            std::println(stderr,
                         "[namespace init] a {} B tmpfs holding a {} B segment read as {} B",
                         kSmallTmpfsBytes, Wide::segment_bytes(slots).value_or(0U),
                         capacity.has_value() ? std::to_string(*capacity) : "nothing");
            return kCapacityMisread;
        }
        return kCapacityReadAsSized;
    }
    catch (const std::exception& refusal)
    {
        std::println(stderr, "[namespace init] the filler segment was refused: {}", refusal.what());
        return kFillerRefused;
    }
}

} // namespace

// refs: DN-103.D20
TEST(SegmentReservation, TheCapacityIsTheTmpfsSizeNotWhatIsFree)
{
    const auto run{coderoast::ipc::testing::run_in_a_private_pid_namespace(
        [] { return read_the_capacity_of_a_small_tmpfs(); })};
    if (run.refused || run.exit_code == kNamespaceUnavailable)
    {
        GTEST_SKIP() << "this host refuses an unprivileged user, pid or mount namespace, so no "
                        "private tmpfs can be sized; the line above names the errno";
    }
    EXPECT_EQ(run.exit_code, kCapacityReadAsSized)
        << "exit 3 = the capacity read is not the tmpfs size; 4 = the filler segment was refused; "
           "121 = the namespace's first process died by the signal named above; 122 = another "
           "exception";
}

// refs: DN-102.D3, DN-103.D20
TEST(SegmentReservation, ACreateLargerThanTheTmpfsIsRefusedAndTheProcessSurvives)
{
    const auto run{coderoast::ipc::testing::run_in_a_private_pid_namespace(
        [] { return create_past_a_small_tmpfs(); })};
    if (run.refused || run.exit_code == kNamespaceUnavailable)
    {
        GTEST_SKIP() << "this host refuses an unprivileged user, pid or mount namespace, so no "
                        "private tmpfs can be sized; the line above names the errno";
    }
    EXPECT_EQ(run.exit_code, kRefusedAndSurvived)
        << "exit 3 = the create returned; 4 = the refusal omits the channel, the bytes or the "
           "errno; 5 = the refused create left its name; 6 = the refusal is not a "
           "std::system_error carrying ENOSPC; 121 = the namespace's first process "
           "died by the signal named above, SIGBUS if the create committed pages its tmpfs could "
           "not hold; 122 = another exception";
}

namespace
{

// invariant: set only in the rerun the dead-owner arm starts, to the name its parent minted.
constexpr const char* kDeadOwnerVariable{"CODEROAST_IPC_DEAD_OWNER_SEGMENT"};

// post: the checks of the dead-owner arm, run inside the private namespace its parent started.
void judge_a_dead_owner_in_this_namespace(const std::string& name)
{
    const auto child{::fork()};
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        try
        {
            const auto producer{Channel::create(coderoast::ipc::ChannelConfig{
                .name = name, .slot_count = 4U, .seal_grid = kAnyGrid})};
            std::_Exit(0);
        }
        catch (...)
        {
            std::_Exit(1);
        }
    }
    int status{0};
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0)
        << "the child that creates '/" << name << "' did not exit 0";
    ASSERT_TRUE(name_resolves(name)) << "premise: a producer ended by _Exit runs no close";

    ASSERT_TRUE(coderoast::ipc::testing::let_an_outside_reaper_judge(name))
        << "no reaper outside this namespace served the checkpoint";
    ASSERT_TRUE(name_resolves(name)) << "a reaper outside this pid namespace removed '/" << name
                                     << "', whose owner died inside it, before this arm's own reap";

    const auto reaped{reap_only(name)};
    ASSERT_TRUE(reaped.has_value());
    EXPECT_EQ(reaped->verdict, SegmentVerdict::Orphaned)
        << "pid " << child << " has exited and been waited for, yet its segment was judged "
        << verdict_name(reaped->verdict);
    EXPECT_TRUE(reaped->removed);
    if (name_resolves(name))
    {
        ADD_FAILURE() << "'/" << name << "' outlived the reaper though its owner is dead";
        Channel::unlink(name);
    }
}

} // namespace

// refs: DN-102.D2
// invariant: the owner lives and dies in a private pid namespace, so a reaper any other process
// starts on the host judges the segment Unknown and cannot remove it before this arm's own reap.
// invariant: the checkpoint runs one such reaper between the owner's death and that reap.
TEST(SegmentReaper, RemovesASegmentWhoseOwnerDiedWithoutClosingIt)
{
    if (coderoast::ipc::testing::in_a_private_pid_namespace_rerun())
    {
        const char* name{std::getenv(kDeadOwnerVariable)};
        ASSERT_NE(name, nullptr) << kDeadOwnerVariable << " is unset in the rerun";
        judge_a_dead_owner_in_this_namespace(name);
        return;
    }

    const auto name{unique_channel("reap_dead")};
    const auto run{coderoast::ipc::testing::rerun_this_test_in_a_private_pid_namespace(
        {{kDeadOwnerVariable, name}})};
    // note: a segment the rerun left is Unknown to every reaper once its namespace is gone.
    if (name_resolves(name))
    {
        Channel::unlink(name);
    }
    if (run.refused)
    {
        GTEST_SKIP() << "this host refuses an unprivileged user, pid or mount namespace, so the "
                        "dead owner cannot be isolated from the host's other reapers; the line "
                        "above names the errno";
    }
    EXPECT_EQ(run.exit_code, 0) << "the rerun in the private namespace failed; its output is above";
    ASSERT_EQ(run.outside_reaps.size(), 1U)
        << "the rerun asked a reaper outside its namespace to judge " << run.outside_reaps.size()
        << " time(s); the arm asks once, between its owner's death and its own reap";
    const auto& outside{run.outside_reaps.front()};
    ASSERT_EQ(outside.segments.size(), 1U)
        << "the outside reaper listed " << outside.segments.size() << " entries under '" << name
        << "'; the owner's death leaves exactly that one segment";
    EXPECT_EQ(outside.segments.front().verdict, SegmentVerdict::Unknown)
        << "a reaper outside the namespace judged '/" << name << "' "
        << verdict_name(outside.segments.front().verdict)
        << "; an owner recorded in another pid namespace is Unknown to it";
    EXPECT_FALSE(outside.segments.front().removed)
        << "a reaper outside the namespace removed '/" << name << "' while the arm held it";
}

namespace
{

// invariant: set only in the run the executable-start arm starts, whose test body then asserts on
// the two segments its parent planted instead of planting its own.
constexpr const char* kStartOrphanVariable{"CODEROAST_IPC_START_REAPER_ORPHAN"};
constexpr const char* kStartLiveVariable{"CODEROAST_IPC_START_REAPER_LIVE"};

constexpr int kStartReaperHeld{0};
constexpr int kStartPlantFailed{3};
constexpr int kStartReportMissing{4};
constexpr int kStartOrphanRemained{5};
constexpr int kStartLiveRemoved{6};
constexpr int kStartRunFailed{7};

// post: the verdict of one run of this executable over a segment whose owner exited before the run
// started and one this process keeps alive through it.
// pre: runs as the first process of a private pid namespace, so every other reaper on the host
// judges both segments Unknown and never removes them first.
[[nodiscard]] int judge_a_start_in_this_namespace(const std::string& orphan,
                                                  const std::string& live,
                                                  const std::string& filter)
{
    const auto creator{::fork()};
    if (creator == 0)
    {
        try
        {
            const auto producer{Channel::create(coderoast::ipc::ChannelConfig{
                .name = orphan, .slot_count = 4U, .seal_grid = kAnyGrid})};
            std::_Exit(0);
        }
        catch (...)
        {
            std::_Exit(1);
        }
    }
    int creator_status{0};
    if (creator < 0 || ::waitpid(creator, &creator_status, 0) != creator ||
        !WIFEXITED(creator_status) || WEXITSTATUS(creator_status) != 0 || !name_resolves(orphan))
    {
        std::println(stderr, "[namespace init] the child that creates '/{}' left no segment",
                     orphan);
        return kStartPlantFailed;
    }

    const auto live_producer{Channel::create(
        coderoast::ipc::ChannelConfig{.name = live, .slot_count = 4U, .seal_grid = kAnyGrid})};
    const auto run{coderoast::ipc::testing::run_this_executable(
        filter, {{kStartOrphanVariable, orphan}, {kStartLiveVariable, live}})};
    const bool reported{run.output.contains("segment reaper: removed /dev/shm/" + orphan + ",")};
    const bool orphan_gone{!name_resolves(orphan)};
    const bool live_kept{name_resolves(live)};
    const bool run_passed{run.started && WIFEXITED(run.status) && WEXITSTATUS(run.status) == 0};
    if (!reported || !orphan_gone || !live_kept || !run_passed)
    {
        std::println(stderr, "[namespace init] the run of {} printed:\n{}", filter, run.output);
        std::println(stderr,
                     "[namespace init] reported removing '/{}': {}; that segment gone: {}; '/{}' "
                     "kept: {}; the run started: {}, exit status {}",
                     orphan, reported, orphan_gone, live, live_kept, run.started,
                     WIFEXITED(run.status) ? WEXITSTATUS(run.status) : -WTERMSIG(run.status));
    }
    if (!reported)
    {
        return kStartReportMissing;
    }
    if (!orphan_gone)
    {
        return kStartOrphanRemained;
    }
    if (!live_kept)
    {
        return kStartLiveRemoved;
    }
    return run_passed ? kStartReaperHeld : kStartRunFailed;
}

} // namespace

// refs: DN-102.D2
// invariant: this executable, started again, removes a segment whose owner exited before it started
// and keeps one whose owner lives, before its first test body runs.
// invariant: that start runs in a private pid namespace, so no other process's reaper on the host
// judges either segment Orphaned and removes it first, and the run reaps nothing of the host's.
TEST(SegmentReaper, TheExecutableRemovesAnOrphanAndKeepsALiveSegmentAtItsStart)
{
    const char* planted_orphan{std::getenv(kStartOrphanVariable)};
    const char* planted_live{std::getenv(kStartLiveVariable)};
    if (planted_orphan != nullptr && planted_live != nullptr)
    {
        EXPECT_FALSE(name_resolves(planted_orphan))
            << "'/" << planted_orphan
            << "', whose owner exited before this executable started, still resolves when its "
               "first test body runs";
        EXPECT_TRUE(name_resolves(planted_live))
            << "'/" << planted_live << "', whose owner is alive, was removed at this start";
        return;
    }

    const auto orphan{unique_channel("start_orphan")};
    const auto live{unique_channel("start_live")};
    const auto* const current{::testing::UnitTest::GetInstance()->current_test_info()};
    const std::string filter{std::string{current->test_suite_name()} + "." + current->name()};
    const auto run{coderoast::ipc::testing::run_in_a_private_pid_namespace(
        [&orphan, &live, &filter]
        { return judge_a_start_in_this_namespace(orphan, live, filter); })};
    for (const auto& name : {orphan, live})
    {
        if (name_resolves(name))
        {
            Channel::unlink(name);
        }
    }

    if (run.refused)
    {
        GTEST_SKIP() << "this host refuses an unprivileged user, pid or mount namespace, so no "
                        "start can be isolated from the host's other reapers; the line above "
                        "names the errno";
    }
    EXPECT_EQ(run.exit_code, kStartReaperHeld)
        << "exit 3 = no orphan could be planted; 4 = the run printed no removal of '/" << orphan
        << "', so its start reaped nothing; 5 = that segment outlived the run; 6 = the run removed "
           "'/"
        << live << "', whose owner lives; 7 = the run itself failed; 121 = the namespace's first "
        << "process died; 122 = an exception; the lines above give the run's output";
}
