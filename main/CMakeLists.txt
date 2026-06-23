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

# Change this (or override it without editing this file):
# CMAKE_NAMED_ENV_TYPE selects the per-machine toolchain block below. Resolution
# order: an environment variable (set e.g. by a CMake preset's `environment`
# field or `export CMAKE_NAMED_ENV_TYPE=...`) wins; then any pre-set cache/normal
# value; otherwise it defaults to "H" (the arm64 macOS "home" box). This keeps a
# single checked-in default while letting the AWS x86_64 reference box select its
# own toolchain via the `aws-x64-linux` preset. Supported: "W", "H", "AWS_MACHINE_X64".
if(DEFINED ENV{CMAKE_NAMED_ENV_TYPE})
  set(CMAKE_NAMED_ENV_TYPE "$ENV{CMAKE_NAMED_ENV_TYPE}")
elseif(NOT DEFINED CMAKE_NAMED_ENV_TYPE)
  set(CMAKE_NAMED_ENV_TYPE "W")
endif()

# Override environment, don't change it
set(CMAKE_NAMED_ENV_TYPE "H")

if(${CMAKE_NAMED_ENV_TYPE} STREQUAL "H")
  set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm@22/bin/clang)
  set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm@22/bin/clang++)
  # The `import std` experimental-feature gate UUID is CMake-version-specific.
  # CMake 4.3.1 (the version installed on this "H" box) expects
  # 451f2fe2-... (same value the "W" llvm box already uses); the previous
  # d0edc3af-... value is for an older CMake and 4.3.1 rejects it with
  # "CMAKE_EXPERIMENTAL_CXX_IMPORT_STD is set to incorrect value", which
  # disables import-std and fails the generate step. (Confirmed against the
  # `CxxImportStd` UUID embedded in the cmake 4.3.1 binary.)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
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
elseif(${CMAKE_NAMED_ENV_TYPE} STREQUAL "AWS_MACHINE_X64")
  # x86_64 Ubuntu reference box provisioned by main/infra (see
  # main/infra/testing_infra + main/infra/provisioner.py). clang/libc++ 22 are
  # installed from apt.llvm.org under /usr/lib/llvm-22; CMake is pinned to 4.2.1,
  # whose import-std experimental UUID is the same value the "H" box uses. The
  # libc++ modules.json lives under a multiarch sub-path on Linux, so the
  # provisioner discovers the exact file and exports BIOSEQ_LIBCXX_MODULES_JSON;
  # we honour that override and otherwise fall back to the canonical location.
  set(CMAKE_C_COMPILER /usr/lib/llvm-22/bin/clang)
  set(CMAKE_CXX_COMPILER /usr/lib/llvm-22/bin/clang++)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
  if(DEFINED ENV{BIOSEQ_LIBCXX_MODULES_JSON})
    set(CMAKE_CXX_STDLIB_MODULES_JSON "$ENV{BIOSEQ_LIBCXX_MODULES_JSON}")
  else()
    set(CMAKE_CXX_STDLIB_MODULES_JSON /usr/lib/llvm-22/lib/x86_64-unknown-linux-gnu/c++/libc++.modules.json)
  endif()
else()
  message(FATAL_ERROR "SET CMAKE_NAMED_ENV_TYPE to valid value!")
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
set(CMAKE_CXX_MODULE_STD ON)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED YES)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE INTERNAL "")

# Support CXX modules on MacOS correctly
if (APPLE)
  set(SDKROOT "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk")
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -isysroot ${SDKROOT}")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isysroot ${SDKROOT}")
endif()

# NOTE: the macOS -isysroot flags are applied inside the if(APPLE) block above.
# A duplicate, *unconditional* copy of those two SET()s used to live here; on a
# non-Apple toolchain (e.g. AWS_MACHINE_X64) SDKROOT is empty there, so it
# expanded to a bare `-isysroot ` that swallowed the next flag and broke every
# compile. Removed — Apple still gets its isysroot from the guarded block.

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
  LANGUAGES CXX
)

# --- Build-speed: Reduced BMI for C++ modules -------------------------------
# clang's reduced BMI (P2615) drops definitions that importers don't need from
# each module's BMI, shrinking the BMI files and cutting the per-TU module
# scan/load cost across the ~70-module graph (x3 targets). Injected via the
# per-module-compile flag so it applies only to our module-interface units and
# leaves -O3 codegen untouched. Probe-guarded so a toolchain lacking the flag
# still configures cleanly.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag("-fmodules-reduced-bmi" BIOSEQ_HAS_REDUCED_BMI)
if(BIOSEQ_HAS_REDUCED_BMI)
  string(APPEND CMAKE_CXX_MODULE_MAP_FLAG " -fmodules-reduced-bmi")
  message(STATUS "bioseq: -fmodules-reduced-bmi enabled; CMAKE_CXX_MODULE_MAP_FLAG='${CMAKE_CXX_MODULE_MAP_FLAG}'")
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
set(BIOSEQ_HEAVY_JOBS "" CACHE STRING
  "Cap on concurrent C++ module compiles for the module-heavy targets. \
Empty/0 = unlimited; set to e.g. 2 on a 16 GB host.")
if(BIOSEQ_HEAVY_JOBS AND BIOSEQ_HEAVY_JOBS GREATER 0)
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS heavy_compile=${BIOSEQ_HEAVY_JOBS})
  message(STATUS "bioseq: throttling heavy module compiles to ${BIOSEQ_HEAVY_JOBS} concurrent (Ninja JOB_POOL=heavy_compile)")
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
if(BIOSEQ_TIME_TRACE)
  string(APPEND CMAKE_C_FLAGS " -ftime-trace")
  string(APPEND CMAKE_CXX_FLAGS " -ftime-trace")
  message(STATUS "bioseq: -ftime-trace enabled; profiling JSON written next to each .o")
endif()

# set this to "${PROJECT_VERSION}" on release
#set(INSTALL_VERSION "${PROJECT_VERSION}")
set(INSTALL_VERSION "1.0")
set(H3_CORE_VERSION 4.2.0)
set(H3_CORE_SHA256 438db46fc2b388785d2a0d8e26aa5509739240a7b50b2510c416778d871a4e11)

# Support color diagnostics
SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fdiagnostics-color=always -Wno-everything")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fdiagnostics-color=always -Wno-everything")

set(CMAKE_OSX_DEPLOYMENT_TARGET "14.0" CACHE STRING "" FORCE)

# If you set any CMAKE_ variables, that can go here.
# (But usually don't do this, except maybe for C++ standard)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")


include(AddPostgreSQLExtension)

# add_definitions(-DPOSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})

# https://cliutils.gitlab.io/modern-cmake/chapters/testing.html
if(CMAKE_PROJECT_NAME STREQUAL PROJECT_NAME)
  include(CTest)
endif()

# Include extensions and their shared code
#add_subdirectory(include)
add_subdirectory(bioseq)
add_subdirectory(vendor)
add_subdirectory(papers)
#add_subdirectory(h3_postgis)

# Add target that bundles for pgxn
#configure_file(META.json.in META.json)
# add_custom_target(pgxn
#   COMMAND git archive --format zip
#                       --prefix="h3-${INSTALL_VERSION}/"
#                       --add-file ${CMAKE_BINARY_DIR}/META.json
#                       -o h3-${INSTALL_VERSION}.zip HEAD
#   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
# )
