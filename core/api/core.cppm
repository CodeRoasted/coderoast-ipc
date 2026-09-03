// coderoast.ipc.core — PURE named module (1.5.1 unwrap of the §8.1 wrapper). Header-only: the
// former api/coderoast/ipc/{frame,channel}.hpp content now lives in this module interface. std via
// `import std`.
//
// ADR-3.D4 cascade rule (errno-in-module): the POSIX shm syscalls touch C MACROS (errno, O_*,
// PROT_*, MAP_*) that CANNOT reach a module purview — `import std` poisons libc's include guards so
// a textual GMF `#include <cerrno>` no-ops, and macros never cross a module boundary regardless. So
// every syscall
// + macro lives in the TEXTUAL implementation unit coderoast_ipc_core_impl.cpp (own GMF, NO import
// std); this interface holds only the non-template `shm_*` declarations, crossing the
// boundary with primitives only (int/size_t/void*/const char*) — no std class type unifies across
// import-std↔textual. detail is a SEALED non-export namespace (ADR-3.D4): consumers get the public
// surface, not the helpers.
export module coderoast.ipc.core;
import std;

// ── Public surface: frame + channel data types ──────────────────────────────
export namespace coderoast::ipc
{

inline constexpr std::uint32_t kIpcAbiVersion{3U};
inline constexpr std::size_t kDefaultLineFramePayloadBytes{4096U};

// ── Shard channel naming: the WIRE CONTRACT, one definition ─────────────────
// Producer and consumer must derive the same channel name from the same (base, shard_id)
// or they open different segments and the pipeline silently connects to nothing. That makes
// this a contract, not a formatting helper — it lives here, in the module BOTH sides already
// import, so the two ends cannot drift.
//
// Member `+=` (extern-instantiated in libstdc++), NOT the free `operator+(std::string&&,
// const char*)`. THE SHAPE is the durable half: under `import std`, libstdc++ extern-instantiates
// only the lvalue overloads, so a pure-module TU that chains `operator+` over temporaries can link
// undefined in a downstream pure-module target (the insight_e2e SHM bench is where it landed).
// First met on gcc-15. See ADR-3.
//
// RE-MEASURED 2026-09-03 on the gcc-16.2 ship leg (`linux-gcc16-release`), wiped build tree: this
// function restored to `std::string{base} + "_shard_" + std::to_string(shard_id)`, together with
// the three sibling sites in insight-e2e, and `insight_e2e_bench` — the very target the defect
// used to break — LINKED CLEAN. So the overload IS emitted by the current ship compiler and the
// shape is inert here today. RETAINED per ADR-3.D7: the measurement covers the gcc-16.2 leg at
// this commit and not the MSVC leg, and the asymmetry decides — a wrong removal reds the ship leg
// at the tag, a wrong retention costs one spelling. "gcc-15" was never a bound on where the
// defect lives, only the compiler it was met on.
[[nodiscard]] inline std::string shard_channel_name(std::string_view base, std::size_t shard_id)
{
    std::string name{base};
    name += "_shard_";
    name += std::to_string(shard_id);
    return name;
}

// ── Why there is no `FrameFormat` here (ADR-22) ─────────────────────────────
// A per-line IntentFormat tag USED to ride this header. It was erased, and the rule that erased it
// governs every future field on this transport:
//
//   The transport carries exactly the facts the bytes cannot carry, and nothing they can.
//
// The IntentFormat is INTRINSIC — the bytes carry it (a JSON line looks like JSON; a GHA log
// contains
// `##[group]Run `), and canon MUST recover it from them, because bytes are all a real log gives it.
// A real GHA log arrives over HTTP with no frame header. So a transport that hands canon the answer
// is a CHEAT CHANNEL: the moment anything reads such a tag, the LogCraft→InSight path is measuring
// a privileged channel that does not exist in production, and every calibration number it produces
// is optimistic by an unmeasured amount. The moat is that canon recovers intent FROM BYTES.
//
// The IntentChannel (SharedChannelHeader::intent_channel, below) is the OPPOSITE case and is
// carried: no byte carries it, so someone must declare it — and SHM forwards that declaration
// rather than inventing it, leaving both paths equally informed. Erasing one field and adding the
// other is ONE rule applied in two directions, not an inconsistency.
//
// The test for any future field here: can a real log's bytes carry this fact? Yes ⇒ the transport
// MUST NOT carry it. No ⇒ it may.

// uint16_t is intentional: stable IPC ABI (paired with `reserved` to
// keep the header free of implicit padding — see LineFrameHeader).
enum class LineFrameFlags : std::uint16_t // NOLINT(performance-enum-size)
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
    // Explicit tail padding, sized so the header has NO implicit padding (asserted below). The
    // erased FrameFormat used to be the uint16 that paired with `flags`; `reserved` takes that slot
    // rather than letting the compiler insert 2 anonymous bytes, keeping the "no padding" property
    // structural.
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
// The header is memcpy'd across a process boundary, so implicit padding would put INDETERMINATE
// bytes on the wire — unreadable to a consumer that ever inspected them, and a bit-identity hazard
// for anything that hashes a frame. `reserved` exists to make the property hold structurally; this
// asserts it stays true, so a future field that reintroduces a hole fails the build instead of
// shipping.
static_assert(std::has_unique_object_representations_v<LineFrameHeader>,
              "LineFrameHeader has implicit padding — size the trailing `reserved` field so the "
              "members tile the struct exactly (an IPC header is memcpy'd; padding bytes are "
              "indeterminate on the wire)");

// Concept satisfied by LineFrame<N> (and any user type with the same header layout).
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

inline constexpr std::uint64_t kSharedChannelMagic{0x4352495043535053ULL}; // CRIPCSPS
inline constexpr std::uint32_t kSharedChannelAbiVersion{5U};
inline constexpr std::size_t kDefaultSharedChannelSlotCount{8192U};

// Capacity of SharedChannelHeader::intent_channel, including the NUL terminator. A channel name is
// a short package-declared identifier ("annotated", "stripped"); 32 leaves generous headroom while
// keeping the header a fixed-layout POD. A longer name is REFUSED at open (never truncated — a
// truncated name would either fail the consumer's vocabulary check far from its cause, or, worse,
// silently alias another declared name).
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

/// Lifecycle state of a channel.  Stored in shared memory; both
/// endpoints observe the same value via acquire loads.
enum class ChannelState : std::uint8_t
{
    Open = 0,    //< Producer may push, consumer may pop.
    Closing = 1, //< Graceful close requested; no new pushes, drain in progress.
    Closed = 2,  //< Consumer has drained every frame written before Closing.
    Aborted = 3, //< Force-stop (terminal).  All blocked ops wake.
};

/// Outcome of a producer-side write attempt.
enum class PushStatus : std::uint8_t
{
    Ok = 0,      //< Frame written to the ring.
    Full = 1,    //< Ring full (non-blocking variant or DropNewest policy).
    Closed = 2,  //< `close_graceful` was called; no further data accepted.
    Aborted = 3, //< `close_abort` was called; producer must give up.
};

/// Outcome of a consumer-side read attempt.
enum class PopStatus : std::uint8_t
{
    Ok = 0,      //< Frame popped.
    Empty = 1,   //< Ring empty, channel still Open.
    Closed = 2,  //< Ring empty and producer has gracefully closed; no
                 //   further frames will ever arrive.
    Aborted = 3, //< Channel was force-stopped.  Residual frames may be
                 //   discarded or drained at consumer's discretion.
};

struct ChannelConfig
{
    std::string name; // the SHM ring's name — NOT the intent_channel below
    std::size_t slot_count{kDefaultSharedChannelSlotCount};
    BackpressurePolicy backpressure{BackpressurePolicy::Block};
    WaitStrategy wait_strategy{WaitStrategy::Adaptive};
    bool unlink_before_create{true};
    bool unlink_on_destroy{false};
    // The declared underlying IntentChannel this ring TRANSPORTS (ADR-22). Set by the producer
    // at create(); the consumer reads it back off the header at open(). Empty = Unspecified (the
    // producer did not declare), which is every non-dialect stream.
    //
    // Spelled in FULL, never bare `channel`: `name` above is this ring, and `channel` in this
    // namespace means the ring throughout. The collision is not an accident to hide but the
    // structure to state — the SHM channel CONTAINS an IntentChannel.
    //
    // WHY THE TRANSPORT MAY CARRY THIS, when it may not carry a format (ADR-22): the channel
    // is EXTRINSIC. No byte carries it — a prose line is byte-identical across channels, which is
    // exactly why it must be declared and cannot be recovered. So SHM FORWARDS the producer's
    // declaration rather than inventing an answer, leaving the SHM path and the real `--channel`
    // path equally informed. That is not a cheat. A format tag would be: the bytes carry the
    // format, so handing it over would privilege this path over the real one.
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

// ── Internal helpers (SEALED, non-export) — uses the public data above ───────
// NAMED namespace, NOT anonymous: these helpers are referenced by the INLINE channel
// templates below, so a consumer instantiating those templates would expose them. An
// anonymous namespace gives them TU-local linkage → "instantiation exposes TU-local entity"
// under modules (ADR-3.D4). `detail` (non-export) seals them from consumers while giving
// them module linkage. Do NOT let clang-tidy `misc-use-anonymous-namespace` revert this.
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

// ── POSIX shm syscall wrappers — DEFINED in coderoast_ipc_core_impl.cpp ──────
// Each owns one syscall + its C macros (errno, O_*, PROT_*, MAP_*) which cannot
// live in this import-std interface (ADR-3.D4 cascade rule, see file header). The
// boundary crosses primitives only; the throwing ones raise std::runtime_error
// built inside the impl unit. fd helpers return a VALID descriptor or throw.
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

/// Shared-memory header layout.  Lives at the start of the SHM
/// mapping; both endpoints see the same atomics.
///
/// Layout discipline: hot producer/consumer cursors are on their
/// own cache lines; the state-machine fields share a separate line
/// (touched only on close/abort, no false sharing with data path).
// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding) Explicit padding for false sharing
struct SharedChannelHeader
{
    // --- identity ---
    std::uint64_t magic{kSharedChannelMagic};
    std::uint32_t abi_version{kSharedChannelAbiVersion};
    std::uint32_t header_size{sizeof(SharedChannelHeader)};

