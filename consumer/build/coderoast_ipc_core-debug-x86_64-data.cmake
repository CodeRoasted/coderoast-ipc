########### AGGREGATED COMPONENTS AND DEPENDENCIES FOR THE MULTI CONFIG #####################
#############################################################################################

set(coderoast_ipc_core_COMPONENT_NAMES "")
if(DEFINED coderoast_ipc_core_FIND_DEPENDENCY_NAMES)
  list(APPEND coderoast_ipc_core_FIND_DEPENDENCY_NAMES )
  list(REMOVE_DUPLICATES coderoast_ipc_core_FIND_DEPENDENCY_NAMES)
else()
  set(coderoast_ipc_core_FIND_DEPENDENCY_NAMES )
endif()

########### VARIABLES #######################################################################
#############################################################################################
set(coderoast_ipc_core_PACKAGE_FOLDER_DEBUG "/home/windows/workspace/coderoast/coderoast-ipc/.conan2/p/b/coder82d41dc26c802/p")
set(coderoast_ipc_core_BUILD_MODULES_PATHS_DEBUG )


set(coderoast_ipc_core_INCLUDE_DIRS_DEBUG "${coderoast_ipc_core_PACKAGE_FOLDER_DEBUG}/include")
set(coderoast_ipc_core_RES_DIRS_DEBUG )
set(coderoast_ipc_core_DEFINITIONS_DEBUG )
set(coderoast_ipc_core_SHARED_LINK_FLAGS_DEBUG )
set(coderoast_ipc_core_EXE_LINK_FLAGS_DEBUG )
set(coderoast_ipc_core_OBJECTS_DEBUG )
set(coderoast_ipc_core_COMPILE_DEFINITIONS_DEBUG )
set(coderoast_ipc_core_COMPILE_OPTIONS_C_DEBUG )
set(coderoast_ipc_core_COMPILE_OPTIONS_CXX_DEBUG )
set(coderoast_ipc_core_LIB_DIRS_DEBUG )
set(coderoast_ipc_core_BIN_DIRS_DEBUG )
set(coderoast_ipc_core_LIBRARY_TYPE_DEBUG UNKNOWN)
set(coderoast_ipc_core_IS_HOST_WINDOWS_DEBUG 0)
set(coderoast_ipc_core_LIBS_DEBUG )
set(coderoast_ipc_core_SYSTEM_LIBS_DEBUG )
set(coderoast_ipc_core_FRAMEWORK_DIRS_DEBUG )
set(coderoast_ipc_core_FRAMEWORKS_DEBUG )
set(coderoast_ipc_core_BUILD_DIRS_DEBUG )
set(coderoast_ipc_core_NO_SONAME_MODE_DEBUG FALSE)


# COMPOUND VARIABLES
set(coderoast_ipc_core_COMPILE_OPTIONS_DEBUG
    "$<$<COMPILE_LANGUAGE:CXX>:${coderoast_ipc_core_COMPILE_OPTIONS_CXX_DEBUG}>"
    "$<$<COMPILE_LANGUAGE:C>:${coderoast_ipc_core_COMPILE_OPTIONS_C_DEBUG}>")
set(coderoast_ipc_core_LINKER_FLAGS_DEBUG
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,SHARED_LIBRARY>:${coderoast_ipc_core_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,MODULE_LIBRARY>:${coderoast_ipc_core_SHARED_LINK_FLAGS_DEBUG}>"
    "$<$<STREQUAL:$<TARGET_PROPERTY:TYPE>,EXECUTABLE>:${coderoast_ipc_core_EXE_LINK_FLAGS_DEBUG}>")


set(coderoast_ipc_core_COMPONENTS_DEBUG )