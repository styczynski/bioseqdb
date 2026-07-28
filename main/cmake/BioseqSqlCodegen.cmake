include_guard(GLOBAL)

# DOC0388 SQL code generation is driven only by owners registered through this
# file.  The registry is intentionally strict: a source cannot be inferred from
# a build target, shared by two owners, or silently added to only one profile.

function(_bioseq_sql_registry_fail message_text)
  string(JOIN "" _full_message "${message_text}" ${ARGN})
  message(FATAL_ERROR
    "DOC0388 SQL owner registry: ${_full_message}")
endfunction()

function(_bioseq_sql_require_unique_values label)
  set(_values ${ARGN})
  set(_unique_values ${_values})
  list(REMOVE_DUPLICATES _unique_values)
  list(LENGTH _values _value_count)
  list(LENGTH _unique_values _unique_value_count)
  if(NOT _value_count EQUAL _unique_value_count)
    _bioseq_sql_registry_fail(
      "${label} contains a duplicate value: ${_values}")
  endif()
endfunction()

function(_bioseq_sql_annotation_authorized
    output_variable source_path annotation_index
    explicit_annotation_free_authority)
  # source_path is deliberately not pattern-matched: a basename can never
  # grant SQL ownership.
  set(_authorized FALSE)
  if(NOT annotation_index EQUAL -1
      OR explicit_annotation_free_authority)
    set(_authorized TRUE)
  endif()
  set(${output_variable} "${_authorized}" PARENT_SCOPE)
endfunction()

function(_bioseq_sql_legacy_fmgr_contract_authorized output_variable)
  cmake_parse_arguments(
    PARSE_ARGV 1 ARG
    ""
    "OWNER_KEY;MODULE;SOURCE;PROFILES;AUTHORITIES;SYMBOL"
    "")
  set(_authorized TRUE)
  if(ARG_SYMBOL)
    set(_authorized FALSE)
    if(ARG_OWNER_KEY STREQUAL "external_relation_legacy_fmgr"
        AND ARG_MODULE STREQUAL "bioseq.pg.external.relation"
        AND ARG_SOURCE STREQUAL
          "backend/pg/external/relation.cppm"
        AND ARG_PROFILES STREQUAL "production;test_indexes"
        AND ARG_AUTHORITIES STREQUAL "FINAL"
        AND ARG_SYMBOL STREQUAL "bioseq_external_handler")
      set(_authorized TRUE)
    endif()
  endif()
  set(${output_variable} "${_authorized}" PARENT_SCOPE)
endfunction()

function(_bioseq_sql_run_annotation_authority_contracts)
  _bioseq_sql_annotation_authorized(
    _unregistered_named_like_row
    "/fixture/unregistered_sql_rows.cppm" -1 FALSE)
  if(_unregistered_named_like_row)
    _bioseq_sql_registry_fail(
      "annotation authority contract accepted an unregistered "
      "*_sql_rows.cppm basename")
  endif()

  _bioseq_sql_annotation_authorized(
    _explicit_result_row
    "/fixture/registered_result.cppm" -1 TRUE)
  if(NOT _explicit_result_row)
    _bioseq_sql_registry_fail(
      "annotation authority contract rejected an explicit result-row facet")
  endif()

  _bioseq_sql_annotation_authorized(
    _unregistered_named_like_external
    "/fixture/external/relation.cppm" -1 FALSE)
  if(_unregistered_named_like_external)
    _bioseq_sql_registry_fail(
      "annotation authority contract accepted an unregistered external path")
  endif()

  _bioseq_sql_annotation_authorized(
    _explicit_legacy_fmgr_scan
    "/fixture/external/relation.cppm" -1 TRUE)
  if(NOT _explicit_legacy_fmgr_scan)
    _bioseq_sql_registry_fail(
      "annotation authority contract rejected an explicit legacy-fmgr facet")
  endif()

  _bioseq_sql_annotation_authorized(
    _annotated_owner
    "/fixture/ordinary.cppm" 0 FALSE)
  if(NOT _annotated_owner)
    _bioseq_sql_registry_fail(
      "annotation authority contract rejected an annotated owner")
  endif()

  _bioseq_sql_legacy_fmgr_contract_authorized(
    _natural_external_rows
    OWNER_KEY external_sql_catalog
    MODULE bioseq.pg.external.sql_catalog
    SOURCE backend/pg/external/sql_catalog.cppm
    PROFILES "production;test_indexes"
    AUTHORITIES FINAL)
  if(NOT _natural_external_rows)
    _bioseq_sql_registry_fail(
      "legacy-fmgr contract rejected a natural FINAL external SQL owner")
  endif()

  _bioseq_sql_legacy_fmgr_contract_authorized(
    _accidental_external_legacy_scan
    OWNER_KEY external_sql_catalog
    MODULE bioseq.pg.external.sql_catalog
    SOURCE backend/pg/external/sql_catalog.cppm
    PROFILES "production;test_indexes"
    AUTHORITIES FINAL
    SYMBOL accidental_external_handler)
  if(_accidental_external_legacy_scan)
    _bioseq_sql_registry_fail(
      "legacy-fmgr contract accepted an accidental external scan")
  endif()

  _bioseq_sql_legacy_fmgr_contract_authorized(
    _reviewed_external_legacy_scan
    OWNER_KEY external_relation_legacy_fmgr
    MODULE bioseq.pg.external.relation
    SOURCE backend/pg/external/relation.cppm
    PROFILES "production;test_indexes"
    AUTHORITIES FINAL
    SYMBOL bioseq_external_handler)
  if(NOT _reviewed_external_legacy_scan)
    _bioseq_sql_registry_fail(
      "legacy-fmgr contract rejected the reviewed external handler")
  endif()