    std::uint64_t slot_count{0};
    std::uint64_t slot_size{0};

    // --- the transported IntentChannel (ADR-22) ---
    // The declared underlying IntentChannel of the stream this ring carries, NUL-terminated ASCII;
    // all-zero = Unspecified. CHANNEL-LEVEL, deliberately never per-frame: one IntentChannel per
    // TREE is the contract, and a per-frame field would *permit* the multi-channel tree that
    // contract forbids — as well as paying bytes per line for a fact that is constant per stream.
    //
    // Written once by the producer at create(), read once by the consumer at open(): a cold,
    // header-only fact, out of the frame path entirely. It sits with slot_count/slot_size (the
    // other create-time identity fields), not on a cache line with the hot cursors.
    std::array<char, kIntentChannelNameCapacity> intent_channel{};

    // --- atomic control plane (isolated cache lines) ---
    alignas(kCacheLineBytes) std::atomic<std::uint32_t> wake_epoch{0};
    alignas(kCacheLineBytes) std::atomic<std::uint32_t> parker_count{0};

    // --- cursors ---
    Cursor write_sequence{};
    Cursor read_sequence{};
    Cursor closing_at{};
    // --- stats (relaxed updates) ---
    Cursor dropped{};
    Cursor blocked_events{};
    Cursor wait_loops{};

