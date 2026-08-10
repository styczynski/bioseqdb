find_package(Catch2 3 REQUIRED)

if(NOT TARGET bioseq_cache_test_context)
  add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../support"
    "${CMAKE_CURRENT_BINARY_DIR}/support")
endif()

add_executable(bioseq_cache_integration_tests
  "${CMAKE_CURRENT_LIST_DIR}/../support/cache_catch_runner.cpp"
  backend_recovery_test.cpp
  bootstrap_recovery_test.cpp
  concurrency_test.cpp
  cross_database_test.cpp
  exact_cache_test.cpp
  install_security_test.cpp
  isolation_failures_test.cpp
  isolation_provider_validity_test.cpp
  isolation_visibility_test.cpp
  isolation_waiters_test.cpp
  libpq_wire_observer_test.cpp
  owner_liveness_test.cpp
  planner_test.cpp
  provider_reuse_faults_test.cpp
  provider_test.cpp
  security_policy_test.cpp
  storage_lifecycle_test.cpp
  transaction_and_dml_test.cpp
  transactions_test.cpp
  two_phase_commit_test.cpp
  types_test.cpp
  upper_plan_nodes_test.cpp)
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

  # The external watchdog owns each of the 50 Catch cases independently. Keep
  # CTest's aggregate ceiling above 50 * 480 seconds, with lifecycle/reporting
  # headroom, so it cannot preempt a later case's watchdog.
  set(_bioseq_cache_case_timeout_seconds 480)
  set(_bioseq_cache_suite_timeout_seconds 28800)

  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/reports")
  add_test(
    NAME bioseq_cache_integration_tests
    COMMAND bioseq_cache_integration_tests
      --postgres-bin-dir
        "$<TARGET_GENEX_EVAL:bioseq_cache_test_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_test_support_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>"
      --postgres-stage-root
        "$<TARGET_GENEX_EVAL:bioseq_cache_test_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_test_support_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>"
      --postgres-stage-manifest
        "$<TARGET_GENEX_EVAL:bioseq_cache_test_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_test_support_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>"
      --cache-control
        "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_CONTROL_FILE>"
      --cache-install-sql
        "$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_TEST_INSTALL_SQL>"
      --cache-dso
        "$<TARGET_GENEX_EVAL:postgresql_bioseq_cache_test_support,$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_DSO>>"
      --report
        "${CMAKE_BINARY_DIR}/reports/bioseq-cache-integration.json"
      --case-timeout-seconds
        "${_bioseq_cache_case_timeout_seconds}"
      "[component:cache][layer:integration]")
  set_tests_properties(bioseq_cache_integration_tests PROPERTIES
    LABELS
      "component:cache;layer:integration;feature:isolation;feature:provider;feature:recovery;feature:reporting;feature:result_cache;feature:security;feature:storage_lifecycle;feature:transaction;resource:filesystem;resource:postgres"
    RESOURCE_LOCK "bioseq_private_postgres"
    TIMEOUT "${_bioseq_cache_suite_timeout_seconds}")
endif()
