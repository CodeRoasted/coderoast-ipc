# coderoast-ipc

Shared-memory IPC primitives for CodeRoast pipelines: high-performance SPSC channels, frame types, and producer/consumer adapters.

**Status:** Header-only C++23 library. No runtime dependencies. Suitable for embedding in any log pipeline.

**Current package baseline:** `coderoast_ipc_core/1.0.1`, `coderoast_ipc_consumer/1.0.1`, and `coderoast_ipc_producer/1.0.1`. Cross-repo compatibility is tracked in [../technical_docs/compatibility_matrix.md](../technical_docs/compatibility_matrix.md), and planning lives in [../technical_docs/ROADMAP.md](../technical_docs/ROADMAP.md).

---

## Package Overview

`coderoast-ipc` provides three distinct packages (all versioned together):

| Package | Purpose | Dependencies | Use When |
|---------|---------|--------------|----------|
| **coderoast_ipc_core** | SPSC channels, frame types, ABI constants | None | You need low-level transport primitives |
| **coderoast_ipc_consumer** | Adapter for consuming ordered frames | `coderoast_ipc_core` | You're building a frame consumer/sink |
| **coderoast_ipc_producer** | Helper for building and sequencing frames | `coderoast_ipc_core` | You're building a frame producer/source |

All packages are header-only: no compilation, instant build times, zero runtime overhead.

---

## Quick Start

### Building Locally (No External Tools Required)

```bash
# Clone the repo
cd coderoast-ipc

# Build all three packages into local .conan2 cache
conan create . \
  --profile:host=.conan2/profiles/linux-gcc13-release \
  --profile:build=.conan2/profiles/linux-gcc13-release \
  --build=missing \
  --build-test=missing
```

**What this does:**
- Detects profile at `.conan2/profiles/linux-gcc13-release`
- Builds/tests core, consumer, and producer packages
- Stores packages in local `.conan2` cache
- Exits with status 0 on success

### Building Individual Packages

If you only need specific packages:

```bash
# Build core only
conan create core \
  --profile:host=.conan2/profiles/linux-gcc13-release \
  --profile:build=.conan2/profiles/linux-gcc13-release \
  --build=missing

# Build consumer (automatically pulls core dependency)
conan create consumer \
  --profile:host=.conan2/profiles/linux-gcc13-release \
  --profile:build=.conan2/profiles/linux-gcc13-release \
  --build=missing

# Build producer (automatically pulls core dependency)
conan create producer \
  --profile:host=.conan2/profiles/linux-gcc13-release \
  --profile:build=.conan2/profiles/linux-gcc13-release \
  --build=missing
```

---

## API Usage

### Core Package: SPSC Channels

Shared-memory single-producer-single-consumer queue:

```cpp
#include "coderoast/ipc/channel.hpp"
#include "coderoast/ipc/frame.hpp"

using namespace coderoast::ipc;

// Configure channel
ChannelConfig cfg{
    .name = "myapp.pipeline",
    .slot_count = 8192,
    .backpressure = BackpressurePolicy::DropNewest,
};

// Sender side
SharedMemoryChannel<DefaultLineFrame> sender{cfg};
{
    auto [frame, acquired] = sender.try_acquire_write();
    if (acquired) {
        frame->header.sequence = 42;
        frame->header.timestamp_unix_ns = std::chrono::system_clock::now().time_since_epoch().count();
        std::memcpy(frame->payload.data(), "log line", 8);
        frame->header.payload_size = 8;
        sender.commit_write();
    }
}

// Receiver side
SharedMemoryChannel<DefaultLineFrame> receiver{
    ChannelConfig{.name = "myapp.pipeline", .unlink_before_create = false}
};
{
    auto [frame, acquired] = receiver.try_acquire_read();
    if (acquired) {
        std::cout << "Seq: " << frame->header.sequence << "\n";
        std::cout << "Payload: " << std::string_view(
            reinterpret_cast<const char*>(frame->payload.data()),
            frame->header.payload_size) << "\n";
        receiver.commit_read();
    }
}
```

**Key Types:**
- `SharedMemoryChannel<Frame>` - SPSC queue template
- `DefaultLineFrame` - 4KB payload frames (customizable)
- `LineFrameHeader` - Transport sequence, causal key, timestamp, format, payload metadata
- `BackpressurePolicy` - Block, DropNewest, OverwriteOldest
- `WaitStrategy` - Spin, SpinYield, Adaptive, AdaptivePark, ParkOnly

### Consumer Package: Ordered Frame Stream

Adapter for consuming frames by the `header.sequence` values supplied by the producer, with gap handling:

```cpp
#include "coderoast/ipc/consumer/shared_memory_source.hpp"

using namespace coderoast::ipc::consumer;

// Create consumer over multiple shards
SharedMemorySource<> source{SharedMemorySource<>::Config{
    .channel = "myapp.pipeline",
    .shard_count = 4,
    .first_sequence = 1U,
    .gap_policy     = coderoast::ipc::consumer::SequenceGapPolicy::WaitForMissing,
    .ordering       = coderoast::ipc::consumer::FrameOrdering::CausalKey,
    .backpressure   = coderoast::ipc::BackpressurePolicy::Block,
    .wait_strategy  = coderoast::ipc::WaitStrategy::Adaptive,
}};

// Consume ordered frames
std::string_view payload;
while (source.try_pop(payload)) {
    // payload is a zero-copy view into internal buffer
    // valid until next try_pop() call
    std::cout << "Frame: " << payload << "\n";
    
    if (payload == "END") break;
}
```

