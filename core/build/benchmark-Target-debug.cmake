# Avoid multiple calls to find_package to append duplicated properties to the targets
include_guard()########### VARIABLES #######################################################################
#############################################################################################
set(benchmark_FRAMEWORKS_FOUND_DEBUG "") # Will be filled later
conan_find_apple_frameworks(benchmark_FRAMEWORKS_FOUND_DEBUG "${benchmark_FRAMEWORKS_DEBUG}" "${benchmark_FRAMEWORK_DIRS_DEBUG}")

set(benchmark_LIBRARIES_TARGETS "") # Will be filled later


######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
if(NOT TARGET benchmark_DEPS_TARGET)
    add_library(benchmark_DEPS_TARGET INTERFACE IMPORTED)
endif()

set_property(TARGET benchmark_DEPS_TARGET
             APPEND PROPERTY INTERFACE_LINK_LIBRARIES
             $<$<CONFIG:Debug>:${benchmark_FRAMEWORKS_FOUND_DEBUG}>
             $<$<CONFIG:Debug>:${benchmark_SYSTEM_LIBS_DEBUG}>
             $<$<CONFIG:Debug>:benchmark::benchmark>)

####### Find the libraries declared in cpp_info.libs, create an IMPORTED target for each one and link the
####### benchmark_DEPS_TARGET to all of them
conan_package_library_targets("${benchmark_LIBS_DEBUG}"    # libraries
                              "${benchmark_LIB_DIRS_DEBUG}" # package_libdir
                              "${benchmark_BIN_DIRS_DEBUG}" # package_bindir
                              "${benchmark_LIBRARY_TYPE_DEBUG}"
                              "${benchmark_IS_HOST_WINDOWS_DEBUG}"
                              benchmark_DEPS_TARGET
                              benchmark_LIBRARIES_TARGETS  # out_libraries_targets
                              "_DEBUG"
                              "benchmark"    # package_name
                              "${benchmark_NO_SONAME_MODE_DEBUG}")  # soname

# FIXME: What is the result of this for multi-config? All configs adding themselves to path?
set(CMAKE_MODULE_PATH ${benchmark_BUILD_DIRS_DEBUG} ${CMAKE_MODULE_PATH})

