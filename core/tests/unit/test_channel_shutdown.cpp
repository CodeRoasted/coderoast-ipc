#include <fcntl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

import coderoast.ipc.core.test;

namespace
{
using namespace std::chrono_literals;
using Frame = coderoast::ipc::LineFrame<64>;
using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

// refs: DN-103.D29
// invariant: the seal grid these channels declare; nothing here admits by it, so any non-zero one.
constexpr coderoast::ipc::SealGrid kAnyGrid{
    .origin_unix_ns = 0U, .window_length_ns = 1U, .frontier_step_ns = 1U};

[[nodiscard]] std::string unique_channel(const char* suffix)
{
    static std::atomic<std::uint64_t> counter{0};
    return std::string{"coderoast_ipc_shutdown_"} + suffix + "_" + std::to_string(::getpid()) +
           "_" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
}

[[nodiscard]] Frame make_frame(std::uint64_t sequence)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.payload_size = 0U;
    return frame;
}

struct ScopedChannel
{
    std::string name;
    Channel producer;
    Channel consumer;

    explicit ScopedChannel(
        const char* suffix, std::size_t slot_count = 8U,
        coderoast::ipc::BackpressurePolicy backpressure = coderoast::ipc::BackpressurePolicy::Block,
        coderoast::ipc::WaitStrategy wait_strategy = coderoast::ipc::WaitStrategy::AdaptivePark)
        : name{unique_channel(suffix)}, producer{Channel::create(coderoast::ipc::ChannelConfig{
                                            .name = name,
                                            .slot_count = slot_count,
                                            .backpressure = backpressure,
                                            .wait_strategy = wait_strategy,
                                            .seal_grid = kAnyGrid,
                                        })},
          consumer{Channel::open(name, backpressure, wait_strategy)}
    {
    }

    ScopedChannel(const ScopedChannel&) = delete;
    ScopedChannel& operator=(const ScopedChannel&) = delete;
    ScopedChannel(ScopedChannel&&) = delete;
    ScopedChannel& operator=(ScopedChannel&&) = delete;
    ~ScopedChannel() = default;
};
} // namespace

TEST(ChannelShutdown, OpenCloseGracefulIsIdempotent)
{
    ScopedChannel ch{"idempotent"};

    EXPECT_EQ(ch.producer.state(), coderoast::ipc::ChannelState::Open);

    ch.producer.close_graceful();
    ch.producer.close_graceful();
    ch.producer.close_graceful();

    Frame out{};
    const auto status{ch.consumer.try_pop_status(out)};
    EXPECT_EQ(status, coderoast::ipc::PopStatus::Closed);
    // assert: try_pop_status is what promotes Closing to Closed; state() alone never does.
    EXPECT_EQ(ch.consumer.state(), coderoast::ipc::ChannelState::Closed);
}

TEST(ChannelShutdown, CloseGracefulRejectsPush)
{
    ScopedChannel ch{"reject_push"};

    EXPECT_EQ(ch.producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);
    ch.producer.close_graceful();

    EXPECT_EQ(ch.producer.try_push_status(make_frame(2)), coderoast::ipc::PushStatus::Closed);
    EXPECT_EQ(ch.producer.push_status(make_frame(3)), coderoast::ipc::PushStatus::Closed);

    EXPECT_FALSE(ch.producer.try_push(make_frame(4)));
    EXPECT_FALSE(ch.producer.push(make_frame(5)));
}

TEST(ChannelShutdown, CloseGracefulPreservesPending)
{
    ScopedChannel ch{"preserve_pending"};

    constexpr std::uint64_t kCount{5U};
    for (std::uint64_t i{1}; i <= kCount; ++i)
    {
        ASSERT_EQ(ch.producer.try_push_status(make_frame(i)), coderoast::ipc::PushStatus::Ok);
    }
    ch.producer.close_graceful();

    Frame out{};
    for (std::uint64_t i{1}; i <= kCount; ++i)
    {
        ASSERT_EQ(ch.consumer.try_pop_status(out), coderoast::ipc::PopStatus::Ok)
            << "frame " << i << " should have been preserved";
        EXPECT_EQ(out.header.sequence, i);
    }

    EXPECT_EQ(ch.consumer.try_pop_status(out), coderoast::ipc::PopStatus::Closed);
    // assert: Closed is sticky — a further pop cannot fall back to Empty.
    EXPECT_EQ(ch.consumer.try_pop_status(out), coderoast::ipc::PopStatus::Closed);
}

