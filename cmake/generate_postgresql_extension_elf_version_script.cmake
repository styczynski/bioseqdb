# Generate the exact dynamic-export surface for one PostgreSQL extension.
# The generated SQL manifest is the single fmgr ownership authority.  ELF is
# the direct-call default; the Mach-O wrapper sets
# BIOSEQ_POSTGRESQL_EXTENSION_EXPORT_FORMAT=MACHO and reuses the same strict
# manifest validation before rendering Darwin's leading-underscore names.
if(NOT DEFINED BIOSEQ_POSTGRESQL_EXTENSION_EXPORT_FORMAT)
  set(BIOSEQ_POSTGRESQL_EXTENSION_EXPORT_FORMAT ELF)
endif()
set(_bioseq_supported_export_formats ELF MACHO)
list(FIND
  _bioseq_supported_export_formats
  "${BIOSEQ_POSTGRESQL_EXTENSION_EXPORT_FORMAT}"
  _bioseq_export_format_index)
if(_bioseq_export_format_index EQUAL -1)
  message(FATAL_ERROR
    "PostgreSQL extension export map format is invalid: "
    "${BIOSEQ_POSTGRESQL_EXTENSION_EXPORT_FORMAT}")
endif()
if(NOT DEFINED INPUT_MANIFEST OR NOT IS_ABSOLUTE "${INPUT_MANIFEST}")
  message(FATAL_ERROR
    "PostgreSQL extension export map requires an absolute INPUT_MANIFEST")
endif()
if(NOT DEFINED OUTPUT OR NOT IS_ABSOLUTE "${OUTPUT}")
  message(FATAL_ERROR
    "PostgreSQL extension export map requires an absolute OUTPUT")
endif()
if(NOT EXISTS "${INPUT_MANIFEST}")
  message(FATAL_ERROR
    "PostgreSQL extension export map manifest is missing: ${INPUT_MANIFEST}")
endif()

file(READ "${INPUT_MANIFEST}" _bioseq_manifest_json)
set(_bioseq_manifest_fields
  schema_revision
  authority_mode
  compiler_profile
  clang_version
  compilation_signature
  extension_name
  extension_schema
  generated_module_prefix
  fmgr_boundary_module
  script_sha256
  statement_count
  statements
  entity_count
  public_entity_count
  public_stable_identities
  public_installed_identities
  routine_entity_count
  c_routine_identity_count
  c_link_symbol_inventory_count
  c_finfo_symbol_inventory_count
  c_wrapper_entity_count
  generated_wrapper_symbol_count
  handwritten_fmgr_export_symbol_count
  handwritten_fmgr_export_external_symbol_count
  handwritten_fmgr_export_owner_count
  c_symbol_sharing_group_count
  generated_cpp_profile_invariant
  entity_handle_count
  entity_handle_module_count
  entity_handle_modules
  row_codec_count
  row_codec_module_count
  row_codec_modules
  routine_boundary_contract_count
  routine_boundary_contracts
  result_shape_contract_count
  result_shape_contracts
  row_codec_contract_count
  row_codec_contracts
  c_routine_identities
  c_link_symbols
  c_finfo_symbols
  generated_wrapper_symbols
  handwritten_fmgr_export_symbols
  handwritten_fmgr_export_external_symbols
  fmgr_entrypoint_module_count
  fmgr_entrypoint_modules
  handwritten_fmgr_export_owners
  c_symbol_owners
  entity_counts_by_kind
  entities
  guc_generation)
string(JSON _bioseq_manifest_root_type
  ERROR_VARIABLE _bioseq_manifest_root_type_error
  TYPE "${_bioseq_manifest_json}")
string(JSON _bioseq_manifest_field_count
  ERROR_VARIABLE _bioseq_manifest_field_count_error
  LENGTH "${_bioseq_manifest_json}")
if(_bioseq_manifest_root_type_error
    OR NOT _bioseq_manifest_root_type STREQUAL "OBJECT"
    OR _bioseq_manifest_field_count_error)
  message(FATAL_ERROR
    "PostgreSQL extension export map manifest root is invalid")
