// refs: ADR-11.D7, DN-102.D2, DN-102.D3, DN-142.D29
// invariant: every name here carries this process's id, so no arm opens, reserves or removes a
// segment another process created.
// invariant: no arm here changes mount topology: the four that mount a private tmpfs live in
// coderoast_ipc_namespace_tests, the package declared unconfined (DN-142.D29 (ix)).
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

#include <gtest/gtest.h>

import coderoast.ipc.core.test;

#include "segment_test_helpers.hpp"

namespace
{
using namespace coderoast::ipc::testing;
using Frame = SegmentFrame;
using Channel = SegmentChannel;
constexpr coderoast::ipc::SealGrid kAnyGrid{kAnySegmentGrid};

[[nodiscard]] Frame make_frame(std::uint64_t sequence)
{
    Frame frame{};
    frame.header.sequence = sequence;
    return frame;
}

} // namespace

// refs: ADR-11.D7
TEST(SegmentLifetime, AClosedProducerLeavesNoNameWhileAnAttachedConsumerStillDrainsToClosed)
{
    constexpr std::uint64_t kFrames{2U};
    const auto name{unique_channel("closed_producer")};
    std::optional<Channel> producer{Channel::create(
        coderoast::ipc::ChannelConfig{.name = name, .slot_count = 4U, .seal_grid = kAnyGrid})};
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

// refs: ADR-11.D7
TEST(SegmentLifetime, AProducerWhoseNameWasRecreatedLeavesTheReplacementInPlace)
{
    constexpr std::size_t kFirstSlots{4U};
    constexpr std::size_t kReplacementSlots{8U};
    const auto name{unique_channel("recreated")};
    std::optional<Channel> first{Channel::create(coderoast::ipc::ChannelConfig{
        .name = name, .slot_count = kFirstSlots, .seal_grid = kAnyGrid})};
    const auto replacement{Channel::create(coderoast::ipc::ChannelConfig{
        .name = name, .slot_count = kReplacementSlots, .seal_grid = kAnyGrid})};

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

using coderoast::ipc::ReapedSegment;
using coderoast::ipc::SegmentVerdict;

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
            const auto producer{Channel::create(coderoast::ipc::ChannelConfig{
                .name = name, .slot_count = 4U, .seal_grid = kAnyGrid})};
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
