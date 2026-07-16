# coderoast-ipc

Shared-memory IPC primitives for CodeRoast pipelines: high-performance SPSC channels, frame types, and producer/consumer adapters.

**Status:** C++23 named-module library (`coderoast.ipc.core` / `.producer` / `.consumer`). No third-party runtime dependencies. Suitable for embedding in any log pipeline.

---

## Determinism

`coderoast-ipc` exists to make a sharded, shared-memory pipeline **deterministically replayable**: the same logical inputs produce a **bit-identical consumed stream**, run after run, independent of thread scheduling or host. That property is what makes any pipeline built on it reproducibly testable — and it is a deliberate guarantee, not an accident of timing.

The honest subtlety: a multi-shard shared-memory transport is *physically* racy, and we don't hide that. Determinism is restored at one precise, narrow boundary — the consumer's causal merge — and the contract draws a bright line on either side of it.

**Raw transport — deliberately non-deterministic. Never assert on these:**
- `header.sequence`, the global transport counter — the OS scheduler picks the next slot winner.
- The order in which frames from different shards *arrive* at the consumer, before the merge.
- The order in which per-shard `WindowSeal` frames physically land.

**Consumed stream — bit-identical. Rely on these:**
- The causal merge reorders every frame by `CausalKey = (logical_tick, agent_order, intra_agent_index, shard_id)` behind a frontier/watermark gate, so the same inputs yield the same consumed order, always.
- Per-window frame membership and the data-before-seal ordering within each window are reconciled and reproducible.
- `WindowClosedConsumer` collapses the per-shard seals into exactly one `WindowClosed` event per window, so the **set and count of closed windows are fixed** for a given replay — never a function of transport timing.

The racy interleave is **quarantined to the raw transport and never observed by application code**; everything the causal merge reconciles is a guarantee you can build on. Restoring determinism across that concurrent boundary *is* the point of this library.

---

## Package Overview

`coderoast-ipc` provides three distinct packages (all versioned together):

| Package | Module | Purpose | Dependencies | Use When |
|---------|--------|---------|--------------|----------|
| **coderoast_ipc_core** | `coderoast.ipc.core` | SPSC channels, frame types, ABI constants | None | You need low-level transport primitives |
| **coderoast_ipc_consumer** | `coderoast.ipc.consumer` | Adapter for consuming ordered frames | `coderoast_ipc_core` | You're building a frame consumer/sink |
| **coderoast_ipc_producer** | `coderoast.ipc.producer` | Helper for building and sequencing frames | `coderoast_ipc_core` | You're building a frame producer/source |

Each package ships as a **pure C++ named module** — consumers `import` it; there are no textual headers to `#include`. The interface compiles once into a small static library (the former header-only surface now lives in the module interface, plus one impl unit for the syscall wrappers); downstream builds consume the prebuilt BMI and `.a`.

---

## Quick Start

### Building Locally (No External Tools Required)

```bash
# Clone the repo
cd coderoast-ipc

# Build all three packages into local .conan2 cache
conan create . \
  --profile:host=.conan2/profiles/linux-gcc15-release \
  --profile:build=.conan2/profiles/linux-gcc15-release \
  --build=missing \
  --build-test=missing
```

**What this does:**
- Detects profile at `.conan2/profiles/linux-gcc15-release`
- Builds/tests core, consumer, and producer packages
- Stores packages in local `.conan2` cache
- Exits with status 0 on success

### Building Individual Packages

If you only need specific packages:

```bash
# Build core only
conan create core \
  --profile:host=.conan2/profiles/linux-gcc15-release \
  --profile:build=.conan2/profiles/linux-gcc15-release \
  --build=missing

# Build consumer (automatically pulls core dependency)
conan create consumer \
  --profile:host=.conan2/profiles/linux-gcc15-release \
  --profile:build=.conan2/profiles/linux-gcc15-release \
  --build=missing

# Build producer (automatically pulls core dependency)
conan create producer \
  --profile:host=.conan2/profiles/linux-gcc15-release \
  --profile:build=.conan2/profiles/linux-gcc15-release \
  --build=missing
```

---

## API Usage

### Core Package: SPSC Channels

Shared-memory single-producer-single-consumer queue:

```cpp
import coderoast.ipc.core;

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

### Consumer Package: Causal Frame Stream

The recommended consumer entry point is `CausalShmConsumer<Frame>` — a
pull-based, threadless pipeline that drains every shard's SPSC ring on
demand, k-way merges by `CausalKey`, and applies a control-frame filter:

```cpp
import coderoast.ipc.core;      // frame/channel types, BackpressurePolicy, WaitStrategy
import coderoast.ipc.consumer;

using namespace coderoast::ipc::consumer;

CausalShmConsumer<> consumer{CausalShmConsumer<>::Config{
    .channel        = "myapp.pipeline",
    .shard_count    = 4,
    .backpressure   = coderoast::ipc::BackpressurePolicy::Block,
    .wait_strategy  = coderoast::ipc::WaitStrategy::Adaptive,
    // emit_control_frames = false: WindowSeal/EOS absorbed by the drainer
}};

