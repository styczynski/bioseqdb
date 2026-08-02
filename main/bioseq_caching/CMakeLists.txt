option(BIOSEQ_CACHE_BUILD_TESTS
  "Build bioseq_cache Catch2 and private-cluster tests" OFF)
option(BIOSEQ_CACHE_TESTING
  "Build the noninstalled bioseq_cache test-support DSO" OFF)

function(_bioseq_cache_verify_sql_row_profile_guard)
  set(_rows_source "${CMAKE_CURRENT_LIST_DIR}/pg/sql/rows/index.cppm")
  file(READ "${_rows_source}" _rows_text)
  string(FIND "${_rows_text}"
    "#if defined(BIOSEQ_CACHE_TESTING)" _guard_begin)
  string(FIND "${_rows_text}"
    "#endif  // defined(BIOSEQ_CACHE_TESTING)" _guard_end)
  if(_guard_begin LESS 0 OR _guard_end LESS 0
      OR _guard_end LESS_EQUAL _guard_begin)
    message(FATAL_ERROR
      "bioseq_cache SQL row profile contract requires one ordered "
      "BIOSEQ_CACHE_TESTING guard")
  endif()
  foreach(_row IN ITEMS
      storage_tier_stats_row
      entry_state_row
      transition_state_row
      relation_transition_state_row
      planner_reference_counts_row
      provider_callback_stats_row
      plan_build_stats_row
      injected_coverage_row
      coverage_bucket_state_row)
    string(FIND "${_rows_text}" "struct ${_row} final" _row_begin)
    string(FIND "${_rows_text}"
      "static_assert(std::is_aggregate_v<${_row}>);" _row_contract)
    if(_row_begin LESS _guard_begin OR _row_begin GREATER _guard_end
        OR _row_contract LESS _guard_begin
        OR _row_contract GREATER _guard_end)
      message(FATAL_ERROR
        "bioseq_cache SQL row profile contract exposes test-only ${_row} "
        "outside BIOSEQ_CACHE_TESTING")
    endif()
  endforeach()
  foreach(_row IN ITEMS stats_row cache_entry_row)
    string(FIND "${_rows_text}" "struct ${_row} final" _row_begin)
    string(FIND "${_rows_text}"
      "static_assert(std::is_aggregate_v<${_row}>);" _row_contract)
    if(_row_begin LESS 0 OR _row_contract LESS 0
        OR _row_begin GREATER_EQUAL _guard_begin
        OR _row_contract GREATER_EQUAL _guard_begin)
      message(FATAL_ERROR
        "bioseq_cache SQL row profile contract hides production ${_row}")
    endif()
  endforeach()
endfunction()

_bioseq_cache_verify_sql_row_profile_guard()

add_subdirectory(core)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/BioseqSqlCodegen.cmake")

