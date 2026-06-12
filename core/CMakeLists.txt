cmake_minimum_required(VERSION 3.28)

# 1.5.1 unwrap (Argos to own/review topology): the pure module does `import std`, so CMake's
# experimental import-std support is required; the gate UUID MUST be set before project()/CXX-enable
# (451f2fe2-… = CMake 4.3.1's CMAKE_EXPERIMENTAL_CXX_IMPORT_STD, matching the conan tool-cmake).
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")

project(coderoast_ipc_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CODEROAST_IPC_CORE_BUILD_TESTS "Build coderoast_ipc_core unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CODEROAST_IPC_CORE_BUILD_BENCH "Build coderoast_ipc_core benchmarks" ${PROJECT_IS_TOP_LEVEL})

###########################################################
#  Sources
###########################################################
###########################################################
#  Module-bearing library (1.5.1 unwrap — header-only code now lives in the module interface;
#  the §8.1 wrapper + textual api/ headers are RETIRED). One target carries the file set.
#  1.5.2 §11.9.11 sizing ruling: DELIBERATELY a SINGLE module (no internal/api/detail layers) —
#  one cohesive SHM-channel concern, all-public surface, the only non-interface code being the
#  §11.9.9 textual POSIX impl unit. Nothing is sealed, so the unit tests stay CONSUMERS (they
#  double as the module-import proof — the reason the standalone proof target was retired).
###########################################################
add_library(coderoast_ipc_core STATIC)
target_sources(coderoast_ipc_core
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/api
        FILES    ${CMAKE_CURRENT_SOURCE_DIR}/api/core.cppm
)
# §11.9 cascade rule: the POSIX shm syscalls + their C macros (errno, O_*, PROT_*, MAP_*) cannot live in
# the import-std interface — they live in this TEXTUAL implementation unit (own GMF, no import std). It is
# an ordinary source attached to the module (`module coderoast.ipc.core;`), NOT a file-set interface.
target_sources(coderoast_ipc_core
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/core_impl.cpp
)
# EXPORT_NAME = core: with NAMESPACE coderoast::ipc:: the exported target is coderoast::ipc::core
# (under cmake_find_mode=none the NATIVE export name is the consumed one, §10.12 — must equal the alias).
set_target_properties(coderoast_ipc_core PROPERTIES EXPORT_NAME core
    CXX_SCAN_FOR_MODULES ON CXX_MODULE_STD ON)
target_compile_features(coderoast_ipc_core PUBLIC cxx_std_23)
add_library(coderoast::ipc::core ALIAS coderoast_ipc_core)
# 1.5.1: the separate _module target is retired (the module lives on coderoast_ipc_core itself).
# Transitional alias so existing consumers/tests referencing core_module still resolve.
add_library(coderoast::ipc::core_module ALIAS coderoast_ipc_core)

###########################################################
#  Install rules (used by `conan create`)
###########################################################
include(GNUInstallDirs)
# Ships the module file set only — no install(DIRECTORY api/) (textual headers deleted in the unwrap).
install(TARGETS coderoast_ipc_core
    EXPORT coderoast_ipc_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILE_SET cxx_modules DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/coderoast_ipc_core_modules
)
install(EXPORT coderoast_ipc_coreTargets
    NAMESPACE coderoast::ipc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc_core
)

###########################################################
#  Cross-package module shipping (Argos — §10.7/§10.9)
###########################################################
# CMakeConfigDeps does not yet emit FILE_SET CXX_MODULES (§10.6) → ship the
# package's OWN cmake config (carrying the file set), conan defers via
# cmake_find_mode=none. create → install(EXPORT); editable → BUILD-TREE export().
# core is header-only/std-only → the config has NO find_dependency() lines.
export(EXPORT coderoast_ipc_coreTargets
    NAMESPACE coderoast::ipc::
    FILE ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_coreTargets.cmake)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_core-config.cmake
"include(\"\${CMAKE_CURRENT_LIST_DIR}/coderoast_ipc_coreTargets.cmake\")
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_core-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc_core)

###########################################################
#  Tests (only when building the root project)
###########################################################
if(CODEROAST_IPC_CORE_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp"
    )
    if(TEST_SOURCES)
        include(CTest)
        find_package(GTest REQUIRED CONFIG)
        enable_testing()

        # CONSUMER test target (links the lib — doubles as the import proof; §11.9.7
        # members are pointless here: nothing is sealed). Test TUs import the
        # coderoast.ipc.core.test aggregate, compiled in-target. Test TUs carry their
        # own main() (InitGoogleTest) → link GTest::gtest, not gtest_main.
        add_executable(coderoast_ipc_core_tests ${TEST_SOURCES})
        target_sources(coderoast_ipc_core_tests
            PRIVATE
                FILE_SET cxx_modules TYPE CXX_MODULES
                BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tests
                FILES    ${CMAKE_CURRENT_SOURCE_DIR}/tests/core.test.cppm)
        target_link_libraries(coderoast_ipc_core_tests PRIVATE
                coderoast_ipc_core
                GTest::gtest
        )
        target_compile_features(coderoast_ipc_core_tests PRIVATE cxx_std_23)
        set_target_properties(coderoast_ipc_core_tests PROPERTIES
            CXX_SCAN_FOR_MODULES ON CXX_MODULE_STD ON)
        include(GoogleTest)
        gtest_discover_tests(coderoast_ipc_core_tests)

        # (The standalone module-import proof is retired: every unit test now `import`s the
        #  module via the aggregate — the import path is exercised by the whole suite.)
    endif()
endif()

###########################################################
#  Benchmarks (only when building the root project)
###########################################################
if(CODEROAST_IPC_CORE_BUILD_BENCH)
    file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp"
    )
    if(BENCH_SOURCES)
        find_package(benchmark REQUIRED CONFIG)

        # CONSUMER bench target; no coderoast.ipc.core.bench aggregate manufactured —
        # a single bench TU with a single import (§11.9.11 sizing rule). The TU
        # carries BENCHMARK_MAIN() → link benchmark::benchmark, not benchmark_main.
        add_executable(coderoast_ipc_core_bench ${BENCH_SOURCES})
        target_link_libraries(coderoast_ipc_core_bench PRIVATE
            coderoast_ipc_core
            benchmark::benchmark
        )
        target_compile_features(coderoast_ipc_core_bench PRIVATE cxx_std_23)
        set_target_properties(coderoast_ipc_core_bench PROPERTIES
            CXX_SCAN_FOR_MODULES ON CXX_MODULE_STD ON)
    endif()
endif()
