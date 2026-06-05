// NOLINTBEGIN Module-import proof: allow short identifiers / test patterns.
// test_ipc_core_module_import.cpp
//
// In-repo proof that the `coderoast.ipc.core` named module
// (modules/coderoast_ipc_core.cppm) imports cleanly and re-exports the public
// frame/channel surface, under BOTH toolchains (gcc-15/libstdc++ and
// clang-21/libc++). coderoast-ipc is header-only, so this proves the §8.1 wrapper
// compiles + exposes its surface through the module boundary
// (cxx_modules_migration_contract §10.15).
//
// §8.1 BUILD GOTCHA: textual std-pulling includes (gtest) MUST precede the
// `import`, or GCC reports std redefinitions (GMF-std vs textual-std clash).

#include <gtest/gtest.h> // textual std-pulling include FIRST

#include <cstddef>
#include <cstdint>

import coderoast.ipc.core; // ...then the module under test

TEST(IpcCoreModuleImport, FrameAndChannelSurfaceResolveThroughModule)
{
    EXPECT_EQ(coderoast::ipc::kIpcAbiVersion, 2U);
    EXPECT_EQ(coderoast::ipc::kDefaultLineFramePayloadBytes, std::size_t{4096});
    EXPECT_EQ(coderoast::ipc::kSharedChannelMagic, 0x4352495043535053ULL); // "CRIPCSPS"
    EXPECT_EQ(coderoast::ipc::kDefaultSharedChannelSlotCount, std::size_t{8192});

    // the frame type re-exports and is nameable/usable through the module boundary
    EXPECT_GE(sizeof(coderoast::ipc::DefaultLineFrame),
              coderoast::ipc::kDefaultLineFramePayloadBytes);

    // the SPSC channel template re-exports (named here without instantiation)
    using Channel = coderoast::ipc::SharedMemorySpscChannel<coderoast::ipc::DefaultLineFrame>;
    Channel* channel_ptr{nullptr};
    EXPECT_EQ(channel_ptr, nullptr);
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

// NOLINTEND