########## COMPONENTS TARGET PROPERTIES Debug ########################################

    ########## COMPONENT benchmark::benchmark_main #############

        set(benchmark_benchmark_benchmark_main_FRAMEWORKS_FOUND_DEBUG "")
        conan_find_apple_frameworks(benchmark_benchmark_benchmark_main_FRAMEWORKS_FOUND_DEBUG "${benchmark_benchmark_benchmark_main_FRAMEWORKS_DEBUG}" "${benchmark_benchmark_benchmark_main_FRAMEWORK_DIRS_DEBUG}")

        set(benchmark_benchmark_benchmark_main_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET benchmark_benchmark_benchmark_main_DEPS_TARGET)
            add_library(benchmark_benchmark_benchmark_main_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET benchmark_benchmark_benchmark_main_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_FRAMEWORKS_FOUND_DEBUG}>
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_SYSTEM_LIBS_DEBUG}>
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_DEPENDENCIES_DEBUG}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'benchmark_benchmark_benchmark_main_DEPS_TARGET' to all of them
        conan_package_library_targets("${benchmark_benchmark_benchmark_main_LIBS_DEBUG}"
                              "${benchmark_benchmark_benchmark_main_LIB_DIRS_DEBUG}"
                              "${benchmark_benchmark_benchmark_main_BIN_DIRS_DEBUG}" # package_bindir
                              "${benchmark_benchmark_benchmark_main_LIBRARY_TYPE_DEBUG}"
                              "${benchmark_benchmark_benchmark_main_IS_HOST_WINDOWS_DEBUG}"
                              benchmark_benchmark_benchmark_main_DEPS_TARGET
                              benchmark_benchmark_benchmark_main_LIBRARIES_TARGETS
                              "_DEBUG"
                              "benchmark_benchmark_benchmark_main"
                              "${benchmark_benchmark_benchmark_main_NO_SONAME_MODE_DEBUG}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET benchmark::benchmark_main
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_OBJECTS_DEBUG}>
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_LIBRARIES_TARGETS}>
                     )

        if("${benchmark_benchmark_benchmark_main_LIBS_DEBUG}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET benchmark::benchmark_main
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         benchmark_benchmark_benchmark_main_DEPS_TARGET)
        endif()

        set_property(TARGET benchmark::benchmark_main APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_LINKER_FLAGS_DEBUG}>)
        set_property(TARGET benchmark::benchmark_main APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_INCLUDE_DIRS_DEBUG}>)
        set_property(TARGET benchmark::benchmark_main APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_LIB_DIRS_DEBUG}>)
        set_property(TARGET benchmark::benchmark_main APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_COMPILE_DEFINITIONS_DEBUG}>)
        set_property(TARGET benchmark::benchmark_main APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_main_COMPILE_OPTIONS_DEBUG}>)


    ########## COMPONENT benchmark::benchmark #############

        set(benchmark_benchmark_benchmark_FRAMEWORKS_FOUND_DEBUG "")
        conan_find_apple_frameworks(benchmark_benchmark_benchmark_FRAMEWORKS_FOUND_DEBUG "${benchmark_benchmark_benchmark_FRAMEWORKS_DEBUG}" "${benchmark_benchmark_benchmark_FRAMEWORK_DIRS_DEBUG}")

        set(benchmark_benchmark_benchmark_LIBRARIES_TARGETS "")

        ######## Create an interface target to contain all the dependencies (frameworks, system and conan deps)
        if(NOT TARGET benchmark_benchmark_benchmark_DEPS_TARGET)
            add_library(benchmark_benchmark_benchmark_DEPS_TARGET INTERFACE IMPORTED)
        endif()

        set_property(TARGET benchmark_benchmark_benchmark_DEPS_TARGET
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_FRAMEWORKS_FOUND_DEBUG}>
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_SYSTEM_LIBS_DEBUG}>
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_DEPENDENCIES_DEBUG}>
                     )

        ####### Find the libraries declared in cpp_info.component["xxx"].libs,
        ####### create an IMPORTED target for each one and link the 'benchmark_benchmark_benchmark_DEPS_TARGET' to all of them
        conan_package_library_targets("${benchmark_benchmark_benchmark_LIBS_DEBUG}"
                              "${benchmark_benchmark_benchmark_LIB_DIRS_DEBUG}"
                              "${benchmark_benchmark_benchmark_BIN_DIRS_DEBUG}" # package_bindir
                              "${benchmark_benchmark_benchmark_LIBRARY_TYPE_DEBUG}"
                              "${benchmark_benchmark_benchmark_IS_HOST_WINDOWS_DEBUG}"
                              benchmark_benchmark_benchmark_DEPS_TARGET
                              benchmark_benchmark_benchmark_LIBRARIES_TARGETS
                              "_DEBUG"
                              "benchmark_benchmark_benchmark"
                              "${benchmark_benchmark_benchmark_NO_SONAME_MODE_DEBUG}")


        ########## TARGET PROPERTIES #####################################
        set_property(TARGET benchmark::benchmark
                     APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_OBJECTS_DEBUG}>
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_LIBRARIES_TARGETS}>
                     )

        if("${benchmark_benchmark_benchmark_LIBS_DEBUG}" STREQUAL "")
            # If the component is not declaring any "cpp_info.components['foo'].libs" the system, frameworks etc are not
            # linked to the imported targets and we need to do it to the global target
            set_property(TARGET benchmark::benchmark
                         APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                         benchmark_benchmark_benchmark_DEPS_TARGET)
        endif()

        set_property(TARGET benchmark::benchmark APPEND PROPERTY INTERFACE_LINK_OPTIONS
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_LINKER_FLAGS_DEBUG}>)
        set_property(TARGET benchmark::benchmark APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_INCLUDE_DIRS_DEBUG}>)
        set_property(TARGET benchmark::benchmark APPEND PROPERTY INTERFACE_LINK_DIRECTORIES
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_LIB_DIRS_DEBUG}>)
        set_property(TARGET benchmark::benchmark APPEND PROPERTY INTERFACE_COMPILE_DEFINITIONS
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_COMPILE_DEFINITIONS_DEBUG}>)
        set_property(TARGET benchmark::benchmark APPEND PROPERTY INTERFACE_COMPILE_OPTIONS
                     $<$<CONFIG:Debug>:${benchmark_benchmark_benchmark_COMPILE_OPTIONS_DEBUG}>)


    ########## AGGREGATED GLOBAL TARGET WITH THE COMPONENTS #####################
    set_property(TARGET benchmark::benchmark_main APPEND PROPERTY INTERFACE_LINK_LIBRARIES benchmark::benchmark_main)
    set_property(TARGET benchmark::benchmark_main APPEND PROPERTY INTERFACE_LINK_LIBRARIES benchmark::benchmark)

########## For the modules (FindXXX)
set(benchmark_LIBRARIES_DEBUG benchmark::benchmark_main)
