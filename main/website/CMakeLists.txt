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
set(_gen_dep bioseq_codegen_project "${_gen}")

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
file(RELATIVE_PATH _output_relative "${_binary_real}" "${_output_real}")
if(IS_ABSOLUTE "${_output_relative}"
   OR _output_relative STREQUAL ".."
   OR _output_relative MATCHES "^\\.\\.[/\\\\]")
  message(FATAL_ERROR
    "website: BIOSEQ_WEBSITE_OUTPUT_DIRECTORY must resolve inside the build tree")
endif()
file(MAKE_DIRECTORY "${_output_root}")

# Diagnostic build-report tree (§18.3): a sibling of the published site (never
# under <output_root>/site, so it is never a manifest entry or a served route).
# Each phase drops its own fragment here; publish aggregates them into
# build-report.json. It lives in the build tree and carries no machine paths.
set(_report_dir "${_output_root}/report")
file(MAKE_DIRECTORY "${_report_dir}")

# The base path must be a validated mount ("/" or "/seg/.../"). Per the cache
# variable's contract ("empty uses site.yaml"), an unset deployment base resolves
# from site.yaml's default_base_path -- the authored mount the site is deployed at
# (/bioseqdb/ for the GitHub Pages project site). Substituting "/" here instead
# would render every asset URL at the origin root, so the deployed pages would
# fetch /assets/... from /bioseqdb/... and load unstyled. Every downstream
# validator reads the base path back out of the rendered tree, so such a mismatch
# is internally consistent and passes: site.yaml is the only source that pins the
# real mount. Deployers override with BIOSEQ_WEBSITE_BASE_PATH for another mount.
set(_base_path "${BIOSEQ_WEBSITE_BASE_PATH}")
if(_base_path STREQUAL "")
  # file(READ) resolves at configure time but registers no dependency, so without
  # this the generator keeps rendering at whatever base path the previous configure
  # baked in and an edited default_base_path silently does nothing.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_source_dir}/site.yaml")
  file(READ "${_source_dir}/site.yaml" _site_yaml_text)
  if(NOT _site_yaml_text MATCHES "(^|\n)default_base_path:[ \t]*([^\n]*)")
    message(FATAL_ERROR
      "website: site.yaml declares no default_base_path and "
      "BIOSEQ_WEBSITE_BASE_PATH is unset; refusing to guess the deploy mount")
  endif()
  set(_base_path "${CMAKE_MATCH_2}")
  string(REGEX REPLACE "[ \t]+#.*$" "" _base_path "${_base_path}")
  string(REGEX REPLACE "[ \t\r]+$" "" _base_path "${_base_path}")
  string(REGEX REPLACE "^[\"']" "" _base_path "${_base_path}")
  string(REGEX REPLACE "[\"']$" "" _base_path "${_base_path}")
  if(_base_path STREQUAL "")
    message(FATAL_ERROR "website: site.yaml default_base_path is empty")
  endif()
endif()
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
file(CONFIGURE
  OUTPUT "${_reachable_units_file}"
  CONTENT "${_units_lines}\n"
  @ONLY)
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
file(CONFIGURE
  OUTPUT "${_source_universe}"
  CONTENT "${_universe_json}"
  @ONLY)

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
# CMake rewrites compile_commands.json on every configure even when its bytes are
# unchanged.  Copy through a restatted output so website analysis reruns only
# when the compilation database's content actually changes.
set(_compile_db_source "${CMAKE_BINARY_DIR}/compile_commands.json")
set(_compile_db "${CMAKE_BINARY_DIR}/compile_commands.stable.json")
add_custom_command(
  OUTPUT "${_compile_db}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${_compile_db_source}" "${_compile_db}"
  DEPENDS "${_compile_db_source}"
  COMMENT "website: stabilize compile database by content"
  VERBATIM)

# --- Private-runner tests (§17.3) ---------------------------------------------
find_package(Python3 3.12 COMPONENTS Interpreter REQUIRED)

