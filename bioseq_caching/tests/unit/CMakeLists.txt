find_package(Catch2 3 REQUIRED)

add_executable(bioseq_cache_unit_tests
  "${CMAKE_CURRENT_LIST_DIR}/../support/fake_provider.cpp"
  admission_provider_test.cpp
  checked_math_test.cpp
  checked_state_ttl_test.cpp
  column_format_test.cpp
  coverage_readiness_test.cpp
  dependency_directory_test.cpp
  dependency_vector_test.cpp
  object_definition_dependencies_test.cpp
  provider_api_abi_test.cpp
  reusable_planning_test.cpp
  reusable_registry_test.cpp
  transition_ledger_test.cpp
  write_observation_test.cpp)

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
