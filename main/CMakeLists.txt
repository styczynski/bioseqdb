# Copyright 2022 Zacharias Knudsen
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Almost all CMake files should start with this
# You should always specify a range with the newest
# and oldest tested versions of CMake. This will ensure
# you pick up the best policies.

# Keep minimum version in sync with:
# https://github.com/uber/h3/blob/master/CMakeLists.txt
cmake_minimum_required(VERSION 3.20..3.24)

option(BIOSEQ_BUILD_CORE
  "Build sequence types, table AM, aligner, CLI and core benchmarks" ON)
option(BIOSEQ_BUILD_CACHE
  "Build the independent PostgreSQL query cache extension" ON)
option(BIOSEQ_BUILD_PAPERS
  "Build paper and chart targets" ON)
option(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS
  "Configure generated external API clients and their integration tests" OFF)
option(BIOSEQ_BUILD_WEBSITE
  "Build versioned static documentation website" OFF)
option(BIOSEQ_PACKAGE_BUILD
  "Build relocatable native distribution packages with a static C++ runtime" OFF)
option(BIOSEQ_HIGHWAY_GCC_CANARY
  "Build the local Darwin Clang-generator/Homebrew-GCC module boundary canary" OFF)
if(BIOSEQ_HIGHWAY_GCC_CANARY
    AND NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR
    "BIOSEQ_HIGHWAY_GCC_CANARY is an opt-in local Darwin build-only proof")
endif()
if(BIOSEQ_HIGHWAY_GCC_CANARY AND NOT BIOSEQ_BUILD_CORE)
  message(FATAL_ERROR
    "BIOSEQ_HIGHWAY_GCC_CANARY requires BIOSEQ_BUILD_CORE=ON")
endif()
set(BIOSEQ_WEBSITE_VERSION "" CACHE STRING
  "Website source version directory, for example v0.1")
set(BIOSEQ_WEBSITE_BASE_PATH "" CACHE STRING
  "URL base path; empty uses site.yaml")
set(BIOSEQ_WEBSITE_OUTPUT_DIRECTORY "" CACHE PATH
  "Build-tree site output; empty uses <binary>/website/site")
set(BIOSEQ_WEBSITE_SOURCE_REVISION "" CACHE STRING
  "40-hex source commit; empty resolves Git HEAD")
set(BIOSEQ_WEBSITE_VERSION_ARTIFACTS "" CACHE STRING
  "Semicolon-separated successful historical site roots to assemble")
option(BIOSEQ_WEBSITE_INCLUDE_DRAFTS
  "Render validated draft tutorials" OFF)
option(BIOSEQ_WEBSITE_BROWSER_TESTS
  "Run pinned Playwright acceptance tests" OFF)

# Change this (or override it without editing this file):
# CMAKE_NAMED_ENV_TYPE selects the per-machine toolchain block below. Resolution
# order: an environment variable (set e.g. by a CMake preset's `environment`
# field or `export CMAKE_NAMED_ENV_TYPE=...`) wins; then any pre-set cache/normal
# value; otherwise it selects the installed Linux toolchain on Linux and "H"
# (the arm64 macOS "home" box) elsewhere. Presets can still select their own
# toolchain explicitly. Supported: "W", "H",
# "AWS_MACHINE_X64", "AWS_MACHINE_LINUX_CLANG22",
# "AWS_MACHINE_X64_GCC16".
if(DEFINED ENV{CMAKE_NAMED_ENV_TYPE})
  set(CMAKE_NAMED_ENV_TYPE "$ENV{CMAKE_NAMED_ENV_TYPE}")
elseif(NOT DEFINED CMAKE_NAMED_ENV_TYPE)
  if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux"
      AND EXISTS "/opt/gcc-16.1.0/bin/g++-16")
    set(CMAKE_NAMED_ENV_TYPE "AWS_MACHINE_X64_GCC16")
  elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(CMAKE_NAMED_ENV_TYPE "AWS_MACHINE_X64")
  else()
    set(CMAKE_NAMED_ENV_TYPE "H")
  endif()
endif()

if(${CMAKE_NAMED_ENV_TYPE} STREQUAL "H")
  set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm@22/bin/clang)
  set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm@22/bin/clang++)
  # The `import std` experimental-feature gate UUID is CMake-version-specific.
  # Homebrew CMake 4.2.x expects d0edc3af-..., while 4.3.x expects
  # 451f2fe2-.... Select it explicitly so a fresh build tree remains usable
  # when Homebrew moves between those supported versions.
  if(CMAKE_VERSION VERSION_LESS "4.3")
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
  else()
    set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
  endif()
  # clang >= 22 no longer resolves libc++.modules.json via the compiler's
  # `-print-file-name` search path (it lives under <toolchain>/lib/c++/ now, not
  # the clang resource dir), so CMake's import-std / CMAKE_CXX_MODULE_STD support
  # can fail to locate the `std` module mapping. Point it at the file explicitly.
  # The @22 opt symlink keeps this version-independent across llvm@22 point bumps.
  set(CMAKE_CXX_STDLIB_MODULES_JSON /opt/homebrew/opt/llvm@22/lib/c++/libc++.modules.json)
elseif(${CMAKE_NAMED_ENV_TYPE} STREQUAL "W")
  set(CMAKE_C_COMPILER /usr/local/opt/llvm/bin/clang)
  set(CMAKE_CXX_COMPILER /usr/local/opt/llvm/bin/clang++)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
  set(CMAKE_CXX_STDLIB_MODULES_JSON /usr/local/opt/llvm/lib/c++/libc++.modules.json)
