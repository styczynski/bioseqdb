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

function(_bioseq_highway_canonical_identity_directory output label value)
  _bioseq_highway_reject_control_characters("${label}" "${value}")
  if("${value}" MATCHES "[;|]")
    _bioseq_highway_fail(
      "${label} contains a reserved identity separator: ${value}")
  endif()
  if(NOT IS_ABSOLUTE "${value}" OR NOT IS_DIRECTORY "${value}")
    _bioseq_highway_fail(
      "${label} must identify an existing absolute directory: ${value}")
  endif()
  file(REAL_PATH "${value}" _canonical)
  set(${output} "${_canonical}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_effective_apple_architecture output)
  set(_apple_architectures ${CMAKE_OSX_ARCHITECTURES})
  list(REMOVE_DUPLICATES _apple_architectures)
  if(NOT _apple_architectures)
    set(_apple_architectures ${_CMAKE_APPLE_ARCHS_DEFAULT})
    list(REMOVE_DUPLICATES _apple_architectures)
  endif()
  if(NOT _apple_architectures)
    set(_apple_architectures ${CMAKE_SYSTEM_PROCESSOR})
    list(REMOVE_DUPLICATES _apple_architectures)
  endif()
  list(LENGTH _apple_architectures _apple_architecture_count)
  if(NOT _apple_architecture_count EQUAL 1)
    _bioseq_highway_fail(
      "configure-time Highway target probing cannot represent the ambiguous "
      "multi-architecture CMAKE_OSX_ARCHITECTURES='"
      "${CMAKE_OSX_ARCHITECTURES}'")
  endif()
  list(GET _apple_architectures 0 _apple_architecture)
  _bioseq_highway_reject_control_characters(
    "Apple probe architecture" "${_apple_architecture}")
  if(NOT _apple_architecture MATCHES "^[A-Za-z0-9_][A-Za-z0-9_+-]*$")
    _bioseq_highway_fail(
      "Apple probe architecture has ambiguous driver spelling: "
      "${_apple_architecture}")
  endif()
  set(${output} "${_apple_architecture}" PARENT_SCOPE)
endfunction()

# CMake adds these driver options outside CMAKE_CXX_FLAGS for ordinary target
# compilation. The configure-time preprocessor is a direct compiler invocation,
# so reconstruct the same unambiguous identity explicitly and seal its canonical
# spelling into the Highway policy/provenance.
function(_bioseq_highway_configured_probe_identity
    output_flags output_records output_digest)
  set(_flags)
  set(_records)

  if(CMAKE_CXX_COMPILER_TARGET)
    _bioseq_highway_reject_control_characters(
      "CMAKE_CXX_COMPILER_TARGET" "${CMAKE_CXX_COMPILER_TARGET}")
    if(NOT CMAKE_CXX_COMPILER_TARGET MATCHES
        "^[A-Za-z0-9_][A-Za-z0-9_.+-]*$")
      _bioseq_highway_fail(
        "CMAKE_CXX_COMPILER_TARGET has ambiguous driver spelling: "
        "${CMAKE_CXX_COMPILER_TARGET}")
    elseif(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
      _bioseq_highway_fail(
        "CMAKE_CXX_COMPILER_TARGET requires a Clang-family probe driver; got "
        "${CMAKE_CXX_COMPILER_ID}")
    endif()
    set(_target_flag "--target=${CMAKE_CXX_COMPILER_TARGET}")
    list(APPEND _flags "${_target_flag}")
    list(APPEND _records "target=${_target_flag}")
  endif()

  if(CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "^(Clang|AppleClang)$")
      _bioseq_highway_fail(
        "CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN requires a Clang-family "
        "probe driver; got ${CMAKE_CXX_COMPILER_ID}")
    endif()
    _bioseq_highway_canonical_identity_directory(
      _external_toolchain "CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN"
      "${CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN}")
    set(_toolchain_flag "--gcc-toolchain=${_external_toolchain}")
    list(APPEND _flags "${_toolchain_flag}")
    list(APPEND _records "external-toolchain=${_toolchain_flag}")
  endif()

  set(_generic_sysroot "")
  set(_generic_sysroot_owner "")
  if(CMAKE_SYSROOT_COMPILE)
    set(_generic_sysroot "${CMAKE_SYSROOT_COMPILE}")
    set(_generic_sysroot_owner CMAKE_SYSROOT_COMPILE)
  elseif(CMAKE_SYSROOT)
    set(_generic_sysroot "${CMAKE_SYSROOT}")
    set(_generic_sysroot_owner CMAKE_SYSROOT)
  endif()
  if(_generic_sysroot)
    _bioseq_highway_canonical_identity_directory(
      _generic_sysroot "${_generic_sysroot_owner}" "${_generic_sysroot}")
  endif()

  if(APPLE)
    set(_apple_sysroot "")
    set(_cmake_apple_sysroot "")
    if(_CMAKE_OSX_SYSROOT_PATH)
      _bioseq_highway_canonical_identity_directory(
        _cmake_apple_sysroot "CMake-resolved Apple SDK"
        "${_CMAKE_OSX_SYSROOT_PATH}")
    endif()
    if(CMAKE_OSX_SYSROOT)
      if(IS_ABSOLUTE "${CMAKE_OSX_SYSROOT}")
        _bioseq_highway_canonical_identity_directory(
          _apple_sysroot "CMAKE_OSX_SYSROOT" "${CMAKE_OSX_SYSROOT}")
      elseif(CMAKE_OSX_SYSROOT MATCHES "^[A-Za-z0-9_.+-]+$")
        if(_cmake_apple_sysroot)
          set(_apple_sysroot "${_cmake_apple_sysroot}")
        else()
          find_program(_bioseq_highway_xcrun NAMES xcrun)
          if(NOT _bioseq_highway_xcrun)
            _bioseq_highway_fail(
              "CMAKE_OSX_SYSROOT SDK-name spelling '${CMAKE_OSX_SYSROOT}' "
              "requires xcrun for an authenticated absolute identity")
          endif()
          execute_process(
            COMMAND "${_bioseq_highway_xcrun}"
              --sdk "${CMAKE_OSX_SYSROOT}" --show-sdk-path
            RESULT_VARIABLE _sdk_status
            OUTPUT_VARIABLE _sdk_path
            ERROR_VARIABLE _sdk_error
            OUTPUT_STRIP_TRAILING_WHITESPACE)
          if(NOT _sdk_status EQUAL 0)
            _bioseq_highway_fail(
              "cannot resolve CMAKE_OSX_SYSROOT SDK name '"
              "${CMAKE_OSX_SYSROOT}': ${_sdk_error}")
          endif()
          _bioseq_highway_canonical_identity_directory(
            _apple_sysroot "resolved CMAKE_OSX_SYSROOT" "${_sdk_path}")
        endif()
      else()
        _bioseq_highway_fail(
          "CMAKE_OSX_SYSROOT has unsupported non-absolute SDK spelling: "
          "${CMAKE_OSX_SYSROOT}")
      endif()
      if(_cmake_apple_sysroot
          AND NOT _cmake_apple_sysroot STREQUAL _apple_sysroot)
        _bioseq_highway_fail(
          "CMAKE_OSX_SYSROOT disagrees with CMake's resolved Apple SDK path")
      endif()
    elseif(_cmake_apple_sysroot)
      # CMake resolves the compiler's implicit SDK even when the public cache
      # spelling is empty, then injects this path into ordinary compile rules.
      set(_apple_sysroot "${_cmake_apple_sysroot}")
    elseif(NOT _generic_sysroot)
      # The internal resolved path is expected after the C++ language is
      # enabled. Reproduce CMake's default resolution only when it is absent.
      find_program(_bioseq_highway_xcrun NAMES xcrun)
      if(NOT _bioseq_highway_xcrun)
        _bioseq_highway_fail(
          "CMake published neither CMAKE_OSX_SYSROOT nor "
          "_CMAKE_OSX_SYSROOT_PATH, and xcrun is unavailable")
      endif()
      set(_default_sdk_arguments)
      if(CMAKE_GENERATOR STREQUAL "Xcode")
        list(APPEND _default_sdk_arguments --sdk macosx)
      endif()
      execute_process(
        COMMAND "${_bioseq_highway_xcrun}"
          ${_default_sdk_arguments} --show-sdk-path
        RESULT_VARIABLE _sdk_status
        OUTPUT_VARIABLE _sdk_path
        ERROR_VARIABLE _sdk_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
      if(NOT _sdk_status EQUAL 0)
        _bioseq_highway_fail(
          "cannot resolve the effective default Apple SDK: ${_sdk_error}")
      endif()
      _bioseq_highway_canonical_identity_directory(
        _apple_sysroot "resolved default Apple SDK" "${_sdk_path}")
    endif()
    if(_apple_sysroot AND _generic_sysroot
        AND NOT _apple_sysroot STREQUAL _generic_sysroot)
      _bioseq_highway_fail(
        "CMAKE_OSX_SYSROOT and ${_generic_sysroot_owner} resolve to "
        "different compile sysroots")
    elseif(NOT _apple_sysroot)
      set(_apple_sysroot "${_generic_sysroot}")
    endif()
    if(_apple_sysroot)
      list(APPEND _flags -isysroot "${_apple_sysroot}")
      list(APPEND _records "apple-sysroot=-isysroot|${_apple_sysroot}")
    endif()

    _bioseq_highway_effective_apple_architecture(_apple_architecture)
    list(APPEND _flags -arch "${_apple_architecture}")
    list(APPEND _records "apple-architecture=-arch|${_apple_architecture}")

    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      _bioseq_highway_reject_control_characters(
        "CMAKE_OSX_DEPLOYMENT_TARGET" "${CMAKE_OSX_DEPLOYMENT_TARGET}")
      if(NOT CMAKE_OSX_DEPLOYMENT_TARGET MATCHES
          "^[0-9]+(\\.[0-9]+)?(\\.[0-9]+)?$")
        _bioseq_highway_fail(
          "CMAKE_OSX_DEPLOYMENT_TARGET has unsupported version spelling: "
          "${CMAKE_OSX_DEPLOYMENT_TARGET}")
      elseif(NOT CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG)
        _bioseq_highway_fail(
          "CMake did not publish CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG for "
          "configured deployment target ${CMAKE_OSX_DEPLOYMENT_TARGET}")
      endif()
      set(_deployment_flag
        "${CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG}")
      _bioseq_highway_reject_control_characters(
        "CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG" "${_deployment_flag}")
      if(_deployment_flag MATCHES ";")
        _bioseq_highway_fail(
          "CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG has ambiguous list spelling")
      endif()
      string(REPLACE "<ARCH>" "${_apple_architecture}"
        _deployment_flag "${_deployment_flag}")
      if(_deployment_flag MATCHES "<VERSION_MIN>")
        string(REPLACE "<VERSION_MIN>" "${CMAKE_OSX_DEPLOYMENT_TARGET}"
          _deployment_flag "${_deployment_flag}")
      else()
        string(APPEND _deployment_flag "${CMAKE_OSX_DEPLOYMENT_TARGET}")
      endif()
      if(_deployment_flag MATCHES "[<>]" OR _deployment_flag MATCHES "[ |]")
        _bioseq_highway_fail(
          "cannot resolve CMake's Apple deployment-target flag template: "
          "${CMAKE_CXX_OSX_DEPLOYMENT_TARGET_FLAG}")
      endif()
      list(APPEND _flags "${_deployment_flag}")
      list(APPEND _records "apple-deployment=${_deployment_flag}")
    endif()
  else()
    if(CMAKE_OSX_SYSROOT OR CMAKE_OSX_ARCHITECTURES
        OR CMAKE_OSX_DEPLOYMENT_TARGET)
      _bioseq_highway_fail(
        "Apple sysroot/architecture/deployment identity is configured for a "
        "non-Apple probe platform")
    endif()
    if(_generic_sysroot)
      set(_sysroot_flag "--sysroot=${_generic_sysroot}")
      list(APPEND _flags "${_sysroot_flag}")
      list(APPEND _records "sysroot=${_sysroot_flag}")
    endif()
  endif()

  set(_identity_stream "bioseq-highway-probe-compilation-identity-v1\n")
  foreach(_record IN LISTS _records)
    _bioseq_highway_reject_control_characters(
      "Highway probe identity record" "${_record}")
    string(APPEND _identity_stream "${_record}\n")
  endforeach()
  string(SHA256 _identity_digest "${_identity_stream}")
  set(${output_flags} "${_flags}" PARENT_SCOPE)
  set(${output_records} "${_records}" PARENT_SCOPE)
  set(${output_digest} "${_identity_digest}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_require_list_suffix label full_list suffix)
  if(NOT suffix)
    return()
  endif()
  list(LENGTH full_list _full_count)
  list(LENGTH suffix _suffix_count)
  if(_full_count LESS _suffix_count)
    _bioseq_highway_fail("${label} omits configured probe identity flags")
  endif()
  math(EXPR _suffix_begin "${_full_count} - ${_suffix_count}")
  list(SUBLIST full_list ${_suffix_begin} ${_suffix_count} _actual_suffix)
  if(NOT _actual_suffix STREQUAL suffix)
    _bioseq_highway_fail(
      "${label} does not retain normalized probe identity flag ordering")
  endif()
endfunction()

function(_bioseq_highway_validate_policy_replacement name value)
  if(NOT name MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    _bioseq_highway_fail(
      "Highway policy macro name is not a source-safe identifier: ${name}")
  endif()
  _bioseq_highway_reject_control_characters(
    "Highway policy replacement for ${name}" "${value}")
  # The block is emitted as C++ source, so admit only a conservative sequence
  # of identifier/pp-number/expression punctuation tokens. In particular,
  # quotes, whitespace, backslashes, '#', '/' and '*' cannot splice a line,
  # open a comment, or manufacture another directive.
  if(NOT value MATCHES "^[A-Za-z0-9_().,+<>=!&|~^?:-]+$")
    _bioseq_highway_fail(
      "Highway policy definition ${name} has an unsafe canonical source "
      "replacement '${value}'")
  endif()
endfunction()

function(_bioseq_highway_reject_ambient_driver_identity label)
  foreach(_option IN LISTS ARGN)
    _bioseq_highway_reject_control_characters("${label}" "${_option}")
    if(_option MATCHES
        "^(-target|--target|-isysroot|--sysroot|-gcc-toolchain|--gcc-toolchain|-arch)(=|$)"
        OR _option MATCHES
        "(^|[ :])(-target|--target|-isysroot|--sysroot|-gcc-toolchain|--gcc-toolchain|-arch)(=|[ ]|$)"
        OR _option MATCHES "^-m[A-Za-z0-9-]+-version-min(=|$)"
        OR _option MATCHES
        "(^|[ :])-m[A-Za-z0-9-]+-version-min(=|[ ]|$)")
      _bioseq_highway_fail(
        "${label} contains dedicated driver identity option '${_option}'; "
        "use CMAKE_CXX_COMPILER_TARGET, CMAKE_SYSROOT_COMPILE/CMAKE_SYSROOT, "
        "CMAKE_OSX_SYSROOT/CMAKE_OSX_ARCHITECTURES, or "
        "CMAKE_OSX_DEPLOYMENT_TARGET/CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN")
    elseif(_option MATCHES "(^|[ :])-m[A-Za-z0-9_.+-]")
      _bioseq_highway_fail(
        "${label} contains unauthenticated target-shaping machine option "
        "'${_option}'")
    endif()
  endforeach()
endfunction()

function(_bioseq_highway_reject_opaque_preprocessor_inputs label)
  foreach(_option IN LISTS ARGN)
    _bioseq_highway_reject_control_characters("${label}" "${_option}")
    if(_option MATCHES
        "(^|[ :])(-Xclang|-Xpreprocessor|-cc1)(=|[ ]|$)"
        OR _option MATCHES "(^|[ :])-Wp,")
      _bioseq_highway_fail(
        "${label} contains unauthenticated compiler forwarding option "
        "'${_option}'")
    elseif(_option MATCHES
        "(^|[ :])--config(-system-dir|-user-dir)?(=|[ ]|$)"
        OR _option MATCHES "(^|[ :])--?specs(=|[ ]|$)")
      _bioseq_highway_fail(
        "${label} contains opaque compiler configuration option "
        "'${_option}'")
    elseif(_option MATCHES
        "(^|[ :])--(define-macro|undefine-macro)(=|[ ]|$)")
      _bioseq_highway_fail(
        "${label} contains unauthenticated compiler macro mutation option "
        "'${_option}'")
    elseif(_option MATCHES "(^|[ :])-undef(=|[ ]|$)")
      _bioseq_highway_fail(
        "${label} suppresses authenticated compiler predefined macros with "
        "'${_option}'")
    elseif(_option MATCHES
        "(^|[ :])(-I|-F|-isystem|-cxx-isystem|-stdlib[+][+]-isystem|-iquote|-idirafter|-iprefix|-iwithprefixbefore|-iwithprefix|-iwithsysroot|-imultilib|-imultiarch)"
        OR _option MATCHES
        "(^|[ :])(-iframework|-ivfsoverlay|-vfsoverlay|--vfsoverlay|-resource-dir)"
        OR _option MATCHES
        "(^|[ :])(-nostdinc([+][+])?|-nostdlibinc|-nobuiltininc|-ibuiltininc)(=|[ ]|$)"
        OR _option MATCHES
        "(^|[ :])--(include-barrier|no-standard-includes)(=|[ ]|$)"
        OR _option MATCHES
        "(^|[ :])--include-(directory(-after)?|prefix|with-prefix(-after|-before)?)(=|[ ]|$)")
      _bioseq_highway_fail(
        "${label} contains unauthenticated header-search/VFS option "
        "'${_option}'")
    elseif(_option MATCHES
        "(^|[ :,=])(-include-pch|-include-pth|-include|-imacros)"
        OR _option MATCHES "(^|[ :])--(include|imacros)(=|[ ]|$)")
      _bioseq_highway_fail(
        "${label} contains unauthenticated forced preprocessor input "
        "'${_option}'")
    elseif(_option MATCHES "(^|[ :,=])@[^ ]+")
      _bioseq_highway_fail(
        "${label} contains opaque compiler response-file argument "
        "'${_option}'")
    endif()
  endforeach()
endfunction()

function(_bioseq_highway_require_single_config_probe)
  if(CMAKE_CONFIGURATION_TYPES)
    _bioseq_highway_fail(
      "configure-time Highway target probing does not support multi-config "
      "generators; CMAKE_CONFIGURATION_TYPES='"
      "${CMAKE_CONFIGURATION_TYPES}'")
  endif()
endfunction()

function(_bioseq_highway_validate_ambient_policy_mutations
    label arguments definitions)
  set(_policy_names)
  set(_policy_values)
  foreach(_definition IN LISTS definitions)
    if(_definition MATCHES "^([^=]+)=(.*)$")
      set(_name "${CMAKE_MATCH_1}")
      set(_value "${CMAKE_MATCH_2}")
    else()
      set(_name "${_definition}")
      set(_value 1)
    endif()
    _bioseq_highway_validate_policy_replacement("${_name}" "${_value}")
    list(APPEND _policy_names "${_name}")
    list(APPEND _policy_values "${_value}")
  endforeach()

  set(_expanded_arguments)
  foreach(_argument IN LISTS arguments)
    if(_argument MATCHES "^SHELL:(.*)$")
      set(_shell_spelling "${CMAKE_MATCH_1}")
      separate_arguments(_shell_arguments UNIX_COMMAND "${_shell_spelling}")
      list(APPEND _expanded_arguments ${_shell_arguments})
    elseif(_argument MATCHES "^-Wp,(-[DU].*)$")
      list(APPEND _expanded_arguments "${CMAKE_MATCH_1}")
    else()
      list(APPEND _expanded_arguments "${_argument}")
    endif()
  endforeach()

  list(LENGTH _expanded_arguments _argument_count)
  set(_argument_index 0)
  while(_argument_index LESS _argument_count)
    list(GET _expanded_arguments ${_argument_index} _argument)
    set(_mutation "")
    set(_payload "")
    if(_argument STREQUAL "-D" OR _argument STREQUAL "-U")
      set(_mutation "${_argument}")
      math(EXPR _argument_index "${_argument_index} + 1")
      if(_argument_index GREATER_EQUAL _argument_count)
        _bioseq_highway_fail(
          "${label} contains an unterminated ${_mutation} option")
      endif()
      list(GET _expanded_arguments ${_argument_index} _payload)
    elseif(_argument MATCHES "^(-D|-U)(.*)$")
      set(_mutation "${CMAKE_MATCH_1}")
      set(_payload "${CMAKE_MATCH_2}")
    endif()

    if(_mutation STREQUAL "-D")
      if(_payload MATCHES "^([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
        set(_name "${CMAKE_MATCH_1}")
        set(_value "${CMAKE_MATCH_2}")
      elseif(_payload MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        set(_name "${_payload}")
        set(_value 1)
      else()
        _bioseq_highway_fail(
          "${label} contains malformed preprocessor definition '${_payload}'")
      endif()
      list(FIND _policy_names "${_name}" _policy_index)
      if(NOT _policy_index EQUAL -1)
        list(GET _policy_values ${_policy_index} _policy_value)
        if(NOT _value STREQUAL _policy_value)
          _bioseq_highway_fail(
            "${label} defines policy macro ${_name} as '${_value}', but the "
            "canonical policy block defines '${_policy_value}'")
        endif()
      elseif(_name MATCHES "^(HWY_|BIOSEQ_HIGHWAY_)")
        _bioseq_highway_fail(
          "${label} defines unregistered Highway policy macro ${_name}")
      endif()
    elseif(_mutation STREQUAL "-U")
      if(NOT _payload MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        _bioseq_highway_fail(
          "${label} contains malformed preprocessor undefinition '${_payload}'")
      endif()
      list(FIND _policy_names "${_payload}" _policy_index)
      if(NOT _policy_index EQUAL -1)
        _bioseq_highway_fail(
          "${label} undefines canonical policy macro ${_payload}")
      elseif(_payload MATCHES "^(HWY_|BIOSEQ_HIGHWAY_)")
        _bioseq_highway_fail(
          "${label} undefines unregistered Highway policy macro ${_payload}")
      endif()
    endif()
    math(EXPR _argument_index "${_argument_index} + 1")
  endwhile()
endfunction()

function(_bioseq_highway_render_policy_block output)
  set(_policy_block "")
  foreach(_definition IN LISTS ARGN)
    if(_definition MATCHES "^([^=]+)=(.*)$")
      set(_policy_name "${CMAKE_MATCH_1}")
      set(_policy_value "${CMAKE_MATCH_2}")
    else()
      set(_policy_name "${_definition}")
      set(_policy_value 1)
    endif()
    _bioseq_highway_validate_policy_replacement(
      "${_policy_name}" "${_policy_value}")
    string(APPEND _policy_block
      "#define ${_policy_name} ${_policy_value}\n")
  endforeach()
  set(${output} "${_policy_block}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_render_target_probe_source output policy_block)
  string(CONCAT _probe_contents
    "// Generated configure-time Highway target inventory probe.\n"
    "// The following canonical block is byte-identical to generated units.\n"
    "${policy_block}"
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
  set(${output} "${_probe_contents}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_configure_probe_policy
    output_definitions output_compile_options output_identity_flags
    output_identity_records output_identity_digest)
  cmake_parse_arguments(PARSE_ARGV 5 ARG "" "" "DEFINITIONS;COMPILE_OPTIONS")
  _bioseq_highway_configured_probe_identity(
    _identity_flags _identity_records _identity_digest)
  _bioseq_highway_normalize_definitions(_definitions
    ${ARG_DEFINITIONS}
    "BIOSEQ_HIGHWAY_PROBE_COMPILATION_IDENTITY=${_identity_digest}")
  set(_compile_options ${ARG_COMPILE_OPTIONS})
  _bioseq_highway_reject_ambient_driver_identity(
    "Highway probe policy compile options" ${_compile_options})
  _bioseq_highway_reject_opaque_preprocessor_inputs(
    "Highway probe policy compile options" ${_compile_options})
  list(APPEND _compile_options ${_identity_flags})
  _bioseq_highway_require_list_suffix(
    "Highway probe policy" "${_compile_options}" "${_identity_flags}")
  set(${output_definitions} "${_definitions}" PARENT_SCOPE)
  set(${output_compile_options} "${_compile_options}" PARENT_SCOPE)
  set(${output_identity_flags} "${_identity_flags}" PARENT_SCOPE)
  set(${output_identity_records} "${_identity_records}" PARENT_SCOPE)
  set(${output_identity_digest} "${_identity_digest}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_prepare_direct_probe_policy output)
  cmake_parse_arguments(PARSE_ARGV 1 ARG "" "LABEL"
    "COMPILER_ARGUMENTS;CONFIGURED_FLAGS;COMPILE_OPTIONS;IDENTITY_FLAGS;DEFINITIONS")
  if(NOT ARG_LABEL)
    set(ARG_LABEL "direct Highway preprocessor action")
  endif()

  _bioseq_highway_require_list_suffix(
    "${ARG_LABEL}" "${ARG_COMPILE_OPTIONS}" "${ARG_IDENTITY_FLAGS}")
  set(_ambient_compile_options ${ARG_COMPILE_OPTIONS})
  if(ARG_IDENTITY_FLAGS)
    list(LENGTH _ambient_compile_options _compile_option_count)
    list(LENGTH ARG_IDENTITY_FLAGS _identity_flag_count)
    math(EXPR _ambient_option_count
      "${_compile_option_count} - ${_identity_flag_count}")
    if(_ambient_option_count GREATER 0)
      list(SUBLIST _ambient_compile_options 0 ${_ambient_option_count}
        _ambient_compile_options)
    else()
      set(_ambient_compile_options)
    endif()
  endif()

  _bioseq_highway_reject_ambient_driver_identity(
    "${ARG_LABEL} compiler/configured options"
    ${ARG_COMPILER_ARGUMENTS} ${ARG_CONFIGURED_FLAGS}
    ${_ambient_compile_options})
  _bioseq_highway_reject_opaque_preprocessor_inputs(
    "${ARG_LABEL} compiler/configured options"
    ${ARG_COMPILER_ARGUMENTS} ${ARG_CONFIGURED_FLAGS}
    ${_ambient_compile_options})
  set(_ambient_policy_arguments
    ${ARG_COMPILER_ARGUMENTS} ${ARG_CONFIGURED_FLAGS}
    ${_ambient_compile_options})
  _bioseq_highway_validate_ambient_policy_mutations(
    "${ARG_LABEL}" "${_ambient_policy_arguments}" "${ARG_DEFINITIONS}")

  set(_probe_arguments ${ARG_CONFIGURED_FLAGS} ${ARG_COMPILE_OPTIONS})
  _bioseq_highway_require_list_suffix(
    "${ARG_LABEL}" "${_probe_arguments}" "${ARG_IDENTITY_FLAGS}")
  set(${output} "${_probe_arguments}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_render_probe_identity_provenance
    output identity_digest)
  if(NOT identity_digest MATCHES "^[0-9a-f][0-9a-f]+$")
    _bioseq_highway_fail("probe compilation identity digest is malformed")
  endif()
  string(LENGTH "${identity_digest}" _identity_digest_length)
  if(NOT _identity_digest_length EQUAL 64)
    _bioseq_highway_fail("probe compilation identity digest is not SHA-256")
  endif()
  set(_contents "bioseq-highway-probe-compilation-identity-v1\n")
  string(APPEND _contents "identity\t${identity_digest}\n")
  set(_ordinal 0)
  foreach(_record IN LISTS ARGN)
    string(APPEND _contents "flag\t${_ordinal}\t${_record}\n")
    math(EXPR _ordinal "${_ordinal} + 1")
  endforeach()
  set(${output} "${_contents}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_platform_from_processor output processor label)
  string(TOLOWER "${processor}" _processor)
  if(_processor MATCHES "^(x86_64|x86_64h|amd64|x64)$")
    set(_platform x86_64)
  elseif(_processor MATCHES "^(aarch64[^-]*|arm64[^-]*)$")
    set(_platform aarch64)
  elseif(_processor MATCHES "^(armv7[^-]*|arm)$")
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
        "cannot derive a stable platform slug from ${label}='${processor}'")
    endif()
  endif()
  set(${output} "${_platform}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_resolve_platform output)
  set(_authenticated_platform "")
  if(CMAKE_CXX_COMPILER_TARGET)
    string(REGEX REPLACE "-.*$" "" _target_processor
      "${CMAKE_CXX_COMPILER_TARGET}")
    _bioseq_highway_platform_from_processor(
      _target_platform "${_target_processor}" "CMAKE_CXX_COMPILER_TARGET")
    set(_authenticated_platform "${_target_platform}")
  endif()

  if(APPLE)
    _bioseq_highway_effective_apple_architecture(_apple_architecture)
    _bioseq_highway_platform_from_processor(
      _apple_platform "${_apple_architecture}"
      "effective Apple compile architecture")
    if(_authenticated_platform
        AND NOT _authenticated_platform STREQUAL _apple_platform)
      _bioseq_highway_fail(
        "CMAKE_CXX_COMPILER_TARGET platform ${_authenticated_platform} "
        "disagrees with effective Apple architecture platform "
        "${_apple_platform}")
    endif()
    set(_authenticated_platform "${_apple_platform}")
  endif()

  if(_authenticated_platform)
    set(_platform "${_authenticated_platform}")
  else()
    _bioseq_highway_platform_from_processor(
      _platform "${CMAKE_SYSTEM_PROCESSOR}" "CMAKE_SYSTEM_PROCESSOR")
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
    if(_definition MATCHES "^([^=]+)=(.*)$")
      set(_name "${CMAKE_MATCH_1}")
      set(_value "${CMAKE_MATCH_2}")
    else()
      set(_name "${_definition}")
      set(_value 1)
    endif()
    _bioseq_highway_validate_policy_replacement("${_name}" "${_value}")
    set(_canonical_definition "${_name}=${_value}")
    if(_name STREQUAL "HWY_TARGET" OR _name STREQUAL "HWY_ONCE")
      _bioseq_highway_fail(
        "${_name} is generator-owned and cannot be a policy definition")
    endif()
    list(FIND _definition_names "${_name}" _existing_index)
    if(NOT _existing_index EQUAL -1)
      list(GET _definition_values ${_existing_index} _existing_definition)
      if(NOT _existing_definition STREQUAL _canonical_definition)
        _bioseq_highway_fail(
          "conflicting values for Highway policy definition ${_name}: "
          "'${_existing_definition}' and '${_canonical_definition}'")
      endif()
      continue()
    endif()
    list(APPEND _definition_names "${_name}")
    list(APPEND _definition_values "${_canonical_definition}")
    list(APPEND _definitions "${_canonical_definition}")
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

function(_bioseq_highway_parse_depfile
    output depfile expected_target base_directory)
  if(NOT IS_ABSOLUTE "${depfile}" OR NOT EXISTS "${depfile}"
      OR IS_DIRECTORY "${depfile}")
    _bioseq_highway_fail(
      "target probe did not produce an absolute regular depfile: ${depfile}")
  endif()
  if(NOT IS_ABSOLUTE "${base_directory}"
      OR NOT IS_DIRECTORY "${base_directory}")
    _bioseq_highway_fail(
      "target-probe depfile base must be an absolute directory: "
      "${base_directory}")
  endif()
  if(NOT expected_target MATCHES "^[A-Za-z0-9_.+-]+$")
    _bioseq_highway_fail(
      "target-probe depfile target is malformed: ${expected_target}")
  endif()

  file(READ "${depfile}" _contents)
  if(_contents STREQUAL "")
    _bioseq_highway_fail("target-probe depfile is empty: ${depfile}")
  endif()
  string(REPLACE "\r\n" "\n" _contents "${_contents}")
  if(_contents MATCHES "\r")
    _bioseq_highway_fail(
      "target-probe depfile contains a bare carriage return: ${depfile}")
  endif()
  string(REPLACE "\\\n" " " _contents "${_contents}")
  if(NOT _contents MATCHES "\n$")
    _bioseq_highway_fail(
      "target-probe depfile must end with one complete rule: ${depfile}")
  endif()
  string(REGEX REPLACE "\n$" "" _contents "${_contents}")
  if(_contents MATCHES "\n")
    _bioseq_highway_fail(
      "target-probe depfile contains multiple logical rules: ${depfile}")
  endif()

  set(_prefix "${expected_target}:")
  string(LENGTH "${_prefix}" _prefix_length)
  string(LENGTH "${_contents}" _contents_length)
  if(_contents_length LESS _prefix_length)
    _bioseq_highway_fail(
      "target-probe depfile omits its owned target: ${depfile}")
  endif()
  string(SUBSTRING "${_contents}" 0 ${_prefix_length} _actual_prefix)
  if(NOT _actual_prefix STREQUAL _prefix)
    _bioseq_highway_fail(
      "target-probe depfile has a foreign or ambiguous target: ${depfile}")
  endif()
  string(SUBSTRING "${_contents}" ${_prefix_length} -1 _prerequisites)

  # Parse the make dependency spelling directly. The compiler escapes spaces,
  # '#', ':', and backslashes with a backslash and spells a literal '$' as
  # '$$'. Reject incomplete or multi-rule syntax instead of accepting a
  # partially decoded dependency set.
  string(LENGTH "${_prerequisites}" _length)
  set(_cursor 0)
  set(_escaped FALSE)
  set(_token "")
  set(_dependencies)
  while(_cursor LESS _length)
    string(SUBSTRING "${_prerequisites}" ${_cursor} 1 _character)
    if(_escaped)
      string(APPEND _token "${_character}")
      set(_escaped FALSE)
    elseif(_character STREQUAL "\\")
      set(_escaped TRUE)
    elseif(_character MATCHES "[ \t]")
      if(NOT _token STREQUAL "")
        list(APPEND _dependencies "${_token}")
        set(_token "")
      endif()
    elseif(_character STREQUAL "$")
      math(EXPR _next_cursor "${_cursor} + 1")
      if(_next_cursor GREATER_EQUAL _length)
        _bioseq_highway_fail(
          "target-probe depfile contains an incomplete '$$' escape: "
          "${depfile}")
      endif()
      string(SUBSTRING "${_prerequisites}" ${_next_cursor} 1
        _next_character)
      if(NOT _next_character STREQUAL "$")
        _bioseq_highway_fail(
          "target-probe depfile contains an unescaped '$': ${depfile}")
      endif()
      string(APPEND _token "$")
      set(_cursor ${_next_cursor})
    elseif(_character STREQUAL "#")
      _bioseq_highway_fail(
        "target-probe depfile contains an unescaped '#': ${depfile}")
    elseif(_character STREQUAL ";")
      _bioseq_highway_fail(
        "target-probe dependency paths cannot contain ';': ${depfile}")
    else()
      string(APPEND _token "${_character}")
    endif()
    math(EXPR _cursor "${_cursor} + 1")
  endwhile()
  if(_escaped)
    _bioseq_highway_fail(
      "target-probe depfile ends with an incomplete escape: ${depfile}")
  endif()
  if(NOT _token STREQUAL "")
    list(APPEND _dependencies "${_token}")
  endif()
  if(NOT _dependencies)
    _bioseq_highway_fail(
      "target-probe depfile contains no dependencies: ${depfile}")
  endif()

  set(_normalized_dependencies)
  foreach(_dependency IN LISTS _dependencies)
    if(NOT IS_ABSOLUTE "${_dependency}")
      get_filename_component(_dependency "${_dependency}" ABSOLUTE
        BASE_DIR "${base_directory}")
    endif()
    cmake_path(NORMAL_PATH _dependency)
    list(APPEND _normalized_dependencies "${_dependency}")
  endforeach()
  list(REMOVE_DUPLICATES _normalized_dependencies)
  list(SORT _normalized_dependencies)
  set(${output} "${_normalized_dependencies}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_header_closure
    output_paths output_records output_digest)
  cmake_parse_arguments(PARSE_ARGV 3 ARG "" ""
    "INCLUDE_DIRS;DEPENDENCIES")
  if(ARG_UNPARSED_ARGUMENTS)
    _bioseq_highway_fail(
      "unknown header-closure arguments: ${ARG_UNPARSED_ARGUMENTS}")
  endif()
  if(NOT ARG_INCLUDE_DIRS OR NOT ARG_DEPENDENCIES)
    _bioseq_highway_fail(
      "header closure requires include roots and compiler dependencies")
  endif()

  # Canonical de-duplication preserves the compiler's include-search order.
  # The resulting ordinal is stable when the same include tree is relocated.
  set(_include_roots)
  foreach(_include_root IN LISTS ARG_INCLUDE_DIRS)
    _bioseq_highway_reject_control_characters(
      "Highway include root" "${_include_root}")
    if(_include_root MATCHES "[;|]")
      _bioseq_highway_fail(
        "Highway include root contains a reserved separator: "
        "${_include_root}")
    endif()
    if(NOT IS_ABSOLUTE "${_include_root}"
        OR NOT IS_DIRECTORY "${_include_root}")
      _bioseq_highway_fail(
        "Highway include root must be an absolute directory: "
        "${_include_root}")
    endif()
    file(REAL_PATH "${_include_root}" _include_root)
    list(FIND _include_roots "${_include_root}" _root_index)
    if(_root_index EQUAL -1)
      list(APPEND _include_roots "${_include_root}")
    endif()
  endforeach()

  set(_canonical_dependencies)
  foreach(_dependency IN LISTS ARG_DEPENDENCIES)
    _bioseq_highway_reject_control_characters(
      "target-probe dependency" "${_dependency}")
    if(_dependency MATCHES "[;|]")
      _bioseq_highway_fail(
        "target-probe dependency contains a reserved separator: "
        "${_dependency}")
    endif()
    if(NOT IS_ABSOLUTE "${_dependency}" OR NOT EXISTS "${_dependency}"
        OR IS_DIRECTORY "${_dependency}")
      _bioseq_highway_fail(
        "target-probe dependency is not an absolute regular file: "
        "${_dependency}")
    endif()
    file(REAL_PATH "${_dependency}" _dependency)
    list(APPEND _canonical_dependencies "${_dependency}")
  endforeach()
  list(REMOVE_DUPLICATES _canonical_dependencies)
  list(SORT _canonical_dependencies)

  set(_closure_entries)
  foreach(_dependency IN LISTS _canonical_dependencies)
    set(_root_ordinal 0)
    foreach(_include_root IN LISTS _include_roots)
      cmake_path(IS_PREFIX _include_root "${_dependency}"
        NORMALIZE _inside_root)
      if(_inside_root)
        file(RELATIVE_PATH _relative_path
          "${_include_root}" "${_dependency}")
        file(TO_CMAKE_PATH "${_relative_path}" _relative_path)
        if(_relative_path STREQUAL ""
            OR _relative_path MATCHES "^\\.\\./")
          _bioseq_highway_fail(
            "cannot derive a contained relative Highway header path for "
            "${_dependency}")
        endif()
        list(APPEND _closure_entries
          "${_root_ordinal}/${_relative_path}\t${_dependency}")
        break()
      endif()
      math(EXPR _root_ordinal "${_root_ordinal} + 1")
    endforeach()
  endforeach()
  if(NOT _closure_entries)
    _bioseq_highway_fail(
      "target-probe depfile contains no dependency beneath a Highway "
      "include root")
  endif()
  list(SORT _closure_entries)

  set(_closure_paths)
  set(_closure_records)
  set(_closure_identities)
  foreach(_entry IN LISTS _closure_entries)
    if(NOT _entry MATCHES "^([^\t]+)\t(.+)$")
      _bioseq_highway_fail(
        "internal malformed Highway closure entry: ${_entry}")
    endif()
    set(_identity "${CMAKE_MATCH_1}")
    set(_dependency "${CMAKE_MATCH_2}")
    list(FIND _closure_identities "${_identity}" _identity_index)
    if(NOT _identity_index EQUAL -1)
      _bioseq_highway_fail(
        "Highway header closure contains duplicate identity ${_identity}")
    endif()
    list(APPEND _closure_identities "${_identity}")
    list(APPEND _closure_paths "${_dependency}")
    file(SHA256 "${_dependency}" _dependency_digest)
    list(APPEND _closure_records "${_identity}\t${_dependency_digest}")
  endforeach()

  foreach(_required_header IN ITEMS
      hwy/highway.h hwy/detect_targets.h hwy/targets.h)
    _bioseq_highway_find_header(
      _required_path "${_include_roots}" "${_required_header}")
    file(REAL_PATH "${_required_path}" _required_path)
    list(FIND _closure_paths "${_required_path}" _required_index)
    if(_required_index EQUAL -1)
      _bioseq_highway_fail(
        "compiler depfile omits required Highway header "
        "${_required_header}")
    endif()
  endforeach()

  set(_digest_stream "bioseq-highway-header-closure-v1\n")
  foreach(_record IN LISTS _closure_records)
    string(APPEND _digest_stream "${_record}\n")
  endforeach()
  string(SHA256 _closure_digest "${_digest_stream}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    ${_closure_paths})

  set(${output_paths} "${_closure_paths}" PARENT_SCOPE)
  set(${output_records} "${_closure_records}" PARENT_SCOPE)
  set(${output_digest} "${_closure_digest}" PARENT_SCOPE)
endfunction()

function(_bioseq_highway_finalize_target_inventory
    output_records output_order output_mask output_static_record
    platform static_target_bit)
  set(_target_records)
  set(_target_order)
  set(_compiled_mask 0)
  set(_static_target_record)
  foreach(_record IN LISTS ARGN)
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
    set(_normalized_record
      "${platform}|${_slug}|${_token}|${_target_namespace}|${_numeric_bit}")
    list(APPEND _target_records "${_normalized_record}")
    list(APPEND _target_order "${_token}")
    math(EXPR _compiled_mask "${_compiled_mask} | ${_numeric_bit}")
    if(_numeric_bit STREQUAL "${static_target_bit}")
      set(_static_target_record "${_normalized_record}")
    endif()
  endforeach()
  if(NOT _static_target_record)
    _bioseq_highway_fail(
      "HWY_STATIC_TARGET is absent from the canonical target inventory")
  endif()

  set(${output_records} "${_target_records}" PARENT_SCOPE)
  set(${output_order} "${_target_order}" PARENT_SCOPE)
  set(${output_mask} "${_compiled_mask}" PARENT_SCOPE)
  set(${output_static_record} "${_static_target_record}" PARENT_SCOPE)
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
    "INCLUDE_DIRS;DEFINITIONS;COMPILE_OPTIONS;IDENTITY_FLAGS")
  foreach(_required IN ITEMS
      PROBE_DIR PLATFORM OUTPUT_RECORDS OUTPUT_ORDER OUTPUT_MASK
      OUTPUT_STATIC_RECORD OUTPUT_POLICY_DIGEST OUTPUT_HIGHWAY_DIGEST)
    if(NOT ARG_${_required})
      _bioseq_highway_fail("target probe requires ${_required}")
    endif()
  endforeach()
  _bioseq_highway_require_single_config_probe()

  file(MAKE_DIRECTORY "${ARG_PROBE_DIR}")
  set(_probe_source "${ARG_PROBE_DIR}/target_inventory_probe.cppm")
  set(_probe_depfile "${ARG_PROBE_DIR}/target_inventory_probe.d")
  set(_probe_depfile_target bioseq_highway_target_inventory_probe)
  _bioseq_highway_render_policy_block(_policy_block ${ARG_DEFINITIONS})
  string(SHA256 _policy_digest "${_policy_block}")
  _bioseq_highway_render_target_probe_source(
    _probe_contents "${_policy_block}")
  _bioseq_highway_write_if_different("${_probe_source}" "${_probe_contents}")

  set(_compiler_command)
  set(_compiler_arg1)
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

  _bioseq_highway_prepare_direct_probe_policy(_probe_arguments
    LABEL "direct Highway preprocessor action"
    COMPILER_ARGUMENTS ${_compiler_arg1}
    CONFIGURED_FLAGS ${_configured_flags}
    COMPILE_OPTIONS ${ARG_COMPILE_OPTIONS}
    IDENTITY_FLAGS ${ARG_IDENTITY_FLAGS}
    DEFINITIONS ${ARG_DEFINITIONS})
  if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    if(CMAKE_CXX_EXTENSIONS)
      list(APPEND _probe_arguments "-std=gnu++${CMAKE_CXX_STANDARD}")
    else()
      list(APPEND _probe_arguments "-std=c++${CMAKE_CXX_STANDARD}")
    endif()
    list(APPEND _probe_arguments -E -P -x c++
      -MD -MF "${_probe_depfile}" -MT "${_probe_depfile_target}")
    foreach(_include_dir IN LISTS ARG_INCLUDE_DIRS)
      list(APPEND _probe_arguments -isystem "${_include_dir}")
    endforeach()
    list(APPEND _probe_arguments "${_probe_source}")
  else()
    _bioseq_highway_fail(
      "full-closure target probe requires GNU, Clang, or AppleClang; got "
      "${CMAKE_CXX_COMPILER_ID}")
  endif()

  file(REMOVE "${_probe_depfile}")
  set(_probe_working_directory "${CMAKE_CURRENT_BINARY_DIR}")
  execute_process(
    COMMAND ${_compiler_command} ${_probe_arguments}
    WORKING_DIRECTORY "${_probe_working_directory}"
    RESULT_VARIABLE _probe_status
    OUTPUT_VARIABLE _probe_output
    ERROR_VARIABLE _probe_error)
  if(NOT _probe_status EQUAL 0)
    _bioseq_highway_fail(
      "HWY_VISIT_TARGETS preprocessor probe failed with exit code "
      "${_probe_status}:\n${_probe_error}")
  endif()
  _bioseq_highway_parse_depfile(
    _probe_dependencies "${_probe_depfile}" "${_probe_depfile_target}"
    "${_probe_working_directory}")
  _bioseq_highway_header_closure(
    _highway_header_paths _highway_header_records _highway_digest
    INCLUDE_DIRS ${ARG_INCLUDE_DIRS}
    DEPENDENCIES ${_probe_dependencies})

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

  # _records_by_bit is already deduplicated and canonicalized in exact
  # HWY_VISIT_TARGETS inventory/provenance order. Preserve that order rather
  # than sorting by slug. Runtime priority is independently defined by the
  # least-significant runnable bit / SupportedAndGeneratedTargets ordering.
  _bioseq_highway_finalize_target_inventory(
    _target_records _target_order _compiled_mask _static_target_record
    "${ARG_PLATFORM}" "${_static_target_bit}" ${_records_by_bit})

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
  _bioseq_highway_configure_probe_policy(
    _policy_definitions _policy_compile_options
    _probe_identity_flags _probe_identity_records _probe_identity_digest
    DEFINITIONS ${_highway_target_definitions} ${ARG_POLICY_DEFINITIONS}
    COMPILE_OPTIONS
      ${_highway_target_compile_options} ${ARG_POLICY_COMPILE_OPTIONS})
  foreach(_compile_option IN LISTS _policy_compile_options)
    _bioseq_highway_reject_control_characters(
      "Highway policy compile option" "${_compile_option}")
    if(_compile_option MATCHES "\\$<")
      _bioseq_highway_fail(
        "Highway policy compile options cannot contain generator "
        "expressions: ${_compile_option}")
    endif()
  endforeach()
  set(BIOSEQ_HIGHWAY_PROBE_COMPILATION_IDENTITY
    "${_probe_identity_digest}" CACHE INTERNAL
    "Authenticated Highway configure-probe compilation identity" FORCE)
  set(BIOSEQ_HIGHWAY_PROBE_COMPILATION_FLAGS
    "${_probe_identity_flags}" CACHE INTERNAL
    "Normalized Highway configure-probe driver flags" FORCE)
  set(BIOSEQ_HIGHWAY_PROBE_COMPILATION_RECORDS
    "${_probe_identity_records}" CACHE INTERNAL
    "Normalized Highway configure-probe provenance records" FORCE)

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
  get_target_property(
    _published_policy_compile_options "${ARG_POLICY_TARGET}"
    INTERFACE_COMPILE_OPTIONS)
  if(NOT _published_policy_compile_options
      OR _published_policy_compile_options STREQUAL
        "_published_policy_compile_options-NOTFOUND")
    set(_published_policy_compile_options)
  endif()
  _bioseq_highway_require_list_suffix(
    "Highway policy target"
    "${_published_policy_compile_options}" "${_probe_identity_flags}")

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
  set(_probe_identity_manifest
    "${_metadata_dir}/probe/compilation_identity.tsv")
  _bioseq_highway_render_probe_identity_provenance(
    _probe_identity_manifest_contents "${_probe_identity_digest}"
    ${_probe_identity_records})
  _bioseq_highway_write_if_different(
    "${_probe_identity_manifest}" "${_probe_identity_manifest_contents}")
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
    IDENTITY_FLAGS ${_probe_identity_flags}
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
  set(_slot_owners)
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
    list(APPEND _slot_owners "${_owner}")
    if(_slot_field STREQUAL "@")
      list(APPEND _whole_slot_families "${_family_field}")
    else()
      list(APPEND _partial_slot_families "${_family_field}")
    endif()
    list(APPEND _normalized_slot_records
      "${_owner}|${_family_field}|${_slot_field}|${_symbol}")
  endforeach()
  list(SORT _normalized_slot_records)
  list(SORT _slot_owners)
  list(REMOVE_DUPLICATES _slot_owners)
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
    foreach(_slot_owner IN LISTS _slot_owners)
      list(APPEND _generated_target_implementations
        "${_target_dir}/instances/${_slot_owner}.cppm")
      if(_target_record STREQUAL _static_target_record)
        list(APPEND _generated_test_implementations
          "${_target_dir}/instances/${_slot_owner}.cppm")
      endif()
    endforeach()
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
    BIOSEQ_HIGHWAY_PROBE_COMPILATION_IDENTITY "${_probe_identity_digest}"
    BIOSEQ_HIGHWAY_PROBE_COMPILATION_FLAGS "${_probe_identity_flags}"
    BIOSEQ_HIGHWAY_PROBE_COMPILATION_RECORDS "${_probe_identity_records}"
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
