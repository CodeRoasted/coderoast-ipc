// note: getpid() is POSIX and not in `import std`, so this header stays textual.
#include <unistd.h>

#include <benchmark/benchmark.h>

import std;
import coderoast.ipc.core;

#include "reap_orphaned_segments_at_start.hpp"

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
}

BENCHMARK(BM_SharedMemoryPushPop)->Arg(1024)->Arg(8192)->Arg(65536);
} // namespace

// refs: DN-102.D2
// invariant: the reap runs once the arguments are parsed, so a --help run reaps nothing.
int main(int argc, char** argv)
{
    benchmark::MaybeReenterWithoutASLR(argc, argv);
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
    {
        return 1;
    }
    coderoast::ipc::testing::reap_orphaned_segments_at_start();
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
