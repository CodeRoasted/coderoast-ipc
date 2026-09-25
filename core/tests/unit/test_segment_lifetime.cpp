// refs: DN-102.D1, DN-102.D2, DN-102.D3
// invariant: every name here carries this process's id, so no arm opens, reserves or removes a
// segment another process created.
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

#include <gtest/gtest.h>

import coderoast.ipc.core.test;

namespace
{
using Frame = coderoast::ipc::LineFrame<64>;
using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

[[nodiscard]] std::string unique_channel(const char* suffix)
{
    static std::atomic<std::uint64_t> counter{0};
    return std::string{"coderoast_ipc_lifetime_"} + suffix + "_" + std::to_string(::getpid()) +
           "_" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] Frame make_frame(std::uint64_t sequence)
{
    Frame frame{};
    frame.header.sequence = sequence;
    return frame;
}

// pre: `name` carries no '.', so its segment is `/<name>` exactly as create() normalises it.
// post: whether the name still resolves to a segment; an error other than ENOENT fails the arm.
[[nodiscard]] bool name_resolves(const std::string& name)
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

} // namespace

// refs: DN-102.D1
TEST(SegmentLifetime, AClosedProducerLeavesNoNameWhileAnAttachedConsumerStillDrainsToClosed)
{
    constexpr std::uint64_t kFrames{2U};
    const auto name{unique_channel("closed_producer")};
    std::optional<Channel> producer{
        Channel::create(coderoast::ipc::ChannelConfig{.name = name, .slot_count = 4U})};
    auto consumer{Channel::open(name)};
    for (std::uint64_t sequence{1}; sequence <= kFrames; ++sequence)
    {
        ASSERT_EQ(producer->try_push_status(make_frame(sequence)), coderoast::ipc::PushStatus::Ok);
    }

    producer.reset();

    if (name_resolves(name))
    {
        ADD_FAILURE() << "the producer that created '/" << name
                      << "' was destroyed and its name still resolves, so the segment outlives "
                         "every handle that will ever read it";
        Channel::unlink(name);
    }
    Frame out{};
    for (std::uint64_t sequence{1}; sequence <= kFrames; ++sequence)
    {
        ASSERT_EQ(consumer.try_pop_status(out), coderoast::ipc::PopStatus::Ok)
            << "frame " << sequence << " of " << kFrames
            << " written before the close is gone from the attached consumer's mapping";
        EXPECT_EQ(out.header.sequence, sequence);
    }
    EXPECT_EQ(consumer.try_pop_status(out), coderoast::ipc::PopStatus::Closed)
        << "the attached consumer must read the producer's graceful close after its last frame";
}

// refs: DN-102.D1
TEST(SegmentLifetime, AProducerWhoseNameWasRecreatedLeavesTheReplacementInPlace)
{
    constexpr std::size_t kFirstSlots{4U};
    constexpr std::size_t kReplacementSlots{8U};
    const auto name{unique_channel("recreated")};
    std::optional<Channel> first{
        Channel::create(coderoast::ipc::ChannelConfig{.name = name, .slot_count = kFirstSlots})};
    const auto replacement{Channel::create(
        coderoast::ipc::ChannelConfig{.name = name, .slot_count = kReplacementSlots})};

    first.reset();

    try
    {
        const auto reopened{Channel::open(name)};
        EXPECT_EQ(reopened.slot_count(), kReplacementSlots)
            << "'/" << name << "' reopened to a " << reopened.slot_count()
            << "-slot segment; the replacement created after the first producer has "
            << kReplacementSlots;
    }
    catch (const std::exception& error)
    {
        ADD_FAILURE() << "destroying the first producer removed the name its replacement holds: "
                         "reopening '/"
                      << name << "' failed with '" << error.what() << "'";
    }
}

