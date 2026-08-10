include_guard(GLOBAL)

# Resolve one GCC 16.1+ installation together with the libstdc++ standard
# module inventory that belongs to that exact compiler prefix.  This helper is
# intentionally usable before project(): the GCC-only preset must fail closed
# before CMake can cache a mismatched compiler/inventory pair.

function(_bioseq_query_gcc_version compiler output_version)
  execute_process(
    COMMAND "${compiler}" -dumpfullversion
    OUTPUT_VARIABLE _version
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _version_status)
  if(NOT _version_status EQUAL 0 OR NOT _version)
    execute_process(
      COMMAND "${compiler}" -dumpversion
      OUTPUT_VARIABLE _version
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY)
  endif()
  set(${output_version} "${_version}" PARENT_SCOPE)
endfunction()

function(_bioseq_gcc_modules_json_is_authentic
    candidate compiler_prefix output_valid output_metadata_sha256
    output_source_closure_sha256)
  set(_valid FALSE)
  set(_metadata_sha256 "")
  set(_source_closure_sha256 "")
  if(NOT candidate OR NOT EXISTS "${candidate}")
    set(${output_valid} FALSE PARENT_SCOPE)
    set(${output_metadata_sha256} "" PARENT_SCOPE)
    set(${output_source_closure_sha256} "" PARENT_SCOPE)
    return()
  endif()

  file(REAL_PATH "${candidate}" _candidate_real)
  file(REAL_PATH "${compiler_prefix}" _compiler_prefix_real)
  get_filename_component(_candidate_name "${_candidate_real}" NAME)
  string(FIND "${_candidate_real}/"
    "${_compiler_prefix_real}/" _candidate_prefix_offset)
  if(NOT _candidate_name STREQUAL "libstdc++.modules.json"
      OR NOT _candidate_prefix_offset EQUAL 0)
    set(${output_valid} FALSE PARENT_SCOPE)
    set(${output_metadata_sha256} "" PARENT_SCOPE)
    set(${output_source_closure_sha256} "" PARENT_SCOPE)
    return()
  endif()

  file(SHA256 "${_candidate_real}" _metadata_sha256)
  file(READ "${_candidate_real}" _inventory)
  string(JSON _inventory_version
    ERROR_VARIABLE _inventory_version_error
    GET "${_inventory}" version)
  string(JSON _module_count
    ERROR_VARIABLE _module_count_error
    LENGTH "${_inventory}" modules)
  if(NOT _inventory_version_error STREQUAL "NOTFOUND"
      OR NOT _module_count_error STREQUAL "NOTFOUND"
      OR NOT _inventory_version EQUAL 1
      OR _module_count LESS 2)
    set(${output_valid} FALSE PARENT_SCOPE)
    set(${output_metadata_sha256} "" PARENT_SCOPE)
    set(${output_source_closure_sha256} "" PARENT_SCOPE)
    return()
  endif()

  get_filename_component(_inventory_dir "${_candidate_real}" DIRECTORY)
  set(_saw_std FALSE)
  set(_saw_std_compat FALSE)
  set(_logical_names)
  set(_source_identity_rows)
  math(EXPR _last_module_index "${_module_count} - 1")
  foreach(_module_index RANGE 0 ${_last_module_index})
    string(JSON _logical_name
      ERROR_VARIABLE _logical_name_error
      GET "${_inventory}" modules ${_module_index} logical-name)
    string(JSON _source_path
      ERROR_VARIABLE _source_path_error
      GET "${_inventory}" modules ${_module_index} source-path)
    string(JSON _is_std_library
      ERROR_VARIABLE _is_std_library_error
      GET "${_inventory}" modules ${_module_index} is-std-library)
    if(NOT _logical_name_error STREQUAL "NOTFOUND"
        OR NOT _source_path_error STREQUAL "NOTFOUND"
        OR NOT _is_std_library_error STREQUAL "NOTFOUND"
        OR NOT _is_std_library)
      set(${output_valid} FALSE PARENT_SCOPE)
      set(${output_metadata_sha256} "" PARENT_SCOPE)
      set(${output_source_closure_sha256} "" PARENT_SCOPE)
      return()
    endif()
    list(FIND _logical_names "${_logical_name}" _logical_name_index)
    if(NOT _logical_name_index EQUAL -1)
      set(${output_valid} FALSE PARENT_SCOPE)
      set(${output_metadata_sha256} "" PARENT_SCOPE)
      set(${output_source_closure_sha256} "" PARENT_SCOPE)
      return()
    endif()
    list(APPEND _logical_names "${_logical_name}")

    file(REAL_PATH "${_source_path}" _source_real
      BASE_DIRECTORY "${_inventory_dir}")
    string(FIND "${_source_real}/"
      "${_compiler_prefix_real}/" _source_prefix_offset)
    if(NOT EXISTS "${_source_real}"
        OR NOT _source_prefix_offset EQUAL 0)
      set(${output_valid} FALSE PARENT_SCOPE)
      set(${output_metadata_sha256} "" PARENT_SCOPE)
      set(${output_source_closure_sha256} "" PARENT_SCOPE)
      return()
    endif()
    file(SHA256 "${_source_real}" _source_sha256)
    list(APPEND _source_identity_rows
      "${_logical_name}\t${_source_real}\t${_source_sha256}")

    if(_logical_name STREQUAL "std")
      if(_saw_std)
        set(${output_valid} FALSE PARENT_SCOPE)
        set(${output_metadata_sha256} "" PARENT_SCOPE)
        set(${output_source_closure_sha256} "" PARENT_SCOPE)
        return()
      endif()
      set(_saw_std TRUE)
    elseif(_logical_name STREQUAL "std.compat")
      if(_saw_std_compat)
        set(${output_valid} FALSE PARENT_SCOPE)
        set(${output_metadata_sha256} "" PARENT_SCOPE)
        set(${output_source_closure_sha256} "" PARENT_SCOPE)
        return()
      endif()
      set(_saw_std_compat TRUE)
    endif()
  endforeach()

  if(_saw_std AND _saw_std_compat)
    set(_valid TRUE)
    list(SORT _source_identity_rows)
    set(_source_closure_material
      "bioseq_gcc_module_source_closure\n"
      "revision=1\n"
      "${_metadata_sha256}\n")
    foreach(_source_identity_row IN LISTS _source_identity_rows)
      string(APPEND _source_closure_material
        "${_source_identity_row}\n")
    endforeach()
    string(SHA256 _source_closure_sha256
      "${_source_closure_material}")
  endif()
  set(${output_valid} "${_valid}" PARENT_SCOPE)
  set(${output_metadata_sha256} "${_metadata_sha256}" PARENT_SCOPE)
  set(${output_source_closure_sha256}
    "${_source_closure_sha256}" PARENT_SCOPE)
