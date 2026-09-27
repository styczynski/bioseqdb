find_package(Catch2 3 REQUIRED)

if(NOT TARGET bioseq_cache_test_context)
  add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../support"
    "${CMAKE_CURRENT_BINARY_DIR}/support")
endif()

bioseq_collect_authored_sources(
  BIOSEQ_CACHE_BIOSEQ_LARGE_INPUT_E2E_TEST_SOURCES
  BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
  INCLUDE_PATTERNS "*_test.cpp"
  EXPECTED_COUNT 1)
add_executable(bioseq_cache_bioseq_large_input_e2e_tests
  "${CMAKE_CURRENT_LIST_DIR}/../../support/cache_catch_runner.cpp"
  ${BIOSEQ_CACHE_BIOSEQ_LARGE_INPUT_E2E_TEST_SOURCES})
set_target_properties(
  bioseq_cache_bioseq_large_input_e2e_tests PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
target_link_libraries(
  bioseq_cache_bioseq_large_input_e2e_tests
  PRIVATE
    bioseq_cache_test_context
    Catch2::Catch2)
add_dependencies(
  bioseq_cache_bioseq_large_input_e2e_tests
  postgresql_bioseq_cache
  postgresql_bioseq
  bioseq_cache_bioseq_production_postgres_stage)

set(_bioseq_cache_bioseq_case_timeout_seconds 6300)
set(_bioseq_cache_bioseq_large_input_suite_timeout_seconds 6900)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/reports")
add_test(
  NAME bioseq_cache_bioseq_large_input_e2e_tests
  COMMAND bioseq_cache_bioseq_large_input_e2e_tests
    --postgres-bin-dir
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_production_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>"
    --postgres-stage-root
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_production_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>"
    --postgres-stage-manifest
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_production_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>"
    --cache-control
      "$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_CONTROL_FILE>"
    --cache-install-sql
      "$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_INSTALL_SQL>"
    --cache-dso
      "$<TARGET_GENEX_EVAL:postgresql_bioseq_cache,$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_DSO>>"
    --bioseq-control
      "$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_CONTROL_FILE>"
    --bioseq-install-sql
      "$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_INSTALL_SQL>"
    --bioseq-dso
      "$<TARGET_GENEX_EVAL:postgresql_bioseq,$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_DSO>>"
    --report
      "${CMAKE_BINARY_DIR}/reports/bioseq-cache-bioseq-large-input-e2e.json"
    --case-timeout-seconds
      "${_bioseq_cache_bioseq_case_timeout_seconds}"
    "[component:cache][layer:e2e][feature:result_cache][resource:large_memory]")
set_tests_properties(
  bioseq_cache_bioseq_large_input_e2e_tests PROPERTIES
  LABELS
    "component:cache;layer:e2e;feature:result_cache;resource:large_memory"
  RESOURCE_LOCK "bioseq_private_postgres"
  TIMEOUT "${_bioseq_cache_bioseq_large_input_suite_timeout_seconds}")
