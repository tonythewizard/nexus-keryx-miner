cmake_minimum_required(VERSION 3.25)
project(nexus_keryx_miner VERSION 0.0.3 LANGUAGES C CXX)
set(NEXUS_RECOVERY_ROOT "${CMAKE_BINARY_DIR}/runtime" CACHE PATH "Private persistent runtime/test data")
set(NEXUS_PRIMARY_GPU_UUID "GPU-00000000-0000-0000-0000-000000000000" CACHE STRING "Explicit NVIDIA UUID; configure locally for GPU execution")
set(NEXUS_SECONDARY_GPU_ID "pci:0000:00:00.0" CACHE STRING "Explicit secondary PCI identity")
if(NOT NEXUS_PRIMARY_GPU_UUID MATCHES "^GPU-[0-9a-fA-F-]+$" OR NOT NEXUS_SECONDARY_GPU_ID MATCHES "^pci:[0-9a-fA-F:.]+$")
  message(FATAL_ERROR "Invalid device identity syntax")
endif()
add_compile_definitions(NEXUS_PRIMARY_GPU_UUID="${NEXUS_PRIMARY_GPU_UUID}" NEXUS_SECONDARY_GPU_ID="${NEXUS_SECONDARY_GPU_ID}")
set(NEXUS_TEST_MODEL "" CACHE FILEPATH "External authentic GGUF fixture")
set(NEXUS_TEST_TREE "" CACHE FILEPATH "External Merkle tree")
set(NEXUS_TEST_TREE_META "" CACHE FILEPATH "External Merkle metadata")
set(NEXUS_TEST_LAUNCHER "" CACHE FILEPATH "External test mining profile")
set(NEXUS_TEST_ESCROW_KEY "" CACHE FILEPATH "External test key; never a production key")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_assets.py"
  RESULT_VARIABLE NEXUS_ASSET_RESULT)
if(NOT NEXUS_ASSET_RESULT EQUAL 0)
  message(FATAL_ERROR "Source asset preparation failed")
endif()

option(NEXUS_ENABLE_GRPC "Build native gRPC ingress, AI and escrow components" OFF)
option(NEXUS_ENABLE_CUDA "Build the independent CUDA PoM backend" OFF)
option(NEXUS_ENABLE_VULKAN_JIT "Build experimental AMD Vulkan JIT support tools (no production admission)" OFF)
set(NEXUS_VULKAN_HEADERS "" CACHE PATH "Isolated official Vulkan-Headers include directory")
set(NEXUS_RUST_ORACLE "" CACHE FILEPATH "Built official Rust oracle executable")
set(NEXUS_LIVE_CAPTURE_DIR "" CACHE PATH "Persistent directory with verified captured templates")
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# PoM's CPU reference, canonical serialization and sparse Merkle paths are on
# the live critical path even when device execution is enabled. A single-config
# build with no build type must not silently run these loops at -O0. Keep all
# integrity checks; neither fast-math nor NDEBUG is added by this option.
option(NEXUS_OPTIMIZE_POM_HOST "Optimize the PoM host critical path" ON)
function(nexus_optimize_pom_host target)
  if(NEXUS_OPTIMIZE_POM_HOST AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE $<$<NOT:$<CONFIG:Debug>>:-O3>)
  endif()
endfunction()

if(WIN32)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  find_package(blake3 CONFIG REQUIRED)
  set(BLAKE3_LIBRARY BLAKE3::blake3)
  include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/blake2-reference.cmake")
else()
  find_path(BLAKE3_INCLUDE_DIR blake3.h REQUIRED)
  find_library(BLAKE3_LIBRARY NAMES blake3 REQUIRED)
  find_path(BLAKE2_INCLUDE_DIR blake2.h REQUIRED)
  find_library(BLAKE2_LIBRARY NAMES b2 REQUIRED)
endif()
function(nexus_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
  else()
    target_compile_options(${target} PRIVATE ${ARGN})
  endif()
endfunction()
find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Python3 COMPONENTS Interpreter QUIET)

