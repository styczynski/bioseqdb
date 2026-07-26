# bench/bench_e2e.cmake — DOC0286 Part 2. End-to-end SQL benchmark target.
# Mirrors bench_v10.cmake (AddCommonTargetX) but links the SQL client + Catch2.
# Self-define the path vars exactly like bench_v10.cmake:8-10 (do NOT rely on outer
# scope) so this file is `include()`-able from the same top-level CMake that
# includes bench_v10.cmake:
get_filename_component(BIOSEQ_DIR "${CMAKE_CURRENT_LIST_DIR}/../bioseq" ABSOLUTE)
set(BENCH_FRAMEWORK_DIR "${BIOSEQ_DIR}/../bench_framework")
set(BENCH_E2E_DIR "${CMAKE_CURRENT_LIST_DIR}")        # = main/bench

# minimap2 CLI binary the bench shells out to. -D cache override wins; otherwise
# default to PATH `minimap2` (DOC0286 R3). The bench aborts at run time if the
# binary is absent or any command exits non-zero.
if(NOT DEFINED BIOSEQ_MINIMAP2_BIN OR BIOSEQ_MINIMAP2_BIN STREQUAL "")
  set(BIOSEQ_MINIMAP2_BIN "minimap2")
endif()
# BIOSEQ_MINIMAP2_BIN defaults to the PATH-resolved executable above.

# Keep the code-generation guardrail inventory closed over the declared
# production outputs. Directory scans would also count stale files left by an
# earlier generator shape and would omit the core owner modules.
set(_bioseq_sql_guardrail_sql_file_list
  "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}")
set(_bioseq_sql_guardrail_json_file_list
  "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
  "${BIOSEQ_SQL_PRODUCTION_SUPPORT_MANIFEST}"
  "${BIOSEQ_SQL_CORE_MANIFEST}")
set(_bioseq_sql_guardrail_cpp_file_list
  ${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES}
  ${BIOSEQ_SQL_PRODUCTION_ROW_CODEC_MODULES}
  ${BIOSEQ_SQL_GUC_production_MODULES}
  ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES})
foreach(_bioseq_sql_guardrail_inventory IN ITEMS
    _bioseq_sql_guardrail_sql_file_list
    _bioseq_sql_guardrail_json_file_list
    _bioseq_sql_guardrail_cpp_file_list)
  if("${${_bioseq_sql_guardrail_inventory}}" STREQUAL "")
    message(FATAL_ERROR
      "SQL code-generation guardrail inventory is empty: "
      "${_bioseq_sql_guardrail_inventory}")
  endif()
endforeach()
string(JOIN "|"
  _bioseq_sql_guardrail_sql_files
  ${_bioseq_sql_guardrail_sql_file_list})
string(JOIN "|"
  _bioseq_sql_guardrail_json_files
  ${_bioseq_sql_guardrail_json_file_list})
string(JOIN "|"
  _bioseq_sql_guardrail_cpp_files
  ${_bioseq_sql_guardrail_cpp_file_list})

set(BIOSEQ_SQL_GUARDRAIL_TOOL
  "${BENCH_E2E_DIR}/sql_codegen_guardrail.py")
set(BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG
  "${BENCH_E2E_DIR}/bench_sql_codegen_guardrail_calibration_configuration.json")
set(BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE
  "${BENCH_E2E_DIR}/baselines/doc0388_sql_codegen_reviewed.json"
  CACHE FILEPATH
  "Reviewed DOC0388 Stage-0 or explicitly approved recovered baseline")
set(BIOSEQ_SQL_GUARDRAIL_QUALIFICATION
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/guardrail/qualification.json"
  CACHE FILEPATH
  "Passing DOC0388 SQL codegen qualification consumed by the benchmark")
set(BIOSEQ_SQL_GUARDRAIL_CANDIDATE_MEASUREMENT
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/guardrail/candidate-measurement.json"
  CACHE FILEPATH
  "Raw DOC0388 candidate measurement produced in a dedicated build tree")
mark_as_advanced(
  BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE
  BIOSEQ_SQL_GUARDRAIL_CANDIDATE_MEASUREMENT
  BIOSEQ_SQL_GUARDRAIL_QUALIFICATION)
