# libb2's autotools/SIMD build is not required on MSVC. Build the exact public
# reference implementation with its retained notice, without changing the hash.
include(FetchContent)
FetchContent_Declare(nexus_blake2
  URL https://github.com/BLAKE2/libb2/archive/2c5142f12a2cd52f3ee0a43e50a3a76f75badf85.tar.gz
  URL_HASH SHA512=cf29cf9391ae37a978eb6618de6f856f3defa622b8f56c2d5a519ab34fd5e4d91f3bb868601a44e9c9164a2992e80dde188ccc4d1605dffbdf93687336226f8d
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(nexus_blake2)
add_library(nexus_blake2_reference STATIC "${nexus_blake2_SOURCE_DIR}/src/blake2b-ref.c")
set(BLAKE2_INCLUDE_DIR "${nexus_blake2_SOURCE_DIR}/src")
target_include_directories(nexus_blake2_reference PUBLIC "${BLAKE2_INCLUDE_DIR}")
set(BLAKE2_LIBRARY nexus_blake2_reference)
