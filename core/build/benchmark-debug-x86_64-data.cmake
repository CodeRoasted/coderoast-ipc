########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

list(APPEND benchmark_COMPONENT_NAMES benchmark::benchmark benchmark::benchmark_main)
list(REMOVE_DUPLICATES benchmark_COMPONENT_NAMES)
if(DEFINED benchmark_FIND_DEPENDENCY_NAMES)
  list(APPEND benchmark_FIND_DEPENDENCY_NAMES )
  list(REMOVE_DUPLICATES benchmark_FIND_DEPENDENCY_NAMES)
else()
  set(benchmark_FIND_DEPENDENCY_NAMES )
endif()

########### VARIABLES #######################################################################
#############################################################################################
set(benchmark_PACKAGE_FOLDER_DEBUG "/home/windows/workspace/coderoast/coderoast-ipc/.conan2/p/b/bench3690fbf9cddcb/p")
set(benchmark_BUILD_MODULES_PATHS_DEBUG )


set(benchmark_INCLUDE_DIRS_DEBUG "${benchmark_PACKAGE_FOLDER_DEBUG}/include")
set(benchmark_RES_DIRS_DEBUG )
set(benchmark_DEFINITIONS_DEBUG "-DBENCHMARK_STATIC_DEFINE")
set(benchmark_SHARED_LINK_FLAGS_DEBUG )
set(benchmark_EXE_LINK_FLAGS_DEBUG )
set(benchmark_OBJECTS_DEBUG )
set(benchmark_COMPILE_DEFINITIONS_DEBUG "BENCHMARK_STATIC_DEFINE")
set(benchmark_COMPILE_OPTIONS_C_DEBUG )
set(benchmark_COMPILE_OPTIONS_CXX_DEBUG )
set(benchmark_LIB_DIRS_DEBUG "${benchmark_PACKAGE_FOLDER_DEBUG}/lib")
set(benchmark_BIN_DIRS_DEBUG )
set(benchmark_LIBRARY_TYPE_DEBUG STATIC)
set(benchmark_IS_HOST_WINDOWS_DEBUG 0)
set(benchmark_LIBS_DEBUG benchmark_main benchmark)
set(benchmark_SYSTEM_LIBS_DEBUG pthread rt m)
set(benchmark_FRAMEWORK_DIRS_DEBUG )
set(benchmark_FRAMEWORKS_DEBUG )
set(benchmark_BUILD_DIRS_DEBUG )
set(benchmark_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(benchmark_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${benchmark_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${benchmark_COMPILE_OPTIONS_C_DEBUG}>")
set(benchmark_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${benchmark_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${benchmark_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${benchmark_EXE_LINK_FLAGS_DEBUG}>")


set(benchmark_COMPONENTS_DEBUG benchmark::benchmark benchmark::benchmark_main)
########### COMPONENT benchmark::benchmark_main VARIABLES ############################################

set(benchmark_benchmark_benchmark_main_INCLUDE_DIRS_DEBUG "${benchmark_PACKAGE_FOLDER_DEBUG}/include")
set(benchmark_benchmark_benchmark_main_LIB_DIRS_DEBUG "${benchmark_PACKAGE_FOLDER_DEBUG}/lib")
set(benchmark_benchmark_benchmark_main_BIN_DIRS_DEBUG )
set(benchmark_benchmark_benchmark_main_LIBRARY_TYPE_DEBUG STATIC)
set(benchmark_benchmark_benchmark_main_IS_HOST_WINDOWS_DEBUG 0)
set(benchmark_benchmark_benchmark_main_RES_DIRS_DEBUG )
set(benchmark_benchmark_benchmark_main_DEFINITIONS_DEBUG )
set(benchmark_benchmark_benchmark_main_OBJECTS_DEBUG )
set(benchmark_benchmark_benchmark_main_COMPILE_DEFINITIONS_DEBUG )
set(benchmark_benchmark_benchmark_main_COMPILE_OPTIONS_C_DEBUG "")
set(benchmark_benchmark_benchmark_main_COMPILE_OPTIONS_CXX_DEBUG "")
set(benchmark_benchmark_benchmark_main_LIBS_DEBUG benchmark_main)
set(benchmark_benchmark_benchmark_main_SYSTEM_LIBS_DEBUG )
set(benchmark_benchmark_benchmark_main_FRAMEWORK_DIRS_DEBUG )
set(benchmark_benchmark_benchmark_main_FRAMEWORKS_DEBUG )
set(benchmark_benchmark_benchmark_main_DEPENDENCIES_DEBUG benchmark::benchmark)
set(benchmark_benchmark_benchmark_main_SHARED_LINK_FLAGS_DEBUG )
set(benchmark_benchmark_benchmark_main_EXE_LINK_FLAGS_DEBUG )
set(benchmark_benchmark_benchmark_main_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(benchmark_benchmark_benchmark_main_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${benchmark_benchmark_benchmark_main_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${benchmark_benchmark_benchmark_main_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${benchmark_benchmark_benchmark_main_EXE_LINK_FLAGS_DEBUG}>
)
set(benchmark_benchmark_benchmark_main_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${benchmark_benchmark_benchmark_main_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${benchmark_benchmark_benchmark_main_COMPILE_OPTIONS_C_DEBUG}>")
########### COMPONENT benchmark::benchmark VARIABLES ############################################

set(benchmark_benchmark_benchmark_INCLUDE_DIRS_DEBUG "${benchmark_PACKAGE_FOLDER_DEBUG}/include")
set(benchmark_benchmark_benchmark_LIB_DIRS_DEBUG "${benchmark_PACKAGE_FOLDER_DEBUG}/lib")
set(benchmark_benchmark_benchmark_BIN_DIRS_DEBUG )
set(benchmark_benchmark_benchmark_LIBRARY_TYPE_DEBUG STATIC)
set(benchmark_benchmark_benchmark_IS_HOST_WINDOWS_DEBUG 0)
set(benchmark_benchmark_benchmark_RES_DIRS_DEBUG )
set(benchmark_benchmark_benchmark_DEFINITIONS_DEBUG "-DBENCHMARK_STATIC_DEFINE")
set(benchmark_benchmark_benchmark_OBJECTS_DEBUG )
set(benchmark_benchmark_benchmark_COMPILE_DEFINITIONS_DEBUG "BENCHMARK_STATIC_DEFINE")
set(benchmark_benchmark_benchmark_COMPILE_OPTIONS_C_DEBUG "")
set(benchmark_benchmark_benchmark_COMPILE_OPTIONS_CXX_DEBUG "")
set(benchmark_benchmark_benchmark_LIBS_DEBUG benchmark)
set(benchmark_benchmark_benchmark_SYSTEM_LIBS_DEBUG pthread rt m)
set(benchmark_benchmark_benchmark_FRAMEWORK_DIRS_DEBUG )
set(benchmark_benchmark_benchmark_FRAMEWORKS_DEBUG )
set(benchmark_benchmark_benchmark_DEPENDENCIES_DEBUG )
set(benchmark_benchmark_benchmark_SHARED_LINK_FLAGS_DEBUG )
set(benchmark_benchmark_benchmark_EXE_LINK_FLAGS_DEBUG )
set(benchmark_benchmark_benchmark_NO_SONAME_MODE_DEBUG FALSE)

# COMPOUND VARIABLES
set(benchmark_benchmark_benchmark_LINKER_FLAGS_DEBUG
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${benchmark_benchmark_benchmark_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${benchmark_benchmark_benchmark_SHARED_LINK_FLAGS_DEBUG}>
        $<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${benchmark_benchmark_benchmark_EXE_LINK_FLAGS_DEBUG}>
)
set(benchmark_benchmark_benchmark_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${benchmark_benchmark_benchmark_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${benchmark_benchmark_benchmark_COMPILE_OPTIONS_C_DEBUG}>")