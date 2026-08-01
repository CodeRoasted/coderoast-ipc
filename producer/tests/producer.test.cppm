// coderoast.ipc.producer.test — shared test infrastructure, on the logcraft.test
// pattern. Test TUs import this instead of spelling out the import block. ipc.producer is a
// SINGLE module (nothing sealed, no partition earns its keep; see the package CMake note); the
// re-exports std + the facade + the upstream core module the tests exercise.
export module coderoast.ipc.producer.test;
export import std;
export import coderoast.ipc.producer;
export import coderoast.ipc.core;
