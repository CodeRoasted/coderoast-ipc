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
# EXPORT_NAME = consumer → exported coderoast::ipc::consumer (native name is the
# consumed one under cmake_find_mode=none, §10.12); must equal the alias.
set_target_properties(coderoast_ipc_consumer PROPERTIES EXPORT_NAME consumer)
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

###########################################################
#  Named-module wrapper (coderoast.ipc.consumer) — §8.1 (1.5.0)
###########################################################
# Header-only lib → the module is the only compiled artifact. Its GMF #includes
# the 10 consumer headers (which textually pull core's frame/channel), so it links
# the lib PUBLIC. cxx_std_23 PUBLIC is the §10.4 producer gotcha.
add_library(coderoast_ipc_consumer_module STATIC)
target_sources(coderoast_ipc_consumer_module
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/modules
        FILES    ${CMAKE_CURRENT_SOURCE_DIR}/modules/coderoast_ipc_consumer.cppm
)
target_link_libraries(coderoast_ipc_consumer_module PUBLIC coderoast_ipc_consumer)
target_compile_features(coderoast_ipc_consumer_module PUBLIC cxx_std_23)
add_library(coderoast::ipc::consumer_module ALIAS coderoast_ipc_consumer_module)

include(GNUInstallDirs)
# Library + module wrapper in ONE export set → single file-set owner (§10.5/§10.8).
install(TARGETS coderoast_ipc_consumer coderoast_ipc_consumer_module
    EXPORT coderoast_ipc_consumerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILE_SET cxx_modules DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/coderoast_ipc_consumer_modules
)
install(DIRECTORY api/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT coderoast_ipc_consumerTargets
    NAMESPACE coderoast::ipc::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc_consumer
)

###########################################################
#  Cross-package module shipping (Argos — §10.7/§10.9)
###########################################################
# Ship the package's OWN cmake config (carries FILE_SET CXX_MODULES); conan
# defers via cmake_find_mode=none. The config find_dependency()s core.
export(EXPORT coderoast_ipc_consumerTargets
    NAMESPACE coderoast::ipc::
    FILE ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_consumerTargets.cmake)
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_consumer-config.cmake
"include(CMakeFindDependencyMacro)
find_dependency(coderoast_ipc_core)
include(\"\${CMAKE_CURRENT_LIST_DIR}/coderoast_ipc_consumerTargets.cmake\")
")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/coderoast_ipc_consumer-config.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/coderoast_ipc_consumer)

if(CODEROAST_IPC_CONSUMER_BUILD_TESTS)
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
            
            add_executable(coderoast_ipc_consumer_tests ${TEST_SOURCES})
            target_link_libraries(coderoast_ipc_consumer_tests PRIVATE
                coderoast_ipc_consumer
                coderoast::ipc::core
                "${GTEST_LIB_DIR}/libgtest.a"
                pthread
            )
            # Ensure core headers are in include path (source tree + installed package)
            target_include_directories(coderoast_ipc_consumer_tests PRIVATE
                "${GTEST_INCLUDE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../core/api"
            )
            
            # Register tests
            add_test(NAME coderoast_ipc_consumer_tests COMMAND coderoast_ipc_consumer_tests)
        else()
            message(FATAL_ERROR "Could not find gtest. INCLUDE=${GTEST_INCLUDE_DIR}, LIB=${GTEST_LIB_DIR}")
        endif()
    endif()
endif()