set(BIOSEQ_CACHE_PRODUCTION_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/pg/provider/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/provider/registry.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/provider/reusable_adapter.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/shared_registry.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/dependency_runtime.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/object_dependency_hooks.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/guc.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/executor/context/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/executor/lifecycle.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/executor/exact_scan.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/executor/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/planner/exact.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/planner/reuse.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/planner/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/storage.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/maintenance.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/runtime.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/rows/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/diagnostics.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/admin.cppm")
set(BIOSEQ_CACHE_PRODUCTION_IMPLEMENTATION_SOURCES
  "${CMAKE_CURRENT_LIST_DIR}/pg/provider/registry_implementation.cpp")
set(BIOSEQ_CACHE_TEST_SUPPORT_MODULES
  ${BIOSEQ_CACHE_PRODUCTION_MODULES}
  "${CMAKE_CURRENT_LIST_DIR}/pg/test_provider.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/test_controls.cppm")

# SQL ownership is explicit and profile-complete. Production remains the
# independently installable sibling extension; test_support is its strict
# noninstalled superset.
bioseq_sql_begin_owner_registry(
  EXTENSION bioseq_cache
  ROOT "${CMAKE_CURRENT_LIST_DIR}"
  PROFILES production test_support
  AUDIT_ROOTS pg)
bioseq_register_sql_owner(
  EXTENSION bioseq_cache
  OWNER_KEY cache_sql_rows
  MODULE bioseq.cache.pg.sql.rows
  SOURCES pg/sql/rows/index.cppm
  PROFILES production test_support
  AUTHORITIES SUPPORT)
bioseq_register_sql_owner(
  EXTENSION bioseq_cache
  OWNER_KEY cache_sql_catalog
  MODULE bioseq.cache.pg.sql
  SOURCES pg/sql/index.cppm
  PROFILES production test_support
  AUTHORITIES FINAL)
bioseq_register_sql_owner(
  EXTENSION bioseq_cache
  OWNER_KEY cache_diagnostics
  MODULE bioseq.cache.pg.sql:diagnostics
  SOURCES pg/sql/diagnostics.cppm
  PROFILES production test_support
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS cache_diagnostics)
bioseq_register_sql_owner(
  EXTENSION bioseq_cache
  OWNER_KEY cache_admin
  MODULE bioseq.cache.pg.sql:admin
  SOURCES pg/sql/admin.cppm
  PROFILES production test_support
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS cache_admin)
bioseq_register_sql_owner(
  EXTENSION bioseq_cache
  OWNER_KEY cache_test_controls
  MODULE bioseq.cache.pg.sql:test_controls
  SOURCES pg/sql/test_controls.cppm
  PROFILES test_support
  AUTHORITIES FINAL
  FMGR_ENTRYPOINTS cache_test_controls)
bioseq_finalize_sql_owner_registry(
  EXTENSION bioseq_cache
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/registries"
  INSTALL_PROFILE production
  TEST_PROFILE test_support
  EXPECTED_FMGR_ENTRYPOINTS
    cache_admin
    cache_diagnostics
    cache_test_controls)
bioseq_add_sql_codegen_context(
  EXTENSION bioseq_cache
  EXTENSION_NAME bioseq_cache
  EXTENSION_SCHEMA bioseq_cache
  GENERATED_MODULE_PREFIX bioseq.cache.sql.generated
  FMGR_BOUNDARY_MODULE bioseq.pg_sql_boundary
  FMGR_PRELUDE postgresql
  OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated/sql"
  GENERATED_CPP_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/bioseq_cache/sql"
  VERSION 1.0
  PROFILES production test_support
  REQUIRE_COMPLETE_CODEGEN)
# The legacy Bioseq registration call is still implicit; restore its context
# before main/bioseq is configured later by the top-level project.
set_property(GLOBAL PROPERTY BIOSEQ_SQL_ACTIVE_CONTEXT bioseq)

set(BIOSEQ_CACHE_PRODUCTION_GENERATED_MODULES
  ${BIOSEQ_SQL_BIOSEQ_CACHE_PRODUCTION_GENERATED_MODULES})
set(BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MODULES
  ${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MODULES})
set_source_files_properties(
  ${BIOSEQ_CACHE_PRODUCTION_GENERATED_MODULES}
  ${BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MODULES}
  PROPERTIES GENERATED TRUE)

AddCommonTargetX(
  postgresql_bioseq_cache
  TYPE POSTGRES_EXTENSION
  CXX 26
  NAME bioseq_cache
  SCHEMA bioseq_cache
  COMMENT "Shared SQL request locking and reusable columnar result cache"
  VERSION 1.0
  COMPONENT "${PROJECT_NAME}"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
  CXX_MOD_FILES
    ${BIOSEQ_CACHE_PRODUCTION_MODULES}
    ${BIOSEQ_CACHE_PRODUCTION_GENERATED_MODULES}
  SOURCES
    ${BIOSEQ_CACHE_PRODUCTION_IMPLEMENTATION_SOURCES}
  SHARED_MODULE_LIBS
    bioseq_cache_core
    bioseq_cache_provider_api
    bioseq_durable_file_io
    bioseq_pg_support
    bioseq_pg_sql_boundary
    bioseq_sql_annotation_model
  LINKS PostgreSQL::ServerHeaders bioseq_memory_budget
  GENERATED_INSTALL
    "${BIOSEQ_SQL_BIOSEQ_CACHE_PRODUCTION_GENERATED_INSTALL}"
  SQL_MANIFEST
    "${BIOSEQ_SQL_BIOSEQ_CACHE_PRODUCTION_GENERATED_MANIFEST}"
  DSO_CLOSURE_CONTRACT manifest
  DSO_ABI_EXPORTS
    Pg_magic_func
    _PG_init
    bioseq_cache_storage_janitor_main
  REQUIRE_COMPLETE_CODEGEN
  PG_ENTRYPOINT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/pg/index.cppm")
set_target_properties(postgresql_bioseq_cache PROPERTIES
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/artifacts/cache_production/$<CONFIG>"
  RUNTIME_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/artifacts/cache_production/$<CONFIG>")
target_compile_options(postgresql_bioseq_cache PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
add_dependencies(
  postgresql_bioseq_cache
  "${BIOSEQ_SQL_BIOSEQ_CACHE_PRODUCTION_TARGET}")
if(BIOSEQ_CACHE_TESTING)
  add_library(postgresql_bioseq_cache_test_support MODULE)
  target_sources(postgresql_bioseq_cache_test_support PRIVATE
    ${BIOSEQ_CACHE_PRODUCTION_IMPLEMENTATION_SOURCES})
  target_sources(postgresql_bioseq_cache_test_support
    PRIVATE FILE_SET CXX_MODULES
      BASE_DIRS
        "${CMAKE_CURRENT_LIST_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}/generated"
      FILES
        ${BIOSEQ_CACHE_TEST_SUPPORT_MODULES}
        ${BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MODULES})
  target_link_libraries(postgresql_bioseq_cache_test_support PRIVATE
    PostgreSQL::ServerHeaders
    bioseq_cache_core
    bioseq_cache_provider_api
    bioseq_durable_file_io
    bioseq_pg_support
    bioseq_pg_sql_boundary
    bioseq_sql_annotation_model
    bioseq_memory_budget)
  if(WIN32)
    target_link_libraries(
      postgresql_bioseq_cache_test_support PRIVATE
      PostgreSQL::PostgreSQL)
  endif()
  target_include_directories(postgresql_bioseq_cache_test_support PRIVATE
    ${PostgreSQL_INCLUDE_DIRS})
  target_compile_definitions(postgresql_bioseq_cache_test_support PRIVATE
    BIOSEQ_CACHE_TESTING=1
    POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})
  target_compile_options(postgresql_bioseq_cache_test_support PRIVATE
    "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
    "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
  set_target_properties(postgresql_bioseq_cache_test_support PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    POSITION_INDEPENDENT_CODE ON
    INTERPROCEDURAL_OPTIMIZATION FALSE
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    PREFIX ""
    OUTPUT_NAME "bioseq_cache"
    LIBRARY_OUTPUT_DIRECTORY
      "${CMAKE_CURRENT_BINARY_DIR}/artifacts/cache_test_support/$<CONFIG>"
    RUNTIME_OUTPUT_DIRECTORY
      "${CMAKE_CURRENT_BINARY_DIR}/artifacts/cache_test_support/$<CONFIG>"
    BIOSEQ_EXTENSION_CONTROL_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache.control"
    BIOSEQ_EXTENSION_DSO
      "$<TARGET_FILE:postgresql_bioseq_cache_test_support>"
    BIOSEQ_EXTENSION_TEST_INSTALL_SQL
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_INSTALL}"
    BIOSEQ_EXTENSION_INSTALL_SQL
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_INSTALL}"
    BIOSEQ_EXTENSION_TEST_SQL_MANIFEST
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MANIFEST}"
    BIOSEQ_EXTENSION_SQL_MANIFEST
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MANIFEST}")
  add_dependencies(
    postgresql_bioseq_cache_test_support
    "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_TARGET}")
  bioseq_attach_extension_dso_closure(
    postgresql_bioseq_cache_test_support
    EXTENSION_NAME "bioseq_cache/test_support"
    SQL_MANIFEST
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MANIFEST}"
    REPORT_PATH
      "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/bioseq_cache-test-support-dso-closure.json"
    CONTRACT manifest
    ABI_EXPORTS
      Pg_magic_func
      _PG_init
      bioseq_cache_storage_janitor_main
    REQUIRE_COMPLETE_CODEGEN)
  if(APPLE)
    set_target_properties(postgresql_bioseq_cache_test_support PROPERTIES
      LINK_FLAGS "${PostgreSQL_LINK_FLAGS}")
    if(PostgreSQL_VERSION_MAJOR VERSION_GREATER_EQUAL 16)
      set_target_properties(
        postgresql_bioseq_cache_test_support PROPERTIES SUFFIX ".dylib")
    endif()
  endif()
