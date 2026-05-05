# Load the debug and release variables
file(GLOB DATA_FILES "${CMAKE_CURRENT_LIST_DIR}/coderoast_ipc_core-*-data.cmake")

foreach(f ${DATA_FILES})
    include(${f})
endforeach()

# Create the targets for all the components
foreach(_COMPONENT ${coderoast_ipc_core_COMPONENT_NAMES} )
    if(NOT TARGET ${_COMPONENT})
        add_library(${_COMPONENT} INTERFACE IMPORTED)
        message(${coderoast_ipc_core_MESSAGE_MODE} "Conan: Component target declared '${_COMPONENT}'")
    endif()
endforeach()

if(NOT TARGET coderoast::ipc::core)
    add_library(coderoast::ipc::core INTERFACE IMPORTED)
    message(${coderoast_ipc_core_MESSAGE_MODE} "Conan: Target declared 'coderoast::ipc::core'")
endif()
# Load the debug and release library finders
file(GLOB CONFIG_FILES "${CMAKE_CURRENT_LIST_DIR}/coderoast_ipc_core-Target-*.cmake")

foreach(f ${CONFIG_FILES})
    include(${f})
endforeach()