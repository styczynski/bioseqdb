# Website CMake (§17.3). Drives the isolated generator through the full
# pipeline: plan -> source_graph -> Doxygen -> cpp_index -> stage -> execute
# -> publish (-> assemble). The explicit, non-ALL target is bioseq_website; it
# never writes generated content into the source tree. This directory is added
# only when BIOSEQ_BUILD_WEBSITE is ON and BIOSEQ_BUILD_CORE is ON.

if(NOT BIOSEQ_BUILD_WEBSITE)
  return()
endif()

# --- Generator + extension handles --------------------------------------------
# The reusable isolated generator (GCC 16.1/libstdc++) is provisioned by the
# root via BioseqCodegen.cmake. bioseq_codegen_project is the ExternalProject
# build target to DEPENDS on; BIOSEQ_CODEGEN_EXECUTABLE is its byproduct path.
if(NOT BIOSEQ_CODEGEN_EXECUTABLE)
  message(FATAL_ERROR
    "BIOSEQ_BUILD_WEBSITE requires the reusable code generator; "
    "BIOSEQ_CODEGEN_EXECUTABLE is unset")
endif()
set(_gen "${BIOSEQ_CODEGEN_EXECUTABLE}")
set(_gen_dep bioseq_codegen_project)

# --- Version / output confinement ---------------------------------------------
set(_source_dir "${CMAKE_CURRENT_SOURCE_DIR}")
set(_version "${BIOSEQ_WEBSITE_VERSION}")
if(_version STREQUAL "")
  # Auto-resolve the sole vN.N source directory when the cache is unset so a
  # bare BIOSEQ_BUILD_WEBSITE=ON configure (e.g. the build MCP preset) still
  # defines the targets instead of failing.
  file(GLOB _version_dirs RELATIVE "${_source_dir}" "${_source_dir}/v[0-9]*")
  list(LENGTH _version_dirs _version_count)
  if(_version_count EQUAL 1)
    list(GET _version_dirs 0 _version)
  else()
    set(_version "v0.1")
  endif()
endif()
if(NOT IS_DIRECTORY "${_source_dir}/${_version}")
  message(FATAL_ERROR "website: source version directory not found: ${_version}")
endif()

if(BIOSEQ_WEBSITE_OUTPUT_DIRECTORY STREQUAL "")
  set(_output_root "${CMAKE_BINARY_DIR}/website")
else()
  set(_output_root "${BIOSEQ_WEBSITE_OUTPUT_DIRECTORY}")
endif()
# Output must resolve inside the active build tree; generated pages never enter
# main/website.
file(REAL_PATH "${_output_root}" _output_real EXPAND_TILDE)
file(REAL_PATH "${CMAKE_BINARY_DIR}" _binary_real EXPAND_TILDE)
string(FIND "${_output_real}" "${_binary_real}" _inside)
if(NOT _inside EQUAL 0)
  message(FATAL_ERROR
    "website: BIOSEQ_WEBSITE_OUTPUT_DIRECTORY must resolve inside the build tree")
endif()
file(MAKE_DIRECTORY "${_output_root}")

set(_base_path "${BIOSEQ_WEBSITE_BASE_PATH}")
set(_revision "${BIOSEQ_WEBSITE_SOURCE_REVISION}")
if(_revision STREQUAL "")
  # Resolve Git HEAD; empty stays empty and the generator falls back.
  find_program(_git_exe git)
  if(_git_exe)
    execute_process(
      COMMAND "${_git_exe}" -C "${CMAKE_SOURCE_DIR}" rev-parse HEAD
      OUTPUT_VARIABLE _revision OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _git_rc)
    if(NOT _git_rc EQUAL 0)
      set(_revision "")
    endif()
  endif()
endif()

set(_site_config "${_source_dir}/site.yaml")
set(_sql_api "${_source_dir}/sql_api.yaml")
set(_templates_dir "${_source_dir}/templates")
set(_static_dir "${_source_dir}/static")

# --- Config inputs that must reconfigure on author additions -------------------
file(GLOB_RECURSE _tutorial_md CONFIGURE_DEPENDS "${_source_dir}/${_version}/*.md")
file(GLOB_RECURSE _template_files CONFIGURE_DEPENDS "${_templates_dir}/*.j2")
file(GLOB_RECURSE _static_files CONFIGURE_DEPENDS "${_static_dir}/*")

