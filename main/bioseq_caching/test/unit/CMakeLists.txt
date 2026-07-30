find_package(Catch2 3 REQUIRED)

add_executable(bioseq_cache_unit_tests
  fake_provider_v2.cpp
  test_checked_state_ttl.cpp
  test_coverage_readiness.cpp
  test_admission_provider.cpp
  test_provider_api_abi.cpp
  test_column_format.cpp
  test_checked_math_compatibility.cpp
  test_reusable_planning.cpp
  test_reusable_registry.cpp
  test_write_observation.cpp
  test_object_definition_dependencies.cpp
  test_legacy_migration_ledger.cpp
  test_transition_ledger.cpp
  test_dependency_directory.cpp
  test_dependency_vector.cpp)

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

add_test(NAME bioseq_cache_unit_tests COMMAND bioseq_cache_unit_tests)
