# io/build.cmake — IO module setup. Brings in kseq++ and includes the
# V10 IO bench wiring (`io_bench_v10.cmake`) for the `bioseq_io_bench`
# target.

include(FetchContent)
set(BUILD_TESTING OFF)
FetchContent_Declare(kseq++ GIT_REPOSITORY https://github.com/cartoonist/kseqpp.git GIT_TAG v1.1.2)
FetchContent_MakeAvailable(kseq++)

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

# === bioseq_io_bench (V10) — built on top of bench_framework ===
include("${CMAKE_CURRENT_LIST_DIR}/io_bench_v10.cmake")
