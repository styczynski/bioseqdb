# Finding dependencies
find_package(ZLIB REQUIRED)   # required by SeqAn
find_package(BZip2 REQUIRED)  # required by SeqAn
find_package(OpenMP)          # required by SeqAn
find_package(SeqAn REQUIRED CONFIG)

if (SeqAn_FOUND AND NOT TARGET SeqAn::SeqAn)
  add_library(SeqAn::SeqAn INTERFACE IMPORTED)
  set_target_properties(SeqAn::SeqAn PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${SEQAN_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${SEQAN_LIBRARIES}")
endif()

set(CMAKE_BUILD_TYPE "Release")

# Defining target kseq++-bench
add_executable(kseq++-bench kseq++_bench.cpp)
target_compile_options(kseq++-bench PRIVATE -g -Wall -Wpedantic -Werror)
target_include_directories(kseq++-bench
  PRIVATE ${PROJECT_SOURCE_DIR}/benchmark/include
  PRIVATE SeqAn::SeqAn
  PRIVATE kseq++::kseq++)
target_link_libraries(kseq++-bench
  PRIVATE SeqAn::SeqAn
  PRIVATE kseq++::kseq++)
