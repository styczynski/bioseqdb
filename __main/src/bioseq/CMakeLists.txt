# Copyright 2020 Mats Kindahl
#
# Licensed under the Apache License, Version 2.0 (the "License"); you may not
# use this file except in compliance with the License. You may obtain a copy of
# the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
# WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
# License for the specific language governing permissions and limitations under
# the License.

set(CMAKE_C_COMPILER "gcc")
set(CMAKE_VERBOSE_MAKEFILE "ON")

message(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
file(GLOB SRC_FILES ${CMAKE_CURRENT_LIST_DIR}/backend/*.c ${CMAKE_CURRENT_LIST_DIR}/include/*.h)
message("SOURCES ${SRC_FILES}")

find_package(Intl)

add_postgresql_extension(
  bioseq
  VERSION 1.0
  SOURCES "${SRC_FILES}"
  SCRIPTS bioseq--1.0.sql
)

include_directories(${CMAKE_CURRENT_LIST_DIR}/include)
include_directories(${Intl_INCLUDE_DIR})
include_directories(${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include)

