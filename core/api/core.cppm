export module coderoast.ipc.core;
import std;

export namespace coderoast::ipc
{

inline constexpr std::uint32_t kIpcAbiVersion{3U};
inline constexpr std::size_t kDefaultLineFramePayloadBytes{4096U};

// invariant: both ends derive a shard's segment name from (base, shard_id) here; a second
// definition would silently open two different segments.
[[nodiscard]] inline std::string shard_channel_name(std::string_view base, std::size_t shard_id)
{
    std::string name{base};
    name += "_shard_";
    name += std::to_string(shard_id);
    return name;
}

// note: uint16_t pairs with LineFrameHeader::reserved to keep the header padding-free.
// NOLINTNEXTLINE(performance-enum-size)
enum class LineFrameFlags : std::uint16_t
{
    kLineFrameFlagNone = 0,
    kLineFrameFlagTruncated = 1U << 0U,
    kLineFrameFlagEndOfStream = 1U << 1U,
    kLineFrameFlagWindowSeal = 1U << 2U,
};

[[nodiscard]] constexpr LineFrameFlags operator|(LineFrameFlags lhs, LineFrameFlags rhs) noexcept
{
    using Raw = std::underlying_type_t<LineFrameFlags>;
    return static_cast<LineFrameFlags>(static_cast<Raw>(lhs) | static_cast<Raw>(rhs));
}

[[nodiscard]] constexpr bool has_flag(LineFrameFlags flags, LineFrameFlags flag) noexcept
{
    using Raw = std::underlying_type_t<LineFrameFlags>;
    return (static_cast<Raw>(flags) & static_cast<Raw>(flag)) != 0U;
}

[[nodiscard]] constexpr bool is_control_frame(LineFrameFlags flags) noexcept
{
    return has_flag(flags, LineFrameFlags::kLineFrameFlagWindowSeal) ||
           has_flag(flags, LineFrameFlags::kLineFrameFlagEndOfStream);
}

// refs: ADR-22.D1, ADR-22.D4
// invariant: the wire carries only facts the bytes cannot: the intent FORMAT is intrinsic and stays
// out, the intent CHANNEL is extrinsic and is forwarded.
struct LineFrameHeader
{
    std::uint64_t sequence{0};
    std::uint64_t shard_sequence{0};
    std::uint64_t timestamp_unix_ns{0};
    std::uint64_t logical_tick{0};
    std::uint64_t run_id{0};
    std::uint64_t window_id{0};
    std::uint32_t payload_size{0};
    std::uint32_t agent_id{0};
    std::uint32_t agent_order{0};
    std::uint32_t intra_agent_index{0};
    std::uint32_t shard_id{0};
    LineFrameFlags flags{LineFrameFlags::kLineFrameFlagNone};
    // invariant: explicit tail padding, sized so LineFrameHeader tiles exactly and the compiler
    // inserts no implicit hole.
    std::uint16_t reserved{0};
};

template <std::size_t MaxPayload> struct LineFrame
{
    static constexpr std::size_t max_payload_bytes{MaxPayload};

    LineFrameHeader header{};
    std::array<std::byte, MaxPayload> payload{};
};

using DefaultLineFrame = LineFrame<kDefaultLineFramePayloadBytes>;

static_assert(std::is_trivially_copyable_v<LineFrameHeader>);
static_assert(std::is_trivially_copyable_v<DefaultLineFrame>);
static_assert(std::has_unique_object_representations_v<LineFrameHeader>,
              "LineFrameHeader has implicit padding — size the trailing `reserved` field so the "
              "members tile the struct exactly (an IPC header is memcpy'd; padding bytes are "
              "indeterminate on the wire)");

template <typename F>
concept FrameLike = std::is_trivially_copyable_v<F> && requires(const F& frame) {
    { frame.header.sequence } -> std::convertible_to<std::uint64_t>;
    { frame.header.logical_tick } -> std::convertible_to<std::uint64_t>;
    { frame.header.timestamp_unix_ns } -> std::convertible_to<std::uint64_t>;
    { frame.header.agent_order } -> std::convertible_to<std::uint32_t>;
    { frame.header.intra_agent_index } -> std::convertible_to<std::uint32_t>;
    { frame.header.shard_id } -> std::convertible_to<std::uint32_t>;
    { frame.header.flags };
    { frame.header.payload_size } -> std::convertible_to<std::uint32_t>;
};

inline constexpr std::uint64_t kSharedChannelMagic{0x4352495043535053ULL};
inline constexpr std::uint32_t kSharedChannelAbiVersion{5U};
inline constexpr std::size_t kDefaultSharedChannelSlotCount{8192U};

// invariant: capacity including the NUL; a longer name is refused at create(), never truncated.
inline constexpr std::size_t kIntentChannelNameCapacity{32U};

enum class BackpressurePolicy : std::uint8_t
{
    Block,
    DropNewest,
};

enum class WaitStrategy : std::uint8_t
{
    Spin,
    SpinYield,
    Adaptive,
    AdaptivePark,
    ParkOnly,
};

// invariant: Closed means the consumer drained every frame written before Closing; the value lives
// in shared memory and both ends read it with acquire.
enum class ChannelState : std::uint8_t
{
    Open = 0,
    Closing = 1,
    Closed = 2,
    Aborted = 3,
};

// invariant: Full is also what DropNewest reports for a frame it dropped.
enum class PushStatus : std::uint8_t
{
    Ok = 0,
    Full = 1,
    Closed = 2,
    Aborted = 3,
};

// invariant: Closed means the ring is empty AND the producer closed gracefully, so no further frame
// will ever arrive.
enum class PopStatus : std::uint8_t
{
    Ok = 0,
    Empty = 1,
    Closed = 2,
    Aborted = 3,
};

struct ChannelConfig
{
    std::string name;
    std::size_t slot_count{kDefaultSharedChannelSlotCount};
    BackpressurePolicy backpressure{BackpressurePolicy::Block};
    WaitStrategy wait_strategy{WaitStrategy::Adaptive};
    bool unlink_before_create{true};
    bool unlink_on_destroy{false};
    // refs: ADR-22.D4
    // invariant: the IntentChannel this ring transports, spelled in full because `name` above is
    // the ring's own name. Empty means the producer declared nothing.
    std::string intent_channel;
};

struct ChannelStats
{
    std::uint64_t pushed{0};
    std::uint64_t popped{0};
    std::uint64_t dropped{0};
    std::uint64_t blocked_events{0};
    std::uint64_t wait_loops{0};
    ChannelState state{ChannelState::Open};
};

} // namespace coderoast::ipc