    // --- state machine ---
    alignas(kCacheLineBytes) std::atomic<std::uint8_t> state{
        static_cast<std::uint8_t>(ChannelState::Open)};
};

static_assert(alignof(SharedChannelHeader) >= kCacheLineBytes);

/// Adaptive busy-wait helper.  See the WaitStrategy documentation
/// at the top of the file for the per-strategy progression.
class AdaptiveWait
{
  public:
    explicit AdaptiveWait(WaitStrategy strategy) noexcept : strategy_{strategy} {}

    /// Perform one wait iteration.
    ///
    /// `header` may be `nullptr`; when non-null and the strategy is
    /// `AdaptivePark`, the late stages of the wait park on the
    /// header's `wake_epoch` atomic.  Notifiers (push/pop/state
    /// transition) bump `wake_epoch` to release parked threads.
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
                // Kernel-park on the wake_epoch.  Bump parker_count
                // around the wait so the notifier side knows to
                // actually wake us up.  Spurious wakeups are fine:
                // the surrounding loop re-checks the condition.
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

// ── Public surface: the shared-memory SPSC channel (uses detail) ─────────────
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
        // Producer-side RAII guarantee: if the owning producer destructs
        // without an explicit close, transition the channel to Closing
        // so consumers observe end-of-stream instead of dangling on Open.
        // Consumer-side handles must not initiate the close transition.
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

        // REFUSED, never truncated (ADR-22): a clipped channel name would reach the consumer
        // as a different string — failing its vocabulary check far from the cause, or in the worst
        // case aliasing another declared name and silently mis-gating recognition.
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
        // Forward the producer's DECLARATION (ADR-22) — the ring encapsulates an
        // IntentChannel, so it must say which. The array is value-initialised, so an empty
        // declaration stays all-zero = Unspecified, and the copy leaves the NUL terminator in place
        // (size < capacity, checked above).
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
        // is_producer_ stays false: consumer-opened handles must not
        // drive state transitions on destruction.
        return channel;
    }

