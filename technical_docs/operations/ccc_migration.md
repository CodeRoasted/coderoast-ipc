# The Code & Comment as Contract migration — `coderoast-ipc`, the ledger

This file is the **evidence** of the migration of `coderoast-ipc` to the closed comment grammar:
for every unit converted, the claims its deleted comments carried, the cold-reader interrogation
that tested whether the code alone still carries them, and where every claim the code did not
carry was re-homed. It is a record, written as each unit lands; it decides nothing. The protocol
it follows is `ADR-26.D8`, its operator's order of steps is `OPS-8`, and the grammar is
`ADR-26.D5`; the gate that judges a converted unit is the second phase of `malf format --check`,
reading post-format text.

**Claim classes.** Every comment block of a unit was read before deletion and each claim it
carried was classed: **M** a mirror of the code beside it · **H** history or intention (*"this
used to"*, *"will"*) · **C** a contract (`pre` / `post` / `invariant` / `assert`) · **X** a
citation · **R** rationale — a why, a measurement, a rejected alternative, an ordering that is
content. M and H are deleted; C becomes tagged lines; X becomes `refs:`; **R is held** until a
fresh agent, reading the converted working tree only and never git, answers one neutral question
per R claim. *Recovered* means the prose was redundant and stays deleted. *Not recovered* or
*wrong* means the claim needs a home above the comment rung — a law block, a paragraph in an
owning doc, or a single `note:` — and never comes back as prose.

**Witnesses every unit carries.** Comment-only: the code token stream of each file, comments
removed and whitespace dropped, is byte-identical to `HEAD`'s. Grammar: `malf format --check
<unit>` reports zero would-be violations for the unit. Behaviour: `malf test coderoast-ipc` green
on clang-21 and on gcc-16 after the commit, against a green baseline taken before the first
conversion. A binary diff is not a witness — `__LINE__` legitimately changes when a comment is
deleted.

**Baseline, 2026-09-05, before any conversion (the gate's own count):** 18 files, 995 comment
lines, 959 would-be violations — bare 417, `///` 440, spacer 38, ruler 26, trailing 31, trailing
`NOLINT` 4, suppression without a why 3; tool forms already present 36. Behaviour baseline the
same day, both toolchains equal: `coderoast_ipc_core` 12 tests, `coderoast_ipc_consumer` 19,
`coderoast_ipc_producer` 5 — 36 of 36 passing on clang-21 and on gcc-16.2.

**Law numbering.** This repo declares **zero** law blocks. No disposition of either unit needed
one, and the workspace's numbering was mid-repair while this run was live, so a mint here would
have collided. The token itself is not spelled in this file: the registry lint reads a spelled
`D-LSRC-<digits>` anywhere as a declaration.

**Why two units and not one.** 995 comment lines fit inside `OPS-8.S2`'s ~1 500 budget, so the
repo could have been one unit. It was split source tier / test tier for two reasons. `OPS-8.S2`
orders source before tests because a test's `refs:` cites the slot the source unit names, and
here the tests' witnesses are contracts written in unit 1. And the questionnaire, not the line
count, is the binding constraint: the source tier alone produced 41 held claims, at the top of
what one cold reader answers in a single pass.

---

## Unit 1 — the source tier (5 files, 2 493 lines, 723 comment lines at HEAD)

`core/api/core.cppm` (the frame ABI, the SPSC channel, the adaptive wait), `core/src/core_impl.cpp`
(the POSIX syscall wrappers), `core/benchmarks/bench_ipc.cpp`, `producer/api/producer.cppm` (the
frame builder) and `consumer/api/consumer.cppm` (the three-stage causal pipeline and the
WindowClosed adapter). `consumer.cppm` alone carried 486 of the 723 comment lines, nearly all of
them `///` documentation blocks that the retired third slash makes violations in full.

| unit | files | comment lines HEAD → gate | forms written |
|---|---|---|---|
| source tier — `core/api`, `core/src`, `core/benchmarks`, `producer/api`, `consumer/api` | 5 | 723 → 163 | pre 3 · post 24 · invariant 44 · assert 6 · note 8 · refs 14 · 51 continuations · 13 tool |

The `refs:` lines address `ADR-3.D4` (named modules, the errno-in-module cascade), `ADR-11.D3`
(physical versus causal order, and the carve-out no reconciled surface may consult), `ADR-11.D4`
(backpressure is `Block | DropNewest`), `ADR-22.D1` (authorship is the discriminator),
`ADR-22.D4` (`Medium = DIALECT × IntentChannel`, channel-level and never per-frame), `ADR-22.D5`
(fail closed on depth when a declared coordinate is absent) and
`F-SRC-coderoast-ipc:core_impl.cpp`.

### The census (`OPS-8.S4`) — six suppressions, every decision measured

Taken over the five files before the strip and matched after: **`NOLINT` 6 → 6, `/*name=*/`
2 → 2, namespace closers 8 → 8, `clang-format off` 0, `wall-clock:` 0, `SPDX` 0. Zero
differences** — the stripper never deletes a suppression by construction. The decisions were
therefore not about recovery but about whether each directive has a reader at all, and each one
was **measured**: `malf lint --all-files` over the repo with all six removed (2 findings) against
the same run with them in place (0 findings), plus a third run over the converted tree
(0 findings).

| site | directive | measured | decision |
|---|---|---|---|
| `core.cppm` `LineFrameFlags` | trailing `NOLINT(performance-enum-size)` | clang-tidy errors without it | moved onto its own `NOLINTNEXTLINE` line under a `note:` naming the ABI reason |
| `core.cppm` `move_from` | `cppcoreguidelines-rvalue-reference-param-not-moved` | clang-tidy errors without it | kept; a `note:` added above it |
| `core.cppm` `SharedChannelHeader` | `clang-analyzer-optin.performance.Padding` | **silences nothing** — `-clang-analyzer-*` is disabled in the one shared `malf/config/.clang-tidy`, and this checker is opt-in on top of that | **deleted**; its trailing prose became the struct's `invariant:` |
| `core.cppm` `slot_ptr` ×2 | bare trailing `// NOLINT` | **silences nothing** — `-cppcoreguidelines-pro-bounds-pointer-arithmetic` is disabled in the same file | **deleted** |
| `consumer.cppm` `try_select` | `cppcoreguidelines-pro-type-const-cast` | did not fire, but the check is ARMED | **kept**, `note:` added. *Verified:* 0 findings. *Inferred:* the reason is that the `const_cast` sits in a class template the interface TU never instantiates, not a disabled check — so a downstream instantiating TU would red |

### The claims

| id | class | the claim, as the deleted comment stated it | disposition |
|---|---|---|---|
| M1–M40 | M | forty blocks restating a signature, an enumerator name, a `static_assert` message or the line below them — the `///` per-field docs of the five metrics structs, the per-enumerator `//<` glosses, `// Block policy.`, `// Fill the ring so the next push blocks.`, the four `// --- identity ---`-style section rulers of `SharedChannelHeader` | deleted |
| H1 | H | *"PURE named module (1.5.1 unwrap of the §8.1 wrapper)"*, in all three module headers, plus *"the former `api/coderoast/ipc/{frame,channel}.hpp` content now lives here"* and *"(ingest.hpp umbrella retired.)"* | deleted — intention and history |
| H2 | H | the bool overloads are *"shims for backwards compatibility"* used by *"legacy callers"* | deleted, and the framing is **false**: `push`, `try_push` and `try_pop` are live API, exercised by this repo's own README example, its benchmark and four of its tests. What survived is the `post:` naming what the bool loses |
| H3 | H | `test_channel_shutdown`'s *"EOS-as-state-transition contract that replaces the legacy in-band EOS frame"* (unit 2) | deleted |
| C1–C24 | C | the interface contracts of the channel (`close_graceful` idempotent and non-downgradable, `close_abort` terminal, `closing_at`'s meaning, the two notify paths, `create` refusing an over-long name, the destructor's producer-only transition) and of the pipeline (`try_pull` absorbing EOS and surfacing seals, `transport_complete`, `drained`, the frontier gate, the monotonicity check, the adapter's one-event-per-call rule) | `pre:` / `post:` / `invariant:` / `assert:` at the declarations |
| X1–X7 | X | ADR-3.D4 named at three sites, ADR-22 at five, the determinism carve-out named as *"CLAUDE.md § Determinism & Replay"* | `refs:`, with the carve-out repointed to `ADR-11.D3`, its owning slot |
| R1–R41 | R | the forty-one rationales listed in the interrogation below | held → Q1–Q41 |

### Stale or false claims found in the old prose, and deleted

Thirteen, every one re-derived at the artifact before the deletion:

1. `kIntentChannelNameCapacity`: *"A longer name is REFUSED at open"* — the refusal is in `create()`;
   `open()` performs no name check at all. The `create()` site's own comment said it correctly, so
   the file disagreed with itself. Corrected into the constant's `invariant:`.
2. `AdaptiveWait`: *"See the WaitStrategy documentation at the top of the file for the per-strategy
   progression."* **There is no such documentation anywhere in the file** — `WaitStrategy` is a bare
   five-enumerator enum with no comment. A dangling cross-reference; the progression is the `switch`.
3. `CausalReorderBuffer::refill`: *"it never reaches into SHM."* It does — `refill` calls `try_pull`,
   which is a `try_pop_status` straight onto the shard's mapped ring, as the drainer's own prose said
   one screen earlier. Corrected into `refill`'s `post:`.
4. `FrameEmitter`: *"Counters are best-effort relaxed atomics."* `emitted_`, `control_dropped_` and
   `last_sequence_` are plain `std::uint64_t`. Corrected into `EmitterMetrics`' `invariant:`, which
   now states the single-owner-thread reason they may be plain.
5. `CausalShmConsumer`'s facade block: *"a `ConsumerMetrics` snapshot aggregating the three
   sub-stages' atomic counters"* — one of the three is not atomic. Same correction.
6. `CausalShmConsumer::set_observer`: *"Register the same observer with every sub-stage."* It
   registers with two of three; `FrameEmitter` has no `set_observer` and raises no event. Corrected
   into the method's `post:`.
7. `WindowClosedConsumer`: *"the internal seal-counter map grows at most
   `concurrent_in_flight_windows` entries."* **That identifier exists nowhere in the workspace** — a
   phantom bound. The real invariant (an entry lives only while a window is partially sealed) is now
   written; the degenerate case is a finding below.
8. `WindowClosedConsumer`: *"see `default_insight_pipeline_config()` and `pyramid.window_ns`."* The
   first exists in coderoast-server; **`pyramid.window_ns` exists nowhere**. The 25 s figure is right
   (`InsightConsumerState::kWindowDuration`). The whole pairing paragraph is already owned by this
   repo's `README.md`, so it was deleted rather than repointed.
9. `causal_less`: *"the pull-based pipeline and the legacy iterator must produce the SAME frame
   order."* No iterator exists in this repo; the only surviving mention of one is a comment in
   `coderoast-server/insight-mcp/src/main.cpp`. The durable half became an `invariant:`.
10. `core.cppm`'s file header named the implementation unit `coderoast_ipc_core_impl.cpp`. The file
    is `core/src/core_impl.cpp`. Replaced by the checkable `F-SRC-coderoast-ipc:core_impl.cpp`.
11. `detail`'s block ended *"Do NOT let clang-tidy `misc-use-anonymous-namespace` revert this."*
    `misc-*` is not enabled in `malf/config/.clang-tidy` — that file's own note says so — so the
    warning names a check that does not run here.
12. `bench_ipc.cpp`: *"Custom entry point (was BENCHMARK_MAIN()) so the run disables ASLR first."*
    At the pinned `benchmark/1.9.5`, `BENCHMARK_MAIN()` **already** calls
    `benchmark::MaybeReenterWithoutASLR` as its first statement, so the hand-written `main` buys
    nothing over the macro. Found by the cold reader, in the pinned header. See the findings below.
13. The bool-overload framing (H2 above).

### Interrogation

One fresh agent, 41 questions, 50 tool uses, 162 k tokens, 13.5 minutes, **no git command**, the
ledger and every build directory forbidden. **41 of 41 recovered** — 37 at high confidence; Q5
high on the fact and low on an intent the tree never states; Q30, Q39 and Q41 high or high/medium
on the documented design and **medium on corollaries the code does not document, each of which is
a finding below**.

| Q | claim | verdict | what the reader found, and where |
|---|---|---|---|
| Q1 | the one-definition channel-name contract | **recovered**, high | named the silent failure the prose only implied: a stale or foreign segment whose header passes `validate_header` because magic, ABI, `slot_size` and `slot_count` all match, so the consumer reads the wrong stream |
| Q2 | the transport carries only what the bytes cannot | **recovered**, high | `ADR-22.D1`/`D4`, `ADR-11.D2`, and this repo's own `README.md § Frame Format & ABI`, which already records that a `FrameFormat` field rode the header until 1.8.1 with three writers and zero readers |
| Q3 | `uint16_t` for the ABI | **recovered**, high | re-derived the arithmetic unaided: 6 × `uint64_t` + 5 × `uint32_t` = 68, plus 2 + 2 lands on 72, an exact multiple of the 8-byte alignment |
| Q4 | explicit tail padding | **recovered**, high | and bounded it: `validate_header` catches a size change through `slot_size != sizeof(Frame)`, but **a same-size field reshuffle is caught by nothing but an ABI bump** |
| Q5 | the magic's encoding | **recovered** on the bytes | decoded `CRIPCSPS` from the literal with no help; explicitly declined to claim the expansion, which the tree indeed never states |
| Q6 | refused, never truncated | **recovered**, high | and at `create()`, on the producer side — the coordinate the old prose had wrong |
| Q7 | named, non-export namespace | **recovered**, high | found `ADR-3.D4` rule 4 stating it flatly, a stronger home than the site's own `invariant:` |
| Q8 | the primitives-only boundary | **recovered**, high | read it off the signatures: `shm_fstat_size` returns `std::size_t` and not `off_t`, `struct stat` never appears |
| Q9 | the cache-line split | **recovered**, high | derived the rule — every concurrently mutated atomic gets a line, the write-once identity block does not — and cited `static_assert(alignof(SharedChannelHeader) >= kCacheLineBytes)` |
| Q10 | channel-level, never per-frame | **recovered**, high | `ADR-22.D4` |
| Q11 | the null-header wait | **recovered**, high | only `AdaptivePark` changes; it degrades to what `ParkOnly` always does |
| Q12 | `parker_count` | **recovered**, high | including why it must be a live count and not a flag: parkers come and go, and both ends can park from different processes |
| Q13 | the producer-only close transition | **recovered**, high | added the consequence the prose never drew: a consumer handle closing would fabricate an EOS the drainer latches, reporting the transport complete while the producer still writes |
| Q14 | what the bool loses | **recovered**, high | and that under `DropNewest`, `Full` also means *dropped and counted* |
| Q15 | the two notify paths | **recovered**, high | the asymmetry argument the prose lacked: a missed progress wake is repaired by the next progress event, a missed state change has no next event |
| Q16 | `move_from` | **recovered**, high | |
| Q17 | `kIpcAbiVersion` | **recovered — "Nowhere."** | independently re-ran the workspace sweep and found the declaration plus one README line, nothing else. See the findings |
| Q18 | the atomic/plain asymmetry | **recovered**, high | **and sharper than the line the conversion wrote**: the single-writer requirement binds on the normalized slot `shard_id % shard_count_`, not on the raw argument |
| Q19 | no format stamped | **recovered**, high | |
| Q20 | `sequence` is not an ordering key | **recovered**, high | `ADR-11.D3` |
| Q21 | FNV-1a over `std::hash` | **recovered**, high | with the argument the prose never made: `std::hash` is implementation-defined, differs between libstdc++ and libc++ — both shipped here — and may be per-process salted |
| Q22 | the GMF split | **recovered**, high | `ADR-3.D4` rules 2 and 6 |
| Q23 | the ASLR entry point | **recovered — and the prose was OBSOLETE** | read the pinned `benchmark/1.9.5` header: `BENCHMARK_MAIN()` already calls `MaybeReenterWithoutASLR`, so the hand-written `main` is that macro minus its `argv == nullptr` fallback. It also caught a stale CMake comment claiming the TU carries `BENCHMARK_MAIN()` |
| Q24 | the textual `<unistd.h>` | **recovered**, high | and why it costs nothing here and would in a module unit |
| Q25 | `sequence` out of the key | **recovered**, high | with a second reason the prose never gave: `sequence` is unique, so consulting it would make every comparison resolve on it and `check_causal_monotonicity` could never see the tie that reveals a broken premise |
| Q26 | `shard_id` is a reachable tie-break | **recovered**, high | verified at logcraft's `emit_control_frame`, which builds a seal with `agent_id_hash = 0` and sets neither ordering field |
| Q27 | the tick fallback belongs in the key | **recovered**, high | named the three consumers that would otherwise disagree |
| Q28 | the double unlink | **recovered**, high | |
| Q29 | EOS absorbed, seal surfaced | **recovered**, high | |
| Q30 | the heaps are uncapped by design | **recovered** on the design, medium on a corollary | `ADR-11.D4` and the site's `invariant:`; **and it showed the intended bound does not hold** — see the findings |
| Q31 | the frontier condition | **recovered**, high | stated the three-way conjunction exactly as the code has it |
| Q32 | the play-to-target freeze | **recovered**, high | found logcraft's `time_control_model.md` and the named producer-side gate |
| Q33 | what one `refill` does | **recovered**, high | answered *yes it touches shared memory* and enumerated the acquire loads, the `memcpy` and the release store — the half the old prose denied |
| Q34 | terminate rather than log | **recovered**, high | with a richer taxonomy of upstream causes than the prose carried |
| Q35 | the emitter's counters off-thread | **recovered**, high | *no* — and traced the consequence out to `CausalShmConsumer::emitted()`, `last_sequence()` and `metrics()` |
| Q36 | which sub-stages hold the observer | **recovered**, high | two of three |
| Q37 | non-movable | **recovered**, high | |
| Q38 | forced control frames | **recovered**, high | and that `underlying()` is not a substitute, because it is the adapter's `try_next` that consumes the seals |
| Q39 | the seal-counter bound | **recovered** on the mechanism, medium on a degenerate case | the entry is erased at the N-th seal and nothing else bounds the map — see the findings |
| Q40 | allocation on the data path | **recovered**, high | **and bounded it**: the adapter and the emitter allocate nothing, but the same call drives `refill`, whose `priority_queue::push` reallocates when a heap grows |
| Q41 | replay-identical `WindowClosed` events | **recovered**, medium | reasoned it through the total order and confirmed `seal_counts_` is keyed lookup only, so no bucket order can leak — and found a gap in the frontier's release test, see the findings |

### Conversion faults caught before the commit

Five lines the conversion itself wrote were wrong or imprecise. Three were caught by the reader's
own evidence and two by the converter's re-read; all five were corrected in the tree before the
commit, and the unit was re-formatted, re-gated and re-witnessed afterwards.

1. `notify_progress`'s `post:` said *"one relaxed load when nobody is parked"*, carried straight
   from the old prose. The load is `std::memory_order_acquire`. The reader's Q12 answer says
   *acquire* on its own reading of the code.
2. `FrameBuilder::build`'s `pre:` said *"one caller thread per shard_id"*. The reader's Q18 answer
   is sharper and correct: the requirement binds on `shard_id % shard_count_`, so two distinct
   arguments that alias one slot are the same single-writer constraint.
3. `CausalReorderBuffer`'s `invariant:` said the heaps are uncapped because *"backpressure is the
   producer's policy"*, which implies the producer's policy bounds the memory. The reader's Q30
   answer shows it does not: the ring's `slot_count` bounds the **ring**, and `refill` drains the
   ring wholesale into a heap. The line now says exactly that.
4. `all_shards_done`'s `post:` said `kDrainComplete` *"fires once here"*; it fires in `try_select`,
   on the same condition. Caught by the converter's re-read.
5. The `shm_*` block's `post:` read *"an fd helper returns..."* while attaching, for
   `contract-gen`, to `shm_open_create` alone. Reworded to name both. Converter's re-read.

### Dispositions

**Nothing re-homed.** Every one of the 41 held claims is carried by the converted code, by its
callees, by the repo's own `README.md`, or by the ADR slot a `refs:` now names. Two claims the
recovery rested on are prose in other repos and are recorded here as owed rather than fixed:
logcraft's `emit_control_frame` owes an `invariant:` stating that a seal carries no agent
identity, which is what makes `shard_id` reachable (Q26); and `core/CMakeLists.txt`'s bench note
owes a repair (Q23, below).

### Findings, outside this comment-only migration

1. **`kIpcAbiVersion` has no reader** (`core/api/core.cppm`). It is declared `3U`, exported, and
   documented at `README.md:400`; `validate_header` checks `kSharedChannelAbiVersion` (`5U`)
   instead. A workspace-wide allowlist sweep returns exactly those two sites, and the cold reader
   reached the same verdict independently (Q17). *Justification search:* `ADR-11.D1` requires a
   shared-header layout change to bump **the** ABI version, and the one `validate_header` enforces
   is `kSharedChannelAbiVersion`; nothing in the ADRs, the studies, the registers or the memory
   store claims a second token. Removing it is a public-API change with a README cascade —
   **Hephaïstos**, with **Eqya** on whether the README's ABI section should describe one version.
2. **The per-shard heaps have no bound once the frontier blocks** (Q30). The `invariant:` says
   capping them would drop frames silently, which is right; but `refill` drains a whole ring into a
   heap, so the ring only fills — and backpressure only bites — when the caller stops calling
   `try_select`. A frontier blocked on one lagging shard while the others keep producing grows the
   heaps with nothing capping them anywhere. `ADR-26.D3` forbids unbounded allocation growth, so
   this is either a declared exception that needs stating or a defect — **Daidalos**, then
   **Hephaïstos**.
3. **`WindowClosedConsumer::seal_counts_` can leak an entry forever** (Q39). The entry is erased
   only when a window's N-th seal lands. A shard that reaches EOS or dies while the others keep
   sealing, or a `shard_count_` larger than the number of shards actually sealing, leaves an entry
   that is never removed and the map grows for the life of the object. No test covers it —
   **Hephaïstos** for the reclamation, **Kleio** for the case.
4. **The frontier's release test admits a same-tick overtake** (Q41). A shard blocks only when its
   `watermark_tick < best_tick`; a shard whose watermark **equals** the candidate's tick does not
   block, yet it could still deliver a frame at that same tick with a lower `agent_order`. It does
   not diverge silently — `check_causal_monotonicity` sees the inversion and terminates — but a
   deterministic-replay product turning a reachable ordering hole into an abort is a design call,
   not an implementation detail. **Daidalos**.
5. **`validate_header` cannot see a same-size field reshuffle** (Q4). It compares magic, ABI
   version, `slot_size` and `slot_count`; a header whose fields are reordered without changing
   `sizeof` passes, and only the ABI-version discipline covers it. That discipline is `ADR-11.D1`'s
   and is stated nowhere in this repo. **Daidalos** for the statement, **Argos** for whether the
   release gate should assert it.
6. **`bench_ipc.cpp`'s hand-written `main` is `BENCHMARK_MAIN()` minus a fallback** (Q23). At the
   pinned `benchmark/1.9.5` the macro already calls `MaybeReenterWithoutASLR` first; the local
   `main` reproduces its body but drops the `argv == nullptr` guard. And `core/CMakeLists.txt`'s
   bench note still asserts *"The TU carries `BENCHMARK_MAIN()`"*, which is not true today.
   **Hephaïstos**, for both halves in one pass.

### Witnesses

Comment-only: the code token stream of all five files is identical to `HEAD`'s. Grammar:
`malf format --check` over the five directories — 163 comment lines, **0 would-be violations**.
Behaviour: `malf test coderoast-ipc` on clang-21 and on gcc-16.2, 12 / 19 / 5 each, equal to the
baseline. Lint: `malf lint --all-files` over the repo, **0 findings** after the conversion, the
same verdict as before it. Count: **723 comment lines at HEAD to 163 as the gate counts them, and
713 would-be violations to 0.**
