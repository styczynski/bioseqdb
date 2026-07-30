find_package(Catch2 3 REQUIRED)

if(NOT TARGET bioseq_cache_test_context)
  add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../support"
    "${CMAKE_CURRENT_BINARY_DIR}/support")
endif()

add_executable(bioseq_cache_bioseq_integration_tests
  "${CMAKE_CURRENT_LIST_DIR}/../support/cache_catch_runner.cpp"
  test_coexistence.cpp
  test_coexistence_acceptance.cpp
  test_exact_provider.cpp
  test_bioseq_2gbp_cache.cpp)
set_target_properties(bioseq_cache_bioseq_integration_tests PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
target_link_libraries(bioseq_cache_bioseq_integration_tests
  PRIVATE
    bioseq_cache_test_context
    Catch2::Catch2)
add_dependencies(
  bioseq_cache_bioseq_integration_tests
  postgresql_bioseq_cache_test_support
  postgresql_bioseq)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/reports")
add_test(
  NAME bioseq_cache_bioseq_integration_tests
  COMMAND bioseq_cache_bioseq_integration_tests
    --postgres-bin-dir "${PostgreSQL_BIN_DIR}"
    --cache-control
      "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_CONTROL_FILE>"
    --cache-install-sql
      "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_TEST_INSTALL_SQL>"
    --cache-dso
      "$<TARGET_GENEX_EVAL:postgresql_bioseq_cache_test_support,$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_DSO>>"
    --bioseq-control
      "$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_CONTROL_FILE>"
    --bioseq-install-sql
      "$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_INSTALL_SQL>"
    --bioseq-dso
      "$<TARGET_GENEX_EVAL:postgresql_bioseq,$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_DSO>>"
    --migration-ledger
      "${CMAKE_CURRENT_LIST_DIR}/../legacy_migration_ledger.tsv"
    --report
      "${CMAKE_BINARY_DIR}/reports/bioseq-cache-bioseq-integration.json"
    --case-timeout-seconds 6300)
set_tests_properties(bioseq_cache_bioseq_integration_tests PROPERTIES
  LABELS "cache;integration;bioseq;2gbp"
  RESOURCE_LOCK "bioseq_private_postgres"
  TIMEOUT 7200)

include(
  "${CMAKE_CURRENT_LIST_DIR}/../support/LegacyMigrationContract.cmake")
bioseq_cache_add_legacy_migration_contract(
  LEDGER
    "${CMAKE_CURRENT_LIST_DIR}/../legacy_migration_ledger.tsv"
  LEGACY_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/../.."
  TARGETS
    bioseq_cache_unit_tests
    bioseq_cache_integration_tests
    bioseq_cache_bioseq_integration_tests
  LEGACY_ASSETS
    bench/run_private_cluster_bench.py
    bench/sql_cases.json
    bench/test_run_private_cluster_bench.py
    sql/bioseq_cache_extension.sql.in
    sql/bioseq_cache_objects.sql.in
    sql/bioseq_cache_test_objects.sql.in
    test/run_private_provider_tests.py
    test/regress
    test/isolation
    test/tap)
