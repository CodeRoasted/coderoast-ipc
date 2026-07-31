// coderoast.ipc.producer.test — shared test infrastructure (ADR-3.D4, the logcraft.test
// pattern). Test TUs import this instead of spelling out the import block. ipc.producer is a
// SINGLE module (the ADR-3.D4 sizing rule; see the package CMake note); the aggregate
// re-exports std + the facade + the upstream core module the tests exercise.
export module coderoast.ipc.producer.test;
export import std;
export import coderoast.ipc.producer;
export import coderoast.ipc.core;
