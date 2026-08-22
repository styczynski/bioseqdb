include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/bioseq_build_config_codegen.cmake")

function(_bioseq_template_flatten_fail message_text)
  message(FATAL_ERROR "Bioseq template-flatten codegen: ${message_text}")
endfunction()

function(_bioseq_template_flatten_scheduler_count_error profile expected actual
    output_variable)
  string(CONCAT _message
    "scheduler profile ${profile} must enable exactly "
    "${expected} heavy units; got ${actual}")
  set(${output_variable} "${_message}" PARENT_SCOPE)
endfunction()

function(_bioseq_template_flatten_json_quote value output_variable)
  set(_value "${value}")
  string(REPLACE "\\" "\\\\" _value "${_value}")
  string(REPLACE "\"" "\\\"" _value "${_value}")
  string(REPLACE "\n" "\\n" _value "${_value}")
  string(REPLACE "\r" "\\r" _value "${_value}")
  string(REPLACE "\t" "\\t" _value "${_value}")
  set(${output_variable} "\"${_value}\"" PARENT_SCOPE)
endfunction()

function(_bioseq_template_flatten_write_if_different path contents)
  if(EXISTS "${path}")
    file(READ "${path}" _existing)
    if("${_existing}" STREQUAL "${contents}")
      return()
    endif()
  endif()
  get_filename_component(_directory "${path}" DIRECTORY)
  file(MAKE_DIRECTORY "${_directory}")
  set(_temporary "${path}.configure-tmp")
  file(WRITE "${_temporary}" "${contents}")
  file(RENAME "${_temporary}" "${path}")
endfunction()

function(_bioseq_template_flatten_canonical_root path output_variable)
  if(NOT IS_ABSOLUTE "${path}")
    _bioseq_template_flatten_fail(
      "OUTPUT_ROOT must use one absolute spelling; got '${path}'")
  endif()
  cmake_path(NORMAL_PATH path OUTPUT_VARIABLE _normalized)
  if(NOT "${_normalized}" STREQUAL "${path}")
    _bioseq_template_flatten_fail(
      "OUTPUT_ROOT is not lexically canonical: '${path}'")
  endif()
  file(MAKE_DIRECTORY "${path}")
  file(REAL_PATH "${path}" _real)
  if(NOT "${_real}" STREQUAL "${path}")
    _bioseq_template_flatten_fail(
      "OUTPUT_ROOT must not use a symlink or alternate spelling: "
      "requested='${path}', canonical='${_real}'")
  endif()
  cmake_path(IS_PREFIX CMAKE_BINARY_DIR "${_real}"
    NORMALIZE _inside_build)
  if(NOT _inside_build OR _real STREQUAL CMAKE_BINARY_DIR)
    _bioseq_template_flatten_fail(
      "OUTPUT_ROOT must be an owned subdirectory of CMAKE_BINARY_DIR; "
      "got '${_real}'")
  endif()
  set(${output_variable} "${_real}" PARENT_SCOPE)
endfunction()

function(_bioseq_template_flatten_canonical_source path label output_variable)
  if(NOT IS_ABSOLUTE "${path}" OR NOT EXISTS "${path}"
      OR IS_DIRECTORY "${path}")
    _bioseq_template_flatten_fail(
      "${label} must name an existing absolute regular file; got '${path}'")
  endif()
  file(REAL_PATH "${path}" _real)
  cmake_path(NORMAL_PATH path OUTPUT_VARIABLE _normalized)
  if(NOT _real STREQUAL _normalized)
    _bioseq_template_flatten_fail(
      "${label} must use its canonical non-symlink spelling: "
      "requested='${path}', canonical='${_real}'")
  endif()
  set(${output_variable} "${_real}" PARENT_SCOPE)
endfunction()

function(_bioseq_template_flatten_parse_string_arguments
    argument_text output_variable)
  string(LENGTH "${argument_text}" _length)
  set(_cursor 0)
  set(_arguments)
  set(_expect_argument TRUE)
  while(_cursor LESS _length)
    string(SUBSTRING "${argument_text}" ${_cursor} 1 _character)
    while(_cursor LESS _length AND _character MATCHES "[ \t\r\n]")
      math(EXPR _cursor "${_cursor} + 1")
      if(_cursor LESS _length)
        string(SUBSTRING "${argument_text}" ${_cursor} 1 _character)
      endif()
    endwhile()
    if(_cursor EQUAL _length)
      if(_expect_argument AND _arguments)
        _bioseq_template_flatten_fail(
          "template_flatten argument list ends after a comma")
      endif()
      break()
    endif()
    if(NOT _expect_argument OR NOT _character STREQUAL "\"")
      _bioseq_template_flatten_fail(
        "template_flatten arguments must be separately quoted ordinary "
        "narrow string literals")
    endif()

    math(EXPR _cursor "${_cursor} + 1")
    set(_decoded "")
    set(_closed FALSE)
    while(_cursor LESS _length)
      string(SUBSTRING "${argument_text}" ${_cursor} 1 _character)
      if(_character STREQUAL "\"")
        set(_closed TRUE)
        math(EXPR _cursor "${_cursor} + 1")
        break()
      elseif(_character STREQUAL "\\")
        math(EXPR _cursor "${_cursor} + 1")
        if(_cursor EQUAL _length)
          _bioseq_template_flatten_fail(
            "template_flatten string literal ends in a backslash")
        endif()
        string(SUBSTRING "${argument_text}" ${_cursor} 1 _escaped)
        if(_escaped STREQUAL "\\" OR _escaped STREQUAL "\""
            OR _escaped STREQUAL "'" OR _escaped STREQUAL "?")
          string(APPEND _decoded "${_escaped}")
        else()
          _bioseq_template_flatten_fail(
            "template_flatten rejects control, numeric, and universal "
            "escapes; spell the replacement as printable UTF-8")
        endif()
      else()
        string(APPEND _decoded "${_character}")
      endif()
      math(EXPR _cursor "${_cursor} + 1")
    endwhile()
    if(NOT _closed)
      _bioseq_template_flatten_fail(
        "unterminated template_flatten string literal")
    endif()
    if(_decoded STREQUAL "" OR _decoded MATCHES "[;\n\r]")
      _bioseq_template_flatten_fail(
        "template_flatten values must be non-empty and cannot contain "
        "CMake list metacharacters or line breaks")
    endif()
    list(APPEND _arguments "${_decoded}")
    set(_expect_argument FALSE)

    while(_cursor LESS _length)
      string(SUBSTRING "${argument_text}" ${_cursor} 1 _character)
      if(NOT _character MATCHES "[ \t\r\n]")
        break()
      endif()
      math(EXPR _cursor "${_cursor} + 1")
    endwhile()
    if(_cursor LESS _length)
      if(NOT _character STREQUAL ",")
        _bioseq_template_flatten_fail(
          "template_flatten string literals must be comma-separated")
      endif()
      math(EXPR _cursor "${_cursor} + 1")
      set(_expect_argument TRUE)
    endif()
  endwhile()
  set(${output_variable} "${_arguments}" PARENT_SCOPE)
endfunction()