if(NOT EXISTS "${BIOSEQ_SQL_GUARDRAIL_TOOL}")
  message(FATAL_ERROR
    "Missing DOC0388 SQL codegen guardrail tool: "
    "${BIOSEQ_SQL_GUARDRAIL_TOOL}")
endif()

# The same closed artifact list is consumed by the in-process size benchmark
# and by the destructive, dedicated-build-tree measurement tool.  A TSV avoids
# shell quoting and ensures calibration cannot drift to directory scanning.
set(BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/guardrail/artifacts.tsv")
file(MAKE_DIRECTORY
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/guardrail")
set(_bioseq_sql_guardrail_artifact_inventory
  "# DOC0388 generated artifact inventory v1\n")
foreach(_bioseq_sql_guardrail_path
    IN LISTS _bioseq_sql_guardrail_sql_file_list)
  string(APPEND _bioseq_sql_guardrail_artifact_inventory
    "sql\t${_bioseq_sql_guardrail_path}\n")
endforeach()
foreach(_bioseq_sql_guardrail_path
    IN LISTS _bioseq_sql_guardrail_json_file_list)
  string(APPEND _bioseq_sql_guardrail_artifact_inventory
    "json\t${_bioseq_sql_guardrail_path}\n")
endforeach()
foreach(_bioseq_sql_guardrail_path
    IN LISTS _bioseq_sql_guardrail_cpp_file_list)
  string(APPEND _bioseq_sql_guardrail_artifact_inventory
    "cpp\t${_bioseq_sql_guardrail_path}\n")
endforeach()
file(GENERATE
  OUTPUT "${BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY}"
  CONTENT "${_bioseq_sql_guardrail_artifact_inventory}")

# The cold-generator boundary removes only outputs owned by the FINAL
# production generator edge. Support/core/entity-handle dependencies remain
# primed and hashed; recursively cleaning the target would time their compiler
# and prepare closures instead of the generator itself.
set(_bioseq_sql_guardrail_generator_output_file_list
  "${BIOSEQ_SQL_PRODUCTION_GENERATED_STAMP}"
  "${BIOSEQ_SQL_PRODUCTION_GENERATED_INSTALL}"
  "${BIOSEQ_SQL_PRODUCTION_GENERATED_MANIFEST}"
  ${BIOSEQ_SQL_PRODUCTION_FINAL_FMGR_ENTRYPOINT_MODULES})
list(REMOVE_DUPLICATES
  _bioseq_sql_guardrail_generator_output_file_list)
if("${_bioseq_sql_guardrail_generator_output_file_list}" STREQUAL "")
  message(FATAL_ERROR
    "SQL guardrail generator-output inventory is empty")
endif()
set(BIOSEQ_SQL_GUARDRAIL_GENERATOR_OUTPUT_INVENTORY
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/guardrail/generator-outputs.txt")
set(_bioseq_sql_guardrail_generator_outputs "")
foreach(_bioseq_sql_guardrail_path
    IN LISTS _bioseq_sql_guardrail_generator_output_file_list)
  string(APPEND _bioseq_sql_guardrail_generator_outputs
    "${_bioseq_sql_guardrail_path}\n")
endforeach()
file(GENERATE
  OUTPUT "${BIOSEQ_SQL_GUARDRAIL_GENERATOR_OUTPUT_INVENTORY}"
  CONTENT "${_bioseq_sql_guardrail_generator_outputs}")

# Explicit workflow targets are intentionally not dependencies of compilation:
# calibration must be able to build the benchmark. The MCP selects this
# workflow-only benchmark only for sealed calibration/qualification requests;
# ordinary bench-e2e runs retain the five pre-existing benchmark keys.
add_custom_target(bioseq_sql_guardrail_validate_reviewed_baseline
  COMMAND "${Python3_EXECUTABLE}"
    "${BIOSEQ_SQL_GUARDRAIL_TOOL}" validate-baseline
    --baseline "${BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE}"
  COMMENT "Validating reviewed DOC0388 SQL codegen baseline"
  VERBATIM)
add_custom_target(bioseq_sql_guardrail_qualify
  COMMAND "${CMAKE_COMMAND}" -E echo
    "DOC0388 qualification is MCP-workflow-only; use bioseq-builder bench-e2e with a sealed candidate_qualification request"
  COMMAND "${CMAKE_COMMAND}" -E false
  COMMENT "Rejecting non-MCP DOC0388 qualification"
  VERBATIM)

# Module TUs the bench owns directly. The FASTA/parse helpers are folded into
# bench_e2e_main.cpp (SOURCES, HWY-wrapped) — NOT a module (§11). The shared
# NO_POSTGRES modules (IO reader, generator, sequences, instrumentation) arrive via
# SHARED_MODULE_LIBS below. integration_tests/common.cppm is the SQL client; it
# export-imports testing, so this target must own testing.cppm's BMI too.
set(BENCH_E2E_MOD_FILES
  "${BIOSEQ_DIR}/backend/common/testing.cppm"
  "${BIOSEQ_DIR}/integration_tests/common.cppm"
  "${BENCH_FRAMEWORK_DIR}/index.cppm"
  "${BENCH_E2E_DIR}/bench_sql_codegen_guardrail.cppm")

AddCommonTargetX(
  bioseq_bench_e2e
  TYPE EXEC CXX 26
  MAIN "bench_e2e_main\\.cpp"
  RELOCATABLE NAME bioseq_bench_e2e
  COMMENT "BioSeq end-to-end SQL (bioseq.load + mmsearch) benchmark"
  VERSION ${INSTALL_VERSION} COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}/../config" "${BIOSEQ_DIR}/include2"
    "${BIOSEQ_DIR}/backend"
    "${BIOSEQ_DIR}/../vendor/fmtlog" "${BIOSEQ_DIR}/../vendor/fmt"
    "${BIOSEQ_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static fmt reflectcpp::reflectcpp libassert::assert
    foonathan_memory ftxui::component ftxui::dom ftxui::screen
    simdjson::simdjson nlohmann_json_schema_validator::validator
    TBB::tbb lfreist-hwinfo::hwinfo
    PostgreSQL::PostgreSQL                     # libpq — the SQL client (NEW vs bench_v10)
    sqlgen::sqlgen                             # sqlgen PostgresV2Connection (find_package imported target)
    Catch2::Catch2                             # common.cppm `export import testing` (ASSERT macro)
    Vulkan::Vulkan                             # Vulkan loader (bioseq_modules_nopg uses vulkan/shader_compiler)
    Vulkan::glslang                            # GLSL→SPIR-V compiler (shader_compiler.cppm)
    spirv-cross-msl                            # SPIR-V → MSL cross-compiler (MoltenVK path)
    Vulkan::LayerSettings                      # validation-layer settings (Vulkan::Vulkan companion)
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BENCH_E2E_DIR}/bench_e2e_main.cpp"
    "${BENCH_FRAMEWORK_DIR}/interactive.cppm"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS "${BIOSEQ_DIR}" "${BENCH_E2E_DIR}" "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BENCH_E2E_MOD_FILES}                     # common.cppm + bench index.cppm (fasta helpers are in main.cpp SOURCES)
  HWY 1.3.0)

# Compile the production serializer itself into a dedicated Catch2 executable.
# This is deliberately a separate target instead of AddCommonTargetX(TESTS):
# the latter changes the production executable's Catch2 main/definitions too.
AddCommonTargetX(
  bioseq_bench_e2e_tests_exec
  TYPE EXEC CXX 26
  RELOCATABLE NAME bioseq_bench_e2e_tests_exec
  COMMENT "BioSeq E2E raw serializer contract tests"
  VERSION ${INSTALL_VERSION} COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${BIOSEQ_DIR}/../config" "${BIOSEQ_DIR}/include2"
    "${BIOSEQ_DIR}/backend"
    "${BIOSEQ_DIR}/../vendor/fmtlog" "${BIOSEQ_DIR}/../vendor/fmt"
    "${BIOSEQ_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static fmt reflectcpp::reflectcpp libassert::assert
    foonathan_memory ftxui::component ftxui::dom ftxui::screen
    simdjson::simdjson nlohmann_json_schema_validator::validator
    TBB::tbb lfreist-hwinfo::hwinfo
    PostgreSQL::PostgreSQL
    sqlgen::sqlgen
    Catch2::Catch2WithMain
    Vulkan::Vulkan
    Vulkan::glslang
    spirv-cross-msl
    Vulkan::LayerSettings
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SOURCES
    "${BENCH_E2E_DIR}/bench_e2e_main_test.cpp"
    "${BENCH_FRAMEWORK_DIR}/interactive.cppm"
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS "${BIOSEQ_DIR}" "${BENCH_E2E_DIR}" "${BENCH_FRAMEWORK_DIR}"
  CXX_MOD_FILES
    ${BENCH_E2E_MOD_FILES}
  HWY 1.3.0)

# Match the project's generated Catch2 targets: ThinLTO can drop TLS init
# wrappers for module-attached mlogging state in test executables.
set_target_properties(bioseq_bench_e2e_tests_exec PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE)

# Retained handwritten DOC0388 comparators live in their own test-only DSO.
# They must never enter postgresql_bioseq: doing so would alter the production
# export/safety closure.  Linking the generated row-codec provider here gives
# this isolated module the exact BMI/object closure needed by its comparator.
add_library(bioseq_sql_guardrail_comparators MODULE EXCLUDE_FROM_ALL
  "${BENCH_E2E_DIR}/sql_codegen_handwritten_comparators.cpp")
target_include_directories(bioseq_sql_guardrail_comparators PRIVATE
  "${BIOSEQ_DIR}/../config"
  "${BIOSEQ_DIR}/include2"
  "${BIOSEQ_DIR}/backend")
target_link_libraries(bioseq_sql_guardrail_comparators PRIVATE
  PostgreSQL::ServerHeaders
  bioseq_sql_compile_contract_providers)
target_compile_definitions(bioseq_sql_guardrail_comparators PRIVATE
  POSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})
