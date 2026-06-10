// coderoast.ipc.consumer.test — shared test infrastructure (§11.9.11, the logcraft.test
// pattern). Test TUs import this instead of spelling out the import block. ipc.consumer is a
// SINGLE module (the §11.9.11 sizing rule; see the package CMake note); the aggregate
// re-exports std + the facade + the upstream core module the tests exercise.
export module coderoast.ipc.consumer.test;
export import std;
export import coderoast.ipc.consumer;
export import coderoast.ipc.core;
