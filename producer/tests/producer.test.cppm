// refs: ADR-3.D4, F-SRC-coderoast-ipc:producer/CMakeLists.txt
// invariant: the package is one module with nothing sealed, so this aggregate re-exports the whole
// facade and no partition is declared.
export module coderoast.ipc.producer.test;
export import std;
export import coderoast.ipc.producer;
export import coderoast.ipc.core;
