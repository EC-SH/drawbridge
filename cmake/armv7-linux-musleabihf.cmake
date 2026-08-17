# CMake toolchain — cross-compile pocket-dial to ARMv7 musl (fully static) for the
# Orbic RC400L and other rayhunter ARMv7 Linux hotspot targets (issue #82).
#
# Needs an arm-linux-musleabihf musl cross toolchain on disk, e.g.:
#   - musl.cc:                /opt/arm-linux-musleabihf-cross   (default below)
#   - messense/rust-musl-cross:armv7-musleabihf  (rayhunter's CI image)
# Override the location with -DMUSL_TOOLCHAIN_DIR=/path or $MUSL_TOOLCHAIN_DIR.
#
# Usage:
#   cmake -S . -B build_orbic \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/armv7-linux-musleabihf.cmake \
#         -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
#   cmake --build build_orbic --target SipServer
# Output: build_orbic/SipServer — statically linked ARMv7 binary, no runtime deps.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

if(NOT DEFINED MUSL_TOOLCHAIN_DIR)
    if(DEFINED ENV{MUSL_TOOLCHAIN_DIR})
        set(MUSL_TOOLCHAIN_DIR $ENV{MUSL_TOOLCHAIN_DIR})
    else()
        set(MUSL_TOOLCHAIN_DIR /opt/arm-linux-musleabihf-cross)
    endif()
endif()

set(_prefix ${MUSL_TOOLCHAIN_DIR}/bin/arm-linux-musleabihf)
set(CMAKE_C_COMPILER   ${_prefix}-gcc)
set(CMAKE_CXX_COMPILER ${_prefix}-g++)

# Fully static so the binary runs on the Orbic's musl/busybox userland with zero deps.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")

set(CMAKE_FIND_ROOT_PATH ${MUSL_TOOLCHAIN_DIR})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
