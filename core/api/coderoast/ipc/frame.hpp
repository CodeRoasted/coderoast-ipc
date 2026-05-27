#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace coderoast::ipc
{

inline constexpr std::uint32_t kIpcAbiVersion{2U};
inline constexpr std::size_t kDefaultLineFramePayloadBytes{4096U};

// uint16_t is intentional: stable IPC ABI (paired with `flags` to
// keep the header at 4-byte alignment) and headroom past 256 formats.
enum class FrameFormat : std::uint16_t // NOLINT(performance-enum-size)
{
    Unknown = 0,
    Json = 1,
    Text = 2,
    Clf = 3,
    ApacheError = 4,
    Log4j = 5,
    Syslog = 6,
    Rfc5424 = 7,
    NginxError = 8,
    Kv = 9,
    AndroidLogcat = 10,
    WindowsCbs = 11,
    SparkHdfs = 12,
    HealthApp = 13,
    Proxifier = 14,
    CloudWatch = 15,
    SystemdJournal = 16,
    Hpc = 17,
    IisW3c = 18,
    Ecs = 19,
    OtelJson = 20,
    GitHubActions = 21,
};

// uint16_t is intentional: stable IPC ABI (paired with `flags` to
// keep the header at 4-byte alignment) and headroom past 256 formats.
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
    FrameFormat format{FrameFormat::Unknown};
    LineFrameFlags flags{LineFrameFlags::kLineFrameFlagNone};
    std::uint32_t reserved{0};
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

} // namespace coderoast::ipc