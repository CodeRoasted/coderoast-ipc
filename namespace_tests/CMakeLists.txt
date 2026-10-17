cmake_minimum_required(VERSION 3.28)

# The tests `import std`, so CMake's experimental import-std support is required; the gate UUID MUST
# be set before project() (451f2fe2-… = CMake 4.3.1's CMAKE_EXPERIMENTAL_CXX_IMPORT_STD).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")

project(coderoast_ipc_namespace_tests LANGUAGES CXX)

# Warnings on for every first-party translation unit (Founder, 2026-09-10) — reported, not promoted.
add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wall;-Wextra>"
                    "$<$<COMPILE_LANG_AND_ID:CXX,GNU>:-Wno-missing-field-initializers>"
                    "$<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wno-missing-designated-field-initializers>"
                    "$<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/W4>")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# DN-142.D29 (ix): the core's mount-namespace segment arms, built against the STORED core, whose
# config defines the test support functions (tests/support/ of coderoast_ipc_core, installed beside
# it). The package is declared `unconfined:` in packages.yml, because a Landlock domain forbids the
# private tmpfs mount every arm here needs.
#
# The test switch defaults OFF when BUILD_TESTING is defined false, which conan does under
# tools.build:skip_test (DN-142.D5): a package with nothing but tests then configures nothing.
set(CODEROAST_IPC_NAMESPACE_TESTS_DEFAULT ON)
if(DEFINED BUILD_TESTING AND NOT BUILD_TESTING)
    set(CODEROAST_IPC_NAMESPACE_TESTS_DEFAULT OFF)
endif()
option(CODEROAST_IPC_NAMESPACE_BUILD_TESTS "Build the namespace segment tests" ${CODEROAST_IPC_NAMESPACE_TESTS_DEFAULT})

if(CODEROAST_IPC_NAMESPACE_BUILD_TESTS)
    find_package(coderoast_ipc_core REQUIRED CONFIG)
    find_package(GTest REQUIRED CONFIG)
    include(CTest)
    enable_testing()

    add_executable(coderoast_ipc_namespace_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_segment_namespace.cpp)
    target_link_libraries(coderoast_ipc_namespace_tests PRIVATE
        coderoast::ipc::core
        GTest::gtest
        GTest::gtest_main
    )
    target_compile_features(coderoast_ipc_namespace_tests PRIVATE cxx_std_23)
    set_target_properties(coderoast_ipc_namespace_tests PROPERTIES
        CXX_SCAN_FOR_MODULES ON CXX_MODULE_STD ON)
    # DN-102.D2: the arms create shared-memory segments, so the binary reaps orphans at its start,
    # and the executable-start arm asserts on that very reap.
    coderoast_ipc_reap_orphaned_segments_at_start(coderoast_ipc_namespace_tests)
    coderoast_ipc_private_pid_namespace(coderoast_ipc_namespace_tests)
    include(GoogleTest)
    # A googletest skip exits 0, so without this property ctest counts a skipped case as PASSED;
    # with it the case is listed under "The following tests did not run", where malf reds an
    # undeclared one (DN-142.D23): a host refusing an unprivileged namespace reds this package.
    gtest_discover_tests(coderoast_ipc_namespace_tests
        PROPERTIES SKIP_REGULAR_EXPRESSION "\\[  SKIPPED \\]")
endif()
