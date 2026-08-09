if(TARGET bioseq_cache_test_context)
  return()
endif()

if(TARGET bioseq_durable_file_io_contract_tests)
  set_target_properties(
    bioseq_durable_file_io_contract_tests
    PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
endif()

add_library(bioseq_cache_test_context STATIC)
set_target_properties(bioseq_cache_test_context PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_cache_test_context
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/cache_test_context.cppm")
target_link_libraries(bioseq_cache_test_context
  PUBLIC bioseq_postgres_test_support)

add_executable(
  bioseq_cache_legacy_inventory_tracking
  "${CMAKE_CURRENT_LIST_DIR}/legacy_migration_contract.cpp")
set_target_properties(
  bioseq_cache_legacy_inventory_tracking
  PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    INTERPROCEDURAL_OPTIMIZATION FALSE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
target_include_directories(
  bioseq_cache_legacy_inventory_tracking
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(
  bioseq_cache_legacy_inventory_tracking
  PRIVATE bioseq_postgres_test_support)

find_package(simdjson CONFIG REQUIRED)
add_executable(
  bioseq_cache_legacy_retirement_gate
  "${CMAKE_CURRENT_LIST_DIR}/legacy_retirement_gate.cpp")
set_target_properties(
  bioseq_cache_legacy_retirement_gate
  PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    INTERPROCEDURAL_OPTIMIZATION FALSE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
target_include_directories(
  bioseq_cache_legacy_retirement_gate
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(
  bioseq_cache_legacy_retirement_gate
  PRIVATE
    bioseq_durable_file_io
    bioseq_postgres_test_support
    simdjson::simdjson)
