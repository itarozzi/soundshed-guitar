# Applies a patch to a fetched dependency, from FetchContent's PATCH_COMMAND, in the
# dependency's source directory:
#
#   PATCH_COMMAND ${CMAKE_COMMAND} -DPATCH=<file.patch> -P ApplyPatch.cmake
#
# FetchContent runs the patch step again whenever it re-runs its update step (a changed
# declaration, a cleaned build tree), on a source tree that may already carry the patch. So a
# patch that reverses cleanly is taken as applied and left alone; anything else that does not
# apply stops the configure, since the pin and the patch no longer agree.
#
# git apply, not git am or patch(1): it needs no identity and no commit, and works on a
# submodule's files from the superproject (Elements' lib/artist). Used for Nano Q's iOS build;
# see docs/plans/nano-q-ios.md.

if(NOT PATCH OR NOT EXISTS "${PATCH}")
    message(FATAL_ERROR "ApplyPatch.cmake: PATCH is not a file: '${PATCH}'")
endif()

find_package(Git REQUIRED QUIET)
get_filename_component(_name "${PATCH}" NAME)

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --reverse --check "${PATCH}"
    RESULT_VARIABLE _applied OUTPUT_QUIET ERROR_QUIET)
if(_applied EQUAL 0)
    message(STATUS "${_name}: already applied")
    return()
endif()

execute_process(
    COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${PATCH}"
    RESULT_VARIABLE _result ERROR_VARIABLE _error)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${_name} does not apply to ${CMAKE_CURRENT_SOURCE_DIR} (has the pin moved?):\n${_error}")
endif()
message(STATUS "${_name}: applied")