function(_bioseq_template_flatten_find_attribute_end
    source start_offset output_variable)
  string(LENGTH "${source}" _length)
  set(_cursor ${start_offset})
  set(_quoted FALSE)
  set(_escaped FALSE)
  set(_result -1)
  while(_cursor LESS _length)
    string(SUBSTRING "${source}" ${_cursor} 1 _character)
    if(_quoted)
      if(_escaped)
        set(_escaped FALSE)
      elseif(_character STREQUAL "\\")
        set(_escaped TRUE)
      elseif(_character STREQUAL "\"")
        set(_quoted FALSE)
      endif()
    elseif(_character STREQUAL "\"")
      set(_quoted TRUE)
    else()
      math(EXPR _remaining "${_length} - ${_cursor}")
      if(_remaining GREATER_EQUAL 3)
        string(SUBSTRING "${source}" ${_cursor} 3 _candidate)
        if(_candidate STREQUAL ")]]")
          set(_result ${_cursor})
          break()
        endif()
      endif()
    endif()
    math(EXPR _cursor "${_cursor} + 1")
  endwhile()
  set(${output_variable} ${_result} PARENT_SCOPE)
endfunction()

function(_bioseq_template_flatten_profile_enabled
    profile_ordinal alphabet_label width_label output_variable)
  set(_enabled TRUE)
  if(profile_ordinal EQUAL 0)
    if((NOT alphabet_label STREQUAL "" AND
        NOT alphabet_label STREQUAL "dna4") OR
       (NOT width_label STREQUAL "" AND NOT width_label STREQUAL "w8"))
      set(_enabled FALSE)
    endif()
  elseif(profile_ordinal EQUAL 1)
    if((NOT alphabet_label STREQUAL "" AND
        NOT (alphabet_label STREQUAL "dna4" OR
             alphabet_label STREQUAL "dna5")) OR
       (NOT width_label STREQUAL "" AND NOT width_label STREQUAL "w8"))
      set(_enabled FALSE)
    endif()
  endif()
  set(${output_variable} ${_enabled} PARENT_SCOPE)
endfunction()

function(_bioseq_template_flatten_resolve_emission_policy
    requested output_variable)
  if("${requested}" STREQUAL "")
    set(_resolved selector_api)
  elseif("${requested}" STREQUAL "selector_api" OR
      "${requested}" STREQUAL "implementation_owners")
    set(_resolved "${requested}")
  else()
    _bioseq_template_flatten_fail(
      "EMISSION_POLICY must be selector_api or implementation_owners; "
      "got '${requested}'")
  endif()
  set(${output_variable} "${_resolved}" PARENT_SCOPE)
endfunction()