# --- Supplied-design translation parity --------------------------------------
# The reference export is the visual source of truth.  The checked-in templates,
# design CSS and copied assets must be the byte-for-byte output of the mechanical
# translator before any plan/render/publish step may consume them.  --check
# regenerates only in a temporary directory and never mutates the source tree.
set(_design_reference_dir "${CMAKE_SOURCE_DIR}/../website_design")
set(_design_translator "${_source_dir}/tools/translate_reference_design.py")
if(NOT EXISTS "${_design_reference_dir}/index.html"
   OR NOT EXISTS "${_design_reference_dir}/docs.html")
  message(FATAL_ERROR
    "website: supplied website_design/index.html and website_design/docs.html "
    "are required for design-translation parity")
endif()
file(GLOB_RECURSE _design_reference_files CONFIGURE_DEPENDS
  "${_design_reference_dir}/*")
set(_design_translation_outputs
  "${_source_dir}/templates/base.html.j2"
  "${_source_dir}/templates/landing.html.j2"
  "${_source_dir}/static/css/docs-design.css"
  "${_source_dir}/static/css/landing-design.css")
set(_design_translation_inputs)
foreach(_relative
    index.html
    docs.html
    assets/bioseq-mark.svg
    assets/logos/bytedance.png
    assets/logos/ef.png
    assets/logos/goldman.png
    assets/logos/google.png
    assets/logos/sumo.png
    uploads/small_vid.mp4
    uploads/vials.mp4)
  list(APPEND _design_translation_inputs
    "${_design_reference_dir}/${_relative}")
  list(APPEND _design_translation_outputs
    "${_source_dir}/design_source/${_relative}")
endforeach()
foreach(_relative
    bioseq-mark.svg
    favicon.svg
    logos/bytedance.png
    logos/ef.png
    logos/goldman.png
    logos/google.png
    logos/sumo.png
    media/small_vid.mp4
    media/vials.mp4)
  list(APPEND _design_translation_outputs "${_source_dir}/static/${_relative}")
endforeach()
set(_design_translation_stamp "${_output_root}/design-translation-check.stamp")
add_custom_command(
  OUTPUT "${_design_translation_stamp}"
  COMMAND "${Python3_EXECUTABLE}" "${_design_translator}" --check
  COMMAND "${CMAKE_COMMAND}" -E touch "${_design_translation_stamp}"
  DEPENDS "${_design_translator}" ${_design_translation_inputs}
          ${_design_translation_outputs}
  COMMENT "website: verify non-mutating supplied-design translation parity"
  VERBATIM)

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
# An empty $<BOOL:...:--include-drafts> genex still expands to an empty positional
# argument under VERBATIM, which the CLI rejects; use an (un)set list variable so the
# flag is omitted entirely when drafts are off.
set(_plan_drafts_flag)
if(BIOSEQ_WEBSITE_INCLUDE_DRAFTS)
  set(_plan_drafts_flag --include-drafts)
endif()
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
          ${_plan_drafts_flag}
          --output "${_plan_json}"
          --report-dir "${_report_dir}"
  DEPENDS ${_gen_dep} "${_selftest_stamp}" "${_design_translation_stamp}"
          "${_site_config}" "${_sql_api}"
          ${_tutorial_md} ${_template_files} ${_static_files}
  COMMENT "website: plan (config/template/frontmatter validation)"
  VERBATIM)

# --- source_graph (§17.3 step 4) ----------------------------------------------
# Depends on the reachable universe + compile DB + provider BMIs (postgresql_bioseq).
set(_source_graph_json "${_output_root}/source-graph.json")
set(_source_graph_work "${_output_root}/source-graph-work")
set(_source_graph_raw_json "${_source_graph_work}/source-graph.json")
set(_reachable_closure_raw "${_source_graph_work}/reachable-interface-units.txt")
set(_source_graph_run_stamp "${_source_graph_work}/complete.stamp")
add_custom_command(
  OUTPUT "${_source_graph_run_stamp}"
  BYPRODUCTS "${_source_graph_raw_json}" "${_reachable_closure_raw}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_source_graph_work}"
  COMMAND "${_gen}" website source_graph
          --reachable-units "${_reachable_units_file}"
          --compile-db "${_compile_db}"
          --site-config "${_site_config}"
          --output "${_source_graph_raw_json}"
          --reachable-output "${_reachable_closure_raw}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_source_graph_run_stamp}"
  DEPENDS ${_gen_dep} postgresql_bioseq "${_source_universe}"
          "${_reachable_units_file}" "${_site_config}" "${_compile_db}"
  COMMENT "website: source_graph (export-import reachable closure)"
  VERBATIM)
