cmake_minimum_required(VERSION 3.28)

project(coderoast_ipc_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CODEROAST_IPC_CONSUMER_BUILD_TESTS "Build coderoast_ipc_consumer unit tests" ${PROJECT_IS_TOP_LEVEL})

# Conan dependencies
find_package(coderoast_ipc_core REQUIRED CONFIG)

file(GLOB_RECURSE API_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/api/*.hpp")

add_library(coderoast_ipc_consumer INTERFACE ${API_HEADERS})
set_target_properties(coderoast_ipc_consumer PROPERTIES EXPORT_NAME ipc_consumer)
add_library(coderoast::ipc::consumer ALIAS coderoast_ipc_consumer)

target_include_directories(coderoast_ipc_consumer
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/api>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(coderoast_ipc_consumer
    INTERFACE
        coderoast::ipc::core
)

include(GNUInstallDirs)
install(TARGETS coderoast_ipc_consumer
    EXPORT coderoast_ipc_consumerTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY api/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT coderoast_ipc_consumerTargets
    NAMESPACE coderoast::ipc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc_consumer
)

if(CODEROAST_IPC_CONSUMER_BUILD_TESTS)
    file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/*.cpp"
    )
    if(TEST_SOURCES)
        include(CTest)
        find_package(GTest REQUIRED CONFIG)
        enable_testing()

        add_executable(coderoast_ipc_consumer_tests ${TEST_SOURCES})
        target_link_libraries(coderoast_ipc_consumer_tests PRIVATE
            coderoast_ipc_consumer
            GTest::gtest
            GTest::gtest_main
        )
        include(GoogleTest)
        gtest_discover_tests(coderoast_ipc_consumer_tests)
    endif()
endif()
