export module coderoast.ipc.core;
import std;

export namespace coderoast::ipc
{

inline constexpr std::size_t kDefaultLineFramePayloadBytes{4096U};

// invariant: both ends derive a shard's segment name from (base, shard_id) here; a second
// definition would silently open two different segments.
// post: `<base>_shard_<shard_id>`, the one segment name both processes agree on.
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
    // note: bit 1 was an EndOfStream flag nobody set; end of stream is the channel state.
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
    return has_flag(flags, LineFrameFlags::kLineFrameFlagWindowSeal);
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
inline constexpr std::uint32_t kSharedChannelAbiVersion{7U};
inline constexpr std::size_t kDefaultSharedChannelSlotCount{8192U};

// invariant: capacity including the NUL; a longer name is refused at create(), never truncated.
inline constexpr std::size_t kIntentChannelNameCapacity{32U};

// refs: DN-103.D29
// invariant: the producer's seal grid: windows of window_length_ns from origin_unix_ns, and the
// frontier points it publishes its seal horizon at, every frontier_step_ns from the same origin.
// invariant: both lengths are non-zero on every channel create() admits.
struct SealGrid
{
    std::uint64_t origin_unix_ns{0};
    std::uint64_t window_length_ns{0};
    std::uint64_t frontier_step_ns{0};

