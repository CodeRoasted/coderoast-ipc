cmake_minimum_required(VERSION 3.28)

project(coderoast_ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CODEROAST_IPC_BUILD_TESTS "Build coderoast_ipc unit tests" ${PROJECT_IS_TOP_LEVEL})
option(CODEROAST_IPC_BUILD_BENCHMARKS "Build coderoast_ipc microbenchmarks" ${PROJECT_IS_TOP_LEVEL})

file(GLOB_RECURSE API_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/api/*.hpp")

add_library(coderoast_ipc INTERFACE ${API_HEADERS})
set_target_properties(coderoast_ipc PROPERTIES EXPORT_NAME ipc)
add_library(coderoast::ipc ALIAS coderoast_ipc)

target_include_directories(coderoast_ipc
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/api>
        $<INSTALL_INTERFACE:include>
)

include(GNUInstallDirs)
install(TARGETS coderoast_ipc
    EXPORT coderoast_ipcTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY api/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT coderoast_ipcTargets
    NAMESPACE coderoast::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc
)

if(CODEROAST_IPC_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp"
    )
    if(TEST_SOURCES)
        include(CTest)
        find_package(GTest REQUIRED CONFIG)
        enable_testing()

        add_executable(coderoast_ipc_tests ${TEST_SOURCES})
        target_link_libraries(coderoast_ipc_tests PRIVATE
            coderoast_ipc
            GTest::gtest
            GTest::gtest_main
        )
        include(GoogleTest)
        gtest_discover_tests(coderoast_ipc_tests)
    endif()
endif()

if(CODEROAST_IPC_BUILD_BENCHMARKS)
    file(GLOB_RECURSE BENCH_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/benchmarks/*.cpp"
    )
    if(BENCH_SOURCES)
        find_package(benchmark REQUIRED CONFIG)
        add_executable(coderoast_ipc_bench ${BENCH_SOURCES})
        target_link_libraries(coderoast_ipc_bench PRIVATE
            coderoast_ipc
            benchmark::benchmark
        )
    endif()
endif()
