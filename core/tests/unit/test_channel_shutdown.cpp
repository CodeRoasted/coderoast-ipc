#include <unistd.h>

#include <gtest/gtest.h>

import coderoast.ipc.core.test;

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

    Channel::unlink(name);
}
