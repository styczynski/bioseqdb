add_library(bioseq_cache_core STATIC)

set_target_properties(bioseq_cache_core PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

target_link_libraries(bioseq_cache_core
  PUBLIC
    bioseq_cache_provider_api
    bioseq_checked_arithmetic_core
    bioseq_memory_budget)

bioseq_collect_authored_sources(
  BIOSEQ_CACHE_CORE_MODULES
  BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
  INCLUDE_PATTERNS "*.cppm"
  EXPECTED_COUNT 16)
target_sources(bioseq_cache_core
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}/.."
    FILES ${BIOSEQ_CACHE_CORE_MODULES})

add_library(bioseq::cache_core ALIAS bioseq_cache_core)
