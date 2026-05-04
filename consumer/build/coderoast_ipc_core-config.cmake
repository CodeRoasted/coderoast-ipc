########## MACROS ###########################################################################
#############################################################################################

# Requires CMake > 3.15
if(${CMAKE_VERSION} VERSION_LESS "3.15")
    message(FATAL_ERROR "The 'CMakeDeps' generator only works with CMake >= 3.15")
endif()

if(coderoast_ipc_core_FIND_QUIETLY)
    set(coderoast_ipc_core_MESSAGE_MODE VERBOSE)
else()
    set(coderoast_ipc_core_MESSAGE_MODE STATUS)
endif()

include(${CMAKE_CURRENT_LIST_DIR}/cmakedeps_macros.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/coderoast_ipc_coreTargets.cmake)
include(CMakeFindDependencyMacro)

check_build_type_defined()

foreach(_DEPENDENCY ${coderoast_ipc_core_FIND_DEPENDENCY_NAMES} )
    # Check that we have not already called a find_package with the transitive dependency
    if(NOT ${_DEPENDENCY}_FOUND)
        find_dependency(${_DEPENDENCY} REQUIRED ${${_DEPENDENCY}_FIND_MODE})
    endif()
endforeach()

set(coderoast_ipc_core_VERSION_STRING "1.0.0")
set(coderoast_ipc_core_INCLUDE_DIRS ${coderoast_ipc_core_INCLUDE_DIRS_DEBUG} )
set(coderoast_ipc_core_INCLUDE_DIR ${coderoast_ipc_core_INCLUDE_DIRS_DEBUG} )
set(coderoast_ipc_core_LIBRARIES ${coderoast_ipc_core_LIBRARIES_DEBUG} )
set(coderoast_ipc_core_DEFINITIONS ${coderoast_ipc_core_DEFINITIONS_DEBUG} )


# Definition of extra CMake variables from cmake_extra_variables


# Only the last installed configuration BUILD_MODULES are included to avoid the collision
foreach(_BUILD_MODULE ${coderoast_ipc_core_BUILD_MODULES_PATHS_DEBUG} )
    message(${coderoast_ipc_core_MESSAGE_MODE} "Conan: Including build module from '${_BUILD_MODULE}'")
    include(${_BUILD_MODULE})
endforeach()


