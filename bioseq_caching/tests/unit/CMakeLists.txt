find_package(Catch2 3 REQUIRED)

bioseq_collect_authored_sources(
  BIOSEQ_CACHE_UNIT_TEST_SOURCES
  BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
  INCLUDE_PATTERNS "*_test.cpp"
  EXPECTED_COUNT 13)
add_executable(bioseq_cache_unit_tests
  "${CMAKE_CURRENT_LIST_DIR}/../support/fake_provider.cpp"
  ${BIOSEQ_CACHE_UNIT_TEST_SOURCES})

set_target_properties(bioseq_cache_unit_tests PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching"
  INTERPROCEDURAL_OPTIMIZATION FALSE)

target_link_libraries(bioseq_cache_unit_tests
  PRIVATE
    bioseq_cache_core
    Catch2::Catch2WithMain)
target_include_directories(
  bioseq_cache_unit_tests
  PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../support")

add_test(
  NAME bioseq_cache_unit_tests
  COMMAND "$<TARGET_FILE:bioseq_cache_unit_tests>"
    "[component:cache][layer:unit]")
set_tests_properties(bioseq_cache_unit_tests PROPERTIES
  LABELS
    "component:cache;layer:unit;feature:admission;feature:column_format;feature:dependency_tracking;feature:provider;feature:registry;feature:result_cache;feature:transaction;feature:write_observation")
