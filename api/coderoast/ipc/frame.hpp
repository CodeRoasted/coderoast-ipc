#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace coderoast::ipc
{

inline constexpr std::uint32_t kIpcAbiVersion{1U};
inline constexpr std::size_t kDefaultLineFramePayloadBytes{4096U};

enum class FrameFormat : std::uint16_t
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
};

enum LineFrameFlags : std::uint16_t
{
    kLineFrameFlagNone = 0,
    kLineFrameFlagTruncated = 1U << 0U,
    kLineFrameFlagEndOfStream = 1U << 1U,
    kLineFrameFlagWindowSeal = 1U << 2U,
};

struct LineFrameHeader
{
    std::uint64_t sequence{0};
    std::uint64_t shard_sequence{0};
    std::uint64_t timestamp_unix_ns{0};
    std::uint64_t run_id{0};
    std::uint64_t window_id{0};
    std::uint32_t payload_size{0};
    std::uint32_t agent_id{0};
    std::uint32_t shard_id{0};
    FrameFormat format{FrameFormat::Unknown};
    std::uint16_t flags{kLineFrameFlagNone};
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