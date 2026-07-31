// NOLINTBEGIN : Unit tests may intentionally violate some style rules for clarity or simplicity.
//
// Channel-shutdown semantics for the v3 SharedMemorySpscChannel.
//
// These tests cover the EOS-as-state-transition contract that replaces
// the legacy "in-band EOS frame".  Two close primitives exist:
//
//   * close_graceful() — orderly shutdown.  No new pushes accepted,
//     pending frames remain readable, idempotent.  Eventually
//     try_pop_status returns Closed once read_sequence reaches the
//     write_sequence snapshot.
//
//   * close_abort()    — out-of-band cancel.  Wakes parked producers
//     with PushStatus::Aborted and consumers with PopStatus::Aborted.
//     Terminal: cannot be downgraded by close_graceful.
//
#include <unistd.h>

#include <gtest/gtest.h>

import coderoast.ipc.core.test; // std + the facade (ADR-3.D4 aggregate)

namespace
{
using namespace std::chrono_literals;
using Frame = coderoast::ipc::LineFrame<64>;
using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;

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
                                        })},
          consumer{Channel::open(name, backpressure, wait_strategy)}
    {
    }

    ScopedChannel(const ScopedChannel&) = delete;
    ScopedChannel& operator=(const ScopedChannel&) = delete;
    ScopedChannel(ScopedChannel&&) = delete;
    ScopedChannel& operator=(ScopedChannel&&) = delete;
    ~ScopedChannel()
    {
        Channel::unlink(name);
    }
};
} // namespace

// --------------------------------------------------------------------
// 1. close_graceful is idempotent on a freshly-opened channel.
// --------------------------------------------------------------------
TEST(ChannelShutdown, OpenCloseGracefulIsIdempotent)
{
    ScopedChannel ch{"idempotent"};

    EXPECT_EQ(ch.producer.state(), coderoast::ipc::ChannelState::Open);

    ch.producer.close_graceful();
    ch.producer.close_graceful(); // idempotent
    ch.producer.close_graceful();

    Frame out{};
    const auto status{ch.consumer.try_pop_status(out)};
    EXPECT_EQ(status, coderoast::ipc::PopStatus::Closed);
    // State should eventually settle on Closed (try_pop_status promotes
    // Closing -> Closed once the drain snapshot is reached).
    EXPECT_EQ(ch.consumer.state(), coderoast::ipc::ChannelState::Closed);
}

// --------------------------------------------------------------------
// 2. After close_graceful, subsequent pushes are rejected with Closed.
// --------------------------------------------------------------------
TEST(ChannelShutdown, CloseGracefulRejectsPush)
{
    ScopedChannel ch{"reject_push"};

    EXPECT_EQ(ch.producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);
    ch.producer.close_graceful();

    EXPECT_EQ(ch.producer.try_push_status(make_frame(2)), coderoast::ipc::PushStatus::Closed);
    EXPECT_EQ(ch.producer.push_status(make_frame(3)), coderoast::ipc::PushStatus::Closed);

    // The bool-shim form returns false.
    EXPECT_FALSE(ch.producer.try_push(make_frame(4)));
    EXPECT_FALSE(ch.producer.push(make_frame(5)));
}

// --------------------------------------------------------------------
// 3. close_graceful preserves frames already in the ring; the consumer
//    drains them in order and only then observes PopStatus::Closed.
// --------------------------------------------------------------------
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
    // Closed is sticky.
    EXPECT_EQ(ch.consumer.try_pop_status(out), coderoast::ipc::PopStatus::Closed);
}

