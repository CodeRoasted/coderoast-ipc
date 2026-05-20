#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

namespace coderoast::ipc
{

inline constexpr std::uint64_t kSharedChannelMagic{0x4352495043535053ULL}; // CRIPCSPS
inline constexpr std::uint32_t kSharedChannelAbiVersion{2U};
inline constexpr std::size_t kDefaultSharedChannelSlotCount{8192U};

enum class BackpressurePolicy : std::uint8_t
{
    Block,
    DropNewest,
    OverwriteOldest,
};

enum class WaitStrategy : std::uint8_t
{
    SpinYieldPark,
    YieldPark,
    ParkOnly,
};

struct ChannelConfig
{
    std::string name;
    std::size_t slot_count{kDefaultSharedChannelSlotCount};
    BackpressurePolicy backpressure{BackpressurePolicy::Block};
    WaitStrategy wait_strategy{WaitStrategy::SpinYieldPark};
    bool unlink_before_create{true};
    bool unlink_on_destroy{false};
};

struct ChannelStats
{
    std::uint64_t pushed{0};
    std::uint64_t popped{0};
    std::uint64_t dropped{0};
    std::uint64_t overwritten{0};
    std::uint64_t blocked_events{0};
    std::uint64_t wait_loops{0};
};

namespace detail
{
    inline constexpr std::size_t kCacheLineBytes{64U};
    inline constexpr mode_t kSharedMemoryPermissions{0600};

    [[nodiscard]] inline std::size_t align_up(std::size_t value, std::size_t alignment) noexcept
    {
        return ((value + alignment - 1U) / alignment) * alignment;
    }

    [[nodiscard]] inline std::string normalise_channel_name(std::string_view name)
    {
        if (name.empty())
        {
            throw std::invalid_argument("IPC channel name must not be empty");
        }
        std::string out{name};
        if (out.front() != '/')
        {
            out.insert(out.begin(), '/');
        }
        std::ranges::replace(out, '.', '_');
        return out;
    }

    inline void throw_errno(std::string_view action)
    {
        const auto error_number{errno};
        throw std::runtime_error(std::string(action) + " failed: " +
                                 std::error_code(error_number, std::generic_category()).message());
    }

    class AdaptiveWait
    {
      public:
        explicit AdaptiveWait(WaitStrategy strategy) : strategy_{strategy} {}

        void wait()
        {
            ++loops_;
            if (strategy_ == WaitStrategy::ParkOnly)
            {
                std::this_thread::sleep_for(std::chrono::microseconds{1});
                return;
            }
            if (strategy_ == WaitStrategy::YieldPark)
            {
                if (loops_ < kYieldLoops)
                {
                    std::this_thread::yield();
                    return;
                }
                std::this_thread::sleep_for(std::chrono::microseconds{1});
                return;
            }
            if (loops_ < kSpinLoops)
            {
#if defined(__x86_64__) || defined(__i386__)
                __builtin_ia32_pause();
#else
                std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
                return;
            }
            if (loops_ < kYieldLoops)
            {
                std::this_thread::yield();
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds{1});
        }

        [[nodiscard]] std::uint64_t loops() const noexcept
        {
            return loops_;
        }

        /// Reset the loop counter so the next idle period starts back at the
        /// fastest (spin) phase.  Call after a successful operation to ensure
        /// the wait strategy re-enters the hot path promptly after a burst.
        void reset() noexcept
        {
            loops_ = 0;
        }

      private:
        static constexpr std::uint64_t kSpinLoops{64U};
        static constexpr std::uint64_t kYieldLoops{256U};

        WaitStrategy strategy_;
        std::uint64_t loops_{0};
    };

    struct alignas(kCacheLineBytes) Cursor
    {
        std::atomic<std::uint64_t> value{0};
    };

    struct SharedChannelHeader
    {
        std::uint64_t magic{kSharedChannelMagic};
        std::uint32_t abi_version{kSharedChannelAbiVersion};
        std::uint32_t header_size{sizeof(SharedChannelHeader)};
        std::uint64_t slot_count{0};
        std::uint64_t slot_size{0};
        Cursor write_sequence{};
        Cursor read_sequence{};
        Cursor dropped{};
        Cursor overwritten{};
        Cursor blocked_events{};
        Cursor wait_loops{};
    };

    static_assert(alignof(SharedChannelHeader) >= kCacheLineBytes);
} // namespace detail

template <typename Frame> class SharedMemorySpscChannel
{
  public:
    static_assert(std::is_trivially_copyable_v<Frame>, "IPC frames must be trivially copyable");
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "coderoast_ipc requires lock-free uint64_t atomics");

    SharedMemorySpscChannel() = default;
    SharedMemorySpscChannel(const SharedMemorySpscChannel&) = delete;
    SharedMemorySpscChannel& operator=(const SharedMemorySpscChannel&) = delete;

    SharedMemorySpscChannel(SharedMemorySpscChannel&& other) noexcept
    {
        move_from(std::move(other));
    }