coderoast::ipc::DefaultLineFrame frame{};
while (!consumer.all_shards_done()) {
    if (consumer.try_next(frame)) {
        // frame.payload is valid until the next try_next() call
    }
}
```

For callers that need per-window barriers — "tell me when window K has been
sealed by every shard" — wrap the consumer in `WindowClosedConsumer<Frame>`
(see the section below). The legacy `SharedMemorySource<Frame>` adapter
(transport-sequence ordering, `try_pop(payload)` shape) still exists for
backwards compatibility but new callers should prefer `CausalShmConsumer`.

**Key features:**
- Threadless pull pipeline: `ShmTransportDrainer` -> `CausalReorderBuffer` -> `FrameEmitter`
- Deterministic byte-identical replay via `CausalKey = (logical_tick, agent_order, intra_agent_index, shard_id)`
- Zero-copy: frame payload is a view into the consumer's internal buffer, valid until the next `try_next()`
- Shard-aware: handles multi-shard producers with frontier gating until every shard has produced or EOS'd
- EOS is absorbed internally; `all_shards_done()` flips true once every shard has EOS'd and every heap is empty

`header.sequence` remains a transport sequence, not a deterministic
simulation order, and is used internally only for gap detection on a single
shard. Deterministic cross-run replay relies on the logical merge key
`(logical_tick, agent_order, intra_agent_index, shard_id)` carried in every
frame header.

### Producer Package: Frame Builder

Helper for constructing and sequencing frames:

```cpp
import coderoast.ipc.core;      // the frame header types
import coderoast.ipc.producer;

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
    /*agent_id_hash=*/agent_id
);

// Frame has auto-incremented transport + per-shard sequences
std::cout << "Transport seq: " << frame.header.sequence << "\n";
std::cout << "Shard seq: " << frame.header.shard_sequence << "\n";
```

**Key Features:**
- Auto-incrementing transport sequence and per-shard sequence
- Stable agent ID hashing (FNV-1a)
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
  --profile:host=.conan2/profiles/linux-gcc15-release \
  --profile:build=.conan2/profiles/linux-gcc15-release \
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
clang-tidy -p build core/**/*.{cppm,cpp} \
  --header-filter="^(?!.*\.conan2|.*build).*$" \
  --config-file=.clang-tidy

# Format (clang-format)
clang-format -i core/**/*.{cppm,cpp} consumer/**/*.{cppm,cpp} producer/**/*.{cppm,cpp}
```

In the workspace, `malf lint coderoast-ipc` runs the same checks under the pinned toolchain.

---

## Publishing & Stable Cache

### Exporting to Shared Stable Cache

After bumping version or changing ABI, export to the shared cache where downstream repos resolve dependencies:

```bash
# Export all three packages to /opt/coderoast/conan-stable
CONAN_HOME=/opt/coderoast/conan-stable conan create . \
  --profile:host=linux-gcc15-release \
  --profile:build=linux-gcc15-release \
  --build=missing
```

This makes packages available to:
- `logcraft` (consumes `coderoast_ipc_core`)
- `insight-eidos` (consumes `coderoast_ipc_consumer`)
- Any other downstream package

### GitHub Release Workflow

Tags use `vX.Y.Z` semver format. GitHub Actions automatically:

1. Verify `conanfile.py` version matches tag
2. Build all three packages
3. Export Conan cache tarball (`coderoast_ipc-X.Y.Z.tgz`)
4. Attach tarball to GitHub release

Consumers restore with:
```bash
conan cache restore coderoast_ipc-X.Y.Z.tgz
```

---

## Requirements

- **OS:** Linux (macOS/Windows support possible with POSIX shim)
- **C++ Standard:** C++23 with modules + `import std` (compilers: GCC 15 / libstdc++, Clang 21 / libc++)
- **Build System:** CMake with `CMakeConfigDeps` + Ninja (a recent CMake with C++20-module + `import std` support)
- **Package Manager:** Conan 2.x
- **Runtime Dependencies:** None (small static library, no third-party runtime deps)
- **Test Dependencies:** GTest (test-only), Google Benchmark (benchmark-only)

---

## Troubleshooting

### Profile not found

If you get `profile 'linux-gcc15-release' does not exist`, ensure it's created:

```bash
conan profile detect --force
# Then copy/customize as needed to .conan2/profiles/linux-gcc15-release
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
    LineFrameFlags flags;        // Truncated, EndOfStream, WindowSeal
    uint16_t reserved;           // Explicit tail padding (no implicit holes — see below)
};

template <size_t MaxPayload>
struct LineFrame {
    LineFrameHeader header;
    array<byte, MaxPayload> payload;
};
```

The header carries **no format tag**, by rule (ADR 0029 D2): *the transport carries exactly the facts
the bytes cannot carry, and nothing they can.* The format is intrinsic — the payload bytes carry it and
the consumer recovers it from them, exactly as it must for a real log that arrives with no frame header.
A `FrameFormat format` field did ride here until 1.8.1; it had three writers and zero readers, and was
erased as a latent cheat channel rather than left one `read` away from letting a generator hand the
consumer an answer production never supplies.

`reserved` is sized so the members tile the struct exactly: an IPC header is `memcpy`'d across a process
boundary, and implicit padding would put indeterminate bytes on the wire. A `static_assert` on
`has_unique_object_representations_v` holds the property, so a future field that opens a hole fails the
build.

ABI version constants ensure compatibility:
- `kIpcAbiVersion = 3`
- `kSharedChannelAbiVersion = 3`

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