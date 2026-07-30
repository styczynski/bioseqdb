find_package(Threads REQUIRED)

add_library(bioseq_cache_benchmark_support STATIC)
target_sources(bioseq_cache_benchmark_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES "${CMAKE_CURRENT_SOURCE_DIR}/cache_benchmark.cppm")
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
    Threads::Threads)

add_executable(
  bioseq_cache_bench
  "${CMAKE_CURRENT_SOURCE_DIR}/cache_bench_main.cpp")
add_executable(
  bioseq_cache_bioseq_bench
  "${CMAKE_CURRENT_SOURCE_DIR}/cache_bioseq_bench_main.cpp")

foreach(_bioseq_cache_benchmark_target IN ITEMS
    bioseq_cache_bench
    bioseq_cache_bioseq_bench)
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

find_package(Catch2 3 REQUIRED)
add_executable(
  bioseq_cache_benchmark_report_contract_tests
  "${CMAKE_CURRENT_SOURCE_DIR}/cache_benchmark_report_tests.cpp")
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
  COMMAND bioseq_cache_benchmark_report_contract_tests)

# The MCP reads this generated path inventory and passes the exact source- and
# build-tree inputs to native private-cluster executables. Forward slashes keep
# the JSON portable across CMake hosts.
set(_bioseq_cache_test_control "")
set(_bioseq_cache_test_install_sql "")
set(_bioseq_cache_test_dso "")
if(TARGET postgresql_bioseq_cache_test_support)
  set(_bioseq_cache_test_control
    "$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_CONTROL_FILE>>")
  set(_bioseq_cache_test_install_sql
    "$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_TEST_INSTALL_SQL>>")
  set(_bioseq_cache_test_dso
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:postgresql_bioseq_cache_test_support,$<TARGET_PROPERTY:postgresql_bioseq_cache_test_support,BIOSEQ_EXTENSION_DSO>>>")
endif()

set(_bioseq_control "")
set(_bioseq_install_sql "")
set(_bioseq_dso "")
set(_bioseq_cache_provider_peer_test_dso "")
if(BIOSEQ_BUILD_CORE)
  # postgresql_bioseq is declared after bioseq_caching at the top level. These
  # generator expressions resolve after the complete target graph exists.
  set(_bioseq_control
    "$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_CONTROL_FILE>>")
  set(_bioseq_install_sql
    "$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_INSTALL_SQL>>")
  set(_bioseq_dso
    "$<SHELL_PATH:$<TARGET_GENEX_EVAL:postgresql_bioseq,$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_DSO>>>")
  if(BIOSEQ_CACHE_BUILD_TESTS AND BIOSEQ_CACHE_TESTING)
    set(_bioseq_cache_provider_peer_test_dso
      "$<SHELL_PATH:$<TARGET_FILE:bioseq_cache_provider_peer_test>>")
  endif()
endif()

set(_bioseq_cache_native_artifacts
"{
  \"postgres_bin_dir\": \"$<SHELL_PATH:${PostgreSQL_BIN_DIR}>\",
  \"cache_control\": \"$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_CONTROL_FILE>>\",
  \"cache_install_sql\": \"$<SHELL_PATH:$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_INSTALL_SQL>>\",
  \"cache_dso\": \"$<SHELL_PATH:$<TARGET_GENEX_EVAL:postgresql_bioseq_cache,$<TARGET_PROPERTY:postgresql_bioseq_cache,BIOSEQ_EXTENSION_DSO>>>\",
  \"cache_test_control\": \"${_bioseq_cache_test_control}\",
  \"cache_test_install_sql\": \"${_bioseq_cache_test_install_sql}\",
  \"cache_test_dso\": \"${_bioseq_cache_test_dso}\",
  \"bioseq_control\": \"${_bioseq_control}\",
  \"bioseq_install_sql\": \"${_bioseq_install_sql}\",
  \"bioseq_dso\": \"${_bioseq_dso}\",
  \"provider_peer_test_dso\": \"${_bioseq_cache_provider_peer_test_dso}\",
  \"migration_ledger\": \"$<SHELL_PATH:${CMAKE_CURRENT_LIST_DIR}/../test/legacy_migration_ledger.tsv>\"
}
")
file(GENERATE
  OUTPUT
    "${CMAKE_BINARY_DIR}/bioseq_caching/cache_native_artifacts.json"
  CONTENT "${_bioseq_cache_native_artifacts}")
