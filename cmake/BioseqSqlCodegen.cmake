include_guard(GLOBAL)

# SQL generation is driven only by owners registered through this file. The
# registry is intentionally strict: a source cannot be inferred from a build
# target, shared by two owners, or silently added to only one profile.

function(_bioseq_sql_registry_fail message_text)
  string(JOIN "" _full_message "${message_text}" ${ARGN})
  message(FATAL_ERROR
    "SQL owner registry: ${_full_message}")
endfunction()

# Instantiates the owner-derived FINAL collection/render/validation pipeline for
# a finalized extension context.  SUPPORT and CORE remain owned by contexts
# that registered those facets; a FINAL/fmgr-only sibling extension therefore
# does not fabricate Bioseq-only stages.
function(bioseq_add_sql_codegen_context)
  cmake_parse_arguments(
    ARG
    "REQUIRE_COMPLETE_CODEGEN"
    "EXTENSION;EXTENSION_NAME;EXTENSION_SCHEMA;GENERATED_MODULE_PREFIX;FMGR_BOUNDARY_MODULE;FMGR_PRELUDE;OUTPUT_DIR;GENERATED_CPP_DIR;VERSION;RAW_COMPILE_DATABASE"
    "PROFILES"
    ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_sql_registry_fail(
      "bioseq_add_sql_codegen_context received unknown arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  foreach(_required IN ITEMS
      EXTENSION EXTENSION_NAME EXTENSION_SCHEMA GENERATED_MODULE_PREFIX
      FMGR_BOUNDARY_MODULE OUTPUT_DIR GENERATED_CPP_DIR VERSION)
    if(NOT ARG_${_required})
      _bioseq_sql_registry_fail(
        "bioseq_add_sql_codegen_context requires ${_required}")
    endif()
  endforeach()
  _bioseq_sql_context(_context_key _context_prefix "${ARG_EXTENSION}")
  get_property(_finalized GLOBAL
    PROPERTY "${_context_prefix}_FINALIZED")
  if(NOT _finalized)
    _bioseq_sql_registry_fail(
      "extension context '${ARG_EXTENSION}' must be finalized before its "
      "generation targets are added")
  endif()
  if(NOT TARGET bioseq_codegen)
    _bioseq_sql_registry_fail(
      "the shared bioseq_codegen target must exist before adding extension "
      "generation context '${ARG_EXTENSION}'")
  endif()
  if(NOT ARG_PROFILES)
    get_property(ARG_PROFILES GLOBAL
      PROPERTY "${_context_prefix}_PROFILES")
  endif()
  if(NOT ARG_RAW_COMPILE_DATABASE)
    set(ARG_RAW_COMPILE_DATABASE
      "${CMAKE_BINARY_DIR}/compile_commands.json")
  endif()
  if(NOT ARG_FMGR_PRELUDE)
    set(ARG_FMGR_PRELUDE bioseq_common)
  endif()
  if(NOT ARG_FMGR_PRELUDE STREQUAL "bioseq_common"
      AND NOT ARG_FMGR_PRELUDE STREQUAL "postgresql")
    _bioseq_sql_registry_fail(
      "invalid FMGR_PRELUDE '${ARG_FMGR_PRELUDE}'; expected "
      "bioseq_common or postgresql")
  endif()
  set(_codegen_dependencies)
  if(TARGET bioseq_codegen_project)
    list(APPEND _codegen_dependencies bioseq_codegen_project)
  endif()
  foreach(_tool_path IN ITEMS
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}")
    if(_tool_path)
      list(APPEND _codegen_dependencies "${_tool_path}")
    endif()
  endforeach()

  set(_identity_arguments
    --extension-name "${ARG_EXTENSION_NAME}"
    --extension-schema "${ARG_EXTENSION_SCHEMA}"
    --generated-module-prefix "${ARG_GENERATED_MODULE_PREFIX}"
    --fmgr-boundary-module "${ARG_FMGR_BOUNDARY_MODULE}"
    --fmgr-prelude "${ARG_FMGR_PRELUDE}")
  set(_completion_arguments)
  if(ARG_REQUIRE_COMPLETE_CODEGEN)
    list(APPEND _completion_arguments --require-complete-codegen)
  endif()

  foreach(_profile IN LISTS ARG_PROFILES)
    _bioseq_sql_profile_key(_profile_key "${_profile}")
    get_property(_fmgr_owner_keys GLOBAL
      PROPERTY
        "${_context_prefix}_${_profile_key}_FMGR_ENTRYPOINTS")
    get_property(_fmgr_owner_records GLOBAL
      PROPERTY
        "${_context_prefix}_${_profile_key}_FMGR_OWNER_RECORDS")
    list(SORT _fmgr_owner_keys)
    list(SORT _fmgr_owner_records)
    set(_registry_file_variable
      "${_context_prefix}_REGISTRY_${_profile_key}_FINAL_FILE")
    set(_registry_sources_variable
      "${_context_prefix}_REGISTRY_${_profile_key}_FINAL_SOURCES")
    set(_registry_file "${${_registry_file_variable}}")
    set(_registry_sources ${${_registry_sources_variable}})
    if(NOT _registry_file OR NOT _registry_sources)
      _bioseq_sql_registry_fail(
        "${ARG_EXTENSION}/${_profile} has no finalized FINAL registry")
    endif()

    set(_profile_sql_dir
      "${ARG_OUTPUT_DIR}/profiles/${_profile}")
    set(_profile_cpp_dir
      "${ARG_GENERATED_CPP_DIR}/${_profile}")
    set(_profile_clang_dir
      "${ARG_OUTPUT_DIR}/clang/${_profile}")
    set(_pcm_root "${_profile_clang_dir}/pcm")
    set(_compile_database
      "${_profile_clang_dir}/compile_commands.json")
    set(_pcm_manifest "${_profile_clang_dir}/pcm-manifest.json")
    set(_prepare_stamp "${_profile_clang_dir}/collect.stamp")
    set(_prepare_depfile "${_profile_clang_dir}/collect.d")
    set(_generated_install
      "${_profile_sql_dir}/${ARG_EXTENSION_NAME}--${ARG_VERSION}.sql")
    set(_generated_manifest
      "${_profile_sql_dir}/${ARG_EXTENSION_NAME}.manifest.json")
    set(_render_stamp "${_profile_sql_dir}/render.stamp")
    set(_validate_stamp "${_profile_sql_dir}/validate.stamp")
    set(_fmgr_arguments)
    set(_fmgr_outputs)
    foreach(_owner_key IN LISTS _fmgr_owner_keys)
      set(_owner_output
        "${_profile_cpp_dir}/${_owner_key}.entrypoints.cppm")
      list(APPEND _fmgr_arguments
        --fmgr-entrypoints-output "${_owner_key}=${_owner_output}")
      list(APPEND _fmgr_outputs "${_owner_output}")
      string(TOUPPER "${_owner_key}" _owner_variable_key)
      set(
        "${_context_prefix}_${_profile_key}_FMGR_ENTRYPOINTS_${_owner_variable_key}"
        "${_owner_output}" PARENT_SCOPE)
    endforeach()
    foreach(_owner_record IN LISTS _fmgr_owner_records)
      list(APPEND _fmgr_arguments
        --fmgr-owner-module "${_owner_record}")
    endforeach()

    add_custom_command(
      OUTPUT "${_prepare_stamp}"
      BYPRODUCTS "${_compile_database}" "${_pcm_manifest}"
      COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_pcm_root}"
      COMMAND
        "$<TARGET_FILE:bioseq_codegen>" sql prepare
        --authority final
        --profile "${_profile}"
        --source-list "${_registry_file}"
        --raw-compile-database "${ARG_RAW_COMPILE_DATABASE}"
        --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
        --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
        --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
        --compiler-module-inventory
          "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
        --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
        --clang-scan-deps-sha256
          "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
        --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
        --compiler-module-inventory-sha256
          "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
        --compile-database-output "${_compile_database}"
        --pcm-root "${_pcm_root}"
        --pcm-manifest "${_pcm_manifest}"
        --depfile "${_prepare_depfile}"
        --stamp "${_prepare_stamp}"
        ${_identity_arguments}
      DEPENDS
        ${_codegen_dependencies}
        "${ARG_RAW_COMPILE_DATABASE}"
        "${_registry_file}"
        ${_registry_sources}
      DEPFILE "${_prepare_depfile}"
      COMMENT
        "Collecting ${ARG_EXTENSION}/${_profile} annotated SQL owners"
      VERBATIM)
    add_custom_target(
      "bioseq_sql_${ARG_EXTENSION}_${_profile}_collect"
      DEPENDS "${_prepare_stamp}")

    add_custom_command(
      OUTPUT
        "${_generated_install}"
        "${_generated_manifest}"
        "${_render_stamp}"
      BYPRODUCTS ${_fmgr_outputs}
      COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${_profile_sql_dir}" "${_profile_cpp_dir}"
      COMMAND "${CMAKE_COMMAND}" -E env
        "BIOSEQ_REVIEWED_REPOSITORY_ROOT=${BIOSEQ_REPOSITORY_ROOT}"
        "$<TARGET_FILE:bioseq_codegen>" sql generate
        --stage final
        --profile "${_profile}"
        --source-list "${_registry_file}"
        --compile-database "${_compile_database}"
        --pcm-root "${_pcm_root}"
        --pcm-manifest "${_pcm_manifest}"
        --clang-binary "${BIOSEQ_CODEGEN_CLANG_BINARY}"
        --clang-scan-deps "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
        --llvm-config "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
        --compiler-module-inventory
          "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
        --clang-sha256 "${BIOSEQ_CODEGEN_CLANG_SHA256}"
        --clang-scan-deps-sha256
          "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256}"
        --llvm-config-sha256 "${BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256}"
        --compiler-module-inventory-sha256
          "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256}"
        --output "${_generated_install}"
        --manifest "${_generated_manifest}"
        --stamp "${_render_stamp}"
        ${_fmgr_arguments}
        ${_completion_arguments}
        ${_identity_arguments}
      DEPENDS
        ${_codegen_dependencies}
        "${_prepare_stamp}"
        "${_compile_database}"
        "${_pcm_manifest}"
        "${_registry_file}"
        ${_registry_sources}
      COMMENT
        "Rendering ${ARG_EXTENSION}/${_profile} PostgreSQL extension SQL"
      VERBATIM)
    add_custom_target(
      "bioseq_sql_${ARG_EXTENSION}_${_profile}_render"
      DEPENDS
        "${_generated_install}"
        "${_generated_manifest}"
        "${_render_stamp}"
        ${_fmgr_outputs})
    add_dependencies(
      "bioseq_sql_${ARG_EXTENSION}_${_profile}_render"
      "bioseq_sql_${ARG_EXTENSION}_${_profile}_collect")

    add_custom_command(
      OUTPUT "${_validate_stamp}"
      COMMAND "$<TARGET_FILE:bioseq_codegen>" self_test sql
      COMMAND "${CMAKE_COMMAND}" -E touch "${_validate_stamp}"
      DEPENDS
        ${_codegen_dependencies}
        "${_generated_install}"
        "${_generated_manifest}"
        "${_render_stamp}"
        ${_fmgr_outputs}
      COMMENT
        "Validating ${ARG_EXTENSION}/${_profile} generated SQL artifacts"
      VERBATIM)
    add_custom_target(
      "bioseq_sql_${ARG_EXTENSION}_${_profile}_validate"
      DEPENDS "${_validate_stamp}")
    add_dependencies(
      "bioseq_sql_${ARG_EXTENSION}_${_profile}_validate"
      "bioseq_sql_${ARG_EXTENSION}_${_profile}_render")
    add_custom_target(
      "bioseq_sql_${ARG_EXTENSION}_${_profile}")
    add_dependencies(
      "bioseq_sql_${ARG_EXTENSION}_${_profile}"
      "bioseq_sql_${ARG_EXTENSION}_${_profile}_validate")

    set("${_context_prefix}_${_profile_key}_GENERATED_INSTALL"
      "${_generated_install}" PARENT_SCOPE)
    set("${_context_prefix}_${_profile_key}_GENERATED_MANIFEST"
      "${_generated_manifest}" PARENT_SCOPE)
    set("${_context_prefix}_${_profile_key}_GENERATED_MODULES"
      "${_fmgr_outputs}" PARENT_SCOPE)
    set("${_context_prefix}_${_profile_key}_TARGET"
      "bioseq_sql_${ARG_EXTENSION}_${_profile}" PARENT_SCOPE)
  endforeach()
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

