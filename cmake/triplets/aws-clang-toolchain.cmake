# Chainloaded by the native x64/arm64 libc++ triplets for every vcpkg port
# build. Pins clang-22 to the same complete LLVM root selected by the caller,
# then delegates to vcpkg's Linux toolchain so
# VCPKG_CXX_FLAGS/VCPKG_LINKER_FLAGS from the triplet become CMake init flags for
# every port build.
if(DEFINED ENV{BIOSEQ_LLVM_ROOT})
  set(_BIOSEQ_AWS_LLVM_ROOT "$ENV{BIOSEQ_LLVM_ROOT}")
else()
  set(_BIOSEQ_AWS_LLVM_ROOT /usr/lib/llvm-22)
endif()
set(CMAKE_C_COMPILER "${_BIOSEQ_AWS_LLVM_ROOT}/bin/clang" CACHE STRING "" FORCE)
set(CMAKE_CXX_COMPILER "${_BIOSEQ_AWS_LLVM_ROOT}/bin/clang++" CACHE STRING "" FORCE)

if(DEFINED ENV{BIOSEQ_COMPILER_LAUNCHER})
  if(NOT IS_ABSOLUTE "$ENV{BIOSEQ_COMPILER_LAUNCHER}"
      OR NOT EXISTS "$ENV{BIOSEQ_COMPILER_LAUNCHER}")
    message(FATAL_ERROR
      "BIOSEQ_COMPILER_LAUNCHER must be an existing absolute path")
  endif()
  set(CMAKE_C_COMPILER_LAUNCHER "$ENV{BIOSEQ_COMPILER_LAUNCHER}"
    CACHE FILEPATH "Bioseq package compiler priority launcher" FORCE)
  set(CMAKE_CXX_COMPILER_LAUNCHER "$ENV{BIOSEQ_COMPILER_LAUNCHER}"
    CACHE FILEPATH "Bioseq package compiler priority launcher" FORCE)
endif()

if(DEFINED Z_VCPKG_ROOT_DIR)
  set(_BIOSEQ_VCPKG_SCRIPTS_DIR "${Z_VCPKG_ROOT_DIR}/scripts")
elseif(DEFINED _VCPKG_ROOT_DIR)
  set(_BIOSEQ_VCPKG_SCRIPTS_DIR "${_VCPKG_ROOT_DIR}/scripts")
else()
  get_filename_component(_BIOSEQ_VCPKG_BUILDSYSTEMS_DIR "${CMAKE_TOOLCHAIN_FILE}" DIRECTORY)
  get_filename_component(_BIOSEQ_VCPKG_SCRIPTS_DIR "${_BIOSEQ_VCPKG_BUILDSYSTEMS_DIR}" DIRECTORY)
endif()
set(_BIOSEQ_VCPKG_LINUX_TOOLCHAIN "${_BIOSEQ_VCPKG_SCRIPTS_DIR}/toolchains/linux.cmake")
if(EXISTS "${_BIOSEQ_VCPKG_LINUX_TOOLCHAIN}")
  include("${_BIOSEQ_VCPKG_LINUX_TOOLCHAIN}")
else()
  message(FATAL_ERROR
    "bioseq AWS clang vcpkg chainload could not find vcpkg Linux toolchain at "
    "${_BIOSEQ_VCPKG_LINUX_TOOLCHAIN}")
endif()
