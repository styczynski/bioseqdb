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
