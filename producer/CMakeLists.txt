cmake_minimum_required(VERSION 3.28)

project(coderoast_ipc_producer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CODEROAST_IPC_PRODUCER_BUILD_TESTS "Build coderoast_ipc_producer unit tests" ${PROJECT_IS_TOP_LEVEL})

# Conan dependencies
find_package(coderoast_ipc_core REQUIRED CONFIG)

file(GLOB_RECURSE API_HEADERS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/api/*.hpp")

add_library(coderoast_ipc_producer INTERFACE ${API_HEADERS})
set_target_properties(coderoast_ipc_producer PROPERTIES EXPORT_NAME ipc_producer)
add_library(coderoast::ipc::producer ALIAS coderoast_ipc_producer)

target_include_directories(coderoast_ipc_producer
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/api>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(coderoast_ipc_producer
    INTERFACE
        coderoast::ipc::core
)

include(GNUInstallDirs)
install(TARGETS coderoast_ipc_producer
    EXPORT coderoast_ipc_producerTargets
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY api/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT coderoast_ipc_producerTargets
    NAMESPACE coderoast::ipc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc_producer
)

if(CODEROAST_IPC_PRODUCER_BUILD_TESTS)
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
            
            add_executable(coderoast_ipc_producer_tests ${TEST_SOURCES})
            target_link_libraries(coderoast_ipc_producer_tests PRIVATE
                coderoast_ipc_producer
                coderoast::ipc::core
                "${GTEST_LIB_DIR}/libgtest.a"
                pthread
            )
            # Ensure core headers are in include path (source tree + installed package)
            target_include_directories(coderoast_ipc_producer_tests PRIVATE
                "${GTEST_INCLUDE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../core/api"
            )
            
            # Register tests
            add_test(NAME coderoast_ipc_producer_tests COMMAND coderoast_ipc_producer_tests)
        else()
            message(FATAL_ERROR "Could not find gtest. INCLUDE=${GTEST_INCLUDE_DIR}, LIB=${GTEST_LIB_DIR}")
        endif()
    endif()
endif()
