if(NOT TARGET PostgreSQL::PostgreSQL)
  message(FATAL_ERROR
    "bioseq_postgres_test_support requires PostgreSQL::PostgreSQL")
endif()
find_package(Threads REQUIRED)

add_executable(bioseq_postgres_test_launch_broker
  "${CMAKE_CURRENT_LIST_DIR}/launch_broker.cpp")
set_target_properties(bioseq_postgres_test_launch_broker PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)

# The shared libpq client is a standalone module target for native PostgreSQL
# tests. Codegen may compile the same source into its isolated executable, so
# guard the target name for combined trees without changing that source owner.
if(NOT TARGET bioseq_libpq_client)
  add_library(bioseq_libpq_client STATIC)
  set_target_properties(bioseq_libpq_client PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    POSITION_INDEPENDENT_CODE ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_sources(bioseq_libpq_client
    PUBLIC FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}/../libpq"
      FILES "${CMAKE_CURRENT_LIST_DIR}/../libpq/index.cppm")
  target_link_libraries(bioseq_libpq_client
    PUBLIC PostgreSQL::PostgreSQL)
endif()

add_library(bioseq_postgres_test_support STATIC)
set_target_properties(bioseq_postgres_test_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_postgres_test_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/row_hash.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/supervision.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/process.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/artifact.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/resource_sampler.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/qualification.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/cluster.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/fixture.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/explain.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/migration_probe.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/report.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/index.cppm")
target_link_libraries(bioseq_postgres_test_support
  PUBLIC
    bioseq_libpq_client
    Threads::Threads)
target_compile_definitions(bioseq_postgres_test_support
  PRIVATE
    "BIOSEQ_POSTGRES_TEST_LAUNCH_BROKER_PATH=\"$<TARGET_FILE:bioseq_postgres_test_launch_broker>\"")
add_dependencies(
  bioseq_postgres_test_support
  bioseq_postgres_test_launch_broker)

if(BUILD_TESTING)
  find_package(Catch2 3 REQUIRED)
  add_executable(bioseq_postgres_test_support_contract_tests
    "${CMAKE_CURRENT_LIST_DIR}/contract_tests.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/qualification_contract_tests.cpp")
  set_target_properties(bioseq_postgres_test_support_contract_tests PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_link_libraries(bioseq_postgres_test_support_contract_tests
    PRIVATE
      bioseq_postgres_test_support
      Catch2::Catch2WithMain)
  add_test(
    NAME bioseq_postgres_test_support_contract_tests
    COMMAND bioseq_postgres_test_support_contract_tests)
endif()
