cmake_minimum_required(VERSION 3.25)
# Share the CUDA and MSVC runtimes across the wrapper and static backends.
set(CMAKE_CUDA_RUNTIME_LIBRARY Shared)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
project(nexus_inference_runtime LANGUAGES C CXX CUDA)
include(FetchContent)
find_package(Git REQUIRED)
find_package(CUDAToolkit 13 REQUIRED)
set(CMAKE_CUDA_ARCHITECTURES 86)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_CUDA ON CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_CUDA_NCCL OFF CACHE BOOL "" FORCE)
# Official miner 7f9704db: trace hooks live in the quantized CUDA kernels.
set(GGML_CUDA_FORCE_MMQ ON CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
get_filename_component(NEXUS_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(NEXUS_RUNTIME_ABI "8" CACHE STRING "Runtime ABI: 8 distribution baseline, 9 isolated candidate")
set_property(CACHE NEXUS_RUNTIME_ABI PROPERTY STRINGS 8 9)
if(NEXUS_RUNTIME_ABI STREQUAL "9")
  set(NEXUS_RUNTIME_WRAPPER "${NEXUS_SOURCE}/vendor/keryx-trace-abi6/tools/keryx-llama/abi9/keryx_llama.cpp")
  set(NEXUS_PATCH_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/apply_runtime_patches.cmake")
  set(NEXUS_RUNTIME_MANIFEST "${CMAKE_CURRENT_LIST_DIR}/abi9-patches.json")
  set(NEXUS_PATCH_ARGUMENT "-DNEXUS_RUNTIME_MANIFEST=${NEXUS_RUNTIME_MANIFEST}")
  file(SHA256 "${NEXUS_RUNTIME_MANIFEST}" NEXUS_PATCH_SET_SHA256)
  set(NEXUS_RUNTIME_MINER_COMMIT "c21c873726049195a3a40af6f19a013fc44ebd82")
  set(NEXUS_CANDIDATE_ONLY true)
else()
  if(NOT NEXUS_RUNTIME_ABI STREQUAL "8")
    message(FATAL_ERROR "Runtime ABI must be 8 or 9")
  endif()
  set(NEXUS_RUNTIME_WRAPPER "${NEXUS_SOURCE}/vendor/keryx-trace-abi6/tools/keryx-llama/keryx_llama.cpp")
  set(NEXUS_TRACE_PATCH "${NEXUS_SOURCE}/vendor/keryx-trace-abi6/tools/keryx-llama/patches/0001-canonical-trace.patch")
  set(NEXUS_PATCH_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/apply_trace_patch.cmake")
  set(NEXUS_PATCH_ARGUMENT "-DNEXUS_TRACE_PATCH=${NEXUS_TRACE_PATCH}")
  file(SHA256 "${NEXUS_TRACE_PATCH}" NEXUS_PATCH_SET_SHA256)
  set(NEXUS_RUNTIME_MINER_COMMIT "f2561aedeb649da7b2c46fc4e845e487c9a7d9e5")
  set(NEXUS_CANDIDATE_ONLY false)
endif()
FetchContent_Declare(nexus_llama
  URL https://github.com/ggml-org/llama.cpp/archive/12127defda4f41b7679cb2477a4b0d65ee6a0c8f.tar.gz
  URL_HASH SHA256=4542268daecd0b70e8e6dba964bd70d68993fdb933d7469f1dcf20ba4d60c952
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PATCH_COMMAND "${CMAKE_COMMAND}" "-DNEXUS_LLAMA_SOURCE=<SOURCE_DIR>"
    "${NEXUS_PATCH_ARGUMENT}" -P "${NEXUS_PATCH_SCRIPT}")
# An archive dependency must not inherit the parent Nexus Git identity in ggml's
# build-info probes. Its authoritative source pin is recorded in the sidecar.
if(FETCHCONTENT_SOURCE_DIR_NEXUS_LLAMA)
  get_filename_component(nexus_git_ceiling "${FETCHCONTENT_SOURCE_DIR_NEXUS_LLAMA}" DIRECTORY)
else()
  set(nexus_git_ceiling "${FETCHCONTENT_BASE_DIR}")
endif()
set(nexus_saved_git_ceiling "$ENV{GIT_CEILING_DIRECTORIES}")
set(ENV{GIT_CEILING_DIRECTORIES} "${nexus_git_ceiling}")
FetchContent_MakeAvailable(nexus_llama)
set(ENV{GIT_CEILING_DIRECTORIES} "${nexus_saved_git_ceiling}")
# Also validate pre-populated/overridden sources and repeated configuration.
execute_process(COMMAND "${CMAKE_COMMAND}" "-DNEXUS_LLAMA_SOURCE=${nexus_llama_SOURCE_DIR}"
  "${NEXUS_PATCH_ARGUMENT}" -DNEXUS_VERIFY_ONLY=ON -P "${NEXUS_PATCH_SCRIPT}"
  RESULT_VARIABLE nexus_patch_result)
if(NOT nexus_patch_result EQUAL 0)
  message(FATAL_ERROR "Canonical trace source verification failed")
endif()
add_library(nexus_inference_runtime SHARED
  "${NEXUS_RUNTIME_WRAPPER}")
set_target_properties(nexus_inference_runtime PROPERTIES
  OUTPUT_NAME keryx-llama CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(nexus_inference_runtime PRIVATE
  "${nexus_llama_SOURCE_DIR}/include" "${nexus_llama_SOURCE_DIR}/src"
  "${nexus_llama_SOURCE_DIR}/ggml/include" "${nexus_llama_SOURCE_DIR}/common")
target_link_libraries(nexus_inference_runtime PRIVATE llama ggml-cuda CUDA::cudart CUDA::cublas CUDA::cublasLt)
if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  # Linux otherwise permits a shared library with unresolved trace functions.
  target_link_options(nexus_inference_runtime PRIVATE "LINKER:--no-undefined")
endif()
install(TARGETS nexus_inference_runtime RUNTIME DESTINATION . COMPONENT nexus-runtime LIBRARY DESTINATION . COMPONENT nexus-runtime)

install(FILES "${nexus_llama_SOURCE_DIR}/LICENSE" DESTINATION licenses/llama COMPONENT nexus-runtime)
install(FILES "${NEXUS_SOURCE}/vendor/keryx-trace-abi6/LICENSE-APACHE" DESTINATION licenses/keryx-runtime COMPONENT nexus-runtime)

# A sidecar distinguishes the isolated ABI9 candidate from the current ABI8 package.
file(SHA256 "${NEXUS_RUNTIME_WRAPPER}" NEXUS_WRAPPER_SHA256)
set(nexus_runtime_metadata [=[{
  "application_abi": @NEXUS_RUNTIME_ABI@,
  "candidate_only": @NEXUS_CANDIDATE_ONLY@,
  "force_mmq": true,
  "build_policy_miner_commit": "7a9a39355a298250de28ce823a749a30e6f3405f",
  "wrapper_sha256": "@NEXUS_WRAPPER_SHA256@",
  "patch_set_sha256": "@NEXUS_PATCH_SET_SHA256@",
  "miner_commit": "@NEXUS_RUNTIME_MINER_COMMIT@",
  "llama_commit": "12127defda4f41b7679cb2477a4b0d65ee6a0c8f"
}
]=])
string(CONFIGURE "${nexus_runtime_metadata}" nexus_runtime_metadata @ONLY)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/NEXUS_RUNTIME.json" "${nexus_runtime_metadata}")
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/NEXUS_RUNTIME.json" DESTINATION . COMPONENT nexus-runtime)