    [[nodiscard]] bool operator==(const SealGrid&) const = default;
};

static_assert(std::has_unique_object_representations_v<SealGrid>);

// refs: DN-103.D29
// pre: grid.window_length_ns and grid.frontier_step_ns are non-zero.
// post: E(window), the first frontier point at or after the end of `window`: origin + ceil((window
// + 1) * I / e) * e, in integers; the largest uint64 when that sum does not fit one.
[[nodiscard]] constexpr std::uint64_t frontier_point_of_window(const SealGrid& grid,
                                                               std::uint64_t window) noexcept
{
    constexpr std::uint64_t kUnbounded{std::numeric_limits<std::uint64_t>::max()};
    if (window >= kUnbounded / grid.window_length_ns)
    {
        return kUnbounded;
    }
    const std::uint64_t window_end_offset{(window + 1U) * grid.window_length_ns};
    const std::uint64_t steps{(window_end_offset / grid.frontier_step_ns) +
                              (window_end_offset % grid.frontier_step_ns != 0U ? 1U : 0U)};
    if (steps > kUnbounded / grid.frontier_step_ns)
    {
        return kUnbounded;
    }
    const std::uint64_t frontier_offset{steps * grid.frontier_step_ns};
    if (frontier_offset > kUnbounded - grid.origin_unix_ns)
    {
        return kUnbounded;
    }
    return grid.origin_unix_ns + frontier_offset;
}

// refs: DN-103.D29
// pre: grid.window_length_ns and grid.frontier_step_ns are non-zero.
// post: L, the admission horizon of a merge whose last emitted tick is `anchor_tick`: the frontier
// point of the window holding max(origin, anchor_tick).
[[nodiscard]] constexpr std::uint64_t admission_horizon(const SealGrid& grid,
                                                        std::uint64_t anchor_tick) noexcept
{
    const std::uint64_t anchor{std::max(grid.origin_unix_ns, anchor_tick)};
    return frontier_point_of_window(grid, (anchor - grid.origin_unix_ns) / grid.window_length_ns);
}

// refs: DN-103.D29
// pre: grid.window_length_ns and grid.frontier_step_ns are non-zero.
// post: the lowest window whose frontier point is at or after `tick`, so a frame at `tick` counts
// toward W⁺ of every window from it through the frame's own.
// invariant: ceil(x) >= k exactly when x > k - 1, so E(m) >= tick exactly when (m + 1) * I >
// (k - 1) * e, where k = ceil((tick - origin) / e); a tick at or before the origin opens window 0.
[[nodiscard]] constexpr std::uint64_t first_window_open_at(const SealGrid& grid,
                                                           std::uint64_t tick) noexcept
{
    if (tick <= grid.origin_unix_ns)
    {
        return 0U;
    }
    const std::uint64_t offset{tick - grid.origin_unix_ns};
    const std::uint64_t steps_before{(offset - 1U) / grid.frontier_step_ns};
    const std::uint64_t closed_offset{steps_before * grid.frontier_step_ns};
    return closed_offset / grid.window_length_ns;
}

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
    // refs: ADR-11.D7
    // invariant: create() unlinks a stale segment of the same name first; the producer handle
    // unlinks the name when it closes, and only while the name still resolves to its own segment.
    bool unlink_before_create{true};
    // refs: ADR-22.D4
    // invariant: the IntentChannel this ring transports, spelled in full because `name` above is
    // the ring's own name. Empty means the producer declared nothing.
    std::string intent_channel;
    // refs: DN-103.D29
    // invariant: required: create() refuses a zero window length or frontier step, so a producer
    // that omits it declares nothing and is refused.
    SealGrid seal_grid;
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

// refs: DN-102.D2
// invariant: the producer a segment's header names: its pid, that pid's start time (field 22 of
// /proc/<pid>/stat) and the inode of its pid namespace; a zero field means /proc could not say.
struct SegmentOwner
{
    std::uint64_t start_time{0};
    std::uint64_t pid_namespace{0};
    std::int32_t pid{0};
    // invariant: explicit tail padding, so the owner tiles exactly inside the shared header.
    std::uint32_t reserved{0};
};

static_assert(std::has_unique_object_representations_v<SegmentOwner>);

enum class SegmentVerdict : std::uint8_t
{
    Alive,
    Orphaned,
    Unknown,
};

struct ReapedSegment
{
    std::string name;
    SegmentVerdict verdict{SegmentVerdict::Unknown};
    bool removed{false};
};

// post: this process's owner; a field /proc cannot supply stays zero.
[[nodiscard]] SegmentOwner current_segment_owner();

// refs: DN-102.D2
// post: Alive when the owner's pid runs in this pid namespace with the recorded start time.
// post: Orphaned when that pid is absent from this pid namespace or runs with another start time.
// post: Unknown for another pid namespace, a zero field, or a /proc entry that cannot be read.
[[nodiscard]] SegmentVerdict judge_segment_owner(const SegmentOwner& owner);

// refs: DN-102.D2
// post: every /dev/shm regular file whose name starts with `name_prefix`, judged from its header
// as read through a descriptor, never a mapping, so a short or foreign file cannot fault.
// post: a foreign magic, another ABI version or a short file is Unknown; only an Orphaned segment
// is unlinked, and only while its name still resolves to the segment judged.
[[nodiscard]] std::vector<ReapedSegment> reap_orphaned_segments(std::string_view name_prefix = {});

// refs: DN-103.D20
// post: the bytes the tmpfs holding every segment can hold, its size and not what is free, read by
// statvfs; nothing when /dev/shm cannot be read or its size exceeds std::size_t.
[[nodiscard]] std::optional<std::size_t> shared_memory_capacity() noexcept;

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

// refs: ADR-11.D7
// invariant: a segment's identity is its object's (st_dev, st_ino), which no later segment created
// under the same name shares while this one exists.
struct SegmentIdentity
{
    std::uint64_t device{0};
    std::uint64_t inode{0};
};

struct CreatedSegment
{
    int descriptor{-1};
    SegmentIdentity identity;
};

// refs: ADR-3.D4, F-SRC-coderoast-ipc:core_impl.cpp
// invariant: defined in the textual impl unit, which owns the POSIX macros this import-std
// interface cannot see; the boundary crosses primitives only.
// post: a valid descriptor or a throw — never a negative fd, here and at shm_open_existing.
[[nodiscard]] CreatedSegment shm_open_create(const char* name);
[[nodiscard]] int shm_open_existing(const char* name);
// refs: DN-102.D3, DN-103.D20
// post: `size` bytes are reserved for the object, or a throw naming the channel, the bytes and the
// errno, so a full tmpfs refuses here instead of faulting a later write.
// post: the throw is a std::system_error carrying the errno, so a caller tells a full tmpfs
// (ENOSPC) from any other failure.
void shm_reserve(int descriptor, std::size_t size, const char* channel);
[[nodiscard]] std::size_t shm_fstat_size(int descriptor);
[[nodiscard]] void* shm_map(int descriptor, std::size_t size);
void shm_unmap(void* address, std::size_t size) noexcept;
void close_descriptor(int descriptor) noexcept;
void shm_unlink_name(const char* name) noexcept;
// post: `name` is unlinked only if it still resolves to `identity`; true when it was.
// note: POSIX has no unlink by inode, so a create racing its compare and unlink is undefended.
bool shm_unlink_if_identity(const char* name, SegmentIdentity identity) noexcept;
// refs: DN-103.D29
// invariant: a process-shared futex on a word of a shared mapping, never std::atomic::wait, whose
// futex is private to the process: a consumer in another process could never wake it.
// post: returns at once when `word` no longer reads `expected`; otherwise sleeps until a
// shm_unpark_all on the same word from any process, a signal or a spurious wake.
void shm_park(const std::atomic<std::uint32_t>& word, std::uint32_t expected) noexcept;
// post: every thread of every process parked on `word` is woken.
void shm_unpark_all(std::atomic<std::uint32_t>& word) noexcept;

struct alignas(kCacheLineBytes) Cursor
{
    std::atomic<std::uint64_t> value{0};
};

// invariant: lives at the start of the SHM mapping; the hot cursors sit on their own cache lines
// and the state machine on another, so close/abort never false-shares.
struct SharedChannelHeader
{
    std::uint64_t magic{kSharedChannelMagic};
    // invariant: kSharedChannelAbiVersion moves on ANY layout change of this header or the frame,
    // a same-size field reshuffle included — validate_header compares sizes, never shapes.
    std::uint32_t abi_version{kSharedChannelAbiVersion};
    std::uint32_t header_size{sizeof(SharedChannelHeader)};