// refs: ADR-3.D4
// invariant: named and non-export, not anonymous: the inline channel templates below use these
// helpers, so TU-local linkage would make an instantiation ill-formed.
namespace coderoast::ipc
{

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

inline void cpu_pause() noexcept
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elifdef __aarch64__
    asm volatile("yield" ::: "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

inline constexpr std::size_t kCacheLineBytes{64U};

// refs: ADR-3.D4, F-SRC-coderoast-ipc:core_impl.cpp
// invariant: defined in the textual impl unit, which owns the POSIX macros this import-std
// interface cannot see; the boundary crosses primitives only.
// post: a valid descriptor or a throw — never a negative fd, here and at shm_open_existing.
[[nodiscard]] int shm_open_create(const char* name);
[[nodiscard]] int shm_open_existing(const char* name);
void shm_truncate(int descriptor, std::size_t size);
[[nodiscard]] std::size_t shm_fstat_size(int descriptor);
[[nodiscard]] void* shm_map(int descriptor, std::size_t size);
void shm_unmap(void* address, std::size_t size) noexcept;
void close_descriptor(int descriptor) noexcept;
void shm_unlink_name(const char* name) noexcept;

struct alignas(kCacheLineBytes) Cursor
{
    std::atomic<std::uint64_t> value{0};
};

// invariant: lives at the start of the SHM mapping; the hot cursors sit on their own cache lines
// and the state machine on another, so close/abort never false-shares.
struct SharedChannelHeader
{
    std::uint64_t magic{kSharedChannelMagic};
    std::uint32_t abi_version{kSharedChannelAbiVersion};
    std::uint32_t header_size{sizeof(SharedChannelHeader)};

    std::uint64_t slot_count{0};
    std::uint64_t slot_size{0};

    // refs: ADR-22.D4
    // invariant: channel-level and never per-frame; written once at create(), read once at open();
    // all-zero means Unspecified.
    std::array<char, kIntentChannelNameCapacity> intent_channel{};

    alignas(kCacheLineBytes) std::atomic<std::uint32_t> wake_epoch{0};
    alignas(kCacheLineBytes) std::atomic<std::uint32_t> parker_count{0};

    Cursor write_sequence{};
    Cursor read_sequence{};
    Cursor closing_at{};
    Cursor dropped{};
    Cursor blocked_events{};
    Cursor wait_loops{};

    alignas(kCacheLineBytes) std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(ChannelState::Open)};
};

static_assert(alignof(SharedChannelHeader) >= kCacheLineBytes);

class AdaptiveWait
{
  public:
    explicit AdaptiveWait(WaitStrategy strategy) noexcept : strategy_{strategy} {}

