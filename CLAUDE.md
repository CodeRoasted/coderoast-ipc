# coderoast-ipc — CLAUDE.md

See root CLAUDE.md for global rules.

## coderoast-ipc — Shared-memory IPC transport

- What: header-only C++23 SHM primitives — SPSC channels, frame types/ABI, and
  producer/consumer adapters. No runtime deps. Packaged via Conan as
  `coderoast_ipc_core` / `coderoast_ipc_consumer` / `coderoast_ipc_producer`
  (versioned together).
- Layout: `core/` (channels, frame, ABI constants), `producer/` (frame build +
  sequencing), `consumer/` (causal-ordered draining).

## Determinism contract (read before changing ordering)

- The **producer** guarantees per-shard *data content* and that records from a
  single producer keep their order. It does NOT guarantee global ordering.
- `header.sequence` (global atomic) and the relative arrival order of frames
  across shards are **race-prone by design** — never assert on them.
- The **consumer** restores determinism via causal order:
  CausalKey = (logical_tick → agent_order → intra_agent_index → shard_id),
  with a frontier/watermark gate. This causal stream is the bit-identical
  guarantee, not the transport-level `shard_sequence`.
- Per-window WindowSeal frames are emitted once per shard and their cross-shard
  order is intentionally non-deterministic. Treat seals as a *set*, or use the
  `WindowClosedConsumer` adapter to coalesce them into one deterministic event.

## Docs: coderoast-ipc/README.md, technical_docs/compatibility_matrix.md