endfunction()

_bioseq_sql_run_annotation_authority_contracts()

function(_bioseq_sql_write_registry_file output_path)
  set(_entries ${ARGN})
  list(SORT _entries)
  list(REMOVE_DUPLICATES _entries)
  string(JOIN "\n" _content ${_entries})
  string(APPEND _content "\n")
  if(EXISTS "${output_path}")
    file(READ "${output_path}" _existing_content)
    if(_existing_content STREQUAL _content)
      return()
    endif()
  endif()
  file(WRITE "${output_path}" "${_content}")
endfunction()

function(bioseq_sql_begin_owner_registry)
  cmake_parse_arguments(
    ARG
    ""
    "ROOT"
    "PROFILES;AUDIT_ROOTS"
    ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_sql_registry_fail(
      "bioseq_sql_begin_owner_registry received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT ARG_ROOT)
    _bioseq_sql_registry_fail(
      "bioseq_sql_begin_owner_registry requires ROOT")
  endif()
  if(NOT ARG_PROFILES)
    _bioseq_sql_registry_fail(
      "bioseq_sql_begin_owner_registry requires PROFILES")
  endif()

  get_property(_registry_started GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_STARTED SET)
  if(_registry_started)
    _bioseq_sql_registry_fail("the owner registry was initialized twice")
  endif()

  file(REAL_PATH "${ARG_ROOT}" _registry_root)
  if(NOT IS_DIRECTORY "${_registry_root}")
    _bioseq_sql_registry_fail(
      "registry root does not exist: ${ARG_ROOT}")
  endif()

  _bioseq_sql_require_unique_values("PROFILES" ${ARG_PROFILES})
  set(_profiles ${ARG_PROFILES})
  list(SORT _profiles)
  if(NOT "${_profiles}" STREQUAL "production;test_indexes")
    _bioseq_sql_registry_fail(
      "the closed profile set must be exactly production;test_indexes, got "
      "${_profiles}")
  endif()

  set(_audit_roots)
  foreach(_audit_root IN LISTS ARG_AUDIT_ROOTS)
    if(IS_ABSOLUTE "${_audit_root}")
      set(_audit_candidate "${_audit_root}")
    else()
      set(_audit_candidate "${_registry_root}/${_audit_root}")
    endif()
    file(REAL_PATH "${_audit_candidate}" _audit_real)
    if(NOT IS_DIRECTORY "${_audit_real}")
      _bioseq_sql_registry_fail(
        "audit root does not exist: ${_audit_candidate}")
    endif()
    string(FIND "${_audit_real}/" "${_registry_root}/" _root_prefix)
    if(NOT _root_prefix EQUAL 0)
      _bioseq_sql_registry_fail(
        "audit root escapes ROOT: ${_audit_real}")
    endif()
    list(APPEND _audit_roots "${_audit_real}")
  endforeach()
  if(NOT _audit_roots)
    _bioseq_sql_registry_fail(
      "at least one AUDIT_ROOTS entry is required")
  endif()
  _bioseq_sql_require_unique_values("AUDIT_ROOTS" ${_audit_roots})

  set_property(GLOBAL PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_STARTED TRUE)
  set_property(GLOBAL PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_FINALIZED FALSE)
  set_property(GLOBAL PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_ROOT
    "${_registry_root}")
  set_property(GLOBAL PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_PROFILES
    "${_profiles}")
  set_property(GLOBAL PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_AUDIT_ROOTS
    "${_audit_roots}")
endfunction()

function(bioseq_register_sql_owner)
  cmake_parse_arguments(
    ARG
    ""
    "OWNER_KEY;MODULE"
    "SOURCES;PROFILES;AUTHORITIES;ROW_CODECS;ENTITY_HANDLES;FMGR_ENTRYPOINTS;GUC_OWNERS;ANNOTATION_FREE_RESULT_ROWS;LEGACY_FMGR_SCANS"
    ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_sql_registry_fail(
      "bioseq_register_sql_owner received unknown arguments for "
      "${ARG_OWNER_KEY}: ${ARG_UNPARSED_ARGUMENTS}")
  endif()

  get_property(_registry_started GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_STARTED SET)
  if(NOT _registry_started)
    _bioseq_sql_registry_fail(
      "bioseq_sql_begin_owner_registry must be called before registration")
  endif()
  get_property(_registry_finalized GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_FINALIZED)
  if(_registry_finalized)
    _bioseq_sql_registry_fail(
      "owner ${ARG_OWNER_KEY} was registered after finalization")
  endif()

  foreach(_required_argument IN ITEMS OWNER_KEY MODULE)
    if(NOT ARG_${_required_argument})
      _bioseq_sql_registry_fail(
        "bioseq_register_sql_owner requires ${_required_argument}")
    endif()
  endforeach()
  if(NOT ARG_SOURCES)
    _bioseq_sql_registry_fail(
      "owner ${ARG_OWNER_KEY} has no SOURCES")
  endif()
  if(NOT ARG_PROFILES)
    _bioseq_sql_registry_fail(
      "owner ${ARG_OWNER_KEY} has no PROFILES")
  endif()
  if(NOT ARG_AUTHORITIES)
    _bioseq_sql_registry_fail(
      "owner ${ARG_OWNER_KEY} has no AUTHORITIES")
  endif()

  if(NOT ARG_OWNER_KEY MATCHES "^[a-z][a-z0-9_]*$")
    _bioseq_sql_registry_fail(
      "invalid stable owner key '${ARG_OWNER_KEY}'")
  endif()
  if(NOT ARG_MODULE MATCHES
      "^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*(:[A-Za-z_][A-Za-z0-9_]*)?$")
    _bioseq_sql_registry_fail(
      "invalid C++ module name '${ARG_MODULE}' for ${ARG_OWNER_KEY}")
  endif()

  get_property(_registered_owner_keys GLOBAL
    PROPERTY BIOSEQ_SQL_REGISTERED_OWNER_KEYS)
  list(FIND _registered_owner_keys "${ARG_OWNER_KEY}" _owner_key_index)
  if(NOT _owner_key_index EQUAL -1)
    _bioseq_sql_registry_fail(
      "duplicate owner key '${ARG_OWNER_KEY}'")
  endif()
  get_property(_registered_modules GLOBAL
    PROPERTY BIOSEQ_SQL_REGISTERED_MODULES)
  list(FIND _registered_modules "${ARG_MODULE}" _module_index)
  if(NOT _module_index EQUAL -1)
    _bioseq_sql_registry_fail(
      "module '${ARG_MODULE}' is registered by more than one owner")
  endif()

  _bioseq_sql_require_unique_values(
    "profiles for ${ARG_OWNER_KEY}" ${ARG_PROFILES})
  get_property(_allowed_profiles GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_PROFILES)
  foreach(_profile IN LISTS ARG_PROFILES)
    list(FIND _allowed_profiles "${_profile}" _profile_index)
    if(_profile_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} uses unknown profile '${_profile}'")
    endif()
  endforeach()

  _bioseq_sql_require_unique_values(
    "authorities for ${ARG_OWNER_KEY}" ${ARG_AUTHORITIES})
  list(LENGTH ARG_AUTHORITIES _authority_count)
  if(NOT _authority_count EQUAL 1)
    _bioseq_sql_registry_fail(
      "owner ${ARG_OWNER_KEY} must select exactly one of "
      "SUPPORT, CORE, or FINAL")
  endif()
  foreach(_authority IN LISTS ARG_AUTHORITIES)
    if(NOT _authority MATCHES "^(SUPPORT|CORE|FINAL)$")
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} uses unknown authority '${_authority}'")
    endif()
  endforeach()
  list(FIND ARG_AUTHORITIES SUPPORT _support_index)
  list(FIND ARG_AUTHORITIES CORE _core_index)
  list(FIND ARG_AUTHORITIES FINAL _final_index)
  if(NOT _support_index EQUAL -1 AND NOT _final_index EQUAL -1)
    _bioseq_sql_registry_fail(
      "owner ${ARG_OWNER_KEY} cannot combine dependency-only SUPPORT and FINAL")
  endif()
  if(NOT _core_index EQUAL -1)
    set(_core_profiles ${ARG_PROFILES})
    list(SORT _core_profiles)
    if(NOT "${_core_profiles}" STREQUAL "production;test_indexes")
      _bioseq_sql_registry_fail(
        "CORE owner ${ARG_OWNER_KEY} must be registered in both profiles")
    endif()
  endif()

  foreach(_facet_type IN ITEMS
      ROW_CODECS ENTITY_HANDLES FMGR_ENTRYPOINTS GUC_OWNERS
      ANNOTATION_FREE_RESULT_ROWS LEGACY_FMGR_SCANS)
    set(_facet_values ${ARG_${_facet_type}})
    _bioseq_sql_require_unique_values(
      "${_facet_type} facets for ${ARG_OWNER_KEY}" ${_facet_values})
    foreach(_facet_key IN LISTS _facet_values)
      if(NOT _facet_key MATCHES "^[a-z][a-z0-9_]*$")
        _bioseq_sql_registry_fail(
          "invalid ${_facet_type} key '${_facet_key}' on ${ARG_OWNER_KEY}")
      endif()
      get_property(_registered_facets GLOBAL
        PROPERTY BIOSEQ_SQL_REGISTERED_${_facet_type})
      list(FIND _registered_facets "${_facet_key}" _facet_index)
      if(NOT _facet_index EQUAL -1)
        _bioseq_sql_registry_fail(
          "${_facet_type} key '${_facet_key}' has multiple owners")
      endif()
      set_property(GLOBAL APPEND
        PROPERTY BIOSEQ_SQL_REGISTERED_${_facet_type}
        "${_facet_key}")
    endforeach()
  endforeach()
  if(ARG_ROW_CODECS OR ARG_GUC_OWNERS
      OR ARG_ANNOTATION_FREE_RESULT_ROWS)
    if(_support_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "row-codec/GUC/annotation-free-result-row owner ${ARG_OWNER_KEY} "
        "must have SUPPORT authority")
    endif()
  endif()
  if(ARG_ANNOTATION_FREE_RESULT_ROWS)
    list(LENGTH ARG_ANNOTATION_FREE_RESULT_ROWS
      _annotation_free_result_row_count)
    list(LENGTH ARG_SOURCES _annotation_free_source_count)
    if(NOT _annotation_free_result_row_count EQUAL 1
        OR NOT _annotation_free_source_count EQUAL 1)
      _bioseq_sql_registry_fail(
        "annotation-free result-row owner ${ARG_OWNER_KEY} must register "
        "exactly one authority key and one source")
    endif()
    if(ARG_ROW_CODECS OR ARG_GUC_OWNERS)
      _bioseq_sql_registry_fail(
        "annotation-free result-row owner ${ARG_OWNER_KEY} cannot also "
        "own row-codec or GUC facets")
    endif()
  endif()
  if(ARG_LEGACY_FMGR_SCANS)
    list(LENGTH ARG_LEGACY_FMGR_SCANS _legacy_fmgr_scan_count)
    list(LENGTH ARG_SOURCES _legacy_fmgr_source_count)
    if(NOT _legacy_fmgr_scan_count EQUAL 1
        OR NOT _legacy_fmgr_source_count EQUAL 1)
      _bioseq_sql_registry_fail(
        "legacy-fmgr scan owner ${ARG_OWNER_KEY} must register exactly one "
        "reviewed symbol and one source")
    endif()
    if(_final_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "legacy-fmgr scan owner ${ARG_OWNER_KEY} must have FINAL authority")
    endif()
    if(ARG_ROW_CODECS OR ARG_ENTITY_HANDLES OR ARG_FMGR_ENTRYPOINTS
        OR ARG_GUC_OWNERS OR ARG_ANNOTATION_FREE_RESULT_ROWS)
      _bioseq_sql_registry_fail(
        "legacy-fmgr scan owner ${ARG_OWNER_KEY} cannot own generated SQL "
        "facets")
    endif()
    list(GET ARG_SOURCES 0 _legacy_fmgr_source)
    list(GET ARG_LEGACY_FMGR_SCANS 0 _legacy_fmgr_symbol)
    set(_legacy_fmgr_profiles ${ARG_PROFILES})
    list(SORT _legacy_fmgr_profiles)
    set(_legacy_fmgr_authorities ${ARG_AUTHORITIES})
    list(SORT _legacy_fmgr_authorities)
    _bioseq_sql_legacy_fmgr_contract_authorized(
      _legacy_fmgr_contract_authorized
      OWNER_KEY "${ARG_OWNER_KEY}"
      MODULE "${ARG_MODULE}"
      SOURCE "${_legacy_fmgr_source}"
      PROFILES "${_legacy_fmgr_profiles}"
      AUTHORITIES "${_legacy_fmgr_authorities}"
      SYMBOL "${_legacy_fmgr_symbol}")
    if(NOT _legacy_fmgr_contract_authorized)
      _bioseq_sql_registry_fail(
        "legacy-fmgr scan authority is restricted to "
        "external_relation_legacy_fmgr / bioseq.pg.external.relation / "
        "backend/pg/external/relation.cppm / bioseq_external_handler in "
        "both profiles with FINAL-only authority")
    endif()
  endif()
  if(ARG_ENTITY_HANDLES AND _core_index EQUAL -1)
    _bioseq_sql_registry_fail(
      "entity-handle owner ${ARG_OWNER_KEY} must have CORE authority")
  endif()
  if(ARG_FMGR_ENTRYPOINTS AND _final_index EQUAL -1)
    _bioseq_sql_registry_fail(
      "fmgr owner ${ARG_OWNER_KEY} must have FINAL authority")
  endif()

  get_property(_registry_root GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_ROOT)
  get_property(_audit_roots GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_AUDIT_ROOTS)
  get_property(_registered_sources GLOBAL
    PROPERTY BIOSEQ_SQL_REGISTERED_SOURCES)
  set(_owner_sources)
  foreach(_source IN LISTS ARG_SOURCES)
    if(IS_ABSOLUTE "${_source}")
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} source paths must be ROOT-relative: ${_source}")
    endif()
    cmake_path(NORMAL_PATH _source OUTPUT_VARIABLE _normalized_source)
    if(_normalized_source MATCHES "(^|/)\\.\\.(/|$)")
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} source escapes ROOT: ${_source}")
    endif()
    if(_normalized_source MATCHES
        "(^|/)(build|cmake-build[^/]*|generated|integration_tests|unit_tests|tests|vendor)(/|$)")
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} uses a forbidden source path: "
        "${_normalized_source}")
    endif()
    set(_source_candidate "${_registry_root}/${_normalized_source}")
    if(NOT EXISTS "${_source_candidate}")
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} source does not exist: ${_source_candidate}")
    endif()
    file(REAL_PATH "${_source_candidate}" _source_real)
    string(FIND "${_source_real}" "${_registry_root}/" _root_prefix)
    if(NOT _root_prefix EQUAL 0)
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} source escapes ROOT after resolution: "
        "${_source_real}")
    endif()
    if(NOT _source_real MATCHES "\\.cppm$")
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} source is not a .cppm file: ${_source_real}")
    endif()

    set(_inside_audit_root FALSE)
    foreach(_audit_root IN LISTS _audit_roots)
      string(FIND "${_source_real}" "${_audit_root}/" _audit_prefix)
      if(_audit_prefix EQUAL 0)
        set(_inside_audit_root TRUE)
      endif()
    endforeach()
    if(NOT _inside_audit_root)
      _bioseq_sql_registry_fail(
        "owner ${ARG_OWNER_KEY} source is outside AUDIT_ROOTS: ${_source_real}")
    endif()

    list(FIND _registered_sources "${_source_real}" _source_index)
    if(NOT _source_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "source is shared by multiple SQL owners: ${_source_real}")
    endif()
    list(APPEND _registered_sources "${_source_real}")
    list(APPEND _owner_sources "${_source_real}")

    file(READ "${_source_real}" _source_content)
    string(FIND "${_source_content}" "bioseq_sql::" _annotation_index)
    set(_explicit_annotation_free_authority FALSE)
    if(ARG_ANNOTATION_FREE_RESULT_ROWS)
      # This is an explicit owner-registry authority, not a filename
      # convention. Its row contract is derived from the matching FINAL
      # routine's structural return type.
      set(_explicit_annotation_free_authority TRUE)
    elseif(ARG_LEGACY_FMGR_SCANS
        AND _annotation_index EQUAL -1)
      # A legacy scan can authorize an otherwise annotation-free compatibility
      # source.  The reviewed external handler now also owns natural SQL
      # annotations, in which case those annotations provide source authority
      # and the exact legacy facet independently closes wrapper ownership.
      set(_explicit_annotation_free_authority TRUE)
    endif()
    _bioseq_sql_annotation_authorized(
      _annotation_authorized
      "${_source_real}"
      "${_annotation_index}"
      "${_explicit_annotation_free_authority}")
    if(NOT _annotation_authorized)
      _bioseq_sql_registry_fail(
        "registered source has no bioseq_sql annotation: ${_source_real}")
    endif()
    if(NOT _annotation_index EQUAL -1
        AND _explicit_annotation_free_authority)
      _bioseq_sql_registry_fail(
        "annotation-free registry authority is unnecessary for annotated "
        "source: ${_source_real}")
    endif()
    string(REGEX MATCH
      "export[ \t\r\n]+module[ \t\r\n]+([A-Za-z_][A-Za-z0-9_.:]*)([ \t\r\n]*);"
      _module_declaration
      "${_source_content}")
    if(NOT _module_declaration)
      _bioseq_sql_registry_fail(
        "registered owner source has no exported module declaration: "
        "${_source_real}")
    endif()
    set(_declared_source_module "${CMAKE_MATCH_1}")
    if(NOT _declared_source_module STREQUAL ARG_MODULE
        AND NOT _declared_source_module MATCHES
          "^${ARG_MODULE}[:.][A-Za-z_][A-Za-z0-9_]*$")
      _bioseq_sql_registry_fail(
        "source module '${_declared_source_module}' is outside registered "
        "owner closure '${ARG_MODULE}': ${_source_real}")
    endif()
    if(NOT _support_index EQUAL -1)
      if(NOT _normalized_source MATCHES
          "(^|/)([^/]*_sql_rows|sql_rows|[^/]*_sql_guc_settings|sql_guc_settings)\\.cppm$")
        _bioseq_sql_registry_fail(
          "SUPPORT source is not a dependency-only sql_rows/GUC module: "
          "${_normalized_source}")
      endif()
      if(_source_content MATCHES
          "import[ \t\r\n]+bioseq\\.sql\\.generated")
        _bioseq_sql_registry_fail(
          "SUPPORT source imports generated SQL modules: ${_source_real}")
      endif()
    endif()
    if(NOT _core_index EQUAL -1
        AND _source_content MATCHES
          "import[ \t\r\n]+bioseq\\.sql\\.generated")
      _bioseq_sql_registry_fail(
        "CORE source imports generated SQL modules and cannot bootstrap: "
        "${_source_real}")
    endif()
  endforeach()

  set(_profiles_for_record ${ARG_PROFILES})
  list(SORT _profiles_for_record)
  string(JOIN "," _profile_record ${_profiles_for_record})
  set(_authorities_for_record ${ARG_AUTHORITIES})
  list(SORT _authorities_for_record)
  string(JOIN "," _authority_record ${_authorities_for_record})
  set(_facets_for_record)
  foreach(_facet_key IN LISTS ARG_ROW_CODECS)
    list(APPEND _facets_for_record "row_codecs=${_facet_key}")
  endforeach()
  foreach(_facet_key IN LISTS ARG_ENTITY_HANDLES)
    list(APPEND _facets_for_record "entity_handles=${_facet_key}")
  endforeach()
  foreach(_facet_key IN LISTS ARG_FMGR_ENTRYPOINTS)
    list(APPEND _facets_for_record "fmgr_entrypoints=${_facet_key}")
  endforeach()
  foreach(_facet_key IN LISTS ARG_GUC_OWNERS)
    list(APPEND _facets_for_record "guc_owner=${_facet_key}")
  endforeach()
  foreach(_facet_key IN LISTS ARG_ANNOTATION_FREE_RESULT_ROWS)
    list(APPEND _facets_for_record
      "annotation_free_result_row=${_facet_key}")
  endforeach()
  foreach(_facet_key IN LISTS ARG_LEGACY_FMGR_SCANS)
    list(APPEND _facets_for_record
      "legacy_fmgr_scan=${_facet_key}")
  endforeach()
  list(SORT _facets_for_record)
  string(JOIN "," _facet_record ${_facets_for_record})

  foreach(_source_real IN LISTS _owner_sources)
    foreach(_profile IN LISTS ARG_PROFILES)
      string(TOUPPER "${_profile}" _profile_key)
      set_property(GLOBAL APPEND
        PROPERTY BIOSEQ_SQL_${_profile_key}_FINAL_SOURCES
        "${_source_real}")
      if(NOT _support_index EQUAL -1)
        set_property(GLOBAL APPEND
          PROPERTY BIOSEQ_SQL_${_profile_key}_SUPPORT_SOURCES
          "${_source_real}")
      endif()
      if(NOT _core_index EQUAL -1)
        set_property(GLOBAL APPEND
          PROPERTY BIOSEQ_SQL_${_profile_key}_CORE_SOURCES
          "${_source_real}")
      endif()
    endforeach()
    set_property(GLOBAL APPEND PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_RECORDS
      "${ARG_OWNER_KEY}\t${ARG_MODULE}\t${_profile_record}\t${_authority_record}\t${_facet_record}\t${_source_real}")
  endforeach()

  set_property(GLOBAL APPEND PROPERTY BIOSEQ_SQL_REGISTERED_OWNER_KEYS
    "${ARG_OWNER_KEY}")
  set_property(GLOBAL APPEND PROPERTY BIOSEQ_SQL_REGISTERED_MODULES
    "${ARG_MODULE}")
  set_property(GLOBAL PROPERTY BIOSEQ_SQL_REGISTERED_SOURCES
    "${_registered_sources}")