endif()
if(_bioseq_manifest_field_count GREATER 0)
  math(EXPR _bioseq_manifest_last_field
    "${_bioseq_manifest_field_count} - 1")
  foreach(_bioseq_manifest_field_index
      RANGE ${_bioseq_manifest_last_field})
    string(JSON _bioseq_manifest_field
      ERROR_VARIABLE _bioseq_manifest_field_error
      MEMBER "${_bioseq_manifest_json}"
      ${_bioseq_manifest_field_index})
    list(FIND _bioseq_manifest_fields
      "${_bioseq_manifest_field}"
      _bioseq_manifest_field_known)
    if(_bioseq_manifest_field_error
        OR _bioseq_manifest_field_known EQUAL -1)
      message(FATAL_ERROR
        "PostgreSQL extension export map manifest has an unknown field")
    endif()
  endforeach()
endif()
string(JSON _bioseq_schema_revision_type
  ERROR_VARIABLE _bioseq_revision_type_error
  TYPE "${_bioseq_manifest_json}" schema_revision)
string(JSON _bioseq_schema_revision
  ERROR_VARIABLE _bioseq_revision_error
  GET "${_bioseq_manifest_json}" schema_revision)
if(_bioseq_revision_type_error
    OR NOT _bioseq_schema_revision_type STREQUAL "NUMBER"
    OR _bioseq_revision_error
    OR NOT _bioseq_schema_revision MATCHES "^[0-9]+$"
    OR NOT _bioseq_schema_revision EQUAL 1)
  message(FATAL_ERROR
    "PostgreSQL extension export map schema_revision is invalid")
endif()

function(_bioseq_manifest_symbol_array FIELD COUNT_FIELD ALLOW_EMPTY OUT)
  string(JSON _bioseq_array_type ERROR_VARIABLE _bioseq_array_type_error
    TYPE "${_bioseq_manifest_json}" "${FIELD}")
  string(JSON _bioseq_count ERROR_VARIABLE _bioseq_count_error
    LENGTH "${_bioseq_manifest_json}" "${FIELD}")
  if(_bioseq_array_type_error OR NOT _bioseq_array_type STREQUAL "ARRAY"
      OR _bioseq_count_error OR (_bioseq_count EQUAL 0 AND NOT ALLOW_EMPTY))
    message(FATAL_ERROR
      "PostgreSQL extension export map manifest ${FIELD} is invalid or empty")
  endif()
  string(JSON _bioseq_declared_type ERROR_VARIABLE _bioseq_declared_type_error
    TYPE "${_bioseq_manifest_json}" "${COUNT_FIELD}")
  string(JSON _bioseq_declared_count ERROR_VARIABLE _bioseq_declared_error
    GET "${_bioseq_manifest_json}" "${COUNT_FIELD}")
  if(_bioseq_declared_type_error OR NOT _bioseq_declared_type STREQUAL "NUMBER"
      OR _bioseq_declared_error
      OR NOT _bioseq_declared_count MATCHES "^[0-9]+$"
      OR NOT _bioseq_declared_count EQUAL _bioseq_count)
    message(FATAL_ERROR
      "PostgreSQL extension export map manifest ${COUNT_FIELD} disagrees with ${FIELD}")
  endif()
  set(_bioseq_values)
  if(_bioseq_count GREATER 0)
    math(EXPR _bioseq_last "${_bioseq_count} - 1")
    foreach(_bioseq_index RANGE ${_bioseq_last})
      string(JSON _bioseq_symbol_type ERROR_VARIABLE _bioseq_symbol_type_error
        TYPE "${_bioseq_manifest_json}" "${FIELD}" ${_bioseq_index})
      string(JSON _bioseq_symbol ERROR_VARIABLE _bioseq_symbol_error
        GET "${_bioseq_manifest_json}" "${FIELD}" ${_bioseq_index})
      if(_bioseq_symbol_type_error OR NOT _bioseq_symbol_type STREQUAL "STRING"
          OR _bioseq_symbol_error
          OR NOT _bioseq_symbol MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
          "PostgreSQL extension export map manifest ${FIELD}[${_bioseq_index}] is invalid")
      endif()
      list(FIND _bioseq_values "${_bioseq_symbol}" _bioseq_duplicate)
      if(NOT _bioseq_duplicate EQUAL -1)
        message(FATAL_ERROR
          "PostgreSQL extension export map manifest ${FIELD} has duplicate ${_bioseq_symbol}")
      endif()
      list(APPEND _bioseq_values "${_bioseq_symbol}")
    endforeach()
  endif()
  set(${OUT} "${_bioseq_values}" PARENT_SCOPE)
