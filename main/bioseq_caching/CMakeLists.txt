option(BIOSEQ_CACHE_BUILD_TESTS
  "Build standalone bioseq_cache unit and private-cluster test targets" OFF)
option(BIOSEQ_CACHE_TESTING
  "Compile deterministic bioseq_cache fault-injection hooks" OFF)

add_subdirectory(core)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(postgresql_bioseq_cache MODULE)
set_target_properties(postgresql_bioseq_cache PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  PREFIX ""
  OUTPUT_NAME "bioseq_cache")
target_sources(postgresql_bioseq_cache
  PRIVATE FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/pg/provider_registry.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/pg/storage.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/pg/maintenance.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/pg/reusable_adapter.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/pg/index.cppm")
target_include_directories(postgresql_bioseq_cache PRIVATE
  ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(postgresql_bioseq_cache PRIVATE
  PostgreSQL::PostgreSQL
  bioseq_cache_core
  bioseq_cache_provider_api
  bioseq_pg_support
  bioseq_memory_budget)
if(BIOSEQ_CACHE_TESTING)
  target_compile_definitions(postgresql_bioseq_cache PRIVATE
    BIOSEQ_CACHE_TESTING=1)
endif()
if(APPLE)
  set_target_properties(postgresql_bioseq_cache PROPERTIES
    LINK_FLAGS "${PostgreSQL_LINK_FLAGS}")
  if(PostgreSQL_VERSION_MAJOR VERSION_GREATER_EQUAL 16)
    set_target_properties(postgresql_bioseq_cache PROPERTIES SUFFIX ".dylib")
  endif()
endif()

set(BIOSEQ_CACHE_MODULE_PATH "MODULE_PATHNAME")
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq_cache_objects.sql.in"
  "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache--1.0.sql"
  @ONLY)

set(EXTENSION_COMMENT
  "Shared SQL request locking and reusable columnar result cache")
set(EXTENSION_VERSION "1.0")
set(EXTENSION_RELOCATABLE "false")
set(EXTENSION_REQUIRES_CONTROL_LINE "")
set(EXTENSION_SCHEMA_CONTROL_LINE "schema = 'bioseq_cache'")
set(EXTENSION_NAME "bioseq_cache")
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/../cmake/control.in"
  "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache.control"
  @ONLY)

install(TARGETS postgresql_bioseq_cache
  LIBRARY DESTINATION "${PostgreSQL_PKG_LIBRARY_DIR}"
  COMPONENT "${PROJECT_NAME}")
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache.control"
  "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache--1.0.sql"
  DESTINATION "${PostgreSQL_SHARE_DIR}/extension"
  COMPONENT "${PROJECT_NAME}")

set(BIOSEQ_CACHE_MODULE_PATH "__BIOSEQ_CACHE_BUILD_DSO__")
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq_cache_objects.sql.in"
  "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_objects.build.sql.in"
  @ONLY)
configure_file(
  "${CMAKE_CURRENT_LIST_DIR}/sql/bioseq_cache_test_objects.sql.in"
  "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_test_objects.build.sql.in"
  @ONLY)
file(READ
  "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_objects.build.sql.in"
  BIOSEQ_CACHE_PUBLIC_OBJECTS)
file(READ
  "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_test_objects.build.sql.in"
  BIOSEQ_CACHE_TEST_OBJECTS)
string(REPLACE "__BIOSEQ_CACHE_BUILD_DSO__"
  "$<TARGET_FILE:postgresql_bioseq_cache>"
  BIOSEQ_CACHE_PUBLIC_OBJECTS "${BIOSEQ_CACHE_PUBLIC_OBJECTS}")
string(REPLACE "__BIOSEQ_CACHE_BUILD_DSO__"
  "$<TARGET_FILE:postgresql_bioseq_cache>"
  BIOSEQ_CACHE_TEST_OBJECTS "${BIOSEQ_CACHE_TEST_OBJECTS}")
