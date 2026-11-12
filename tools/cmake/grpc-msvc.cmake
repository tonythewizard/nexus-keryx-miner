# CMAKE_PROJECT_grpc_INCLUDE: only the Windows grpc port receives this hook.
# No standards/ABI changes and no dependency downgrade.
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR
   CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.50" OR
   NOT CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.52")
  return()
endif()
if(NOT PACKAGE_VERSION STREQUAL "1.81.1")
  message(FATAL_ERROR "Review the gRPC MSVC workaround before updating the dependency")
endif()
find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/grpc_msvc.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/call/call_filters.h"
  RESULT_VARIABLE nexus_grpc_patch_result
  OUTPUT_VARIABLE nexus_grpc_patch_output
  ERROR_VARIABLE nexus_grpc_patch_error)
if(NOT nexus_grpc_patch_result EQUAL 0)
  message(FATAL_ERROR "Pinned gRPC MSVC workaround failed: ${nexus_grpc_patch_error}")
endif()
string(STRIP "${nexus_grpc_patch_output}" nexus_grpc_patch_output)
message(STATUS "${nexus_grpc_patch_output}")
