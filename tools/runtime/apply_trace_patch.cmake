cmake_minimum_required(VERSION 3.25)
if(NOT IS_DIRECTORY "${NEXUS_LLAMA_SOURCE}" OR NOT EXISTS "${NEXUS_TRACE_PATCH}")
  message(FATAL_ERROR "Canonical trace source directory and patch are required")
endif()
find_package(Git REQUIRED)
get_filename_component(NEXUS_LLAMA_SOURCE "${NEXUS_LLAMA_SOURCE}" REALPATH)
get_filename_component(nexus_source_parent "${NEXUS_LLAMA_SOURCE}" DIRECTORY)
# Stop Git discovering the enclosing Nexus checkout. Otherwise git apply can
# silently skip every dependency-relative path and still return success.
set(nexus_apply_command "${CMAKE_COMMAND}" -E env
  --unset=GIT_DIR --unset=GIT_WORK_TREE --unset=GIT_INDEX_FILE
  "GIT_CEILING_DIRECTORIES=${nexus_source_parent}"
  # The archive is LF; ignore the host's Windows checkout conversion settings
  # for both applying the patch and verifying the resulting source bytes.
  "${GIT_EXECUTABLE}" -c core.autocrlf=false -c core.eol=lf
  -C "${NEXUS_LLAMA_SOURCE}" apply)
function(nexus_trace_patch_verified output)
  if(NOT EXISTS "${NEXUS_LLAMA_SOURCE}/ggml/src/ggml-cuda/q8canon.cu" OR
     NOT EXISTS "${NEXUS_LLAMA_SOURCE}/ggml/src/ggml-cuda/q8canon.cuh")
    set(${output} FALSE PARENT_SCOPE)
    return()
  endif()
  execute_process(COMMAND ${nexus_apply_command} --reverse --check "${NEXUS_TRACE_PATCH}"
    RESULT_VARIABLE result OUTPUT_QUIET ERROR_QUIET)
  if(result EQUAL 0)
    set(${output} TRUE PARENT_SCOPE)
  else()
    set(${output} FALSE PARENT_SCOPE)
  endif()
endfunction()
nexus_trace_patch_verified(nexus_verified)
if(nexus_verified)
  message(STATUS "Canonical trace patch verified")
  return()
endif()
if(NEXUS_VERIFY_ONLY)
  message(FATAL_ERROR "Canonical trace patch is missing or incomplete; refusing to build the runtime")
endif()
execute_process(COMMAND ${nexus_apply_command} --check "${NEXUS_TRACE_PATCH}"
  RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE detail)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Canonical trace patch does not match the pinned source: ${detail}")
endif()
execute_process(COMMAND ${nexus_apply_command} "${NEXUS_TRACE_PATCH}"
  RESULT_VARIABLE result OUTPUT_QUIET ERROR_VARIABLE detail)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Canonical trace patch failed: ${detail}")
endif()
nexus_trace_patch_verified(nexus_verified)
if(NOT nexus_verified)
  message(FATAL_ERROR "Canonical trace patch was not applied completely")
endif()
message(STATUS "Canonical trace patch applied and verified")
