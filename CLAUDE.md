# coderoast-ipc — shared-memory transport (frame ABI + SPSC channels)

The transport link of the deterministic pipeline: SPSC shared-memory channels,
frame types, ABI constants, and producer/consumer adapters. Three packages
versioned together (`packages.yml`): `coderoast_ipc_core` / `_producer` /
`_consumer`, each a pure C++ named module.

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
  change cascades — fix all consumers in the same pass; the consumer list is in
  `../technical_docs/compatibility_matrix.md`.
