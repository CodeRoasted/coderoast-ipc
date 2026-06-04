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
# EXPORT_NAME = producer → exported coderoast::ipc::producer (native name is the
# consumed one under cmake_find_mode=none, §10.12); must equal the alias.
set_target_properties(coderoast_ipc_producer PROPERTIES EXPORT_NAME producer)
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

###########################################################
#  Named-module wrapper (coderoast.ipc.producer) — §8.1 (1.5.0)
###########################################################
# Header-only lib → the module is the only compiled artifact. Its GMF #includes
# the producer header (which textually pulls core's frame.hpp), so it links the
# lib PUBLIC (api/ includes + core). cxx_std_23 PUBLIC is the §10.4 producer gotcha.
add_library(coderoast_ipc_producer_module STATIC)
target_sources(coderoast_ipc_producer_module
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules
        FILES    ${CMAKE_CURRENT_SOURCE_DIR}/modules/coderoast_ipc_producer.cppm
)
target_link_libraries(coderoast_ipc_producer_module PUBLIC coderoast_ipc_producer)
target_compile_features(coderoast_ipc_producer_module PUBLIC cxx_std_23)
add_library(coderoast::ipc::producer_module ALIAS coderoast_ipc_producer_module)

include(GNUInstallDirs)
# Library + module wrapper in ONE export set → single file-set owner (§10.5/§10.8).
install(TARGETS coderoast_ipc_producer coderoast_ipc_producer_module
    EXPORT coderoast_ipc_producerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILE_SET cxx_modules DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/coderoast_ipc_producer_modules
)
install(DIRECTORY api/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT coderoast_ipc_producerTargets
    NAMESPACE coderoast::ipc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc_producer
)

###########################################################
#  Cross-package module shipping (Argos — §10.7/§10.9)
###########################################################
# Ship the package's OWN cmake config (carries FILE_SET CXX_MODULES); conan
# defers via cmake_find_mode=none. create → install(EXPORT); editable →
# BUILD-TREE export(). The config find_dependency()s core (the direct dep).
export(EXPORT coderoast_ipc_producerTargets
    NAMESPACE coderoast::ipc::
    FILE ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_producerTargets.cmake)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_producer-config.cmake
"include(CMakeFindDependencyMacro)
find_dependency(coderoast_ipc_core)
include(\"\${CMAKE_CURRENT_LIST_DIR}/coderoast_ipc_producerTargets.cmake\")
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_producer-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc_producer)

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
