// coderoast.ipc.core.test — shared test infrastructure (ADR-3.D4, the logcraft.test pattern).
// Test TUs import this instead of spelling out the import block. ipc.core is a SINGLE module
// (the ADR-3.D4 sizing rule — one cohesive SHM-channel concern, nothing sealed; see the
// package CMake note), so the aggregate re-exports std + the facade and nothing else.
export module coderoast.ipc.core.test;
export import std;
export import coderoast.ipc.core;