    std::uint64_t slot_count{0};
    std::uint64_t slot_size{0};

    // refs: ADR-22.D4
    // invariant: channel-level and never per-frame; written once at create(), read once at open();
    // all-zero means Unspecified.
    std::array<char, kIntentChannelNameCapacity> intent_channel{};

    // refs: DN-103.D29
    // invariant: channel-level, written once at create() and read once at open(); the consumer's
    // admission reads it and no consumer re-derives it.
    SealGrid seal_grid{};

    // refs: DN-102.D2, ADR-11.D8
    // invariant: written once at create() and read only by the reaper, so no content, ordering or
    // window-membership surface depends on it.
    SegmentOwner owner{};

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
// invariant: standard layout, so the reaper reads a field at its offsetof from bytes it pread.
static_assert(std::is_standard_layout_v<SharedChannelHeader>);

class AdaptiveWait
{
  public:
    explicit AdaptiveWait(WaitStrategy strategy) noexcept : strategy_{strategy} {}

    // pre: `header` may be null; AdaptivePark then sleeps instead of parking on wake_epoch.
    // pre: `still_blocked` re-reads the condition the caller waits out; only AdaptivePark calls it.
    // refs: DN-103.D29
    // invariant: a parker registers, then re-reads its condition, then parks on the epoch it read
    // first; a freeing thread publishes its change, then reads parker_count, and a notifier bumps.
    // invariant: both sides fence seq_cst between their write and their read, so either the parker
    // sees the change or the freeing thread sees the parker and bumps the epoch it parks on.
    template <typename StillBlocked>
    void wait(SharedChannelHeader* header, StillBlocked still_blocked) noexcept
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
                park(*header, still_blocked);
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

