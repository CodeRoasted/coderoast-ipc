#include <cstring> // ADR-3.D4 gcc-15 ICE: this TU calls std::memset (module-reachable) AND imports a
                   // first-party module (coderoast.ipc.core) — the combo ICEs the ealias pass
// (nonnull_arg_p). A textual <cstring> gives mem* a canonical decl and dodges it;
// everything else stays `import std`. (clang-21/libc++ has no such issue.)
#include <unistd.h> // POSIX getpid() — not in import std

#include <benchmark/benchmark.h>

import std;
import coderoast.ipc.core; // ADR-3.D4: ipc is a pure module now (headers deleted)

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

// Custom entry point (was BENCHMARK_MAIN()) so the run disables ASLR first — address-layout
// randomization otherwise adds run-to-run timing noise (Google Benchmark's "ASLR is enabled"
// warning).
int main(int argc, char** argv)
{
    benchmark::MaybeReenterWithoutASLR(argc, argv);
    benchmark::Initialize(&argc, argv);
    if (benchmark::ReportUnrecognizedArguments(argc, argv))
    {
        return 1;
    }
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