namespace
{

constexpr int kRefusedAndSurvived{0};
constexpr int kNamespaceUnavailable{2};
constexpr int kCreateReturned{3};
constexpr int kRefusalIncomplete{4};
constexpr int kNameLeftBehind{5};
constexpr int kWrongException{6};

constexpr std::size_t kSmallTmpfsBytes{std::size_t{1024} * 1024};

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

// post: the child's verdict, after it gave itself a private /dev/shm of kSmallTmpfsBytes in an
// unprivileged user and mount namespace and asked for a segment twice that size.
// invariant: it runs only in a forked child, which dies with that namespace; the host's /dev/shm
// is never touched.
[[nodiscard]] int create_past_a_small_tmpfs() noexcept
{
    try
    {
        const auto uid{::getuid()};
        const auto gid{::getgid()};
        if (::unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0 ||
            !write_proc_file("/proc/self/setgroups", "deny") ||
            !write_proc_file("/proc/self/uid_map", std::format("0 {} 1", uid)) ||
            !write_proc_file("/proc/self/gid_map", std::format("0 {} 1", gid)) ||
            ::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0 ||
            ::mount("tmpfs", "/dev/shm", "tmpfs", 0,
                    std::format("size={}", kSmallTmpfsBytes).c_str()) != 0)
        {
            std::println(stderr, "[child] no private tmpfs: {}",
                         std::error_code(errno, std::generic_category()).message());
            return kNamespaceUnavailable;
        }

        using Wide = coderoast::ipc::SharedMemorySpscChannel<coderoast::ipc::DefaultLineFrame>;
        const std::size_t slots{(2U * kSmallTmpfsBytes) / sizeof(coderoast::ipc::DefaultLineFrame)};
        const auto name{unique_channel("past_tmpfs")};
        const auto requested{Wide::segment_bytes(slots).value_or(0U)};
        try
        {
            const auto channel{
                Wide::create(coderoast::ipc::ChannelConfig{.name = name, .slot_count = slots})};
            std::println(stderr, "[child] a {} B create returned over a {} B tmpfs", requested,
                         kSmallTmpfsBytes);
            return kCreateReturned;
        }
        catch (const std::runtime_error& refusal)
        {
            const std::string_view message{refusal.what()};
            std::println(stderr, "[child] refused: {}", message);
            const auto no_space{std::make_error_code(std::errc::no_space_on_device).message()};
            if (!message.contains(name) || !message.contains(std::to_string(requested)) ||
                !message.contains(no_space))
            {
                std::println(stderr,
                             "[child] the refusal must name the channel '{}', the {} B requested "
                             "and '{}'",
                             name, requested, no_space);
                return kRefusalIncomplete;
            }
            if (name_resolves(name))
            {
                std::println(stderr, "[child] the refused create left '/{}' behind", name);
                return kNameLeftBehind;
            }
            return kRefusedAndSurvived;
        }
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "[child] unexpected exception: {}", error.what());
        return kWrongException;
    }
}

} // namespace

// refs: DN-102.D3
TEST(SegmentReservation, ACreateLargerThanTheTmpfsIsRefusedAndTheProcessSurvives)
{
    const auto child{::fork()};
    ASSERT_GE(child, 0) << "fork failed: "
                        << std::error_code(errno, std::generic_category()).message();
    if (child == 0)
    {
        std::_Exit(create_past_a_small_tmpfs());
    }

    int status{0};
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    if (WIFEXITED(status) && WEXITSTATUS(status) == kNamespaceUnavailable)
    {
        GTEST_SKIP() << "this host refuses an unprivileged user and mount namespace, so no private "
                        "tmpfs can be sized; the child's line above names the errno";
    }
    ASSERT_TRUE(WIFEXITED(status))
        << "the child died by signal " << WTERMSIG(status)
        << (WTERMSIG(status) == SIGBUS
                ? " (SIGBUS): the create committed pages its tmpfs could not hold"
                : "");
    EXPECT_EQ(WEXITSTATUS(status), kRefusedAndSurvived)
        << "exit 3 = the create returned; 4 = the refusal omits the channel, the bytes or the "
           "errno; 5 = the refused create left its name; 6 = another exception";
}

