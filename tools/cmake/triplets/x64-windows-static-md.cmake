set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
# Preview archives contain Release executables only.
set(VCPKG_BUILD_TYPE release)
# Protobuf's exported empty-string type differs between MSVC C++17 and C++20.
# Keep all CMake dependencies consistent with the C++20 consumer. These options
# are appended after the port's defaults, including protobuf's C++17 setting.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON)

# gRPC's fused-filter templates need eager method deduction on MSVC 19.50/19.51.
# Apply only to this port; hash both helpers into its binary-cache ABI.
if(PORT STREQUAL "grpc")
  get_filename_component(NEXUS_GRPC_HOOK_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
  list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
    "-DCMAKE_PROJECT_grpc_INCLUDE=${NEXUS_GRPC_HOOK_DIR}/grpc-msvc.cmake")
  list(APPEND VCPKG_HASH_ADDITIONAL_FILES
    "${NEXUS_GRPC_HOOK_DIR}/grpc-msvc.cmake"
    "${NEXUS_GRPC_HOOK_DIR}/grpc_msvc.py")
endif()