elseif(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64"
    OR CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_LINUX_CLANG22")
  # Native Linux Clang/libc++ 22 environment. AWS_MACHINE_X64 remains the
  # development preset; AWS_MACHINE_LINUX_CLANG22 is the architecture-neutral
  # package mode used on both x86_64 and arm64 workers. BIOSEQ_LLVM_ROOT points
  # at the pinned complete LLVM archive in package containers. CMake 4.2.1's
  # import-std experimental UUID is the same value the "H" box uses.
  set(_BIOSEQ_REQUESTED_LLVM_ROOT "")
  if(DEFINED ENV{BIOSEQ_LLVM_ROOT})
    set(_BIOSEQ_REQUESTED_LLVM_ROOT "$ENV{BIOSEQ_LLVM_ROOT}")
  elseif(BIOSEQ_LLVM_ROOT)
    set(_BIOSEQ_REQUESTED_LLVM_ROOT "${BIOSEQ_LLVM_ROOT}")
  endif()
  set(_BIOSEQ_REQUESTED_LIBCXX_MODULES_JSON "")
  if(DEFINED ENV{BIOSEQ_LIBCXX_MODULES_JSON}
      AND EXISTS "$ENV{BIOSEQ_LIBCXX_MODULES_JSON}")
    file(REAL_PATH "$ENV{BIOSEQ_LIBCXX_MODULES_JSON}"
      _BIOSEQ_REQUESTED_LIBCXX_MODULES_JSON)
    get_filename_component(_BIOSEQ_REQUESTED_LIBCXX_MODULES_NAME
      "${_BIOSEQ_REQUESTED_LIBCXX_MODULES_JSON}" NAME)
    if(NOT "${_BIOSEQ_REQUESTED_LIBCXX_MODULES_NAME}"
        STREQUAL "libc++.modules.json")
      set(_BIOSEQ_REQUESTED_LIBCXX_MODULES_JSON "")
    endif()
  endif()
  set(BIOSEQ_LLVM_ROOT "")
  set(BIOSEQ_LLVM_ROOT "" CACHE PATH
    "LLVM/Clang prefix for the AWS-compatible Clang toolchain"
    FORCE)
  set(_BIOSEQ_SELECTED_LIBCXX_MODULES_JSON "")
  foreach(_BIOSEQ_LLVM_ROOT_CANDIDATE IN ITEMS
      "${_BIOSEQ_REQUESTED_LLVM_ROOT}"
      "/usr/lib/llvm-22"
      "/opt/llvm-22"
      "/opt/homebrew/opt/llvm@22"
      "/usr/local/opt/llvm")
    if(_BIOSEQ_LLVM_ROOT_CANDIDATE
        AND EXISTS "${_BIOSEQ_LLVM_ROOT_CANDIDATE}/bin/clang"
        AND EXISTS "${_BIOSEQ_LLVM_ROOT_CANDIDATE}/bin/clang++"
        AND (EXISTS
               "${_BIOSEQ_LLVM_ROOT_CANDIDATE}/bin/llvm-config"
             OR EXISTS
               "${_BIOSEQ_LLVM_ROOT_CANDIDATE}/bin/llvm-config-22"))
      file(REAL_PATH "${_BIOSEQ_LLVM_ROOT_CANDIDATE}"
        _BIOSEQ_LLVM_ROOT_CANDIDATE_REAL)
      set(_BIOSEQ_REQUESTED_MODULES_UNDER_CANDIDATE OFF)
      if(_BIOSEQ_REQUESTED_LIBCXX_MODULES_JSON)
        string(FIND
          "${_BIOSEQ_REQUESTED_LIBCXX_MODULES_JSON}/"
          "${_BIOSEQ_LLVM_ROOT_CANDIDATE_REAL}/"
          _BIOSEQ_REQUESTED_MODULES_PREFIX)
        if(_BIOSEQ_REQUESTED_MODULES_PREFIX EQUAL 0)
          set(_BIOSEQ_REQUESTED_MODULES_UNDER_CANDIDATE ON)
        endif()
      endif()
      if(_BIOSEQ_REQUESTED_MODULES_UNDER_CANDIDATE
          OR EXISTS
            "${_BIOSEQ_LLVM_ROOT_CANDIDATE}/lib/x86_64-unknown-linux-gnu/c++/libc++.modules.json"
          OR EXISTS
            "${_BIOSEQ_LLVM_ROOT_CANDIDATE}/lib/aarch64-unknown-linux-gnu/c++/libc++.modules.json"
          OR EXISTS
            "${_BIOSEQ_LLVM_ROOT_CANDIDATE}/lib/c++/libc++.modules.json"
          OR EXISTS
            "${_BIOSEQ_LLVM_ROOT_CANDIDATE}/lib/libc++.modules.json")
        get_filename_component(BIOSEQ_LLVM_ROOT
          "${_BIOSEQ_LLVM_ROOT_CANDIDATE}" ABSOLUTE)
        if(_BIOSEQ_REQUESTED_MODULES_UNDER_CANDIDATE)
          set(_BIOSEQ_SELECTED_LIBCXX_MODULES_JSON
            "${_BIOSEQ_REQUESTED_LIBCXX_MODULES_JSON}")
        endif()
        break()
      endif()
    endif()
  endforeach()
  if(NOT BIOSEQ_LLVM_ROOT)
    message(FATAL_ERROR
      "a complete LLVM 22 prefix was not found in BIOSEQ_LLVM_ROOT or "
      "the supported Linux/macOS installation locations")
  endif()
  set(BIOSEQ_LLVM_ROOT "${BIOSEQ_LLVM_ROOT}" CACHE PATH
    "LLVM/Clang prefix for the AWS-compatible Clang toolchain"
    FORCE)
  set(CMAKE_C_COMPILER "${BIOSEQ_LLVM_ROOT}/bin/clang"
    CACHE FILEPATH "Selected C compiler" FORCE)
  set(CMAKE_CXX_COMPILER "${BIOSEQ_LLVM_ROOT}/bin/clang++"
    CACHE FILEPATH "Selected C++ compiler" FORCE)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
  set(CMAKE_CXX_STDLIB_MODULES_JSON "")
  set(CMAKE_CXX_STDLIB_MODULES_JSON "" CACHE FILEPATH
    "C++ standard-library module metadata" FORCE)
  if(_BIOSEQ_SELECTED_LIBCXX_MODULES_JSON)
    set(CMAKE_CXX_STDLIB_MODULES_JSON
      "${_BIOSEQ_SELECTED_LIBCXX_MODULES_JSON}")
  else()
    # The official LLVM tarball and apt.llvm.org packages install this file in
    # different locations. Detached build workers do not necessarily inherit
    # bootstrap.sh's BIOSEQ_LIBCXX_MODULES_JSON profile export, and an
    # AWS-named configuration can run on a macOS worker, so discover every
    # supported layout from the validated selected LLVM root.
    set(_BIOSEQ_LIBCXX_MODULES_JSON_CANDIDATES
      "${BIOSEQ_LLVM_ROOT}/lib/x86_64-unknown-linux-gnu/c++/libc++.modules.json"
      "${BIOSEQ_LLVM_ROOT}/lib/aarch64-unknown-linux-gnu/c++/libc++.modules.json"
      "${BIOSEQ_LLVM_ROOT}/lib/c++/libc++.modules.json"
      "${BIOSEQ_LLVM_ROOT}/lib/libc++.modules.json")
    foreach(_BIOSEQ_LIBCXX_MODULES_JSON
        IN LISTS _BIOSEQ_LIBCXX_MODULES_JSON_CANDIDATES)
      if(EXISTS "${_BIOSEQ_LIBCXX_MODULES_JSON}")
        set(CMAKE_CXX_STDLIB_MODULES_JSON "${_BIOSEQ_LIBCXX_MODULES_JSON}")
        break()
      endif()
    endforeach()
    if(NOT CMAKE_CXX_STDLIB_MODULES_JSON)
      message(FATAL_ERROR
        "libc++.modules.json was not found under BIOSEQ_LLVM_ROOT='${BIOSEQ_LLVM_ROOT}'")
    endif()
    unset(_BIOSEQ_LIBCXX_MODULES_JSON)
    unset(_BIOSEQ_LIBCXX_MODULES_JSON_CANDIDATES)
  endif()
  file(REAL_PATH "${BIOSEQ_LLVM_ROOT}"
    _BIOSEQ_SELECTED_LLVM_ROOT_REAL)
  file(REAL_PATH "${CMAKE_CXX_STDLIB_MODULES_JSON}"
    _BIOSEQ_SELECTED_LIBCXX_MODULES_REAL)
  get_filename_component(_BIOSEQ_SELECTED_LIBCXX_MODULES_NAME
    "${_BIOSEQ_SELECTED_LIBCXX_MODULES_REAL}" NAME)
  string(FIND "${_BIOSEQ_SELECTED_LIBCXX_MODULES_REAL}/"
    "${_BIOSEQ_SELECTED_LLVM_ROOT_REAL}/"
    _BIOSEQ_SELECTED_LIBCXX_MODULES_PREFIX)
  if(NOT "${_BIOSEQ_SELECTED_LIBCXX_MODULES_NAME}"
        STREQUAL "libc++.modules.json"
      OR NOT _BIOSEQ_SELECTED_LIBCXX_MODULES_PREFIX EQUAL 0)
    message(FATAL_ERROR
      "selected libc++.modules.json is outside the selected LLVM prefix")
  endif()
  set(CMAKE_CXX_STDLIB_MODULES_JSON
    "${_BIOSEQ_SELECTED_LIBCXX_MODULES_REAL}")
  set(CMAKE_CXX_STDLIB_MODULES_JSON
    "${CMAKE_CXX_STDLIB_MODULES_JSON}" CACHE FILEPATH
    "C++ standard-library module metadata" FORCE)
  unset(_BIOSEQ_LLVM_ROOT_CANDIDATE)
  unset(_BIOSEQ_LLVM_ROOT_CANDIDATE_REAL)
  unset(_BIOSEQ_REQUESTED_LLVM_ROOT)
  unset(_BIOSEQ_REQUESTED_LIBCXX_MODULES_JSON)
  unset(_BIOSEQ_REQUESTED_LIBCXX_MODULES_NAME)
  unset(_BIOSEQ_REQUESTED_MODULES_PREFIX)
  unset(_BIOSEQ_REQUESTED_MODULES_UNDER_CANDIDATE)
  unset(_BIOSEQ_SELECTED_LIBCXX_MODULES_JSON)
  unset(_BIOSEQ_SELECTED_LIBCXX_MODULES_NAME)
  unset(_BIOSEQ_SELECTED_LIBCXX_MODULES_PREFIX)
  unset(_BIOSEQ_SELECTED_LIBCXX_MODULES_REAL)
  unset(_BIOSEQ_SELECTED_LLVM_ROOT_REAL)
  # CMake 4.2 + Ninja on the AWS box cannot relink the PostgreSQL extension to
  # swap from build-tree rpaths to install rpaths during `install(TARGETS ...)`.
  # Build directly with the install rpath instead; the preset is disposable and
  # always runs from the vcpkg-installed tree paths.
  if(NOT BIOSEQ_PACKAGE_BUILD)
    set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
  endif()
elseif(${CMAKE_NAMED_ENV_TYPE} STREQUAL "AWS_MACHINE_X64_GCC16")
  # GCC-only C++26 build. Select one paired compiler prefix and authenticate
  # libstdc++'s std/std.compat module inventory before project() can cache a
  # mixed toolchain. The ordinary macOS environment remains Clang; Homebrew GCC
  # is considered only when this GCC-only environment is explicitly selected.
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/BioseqGccToolchain.cmake")
  set(_BIOSEQ_REQUESTED_GCC16_ROOT "")
  if(DEFINED ENV{BIOSEQ_GCC16_ROOT})
    set(_BIOSEQ_REQUESTED_GCC16_ROOT "$ENV{BIOSEQ_GCC16_ROOT}")
  elseif(BIOSEQ_GCC16_ROOT)
    set(_BIOSEQ_REQUESTED_GCC16_ROOT "${BIOSEQ_GCC16_ROOT}")
  endif()
  set(_BIOSEQ_REQUESTED_LIBSTDCXX_MODULES "")
  if(DEFINED ENV{BIOSEQ_LIBSTDCXX_MODULES_JSON})
    set(_BIOSEQ_REQUESTED_LIBSTDCXX_MODULES
      "$ENV{BIOSEQ_LIBSTDCXX_MODULES_JSON}")
  elseif(CMAKE_CXX_STDLIB_MODULES_JSON)
    set(_BIOSEQ_REQUESTED_LIBSTDCXX_MODULES
      "${CMAKE_CXX_STDLIB_MODULES_JSON}")
  endif()

  bioseq_select_gcc16_toolchain(
    REQUESTED_ROOT "${_BIOSEQ_REQUESTED_GCC16_ROOT}"
    REQUESTED_MODULES "${_BIOSEQ_REQUESTED_LIBSTDCXX_MODULES}"
    OUTPUT_ROOT _BIOSEQ_AUTHENTICATED_GCC16_ROOT
    OUTPUT_C _BIOSEQ_AUTHENTICATED_GCC16_C
    OUTPUT_CXX _BIOSEQ_AUTHENTICATED_GCC16_CXX
    OUTPUT_MODULES _BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULES
    OUTPUT_VERSION _BIOSEQ_AUTHENTICATED_GCC16_VERSION
    OUTPUT_C_SHA256 _BIOSEQ_AUTHENTICATED_GCC16_C_SHA256
    OUTPUT_CXX_SHA256 _BIOSEQ_AUTHENTICATED_GCC16_CXX_SHA256
    OUTPUT_MODULES_SHA256
      _BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULES_SHA256
    OUTPUT_MODULE_SOURCE_CLOSURE_SHA256
      _BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULE_SOURCE_CLOSURE_SHA256)
  set(BIOSEQ_GCC16_ROOT "${_BIOSEQ_AUTHENTICATED_GCC16_ROOT}"
    CACHE PATH "Authenticated GCC 16.1+ prefix" FORCE)
  set(BIOSEQ_GCC16_VERSION "${_BIOSEQ_AUTHENTICATED_GCC16_VERSION}"
    CACHE INTERNAL "Authenticated GCC C/C++ version" FORCE)
  set(BIOSEQ_GCC16_C_SHA256
    "${_BIOSEQ_AUTHENTICATED_GCC16_C_SHA256}"
    CACHE INTERNAL "Authenticated GCC C compiler identity" FORCE)
  set(BIOSEQ_GCC16_CXX_SHA256
    "${_BIOSEQ_AUTHENTICATED_GCC16_CXX_SHA256}"
    CACHE INTERNAL "Authenticated GCC C++ compiler identity" FORCE)
  set(BIOSEQ_GCC16_MODULES_SHA256
    "${_BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULES_SHA256}"
    CACHE INTERNAL "Authenticated libstdc++ module inventory identity" FORCE)
  set(BIOSEQ_GCC16_MODULE_SOURCE_CLOSURE_SHA256
    "${_BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULE_SOURCE_CLOSURE_SHA256}"
    CACHE INTERNAL "Authenticated libstdc++ module source closure" FORCE)
  set(CMAKE_C_COMPILER "${_BIOSEQ_AUTHENTICATED_GCC16_C}"
    CACHE FILEPATH "Selected GCC C compiler" FORCE)
  set(CMAKE_CXX_COMPILER "${_BIOSEQ_AUTHENTICATED_GCC16_CXX}"
    CACHE FILEPATH "Selected GCC C++ compiler" FORCE)
  set(BIOSEQ_GCC16_C_COMPILER_LAUNCHER
    "${CMAKE_SOURCE_DIR}/cmake/gcc_hwy_header_unit_launcher.py"
    "--bioseq-expected-compiler-sha256=${BIOSEQ_GCC16_C_SHA256}"
    "--bioseq-gcc-root=${BIOSEQ_GCC16_ROOT}"
    "--bioseq-gcc-module-inventory=${_BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULES}"
    "--bioseq-expected-module-inventory-sha256=${BIOSEQ_GCC16_MODULES_SHA256}"
    "--bioseq-expected-module-source-closure-sha256=${BIOSEQ_GCC16_MODULE_SOURCE_CLOSURE_SHA256}")
  set(BIOSEQ_GCC16_CXX_COMPILER_LAUNCHER
    "${CMAKE_SOURCE_DIR}/cmake/gcc_hwy_header_unit_launcher.py"
    "--bioseq-expected-compiler-sha256=${BIOSEQ_GCC16_CXX_SHA256}"
    "--bioseq-gcc-root=${BIOSEQ_GCC16_ROOT}"
    "--bioseq-gcc-module-inventory=${_BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULES}"
    "--bioseq-expected-module-inventory-sha256=${BIOSEQ_GCC16_MODULES_SHA256}"
    "--bioseq-expected-module-source-closure-sha256=${BIOSEQ_GCC16_MODULE_SOURCE_CLOSURE_SHA256}")
  set(CMAKE_C_COMPILER_LAUNCHER
    "${BIOSEQ_GCC16_C_COMPILER_LAUNCHER}"
    CACHE STRING "Authenticated GCC C compiler launcher" FORCE)
  set(CMAKE_CXX_COMPILER_LAUNCHER
    "${BIOSEQ_GCC16_CXX_COMPILER_LAUNCHER}"
    CACHE STRING "Authenticated GCC C++ compiler launcher" FORCE)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
  set(CMAKE_CXX_STDLIB_MODULES_JSON
    "${_BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULES}"
    CACHE FILEPATH "Authenticated libstdc++ module inventory" FORCE)
  set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
else()
  message(FATAL_ERROR "SET CMAKE_NAMED_ENV_TYPE to valid value!")
endif()

# Compiler paths are production-evidence coordinates.  Persist both selected
# languages uniformly so the sealed DSO receipt can verify the live CMake
# context on every supported local and package toolchain.
set(CMAKE_C_COMPILER "${CMAKE_C_COMPILER}"
  CACHE FILEPATH "Selected C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${CMAKE_CXX_COMPILER}"
  CACHE FILEPATH "Selected C++ compiler" FORCE)

# Distribution packages must never retain a path into the disposable build
# container. Force this globally before targets are created; target-level
# properties below provide a second, auditable guard on the two shipped DSOs.
if(BIOSEQ_PACKAGE_BUILD)
  set(CMAKE_SKIP_RPATH ON CACHE BOOL
    "Do not emit build/install RPATHs in distribution packages" FORCE)
  set(CMAKE_SKIP_INSTALL_RPATH ON CACHE BOOL
    "Do not emit install RPATHs in distribution packages" FORCE)
  set(CMAKE_BUILD_WITH_INSTALL_RPATH OFF CACHE BOOL
    "Package builds never use an install RPATH in the build tree" FORCE)
endif()

# Fallback defaults for the per-build-type flag variables. These run AFTER
# CMake has imported cache values from CMakePresets.json, so they MUST be
# guarded — an unconditional `set(CMAKE_CXX_FLAGS_DEBUG "-g")` silently
# clobbers presets that explicitly cleared the variable to drop -g (e.g.
# bench-opt sets it to "" so DWARF generation is OFF for the -O3 + Debug
# combo; without the guard, every bench-opt build compiles every TU with
# -g and pays the DWARF RSS tax that the preset comment warns about
# — dispatch_buffer.cppm jumps from ~5 GB to >11 GB peak RSS, which is
# enough to push lower-RAM Apple Silicon hosts into swap and stall single
# compiles for hours on memory-pressure-driven major page faults; see
# DOC{NNNN}_BUILD_SPEED_RAM_LIMITED). Using DEFINED CACHE{...} keeps
# the fallback for non-preset configures while letting any preset value
# (including the empty string) take precedence.
if(NOT DEFINED CACHE{CMAKE_CXX_FLAGS_DEBUG})
  set(CMAKE_CXX_FLAGS_DEBUG "-g")
endif()
if(NOT DEFINED CACHE{CMAKE_CXX_FLAGS_RELEASE})
  set(CMAKE_CXX_FLAGS_RELEASE "-O3")
endif()

message("CMAKE is ${CMAKE_VERSION}")
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS ON)
if(BIOSEQ_BUILD_CORE OR BIOSEQ_BUILD_CACHE)
  # Both production trees use C++26 module interface units. Cache-only builds
  # still need dependency scanning even though they skip the large Bioseq graph.
  set(CMAKE_CXX_MODULE_STD ON)
  set(CMAKE_CXX_SCAN_FOR_MODULES ON)
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Prefer the Command Line Tools SDK on a native macOS configuration, but
# publish it through CMake's authoritative sysroot input before project()
# enables C/C++. Cross configurations must provide their own Apple SDK and
# deployment identity instead of inheriting host defaults.
set(_bioseq_apple_sdk_cache_help
  "Apple SDK used by Bioseq compilation")
