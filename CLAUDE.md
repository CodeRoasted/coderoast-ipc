# coderoast-ipc — shared-memory transport (frame ABI + SPSC channels)

The transport link of the deterministic pipeline: SPSC shared-memory channels,
frame types, ABI constants, and producer/consumer adapters. Three packages
versioned together (`packages.yml`): `coderoast_ipc_core` / `_producer` /
`_consumer`, each a pure C++ named module. A fourth, test-only package,
`coderoast_ipc_namespace_tests` (`namespace_tests/`), holds the core's segment
tests that mount a private tmpfs in a fresh namespace: never released, declared
`unconfined:` because a Landlock domain forbids the mount (`DN-142.D29` (ix)). A
test that mounts belongs there, never in a confined package.

## Arrival

- Build/test: `malf build` / `malf test` at the repo root, or per package dir
  (`core/`, `producer/`, `consumer/`).
- The determinism contract — raw transport deliberately racy, the consumer's
  causal merge restores bit-identity — is stated authoritatively in
  `README.md § Determinism`; the root CLAUDE.md carve-out is its digest.

## Constraints & traps

- Zero third-party runtime dependencies — keep it that way; this library must
  embed anywhere.
- The frame layout and ABI constants are a WIRE CONTRACT: `logcraft_core` on
  the producer side, insight-eidos's mcp/e2e on the consumer side. An ABI
  change cascades — fix all consumers in the same pass; the consumer list is the
  superproject's `technical_docs/compatibility_matrix.md` (present only in a
  full workspace checkout).