file(GENERATE
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_test_bootstrap.sql"
  CONTENT
"CREATE SCHEMA IF NOT EXISTS bioseq_cache;
CREATE SCHEMA IF NOT EXISTS bioseq_cache_test;
${BIOSEQ_CACHE_PUBLIC_OBJECTS}
${BIOSEQ_CACHE_TEST_OBJECTS}")
set_target_properties(postgresql_bioseq_cache PROPERTIES
  BIOSEQ_TEST_BOOTSTRAP_SQL
    "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_test_bootstrap.sql")

if(BIOSEQ_CACHE_BUILD_TESTS)
  add_subdirectory(test/unit)
  add_subdirectory(bench)
endif()

if(BIOSEQ_CACHE_BUILD_TESTS AND BIOSEQ_CACHE_TESTING)
  find_program(BIOSEQ_CACHE_PG_REGRESS pg_regress REQUIRED
    HINTS "${PostgreSQL_PKG_LIBRARY_DIR}/pgxs/src/test/regress"
          "${PostgreSQL_BIN_DIR}")
  find_program(BIOSEQ_CACHE_PG_ISOLATION_REGRESS pg_isolation_regress REQUIRED
    HINTS "${PostgreSQL_PKG_LIBRARY_DIR}/pgxs/src/test/isolation"
          "${PostgreSQL_BIN_DIR}")
  find_program(BIOSEQ_CACHE_PROVE prove REQUIRED)

  add_custom_target(test_bioseq_cache_regress
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/test/run_private_cluster.py"
      --kind regress
      --runner "${BIOSEQ_CACHE_PG_REGRESS}"
      --bindir "${PostgreSQL_BIN_DIR}"
      --dso "$<TARGET_FILE:postgresql_bioseq_cache>"
      --bootstrap "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_test_bootstrap.sql"
      --inputdir "${CMAKE_CURRENT_LIST_DIR}/test/regress"
      --schedule "${CMAKE_CURRENT_LIST_DIR}/test/regress/parallel_schedule"
      --output-root "${CMAKE_CURRENT_BINARY_DIR}/private_regress"
    DEPENDS postgresql_bioseq_cache
    USES_TERMINAL)

  add_custom_target(test_bioseq_cache_isolation
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/test/run_private_cluster.py"
      --kind isolation
      --runner "${BIOSEQ_CACHE_PG_ISOLATION_REGRESS}"
      --bindir "${PostgreSQL_BIN_DIR}"
      --dso "$<TARGET_FILE:postgresql_bioseq_cache>"
      --bootstrap "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_test_bootstrap.sql"
      --inputdir "${CMAKE_CURRENT_LIST_DIR}/test/isolation"
      --schedule "${CMAKE_CURRENT_LIST_DIR}/test/isolation/isolation_schedule"
      --output-root "${CMAKE_CURRENT_BINARY_DIR}/private_isolation"
    DEPENDS postgresql_bioseq_cache
    USES_TERMINAL)

  add_custom_target(test_bioseq_cache_tap
    COMMAND "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/test/run_private_cluster.py"
      --kind tap
      --bindir "${PostgreSQL_BIN_DIR}"
      --dso "$<TARGET_FILE:postgresql_bioseq_cache>"
      --bootstrap "${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_test_bootstrap.sql"
      --prove "${BIOSEQ_CACHE_PROVE}"
      --inputdir "${CMAKE_CURRENT_LIST_DIR}/test/tap"
      --output-root "${CMAKE_CURRENT_BINARY_DIR}/private_tap"
    DEPENDS postgresql_bioseq_cache
    USES_TERMINAL)
endif()

if(BIOSEQ_CACHE_BUILD_TESTS)
  add_custom_target(run_bioseq_cache_bench
    COMMAND "${CMAKE_COMMAND}" -E env
      "BIOSEQ_CACHE_DSO=$<TARGET_FILE:postgresql_bioseq_cache>"
      "BIOSEQ_CACHE_BOOTSTRAP_SQL=${CMAKE_CURRENT_BINARY_DIR}/bioseq_cache_test_bootstrap.sql"
      "BIOSEQ_CACHE_CORE_BENCH=$<TARGET_FILE:bioseq_cache_bench>"
      "BIOSEQ_CACHE_REPORT_DIR=${CMAKE_SOURCE_DIR}/reporting/bioseq_cache_bench"
      "POSTGRESQL_BINDIR=${PostgreSQL_BIN_DIR}"
      "CMAKE_PRESET_NAME=${CMAKE_PRESET_NAME}"
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_LIST_DIR}/bench/run_private_cluster_bench.py"
    DEPENDS postgresql_bioseq_cache bioseq_cache_bench
    USES_TERMINAL)
endif()
