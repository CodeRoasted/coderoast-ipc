// NOLINTBEGIN : Benchmarks are not production code and may intentionally violate some style rules
// for clarity or simplicity.
#include <cstddef>
#include <cstring>
#include <string>
#include <unistd.h>

#include <benchmark/benchmark.h>

#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/frame.hpp"

namespace
{
using Frame = coderoast::ipc::LineFrame<256>;

[[nodiscard]] std::string bench_channel_name()
{
    return "coderoast_ipc_bench_" + std::to_string(::getpid());
}

[[nodiscard]] Frame make_frame(std::uint64_t sequence)
{
    Frame frame{};
    frame.header.sequence = sequence;
    frame.header.shard_sequence = sequence;
    frame.header.payload_size = 32;
    std::memset(frame.payload.data(), static_cast<int>(sequence & 0x7FU),
                frame.header.payload_size);
    return frame;
}

void BM_SharedMemoryPushPop(benchmark::State& state)
{
    const auto name{bench_channel_name()};
    auto producer{coderoast::ipc::SharedMemorySpscChannel<Frame>::create(
        coderoast::ipc::ChannelConfig{.name = name,
                                      .slot_count = static_cast<std::size_t>(state.range(0)),
                                      .backpressure = coderoast::ipc::BackpressurePolicy::Block})};
    auto consumer{coderoast::ipc::SharedMemorySpscChannel<Frame>::open(name)};

    std::uint64_t sequence{0};
    Frame out{};
    for (auto _ : state)
    {
        benchmark::DoNotOptimize(producer.push(make_frame(++sequence)));
        benchmark::DoNotOptimize(consumer.try_pop(out));
    }
    state.counters["slots"] = static_cast<double>(state.range(0));
    coderoast::ipc::SharedMemorySpscChannel<Frame>::unlink(name);
}

BENCHMARK(BM_SharedMemoryPushPop)->Arg(1024)->Arg(8192)->Arg(65536);
} // namespace

BENCHMARK_MAIN();
// NOLINTEND : Benchmarks are not production code and may intentionally violate some style rules for
// clarity or simplicity.