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

# Change this:
set(CMAKE_NAMED_ENV_TYPE "H") # suppported: "W", "H"

if(${CMAKE_NAMED_ENV_TYPE} STREQUAL "H")
  set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm@20/bin/clang)
  set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm@20/bin/clang++)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "d0edc3af-4c50-42ea-a356-e2862fe7a444")
elseif(${CMAKE_NAMED_ENV_TYPE} STREQUAL "W")
  set(CMAKE_C_COMPILER /usr/local/opt/llvm@21/bin/clang)
  set(CMAKE_CXX_COMPILER /usr/local/opt/llvm@21/bin/clang++)
  set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
else()
  message(FATAL_ERROR "SET CMAKE_NAMED_ENV_TYPE to valid value!")
endif()

set(CMAKE_CXX_FLAGS_DEBUG "-g")
set(CMAKE_CXX_FLAGS_RELEASE "-O3")

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

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -isysroot ${SDKROOT}")
SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -isysroot ${SDKROOT}")

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