    // post: whether the last wait() was taken past the spin and yield phases; meaningful for the
    // Adaptive and AdaptivePark progressions, the two that have a sleep phase after both.
    [[nodiscard]] bool past_yield_phase() const noexcept
    {
        return loops_ >= kYieldLoops;
    }

    void reset() noexcept
    {
        loops_ = 0;
    }

  private:
    static constexpr std::uint64_t kSpinLoops{64U};
    static constexpr std::uint64_t kYieldLoops{256U};

    // invariant: parker_count is raised across the park so a freeing thread knows a wake is owed; a
    // spurious wake is safe because the caller's loop re-checks.
    template <typename StillBlocked>
    static void park(SharedChannelHeader& header, StillBlocked& still_blocked) noexcept
    {
        header.parker_count.fetch_add(1, std::memory_order_seq_cst);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const auto epoch{header.wake_epoch.load(std::memory_order_seq_cst)};
        if (still_blocked())
        {
            shm_park(header.wake_epoch, epoch);
        }
        header.parker_count.fetch_sub(1, std::memory_order_seq_cst);
    }

    WaitStrategy strategy_;
    std::uint64_t loops_{0};
};

} // namespace coderoast::ipc

export namespace coderoast::ipc
{

// refs: ADR-26.D2
// invariant: WaitStrategy::Adaptive's paused spin and yields for a caller that owns no channel,
// then sleeps that double from 1 us to kIdleSleepCeiling instead of Adaptive's flat 1 us.
// invariant: the tail is this door's alone — a blocked push must resume the moment a slot frees,
// while an idle poller at a flat 1 us wakes tens of thousands of times a second to find nothing.
// invariant: the header it hands AdaptiveWait is null, which Adaptive never reads, so this door
// exposes no SharedChannelHeader.
class AdaptivePoll
{
  public:
    // post: no single sleep exceeds kIdleSleepCeiling, which bounds how late the first frame after
    // an idle stretch and a stop request are seen.
    void wait() noexcept
    {
        if (!wait_.past_yield_phase())
        {
            wait_.wait(nullptr, [] noexcept { return true; });
            return;
        }
        std::this_thread::sleep_for(idle_sleep_);
        idle_sleep_ = std::min(idle_sleep_ * kIdleSleepGrowth, kIdleSleepCeiling);
    }

    // post: the next wait() starts the progression over, from the paused spin.
    void reset() noexcept
    {
        wait_.reset();
        idle_sleep_ = kFirstIdleSleep;
    }

  private:
    static constexpr std::chrono::microseconds kFirstIdleSleep{2};
    static constexpr int kIdleSleepGrowth{2};
    static constexpr std::chrono::microseconds kIdleSleepCeiling{1000};

    AdaptiveWait wait_{WaitStrategy::Adaptive};
    std::chrono::microseconds idle_sleep_{kFirstIdleSleep};
};

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

    // post: the bytes a channel of `slot_count` slots maps, header included; nullopt when that size
    // is not representable in std::size_t.
    [[nodiscard]] static std::optional<std::size_t> segment_bytes(std::size_t slot_count) noexcept
    {
        const auto offset{data_offset()};
        if (slot_count > (std::numeric_limits<std::size_t>::max() - offset) / sizeof(Frame))
        {
            return std::nullopt;
        }
        return offset + (slot_count * sizeof(Frame));
    }

