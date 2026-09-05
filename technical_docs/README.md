# coderoast-ipc — technical documentation index

The as-built API and the determinism contract of this transport live in
[../README.md](../README.md); the decision-and-why layer lives in the superproject's ADR series
(`ADR-11` owns the shared-memory bus, its causal order and its carve-out boundary; `ADR-22` owns
the IntentChannel a ring transports; `ADR-3.D4` owns the named-module and `import std` posture).
This shelf holds only what neither of those carries: the records this repo produced about itself.

## Read Order

1. [operations/ccc_migration.md](operations/ccc_migration.md) - the Code & Comment as Contract
   migration ledger: unit by unit, the claims each deleted comment carried, the cold-reader
   interrogation that tested whether the code alone still carries them, where every unrecovered
   claim was re-homed, and the findings the run handed to other lanes.

## Navigation

| Where | What |
|---|---|
| [../README.md](../README.md) | the as-built API, the frame ABI, the determinism contract |
| [../CLAUDE.md](../CLAUDE.md) | the repo's arrival notes and standing traps |
| [../../technical_docs/README.md](../../technical_docs/README.md) | CodeRoast parent docs: strategy, cross-repo architecture, ADRs, operations |