    // The declared underlying IntentChannel this ring transports (ADR-22). Empty =
    // Unspecified: the producer did not declare, so a consumer must not claim dialect depth from
    // this stream — the same fail-closed-on-DEPTH posture as an undeclared `--channel`. Read off
    // the header, so a consumer never has to be told out-of-band what it is already carrying.
    [[nodiscard]] std::string_view intent_channel() const noexcept
    {
        if (header_ == nullptr)
        {
            return {};
        }
        // NUL-terminated by construction (create() refuses a name that would fill the array).
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

    // -- State / lifecycle --------------------------------------------

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

    /// Snapshot of the write_sequence at the moment graceful close was
    /// requested.  Meaningful only when `state() != Open`.  Consumers
    /// use it to detect "all data delivered" without an EOS frame.
    [[nodiscard]] std::uint64_t closing_at() const noexcept
    {
        return header_ == nullptr ? 0U : header_->closing_at.value.load(std::memory_order_acquire);
    }

    /// Producer-side: signal "no more data will be written".  Atomic,
    /// non-blocking, idempotent.  After this, push() returns Closed.
    /// Consumers keep popping frames already in the ring; once their
    /// read_sequence reaches `closing_at`, try_pop returns Closed.
    ///
    /// Safe to call from ANY thread (including a watchdog).  Cannot
    /// override an Aborted state.
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
        // Always notify: a producer may be blocked in push() under
        // Block policy; it must observe the state change and exit.
        notify_state_change();
    }

    /// Out-of-band cancel.  Transitions to Aborted (terminal), wakes
    /// every parked thread.  In-flight frames may still be readable by
    /// the consumer depending on its policy.  Safe to call from any
    /// thread.
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

    // -- Hot-path producer / consumer API -----------------------------

    /// Non-blocking write.  Returns Ok / Full / Closed / Aborted.
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

    /// Backpressured write honouring the configured policy.  Block
    /// policy waits via the configured WaitStrategy and is cancellable
    /// by close_graceful / close_abort.
    [[nodiscard]] PushStatus push_status(const Frame& frame) noexcept
    {
        if (policy_ == BackpressurePolicy::DropNewest)
        {
            return try_push_status(frame);
        }
        // Block policy.
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

    /// Non-blocking read.  Returns Ok / Empty / Closed / Aborted.
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
        // Closing or Closed.  Re-confirm we have actually drained up to
        // closing_at; if the consumer reached the snapshot the stream is
        // terminally closed and we promote Closing -> Closed for clarity.
        const auto closing_seq{header_->closing_at.value.load(std::memory_order_acquire)};
        if (read >= closing_seq)
        {
            std::uint8_t exp{static_cast<std::uint8_t>(ChannelState::Closing)};
            (void)header_->state.compare_exchange_strong(
                exp, static_cast<std::uint8_t>(ChannelState::Closed), std::memory_order_acq_rel,
                std::memory_order_acquire);
            return PopStatus::Closed;
        }
        // Producer raced us -- closing_at not reached yet but we just
        // saw the ring empty.  The next call will see the freshly-written
        // frame or the closing condition.
        return PopStatus::Empty;
    }

    // -- Bool-returning shims for backwards compatibility -------------

    /// Returns true iff the push wrote a frame.  Closure or abort look
    /// the same as "full" to legacy callers -- they must use
    /// `try_push_status` if they need to disambiguate.
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
        return base + data_offset() + (index * sizeof(Frame)); // NOLINT
    }

    [[nodiscard]] const void* slot_ptr(std::uint64_t sequence) const noexcept
    {
        const auto index{sequence % header_->slot_count};
        const auto* base{static_cast<const std::byte*>(mapping_)};
        return base + data_offset() + (index * sizeof(Frame)); // NOLINT
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

    /// Fast-path notify: bump wake_epoch only if someone is parked.
    /// Cost in the no-contention case: one relaxed load + branch.
    void notify_progress() noexcept
    {
        if (header_->parker_count.load(std::memory_order_acquire) == 0U)
        {
            return;
        }
        header_->wake_epoch.fetch_add(1, std::memory_order_acq_rel);
        header_->wake_epoch.notify_all();
    }

    /// Rare-path notify: bump wake_epoch unconditionally so any parked
    /// thread observes the state change on its next iteration.
    void notify_state_change() noexcept
    {
        header_->wake_epoch.fetch_add(1, std::memory_order_acq_rel);
        header_->wake_epoch.notify_all();
    }

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
