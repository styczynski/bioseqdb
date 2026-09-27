# Native arm64 Linux triplet for the pinned Clang 22/libc++ package toolchain.
# All C++ dependencies are static archives so the final extension modules can
# link the pinned libc++/libc++abi/libunwind archives explicitly.
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Linux)

set(VCPKG_C_FLAGS "")
set(VCPKG_C_FLAGS_RELEASE "-O3")
set(VCPKG_CXX_FLAGS
  "-stdlib=libc++ -D__TBB_WAITPKG_INTRINSICS_PRESENT=0 -Wno-unused-command-line-argument")
set(VCPKG_CXX_FLAGS_RELEASE "-O3")
set(VCPKG_LINKER_FLAGS "-stdlib=libc++ -rtlib=compiler-rt")

set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE
  "${CMAKE_CURRENT_LIST_DIR}/aws-clang-toolchain.cmake")