if(NEXUS_ENABLE_VULKAN_JIT)
  find_path(NEXUS_VULKAN_INCLUDE vulkan/vulkan_core.h HINTS "${NEXUS_VULKAN_HEADERS}" REQUIRED)
  find_library(NEXUS_VULKAN_LOADER NAMES vulkan libvulkan.so.1 REQUIRED)
  add_executable(nexus-vulkan-device-info src/gpu/vulkan_device_info.cpp)
  target_include_directories(nexus-vulkan-device-info PRIVATE "${NEXUS_VULKAN_INCLUDE}")
  target_link_libraries(nexus-vulkan-device-info PRIVATE "${NEXUS_VULKAN_LOADER}")
  nexus_warnings(nexus-vulkan-device-info -Wall -Wextra -Wno-missing-field-initializers)
  add_executable(nexus-vulkan-offline-probe experiments/vulkan_jit/scatter_probe.cpp)
  target_include_directories(nexus-vulkan-offline-probe PRIVATE "${NEXUS_VULKAN_INCLUDE}")
  target_link_libraries(nexus-vulkan-offline-probe PRIVATE "${NEXUS_VULKAN_LOADER}" nexus_gpu_work)
  nexus_warnings(nexus-vulkan-offline-probe -Wall -Wextra -Wno-missing-field-initializers)
  add_executable(nexus-vulkan-proof-probe experiments/vulkan_jit/proof_probe.cpp)
  target_link_libraries(nexus-vulkan-proof-probe PRIVATE nexus_consensus)
  nexus_warnings(nexus-vulkan-proof-probe -Wall -Wextra)
  add_executable(nexus-vulkan-real-model-probe experiments/vulkan_jit/real_model_probe.cpp)
  target_link_libraries(nexus-vulkan-real-model-probe PRIVATE nexus_model)
  nexus_warnings(nexus-vulkan-real-model-probe -Wall -Wextra)
  add_library(nexus_gpu_work src/gpu/gpu_work_coordinator.cpp src/gpu/secondary_work_lane.cpp)
  target_link_libraries(nexus_gpu_work PUBLIC nexus_consensus)
  nexus_warnings(nexus_gpu_work -Wall -Wextra -Wpedantic)
  add_library(nexus_vulkan_walk src/gpu/vulkan_walk_engine.cpp src/gpu/vulkan_bda_resident_engine.cpp)
  target_include_directories(nexus_vulkan_walk PRIVATE "${NEXUS_VULKAN_INCLUDE}")
  target_link_libraries(nexus_vulkan_walk PUBLIC nexus_consensus PRIVATE "${NEXUS_VULKAN_LOADER}" OpenSSL::Crypto)
  nexus_warnings(nexus_vulkan_walk -Wall -Wextra -Wno-missing-field-initializers)
  # Explicit GPU validation executable; never run implicitly on a mining device.
  add_executable(nexus-vulkan-bda-validation-test tests/vulkan_bda_validation_test.cpp)
  target_link_libraries(nexus-vulkan-bda-validation-test PRIVATE nexus_vulkan_walk)
  add_executable(nexus-vulkan-banked-resident-probe experiments/vulkan_jit/banked_resident_probe.cpp)
  target_link_libraries(nexus-vulkan-banked-resident-probe PRIVATE nexus_vulkan_walk nexus_model)
  add_executable(nexus-vulkan-persistent-probe experiments/vulkan_jit/persistent_probe.cpp)
  target_link_libraries(nexus-vulkan-persistent-probe PRIVATE nexus_vulkan_walk nexus_gpu_work)
  add_executable(nexus-vulkan-ai-fixture-probe experiments/vulkan_jit/dual_ai_fixture_probe.cpp)
  target_include_directories(nexus-vulkan-ai-fixture-probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
  target_link_libraries(nexus-vulkan-ai-fixture-probe PRIVATE nexus_vulkan_walk nexus_gpu_work nexus_model)

endif()

if(NEXUS_ENABLE_CUDA)
  if(NOT NEXUS_ENABLE_GRPC)
    message(FATAL_ERROR "NEXUS_ENABLE_CUDA currently requires NEXUS_ENABLE_GRPC for captured-job binding")
  endif()
  enable_language(CUDA)
  if(NEXUS_PRIMARY_GPU_UUID STREQUAL "GPU-00000000-0000-0000-0000-000000000000")
    message(FATAL_ERROR "Configure NEXUS_PRIMARY_GPU_UUID explicitly for a CUDA build")
  endif()
  find_package(CUDAToolkit 13 REQUIRED)
  set(CMAKE_CUDA_STANDARD 20)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  set(CMAKE_CUDA_ARCHITECTURES 86)
endif()

add_library(nexus_consensus consensus/pom_v4.cpp)
nexus_optimize_pom_host(nexus_consensus)
target_include_directories(nexus_consensus PUBLIC include ${BLAKE3_INCLUDE_DIR} PRIVATE ${BLAKE2_INCLUDE_DIR})
target_link_libraries(nexus_consensus PUBLIC ${BLAKE3_LIBRARY} PRIVATE ${BLAKE2_LIBRARY})
nexus_warnings(nexus_consensus -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)

add_library(nexus_capture src/network/capture.cpp src/network/pipeline.cpp
  src/network/live_job_queue.cpp)
target_include_directories(nexus_capture PUBLIC include ${BLAKE3_INCLUDE_DIR})
target_link_libraries(nexus_capture PUBLIC ${BLAKE3_LIBRARY} Threads::Threads)
nexus_warnings(nexus_capture -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
target_link_libraries(nexus_consensus PUBLIC nexus_capture)

if(NEXUS_ENABLE_CUDA)
  find_program(NEXUS_OBJCOPY_EXECUTABLE NAMES objcopy REQUIRED)
  # The PoM host path uses the Driver API. Compile the unchanged device kernels
  # to an SM 8.6 cubin, embed it in the library, and launch via cuModule/cuLaunch
  # so the worker does not acquire a CUDA Runtime SONAME of its own.
  set(NEXUS_POM_CUBIN "${CMAKE_CURRENT_BINARY_DIR}/nexus_pom.cubin")
  set(NEXUS_POM_IMAGE_OBJECT "${CMAKE_CURRENT_BINARY_DIR}/nexus_pom_image.o")
  set(NEXUS_CUDA_TEMP "${CMAKE_CURRENT_BINARY_DIR}/persistent-tmp")
  file(MAKE_DIRECTORY "${NEXUS_CUDA_TEMP}")
  add_custom_command(OUTPUT "${NEXUS_POM_IMAGE_OBJECT}"
    COMMAND ${CMAKE_COMMAND} -E env
      "TMPDIR=${NEXUS_CUDA_TEMP}" "TMP=${NEXUS_CUDA_TEMP}" "TEMP=${NEXUS_CUDA_TEMP}"
      ${CMAKE_CUDA_COMPILER} --cubin --cudart=none -arch=sm_86
      "${CMAKE_CURRENT_SOURCE_DIR}/consensus/cuda_walk_kernels.cu"
      -o "${NEXUS_POM_CUBIN}"
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_CURRENT_BINARY_DIR}"
      ${NEXUS_OBJCOPY_EXECUTABLE} --input-target=binary --output-target=elf64-x86-64
      --binary-architecture=i386:x86-64 nexus_pom.cubin nexus_pom_image.o
    DEPENDS consensus/cuda_walk_kernels.cu
    VERBATIM)
  set_source_files_properties("${NEXUS_POM_IMAGE_OBJECT}" PROPERTIES
    GENERATED TRUE EXTERNAL_OBJECT TRUE)
  set(NEXUS_RESIDENT_IMAGE "${CMAKE_CURRENT_BINARY_DIR}/nexus_resident_image.o")
  add_custom_command(OUTPUT "${NEXUS_RESIDENT_IMAGE}"
    COMMAND ${CMAKE_COMMAND} -E env "TMPDIR=${NEXUS_CUDA_TEMP}" "TMP=${NEXUS_CUDA_TEMP}" "TEMP=${NEXUS_CUDA_TEMP}"
      ${CMAKE_CUDA_COMPILER} --cubin --cudart=none -arch=sm_86
      "${CMAKE_CURRENT_SOURCE_DIR}/consensus/cuda_resident_kernels.cu" -o "${CMAKE_CURRENT_BINARY_DIR}/nexus_resident.cubin"
    COMMAND ${CMAKE_COMMAND} -E chdir "${CMAKE_CURRENT_BINARY_DIR}"
      ${NEXUS_OBJCOPY_EXECUTABLE} --input-target=binary --output-target=elf64-x86-64
      --binary-architecture=i386:x86-64 nexus_resident.cubin nexus_resident_image.o
    DEPENDS consensus/cuda_resident_kernels.cu consensus/resident_keccak.cuh VERBATIM)
  set_source_files_properties("${NEXUS_RESIDENT_IMAGE}" PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
  add_library(nexus_cuda consensus/cuda_walk.cpp consensus/cuda_resident.cpp "${NEXUS_POM_IMAGE_OBJECT}" "${NEXUS_RESIDENT_IMAGE}")
  add_custom_target(nexus_pom_image DEPENDS "${NEXUS_POM_IMAGE_OBJECT}")
  add_dependencies(nexus_cuda nexus_pom_image)
  target_include_directories(nexus_cuda PUBLIC include)
  target_link_libraries(nexus_cuda PUBLIC CUDA::cuda_driver nexus_model)
endif()

add_library(nexus_model src/model/gguf_reader.cpp)
nexus_optimize_pom_host(nexus_model)
target_include_directories(nexus_model PUBLIC include ${BLAKE3_INCLUDE_DIR})
target_link_libraries(nexus_model PUBLIC nexus_consensus OpenSSL::Crypto)
nexus_warnings(nexus_model -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
add_executable(nexus-pom-host-replay tests/pom_host_replay.cpp)
target_link_libraries(nexus-pom-host-replay PRIVATE nexus_model)
add_executable(nexus-gguf-inspect src/model/gguf_main.cpp)
target_link_libraries(nexus-gguf-inspect PRIVATE nexus_model)

if(WIN32 AND NOT NEXUS_ENABLE_GRPC)
  include("${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/windows-tests.cmake")
  return()
endif()

if(NEXUS_ENABLE_GRPC)
  find_package(Protobuf CONFIG REQUIRED)
  find_package(gRPC CONFIG REQUIRED)
  find_package(CURL REQUIRED)
  find_package(ZLIB REQUIRED)
  find_program(CARGO_EXECUTABLE cargo REQUIRED)
  set(NEXUS_AI_BRIDGE_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/ai-bridge-target")
  if(WIN32)
    set(NEXUS_AI_BRIDGE_LIBRARY "${NEXUS_AI_BRIDGE_TARGET_DIR}/release/nexus_ai_response_bridge.lib")
  else()
    set(NEXUS_AI_BRIDGE_LIBRARY "${NEXUS_AI_BRIDGE_TARGET_DIR}/release/libnexus_ai_response_bridge.a")
  endif()
  set(NEXUS_AI_BRIDGE_CARGO_HOME "$ENV{CARGO_HOME}" CACHE PATH "Cargo cache")
  if(NOT NEXUS_AI_BRIDGE_CARGO_HOME)
    set(NEXUS_AI_BRIDGE_CARGO_HOME "${CMAKE_BINARY_DIR}/cargo-home")
  endif()
  set(NEXUS_PERSISTENT_TMP "${CMAKE_CURRENT_BINARY_DIR}/persistent-tmp")
  file(GLOB NEXUS_VERIFIED_INFERENCE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor/keryx-verifiable-inference/src/*.rs")
  file(GLOB NEXUS_TRACE_MANIFESTS CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/ai-response-bridge/assets/trace-manifests/*.bin")
  add_custom_command(OUTPUT "${NEXUS_AI_BRIDGE_LIBRARY}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NEXUS_AI_BRIDGE_TARGET_DIR}" "${NEXUS_AI_BRIDGE_CARGO_HOME}" "${NEXUS_PERSISTENT_TMP}"
    COMMAND ${CMAKE_COMMAND} -E env
      "CARGO_HOME=${NEXUS_AI_BRIDGE_CARGO_HOME}"
      "CARGO_TARGET_DIR=${NEXUS_AI_BRIDGE_TARGET_DIR}"
      "CARGO_BUILD_JOBS=1"
      "TMPDIR=${NEXUS_PERSISTENT_TMP}" "TMP=${NEXUS_PERSISTENT_TMP}" "TEMP=${NEXUS_PERSISTENT_TMP}"
      ${CARGO_EXECUTABLE} build --release --locked --manifest-path "${CMAKE_CURRENT_SOURCE_DIR}/ai-response-bridge/Cargo.toml"
    DEPENDS ai-response-bridge/Cargo.toml ai-response-bridge/Cargo.lock
      ai-response-bridge/src/verified_inference.rs ai-response-bridge/src/trace_producer.rs
      ai-response-bridge/src/trace_store.rs ai-response-bridge/src/trace_recovery.rs
      vendor/keryx-verifiable-inference/Cargo.toml ${NEXUS_VERIFIED_INFERENCE_SOURCES} ${NEXUS_TRACE_MANIFESTS}
      ai-response-bridge/build.rs ai-response-bridge/src/lib.rs
      vendor/keryx-miner-v057/src/escrow.rs vendor/keryx-miner-v057/proto/rpc.proto
      vendor/keryx-inference-v057/inference/Cargo.toml
      vendor/keryx-inference-v057/inference/src/ai_payload.rs
      vendor/keryx-inference-v057/inference/src/private.rs
      vendor/keryx-inference-v057/inference/src/lib.rs
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}" VERBATIM)
  add_custom_target(nexus_ai_response_bridge_build DEPENDS "${NEXUS_AI_BRIDGE_LIBRARY}")
  add_library(nexus_ai_response_bridge STATIC IMPORTED GLOBAL)
  set_target_properties(nexus_ai_response_bridge PROPERTIES IMPORTED_LOCATION "${NEXUS_AI_BRIDGE_LIBRARY}")
  add_dependencies(nexus_ai_response_bridge_build nexus_node_proto)
  set(PROTO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/proto/current")
  set(PROTO_GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
  file(MAKE_DIRECTORY "${PROTO_GEN}")
  add_custom_command(
    OUTPUT "${PROTO_GEN}/rpc.pb.cc" "${PROTO_GEN}/messages.pb.cc"
           "${PROTO_GEN}/messages.grpc.pb.cc"
           "${PROTO_GEN}/rpc.pb.h" "${PROTO_GEN}/messages.pb.h"
           "${PROTO_GEN}/messages.grpc.pb.h"
    COMMAND $<TARGET_FILE:protobuf::protoc>
      --proto_path=${PROTO_DIR} --cpp_out=${PROTO_GEN}
      --grpc_out=${PROTO_GEN} --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
      rpc.proto messages.proto
    DEPENDS "${PROTO_DIR}/rpc.proto" "${PROTO_DIR}/messages.proto"
    WORKING_DIRECTORY "${PROTO_DIR}" VERBATIM)
  add_library(nexus_node_proto
    "${PROTO_GEN}/rpc.pb.cc" "${PROTO_GEN}/messages.pb.cc" "${PROTO_GEN}/messages.grpc.pb.cc")
  target_include_directories(nexus_node_proto PUBLIC "${PROTO_GEN}")
  target_link_libraries(nexus_node_proto PUBLIC protobuf::libprotobuf gRPC::grpc++)

  add_library(nexus_grpc src/network/grpc_client.cpp src/network/grpc_header.cpp
    src/network/ai_reorg_reconciliation.cpp
    src/network/submit_block_client.cpp src/network/submission_pipeline.cpp
    src/network/opoi.cpp src/network/trace_capture.cpp src/network/ai_service.cpp src/network/ai_response_service.cpp
    src/network/escrow_watcher.cpp src/network/strike_monitor.cpp)
  add_dependencies(nexus_grpc nexus_ai_response_bridge_build)
  target_include_directories(nexus_grpc PUBLIC include PRIVATE "${PROTO_GEN}")
  target_link_libraries(nexus_grpc PUBLIC nexus_capture nexus_node_proto nexus_consensus gRPC::grpc++
    OpenSSL::Crypto ${BLAKE2_LIBRARY} ${CMAKE_DL_LIBS} CURL::libcurl ZLIB::ZLIB
    nexus_ai_response_bridge Threads::Threads ${CMAKE_DL_LIBS})
  if(WIN32)
    target_link_libraries(nexus_grpc PUBLIC ws2_32 userenv bcrypt ntdll advapi32 secur32 crypt32)
  else()
    target_link_libraries(nexus_grpc PUBLIC m rt util)
  endif()
  nexus_warnings(nexus_grpc -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion)
  if(NEXUS_ENABLE_VULKAN_JIT)
    add_library(nexus_amd_candidate src/gpu/amd_candidate_pipeline.cpp)
    target_include_directories(nexus_amd_candidate PRIVATE "${PROTO_GEN}")
    target_link_libraries(nexus_amd_candidate PUBLIC nexus_grpc nexus_model nexus_vulkan_walk nexus_gpu_work)
    add_executable(nexus-amd-candidate-probe experiments/vulkan_jit/candidate_probe.cpp)
    target_include_directories(nexus-amd-candidate-probe PRIVATE "${PROTO_GEN}")
    target_link_libraries(nexus-amd-candidate-probe PRIVATE nexus_amd_candidate)
    add_executable(nexus-nvidia-vulkan-workflow-test tests/nvidia_vulkan_workflow_test.cpp)
    target_include_directories(nexus-nvidia-vulkan-workflow-test PRIVATE "${PROTO_GEN}")
    target_link_libraries(nexus-nvidia-vulkan-workflow-test PRIVATE nexus_amd_candidate)
  endif()
  add_executable(keryx-nexus-live src/network/live_main.cpp)
  target_link_libraries(keryx-nexus-live PRIVATE nexus_grpc)
  add_executable(keryx-nexus-replay src/network/replay_main.cpp)
  target_include_directories(keryx-nexus-replay PRIVATE "${PROTO_GEN}")
  target_link_libraries(keryx-nexus-replay PRIVATE nexus_capture nexus_node_proto)

  add_executable(nexus-live-header-capture-check tests/live_header_capture_check.cpp)
  target_link_libraries(nexus-live-header-capture-check PRIVATE nexus_grpc)
  add_executable(nexus-real-model-proof src/model/real_model_proof.cpp)
  target_include_directories(nexus-real-model-proof PRIVATE "${PROTO_GEN}")
  target_link_libraries(nexus-real-model-proof PRIVATE nexus_grpc nexus_model)
  if(NEXUS_ENABLE_CUDA)
    add_executable(nexus-cuda-real-model src/model/cuda_real_model.cpp)
    target_include_directories(nexus-cuda-real-model PRIVATE "${PROTO_GEN}")
    target_link_libraries(nexus-cuda-real-model PRIVATE nexus_cuda nexus_grpc nexus_model)
    add_executable(nexus-live-cuda-worker src/model/live_cuda_worker.cpp)
    if(NEXUS_ENABLE_VULKAN_JIT)
      target_compile_definitions(nexus-live-cuda-worker PRIVATE NEXUS_ENABLE_SECONDARY_VULKAN=1)
      target_link_libraries(nexus-live-cuda-worker PRIVATE nexus_amd_candidate)
    endif()
    target_compile_definitions(nexus-live-cuda-worker PRIVATE
      NEXUS_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
      NEXUS_RECOVERY_ROOT="${NEXUS_RECOVERY_ROOT}")
    target_include_directories(nexus-live-cuda-worker PRIVATE "${PROTO_GEN}")
    target_link_libraries(nexus-live-cuda-worker PRIVATE nexus_cuda nexus_grpc nexus_model)
    add_executable(nexus-opoi-preemption-cuda-test tests/opoi_preemption_cuda_test.cpp)
    target_include_directories(nexus-opoi-preemption-cuda-test PRIVATE "${PROTO_GEN}")
    target_link_libraries(nexus-opoi-preemption-cuda-test PRIVATE nexus_cuda nexus_grpc nexus_model)
    add_executable(nexus-opoi-reference-runner tests/opoi_reference_runner.cpp)
    target_include_directories(nexus-opoi-reference-runner PRIVATE include)
    target_link_libraries(nexus-opoi-reference-runner PRIVATE nexus_grpc CUDA::cudart OpenSSL::Crypto ${CMAKE_DL_LIBS})
    add_executable(nexus-full-vram-probe src/model/full_vram_probe.cu)
    target_link_libraries(nexus-full-vram-probe PRIVATE CUDA::cudart OpenSSL::Crypto)
    set_target_properties(nexus-full-vram-probe PROPERTIES CUDA_ARCHITECTURES 86)
  endif()
  if(NEXUS_LIVE_CAPTURE_DIR AND EXISTS "${NEXUS_LIVE_CAPTURE_DIR}/live-mainnet.capture")
    add_test(NAME live_template_header_consensus COMMAND nexus-live-header-capture-check
      "${CMAKE_BINARY_DIR}/test-data/live-header-vectors.bin"
      "${NEXUS_LIVE_CAPTURE_DIR}/live-mainnet.capture"
      "${NEXUS_LIVE_CAPTURE_DIR}/live-reconnect.capture"
      "${NEXUS_LIVE_CAPTURE_DIR}/live-validation.capture")
    if(NEXUS_RUST_ORACLE AND EXISTS "${NEXUS_RUST_ORACLE}")
      add_test(NAME official_rust_live_template_parity COMMAND "${NEXUS_RUST_ORACLE}" header-live
        "${CMAKE_BINARY_DIR}/test-data/live-header-vectors.bin")
      set_tests_properties(official_rust_live_template_parity PROPERTIES DEPENDS live_template_header_consensus)
    endif()
  endif()
  add_executable(nexus-grpc-mock tests/grpc_mock_node.cpp)
  target_include_directories(nexus-grpc-mock PRIVATE "${PROTO_GEN}")
  target_link_libraries(nexus-grpc-mock PRIVATE nexus_capture nexus_node_proto gRPC::grpc++)
  add_executable(nexus-worker-mode-test tests/worker_mode_test.cpp)
  target_include_directories(nexus-worker-mode-test PRIVATE include)
  add_executable(nexus-live-worker-config-test tests/live_worker_config_test.cpp)
  target_include_directories(nexus-live-worker-config-test PRIVATE include)
  target_link_libraries(nexus-live-worker-config-test PRIVATE Threads::Threads)
  add_test(NAME live_worker_configuration_fail_closed COMMAND nexus-live-worker-config-test)
  add_executable(nexus-primary-vulkan-policy-test tests/primary_vulkan_policy_test.cpp)
  target_include_directories(nexus-primary-vulkan-policy-test PRIVATE include)
  target_link_libraries(nexus-primary-vulkan-policy-test PRIVATE Threads::Threads)
  add_test(NAME primary_vulkan_workflow_no_cuda_fallback COMMAND nexus-primary-vulkan-policy-test)

  add_executable(nexus-startup-readiness-test tests/startup_readiness_test.cpp)
  target_include_directories(nexus-startup-readiness-test PRIVATE include)
  add_test(NAME inference_safe_startup_state_machine COMMAND nexus-startup-readiness-test)
  add_executable(nexus-ai-service-test tests/ai_service_test.cpp)
  target_link_libraries(nexus-ai-service-test PRIVATE nexus_grpc)
  add_test(NAME ai_service_payload_assignment_journal COMMAND nexus-ai-service-test
    "${CMAKE_BINARY_DIR}/test-data/ai-service-journal")
  add_executable(nexus-ai-response-service-test tests/ai_response_service_test.cpp)
  target_link_libraries(nexus-ai-response-service-test PRIVATE nexus_grpc)
  if(NEXUS_ENABLE_CUDA)
    target_link_libraries(nexus-ai-response-service-test PRIVATE nexus_cuda nexus_model CUDA::cuda_driver)
    target_compile_definitions(nexus-ai-response-service-test PRIVATE NEXUS_TEST_HAS_CUDA=1)
    if(NEXUS_ENABLE_VULKAN_JIT)
      target_compile_definitions(nexus-ai-response-service-test PRIVATE NEXUS_TEST_HAS_VULKAN=1)
      target_link_libraries(nexus-ai-response-service-test PRIVATE nexus_vulkan_walk nexus_gpu_work)
    endif()
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME ai_response_public_private_submit_recovery
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ai_response_service_test.py"
        "$<TARGET_FILE:nexus-ai-response-service-test>" "${CARGO_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/ai-response-bridge/Cargo.toml"
        "${NEXUS_AI_BRIDGE_CARGO_HOME}" "${NEXUS_AI_BRIDGE_TARGET_DIR}"
        "${CMAKE_BINARY_DIR}/test-data/ai-response-e2e")
    set_tests_properties(ai_response_public_private_submit_recovery PROPERTIES
      TIMEOUT 180
      ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-data/ai-response-e2e/tmp;TMP=${CMAKE_BINARY_DIR}/test-data/ai-response-e2e/tmp;TEMP=${CMAKE_BINARY_DIR}/test-data/ai-response-e2e/tmp")
    add_test(NAME cuda_integration_exclusive_process_identity
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/cuda_test_isolation_test.py")
    add_test(NAME abi6_capture_runner_fail_closed
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/abi6_capture_runner_test.py")
    set_tests_properties(abi6_capture_runner_fail_closed PROPERTIES
      ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-data/ai-response-e2e/tmp;TMP=${CMAKE_BINARY_DIR}/test-data/ai-response-e2e/tmp;TEMP=${CMAKE_BINARY_DIR}/test-data/ai-response-e2e/tmp")
    if(NEXUS_ENABLE_CUDA)



      set(NEXUS_AI_PREEMPTION_TRACE
        "${NEXUS_RECOVERY_ROOT}/fixtures/preemption.trace")
      set(NEXUS_AI_PREEMPTION_CONTEXT
        "${NEXUS_RECOVERY_ROOT}/fixtures/preemption.context")
      set(NEXUS_AI_PREEMPTION_ORACLE "${NEXUS_RUST_ORACLE}")
      # The test requires the stock v0.5.7 wrapper ABI (keryx_llama_*), not
      # llama.cpp's generic libllama.so. Keep the stock library path so its
      # $ORIGIN RUNPATH and adjacent CUDA 12 runtime remain a single closure.
      set(NEXUS_AI_PREEMPTION_LLAMA
        "${CMAKE_BINARY_DIR}/official-stock-runtime/libkeryx-llama.so")
      if(NEXUS_AI_PREEMPTION_LLAMA AND EXISTS "${NEXUS_TEST_MODEL}" AND
         EXISTS "${NEXUS_TEST_TREE}" AND EXISTS "${NEXUS_TEST_TREE_META}" AND
         EXISTS "${NEXUS_AI_PREEMPTION_TRACE}" AND EXISTS "${NEXUS_AI_PREEMPTION_CONTEXT}" AND
         EXISTS "${NEXUS_AI_PREEMPTION_ORACLE}")
        set(NEXUS_AI_PREEMPTION_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-data/ai-pom-preemption")
        add_test(NAME ai_request_public_private_during_active_pom_cuda
          COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_ai_response_service_test.py"
            "$<TARGET_FILE:nexus-ai-response-service-test>" "${CARGO_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/ai-response-bridge/Cargo.toml"
            "${NEXUS_AI_BRIDGE_CARGO_HOME}" "${NEXUS_AI_BRIDGE_TARGET_DIR}"
            "${NEXUS_AI_PREEMPTION_DIR}"
            --real-inference "${NEXUS_TEST_MODEL}" "${NEXUS_AI_PREEMPTION_LLAMA}"
            --grpc-capture "${NEXUS_LIVE_CAPTURE_DIR}/live-mainnet.capture"
            --pom-cuda "${NEXUS_TEST_TREE}" "${NEXUS_TEST_TREE_META}"
              "${NEXUS_AI_PREEMPTION_TRACE}" "${NEXUS_AI_PREEMPTION_CONTEXT}"
              "${NEXUS_AI_PREEMPTION_ORACLE}" "${NEXUS_AI_PREEMPTION_DIR}/proofs")
        set_tests_properties(ai_request_public_private_during_active_pom_cuda PROPERTIES
          TIMEOUT 1200 LABELS "cuda;ai-service;real-inference;rust-proof"
          ENVIRONMENT "CUDA_VISIBLE_DEVICES=${NEXUS_PRIMARY_GPU_UUID};KERYX_LLAMA_CTX=8192;TMPDIR=${NEXUS_AI_PREEMPTION_DIR}/tmp;TMP=${NEXUS_AI_PREEMPTION_DIR}/tmp;TEMP=${NEXUS_AI_PREEMPTION_DIR}/tmp")
      else()
        message(STATUS "Skipping real AiRequest/PoM CUDA integration test: required authenticated fixture/model/runtime/oracle unavailable")
      endif()
    endif()
  endif()
  add_executable(nexus-ipfs-transport-test tests/ipfs_transport_test.cpp)
  target_link_libraries(nexus-ipfs-transport-test PRIVATE nexus_grpc)
  if(Python3_Interpreter_FOUND)
    add_test(NAME ai_ipfs_unixfs_integrity_and_keepalive
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ipfs_transport_mock.py"
        "$<TARGET_FILE:nexus-ipfs-transport-test>")
    set_tests_properties(ai_ipfs_unixfs_integrity_and_keepalive PROPERTIES TIMEOUT 30)
  endif()
  add_executable(nexus-ai-service-rpc-test tests/ai_service_rpc_test.cpp)
  target_link_libraries(nexus-ai-service-rpc-test PRIVATE nexus_grpc)
  add_executable(nexus-escrow-status-probe src/model/escrow_status_probe.cpp)
  target_link_libraries(nexus-escrow-status-probe PRIVATE nexus_grpc)
  add_executable(nexus-ai-service-probe src/network/ai_service_probe_main.cpp)
  target_link_libraries(nexus-ai-service-probe PRIVATE nexus_grpc)
  add_executable(nexus-ai-backfill-audit src/network/ai_backfill_audit_main.cpp)
  target_link_libraries(nexus-ai-backfill-audit PRIVATE nexus_grpc)
  add_executable(nexus-ai-journal-archive-test tests/ai_journal_archive_test.cpp)
  target_link_libraries(nexus-ai-journal-archive-test PRIVATE nexus_grpc)
  add_test(NAME ai_journal_archive_canonical_identity_and_deadline
    COMMAND nexus-ai-journal-archive-test)
  add_executable(nexus-ai-backfill-audit-test tests/ai_backfill_audit_test.cpp)
  target_link_libraries(nexus-ai-backfill-audit-test PRIVATE nexus_grpc)
  add_test(NAME ai_backfill_read_only_stream_and_bounded_ancestors
    COMMAND nexus-ai-backfill-audit-test)
  set_tests_properties(ai_backfill_read_only_stream_and_bounded_ancestors PROPERTIES TIMEOUT 15)
  add_executable(nexus-ai-service-ingress-test tests/ai_service_ingress_test.cpp)
  target_link_libraries(nexus-ai-service-ingress-test PRIVATE nexus_grpc)
  if(Python3_Interpreter_FOUND AND NEXUS_LIVE_CAPTURE_DIR AND
     EXISTS "${NEXUS_LIVE_CAPTURE_DIR}/live-mainnet.capture")
    add_test(NAME ai_service_official_block_backfill_mock
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_service_ingress_mock.py"
        "$<TARGET_FILE:nexus-ai-service-ingress-test>" "$<TARGET_FILE:nexus-grpc-mock>"
        "${NEXUS_LIVE_CAPTURE_DIR}/live-mainnet.capture"
        "${CMAKE_BINARY_DIR}/test-data/ai-service-ingress")
    set_tests_properties(ai_service_official_block_backfill_mock PROPERTIES
      ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-data/ai-service-ingress/tmp;TMP=${CMAKE_BINARY_DIR}/test-data/ai-service-ingress/tmp;TEMP=${CMAKE_BINARY_DIR}/test-data/ai-service-ingress/tmp"
      TIMEOUT 25)
  endif()
  if(Python3_Interpreter_FOUND)
    add_test(NAME ai_service_official_rpc_mock
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/ai_service_rpc_mock.py"
        "$<TARGET_FILE:nexus-ai-service-rpc-test>" "$<TARGET_FILE:nexus-grpc-mock>"
        "${CMAKE_BINARY_DIR}/test-data/ai-service-rpc")
    set_tests_properties(ai_service_official_rpc_mock PROPERTIES
      ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-data/ai-service-rpc/tmp;TMP=${CMAKE_BINARY_DIR}/test-data/ai-service-rpc/tmp;TEMP=${CMAKE_BINARY_DIR}/test-data/ai-service-rpc/tmp"
      TIMEOUT 20)
  endif()
endif()

include(CTest)
if(BUILD_TESTING)
  add_executable(nexus-vulkan-bounded-upload-test tests/vulkan_bounded_upload_test.cpp)
  add_test(NAME bounded_native_inference_tensor_upload COMMAND nexus-vulkan-bounded-upload-test)
  add_executable(nexus-bounded-file-sha256-test tests/bounded_file_sha256_test.cpp)
  target_include_directories(nexus-bounded-file-sha256-test PRIVATE include)
  target_link_libraries(nexus-bounded-file-sha256-test PRIVATE OpenSSL::Crypto)
  add_test(NAME bounded_full_file_sha256_direct_and_buffered
    COMMAND nexus-bounded-file-sha256-test "${CMAKE_BINARY_DIR}/test-data/bounded-sha256")
  add_executable(nexus-vulkan-inference-process-test tests/vulkan_inference_process_test.cpp)
  target_include_directories(nexus-vulkan-inference-process-test PRIVATE include)
  target_link_libraries(nexus-vulkan-inference-process-test PRIVATE OpenSSL::Crypto Threads::Threads)
  add_test(NAME vulkan_inference_child_transport_and_lease
    COMMAND nexus-vulkan-inference-process-test "${CMAKE_BINARY_DIR}/test-data/vulkan-child")
  set_tests_properties(vulkan_inference_child_transport_and_lease PROPERTIES TIMEOUT 15)
  add_executable(nexus-scoped-file-cache-guard-test tests/scoped_file_cache_guard_test.cpp)
  target_include_directories(nexus-scoped-file-cache-guard-test PRIVATE include)
  target_link_libraries(nexus-scoped-file-cache-guard-test PRIVATE Threads::Threads)
  add_executable(nexus-submission-concurrency-test tests/submission_concurrency_test.cpp)
  target_include_directories(nexus-submission-concurrency-test PRIVATE include)
  target_link_libraries(nexus-submission-concurrency-test PRIVATE Threads::Threads)
  add_test(NAME submission_concurrent_deduplication COMMAND nexus-submission-concurrency-test)
  if(NEXUS_ENABLE_VULKAN_JIT)
    add_executable(nexus-gpu-work-coordinator-test tests/gpu_work_coordinator_test.cpp)
    target_link_libraries(nexus-gpu-work-coordinator-test PRIVATE nexus_gpu_work)
    add_test(NAME dual_gpu_nonce_ownership_and_stale_recovery COMMAND nexus-gpu-work-coordinator-test)
    set_tests_properties(dual_gpu_nonce_ownership_and_stale_recovery PROPERTIES TIMEOUT 20)
    if(UNIX AND NOT APPLE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      add_executable(nexus-shared-gpu-job-test tests/shared_gpu_job_test.cpp)
      target_link_libraries(nexus-shared-gpu-job-test PRIVATE nexus_gpu_work)
      target_link_options(nexus-shared-gpu-job-test PRIVATE "-Wl,--wrap=_Znwm")
      add_test(NAME immutable_gpu_job_snapshot_and_allocation_failure COMMAND nexus-shared-gpu-job-test)
    endif()
    add_executable(nexus-secondary-work-lane-test tests/secondary_work_lane_test.cpp)
    target_link_libraries(nexus-secondary-work-lane-test PRIVATE nexus_gpu_work)
    add_test(NAME secondary_lane_pause_drain_and_shutdown COMMAND nexus-secondary-work-lane-test)
    set_tests_properties(secondary_lane_pause_drain_and_shutdown PROPERTIES TIMEOUT 20)
  endif()
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-data")
  if(Python3_Interpreter_FOUND)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-data/banked-cpu/tmp")
    add_test(NAME banked_cuda_runtime_source_specialization
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/banked_source_test.py")
    add_test(NAME agent_diagnostics_session_claims
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/agent_diagnostics_test.py")
    set_tests_properties(banked_cuda_runtime_source_specialization agent_diagnostics_session_claims PROPERTIES
      ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-data/banked-cpu/tmp;TMP=${CMAKE_BINARY_DIR}/test-data/banked-cpu/tmp;TEMP=${CMAKE_BINARY_DIR}/test-data/banked-cpu/tmp;PYTHONDONTWRITEBYTECODE=1"
      TIMEOUT 20)
  endif()
  add_executable(nexus-consensus-tests tests/consensus_test.cpp)
  target_link_libraries(nexus-consensus-tests PRIVATE nexus_consensus)
  if(NEXUS_RUST_ORACLE AND EXISTS "${NEXUS_RUST_ORACLE}")
  add_test(NAME consensus_cpu_vectors COMMAND nexus-consensus-tests
    "${CMAKE_BINARY_DIR}/test-data/official-v4-fixture.bin"
    "${CMAKE_BINARY_DIR}/test-data/cpp-proof.bin")
  endif()
  if(NEXUS_RUST_ORACLE AND EXISTS "${NEXUS_RUST_ORACLE}")
    add_test(NAME official_pom_fixture_generate COMMAND "${NEXUS_RUST_ORACLE}" generate
      "${CMAKE_BINARY_DIR}/test-data/official-v4-fixture.bin")
    set_tests_properties(official_pom_fixture_generate PROPERTIES FIXTURES_SETUP nexus_official_pom_fixture)
    set_tests_properties(consensus_cpu_vectors PROPERTIES FIXTURES_REQUIRED nexus_official_pom_fixture)
  endif()
  if(NEXUS_ENABLE_GRPC)
  add_executable(nexus-block-task-header-test tests/block_task_header_test.cpp)
  target_link_libraries(nexus-block-task-header-test PRIVATE nexus_grpc)
  add_test(NAME block_task_header_compatibility COMMAND nexus-block-task-header-test)
  add_executable(nexus-header-parity-test tests/header_parity_test.cpp)
  target_link_libraries(nexus-header-parity-test PRIVATE nexus_grpc)
  add_test(NAME official_header_target_seed_parity COMMAND nexus-header-parity-test
    "${CMAKE_BINARY_DIR}/test-data/official-header.bin")
  if(NEXUS_RUST_ORACLE AND EXISTS "${NEXUS_RUST_ORACLE}")
    add_test(NAME official_header_vector_generate COMMAND "${NEXUS_RUST_ORACLE}" header
      "${CMAKE_BINARY_DIR}/test-data/official-header.bin")
    set_tests_properties(official_header_vector_generate PROPERTIES FIXTURES_SETUP nexus_official_header_vector)
    set_tests_properties(official_header_target_seed_parity PROPERTIES FIXTURES_REQUIRED nexus_official_header_vector)
  endif()
  if(NEXUS_RUST_ORACLE AND EXISTS "${NEXUS_RUST_ORACLE}")
    add_test(NAME official_rust_container_verify COMMAND "${NEXUS_RUST_ORACLE}" verify
      "${CMAKE_BINARY_DIR}/test-data/cpp-proof.bin")
    set_tests_properties(official_rust_container_verify PROPERTIES DEPENDS consensus_cpu_vectors)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME official_rust_negative_proofs COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/oracle_negative_suite.py" "${NEXUS_RUST_ORACLE}"
      "${CMAKE_BINARY_DIR}/test-data/cpp-proof.bin" "${CMAKE_BINARY_DIR}/test-data")
    set_tests_properties(official_rust_negative_proofs PROPERTIES DEPENDS consensus_cpu_vectors)
  endif()
  endif()
  add_executable(nexus-pipeline-tests tests/pipeline_test.cpp)
  target_link_libraries(nexus-pipeline-tests PRIVATE nexus_capture)
  add_test(NAME job_generation_and_stale_lease COMMAND nexus-pipeline-tests)
  add_executable(nexus-active-template-job-test tests/active_template_job_test.cpp)
  target_link_libraries(nexus-active-template-job-test PRIVATE nexus_capture)
  add_test(NAME active_template_job_ownership_and_refresh COMMAND nexus-active-template-job-test)
  if(NEXUS_ENABLE_GRPC)
    add_executable(nexus-rpc-template-cache-test tests/rpc_template_cache_test.cpp)
    target_link_libraries(nexus-rpc-template-cache-test PRIVATE nexus_grpc)
    add_test(NAME rpc_template_cache_exact_wire_and_recovery COMMAND nexus-rpc-template-cache-test)
    add_executable(nexus-prepared-rpc-template-test tests/prepared_rpc_template_test.cpp)
    target_link_libraries(nexus-prepared-rpc-template-test PRIVATE nexus_grpc)
    if(NEXUS_LIVE_CAPTURE_DIR AND EXISTS "${NEXUS_LIVE_CAPTURE_DIR}/live-mainnet.capture")
      add_test(NAME prepared_template_exact_header_and_target COMMAND nexus-prepared-rpc-template-test
        "${NEXUS_LIVE_CAPTURE_DIR}/live-mainnet.capture")
    endif()
  endif()
  add_executable(nexus-live-job-queue-test tests/live_job_queue_test.cpp)
  target_link_libraries(nexus-live-job-queue-test PRIVATE nexus_capture)
  add_test(NAME live_job_queue_generation_and_cancellation COMMAND nexus-live-job-queue-test)
  add_executable(nexus-native-telemetry-test tests/native_telemetry_test.cpp)
  target_include_directories(nexus-native-telemetry-test PRIVATE include)
  add_test(NAME native_pom_telemetry_fresh_rate_and_windows COMMAND nexus-native-telemetry-test)
  add_executable(nexus-gpu-preemption-test tests/gpu_preemption_test.cpp)
  target_include_directories(nexus-gpu-preemption-test PRIVATE include)
  target_link_libraries(nexus-gpu-preemption-test PRIVATE Threads::Threads)
  add_test(NAME gpu_opoi_preemption_lifecycle COMMAND nexus-gpu-preemption-test)
  if(NEXUS_ENABLE_GRPC)
    add_executable(nexus-opoi-protocol-test tests/opoi_protocol_test.cpp)
  add_executable(nexus-trace-capture-test tests/trace_capture_test.cpp src/network/trace_capture.cpp)
  target_include_directories(nexus-trace-capture-test PRIVATE include)
  add_test(NAME abi6_trace_capture_bounds_and_replay COMMAND nexus-trace-capture-test)
  add_executable(nexus-trace-stage2-adapter-test tests/trace_stage2_adapter_test.cpp src/network/trace_capture.cpp)
  target_include_directories(nexus-trace-stage2-adapter-test PRIVATE include)
  add_test(NAME abi8_trace_capture_bounds_and_single_replay COMMAND nexus-trace-stage2-adapter-test)
  # Explicit offline fixture target; never admitted automatically as a CPU test.
  add_executable(nexus-abi6-capture-probe tests/abi6_capture_probe.cpp)
  target_link_libraries(nexus-abi6-capture-probe PRIVATE nexus_grpc)
    target_link_libraries(nexus-opoi-protocol-test PRIVATE nexus_grpc)
    add_test(NAME official_opoi_challenge_format COMMAND nexus-opoi-protocol-test)
    add_executable(nexus-template-ownership-equivalence-test tests/template_ownership_equivalence_test.cpp)
    target_link_libraries(nexus-template-ownership-equivalence-test PRIVATE nexus_grpc)
    add_test(NAME template_ownership_exact_identity_equivalence COMMAND nexus-template-ownership-equivalence-test)
    add_executable(nexus-live-grpc-queue-test tests/live_grpc_queue_test.cpp)
    target_link_libraries(nexus-live-grpc-queue-test PRIVATE nexus_grpc)
    add_test(NAME direct_grpc_two_job_memory_queue COMMAND nexus-live-grpc-queue-test)
    set_tests_properties(direct_grpc_two_job_memory_queue PROPERTIES TIMEOUT 20)
    add_executable(nexus-canonical-job-identity-test tests/canonical_job_identity_test.cpp)
    target_link_libraries(nexus-canonical-job-identity-test PRIVATE nexus_grpc)
    add_test(NAME canonical_consensus_job_fingerprint COMMAND nexus-canonical-job-identity-test)
  endif()
  if(NEXUS_ENABLE_GRPC)
  add_executable(nexus-capture-tests tests/capture_test.cpp)
  target_include_directories(nexus-capture-tests PRIVATE "${PROTO_GEN}")
  target_link_libraries(nexus-capture-tests PRIVATE nexus_capture nexus_node_proto)
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-data")
  add_test(NAME blake3_capture_roundtrip COMMAND nexus-capture-tests "${CMAKE_BINARY_DIR}/test-data/blake3-roundtrip.capture")
  endif()
  add_executable(nexus-cpu-walk-dispatch-test tests/cpu_walk_dispatch_test.cpp)
  target_link_libraries(nexus-cpu-walk-dispatch-test PRIVATE nexus_consensus)
  add_test(NAME cpu_walk_dispatch_signed_byte_parity COMMAND nexus-cpu-walk-dispatch-test)
  add_executable(nexus-borsh-v4-size-test tests/borsh_v4_size_test.cpp)
  target_link_libraries(nexus-borsh-v4-size-test PRIVATE nexus_consensus)
  add_test(NAME borsh_v4_exact_size_and_field_parity COMMAND nexus-borsh-v4-size-test)
  add_executable(nexus-gguf-tests tests/gguf_reader_test.cpp)
  target_link_libraries(nexus-gguf-tests PRIVATE nexus_model)
  add_test(NAME gguf_read_only_parser_and_merkle COMMAND nexus-gguf-tests "${CMAKE_BINARY_DIR}/test-data/gguf-reader")
endif()

find_package(Python3 COMPONENTS Interpreter QUIET)
if(BUILD_TESTING AND NEXUS_ENABLE_GRPC)
  add_executable(nexus-escrow-claim-mock-test tests/escrow_claim_mock_test.cpp)
  target_link_libraries(nexus-escrow-claim-mock-test PRIVATE nexus_grpc)
  add_test(NAME escrow_claim_mock COMMAND nexus-escrow-claim-mock-test "${CMAKE_BINARY_DIR}/test-data/escrow-claim-mock")
  add_test(NAME worker_submission_mode_policy COMMAND nexus-worker-mode-test)
  if(TARGET nexus-live-cuda-worker AND Python3_Interpreter_FOUND)
    add_test(NAME worker_binary_submission_boundary
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/worker_binary_submission_boundary_test.py"
              "$<TARGET_FILE:nexus-live-cuda-worker>")
  endif()
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND AND NEXUS_ENABLE_GRPC)
  add_executable(nexus-node-stream-liveness-test tests/node_stream_liveness_test.cpp)
  target_include_directories(nexus-node-stream-liveness-test PRIVATE include)
  target_link_libraries(nexus-node-stream-liveness-test PRIVATE Threads::Threads)
  add_test(NAME node_stream_liveness_policy COMMAND nexus-node-stream-liveness-test)
  add_executable(nexus-grpc-silent-node-test tests/grpc_silent_node_test.cpp)
  target_link_libraries(nexus-grpc-silent-node-test PRIVATE nexus_grpc)
  add_test(NAME grpc_silent_node_recovery COMMAND nexus-grpc-silent-node-test)
  set_tests_properties(grpc_silent_node_recovery PROPERTIES TIMEOUT 120)
  add_test(NAME nexus_launcher_submission_policy
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nexus_launcher_policy_test.py")
  set_tests_properties(nexus_launcher_submission_policy PROPERTIES
    ENVIRONMENT "NEXUS_TEST_RECOVERY_ROOT=${NEXUS_RECOVERY_ROOT}")
  foreach(name refinement escrow_readers restart_workers updates notifications)
    set(temporary "${CMAKE_BINARY_DIR}/test-data/${name}/tmp")
    file(MAKE_DIRECTORY "${temporary}")
    add_test(NAME nexus_${name}_regression
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nexus_${name}_test.py")
    set_tests_properties(nexus_${name}_regression PROPERTIES TIMEOUT 30
      ENVIRONMENT "TMPDIR=${temporary};TMP=${temporary};TEMP=${temporary};PYTHONDONTWRITEBYTECODE=1")
  endforeach()
  add_test(NAME nexus_tauri_actions
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop/test_tauri_actions.py")
  set_tests_properties(nexus_tauri_actions PROPERTIES TIMEOUT 30
    ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-data/refinement/tmp;PYTHONDONTWRITEBYTECODE=1")
  find_program(NEXUS_NODE_EXECUTABLE node)
  if(NEXUS_NODE_EXECUTABLE)
    add_test(NAME nexus_tauri_dom
      COMMAND "${NEXUS_NODE_EXECUTABLE}" --test "${CMAKE_CURRENT_SOURCE_DIR}/tests/desktop/tauri_dom_test.cjs")
  endif()
  foreach(name gpu_selection vulkan_shader_embedding vulkan_q8_probe)
    set(temporary "${CMAKE_BINARY_DIR}/test-data/${name}/tmp")
    file(MAKE_DIRECTORY "${temporary}")
    add_test(NAME nexus_${name}_regression
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/${name}_test.py")
    set_tests_properties(nexus_${name}_regression PROPERTIES TIMEOUT 30
      ENVIRONMENT "TMPDIR=${temporary};TMP=${temporary};TEMP=${temporary};PYTHONDONTWRITEBYTECODE=1")
  endforeach()
  add_test(NAME nexus_wallet_authorization
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nexus_wallet_authorization_test.py"
      "${CMAKE_BINARY_DIR}/test-data/wallet-authorization")
  add_test(NAME nexus_terminal_dashboard_headless
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nexus_dashboard_test.py"
      "${CMAKE_BINARY_DIR}/test-data/dashboard")
endif()
find_program(NEXUS_PROTOC protoc)
find_program(NEXUS_OPENSSL openssl)
if(BUILD_TESTING AND Python3_Interpreter_FOUND AND NEXUS_ENABLE_GRPC)
  add_test(NAME protobuf_v057_wire_compat
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/proto_wire_compat.py"
      "${NEXUS_PROTOC}" "${CMAKE_CURRENT_SOURCE_DIR}/proto/current"
      "${CMAKE_CURRENT_SOURCE_DIR}/proto/v057" "${CMAKE_BINARY_DIR}/test-data/wire")
  if(NEXUS_OPENSSL)
    add_test(NAME grpc_tls_disconnect_reconnect_live_mock
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/grpc_tls_reconnect.py"
        "$<TARGET_FILE:keryx-nexus-live>" "$<TARGET_FILE:nexus-grpc-mock>"
        "$<TARGET_FILE:keryx-nexus-replay>" "${CMAKE_BINARY_DIR}/test-data/tls")
    set(M24_MAINNET_CAPTURE "${NEXUS_RECOVERY_ROOT}/artifacts/live/live-mainnet.capture")
    if(EXISTS "${M24_MAINNET_CAPTURE}")
      file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-data/m24/tmp")
      add_test(NAME grpc_authentic_capture_replay_and_cooperative_cancel
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/grpc_capture_pipeline.py"
          "$<TARGET_FILE:keryx-nexus-live>" "$<TARGET_FILE:nexus-grpc-mock>"
          "$<TARGET_FILE:keryx-nexus-replay>" "${M24_MAINNET_CAPTURE}"
          "${CMAKE_BINARY_DIR}/test-data/m24")
      set_tests_properties(grpc_authentic_capture_replay_and_cooperative_cancel PROPERTIES
        ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-data/m24/tmp;TMP=${CMAKE_BINARY_DIR}/test-data/m24/tmp;TEMP=${CMAKE_BINARY_DIR}/test-data/m24/tmp"
        TIMEOUT 40)
    endif()
  endif()
endif()
if(BUILD_TESTING AND Python3_Interpreter_FOUND)
  add_test(NAME m22_gpu_guard_safety
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/m22_gpu_guard_test.py")
  add_test(NAME m223_rtx3070ti_identity_safety
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/m223_gpu_identity_test.py")
  add_test(NAME m224_gpu_process_attribution_safety
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/m224_gpu_attribution_test.py")
  add_test(NAME m2212_resumable_cuda_journal
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/m2212_resumable_cuda_test.py")
  add_test(NAME m233_build_guard_attribution
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/m233_build_guard_test.py")
  add_test(NAME m28_force_shadow_telemetry_only
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/nexus_force_shadow_test.py")
  if(NEXUS_ENABLE_GRPC)
    add_executable(nexus-grpc-submit-mock-test tests/grpc_submit_mock.cpp)
    target_include_directories(nexus-grpc-submit-mock-test PRIVATE include "${PROTO_GEN}")
    target_link_libraries(nexus-grpc-submit-mock-test PRIVATE nexus_grpc nexus_model)
    if(NEXUS_RUST_ORACLE AND EXISTS "${NEXUS_RUST_ORACLE}")
      set(NEXUS_TEST_WORKSPACE "${NEXUS_RECOVERY_ROOT}")



      set(NEXUS_TEST_CAPTURE_DIR "${NEXUS_TEST_WORKSPACE}/artifacts/live")
      set(NEXUS_TEST_SUBMIT_DIR "${CMAKE_CURRENT_BINARY_DIR}/test-data/submission-mock")
      foreach(required_file "${NEXUS_TEST_MODEL}" "${NEXUS_TEST_TREE}" "${NEXUS_TEST_TREE_META}" "${NEXUS_RUST_ORACLE}")
        if(NOT EXISTS "${required_file}")
          message(FATAL_ERROR "Authentic proof SubmitBlock test requires ${required_file}")
        endif()
      endforeach()
      add_test(NAME official_submit_block_mock_protocol
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_submit_mock.py"
          "$<TARGET_FILE:nexus-grpc-submit-mock-test>" "${NEXUS_TEST_MODEL}"
          "${NEXUS_TEST_TREE}" "${NEXUS_TEST_TREE_META}" "${NEXUS_TEST_CAPTURE_DIR}"
          "${NEXUS_RUST_ORACLE}" "${NEXUS_TEST_SUBMIT_DIR}"
          "${NEXUS_TEST_LAUNCHER}"
          "${NEXUS_TEST_ESCROW_KEY}")
      set_tests_properties(official_submit_block_mock_protocol PROPERTIES
        TIMEOUT 900 LABELS "consensus;submit-mock;authentic-gguf")
    else()
      message(FATAL_ERROR "Authentic-proof SubmitBlock mock requires NEXUS_RUST_ORACLE")
    endif()
  endif()
endif()

add_executable(nexus-proof-artifact-test tests/proof_artifact_test.cpp)
target_include_directories(nexus-proof-artifact-test PRIVATE include)
add_test(NAME proof_artifact_lifetime
  COMMAND nexus-proof-artifact-test "${CMAKE_CURRENT_BINARY_DIR}/test-data/proof-artifact-lifetime")

add_executable(nexus-merkle-prefetch-test tests/merkle_prefetch_test.cpp)
target_link_libraries(nexus-merkle-prefetch-test PRIVATE nexus_model)
add_test(NAME bounded_merkle_prefetch_and_upper_parity COMMAND nexus-merkle-prefetch-test "${CMAKE_BINARY_DIR}/test-data/merkle-prefetch")

add_executable(nexus-proof-transport-test tests/proof_transport_test.cpp)
target_link_libraries(nexus-proof-transport-test PRIVATE nexus_consensus nexus_capture)
add_test(NAME bounded_proof_artifacts_and_chain_notifications COMMAND nexus-proof-transport-test)

if(Python3_Interpreter_FOUND AND NEXUS_RUST_ORACLE AND
   EXISTS "${NEXUS_AI_PREEMPTION_CONTEXT}" AND EXISTS "${NEXUS_AI_PREEMPTION_TRACE}")
  get_filename_component(NEXUS_VERIFIER_DIR "${NEXUS_AI_PREEMPTION_CONTEXT}" DIRECTORY)
  get_filename_component(NEXUS_VERIFIER_NAME "${NEXUS_AI_PREEMPTION_CONTEXT}" NAME_WE)
  set(NEXUS_VERIFIER_PROOF "${NEXUS_VERIFIER_DIR}/${NEXUS_VERIFIER_NAME}.proof")
  if(EXISTS "${NEXUS_VERIFIER_PROOF}")
    add_test(NAME official_rust_persistent_verifier_transport
      COMMAND nexus-proof-transport-test "${NEXUS_RUST_ORACLE}"
        "${NEXUS_AI_PREEMPTION_CONTEXT}" "${NEXUS_AI_PREEMPTION_TRACE}" "${NEXUS_VERIFIER_PROOF}")
    add_test(NAME official_rust_verifier_stream_rejects_invalid_frames
      COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/tests/rust_verifier_stream_test.py"
        "${NEXUS_RUST_ORACLE}" "${NEXUS_AI_PREEMPTION_CONTEXT}"
        "${NEXUS_AI_PREEMPTION_TRACE}" "${NEXUS_VERIFIER_PROOF}")
    set_tests_properties(official_rust_persistent_verifier_transport
      official_rust_verifier_stream_rejects_invalid_frames PROPERTIES TIMEOUT 30 LABELS "cpu;rust-proof")
  endif()
endif()

if(NEXUS_ENABLE_GRPC)
  add_executable(nexus-verified-inference-codec-test tests/verified_inference_codec_test.cpp)
  target_compile_definitions(nexus-verified-inference-codec-test PRIVATE NEXUS_TEST_TRACE_SUPPORTED=1)
  target_link_libraries(nexus-verified-inference-codec-test PRIVATE nexus_grpc)
  add_test(NAME official_verified_inference_codec_compatibility COMMAND nexus-verified-inference-codec-test)
endif()

# Read-only regeneration of a recorded mainnet proof; fixture paths are explicit.
add_executable(nexus-merkle-root-real-proof-test tests/merkle_root_batch_real_proof_test.cpp)
target_link_libraries(nexus-merkle-root-real-proof-test PRIVATE nexus_model)

# Pure policy checks also belong to the dependency-light CPU configuration.
if(BUILD_TESTING AND NOT NEXUS_ENABLE_GRPC)
  foreach(case live_worker_config primary_vulkan_policy startup_readiness)
    add_executable(nexus-${case}-cpu-test tests/${case}_test.cpp)
    target_include_directories(nexus-${case}-cpu-test PRIVATE include)
    target_link_libraries(nexus-${case}-cpu-test PRIVATE Threads::Threads)
    add_test(NAME ${case}_cpu_policy COMMAND nexus-${case}-cpu-test)
  endforeach()
endif()

if(BUILD_TESTING)
  add_executable(nexus-platform-file-test tests/platform_file_test.cpp)
  target_link_libraries(nexus-platform-file-test PRIVATE nexus_model)
  add_test(NAME native_file_reads COMMAND nexus-platform-file-test "${CMAKE_BINARY_DIR}/test-data/platform-file")
endif()