add_custom_command(
  OUTPUT "${_source_graph_json}" "${_reachable_closure}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${_source_graph_raw_json}" "${_source_graph_json}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${_reachable_closure_raw}" "${_reachable_closure}"
  DEPENDS "${_source_graph_run_stamp}"
  COMMENT "website: stabilize source-graph outputs by content"
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
          --report-dir "${_report_dir}"
  DEPENDS ${_gen_dep} "${_doxygen_stamp}" "${_plan_json}" "${_source_graph_json}"
          "${_compile_db}" "${_reachable_closure}"
  COMMENT "website: cpp_index (Doxygen/Clang merge + coverage over the closure)"
  VERBATIM)

# --- Relocatable PostgreSQL stage (§13.2 / §17.3 step 6) ----------------------
set(_stage_root "${_output_root}/postgres-prefix")
set(_stage_manifest "${_output_root}/stage-manifest.json")
set(_stage_extension_hash "${_output_root}/staged-extension.sha256")
string(REGEX REPLACE "^/+" "" _pg_bindir_rel "${_pg_bindir}")
set(_staged_bindir "${_stage_root}/${_pg_bindir_rel}")
get_target_property(
  _extension_control_file postgresql_bioseq BIOSEQ_EXTENSION_CONTROL_FILE)
get_target_property(
  _extension_install_sql postgresql_bioseq BIOSEQ_EXTENSION_INSTALL_SQL)
if(NOT _extension_control_file OR NOT _extension_install_sql)
  message(FATAL_ERROR
    "postgresql_bioseq must publish its generated control and install-SQL paths")
endif()
add_custom_command(
  OUTPUT "${_stage_manifest}" "${_stage_extension_hash}"
  COMMAND "${CMAKE_COMMAND}"
          -DPG_CONFIG=${PostgreSQL_CONFIG}
          -DSTAGE_ROOT=${_stage_root}
          "-DCONTROL_FILE=${_extension_control_file}"
          "-DINSTALL_SQL=${_extension_install_sql}"
          "-DLIBRARY_FILE=$<TARGET_FILE:postgresql_bioseq>"
          -DEXTENSION_VERSION=${_version}
          -DMANIFEST_OUT=${_stage_manifest}
          -DHASH_OUT=${_stage_extension_hash}
          -P "${CMAKE_SOURCE_DIR}/cmake/stage_private_postgres.cmake"
  DEPENDS postgresql_bioseq
          "${_extension_control_file}"
          "${_extension_install_sql}"
          "${CMAKE_SOURCE_DIR}/cmake/stage_private_postgres.cmake"
  COMMENT "website: stage relocatable PostgreSQL + current extension artifacts"
  VERBATIM)

# Exercise the generator's database-per-example security and result projection
# against a second disposable cluster. This transcript intentionally needs no
# extension bootstrap: it proves restricted clone ACLs, same-clone type-name
# lookup (including a clone-local enum), multi-results, PQcmdTuples, teardown,
# and forced cleanup independently of the authored website examples.
set(_isolation_tests_stamp "${_output_root}/website-isolation-tests.stamp")
add_custom_command(
  OUTPUT "${_isolation_tests_stamp}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/cmake/run_private_postgres.py"
          --pg-bin-dir "${_staged_bindir}"
          --data-dir-parent "${_output_root}/isolation-tests/cluster"
          --socket-dir-parent /tmp
          --log-dir "${_output_root}/isolation-tests/log"
          --database wtmpl
          --address-space-limit-mib 4096
          -- "${Python3_EXECUTABLE}"
             "${CMAKE_SOURCE_DIR}/cmake/test_website_isolation.py"
             --generator "${_gen}"
             --pg-bin-dir "${_staged_bindir}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_isolation_tests_stamp}"
  DEPENDS ${_gen_dep} "${_stage_manifest}" "${_stage_extension_hash}"
          "${CMAKE_SOURCE_DIR}/cmake/run_private_postgres.py"
          "${CMAKE_SOURCE_DIR}/cmake/private_postgres.py"
          "${CMAKE_SOURCE_DIR}/cmake/private_postgresql.conf.in"
          "${CMAKE_SOURCE_DIR}/cmake/test_website_isolation.py"
  COMMENT "website: isolated SQL executor + result metadata integration tests"
  USES_TERMINAL VERBATIM)
