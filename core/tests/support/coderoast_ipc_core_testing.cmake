# DN-102.D2 routes the orphaned-segment reaper to every test executable that creates shared-memory
# segments: a crashed test process runs no destructor, so on a desk or a runner sharing the host's
# /dev/shm its segments accumulate. Each such executable reaps once, at its start, through the
# googletest environment in reap_orphaned_segments_at_start.cpp.
#
# The unit is compiled INTO the named test target and never into coderoast_ipc_core, so the shipped
# archive carries no googletest dependency. The target must link GTest::gtest and
# coderoast::ipc::core and build with CXX_MODULE_STD ON; every caller already does.
function(coderoast_ipc_reap_orphaned_segments_at_start target)
    target_sources(${target} PRIVATE
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/reap_orphaned_segments_at_start.cpp")
endfunction()