namespace
{

using coderoast::ipc::ReapedSegment;
using coderoast::ipc::SegmentVerdict;

[[nodiscard]] std::string_view verdict_name(SegmentVerdict verdict) noexcept
{
    switch (verdict)
    {
    case SegmentVerdict::Alive:
        return "Alive";
    case SegmentVerdict::Orphaned:
        return "Orphaned";
    case SegmentVerdict::Unknown:
        return "Unknown";
    }
    return "an undeclared verdict";
}

// post: the reaper's reading of `name` alone, the only entry its prefix admits; an empty or wider
// reading fails the arm and yields nullopt.
[[nodiscard]] std::optional<ReapedSegment> reap_only(const std::string& name)
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

// invariant: a forked child creates `name` and holds its producer until release() or destruction,
// so the segment's owner is alive and is not this process.
class LiveOwner
{
  public:
    explicit LiveOwner(const std::string& name)
    {
        std::array<int, 2> ready{-1, -1};
        if (::pipe(ready.data()) != 0 || ::pipe(release_.data()) != 0)
        {
            ADD_FAILURE() << "pipe failed: "
                          << std::error_code(errno, std::generic_category()).message();
            return;
        }
        child_ = ::fork();
        if (child_ == 0)
        {
            static_cast<void>(::close(ready[0]));
            static_cast<void>(::close(release_[1]));
            hold_until_released(name, ready[1], release_[0]);
        }
        static_cast<void>(::close(ready[1]));
        static_cast<void>(::close(release_[0]));
        char signal{0};
        ready_ = child_ > 0 && ::read(ready[0], &signal, 1) == 1;
        static_cast<void>(::close(ready[0]));
    }

    LiveOwner(const LiveOwner&) = delete;
    LiveOwner& operator=(const LiveOwner&) = delete;
    LiveOwner(LiveOwner&&) = delete;
    LiveOwner& operator=(LiveOwner&&) = delete;

    ~LiveOwner()
    {
        static_cast<void>(::close(release_[1]));
        if (child_ > 0)
        {
            int status{0};
            static_cast<void>(::waitpid(child_, &status, 0));
        }
    }

    [[nodiscard]] bool ready() const noexcept
    {
        return ready_;
    }

  private:
    [[noreturn]] static void hold_until_released(const std::string& name, int ready_fd,
                                                 int release_fd) noexcept
    {
        try
        {
            const auto producer{
                Channel::create(coderoast::ipc::ChannelConfig{.name = name, .slot_count = 4U})};
            const char signal{1};
            if (::write(ready_fd, &signal, 1) != 1)
            {
                std::_Exit(1);
            }
            char released{0};
            static_cast<void>(::read(release_fd, &released, 1));
        }
        catch (...)
        {
            std::_Exit(1);
        }
        std::_Exit(0);
    }

    std::array<int, 2> release_{-1, -1};
    ::pid_t child_{-1};
    bool ready_{false};
};

// pre: `name` carries no '.', as unique_channel's names never do.
// post: `name` holds `bytes` as a plain file under /dev/shm, created by this process.
void plant_file(const std::string& name, std::span<const std::byte> bytes)
{
    const auto path{"/" + name};
    const int descriptor{::shm_open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600)};
    ASSERT_GE(descriptor, 0) << "shm_open('" << path
                             << "'): " << std::error_code(errno, std::generic_category()).message();
    const auto written{::write(descriptor, bytes.data(), bytes.size())};
    static_cast<void>(::close(descriptor));
    ASSERT_EQ(written, static_cast<::ssize_t>(bytes.size())) << "planting '" << path << "'";
}

// invariant: larger than any channel header, so a planted file is never Unknown for being short.
constexpr std::size_t kPlantedHeaderBytes{4096U};

} // namespace

