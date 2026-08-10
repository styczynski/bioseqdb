# IO module setup and benchmark target wiring.

include(FetchContent)
set(_bioseq_io_build_testing_was_defined FALSE)
if(DEFINED BUILD_TESTING)
  set(_bioseq_io_build_testing_was_defined TRUE)
  set(_bioseq_io_saved_build_testing "${BUILD_TESTING}")
endif()
set(BUILD_TESTING OFF)
FetchContent_Declare(kseq++ GIT_REPOSITORY https://github.com/cartoonist/kseqpp.git GIT_TAG v1.1.2)
FetchContent_MakeAvailable(kseq++)
if(_bioseq_io_build_testing_was_defined)
  set(BUILD_TESTING "${_bioseq_io_saved_build_testing}")
else()
  unset(BUILD_TESTING)
endif()
unset(_bioseq_io_saved_build_testing)
unset(_bioseq_io_build_testing_was_defined)

# kseq++'s upstream CMakeLists hard-requires BZip2 and chains its
# INTERFACE_INCLUDE_DIRECTORIES through the kseq++ target, even though
# kseq++'s actual headers only #include <zlib.h> and <thread>. On macOS
# `find_package(BZip2)` resolves to the Xcode SDK's `usr/include`, which
# we then re-inject via `-isystem` ahead of the `-isysroot` that points
# at the Command Line Tools SDK. That mismatch makes libc++'s `<cctype>`
# fail to locate its own `<ctype.h>` wrapper. Strip kseq++ down to the
# only deps it really uses: its own headers + ZLIB.
set_target_properties(kseq++ PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES
    "$<BUILD_INTERFACE:${kseq++_SOURCE_DIR}/include>"
  INTERFACE_LINK_LIBRARIES
    "$<BUILD_INTERFACE:ZLIB::ZLIB>")

# The benchmark owns its launcher and named logic module under benchmark/.
include("${CMAKE_CURRENT_LIST_DIR}/benchmark/build.cmake")