endfunction()

_bioseq_manifest_symbol_array(c_link_symbols c_link_symbol_inventory_count FALSE _bioseq_links)
_bioseq_manifest_symbol_array(c_finfo_symbols c_finfo_symbol_inventory_count FALSE _bioseq_finfos)
_bioseq_manifest_symbol_array(
  handwritten_fmgr_export_external_symbols
  handwritten_fmgr_export_external_symbol_count
  TRUE
  _bioseq_external)
list(LENGTH _bioseq_links _bioseq_link_count)
list(LENGTH _bioseq_finfos _bioseq_finfo_count)
if(NOT _bioseq_link_count EQUAL _bioseq_finfo_count)
  message(FATAL_ERROR
    "PostgreSQL extension export map manifest fmgr/finfo inventory counts differ")
endif()

foreach(_bioseq_symbol IN LISTS _bioseq_links)
  list(FIND _bioseq_finfos "pg_finfo_${_bioseq_symbol}" _bioseq_finfo_index)
  if(_bioseq_finfo_index EQUAL -1)
    message(FATAL_ERROR
      "PostgreSQL extension export map manifest lacks pg_finfo_${_bioseq_symbol}")
  endif()
endforeach()
foreach(_bioseq_symbol IN LISTS _bioseq_external)
  list(FIND _bioseq_links "${_bioseq_symbol}" _bioseq_overlap)
  if(NOT _bioseq_overlap EQUAL -1)
    message(FATAL_ERROR
      "PostgreSQL extension export map generated/handwritten symbol ownership overlaps")
  endif()
endforeach()

string(JSON _bioseq_owner_array_type ERROR_VARIABLE _bioseq_owner_array_type_error
  TYPE "${_bioseq_manifest_json}" c_symbol_owners)
string(JSON _bioseq_owner_count ERROR_VARIABLE _bioseq_owner_error
  LENGTH "${_bioseq_manifest_json}" c_symbol_owners)
if(_bioseq_owner_array_type_error OR NOT _bioseq_owner_array_type STREQUAL "ARRAY"
    OR _bioseq_owner_error OR NOT _bioseq_owner_count EQUAL _bioseq_link_count)
  message(FATAL_ERROR
    "PostgreSQL extension export map manifest c_symbol_owners does not close over c_link_symbols")
endif()
set(_bioseq_owned)
if(_bioseq_owner_count GREATER 0)
  math(EXPR _bioseq_last_owner "${_bioseq_owner_count} - 1")
  foreach(_bioseq_index RANGE ${_bioseq_last_owner})
    string(JSON _bioseq_owner_type ERROR_VARIABLE _bioseq_owner_type_error
      TYPE "${_bioseq_manifest_json}" c_symbol_owners ${_bioseq_index})
    string(JSON _bioseq_symbol_type ERROR_VARIABLE _bioseq_symbol_type_error
      TYPE "${_bioseq_manifest_json}" c_symbol_owners ${_bioseq_index} symbol)
    string(JSON _bioseq_symbol ERROR_VARIABLE _bioseq_owner_error
      GET "${_bioseq_manifest_json}" c_symbol_owners ${_bioseq_index} symbol)
    string(JSON _bioseq_finfo_type ERROR_VARIABLE _bioseq_finfo_type_error
      TYPE "${_bioseq_manifest_json}" c_symbol_owners ${_bioseq_index} finfo_symbol)
    string(JSON _bioseq_finfo ERROR_VARIABLE _bioseq_finfo_error
      GET "${_bioseq_manifest_json}" c_symbol_owners ${_bioseq_index} finfo_symbol)
    list(FIND _bioseq_links "${_bioseq_symbol}" _bioseq_link_index)
    list(FIND _bioseq_owned "${_bioseq_symbol}" _bioseq_duplicate)
    if(_bioseq_owner_type_error OR NOT _bioseq_owner_type STREQUAL "OBJECT"
        OR _bioseq_symbol_type_error OR NOT _bioseq_symbol_type STREQUAL "STRING"
        OR _bioseq_finfo_type_error OR NOT _bioseq_finfo_type STREQUAL "STRING"
        OR _bioseq_owner_error OR _bioseq_finfo_error OR _bioseq_link_index EQUAL -1
        OR NOT _bioseq_finfo STREQUAL "pg_finfo_${_bioseq_symbol}"
        OR NOT _bioseq_duplicate EQUAL -1)
      message(FATAL_ERROR
        "PostgreSQL extension export map manifest c_symbol_owners is inconsistent")
    endif()
    list(APPEND _bioseq_owned "${_bioseq_symbol}")
  endforeach()