endfunction()

function(bioseq_finalize_sql_owner_registry)
  cmake_parse_arguments(
    ARG
    ""
    "OUTPUT_DIR"
    "EXPECTED_ROW_CODECS;EXPECTED_ENTITY_HANDLES;EXPECTED_FMGR_ENTRYPOINTS;EXPECTED_GUC_OWNERS;EXPECTED_ANNOTATION_FREE_RESULT_ROWS;EXPECTED_LEGACY_FMGR_SCANS"
    ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_sql_registry_fail(
      "bioseq_finalize_sql_owner_registry received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT ARG_OUTPUT_DIR)
    _bioseq_sql_registry_fail(
      "bioseq_finalize_sql_owner_registry requires OUTPUT_DIR")
  endif()

  get_property(_registry_started GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_STARTED SET)
  if(NOT _registry_started)
    _bioseq_sql_registry_fail(
      "cannot finalize an owner registry that was not initialized")
  endif()
  get_property(_registry_finalized GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_FINALIZED)
  if(_registry_finalized)
    _bioseq_sql_registry_fail("the owner registry was finalized twice")
  endif()

  get_filename_component(_output_dir "${ARG_OUTPUT_DIR}" ABSOLUTE)
  get_property(_registry_root GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_ROOT)
  string(FIND "${_output_dir}/" "${_registry_root}/" _source_tree_prefix)
  if(_source_tree_prefix EQUAL 0)
    _bioseq_sql_registry_fail(
      "registry outputs must be in the build tree, not under ${_registry_root}")
  endif()
  file(MAKE_DIRECTORY "${_output_dir}")

  foreach(_facet_type IN ITEMS
      ROW_CODECS ENTITY_HANDLES FMGR_ENTRYPOINTS GUC_OWNERS
      ANNOTATION_FREE_RESULT_ROWS LEGACY_FMGR_SCANS)
    set(_expected_argument "ARG_EXPECTED_${_facet_type}")
    set(_expected_facets ${${_expected_argument}})
    _bioseq_sql_require_unique_values(
      "EXPECTED_${_facet_type}" ${_expected_facets})
    get_property(_actual_facets GLOBAL
      PROPERTY BIOSEQ_SQL_REGISTERED_${_facet_type})
    list(SORT _expected_facets)
    list(SORT _actual_facets)
    if(NOT "${_actual_facets}" STREQUAL "${_expected_facets}")
      _bioseq_sql_registry_fail(
        "${_facet_type} output keys do not match registered ownership; "
        "registered='${_actual_facets}', expected='${_expected_facets}'")
    endif()
  endforeach()

  get_property(_registered_sources GLOBAL
    PROPERTY BIOSEQ_SQL_REGISTERED_SOURCES)
  get_property(_audit_roots GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_AUDIT_ROOTS)
  foreach(_audit_root IN LISTS _audit_roots)
    file(GLOB_RECURSE _audit_candidates CONFIGURE_DEPENDS
      "${_audit_root}/*.cppm")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      ${_audit_candidates})
    foreach(_candidate IN LISTS _audit_candidates)
      file(READ "${_candidate}" _candidate_content)
      string(FIND "${_candidate_content}" "bioseq_sql::" _annotation_index)
      if(NOT _annotation_index EQUAL -1)
        file(REAL_PATH "${_candidate}" _candidate_real)
        list(FIND _registered_sources "${_candidate_real}" _registered_index)
        if(_registered_index EQUAL -1)
          _bioseq_sql_registry_fail(
            "annotated source is not explicitly registered: "
            "${_candidate_real}")
        endif()
      endif()
    endforeach()
  endforeach()

  get_property(_profiles GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_PROFILES)
  foreach(_profile IN LISTS _profiles)
    string(TOUPPER "${_profile}" _profile_key)
    foreach(_stage IN ITEMS SUPPORT CORE FINAL)
      get_property(_stage_sources GLOBAL
        PROPERTY BIOSEQ_SQL_${_profile_key}_${_stage}_SOURCES)
      if(NOT _stage_sources)
        _bioseq_sql_registry_fail(
          "${_profile}/${_stage} registry is empty")
      endif()
      list(SORT _stage_sources)
      list(REMOVE_DUPLICATES _stage_sources)
      string(TOLOWER "${_stage}" _stage_name)
      set(_registry_file
        "${_output_dir}/owners-${_profile}-${_stage_name}.txt")
      _bioseq_sql_write_registry_file(
        "${_registry_file}" ${_stage_sources})
      set(BIOSEQ_SQL_REGISTRY_${_profile_key}_${_stage}_FILE
        "${_registry_file}" PARENT_SCOPE)
      set(BIOSEQ_SQL_REGISTRY_${_profile_key}_${_stage}_SOURCES
        "${_stage_sources}" PARENT_SCOPE)
      set(_profile_${_stage}_sources "${_stage_sources}")
    endforeach()

    if(_profile STREQUAL "production")
      set(_production_final_sources ${_profile_FINAL_sources})
      set(_production_support_sources ${_profile_SUPPORT_sources})
      set(_production_core_sources ${_profile_CORE_sources})
    elseif(_profile STREQUAL "test_indexes")
      set(_test_indexes_final_sources ${_profile_FINAL_sources})
      set(_test_indexes_support_sources ${_profile_SUPPORT_sources})
      set(_test_indexes_core_sources ${_profile_CORE_sources})
    endif()
  endforeach()

  foreach(_production_source IN LISTS _production_final_sources)
    list(FIND _test_indexes_final_sources
      "${_production_source}" _test_indexes_index)
    if(_test_indexes_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "production owner is absent from test_indexes closure: "
        "${_production_source}")
    endif()
  endforeach()
  foreach(_production_source IN LISTS _production_support_sources)
    list(FIND _test_indexes_support_sources
      "${_production_source}" _test_indexes_index)
    if(_test_indexes_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "production support owner is absent from test_indexes closure: "
        "${_production_source}")
    endif()
  endforeach()
  if(NOT "${_production_core_sources}" STREQUAL
      "${_test_indexes_core_sources}")
    _bioseq_sql_registry_fail(
      "CORE registry must be profile-neutral; production='"
      "${_production_core_sources}', test_indexes='"
      "${_test_indexes_core_sources}'")
  endif()

  set(_core_registry_file "${_output_dir}/owners-core.txt")
  _bioseq_sql_write_registry_file(
    "${_core_registry_file}" ${_production_core_sources})
  set(BIOSEQ_SQL_REGISTRY_CORE_FILE
    "${_core_registry_file}" PARENT_SCOPE)
  set(BIOSEQ_SQL_REGISTRY_CORE_SOURCES
    "${_production_core_sources}" PARENT_SCOPE)

  get_property(_registry_records GLOBAL
    PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_RECORDS)
  list(SORT _registry_records)
  string(JOIN "\n" _registry_record_content ${_registry_records})
  set(_metadata_file "${_output_dir}/owners.tsv")
  string(CONCAT _metadata_content
    "owner_key\tmodule\tprofiles\tauthorities\tfacets\tsource\n"
    "${_registry_record_content}\n")
  if(EXISTS "${_metadata_file}")
    file(READ "${_metadata_file}" _existing_metadata)
  else()
    set(_existing_metadata)
  endif()
  if(NOT _existing_metadata STREQUAL _metadata_content)
    file(WRITE "${_metadata_file}" "${_metadata_content}")
  endif()
  set(BIOSEQ_SQL_OWNER_REGISTRY_METADATA
    "${_metadata_file}" PARENT_SCOPE)

  set_property(GLOBAL PROPERTY BIOSEQ_SQL_OWNER_REGISTRY_FINALIZED TRUE)
endfunction()