target_compile_options(bioseq_sql_guardrail_comparators PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
set_target_properties(bioseq_sql_guardrail_comparators PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  POSITION_INDEPENDENT_CODE ON
  PREFIX "")
if(APPLE)
  target_link_options(bioseq_sql_guardrail_comparators PRIVATE
    "-Wl,-undefined,dynamic_lookup")
endif()
add_dependencies(
  bioseq_sql_guardrail_comparators
  bioseq_sql_final)
add_dependencies(
  bioseq_bench_e2e
  bioseq_sql_guardrail_comparators)
if(TARGET bioseq_bench_e2e_tests_exec)
  add_dependencies(
    bioseq_bench_e2e_tests_exec
    bioseq_sql_guardrail_comparators)
endif()

target_compile_definitions(bioseq_bench_e2e PRIVATE
  "BIOSEQ_E2E_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bioseq_bench_e2e\""
  "BIOSEQ_E2E_DATA_DIR=\"${BENCH_E2E_DIR}\""
  "BIOSEQ_E2E_CONFIG_PATH=\"${BENCH_E2E_DIR}/bench_e2e_configuration.json\""
  "BIOSEQ_E2E_MINIMAP2_BIN=\"${BIOSEQ_MINIMAP2_BIN}\""
  "BIOSEQ_SQL_GUARDRAIL_CONFIG_PATH=\"${BENCH_E2E_DIR}/bench_sql_codegen_guardrail_configuration.json\""
  "BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG_PATH=\"${BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG}\""
  "BIOSEQ_SQL_GUARDRAIL_SQL_FILES=\"${_bioseq_sql_guardrail_sql_files}\""
  "BIOSEQ_SQL_GUARDRAIL_JSON_FILES=\"${_bioseq_sql_guardrail_json_files}\""
  "BIOSEQ_SQL_GUARDRAIL_CPP_FILES=\"${_bioseq_sql_guardrail_cpp_files}\""
  "BIOSEQ_SQL_GUARDRAIL_QUALIFICATION_PATH=\"${BIOSEQ_SQL_GUARDRAIL_QUALIFICATION}\""
  "BIOSEQ_SQL_GUARDRAIL_TOOL_PATH=\"${BIOSEQ_SQL_GUARDRAIL_TOOL}\""
  "BIOSEQ_SQL_GUARDRAIL_PYTHON=\"${Python3_EXECUTABLE}\""
  "BIOSEQ_SQL_GUARDRAIL_REPOSITORY_ROOT=\"${CMAKE_SOURCE_DIR}/..\""
  "BIOSEQ_SQL_GUARDRAIL_BUILD_DIR=\"${CMAKE_BINARY_DIR}\""
  "BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY_PATH=\"${BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY}\""
  "BIOSEQ_SQL_GUARDRAIL_COMPARATOR_DSO_PATH=\"$<TARGET_FILE:bioseq_sql_guardrail_comparators>\""
  "BIOSEQ_SQL_GUARDRAIL_GENERATOR_EXECUTABLE_PATH=\"${BIOSEQ_CODEGEN_EXECUTABLE}\""
  "BIOSEQ_SQL_GUARDRAIL_PRODUCTION_DSO_PATH=\"$<TARGET_FILE:postgresql_bioseq>\""
  "BIOSEQ_SQL_GUARDRAIL_INSTALLED_PRODUCTION_DSO_PATH=\"${PostgreSQL_PKG_LIBRARY_DIR}/$<TARGET_FILE_NAME:postgresql_bioseq>\""
  "BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE_PATH=\"${BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE}\"")

if(TARGET bioseq_bench_e2e_tests_exec)
  target_compile_definitions(bioseq_bench_e2e_tests_exec PRIVATE
    BIOSEQ_E2E_CONTRACT_TEST=1
    "BIOSEQ_E2E_REPORT_DIR=\"${BIOSEQ_DIR}/../reporting/bioseq_bench_e2e\""
    "BIOSEQ_E2E_DATA_DIR=\"${BENCH_E2E_DIR}\""
    "BIOSEQ_E2E_CONFIG_PATH=\"${BENCH_E2E_DIR}/bench_e2e_configuration.json\""
    "BIOSEQ_E2E_MINIMAP2_BIN=\"${BIOSEQ_MINIMAP2_BIN}\""
    "BIOSEQ_SQL_GUARDRAIL_CONFIG_PATH=\"${BENCH_E2E_DIR}/bench_sql_codegen_guardrail_configuration.json\""
    "BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG_PATH=\"${BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG}\""
    "BIOSEQ_SQL_GUARDRAIL_SQL_FILES=\"${_bioseq_sql_guardrail_sql_files}\""
    "BIOSEQ_SQL_GUARDRAIL_JSON_FILES=\"${_bioseq_sql_guardrail_json_files}\""
    "BIOSEQ_SQL_GUARDRAIL_CPP_FILES=\"${_bioseq_sql_guardrail_cpp_files}\""
    "BIOSEQ_SQL_GUARDRAIL_QUALIFICATION_PATH=\"${BIOSEQ_SQL_GUARDRAIL_QUALIFICATION}\""
    "BIOSEQ_SQL_GUARDRAIL_TOOL_PATH=\"${BIOSEQ_SQL_GUARDRAIL_TOOL}\""
    "BIOSEQ_SQL_GUARDRAIL_PYTHON=\"${Python3_EXECUTABLE}\""
    "BIOSEQ_SQL_GUARDRAIL_REPOSITORY_ROOT=\"${CMAKE_SOURCE_DIR}/..\""
    "BIOSEQ_SQL_GUARDRAIL_BUILD_DIR=\"${CMAKE_BINARY_DIR}\""
    "BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY_PATH=\"${BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY}\""
    "BIOSEQ_SQL_GUARDRAIL_COMPARATOR_DSO_PATH=\"$<TARGET_FILE:bioseq_sql_guardrail_comparators>\""
    "BIOSEQ_SQL_GUARDRAIL_GENERATOR_EXECUTABLE_PATH=\"${BIOSEQ_CODEGEN_EXECUTABLE}\""
    "BIOSEQ_SQL_GUARDRAIL_PRODUCTION_DSO_PATH=\"$<TARGET_FILE:postgresql_bioseq>\""
    "BIOSEQ_SQL_GUARDRAIL_INSTALLED_PRODUCTION_DSO_PATH=\"${PostgreSQL_PKG_LIBRARY_DIR}/$<TARGET_FILE_NAME:postgresql_bioseq>\""
    "BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE_PATH=\"${BIOSEQ_SQL_GUARDRAIL_REVIEWED_BASELINE}\"")
endif()

# CMake owns the exact paths consumed by the bioseq-builder MCP workflow.
# The worker reads this generated manifest only after the target graph has
# completed, so measurement never guesses output locations or bypasses CMake.
file(GENERATE
  OUTPUT "${CMAKE_BINARY_DIR}/bioseq_sql_guardrail_workflow_paths.json"
  CONTENT
"{
  \"schema_version\": 1,
  \"artifact_kind\": \"doc0388_sql_codegen_guardrail_workflow_paths\",
  \"repository_root\": \"${CMAKE_SOURCE_DIR}/..\",
  \"cmake_source_dir\": \"${CMAKE_SOURCE_DIR}\",
  \"build_dir\": \"${CMAKE_BINARY_DIR}\",
  \"tool\": \"${BIOSEQ_SQL_GUARDRAIL_TOOL}\",
  \"artifact_inventory\": \"${BIOSEQ_SQL_GUARDRAIL_ARTIFACT_INVENTORY}\",
  \"generator_output_inventory\": \"${BIOSEQ_SQL_GUARDRAIL_GENERATOR_OUTPUT_INVENTORY}\",
  \"benchmark_configuration\": \"${BENCH_E2E_DIR}/bench_sql_codegen_guardrail_configuration.json\",
  \"calibration_configuration\": \"${BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG}\",
  \"generator_executable\": \"${BIOSEQ_CODEGEN_EXECUTABLE}\",
  \"production_dso\": \"$<TARGET_FILE:postgresql_bioseq>\",
  \"installed_production_dso\": \"${PostgreSQL_PKG_LIBRARY_DIR}/$<TARGET_FILE_NAME:postgresql_bioseq>\",
  \"benchmark_executable\": \"$<TARGET_FILE:bioseq_bench_e2e>\",
  \"comparator_dso\": \"$<TARGET_FILE:bioseq_sql_guardrail_comparators>\",
  \"stage0_measurement\": \"${CMAKE_BINARY_DIR}/bioseq/generated/sql/guardrail/stage0-measurement.json\",
  \"recovered_measurement\": \"${CMAKE_BINARY_DIR}/bioseq/generated/sql/guardrail/recovered-measurement.json\",
  \"candidate_measurement\": \"${BIOSEQ_SQL_GUARDRAIL_CANDIDATE_MEASUREMENT}\",
  \"runtime_samples\": \"${CMAKE_BINARY_DIR}/bioseq/generated/sql/guardrail/runtime-samples.json\",
  \"qualification\": \"${BIOSEQ_SQL_GUARDRAIL_QUALIFICATION}\"
}
")

set(_bioseq_sql_guardrail_self_test_stamp
  "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/guardrail/self-test.stamp")
add_custom_command(
  OUTPUT "${_bioseq_sql_guardrail_self_test_stamp}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/guardrail"
  COMMAND "${Python3_EXECUTABLE}"
    "${BIOSEQ_SQL_GUARDRAIL_TOOL}" self-test
    --configuration
      "${BENCH_E2E_DIR}/bench_sql_codegen_guardrail_configuration.json"
    --calibration-configuration
      "${BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG}"
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${_bioseq_sql_guardrail_self_test_stamp}"
  DEPENDS
    "${BIOSEQ_SQL_GUARDRAIL_TOOL}"
    "${BENCH_E2E_DIR}/bench_sql_codegen_guardrail_configuration.json"
    "${BIOSEQ_SQL_GUARDRAIL_CALIBRATION_CONFIG}"
  COMMENT "Running DOC0388 SQL codegen guardrail schema self-tests"
  VERBATIM)
add_custom_target(bioseq_sql_guardrail_static_self_test
  DEPENDS "${_bioseq_sql_guardrail_self_test_stamp}")
add_dependencies(
  bioseq_bench_e2e
  bioseq_sql_guardrail_static_self_test)
if(TARGET bioseq_bench_e2e_tests_exec)
  add_dependencies(
    bioseq_bench_e2e_tests_exec
    bioseq_sql_guardrail_static_self_test)
endif()

if(APPLE)
    target_link_options(bioseq_bench_e2e PRIVATE
      -L/opt/homebrew/opt/llvm/lib/c++)
    if(TARGET bioseq_bench_e2e_tests_exec)
      target_link_options(bioseq_bench_e2e_tests_exec PRIVATE
        -L/opt/homebrew/opt/llvm/lib/c++)
    endif()
endif()