set(_bioseq_apple_deployment_cache_help
  "Minimum Apple deployment target used by Bioseq compilation")
set(_bioseq_native_apple_intent FALSE)
if(CMAKE_HOST_APPLE
    AND NOT DEFINED CMAKE_TOOLCHAIN_FILE
    AND NOT DEFINED ENV{CMAKE_TOOLCHAIN_FILE}
    AND (NOT DEFINED CMAKE_SYSTEM_NAME
      OR CMAKE_SYSTEM_NAME STREQUAL "Darwin"))
  set(_bioseq_native_apple_intent TRUE)
endif()
if(_bioseq_native_apple_intent AND NOT CMAKE_OSX_SYSROOT)
  set(_bioseq_default_apple_sdk
    "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
  if(IS_DIRECTORY "${_bioseq_default_apple_sdk}")
    set(CMAKE_OSX_SYSROOT "${_bioseq_default_apple_sdk}" CACHE PATH
      "${_bioseq_apple_sdk_cache_help}" FORCE)
  endif()
  unset(_bioseq_default_apple_sdk)
endif()
if(_bioseq_native_apple_intent
    AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
  set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING
    "${_bioseq_apple_deployment_cache_help}")
endif()
unset(_bioseq_native_apple_intent)

# NOTE: do not append `-isysroot` manually to CMAKE_<LANG>_FLAGS here. CMake
# emits the selected CMAKE_OSX_SYSROOT for each enabled language itself.

# Native build
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")

set(CMAKE_VERBOSE_MAKEFILE ON CACHE BOOL "ON")

if(POLICY CMP0135)
  # Set the timestamps of all extracted contents to the time of the extraction
  cmake_policy(SET CMP0135 NEW)
endif()

# This is your project statement. You should always list languages;
# Listing the version is nice here since it sets lots of useful variables
project(
  bioseq
  VERSION 4.2.3
  LANGUAGES C CXX
)

# `APPLE` now reflects the configured target rather than pre-project host
# state. Remove only exact cache values proven to have been seeded by Bioseq's
# former host defaults; explicit command-line/toolchain identities retain
# their distinct help marker and remain fail-closed in probe validation.
if(NOT APPLE)
  if(DEFINED CACHE{CMAKE_OSX_SYSROOT})
    get_property(_bioseq_legacy_sysroot_value CACHE
      CMAKE_OSX_SYSROOT PROPERTY VALUE)
    get_property(_bioseq_legacy_sysroot_help CACHE
      CMAKE_OSX_SYSROOT PROPERTY HELPSTRING)
    if(_bioseq_legacy_sysroot_value STREQUAL
        "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
        AND _bioseq_legacy_sysroot_help STREQUAL
        "${_bioseq_apple_sdk_cache_help}")
      unset(CMAKE_OSX_SYSROOT CACHE)
      if(CMAKE_OSX_SYSROOT STREQUAL "${_bioseq_legacy_sysroot_value}")
        unset(CMAKE_OSX_SYSROOT)
      endif()
    endif()
    unset(_bioseq_legacy_sysroot_value)
    unset(_bioseq_legacy_sysroot_help)
  endif()

  if(DEFINED CACHE{CMAKE_OSX_DEPLOYMENT_TARGET})
    get_property(_bioseq_legacy_deployment_value CACHE
      CMAKE_OSX_DEPLOYMENT_TARGET PROPERTY VALUE)
    get_property(_bioseq_legacy_deployment_help CACHE
      CMAKE_OSX_DEPLOYMENT_TARGET PROPERTY HELPSTRING)
    if(_bioseq_legacy_deployment_value STREQUAL "14.0"
        AND (_bioseq_legacy_deployment_help STREQUAL ""
        OR _bioseq_legacy_deployment_help STREQUAL
          "${_bioseq_apple_deployment_cache_help}"))
      unset(CMAKE_OSX_DEPLOYMENT_TARGET CACHE)
      if(CMAKE_OSX_DEPLOYMENT_TARGET STREQUAL
          "${_bioseq_legacy_deployment_value}")
        unset(CMAKE_OSX_DEPLOYMENT_TARGET)
      endif()
    endif()
    unset(_bioseq_legacy_deployment_value)
    unset(_bioseq_legacy_deployment_help)
  endif()
endif()
unset(_bioseq_apple_sdk_cache_help)
unset(_bioseq_apple_deployment_cache_help)

# CMake's import-std support creates this static helper during project().  Its
# objects are folded into static libraries that are subsequently linked into
# the PostgreSQL shared modules, so they must be position-independent too.
if(TARGET __cmake_cxx26)
  set_property(TARGET __cmake_cxx26
    PROPERTY POSITION_INDEPENDENT_CODE ON)
endif()

if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64"
    OR CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_LINUX_CLANG22")
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"
      OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "22.1.6")
    message(FATAL_ERROR
      "${CMAKE_NAMED_ENV_TYPE} requires clang++ >= 22.1.6; found "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} at "
      "${CMAKE_CXX_COMPILER}.")
  endif()