// refs: DN-102.D2
TEST(SegmentOwnerVerdict, JudgesTheAnchorByPidStartTimeAndPidNamespace)
{
    using coderoast::ipc::judge_segment_owner;
    const auto self{coderoast::ipc::current_segment_owner()};
    ASSERT_GT(self.pid, 0);
    ASSERT_NE(self.start_time, 0U) << "/proc/" << self.pid << "/stat gave no start time";
    ASSERT_NE(self.pid_namespace, 0U) << "/proc/self/ns/pid gave no namespace inode";

    const auto judged_self{judge_segment_owner(self)};
    EXPECT_EQ(judged_self, SegmentVerdict::Alive)
        << "this process, as it names itself, was judged " << verdict_name(judged_self);

    auto reused{self};
    ++reused.start_time;
    const auto judged_reused{judge_segment_owner(reused)};
    EXPECT_EQ(judged_reused, SegmentVerdict::Orphaned)
        << "pid " << self.pid << " started at tick " << self.start_time << ", not "
        << reused.start_time << ", so the recorded owner is dead and its pid reused; judged "
        << verdict_name(judged_reused);

    auto foreign{self};
    ++foreign.pid_namespace;
    const auto judged_foreign{judge_segment_owner(foreign)};
    EXPECT_EQ(judged_foreign, SegmentVerdict::Unknown)
        << "a pid from namespace inode " << foreign.pid_namespace << " names nothing in this one ("
        << self.pid_namespace << "); judged " << verdict_name(judged_foreign);

    auto unstamped{self};
    unstamped.start_time = 0U;
    const auto judged_unstamped{judge_segment_owner(unstamped)};
    EXPECT_EQ(judged_unstamped, SegmentVerdict::Unknown)
        << "an owner whose start time /proc could not supply is never judged dead; judged "
        << verdict_name(judged_unstamped);
}

// refs: DN-102.D2
TEST(SegmentReaper, SparesASegmentWhoseOwnerIsAliveInAnotherProcess)
{
    const auto name{unique_channel("reap_live")};
    const LiveOwner owner{name};
    ASSERT_TRUE(owner.ready()) << "the child never reported its producer created";

    const auto reaped{reap_only(name)};
    ASSERT_TRUE(reaped.has_value());
    EXPECT_EQ(reaped->verdict, SegmentVerdict::Alive)
        << "the live child's segment was judged " << verdict_name(reaped->verdict);
    EXPECT_FALSE(reaped->removed);
    EXPECT_TRUE(name_resolves(name))
        << "the reaper removed '/" << name << "', whose producer is a live process";
}

