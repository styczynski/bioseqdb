# Runtime ERROR validation is an explicit, non-ALL gate. One build tree owns the
# active profile DSO and both profile-specific sealed harnesses; the inverse
# profile DSO must come from a separately configured, runtime-instrumented build
# so the two compilation profiles can never collapse to one binary.
if("${BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE}" STREQUAL "")
  if(NOT "${BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR}" STREQUAL "")
    message(FATAL_ERROR
      "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR requires "
      "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE")
  endif()
  add_custom_target(bioseq_sql_postgres_error_safety_evidence
    COMMAND "${CMAKE_COMMAND}" -E echo
      "Runtime safety validation is not configured. Set "
      "BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE to build sealed profile "
      "plans and instrument the active DSO."
    COMMAND "${CMAKE_COMMAND}" -E false
    USES_TERMINAL
    VERBATIM)
else()
  get_filename_component(_bioseq_sql_runtime_case_template
    "${BIOSEQ_SQL_SAFETY_RUNTIME_CASE_TEMPLATE}" ABSOLUTE)
  if(NOT EXISTS "${_bioseq_sql_runtime_case_template}"
     OR IS_DIRECTORY "${_bioseq_sql_runtime_case_template}"
     OR IS_SYMLINK "${_bioseq_sql_runtime_case_template}")
    message(FATAL_ERROR
      "Runtime case template must be an existing regular non-symlink file: "
      "${_bioseq_sql_runtime_case_template}")
  endif()

  find_package(OpenSSL REQUIRED)
  find_package(Threads REQUIRED)
  get_target_property(_bioseq_sql_runtime_control_file
    postgresql_bioseq BIOSEQ_EXTENSION_CONTROL_FILE)
  if(NOT _bioseq_sql_runtime_control_file
     OR _bioseq_sql_runtime_control_file MATCHES "-NOTFOUND$")
    message(FATAL_ERROR
      "postgresql_bioseq did not publish BIOSEQ_EXTENSION_CONTROL_FILE")
  endif()
  if(NOT EXISTS "${_bioseq_sql_runtime_control_file}"
     OR IS_DIRECTORY "${_bioseq_sql_runtime_control_file}"
     OR IS_SYMLINK "${_bioseq_sql_runtime_control_file}")
    message(FATAL_ERROR
      "postgresql_bioseq control file is absent or ambiguous: "
      "${_bioseq_sql_runtime_control_file}")
  endif()

  set(_bioseq_sql_runtime_root
    "${BIOSEQ_SQL_SAFETY_ARTIFACT_ROOT}/runtime")
  set(_bioseq_sql_runtime_control_target
    bioseq_sql_postgres_error_safety_runtime_control)
  bioseq_add_postgres_error_safety_runtime_control(
    TARGET "${_bioseq_sql_runtime_control_target}"
    SOURCE
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_control.cppm"
    INCLUDE_DIRS
      "${CMAKE_SOURCE_DIR}"
    LINKS
      PostgreSQL::ServerHeaders
      ${CMAKE_DL_LIBS})

  foreach(_bioseq_sql_runtime_profile IN ITEMS production test_indexes)
    string(TOUPPER "${_bioseq_sql_runtime_profile}"
      _bioseq_sql_runtime_profile_key)
    set(_bioseq_sql_runtime_harness_target
      "bioseq_sql_postgres_error_safety_runtime_${_bioseq_sql_runtime_profile}_harness")
    bioseq_add_postgres_error_safety_runtime_profile(
      TARGET "${_bioseq_sql_runtime_harness_target}"
      PROFILE "${_bioseq_sql_runtime_profile}"
      LEDGER
        "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_runtime_profile_key}_GENERATED_LEDGER}"
      RUNTIME_INVENTORY
        "${BIOSEQ_SQL_SAFETY_${_bioseq_sql_runtime_profile_key}_RUNTIME_INVENTORY}"
      RUNTIME_INVENTORY_PRODUCER_EXECUTABLE
        "${BIOSEQ_CODEGEN_EXECUTABLE}"
      SQL_MANIFEST
        "${BIOSEQ_SQL_${_bioseq_sql_runtime_profile_key}_GENERATED_MANIFEST}"
      SQL_FILE
        "${BIOSEQ_SQL_${_bioseq_sql_runtime_profile_key}_GENERATED_INSTALL}"
      CONTROL_FILE "${_bioseq_sql_runtime_control_file}"
      CONTROL_DSO_TARGET "${_bioseq_sql_runtime_control_target}"
      RUNTIME_POLICY "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
      CASE_TEMPLATE "${_bioseq_sql_runtime_case_template}"
      HARNESS_MODULES
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_support.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_core_support.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_case_support.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_manifest_support.cppm"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_scenario_support.cppm"
      HARNESS_SOURCES
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_main.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_activation.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_describe.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_run_case.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/support/safety_runtime_harness_self_test.cpp"
      OUTPUT_DIRECTORY
        "${_bioseq_sql_runtime_root}/${_bioseq_sql_runtime_profile}"
      LINKS
        fmt
        PostgreSQL::PostgreSQL
        OpenSSL::Crypto
        nlohmann_json_schema_validator::validator
        Threads::Threads
      DEPENDS
        "bioseq_sql_postgres_error_safety_${_bioseq_sql_runtime_profile}"
        "bioseq_sql_postgres_error_safety_inventory_${_bioseq_sql_runtime_profile}"
        "bioseq_sql_final_${_bioseq_sql_runtime_profile}")
    set(_bioseq_sql_runtime_plan_variable
      "${_bioseq_sql_runtime_harness_target}_CASE_PLAN")
    set(_bioseq_sql_runtime_self_test_variable
      "${_bioseq_sql_runtime_harness_target}_SELF_TEST_STAMP")
    set(BIOSEQ_SQL_SAFETY_RUNTIME_${_bioseq_sql_runtime_profile_key}_HARNESS
      "${_bioseq_sql_runtime_harness_target}")
    set(BIOSEQ_SQL_SAFETY_RUNTIME_${_bioseq_sql_runtime_profile_key}_PLAN
      "${${_bioseq_sql_runtime_plan_variable}}")
    set(BIOSEQ_SQL_SAFETY_RUNTIME_${_bioseq_sql_runtime_profile_key}_SELF_TEST
      "${${_bioseq_sql_runtime_self_test_variable}}")
  endforeach()

  if(BIOSEQ_TEST_INDEXES)
    set(_bioseq_sql_runtime_active_profile test_indexes)
  else()
    set(_bioseq_sql_runtime_active_profile production)
  endif()
  target_compile_definitions(postgresql_bioseq PRIVATE
    BIOSEQ_SAFETY_RUNTIME_PROBES=1
    "BIOSEQ_SAFETY_RUNTIME_PROFILE=\"${_bioseq_sql_runtime_active_profile}\"")
  target_compile_definitions(bioseq_pipeline_dispatch_owners PRIVATE
    BIOSEQ_SAFETY_RUNTIME_PROBES=1
    "BIOSEQ_SAFETY_RUNTIME_PROFILE=\"${_bioseq_sql_runtime_active_profile}\"")
  target_compile_definitions(bioseq_pg_support PRIVATE
    BIOSEQ_SAFETY_RUNTIME_PROBES=1
    "BIOSEQ_SAFETY_RUNTIME_PROFILE=\"${_bioseq_sql_runtime_active_profile}\"")
  target_compile_definitions(bioseq_pg_sql_boundary PRIVATE
    BIOSEQ_SAFETY_RUNTIME_PROBES=1
    "BIOSEQ_SAFETY_RUNTIME_PROFILE=\"${_bioseq_sql_runtime_active_profile}\"")

  add_custom_target(bioseq_sql_postgres_error_safety_runtime_profiles
    DEPENDS
      "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_SELF_TEST}"
      "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_SELF_TEST}")
  add_dependencies(
    bioseq_sql_postgres_error_safety_runtime_profiles
    postgresql_bioseq
    "${_bioseq_sql_runtime_control_target}"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_HARNESS}_self_test"
    "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_HARNESS}_self_test")

  if("${BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR}" STREQUAL "")
    add_custom_target(bioseq_sql_postgres_error_safety_evidence
      COMMAND "${CMAKE_COMMAND}" -E echo
        "Sealed profile artifacts are configured, but the live "
        "dual-profile validation gate requires "
        "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR."
      COMMAND "${CMAKE_COMMAND}" -E false
      USES_TERMINAL
      VERBATIM)
  else()
    if(BIOSEQ_TEST_INDEXES)
      message(FATAL_ERROR
        "Aggregate validation must run from the production profile "
        "with a sibling test-index build tree")
    endif()
    if(NOT BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE STREQUAL "isolated")
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_CLUSTER_MODE must be isolated for "
        "dual-profile validation")
    endif()
    if(NOT BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS
       MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS must be an integer in "
        "the range 1..600")
    elseif(BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS GREATER 600)
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS must be an integer in "
        "the range 1..600")
    endif()
    get_filename_component(_bioseq_sql_runtime_peer_build_dir
      "${BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR}" ABSOLUTE)
    if("${_bioseq_sql_runtime_peer_build_dir}" STREQUAL
       "${CMAKE_BINARY_DIR}")
      message(FATAL_ERROR
        "BIOSEQ_SQL_SAFETY_RUNTIME_PEER_BUILD_DIR must name an isolated "
        "sibling build tree")
    endif()

    # The sibling profile is a build output, so configure must not probe it.
    # The exact expected path becomes a build dependency and fails closed if
    # the separately built test-index tree did not publish that DSO.
    set(_bioseq_sql_runtime_production_dso
      "$<TARGET_FILE:postgresql_bioseq>")
    string(CONCAT _bioseq_sql_runtime_test_indexes_dso
      "${_bioseq_sql_runtime_peer_build_dir}/bioseq/"
      "$<TARGET_FILE_NAME:postgresql_bioseq>")

    set(_bioseq_sql_runtime_lifecycle
      "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/run_private_postgres_safety_tests.py")
    set(_bioseq_sql_runtime_stage_script
      "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/stage_private_postgres.cmake")
    set(_bioseq_sql_runtime_config_template
      "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/private_postgresql.conf.in")
    set(_bioseq_sql_runtime_producer
      "${CMAKE_CURRENT_SOURCE_DIR}/../codegen/safety/run_postgres_error_safety_tests.py")
    file(REAL_PATH "${CMAKE_COMMAND}"
      _bioseq_sql_runtime_cmake_executable)
    if(NOT EXISTS "${_bioseq_sql_runtime_cmake_executable}"
       OR IS_DIRECTORY "${_bioseq_sql_runtime_cmake_executable}"
       OR IS_SYMLINK "${_bioseq_sql_runtime_cmake_executable}")
      message(FATAL_ERROR
        "Runtime validation requires CMake to resolve to a regular "
        "executable: ${_bioseq_sql_runtime_cmake_executable}")
    endif()
    foreach(_bioseq_sql_runtime_source IN ITEMS
        _bioseq_sql_runtime_lifecycle
        _bioseq_sql_runtime_stage_script
        _bioseq_sql_runtime_config_template
        _bioseq_sql_runtime_producer)
      if(NOT EXISTS "${${_bioseq_sql_runtime_source}}"
         OR IS_DIRECTORY "${${_bioseq_sql_runtime_source}}")
        message(FATAL_ERROR
          "Runtime validation source is missing: "
          "${${_bioseq_sql_runtime_source}}")
      endif()
    endforeach()

    set(_bioseq_sql_runtime_candidate_root
      "${_bioseq_sql_runtime_root}/candidate")
    set(_bioseq_sql_runtime_candidate_production_ledger
      "${_bioseq_sql_runtime_candidate_root}/production-ledger.json")
    set(_bioseq_sql_runtime_candidate_test_indexes_ledger
      "${_bioseq_sql_runtime_candidate_root}/test-indexes-ledger.json")
    set(_bioseq_sql_runtime_candidate_matrix
      "${_bioseq_sql_runtime_candidate_root}/runtime-matrix.json")
    set(_bioseq_sql_runtime_candidate_stamp
      "${_bioseq_sql_runtime_candidate_root}/candidate.stamp")
    set(_bioseq_sql_runtime_candidate_production_stage_manifest
      "${_bioseq_sql_runtime_candidate_root}/stages/production/stage-manifest.json")
    set(_bioseq_sql_runtime_candidate_test_indexes_stage_manifest
      "${_bioseq_sql_runtime_candidate_root}/stages/test_indexes/stage-manifest.json")
    set(_bioseq_sql_runtime_reviewed_root
      "${_bioseq_sql_runtime_candidate_root}/reviewed")
    set(_bioseq_sql_runtime_validation_report
      "${_bioseq_sql_runtime_candidate_root}/runtime-validation.json")

    add_custom_command(
      OUTPUT
        "${_bioseq_sql_runtime_candidate_production_ledger}"
        "${_bioseq_sql_runtime_candidate_test_indexes_ledger}"
        "${_bioseq_sql_runtime_candidate_matrix}"
        "${_bioseq_sql_runtime_candidate_stamp}"
        "${_bioseq_sql_runtime_candidate_production_stage_manifest}"
        "${_bioseq_sql_runtime_candidate_test_indexes_stage_manifest}"
      COMMAND "${_bioseq_sql_runtime_cmake_executable}" -E env
        "PYTHONDONTWRITEBYTECODE=1"
        "${Python3_EXECUTABLE}"
        "${_bioseq_sql_runtime_lifecycle}"
        --cmake "${_bioseq_sql_runtime_cmake_executable}"
        --pg-config "${PostgreSQL_CONFIG}"
        --stage-script "${_bioseq_sql_runtime_stage_script}"
        --config-template "${_bioseq_sql_runtime_config_template}"
        --runtime-producer "${_bioseq_sql_runtime_producer}"
        --production-ledger
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_GENERATED_LEDGER}"
        --test-indexes-ledger
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_GENERATED_LEDGER}"
        --production-runtime-inventory
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_RUNTIME_INVENTORY}"
        --test-indexes-runtime-inventory
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_RUNTIME_INVENTORY}"
        --runtime-inventory-producer-executable
          "${BIOSEQ_CODEGEN_EXECUTABLE}"
        --production-sql-manifest
          "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
        --test-indexes-sql-manifest
          "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
        --production-sql-file
          "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}"
        --test-indexes-sql-file
          "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_INSTALL}"
        --control-file "${_bioseq_sql_runtime_control_file}"
        --control-dso "$<TARGET_FILE:${_bioseq_sql_runtime_control_target}>"
        --runtime-policy "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
        --production-case-plan
          "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_PLAN}"
        --test-indexes-case-plan
          "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_PLAN}"
        --production-dso "${_bioseq_sql_runtime_production_dso}"
        --test-indexes-dso "${_bioseq_sql_runtime_test_indexes_dso}"
        --production-test-executable
          "$<TARGET_FILE:${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_HARNESS}>"
        --test-indexes-test-executable
          "$<TARGET_FILE:${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_HARNESS}>"
        --output-root "${_bioseq_sql_runtime_candidate_root}"
        --stamp "${_bioseq_sql_runtime_candidate_stamp}"
        --timeout-seconds
          "${BIOSEQ_SQL_SAFETY_RUNTIME_TIMEOUT_SECONDS}"
      DEPENDS
        bioseq_sql_postgres_error_safety_runtime_profiles
        postgresql_bioseq
        "${_bioseq_sql_runtime_control_target}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_HARNESS}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_HARNESS}"
        "${_bioseq_sql_runtime_lifecycle}"
        "${_bioseq_sql_runtime_stage_script}"
        "${_bioseq_sql_runtime_config_template}"
        "${_bioseq_sql_runtime_producer}"
        "${_bioseq_sql_runtime_cmake_executable}"
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_GENERATED_LEDGER}"
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_GENERATED_LEDGER}"
        "${BIOSEQ_SQL_SAFETY_PRODUCTION_RUNTIME_INVENTORY}"
        "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_RUNTIME_INVENTORY}"
        "${BIOSEQ_CODEGEN_EXECUTABLE}"
        "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
        "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
        "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}"
        "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_INSTALL}"
        "${_bioseq_sql_runtime_control_file}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_PLAN}"
        "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_PLAN}"
        "${_bioseq_sql_runtime_test_indexes_dso}"
      COMMENT
        "Publishing dual-profile PostgreSQL ERROR runtime validation candidate"
      USES_TERMINAL
      VERBATIM)

    add_custom_target(bioseq_sql_postgres_error_safety_candidate
      DEPENDS
        "${_bioseq_sql_runtime_candidate_production_ledger}"
        "${_bioseq_sql_runtime_candidate_test_indexes_ledger}"
        "${_bioseq_sql_runtime_candidate_matrix}"
        "${_bioseq_sql_runtime_candidate_stamp}"
        "${_bioseq_sql_runtime_candidate_production_stage_manifest}"
        "${_bioseq_sql_runtime_candidate_test_indexes_stage_manifest}")

    add_custom_target(bioseq_sql_postgres_error_safety_evidence
      BYPRODUCTS "${_bioseq_sql_runtime_validation_report}"
      COMMAND "${_bioseq_sql_runtime_cmake_executable}" -E rm -f
        "${_bioseq_sql_runtime_validation_report}"
      COMMAND "${_bioseq_sql_runtime_cmake_executable}" -E env
        "PYTHONDONTWRITEBYTECODE=1"
        "${Python3_EXECUTABLE}"
        "${BIOSEQ_SQL_VALIDATOR}"
        --evidence-only
        --require-complete-codegen
        --production-ledger
          "${_bioseq_sql_runtime_candidate_production_ledger}"
        --test-indexes-ledger
          "${_bioseq_sql_runtime_candidate_test_indexes_ledger}"
        --reviewed-production-ledger
          "${_bioseq_sql_runtime_reviewed_root}/production-ledger.json"
        --reviewed-test-indexes-ledger
          "${_bioseq_sql_runtime_reviewed_root}/test-indexes-ledger.json"
        --production-source-list
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_SOURCE_LIST}"
        --test-indexes-source-list
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_SOURCE_LIST}"
        --production-compile-database
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_DATABASE}"
        --test-indexes-compile-database
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_DATABASE}"
        --production-compile-graph
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILE_GRAPH}"
        --test-indexes-compile-graph
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILE_GRAPH}"
        --production-pcm-manifest
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_PCM_MANIFEST}"
        --test-indexes-pcm-manifest
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_PCM_MANIFEST}"
        --production-compiler-facts
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_COMPILER_FACTS}"
        --test-indexes-compiler-facts
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_COMPILER_FACTS}"
        --production-compiler-facts-producer-executable
          "${BIOSEQ_CODEGEN_EXECUTABLE}"
        --test-indexes-compiler-facts-producer-executable
          "${BIOSEQ_CODEGEN_EXECUTABLE}"
        --provenance "${BIOSEQ_SQL_SAFETY_PROVENANCE}"
        --production-sql-manifest
          "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
        --test-indexes-sql-manifest
          "${BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST}"
        --runtime-policy "${BIOSEQ_SQL_SAFETY_RUNTIME_POLICY}"
        --runtime-matrix "${_bioseq_sql_runtime_candidate_matrix}"
        --reviewed-runtime-matrix
          "${_bioseq_sql_runtime_reviewed_root}/runtime-matrix.json"
        --production-case-plan
          "${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_PLAN}"
        --test-indexes-case-plan
          "${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_PLAN}"
        --production-runtime-inventory
          "${BIOSEQ_SQL_SAFETY_PRODUCTION_RUNTIME_INVENTORY}"
        --test-indexes-runtime-inventory
          "${BIOSEQ_SQL_SAFETY_TEST_INDEXES_RUNTIME_INVENTORY}"
        --runtime-inventory-producer-executable
          "${BIOSEQ_CODEGEN_EXECUTABLE}"
        --production-profile-dso
          "${_bioseq_sql_runtime_production_dso}"
        --test-indexes-profile-dso
          "${_bioseq_sql_runtime_test_indexes_dso}"
        --production-stage-manifest
          "${_bioseq_sql_runtime_candidate_production_stage_manifest}"
        --test-indexes-stage-manifest
          "${_bioseq_sql_runtime_candidate_test_indexes_stage_manifest}"
        --runtime-control-dso
          "$<TARGET_FILE:${_bioseq_sql_runtime_control_target}>"
        --runtime-producer-executable
          "${_bioseq_sql_runtime_producer}"
        --production-runtime-test-executable
          "$<TARGET_FILE:${BIOSEQ_SQL_SAFETY_RUNTIME_PRODUCTION_HARNESS}>"
        --test-indexes-runtime-test-executable
          "$<TARGET_FILE:${BIOSEQ_SQL_SAFETY_RUNTIME_TEST_INDEXES_HARNESS}>"
        --report "${_bioseq_sql_runtime_validation_report}"
      DEPENDS
        "${_bioseq_sql_runtime_candidate_production_ledger}"
        "${_bioseq_sql_runtime_candidate_test_indexes_ledger}"
        "${_bioseq_sql_runtime_candidate_matrix}"
        "${_bioseq_sql_runtime_candidate_stamp}"
        "${_bioseq_sql_runtime_candidate_production_stage_manifest}"
        "${_bioseq_sql_runtime_candidate_test_indexes_stage_manifest}"
        "${_bioseq_sql_runtime_cmake_executable}"
      COMMENT
        "Validating promoted dual-profile PostgreSQL ERROR runtime results"
      USES_TERMINAL
      VERBATIM)
  endif()
endif()