    // pre: `header` may be null; AdaptivePark then sleeps instead of parking on wake_epoch.
    void wait(SharedChannelHeader* header) noexcept
    {
        ++loops_;
        switch (strategy_)
        {
        case WaitStrategy::Spin:
            cpu_pause();
            return;
        case WaitStrategy::SpinYield:
            if (loops_ < kSpinLoops)
            {
                cpu_pause();
            }
            else
            {
                std::this_thread::yield();
            }
            return;
        case WaitStrategy::Adaptive:
            if (loops_ < kSpinLoops)
            {
                cpu_pause();
                return;
            }
            if (loops_ < kYieldLoops)
            {
                std::this_thread::yield();
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds{1});
            return;
        case WaitStrategy::AdaptivePark:
            if (loops_ < kSpinLoops)
            {
                cpu_pause();
                return;
            }
            if (loops_ < kYieldLoops)
            {
                std::this_thread::yield();
                return;
            }
            if (header != nullptr)
            {
                // invariant: parker_count is raised across the park so a notifier knows a wake is
                // owed; a spurious wake is safe because the caller's loop re-checks.
                header->parker_count.fetch_add(1, std::memory_order_acq_rel);
                const auto epoch{header->wake_epoch.load(std::memory_order_acquire)};
                header->wake_epoch.wait(epoch, std::memory_order_acquire);
                header->parker_count.fetch_sub(1, std::memory_order_acq_rel);
                return;
            }
            std::this_thread::sleep_for(std::chrono::microseconds{1});
            return;
        case WaitStrategy::ParkOnly:
            std::this_thread::sleep_for(std::chrono::microseconds{1});
            return;
        }
    }

