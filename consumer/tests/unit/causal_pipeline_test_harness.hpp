// pre: include after `import coderoast.ipc.consumer.test;` — it names that module's types.
// invariant: textual and not a module partition, so each including TU gets its own internal-linkage
// copy of the fixtures.
#pragma once

#include <unistd.h>

namespace
{
using Frame = coderoast::ipc::DefaultLineFrame;
using Channel = coderoast::ipc::SharedMemorySpscChannel<Frame>;
using Drainer = coderoast::ipc::consumer::ShmTransportDrainer<Frame>;
using Buffer = coderoast::ipc::consumer::CausalReorderBuffer<Frame>;
using Emitter = coderoast::ipc::consumer::FrameEmitter<Frame>;
using Consumer = coderoast::ipc::consumer::CausalShmConsumer<Frame>;
using Flags = coderoast::ipc::LineFrameFlags;

// refs: DN-103.D29
// invariant: a grid whose first window outlasts every tick these fixtures carry, so the consumer's
// admission never holds a frame back and each arm tests the merge alone.
constexpr coderoast::ipc::SealGrid kOpenGrid{.origin_unix_ns = 0U,
                                             .window_length_ns =
                                                 std::numeric_limits<std::uint64_t>::max() / 2U,
                                             .frontier_step_ns = 1U};

[[nodiscard]] std::string unique_channel(const char* suffix)
{
    return std::string{"coderoast_drainer_test_"} + suffix + "_" + std::to_string(::getpid());
}

[[nodiscard]] Frame make_frame(std::uint64_t sequence, std::uint32_t shard_id, const char* payload,
                               std::uint64_t logical_tick = 0, std::uint32_t agent_order = 0,
                               std::uint32_t intra_agent_index = 0,
                               Flags flags = Flags::kLineFrameFlagNone)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.shard_id = shard_id;
    frame.header.shard_sequence = sequence;
    frame.header.logical_tick = logical_tick;
    frame.header.agent_order = agent_order;
    frame.header.intra_agent_index = intra_agent_index;
    frame.header.flags = flags;
    frame.header.payload_size = static_cast<std::uint32_t>(std::strlen(payload));
    std::memcpy(frame.payload.data(), payload, frame.header.payload_size);
    return frame;
}

[[nodiscard, maybe_unused]] std::string payload_of(const Frame& frame)
{
    // note: the payload is raw bytes with an explicit size, not a NUL-terminated string.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return std::string{reinterpret_cast<const char*>(frame.payload.data()),
                       frame.header.payload_size};
}

constexpr std::size_t kHarnessSlots{16U};

struct ProducerHarness
{
    std::string base;
    std::vector<Channel> producers;

    ProducerHarness(const char* suffix, std::size_t shard_count,
                    coderoast::ipc::SealGrid grid = kOpenGrid,
                    std::size_t slot_count = kHarnessSlots)
        : base{unique_channel(suffix)}
    {
        producers.reserve(shard_count);
        for (std::size_t shard_id{0}; shard_id < shard_count; ++shard_id)
        {
            producers.emplace_back(Channel::create(coderoast::ipc::ChannelConfig{
                .name = coderoast::ipc::shard_channel_name(base, shard_id),
                .slot_count = slot_count,
                .seal_grid = grid,
            }));
        }
    }

    ~ProducerHarness() = default;

    ProducerHarness(const ProducerHarness&) = delete;
    ProducerHarness& operator=(const ProducerHarness&) = delete;
    ProducerHarness(ProducerHarness&&) = delete;
    ProducerHarness& operator=(ProducerHarness&&) = delete;
};
} // namespace
