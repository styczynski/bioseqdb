option(BIOSEQ_CACHE_BUILD_TESTS
  "Build bioseq_cache Catch2 and private-cluster tests" OFF)
option(BIOSEQ_CACHE_TESTING
  "Build the noninstalled bioseq_cache test-support DSO" OFF)

add_subdirectory(core)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/BioseqSqlCodegen.cmake")

# Cache-only builds still require the shared SQL generator. The top-level
# project already creates it when the Bioseq extension is enabled.
if(NOT TARGET bioseq_codegen)
  bioseq_add_codegen(
    TARGET bioseq_codegen
    PROJECT_TARGET bioseq_codegen_project
    BINARY_DIR "${CMAKE_BINARY_DIR}/codegen"
    COMPONENTS sql)
endif()

# The cache is configured before main/bioseq, so it owns a small, cache-local
# BMI provider for the authored SQL annotation model.
set(BIOSEQ_CACHE_SQL_ANNOTATION_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/annotation_model/entities.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/annotation_model/migration.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/annotation_model/options.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/annotation_model/validation.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/../codegen/sql/annotation_model/index.cppm")
add_library(bioseq_cache_sql_annotation_model STATIC)
target_sources(bioseq_cache_sql_annotation_model
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../codegen"
    FILES ${BIOSEQ_CACHE_SQL_ANNOTATION_MODULES})
set_target_properties(bioseq_cache_sql_annotation_model PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)

set(BIOSEQ_CACHE_PRODUCTION_MODULES
  "${CMAKE_CURRENT_LIST_DIR}/pg/provider_registry.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/shared_registry.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/dependency_runtime.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/object_dependency_hooks.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/storage.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/maintenance.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/reusable_adapter.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/reusable_planning.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/rows.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/index.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/diagnostics.cppm"
  "${CMAKE_CURRENT_LIST_DIR}/pg/sql/admin.cppm")
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
  SOURCES pg/sql/rows.cppm
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
  SHARED_MODULE_LIBS
    bioseq_cache_core
    bioseq_cache_provider_api
    bioseq_pg_support
    bioseq_pg_sql_boundary
    bioseq_cache_sql_annotation_model
  LINKS PostgreSQL::ServerHeaders bioseq_memory_budget
  GENERATED_INSTALL
    "${BIOSEQ_SQL_BIOSEQ_CACHE_PRODUCTION_GENERATED_INSTALL}"
  SQL_MANIFEST
    "${BIOSEQ_SQL_BIOSEQ_CACHE_PRODUCTION_GENERATED_MANIFEST}"
  REQUIRE_COMPLETE_CODEGEN
  PG_ENTRYPOINT_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/pg/index.cppm")
set_target_properties(postgresql_bioseq_cache PROPERTIES
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  LIBRARY_OUTPUT_DIRECTORY
    "${CMAKE_CURRENT_BINARY_DIR}/artifacts/production")
target_compile_options(postgresql_bioseq_cache PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
add_dependencies(
  postgresql_bioseq_cache
  "${BIOSEQ_SQL_BIOSEQ_CACHE_PRODUCTION_TARGET}")
set_target_properties(postgresql_bioseq_cache PROPERTIES
  BIOSEQ_EXTENSION_DSO
    "$<TARGET_FILE:postgresql_bioseq_cache>"
  BIOSEQ_TEST_BOOTSTRAP_SQL
    "${BIOSEQ_SQL_BIOSEQ_CACHE_PRODUCTION_GENERATED_INSTALL}")

if(BIOSEQ_CACHE_TESTING)
  add_library(postgresql_bioseq_cache_test_support MODULE)
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
    bioseq_pg_support
    bioseq_pg_sql_boundary
    bioseq_cache_sql_annotation_model
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
    PREFIX ""
    OUTPUT_NAME "bioseq_cache"
    LIBRARY_OUTPUT_DIRECTORY
      "${CMAKE_CURRENT_BINARY_DIR}/artifacts/test_support"
    BIOSEQ_EXTENSION_CONTROL_FILE
      "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache.control"
    BIOSEQ_EXTENSION_DSO
      "$<TARGET_FILE:postgresql_bioseq_cache_test_support>"
    BIOSEQ_EXTENSION_TEST_INSTALL_SQL
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_INSTALL}"
    BIOSEQ_TEST_BOOTSTRAP_SQL
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_INSTALL}"
    BIOSEQ_EXTENSION_INSTALL_SQL
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_INSTALL}"
    BIOSEQ_EXTENSION_SQL_MANIFEST
      "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_GENERATED_MANIFEST}")
  add_dependencies(
    postgresql_bioseq_cache_test_support
    "${BIOSEQ_SQL_BIOSEQ_CACHE_TEST_SUPPORT_TARGET}")
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
  if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/test/bioseq/CMakeLists.txt")
    add_subdirectory(test/bioseq)
  endif()
endif()
