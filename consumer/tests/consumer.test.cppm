// refs: ADR-3.D4, F-SRC-coderoast-ipc:consumer/CMakeLists.txt
// invariant: the package is one module with nothing sealed, so this aggregate re-exports the whole
// facade and no partition is declared.
export module coderoast.ipc.consumer.test;
export import std;
export import coderoast.ipc.consumer;
export import coderoast.ipc.core;
