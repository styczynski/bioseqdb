add_library(bioseq_cache_core STATIC)

set_target_properties(bioseq_cache_core PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)

target_link_libraries(bioseq_cache_core
  PUBLIC
    bioseq_cache_provider_api
    bioseq_memory_budget)

target_sources(bioseq_cache_core
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}/.."
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/index.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/wire.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/checked_math.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/state_machine.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/coverage.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/ttl.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/admission.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/readiness.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/provider_validation.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/column_format.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/reusable_registry.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/../pg/reusable_planning.cppm")

add_library(bioseq::cache_core ALIAS bioseq_cache_core)