add_custom_target(bioseq_website_isolation_tests
  DEPENDS "${_isolation_tests_stamp}")

# --- catalog + execute on the staged cluster (§17.3 step 7) -------------------
# One owner process starts the copied server under RLIMIT_AS containment, applies
# CREATE EXTENSION bioseq, then launches a single generator child: it captures the
# live SQL catalog into sql-entities.json and then runs `execute`, which resolves
# and runs every landing/tutorial runnable example against the live extension and
# writes the database-independent resolved site model (schema site_model/v1, §180).
# Nothing after this step needs a cluster.
set(_sql_entities "${_output_root}/sql-entities.json")
set(_site_model "${_output_root}/site-model.json")
set(_landing_fasta "${_source_dir}/tests/fixtures/landing_reads.fasta")
if(NOT EXISTS "${_landing_fasta}")
  message(FATAL_ERROR "website: real bioseq.load fixture is missing: ${_landing_fasta}")
endif()

# The template owns the extension objects and globally revokes internal routine
# execution. The executor grants each unique clone owner only the schema/table
# ACLs needed inside that clone; direct per-clone grants keep PostgreSQL 13
# compatibility because pg_database_owner exists only in PostgreSQL 14+.
file(WRITE "${_output_root}/create-extension.sql"
  "CREATE EXTENSION bioseq;\n"
  "REVOKE EXECUTE ON ALL ROUTINES IN SCHEMA bioseq_internal FROM PUBLIC;\n")

set(_drafts_flag "")
if(BIOSEQ_WEBSITE_INCLUDE_DRAFTS)
  set(_drafts_flag "--include-drafts")
endif()
set(_cluster_child "${_output_root}/run-website-cluster.sh")
file(WRITE "${_cluster_child}"
"#!/usr/bin/env bash\n"
"set -euo pipefail\n"
"CONN=\"host=$PGHOST port=$PGPORT dbname=$PGDATABASE user=$PGUSER\"\n"
"ADMIN_CONN=\"host=$PGHOST port=$PGPORT dbname=postgres user=$PGUSER\"\n"
"IFS= read -r STAGED_EXTENSION_SHA256 < '${_stage_extension_hash}'\n"
"MAX_PREPARED=$(\"${_staged_bindir}/psql\" -tAqX --dbname \"$ADMIN_CONN\" -c 'SHOW max_prepared_transactions')\n"
"if [[ \"$MAX_PREPARED\" != '0' ]]; then\n"
"  echo \"website: expected max_prepared_transactions=0, got $MAX_PREPARED\" >&2\n"
"  exit 1\n"
"fi\n"
"PRELOAD=$(\"${_staged_bindir}/psql\" -tAqX --dbname \"$ADMIN_CONN\" -c 'SHOW shared_preload_libraries')\n"
"if [[ \"$PRELOAD\" != 'bioseq' ]]; then\n"
"  echo \"website: expected shared_preload_libraries=bioseq, got $PRELOAD\" >&2\n"
"  exit 1\n"
"fi\n"
"\"${CMAKE_COMMAND}\" -E copy_if_different '${_landing_fasta}' \"$BIOSEQ_TEST_PG_DATA_DIR/landing_reads.fasta\"\n"
"\"${_gen}\" website catalog --conninfo \"$CONN\" \\\n"
"  --schemas 'public,bioseq' --output '${_sql_entities}'\n"
"\"${_gen}\" website execute \\\n"
"  --source '${_source_dir}' --version '${_version}' \\\n"
"  --site-config '${_site_config}' --sql-api '${_sql_api}' \\\n"
"  --cpp-entities '${_cpp_entities}' --sql-entities '${_sql_entities}' \\\n"
"  --base-path '${_base_path}' --source-revision '${_revision}' \\\n"
"  --staged-extension-sha256 \"$STAGED_EXTENSION_SHA256\" \\\n"
"  --isolated --conninfo \"$CONN\" --admin-conninfo \"$ADMIN_CONN\" \\\n"
"  --socket-dir \"$PGHOST\" --port \"$PGPORT\" --template-db \"$PGDATABASE\" \\\n"
"  ${_drafts_flag} \\\n"
"  --output-model '${_site_model}' \\\n"
"  --report-dir '${_report_dir}'\n"
"LEFTOVERS=$(\"${_staged_bindir}/psql\" -tAqX --dbname \"$ADMIN_CONN\" -c \"SELECT \\\n"
"  (SELECT count(*) FROM pg_database WHERE left(datname, 10) = 'biosite_d_')::text || '|' || \\\n"
"  (SELECT count(*) FROM pg_roles WHERE left(rolname, 10) IN ('biosite_r_', 'biosite_p_'))::text || '|' || \\\n"
"  (SELECT count(*) FROM pg_prepared_xacts WHERE left(database::text, 10) = 'biosite_d_')::text\")\n"
"if [[ \"$LEFTOVERS\" != '0|0|0' ]]; then\n"
"  echo \"website: isolated executor leaked database|role|prepared-xact counts: $LEFTOVERS\" >&2\n"
"  exit 1\n"
"fi\n")

