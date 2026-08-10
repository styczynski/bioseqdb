# Live PostgreSQL 17 acceptance for the feature-gated external SQL package.
# Include this after add_subdirectory(bioseq), when postgresql_bioseq and the
# generated SQL contract target are available.

if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  foreach(_bioseq_external_acceptance_target
          postgresql_bioseq bioseq_sql_external_feature_contract)
    if(NOT TARGET "${_bioseq_external_acceptance_target}")
      message(FATAL_ERROR
        "External PostgreSQL acceptance requires target "
        "${_bioseq_external_acceptance_target}")
    endif()
  endforeach()

  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_package(Git REQUIRED)

  get_target_property(_bioseq_external_acceptance_control
    postgresql_bioseq BIOSEQ_EXTENSION_CONTROL_FILE)
  get_target_property(_bioseq_external_acceptance_sql
    postgresql_bioseq BIOSEQ_EXTENSION_INSTALL_SQL)
  get_target_property(_bioseq_external_acceptance_manifest
    postgresql_bioseq BIOSEQ_EXTENSION_SQL_MANIFEST)
  foreach(_bioseq_external_acceptance_property IN ITEMS
          _bioseq_external_acceptance_control
          _bioseq_external_acceptance_sql
          _bioseq_external_acceptance_manifest)
    set(_bioseq_external_acceptance_property_value
      "${${_bioseq_external_acceptance_property}}")
    if(_bioseq_external_acceptance_property_value STREQUAL ""
       OR _bioseq_external_acceptance_property_value MATCHES "-NOTFOUND$")
      message(FATAL_ERROR
        "postgresql_bioseq did not publish "
        "${_bioseq_external_acceptance_property}")
    endif()
  endforeach()
  if(NOT PostgreSQL_CONFIG)
    message(FATAL_ERROR
      "External PostgreSQL acceptance requires PostgreSQL_CONFIG")
  endif()

  set(_bioseq_external_acceptance_runner
    "${CMAKE_CURRENT_LIST_DIR}/run_external_postgres_acceptance.py")
  set(_bioseq_external_acceptance_stage
    "${CMAKE_CURRENT_LIST_DIR}/stage_private_postgres.cmake")
  set(_bioseq_external_acceptance_config
    "${CMAKE_CURRENT_LIST_DIR}/private_postgresql.conf.in")
  set(_bioseq_external_acceptance_static_contract
    "${BIOSEQ_REPOSITORY_ROOT}/codegen/sql/tests/contract/test_external_feature.py")
  set(_bioseq_external_acceptance_output
    "${CMAKE_BINARY_DIR}/bioseq/generated/sql/external-postgres-acceptance")

  add_custom_target(bioseq_external_postgres_acceptance
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONDONTWRITEBYTECODE=1"
      "${Python3_EXECUTABLE}"
      "${_bioseq_external_acceptance_runner}"
      self-test
    COMMAND "${CMAKE_COMMAND}" -E env
      "PYTHONDONTWRITEBYTECODE=1"
      "${Python3_EXECUTABLE}"
      "${_bioseq_external_acceptance_runner}"
      run
      --repository-root "${BIOSEQ_REPOSITORY_ROOT}"
      --output-root "${_bioseq_external_acceptance_output}"
      --cmake "${CMAKE_COMMAND}"
      --git "${GIT_EXECUTABLE}"
      --pg-config "${PostgreSQL_CONFIG}"
      --stage-script "${_bioseq_external_acceptance_stage}"
      --config-template "${_bioseq_external_acceptance_config}"
      --control-file "${_bioseq_external_acceptance_control}"
      --install-sql "${_bioseq_external_acceptance_sql}"
      --manifest "${_bioseq_external_acceptance_manifest}"
      --dso "$<TARGET_FILE:postgresql_bioseq>"
      --static-contract-script
        "${_bioseq_external_acceptance_static_contract}"
      --cmake-fragment "${CMAKE_CURRENT_LIST_FILE}"
      --mode-source "${BIOSEQ_REPOSITORY_ROOT}/build_mcp/server.py"
      --timeout-seconds 180
    DEPENDS
      postgresql_bioseq
      bioseq_sql_external_feature_contract
      "${_bioseq_external_acceptance_runner}"
      "${_bioseq_external_acceptance_stage}"
      "${_bioseq_external_acceptance_config}"
      "${_bioseq_external_acceptance_control}"
      "${_bioseq_external_acceptance_sql}"
      "${_bioseq_external_acceptance_manifest}"
      "${_bioseq_external_acceptance_static_contract}"
      "${CMAKE_CURRENT_LIST_FILE}"
      "${BIOSEQ_REPOSITORY_ROOT}/build_mcp/server.py"
      "${CMAKE_CURRENT_LIST_DIR}/private_postgres.py"
    COMMENT
      "Running external-enabled generated SQL in a clean private PostgreSQL 17 cluster"
    USES_TERMINAL
    VERBATIM)
  set_target_properties(bioseq_external_postgres_acceptance PROPERTIES
    BIOSEQ_EXTERNAL_POSTGRES_ACCEPTANCE_OUTPUT_ROOT
      "${_bioseq_external_acceptance_output}")
endif()