function(_bioseq_sql_context output_key output_prefix extension)
  if(NOT extension)
    get_property(extension GLOBAL PROPERTY BIOSEQ_SQL_ACTIVE_CONTEXT)
  endif()
  if(NOT extension)
    # Preserve the existing Bioseq configure graph while callers move to the
    # explicit EXTENSION form.  A second context is never inferred.
    set(extension bioseq)
  endif()
  if(NOT extension MATCHES "^[a-z][a-z0-9_]*$")
    _bioseq_sql_registry_fail(
      "invalid extension context '${extension}'; expected "
      "[a-z][a-z0-9_]*")
  endif()
  string(TOUPPER "${extension}" _context_key)
  set(${output_key} "${_context_key}" PARENT_SCOPE)
  set(${output_prefix} "BIOSEQ_SQL_${_context_key}" PARENT_SCOPE)
endfunction()

function(_bioseq_sql_profile_key output profile)
  if(NOT profile MATCHES "^[a-z][a-z0-9_]*$")
    _bioseq_sql_registry_fail(
      "invalid SQL generation profile '${profile}'; expected "
      "[a-z][a-z0-9_]*")
  endif()
  string(TOUPPER "${profile}" _profile_key)
  set(${output} "${_profile_key}" PARENT_SCOPE)
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

