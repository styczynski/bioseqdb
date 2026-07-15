set(_bioseq_cache_bench_entry
  "${CMAKE_CURRENT_BINARY_DIR}/cache_bench_main.cpp")
file(GENERATE OUTPUT "${_bioseq_cache_bench_entry}" CONTENT [=[
import bioseq.cache.bench;

int main(int argc, char** argv) {
  return bioseq_cache_bench_main(argc, argv);
}
]=])

add_executable(bioseq_cache_bench "${_bioseq_cache_bench_entry}")
target_sources(bioseq_cache_bench
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/cache_bench.cppm")

set_target_properties(bioseq_cache_bench PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)

target_link_libraries(bioseq_cache_bench
  PRIVATE bioseq_cache_core)
