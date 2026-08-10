find_package(Catch2 3 REQUIRED)

if(NOT TARGET bioseq_cache_test_context)
  add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../../support"
    "${CMAKE_CURRENT_BINARY_DIR}/support")
endif()

add_library(bioseq_cache_provider_peer_test MODULE
  "${CMAKE_CURRENT_LIST_DIR}/../../support/provider_peer.cpp")
set_target_properties(bioseq_cache_provider_peer_test PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  PREFIX ""
  OUTPUT_NAME "bioseq_cache_provider_peer_test"
  LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/provider_peer_test")
target_link_libraries(bioseq_cache_provider_peer_test PRIVATE
  PostgreSQL::ServerHeaders
  bioseq_cache_provider_api)
target_include_directories(bioseq_cache_provider_peer_test PRIVATE
  ${PostgreSQL_INCLUDE_DIRS})
if(WIN32)
  target_link_libraries(
    bioseq_cache_provider_peer_test PRIVATE
    PostgreSQL::PostgreSQL)
endif()
if(APPLE)
  set_target_properties(bioseq_cache_provider_peer_test PROPERTIES
    LINK_FLAGS "${PostgreSQL_LINK_FLAGS}")
  if(PostgreSQL_VERSION_MAJOR VERSION_GREATER_EQUAL 16)
    set_target_properties(
      bioseq_cache_provider_peer_test PROPERTIES SUFFIX ".dylib")
  endif()
endif()

add_executable(bioseq_cache_bioseq_support_integration_tests
  "${CMAKE_CURRENT_LIST_DIR}/../../support/cache_catch_runner.cpp"
  coexistence_acceptance_test.cpp
  coexistence_test.cpp
  exact_provider_test.cpp
  provider_peer_failopen_test.cpp)
set_target_properties(
  bioseq_cache_bioseq_support_integration_tests PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
target_link_libraries(bioseq_cache_bioseq_support_integration_tests
  PRIVATE
    bioseq_cache_test_context
    Catch2::Catch2)
add_dependencies(
  bioseq_cache_bioseq_support_integration_tests
  bioseq_cache_provider_peer_test
  postgresql_bioseq_cache_test_support
  postgresql_bioseq
  bioseq_cache_bioseq_support_postgres_stage
  bioseq_cache_bioseq_only_postgres_stage)

# Ordinary combined tests use the noninstalled diagnostics profile and retain
# the cache suite's eight-minute per-case watchdog.
set(_bioseq_cache_bioseq_case_timeout_seconds 480)
set(_bioseq_cache_bioseq_support_suite_timeout_seconds 900)

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/reports")
add_test(
  NAME bioseq_cache_bioseq_support_integration_tests
  COMMAND bioseq_cache_bioseq_support_integration_tests
    --postgres-bin-dir
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_support_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>"
    --postgres-stage-root
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_support_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>"
    --postgres-stage-manifest
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_support_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>"
    --bioseq-only-postgres-bin-dir
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_only_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_only_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>"
    --bioseq-only-postgres-stage-root
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_only_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_only_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>"
    --bioseq-only-postgres-stage-manifest
      "$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_only_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_only_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>"
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
    --provider-peer-test-dso
      "$<TARGET_FILE:bioseq_cache_provider_peer_test>"
    --report
      "${CMAKE_BINARY_DIR}/reports/bioseq-cache-bioseq-support-integration.json"
    --case-timeout-seconds
      "${_bioseq_cache_bioseq_case_timeout_seconds}"
    "[component:cache][layer:integration][feature:provider]")
set_tests_properties(
  bioseq_cache_bioseq_support_integration_tests PROPERTIES
  LABELS
    "component:cache;layer:integration;feature:provider;resource:postgres"
  RESOURCE_LOCK "bioseq_private_postgres"
  TIMEOUT "${_bioseq_cache_bioseq_support_suite_timeout_seconds}")