# --- Source universe (§17.3 step 2) -------------------------------------------
# Enumerate the project's C++ module interface units plus the extension's exact
# PG entrypoint, written path/owner-only at configure time. CONFIGURE_DEPENDS
# reconfigures when a module interface is added or removed.
file(GLOB_RECURSE _module_units CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/bioseq/*.cppm"
  "${CMAKE_SOURCE_DIR}/common/*.cppm")
list(SORT _module_units)
get_target_property(_pg_entrypoints postgresql_bioseq
  BIOSEQ_EXTENSION_PG_ENTRYPOINT_SOURCES)
if(_pg_entrypoints)
  foreach(_entry ${_pg_entrypoints})
    if(NOT _entry IN_LIST _module_units)
      list(APPEND _module_units "${_entry}")
    endif()
  endforeach()
endif()

set(_reachable_units_file "${_output_root}/reachable-units.txt")
set(_source_universe "${_output_root}/source-universe.json")
string(REPLACE ";" "\n" _units_lines "${_module_units}")
file(WRITE "${_reachable_units_file}" "${_units_lines}\n")
set(_universe_json "{\n  \"schema\": \"source_universe/v1\",\n  \"owner\": \"bioseq_website\",\n  \"units\": [\n")
set(_first TRUE)
foreach(_unit ${_module_units})
  if(_first)
    set(_first FALSE)
  else()
    string(APPEND _universe_json ",\n")
  endif()
  string(APPEND _universe_json "    \"${_unit}\"")
endforeach()
string(APPEND _universe_json "\n  ]\n}\n")
file(WRITE "${_source_universe}" "${_universe_json}")

# --- Doxygen 1.9.8 (exact) ----------------------------------------------------
find_package(Doxygen 1.9.8 EXACT REQUIRED)
set(_doxygen_output "${_output_root}/doxygen")
set(_doxygen_xml "${_doxygen_output}/xml")
set(BIOSEQ_WEBSITE_PROJECT_NAME "BioseqDB")
set(BIOSEQ_WEBSITE_DOXYGEN_OUTPUT "${_doxygen_output}")
set(BIOSEQ_WEBSITE_STRIP_FROM_PATH "${CMAKE_SOURCE_DIR}")
set(BIOSEQ_WEBSITE_DOXYGEN_WARN_LOG "${_output_root}/doxygen-warnings.log")
# Doxygen INPUT is driven by the reachable interface-unit closure (§11.1), not the
# recursive glob: source_graph writes the closure and a build-time fragment sets the
# Doxyfile INPUT to exactly those files. The @INCLUDEd fragment path is baked here.
set(_reachable_closure "${_output_root}/reachable-interface-units.txt")
set(_doxygen_input_fragment "${_output_root}/doxygen-input.cfg")
set(BIOSEQ_WEBSITE_DOXYGEN_INPUT_INCLUDE "${_doxygen_input_fragment}")
configure_file("${_source_dir}/Doxyfile.in" "${_output_root}/Doxyfile" @ONLY)

# --- Compile database ---------------------------------------------------------
set(_compile_db "${CMAKE_BINARY_DIR}/compile_commands.json")

# --- Private-runner tests (§17.3) ---------------------------------------------
find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

# --- Pinned Python validation environment (§4.4) ------------------------------
# The independent html5lib validator and the Playwright browser tests run in a
# hash-locked virtualenv provisioned from tests/requirements.lock, so html5lib and
# (when browser tests are on) Playwright's pinned Chromium are deterministic and
# offline-verifiable. html5lib is a hard prerequisite of publication, so a failed
# provisioning fails the build rather than skipping the check.
set(_venv_dir "${_output_root}/venv")
set(_venv_python "${_venv_dir}/bin/python")
set(_venv_stamp "${_output_root}/venv.stamp")
set(_requirements_lock "${CMAKE_SOURCE_DIR}/website/tests/requirements.lock")
set(_browser_provision "")
if(BIOSEQ_WEBSITE_BROWSER_TESTS)
  set(_browser_provision
    COMMAND "${CMAKE_COMMAND}" -E env
            "PLAYWRIGHT_BROWSERS_PATH=${_output_root}/playwright-browsers"
            "${_venv_python}" -m playwright install chromium)
endif()
add_custom_command(
  OUTPUT "${_venv_stamp}"
  COMMAND "${Python3_EXECUTABLE}" -m venv "${_venv_dir}"
  COMMAND "${_venv_python}" -m pip install --quiet --disable-pip-version-check
          --require-hashes -r "${_requirements_lock}"
  ${_browser_provision}
  COMMAND "${CMAKE_COMMAND}" -E touch "${_venv_stamp}"
  DEPENDS "${_requirements_lock}"
  COMMENT "website: provision pinned Python validation venv (§4.4)"
  VERBATIM)

set(_pg_bindir "${PostgreSQL_BIN_DIR}")
if(NOT _pg_bindir)
  get_filename_component(_pg_bindir "${PostgreSQL_CONFIG}" DIRECTORY)
endif()
set(_runner_tests_stamp "${_output_root}/private-runner-tests.stamp")
add_custom_command(
  OUTPUT "${_runner_tests_stamp}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/cmake/test_private_postgres.py"
          --pg-bin-dir "${_pg_bindir}"
          --scratch-root "${_output_root}/private-runner-tests"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_runner_tests_stamp}"
  DEPENDS "${CMAKE_SOURCE_DIR}/cmake/test_private_postgres.py"
          "${CMAKE_SOURCE_DIR}/cmake/run_private_postgres.py"
          "${CMAKE_SOURCE_DIR}/cmake/private_postgres.py"
  COMMENT "website: private PostgreSQL runner self-tests"
  USES_TERMINAL VERBATIM)
add_custom_target(bioseq_private_postgres_tests DEPENDS "${_runner_tests_stamp}")

# --- Generator self-test (§17.3 step 3) ---------------------------------------
set(_selftest_stamp "${_output_root}/self-test.stamp")
add_custom_command(
  OUTPUT "${_selftest_stamp}"
  COMMAND "${_gen}" self_test website
  COMMAND "${CMAKE_COMMAND}" -E touch "${_selftest_stamp}"
  DEPENDS ${_gen_dep}
  COMMENT "website: generator self-test"
  VERBATIM)

# --- plan (§17.3 step 3) ------------------------------------------------------
set(_plan_json "${_output_root}/site-plan.json")
add_custom_command(
  OUTPUT "${_plan_json}"
  COMMAND "${_gen}" website plan
          --source "${_source_dir}"
          --version "${_version}"
          --site-config "${_site_config}"
          --templates "${_templates_dir}"
          --static "${_static_dir}"
          --base-path "${_base_path}"
          --source-revision "${_revision}"
          $<$<BOOL:${BIOSEQ_WEBSITE_INCLUDE_DRAFTS}>:--include-drafts>
          --output "${_plan_json}"
  DEPENDS ${_gen_dep} "${_selftest_stamp}" "${_site_config}" "${_sql_api}"
          ${_tutorial_md} ${_template_files} ${_static_files}
  COMMENT "website: plan (config/template/frontmatter validation)"
  VERBATIM)

# --- source_graph (§17.3 step 4) ----------------------------------------------
# Depends on the reachable universe + compile DB + provider BMIs (postgresql_bioseq).
set(_source_graph_json "${_output_root}/source-graph.json")
add_custom_command(
  OUTPUT "${_source_graph_json}" "${_reachable_closure}"
  COMMAND "${_gen}" website source_graph
          --reachable-units "${_reachable_units_file}"
          --compile-db "${_compile_db}"
          --site-config "${_site_config}"
          --output "${_source_graph_json}"
          --reachable-output "${_reachable_closure}"
  DEPENDS ${_gen_dep} postgresql_bioseq "${_source_universe}"
          "${_reachable_units_file}" "${_site_config}" "${_compile_db}"
  COMMENT "website: source_graph (export-import reachable closure)"
  VERBATIM)

# --- Doxygen XML (§17.3 step 5) -----------------------------------------------
# Doxygen INPUT is the reachable interface-unit closure, never the glob (§11.1): the
# fragment step rewrites the Doxyfile INPUT from source_graph's closure before the run.
set(_doxygen_stamp "${_output_root}/doxygen.stamp")
add_custom_command(
  OUTPUT "${_doxygen_stamp}"
  COMMAND "${CMAKE_COMMAND}"
          -DCLOSURE=${_reachable_closure}
          -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
          -DFRAGMENT_OUT=${_doxygen_input_fragment}
          -P "${CMAKE_SOURCE_DIR}/cmake/doxygen_input_fragment.cmake"
  COMMAND "${DOXYGEN_EXECUTABLE}" "${_output_root}/Doxyfile"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_doxygen_stamp}"
  DEPENDS "${_source_graph_json}" "${_reachable_closure}" "${_output_root}/Doxyfile"
          "${CMAKE_SOURCE_DIR}/cmake/doxygen_input_fragment.cmake" ${_module_units}
  COMMENT "website: Doxygen 1.9.8 XML over the reachable closure"
  VERBATIM)

# --- cpp_index (§17.3 step 5) -------------------------------------------------
# cpp_index parses exactly the reachable closure (§11.1), not the recursive glob.
set(_cpp_entities "${_output_root}/cpp-entities.json")
set(_cpp_coverage "${_output_root}/cpp-coverage.json")
add_custom_command(
  OUTPUT "${_cpp_entities}" "${_cpp_coverage}"
  COMMAND "${_gen}" website cpp_index
          --doxygen-xml "${_doxygen_xml}"
          --compile-db "${_compile_db}"
          --reachable-units "${_reachable_closure}"
          --site-config "${_site_config}"
          --output "${_cpp_entities}"
          --coverage-output "${_cpp_coverage}"
  DEPENDS ${_gen_dep} "${_doxygen_stamp}" "${_plan_json}" "${_source_graph_json}"
          "${_compile_db}" "${_reachable_closure}"
  COMMENT "website: cpp_index (Doxygen/Clang merge + coverage over the closure)"
  VERBATIM)

# --- Relocatable PostgreSQL stage (§13.2 / §17.3 step 6) ----------------------
set(_stage_root "${_output_root}/postgres-prefix")
set(_stage_manifest "${_output_root}/stage-manifest.json")
add_custom_command(
  OUTPUT "${_stage_manifest}"
  COMMAND "${CMAKE_COMMAND}"
          -DPG_CONFIG=${PostgreSQL_CONFIG}
          -DSTAGE_ROOT=${_stage_root}
          "-DCONTROL_FILE=$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_CONTROL_FILE>"
          "-DINSTALL_SQL=$<TARGET_PROPERTY:postgresql_bioseq,BIOSEQ_EXTENSION_INSTALL_SQL>"
          "-DLIBRARY_FILE=$<TARGET_FILE:postgresql_bioseq>"
          -DEXTENSION_VERSION=${_version}
          -DMANIFEST_OUT=${_stage_manifest}
          -P "${CMAKE_SOURCE_DIR}/cmake/stage_private_postgres.cmake"
  DEPENDS postgresql_bioseq "${CMAKE_SOURCE_DIR}/cmake/stage_private_postgres.cmake"
  COMMENT "website: stage relocatable PostgreSQL + current extension artifacts"
  VERBATIM)

# --- catalog + execute on the staged cluster (§17.3 step 7) -------------------
# One owner process starts the copied server under RLIMIT_AS containment, applies
# CREATE EXTENSION bioseq, then launches a single generator child: it captures the
# live SQL catalog into sql-entities.json and then runs `execute`, which resolves
# and runs every landing/tutorial runnable example against the live extension and
# writes the database-independent resolved site model (schema site_model/v1, §180).
# Nothing after this step needs a cluster.
string(REGEX REPLACE "^/+" "" _pg_bindir_rel "${_pg_bindir}")
set(_staged_bindir "${_stage_root}/${_pg_bindir_rel}")
set(_sql_entities "${_output_root}/sql-entities.json")
set(_site_model "${_output_root}/site-model.json")

# The private CREATE EXTENSION bootstrap the runner applies (§13.2).
file(WRITE "${_output_root}/create-extension.sql" "CREATE EXTENSION bioseq;\n")

set(_drafts_flag "")
if(BIOSEQ_WEBSITE_INCLUDE_DRAFTS)
  set(_drafts_flag "--include-drafts")
endif()
set(_cluster_child "${_output_root}/run-website-cluster.sh")
file(WRITE "${_cluster_child}"
"#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"CONN=\"host=$PGHOST port=$PGPORT dbname=$PGDATABASE user=$PGUSER\"\n"
"\"${_gen}\" website catalog --conninfo \"$CONN\" \\\n"
"  --schemas 'public,bioseq' --output '${_sql_entities}'\n"
"exec \"${_gen}\" website execute \\\n"
"  --source '${_source_dir}' --version '${_version}' \\\n"
"  --site-config '${_site_config}' \\\n"
"  --cpp-entities '${_cpp_entities}' --sql-entities '${_sql_entities}' \\\n"
"  --base-path '${_base_path}' --source-revision '${_revision}' \\\n"
"  --conninfo \"$CONN\" ${_drafts_flag} \\\n"
"  --output-model '${_site_model}'\n")

add_custom_command(
  OUTPUT "${_site_model}" "${_sql_entities}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/cmake/run_private_postgres.py"
          --pg-bin-dir "${_staged_bindir}"
          --data-dir-parent "${_output_root}/cluster"
          --socket-dir-parent /tmp
          --log-dir "${_output_root}/cluster-log"
          --database bioseq_site
          --address-space-limit-mib 4096
          --bootstrap "${_output_root}/create-extension.sql"
          -- bash "${_cluster_child}"
  DEPENDS ${_gen_dep} "${_stage_manifest}" "${_cluster_child}"
          "${_cpp_entities}" "${_plan_json}"
          ${_tutorial_md} ${_template_files} ${_static_files}
          bioseq_private_postgres_tests
  COMMENT "website: catalog + execute runnable SQL into the resolved site model"
  USES_TERMINAL VERBATIM)

# --- render the resolved model into staging without a database (step 8) -------
set(_staging_dir "${_output_root}/staging")
set(_staging_manifest "${_staging_dir}/site-manifest.json")
set(_model_validation_stamp "${_output_root}/model-validation.stamp")
add_custom_command(
  OUTPUT "${_staging_manifest}" "${_model_validation_stamp}"
  COMMAND "${_gen}" website render
          --model "${_site_model}"
          --site-config "${_site_config}"
          --templates "${_templates_dir}"
          --static "${_static_dir}"
          --base-path "${_base_path}"
          --source-revision "${_revision}"
          --output "${_staging_dir}"
          --model-validation-stamp "${_model_validation_stamp}"
  DEPENDS ${_gen_dep} "${_site_model}" ${_template_files} ${_static_files}
  COMMENT "website: render resolved model into owner-marked staging (no database)"
  VERBATIM)

# --- independent html5lib validation of staging BEFORE the swap (step 9) ------
# The pinned Python validators (§4.4) inspect staging, not the published tree, so a
# rejected page blocks publication rather than being served first (fail-closed §14).
set(_html_validation_stamp "${_output_root}/html-validation.stamp")
add_custom_command(
  OUTPUT "${_html_validation_stamp}"
  COMMAND "${_venv_python}" "${CMAKE_SOURCE_DIR}/website/tests/validate_site.py"
          --site "${_staging_dir}"
          --stamp "${_html_validation_stamp}"
  DEPENDS "${_staging_manifest}" "${_venv_stamp}"
          "${CMAKE_SOURCE_DIR}/website/tests/validate_site.py"
  COMMENT "website: independent html5lib validation of staging"
  VERBATIM)

# --- optional pinned-browser acceptance at two base paths (step 10) -----------
# When enabled, the SAME executed model is re-rendered at base paths / and
# /bioseqdb/ into private fixture roots, each independently validated, and Playwright
# runs against staging plus both fixtures. The browser stamp then gates publish.
# Browser tests degrade cleanly (SKIP) when Chromium is unprovisioned.
set(_browser_dep "")
if(BIOSEQ_WEBSITE_BROWSER_TESTS)
  set(_browser_stamp "${_output_root}/browser-validation.stamp")
  set(_fixture_root_slash "${_output_root}/fixture-root")
  set(_fixture_root_bioseqdb "${_output_root}/fixture-bioseqdb")
  add_custom_command(
    OUTPUT "${_browser_stamp}"
    COMMAND "${_gen}" website render --model "${_site_model}"
            --site-config "${_site_config}" --templates "${_templates_dir}"
            --static "${_static_dir}" --base-path "/"
            --source-revision "${_revision}" --output "${_fixture_root_slash}"
    COMMAND "${_gen}" website render --model "${_site_model}"
            --site-config "${_site_config}" --templates "${_templates_dir}"
            --static "${_static_dir}" --base-path "/bioseqdb/"
            --source-revision "${_revision}" --output "${_fixture_root_bioseqdb}"
    COMMAND "${_venv_python}" "${CMAKE_SOURCE_DIR}/website/tests/validate_site.py"
            --site "${_fixture_root_slash}"
            --stamp "${_output_root}/fixture-root-validation.stamp"
    COMMAND "${_venv_python}" "${CMAKE_SOURCE_DIR}/website/tests/validate_site.py"
            --site "${_fixture_root_bioseqdb}"
            --stamp "${_output_root}/fixture-bioseqdb-validation.stamp"
    COMMAND "${CMAKE_COMMAND}" -E env
            "PLAYWRIGHT_BROWSERS_PATH=${_output_root}/playwright-browsers"
            "${_venv_python}" "${CMAKE_SOURCE_DIR}/website/tests/browser_test.py"
            --site "${_staging_dir}" --base-path "${_base_path}"
            --version "${_version}" --stamp "${_browser_stamp}"
    DEPENDS "${_html_validation_stamp}" "${_site_model}" "${_venv_stamp}"
            "${CMAKE_SOURCE_DIR}/website/tests/browser_test.py"
            "${CMAKE_SOURCE_DIR}/website/tests/validate_site.py"
    COMMENT "website: two-base-path render + pinned browser acceptance"
    USES_TERMINAL VERBATIM)
  set(_browser_dep "${_browser_stamp}")
endif()

# --- publish: verify validator stamps + atomic swap (step 11) -----------------
set(_publish_stamp "${_output_root}/site/site-manifest.json")
add_custom_command(
  OUTPUT "${_publish_stamp}"
  COMMAND "${_gen}" website publish
          --staging "${_staging_dir}"
          --output "${_output_root}/site"
          --model-validation-stamp "${_model_validation_stamp}"
          --html-validation-stamp "${_html_validation_stamp}"
  DEPENDS ${_gen_dep} "${_staging_manifest}" "${_model_validation_stamp}"
          "${_html_validation_stamp}" ${_browser_dep}
  COMMENT "website: verify validator stamps and atomically publish staging"
  VERBATIM)

# --- bioseq_website final target (§17.3 step 11) ------------------------------
set(_website_success "${_output_root}/site.success")
add_custom_command(
  OUTPUT "${_website_success}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_website_success}"
  DEPENDS "${_html_validation_stamp}" "${_cpp_coverage}"
  COMMENT "website: publication success stamp"
  VERBATIM)
add_custom_target(bioseq_website
  DEPENDS "${_website_success}" bioseq_private_postgres_tests)

# --- assemble (§17.3 step 12) -------------------------------------------------
if(NOT BIOSEQ_WEBSITE_VERSION_ARTIFACTS STREQUAL "")
  set(_assembled "${_output_root}/assembled-site")
  set(_assembled_success "${_output_root}/assembled-site.success")
  string(REPLACE ";" "," _roots_csv "${BIOSEQ_WEBSITE_VERSION_ARTIFACTS}")
  add_custom_command(
    OUTPUT "${_assembled_success}"
    COMMAND "${_gen}" website assemble
            --site-roots "${_output_root}/site,${_roots_csv}"
            --output "${_assembled}"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_assembled_success}"
    DEPENDS ${_gen_dep} "${_website_success}"
    COMMENT "website: assemble multi-version site"
    VERBATIM)
  add_custom_target(bioseq_website_assemble DEPENDS "${_assembled_success}")
endif()

message(STATUS "website: configured bioseq_website for ${_version} -> ${_output_root}/site")