endfunction()

function(bioseq_select_gcc16_toolchain)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "REQUESTED_ROOT;REQUESTED_MODULES;OUTPUT_ROOT;OUTPUT_C;OUTPUT_CXX;OUTPUT_MODULES;OUTPUT_VERSION;OUTPUT_C_SHA256;OUTPUT_CXX_SHA256;OUTPUT_MODULES_SHA256;OUTPUT_MODULE_SOURCE_CLOSURE_SHA256"
    "")
  foreach(_required IN ITEMS
      OUTPUT_ROOT OUTPUT_C OUTPUT_CXX OUTPUT_MODULES OUTPUT_VERSION
      OUTPUT_C_SHA256 OUTPUT_CXX_SHA256 OUTPUT_MODULES_SHA256
      OUTPUT_MODULE_SOURCE_CLOSURE_SHA256)
    if(NOT ARG_${_required})
      message(FATAL_ERROR
        "bioseq_select_gcc16_toolchain requires ${_required}")
    endif()
  endforeach()

  set(_root_candidates)
  if(ARG_REQUESTED_ROOT)
    list(APPEND _root_candidates "${ARG_REQUESTED_ROOT}")
  else()
    list(APPEND _root_candidates
      "/opt/gcc-16.1.0"
      "/usr")
    if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
      # These paths are considered only when the caller explicitly selects
      # the GCC-only environment. They do not make ordinary macOS/Clang builds
      # silently switch their compiler.
      list(APPEND _root_candidates
        "/opt/homebrew/opt/gcc@16"
        "/opt/homebrew/opt/gcc")
    endif()
  endif()
  list(REMOVE_DUPLICATES _root_candidates)

  set(_selected_root "")
  set(_selected_c "")
  set(_selected_cxx "")
  foreach(_root IN LISTS _root_candidates)
    if(EXISTS "${_root}/bin/gcc-16"
        AND EXISTS "${_root}/bin/g++-16")
      file(REAL_PATH "${_root}" _root_real)
      file(REAL_PATH "${_root}/bin/gcc-16" _c_real)
      file(REAL_PATH "${_root}/bin/g++-16" _cxx_real)
      get_filename_component(_c_prefix "${_c_real}/../.." ABSOLUTE)
      get_filename_component(_cxx_prefix "${_cxx_real}/../.." ABSOLUTE)
      file(REAL_PATH "${_c_prefix}" _c_prefix_real)
      file(REAL_PATH "${_cxx_prefix}" _cxx_prefix_real)
      if(_c_prefix_real STREQUAL _cxx_prefix_real
          AND _cxx_prefix_real STREQUAL _root_real)
        set(_selected_root "${_root_real}")
        set(_selected_c "${_c_real}")
        set(_selected_cxx "${_cxx_real}")
        break()
      endif()
    endif()
  endforeach()
  if(NOT _selected_cxx)
    message(FATAL_ERROR
      "GCC-only configuration requires paired gcc-16/g++-16 executables "
      "inside one compiler prefix; searched: ${_root_candidates}")
  endif()

  _bioseq_query_gcc_version("${_selected_cxx}" _gcc_version)
  if(_gcc_version VERSION_LESS "16.1")
    message(FATAL_ERROR
      "GCC-only configuration requires g++ >= 16.1; found "
      "${_gcc_version} at ${_selected_cxx}")
  endif()
  _bioseq_query_gcc_version("${_selected_c}" _gcc_c_version)
  if(NOT _gcc_c_version VERSION_EQUAL _gcc_version)
    message(FATAL_ERROR
      "GCC-only configuration found a version-mismatched gcc/g++ pair: "
      "${_gcc_c_version} at ${_selected_c}, ${_gcc_version} at "
      "${_selected_cxx}")
  endif()

  execute_process(
    COMMAND "${_selected_cxx}"
      -print-file-name=libstdc++.modules.json
    OUTPUT_VARIABLE _printed_modules
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCH "^[0-9]+" _gcc_major "${_gcc_version}")
  file(GLOB _module_candidates LIST_DIRECTORIES FALSE
    "${_selected_root}/lib/gcc/*/${_gcc_version}/libstdc++.modules.json"
    "${_selected_root}/lib/gcc/*/${_gcc_major}/libstdc++.modules.json"
    "${_selected_root}/lib/gcc/current/libstdc++.modules.json")
  if(ARG_REQUESTED_MODULES)
    list(APPEND _module_candidates "${ARG_REQUESTED_MODULES}")
  endif()
  list(APPEND _module_candidates
    "${_printed_modules}"
    "${_selected_root}/lib64/libstdc++.modules.json")
  list(REMOVE_DUPLICATES _module_candidates)

  set(_valid_modules)
  foreach(_candidate IN LISTS _module_candidates)
    _bioseq_gcc_modules_json_is_authentic(
      "${_candidate}" "${_selected_root}" _candidate_valid
      _candidate_metadata_sha256 _candidate_source_closure_sha256)
    if(_candidate_valid)
      file(REAL_PATH "${_candidate}" _candidate_real)
      list(APPEND _valid_modules "${_candidate_real}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _valid_modules)
  list(SORT _valid_modules)

  set(_selected_modules "")
  if(ARG_REQUESTED_MODULES AND NOT EXISTS "${ARG_REQUESTED_MODULES}")
    message(FATAL_ERROR
      "BIOSEQ_LIBSTDCXX_MODULES_JSON does not exist: "
      "${ARG_REQUESTED_MODULES}")
  elseif(ARG_REQUESTED_MODULES)
    file(REAL_PATH "${ARG_REQUESTED_MODULES}" _requested_modules_real)
    list(FIND _valid_modules "${_requested_modules_real}"
      _requested_modules_index)
    if(_requested_modules_index EQUAL -1)
      message(FATAL_ERROR
        "BIOSEQ_LIBSTDCXX_MODULES_JSON is not an authenticated "
        "libstdc++.modules.json from the selected GCC prefix: "
        "${ARG_REQUESTED_MODULES}")
    endif()
    set(_selected_modules "${_requested_modules_real}")
  elseif(_valid_modules)
    list(GET _valid_modules 0 _selected_modules)
  endif()
  if(NOT _selected_modules)
    message(FATAL_ERROR
      "no authenticated libstdc++.modules.json was found for "
      "${_selected_cxx}; every std/std.compat source must resolve inside "
      "the selected compiler prefix")
  endif()

  _bioseq_gcc_modules_json_is_authentic(
    "${_selected_modules}" "${_selected_root}" _selected_modules_valid
    _selected_modules_sha256 _selected_module_source_closure_sha256)
  if(NOT _selected_modules_valid)
    message(FATAL_ERROR
      "selected libstdc++ module inventory lost its authenticated "
      "std/std.compat source closure")
  endif()
  file(SHA256 "${_selected_c}" _selected_c_sha256)
  file(SHA256 "${_selected_cxx}" _selected_cxx_sha256)

  set(${ARG_OUTPUT_ROOT} "${_selected_root}" PARENT_SCOPE)
  set(${ARG_OUTPUT_C} "${_selected_c}" PARENT_SCOPE)
  set(${ARG_OUTPUT_CXX} "${_selected_cxx}" PARENT_SCOPE)
  set(${ARG_OUTPUT_MODULES} "${_selected_modules}" PARENT_SCOPE)
  set(${ARG_OUTPUT_VERSION} "${_gcc_version}" PARENT_SCOPE)
  set(${ARG_OUTPUT_C_SHA256} "${_selected_c_sha256}" PARENT_SCOPE)
  set(${ARG_OUTPUT_CXX_SHA256} "${_selected_cxx_sha256}" PARENT_SCOPE)
  set(${ARG_OUTPUT_MODULES_SHA256}
    "${_selected_modules_sha256}" PARENT_SCOPE)
  set(${ARG_OUTPUT_MODULE_SOURCE_CLOSURE_SHA256}
    "${_selected_module_source_closure_sha256}" PARENT_SCOPE)
endfunction()

function(bioseq_verify_gcc16_toolchain_identity)
  cmake_parse_arguments(PARSE_ARGV 0 ARG
    ""
    "ROOT;C;CXX;MODULES;VERSION;C_SHA256;CXX_SHA256;MODULES_SHA256;MODULE_SOURCE_CLOSURE_SHA256"
    "")
  foreach(_required IN ITEMS
      ROOT C CXX MODULES VERSION C_SHA256 CXX_SHA256 MODULES_SHA256
      MODULE_SOURCE_CLOSURE_SHA256)
    if(NOT ARG_${_required})
      message(FATAL_ERROR
        "bioseq_verify_gcc16_toolchain_identity requires ${_required}")
    endif()
  endforeach()

  foreach(_path_variable IN ITEMS ROOT C CXX MODULES)
    if(NOT EXISTS "${ARG_${_path_variable}}")
      message(FATAL_ERROR
        "authenticated GCC ${_path_variable} path is absent: "
        "${ARG_${_path_variable}}")
    endif()
  endforeach()
  file(REAL_PATH "${ARG_ROOT}" _root_real)
  file(REAL_PATH "${ARG_C}" _c_real)
  file(REAL_PATH "${ARG_CXX}" _cxx_real)
  file(REAL_PATH "${ARG_MODULES}" _modules_real)
  foreach(_owned_path IN ITEMS _c_real _cxx_real _modules_real)
    string(FIND "${${_owned_path}}/" "${_root_real}/" _owned_offset)
    if(NOT _owned_offset EQUAL 0)
      message(FATAL_ERROR
        "authenticated GCC identity path escaped its compiler prefix: "
        "${${_owned_path}}")
    endif()
  endforeach()

  _bioseq_query_gcc_version("${_c_real}" _c_version)
  _bioseq_query_gcc_version("${_cxx_real}" _cxx_version)
  if(NOT _c_version VERSION_EQUAL ARG_VERSION
      OR NOT _cxx_version VERSION_EQUAL ARG_VERSION
      OR _cxx_version VERSION_LESS "16.1")
    message(FATAL_ERROR
      "authenticated GCC compiler version changed: expected "
      "${ARG_VERSION}, found C=${_c_version}, CXX=${_cxx_version}")
  endif()
  file(SHA256 "${_c_real}" _c_sha256)
  file(SHA256 "${_cxx_real}" _cxx_sha256)
  if(NOT _c_sha256 STREQUAL ARG_C_SHA256)
    message(FATAL_ERROR
      "authenticated GCC C compiler SHA-256 changed")
  endif()
  if(NOT _cxx_sha256 STREQUAL ARG_CXX_SHA256)
    message(FATAL_ERROR
      "authenticated GCC C++ compiler SHA-256 changed")
  endif()

  _bioseq_gcc_modules_json_is_authentic(
    "${_modules_real}" "${_root_real}" _modules_valid
    _modules_sha256 _module_source_closure_sha256)
  if(NOT _modules_valid)
    message(FATAL_ERROR
      "authenticated libstdc++ module inventory/source closure is invalid")
  endif()
  if(NOT _modules_sha256 STREQUAL ARG_MODULES_SHA256)
    message(FATAL_ERROR
      "authenticated libstdc++ module inventory SHA-256 changed")
  endif()
  if(NOT _module_source_closure_sha256
      STREQUAL ARG_MODULE_SOURCE_CLOSURE_SHA256)
    message(FATAL_ERROR
      "authenticated libstdc++ module source closure SHA-256 changed")
  endif()
endfunction()
