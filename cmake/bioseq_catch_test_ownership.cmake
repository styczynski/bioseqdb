include_guard(GLOBAL)

function(_bioseq_collect_buildsystem_targets p_directory p_output)
  get_property(
    _bioseq_directory_targets
    DIRECTORY "${p_directory}"
    PROPERTY BUILDSYSTEM_TARGETS)
  get_property(
    _bioseq_subdirectories
    DIRECTORY "${p_directory}"
    PROPERTY SUBDIRECTORIES)
  foreach(_bioseq_subdirectory IN LISTS _bioseq_subdirectories)
    _bioseq_collect_buildsystem_targets(
      "${_bioseq_subdirectory}" _bioseq_child_targets)
    list(APPEND _bioseq_directory_targets ${_bioseq_child_targets})
  endforeach()
  set(${p_output} "${_bioseq_directory_targets}" PARENT_SCOPE)
endfunction()

function(
    _bioseq_normalize_target_source
    p_target
    p_source
    p_output)
  if("${p_source}" MATCHES "^\\$<")
    set(${p_output} "" PARENT_SCOPE)
    return()
  endif()
  if(IS_ABSOLUTE "${p_source}")
    set(_bioseq_source_path "${p_source}")
  else()
    get_target_property(_bioseq_source_directory "${p_target}" SOURCE_DIR)
    if(NOT _bioseq_source_directory
       OR _bioseq_source_directory MATCHES "-NOTFOUND$")
      message(FATAL_ERROR
        "Cannot resolve source directory for test owner ${p_target}")
    endif()
    set(_bioseq_source_path "${_bioseq_source_directory}/${p_source}")
  endif()
  cmake_path(
    NORMAL_PATH _bioseq_source_path
    OUTPUT_VARIABLE _bioseq_normalized_source)
  set(${p_output} "${_bioseq_normalized_source}" PARENT_SCOPE)
endfunction()