// refs: DN-102.D2
TEST(SegmentReaper, RemovesASegmentWhoseOwnerDiedWithoutClosingIt)
{
    const auto name{unique_channel("reap_dead")};
    const auto child{::fork()};
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        try
        {
            const auto producer{
                Channel::create(coderoast::ipc::ChannelConfig{.name = name, .slot_count = 4U})};
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

// refs: DN-102.D2
TEST(SegmentReaper, KeepsEveryFileWhoseHeaderItCannotReadAsThisAbisChannel)
{
    std::array<std::byte, kPlantedHeaderBytes> foreign_bytes{};
    foreign_bytes.fill(std::byte{0xAB});
    // invariant: the wire header opens with the magic, then the ABI version.
    std::array<std::byte, kPlantedHeaderBytes> older_abi{};
    const auto older_version{coderoast::ipc::kSharedChannelAbiVersion - 1U};
    std::memcpy(older_abi.data(), &coderoast::ipc::kSharedChannelMagic,
                sizeof(coderoast::ipc::kSharedChannelMagic));
    std::memcpy(std::span{older_abi}.subspan(sizeof(coderoast::ipc::kSharedChannelMagic)).data(),
                &older_version, sizeof(older_version));
    const std::array<std::byte, 1> short_bytes{std::byte{0}};

    const std::array planted{
        std::pair{unique_channel("reap_foreign"), std::span<const std::byte>{foreign_bytes}},
        std::pair{unique_channel("reap_older_abi"), std::span<const std::byte>{older_abi}},
        std::pair{unique_channel("reap_short"), std::span<const std::byte>{short_bytes}},
    };
    for (const auto& [name, bytes] : planted)
    {
        plant_file(name, bytes);
    }

    for (const auto& [name, bytes] : planted)
    {
        const auto reaped{reap_only(name)};
        if (reaped.has_value())
        {
            EXPECT_EQ(reaped->verdict, SegmentVerdict::Unknown)
                << "'/" << name << "' (" << bytes.size() << " B) was judged "
                << verdict_name(reaped->verdict);
            EXPECT_FALSE(reaped->removed);
        }
        EXPECT_TRUE(name_resolves(name))
            << "the reaper removed '/" << name << "', which it cannot read as a channel header";
        Channel::unlink(name);
    }
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
constexpr int kStartInitDied{8};
constexpr int kStartThrew{9};
constexpr int kExecFailed{127};

constexpr std::size_t kOutputChunkBytes{4096U};

struct ExecutableRun
{
    std::string output;
    int status{-1};
    bool started{false};
};

// post: this executable run again on `filter` alone, the two names in its environment, with its
// stdout and stderr captured together.
[[nodiscard]] ExecutableRun run_this_executable(const std::string& filter,
                                                const std::string& orphan, const std::string& live)
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
        if (::setenv(kStartOrphanVariable, orphan.c_str(), 1) == 0 &&
            ::setenv(kStartLiveVariable, live.c_str(), 1) == 0)
        {
            static_cast<void>(::execv(executable.c_str(), arguments.data()));
        }
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

// post: the verdict of one run of this executable over a segment whose owner exited before the run
// started and one this process keeps alive through it.
// invariant: it runs as the first process of a private pid namespace with its own /proc, so every
// other reaper on the host judges both segments Unknown and never removes them first.
[[nodiscard]] int judge_a_start_in_this_namespace(const std::string& orphan,
                                                  const std::string& live,
                                                  const std::string& filter)
{
    if (::mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr) != 0 ||
        ::mount("proc", "/proc", "proc", 0, nullptr) != 0)
    {
        std::println(stderr, "[namespace init] no private /proc: {}",
                     std::error_code(errno, std::generic_category()).message());
        return kNamespaceUnavailable;
    }

    const auto creator{::fork()};
    if (creator == 0)
    {
        try
        {
            const auto producer{
                Channel::create(coderoast::ipc::ChannelConfig{.name = orphan, .slot_count = 4U})};
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

    const auto live_producer{
        Channel::create(coderoast::ipc::ChannelConfig{.name = live, .slot_count = 4U})};
    const auto run{run_this_executable(filter, orphan, live)};
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

// post: the verdict of judge_a_start_in_this_namespace, run from a new user, pid and mount
// namespace; kNamespaceUnavailable when the host refuses one.
[[nodiscard]] int judge_a_start_in_a_private_pid_namespace(const std::string& orphan,
                                                           const std::string& live,
                                                           const std::string& filter) noexcept
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
            std::println(stderr, "[child] no private pid namespace: {}",
                         std::error_code(errno, std::generic_category()).message());
            return kNamespaceUnavailable;
        }
        const auto init{::fork()};
        if (init == 0)
        {
            try
            {
                std::_Exit(judge_a_start_in_this_namespace(orphan, live, filter));
            }
            catch (const std::exception& error)
            {
                std::println(stderr, "[namespace init] unexpected exception: {}", error.what());
                std::_Exit(kStartThrew);
            }
        }
        int status{0};
        if (init < 0 || ::waitpid(init, &status, 0) != init || !WIFEXITED(status))
        {
            std::println(stderr,
                         "[child] the namespace's first process ended without an exit code");
            return kStartInitDied;
        }
        return WEXITSTATUS(status);
    }
    catch (const std::exception& error)
    {
        std::println(stderr, "[child] unexpected exception: {}", error.what());
        return kStartThrew;
    }
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
    const auto child{::fork()};
    ASSERT_GE(child, 0) << "fork failed: "
                        << std::error_code(errno, std::generic_category()).message();
    if (child == 0)
    {
        std::_Exit(judge_a_start_in_a_private_pid_namespace(orphan, live, filter));
    }
    int status{0};
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    for (const auto& name : {orphan, live})
    {
        if (name_resolves(name))
        {
            Channel::unlink(name);
        }
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == kNamespaceUnavailable)
    {
        GTEST_SKIP() << "this host refuses an unprivileged user, pid or mount namespace, so no "
                        "start can be isolated from the host's other reapers; the child's line "
                        "above names the errno";
    }
    ASSERT_TRUE(WIFEXITED(status)) << "the child died by signal " << WTERMSIG(status);
    const int verdict{WEXITSTATUS(status)};
    EXPECT_EQ(verdict, kStartReaperHeld)
        << "exit 3 = no orphan could be planted; 4 = the run printed no removal of '/" << orphan
        << "', so its start reaped nothing; 5 = that segment outlived the run; 6 = the run removed "
           "'/"
        << live << "', whose owner lives; 7 = the run itself failed; 8 = the namespace's first "
        << "process died; 9 = an exception; the lines above give the run's output";
}