TEST(ChannelShutdown, CloseAbortUnblocksBlockedPush)
{
    ScopedChannel ch{"abort_unblocks_push", /*slot_count=*/2U};

    ASSERT_EQ(ch.producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);
    ASSERT_EQ(ch.producer.try_push_status(make_frame(2)), coderoast::ipc::PushStatus::Ok);
    ASSERT_EQ(ch.producer.try_push_status(make_frame(3)), coderoast::ipc::PushStatus::Full);

    std::atomic<coderoast::ipc::PushStatus> push_result{coderoast::ipc::PushStatus::Ok};
    std::thread producer_thread{[&]() noexcept
                                { push_result.store(ch.producer.push_status(make_frame(3))); }};

    // assert: the sleep lets the producer reach the park; too short and the abort would race the
    // park rather than exercise the wake.
    std::this_thread::sleep_for(50ms);

    ch.producer.close_abort();

    const auto deadline{std::chrono::steady_clock::now() + 2s};
    producer_thread.join();
    EXPECT_LT(std::chrono::steady_clock::now(), deadline)
        << "producer did not wake from close_abort within 2 s";
    EXPECT_EQ(push_result.load(), coderoast::ipc::PushStatus::Aborted);

    Frame out{};
    while (ch.consumer.try_pop_status(out) == coderoast::ipc::PopStatus::Ok)
    {
    }
    EXPECT_EQ(ch.consumer.try_pop_status(out), coderoast::ipc::PopStatus::Aborted);
}