    // post: throws std::invalid_argument on a zero slot_count, a nullopt segment_bytes, or an
    // intent_channel at or past kIntentChannelNameCapacity, before any segment exists.
    // post: a stale segment of the same name is unlinked first.
    // post: from the moment the name exists the handle is its producer, so a throw past that point
    // unlinks it through close(), identity-checked like every close.
    // refs: DN-102.D3, DN-103.D20
    // post: throws shm_reserve's std::system_error, leaving no name, when the tmpfs cannot reserve
    // the segment.
    [[nodiscard]] static SharedMemorySpscChannel create(const ChannelConfig& config)
    {
        if (config.slot_count == 0U)
        {
            throw std::invalid_argument("IPC slot_count must be greater than zero");
        }
        const auto bytes{segment_bytes(config.slot_count)};
        if (!bytes.has_value())
        {
            throw std::invalid_argument("IPC slot_count " + std::to_string(config.slot_count) +
                                        " maps more bytes than std::size_t represents");
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
        // refs: DN-103.D29
        // assert: a consumer's admission divides by both lengths, so a channel declaring either as
        // zero is refused here rather than read as a grid by every consumer.
        if (config.seal_grid.window_length_ns == 0U || config.seal_grid.frontier_step_ns == 0U)
        {
            throw std::invalid_argument(std::format(
                "IPC channel '{}' declares a seal grid with window length {} ns and "
                "frontier step {} ns; both must be greater than zero",
                config.name, config.seal_grid.window_length_ns, config.seal_grid.frontier_step_ns));
        }

        SharedMemorySpscChannel channel;
        channel.name_ = normalise_channel_name(config.name);
        channel.policy_ = config.backpressure;
        channel.wait_strategy_ = config.wait_strategy;
        channel.map_size_ = *bytes;
        channel.slot_count_ = config.slot_count;
        channel.seal_grid_ = config.seal_grid;

        if (config.unlink_before_create)
        {
            shm_unlink_name(channel.name_.c_str());
        }

        const auto created{shm_open_create(channel.name_.c_str())};
        channel.fd_ = created.descriptor;
        channel.identity_ = created.identity;
        channel.is_producer_ = true;
        shm_reserve(channel.fd_, channel.map_size_, channel.name_.c_str());
        channel.map_memory();
        std::memset(channel.mapping_, 0, channel.map_size_);
        auto* header{new (channel.mapping_) SharedChannelHeader{}};
        header->slot_count = config.slot_count;
        header->slot_size = sizeof(Frame);
        // invariant: the array is value-initialised, so an empty declaration stays all-zero =
        // Unspecified and the copy keeps the NUL (size < capacity, checked above).
        std::memcpy(header->intent_channel.data(), config.intent_channel.data(),
                    config.intent_channel.size());
        header->seal_grid = config.seal_grid;
        header->owner = current_segment_owner();
        channel.header_ = header;
        channel.validate_header();
        return channel;
    }

    // post: throws std::runtime_error when the mapped header's magic, ABI version, slot size or
    // slot count disagrees with this build's: both ends instantiate one Frame, or neither opens.
    // post: throws std::runtime_error when the header's slot count maps any size but the segment's,
    // so no header indexes past the mapping.
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
        if (channel.map_size_ < data_offset())
        {
            throw std::runtime_error(
                std::format("IPC shared-memory channel '{}' is {} B, shorter than its {} B header",
                            channel.name_, channel.map_size_, data_offset()));
        }
        channel.map_memory();
        channel.header_ = static_cast<SharedChannelHeader*>(channel.mapping_);
        // assert: read once and kept, so a writer changing the header after this check cannot
        // move the ring past the mapping.
        channel.slot_count_ = static_cast<std::size_t>(channel.header_->slot_count);
        channel.validate_header();
        channel.seal_grid_ = channel.header_->seal_grid;
        return channel;
    }

    // refs: DN-103.D29
    // post: the grid the producer declared at create(), as this handle read it once; all zero on a
    // closed handle.
    [[nodiscard]] const SealGrid& seal_grid() const noexcept
    {
        return seal_grid_;
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
        return header_ == nullptr ? 0U : slot_count_;
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
                return PushStatus::Ok;
            }
            blocked = true;
            wait.wait(header_,
                      [this]() noexcept { return state() == ChannelState::Open && full(); });
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
            notify_space_freed();
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