# Performs bounded configure-time lexical topology discovery without rendering
# or compiling production C++.
function(bioseq_template_flatten_probe)
  cmake_parse_arguments(PARSE_ARGV 0 ARG ""
    "NAME;SOURCE;OUTPUT_ROOT;OUTPUT_MODULE;PROFILE;EMISSION_POLICY;OUT_CONFIGURE_TOPOLOGY_MANIFEST;OUT_TOPOLOGY_DIGEST;OUT_ENABLED_ORDINALS;OUT_ENABLED_SOURCES"
    "")
  foreach(_required IN ITEMS NAME SOURCE OUTPUT_ROOT OUTPUT_MODULE PROFILE
      OUT_CONFIGURE_TOPOLOGY_MANIFEST OUT_TOPOLOGY_DIGEST
      OUT_ENABLED_ORDINALS OUT_ENABLED_SOURCES)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      _bioseq_template_flatten_fail(
        "bioseq_template_flatten_probe requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_template_flatten_fail(
      "unknown probe arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()
  _bioseq_template_flatten_resolve_emission_policy(
    "${ARG_EMISSION_POLICY}" _emission_policy)
  if(NOT ARG_NAME MATCHES "^[a-z][a-z0-9_]*$")
    _bioseq_template_flatten_fail("invalid NAME '${ARG_NAME}'")
  endif()
  if(NOT ARG_OUTPUT_MODULE MATCHES
      "^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)+$")
    _bioseq_template_flatten_fail(
      "invalid primary OUTPUT_MODULE '${ARG_OUTPUT_MODULE}'")
  endif()
  _bioseq_build_config_profile("${ARG_PROFILE}"
    _bioseq_template_flatten_profile_index
    _bioseq_template_flatten_all_alphabets
    _bioseq_template_flatten_all_coordinates
    _bioseq_template_flatten_adaptive_lengths
    _bioseq_template_flatten_extended_coordinates
    _bioseq_template_flatten_full_specialization
    _bioseq_template_flatten_expected_scheduler_units)

  _bioseq_template_flatten_canonical_source(
    "${ARG_SOURCE}" SOURCE _source_path)
  _bioseq_template_flatten_canonical_root(
    "${ARG_OUTPUT_ROOT}" _output_root)
  set(_configure_manifest "${_output_root}/topology.configure.json")
  if(_emission_policy STREQUAL "selector_api")
    set(_api_partition "${_output_root}/typed_flat_api.cppm")
    set(_selector_partition "${_output_root}/typed_flat_selector.cppm")
  else()
    set(_api_partition "")
    set(_selector_partition "")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_source_path}")
  file(READ "${_source_path}" _source)
  file(SHA256 "${_source_path}" _source_sha256)

  string(REGEX MATCH
    "export[ \t\r\n]+module[ \t\r\n]+([A-Za-z_][A-Za-z0-9_.]*)[ \t\r\n]*;"
    _source_module_match "${_source}")
  if(NOT _source_module_match)
    _bioseq_template_flatten_fail(
      "SOURCE must contain one source-spelled export module declaration")
  endif()
  set(_source_module "${CMAKE_MATCH_1}")

  set(_marker "[[bioseq::template_flatten(")
  string(LENGTH "${_marker}" _marker_length)
  string(LENGTH "${_source}" _source_length)
  set(_search_offset 0)
  set(_groups)
  set(_binding_count 0)
  set(_binding_names)
  while(_search_offset LESS _source_length)
    string(SUBSTRING "${_source}" ${_search_offset} -1 _tail)
    string(FIND "${_tail}" "${_marker}" _relative_marker)
    if(_relative_marker EQUAL -1)
      break()
    endif()
    math(EXPR _marker_offset "${_search_offset} + ${_relative_marker}")
    math(EXPR _arguments_offset "${_marker_offset} + ${_marker_length}")
    _bioseq_template_flatten_find_attribute_end(
      "${_source}" ${_arguments_offset} _attribute_end)
    if(_attribute_end EQUAL -1)
      _bioseq_template_flatten_fail(
        "unterminated template_flatten attribute in ${_source_path}")
    endif()
    math(EXPR _argument_length "${_attribute_end} - ${_arguments_offset}")
    string(SUBSTRING "${_source}" ${_arguments_offset}
      ${_argument_length} _argument_text)
    _bioseq_template_flatten_parse_string_arguments(
      "${_argument_text}" _arguments)
    list(LENGTH _arguments _argument_count)
    if(_argument_count LESS 4)
      _bioseq_template_flatten_fail(
        "template_flatten requires role, group, and at least one "
        "label/replacement pair")
    endif()
    math(EXPR _pair_remainder "(${_argument_count} - 2) % 2")
    if(NOT _pair_remainder EQUAL 0)
      _bioseq_template_flatten_fail(
        "template_flatten label/replacement arguments are not paired")
    endif()
    list(GET _arguments 0 _role)
    list(GET _arguments 1 _group)
    if(NOT (_role STREQUAL "identity" OR _role STREQUAL "shard"))
      _bioseq_template_flatten_fail(
        "template_flatten role must be identity or shard; got '${_role}'")
    endif()
    if(NOT _group MATCHES "^[a-z][a-z0-9_]*$")
      _bioseq_template_flatten_fail(
        "invalid template_flatten group '${_group}'")
    endif()

    string(SUBSTRING "${_source}" 0 ${_marker_offset} _source_prefix)
    string(REGEX MATCH "([A-Za-z_][A-Za-z0-9_]*)[ \t\r\n]*$"
      _binding_match "${_source_prefix}")
    if(NOT _binding_match)
      _bioseq_template_flatten_fail(
        "cannot identify the declaration name immediately before a "
        "template_flatten attribute")
    endif()
    set(_binding_name "${CMAKE_MATCH_1}")
    if(_binding_name IN_LIST _binding_names)
      _bioseq_template_flatten_fail(
        "duplicate template_flatten binding '${_binding_name}'")
    endif()
    list(APPEND _binding_names "${_binding_name}")

    set(_labels)
    set(_replacements)
    set(_argument_index 2)
    while(_argument_index LESS _argument_count)
      list(GET _arguments ${_argument_index} _label)
      math(EXPR _replacement_index "${_argument_index} + 1")
      list(GET _arguments ${_replacement_index} _replacement)
      if(NOT _label MATCHES "^[a-z][a-z0-9_]*$")
        _bioseq_template_flatten_fail(
          "invalid case label '${_label}' in group '${_group}'")
      endif()
      if(_label IN_LIST _labels)
        _bioseq_template_flatten_fail(
          "duplicate case label '${_label}' in group '${_group}'")
      endif()
      if(_replacement STREQUAL "" OR _replacement MATCHES
          "(#|//|/\\*|\\*/|;|\n|\r)")
        _bioseq_template_flatten_fail(
          "replacement for ${_group}.${_label} contains a forbidden "
          "directive/comment/terminator")
      endif()
      list(APPEND _labels "${_label}")
      list(APPEND _replacements "${_replacement}")
      math(EXPR _argument_index "${_argument_index} + 2")
    endwhile()

    set(_group_role_variable "_bioseq_tf_group_${_group}_role")
    set(_group_labels_variable "_bioseq_tf_group_${_group}_labels")
    set(_group_bindings_variable "_bioseq_tf_group_${_group}_bindings")
    if(DEFINED ${_group_role_variable})
      if(NOT "${${_group_role_variable}}" STREQUAL "${_role}")
        _bioseq_template_flatten_fail(
          "zipped group '${_group}' mixes axis roles")
      endif()
      if(NOT "${${_group_labels_variable}}" STREQUAL "${_labels}")
        _bioseq_template_flatten_fail(
          "zipped group '${_group}' must repeat the exact ordered case labels")
      endif()
    else()
      list(APPEND _groups "${_group}")
      set(${_group_role_variable} "${_role}")
      set(${_group_labels_variable} "${_labels}")
    endif()
    list(APPEND ${_group_bindings_variable} ${_binding_count})
    set(_binding_${_binding_count}_name "${_binding_name}")
    set(_binding_${_binding_count}_group "${_group}")
    set(_binding_${_binding_count}_role "${_role}")
    set(_binding_${_binding_count}_labels "${_labels}")
    set(_binding_${_binding_count}_replacements "${_replacements}")
    math(EXPR _binding_count "${_binding_count} + 1")
    math(EXPR _search_offset "${_attribute_end} + 3")
  endwhile()
  if(_binding_count EQUAL 0)
    _bioseq_template_flatten_fail(
      "SOURCE has no canonical [[bioseq::template_flatten(...)]] binding")
  endif()

  set(_coordinate_count 1)
  foreach(_group IN LISTS _groups)
    set(_labels_variable "_bioseq_tf_group_${_group}_labels")
    list(LENGTH ${_labels_variable} _case_count)
    math(EXPR _coordinate_count "${_coordinate_count} * ${_case_count}")
  endforeach()
  if(_coordinate_count GREATER 4096)
    _bioseq_template_flatten_fail(
      "coordinate product ${_coordinate_count} exceeds the ceiling 4096")
  endif()

  set(_canonical
    "bioseq-template-flatten-topology\nrevision=2\n")
  string(APPEND _canonical "output-module\t${ARG_OUTPUT_MODULE}\n")
  string(APPEND _canonical "emission-policy\t${_emission_policy}\n")
  set(_group_ordinal 0)
  foreach(_group IN LISTS _groups)
    set(_role_variable "_bioseq_tf_group_${_group}_role")
    set(_bindings_variable "_bioseq_tf_group_${_group}_bindings")
    string(APPEND _canonical
      "group\t${_group_ordinal}\t${${_role_variable}}\t${_group}\n")
    set(_binding_in_group 0)
    foreach(_binding_index IN LISTS ${_bindings_variable})
      string(APPEND _canonical
        "binding\t${_group_ordinal}\t${_binding_in_group}\t${_binding_${_binding_index}_name}\n")
      list(LENGTH _binding_${_binding_index}_labels _case_count)
      math(EXPR _last_case "${_case_count} - 1")
      foreach(_case_ordinal RANGE 0 ${_last_case})
        list(GET _binding_${_binding_index}_labels ${_case_ordinal} _label)
        list(GET _binding_${_binding_index}_replacements
          ${_case_ordinal} _replacement)
        string(APPEND _canonical
          "case\t${_group_ordinal}\t${_binding_in_group}\t${_case_ordinal}\t${_label}\t${_replacement}\n")
      endforeach()
      math(EXPR _binding_in_group "${_binding_in_group} + 1")
    endforeach()
    math(EXPR _group_ordinal "${_group_ordinal} + 1")
  endforeach()

  set(_coordinate_records)
  set(_enabled_ordinals)
  set(_enabled_sources)
  math(EXPR _last_coordinate "${_coordinate_count} - 1")
  foreach(_ordinal RANGE 0 ${_last_coordinate})
    string(APPEND _canonical "coordinate\t${_ordinal}")
    set(_remainder ${_ordinal})
    set(_assignments)
    set(_alphabet "")
    set(_width "")
    set(_group_ordinal 0)
    foreach(_group IN LISTS _groups)
      set(_stride 1)
      set(_later_ordinal 0)
      foreach(_later_group IN LISTS _groups)
        if(_later_ordinal GREATER _group_ordinal)
          set(_later_labels_variable
            "_bioseq_tf_group_${_later_group}_labels")
          list(LENGTH ${_later_labels_variable} _later_case_count)
          math(EXPR _stride "${_stride} * ${_later_case_count}")
        endif()
        math(EXPR _later_ordinal "${_later_ordinal} + 1")
      endforeach()
      set(_labels_variable "_bioseq_tf_group_${_group}_labels")
      list(LENGTH ${_labels_variable} _case_count)
      math(EXPR _case_ordinal "(${_ordinal} / ${_stride}) % ${_case_count}")
      list(GET ${_labels_variable} ${_case_ordinal} _label)
      set(_role_variable "_bioseq_tf_group_${_group}_role")
      list(APPEND _assignments
        "${_group}|${${_role_variable}}|${_label}|${_case_ordinal}")
      string(APPEND _canonical "\t${_group}=${_label}")
      if(_group STREQUAL "alphabet")
        set(_alphabet "${_label}")
      elseif(_group STREQUAL "width")
        set(_width "${_label}")
      endif()
      math(EXPR _group_ordinal "${_group_ordinal} + 1")
    endforeach()
    string(APPEND _canonical "\n")
    _bioseq_template_flatten_profile_enabled(
      "${_bioseq_template_flatten_profile_index}"
      "${_alphabet}" "${_width}" _enabled)
    set(_relative_path "instance_${_ordinal}.cppm")
    string(REPLACE ";" "," _assignments_encoded "${_assignments}")
    list(APPEND _coordinate_records
      "${_ordinal}~${_relative_path}~${_enabled}~${_assignments_encoded}")
    if(_enabled)
      list(APPEND _enabled_ordinals ${_ordinal})
      list(APPEND _enabled_sources "${_output_root}/${_relative_path}")
    endif()
  endforeach()
  string(SHA256 _topology_digest "${_canonical}")

  if(ARG_NAME STREQUAL "dispatch_processor_typed" AND
      _emission_policy STREQUAL "selector_api")
    list(LENGTH _enabled_ordinals _enabled_count)
    if(NOT _enabled_count EQUAL
        _bioseq_template_flatten_expected_scheduler_units)
      _bioseq_template_flatten_scheduler_count_error(
        "${ARG_PROFILE}"
        "${_bioseq_template_flatten_expected_scheduler_units}"
        "${_enabled_count}"
        _scheduler_count_error)
      _bioseq_template_flatten_fail("${_scheduler_count_error}")
    endif()
  endif()

  _bioseq_template_flatten_json_quote(
    "bioseq-namespace-materialization" _q_generator_identity)
  _bioseq_template_flatten_json_quote("template_flatten" _q_policy)
  _bioseq_template_flatten_json_quote(
    "${_emission_policy}" _q_emission_policy)
  _bioseq_template_flatten_json_quote("${ARG_NAME}" _q_name)
  _bioseq_template_flatten_json_quote("${_source_path}" _q_source)
  _bioseq_template_flatten_json_quote("${_source_sha256}" _q_source_sha)
  _bioseq_template_flatten_json_quote("${_output_root}" _q_root)
  _bioseq_template_flatten_json_quote("${ARG_OUTPUT_MODULE}" _q_module)
  _bioseq_template_flatten_json_quote("${ARG_PROFILE}" _q_profile)
  _bioseq_template_flatten_json_quote("${_topology_digest}" _q_digest)
  if(_emission_policy STREQUAL "selector_api")
    _bioseq_template_flatten_json_quote("${_api_partition}" _q_api)
    _bioseq_template_flatten_json_quote(
      "${_selector_partition}" _q_selector)
  else()
    set(_q_api null)
    set(_q_selector null)
  endif()
  set(_json "{\n  \"schema_revision\": 2,\n")
  string(APPEND _json
    "  \"generator_identity\": ${_q_generator_identity},\n"
    "  \"policy\": ${_q_policy},\n"
    "  \"emission_policy\": ${_q_emission_policy},\n"
    "  \"name\": ${_q_name},\n"
    "  \"source_path\": ${_q_source},\n"
    "  \"source_sha256\": ${_q_source_sha},\n"
    "  \"output_root\": ${_q_root},\n"
    "  \"output_module\": ${_q_module},\n"
    "  \"profile\": ${_q_profile},\n"
    "  \"topology_digest\": ${_q_digest},\n"
    "  \"bindings\": [\n")
  if(_binding_count GREATER 0)
    math(EXPR _last_binding "${_binding_count} - 1")
    foreach(_binding_index RANGE 0 ${_last_binding})
      _bioseq_template_flatten_json_quote(
        "${_binding_${_binding_index}_name}" _q_binding_name)
      _bioseq_template_flatten_json_quote(
        "${_binding_${_binding_index}_group}" _q_binding_group)
      _bioseq_template_flatten_json_quote(
        "${_binding_${_binding_index}_role}" _q_binding_role)
      string(APPEND _json
        "    {\"ordinal\":${_binding_index},\"name\":${_q_binding_name},\"group\":${_q_binding_group},\"role\":${_q_binding_role},\"cases\":[")
      list(LENGTH _binding_${_binding_index}_labels _case_count)
      math(EXPR _last_case "${_case_count} - 1")
      foreach(_case_ordinal RANGE 0 ${_last_case})
        if(_case_ordinal GREATER 0)
          string(APPEND _json ",")
        endif()
        list(GET _binding_${_binding_index}_labels ${_case_ordinal} _label)
        list(GET _binding_${_binding_index}_replacements
          ${_case_ordinal} _replacement)
        _bioseq_template_flatten_json_quote("${_label}" _q_label)
        _bioseq_template_flatten_json_quote("${_replacement}" _q_replacement)
        string(APPEND _json
          "{\"ordinal\":${_case_ordinal},\"label\":${_q_label},\"replacement\":${_q_replacement}}")
      endforeach()
      if(_binding_index EQUAL _last_binding)
        string(APPEND _json "]}\n")
      else()
        string(APPEND _json "]},\n")
      endif()
    endforeach()
  endif()
  string(APPEND _json "  ],\n  \"groups\": [\n")
  list(LENGTH _groups _group_count)
  math(EXPR _last_group "${_group_count} - 1")
  set(_group_ordinal 0)
  foreach(_group IN LISTS _groups)
    set(_role_variable "_bioseq_tf_group_${_group}_role")
    set(_bindings_variable "_bioseq_tf_group_${_group}_bindings")
    list(GET ${_bindings_variable} 0 _first_binding)
    _bioseq_template_flatten_json_quote("${_group}" _q_group)
    _bioseq_template_flatten_json_quote("${${_role_variable}}" _q_role)
    string(APPEND _json
      "    {\"ordinal\":${_group_ordinal},\"name\":${_q_group},\"role\":${_q_role},\"binding_ordinals\":[")
    set(_first_binding_ordinal TRUE)
    foreach(_binding_ordinal IN LISTS ${_bindings_variable})
      if(NOT _first_binding_ordinal)
        string(APPEND _json ",")
      endif()
      string(APPEND _json "${_binding_ordinal}")
      set(_first_binding_ordinal FALSE)
    endforeach()
    string(APPEND _json "],\"cases\":[")
    list(LENGTH _binding_${_first_binding}_labels _case_count)
    math(EXPR _last_case "${_case_count} - 1")
    foreach(_case_ordinal RANGE 0 ${_last_case})
      if(_case_ordinal GREATER 0)
        string(APPEND _json ",")
      endif()
      list(GET _binding_${_first_binding}_labels ${_case_ordinal} _label)
      list(GET _binding_${_first_binding}_replacements
        ${_case_ordinal} _replacement)
      _bioseq_template_flatten_json_quote("${_label}" _q_label)
      _bioseq_template_flatten_json_quote("${_replacement}" _q_replacement)
      string(APPEND _json
        "{\"ordinal\":${_case_ordinal},\"label\":${_q_label},\"replacement\":${_q_replacement}}")
    endforeach()
    if(_group_ordinal EQUAL _last_group)
      string(APPEND _json "]}\n")
    else()
      string(APPEND _json "]},\n")
    endif()
    math(EXPR _group_ordinal "${_group_ordinal} + 1")
  endforeach()
  string(APPEND _json "  ],\n  \"coordinates\": [\n")
  set(_coordinate_index 0)
  foreach(_record IN LISTS _coordinate_records)
    if(NOT _record MATCHES "^([0-9]+)~([^~]+)~(TRUE|FALSE)~(.*)$")
      _bioseq_template_flatten_fail("internal malformed coordinate record")
    endif()
    set(_ordinal "${CMAKE_MATCH_1}")
    set(_relative_path "${CMAKE_MATCH_2}")
    set(_enabled "${CMAKE_MATCH_3}")
    set(_assignments "${CMAKE_MATCH_4}")
    string(REPLACE "," ";" _assignments "${_assignments}")
    _bioseq_template_flatten_json_quote("${_relative_path}" _q_relative)
    string(APPEND _json
      "    {\"ordinal\":${_ordinal},\"assignments\":[")
    set(_assignment_index 0)
    foreach(_assignment IN LISTS _assignments)
      if(NOT _assignment MATCHES "^([^|]+)\\|([^|]+)\\|([^|]+)\\|([0-9]+)$")
        _bioseq_template_flatten_fail("internal malformed assignment")
      endif()
      if(_assignment_index GREATER 0)
        string(APPEND _json ",")
      endif()
      _bioseq_template_flatten_json_quote("${CMAKE_MATCH_1}" _q_group)
      _bioseq_template_flatten_json_quote("${CMAKE_MATCH_2}" _q_role)
      _bioseq_template_flatten_json_quote("${CMAKE_MATCH_3}" _q_label)
      set(_case_ordinal "${CMAKE_MATCH_4}")
      string(APPEND _json
        "{\"group\":${_q_group},\"role\":${_q_role},\"case_ordinal\":${_case_ordinal},\"label\":${_q_label}}")
      math(EXPR _assignment_index "${_assignment_index} + 1")
    endforeach()
    string(TOLOWER "${_enabled}" _enabled_json)
    if(_enabled)
      _bioseq_template_flatten_json_quote(
        "${_output_root}/${_relative_path}" _q_coordinate_source)
    else()
      set(_q_coordinate_source "null")
    endif()
    string(APPEND _json
      "],\"enabled\":${_enabled_json},\"source\":${_q_coordinate_source}}")
    math(EXPR _coordinate_index "${_coordinate_index} + 1")
    if(_coordinate_index EQUAL _coordinate_count)
      string(APPEND _json "\n")
    else()
      string(APPEND _json ",\n")
    endif()
  endforeach()
  string(APPEND _json "  ],\n  \"enabled_ordinals\": [")
  set(_first TRUE)
  foreach(_ordinal IN LISTS _enabled_ordinals)
    if(NOT _first)
      string(APPEND _json ",")
    endif()
    string(APPEND _json "${_ordinal}")
    set(_first FALSE)
  endforeach()
  string(APPEND _json "],\n  \"enabled_sources\": [")
  set(_first TRUE)
  foreach(_enabled_source IN LISTS _enabled_sources)
    if(NOT _first)
      string(APPEND _json ",")
    endif()
    _bioseq_template_flatten_json_quote("${_enabled_source}" _q_enabled_source)
    string(APPEND _json "${_q_enabled_source}")
    set(_first FALSE)
  endforeach()
  string(APPEND _json
    "],\n  \"api_partition\": ${_q_api},\n"
    "  \"selector_partition\": ${_q_selector}\n}\n")
  _bioseq_template_flatten_write_if_different(
    "${_configure_manifest}" "${_json}")

  set_property(SOURCE "${_source_path}" PROPERTY
    BIOSEQ_TEMPLATE_FLATTEN_GENERATOR_INPUT TRUE)
  set_property(GLOBAL APPEND PROPERTY BIOSEQ_TEMPLATE_FLATTEN_INPUTS
    "${_source_path}")
  set(${ARG_OUT_CONFIGURE_TOPOLOGY_MANIFEST}
    "${_configure_manifest}" PARENT_SCOPE)
  set(${ARG_OUT_TOPOLOGY_DIGEST} "${_topology_digest}" PARENT_SCOPE)
  set(${ARG_OUT_ENABLED_ORDINALS} "${_enabled_ordinals}" PARENT_SCOPE)
  set(${ARG_OUT_ENABLED_SOURCES} "${_enabled_sources}" PARENT_SCOPE)
