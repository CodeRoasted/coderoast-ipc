# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(coderoast_ipc_core_FRAMEWORKS_FOUND_DEBUG "") # Will be filled later
conan_find_apple_frameworks(coderoast_ipc_core_FRAMEWORKS_FOUND_DEBUG "${coderoast_ipc_core_FRAMEWORKS_DEBUG}" "${coderoast_ipc_core_FRAMEWORK_DIRS_DEBUG}")

set(coderoast_ipc_core_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET coderoast_ipc_core_DEPS_TARGET)
    add_library(coderoast_ipc_core_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET coderoast_ipc_core_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Debug>:${coderoast_ipc_core_FRAMEWORKS_FOUND_DEBUG}>
             $<$<CONFIG:Debug>:${coderoast_ipc_core_SYSTEM_LIBS_DEBUG}>
             $<$<CONFIG:Debug>:>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### coderoast_ipc_core_DEPS_TARGET to all of them
conan_package_library_targets("${coderoast_ipc_core_LIBS_DEBUG}"    # libraries
                              "${coderoast_ipc_core_LIB_DIRS_DEBUG}" # package_libdir
                              "${coderoast_ipc_core_BIN_DIRS_DEBUG}" # package_bindir
                              "${coderoast_ipc_core_LIBRARY_TYPE_DEBUG}"
                              "${coderoast_ipc_core_IS_HOST_WINDOWS_DEBUG}"
                              coderoast_ipc_core_DEPS_TARGET
                              coderoast_ipc_core_LIBRARIES_TARGETS  # out_libraries_targets
                              "_DEBUG"
                              "coderoast_ipc_core"    # package_name
                              "${coderoast_ipc_core_NO_SONAME_MODE_DEBUG}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${coderoast_ipc_core_BUILD_DIRS_DEBUG} ${CMAKE_MODULE_PATH})

########## GLOBAL TARGET PROPERTIES Debug ########################################
    set_property(TARGET coderoast::ipc::core
                 APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                 $<$<CONFIG:Debug>:${coderoast_ipc_core_OBJECTS_DEBUG}>
                 $<$<CONFIG:Debug>:${coderoast_ipc_core_LIBRARIES_TARGETS}>
                 )

    if("${coderoast_ipc_core_LIBS_DEBUG}" STREQUAL "")
        # If the package is not declaring any "cpp_info.libs" the package deps, system libs,
        # frameworks etc are not linked to the imported targets and we need to do it to the
        # global target
        set_property(TARGET coderoast::ipc::core
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     coderoast_ipc_core_DEPS_TARGET)
    endif()

    set_property(TARGET coderoast::ipc::core
                 APPEND PROPERTY INTERFACE_LINK_OPTIONS
                 $<$<CONFIG:Debug>:${coderoast_ipc_core_LINKER_FLAGS_DEBUG}>)
    set_property(TARGET coderoast::ipc::core
                 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                 $<$<CONFIG:Debug>:${coderoast_ipc_core_INCLUDE_DIRS_DEBUG}>)
    # Necessary to find LINK shared libraries in Linux
    set_property(TARGET coderoast::ipc::core
                 APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                 $<$<CONFIG:Debug>:${coderoast_ipc_core_LIB_DIRS_DEBUG}>)
    set_property(TARGET coderoast::ipc::core
                 APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                 $<$<CONFIG:Debug>:${coderoast_ipc_core_COMPILE_DEFINITIONS_DEBUG}>)
    set_property(TARGET coderoast::ipc::core
                 APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                 $<$<CONFIG:Debug>:${coderoast_ipc_core_COMPILE_OPTIONS_DEBUG}>)

########## For the modules (FindXXX)
set(coderoast_ipc_core_LIBRARIES_DEBUG coderoast::ipc::core)