function(_bioseq_sql_dependency_only_support_source
    output_variable normalized_source)
  set(_authorized FALSE)
  if(normalized_source MATCHES
      "(^|/)([^/]*_sql_rows|sql_rows|rows|[^/]*_sql_guc_settings|sql_guc_settings)(\\.cppm|/index\\.cppm)$")
    set(_authorized TRUE)
  endif()
  set(${output_variable} "${_authorized}" PARENT_SCOPE)
endfunction()

function(_bioseq_sql_reviewed_handwritten_fmgr_contract_authorized
    output_variable)
  cmake_parse_arguments(
    PARSE_ARGV 1 ARG
    ""
    "OWNER_KEY;MODULE;SOURCE;PROFILES;AUTHORITIES;SYMBOL"
    "")
  set(_authorized TRUE)
  if(ARG_SYMBOL)
    set(_authorized FALSE)
    if(ARG_OWNER_KEY STREQUAL "external_relation_fmgr_export"
        AND ARG_MODULE STREQUAL "bioseq.postgres.external.relation"
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

function(_bioseq_sql_run_dependency_only_support_contracts)
  foreach(_source IN ITEMS
      "/fixture/cache_sql_rows.cppm"
      "/fixture/sql_rows.cppm"
      "/fixture/rows.cppm"
      "/fixture/cache_sql_guc_settings.cppm"
      "/fixture/sql_guc_settings.cppm"
      "/fixture/cache_sql_rows/index.cppm"
      "/fixture/sql_rows/index.cppm"
      "/fixture/rows/index.cppm"
      "/fixture/cache_sql_guc_settings/index.cppm"
      "/fixture/sql_guc_settings/index.cppm")
    _bioseq_sql_dependency_only_support_source(
      _authorized_support_source "${_source}")
    if(NOT _authorized_support_source)
      _bioseq_sql_registry_fail(
        "dependency-only SUPPORT contract rejected: ${_source}")
    endif()
  endforeach()

  foreach(_source IN ITEMS
      "/fixture/index.cppm"
      "/fixture/cache_rows/index.cppm"
      "/fixture/row/index.cppm"
      "/fixture/rows/not_index.cppm"
      "/fixture/sql_rows/nested/index.cppm")
    _bioseq_sql_dependency_only_support_source(
      _authorized_support_source "${_source}")
    if(_authorized_support_source)
      _bioseq_sql_registry_fail(
        "dependency-only SUPPORT contract accepted: ${_source}")
    endif()
  endforeach()
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
    _explicit_reviewed_handwritten_fmgr_export
    "/fixture/external/relation.cppm" -1 TRUE)
  if(NOT _explicit_reviewed_handwritten_fmgr_export)
    _bioseq_sql_registry_fail(
      "annotation authority contract rejected an explicit reviewed "
      "handwritten FMGR export facet")
  endif()

  _bioseq_sql_annotation_authorized(
    _annotated_owner
    "/fixture/ordinary.cppm" 0 FALSE)
  if(NOT _annotated_owner)
    _bioseq_sql_registry_fail(
      "annotation authority contract rejected an annotated owner")
  endif()

  _bioseq_sql_reviewed_handwritten_fmgr_contract_authorized(
    _natural_external_rows
    OWNER_KEY external_sql_catalog
    MODULE bioseq.postgres.external.sql_catalog
    SOURCE backend/pg/external/sql_catalog.cppm
    PROFILES "production;test_indexes"
    AUTHORITIES FINAL)
  if(NOT _natural_external_rows)
    _bioseq_sql_registry_fail(
      "reviewed handwritten FMGR contract rejected a natural FINAL "
      "external SQL owner")
  endif()

  _bioseq_sql_reviewed_handwritten_fmgr_contract_authorized(
    _accidental_reviewed_handwritten_fmgr_export
    OWNER_KEY external_sql_catalog
    MODULE bioseq.postgres.external.sql_catalog
    SOURCE backend/pg/external/sql_catalog.cppm
    PROFILES "production;test_indexes"
    AUTHORITIES FINAL
    SYMBOL accidental_external_handler)
  if(_accidental_reviewed_handwritten_fmgr_export)
    _bioseq_sql_registry_fail(
      "reviewed handwritten FMGR contract accepted an accidental export")
  endif()

  _bioseq_sql_reviewed_handwritten_fmgr_contract_authorized(
    _reviewed_handwritten_fmgr_export
    OWNER_KEY external_relation_fmgr_export
    MODULE bioseq.postgres.external.relation
    SOURCE backend/pg/external/relation.cppm
    PROFILES "production;test_indexes"
    AUTHORITIES FINAL
    SYMBOL bioseq_external_handler)
  if(NOT _reviewed_handwritten_fmgr_export)
    _bioseq_sql_registry_fail(
      "reviewed handwritten FMGR contract rejected the external handler")
  endif()
