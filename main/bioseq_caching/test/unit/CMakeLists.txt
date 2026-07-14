find_package(Catch2 3 REQUIRED)

add_executable(bioseq_cache_unit_tests
  test_checked_state_ttl.cpp
  test_coverage_readiness.cpp
  test_admission_provider.cpp
  test_column_format.cpp)

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

add_test(NAME bioseq_cache_unit_tests COMMAND bioseq_cache_unit_tests)
