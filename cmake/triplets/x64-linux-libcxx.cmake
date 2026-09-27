# AWS x86_64 Linux triplet for the clang-22 + libc++ preset. It forces clang +
# libc++ for every vcpkg dependency so their C++ ABI matches bioseq's libc++
# build.
#
# This mirrors the built-in `x64-linux` triplet (static archives, dynamic glibc
# CRT) but forces clang + libc++ 22 for every port build via a chainloaded
# toolchain + `-stdlib=libc++`. This is REQUIRED for ABI compatibility: the
# bioseq targets are compiled with clang-22 + libc++ (the only configuration in
# which `import std` / `CMAKE_CXX_MODULE_STD` works on clang). If the vcpkg
# dependencies were built with the host's default gcc + libstdc++ instead, the
# C++ ones (Boost.program_options/regex/serialization, glslang, spirv-cross,
# TBB, reflectcpp, sqlgen, ...) would expose `std::__cxx11::*` symbols that the
# libc++ `std::__1::*` consumer cannot link against — the failure would surface
# the moment the (out-of-scope) ::NEON SIMD code is ported and the targets
# actually link. Keeping the whole graph on clang+libc++ avoids that wall.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

# Applied on top of each port's own flags by the vcpkg toolchain.
set(VCPKG_C_FLAGS "")
set(VCPKG_C_FLAGS_RELEASE "-O3")
# oneTBB's `_tpause` (WAITPKG) intrinsic is `always_inline`, but clang errors if
# it is compiled into a function without the waitpkg target feature. Disabling
# that optional path is safer than adding `-mwaitpkg`, because the current AWS
# m4 host does not advertise WAITPKG.
set(VCPKG_CXX_FLAGS "-stdlib=libc++ -D__TBB_WAITPKG_INTRINSICS_PRESENT=0 -Wno-unused-command-line-argument")
set(VCPKG_CXX_FLAGS_RELEASE "-O3")
set(VCPKG_LINKER_FLAGS "-stdlib=libc++")

# Pins the compiler to clang-22 for port builds (vcpkg uses its own bundled
# CMake/Ninja, so this only governs the compiler, not the build driver).
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/aws-clang-toolchain.cmake")