endif()

if(BIOSEQ_CACHE_BUILD_TESTS)
  include(
    "${CMAKE_SOURCE_DIR}/cmake/BioseqPrivatePostgresStage.cmake")
  bioseq_add_private_postgres_stage(
    TARGET bioseq_cache_production_postgres_stage
    PG_CONFIG "${PostgreSQL_CONFIG}"
    EXTENSION_NAMES
      bioseq_cache
    EXTENSION_TARGETS
      postgresql_bioseq_cache
    CONTROL_PROPERTIES
      BIOSEQ_EXTENSION_CONTROL_FILE
    INSTALL_SQL_PROPERTIES
      BIOSEQ_EXTENSION_INSTALL_SQL)
  if(TARGET postgresql_bioseq_cache_test_support)
    bioseq_add_private_postgres_stage(
      TARGET bioseq_cache_test_support_postgres_stage
      PG_CONFIG "${PostgreSQL_CONFIG}"
      EXTENSION_NAMES
        bioseq_cache
      EXTENSION_TARGETS
        postgresql_bioseq_cache_test_support
      CONTROL_PROPERTIES
        BIOSEQ_EXTENSION_CONTROL_FILE
      INSTALL_SQL_PROPERTIES
        BIOSEQ_EXTENSION_TEST_INSTALL_SQL)
  endif()

  if(NOT TARGET bioseq_postgres_test_support)
    add_subdirectory(
      "${CMAKE_CURRENT_LIST_DIR}/../common/postgres_test_support"
      "${CMAKE_CURRENT_BINARY_DIR}/postgres_test_support")
  endif()
  add_subdirectory(test/unit)
  add_subdirectory(bench)
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/test/integration/CMakeLists.txt")
    add_subdirectory(test/integration)
  endif()

endif()