    // refs: ADR-11.D7
    // post: the mapping and descriptor are released; a producer handle also unlinks its name while
    // that name still resolves to the segment it created, and a consumer handle never unlinks.
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
        if (is_producer_ && !name_.empty())
        {
            static_cast<void>(shm_unlink_if_identity(name_.c_str(), identity_));
        }
        mapping_ = nullptr;
        header_ = nullptr;
        fd_ = -1;
        map_size_ = 0U;
        slot_count_ = 0U;
        seal_grid_ = {};
        identity_ = {};
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
            header_->slot_size != sizeof(Frame) || slot_count_ == 0U)
        {
            throw std::runtime_error("IPC shared-memory channel header is incompatible");
        }
        const auto expected{segment_bytes(slot_count_)};
        if (!expected.has_value() || *expected != map_size_)
        {
            throw std::runtime_error(std::format("IPC shared-memory channel '{}' header claims {} "
                                                 "slots, {} B, over a mapping of {} B",
                                                 name_, slot_count_,
                                                 expected.has_value() ? std::to_string(*expected)
                                                                      : "more than size_t holds",
                                                 map_size_));
        }
    }

    [[nodiscard]] void* slot_ptr(std::uint64_t sequence) noexcept
    {
        const auto index{sequence % slot_count_};
        auto* base{static_cast<std::byte*>(mapping_)};
        return base + data_offset() + (index * sizeof(Frame));
    }

    [[nodiscard]] const void* slot_ptr(std::uint64_t sequence) const noexcept
    {
        const auto index{sequence % slot_count_};
        const auto* base{static_cast<const std::byte*>(mapping_)};
        return base + data_offset() + (index * sizeof(Frame));
    }

    [[nodiscard]] bool try_push_impl(const Frame& frame, bool count_drop) noexcept
    {
        const auto write{header_->write_sequence.value.load(std::memory_order_relaxed)};
        const auto read{header_->read_sequence.value.load(std::memory_order_acquire)};
        if (write - read >= slot_count_)
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

    // refs: DN-103.D29
    // invariant: only a Block producer parks on a channel, for a free slot, so only a pop owes it a
    // wake; a push frees nothing and wakes nobody.
    // post: wakes every parker only when parker_count is non-zero; the seq_cst fence pairs with
    // AdaptiveWait::park's, so a parker that missed this pop is seen here.
    void notify_space_freed() noexcept
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (header_->parker_count.load(std::memory_order_relaxed) == 0U)
        {
            return;
        }
        header_->wake_epoch.fetch_add(1, std::memory_order_seq_cst);
        shm_unpark_all(header_->wake_epoch);
    }

    // post: bumps wake_epoch unconditionally, so a parked thread sees the state change on its next
    // iteration.
    void notify_state_change() noexcept
    {
        header_->wake_epoch.fetch_add(1, std::memory_order_seq_cst);
        shm_unpark_all(header_->wake_epoch);
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
        slot_count_ = std::exchange(other.slot_count_, 0U);
        seal_grid_ = std::exchange(other.seal_grid_, {});
        policy_ = other.policy_;
        wait_strategy_ = other.wait_strategy_;
        identity_ = std::exchange(other.identity_, {});
        is_producer_ = std::exchange(other.is_producer_, false);
    }

    std::string name_;
    int fd_{-1};
    void* mapping_{nullptr};
    SharedChannelHeader* header_{nullptr};
    std::size_t map_size_{0};
    // invariant: the slot count validate_header checked against map_size_; the ring indexes with
    // it, never with the header's field.
    std::size_t slot_count_{0};
    SealGrid seal_grid_{};
    BackpressurePolicy policy_{BackpressurePolicy::Block};
    WaitStrategy wait_strategy_{WaitStrategy::Adaptive};
    SegmentIdentity identity_{};
    bool is_producer_{false};
};

} // namespace coderoast::ipc