endfunction()

_bioseq_sql_run_dependency_only_support_contracts()
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
    "EXTENSION;ROOT"
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

  _bioseq_sql_context(_context_key _context_prefix "${ARG_EXTENSION}")
  get_property(_registry_started GLOBAL
    PROPERTY "${_context_prefix}_INITIALIZED" SET)
  if(_registry_started)
    _bioseq_sql_registry_fail(
      "the ${_context_key} owner registry was initialized twice")
  endif()

  file(REAL_PATH "${ARG_ROOT}" _registry_root)
  if(NOT IS_DIRECTORY "${_registry_root}")
    _bioseq_sql_registry_fail(
      "registry root does not exist: ${ARG_ROOT}")
  endif()

  _bioseq_sql_require_unique_values("PROFILES" ${ARG_PROFILES})
  set(_profiles ${ARG_PROFILES})
  list(SORT _profiles)
  foreach(_profile IN LISTS _profiles)
    _bioseq_sql_profile_key(_unused_profile_key "${_profile}")
  endforeach()

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

  set_property(GLOBAL PROPERTY "${_context_prefix}_INITIALIZED" TRUE)
  set_property(GLOBAL PROPERTY "${_context_prefix}_FINALIZED" FALSE)
  set_property(GLOBAL PROPERTY "${_context_prefix}_ROOT"
    "${_registry_root}")
  set_property(GLOBAL PROPERTY "${_context_prefix}_PROFILES"
    "${_profiles}")
  set_property(GLOBAL PROPERTY "${_context_prefix}_AUDIT_ROOTS"
    "${_audit_roots}")
  set_property(GLOBAL PROPERTY "${_context_prefix}_OWNERS" "")
  set_property(GLOBAL PROPERTY BIOSEQ_SQL_ACTIVE_CONTEXT
    "${ARG_EXTENSION}")
  if(NOT ARG_EXTENSION)
    set_property(GLOBAL PROPERTY BIOSEQ_SQL_ACTIVE_CONTEXT bioseq)
  endif()