    [[nodiscard]] std::uint64_t loops() const noexcept
    {
        return loops_;
    }

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

} // namespace coderoast::ipc

export namespace coderoast::ipc
{

template <FrameLike Frame> class SharedMemorySpscChannel
{
  public:
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
                  "coderoast_ipc requires lock-free uint64_t atomics");
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free,
                  "coderoast_ipc requires lock-free uint32_t atomics for AdaptivePark");
    static_assert(std::atomic<std::uint8_t>::is_always_lock_free,
                  "coderoast_ipc requires lock-free uint8_t atomics for the state machine");

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
        // post: a producer handle destructing without an explicit close leaves the channel Closing,
        // so a consumer sees end-of-stream instead of a live Open.
        if (is_producer_ && header_ != nullptr && state() == ChannelState::Open)
        {
            close_graceful();
        }
        close();
    }

    [[nodiscard]] static SharedMemorySpscChannel create(const ChannelConfig& config)
    {
        if (config.slot_count == 0U)
        {
            throw std::invalid_argument("IPC slot_count must be greater than zero");
        }

        // refs: ADR-22.D4
        // assert: refusing beats truncating — a clipped name aliases another declared channel or
        // fails the consumer's vocabulary check far from here.
        if (config.intent_channel.size() >= kIntentChannelNameCapacity)
        {
            throw std::invalid_argument("IPC intent_channel name exceeds " +
                                        std::to_string(kIntentChannelNameCapacity - 1U) +
                                        " bytes: '" + config.intent_channel + "'");
        }

        SharedMemorySpscChannel channel;
        channel.name_ = normalise_channel_name(config.name);
        channel.policy_ = config.backpressure;
        channel.wait_strategy_ = config.wait_strategy;
        channel.unlink_on_destroy_ = config.unlink_on_destroy;
        channel.map_size_ = map_size_for(config.slot_count);

        if (config.unlink_before_create)
        {
            shm_unlink_name(channel.name_.c_str());
        }

        channel.fd_ = shm_open_create(channel.name_.c_str());
        shm_truncate(channel.fd_, channel.map_size_);
        channel.map_memory();
        std::memset(channel.mapping_, 0, channel.map_size_);
        auto* header{new (channel.mapping_) SharedChannelHeader{}};
        header->slot_count = config.slot_count;
        header->slot_size = sizeof(Frame);
        // invariant: the array is value-initialised, so an empty declaration stays all-zero =
        // Unspecified and the copy keeps the NUL (size < capacity, checked above).
        std::memcpy(header->intent_channel.data(), config.intent_channel.data(),
                    config.intent_channel.size());
        channel.header_ = header;
        channel.validate_header();
        channel.is_producer_ = true;
        return channel;
    }

    [[nodiscard]] static SharedMemorySpscChannel
    open(std::string_view name, BackpressurePolicy backpressure = BackpressurePolicy::Block,
         WaitStrategy wait_strategy = WaitStrategy::Adaptive)
    {
        SharedMemorySpscChannel channel;
        channel.name_ = normalise_channel_name(name);
        channel.policy_ = backpressure;
        channel.wait_strategy_ = wait_strategy;
        channel.fd_ = shm_open_existing(channel.name_.c_str());
        channel.map_size_ = shm_fstat_size(channel.fd_);
        channel.map_memory();
        channel.header_ = static_cast<SharedChannelHeader*>(channel.mapping_);
        channel.validate_header();
        return channel;
    }

    // refs: ADR-22.D5
    // post: empty means the producer declared nothing, and a consumer must then fail closed on
    // dialect depth rather than assume a channel.
    [[nodiscard]] std::string_view intent_channel() const noexcept
    {
        if (header_ == nullptr)
        {
            return {};
        }
        // assert: NUL-terminated by construction — create() refuses a name that fills it.
        return std::string_view{header_->intent_channel.data()};
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

    [[nodiscard]] ChannelState state() const noexcept
    {
        if (header_ == nullptr)
        {
            return ChannelState::Aborted;
        }
        return static_cast<ChannelState>(header_->state.load(std::memory_order_acquire));
    }

    [[nodiscard]] bool is_open() const noexcept
    {
        return state() == ChannelState::Open;
    }
    [[nodiscard]] bool is_closing() const noexcept
    {
        return state() == ChannelState::Closing;
    }
    [[nodiscard]] bool is_closed() const noexcept
    {
        const auto _state{state()};
        return _state == ChannelState::Closing || _state == ChannelState::Closed;
    }
    [[nodiscard]] bool is_aborted() const noexcept
    {
        return state() == ChannelState::Aborted;
    }

    // post: the write_sequence snapshot taken at close_graceful, meaningful only once state() is
    // not Open; it is how a consumer detects delivery with no EOS frame.
    [[nodiscard]] std::uint64_t closing_at() const noexcept
    {
        return header_ == nullptr ? 0U : header_->closing_at.value.load(std::memory_order_acquire);
    }

    // post: idempotent, non-blocking, callable from any thread; later pushes return Closed, and it
    // cannot downgrade an Aborted channel.
    void close_graceful() noexcept
    {
        if (header_ == nullptr)
        {
            return;
        }
        std::uint8_t expected{static_cast<std::uint8_t>(ChannelState::Open)};
        if (header_->state.compare_exchange_strong(
                expected, static_cast<std::uint8_t>(ChannelState::Closing),
                std::memory_order_acq_rel, std::memory_order_acquire))
        {
            header_->closing_at.value.store(
                header_->write_sequence.value.load(std::memory_order_acquire),
                std::memory_order_release);
        }
        // assert: a producer parked in push() under Block must see the state change and exit.
        notify_state_change();
    }

    // post: terminal; every parked thread wakes and frames already in the ring may still be popped.
    // Callable from any thread.
    void close_abort() noexcept
    {
        if (header_ == nullptr)
        {
            return;
        }
        header_->state.store(static_cast<std::uint8_t>(ChannelState::Aborted),
                             std::memory_order_release);
        notify_state_change();
    }

    [[nodiscard]] PushStatus try_push_status(const Frame& frame) noexcept
    {
        const auto _state{state()};
        if (_state == ChannelState::Aborted)
        {
            return PushStatus::Aborted;
        }
        if (_state != ChannelState::Open)
        {
            return PushStatus::Closed;
        }
        if (!try_push_impl(frame, /*count_drop=*/true))
        {
            return PushStatus::Full;
        }
        notify_progress();
        return PushStatus::Ok;
    }

    // post: under Block, waits on the configured WaitStrategy and is cancellable by
    // close_graceful() or close_abort().
    [[nodiscard]] PushStatus push_status(const Frame& frame) noexcept
    {
        if (policy_ == BackpressurePolicy::DropNewest)
        {
            return try_push_status(frame);
        }
        AdaptiveWait wait{wait_strategy_};
        bool blocked{false};
        for (;;)
        {
            const auto _state{state()};
            if (_state == ChannelState::Aborted)
            {
                return PushStatus::Aborted;
            }
            if (_state != ChannelState::Open)
            {
                return PushStatus::Closed;
            }
            if (try_push_impl(frame, /*count_drop=*/false))
            {
                if (blocked)
                {
                    header_->blocked_events.value.fetch_add(1, std::memory_order_relaxed);
                    header_->wait_loops.value.fetch_add(wait.loops(), std::memory_order_relaxed);
                }
                notify_progress();
                return PushStatus::Ok;
            }
            blocked = true;
            wait.wait(header_);
        }
    }

    [[nodiscard]] PopStatus try_pop_status(Frame& out) noexcept
    {
        if (header_ == nullptr)
        {
            return PopStatus::Aborted;
        }
        const auto read{header_->read_sequence.value.load(std::memory_order_relaxed)};
        const auto write{header_->write_sequence.value.load(std::memory_order_acquire)};
        if (read != write)
        {
            std::memcpy(&out, slot_ptr(read), sizeof(Frame));
            header_->read_sequence.value.store(read + 1U, std::memory_order_release);
            notify_progress();
            return PopStatus::Ok;
        }
        const auto _state{state()};
        if (_state == ChannelState::Aborted)
        {
            return PopStatus::Aborted;
        }
        if (_state == ChannelState::Open)
        {
            return PopStatus::Empty;
        }
        // assert: reaching closing_at on an empty ring is terminal, so Closing is promoted.
        const auto closing_seq{header_->closing_at.value.load(std::memory_order_acquire)};
        if (read >= closing_seq)
        {
            std::uint8_t exp{static_cast<std::uint8_t>(ChannelState::Closing)};
            (void)header_->state.compare_exchange_strong(
                exp, static_cast<std::uint8_t>(ChannelState::Closed), std::memory_order_acq_rel,
                std::memory_order_acquire);
            return PopStatus::Closed;
        }
        // assert: the ring read empty before closing_at — the producer raced us; the next call
        // sees the frame or the closing condition.
        return PopStatus::Empty;
    }

    // post: false conflates Full, Closed and Aborted; try_push_status() tells them apart.
    [[nodiscard]] bool try_push(const Frame& frame) noexcept
    {
        return try_push_status(frame) == PushStatus::Ok;
    }
    [[nodiscard]] bool push(const Frame& frame) noexcept
    {
        return push_status(frame) == PushStatus::Ok;
    }
    [[nodiscard]] bool try_pop(Frame& out) noexcept
    {
        return try_pop_status(out) == PopStatus::Ok;
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
            .blocked_events = header_->blocked_events.value.load(std::memory_order_relaxed),
            .wait_loops = header_->wait_loops.value.load(std::memory_order_relaxed),
            .state = state(),
        };
    }

    void close() noexcept
    {
        if (mapping_ != nullptr && map_size_ > 0U)
        {
            shm_unmap(mapping_, map_size_);
        }
        if (fd_ >= 0)
        {
            close_descriptor(fd_);
        }
        if (unlink_on_destroy_ && !name_.empty())
        {
            shm_unlink_name(name_.c_str());
        }
        mapping_ = nullptr;
        header_ = nullptr;
        fd_ = -1;
        map_size_ = 0U;
        unlink_on_destroy_ = false;
        is_producer_ = false;
    }

    static void unlink(std::string_view name)
    {
        const auto normalised{normalise_channel_name(name)};
        shm_unlink_name(normalised.c_str());
    }

  private:
    [[nodiscard]] static std::size_t data_offset() noexcept
    {
        return align_up(sizeof(SharedChannelHeader), alignof(Frame));
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
        mapping_ = shm_map(fd_, map_size_);
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
        return base + data_offset() + (index * sizeof(Frame));
    }

    [[nodiscard]] const void* slot_ptr(std::uint64_t sequence) const noexcept
    {
        const auto index{sequence % header_->slot_count};
        const auto* base{static_cast<const std::byte*>(mapping_)};
        return base + data_offset() + (index * sizeof(Frame));
    }

    [[nodiscard]] bool try_push_impl(const Frame& frame, bool count_drop) noexcept
    {
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

    // post: bumps wake_epoch only when parker_count is non-zero — one acquire load and a branch
    // when nobody is parked.
    void notify_progress() noexcept
    {
        if (header_->parker_count.load(std::memory_order_acquire) == 0U)
        {
            return;
        }
        header_->wake_epoch.fetch_add(1, std::memory_order_acq_rel);
        header_->wake_epoch.notify_all();
    }

    // post: bumps wake_epoch unconditionally, so a parked thread sees the state change on its next
    // iteration.
    void notify_state_change() noexcept
    {
        header_->wake_epoch.fetch_add(1, std::memory_order_acq_rel);
        header_->wake_epoch.notify_all();
    }

    // note: `other` is emptied member by member with std::exchange, never moved whole.
    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
    void move_from(SharedMemorySpscChannel&& other) noexcept
    {
        name_ = std::move(other.name_);
        fd_ = std::exchange(other.fd_, -1);
        mapping_ = std::exchange(other.mapping_, nullptr);
        header_ = std::exchange(other.header_, nullptr);
        map_size_ = std::exchange(other.map_size_, 0U);
        policy_ = other.policy_;
        wait_strategy_ = other.wait_strategy_;
        unlink_on_destroy_ = std::exchange(other.unlink_on_destroy_, false);
        is_producer_ = std::exchange(other.is_producer_, false);
    }

    std::string name_;
    int fd_{-1};
    void* mapping_{nullptr};
    SharedChannelHeader* header_{nullptr};
    std::size_t map_size_{0};
    BackpressurePolicy policy_{BackpressurePolicy::Block};
    WaitStrategy wait_strategy_{WaitStrategy::Adaptive};
    bool unlink_on_destroy_{false};
    bool is_producer_{false};
};

} // namespace coderoast::ipc