endif()

set(_bioseq_exports ${_bioseq_links} ${_bioseq_finfos})
foreach(_bioseq_symbol IN LISTS _bioseq_external)
  list(APPEND _bioseq_exports "${_bioseq_symbol}" "pg_finfo_${_bioseq_symbol}")
endforeach()
if(DEFINED ABI_EXPORTS_PIPE AND NOT ABI_EXPORTS_PIPE STREQUAL "")
  string(REPLACE "|" ";" _bioseq_abi_exports "${ABI_EXPORTS_PIPE}")
  set(_bioseq_seen_abi_exports)
  foreach(_bioseq_symbol IN LISTS _bioseq_abi_exports)
    if(NOT _bioseq_symbol MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
      message(FATAL_ERROR
        "PostgreSQL extension export map has invalid reviewed ABI symbol: ${_bioseq_symbol}")
    endif()
    list(FIND _bioseq_seen_abi_exports "${_bioseq_symbol}" _bioseq_abi_duplicate)
    if(NOT _bioseq_abi_duplicate EQUAL -1)
      message(FATAL_ERROR
        "PostgreSQL extension export map has duplicate reviewed ABI symbol: ${_bioseq_symbol}")
    endif()
    list(FIND _bioseq_exports "${_bioseq_symbol}" _bioseq_fmgr_overlap)
    if(NOT _bioseq_fmgr_overlap EQUAL -1)
      message(FATAL_ERROR
        "PostgreSQL extension export map reviewed ABI symbol overlaps fmgr export: ${_bioseq_symbol}")
    endif()
    list(APPEND _bioseq_seen_abi_exports "${_bioseq_symbol}")
    list(APPEND _bioseq_exports "${_bioseq_symbol}")
  endforeach()
endif()
list(REMOVE_DUPLICATES _bioseq_exports)
list(SORT _bioseq_exports)
set(_bioseq_map "")
if(BIOSEQ_POSTGRESQL_EXTENSION_EXPORT_FORMAT STREQUAL "MACHO")
  foreach(_bioseq_symbol IN LISTS _bioseq_exports)
    # Mach-O prefixes every C linkage name with one underscore.  Symbols that
    # already begin with an underscore (for example _PG_init) consequently
    # receive the required double-underscore spelling in the linker list.
    string(APPEND _bioseq_map "_${_bioseq_symbol}\n")
  endforeach()
else()
  string(APPEND _bioseq_map "{\n  global:\n")
  foreach(_bioseq_symbol IN LISTS _bioseq_exports)
    string(APPEND _bioseq_map "    ${_bioseq_symbol};\n")
  endforeach()
  string(APPEND _bioseq_map "  local:\n    *;\n};\n")
endif()
set(_bioseq_temporary "${OUTPUT}.tmp")
file(WRITE "${_bioseq_temporary}" "${_bioseq_map}")
file(RENAME "${_bioseq_temporary}" "${OUTPUT}")