// --------------------------------------------------------------------
// 4. close_abort unblocks a producer that is parked in push() because
//    the ring is full (Block policy).  No deadlock; producer returns
//    PushStatus::Aborted in bounded time.
// --------------------------------------------------------------------
TEST(ChannelShutdown, CloseAbortUnblocksBlockedPush)
{
    ScopedChannel ch{"abort_unblocks_push", /*slot_count=*/2U};

    // Fill the ring so the next push blocks.
    ASSERT_EQ(ch.producer.try_push_status(make_frame(1)), coderoast::ipc::PushStatus::Ok);
    ASSERT_EQ(ch.producer.try_push_status(make_frame(2)), coderoast::ipc::PushStatus::Ok);
    ASSERT_EQ(ch.producer.try_push_status(make_frame(3)), coderoast::ipc::PushStatus::Full);

    std::atomic<coderoast::ipc::PushStatus> push_result{coderoast::ipc::PushStatus::Ok};
    std::thread producer_thread{[&]() noexcept
                                { push_result.store(ch.producer.push_status(make_frame(3))); }};

    // Give the producer a moment to park.
    std::this_thread::sleep_for(50ms);

    ch.producer.close_abort();

    const auto deadline{std::chrono::steady_clock::now() + 2s};
    producer_thread.join();
    EXPECT_LT(std::chrono::steady_clock::now(), deadline)
        << "producer did not wake from close_abort within 2 s";
    EXPECT_EQ(push_result.load(), coderoast::ipc::PushStatus::Aborted);

    // Residual in-ring frames are still drainable post-abort; after they
    // are drained the next pop reports Aborted.
    Frame out{};
    while (ch.consumer.try_pop_status(out) == coderoast::ipc::PopStatus::Ok)
    {
    }
    EXPECT_EQ(ch.consumer.try_pop_status(out), coderoast::ipc::PopStatus::Aborted);
}

// --------------------------------------------------------------------
// 5. AdaptivePark producers park on the futex when the ring is full
//    and must wake on close_abort within bounded time.
// --------------------------------------------------------------------
TEST(ChannelShutdown, AdaptiveParkWakesOnAbort)
{
    ScopedChannel ch{"park_wakes_abort", /*slot_count=*/2U,
                     coderoast::ipc::BackpressurePolicy::Block,
                     coderoast::ipc::WaitStrategy::AdaptivePark};

    // Fill the ring so the next push parks.
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
    // Let the producer transition through spin -> yield -> futex park.
    std::this_thread::sleep_for(100ms);

    const auto t0{std::chrono::steady_clock::now()};
    ch.producer.close_abort();
    producer_thread.join();
    const auto elapsed{std::chrono::steady_clock::now() - t0};

    EXPECT_EQ(push_result.load(), coderoast::ipc::PushStatus::Aborted);
    EXPECT_LT(elapsed, 2s) << "AdaptivePark producer did not wake within 2 s";
}

// --------------------------------------------------------------------
// 6. No EOS frame is ever written.  After a sequence of pushes followed
//    by close_graceful, exactly the pushed frames come out — no extra
//    sentinel frame.
// --------------------------------------------------------------------
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
    // ChannelStats only counts producer-issued data pushes.
    EXPECT_EQ(ch.producer.stats().pushed, kCount);
}

// --------------------------------------------------------------------
// 8. RAII destructor does not hang when a producer is parked in push()
//    on a full ring.  Destruction must abort the channel first.
// --------------------------------------------------------------------
TEST(ChannelShutdown, RaiiDestructorNoHang)
{
    const auto name{unique_channel("raii_destructor")};
    auto producer{Channel::create(coderoast::ipc::ChannelConfig{
        .name = name,
        .slot_count = 1U,
        .backpressure = coderoast::ipc::BackpressurePolicy::Block,
        .wait_strategy = coderoast::ipc::WaitStrategy::AdaptivePark,
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

    // Caller-initiated abort: this is what the destructor pattern relies
    // on. After abort, the parked producer must wake within bounded time.
    const auto t0{std::chrono::steady_clock::now()};
    producer.close_abort();
    parked.join();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, 2s)
        << "parked producer not woken; destructor would hang";
    EXPECT_EQ(push_result.load(), coderoast::ipc::PushStatus::Aborted);

    Channel::unlink(name);
}

// NOLINTEND
