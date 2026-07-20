// NOLINTBEGIN : Unit tests intentionally favour clarity over style.
//
// Shared fixtures for the pull-based causal SHM consumer pipeline unit tests
// (test_transport_drainer / test_causal_reorder_buffer / test_frame_emitter /
// test_causal_consumer — split by domain from the former
// test_causal_shm_consumer.cpp; helper bodies unchanged).
//
// The fixtures use real SHM channels (via SharedMemorySpscChannel) and unique
// PID-suffixed names so they are safe under parallel ctest.
//
// Textual header by design, not a module partition: the anonymous namespace
// gives every including TU its own internal-linkage copy of the fixtures.
// Include it AFTER `import coderoast.ipc.consumer.test;` — it names types
// exported by that module.
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

[[nodiscard]] std::string payload_of(const Frame& frame)
{
    return std::string{// NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                       reinterpret_cast<const char*>(frame.payload.data()),
                       frame.header.payload_size};
}

struct ProducerHarness
{
    std::string base;
    std::vector<Channel> producers;

    ProducerHarness(const char* suffix, std::size_t shard_count) : base{unique_channel(suffix)}
    {
        producers.reserve(shard_count);
        for (std::size_t shard_id{0}; shard_id < shard_count; ++shard_id)
        {
            producers.emplace_back(Channel::create(coderoast::ipc::ChannelConfig{
                .name = coderoast::ipc::shard_channel_name(base, shard_id),
                .slot_count = 16,
            }));
        }
    }

    ~ProducerHarness()
    {
        for (std::size_t shard_id{0}; shard_id < producers.size(); ++shard_id)
        {
            Channel::unlink(coderoast::ipc::shard_channel_name(base, shard_id));
        }
    }

    ProducerHarness(const ProducerHarness&) = delete;
    ProducerHarness& operator=(const ProducerHarness&) = delete;
    ProducerHarness(ProducerHarness&&) = delete;
    ProducerHarness& operator=(ProducerHarness&&) = delete;
};
} // namespace

// NOLINTEND : Unit tests intentionally favour clarity over style.
