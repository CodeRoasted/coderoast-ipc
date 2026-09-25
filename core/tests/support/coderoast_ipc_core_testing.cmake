# DN-102.D2 routes the orphaned-segment reaper to every test and benchmark executable that creates
# shared-memory segments: a crashed process runs no destructor, so on a desk or a runner sharing the
# host's /dev/shm its segments accumulate. Each such executable reaps once, at its start:
# reap_orphaned_segments_at_start.cpp is the one reap and its report, which a googletest executable
# runs through the environment in reap_orphaned_segments_at_start_environment.cpp and a benchmark
# main calls through reap_orphaned_segments_at_start.hpp.
#
# Every unit here is compiled INTO the named target and never into coderoast_ipc_core, so the
# shipped archive carries no googletest or benchmark dependency. The target must link
# coderoast::ipc::core (plus GTest::gtest for the googletest functions) and build with
# CXX_MODULE_STD ON; every caller already does.
function(coderoast_ipc_reap_orphaned_segments_at_start target)
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/reap_orphaned_segments_at_start.cpp"
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/reap_orphaned_segments_at_start_environment.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
endfunction()

# A benchmark main includes reap_orphaned_segments_at_start.hpp and calls
# coderoast::ipc::testing::reap_orphaned_segments_at_start() once its arguments are parsed.
function(coderoast_ipc_reap_orphaned_segments_at_benchmark_start target)
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/reap_orphaned_segments_at_start.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
endfunction()

# A googletest arm that must hold a segment whose owner is dead reruns itself inside a private
# user, pid and mount namespace through private_pid_namespace.hpp: a reaper started by any other
# process on the host then judges that segment Unknown and cannot remove it first.
function(coderoast_ipc_private_pid_namespace target)
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/private_pid_namespace.cpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
endfunction()
