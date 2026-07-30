add_library(bioseq_checked_arithmetic_core STATIC)
set_target_properties(bioseq_checked_arithmetic_core PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
target_sources(bioseq_checked_arithmetic_core
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${CMAKE_CURRENT_LIST_DIR}/index.cppm")
add_library(
  bioseq::checked_arithmetic
  ALIAS bioseq_checked_arithmetic_core)

if(BUILD_TESTING)
  find_package(Catch2 3 REQUIRED)
  add_executable(
    bioseq_checked_arithmetic_contract_tests
    "${CMAKE_CURRENT_LIST_DIR}/contract_tests.cpp")
  set_target_properties(
    bioseq_checked_arithmetic_contract_tests
    PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS ON
      CXX_MODULE_STD ON
      INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_link_libraries(
    bioseq_checked_arithmetic_contract_tests
    PRIVATE
      bioseq_checked_arithmetic_core
      Catch2::Catch2WithMain)
  add_test(
    NAME bioseq_checked_arithmetic_contract_tests
    COMMAND bioseq_checked_arithmetic_contract_tests)
  set_tests_properties(
    bioseq_checked_arithmetic_contract_tests
    PROPERTIES LABELS "common;unit;checked-arithmetic")
endif()