TEST(ChannelShutdown, AdaptiveParkWakesOnAbort)
{
    ScopedChannel ch{"park_wakes_abort", /*slot_count=*/2U,
                     coderoast::ipc::BackpressurePolicy::Block,
                     coderoast::ipc::WaitStrategy::AdaptivePark};

    ASSERT_EQ(ch.producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);
    ASSERT_EQ(ch.producer.try_push_status(make_frame(2)), coderoast::ipc::PushStatus::Ok);

    std::atomic<bool> entered{false};
    std::atomic<coderoast::ipc::PushStatus> push_result{coderoast::ipc::PushStatus::Ok};
    std::thread producer_thread{[&]() noexcept
                                {
                                    entered.store(true, std::memory_order_release);
                                    push_result.store(ch.producer.push_status(make_frame(3)),
                                                      std::memory_order_release);
                                }};

    while (!entered.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    // assert: long enough for the strategy to walk spin, then yield, then the futex park.
    std::this_thread::sleep_for(100ms);

    const auto t0{std::chrono::steady_clock::now()};
    ch.producer.close_abort();
    producer_thread.join();
    const auto elapsed{std::chrono::steady_clock::now() - t0};

    EXPECT_EQ(push_result.load(), coderoast::ipc::PushStatus::Aborted);
    EXPECT_LT(elapsed, 2s) << "AdaptivePark producer did not wake within 2 s";
}

TEST(ChannelShutdown, NoEosFrameWritten)
{
    ScopedChannel ch{"no_eos_frame"};

    constexpr std::uint64_t kCount{3U};
    for (std::uint64_t i{1}; i <= kCount; ++i)
    {
        ASSERT_EQ(ch.producer.try_push_status(make_frame(i)), coderoast::ipc::PushStatus::Ok);
    }
    ch.producer.close_graceful();

    Frame out{};
    std::size_t popped{0};
    for (;;)
    {
        const auto status{ch.consumer.try_pop_status(out)};
        if (status == coderoast::ipc::PopStatus::Ok)
        {
            ++popped;
            continue;
        }
        EXPECT_EQ(status, coderoast::ipc::PopStatus::Closed);
        break;
    }

    EXPECT_EQ(popped, kCount) << "no synthetic EOS frame should be in the stream";
    EXPECT_EQ(ch.producer.stats().pushed, kCount);
}

TEST(ChannelShutdown, RaiiDestructorNoHang)
{
    const auto name{unique_channel("raii_destructor")};
    auto producer{Channel::create(coderoast::ipc::ChannelConfig{
        .name = name,
        .slot_count = 1U,
        .backpressure = coderoast::ipc::BackpressurePolicy::Block,
        .wait_strategy = coderoast::ipc::WaitStrategy::AdaptivePark,
        .seal_grid = kAnyGrid,
    })};

    ASSERT_EQ(producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);

    std::atomic<bool> entered{false};
    std::atomic<coderoast::ipc::PushStatus> push_result{coderoast::ipc::PushStatus::Ok};
    std::thread parked{[&]() noexcept
                       {
                           entered.store(true, std::memory_order_release);
                           push_result.store(producer.push_status(make_frame(2)),
                                             std::memory_order_release);
                       }};

    while (!entered.load(std::memory_order_acquire))
    {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(100ms);

    // note: the destructor wakes a parked pusher the same way, through notify_state_change().
    const auto t0{std::chrono::steady_clock::now()};
    producer.close_abort();
    parked.join();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s)
        << "parked producer not woken; destructor would hang";
    EXPECT_EQ(push_result.load(), coderoast::ipc::PushStatus::Aborted);
}

// refs: DN-103.D29
// invariant: the consumer of a channel may live in another process, as the MCP sidecar does, so a
// Block producer parked on AdaptivePark is woken by that process's pop; a private futex never is.
TEST(ChannelShutdown, AdaptiveParkIsWokenByAPopInAnotherProcess)
{
    constexpr auto kChildPopsAfter{300ms};
    // invariant: under libc++ a private futex wait times out after 2 s, so a bound past that would
    // read a missed cross-process wake as a slow one.
    constexpr auto kWakeBound{1s};
    ScopedChannel ch{"park_cross_process", /*slot_count=*/1U,
                     coderoast::ipc::BackpressurePolicy::Block,
                     coderoast::ipc::WaitStrategy::AdaptivePark};
    ASSERT_EQ(ch.producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);

    // note: forked before any thread starts; the child touches only its inherited shared mapping.
    const ::pid_t child{::fork()};
    ASSERT_GE(child, 0) << "fork failed";
    if (child == 0)
    {
        std::this_thread::sleep_for(kChildPopsAfter);
        Frame out{};
        std::_Exit(ch.consumer.try_pop(out) ? 0 : 1);
    }

    std::atomic<bool> returned{false};
    std::atomic<coderoast::ipc::PushStatus> push_result{coderoast::ipc::PushStatus::Full};
    std::thread producer_thread{[&]() noexcept
                                {
                                    push_result.store(ch.producer.push_status(make_frame(2)));
                                    returned.store(true, std::memory_order_release);
                                }};
    const auto deadline{std::chrono::steady_clock::now() + kChildPopsAfter + kWakeBound};
    while (!returned.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    const bool woken_by_the_pop{returned.load(std::memory_order_acquire)};
    if (!woken_by_the_pop)
    {
        // note: an abort in this process releases the park, so the arm reds instead of hanging.
        ch.producer.close_abort();
    }
    producer_thread.join();
    int child_status{0};
    ASSERT_EQ(::waitpid(child, &child_status, 0), child);

    EXPECT_TRUE(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0)
        << "the child process did not pop the frame";
    EXPECT_TRUE(woken_by_the_pop)
        << "the parked producer was still parked "
        << std::chrono::duration_cast<std::chrono::milliseconds>(kWakeBound).count()
        << " ms after another process popped a frame and freed its slot";
    EXPECT_EQ(push_result.load(), coderoast::ipc::PushStatus::Ok);
}

namespace
{
// invariant: the header's state byte opens its last cache line, which ends where the slots begin.
constexpr std::size_t kHeaderCacheLineBytes{64U};

// post: the state byte read through the segment's file, as every mapping sees it; nullopt after a
// failed read, which fails the arm.
[[nodiscard]] std::optional<std::uint8_t> read_state_byte(const std::string& name, ::off_t offset)
{
    const auto path{"/" + name};
    const int descriptor{::shm_open(path.c_str(), O_RDONLY, 0)};
    if (descriptor < 0)
    {
        ADD_FAILURE() << "shm_open('" << path
                      << "'): " << std::error_code(errno, std::generic_category()).message();
        return std::nullopt;
    }
    std::uint8_t value{0};
    const auto read{::pread(descriptor, &value, sizeof(value), offset)};
    static_cast<void>(::close(descriptor));
    if (read != static_cast<::ssize_t>(sizeof(value)))
    {
        ADD_FAILURE() << "pread of the state byte at offset " << offset << " returned " << read;
        return std::nullopt;
    }
    return value;
}

void write_state_byte(const std::string& name, ::off_t offset, coderoast::ipc::ChannelState state)
{
    const auto path{"/" + name};
    const int descriptor{::shm_open(path.c_str(), O_RDWR, 0)};
    ASSERT_GE(descriptor, 0) << "shm_open('" << path
                             << "'): " << std::error_code(errno, std::generic_category()).message();
    const auto value{static_cast<std::uint8_t>(state)};
    const auto written{::pwrite(descriptor, &value, sizeof(value), offset)};
    static_cast<void>(::close(descriptor));
    ASSERT_EQ(written, static_cast<::ssize_t>(sizeof(value)));
}

// post: the state byte's offset for `slot_count` slots, derived from the layout and witnessed by a
// close_graceful and a close_abort, so a reordered header reds here, never poking another field.
[[nodiscard]] std::optional<::off_t> witnessed_state_offset(std::size_t slot_count)
{
    const auto segment{Channel::segment_bytes(slot_count)};
    if (!segment.has_value())
    {
        ADD_FAILURE() << "segment_bytes(" << slot_count << ") is not representable";
        return std::nullopt;
    }
    const auto offset{
        static_cast<::off_t>(*segment - (slot_count * sizeof(Frame)) - kHeaderCacheLineBytes)};
    ScopedChannel witness{"state_offset_witness", slot_count};
    const auto open_byte{read_state_byte(witness.name, offset)};
    witness.producer.close_graceful();
    const auto closing_byte{read_state_byte(witness.name, offset)};
    witness.producer.close_abort();
    const auto aborted_byte{read_state_byte(witness.name, offset)};
    const bool witnessed{
        open_byte == static_cast<std::uint8_t>(coderoast::ipc::ChannelState::Open) &&
        closing_byte == static_cast<std::uint8_t>(coderoast::ipc::ChannelState::Closing) &&
        aborted_byte == static_cast<std::uint8_t>(coderoast::ipc::ChannelState::Aborted)};
    if (!witnessed)
    {
        ADD_FAILURE() << "the byte at offset " << offset << " read "
                      << static_cast<int>(open_byte.value_or(0xFFU)) << ", "
                      << static_cast<int>(closing_byte.value_or(0xFFU)) << ", "
                      << static_cast<int>(aborted_byte.value_or(0xFFU))
                      << " across Open, close_graceful and close_abort; expected 0, 1, 3";
        return std::nullopt;
    }
    return offset;
}
} // namespace

// invariant: a pop whose write load precedes a push, and whose state load falls between a close's
// state change and its closing_at store, sees this header: empty ring, Closing, closing_at unset.
// assert: Closed there loses the frame pushed before the close, so the pop must report Empty.
TEST(ChannelShutdown, ClosingBeforeClosingAtIsPublishedIsNotClosed)
{
    constexpr std::size_t kSlots{8U};
    const auto offset{witnessed_state_offset(kSlots)};
    ASSERT_TRUE(offset.has_value());

    ScopedChannel ch{"closing_unpublished", kSlots};
    ASSERT_EQ(ch.producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);
    Frame out{};
    ASSERT_EQ(ch.consumer.try_pop_status(out), coderoast::ipc::PopStatus::Ok);
    write_state_byte(ch.name, *offset, coderoast::ipc::ChannelState::Closing);
    ASSERT_EQ(ch.consumer.state(), coderoast::ipc::ChannelState::Closing);

    const auto status{ch.consumer.try_pop_status(out)};
    EXPECT_EQ(status, coderoast::ipc::PopStatus::Empty)
        << "try_pop_status returned PopStatus " << static_cast<int>(status)
        << " (expected Empty = " << static_cast<int>(coderoast::ipc::PopStatus::Empty)
        << ", Closed = " << static_cast<int>(coderoast::ipc::PopStatus::Closed)
        << ") at read position 1 with the state Closing and closing_at never published";
    EXPECT_EQ(ch.consumer.state(), coderoast::ipc::ChannelState::Closing)
        << "the pop promoted the state to " << static_cast<int>(ch.consumer.state())
        << " before closing_at was published";
}

// invariant: a Block push on a full ring nobody pops stays inside push_status until a close, so the
// push is held mid-flight with no sleep and no timing.
// assert: close_graceful with that push in flight aborts in a build without NDEBUG — a graceful
// close never overlaps a push, and close_abort is the wake for a producer blocked in push.
TEST(ChannelShutdownDeathTest, CloseGracefulWithAPushInFlightAborts)
{
#ifdef NDEBUG
    GTEST_SKIP() << "the in-flight check compiles out under NDEBUG; a debug profile runs this arm";
#else
    ScopedChannel ch{"graceful_overlaps_push", /*slot_count=*/1U};
    ASSERT_EQ(ch.producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);
    ASSERT_EQ(ch.producer.pushes_in_flight(), 0U);

    EXPECT_DEATH(
        {
            std::thread pusher{[&ch]() noexcept
                               { static_cast<void>(ch.producer.push_status(make_frame(2))); }};
            while (ch.producer.pushes_in_flight() == 0U)
            {
                std::this_thread::yield();
            }
            ch.producer.close_graceful();
            pusher.join();
        },
        "close_graceful on channel '.*' ran with 1 push\\(es\\) in flight through the same handle");
    EXPECT_EQ(ch.producer.state(), coderoast::ipc::ChannelState::Open)
        << "the child's close reached this process's segment state before its abort";
#endif
}
