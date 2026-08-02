include_guard(GLOBAL)

# Configure-time orchestration for the annotation-driven Highway module
# generator.  The generated production sources are C++ module units only; the
# probe is also emitted as a build-tree .cppm file and is preprocessed without
# being compiled or linked.

function(_bioseq_highway_fail)
  string(JOIN "" _message ${ARGN})
  message(FATAL_ERROR "Bioseq Highway codegen: ${_message}")
endfunction()

function(_bioseq_highway_write_if_different path contents)
  set(_write TRUE)
  if(EXISTS "${path}")
    file(READ "${path}" _current)
    if(_current STREQUAL contents)
      set(_write FALSE)
    endif()
  endif()
  if(_write)
    get_filename_component(_directory "${path}" DIRECTORY)
    file(MAKE_DIRECTORY "${_directory}")
    file(WRITE "${path}" "${contents}")
  endif()
endfunction()

function(_bioseq_highway_reject_control_characters label value)
  if("${value}" MATCHES "[\t\r\n]")
    _bioseq_highway_fail("${label} contains a tab or newline")
  endif()
endfunction()

function(_bioseq_highway_resolve_platform output)
  string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _processor)
  if(_processor MATCHES "^(x86_64|amd64|x64)$")
    set(_platform x86_64)
  elseif(_processor MATCHES "^(aarch64|arm64)$")
    set(_platform aarch64)
  elseif(_processor MATCHES "^(armv7|armv7l|arm)$")
    set(_platform arm)
  elseif(_processor MATCHES "^(ppc64le|powerpc64le)$")
    set(_platform ppc64le)
  elseif(_processor MATCHES "^(ppc64|powerpc64)$")
    set(_platform ppc64)
  elseif(_processor MATCHES "^(s390x)$")
    set(_platform s390x)
  elseif(_processor MATCHES "^(riscv64)$")
    set(_platform riscv64)
  elseif(_processor MATCHES "^(loongarch64)$")
    set(_platform loongarch64)
  elseif(_processor MATCHES "^(wasm32|wasm64)$")
    set(_platform "${_processor}")
  else()
    string(REGEX REPLACE "[^a-z0-9_]+" "_" _platform "${_processor}")
    if(NOT _platform MATCHES "^[a-z][a-z0-9_]*$")
      _bioseq_highway_fail(
        "cannot derive a stable platform slug from CMAKE_SYSTEM_PROCESSOR='"
        "${CMAKE_SYSTEM_PROCESSOR}'")
    endif()
  endif()
  set(${output} "${_platform}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_collect_target_usage
    output_include_dirs output_definitions output_compile_options)
  if(NOT TARGET hwy::hwy)
    _bioseq_highway_fail(
      "hwy::hwy must exist before bioseq_add_highway_codegen is called")
  endif()

  get_target_property(_include_dirs hwy::hwy INTERFACE_INCLUDE_DIRECTORIES)
  if(NOT _include_dirs OR _include_dirs STREQUAL "_include_dirs-NOTFOUND")
    _bioseq_highway_fail("hwy::hwy publishes no include directory")
  endif()
  set(_resolved_include_dirs)
  foreach(_include_dir IN LISTS _include_dirs)
    if(_include_dir MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
      set(_include_dir "${CMAKE_MATCH_1}")
    elseif(_include_dir MATCHES "^\\$<INSTALL_INTERFACE:.*>$")
      continue()
    elseif(_include_dir MATCHES "\\$<")
      _bioseq_highway_fail(
        "unsupported generator expression in hwy::hwy include path: "
        "${_include_dir}")
    endif()
    get_filename_component(_include_dir "${_include_dir}" ABSOLUTE)
    if(NOT IS_DIRECTORY "${_include_dir}")
      _bioseq_highway_fail(
        "hwy::hwy include directory does not exist: ${_include_dir}")
    endif()
    list(APPEND _resolved_include_dirs "${_include_dir}")
  endforeach()
  list(REMOVE_DUPLICATES _resolved_include_dirs)
  list(SORT _resolved_include_dirs)

  get_target_property(_definitions hwy::hwy INTERFACE_COMPILE_DEFINITIONS)
  if(NOT _definitions OR _definitions STREQUAL "_definitions-NOTFOUND")
    set(_definitions)
  endif()
  foreach(_definition IN LISTS _definitions)
    if(_definition MATCHES "\\$<")
      _bioseq_highway_fail(
        "unsupported generator expression in hwy::hwy compile definition: "
        "${_definition}")
    endif()
  endforeach()

  get_target_property(_compile_options hwy::hwy INTERFACE_COMPILE_OPTIONS)
  if(NOT _compile_options
      OR _compile_options STREQUAL "_compile_options-NOTFOUND")
    set(_compile_options)
  endif()
  foreach(_compile_option IN LISTS _compile_options)
    if(_compile_option MATCHES "\\$<")
      _bioseq_highway_fail(
        "unsupported generator expression in hwy::hwy compile option: "
        "${_compile_option}")
    endif()
  endforeach()

  set(${output_include_dirs} "${_resolved_include_dirs}" PARENT_SCOPE)
  set(${output_definitions} "${_definitions}" PARENT_SCOPE)
  set(${output_compile_options} "${_compile_options}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_normalize_definitions output)
  set(_definitions)
  set(_definition_names)
  set(_definition_values)
  foreach(_definition IN LISTS ARGN)
    string(REGEX REPLACE "^-D" "" _definition "${_definition}")
    _bioseq_highway_reject_control_characters(
      "Highway policy definition" "${_definition}")
    if(NOT _definition MATCHES
        "^[A-Za-z_][A-Za-z0-9_]*(=[^; ]+)?$")
      _bioseq_highway_fail(
        "invalid Highway policy definition '${_definition}'")
    endif()
    string(REGEX REPLACE "=.*$" "" _name "${_definition}")
    if(_name STREQUAL "HWY_TARGET" OR _name STREQUAL "HWY_ONCE")
      _bioseq_highway_fail(
        "${_name} is generator-owned and cannot be a policy definition")
    endif()
    list(FIND _definition_names "${_name}" _existing_index)
    if(NOT _existing_index EQUAL -1)
      list(GET _definition_values ${_existing_index} _existing_definition)
      if(NOT _existing_definition STREQUAL _definition)
        _bioseq_highway_fail(
          "conflicting values for Highway policy definition ${_name}: "
          "'${_existing_definition}' and '${_definition}'")
      endif()
      continue()
    endif()
    list(APPEND _definition_names "${_name}")
    list(APPEND _definition_values "${_definition}")
    list(APPEND _definitions "${_definition}")
  endforeach()
  list(SORT _definitions)
  set(${output} "${_definitions}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_find_header output include_dirs relative_path)
  foreach(_include_dir IN LISTS include_dirs)
    if(EXISTS "${_include_dir}/${relative_path}")
      set(${output} "${_include_dir}/${relative_path}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  _bioseq_highway_fail(
    "cannot find ${relative_path} beneath the hwy::hwy include directories")
endfunction()

function(_bioseq_highway_probe_targets)
  set(_one_value_arguments
    PROBE_DIR
    PLATFORM
    OUTPUT_RECORDS
    OUTPUT_ORDER
    OUTPUT_MASK
    OUTPUT_STATIC_RECORD
    OUTPUT_POLICY_DIGEST
    OUTPUT_HIGHWAY_DIGEST)
  cmake_parse_arguments(PARSE_ARGV 0 ARG ""
    "${_one_value_arguments}"
    "INCLUDE_DIRS;DEFINITIONS;COMPILE_OPTIONS")
  foreach(_required IN ITEMS
      PROBE_DIR PLATFORM OUTPUT_RECORDS OUTPUT_ORDER OUTPUT_MASK
      OUTPUT_STATIC_RECORD OUTPUT_POLICY_DIGEST OUTPUT_HIGHWAY_DIGEST)
    if(NOT ARG_${_required})
      _bioseq_highway_fail("target probe requires ${_required}")
    endif()
  endforeach()

  file(MAKE_DIRECTORY "${ARG_PROBE_DIR}")
  set(_probe_source "${ARG_PROBE_DIR}/target_inventory_probe.cppm")
  string(CONCAT _probe_contents
    "// Generated configure-time Highway target inventory probe.\n"
    "// Target operations are irrelevant here; choose opposite toggle state\n"
    "// so highway.h publishes dispatch macros without expanding ops/*.h.\n"
    "#ifdef HWY_TARGET_TOGGLE\n"
    "#undef HWY_TARGET_TOGGLE\n"
    "#endif\n"
    "#define HWY_HIGHWAY_PER_TARGET 1\n"
    "#include <hwy/highway.h>\n"
    "#define BIOSEQ_HIGHWAY_STRING_IMPL(value) #value\n"
    "#define BIOSEQ_HIGHWAY_STRING(value) BIOSEQ_HIGHWAY_STRING_IMPL(value)\n"
    "#define BIOSEQ_HIGHWAY_VISITOR(target, target_namespace) \\\n"
    "  BIOSEQ_HIGHWAY_TARGET_RECORD(#target, \\\n"
    "    BIOSEQ_HIGHWAY_STRING(target_namespace), \\\n"
    "    BIOSEQ_HIGHWAY_STRING(target))\n"
    "BIOSEQ_HIGHWAY_STATIC_RECORD(\\\n"
    "  BIOSEQ_HIGHWAY_STRING(HWY_STATIC_NAMESPACE), \\\n"
    "  BIOSEQ_HIGHWAY_STRING(HWY_STATIC_TARGET))\n"
    "HWY_VISIT_TARGETS(BIOSEQ_HIGHWAY_VISITOR)\n")
  _bioseq_highway_write_if_different("${_probe_source}" "${_probe_contents}")

  set(_compiler_command)
  if(CMAKE_CXX_COMPILER_LAUNCHER)
    list(APPEND _compiler_command ${CMAKE_CXX_COMPILER_LAUNCHER})
  endif()
  list(APPEND _compiler_command "${CMAKE_CXX_COMPILER}")
  if(CMAKE_CXX_COMPILER_ARG1)
    separate_arguments(_compiler_arg1 NATIVE_COMMAND
      "${CMAKE_CXX_COMPILER_ARG1}")
    list(APPEND _compiler_command ${_compiler_arg1})
  endif()

  set(_configured_flags "${CMAKE_CXX_FLAGS}")
  if(CMAKE_BUILD_TYPE)
    string(TOUPPER "${CMAKE_BUILD_TYPE}" _build_type)
    string(APPEND _configured_flags " ${CMAKE_CXX_FLAGS_${_build_type}}")
  endif()
  separate_arguments(_configured_flags NATIVE_COMMAND "${_configured_flags}")

  set(_probe_arguments ${_configured_flags} ${ARG_COMPILE_OPTIONS})
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    if(CMAKE_CXX_EXTENSIONS)
      list(APPEND _probe_arguments "-std=gnu++${CMAKE_CXX_STANDARD}")
    else()
      list(APPEND _probe_arguments "-std=c++${CMAKE_CXX_STANDARD}")
    endif()
    list(APPEND _probe_arguments -E -P -x c++)
    foreach(_definition IN LISTS ARG_DEFINITIONS)
      list(APPEND _probe_arguments "-D${_definition}")
    endforeach()
    foreach(_include_dir IN LISTS ARG_INCLUDE_DIRS)
      list(APPEND _probe_arguments -isystem "${_include_dir}")
    endforeach()
    list(APPEND _probe_arguments "${_probe_source}")
  elseif(MSVC)
    list(APPEND _probe_arguments /nologo /EP /TP /std:c++latest)
    foreach(_definition IN LISTS ARG_DEFINITIONS)
      list(APPEND _probe_arguments "/D${_definition}")
    endforeach()
    foreach(_include_dir IN LISTS ARG_INCLUDE_DIRS)
      list(APPEND _probe_arguments "/I${_include_dir}")
    endforeach()
    list(APPEND _probe_arguments "${_probe_source}")
  else()
    _bioseq_highway_fail(
      "unsupported target-probe compiler ${CMAKE_CXX_COMPILER_ID}")
  endif()

  execute_process(
    COMMAND ${_compiler_command} ${_probe_arguments}
    RESULT_VARIABLE _probe_status
    OUTPUT_VARIABLE _probe_output
    ERROR_VARIABLE _probe_error)
  if(NOT _probe_status EQUAL 0)
    _bioseq_highway_fail(
      "HWY_VISIT_TARGETS preprocessor probe failed with exit code "
      "${_probe_status}:\n${_probe_error}")
  endif()

  string(REGEX MATCHALL
    "BIOSEQ_HIGHWAY_TARGET_RECORD\\([ \\t]*\"[A-Z0-9_]+\",[ \\t]*\"[A-Z0-9_]+\",[ \\t]*\"[^\"]+\"[ \\t]*\\)"
    _raw_records "${_probe_output}")
  if(NOT _raw_records)
    _bioseq_highway_fail(
      "HWY_VISIT_TARGETS probe produced no target records")
  endif()

  string(REGEX MATCHALL
    "BIOSEQ_HIGHWAY_STATIC_RECORD\\([ \\t]*\"[A-Z0-9_]+\",[ \\t]*\"[^\"]+\"[ \\t]*\\)"
    _raw_static_records "${_probe_output}")
  list(LENGTH _raw_static_records _raw_static_record_count)
  if(NOT _raw_static_record_count EQUAL 1)
    _bioseq_highway_fail(
      "Highway probe must produce exactly one HWY_STATIC_TARGET record")
  endif()
  list(GET _raw_static_records 0 _raw_static_record)
  if(NOT _raw_static_record MATCHES
      "^BIOSEQ_HIGHWAY_STATIC_RECORD\\([ \\t]*\"([A-Z0-9_]+)\",[ \\t]*\"([^\"]+)\"[ \\t]*\\)$")
    _bioseq_highway_fail(
      "malformed static target record '${_raw_static_record}'")
  endif()
  set(_static_target_namespace "${CMAKE_MATCH_1}")
  set(_static_target_expression "${CMAKE_MATCH_2}")
  if(NOT _static_target_expression MATCHES
      "^[0-9A-Fa-fxXuUlL ()<>&|+*~/-]+$")
    _bioseq_highway_fail(
      "HWY_STATIC_TARGET has a non-numeric bit expression "
      "'${_static_target_expression}'")
  endif()
  string(REGEX REPLACE "([0-9A-Fa-fxX])[uUlL]+" "\\1"
    _static_target_expression "${_static_target_expression}")
  math(EXPR _static_target_bit
    "${_static_target_expression}" OUTPUT_FORMAT DECIMAL)
  if(_static_target_bit LESS_EQUAL 0)
    _bioseq_highway_fail(
      "HWY_STATIC_TARGET has invalid non-positive bit "
      "${_static_target_bit}")
  endif()

  set(_records_by_bit)
  set(_seen_bits)
  set(_seen_namespaces)
  set(_namespace_bits)
  foreach(_raw_record IN LISTS _raw_records)
    string(CONCAT _record_pattern
      "^BIOSEQ_HIGHWAY_TARGET_RECORD\\([ \\t]*\"([A-Z0-9_]+)\",[ \\t]*"
      "\"([A-Z0-9_]+)\",[ \\t]*\"([^\"]+)\"[ \\t]*\\)$")
    if(NOT _raw_record MATCHES
        "${_record_pattern}")
      _bioseq_highway_fail("malformed target record '${_raw_record}'")
    endif()
    set(_token "${CMAKE_MATCH_1}")
    set(_target_namespace "${CMAKE_MATCH_2}")
    set(_numeric_expression "${CMAKE_MATCH_3}")
    if(NOT _numeric_expression MATCHES
        "^[0-9A-Fa-fxXuUlL ()<>&|+*~/-]+$")
      _bioseq_highway_fail(
        "target ${_token} has a non-numeric bit expression "
        "'${_numeric_expression}'")
    endif()
    string(REGEX REPLACE "([0-9A-Fa-fxX])[uUlL]+" "\\1"
      _numeric_expression "${_numeric_expression}")
    math(EXPR _numeric_bit "${_numeric_expression}" OUTPUT_FORMAT DECIMAL)
    if(_numeric_bit LESS_EQUAL 0)
      _bioseq_highway_fail(
        "target ${_token} has invalid non-positive bit ${_numeric_bit}")
    endif()
    math(EXPR _bit_minus_one "${_numeric_bit} - 1")
    math(EXPR _power_check "${_numeric_bit} & ${_bit_minus_one}")
    if(NOT _power_check EQUAL 0)
      _bioseq_highway_fail(
        "target ${_token} value ${_numeric_bit} is not one bit")
    endif()

    if(_token STREQUAL "HWY_STATIC_TARGET")
      if(NOT _static_target_bit STREQUAL ""
          AND (NOT _static_target_bit STREQUAL "${_numeric_bit}"
            OR NOT _static_target_namespace STREQUAL
              "${_target_namespace}"))
        _bioseq_highway_fail(
          "HWY_STATIC_TARGET maps to conflicting target records")
      endif()
      set(_static_target_bit "${_numeric_bit}")
      set(_static_target_namespace "${_target_namespace}")
    endif()

    list(FIND _seen_namespaces "${_target_namespace}" _namespace_index)
    if(NOT _namespace_index EQUAL -1)
      list(GET _namespace_bits ${_namespace_index} _namespace_bit)
      if(NOT _namespace_bit STREQUAL "${_numeric_bit}")
        _bioseq_highway_fail(
          "namespace ${_target_namespace} maps to conflicting target bits "
          "${_namespace_bit} and ${_numeric_bit}")
      endif()
    else()
      list(APPEND _seen_namespaces "${_target_namespace}")
      list(APPEND _namespace_bits "${_numeric_bit}")
    endif()

    list(FIND _seen_bits "${_numeric_bit}" _bit_index)
    set(_candidate "${_token}|${_target_namespace}|${_numeric_bit}")
    if(_bit_index EQUAL -1)
      list(APPEND _seen_bits "${_numeric_bit}")
      list(APPEND _records_by_bit "${_candidate}")
      continue()
    endif()

    list(GET _records_by_bit ${_bit_index} _existing)
    if(NOT _existing MATCHES "^([^|]+)\\|([^|]+)\\|([0-9]+)$")
      _bioseq_highway_fail("internal malformed target record '${_existing}'")
    endif()
    set(_existing_token "${CMAKE_MATCH_1}")
    set(_existing_namespace "${CMAKE_MATCH_2}")
    if(NOT _existing_namespace STREQUAL _target_namespace)
      _bioseq_highway_fail(
        "target bit ${_numeric_bit} maps to conflicting namespaces "
        "${_existing_namespace} and ${_target_namespace}")
    endif()
    if(_existing_token STREQUAL _token)
      continue()
    elseif(_existing_token STREQUAL "HWY_STATIC_TARGET"
        AND _token MATCHES "^HWY_[A-Z0-9_]+$")
      # HWY_VISIT_FALLBACK aliases the static target. Prefer the later concrete
      # target visit after proving that its bit and namespace are identical.
      list(REMOVE_AT _records_by_bit ${_bit_index})
      list(INSERT _records_by_bit ${_bit_index} "${_candidate}")
    elseif(_token STREQUAL "HWY_STATIC_TARGET"
        AND _existing_token MATCHES "^HWY_[A-Z0-9_]+$")
      # Same alias in the opposite order; retain the concrete spelling.
      continue()
    else()
      _bioseq_highway_fail(
        "target bit ${_numeric_bit} maps to conflicting tokens "
        "${_existing_token} and ${_token}")
    endif()
  endforeach()

  set(_target_records)
  foreach(_record IN LISTS _records_by_bit)
    if(NOT _record MATCHES "^([^|]+)\\|([^|]+)\\|([0-9]+)$")
      _bioseq_highway_fail("internal malformed target record '${_record}'")
    endif()
    set(_token "${CMAKE_MATCH_1}")
    set(_target_namespace "${CMAKE_MATCH_2}")
    set(_numeric_bit "${CMAKE_MATCH_3}")
    if(_token STREQUAL "HWY_STATIC_TARGET")
      _bioseq_highway_fail(
        "could not canonicalize the HWY_VISIT_FALLBACK static target alias")
    endif()
    if(NOT _token MATCHES "^HWY_([A-Z0-9_]+)$")
      _bioseq_highway_fail("invalid Highway target token '${_token}'")
    endif()
    string(TOLOWER "${CMAKE_MATCH_1}" _slug)
    list(APPEND _target_records
      "${ARG_PLATFORM}|${_slug}|${_token}|${_target_namespace}|${_numeric_bit}")
  endforeach()
  list(SORT _target_records)

  set(_target_order)
  set(_compiled_mask 0)
  set(_static_target_record)
  foreach(_record IN LISTS _target_records)
    if(NOT _record MATCHES
        "^([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]+)\\|([0-9]+)$")
      _bioseq_highway_fail("internal malformed normalized record '${_record}'")
    endif()
    list(APPEND _target_order "${CMAKE_MATCH_3}")
    math(EXPR _compiled_mask "${_compiled_mask} | ${CMAKE_MATCH_5}")
    if(CMAKE_MATCH_5 STREQUAL "${_static_target_bit}")
      set(_static_target_record "${_record}")
    endif()
  endforeach()
  if(NOT _static_target_record)
    _bioseq_highway_fail(
      "HWY_STATIC_TARGET is absent from the canonical target inventory")
  endif()

  # This exact, sorted block is also rendered by the generator ahead of every
  # Highway include. A bare -DNAME has the preprocessor value 1, so record that
  # value explicitly rather than relying on driver spelling.
  set(_policy_block "")
  foreach(_definition IN LISTS ARG_DEFINITIONS)
    if(_definition MATCHES "^([^=]+)=(.*)$")
      set(_policy_name "${CMAKE_MATCH_1}")
      set(_policy_value "${CMAKE_MATCH_2}")
    else()
      set(_policy_name "${_definition}")
      set(_policy_value 1)
    endif()
    string(APPEND _policy_block
      "#define ${_policy_name} ${_policy_value}\n")
  endforeach()
  string(SHA256 _policy_digest "${_policy_block}")
  _bioseq_highway_find_header(
    _detect_targets_header "${ARG_INCLUDE_DIRS}" "hwy/detect_targets.h")
  _bioseq_highway_find_header(
    _highway_header "${ARG_INCLUDE_DIRS}" "hwy/highway.h")
  _bioseq_highway_find_header(
    _targets_header "${ARG_INCLUDE_DIRS}" "hwy/targets.h")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_detect_targets_header}" "${_highway_header}" "${_targets_header}")
  file(SHA256 "${_detect_targets_header}" _detect_targets_digest)
  file(SHA256 "${_highway_header}" _highway_header_digest)
  file(SHA256 "${_targets_header}" _targets_header_digest)
  string(SHA256 _highway_digest
    "${_detect_targets_digest}\n${_highway_header_digest}\n${_targets_header_digest}\n")

  set(${ARG_OUTPUT_RECORDS} "${_target_records}" PARENT_SCOPE)
  set(${ARG_OUTPUT_ORDER} "${_target_order}" PARENT_SCOPE)
  set(${ARG_OUTPUT_MASK} "${_compiled_mask}" PARENT_SCOPE)
  set(${ARG_OUTPUT_STATIC_RECORD} "${_static_target_record}" PARENT_SCOPE)
  set(${ARG_OUTPUT_POLICY_DIGEST} "${_policy_digest}" PARENT_SCOPE)
  set(${ARG_OUTPUT_HIGHWAY_DIGEST} "${_highway_digest}" PARENT_SCOPE)
endfunction()

# bioseq_add_highway_codegen(
#   TARGET bioseq_highway_codegen
#   POLICY_TARGET bioseq_highway_policy
#   OUTPUT_ROOT <build>/generated/highway
#   CONTRACT_MODULE bioseq.highway.dispatch_contract
#   DISPATCH_MODULE bioseq.scheduler.dispatch_processor
#   HIGHWAY_VERSION 1.3.0
#   COMPILE_DATABASE <build>/compile_commands.json
#   SIGNATURE_SOURCE /absolute/attached/neutral_primary.cppm
#   PCM_ROOT <build>/generated/highway/pcm
#   SOURCE_RECORDS
#     "key|/absolute/template.cppm|authored.module|partition|family|dep_a,dep_b"
#   SLOT_RECORDS
#     "owner|family_field|slot_field|qualified::{target_namespace}::symbol"
#   REQUIRED_SLOT_RECORDS
#     "family_field|slot_field"
#   OUTPUT_MODULES generated_modules
#   OUTPUT_PRIMARY_MODULES generated_target_primaries
#   OUTPUT_TARGET_IMPLEMENTATIONS generated_target_implementations
#   OUTPUT_DISPATCH_REGISTRY generated_highway_registry
#   OUTPUT_DISPATCH_INVENTORY generated_dispatch_inventory
#   OUTPUT_TEST_PRIMARY_MODULE generated_static_test_primary
#   OUTPUT_TEST_IMPLEMENTATIONS generated_static_test_implementations
#   OUTPUT_TEST_SUPPORT generated_static_test_support
#   OUTPUT_POLICY_TARGET highway_policy_target
#   OUTPUT_PREPARE_TARGET highway_prepare_target
#   OUTPUT_PREPARED_COMPILE_DATABASE highway_owned_compile_database
#   OUTPUT_PCM_MANIFEST highway_pcm_manifest
#   OUTPUT_SOURCE_INPUTS highway_generator_inputs
#   OUTPUT_TARGET_RECORDS compiled_target_records
#   OUTPUT_TARGET_ORDER compiled_target_order
#   OUTPUT_COMPILED_MASK compiled_target_mask)
#
# The source registry remains at the call site so it is explicit and reviewable
# next to the production target that consumes the generated modules.
function(bioseq_add_highway_codegen)
  set(_one_value_arguments
    TARGET
    POLICY_TARGET
    OUTPUT_ROOT
    CONTRACT_MODULE
    DISPATCH_MODULE
    HIGHWAY_VERSION
    COMPILE_DATABASE
    SIGNATURE_SOURCE
    PCM_ROOT
    OUTPUT_MODULES
    OUTPUT_PRIMARY_MODULES
    OUTPUT_TARGET_IMPLEMENTATIONS
    OUTPUT_DISPATCH_REGISTRY
    OUTPUT_DISPATCH_INVENTORY
    OUTPUT_TEST_PRIMARY_MODULE
    OUTPUT_TEST_IMPLEMENTATIONS
    OUTPUT_TEST_SUPPORT
    OUTPUT_POLICY_TARGET
    OUTPUT_PREPARE_TARGET
    OUTPUT_PREPARED_COMPILE_DATABASE
    OUTPUT_PCM_MANIFEST
    OUTPUT_SOURCE_INPUTS
    OUTPUT_TARGET_RECORDS
    OUTPUT_TARGET_ORDER
    OUTPUT_COMPILED_MASK
    OUTPUT_TARGET_MANIFEST
    OUTPUT_SOURCE_MANIFEST
    OUTPUT_ARTIFACT_MANIFEST)
  set(_multi_value_arguments
    SOURCE_RECORDS
    SLOT_RECORDS
    REQUIRED_SLOT_RECORDS
    DOMAIN_TOPOLOGY_MANIFESTS
    POLICY_DEFINITIONS
    POLICY_COMPILE_OPTIONS)
  cmake_parse_arguments(PARSE_ARGV 0 ARG "REQUIRE_COMPLETE_TABLE"
    "${_one_value_arguments}"
    "${_multi_value_arguments}")
  foreach(_required IN ITEMS
      TARGET OUTPUT_ROOT CONTRACT_MODULE DISPATCH_MODULE HIGHWAY_VERSION
      COMPILE_DATABASE SIGNATURE_SOURCE PCM_ROOT SOURCE_RECORDS)
    if(NOT ARG_${_required})
      _bioseq_highway_fail("bioseq_add_highway_codegen requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_highway_fail(
      "unknown bioseq_add_highway_codegen arguments: "
      "${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(TARGET "${ARG_TARGET}")
    _bioseq_highway_fail("target ${ARG_TARGET} already exists")
  endif()
  if(NOT TARGET bioseq_codegen OR NOT TARGET bioseq_codegen_project)
    _bioseq_highway_fail(
      "the shared bioseq_codegen and bioseq_codegen_project targets must "
      "exist before Highway generation is registered")
  endif()
  foreach(_tool_variable IN ITEMS
      BIOSEQ_CODEGEN_CLANG_BINARY
      BIOSEQ_CODEGEN_CLANG_SCAN_DEPS
      BIOSEQ_CODEGEN_LLVM_CONFIG
      BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON)
    if(NOT IS_ABSOLUTE "${${_tool_variable}}"
        OR NOT EXISTS "${${_tool_variable}}")
      _bioseq_highway_fail(
        "${_tool_variable} must identify an existing absolute toolchain "
        "artifact before Highway generation is registered")
    endif()
  endforeach()
  foreach(_digest_variable IN ITEMS
      BIOSEQ_CODEGEN_CLANG_SHA256
      BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256
      BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256
      BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256)
    string(LENGTH "${${_digest_variable}}" _digest_length)
    if(NOT _digest_length EQUAL 64
        OR NOT "${${_digest_variable}}" MATCHES "^[0-9a-f]+$")
      _bioseq_highway_fail(
        "${_digest_variable} must be a configured lowercase SHA-256")
    endif()
  endforeach()
  if(NOT CMAKE_EXPORT_COMPILE_COMMANDS)
    _bioseq_highway_fail(
      "Highway structural generation requires CMAKE_EXPORT_COMPILE_COMMANDS")
  endif()
  foreach(_module IN ITEMS "${ARG_CONTRACT_MODULE}" "${ARG_DISPATCH_MODULE}")
    if(NOT _module MATCHES "^[a-z][a-z0-9_.]*$")
      _bioseq_highway_fail("invalid module name '${_module}'")
    endif()
  endforeach()
  if(NOT ARG_HIGHWAY_VERSION MATCHES
      "^[0-9]+(\\.[0-9]+)*([+-][A-Za-z0-9_.-]+)?$")
    _bioseq_highway_fail(
      "invalid Highway version '${ARG_HIGHWAY_VERSION}'")
  endif()

  get_filename_component(_output_root "${ARG_OUTPUT_ROOT}" ABSOLUTE)
  cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${_output_root}"
    NORMALIZE _output_in_build_tree)
  if(NOT _output_in_build_tree OR _output_root STREQUAL CMAKE_BINARY_DIR)
    _bioseq_highway_fail(
      "OUTPUT_ROOT must be an owned subdirectory of the build tree; got "
      "${_output_root}")
  endif()
  get_filename_component(_compile_database "${ARG_COMPILE_DATABASE}" ABSOLUTE)
  get_filename_component(_signature_source "${ARG_SIGNATURE_SOURCE}" ABSOLUTE)
  if(NOT EXISTS "${_signature_source}"
      OR NOT _signature_source MATCHES "\\.cppm$")
    _bioseq_highway_fail(
      "SIGNATURE_SOURCE must be an existing attached .cppm primary: "
      "${_signature_source}")
  endif()
  get_filename_component(_pcm_root "${ARG_PCM_ROOT}" ABSOLUTE)
  cmake_path(IS_PREFIX _output_root "${_pcm_root}"
    NORMALIZE _pcm_in_output_root)
  if(NOT _pcm_in_output_root)
    _bioseq_highway_fail(
      "PCM_ROOT must reside beneath OUTPUT_ROOT so stale cleanup remains "
      "contained; got ${_pcm_root}")
  endif()
  file(MAKE_DIRECTORY "${_output_root}" "${_pcm_root}")

  _bioseq_highway_collect_target_usage(
    _highway_include_dirs _highway_target_definitions
    _highway_target_compile_options)
  _bioseq_highway_normalize_definitions(_policy_definitions
    ${_highway_target_definitions} ${ARG_POLICY_DEFINITIONS})
  set(_policy_compile_options
    ${_highway_target_compile_options} ${ARG_POLICY_COMPILE_OPTIONS})
  list(REMOVE_DUPLICATES _policy_compile_options)
  foreach(_compile_option IN LISTS _policy_compile_options)
    _bioseq_highway_reject_control_characters(
      "Highway policy compile option" "${_compile_option}")
    if(_compile_option MATCHES "\\$<")
      _bioseq_highway_fail(
        "Highway policy compile options cannot contain generator "
        "expressions: ${_compile_option}")
    endif()
  endforeach()

  if(NOT ARG_POLICY_TARGET)
    set(ARG_POLICY_TARGET "${ARG_TARGET}_policy")
  endif()
  if(TARGET "${ARG_POLICY_TARGET}")
    _bioseq_highway_fail("policy target ${ARG_POLICY_TARGET} already exists")
  endif()
  add_library("${ARG_POLICY_TARGET}" INTERFACE)
  target_link_libraries("${ARG_POLICY_TARGET}" INTERFACE hwy::hwy)
  if(_policy_definitions)
    target_compile_definitions(
      "${ARG_POLICY_TARGET}" INTERFACE ${_policy_definitions})
  endif()
  if(_policy_compile_options)
    target_compile_options(
      "${ARG_POLICY_TARGET}" INTERFACE ${_policy_compile_options})
  endif()

  set(_domain_topology_names)
  set(_domain_topology_paths)
  set(_domain_topology_records)
  set(_domain_topology_cli_arguments)
  set(_domain_topology_ordinal 0)
  foreach(_domain_topology IN LISTS ARG_DOMAIN_TOPOLOGY_MANIFESTS)
    _bioseq_highway_reject_control_characters(
      "domain topology manifest" "${_domain_topology}")
    if(NOT _domain_topology MATCHES "^([a-z][a-z0-9_]*)=(.+)$")
      _bioseq_highway_fail(
        "DOMAIN_TOPOLOGY_MANIFESTS entries must be "
        "name=/absolute/topology.configure.json; got ${_domain_topology}")
    endif()
    set(_domain_name "${CMAKE_MATCH_1}")
    set(_domain_path "${CMAKE_MATCH_2}")
    if(NOT IS_ABSOLUTE "${_domain_path}" OR NOT EXISTS "${_domain_path}")
      _bioseq_highway_fail(
        "domain topology ${_domain_name} must name an existing absolute "
        "manifest: ${_domain_path}")
    endif()
    file(REAL_PATH "${_domain_path}" _domain_path)
    list(FIND _domain_topology_names "${_domain_name}" _domain_name_index)
    list(FIND _domain_topology_paths "${_domain_path}" _domain_path_index)
    if(NOT _domain_name_index EQUAL -1)
      _bioseq_highway_fail("duplicate domain topology name ${_domain_name}")
    elseif(NOT _domain_path_index EQUAL -1)
      _bioseq_highway_fail("duplicate domain topology path ${_domain_path}")
    endif()
    file(READ "${_domain_path}" _domain_document)
    string(REGEX MATCHALL
      "\"topology_digest\"[ \\t\\r\\n]*:[ \\t\\r\\n]*\"[0-9a-f]+\""
      _domain_digest_records "${_domain_document}")
    list(LENGTH _domain_digest_records _domain_digest_record_count)
    if(NOT _domain_digest_record_count EQUAL 1)
      _bioseq_highway_fail(
        "domain topology ${_domain_name} must contain exactly one "
        "topology_digest")
    endif()
    list(GET _domain_digest_records 0 _domain_digest_record)
    string(REGEX REPLACE
      ".*\"([0-9a-f]+)\"$" "\\1"
      _domain_declared_digest "${_domain_digest_record}")
    string(LENGTH "${_domain_declared_digest}" _domain_digest_length)
    if(NOT _domain_digest_length EQUAL 64)
      _bioseq_highway_fail(
        "domain topology ${_domain_name} has a malformed topology_digest")
    endif()
    file(SHA256 "${_domain_path}" _domain_file_digest)
    list(APPEND _domain_topology_names "${_domain_name}")
    list(APPEND _domain_topology_paths "${_domain_path}")
    list(APPEND _domain_topology_records
      "domain-topology\t${_domain_topology_ordinal}\t${_domain_name}\t${_domain_path}\t${_domain_declared_digest}\t${_domain_file_digest}")
    list(APPEND _domain_topology_cli_arguments
      --domain-topology-manifest "${_domain_name}=${_domain_path}")
    math(EXPR _domain_topology_ordinal "${_domain_topology_ordinal} + 1")
  endforeach()
  if(_domain_topology_paths)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
      ${_domain_topology_paths})
  endif()

  _bioseq_highway_resolve_platform(_platform)
  set(_metadata_dir "${_output_root}/metadata")
  set(_target_manifest "${_metadata_dir}/targets.tsv")
  set(_source_manifest "${_metadata_dir}/sources.tsv")
  set(_artifact_manifest "${_output_root}/artifacts.json")
  set(_stamp "${_output_root}/generate.stamp")
  _bioseq_highway_probe_targets(
    PROBE_DIR "${_metadata_dir}/probe"
    PLATFORM "${_platform}"
    INCLUDE_DIRS ${_highway_include_dirs}
    DEFINITIONS ${_policy_definitions}
    COMPILE_OPTIONS ${_policy_compile_options}
    OUTPUT_RECORDS _target_records
    OUTPUT_ORDER _target_order
    OUTPUT_MASK _compiled_mask
    OUTPUT_STATIC_RECORD _static_target_record
    OUTPUT_POLICY_DIGEST _policy_digest
    OUTPUT_HIGHWAY_DIGEST _highway_digest)

  set(_target_manifest_contents "bioseq-highway-targets-v1\n")
  string(APPEND _target_manifest_contents "policy\t${_policy_digest}\n")
  foreach(_definition IN LISTS _policy_definitions)
    if(_definition MATCHES "^([^=]+)=(.*)$")
      set(_policy_name "${CMAKE_MATCH_1}")
      set(_policy_value "${CMAKE_MATCH_2}")
    else()
      set(_policy_name "${_definition}")
      set(_policy_value 1)
    endif()
    string(APPEND _target_manifest_contents
      "policy-definition\t${_policy_name}\t${_policy_value}\n")
  endforeach()
  string(APPEND _target_manifest_contents
    "highway\t${ARG_HIGHWAY_VERSION}\t${_highway_digest}\n")
  if(NOT _static_target_record MATCHES
      "^([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]+)\\|([0-9]+)$")
    _bioseq_highway_fail(
      "internal malformed static target '${_static_target_record}'")
  endif()
  set(_static_target_token "${CMAKE_MATCH_3}")
  string(APPEND _target_manifest_contents
    "static-target\t${_static_target_token}\n")
  foreach(_record IN LISTS _target_records)
    string(REPLACE "|" "\t" _tsv_record "${_record}")
    string(APPEND _target_manifest_contents "target\t${_tsv_record}\n")
  endforeach()
  foreach(_domain_topology_record IN LISTS _domain_topology_records)
    string(APPEND _target_manifest_contents
      "${_domain_topology_record}\n")
  endforeach()
  _bioseq_highway_write_if_different(
    "${_target_manifest}" "${_target_manifest_contents}")

  set(_source_keys)
  set(_source_paths)
  set(_authored_modules)
  set(_partitions)
  set(_normalized_source_records)
  foreach(_record IN LISTS ARG_SOURCE_RECORDS)
    _bioseq_highway_reject_control_characters("source record" "${_record}")
    if(NOT _record MATCHES
        "^([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]*)$")
      _bioseq_highway_fail(
        "malformed source record '${_record}'; expected "
        "key|path|authored_module|partition|family|comma_dependencies")
    endif()
    set(_key "${CMAKE_MATCH_1}")
    set(_source "${CMAKE_MATCH_2}")
    set(_authored_module "${CMAKE_MATCH_3}")
    set(_partition "${CMAKE_MATCH_4}")
    set(_family "${CMAKE_MATCH_5}")
    set(_dependencies_csv "${CMAKE_MATCH_6}")
    if(NOT _key MATCHES "^[a-z][a-z0-9_]*$")
      _bioseq_highway_fail("invalid source key '${_key}'")
    endif()
    if(NOT _authored_module MATCHES "^[a-z][a-z0-9_.]*$")
      _bioseq_highway_fail(
        "invalid authored module '${_authored_module}' for ${_key}")
    endif()
    if(NOT _partition MATCHES "^[a-z][a-z0-9_]*$")
      _bioseq_highway_fail(
        "invalid generated partition '${_partition}' for ${_key}")
    endif()
    if(_partition STREQUAL "index" OR _partition STREQUAL "binder")
      _bioseq_highway_fail(
        "generated partition '${_partition}' for ${_key} collides with a "
        "generator-owned module filename")
    endif()
    if(NOT _family MATCHES "^[a-z][a-z0-9_]*$")
      _bioseq_highway_fail("invalid source family '${_family}' for ${_key}")
    endif()
    get_filename_component(_source "${_source}" ABSOLUTE)
    if(NOT EXISTS "${_source}")
      _bioseq_highway_fail("source ${_key} does not exist: ${_source}")
    endif()
    if(NOT _source MATCHES "\\.cppm$")
      _bioseq_highway_fail(
        "source ${_key} must be a .cppm module generator input: ${_source}")
    endif()
    list(FIND _source_keys "${_key}" _duplicate_key_index)
    list(FIND _source_paths "${_source}" _duplicate_path_index)
    list(FIND _authored_modules "${_authored_module}"
      _duplicate_authored_module_index)
    list(FIND _partitions "${_partition}" _duplicate_partition_index)
    if(NOT _duplicate_key_index EQUAL -1)
      _bioseq_highway_fail("duplicate source key '${_key}'")
    elseif(NOT _duplicate_path_index EQUAL -1)
      _bioseq_highway_fail("duplicate source path '${_source}'")
    elseif(NOT _duplicate_authored_module_index EQUAL -1)
      _bioseq_highway_fail(
        "duplicate authored module '${_authored_module}'")
    elseif(NOT _duplicate_partition_index EQUAL -1)
      _bioseq_highway_fail(
        "duplicate generated partition '${_partition}'")
    endif()
    list(APPEND _source_keys "${_key}")
    list(APPEND _source_paths "${_source}")
    list(APPEND _authored_modules "${_authored_module}")
    list(APPEND _partitions "${_partition}")
    set(_dependencies)
    if(NOT _dependencies_csv STREQUAL "")
      string(REPLACE "," ";" _dependencies "${_dependencies_csv}")
      foreach(_dependency IN LISTS _dependencies)
        if(NOT _dependency MATCHES "^[a-z][a-z0-9_]*$")
          _bioseq_highway_fail(
            "invalid dependency '${_dependency}' for source ${_key}")
        endif()
        if(_dependency STREQUAL _key)
          _bioseq_highway_fail("source ${_key} depends on itself")
        endif()
      endforeach()
      list(LENGTH _dependencies _dependency_count)
      list(REMOVE_DUPLICATES _dependencies)
      list(LENGTH _dependencies _unique_dependency_count)
      if(NOT _dependency_count EQUAL _unique_dependency_count)
        _bioseq_highway_fail(
          "source ${_key} contains a duplicate dependency")
      endif()
      list(SORT _dependencies)
      string(JOIN "," _dependencies_csv ${_dependencies})
    endif()
    set("_bioseq_highway_dependencies_${_key}" "${_dependencies}")
    list(APPEND _normalized_source_records
      "${_key}|${_source}|${_authored_module}|${_partition}|${_family}|${_dependencies_csv}")
  endforeach()

  list(SORT _normalized_source_records)
  foreach(_record IN LISTS _normalized_source_records)
    if(NOT _record MATCHES
        "^([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]*)$")
      _bioseq_highway_fail("internal malformed source record '${_record}'")
    endif()
    set(_key "${CMAKE_MATCH_1}")
    set(_dependencies_csv "${CMAKE_MATCH_6}")
    if(NOT _dependencies_csv STREQUAL "")
      string(REPLACE "," ";" _dependencies "${_dependencies_csv}")
      foreach(_dependency IN LISTS _dependencies)
        list(FIND _source_keys "${_dependency}" _dependency_index)
        if(_dependency_index EQUAL -1)
          _bioseq_highway_fail(
            "source ${_key} names unknown dependency ${_dependency}")
        endif()
      endforeach()
    endif()
  endforeach()

  # Fail at configure time on cycles instead of leaving the module scanner to
  # report an opaque build-time dependency error.
  set(_remaining_source_keys ${_source_keys})
  set(_resolved_source_keys)
  list(LENGTH _remaining_source_keys _remaining_source_count)
  while(_remaining_source_count GREATER 0)
    set(_progress FALSE)
    set(_next_remaining_source_keys)
    foreach(_key IN LISTS _remaining_source_keys)
      set(_ready TRUE)
      foreach(_dependency IN LISTS _bioseq_highway_dependencies_${_key})
        list(FIND _resolved_source_keys "${_dependency}" _resolved_index)
        if(_resolved_index EQUAL -1)
          set(_ready FALSE)
          break()
        endif()
      endforeach()
      if(_ready)
        list(APPEND _resolved_source_keys "${_key}")
        set(_progress TRUE)
      else()
        list(APPEND _next_remaining_source_keys "${_key}")
      endif()
    endforeach()
    if(NOT _progress)
      string(JOIN ", " _cycle_keys ${_next_remaining_source_keys})
      _bioseq_highway_fail(
        "source dependency graph contains a cycle involving: ${_cycle_keys}")
    endif()
    set(_remaining_source_keys ${_next_remaining_source_keys})
    list(LENGTH _remaining_source_keys _remaining_source_count)
  endwhile()
  set(_normalized_slot_records)
  set(_slot_keys)
  set(_whole_slot_families)
  set(_partial_slot_families)
  foreach(_record IN LISTS ARG_SLOT_RECORDS)
    _bioseq_highway_reject_control_characters("slot record" "${_record}")
    if(NOT _record MATCHES
        "^([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]+)$")
      _bioseq_highway_fail(
        "malformed slot record '${_record}'; expected "
        "owner|family_field|slot_field|qualified_symbol")
    endif()
    set(_owner "${CMAKE_MATCH_1}")
    set(_family_field "${CMAKE_MATCH_2}")
    set(_slot_field "${CMAKE_MATCH_3}")
    set(_symbol "${CMAKE_MATCH_4}")
    foreach(_identifier IN ITEMS _owner _family_field)
      if(NOT ${_identifier} MATCHES "^[a-z][a-z0-9_]*$")
        _bioseq_highway_fail(
          "invalid slot identifier '${${_identifier}}' in '${_record}'")
      endif()
    endforeach()
    if(NOT _slot_field STREQUAL "@"
        AND NOT _slot_field MATCHES "^[a-z][a-z0-9_]*$")
      _bioseq_highway_fail(
        "invalid slot field '${_slot_field}' in '${_record}'")
    endif()
    list(FIND _source_keys "${_owner}" _owner_index)
    if(_owner_index EQUAL -1)
      _bioseq_highway_fail(
        "slot owner '${_owner}' is not a registered source key")
    endif()
    string(REGEX MATCHALL "\\{target_namespace\\}" _placeholders "${_symbol}")
    list(LENGTH _placeholders _placeholder_count)
    if(NOT _placeholder_count EQUAL 1)
      _bioseq_highway_fail(
        "slot symbol must contain {target_namespace} exactly once: "
        "${_symbol}")
    endif()
    set(_slot_key "${_family_field}|${_slot_field}")
    list(FIND _slot_keys "${_slot_key}" _slot_index)
    if(NOT _slot_index EQUAL -1)
      _bioseq_highway_fail("duplicate slot ${_slot_key}")
    endif()
    list(APPEND _slot_keys "${_slot_key}")
    if(_slot_field STREQUAL "@")
      list(APPEND _whole_slot_families "${_family_field}")
    else()
      list(APPEND _partial_slot_families "${_family_field}")
    endif()
    list(APPEND _normalized_slot_records
      "${_owner}|${_family_field}|${_slot_field}|${_symbol}")
  endforeach()
  list(SORT _normalized_slot_records)
  list(REMOVE_DUPLICATES _whole_slot_families)
  list(REMOVE_DUPLICATES _partial_slot_families)
  foreach(_family IN LISTS _whole_slot_families)
    list(FIND _partial_slot_families "${_family}" _partial_family_index)
    if(NOT _partial_family_index EQUAL -1)
      _bioseq_highway_fail(
        "slot family '${_family}' mixes whole-family and member records")
    endif()
  endforeach()

  set(_normalized_required_slot_records)
  set(_required_slot_keys)
  foreach(_record IN LISTS ARG_REQUIRED_SLOT_RECORDS)
    _bioseq_highway_reject_control_characters(
      "required slot record" "${_record}")
    if(NOT _record MATCHES "^([^|]+)\\|([^|]+)$")
      _bioseq_highway_fail(
        "malformed required slot '${_record}'; expected family|slot")
    endif()
    set(_family_field "${CMAKE_MATCH_1}")
    set(_slot_field "${CMAKE_MATCH_2}")
    if(NOT _family_field MATCHES "^[a-z][a-z0-9_]*$"
        OR (NOT _slot_field STREQUAL "@"
          AND NOT _slot_field MATCHES "^[a-z][a-z0-9_]*$"))
      _bioseq_highway_fail("invalid required slot '${_record}'")
    endif()
    set(_required_slot_key "${_family_field}|${_slot_field}")
    list(FIND _required_slot_keys "${_required_slot_key}"
      _required_slot_index)
    if(NOT _required_slot_index EQUAL -1)
      _bioseq_highway_fail("duplicate required slot '${_record}'")
    endif()
    list(APPEND _required_slot_keys "${_required_slot_key}")
    list(APPEND _normalized_required_slot_records
      "${_family_field}|${_slot_field}")
  endforeach()
  list(SORT _normalized_required_slot_records)
  if(ARG_REQUIRE_COMPLETE_TABLE)
    set(_require_complete_table 1)
    if(NOT _normalized_required_slot_records)
      _bioseq_highway_fail(
        "REQUIRE_COMPLETE_TABLE requires a non-empty explicit "
        "REQUIRED_SLOT_RECORDS registry")
    endif()
  else()
    set(_require_complete_table 0)
  endif()
  if(_normalized_required_slot_records)
    set(_actual_slot_keys ${_slot_keys})
    set(_expected_slot_keys ${_required_slot_keys})
    list(SORT _actual_slot_keys)
    list(SORT _expected_slot_keys)
    if(NOT _actual_slot_keys STREQUAL _expected_slot_keys)
      _bioseq_highway_fail(
        "slot ownership does not exactly cover REQUIRED_SLOT_RECORDS")
    endif()
  endif()

  set(_source_manifest_contents "bioseq-highway-sources-v1\n")
  string(APPEND _source_manifest_contents
    "contract\t${ARG_CONTRACT_MODULE}\n"
    "dispatch\t${ARG_DISPATCH_MODULE}\n"
    "require-complete-table\t${_require_complete_table}\n")
  foreach(_domain_topology_record IN LISTS _domain_topology_records)
    string(APPEND _source_manifest_contents
      "${_domain_topology_record}\n")
  endforeach()
  foreach(_record IN LISTS _normalized_source_records)
    string(REPLACE "|" "\t" _tsv_record "${_record}")
    string(APPEND _source_manifest_contents "source\t${_tsv_record}\n")
  endforeach()
  foreach(_record IN LISTS _normalized_slot_records)
    string(REPLACE "|" "\t" _tsv_record "${_record}")
    string(APPEND _source_manifest_contents "slot\t${_tsv_record}\n")
  endforeach()
  foreach(_record IN LISTS _normalized_required_slot_records)
    string(REPLACE "|" "\t" _tsv_record "${_record}")
    string(APPEND _source_manifest_contents
      "required-slot\t${_tsv_record}\n")
  endforeach()
  _bioseq_highway_write_if_different(
    "${_source_manifest}" "${_source_manifest_contents}")

  foreach(_source IN LISTS _source_paths)
    cmake_path(IS_PREFIX CMAKE_SOURCE_DIR "${_source}"
      NORMALIZE _source_in_main_tree)
    if(NOT _source_in_main_tree)
      _bioseq_highway_fail(
        "scan-only Highway input must reside beneath CMAKE_SOURCE_DIR: "
        "${_source}")
    endif()
    set_source_files_properties("${_source}" PROPERTIES
      BIOSEQ_HIGHWAY_GENERATOR_INPUT TRUE)
  endforeach()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_source_paths})

  set(_generated_modules)
  set(_generated_primary_modules)
  set(_generated_target_implementations)
  set(_generated_test_primary_module)
  set(_generated_test_implementations)
  foreach(_target_record IN LISTS _target_records)
    if(NOT _target_record MATCHES
        "^([^|]+)\\|([^|]+)\\|([^|]+)\\|([^|]+)\\|([0-9]+)$")
      _bioseq_highway_fail(
        "internal malformed normalized target '${_target_record}'")
    endif()
    set(_target_dir "${_output_root}/${CMAKE_MATCH_1}/${CMAKE_MATCH_2}")
    list(APPEND _generated_primary_modules "${_target_dir}/index.cppm")
    list(APPEND _generated_target_implementations
      "${_target_dir}/binder.cppm")
    if(_target_record STREQUAL _static_target_record)
      set(_generated_test_primary_module "${_target_dir}/index.cppm")
      list(APPEND _generated_test_implementations
        "${_target_dir}/binder.cppm")
    endif()
    foreach(_partition IN LISTS _partitions)
      list(APPEND _generated_target_implementations
        "${_target_dir}/${_partition}.cppm")
      if(_target_record STREQUAL _static_target_record)
        list(APPEND _generated_test_implementations
          "${_target_dir}/${_partition}.cppm")
      endif()
    endforeach()
  endforeach()
  set(_dispatch_registry "${_output_root}/highway_registry.cppm")
  set(_dispatch_inventory "${_output_root}/dispatch_inventory.cppm")
  set(_test_support "${_output_root}/test_support.cppm")
  if(NOT _generated_test_primary_module
      OR NOT _generated_test_implementations)
    _bioseq_highway_fail(
      "could not derive the fixed-target test module closure")
  endif()
  list(APPEND _generated_modules
    ${_generated_primary_modules}
    ${_generated_target_implementations}
    "${_dispatch_registry}"
    "${_dispatch_inventory}"
    "${_test_support}")
  list(REMOVE_DUPLICATES _generated_modules)
  set_source_files_properties(${_generated_modules} PROPERTIES GENERATED TRUE)

  set(_pcm_metadata_dir "${_pcm_root}/metadata")
  set(_owned_compile_database
    "${_pcm_metadata_dir}/owned_compile_commands.json")
  set(_pcm_manifest "${_pcm_metadata_dir}/pcm-manifest.json")
  set(_prepare_depfile "${_pcm_metadata_dir}/prepare.d")
  set(_prepare_stamp "${_pcm_metadata_dir}/prepare.stamp")
  set(_prepare_target "${ARG_TARGET}_prepare")
  if(TARGET "${_prepare_target}")
    _bioseq_highway_fail("prepare target ${_prepare_target} already exists")
  endif()
  add_custom_command(
    OUTPUT
      "${_owned_compile_database}"
      "${_pcm_manifest}"
      "${_prepare_stamp}"
    BYPRODUCTS "${_prepare_depfile}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${_pcm_metadata_dir}"
    COMMAND "$<TARGET_FILE:bioseq_codegen>" highway prepare
      --source-manifest "${_source_manifest}"
      --signature-source "${_signature_source}"
      ${_domain_topology_cli_arguments}
      --raw-compile-database "${_compile_database}"
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
      --pcm-root "${_pcm_root}"
      --compile-database-output "${_owned_compile_database}"
      --pcm-manifest "${_pcm_manifest}"
      --depfile "${_prepare_depfile}"
      --stamp "${_prepare_stamp}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${_source_manifest}"
      "${_compile_database}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${_signature_source}"
      ${_domain_topology_paths}
      ${_source_paths}
    DEPFILE "${_prepare_depfile}"
    COMMENT
      "Preparing isolated Clang PCM authority for Highway target templates"
    VERBATIM)
  add_custom_target("${_prepare_target}"
    DEPENDS
      "${_owned_compile_database}"
      "${_pcm_manifest}"
      "${_prepare_stamp}")
  add_dependencies("${_prepare_target}" bioseq_codegen_project)

  add_custom_command(
    OUTPUT ${_generated_modules} "${_artifact_manifest}" "${_stamp}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${_output_root}" "${_pcm_root}"
    COMMAND "$<TARGET_FILE:bioseq_codegen>" highway generate
      --target-manifest "${_target_manifest}"
      --source-manifest "${_source_manifest}"
      ${_domain_topology_cli_arguments}
      --compile-database "${_owned_compile_database}"
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
      --output-root "${_output_root}"
      --artifact-manifest "${_artifact_manifest}"
      --stamp "${_stamp}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${_target_manifest}"
      "${_source_manifest}"
      "${_prepare_stamp}"
      "${_owned_compile_database}"
      "${_pcm_manifest}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
      "${_signature_source}"
      ${_domain_topology_paths}
      ${_source_paths}
    COMMENT
      "Generating ${_platform} Highway target module graph "
      "(compiled mask ${_compiled_mask})"
    VERBATIM)
  add_custom_target("${ARG_TARGET}"
    DEPENDS ${_generated_modules} "${_artifact_manifest}" "${_stamp}")
  add_dependencies(
    "${ARG_TARGET}" bioseq_codegen_project "${_prepare_target}")
  set_target_properties("${ARG_POLICY_TARGET}" PROPERTIES
    BIOSEQ_HIGHWAY_COMPILED_MASK "${_compiled_mask}"
    BIOSEQ_HIGHWAY_POLICY_DIGEST "${_policy_digest}"
    BIOSEQ_HIGHWAY_TARGET_ORDER "${_target_order}"
    BIOSEQ_HIGHWAY_TARGET_RECORDS "${_target_records}")

  foreach(_output_pair IN ITEMS
      "${ARG_OUTPUT_MODULES}|${_generated_modules}"
      "${ARG_OUTPUT_PRIMARY_MODULES}|${_generated_primary_modules}"
      "${ARG_OUTPUT_TARGET_IMPLEMENTATIONS}|${_generated_target_implementations}"
      "${ARG_OUTPUT_DISPATCH_REGISTRY}|${_dispatch_registry}"
      "${ARG_OUTPUT_DISPATCH_INVENTORY}|${_dispatch_inventory}"
      "${ARG_OUTPUT_TEST_PRIMARY_MODULE}|${_generated_test_primary_module}"
      "${ARG_OUTPUT_TEST_IMPLEMENTATIONS}|${_generated_test_implementations}"
      "${ARG_OUTPUT_TEST_SUPPORT}|${_test_support}"
      "${ARG_OUTPUT_POLICY_TARGET}|${ARG_POLICY_TARGET}"
      "${ARG_OUTPUT_PREPARE_TARGET}|${_prepare_target}"
      "${ARG_OUTPUT_PREPARED_COMPILE_DATABASE}|${_owned_compile_database}"
      "${ARG_OUTPUT_PCM_MANIFEST}|${_pcm_manifest}"
      "${ARG_OUTPUT_SOURCE_INPUTS}|${_source_paths}"
      "${ARG_OUTPUT_TARGET_RECORDS}|${_target_records}"
      "${ARG_OUTPUT_TARGET_ORDER}|${_target_order}"
      "${ARG_OUTPUT_COMPILED_MASK}|${_compiled_mask}"
      "${ARG_OUTPUT_TARGET_MANIFEST}|${_target_manifest}"
      "${ARG_OUTPUT_SOURCE_MANIFEST}|${_source_manifest}"
      "${ARG_OUTPUT_ARTIFACT_MANIFEST}|${_artifact_manifest}")
    if(NOT _output_pair MATCHES "^([^|]*)\\|(.*)$")
      _bioseq_highway_fail("internal malformed output pair")
    endif()
    if(NOT CMAKE_MATCH_1 STREQUAL "")
      set("${CMAKE_MATCH_1}" "${CMAKE_MATCH_2}" PARENT_SCOPE)
    endif()
  endforeach()
endfunction()
