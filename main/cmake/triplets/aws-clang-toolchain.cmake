# Chainloaded by x64-linux-libcxx.cmake for every vcpkg port build on the AWS
# x86_64 Linux box. Pins clang-22 to the same root selected by bootstrap.sh
# (apt.llvm.org under /usr/lib/llvm-22, or the official tarball fallback under
# /opt/llvm-22). libc++ selection is done via VCPKG_CXX_FLAGS/VCPKG_LINKER_FLAGS
# in the triplet, not here, so this file stays a minimal compiler pin.
if(DEFINED ENV{BIOSEQ_LLVM_ROOT})
  set(_BIOSEQ_AWS_LLVM_ROOT "$ENV{BIOSEQ_LLVM_ROOT}")
else()
  set(_BIOSEQ_AWS_LLVM_ROOT /usr/lib/llvm-22)
endif()
set(CMAKE_C_COMPILER "${_BIOSEQ_AWS_LLVM_ROOT}/bin/clang" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER "${_BIOSEQ_AWS_LLVM_ROOT}/bin/clang++" CACHE STRING "" FORCE)
