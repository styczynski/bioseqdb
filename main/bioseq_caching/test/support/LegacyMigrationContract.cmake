include_guard(GLOBAL)

function(bioseq_cache_add_legacy_migration_contract)
  set(_options)
  set(_one_value_args LEDGER LEGACY_ROOT)
  set(_multi_value_args TARGETS LEGACY_ASSETS)
  cmake_parse_arguments(
    PARSE_ARGV 0 CONTRACT
    "${_options}" "${_one_value_args}" "${_multi_value_args}")

  if(NOT CONTRACT_LEDGER OR NOT CONTRACT_LEGACY_ROOT
      OR NOT CONTRACT_TARGETS)
    message(FATAL_ERROR
      "bioseq_cache_add_legacy_migration_contract requires "
      "LEDGER, LEGACY_ROOT, and TARGETS")
  endif()
  if(NOT TARGET bioseq_cache_legacy_migration_contract)
    message(FATAL_ERROR
      "bioseq_cache_legacy_migration_contract target is unavailable")
  endif()

  set(_catch_cases)
  foreach(_target IN LISTS CONTRACT_TARGETS)
    if(NOT TARGET "${_target}")
      message(FATAL_ERROR
        "migration contract Catch target does not exist: ${_target}")
    endif()
    add_dependencies(
      "${_target}" bioseq_cache_legacy_migration_contract)
    get_target_property(_source_dir "${_target}" SOURCE_DIR)
    get_target_property(_sources "${_target}" SOURCES)
    foreach(_source IN LISTS _sources)
      if(_source MATCHES "^\\$<" OR NOT _source MATCHES "\\.cpp$")
        continue()
      endif()
      if(IS_ABSOLUTE "${_source}")
        set(_source_path "${_source}")
      else()
        cmake_path(
          ABSOLUTE_PATH _source
          BASE_DIRECTORY "${_source_dir}"
          OUTPUT_VARIABLE _source_path)
      endif()
      if(NOT EXISTS "${_source_path}")
        message(FATAL_ERROR
          "migration contract cannot inspect Catch source: ${_source_path}")
      endif()

      file(READ "${_source_path}" _source_text)
      string(REGEX MATCHALL
        "TEST_CASE\\([\r\n\t ]*\"[^\"]+\""
        _declarations "${_source_text}")
      string(REGEX MATCHALL "TEST_CASE\\(" _markers "${_source_text}")
      list(LENGTH _declarations _declaration_count)
      list(LENGTH _markers _marker_count)
      if(NOT _declaration_count EQUAL _marker_count)
        message(FATAL_ERROR
          "every bioseq_cache TEST_CASE must use a literal name so the "
          "migration contract can audit the Catch registry: ${_source_path}")
      endif()
      foreach(_declaration IN LISTS _declarations)
        string(REGEX REPLACE
          "^TEST_CASE\\([\r\n\t ]*\"([^\"]+)\"$"
          "\\1" _case_name "${_declaration}")
        if(_case_name IN_LIST _catch_cases)
          message(FATAL_ERROR
            "duplicate bioseq_cache Catch case name: ${_case_name}")
        endif()
        list(APPEND _catch_cases "${_case_name}")
      endforeach()
    endforeach()
  endforeach()
  if(NOT _catch_cases)
    message(FATAL_ERROR
      "migration contract did not discover any Catch test cases")
  endif()

  list(SORT _catch_cases)
  list(JOIN _catch_cases "\n" _catch_case_manifest_text)
  set(_catch_case_manifest
    "${CMAKE_CURRENT_BINARY_DIR}/legacy_migration_catch_cases.txt")
  file(WRITE "${_catch_case_manifest}"
    "${_catch_case_manifest_text}\n")

  set(_legacy_asset_arguments)
  foreach(_asset IN LISTS CONTRACT_LEGACY_ASSETS)
    list(APPEND _legacy_asset_arguments
      "--legacy-asset" "${_asset}")
  endforeach()
  add_test(
    NAME bioseq_cache_legacy_migration_contract
    COMMAND bioseq_cache_legacy_migration_contract
      --ledger "${CONTRACT_LEDGER}"
      --catch-cases "${_catch_case_manifest}"
      --legacy-root "${CONTRACT_LEGACY_ROOT}"
      ${_legacy_asset_arguments})
  set_tests_properties(
    bioseq_cache_legacy_migration_contract
    PROPERTIES
      LABELS "cache;unit;migration;deletion-gate"
      TIMEOUT 30)
endfunction()