    SharedMemorySpscChannel& operator=(SharedMemorySpscChannel&& other) noexcept
    {
        if (this != &other)
        {
            close();
            move_from(std::move(other));
        }
        return *this;
    }

    ~SharedMemorySpscChannel() noexcept
    {
        close();
    }

    [[nodiscard]] static SharedMemorySpscChannel create(const ChannelConfig& config)
    {
        if (config.slot_count == 0U)
        {
            throw std::invalid_argument("IPC slot_count must be greater than zero");
        }

        SharedMemorySpscChannel channel;
        channel.name_ = detail::normalise_channel_name(config.name);
        channel.policy_ = config.backpressure;
        channel.wait_strategy_ = config.wait_strategy;
        channel.unlink_on_destroy_ = config.unlink_on_destroy;
        channel.map_size_ = map_size_for(config.slot_count);

        if (config.unlink_before_create)
        {
            (void)::shm_unlink(channel.name_.c_str());
        }

        channel.fd_ = ::shm_open(channel.name_.c_str(), O_CREAT | O_EXCL | O_RDWR,
                                 detail::kSharedMemoryPermissions);
        if (channel.fd_ < 0)
        {
            detail::throw_errno("shm_open(create)");
        }
        if (::ftruncate(channel.fd_, static_cast<off_t>(channel.map_size_)) != 0)
        {
            detail::throw_errno("ftruncate");
        }
        channel.map_memory();
        std::memset(channel.mapping_, 0, channel.map_size_);
        auto* header{new (channel.mapping_) detail::SharedChannelHeader{}};
        header->slot_count = config.slot_count;
        header->slot_size = sizeof(Frame);
        channel.header_ = header;
        channel.validate_header();
        return channel;
    }

    [[nodiscard]] static SharedMemorySpscChannel
    open(std::string_view name, BackpressurePolicy backpressure = BackpressurePolicy::Block,
         WaitStrategy wait_strategy = WaitStrategy::SpinYieldPark)
    {
        SharedMemorySpscChannel channel;
        channel.name_ = detail::normalise_channel_name(name);
        channel.policy_ = backpressure;
        channel.wait_strategy_ = wait_strategy;
        channel.fd_ = ::shm_open(channel.name_.c_str(), O_RDWR, detail::kSharedMemoryPermissions);
        if (channel.fd_ < 0)
        {
            detail::throw_errno("shm_open(open)");
        }
        struct stat stats{};
        if (::fstat(channel.fd_, &stats) != 0)
        {
            detail::throw_errno("fstat");
        }
        channel.map_size_ = static_cast<std::size_t>(stats.st_size);
        channel.map_memory();
        channel.header_ = static_cast<detail::SharedChannelHeader*>(channel.mapping_);
        channel.validate_header();
        return channel;
    }

    [[nodiscard]] const std::string& name() const noexcept
    {
        return name_;
    }

    [[nodiscard]] std::size_t slot_count() const noexcept
    {
        return header_ == nullptr ? 0U : static_cast<std::size_t>(header_->slot_count);
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return size_approx() == 0U;
    }

    [[nodiscard]] bool full() const noexcept
    {
        return size_approx() >= slot_count();
    }

    [[nodiscard]] std::size_t size_approx() const noexcept
    {
        if (header_ == nullptr)
        {
            return 0U;
        }
        const auto write{header_->write_sequence.value.load(std::memory_order_acquire)};
        const auto read{header_->read_sequence.value.load(std::memory_order_acquire)};
        return static_cast<std::size_t>(write - read);
    }

    [[nodiscard]] bool try_push(const Frame& frame)
    {
        return try_push_impl(frame, true);
    }

    [[nodiscard]] bool push(const Frame& frame)
    {
        if (policy_ == BackpressurePolicy::DropNewest)
        {
            return try_push(frame);
        }
        if (policy_ == BackpressurePolicy::OverwriteOldest)
        {
            overwrite_push(frame);
            return true;
        }

        detail::AdaptiveWait wait{wait_strategy_};
        bool blocked{false};
        while (!try_push_impl(frame, false))
        {
            blocked = true;
            wait.wait();
        }
        if (blocked)
        {
            header_->blocked_events.value.fetch_add(1, std::memory_order_relaxed);
            header_->wait_loops.value.fetch_add(wait.loops(), std::memory_order_relaxed);
        }
        return true;
    }

    [[nodiscard]] bool try_pop(Frame& out)
    {
        ensure_open();
        const auto read{header_->read_sequence.value.load(std::memory_order_relaxed)};
        const auto write{header_->write_sequence.value.load(std::memory_order_acquire)};
        if (read == write)
        {
            return false;
        }
        std::memcpy(&out, slot_ptr(read), sizeof(Frame));
        header_->read_sequence.value.store(read + 1U, std::memory_order_release);
        return true;
    }