endif()

if(BIOSEQ_PACKAGE_BUILD)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "BIOSEQ_PACKAGE_BUILD supports native Linux only")
  endif()
  if(NOT CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_LINUX_CLANG22")
    message(FATAL_ERROR
      "BIOSEQ_PACKAGE_BUILD requires AWS_MACHINE_LINUX_CLANG22")
  endif()
  if(NOT DEFINED ENV{BIOSEQ_OPTTIMIZATION_LEVEL}
      OR NOT "$ENV{BIOSEQ_OPTTIMIZATION_LEVEL}" STREQUAL "0"
      OR NOT "${BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE}" STREQUAL "0")
    message(FATAL_ERROR
      "every package build must explicitly set BIOSEQ_OPTTIMIZATION_LEVEL=0 "
      "and BIOSEQ_OPTTIMIZATION_LEVEL_OVERRIDE=0")
  endif()

  set(BIOSEQ_PACKAGE_LIBCXX_ARCHIVE "" CACHE FILEPATH
    "Pinned static libc++ archive used by package DSOs")
  set(BIOSEQ_PACKAGE_LIBCXXABI_ARCHIVE "" CACHE FILEPATH
    "Pinned static libc++abi archive used by package DSOs")
  set(BIOSEQ_PACKAGE_LIBUNWIND_ARCHIVE "" CACHE FILEPATH
    "Pinned static libunwind archive used by package DSOs")
  foreach(_bioseq_runtime_pair IN ITEMS
      "BIOSEQ_PACKAGE_LIBCXX_ARCHIVE;libc++.a"
      "BIOSEQ_PACKAGE_LIBCXXABI_ARCHIVE;libc++abi.a"
      "BIOSEQ_PACKAGE_LIBUNWIND_ARCHIVE;libunwind.a")
    list(GET _bioseq_runtime_pair 0 _bioseq_runtime_variable)
    list(GET _bioseq_runtime_pair 1 _bioseq_runtime_basename)
    if(NOT IS_ABSOLUTE "${${_bioseq_runtime_variable}}"
        OR NOT EXISTS "${${_bioseq_runtime_variable}}")
      message(FATAL_ERROR
        "${_bioseq_runtime_variable} must identify an existing absolute archive")
    endif()
    file(REAL_PATH "${${_bioseq_runtime_variable}}" _bioseq_runtime_real)
    get_filename_component(_bioseq_runtime_name
      "${_bioseq_runtime_real}" NAME)
    if(NOT _bioseq_runtime_name STREQUAL _bioseq_runtime_basename)
      message(FATAL_ERROR
        "${_bioseq_runtime_variable} must name ${_bioseq_runtime_basename}")
    endif()
    file(REAL_PATH "${BIOSEQ_LLVM_ROOT}" _bioseq_package_llvm_root_real)
    string(FIND "${_bioseq_runtime_real}/"
      "${_bioseq_package_llvm_root_real}/" _bioseq_runtime_under_llvm)
    if(NOT _bioseq_runtime_under_llvm EQUAL 0)
      message(FATAL_ERROR
        "${_bioseq_runtime_variable} must reside under pinned BIOSEQ_LLVM_ROOT")
    endif()
    set(${_bioseq_runtime_variable} "${_bioseq_runtime_real}" CACHE FILEPATH
      "Pinned package compiler runtime archive" FORCE)
  endforeach()
  unset(_bioseq_runtime_pair)
  unset(_bioseq_runtime_variable)
  unset(_bioseq_runtime_basename)
  unset(_bioseq_runtime_real)
  unset(_bioseq_runtime_name)
  unset(_bioseq_runtime_under_llvm)
  unset(_bioseq_package_llvm_root_real)

  # Suppress Clang's implicit shared C++ runtime and link the three pinned PIC
  # archives after target objects/libraries. The rescan group handles the
  # circular libc++ <-> libc++abi exception/runtime references. compiler-rt
  # avoids reintroducing a dynamic libgcc_s dependency through the driver.
  add_library(bioseq_package_static_cxx_runtime INTERFACE)
  target_link_options(bioseq_package_static_cxx_runtime INTERFACE
    -stdlib=libc++
    -rtlib=compiler-rt
    -unwindlib=none
    -nostdlib++
    "LINKER:--exclude-libs,libc++.a:libc++abi.a:libunwind.a")
  target_link_libraries(bioseq_package_static_cxx_runtime INTERFACE
    "-Wl,--start-group"
    "${BIOSEQ_PACKAGE_LIBCXX_ARCHIVE}"
    "${BIOSEQ_PACKAGE_LIBCXXABI_ARCHIVE}"
    "${BIOSEQ_PACKAGE_LIBUNWIND_ARCHIVE}"
    "-Wl,--end-group")
endif()
if(CMAKE_NAMED_ENV_TYPE STREQUAL "AWS_MACHINE_X64_GCC16")
  file(REAL_PATH "${CMAKE_C_COMPILER}" _BIOSEQ_CONFIGURED_GCC_C_REAL)
  file(REAL_PATH "${CMAKE_CXX_COMPILER}" _BIOSEQ_CONFIGURED_GCC_CXX_REAL)
  file(REAL_PATH "${CMAKE_CXX_STDLIB_MODULES_JSON}"
    _BIOSEQ_CONFIGURED_LIBSTDCXX_MODULES_REAL)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
      OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS "16.1"
      OR NOT _BIOSEQ_CONFIGURED_GCC_C_REAL
        STREQUAL _BIOSEQ_AUTHENTICATED_GCC16_C
      OR NOT _BIOSEQ_CONFIGURED_GCC_CXX_REAL
        STREQUAL _BIOSEQ_AUTHENTICATED_GCC16_CXX
      OR NOT _BIOSEQ_CONFIGURED_LIBSTDCXX_MODULES_REAL
        STREQUAL _BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULES)
    message(FATAL_ERROR
      "AWS_MACHINE_X64_GCC16 requires the authenticated GNU >= 16.1 "
      "compiler/inventory pair selected before project(); configured "
      "${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} at "
      "${CMAKE_CXX_COMPILER} with "
      "${CMAKE_CXX_STDLIB_MODULES_JSON}.")
  endif()
  bioseq_verify_gcc16_toolchain_identity(
    ROOT "${_BIOSEQ_AUTHENTICATED_GCC16_ROOT}"
    C "${_BIOSEQ_AUTHENTICATED_GCC16_C}"
    CXX "${_BIOSEQ_AUTHENTICATED_GCC16_CXX}"
    MODULES "${_BIOSEQ_AUTHENTICATED_LIBSTDCXX_MODULES}"
    VERSION "${BIOSEQ_GCC16_VERSION}"
    C_SHA256 "${BIOSEQ_GCC16_C_SHA256}"
    CXX_SHA256 "${BIOSEQ_GCC16_CXX_SHA256}"
    MODULES_SHA256 "${BIOSEQ_GCC16_MODULES_SHA256}"
    MODULE_SOURCE_CLOSURE_SHA256
      "${BIOSEQ_GCC16_MODULE_SOURCE_CLOSURE_SHA256}")
