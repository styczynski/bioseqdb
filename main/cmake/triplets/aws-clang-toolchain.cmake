# Chainloaded by x64-linux-libcxx.cmake for every vcpkg port build on the AWS
# x86_64 Linux box. Pins clang-22 to the same root selected by bootstrap.sh
# (apt.llvm.org under /usr/lib/llvm-22, or the official tarball fallback under
# /opt/llvm-22), then delegates to vcpkg's Linux toolchain so
# VCPKG_CXX_FLAGS/VCPKG_LINKER_FLAGS from the triplet become CMake init flags for
# every port build.
if(DEFINED ENV{BIOSEQ_LLVM_ROOT})
  set(_BIOSEQ_AWS_LLVM_ROOT "$ENV{BIOSEQ_LLVM_ROOT}")
else()
  set(_BIOSEQ_AWS_LLVM_ROOT /usr/lib/llvm-22)
endif()
set(CMAKE_C_COMPILER "${_BIOSEQ_AWS_LLVM_ROOT}/bin/clang" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER "${_BIOSEQ_AWS_LLVM_ROOT}/bin/clang++" CACHE STRING "" FORCE)

get_filename_component(_BIOSEQ_VCPKG_BUILDSYSTEMS_DIR "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
get_filename_component(_BIOSEQ_VCPKG_SCRIPTS_DIR "${_BIOSEQ_VCPKG_BUILDSYSTEMS_DIR}" DIRECTORY)
set(_BIOSEQ_VCPKG_LINUX_TOOLCHAIN "${_BIOSEQ_VCPKG_SCRIPTS_DIR}/toolchains/linux.cmake")
if(EXISTS "${_BIOSEQ_VCPKG_LINUX_TOOLCHAIN}")
  include("${_BIOSEQ_VCPKG_LINUX_TOOLCHAIN}")
else()
  message(FATAL_ERROR
    "bioseq AWS clang vcpkg chainload could not find vcpkg Linux toolchain at "
    "${_BIOSEQ_VCPKG_LINUX_TOOLCHAIN}")
endif()
