cmake_minimum_required(VERSION 3.25)
project(nexus_inference_runtime LANGUAGES C CXX CUDA)
include(FetchContent)
find_package(Git REQUIRED)
find_package(CUDAToolkit 13 REQUIRED)
set(CMAKE_CUDA_ARCHITECTURES 86)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_CUDA ON CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_CUDA_NCCL OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
get_filename_component(NEXUS_SOURCE "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
FetchContent_Declare(nexus_llama
  URL https://github.com/ggml-org/llama.cpp/archive/12127defda4f41b7679cb2477a4b0d65ee6a0c8f.tar.gz
  URL_HASH SHA256=4542268daecd0b70e8e6dba964bd70d68993fdb933d7469f1dcf20ba4d60c952
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  PATCH_COMMAND "${GIT_EXECUTABLE}" apply "${NEXUS_SOURCE}/vendor/keryx-trace-abi6/tools/keryx-llama/patches/0001-canonical-trace.patch")
FetchContent_MakeAvailable(nexus_llama)
add_library(nexus_inference_runtime SHARED
  "${NEXUS_SOURCE}/vendor/keryx-trace-abi6/tools/keryx-llama/keryx_llama.cpp")
set_target_properties(nexus_inference_runtime PROPERTIES
  OUTPUT_NAME keryx-llama CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(nexus_inference_runtime PRIVATE
  "${nexus_llama_SOURCE_DIR}/include" "${nexus_llama_SOURCE_DIR}/src"
  "${nexus_llama_SOURCE_DIR}/ggml/include" "${nexus_llama_SOURCE_DIR}/common")
target_link_libraries(nexus_inference_runtime PRIVATE llama ggml-cuda CUDA::cudart CUDA::cublas CUDA::cublasLt)
install(TARGETS nexus_inference_runtime RUNTIME DESTINATION . LIBRARY DESTINATION .)
