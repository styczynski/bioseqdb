# Chainloaded by x64-linux-libcxx.cmake for every vcpkg port build on the AWS
# x86_64 Linux box. Pins clang-22 (apt.llvm.org install path) so dependencies are
# compiled with the same toolchain as the bioseq targets. libc++ selection is
# done via VCPKG_CXX_FLAGS/VCPKG_LINKER_FLAGS in the triplet, not here, so this
# file stays a minimal compiler pin.
set(CMAKE_C_COMPILER /usr/lib/llvm-22/bin/clang CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER /usr/lib/llvm-22/bin/clang++ CACHE STRING "" FORCE)