endif()

# --- Build-speed: Reduced BMI for C++ modules -------------------------------
# clang's reduced BMI (P2615) drops definitions that importers don't need from
# each module's BMI, shrinking the BMI files and cutting the per-TU module
# scan/load cost across the ~70-module graph (x3 targets). Injected via the
# per-module-compile flag so it applies only to our module-interface units and
# leaves -O3 codegen untouched. Probe-guarded so a toolchain lacking the flag
# still configures cleanly.
if(BIOSEQ_BUILD_CORE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag("-fmodules-reduced-bmi" BIOSEQ_HAS_REDUCED_BMI)
  if(BIOSEQ_HAS_REDUCED_BMI)
    string(APPEND CMAKE_CXX_MODULE_MAP_FLAG " -fmodules-reduced-bmi")
    message(STATUS "bioseq: -fmodules-reduced-bmi enabled; CMAKE_CXX_MODULE_MAP_FLAG='${CMAKE_CXX_MODULE_MAP_FLAG}'")
  endif()
endif()

# --- Build-speed: optional throttle for memory-hungry module compiles -------
# Heavy template TUs in this tree (dispatch_buffer.cppm, two_stacks_minimizers
# .cppm, the columnar_customscan/aggregator units, …) peak at 5+ GB RSS per
# clang invocation even at -O3 without -g. On a 16 GB Apple Silicon laptop
# the default `--parallel 32` from build.sh oversubscribes RAM by ~10x and
# the OS resolves it via compressed-memory + swap, which serializes the
# heavy compile against a single CPU core servicing major page faults. This
# is the failure mode that turns a 1-2 second utils.cppm compile on the
# 64 GB development machine into an hours-long hang on the smaller box.
#
# Setting BIOSEQ_HEAVY_JOBS=N at configure time defines a Ninja job pool
# `heavy_compile=N` that caps the number of concurrent C++ module compiles
# while leaving the rest of the build at full --parallel concurrency. The
# pool is attached to the module-heavy targets in bioseq/CMakeLists.txt.
# Recommended heuristic: 1 heavy compile per ~6 GB of host RAM (so 2 on a
# 16 GB box, 4 on a 32 GB box, leave empty/unset on >=64 GB).
if(BIOSEQ_BUILD_CORE)
  # SQL boundary proofs launch full Clang AST/PCM closures outside ordinary
  # target compilation. Keep those proof processes mutually exclusive while
  # leaving the remaining Ninja jobs available to extension compiles.
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS
    sql_codegen_ast_proof=1)

  set(BIOSEQ_HEAVY_JOBS "" CACHE STRING
    "Cap on concurrent C++ module compiles for the module-heavy targets. \
Empty/0 = unlimited; set to e.g. 2 on a 16 GB host.")
  if(BIOSEQ_HEAVY_JOBS AND BIOSEQ_HEAVY_JOBS GREATER 0)
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS heavy_compile=${BIOSEQ_HEAVY_JOBS})
    message(STATUS "bioseq: throttling heavy module compiles to ${BIOSEQ_HEAVY_JOBS} concurrent (Ninja JOB_POOL=heavy_compile)")
  endif()
