find_package(Threads REQUIRED)

bioseq_collect_authored_sources(
  BIOSEQ_CACHE_BENCHMARK_MODULES
  BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
  RECURSE
  INCLUDE_PATTERNS "*.cppm"
  EXCLUDE_PATHS
    "combined/main.cppm"
    "standalone/main.cppm"
  EXPECTED_COUNT 3)
add_library(bioseq_cache_benchmark_support STATIC)
target_sources(bioseq_cache_benchmark_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES ${BIOSEQ_CACHE_BENCHMARK_MODULES})
set_target_properties(bioseq_cache_benchmark_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_link_libraries(bioseq_cache_benchmark_support
  PUBLIC
    bioseq_postgres_test_support
    bioseq::durable_file_io
    Threads::Threads)

set_source_files_properties(
  "${CMAKE_CURRENT_SOURCE_DIR}/standalone/main.cppm"
  "${CMAKE_CURRENT_SOURCE_DIR}/combined/main.cppm"
  PROPERTIES COMPILE_OPTIONS "-x;c++")
add_executable(
  bioseq_cache_benchmark
  "${CMAKE_CURRENT_SOURCE_DIR}/standalone/main.cppm")
add_executable(
  bioseq_cache_bioseq_benchmark
  "${CMAKE_CURRENT_SOURCE_DIR}/combined/main.cppm")

foreach(_bioseq_cache_benchmark_target IN ITEMS
    bioseq_cache_benchmark
    bioseq_cache_bioseq_benchmark)
  set_target_properties(
    "${_bioseq_cache_benchmark_target}"
    PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS ON
      CXX_MODULE_STD ON
      INTERPROCEDURAL_OPTIMIZATION FALSE
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
  target_link_libraries(
    "${_bioseq_cache_benchmark_target}"
    PRIVATE bioseq_cache_benchmark_support)
  add_dependencies(
    "${_bioseq_cache_benchmark_target}"
    postgresql_bioseq_cache)
endforeach()
add_dependencies(
  bioseq_cache_benchmark
  bioseq_cache_production_postgres_stage)

find_package(Catch2 3 REQUIRED)
bioseq_collect_authored_sources(
  BIOSEQ_CACHE_BENCHMARK_CONTRACT_TEST_SOURCES
  BASE_DIRECTORY
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/contract/benchmark"
  INCLUDE_PATTERNS "*_test.cpp"
  EXPECTED_COUNT 1)
add_executable(
  bioseq_cache_benchmark_report_contract_tests
  ${BIOSEQ_CACHE_BENCHMARK_CONTRACT_TEST_SOURCES})
set_target_properties(
  bioseq_cache_benchmark_report_contract_tests
  PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    INTERPROCEDURAL_OPTIMIZATION FALSE
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bioseq_caching")
target_link_libraries(
  bioseq_cache_benchmark_report_contract_tests
  PRIVATE
    bioseq_cache_benchmark_support
    Catch2::Catch2WithMain)
add_test(
  NAME bioseq_cache_benchmark_report_contract_tests
  COMMAND "$<TARGET_FILE:bioseq_cache_benchmark_report_contract_tests>"
    "[component:cache][layer:contract][feature:reporting]")
set_tests_properties(
  bioseq_cache_benchmark_report_contract_tests
  PROPERTIES LABELS
    "component:cache;layer:contract;feature:reporting")

# The MCP reads this generated path inventory and passes the exact source- and
# build-tree inputs to native private-cluster executables. Forward slashes keep
# the JSON portable across CMake hosts.
set(_bioseq_cache_production_postgres_bin_dir
  "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_production_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>>")
set(_bioseq_cache_production_postgres_stage_root
  "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_production_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>>")
set(_bioseq_cache_production_postgres_stage_manifest
  "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_production_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>>")
set(_bioseq_cache_test_control "")
set(_bioseq_cache_test_install_sql "")
set(_bioseq_cache_test_dso "")
set(_bioseq_cache_test_support_postgres_bin_dir "")
set(_bioseq_cache_test_support_postgres_stage_root "")
set(_bioseq_cache_test_support_postgres_stage_manifest "")
if(TARGET postgresql_bioseq_cache_test_support)
  set(_bioseq_cache_test_control
    "$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_CONTROL_FILE>>")
  set(_bioseq_cache_test_install_sql
    "$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_TEST_INSTALL_SQL>>")
  set(_bioseq_cache_test_dso
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:postgresql_bioseq_cache_test_support,$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_DSO>>>")
  set(_bioseq_cache_test_support_postgres_bin_dir
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_test_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_test_support_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>>")
  set(_bioseq_cache_test_support_postgres_stage_root
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_test_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_test_support_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>>")
  set(_bioseq_cache_test_support_postgres_stage_manifest
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_test_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_test_support_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>>")
endif()

set(_bioseq_control "")
set(_bioseq_install_sql "")
set(_bioseq_dso "")
set(_bioseq_cache_provider_peer_test_dso "")
set(_bioseq_cache_bioseq_only_postgres_bin_dir "")
set(_bioseq_cache_bioseq_only_postgres_stage_root "")
set(_bioseq_cache_bioseq_only_postgres_stage_manifest "")
set(_bioseq_cache_bioseq_support_postgres_bin_dir "")
set(_bioseq_cache_bioseq_support_postgres_stage_root "")
set(_bioseq_cache_bioseq_support_postgres_stage_manifest "")
set(_bioseq_cache_bioseq_production_postgres_bin_dir "")
set(_bioseq_cache_bioseq_production_postgres_stage_root "")
set(_bioseq_cache_bioseq_production_postgres_stage_manifest "")
set(_bioseq_cache_production_dso_qualification_receipt "")
if(BIOSEQ_BUILD_CORE)
  # postgresql_bioseq is declared after bioseq_caching at the top level. These
  # generator expressions resolve after the complete target graph exists.
  set(_bioseq_control
    "$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_CONTROL_FILE>>")
  set(_bioseq_install_sql
    "$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_INSTALL_SQL>>")
  set(_bioseq_dso
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:postgresql_bioseq,$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_DSO>>>")
  set(_bioseq_cache_bioseq_production_postgres_bin_dir
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_production_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>>")
  set(_bioseq_cache_bioseq_production_postgres_stage_root
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_production_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>>")
  set(_bioseq_cache_bioseq_production_postgres_stage_manifest
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_production_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_production_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>>")
  if(BIOSEQ_CACHE_BUILD_TESTS)
    set(_bioseq_cache_production_dso_qualification_receipt
      "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_production_dso_qualification,$<TARGET_PROPERTY:bioseq_cache_bioseq_production_dso_qualification,BIOSEQ_PRODUCTION_DSO_QUALIFICATION_RECEIPT>>>")
  endif()
  if(BIOSEQ_CACHE_BUILD_TESTS AND BIOSEQ_CACHE_TESTING)
    set(_bioseq_cache_provider_peer_test_dso
      "$<SHELL_PATH:$<TARGET_FILE:bioseq_cache_provider_peer_test>>")
    set(_bioseq_cache_bioseq_support_postgres_bin_dir
      "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_support_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>>")
    set(_bioseq_cache_bioseq_support_postgres_stage_root
      "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_support_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>>")
    set(_bioseq_cache_bioseq_support_postgres_stage_manifest
      "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_support_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_support_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>>")
    set(_bioseq_cache_bioseq_only_postgres_bin_dir
      "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_only_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_only_postgres_stage,BIOSEQ_POSTGRES_BIN_DIR>>>")
    set(_bioseq_cache_bioseq_only_postgres_stage_root
      "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_only_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_only_postgres_stage,BIOSEQ_POSTGRES_STAGE_ROOT>>>")
    set(_bioseq_cache_bioseq_only_postgres_stage_manifest
      "$<SHELL_PATH:$<TARGET_GENEX_EVAL:bioseq_cache_bioseq_only_postgres_stage,$<TARGET_PROPERTY:bioseq_cache_bioseq_only_postgres_stage,BIOSEQ_POSTGRES_STAGE_MANIFEST>>>")
  endif()
endif()

set(_bioseq_cache_native_artifacts
"{
  \"cache_test_support_postgres_bin_dir\": \"${_bioseq_cache_test_support_postgres_bin_dir}\",
  \"cache_test_support_postgres_stage_root\": \"${_bioseq_cache_test_support_postgres_stage_root}\",
  \"cache_test_support_postgres_stage_manifest\": \"${_bioseq_cache_test_support_postgres_stage_manifest}\",
  \"cache_production_postgres_bin_dir\": \"${_bioseq_cache_production_postgres_bin_dir}\",
  \"cache_production_postgres_stage_root\": \"${_bioseq_cache_production_postgres_stage_root}\",
  \"cache_production_postgres_stage_manifest\": \"${_bioseq_cache_production_postgres_stage_manifest}\",
  \"cache_bioseq_support_postgres_bin_dir\": \"${_bioseq_cache_bioseq_support_postgres_bin_dir}\",
  \"cache_bioseq_support_postgres_stage_root\": \"${_bioseq_cache_bioseq_support_postgres_stage_root}\",
  \"cache_bioseq_support_postgres_stage_manifest\": \"${_bioseq_cache_bioseq_support_postgres_stage_manifest}\",
  \"cache_bioseq_only_postgres_bin_dir\": \"${_bioseq_cache_bioseq_only_postgres_bin_dir}\",
  \"cache_bioseq_only_postgres_stage_root\": \"${_bioseq_cache_bioseq_only_postgres_stage_root}\",
  \"cache_bioseq_only_postgres_stage_manifest\": \"${_bioseq_cache_bioseq_only_postgres_stage_manifest}\",
  \"cache_bioseq_production_postgres_bin_dir\": \"${_bioseq_cache_bioseq_production_postgres_bin_dir}\",
  \"cache_bioseq_production_postgres_stage_root\": \"${_bioseq_cache_bioseq_production_postgres_stage_root}\",
  \"cache_bioseq_production_postgres_stage_manifest\": \"${_bioseq_cache_bioseq_production_postgres_stage_manifest}\",
  \"production_dso_qualification_receipt\": \"${_bioseq_cache_production_dso_qualification_receipt}\",
  \"cache_control\": \"$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_CONTROL_FILE>>\",
  \"cache_install_sql\": \"$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_INSTALL_SQL>>\",
  \"cache_dso\": \"$<SHELL_PATH:$<TARGET_GENEX_EVAL:postgresql_bioseq_cache,$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_DSO>>>\",
  \"cache_test_control\": \"${_bioseq_cache_test_control}\",
  \"cache_test_install_sql\": \"${_bioseq_cache_test_install_sql}\",
  \"cache_test_dso\": \"${_bioseq_cache_test_dso}\",
  \"bioseq_control\": \"${_bioseq_control}\",
  \"bioseq_install_sql\": \"${_bioseq_install_sql}\",
  \"bioseq_dso\": \"${_bioseq_dso}\",
  \"provider_peer_test_dso\": \"${_bioseq_cache_provider_peer_test_dso}\"
}
")
file(GENERATE
  OUTPUT
    "${CMAKE_BINARY_DIR}/bioseq_caching/cache_native_artifacts.json"
  CONTENT "${_bioseq_cache_native_artifacts}")
