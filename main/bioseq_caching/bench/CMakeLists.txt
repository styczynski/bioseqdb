add_executable(bioseq_cache_bench cache_bench.cpp)

set_target_properties(bioseq_cache_bench PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)

target_link_libraries(bioseq_cache_bench
  PRIVATE bioseq_cache_core)
