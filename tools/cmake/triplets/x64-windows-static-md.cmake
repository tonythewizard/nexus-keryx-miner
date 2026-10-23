set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
# Preview archives contain Release executables only.
set(VCPKG_BUILD_TYPE release)
# Protobuf's exported empty-string type differs between MSVC C++17 and C++20.
# Keep all CMake dependencies consistent with the C++20 consumer. These options
# are appended after the port's defaults, including protobuf's C++17 setting.
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON)