function(_bioseq_expected_test_owner p_source p_output)
  file(RELATIVE_PATH _bioseq_relative_source
    "${CMAKE_SOURCE_DIR}" "${p_source}")
  string(REPLACE "\\" "/"
    _bioseq_relative_source "${_bioseq_relative_source}")

  # Five reviewed *_test.cpp files are compile-only contracts rather than
  # Catch definitions. The PostgreSQL support surface contract compiles inside
  # the common PostgreSQL contract executable; the four SQL consumer fixtures
  # remain owned by the dedicated module-reachability OBJECT target.
  if(_bioseq_relative_source MATCHES
      "^bioseq/tests/unit/.+_test\\.cpp$")
    set(_bioseq_owner bioseq_unit_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bioseq/tests/integration/.+_test\\.cpp$")
    set(_bioseq_owner bioseq_integration_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bioseq/tests/contract/postgres/sql/(active_generated_guc|catalog_entity_handle|generated_guc|sql_boundary_template)_consumer_test\\.cpp$")
    set(_bioseq_owner bioseq_sql_gcc_compile_contract_objects)
  elseif(_bioseq_relative_source MATCHES
      "^bioseq/external/tests/.+_test\\.cpp$")
    set(_bioseq_owner bioseq_external_api_integration_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bench/tests/contract/.+_test\\.cpp$")
    set(_bioseq_owner bioseq_benchmark_e2e_contract_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bench_framework/tests/contract/.+_test\\.cpp$")
    set(_bioseq_owner bioseq_benchmark_framework_contract_tests)
  elseif(_bioseq_relative_source MATCHES
      "^common/checked_arithmetic/tests/contract/.+_test\\.cpp$")
    set(_bioseq_owner bioseq_checked_arithmetic_contract_tests)
  elseif(_bioseq_relative_source MATCHES
      "^common/durable_file_io/tests/contract/.+_test\\.cpp$")
    set(_bioseq_owner bioseq_durable_file_io_contract_tests)
  elseif(_bioseq_relative_source MATCHES
      "^common/postgres/test_support/tests/contract/.+_test\\.cpp$")
    set(_bioseq_owner bioseq_postgres_test_support_contract_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bioseq_caching/tests/unit/[^/]+_test\\.cpp$")
    set(_bioseq_owner bioseq_cache_unit_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bioseq_caching/tests/integration/[^/]+_test\\.cpp$")
    set(_bioseq_owner bioseq_cache_integration_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bioseq_caching/tests/integration/bioseq/[^/]+_test\\.cpp$")
    set(_bioseq_owner bioseq_cache_bioseq_support_integration_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bioseq_caching/tests/e2e/bioseq/[^/]+_test\\.cpp$")
    set(_bioseq_owner bioseq_cache_bioseq_large_input_e2e_tests)
  elseif(_bioseq_relative_source MATCHES
      "^bioseq_caching/tests/contract/benchmark/[^/]+_test\\.cpp$")
    set(_bioseq_owner bioseq_cache_benchmark_report_contract_tests)
  else()
    message(FATAL_ERROR
      "First-party C++ test source has no explicit owner: "
      "${_bioseq_relative_source}")
  endif()
  set(${p_output} "${_bioseq_owner}" PARENT_SCOPE)
endfunction()

function(bioseq_assert_first_party_test_source_ownership)
  set(_bioseq_catch_executables
    bioseq_unit_tests
    bioseq_integration_tests
    bioseq_benchmark_e2e_contract_tests
    bioseq_benchmark_framework_contract_tests
    bioseq_external_api_integration_tests
    bioseq_cache_unit_tests
    bioseq_cache_integration_tests
    bioseq_cache_bioseq_support_integration_tests
    bioseq_cache_bioseq_large_input_e2e_tests
    bioseq_cache_benchmark_report_contract_tests
    bioseq_postgres_test_support_contract_tests
    bioseq_checked_arithmetic_contract_tests
    bioseq_durable_file_io_contract_tests)
  list(LENGTH _bioseq_catch_executables _bioseq_catch_executable_count)
  if(NOT _bioseq_catch_executable_count EQUAL 13)
    message(FATAL_ERROR
      "The first-party Catch ownership table must name exactly 13 executables")
  endif()

  # This inventory is independent of target source collection. Every authored
  # *_test.cpp must match one reviewed path-to-target rule below and appear
  # exactly once in that target's resolved SOURCES property. Explicit target
  # lists and guarded CONFIGURE_DEPENDS target globs therefore receive the same
  # one-owner check without a stale repository-wide cardinality constant.
  file(GLOB_RECURSE _bioseq_authored_test_sources
    CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
    "${CMAKE_SOURCE_DIR}/bench/*_test.cpp"
    "${CMAKE_SOURCE_DIR}/bench_framework/*_test.cpp"
    "${CMAKE_SOURCE_DIR}/bioseq/*_test.cpp"
    "${CMAKE_SOURCE_DIR}/bioseq_caching/*_test.cpp"
    "${CMAKE_SOURCE_DIR}/cmake/*_test.cpp"
    "${CMAKE_SOURCE_DIR}/codegen/*_test.cpp"
    "${CMAKE_SOURCE_DIR}/common/*_test.cpp"
    "${CMAKE_SOURCE_DIR}/infra/*_test.cpp")
  list(FILTER _bioseq_authored_test_sources EXCLUDE REGEX
    "/bench/tool_sneakysnake/")
  list(FILTER _bioseq_authored_test_sources EXCLUDE REGEX
    "/generated/external/api/")
  list(SORT _bioseq_authored_test_sources)

  # This conditional safety-harness self-test is compiled by the runtime
  # profile executables, not by one of the 13 Catch executables. Keep the
  # exclusion exact and fail closed if discovery no longer sees it once.
  set(_bioseq_safety_runtime_harness_self_test_source
    "${CMAKE_SOURCE_DIR}/bioseq/tests/integration/support/safety_runtime_harness_self_test.cpp")
  cmake_path(NORMAL_PATH _bioseq_safety_runtime_harness_self_test_source)
  if(NOT EXISTS "${_bioseq_safety_runtime_harness_self_test_source}")
    message(FATAL_ERROR
      "Reviewed non-Catch safety harness source is missing: "
      "${_bioseq_safety_runtime_harness_self_test_source}")
  endif()
  set(_bioseq_safety_runtime_harness_discovery_count 0)
  foreach(_bioseq_authored_test_source IN LISTS
      _bioseq_authored_test_sources)
    if(_bioseq_authored_test_source STREQUAL
        _bioseq_safety_runtime_harness_self_test_source)
      math(EXPR _bioseq_safety_runtime_harness_discovery_count
        "${_bioseq_safety_runtime_harness_discovery_count} + 1")
    endif()
  endforeach()
  if(NOT _bioseq_safety_runtime_harness_discovery_count EQUAL 1)
    message(FATAL_ERROR
      "Reviewed non-Catch safety harness source must be discovered exactly "
      "once; got ${_bioseq_safety_runtime_harness_discovery_count}: "
      "${_bioseq_safety_runtime_harness_self_test_source}")
  endif()
  list(REMOVE_ITEM _bioseq_authored_test_sources
    "${_bioseq_safety_runtime_harness_self_test_source}")

  set(_bioseq_unit_event_listener_source
    "${CMAKE_SOURCE_DIR}/bioseq/tests/support/test_event_listener.cpp")
  cmake_path(NORMAL_PATH _bioseq_unit_event_listener_source)
  if(NOT EXISTS "${_bioseq_unit_event_listener_source}")
    message(FATAL_ERROR
      "The unit Catch executable requires its prescribed event listener: "
      "${_bioseq_unit_event_listener_source}")
  endif()

  set(_bioseq_generated_test_sources)
  if(TARGET bioseq_external_api_integration_tests)
    get_target_property(
      _bioseq_declared_generated_test_sources
      bioseq_external_api_integration_tests
      BIOSEQ_GENERATED_CATCH_TEST_SOURCES)
    if(NOT _bioseq_declared_generated_test_sources
       OR _bioseq_declared_generated_test_sources MATCHES "-NOTFOUND$")
      message(FATAL_ERROR
        "External Catch executable has no generated-client source inventory")
    endif()
    foreach(_bioseq_generated_test_source IN LISTS
        _bioseq_declared_generated_test_sources)
      _bioseq_normalize_target_source(
        bioseq_external_api_integration_tests
        "${_bioseq_generated_test_source}"
        _bioseq_normalized_generated_test_source)
      list(APPEND _bioseq_generated_test_sources
        "${_bioseq_normalized_generated_test_source}")

      get_filename_component(_bioseq_generated_test_file
        "${_bioseq_normalized_generated_test_source}" NAME_WE)
      get_filename_component(_bioseq_generated_integration_directory
        "${_bioseq_normalized_generated_test_source}" DIRECTORY)
      get_filename_component(_bioseq_generated_tests_directory
        "${_bioseq_generated_integration_directory}" DIRECTORY)
      get_filename_component(_bioseq_generated_client_directory
        "${_bioseq_generated_tests_directory}" DIRECTORY)
      get_filename_component(_bioseq_generated_client_name
        "${_bioseq_generated_client_directory}" NAME)
      if(NOT _bioseq_generated_test_file STREQUAL
          "${_bioseq_generated_client_name}_client_test")
        message(FATAL_ERROR
          "Generated external Catch source does not encode its descriptor "
          "identity: ${_bioseq_normalized_generated_test_source}")
      endif()
    endforeach()
    list(LENGTH
      _bioseq_generated_test_sources _bioseq_generated_test_source_count)
    set(_bioseq_unique_generated_test_sources
      ${_bioseq_generated_test_sources})
    list(REMOVE_DUPLICATES _bioseq_unique_generated_test_sources)
    list(LENGTH
      _bioseq_unique_generated_test_sources
      _bioseq_unique_generated_test_source_count)
    if(NOT _bioseq_generated_test_source_count EQUAL 5
       OR NOT _bioseq_unique_generated_test_source_count EQUAL 5)
      message(FATAL_ERROR
        "External Catch ownership requires five unique generated client "
        "sources; got ${_bioseq_generated_test_source_count} total and "
        "${_bioseq_unique_generated_test_source_count} unique")
    endif()
  endif()

  _bioseq_collect_buildsystem_targets(
    "${CMAKE_SOURCE_DIR}" _bioseq_buildsystem_targets)
  list(REMOVE_DUPLICATES _bioseq_buildsystem_targets)
  set(_bioseq_owned_source_records)
  foreach(_bioseq_target IN LISTS _bioseq_buildsystem_targets)
    get_target_property(_bioseq_target_sources "${_bioseq_target}" SOURCES)
    if(NOT _bioseq_target_sources
       OR _bioseq_target_sources MATCHES "-NOTFOUND$")
      continue()
    endif()
    foreach(_bioseq_target_source IN LISTS _bioseq_target_sources)
      _bioseq_normalize_target_source(
        "${_bioseq_target}" "${_bioseq_target_source}"
        _bioseq_normalized_target_source)
      if(NOT _bioseq_normalized_target_source)
        continue()
      endif()
      if(_bioseq_normalized_target_source IN_LIST
          _bioseq_authored_test_sources
         OR _bioseq_normalized_target_source IN_LIST
          _bioseq_generated_test_sources
         OR _bioseq_normalized_target_source STREQUAL
          _bioseq_unit_event_listener_source)
        list(APPEND _bioseq_owned_source_records
          "${_bioseq_normalized_target_source}|${_bioseq_target}")
      elseif(_bioseq_normalized_target_source MATCHES "_test\\.cpp$"
             AND _bioseq_normalized_target_source MATCHES
               "/generated/external/api/")
        message(FATAL_ERROR
          "Generated external *_test.cpp bypassed its descriptor inventory: "
          "${_bioseq_normalized_target_source}")
      endif()
    endforeach()
  endforeach()

  set(_bioseq_expected_test_sources
    ${_bioseq_authored_test_sources}
    ${_bioseq_generated_test_sources}
    "${_bioseq_unit_event_listener_source}")
  foreach(_bioseq_test_source IN LISTS _bioseq_expected_test_sources)
    if(_bioseq_test_source STREQUAL _bioseq_unit_event_listener_source)
      set(_bioseq_expected_owner bioseq_unit_tests)
    elseif(_bioseq_test_source IN_LIST _bioseq_generated_test_sources)
      set(_bioseq_expected_owner bioseq_external_api_integration_tests)
    else()
      _bioseq_expected_test_owner(
        "${_bioseq_test_source}" _bioseq_expected_owner)
    endif()

    set(_bioseq_source_owners)
    set(_bioseq_record_prefix "${_bioseq_test_source}|")
    string(LENGTH "${_bioseq_record_prefix}" _bioseq_record_prefix_length)
    foreach(_bioseq_owned_source_record IN LISTS
        _bioseq_owned_source_records)
      string(LENGTH
        "${_bioseq_owned_source_record}" _bioseq_record_length)
      if(_bioseq_record_length LESS _bioseq_record_prefix_length)
        continue()
      endif()
      string(SUBSTRING
        "${_bioseq_owned_source_record}"
        0
        ${_bioseq_record_prefix_length}
        _bioseq_candidate_prefix)
      if(_bioseq_candidate_prefix STREQUAL _bioseq_record_prefix)
        string(SUBSTRING
          "${_bioseq_owned_source_record}"
          ${_bioseq_record_prefix_length}
          -1
          _bioseq_source_owner)
        list(APPEND _bioseq_source_owners "${_bioseq_source_owner}")
      endif()
    endforeach()

    list(LENGTH _bioseq_source_owners _bioseq_source_owner_count)
    if(TARGET "${_bioseq_expected_owner}")
      if(NOT _bioseq_source_owner_count EQUAL 1
         OR NOT _bioseq_expected_owner IN_LIST _bioseq_source_owners)
        file(RELATIVE_PATH _bioseq_relative_test_source
          "${CMAKE_SOURCE_DIR}" "${_bioseq_test_source}")
        message(FATAL_ERROR
          "First-party test source must have exactly one expected owner: "
          "${_bioseq_relative_test_source}; expected "
          "${_bioseq_expected_owner}, observed '${_bioseq_source_owners}'")
      endif()
    elseif(NOT _bioseq_source_owner_count EQUAL 0)
      message(FATAL_ERROR
        "Test source is owned while its reviewed target is absent: "
        "${_bioseq_test_source}; observed '${_bioseq_source_owners}'")
    endif()
  endforeach()

  if(BUILD_TESTING
     AND BIOSEQ_BUILD_CORE
     AND BIOSEQ_BUILD_CACHE
     AND BIOSEQ_BUILD_EXTERNAL_API_CLIENTS
     AND BIOSEQ_CACHE_BUILD_TESTS
     AND BIOSEQ_CACHE_TESTING)
    set(_bioseq_configured_catch_executables)
    foreach(_bioseq_catch_executable IN LISTS _bioseq_catch_executables)
      if(TARGET "${_bioseq_catch_executable}")
        list(APPEND _bioseq_configured_catch_executables
          "${_bioseq_catch_executable}")
      endif()
    endforeach()
    list(LENGTH
      _bioseq_configured_catch_executables
      _bioseq_configured_catch_executable_count)
    if(NOT _bioseq_configured_catch_executable_count EQUAL 13)
      message(FATAL_ERROR
        "Combined test profile must configure all 13 reviewed Catch "
        "executables; observed '${_bioseq_configured_catch_executables}'")
    endif()
  endif()
endfunction()