endfunction()

function(bioseq_register_sql_owner)
  cmake_parse_arguments(
    ARG
    ""
    "EXTENSION;OWNER_KEY;MODULE"
    "SOURCES;PROFILES;AUTHORITIES;ROW_CODECS;ENTITY_HANDLES;FMGR_ENTRYPOINTS;GUC_OWNERS;ANNOTATION_FREE_RESULT_ROWS;HANDWRITTEN_FMGR_EXPORTS"
    ${ARGN})
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_sql_registry_fail(
      "bioseq_register_sql_owner received unknown arguments for "
      "${ARG_OWNER_KEY}: ${ARG_UNPARSED_ARGUMENTS}")
  endif()

  _bioseq_sql_context(_context_key _context_prefix "${ARG_EXTENSION}")
  get_property(_registry_started GLOBAL
    PROPERTY "${_context_prefix}_INITIALIZED" SET)
  if(NOT _registry_started)
    _bioseq_sql_registry_fail(
      "bioseq_sql_begin_owner_registry(EXTENSION "
      "${ARG_EXTENSION}) must be called before registration")
  endif()
  get_property(_registry_finalized GLOBAL
    PROPERTY "${_context_prefix}_FINALIZED")
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
    PROPERTY "${_context_prefix}_OWNERS")
  list(FIND _registered_owner_keys "${ARG_OWNER_KEY}" _owner_key_index)
  if(NOT _owner_key_index EQUAL -1)
    _bioseq_sql_registry_fail(
      "duplicate owner key '${ARG_OWNER_KEY}'")
  endif()
  get_property(_registered_modules GLOBAL
    PROPERTY "${_context_prefix}_MODULES")
  list(FIND _registered_modules "${ARG_MODULE}" _module_index)
  if(NOT _module_index EQUAL -1)
    _bioseq_sql_registry_fail(
      "module '${ARG_MODULE}' is registered by more than one owner")
  endif()

  _bioseq_sql_require_unique_values(
    "profiles for ${ARG_OWNER_KEY}" ${ARG_PROFILES})
  get_property(_allowed_profiles GLOBAL
    PROPERTY "${_context_prefix}_PROFILES")
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
    if(NOT "${_core_profiles}" STREQUAL "${_allowed_profiles}")
      _bioseq_sql_registry_fail(
        "CORE owner ${ARG_OWNER_KEY} must be registered in every context "
        "profile; got '${_core_profiles}', expected '${_allowed_profiles}'")
    endif()
  endif()

  foreach(_facet_type IN ITEMS
      ROW_CODECS ENTITY_HANDLES FMGR_ENTRYPOINTS GUC_OWNERS
      ANNOTATION_FREE_RESULT_ROWS HANDWRITTEN_FMGR_EXPORTS)
    set(_facet_values ${ARG_${_facet_type}})
    _bioseq_sql_require_unique_values(
      "${_facet_type} facets for ${ARG_OWNER_KEY}" ${_facet_values})
    foreach(_facet_key IN LISTS _facet_values)
      if(NOT _facet_key MATCHES "^[a-z][a-z0-9_]*$")
        _bioseq_sql_registry_fail(
          "invalid ${_facet_type} key '${_facet_key}' on ${ARG_OWNER_KEY}")
      endif()
      get_property(_registered_facets GLOBAL
        PROPERTY "${_context_prefix}_${_facet_type}")
      list(FIND _registered_facets "${_facet_key}" _facet_index)
      if(NOT _facet_index EQUAL -1)
        _bioseq_sql_registry_fail(
          "${_facet_type} key '${_facet_key}' has multiple owners")
      endif()
      set_property(GLOBAL APPEND
        PROPERTY "${_context_prefix}_${_facet_type}"
        "${_facet_key}")
      foreach(_profile IN LISTS ARG_PROFILES)
        _bioseq_sql_profile_key(_profile_key "${_profile}")
        set_property(GLOBAL APPEND
          PROPERTY
            "${_context_prefix}_${_profile_key}_${_facet_type}"
          "${_facet_key}")
      endforeach()
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
  if(ARG_HANDWRITTEN_FMGR_EXPORTS)
    list(LENGTH ARG_HANDWRITTEN_FMGR_EXPORTS
      _reviewed_handwritten_fmgr_export_count)
    list(LENGTH ARG_SOURCES _reviewed_handwritten_fmgr_source_count)
    if(NOT _reviewed_handwritten_fmgr_export_count EQUAL 1
        OR NOT _reviewed_handwritten_fmgr_source_count EQUAL 1)
      _bioseq_sql_registry_fail(
        "reviewed handwritten FMGR export owner ${ARG_OWNER_KEY} must "
        "register "
        "exactly one "
        "reviewed symbol and one source")
    endif()
    if(_final_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "reviewed handwritten FMGR export owner ${ARG_OWNER_KEY} must have "
        "FINAL authority")
    endif()
    if(ARG_ROW_CODECS OR ARG_ENTITY_HANDLES OR ARG_FMGR_ENTRYPOINTS
        OR ARG_GUC_OWNERS OR ARG_ANNOTATION_FREE_RESULT_ROWS)
      _bioseq_sql_registry_fail(
        "reviewed handwritten FMGR export owner ${ARG_OWNER_KEY} cannot own "
        "generated SQL facets")
    endif()
    list(GET ARG_SOURCES 0 _reviewed_handwritten_fmgr_source)
    list(GET ARG_HANDWRITTEN_FMGR_EXPORTS 0
      _reviewed_handwritten_fmgr_symbol)
    set(_reviewed_handwritten_fmgr_profiles ${ARG_PROFILES})
    list(SORT _reviewed_handwritten_fmgr_profiles)
    set(_reviewed_handwritten_fmgr_authorities ${ARG_AUTHORITIES})
    list(SORT _reviewed_handwritten_fmgr_authorities)
    _bioseq_sql_reviewed_handwritten_fmgr_contract_authorized(
      _reviewed_handwritten_fmgr_contract_authorized
      OWNER_KEY "${ARG_OWNER_KEY}"
      MODULE "${ARG_MODULE}"
      SOURCE "${_reviewed_handwritten_fmgr_source}"
      PROFILES "${_reviewed_handwritten_fmgr_profiles}"
      AUTHORITIES "${_reviewed_handwritten_fmgr_authorities}"
      SYMBOL "${_reviewed_handwritten_fmgr_symbol}")
    if(NOT _reviewed_handwritten_fmgr_contract_authorized)
      _bioseq_sql_registry_fail(
        "reviewed handwritten FMGR export authority is restricted to "
        "external_relation_fmgr_export / "
        "bioseq.postgres.external.relation / "
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
    PROPERTY "${_context_prefix}_ROOT")
  get_property(_audit_roots GLOBAL
    PROPERTY "${_context_prefix}_AUDIT_ROOTS")
  get_property(_registered_sources GLOBAL
    PROPERTY "${_context_prefix}_SOURCES")
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
    elseif(ARG_HANDWRITTEN_FMGR_EXPORTS
        AND _annotation_index EQUAL -1)
      # A reviewed handwritten FMGR export can authorize an otherwise
      # annotation-free source. The external handler now also owns natural
      # SQL annotations, in which case those annotations provide source
      # authority and the export facet independently closes wrapper ownership.
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
      _bioseq_sql_dependency_only_support_source(
        _authorized_support_source "${_normalized_source}")
      if(NOT _authorized_support_source)
        _bioseq_sql_registry_fail(
          "SUPPORT source is not a dependency-only sql_rows/GUC module: "
          "${_normalized_source}")
      endif()
      if(_source_content MATCHES
          "import[ \t\r\n]+[A-Za-z_][A-Za-z0-9_.]*\\.sql\\.generated")
        _bioseq_sql_registry_fail(
          "SUPPORT source imports generated SQL modules: ${_source_real}")
      endif()
    endif()
    if(NOT _core_index EQUAL -1
        AND _source_content MATCHES
          "import[ \t\r\n]+[A-Za-z_][A-Za-z0-9_.]*\\.sql\\.generated")
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
  foreach(_facet_key IN LISTS ARG_HANDWRITTEN_FMGR_EXPORTS)
    list(APPEND _facets_for_record
      "handwritten_fmgr_export=${_facet_key}")
  endforeach()
  list(SORT _facets_for_record)
  string(JOIN "," _facet_record ${_facets_for_record})

  foreach(_source_real IN LISTS _owner_sources)
    foreach(_profile IN LISTS ARG_PROFILES)
      _bioseq_sql_profile_key(_profile_key "${_profile}")
      set_property(GLOBAL APPEND
        PROPERTY "${_context_prefix}_${_profile_key}_FINAL_SOURCES"
        "${_source_real}")
      if(NOT _support_index EQUAL -1)
        set_property(GLOBAL APPEND
          PROPERTY "${_context_prefix}_${_profile_key}_SUPPORT_SOURCES"
          "${_source_real}")
      endif()
      if(NOT _core_index EQUAL -1)
        set_property(GLOBAL APPEND
          PROPERTY "${_context_prefix}_${_profile_key}_CORE_SOURCES"
          "${_source_real}")
      endif()
    endforeach()
    set_property(GLOBAL APPEND
      PROPERTY "${_context_prefix}_OWNER_RECORDS"
      "${ARG_OWNER_KEY}\t${ARG_MODULE}\t${_profile_record}\t${_authority_record}\t${_facet_record}\t${_source_real}")
  endforeach()
  foreach(_fmgr_owner IN LISTS ARG_FMGR_ENTRYPOINTS)
    set_property(GLOBAL APPEND
      PROPERTY "${_context_prefix}_FMGR_OWNER_RECORDS"
      "${_fmgr_owner}=${ARG_MODULE}")
    foreach(_profile IN LISTS ARG_PROFILES)
      _bioseq_sql_profile_key(_profile_key "${_profile}")
      set_property(GLOBAL APPEND
        PROPERTY
          "${_context_prefix}_${_profile_key}_FMGR_OWNER_RECORDS"
        "${_fmgr_owner}=${ARG_MODULE}")
    endforeach()
  endforeach()

  set_property(GLOBAL APPEND PROPERTY "${_context_prefix}_OWNERS"
    "${ARG_OWNER_KEY}")
  set_property(GLOBAL APPEND PROPERTY "${_context_prefix}_MODULES"
    "${ARG_MODULE}")
  set_property(GLOBAL PROPERTY "${_context_prefix}_SOURCES"
    "${_registered_sources}")