endfunction()

function(bioseq_template_flatten_read_group)
  cmake_parse_arguments(PARSE_ARGV 0 ARG ""
    "TOPOLOGY_MANIFEST;ROLE;GROUP;EMISSION_POLICY;OUT_LABELS;OUT_REPLACEMENTS;OUT_ORDINALS"
    "")
  foreach(_required IN ITEMS TOPOLOGY_MANIFEST ROLE GROUP OUT_LABELS
      OUT_REPLACEMENTS OUT_ORDINALS)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      _bioseq_template_flatten_fail(
        "bioseq_template_flatten_read_group requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_template_flatten_fail(
      "unknown read_group arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()
  _bioseq_template_flatten_resolve_emission_policy(
    "${ARG_EMISSION_POLICY}" _emission_policy)
  _bioseq_template_flatten_canonical_source(
    "${ARG_TOPOLOGY_MANIFEST}" TOPOLOGY_MANIFEST _manifest)
  file(READ "${_manifest}" _json)
  string(JSON _schema_type TYPE "${_json}" schema_revision)
  string(JSON _schema_revision GET "${_json}" schema_revision)
  string(JSON _generator_identity GET "${_json}" generator_identity)
  string(JSON _policy GET "${_json}" policy)
  string(JSON _recorded_emission_policy GET
    "${_json}" emission_policy)
  if(NOT _schema_type STREQUAL "NUMBER"
      OR NOT _schema_revision STREQUAL "2"
      OR NOT _generator_identity STREQUAL
        "bioseq-namespace-materialization"
      OR NOT _policy STREQUAL "template_flatten"
      OR NOT _recorded_emission_policy STREQUAL _emission_policy)
    _bioseq_template_flatten_fail(
      "configure topology identity, schema, or emission policy mismatch")
  endif()
  string(JSON _group_count LENGTH "${_json}" groups)
  set(_found FALSE)
  set(_labels)
  set(_replacements)
  set(_ordinals)
  if(_group_count GREATER 0)
    math(EXPR _last_group "${_group_count} - 1")
    foreach(_index RANGE 0 ${_last_group})
      string(JSON _name GET "${_json}" groups ${_index} name)
      if(NOT _name STREQUAL ARG_GROUP)
        continue()
      endif()
      if(_found)
        _bioseq_template_flatten_fail(
          "configure topology repeats group '${ARG_GROUP}'")
      endif()
      set(_found TRUE)
      string(JSON _role GET "${_json}" groups ${_index} role)
      if(NOT _role STREQUAL ARG_ROLE)
        _bioseq_template_flatten_fail(
          "group '${ARG_GROUP}' has role '${_role}', expected '${ARG_ROLE}'")
      endif()
      string(JSON _case_count LENGTH "${_json}" groups ${_index} cases)
      if(_case_count EQUAL 0)
        _bioseq_template_flatten_fail(
          "group '${ARG_GROUP}' has no cases")
      endif()
      math(EXPR _last_case "${_case_count} - 1")
      foreach(_case_index RANGE 0 ${_last_case})
        string(JSON _ordinal GET "${_json}"
          groups ${_index} cases ${_case_index} ordinal)
        string(JSON _label GET "${_json}"
          groups ${_index} cases ${_case_index} label)
        string(JSON _replacement GET "${_json}"
          groups ${_index} cases ${_case_index} replacement)
        if(NOT _ordinal EQUAL _case_index OR _label STREQUAL ""
            OR _replacement STREQUAL "" OR _label IN_LIST _labels
            OR _replacement MATCHES ";")
          _bioseq_template_flatten_fail(
            "group '${ARG_GROUP}' requires contiguous ordinals, unique "
            "labels, and non-empty list-safe replacements")
        endif()
        list(APPEND _ordinals "${_ordinal}")
        list(APPEND _labels "${_label}")
        list(APPEND _replacements "${_replacement}")
      endforeach()
    endforeach()
  endif()
  if(NOT _found)
    _bioseq_template_flatten_fail(
      "configure topology has no ${ARG_ROLE} group '${ARG_GROUP}'")
  endif()
  set(${ARG_OUT_LABELS} "${_labels}" PARENT_SCOPE)
  set(${ARG_OUT_REPLACEMENTS} "${_replacements}" PARENT_SCOPE)
  set(${ARG_OUT_ORDINALS} "${_ordinals}" PARENT_SCOPE)
endfunction()

function(bioseq_add_template_flatten_codegen)
  cmake_parse_arguments(PARSE_ARGV 0 ARG ""
    "NAME;SOURCE;OUTPUT_ROOT;OUTPUT_MODULE;EMISSION_POLICY;CONFIGURE_TOPOLOGY_MANIFEST;EXPECTED_TOPOLOGY_DIGEST;COMPILE_DATABASE;SIGNATURE_SOURCE;PCM_ROOT;OUT_API_PARTITION;OUT_SELECTOR_PARTITION;OUT_IMPLEMENTATION_UNITS;OUT_GENERATION_TARGET;OUT_ARTIFACT_MANIFEST"
    "ENABLED_ORDINALS;PREPARE_DEPENDS")
  foreach(_required IN ITEMS NAME SOURCE OUTPUT_ROOT OUTPUT_MODULE
      CONFIGURE_TOPOLOGY_MANIFEST EXPECTED_TOPOLOGY_DIGEST COMPILE_DATABASE
      SIGNATURE_SOURCE PCM_ROOT OUT_IMPLEMENTATION_UNITS
      OUT_GENERATION_TARGET OUT_ARTIFACT_MANIFEST)
    if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
      _bioseq_template_flatten_fail(
        "bioseq_add_template_flatten_codegen requires ${_required}")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_template_flatten_fail(
      "unknown add arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()
  _bioseq_template_flatten_resolve_emission_policy(
    "${ARG_EMISSION_POLICY}" _emission_policy)
  if(_emission_policy STREQUAL "selector_api")
    foreach(_required IN ITEMS OUT_API_PARTITION OUT_SELECTOR_PARTITION)
      if(NOT DEFINED ARG_${_required} OR "${ARG_${_required}}" STREQUAL "")
        _bioseq_template_flatten_fail(
          "selector_api emission requires ${_required}")
      endif()
    endforeach()
  elseif(DEFINED ARG_OUT_API_PARTITION OR
      DEFINED ARG_OUT_SELECTOR_PARTITION)
    _bioseq_template_flatten_fail(
      "implementation_owners emission does not accept API or selector outputs")
  endif()
  string(LENGTH "${ARG_EXPECTED_TOPOLOGY_DIGEST}" _digest_length)
  if(NOT _digest_length EQUAL 64
      OR NOT ARG_EXPECTED_TOPOLOGY_DIGEST MATCHES "^[0-9a-f]+$")
    _bioseq_template_flatten_fail(
      "EXPECTED_TOPOLOGY_DIGEST must be lowercase SHA-256")
  endif()
  if(NOT TARGET bioseq_codegen_project OR NOT TARGET bioseq_codegen
      OR NOT BIOSEQ_CODEGEN_EXECUTABLE)
    _bioseq_template_flatten_fail(
      "bioseq_add_codegen must provision bioseq_codegen before "
      "template-flatten registration")
  endif()

  _bioseq_template_flatten_canonical_root(
    "${ARG_OUTPUT_ROOT}" _output_root)
  if(_output_root MATCHES "/generated/scheduler/typed/?$")
    _bioseq_template_flatten_fail(
      "generated/scheduler/typed root is forbidden")
  endif()
  _bioseq_template_flatten_canonical_source(
    "${ARG_SOURCE}" SOURCE _source_path)
  _bioseq_template_flatten_canonical_source(
    "${ARG_SIGNATURE_SOURCE}" SIGNATURE_SOURCE _signature_source)
  set(_expected_manifest "${_output_root}/topology.configure.json")
  _bioseq_template_flatten_canonical_source(
    "${ARG_CONFIGURE_TOPOLOGY_MANIFEST}"
    CONFIGURE_TOPOLOGY_MANIFEST _configure_manifest)
  if(NOT _configure_manifest STREQUAL _expected_manifest)
    _bioseq_template_flatten_fail(
      "configure topology manifest must be exactly "
      "${_expected_manifest}; got ${_configure_manifest}")
  endif()

  if(NOT IS_ABSOLUTE "${ARG_PCM_ROOT}")
    _bioseq_template_flatten_fail("PCM_ROOT must be absolute")
  endif()
  cmake_path(NORMAL_PATH ARG_PCM_ROOT OUTPUT_VARIABLE _pcm_normalized)
  set(_expected_pcm_root "${_output_root}/pcm")
  if(NOT _pcm_normalized STREQUAL _expected_pcm_root)
    _bioseq_template_flatten_fail(
      "PCM_ROOT must be exactly '${_expected_pcm_root}'")
  endif()
  file(MAKE_DIRECTORY "${_pcm_normalized}")
  file(REAL_PATH "${_pcm_normalized}" _pcm_root)
  if(NOT _pcm_root STREQUAL _expected_pcm_root)
    _bioseq_template_flatten_fail("PCM_ROOT cannot use a symlink alias")
  endif()

  if(NOT IS_ABSOLUTE "${ARG_COMPILE_DATABASE}")
    _bioseq_template_flatten_fail("COMPILE_DATABASE must be absolute")
  endif()
  cmake_path(NORMAL_PATH ARG_COMPILE_DATABASE
    OUTPUT_VARIABLE _compile_database)
  set(_expected_compile_database "${CMAKE_BINARY_DIR}/compile_commands.json")
  if(NOT _compile_database STREQUAL _expected_compile_database)
    _bioseq_template_flatten_fail(
      "COMPILE_DATABASE must be the root compile database "
      "${_expected_compile_database}")
  endif()

  file(READ "${_configure_manifest}" _json)
  string(JSON _schema_type TYPE "${_json}" schema_revision)
  string(JSON _schema_revision GET "${_json}" schema_revision)
  string(JSON _generator_identity GET "${_json}" generator_identity)
  string(JSON _policy GET "${_json}" policy)
  string(JSON _recorded_emission_policy GET
    "${_json}" emission_policy)
  string(JSON _manifest_name GET "${_json}" name)
  if(NOT _schema_type STREQUAL "NUMBER"
      OR NOT _schema_revision STREQUAL "2"
      OR NOT _generator_identity STREQUAL
        "bioseq-namespace-materialization"
      OR NOT _policy STREQUAL "template_flatten"
      OR NOT _recorded_emission_policy STREQUAL _emission_policy
      OR NOT _manifest_name STREQUAL ARG_NAME)
    _bioseq_template_flatten_fail(
      "configure topology schema/generator/policy/emission/name "
      "authentication failed")
  endif()
  foreach(_manifest_pair IN ITEMS
      "source_path|${_source_path}"
      "output_root|${_output_root}"
      "output_module|${ARG_OUTPUT_MODULE}"
      "topology_digest|${ARG_EXPECTED_TOPOLOGY_DIGEST}")
    string(REPLACE "|" ";" _pair "${_manifest_pair}")
    list(GET _pair 0 _field)
    list(GET _pair 1 _expected)
    string(JSON _actual GET "${_json}" ${_field})
    if(NOT _actual STREQUAL _expected)
      _bioseq_template_flatten_fail(
        "configure manifest ${_field} mismatch: expected '${_expected}', "
        "recorded '${_actual}'")
    endif()
  endforeach()
  if(_emission_policy STREQUAL "selector_api")
    set(_api_partition "${_output_root}/typed_flat_api.cppm")
    set(_selector_partition "${_output_root}/typed_flat_selector.cppm")
    foreach(_partition_pair IN ITEMS
        "api_partition|${_api_partition}"
        "selector_partition|${_selector_partition}")
      string(REPLACE "|" ";" _pair "${_partition_pair}")
      list(GET _pair 0 _field)
      list(GET _pair 1 _expected)
      string(JSON _actual GET "${_json}" ${_field})
      if(NOT _actual STREQUAL _expected)
        _bioseq_template_flatten_fail(
          "configure manifest ${_field} is outside/divergent from OUTPUT_ROOT")
      endif()
    endforeach()
    set(_interface_sources "${_api_partition}" "${_selector_partition}")
  else()
    set(_api_partition "")
    set(_selector_partition "")
    foreach(_field IN ITEMS api_partition selector_partition)
      string(JSON _partition_type TYPE "${_json}" ${_field})
      if(NOT _partition_type STREQUAL "NULL")
        _bioseq_template_flatten_fail(
          "implementation_owners configure manifest ${_field} must be null")
      endif()
    endforeach()
    set(_interface_sources)
  endif()

  set(_manifest_ordinals)
  set(_manifest_sources)
  string(JSON _enabled_count LENGTH "${_json}" enabled_ordinals)
  if(_enabled_count GREATER 0)
    math(EXPR _last_enabled "${_enabled_count} - 1")
    foreach(_index RANGE 0 ${_last_enabled})
      string(JSON _ordinal GET "${_json}" enabled_ordinals ${_index})
      string(JSON _source GET "${_json}" enabled_sources ${_index})
      if(NOT _ordinal MATCHES "^[0-9]+$")
        _bioseq_template_flatten_fail("manifest enabled ordinal is invalid")
      endif()
      set(_predicted "${_output_root}/instance_${_ordinal}.cppm")
      if(NOT _source STREQUAL _predicted)
        _bioseq_template_flatten_fail(
          "manifest enabled source path diverges from ordinal ${_ordinal}")
      endif()
      list(APPEND _manifest_ordinals "${_ordinal}")
      list(APPEND _manifest_sources "${_predicted}")
    endforeach()
  endif()
  if(NOT "${ARG_ENABLED_ORDINALS}" STREQUAL "${_manifest_ordinals}")
    _bioseq_template_flatten_fail(
      "ENABLED_ORDINALS diverges from authenticated configure topology")
  endif()
  list(LENGTH _manifest_sources _source_count)
  if(NOT _source_count EQUAL _enabled_count)
    _bioseq_template_flatten_fail(
      "enabled ordinal/source arrays have different lengths")
  endif()
  string(JSON _coordinate_count LENGTH "${_json}" coordinates)
  if(_coordinate_count EQUAL 0)
    _bioseq_template_flatten_fail(
      "configure topology must contain at least one coordinate")
  endif()
  math(EXPR _last_coordinate "${_coordinate_count} - 1")
  foreach(_index RANGE 0 ${_last_coordinate})
    string(JSON _ordinal GET "${_json}" coordinates ${_index} ordinal)
    string(JSON _enabled GET "${_json}" coordinates ${_index} enabled)
    string(JSON _source_type TYPE "${_json}" coordinates ${_index} source)
    if(NOT _ordinal EQUAL _index)
      _bioseq_template_flatten_fail(
        "configure topology coordinate ordinals must be contiguous")
    endif()
    set(_predicted "${_output_root}/instance_${_ordinal}.cppm")
    if(_enabled)
      if(NOT _ordinal IN_LIST _manifest_ordinals
          OR NOT _source_type STREQUAL "STRING")
        _bioseq_template_flatten_fail(
          "enabled coordinate ${_ordinal} is absent from enabled ownership")
      endif()
      string(JSON _coordinate_source GET
        "${_json}" coordinates ${_index} source)
      if(NOT _coordinate_source STREQUAL _predicted)
        _bioseq_template_flatten_fail(
          "coordinate ${_ordinal} source path is not root-authenticated")
      endif()
    elseif(_ordinal IN_LIST _manifest_ordinals
        OR NOT _source_type STREQUAL "NULL")
      _bioseq_template_flatten_fail(
        "disabled coordinate ${_ordinal} claims a production source")
    endif()
  endforeach()

  file(READ "${_signature_source}" _signature_contents)
  string(REPLACE "." "\\." _output_module_regex "${ARG_OUTPUT_MODULE}")
  string(REGEX MATCH
    "export[ \t\r\n]+module[ \t\r\n]+${_output_module_regex}[ \t\r\n]*;"
    _signature_module "${_signature_contents}")
  if(NOT _signature_module)
    _bioseq_template_flatten_fail(
      "SIGNATURE_SOURCE must be the ${ARG_OUTPUT_MODULE} primary interface")
  endif()

  if(BIOSEQ_REPOSITORY_ROOT)
    file(REAL_PATH "${BIOSEQ_REPOSITORY_ROOT}" _repository_root)
  else()
    file(REAL_PATH "${CMAKE_SOURCE_DIR}" _repository_root)
  endif()
  string(JSON _profile GET "${_json}" profile)
  string(JOIN "," _enabled_csv ${_manifest_ordinals})
  if(_enabled_csv STREQUAL "")
    set(_enabled_csv "-")
  endif()

  set(_metadata_dir "${_pcm_root}/metadata")
  set(_prepared_compile_database
    "${_metadata_dir}/prepare_compile_commands.json")
  set(_pcm_manifest "${_metadata_dir}/pcm_manifest.json")
  set(_prepare_action_trace "${_metadata_dir}/prepare_actions.json")
  set(_prepare_depfile "${_metadata_dir}/prepare.d")
  set(_prepare_stamp "${_metadata_dir}/prepare.stamp")
  set(_topology_manifest "${_output_root}/topology.json")
  set(_artifact_manifest "${_output_root}/artifacts.json")
  set(_generate_depfile "${_output_root}/generate.d")
  set(_generate_stamp "${_output_root}/generate.stamp")
  set(_generation_outputs
    ${_interface_sources}
    ${_manifest_sources}
    "${_topology_manifest}"
    "${_artifact_manifest}"
    "${_generate_stamp}")
  set(_prepare_target "${ARG_NAME}_template_flatten_prepare")
  set(_generation_target "${ARG_NAME}_template_flatten_codegen")
  if(TARGET "${_prepare_target}" OR TARGET "${_generation_target}")
    _bioseq_template_flatten_fail(
      "template-flatten target for '${ARG_NAME}' already exists")
  endif()

  foreach(_tool_identity IN ITEMS
      BIOSEQ_CODEGEN_CLANG_BINARY BIOSEQ_CODEGEN_CLANG_SCAN_DEPS
      BIOSEQ_CODEGEN_LLVM_CONFIG BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON
      BIOSEQ_CODEGEN_CLANG_SHA256 BIOSEQ_CODEGEN_CLANG_SCAN_DEPS_SHA256
      BIOSEQ_CODEGEN_LLVM_CONFIG_SHA256
      BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_SHA256)
    if(NOT DEFINED ${_tool_identity} OR "${${_tool_identity}}" STREQUAL "")
      _bioseq_template_flatten_fail(
        "missing isolated toolchain identity ${_tool_identity}")
    endif()
  endforeach()

  add_custom_command(
    OUTPUT
      "${_prepared_compile_database}"
      "${_pcm_manifest}"
      "${_prepare_action_trace}"
      "${_prepare_stamp}"
    BYPRODUCTS "${_prepare_depfile}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${_metadata_dir}"
    COMMAND "$<TARGET_FILE:bioseq_codegen>"
      namespace_materialization prepare
      --source "${_source_path}"
      --signature-source "${_signature_source}"
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
      --compile-database-output "${_prepared_compile_database}"
      --pcm-manifest "${_pcm_manifest}"
      --action-trace "${_prepare_action_trace}"
      --depfile "${_prepare_depfile}"
      --stamp "${_prepare_stamp}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${_source_path}"
      "${_signature_source}"
      "${_configure_manifest}"
      "${_compile_database}"
      ${ARG_PREPARE_DEPENDS}
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    DEPFILE "${_prepare_depfile}"
    COMMENT "Preparing dependency-only template-flatten module closure"
    VERBATIM)
  add_custom_target("${_prepare_target}"
    DEPENDS
      "${_prepared_compile_database}"
      "${_pcm_manifest}"
      "${_prepare_action_trace}"
      "${_prepare_stamp}")
  add_dependencies("${_prepare_target}" bioseq_codegen_project)

  add_custom_command(
    OUTPUT ${_generation_outputs}
    BYPRODUCTS "${_generate_depfile}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory
      "${_output_root}" "${_pcm_root}"
    COMMAND "$<TARGET_FILE:bioseq_codegen>"
      namespace_materialization generate
      --name "${ARG_NAME}"
      --source "${_source_path}"
      --repository-root "${_repository_root}"
      --output-module "${ARG_OUTPUT_MODULE}"
      --emission-policy "${_emission_policy}"
      --profile "${_profile}"
      --configure-topology-manifest "${_configure_manifest}"
      --expected-topology-digest "${ARG_EXPECTED_TOPOLOGY_DIGEST}"
      --enabled-ordinals "${_enabled_csv}"
      --signature-source "${_signature_source}"
      --compile-database "${_prepared_compile_database}"
      --pcm-manifest "${_pcm_manifest}"
      --prepare-action-trace "${_prepare_action_trace}"
      --prepare-stamp "${_prepare_stamp}"
      --pcm-root "${_pcm_root}"
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
      --topology-manifest "${_topology_manifest}"
      --artifact-manifest "${_artifact_manifest}"
      --depfile "${_generate_depfile}"
      --stamp "${_generate_stamp}"
    DEPENDS
      bioseq_codegen_project
      "${BIOSEQ_CODEGEN_EXECUTABLE}"
      "${_source_path}"
      "${_signature_source}"
      "${_configure_manifest}"
      "${_prepare_stamp}"
      "${_prepared_compile_database}"
      "${_pcm_manifest}"
      "${_prepare_action_trace}"
      "${BIOSEQ_CODEGEN_CLANG_BINARY}"
      "${BIOSEQ_CODEGEN_CLANG_SCAN_DEPS}"
      "${BIOSEQ_CODEGEN_LLVM_CONFIG}"
      "${BIOSEQ_CODEGEN_CLANG_STDLIB_MODULES_JSON}"
    DEPFILE "${_generate_depfile}"
    COMMENT "Generating ${ARG_NAME} template-flatten module topology for profile ${_profile}"
    VERBATIM)
  add_custom_target("${_generation_target}"
    DEPENDS ${_generation_outputs})
  add_dependencies(
    "${_generation_target}" bioseq_codegen_project "${_prepare_target}")

  set(_generated_cppm ${_interface_sources} ${_manifest_sources})
  set_source_files_properties(${_generated_cppm} PROPERTIES
    GENERATED TRUE
    CXX_SCAN_FOR_MODULES ON
    COMPILE_OPTIONS "-x;c++")
  if(_emission_policy STREQUAL "selector_api")
    set(${ARG_OUT_API_PARTITION} "${_api_partition}" PARENT_SCOPE)
    set(${ARG_OUT_SELECTOR_PARTITION}
      "${_selector_partition}" PARENT_SCOPE)
  endif()
  set(${ARG_OUT_IMPLEMENTATION_UNITS} "${_manifest_sources}" PARENT_SCOPE)
  set(${ARG_OUT_GENERATION_TARGET} "${_generation_target}" PARENT_SCOPE)
  set(${ARG_OUT_ARTIFACT_MANIFEST} "${_artifact_manifest}" PARENT_SCOPE)
endfunction()