endif()

# --- Build-speed: optional -ftime-trace for profiling slow compiles ---------
# When BIOSEQ_TIME_TRACE=ON every C++ TU emits an .o.json file (chrome
# tracing format) next to its .o. Drop the files into chrome://tracing or
# https://ui.perfetto.dev to see exactly where clang is spending its time
# (header parsing vs. template instantiation vs. constexpr eval vs. backend
# codegen). The flag has no effect on the emitted .o, so we can keep -O3
# while profiling. Off by default because the JSON files are ~1-5 MB each
# and bloat the build tree.
option(BIOSEQ_TIME_TRACE
  "Emit clang -ftime-trace JSON per TU into the build tree (profiling only)" OFF)
set(BIOSEQ_TIME_TRACE_GRANULARITY "" CACHE STRING
  "Optional non-negative clang -ftime-trace-granularity value")
if(NOT "${BIOSEQ_TIME_TRACE_GRANULARITY}" STREQUAL ""
   AND NOT "${BIOSEQ_TIME_TRACE_GRANULARITY}" MATCHES "^[0-9]+$")
  message(FATAL_ERROR
    "BIOSEQ_TIME_TRACE_GRANULARITY must be empty or a non-negative integer; "
    "got '${BIOSEQ_TIME_TRACE_GRANULARITY}'")
