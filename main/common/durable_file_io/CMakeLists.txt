add_library(bioseq_durable_file_io STATIC)
set_target_properties(bioseq_durable_file_io PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_sources(bioseq_durable_file_io
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/types.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/detail.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/positional_io.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/stable_reads.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/directory_operations.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/publication.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/index.cppm")
add_library(
  bioseq::durable_file_io
  ALIAS bioseq_durable_file_io)

if(BUILD_TESTING)
  find_package(Catch2 3 REQUIRED)
  find_package(Threads REQUIRED)
  add_executable(
    bioseq_durable_file_io_contract_tests
    "${CMAKE_CURRENT_LIST_DIR}/contract_tests.cpp")
  set_target_properties(
    bioseq_durable_file_io_contract_tests
    PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS ON
      CXX_MODULE_STD ON
      INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_link_libraries(
    bioseq_durable_file_io_contract_tests
    PRIVATE
      bioseq_durable_file_io
      Catch2::Catch2WithMain
      Threads::Threads)
  add_test(
    NAME bioseq_durable_file_io_contract_tests
    COMMAND bioseq_durable_file_io_contract_tests)
  set_tests_properties(
    bioseq_durable_file_io_contract_tests
    PROPERTIES LABELS "common;unit;durable-file-io")
endif()
