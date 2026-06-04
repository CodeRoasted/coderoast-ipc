cmake_minimum_required(VERSION 3.28)

project(coderoast_ipc_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CODEROAST_IPC_CORE_BUILD_TESTS "Build coderoast_ipc_core unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CODEROAST_IPC_CORE_BUILD_BENCH "Build coderoast_ipc_core benchmarks" ${PROJECT_IS_TOP_LEVEL})

###########################################################
#  Sources
###########################################################
file(GLOB_RECURSE API_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/api/*.hpp")

###########################################################
#  Header-only library target
###########################################################
add_library(coderoast_ipc_core INTERFACE ${API_HEADERS})
# EXPORT_NAME = core (not ipc_core): with NAMESPACE coderoast::ipc:: the exported
# target is coderoast::ipc::core, matching what consumers reference. Under
# cmake_find_mode=none the NATIVE export name is the consumed one (§10.12) — it
# must equal the alias, else find_package yields a target nobody links.
set_target_properties(coderoast_ipc_core PROPERTIES EXPORT_NAME core)
add_library(coderoast::ipc::core ALIAS coderoast_ipc_core)

target_include_directories(coderoast_ipc_core
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/api>
        $<INSTALL_INTERFACE:include>
)

###########################################################
#  Named-module wrapper (coderoast.ipc.core) — §8.1 (1.5.0)
###########################################################
# The library is INTERFACE (header-only), so the module wrapper is the package's
# ONLY compiled artifact — a minimal .cppm.o that re-exports the global-module
# public surface (frame/channel/SPSC). cxx_std_23 PUBLIC is the §10.4 producer
# gotcha (else "no C++ standard found"); link the lib PUBLIC for the api/ includes.
add_library(coderoast_ipc_core_module STATIC)
target_sources(coderoast_ipc_core_module
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules
        FILES    ${CMAKE_CURRENT_SOURCE_DIR}/modules/coderoast_ipc_core.cppm
)
target_link_libraries(coderoast_ipc_core_module PUBLIC coderoast_ipc_core)
target_compile_features(coderoast_ipc_core_module PUBLIC cxx_std_23)
add_library(coderoast::ipc::core_module ALIAS coderoast_ipc_core_module)

###########################################################
#  Install rules (used by `conan create`)
###########################################################
include(GNUInstallDirs)
# Library + module wrapper in ONE export set → a single file-set owner
# (§10.5/§10.8 diamond ODR-safety). Only the module target carries cxx_modules.
install(TARGETS coderoast_ipc_core coderoast_ipc_core_module
    EXPORT coderoast_ipc_coreTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILE_SET cxx_modules DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/coderoast_ipc_core_modules
)
install(DIRECTORY api/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
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
        enable_testing()
        
        # Try to load gtest paths from conanfile.py injection
        if(EXISTS "${CMAKE_BINARY_DIR}/gtest_paths.cmake")
            include("${CMAKE_BINARY_DIR}/gtest_paths.cmake")
        endif()
        
        # If paths still not set, search in Conan cache
        if(NOT GTEST_INCLUDE_DIR)
            file(GLOB_RECURSE GTEST_HEADER "${CMAKE_BINARY_DIR}/../../../.conan2/**/gtest/gtest.h")
            if(GTEST_HEADER)
                list(GET GTEST_HEADER 0 GTEST_HEADER_PATH)
                get_filename_component(GTEST_INCLUDE_DIR "${GTEST_HEADER_PATH}" DIRECTORY)
                get_filename_component(GTEST_INCLUDE_DIR "${GTEST_INCLUDE_DIR}" DIRECTORY)
            endif()
        endif()
        
        if(NOT GTEST_LIB_DIR)
            file(GLOB_RECURSE GTEST_LIB "${CMAKE_BINARY_DIR}/../../../.conan2/**/libgtest.a")
            if(GTEST_LIB)
                list(GET GTEST_LIB 0 GTEST_LIB_PATH)
                get_filename_component(GTEST_LIB_DIR "${GTEST_LIB_PATH}" DIRECTORY)
            endif()
        endif()
        
        if(GTEST_INCLUDE_DIR AND GTEST_LIB_DIR)
            message(STATUS "Found gtest: INCLUDE=${GTEST_INCLUDE_DIR}, LIB=${GTEST_LIB_DIR}")
            
            add_executable(coderoast_ipc_core_tests ${TEST_SOURCES})
            target_include_directories(coderoast_ipc_core_tests PRIVATE "${GTEST_INCLUDE_DIR}")
            target_link_libraries(coderoast_ipc_core_tests PRIVATE
                coderoast_ipc_core
                "${GTEST_LIB_DIR}/libgtest.a"
                pthread
            )
            
            # Register tests
            add_test(NAME coderoast_ipc_core_tests COMMAND coderoast_ipc_core_tests)
        else()
            message(FATAL_ERROR "Could not find gtest. INCLUDE=${GTEST_INCLUDE_DIR}, LIB=${GTEST_LIB_DIR}")
        endif()
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
        # Try to load benchmark paths from conanfile.py injection
        if(EXISTS "${CMAKE_BINARY_DIR}/benchmark_paths.cmake")
            include("${CMAKE_BINARY_DIR}/benchmark_paths.cmake")
        endif()
        
        # If paths still not set, search in Conan cache
        if(NOT BENCHMARK_INCLUDE_DIR)
            file(GLOB_RECURSE BENCHMARK_HEADER "${CMAKE_BINARY_DIR}/../../../.conan2/**/benchmark/benchmark.h")
            if(BENCHMARK_HEADER)
                list(GET BENCHMARK_HEADER 0 BENCHMARK_HEADER_PATH)
                get_filename_component(BENCHMARK_INCLUDE_DIR "${BENCHMARK_HEADER_PATH}" DIRECTORY)
                get_filename_component(BENCHMARK_INCLUDE_DIR "${BENCHMARK_INCLUDE_DIR}" DIRECTORY)
            endif()
        endif()
        
        if(NOT BENCHMARK_LIB_DIR)
            file(GLOB_RECURSE BENCHMARK_LIB "${CMAKE_BINARY_DIR}/../../../.conan2/**/libbenchmark.a")
            if(BENCHMARK_LIB)
                list(GET BENCHMARK_LIB 0 BENCHMARK_LIB_PATH)
                get_filename_component(BENCHMARK_LIB_DIR "${BENCHMARK_LIB_PATH}" DIRECTORY)
            endif()
        endif()
        
        if(BENCHMARK_INCLUDE_DIR AND BENCHMARK_LIB_DIR)
            message(STATUS "Found benchmark: INCLUDE=${BENCHMARK_INCLUDE_DIR}, LIB=${BENCHMARK_LIB_DIR}")
            
            add_executable(coderoast_ipc_core_bench ${BENCH_SOURCES})
            target_include_directories(coderoast_ipc_core_bench PRIVATE "${BENCHMARK_INCLUDE_DIR}")
            target_link_libraries(coderoast_ipc_core_bench PRIVATE
                coderoast_ipc_core
                "${BENCHMARK_LIB_DIR}/libbenchmark.a"
                pthread
            )
        else()
            message(WARNING "Could not find benchmark. INCLUDE=${BENCHMARK_INCLUDE_DIR}, LIB=${BENCHMARK_LIB_DIR}")
        endif()
    endif()
endif()