add_custom_command(
  OUTPUT "${_site_model}" "${_sql_entities}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/cmake/run_private_postgres.py"
          --pg-bin-dir "${_staged_bindir}"
          --data-dir-parent "${_output_root}/cluster"
          --socket-dir-parent /tmp
          --log-dir "${_output_root}/cluster-log"
          --database bioseq_site
          --address-space-limit-mib 4096
          --preload-library bioseq
          --disable-prepared-transactions
          --bootstrap "${_output_root}/create-extension.sql"
          -- bash "${_cluster_child}"
  DEPENDS ${_gen_dep} "${_stage_manifest}" "${_stage_extension_hash}" "${_cluster_child}"
          "${_cpp_entities}" "${_plan_json}" "${_sql_api}" "${_landing_fasta}"
          ${_tutorial_md} ${_template_files} ${_static_files}
          bioseq_private_postgres_tests bioseq_website_isolation_tests
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
          --report-dir "${_report_dir}"
  DEPENDS ${_gen_dep} "${_site_model}" "${_design_translation_stamp}"
          ${_template_files} ${_static_files}
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
          --asset-lock "${_static_dir}/third_party.lock.yaml"
  DEPENDS "${_staging_manifest}" "${_venv_stamp}"
          "${CMAKE_SOURCE_DIR}/website/tests/validate_site.py"
          "${_static_dir}/third_party.lock.yaml"
  COMMENT "website: independent html5lib + §20.7 site-relationship validation of staging"
  VERBATIM)