endfunction()

function(bioseq_finalize_sql_owner_registry)
  cmake_parse_arguments(
    ARG
    ""
    "EXTENSION;OUTPUT_DIR;INSTALL_PROFILE;TEST_PROFILE"
    "EXPECTED_ROW_CODECS;EXPECTED_ENTITY_HANDLES;EXPECTED_FMGR_ENTRYPOINTS;EXPECTED_GUC_OWNERS;EXPECTED_ANNOTATION_FREE_RESULT_ROWS;EXPECTED_HANDWRITTEN_FMGR_EXPORTS"
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

  _bioseq_sql_context(_context_key _context_prefix "${ARG_EXTENSION}")
  get_property(_registry_started GLOBAL
    PROPERTY "${_context_prefix}_INITIALIZED" SET)
  if(NOT _registry_started)
    _bioseq_sql_registry_fail(
      "cannot finalize extension context '${ARG_EXTENSION}' because its "
      "owner registry was not initialized")
  endif()
  get_property(_registry_finalized GLOBAL
    PROPERTY "${_context_prefix}_FINALIZED")
  if(_registry_finalized)
    _bioseq_sql_registry_fail(
      "the ${_context_key} owner registry was finalized twice")
  endif()

  get_filename_component(_output_dir "${ARG_OUTPUT_DIR}" ABSOLUTE)
  get_property(_registry_root GLOBAL
    PROPERTY "${_context_prefix}_ROOT")
  string(FIND "${_output_dir}/" "${_registry_root}/" _source_tree_prefix)
  if(_source_tree_prefix EQUAL 0)
    _bioseq_sql_registry_fail(
      "registry outputs must be in the build tree, not under ${_registry_root}")
  endif()
  file(MAKE_DIRECTORY "${_output_dir}")
  set_property(GLOBAL PROPERTY "${_context_prefix}_OUTPUT_ROOT"
    "${_output_dir}")

  foreach(_facet_type IN ITEMS
      ROW_CODECS ENTITY_HANDLES FMGR_ENTRYPOINTS GUC_OWNERS
      ANNOTATION_FREE_RESULT_ROWS HANDWRITTEN_FMGR_EXPORTS)
    set(_expected_argument "ARG_EXPECTED_${_facet_type}")
    set(_expected_facets ${${_expected_argument}})
    _bioseq_sql_require_unique_values(
      "EXPECTED_${_facet_type}" ${_expected_facets})
    get_property(_actual_facets GLOBAL
      PROPERTY "${_context_prefix}_${_facet_type}")
    list(SORT _expected_facets)
    list(SORT _actual_facets)
    if(NOT "${_actual_facets}" STREQUAL "${_expected_facets}")
      _bioseq_sql_registry_fail(
        "${_facet_type} output keys do not match registered ownership; "
        "registered='${_actual_facets}', expected='${_expected_facets}'")
    endif()
  endforeach()

  get_property(_registered_sources GLOBAL
    PROPERTY "${_context_prefix}_SOURCES")
  get_property(_audit_roots GLOBAL
    PROPERTY "${_context_prefix}_AUDIT_ROOTS")
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
    PROPERTY "${_context_prefix}_PROFILES")
  if(NOT ARG_INSTALL_PROFILE)
    list(FIND _profiles production _production_profile_index)
    if(NOT _production_profile_index EQUAL -1)
      set(ARG_INSTALL_PROFILE production)
    else()
      list(GET _profiles 0 ARG_INSTALL_PROFILE)
    endif()
  endif()
  if(NOT ARG_TEST_PROFILE)
    list(FIND _profiles test_indexes _test_indexes_profile_index)
    if(NOT _test_indexes_profile_index EQUAL -1)
      set(ARG_TEST_PROFILE test_indexes)
    else()
      set(ARG_TEST_PROFILE "${ARG_INSTALL_PROFILE}")
    endif()
  endif()
  foreach(_selected_profile IN ITEMS
      "${ARG_INSTALL_PROFILE}" "${ARG_TEST_PROFILE}")
    list(FIND _profiles "${_selected_profile}" _selected_profile_index)
    if(_selected_profile_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "selected install/test profile '${_selected_profile}' is not in "
        "the ${_context_key} context profile set '${_profiles}'")
    endif()
  endforeach()

  foreach(_profile IN LISTS _profiles)
    _bioseq_sql_profile_key(_profile_key "${_profile}")
    foreach(_stage IN ITEMS SUPPORT CORE FINAL)
      get_property(_stage_sources GLOBAL
        PROPERTY
          "${_context_prefix}_${_profile_key}_${_stage}_SOURCES")
      if(NOT _stage_sources AND _stage STREQUAL "FINAL")
        _bioseq_sql_registry_fail(
          "${_context_key}/${_profile}/FINAL registry is empty")
      endif()
      if(_stage_sources)
        list(SORT _stage_sources)
        list(REMOVE_DUPLICATES _stage_sources)
        string(TOLOWER "${_stage}" _stage_name)
        set(_registry_file
          "${_output_dir}/owners-${_profile}-${_stage_name}.txt")
        _bioseq_sql_write_registry_file(
          "${_registry_file}" ${_stage_sources})
      else()
        set(_registry_file)
      endif()
      set(_context_file_variable
        "${_context_prefix}_REGISTRY_${_profile_key}_${_stage}_FILE")
      set(_context_sources_variable
        "${_context_prefix}_REGISTRY_${_profile_key}_${_stage}_SOURCES")
      set(${_context_file_variable}
        "${_registry_file}" PARENT_SCOPE)
      set(${_context_sources_variable}
        "${_stage_sources}" PARENT_SCOPE)
      if(_context_key STREQUAL "BIOSEQ")
        set(BIOSEQ_SQL_REGISTRY_${_profile_key}_${_stage}_FILE
          "${_registry_file}" PARENT_SCOPE)
        set(BIOSEQ_SQL_REGISTRY_${_profile_key}_${_stage}_SOURCES
          "${_stage_sources}" PARENT_SCOPE)
      endif()
      set("_profile_${_profile_key}_${_stage}_sources"
        "${_stage_sources}")
    endforeach()
  endforeach()

  _bioseq_sql_profile_key(_install_profile_key "${ARG_INSTALL_PROFILE}")
  _bioseq_sql_profile_key(_test_profile_key "${ARG_TEST_PROFILE}")
  set(_install_final_variable
    "_profile_${_install_profile_key}_FINAL_sources")
  set(_install_support_variable
    "_profile_${_install_profile_key}_SUPPORT_sources")
  set(_install_core_variable
    "_profile_${_install_profile_key}_CORE_sources")
  set(_test_final_variable
    "_profile_${_test_profile_key}_FINAL_sources")
  set(_test_support_variable
    "_profile_${_test_profile_key}_SUPPORT_sources")
  set(_test_core_variable
    "_profile_${_test_profile_key}_CORE_sources")
  set(_install_final_sources
    "${${_install_final_variable}}")
  set(_install_support_sources
    "${${_install_support_variable}}")
  set(_install_core_sources
    "${${_install_core_variable}}")
  set(_test_final_sources
    "${${_test_final_variable}}")
  set(_test_support_sources
    "${${_test_support_variable}}")
  set(_test_core_sources
    "${${_test_core_variable}}")

  foreach(_install_source IN LISTS _install_final_sources)
    list(FIND _test_final_sources
      "${_install_source}" _test_profile_index)
    if(_test_profile_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "${ARG_INSTALL_PROFILE} owner is absent from "
        "${ARG_TEST_PROFILE} closure: ${_install_source}")
    endif()
  endforeach()
  foreach(_install_source IN LISTS _install_support_sources)
    list(FIND _test_support_sources
      "${_install_source}" _test_profile_index)
    if(_test_profile_index EQUAL -1)
      _bioseq_sql_registry_fail(
        "${ARG_INSTALL_PROFILE} support owner is absent from "
        "${ARG_TEST_PROFILE} closure: ${_install_source}")
    endif()
  endforeach()
  if(NOT "${_install_core_sources}" STREQUAL
      "${_test_core_sources}")
    _bioseq_sql_registry_fail(
      "CORE registry must be profile-neutral; ${ARG_INSTALL_PROFILE}='"
      "${_install_core_sources}', ${ARG_TEST_PROFILE}='"
      "${_test_core_sources}'")
  endif()

  if(_install_core_sources)
    set(_core_registry_file "${_output_dir}/owners-core.txt")
    _bioseq_sql_write_registry_file(
      "${_core_registry_file}" ${_install_core_sources})
  else()
    set(_core_registry_file)
  endif()
  set("${_context_prefix}_REGISTRY_CORE_FILE"
    "${_core_registry_file}" PARENT_SCOPE)
  set("${_context_prefix}_REGISTRY_CORE_SOURCES"
    "${_install_core_sources}" PARENT_SCOPE)
  if(_context_key STREQUAL "BIOSEQ")
    set(BIOSEQ_SQL_REGISTRY_CORE_FILE
      "${_core_registry_file}" PARENT_SCOPE)
    set(BIOSEQ_SQL_REGISTRY_CORE_SOURCES
      "${_install_core_sources}" PARENT_SCOPE)
  endif()

  get_property(_registry_records GLOBAL
    PROPERTY "${_context_prefix}_OWNER_RECORDS")
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
  set("${_context_prefix}_OWNER_REGISTRY_METADATA"
    "${_metadata_file}" PARENT_SCOPE)
  set("${_context_prefix}_INSTALL_PROFILE"
    "${ARG_INSTALL_PROFILE}" PARENT_SCOPE)
  set("${_context_prefix}_TEST_PROFILE"
    "${ARG_TEST_PROFILE}" PARENT_SCOPE)
  if(_context_key STREQUAL "BIOSEQ")
    set(BIOSEQ_SQL_OWNER_REGISTRY_METADATA
      "${_metadata_file}" PARENT_SCOPE)
  endif()

  set_property(GLOBAL PROPERTY "${_context_prefix}_FINALIZED" TRUE)
endfunction()