endif()
if(BIOSEQ_TIME_TRACE)
  set(_bioseq_time_trace_flags " -ftime-trace")
  if(NOT "${BIOSEQ_TIME_TRACE_GRANULARITY}" STREQUAL "")
    string(APPEND _bioseq_time_trace_flags
      " -ftime-trace-granularity=${BIOSEQ_TIME_TRACE_GRANULARITY}")
  endif()
  string(APPEND CMAKE_C_FLAGS "${_bioseq_time_trace_flags}")
  string(APPEND CMAKE_CXX_FLAGS "${_bioseq_time_trace_flags}")
  message(STATUS
    "bioseq: clang time trace enabled (${_bioseq_time_trace_flags}); "
    "profiling JSON written next to each .o")
endif()

# set this to "${PROJECT_VERSION}" on release
#set(INSTALL_VERSION "${PROJECT_VERSION}")
set(INSTALL_VERSION "1.0")
set(H3_CORE_VERSION 4.2.0)
set(H3_CORE_SHA256 438db46fc2b388785d2a0d8e26aa5509739240a7b50b2510c416778d871a4e11)

# Support color diagnostics
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdiagnostics-color=always -Wno-everything")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color=always -Wno-everything")

# If you set any CMAKE_ variables, that can go here.
# (But usually don't do this, except maybe for C++ standard)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")


include(AddPostgreSQLExtension)
include(BioseqSqlAnnotationModel)

# add_definitions(-DPOSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})

# https://cliutils.gitlab.io/modern-cmake/chapters/testing.html
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  include(CTest)
endif()

# The website and core namespace-materialization generator need the raw root
# compile database as the signature source for their isolated Clang closures.
if(BIOSEQ_BUILD_WEBSITE)
  if(NOT BIOSEQ_BUILD_CORE)
    message(FATAL_ERROR
      "BIOSEQ_BUILD_WEBSITE=ON requires BIOSEQ_BUILD_CORE=ON")
  endif()
endif()
if(BIOSEQ_BUILD_CORE OR BIOSEQ_BUILD_WEBSITE)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# Provision the reusable isolated code generator once for the enabled component
# union (extension SQL + external API clients + website). Consumers depend on
# the returned bioseq_codegen / bioseq_codegen_project targets and never create
# another ExternalProject.
include(BioseqCodegen)
include(BioseqHighwayCodegen)
include(BioseqTemplateFlattenCodegen)
if(BIOSEQ_BUILD_CORE)
  set(_bioseq_highway_builder_contract_root
    "${CMAKE_CURRENT_BINARY_DIR}/cmake/highway-builder-contracts")
  add_custom_target(bioseq_highway_builder_contracts
    COMMAND "${CMAKE_COMMAND}" -E rm -rf
      "${_bioseq_highway_builder_contract_root}"
    COMMAND "${CMAKE_COMMAND}" -E echo
      "[bioseq-highway-contract] BEGIN header-closure"
    COMMAND "${CMAKE_COMMAND}"
      "-DTEST_ROOT=${_bioseq_highway_builder_contract_root}/header-closure"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_highway_header_closure.cmake"
    COMMAND "${CMAKE_COMMAND}" -E echo
      "[bioseq-highway-contract] PASS header-closure"
    COMMAND "${CMAKE_COMMAND}" -E echo
      "[bioseq-highway-contract] BEGIN target-order"
    COMMAND "${CMAKE_COMMAND}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_highway_target_order.cmake"
    COMMAND "${CMAKE_COMMAND}" -E echo
      "[bioseq-highway-contract] PASS target-order"
    COMMAND "${CMAKE_COMMAND}" -E echo
      "[bioseq-highway-contract] BEGIN probe-identity"
    COMMAND "${CMAKE_COMMAND}"
      "-DTEST_ROOT=${_bioseq_highway_builder_contract_root}/probe-identity"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_highway_probe_identity.cmake"
    COMMAND "${CMAKE_COMMAND}" -E echo
      "[bioseq-highway-contract] PASS probe-identity"
    COMMENT "Running always-on Highway CMake builder contracts"
    VERBATIM
    USES_TERMINAL)
