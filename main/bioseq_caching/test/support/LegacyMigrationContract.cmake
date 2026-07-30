include_guard(GLOBAL)

set(_bioseq_cache_literal_case_pattern
  "TEST_CASE\\([\r\n\t ]*\"[^\"\\\\]+\"[\r\n\t ]*[,)]")
set(_bioseq_cache_single_literal_probe
  "TEST_CASE(\n  \"one literal\",\n  \"[cache]\")")
set(_bioseq_cache_adjacent_literal_probe
  "TEST_CASE(\n  \"adjacent \"\n  \"literals\",\n  \"[cache]\")")
set(_bioseq_cache_nonliteral_probe
  "TEST_CASE(case_name,\n  \"[cache]\")")
string(REGEX MATCHALL "${_bioseq_cache_literal_case_pattern}"
  _bioseq_cache_single_literal_matches
  "${_bioseq_cache_single_literal_probe}")
string(REGEX MATCHALL "${_bioseq_cache_literal_case_pattern}"
  _bioseq_cache_adjacent_literal_matches
  "${_bioseq_cache_adjacent_literal_probe}")
string(REGEX MATCHALL "${_bioseq_cache_literal_case_pattern}"
  _bioseq_cache_nonliteral_matches
  "${_bioseq_cache_nonliteral_probe}")
list(LENGTH _bioseq_cache_single_literal_matches
  _bioseq_cache_single_literal_count)
list(LENGTH _bioseq_cache_adjacent_literal_matches
  _bioseq_cache_adjacent_literal_count)
list(LENGTH _bioseq_cache_nonliteral_matches
  _bioseq_cache_nonliteral_count)
if(NOT _bioseq_cache_single_literal_count EQUAL 1
   OR NOT _bioseq_cache_adjacent_literal_count EQUAL 0
   OR NOT _bioseq_cache_nonliteral_count EQUAL 0)
  message(FATAL_ERROR
    "bioseq_cache Catch manifest literal parser self-check failed")
endif()

function(bioseq_cache_add_legacy_migration_contract)
  set(_options)
  set(_one_value_args LEDGER LEGACY_ROOT LEGACY_INVENTORY PHASE)
  set(_multi_value_args SOURCES TARGETS)
  cmake_parse_arguments(
    PARSE_ARGV 0 CONTRACT
    "${_options}" "${_one_value_args}" "${_multi_value_args}")

  if(NOT CONTRACT_LEDGER OR NOT CONTRACT_LEGACY_ROOT
      OR NOT CONTRACT_LEGACY_INVENTORY
      OR NOT CONTRACT_TARGETS)
    message(FATAL_ERROR
      "bioseq_cache_add_legacy_migration_contract requires "
      "LEDGER, LEGACY_ROOT, LEGACY_INVENTORY, and TARGETS")
  endif()
  if(NOT CONTRACT_PHASE)
    set(CONTRACT_PHASE "current")
  endif()
  if(NOT CONTRACT_PHASE MATCHES "^(current|tracking)$")
    message(FATAL_ERROR
      "migration contract PHASE must be current or tracking")
  endif()
  if(NOT TARGET bioseq_cache_legacy_migration_contract)
    message(FATAL_ERROR
      "bioseq_cache_legacy_migration_contract target is unavailable")
  endif()

  set(_contract_source_paths)
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
      list(APPEND _contract_source_paths "${_source_path}")
    endforeach()
  endforeach()
  foreach(_source IN LISTS CONTRACT_SOURCES)
    if(NOT _source MATCHES "\\.cpp$")
      message(FATAL_ERROR
        "migration contract explicit source is not a .cpp file: ${_source}")
    endif()
    if(IS_ABSOLUTE "${_source}")
      set(_source_path "${_source}")
    else()
      cmake_path(
        ABSOLUTE_PATH _source
        BASE_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE _source_path)
    endif()
    list(APPEND _contract_source_paths "${_source_path}")
  endforeach()
  list(REMOVE_DUPLICATES _contract_source_paths)

  set(_catch_cases)
  foreach(_source_path IN LISTS _contract_source_paths)
    if(NOT EXISTS "${_source_path}")
      message(FATAL_ERROR
        "migration contract cannot inspect Catch source: ${_source_path}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      "${_source_path}")
    file(READ "${_source_path}" _source_text)
    string(REGEX MATCHALL
      "${_bioseq_cache_literal_case_pattern}"
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
        "^TEST_CASE\\([\r\n\t ]*\"([^\"\\\\]+)\"[\r\n\t ]*[,)]$"
        "\\1" _case_name "${_declaration}")
      if(_case_name IN_LIST _catch_cases)
        message(FATAL_ERROR
          "duplicate bioseq_cache Catch case name: ${_case_name}")
      endif()
      list(APPEND _catch_cases "${_case_name}")
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

  file(STRINGS "${CONTRACT_LEGACY_INVENTORY}" _legacy_inventory_rows)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CONTRACT_LEDGER}" "${CONTRACT_LEGACY_INVENTORY}")
  list(POP_FRONT _legacy_inventory_rows _legacy_inventory_header)
  if(NOT _legacy_inventory_header STREQUAL "legacy_path\tsha256")
    message(FATAL_ERROR
      "legacy asset inventory has an unexpected header")
  endif()
  set(_legacy_assets)
  foreach(_inventory_row IN LISTS _legacy_inventory_rows)
    string(REPLACE "\t" ";" _inventory_fields "${_inventory_row}")
    list(LENGTH _inventory_fields _inventory_field_count)
    if(NOT _inventory_field_count EQUAL 2)
      message(FATAL_ERROR
        "legacy asset inventory row must contain path and SHA-256")
    endif()
    list(GET _inventory_fields 0 _inventory_path)
    list(GET _inventory_fields 1 _inventory_sha256)
    string(LENGTH "${_inventory_sha256}" _inventory_sha256_length)
    if(NOT _inventory_sha256_length EQUAL 64
       OR _inventory_sha256 MATCHES "[^0-9a-f]")
      message(FATAL_ERROR
        "legacy asset inventory row has invalid SHA-256")
    endif()
    list(APPEND _legacy_assets "${_inventory_path}")
  endforeach()
  list(LENGTH _legacy_assets _legacy_asset_count)
  if(NOT _legacy_asset_count EQUAL 85)
    message(FATAL_ERROR
      "legacy asset inventory must contain exactly 85 regular-file paths")
  endif()
  set(_unique_legacy_assets ${_legacy_assets})
  list(REMOVE_DUPLICATES _unique_legacy_assets)
  list(LENGTH _unique_legacy_assets _unique_legacy_asset_count)
  if(NOT _unique_legacy_asset_count EQUAL _legacy_asset_count)
    message(FATAL_ERROR "legacy asset inventory contains duplicate paths")
  endif()
  add_test(
    NAME bioseq_cache_legacy_migration_contract
    COMMAND bioseq_cache_legacy_migration_contract
      --ledger "${CONTRACT_LEDGER}"
      --catch-cases "${_catch_case_manifest}"
      --legacy-root "${CONTRACT_LEGACY_ROOT}"
      --legacy-inventory "${CONTRACT_LEGACY_INVENTORY}"
      --phase "${CONTRACT_PHASE}"
  )
  set_tests_properties(
    bioseq_cache_legacy_migration_contract
    PROPERTIES
      LABELS "cache;unit;migration;deletion-gate"
      TIMEOUT 30)
endfunction()
