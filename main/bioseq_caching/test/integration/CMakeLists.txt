find_package(Catch2 3 REQUIRED)

if(NOT TARGET bioseq_cache_test_context)
  add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../support"
    "${CMAKE_CURRENT_BINARY_DIR}/support")
endif()

add_executable(bioseq_cache_integration_tests
  "${CMAKE_CURRENT_LIST_DIR}/../support/cache_catch_runner.cpp"
  test_install_security.cpp
  test_libpq_wire_observer.cpp
  test_exact_cache.cpp
  test_types.cpp
  test_transactions.cpp
  test_concurrency.cpp
  test_isolation_failures.cpp
  test_isolation_waiters.cpp
  test_isolation_visibility.cpp
  test_isolation_provider_validity.cpp
  test_backend_recovery.cpp
  test_cross_database.cpp
  test_owner_liveness.cpp
  test_bootstrap_recovery.cpp
  test_provider.cpp
  test_provider_migration_gaps.cpp
  test_planner.cpp
  test_legacy_dml_parity.cpp
  test_legacy_security_parity.cpp
  test_legacy_upper_parity.cpp
  test_storage_lifecycle.cpp
  test_two_phase_commit.cpp)
set_target_properties(bioseq_cache_integration_tests PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
target_link_libraries(bioseq_cache_integration_tests
  PRIVATE
    bioseq_cache_test_context
    Catch2::Catch2)

if(TARGET postgresql_bioseq_cache_test_support)
  add_dependencies(
    bioseq_cache_integration_tests
    postgresql_bioseq_cache_test_support
    bioseq_cache_test_support_postgres_stage)

  # The external watchdog owns each of the 49 Catch cases independently. Keep
  # CTest's aggregate ceiling above 49 * 480 seconds, with lifecycle/reporting
  # headroom, so it cannot preempt a later case's watchdog.
  set(_bioseq_cache_case_timeout_seconds 480)
  set(_bioseq_cache_suite_timeout_seconds 28800)

  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/reports")
  add_test(
    NAME bioseq_cache_integration_tests
    COMMAND bioseq_cache_integration_tests
      --postgres-bin-dir
        "$<TARGET_GENEX_EVAL:bioseq_cache_test_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_test_support_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>"
      --cache-control
        "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_CONTROL_FILE>"
      --cache-install-sql
        "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_TEST_INSTALL_SQL>"
      --cache-dso
        "$<TARGET_GENEX_EVAL:postgresql_bioseq_cache_test_support,$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_DSO>>"
      --migration-ledger
        "${CMAKE_CURRENT_LIST_DIR}/../legacy_migration_ledger.tsv"
      --report
        "${CMAKE_BINARY_DIR}/reports/bioseq-cache-integration.json"
      --case-timeout-seconds
        "${_bioseq_cache_case_timeout_seconds}")
  set_tests_properties(bioseq_cache_integration_tests PROPERTIES
    LABELS "cache;integration;postgres"
    RESOURCE_LOCK "bioseq_private_postgres"
    TIMEOUT "${_bioseq_cache_suite_timeout_seconds}")
endif()