# --- optional pinned-browser acceptance at two base paths (step 10) -----------
# When enabled, the SAME executed model is re-rendered at base paths / and
# /bioseqdb/ into private fixture roots, each independently validated, and Playwright
# runs against staging plus both fixtures. The browser stamp then gates publish.
# Because this option requests browser acceptance explicitly, a missing pinned
# Chromium is fatal instead of silently weakening the publication gate.
set(_browser_dep "")
if(BIOSEQ_WEBSITE_BROWSER_TESTS)
  set(_browser_stamp "${_output_root}/browser-validation.stamp")
  set(_browser_deploy_stamp "${_output_root}/browser-deployment-base.stamp")
  set(_browser_root_stamp "${_output_root}/browser-root-base.stamp")
  set(_browser_bioseqdb_stamp "${_output_root}/browser-bioseqdb-base.stamp")
  set(_fixture_root_slash "${_output_root}/fixture-root")
  set(_fixture_root_bioseqdb "${_output_root}/fixture-bioseqdb")
  add_custom_command(
    OUTPUT "${_browser_stamp}"
    BYPRODUCTS "${_browser_deploy_stamp}" "${_browser_root_stamp}"
               "${_browser_bioseqdb_stamp}"
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
            --asset-lock "${_static_dir}/third_party.lock.yaml"
    COMMAND "${_venv_python}" "${CMAKE_SOURCE_DIR}/website/tests/validate_site.py"
            --site "${_fixture_root_bioseqdb}"
            --stamp "${_output_root}/fixture-bioseqdb-validation.stamp"
            --asset-lock "${_static_dir}/third_party.lock.yaml"
    COMMAND "${CMAKE_COMMAND}"
            -DPYTHON=${_venv_python}
            -DBROWSER_TEST=${CMAKE_SOURCE_DIR}/website/tests/browser_test.py
            -DSITE=${_staging_dir} -DBASE_PATH=${_base_path} -DVERSION=${_version}
            -DMODEL=${_site_model}
            -DDESIGN_SOURCE=${_design_reference_dir}
            -DARTIFACT_DIR=${_output_root}/browser-artifacts/deployment-base
            -DSTAMP=${_browser_deploy_stamp}
            -DBROWSERS_PATH=${_output_root}/playwright-browsers
            -P "${CMAKE_SOURCE_DIR}/cmake/run_browser_test.cmake"
    COMMAND "${CMAKE_COMMAND}"
            -DPYTHON=${_venv_python}
            -DBROWSER_TEST=${CMAKE_SOURCE_DIR}/website/tests/browser_test.py
            -DSITE=${_fixture_root_slash} -DBASE_PATH=/ -DVERSION=${_version}
            -DMODEL=${_site_model}
            -DDESIGN_SOURCE=${_design_reference_dir}
            -DARTIFACT_DIR=${_output_root}/browser-artifacts/root-base
            -DSTAMP=${_browser_root_stamp}
            -DBROWSERS_PATH=${_output_root}/playwright-browsers
            -P "${CMAKE_SOURCE_DIR}/cmake/run_browser_test.cmake"
    COMMAND "${CMAKE_COMMAND}"
            -DPYTHON=${_venv_python}
            -DBROWSER_TEST=${CMAKE_SOURCE_DIR}/website/tests/browser_test.py
            -DSITE=${_fixture_root_bioseqdb} -DBASE_PATH=/bioseqdb/
            -DVERSION=${_version}
            -DMODEL=${_site_model}
            -DDESIGN_SOURCE=${_design_reference_dir}
            -DARTIFACT_DIR=${_output_root}/browser-artifacts/bioseqdb-base
            -DSTAMP=${_browser_bioseqdb_stamp}
            -DBROWSERS_PATH=${_output_root}/playwright-browsers
            -P "${CMAKE_SOURCE_DIR}/cmake/run_browser_test.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_browser_stamp}"
    DEPENDS ${_gen_dep} "${_html_validation_stamp}" "${_site_model}"
            "${_venv_stamp}"
            "${_design_translation_stamp}"
            "${CMAKE_SOURCE_DIR}/website/tests/browser_test.py"
            "${CMAKE_SOURCE_DIR}/website/tests/validate_site.py"
            "${CMAKE_SOURCE_DIR}/cmake/run_browser_test.cmake"
            ${_design_reference_files}
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
          --report-dir "${_report_dir}"
  DEPENDS ${_gen_dep} "${_staging_manifest}" "${_model_validation_stamp}"
          "${_html_validation_stamp}" ${_browser_dep}
  COMMENT "website: verify validator stamps and atomically publish staging"
  VERBATIM)

# --- bioseq_website final target (§17.3 step 11) ------------------------------
# The success stamp requires the atomic publish (§184) - which itself pulls in the
# staging render, both validator stamps, and the pinned-browser acceptance - so the
# default target drives the pipeline all the way to the swapped-in `site/`, not just
# staging validation.
set(_website_success "${_output_root}/site.success")
add_custom_command(
  OUTPUT "${_website_success}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_website_success}"
  DEPENDS "${_publish_stamp}" "${_cpp_coverage}"
  COMMENT "website: publication success stamp"
  VERBATIM)
add_custom_target(bioseq_website
  DEPENDS "${_website_success}" bioseq_private_postgres_tests
          bioseq_website_isolation_tests)

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

message(STATUS "website: configured bioseq_website for ${_version} "
               "at base path ${_base_path} -> ${_output_root}/site")