**Key Features:**
- Gap detection: Wait for missing sequences or skip
- Ordered delivery by producer-supplied `header.sequence`
- Zero-copy: String views into internal buffers
- Shard-aware: Handles multi-shard producers

`header.sequence` is a transport sequence, not automatically a deterministic
simulation order. If a producer assigns it with an atomic counter from
multiple shard threads, the counter records that run's physical arbitration
order. Deterministic cross-run replay requires a separate logical merge key
such as virtual timestamp, scheduler sequence, stable agent index, and
per-agent record index.

Frames now carry that logical merge key directly as
`(logical_tick, agent_order, intra_agent_index)`. Consumers that need
cross-run determinism should merge by that causal key and use `sequence` only
for gap detection or as a final same-key tie-breaker.

### Producer Package: Frame Builder

Helper for constructing and sequencing frames:

```cpp
#include "coderoast/ipc/producer/shared_memory_producer.hpp"

using namespace coderoast::ipc::producer;

// Create builder for 4 shards
FrameBuilder builder{FrameBuilder::Config{
    .shard_count = 4,
    .first_sequence = 1,
}};

// Get stable agent ID (same across restarts)
auto agent_id = stable_agent_id("myapp");

// Build frame for shard 2
auto frame = builder.build(
    /*shard_id=*/2,
    /*timestamp_unix_ns=*/std::chrono::system_clock::now().time_since_epoch().count(),
    /*payload_size=*/8,
    /*agent_id_hash=*/agent_id,
    /*format=*/FrameFormat::Text
);

// Frame has auto-incremented transport + per-shard sequences
std::cout << "Transport seq: " << frame.header.sequence << "\n";
std::cout << "Shard seq: " << frame.header.shard_sequence << "\n";
```

**Key Features:**
- Auto-incrementing transport sequence and per-shard sequence
- Stable agent ID hashing (FNV-1a)
- Format enum mapping
- Timestamp injection

The transport sequence is unique and useful for tracing and gap detection.
Under concurrent producers it should not be used as a canonical deterministic
global order, because the next sequence holder is chosen by runtime thread
scheduling.

---

## Local Development

### CMake Iteration (Manual Builds & Tests)

For iterative development without Conan:

```bash
# Install dependencies locally
conan install . \
  --output-folder=build \
  --build=missing \
  --profile:host=.conan2/profiles/linux-gcc13-release \
  --profile:build=.conan2/profiles/linux-gcc13-release \
  -s build_type=Debug

# Build with CMake
cmake --preset conan-debug -S . -B build
cmake --build build

# Run tests
ctest --test-dir build --output-on-failure

# Run benchmarks
./build/core/bin/coderoast_ipc_core_bench
```

### Code Quality

```bash
# Lint (clang-tidy, excludes .conan2 and build/)
clang-tidy-18 -p build core/**/*.cpp core/**/*.hpp \
  --header-filter="^(?!.*\.conan2|.*build).*$" \
  --config-file=.clang-tidy

# Format (clang-format)
clang-format-18 -i core/**/*.{cpp,hpp} consumer/**/*.{cpp,hpp} producer/**/*.{cpp,hpp}
```

---

## Publishing & Stable Cache

### Exporting to Shared Stable Cache

After bumping version or changing ABI, export to the shared cache where downstream repos resolve dependencies:

```bash
# Export all three packages to /opt/coderoast/conan-stable
CONAN_HOME=/opt/coderoast/conan-stable conan create . \
  --profile:host=linux-gcc13-release \
  --profile:build=linux-gcc13-release \
  --build=missing
```

This makes packages available to:
- `logcraft` (consumes `coderoast_ipc_core`)
- `insight-eidos` (consumes `coderoast_ipc_consumer`)
- Any other downstream package

### GitHub Release Workflow

Tags use `v1.0.1` semver format. GitHub Actions automatically:

1. Verify `conanfile.py` version matches tag
2. Build all three packages
3. Export Conan cache tarball (`coderoast_ipc-1.0.1.tgz`)
4. Attach tarball to GitHub release

Consumers restore with:
```bash
conan cache restore coderoast_ipc-1.0.1.tgz
```

---

## Requirements

- **OS:** Linux (macOS/Windows support possible with POSIX shim)
- **C++ Standard:** C++23 (compilers: GCC 13+, Clang 16+)
- **Build System:** CMake 3.28+ with Ninja
- **Package Manager:** Conan 2.x
- **Runtime Dependencies:** None (header-only)
- **Test Dependencies:** GTest 1.17.0 (test-only), Google Benchmark 1.8.3 (benchmark-only)

---

## Troubleshooting

### Profile not found

