cmake_minimum_required(VERSION 3.28)

# 1.5.1 unwrap (Argos to own topology): the pure module imports std → gate UUID before project().
set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")

project(coderoast_ipc_producer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CODEROAST_IPC_PRODUCER_BUILD_TESTS "Build coderoast_ipc_producer unit tests" ${PROJECT_IS_TOP_LEVEL})

# Conan dependencies
find_package(coderoast_ipc_core REQUIRED CONFIG)

###########################################################
#  Module-bearing library (1.5.1 unwrap — header-only code now in the module interface; wrapper +
#  textual api/ headers retired). The module `import coderoast.ipc.core`, so link core.
#  1.5.2 §11.9.11 sizing ruling: DELIBERATELY a SINGLE module (no internal/api/detail
#  layers) — one cohesive frame-building concern, all-public surface, nothing sealed.
###########################################################
add_library(coderoast_ipc_producer STATIC)
target_sources(coderoast_ipc_producer
    PUBLIC
        FILE_SET cxx_modules TYPE CXX_MODULES
        BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/api
        FILES    ${CMAKE_CURRENT_SOURCE_DIR}/api/coderoast_ipc_producer.cppm
)
set_target_properties(coderoast_ipc_producer PROPERTIES EXPORT_NAME producer
    CXX_SCAN_FOR_MODULES ON CXX_MODULE_STD ON)
target_compile_features(coderoast_ipc_producer PUBLIC cxx_std_23)
target_link_libraries(coderoast_ipc_producer PUBLIC coderoast::ipc::core)
add_library(coderoast::ipc::producer ALIAS coderoast_ipc_producer)
# Transitional alias (the separate _module target is retired).
add_library(coderoast::ipc::producer_module ALIAS coderoast_ipc_producer)

include(GNUInstallDirs)
install(TARGETS coderoast_ipc_producer
    EXPORT coderoast_ipc_producerTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILE_SET cxx_modules DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/coderoast_ipc_producer_modules
)
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
        find_package(GTest REQUIRED CONFIG)
        enable_testing()

        # CONSUMER test target (links the lib — doubles as the import proof; §11.9.7
        # members are pointless here: nothing is sealed). Test TUs import the
        # coderoast.ipc.producer.test aggregate, compiled in-target. Test TUs carry their
        # own main() (InitGoogleTest) → link GTest::gtest, not gtest_main.
        add_executable(coderoast_ipc_producer_tests ${TEST_SOURCES})
        target_sources(coderoast_ipc_producer_tests
            PRIVATE
                FILE_SET cxx_modules TYPE CXX_MODULES
                BASE_DIRS ${CMAKE_CURRENT_SOURCE_DIR}/tests
                FILES    ${CMAKE_CURRENT_SOURCE_DIR}/tests/producer.test.cppm)
        target_link_libraries(coderoast_ipc_producer_tests PRIVATE
                coderoast_ipc_producer
                coderoast::ipc::core
                GTest::gtest
        )
        target_compile_features(coderoast_ipc_producer_tests PRIVATE cxx_std_23)
        set_target_properties(coderoast_ipc_producer_tests PROPERTIES
            CXX_SCAN_FOR_MODULES ON CXX_MODULE_STD ON)
        include(GoogleTest)
        gtest_discover_tests(coderoast_ipc_producer_tests)
    endif()
endif()