    [[nodiscard]] ChannelStats stats() const noexcept
    {
        if (header_ == nullptr)
        {
            return {};
        }
        const auto pushed{header_->write_sequence.value.load(std::memory_order_acquire)};
        const auto popped{header_->read_sequence.value.load(std::memory_order_acquire)};
        return ChannelStats{
            .pushed = pushed,
            .popped = popped,
            .dropped = header_->dropped.value.load(std::memory_order_relaxed),
            .overwritten = header_->overwritten.value.load(std::memory_order_relaxed),
            .blocked_events = header_->blocked_events.value.load(std::memory_order_relaxed),
            .wait_loops = header_->wait_loops.value.load(std::memory_order_relaxed),
        };
    }

    void close() noexcept
    {
        if (mapping_ != nullptr && map_size_ > 0U)
        {
            (void)::munmap(mapping_, map_size_);
        }
        if (fd_ >= 0)
        {
            (void)::close(fd_);
        }
        if (unlink_on_destroy_ && !name_.empty())
        {
            (void)::shm_unlink(name_.c_str());
        }
        mapping_ = nullptr;
        header_ = nullptr;
        fd_ = -1;
        map_size_ = 0U;
        unlink_on_destroy_ = false;
    }

    static void unlink(std::string_view name)
    {
        const auto normalised{detail::normalise_channel_name(name)};
        (void)::shm_unlink(normalised.c_str());
    }

  private:
    [[nodiscard]] static std::size_t data_offset() noexcept
    {
        return detail::align_up(sizeof(detail::SharedChannelHeader), alignof(Frame));
    }

    [[nodiscard]] static std::size_t map_size_for(std::size_t slot_count) noexcept
    {
        return data_offset() + (slot_count * sizeof(Frame));
    }

    void ensure_open() const
    {
        if (header_ == nullptr)
        {
            throw std::logic_error("IPC channel is not open");
        }
    }

    void map_memory()
    {
        mapping_ = ::mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping_ == MAP_FAILED)
        {
            mapping_ = nullptr;
            detail::throw_errno("mmap");
        }
    }

    void validate_header() const
    {
        ensure_open();
        if (header_->magic != kSharedChannelMagic ||
            header_->abi_version != kSharedChannelAbiVersion ||
            header_->slot_size != sizeof(Frame) || header_->slot_count == 0U)
        {
            throw std::runtime_error("IPC shared-memory channel header is incompatible");
        }
    }

    [[nodiscard]] void* slot_ptr(std::uint64_t sequence) noexcept
    {
        const auto index{sequence % header_->slot_count};
        auto* base{static_cast<std::byte*>(mapping_)};
        return base + data_offset() + (index * sizeof(Frame)); // NOLINT
    }

    [[nodiscard]] const void* slot_ptr(std::uint64_t sequence) const noexcept
    {
        const auto index{sequence % header_->slot_count};
        const auto* base{static_cast<const std::byte*>(mapping_)};
        return base + data_offset() + (index * sizeof(Frame)); // NOLINT
    }

    [[nodiscard]] bool try_push_impl(const Frame& frame, bool count_drop)
    {
        ensure_open();
        const auto write{header_->write_sequence.value.load(std::memory_order_relaxed)};
        const auto read{header_->read_sequence.value.load(std::memory_order_acquire)};
        if (write - read >= header_->slot_count)
        {
            if (count_drop)
            {
                header_->dropped.value.fetch_add(1, std::memory_order_relaxed);
            }
            return false;
        }
        std::memcpy(slot_ptr(write), &frame, sizeof(Frame));
        header_->write_sequence.value.store(write + 1U, std::memory_order_release);
        return true;
    }

    void overwrite_push(const Frame& frame)
    {
        ensure_open();
        const auto write{header_->write_sequence.value.load(std::memory_order_relaxed)};
        const auto read{header_->read_sequence.value.load(std::memory_order_acquire)};
        if (write - read >= header_->slot_count)
        {
            header_->read_sequence.value.store(read + 1U, std::memory_order_release);
            header_->overwritten.value.fetch_add(1, std::memory_order_relaxed);
        }
        std::memcpy(slot_ptr(write), &frame, sizeof(Frame));
        header_->write_sequence.value.store(write + 1U, std::memory_order_release);
    }

    void move_from(SharedMemorySpscChannel&& other) noexcept // NOLINT
    {
        name_ = std::move(other.name_);
        fd_ = std::exchange(other.fd_, -1);
        mapping_ = std::exchange(other.mapping_, nullptr);
        header_ = std::exchange(other.header_, nullptr);
        map_size_ = std::exchange(other.map_size_, 0U);
        policy_ = other.policy_;
        wait_strategy_ = other.wait_strategy_;
        unlink_on_destroy_ = std::exchange(other.unlink_on_destroy_, false);
    }

    std::string name_;
    int fd_{-1};
    void* mapping_{nullptr};
    detail::SharedChannelHeader* header_{nullptr};
    std::size_t map_size_{0};
    BackpressurePolicy policy_{BackpressurePolicy::Block};
    WaitStrategy wait_strategy_{WaitStrategy::SpinYieldPark};
    bool unlink_on_destroy_{false};
};

} // namespace coderoast::ipc