If you get `profile 'linux-gcc13-release' does not exist`, ensure it's created:

```bash
conan profile detect --force
# Then copy/customize as needed to .conan2/profiles/linux-gcc13-release
```

### Cannot find gtest or benchmark

These are test-only dependencies and only needed if building with tests. If you see linker errors, ensure:

```bash
# Use --build=missing to auto-build test dependencies
conan create . --build=missing --build-test=missing
```

### Tests pass but benchmarks won't run

Benchmarks are optional and only built if `CODEROAST_IPC_CORE_BUILD_BENCH=ON`. To enable:

```bash
cmake --preset conan-debug -S core -B core/build -DCODEROAST_IPC_CORE_BUILD_BENCH=ON
cmake --build core/build
./core/build/bin/coderoast_ipc_core_bench
```

---

## Architecture & Design

### Frame Format & ABI

Frames are fixed-layout, trivially copyable structures designed for shared-memory IPC:

```cpp
struct LineFrameHeader {
    uint64_t sequence;           // Transport sequence counter
    uint64_t shard_sequence;     // Per-shard sequence counter
    uint64_t timestamp_unix_ns;  // Nanosecond Unix timestamp
    uint64_t logical_tick;       // Deterministic causal tick
    uint64_t run_id;             // Correlate frames within a pipeline run
    uint64_t window_id;          // Batch/window grouping
    uint32_t payload_size;       // Bytes in payload (0 to max)
    uint32_t agent_id;           // FNV-1a hash of agent name
    uint32_t agent_order;        // Stable scenario agent order
    uint32_t intra_agent_index;  // Per-agent generation counter
    uint32_t shard_id;           // Shard affinity
    FrameFormat format;          // 20+ format types (JSON, Text, CLF, etc.)
    LineFrameFlags flags;        // Truncated, EndOfStream, WindowSeal
    uint32_t reserved;           // ABI padding
};

template <size_t MaxPayload>
struct LineFrame {
    LineFrameHeader header;
    array<byte, MaxPayload> payload;
};
```

ABI version constants ensure compatibility:
- `kIpcAbiVersion = 2`
- `kSharedChannelAbiVersion = 2`

`sequence` and `shard_sequence` are transport metadata. Deterministic consumers
should reconstruct canonical order with `(logical_tick, agent_order,
intra_agent_index, shard_id)`. The trailing `shard_id` is a tie-break for the
otherwise unlikely case where two shards emit frames with identical
`(logical_tick, agent_order, intra_agent_index)` — without it, the k-way merge
would visit those frames in arrival order, which is non-deterministic.

`WindowSeal` and `EndOfStream` are completion barriers. In deterministic
LogCraft runs, seals are emitted at scheduler-defined window/epoch boundaries,
not continuously per frame. A `WindowSeal(window_id, logical_tick=T)` means the
emitting shard will not produce additional data frames with `logical_tick < T`.
Consumers may finalize a window only after every shard has sealed the boundary
or ended.

### `WindowClosedConsumer` — deterministic per-window barriers

Raw `CausalShmConsumer` exposes one `WindowSeal` frame *per shard*, in the
order they arrive at the merge frontier. That order is not strictly
deterministic when several shards drain incrementally (the seal of whichever
shard has buffered records first crosses the frontier first). For consumers
that only need to know *when a window has been fully sealed by every shard*,
`coderoast::ipc::consumer::WindowClosedConsumer<Frame>` wraps a
`CausalShmConsumer` and:

- forwards data frames unchanged,
- absorbs the per-shard seal frames internally, and
- emits exactly one `WindowClosed{window_id, logical_tick}` event after the
  N-th shard has sealed that window.

Pair `shm_window_seal_interval_seconds` on the producer side with the
downstream window cadence (e.g. InSight's MetaLog `pyramid.window_ns`; the
default `kWindowDuration` is 25 s in `coderoast-server`) so that one
`WindowClosed` event corresponds to one consumer-side window close.

### Backpressure Strategies

- **Block:** Sender waits until space available (fairness, bounded latency)
- **DropNewest:** Reject new writes if full (low-latency producers)
- **OverwriteOldest:** Overwrite unread frames if full (sliding window)

### Wait Strategies

- **Spin:** Pure spin (lowest latency, highest CPU).
- **SpinYield:** Spin then `std::this_thread::yield()` (balanced).
- **Adaptive:** Spin, yield, then `std::this_thread::sleep_for(1us)` (default, low-latency).
- **AdaptivePark:** Spin, yield, then futex park via `std::atomic::wait` (efficient).
- **ParkOnly:** Immediately park (battery-friendly).

---

## Performance

Benchmarks measure throughput and latency for typical scenarios:

```bash
# From repo root
./core/build/bin/coderoast_ipc_core_bench
```

Results saved to `core/bench_results/baseline.json` with timestamp.

Typical on modern hardware:
- **Throughput:** 1M+ frames/sec per shard
- **Latency:** Sub-microsecond for small payloads
- **Memory:** Fixed overhead per channel (sharded)

---

## License

Apache 2.0. See [LICENSE](LICENSE).