endif()
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME AND BUILD_TESTING)
  add_test(
    NAME bioseq_highway_header_closure_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cmake/highway-header-closure-contract"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_highway_header_closure.cmake")
  set_tests_properties(
    bioseq_highway_header_closure_contract
    PROPERTIES LABELS "core;unit;cmake;highway")
  add_test(
    NAME bioseq_highway_target_order_contract
    COMMAND "${CMAKE_COMMAND}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_highway_target_order.cmake")
  set_tests_properties(
    bioseq_highway_target_order_contract
    PROPERTIES LABELS "core;unit;cmake;highway")
  add_test(
    NAME bioseq_highway_probe_identity_contract
    COMMAND "${CMAKE_COMMAND}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/cmake/highway-probe-identity-contract"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/test_highway_probe_identity.cmake")
  set_tests_properties(
    bioseq_highway_probe_identity_contract
    PROPERTIES LABELS "core;unit;cmake;highway")
endif()
set(_bioseq_codegen_components)
if(BIOSEQ_BUILD_CORE OR BIOSEQ_BUILD_CACHE)
  list(APPEND _bioseq_codegen_components sql)
endif()
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  list(APPEND _bioseq_codegen_components external)
endif()
if(BIOSEQ_BUILD_WEBSITE)
  list(APPEND _bioseq_codegen_components website)
endif()
if(BIOSEQ_BUILD_CORE)
  list(APPEND _bioseq_codegen_components highway namespace_materialization)
endif()
if(_bioseq_codegen_components)
  bioseq_add_codegen(
    TARGET bioseq_codegen
    PROJECT_TARGET bioseq_codegen_project
    BINARY_DIR "${CMAKE_BINARY_DIR}/codegen"
    COMPONENTS ${_bioseq_codegen_components})
endif()

if(BIOSEQ_BUILD_CORE)
  # Build-only P0 gate for the private implementation-partition topology used
  # by generated template-flatten coordinates. This target intentionally has
  # no runtime test registration: successful module scan, compile, and link on
  # both supported toolchains is the contract.
  add_subdirectory(
    codegen/namespace_materialization/tests/module_canary)
endif()

# Include extensions and their shared code
#add_subdirectory(include)
if(BIOSEQ_BUILD_CORE OR BIOSEQ_BUILD_CACHE)
  bioseq_ensure_sql_annotation_model()
  add_subdirectory(common/checked_arithmetic)
  add_subdirectory(common/durable_file_io)
  add_subdirectory(common/cache_provider_api)
  add_subdirectory(common/memory_budget)
  add_subdirectory(common/pg_support)
endif()

if(BIOSEQ_BUILD_CACHE)
  add_subdirectory(bioseq_caching)
endif()

if(TARGET bioseq_cache_unit_tests)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  add_custom_target(
    bioseq_build_mcp_configure_freshness_contract_tests
    COMMAND
      "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/build_mcp/tests/test_cmake_configure_freshness.py"
    COMMAND
      "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/build_mcp/tests/test_postgresql_config_invariant.py"
    COMMAND
      "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/build_mcp/tests/test_postgres_error_safety_mode.py"
    COMMAND
      "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/build_mcp/tests/test_cache_modes.py"
    COMMAND
      "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/build_mcp/tests/test_package_linux.py"
    COMMAND
      "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/cmake/test_production_dso_qualification.py"
    COMMAND
      "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/cmake/test_postgresql_extension_elf_visibility.py"
    COMMAND
      "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
      "${Python3_EXECUTABLE}"
      "${CMAKE_SOURCE_DIR}/cmake/test_postgresql_extension_macho_visibility.py"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    COMMENT "Running bioseq-build configure freshness contract tests"
    VERBATIM)
endif()

if(BIOSEQ_BUILD_CORE)
  add_subdirectory(bioseq)
  foreach(_bioseq_highway_validation_consumer IN ITEMS
      bioseq_aligner_tests_exec
      bioseq_external_integration_tests_exec)
    if(TARGET ${_bioseq_highway_validation_consumer})
      add_dependencies(
        ${_bioseq_highway_validation_consumer}
        bioseq_highway_builder_contracts
        bioseq_namespace_materialization_module_canary)
    endif()
  endforeach()
  unset(_bioseq_highway_validation_consumer)
  if(TARGET bioseq_cache_bioseq_bench)
    # The cache benchmark target is declared before Bioseq so cache-only builds
    # remain valid. Complete the direct-target dependency once the sibling DSO
    # exists; callers must never have to know this declaration order.
    add_dependencies(
      bioseq_cache_bioseq_bench
      postgresql_bioseq)
  endif()
  if(BIOSEQ_BUILD_CACHE AND BIOSEQ_CACHE_BUILD_TESTS)
    include(
      "${CMAKE_SOURCE_DIR}/cmake/BioseqPrivatePostgresStage.cmake")
    bioseq_add_private_postgres_stage(
      TARGET bioseq_cache_bioseq_production_postgres_stage
      PG_CONFIG "${PostgreSQL_CONFIG}"
      EXTENSION_NAMES
        bioseq_cache
        bioseq
      EXTENSION_TARGETS
        postgresql_bioseq_cache
        postgresql_bioseq
      CONTROL_PROPERTIES
        BIOSEQ_EXTENSION_CONTROL_FILE
        BIOSEQ_EXTENSION_CONTROL_FILE
      INSTALL_SQL_PROPERTIES
        BIOSEQ_EXTENSION_INSTALL_SQL
        BIOSEQ_EXTENSION_INSTALL_SQL)
    include(
      "${CMAKE_SOURCE_DIR}/cmake/BioseqProductionDsoQualification.cmake")
    bioseq_add_production_dso_qualification(
      TARGET bioseq_cache_bioseq_production_dso_qualification
      BIOSEQ_TARGET postgresql_bioseq
      CACHE_TARGET postgresql_bioseq_cache)
    add_dependencies(
      bioseq_cache_bioseq_bench
      bioseq_cache_bioseq_production_postgres_stage)

    if(TARGET postgresql_bioseq_cache_test_support)
      bioseq_add_private_postgres_stage(
        TARGET bioseq_cache_bioseq_support_postgres_stage
        PG_CONFIG "${PostgreSQL_CONFIG}"
        EXTENSION_NAMES
          bioseq_cache
          bioseq
        EXTENSION_TARGETS
          postgresql_bioseq_cache_test_support
          postgresql_bioseq
        CONTROL_PROPERTIES
          BIOSEQ_EXTENSION_CONTROL_FILE
          BIOSEQ_EXTENSION_CONTROL_FILE
        INSTALL_SQL_PROPERTIES
          BIOSEQ_EXTENSION_TEST_INSTALL_SQL
          BIOSEQ_EXTENSION_INSTALL_SQL)

      # The Bioseq-only acceptance cases must prove the cache artifacts are
      # physically absent, not merely unselected from a combined stage.
      bioseq_add_private_postgres_stage(
        TARGET bioseq_cache_bioseq_only_postgres_stage
        PG_CONFIG "${PostgreSQL_CONFIG}"
        EXTENSION_NAMES
          bioseq
        EXTENSION_TARGETS
          postgresql_bioseq
        CONTROL_PROPERTIES
          BIOSEQ_EXTENSION_CONTROL_FILE
        INSTALL_SQL_PROPERTIES
          BIOSEQ_EXTENSION_INSTALL_SQL)
    endif()
  endif()
  include(BioseqExternalPostgresAcceptance)
  add_subdirectory(vendor)
endif()

if(BIOSEQ_PACKAGE_BUILD)
  foreach(_bioseq_package_target IN ITEMS
      postgresql_bioseq postgresql_bioseq_cache)
    if(NOT TARGET ${_bioseq_package_target})
      message(FATAL_ERROR
        "package build did not create required target ${_bioseq_package_target}")
    endif()
    target_link_libraries(${_bioseq_package_target} PRIVATE
      bioseq_package_static_cxx_runtime)
    target_compile_definitions(${_bioseq_package_target} PRIVATE
      BIOSEQ_PACKAGE_O0_PROFILE=1
      BIOSEQ_PIPELINE_FORCE_INLINE=0)
    set_target_properties(${_bioseq_package_target} PROPERTIES
      BUILD_RPATH ""
      INSTALL_RPATH ""
      SKIP_BUILD_RPATH TRUE
      BUILD_WITH_INSTALL_RPATH FALSE
      INTERPROCEDURAL_OPTIMIZATION FALSE)
  endforeach()

  # Package scripts consume the exact production target files. A generated
  # inventory avoids ambiguous recursive basename discovery when a persistent
  # build tree also contains a noninstalled test-support DSO.
  file(GENERATE
    OUTPUT "${CMAKE_BINARY_DIR}/bioseq-package-target-artifacts.tsv"
    CONTENT
"postgresql_bioseq\t$<TARGET_FILE:postgresql_bioseq>
postgresql_bioseq_cache\t$<TARGET_FILE:postgresql_bioseq_cache>
")
endif()

# The website pipeline consumes postgresql_bioseq's target properties and BMIs,
# so it is added after add_subdirectory(bioseq) (§17.1).
if(BIOSEQ_BUILD_WEBSITE)
  add_subdirectory(website)
endif()

if(BIOSEQ_BUILD_PAPERS)
  add_subdirectory(papers)
endif()
#add_subdirectory(h3_postgis)

# The combined native Catch2 runner is declared only after both independently
# installable extension DSOs exist. It owns a private PostgreSQL cluster and
# stages the exact control, generated SQL, and DSO target artifacts.
if(BIOSEQ_BUILD_CORE AND BIOSEQ_BUILD_CACHE
   AND BIOSEQ_CACHE_BUILD_TESTS AND BIOSEQ_CACHE_TESTING)
  if(NOT TARGET postgresql_bioseq_cache
     OR NOT TARGET postgresql_bioseq_cache_test_support
     OR NOT TARGET postgresql_bioseq)
    message(FATAL_ERROR
      "Combined cache/Bioseq tests require production cache, test-support "
      "cache, and Bioseq extension artifact targets")
  endif()
  add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/bioseq_caching/test/bioseq"
    "${CMAKE_CURRENT_BINARY_DIR}/bioseq_caching/test/bioseq")
endif()

# Add target that bundles for pgxn
#configure_file(META.json.in META.json)
# add_custom_target(pgxn
#   COMMAND git archive --format zip
#                       --prefix="h3-${INSTALL_VERSION}/"
#                       --add-file ${CMAKE_BINARY_DIR}/META.json
#                       -o h3-${INSTALL_VERSION}.zip HEAD
#   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
# )
