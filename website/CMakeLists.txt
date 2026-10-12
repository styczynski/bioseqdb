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
# root via bioseq_codegen.cmake. bioseq_codegen_project is the ExternalProject
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
# website/.
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
set(_landing_content "${_source_dir}/landing.yaml")
set(_contact_content "${_source_dir}/contact.yaml")
set(_templates_dir "${_source_dir}/templates")
set(_static_dir "${_source_dir}/static")

# The deployed default is a self-contained, same-origin benchmark bundle.
# Configurations keep their current source owners. Reports are discovered below
# from the repository reporting area and admitted by their validated framework
# content, never by a filename list.
set(_benchmark_configuration_sources
  "${CMAKE_SOURCE_DIR}/bench/alignment_benchmark_configuration.json"
  "${CMAKE_SOURCE_DIR}/bench/bench_e2e_configuration.json"
  "${CMAKE_SOURCE_DIR}/bioseq/io/benchmark/configuration.json")
set(_benchmark_reporting_root "${CMAKE_SOURCE_DIR}/reporting")
set(_benchmark_bundle_generator
  "${_source_dir}/tools/generate_benchmark_bundle.py")

# CONFIGURE_DEPENDS makes an added or deleted committed JSON trigger CMake's
# glob verification and regeneration.  The build-time generator independently
# enumerates the directory and performs the content/schema decision. This glob
# exists only to give Ninja exact filesystem invalidation edges.
file(GLOB_RECURSE _benchmark_reporting_candidates CONFIGURE_DEPENDS
  LIST_DIRECTORIES false "${_benchmark_reporting_root}/*.json")
list(SORT _benchmark_reporting_candidates)
set(_benchmark_discovery_inputs
  ${_benchmark_configuration_sources} ${_benchmark_reporting_candidates})
list(SORT _benchmark_discovery_inputs)
string(JOIN "\n" _benchmark_discovery_inventory_text
  ${_benchmark_discovery_inputs})
set(_benchmark_discovery_inventory
  "${_output_root}/benchmark-discovery-inputs.txt")
file(GENERATE OUTPUT "${_benchmark_discovery_inventory}"
  CONTENT "${_benchmark_discovery_inventory_text}\n")

set(_benchmark_configuration_arguments "")
foreach(_benchmark_configuration IN LISTS _benchmark_configuration_sources)
  list(APPEND _benchmark_configuration_arguments
    --configuration "${_benchmark_configuration}")
endforeach()

# An override is a checked/local manifest file whose entries may point at a
# separately published raw HTTP endpoint.  It is intentionally a file rather
# than a token or download URL: website builds never acquire credentials or
# perform a network fetch while assembling the static bundle.
set(BIOSEQ_WEBSITE_BENCHMARK_MANIFEST_OVERRIDE "" CACHE FILEPATH
  "Optional benchmark manifest file. Empty generates same-origin URLs")
set(_benchmark_manifest_override_arguments "")
set(_benchmark_manifest_override_dependency "")
if(NOT BIOSEQ_WEBSITE_BENCHMARK_MANIFEST_OVERRIDE STREQUAL "")
  get_filename_component(_benchmark_manifest_input
    "${BIOSEQ_WEBSITE_BENCHMARK_MANIFEST_OVERRIDE}" ABSOLUTE
    BASE_DIR "${CMAKE_SOURCE_DIR}")
  if(NOT EXISTS "${_benchmark_manifest_input}")
    message(FATAL_ERROR
      "website: benchmark manifest override does not exist: "
      "${_benchmark_manifest_input}")
  endif()
  list(APPEND _benchmark_manifest_override_arguments
    --manifest-override "${_benchmark_manifest_input}")
  set(_benchmark_manifest_override_dependency
    "${_benchmark_manifest_input}")
endif()

# --- Config inputs that must reconfigure on author additions -------------------
file(GLOB_RECURSE _tutorial_md CONFIGURE_DEPENDS "${_source_dir}/${_version}/*.md")
file(GLOB_RECURSE _blog_md CONFIGURE_DEPENDS "${_source_dir}/blog/*.md")
set(_blog_inputs ${_blog_md} "${_source_dir}/blog.yaml" "${_source_dir}/blog_keywords.yaml")
file(GLOB_RECURSE _template_files CONFIGURE_DEPENDS "${_templates_dir}/*.j2")
file(GLOB_RECURSE _static_files CONFIGURE_DEPENDS "${_static_dir}/*")

# --- Source universe (§17.3 step 2) -------------------------------------------
# The configured production target graph is the source-ownership authority.
# Filesystem globs cannot distinguish CXX_MODULES providers from ordinary
# named-module implementation units that also use the required .cppm suffix.
function(_bioseq_website_collect_link_closure output_variable root_target)
  if(NOT TARGET "${root_target}")
    message(FATAL_ERROR
      "website source-universe root target is missing: ${root_target}")
  endif()

  set(_pending_targets "${root_target}")
  set(_visited_targets)
  while(_pending_targets)
    list(POP_FRONT _pending_targets _target)
    if(_target IN_LIST _visited_targets OR NOT TARGET "${_target}")
      continue()
    endif()
    list(APPEND _visited_targets "${_target}")

    get_target_property(_aliased_target "${_target}" ALIASED_TARGET)
    if(_aliased_target)
      list(APPEND _pending_targets "${_aliased_target}")
      continue()
    endif()

    get_target_property(_direct_links "${_target}" LINK_LIBRARIES)
    get_target_property(_interface_links
      "${_target}" INTERFACE_LINK_LIBRARIES)
    foreach(_link_entry IN LISTS _direct_links _interface_links)
      if(TARGET "${_link_entry}")
        list(APPEND _pending_targets "${_link_entry}")
        continue()
      endif()

      # Link-only/build-interface generator expressions still name their
      # target operands literally. Extract only tokens that resolve to targets;
      # expression operators and non-target libraries are ignored.
      string(REGEX MATCHALL "[A-Za-z0-9_.:+-]+"
        _link_tokens "${_link_entry}")
      foreach(_link_token IN LISTS _link_tokens)
        if(TARGET "${_link_token}")
          list(APPEND _pending_targets "${_link_token}")
        endif()
      endforeach()
    endforeach()
  endwhile()

  list(REMOVE_DUPLICATES _visited_targets)
  set(${output_variable} "${_visited_targets}" PARENT_SCOPE)
endfunction()

function(_bioseq_website_normalize_authored_cppm
    output_variable target source)
  set(${output_variable} "" PARENT_SCOPE)
  if("${source}" MATCHES "\\$<" OR NOT "${source}" MATCHES "\\.cppm$")
    return()
  endif()

  if(IS_ABSOLUTE "${source}")
    set(_candidate "${source}")
  else()
    get_target_property(_target_source_directory "${target}" SOURCE_DIR)
    if(NOT _target_source_directory)
      return()
    endif()
    get_filename_component(_candidate
      "${_target_source_directory}/${source}" ABSOLUTE)
  endif()
  if(NOT EXISTS "${_candidate}" OR IS_DIRECTORY "${_candidate}")
    return()
  endif()

  file(REAL_PATH "${_candidate}" _candidate)
  set(_is_authored_source FALSE)
  foreach(_authored_root IN ITEMS
      "${CMAKE_SOURCE_DIR}/bioseq"
      "${CMAKE_SOURCE_DIR}/common")
    file(REAL_PATH "${_authored_root}" _authored_root_real)
    string(FIND "${_candidate}/" "${_authored_root_real}/" _root_prefix)
    if(_root_prefix EQUAL 0)
      set(_is_authored_source TRUE)
      break()
    endif()
  endforeach()
  if(_is_authored_source)
    set(${output_variable} "${_candidate}" PARENT_SCOPE)
  endif()
endfunction()

_bioseq_website_collect_link_closure(
  _production_source_targets postgresql_bioseq)
set(_module_units)
set(_ordinary_cppm_units)
foreach(_source_target IN LISTS _production_source_targets)
  get_target_property(_module_sets "${_source_target}" CXX_MODULE_SETS)
  get_target_property(_interface_module_sets
    "${_source_target}" INTERFACE_CXX_MODULE_SETS)
  if(NOT _module_sets)
    set(_module_sets)
  endif()
  if(NOT _interface_module_sets)
    set(_interface_module_sets)
  endif()
  list(APPEND _module_sets ${_interface_module_sets})
  list(REMOVE_DUPLICATES _module_sets)
  foreach(_module_set IN LISTS _module_sets)
    if(_module_set STREQUAL "CXX_MODULES")
      get_target_property(_module_sources
        "${_source_target}" CXX_MODULE_SET)
    else()
      get_target_property(_module_sources
        "${_source_target}" "CXX_MODULE_SET_${_module_set}")
    endif()
    foreach(_module_source IN LISTS _module_sources)
      _bioseq_website_normalize_authored_cppm(
        _normalized_source "${_source_target}" "${_module_source}")
      if(_normalized_source)
        list(APPEND _module_units "${_normalized_source}")
      endif()
    endforeach()
  endforeach()

  get_target_property(_target_sources "${_source_target}" SOURCES)
  foreach(_target_source IN LISTS _target_sources)
    _bioseq_website_normalize_authored_cppm(
      _normalized_source "${_source_target}" "${_target_source}")
    if(_normalized_source)
      list(APPEND _ordinary_cppm_units "${_normalized_source}")
    endif()
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _module_units)
list(REMOVE_DUPLICATES _ordinary_cppm_units)
list(REMOVE_ITEM _ordinary_cppm_units ${_module_units})

get_target_property(_pg_entrypoints postgresql_bioseq
  BIOSEQ_EXTENSION_PG_ENTRYPOINT_SOURCES)
if(NOT _pg_entrypoints)
  message(FATAL_ERROR
    "postgresql_bioseq did not publish its PG entrypoint source")
endif()
set(_normalized_pg_entrypoints)
foreach(_entry IN LISTS _pg_entrypoints)
  _bioseq_website_normalize_authored_cppm(
    _normalized_entrypoint postgresql_bioseq "${_entry}")
  if(NOT _normalized_entrypoint)
    message(FATAL_ERROR
      "PG entrypoint is not an authored Bioseq .cppm unit: ${_entry}")
  endif()
  if(_normalized_entrypoint IN_LIST _module_units)
    message(FATAL_ERROR
      "PG entrypoint must be an ordinary target source, not a CXX_MODULES "
      "provider: ${_normalized_entrypoint}")
  endif()
  if(NOT _normalized_entrypoint IN_LIST _ordinary_cppm_units)
    message(FATAL_ERROR
      "PG entrypoint is not owned by postgresql_bioseq as an ordinary source: "
      "${_normalized_entrypoint}")
  endif()
  list(APPEND _normalized_pg_entrypoints "${_normalized_entrypoint}")
endforeach()
list(LENGTH _normalized_pg_entrypoints _pg_entrypoint_count)
list(REMOVE_DUPLICATES _normalized_pg_entrypoints)
list(LENGTH _normalized_pg_entrypoints _unique_pg_entrypoint_count)
if(NOT _pg_entrypoint_count EQUAL _unique_pg_entrypoint_count)
  message(FATAL_ERROR
    "postgresql_bioseq published duplicate PG entrypoint source paths")
endif()
list(REMOVE_DUPLICATES _module_units)
list(SORT _module_units)
list(SORT _normalized_pg_entrypoints)

set(_reachable_units_file "${_output_root}/reachable-units.txt")
set(_entrypoint_units_file "${_output_root}/entrypoint-units.txt")
set(_source_universe "${_output_root}/source-universe.json")
string(REPLACE ";" "\n" _units_lines "${_module_units}")
file(CONFIGURE
  OUTPUT "${_reachable_units_file}"
  CONTENT "${_units_lines}\n"
  @ONLY)
string(REPLACE ";" "\n" _entrypoint_units_lines
  "${_normalized_pg_entrypoints}")
file(CONFIGURE
  OUTPUT "${_entrypoint_units_file}"
  CONTENT "${_entrypoint_units_lines}\n"
  @ONLY)
string(CONCAT _universe_json
  "{\n  \"schema\": \"source_universe\",\n"
  "  \"schema_revision\": 1,\n"
  "  \"owner\": \"bioseq_website\",\n"
  "  \"units\": [\n")
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
# Doxygen INPUT is the interface closure followed by the exact ordinary PG
# entrypoints. source_graph writes that scan inventory; the build-time fragment
# sets Doxyfile INPUT to exactly those files.
set(_reachable_scan_units "${_output_root}/reachable-source-units.txt")
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
set(_design_reference_dir "${_source_dir}/design_source")
set(_design_translator "${_source_dir}/tools/translate_reference_design.py")
if(NOT EXISTS "${_design_reference_dir}/index.html"
   OR NOT EXISTS "${_design_reference_dir}/docs.html"
   OR NOT EXISTS "${_design_reference_dir}/benchmarks.html"
   OR NOT EXISTS "${_design_reference_dir}/bench-loader.js"
   OR NOT EXISTS "${_design_reference_dir}/benchmarks-manifest.json"
   OR NOT EXISTS "${_design_reference_dir}/support.js")
  message(FATAL_ERROR
    "website: supplied landing, docs, benchmark HTML/JS/manifest design sources "
    "are required for design-translation parity")
endif()
file(GLOB_RECURSE _design_reference_files CONFIGURE_DEPENDS
  "${_design_reference_dir}/*")
set(_design_translation_outputs
  "${_source_dir}/templates/base.html.j2"
  "${_source_dir}/templates/benchmarks.html.j2"
  "${_source_dir}/templates/landing.html.j2"
  "${_source_dir}/templates/site-header.html.j2"
  "${_source_dir}/templates/site-footer.html.j2"
  "${_source_dir}/static/css/benchmarks-design.css"
  "${_source_dir}/static/css/docs-design.css"
  "${_source_dir}/static/css/landing-design.css"
  "${_source_dir}/static/js/benchmarks-design.mjs")
set(_design_translation_inputs)
foreach(_relative
    index.html
    docs.html
    benchmarks.html
    bench-loader.js
    benchmarks-manifest.json
    support.js
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
    media/vials.mp4
    js/bench-loader.js
    benchmarks-manifest.json)
  list(APPEND _design_translation_outputs "${_source_dir}/static/${_relative}")
endforeach()
set(_design_translation_stamp "${_output_root}/design-translation-check.stamp")
add_custom_command(
  OUTPUT "${_design_translation_stamp}"
  COMMAND "${Python3_EXECUTABLE}" "${_design_translator}" --check
  COMMAND "${Python3_EXECUTABLE}" "${_source_dir}/tools/theme_palette.py" --check
  COMMAND "${CMAKE_COMMAND}" -E touch "${_design_translation_stamp}"
  DEPENDS "${_design_translator}" ${_design_translation_inputs}
          ${_design_translation_outputs} ${_static_files}
          "${_source_dir}/tools/theme_palette.py"
          "${_source_dir}/tools/theme_refinements.css"
  COMMENT "website: verify non-mutating supplied-design translation parity"
  VERBATIM)

# Generate the validated benchmark bundle independently of the static design
# projection.  The generator replaces only this build-owned tree after complete
# validation/staging, retains every admitted byte, and writes the production manifest.
set(_benchmark_bundle_contract
  "${_source_dir}/tests/contract/test_benchmark_bundle.py")
set(_benchmark_bundle_contract_stamp
  "${_output_root}/benchmark-bundle-contract.stamp")
add_custom_command(
  OUTPUT "${_benchmark_bundle_contract_stamp}"
  COMMAND "${CMAKE_COMMAND}" -E env PYTHONDONTWRITEBYTECODE=1
          "${Python3_EXECUTABLE}" "${_benchmark_bundle_contract}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_benchmark_bundle_contract_stamp}"
  DEPENDS "${_benchmark_bundle_contract}" "${_benchmark_bundle_generator}"
  COMMENT "website: verify dynamic benchmark bundle contract"
  VERBATIM)

set(_benchmark_bundle_dir "${_output_root}/benchmark-bundle")
set(_benchmark_bundle_stamp "${_output_root}/benchmark-bundle.stamp")
add_custom_command(
  OUTPUT "${_benchmark_bundle_stamp}"
  BYPRODUCTS "${_benchmark_bundle_dir}/benchmarks-manifest.json"
  COMMAND "${Python3_EXECUTABLE}" "${_benchmark_bundle_generator}"
          --repository-root "${CMAKE_SOURCE_DIR}"
          --output-root "${_benchmark_bundle_dir}"
          --build-root "${CMAKE_BINARY_DIR}"
          --reporting-root "${_benchmark_reporting_root}"
          ${_benchmark_configuration_arguments}
          ${_benchmark_manifest_override_arguments}
  COMMAND "${CMAKE_COMMAND}" -E touch "${_benchmark_bundle_stamp}"
  DEPENDS "${_benchmark_bundle_contract_stamp}"
          "${_benchmark_bundle_generator}"
          "${_benchmark_discovery_inventory}"
          ${_benchmark_discovery_inputs}
          ${_benchmark_manifest_override_dependency}
  COMMENT "website: discover and validate public benchmark artifacts"
  VERBATIM)

# Prepare the exact static tree consumed by every render.  The source static
# directory remains the mechanical projection of the supplied design.
# benchmark deployment adaptation happens only in this confined build-tree copy.
set(_render_static_dir "${_output_root}/render-static")
set(_render_static_stamp "${_output_root}/render-static.stamp")
set(_render_benchmark_manifest
  "${_render_static_dir}/benchmarks-manifest.json")
add_custom_command(
  OUTPUT "${_render_static_stamp}"
  BYPRODUCTS "${_render_benchmark_manifest}"
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_render_static_dir}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${_static_dir}" "${_render_static_dir}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
          "${_render_static_dir}/benchmark-data"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${_benchmark_bundle_dir}/benchmark-data"
          "${_render_static_dir}/benchmark-data"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${_benchmark_bundle_dir}/benchmarks-manifest.json"
          "${_render_benchmark_manifest}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_render_static_stamp}"
  DEPENDS "${_design_translation_stamp}" "${_benchmark_bundle_stamp}"
          ${_static_files}
  COMMENT "website: prepare same-origin benchmark data in the static bundle"
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
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/cmake/tests/integration/test_private_postgres.py"
          --pg-bin-dir "${_pg_bindir}"
          --scratch-root "${_output_root}/private-runner-tests"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_SOURCE_DIR}/cmake/tests/integration/test_private_postgres_stage.py"
          --cmake "${CMAKE_COMMAND}"
          --scratch-root "${_output_root}/private-stage-tests"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_runner_tests_stamp}"
  DEPENDS
          "${CMAKE_SOURCE_DIR}/cmake/tests/integration/test_private_postgres.py"
          "${CMAKE_SOURCE_DIR}/cmake/tests/integration/test_private_postgres_stage.py"
          "${CMAKE_SOURCE_DIR}/cmake/stage_private_postgres.cmake"
          "${CMAKE_SOURCE_DIR}/cmake/run_private_postgres.py"
          "${CMAKE_SOURCE_DIR}/cmake/private_postgres.py"
  COMMENT "website: private PostgreSQL runner and relocation self-tests"
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
          --static "${_render_static_dir}"
          --base-path "${_base_path}"
          --source-revision "${_revision}"
          ${_plan_drafts_flag}
          --output "${_plan_json}"
          --report-dir "${_report_dir}"
  DEPENDS ${_gen_dep} "${_selftest_stamp}" "${_design_translation_stamp}"
          "${_render_static_stamp}"
          "${_site_config}" "${_landing_content}" "${_contact_content}"
          ${_tutorial_md} ${_blog_inputs} ${_template_files} ${_static_files}
  COMMENT "website: plan (config/template/frontmatter validation)"
  VERBATIM)

# --- source_graph (§17.3 step 4) ----------------------------------------------
# The provider graph consumes only CXX_MODULES units. Exact ordinary PG
# entrypoints are a separate scan inventory appended after graph closure.
set(_source_graph_json "${_output_root}/source-graph.json")
set(_source_graph_work "${_output_root}/source-graph-work")
set(_source_graph_raw_json "${_source_graph_work}/source-graph.json")
set(_reachable_scan_units_raw
  "${_source_graph_work}/reachable-source-units.txt")
set(_source_graph_run_stamp "${_source_graph_work}/complete.stamp")
add_custom_command(
  OUTPUT "${_source_graph_run_stamp}"
  BYPRODUCTS "${_source_graph_raw_json}" "${_reachable_scan_units_raw}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${_source_graph_work}"
  COMMAND "${_gen}" website source_graph
          --source-root "${CMAKE_SOURCE_DIR}"
          --reachable-units "${_reachable_units_file}"
          --entrypoint-units "${_entrypoint_units_file}"
          --compile-db "${_compile_db}"
          --site-config "${_site_config}"
          --output "${_source_graph_raw_json}"
          --reachable-output "${_reachable_scan_units_raw}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_source_graph_run_stamp}"
  DEPENDS ${_gen_dep} postgresql_bioseq "${_source_universe}"
          "${_reachable_units_file}" "${_entrypoint_units_file}"
          "${_site_config}" "${_compile_db}"
          ${_normalized_pg_entrypoints}
  COMMENT "website: interface source graph and PG entrypoint scan inventory"
  VERBATIM)
add_custom_command(
  OUTPUT "${_source_graph_json}" "${_reachable_scan_units}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${_source_graph_raw_json}" "${_source_graph_json}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${_reachable_scan_units_raw}" "${_reachable_scan_units}"
  DEPENDS "${_source_graph_run_stamp}"
  COMMENT "website: stabilize source-graph outputs by content"
  VERBATIM)

# --- Doxygen XML (§17.3 step 5) -----------------------------------------------
# Doxygen INPUT is the interface closure followed by exact PG entrypoints,
# never a recursive source glob. The graph JSON remains interface-only.
set(_doxygen_stamp "${_output_root}/doxygen.stamp")
add_custom_command(
  OUTPUT "${_doxygen_stamp}"
  COMMAND "${CMAKE_COMMAND}"
          -DCLOSURE=${_reachable_scan_units}
          -DSOURCE_ROOT=${CMAKE_SOURCE_DIR}
          -DFRAGMENT_OUT=${_doxygen_input_fragment}
          -P "${CMAKE_SOURCE_DIR}/cmake/doxygen_input_fragment.cmake"
  COMMAND "${DOXYGEN_EXECUTABLE}" "${_output_root}/Doxyfile"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_doxygen_stamp}"
  DEPENDS "${_source_graph_json}" "${_reachable_scan_units}"
          "${_output_root}/Doxyfile"
          "${CMAKE_SOURCE_DIR}/cmake/doxygen_input_fragment.cmake"
          ${_module_units} ${_normalized_pg_entrypoints}
  COMMENT "website: Doxygen 1.9.8 XML over interfaces and PG entrypoints"
  VERBATIM)

# --- cpp_index (§17.3 step 5) -------------------------------------------------
# cpp_index parses the same interface-plus-entrypoint inventory as Doxygen.
set(_cpp_entities "${_output_root}/cpp-entities.json")
set(_cpp_coverage "${_output_root}/cpp-coverage.json")
add_custom_command(
  OUTPUT "${_cpp_entities}" "${_cpp_coverage}"
  COMMAND "${_gen}" website cpp_index
          --source-root "${CMAKE_SOURCE_DIR}"
          --doxygen-xml "${_doxygen_xml}"
          --compile-db "${_compile_db}"
          --reachable-units "${_reachable_scan_units}"
          --site-config "${_site_config}"
          --output "${_cpp_entities}"
          --coverage-output "${_cpp_coverage}"
          --report-dir "${_report_dir}"
  DEPENDS ${_gen_dep} "${_doxygen_stamp}" "${_plan_json}" "${_source_graph_json}"
          "${_compile_db}" "${_reachable_scan_units}"
  COMMENT "website: cpp_index over interfaces and PG entrypoints"
  VERBATIM)

# --- Relocatable PostgreSQL stage (§13.2 / §17.3 step 6) ----------------------
set(_stage_root "${_output_root}/postgres-prefix")
set(_stage_manifest "${_output_root}/stage-manifest.json")
set(_stage_extension_hash "${_output_root}/staged-extension.sha256")
set(_stage_artifact_spec "${_output_root}/extension-artifacts.json")
set(_stage_artifact_input_root "${_output_root}/artifact-inputs")
string(REGEX REPLACE "^/+" "" _pg_bindir_rel "${_pg_bindir}")
set(_staged_bindir "${_stage_root}/${_pg_bindir_rel}")
get_target_property(_cache_extension_control_file
  postgresql_bioseq_cache BIOSEQ_EXTENSION_CONTROL_FILE)
get_target_property(_cache_extension_install_sql
  postgresql_bioseq_cache BIOSEQ_EXTENSION_INSTALL_SQL)
get_target_property(_cache_extension_sql_manifest
  postgresql_bioseq_cache BIOSEQ_EXTENSION_SQL_MANIFEST)
get_target_property(_cache_extension_dso_property
  postgresql_bioseq_cache BIOSEQ_EXTENSION_DSO)
get_target_property(_bioseq_extension_control_file
  postgresql_bioseq BIOSEQ_EXTENSION_CONTROL_FILE)
get_target_property(_bioseq_extension_install_sql
  postgresql_bioseq BIOSEQ_EXTENSION_INSTALL_SQL)
get_target_property(_bioseq_extension_sql_manifest
  postgresql_bioseq BIOSEQ_EXTENSION_SQL_MANIFEST)
get_directory_property(_bioseq_settings_documentation_manifest
  DIRECTORY "${CMAKE_SOURCE_DIR}/bioseq"
  DEFINITION BIOSEQ_SQL_TEST_INDEXES_GENERATED_MANIFEST)
if(NOT _bioseq_settings_documentation_manifest)
  message(FATAL_ERROR "website: test-profile setting documentation manifest is unavailable")
endif()
get_target_property(_bioseq_extension_dso_property
  postgresql_bioseq BIOSEQ_EXTENSION_DSO)
foreach(_extension_artifact_variable IN ITEMS
    _cache_extension_control_file
    _cache_extension_install_sql
    _cache_extension_sql_manifest
    _cache_extension_dso_property
    _bioseq_extension_control_file
    _bioseq_extension_install_sql
    _bioseq_extension_sql_manifest
    _bioseq_extension_dso_property)
  if(NOT DEFINED ${_extension_artifact_variable}
     OR "${${_extension_artifact_variable}}" STREQUAL ""
     OR "${${_extension_artifact_variable}}" MATCHES "-NOTFOUND$")
    message(FATAL_ERROR
      "website: production extension target did not publish "
      "${_extension_artifact_variable}")
  endif()
endforeach()
set(_cache_extension_dso
  "$<TARGET_GENEX_EVAL:postgresql_bioseq_cache,${_cache_extension_dso_property}>")
set(_bioseq_extension_dso
  "$<TARGET_GENEX_EVAL:postgresql_bioseq,${_bioseq_extension_dso_property}>")

# The website package gate independently validates the control, generated
# install SQL, authoritative SQL manifest, and DSO for both production
# extensions before one ordered, relocatable stage can be materialized.
set(_sql_package_verifier
  "${_source_dir}/tests/contract/test_generated_sql_package.py")
set(_cache_sql_public_identity_golden
  "${_source_dir}/tests/fixtures/sql_cache_public_identities.json")
set(_bioseq_sql_public_identity_golden
  "${_source_dir}/tests/fixtures/sql_public_identities.json")
set(_sql_package_verification_stamp
  "${_output_root}/generated-sql-package-tests.stamp")
add_custom_command(
  OUTPUT "${_sql_package_verification_stamp}"
  COMMAND "${Python3_EXECUTABLE}" "${_sql_package_verifier}"
          --artifact
            bioseq_cache
            "${_cache_extension_control_file}"
            "${_cache_extension_install_sql}"
            "${_cache_extension_sql_manifest}"
            "${_cache_extension_dso}"
            "${_cache_sql_public_identity_golden}"
          --artifact
            bioseq
            "${_bioseq_extension_control_file}"
            "${_bioseq_extension_install_sql}"
            "${_bioseq_extension_sql_manifest}"
            "${_bioseq_extension_dso}"
            "${_bioseq_sql_public_identity_golden}"
          --stamp "${_sql_package_verification_stamp}"
  DEPENDS postgresql_bioseq_cache postgresql_bioseq
          "${_cache_extension_control_file}"
          "${_cache_extension_install_sql}"
          "${_cache_extension_sql_manifest}"
          "${_bioseq_extension_control_file}"
          "${_bioseq_extension_install_sql}"
          "${_bioseq_extension_sql_manifest}"
          "${_sql_package_verifier}"
          "${_cache_sql_public_identity_golden}"
          "${_bioseq_sql_public_identity_golden}"
  COMMENT "website: qualify cache and Bioseq production extension artifacts"
  VERBATIM)
add_custom_target(bioseq_generated_sql_package_tests
  DEPENDS "${_sql_package_verification_stamp}")

set(_cache_stage_artifact_name_input
  "${_stage_artifact_input_root}/0-name.txt")
set(_cache_stage_control_input
  "${_stage_artifact_input_root}/0-control.path")
set(_cache_stage_install_sql_input
  "${_stage_artifact_input_root}/0-install_sql.path")
set(_cache_stage_library_input
  "${_stage_artifact_input_root}/0-library.path")
set(_cache_stage_module_override_input
  "${_stage_artifact_input_root}/0-module_pathname_override.txt")
set(_bioseq_stage_artifact_name_input
  "${_stage_artifact_input_root}/1-name.txt")
set(_bioseq_stage_control_input
  "${_stage_artifact_input_root}/1-control.path")
set(_bioseq_stage_install_sql_input
  "${_stage_artifact_input_root}/1-install_sql.path")
set(_bioseq_stage_library_input
  "${_stage_artifact_input_root}/1-library.path")
set(_bioseq_stage_module_override_input
  "${_stage_artifact_input_root}/1-module_pathname_override.txt")
set(_stage_artifact_inputs
  "${_cache_stage_artifact_name_input}"
  "${_cache_stage_control_input}"
  "${_cache_stage_install_sql_input}"
  "${_cache_stage_library_input}"
  "${_cache_stage_module_override_input}"
  "${_bioseq_stage_artifact_name_input}"
  "${_bioseq_stage_control_input}"
  "${_bioseq_stage_install_sql_input}"
  "${_bioseq_stage_library_input}"
  "${_bioseq_stage_module_override_input}")
file(GENERATE
  OUTPUT "${_cache_stage_artifact_name_input}" CONTENT "bioseq_cache")
file(GENERATE
  OUTPUT "${_cache_stage_control_input}"
  CONTENT "${_cache_extension_control_file}")
file(GENERATE
  OUTPUT "${_cache_stage_install_sql_input}"
  CONTENT "${_cache_extension_install_sql}")
file(GENERATE
  OUTPUT "${_cache_stage_library_input}"
  CONTENT "${_cache_extension_dso}")
file(GENERATE
  OUTPUT "${_cache_stage_module_override_input}" CONTENT "-")
file(GENERATE
  OUTPUT "${_bioseq_stage_artifact_name_input}" CONTENT "bioseq")
file(GENERATE
  OUTPUT "${_bioseq_stage_control_input}"
  CONTENT "${_bioseq_extension_control_file}")
file(GENERATE
  OUTPUT "${_bioseq_stage_install_sql_input}"
  CONTENT "${_bioseq_extension_install_sql}")
file(GENERATE
  OUTPUT "${_bioseq_stage_library_input}"
  CONTENT "${_bioseq_extension_dso}")
file(GENERATE
  OUTPUT "${_bioseq_stage_module_override_input}" CONTENT "-")

set(_stage_artifact_writer
  "${CMAKE_SOURCE_DIR}/cmake/write_private_postgres_artifacts.cmake")
add_custom_command(
  OUTPUT "${_stage_artifact_spec}"
  COMMAND "${CMAKE_COMMAND}"
          "-Dartifact_input_root=${_stage_artifact_input_root}"
          -Dartifact_count=2
          "-Dartifact_spec_output=${_stage_artifact_spec}"
          -P "${_stage_artifact_writer}"
  DEPENDS ${_stage_artifact_inputs} "${_stage_artifact_writer}"
  COMMENT "website: write ordered cache and Bioseq artifact specification"
  VERBATIM)

add_custom_command(
  OUTPUT "${_stage_manifest}" "${_stage_extension_hash}"
  COMMAND "${CMAKE_COMMAND}"
          -DPG_CONFIG=${PostgreSQL_CONFIG}
          -DSTAGE_ROOT=${_stage_root}
          -DSTAGE_PARENT=${_output_root}
          "-DARTIFACT_SPEC=${_stage_artifact_spec}"
          -DMANIFEST_OUT=${_stage_manifest}
          -DHASH_OUT=${_stage_extension_hash}
          -P "${CMAKE_SOURCE_DIR}/cmake/stage_private_postgres.cmake"
  DEPENDS postgresql_bioseq_cache postgresql_bioseq
          "${_sql_package_verification_stamp}"
          "${_stage_artifact_spec}"
          "${_cache_extension_control_file}"
          "${_cache_extension_install_sql}"
          "${_bioseq_extension_control_file}"
          "${_bioseq_extension_install_sql}"
          "${CMAKE_SOURCE_DIR}/cmake/stage_private_postgres.cmake"
  COMMENT "website: stage relocatable PostgreSQL + both production extensions"
  VERBATIM)

# Linux can enforce the optional 4 GiB RLIMIT_AS boundary before exec. Darwin
# accounts mandatory process mappings against RLIMIT_AS, so that same boundary
# is already below the launcher's address space and setrlimit rejects it before
# PostgreSQL starts. Keep the unsupported coordinate explicit in the generic
# runner and omit only this optional cap for the Darwin website build; lifecycle
# commands remain isolated in process groups and bounded by their deadlines.
set(_website_private_postgres_limit_args)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_website_private_postgres_limit_args
    --address-space-limit-mib 4096)
elseif(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR
    "website: private PostgreSQL resource policy is unsupported on "
    "${CMAKE_SYSTEM_NAME}")
endif()

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
          ${_website_private_postgres_limit_args}
          -- "${Python3_EXECUTABLE}"
             "${CMAKE_SOURCE_DIR}/website/tests/integration/test_sql_isolation.py"
             --generator "${_gen}"
             --pg-bin-dir "${_staged_bindir}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${_isolation_tests_stamp}"
  DEPENDS ${_gen_dep} "${_stage_manifest}" "${_stage_extension_hash}"
          "${CMAKE_SOURCE_DIR}/cmake/run_private_postgres.py"
          "${CMAKE_SOURCE_DIR}/cmake/private_postgres.py"
          "${CMAKE_SOURCE_DIR}/cmake/private_postgresql.conf.in"
          "${CMAKE_SOURCE_DIR}/website/tests/integration/test_sql_isolation.py"
  COMMENT "website: isolated SQL executor + result metadata integration tests"
  USES_TERMINAL VERBATIM)
add_custom_target(bioseq_website_isolation_tests
  DEPENDS "${_isolation_tests_stamp}")

# --- catalog + execute on the staged cluster (§17.3 step 7) -------------------
# One owner process starts the copied server under the host resource policy,
# installs bioseq_cache before bioseq, then launches one generator child. Both
# authoritative manifests are supplied in that same provider-before-consumer
# order for catalog capture and isolated tutorial execution. Nothing after this
# step needs a cluster.
set(_sql_entities "${_output_root}/sql-entities.json")
set(_site_model "${_output_root}/site-model.json")
set(_landing_fasta "${_source_dir}/tests/fixtures/landing_reads.fasta")
set(_readme_fasta "${_source_dir}/tests/fixtures/readme_samples.fasta")
if(NOT EXISTS "${_landing_fasta}")
  message(FATAL_ERROR "website: real bioseq.load fixture is missing: ${_landing_fasta}")
endif()

# The template owns the extension objects and globally revokes internal routine
# execution. The executor grants each unique clone owner only the schema/table
# ACLs needed inside that clone; direct per-clone grants keep PostgreSQL 13
# compatibility because pg_database_owner exists only in PostgreSQL 14+.
file(WRITE "${_output_root}/create-extension.sql"
  "CREATE EXTENSION bioseq_cache;\n"
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
"if [[ \"$PRELOAD\" != 'bioseq_cache,bioseq' ]]; then\n"
"  echo \"website: expected shared_preload_libraries=bioseq_cache,bioseq, got $PRELOAD\" >&2\n"
"  exit 1\n"
"fi\n"
"\"${CMAKE_COMMAND}\" -E copy_if_different '${_landing_fasta}' \"$BIOSEQ_TEST_PG_DATA_DIR/landing_reads.fasta\"\n"
"\"${CMAKE_COMMAND}\" -E copy_if_different '${_readme_fasta}' \"$BIOSEQ_TEST_PG_DATA_DIR/readme_samples.fasta\"\n"
"\"${_gen}\" website catalog --conninfo \"$CONN\" \\\n"
"  --sql-manifest '${_cache_extension_sql_manifest}' \\\n"
"  --sql-manifest '${_bioseq_extension_sql_manifest}' \\\n"
"  --output '${_sql_entities}'\n"
"\"${_gen}\" website execute \\\n"
"  --source '${_source_dir}' --version '${_version}' \\\n"
"  --site-config '${_site_config}' \\\n"
"  --sql-manifest '${_cache_extension_sql_manifest}' \\\n"
"  --sql-manifest '${_bioseq_extension_sql_manifest}' \\\n"
"  --sql-documentation-manifest '${_bioseq_settings_documentation_manifest}' \\\n"
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
          ${_website_private_postgres_limit_args}
          --preload-library bioseq_cache
          --preload-library bioseq
          --disable-prepared-transactions
          --bootstrap "${_output_root}/create-extension.sql"
          -- bash "${_cluster_child}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_sql_documentation_coverage.py"
          --model "${_site_model}"
          --manifest "${_cache_extension_sql_manifest}"
          --manifest "${_bioseq_extension_sql_manifest}"
          --catalog "${_sql_entities}"
  COMMAND "${Python3_EXECUTABLE}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_setting_documentation.py"
          --model "${_site_model}"
          --manifest "${_bioseq_extension_sql_manifest}"
          --documentation-manifest "${_bioseq_settings_documentation_manifest}"
  DEPENDS ${_gen_dep} "${_stage_manifest}" "${_stage_extension_hash}" "${_cluster_child}"
          "${_cpp_entities}" "${_plan_json}"
          "${_cache_extension_sql_manifest}"
          "${_bioseq_extension_sql_manifest}"
          "${_bioseq_settings_documentation_manifest}"
          "${_landing_fasta}" "${_readme_fasta}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_sql_documentation_coverage.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_setting_documentation.py"
          ${_tutorial_md} ${_blog_inputs} ${_template_files} ${_static_files}
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
          --static "${_render_static_dir}"
          --base-path "${_base_path}"
          --source-revision "${_revision}"
          --output "${_staging_dir}"
          --model-validation-stamp "${_model_validation_stamp}"
          --report-dir "${_report_dir}"
  DEPENDS ${_gen_dep} "${_site_model}" "${_design_translation_stamp}"
          "${_render_static_stamp}" "${_landing_content}" "${_contact_content}"
          ${_template_files} ${_static_files}
  COMMENT "website: render resolved model into owner-marked staging (no database)"
  VERBATIM)

# --- independent html5lib validation of staging BEFORE the swap (step 9) ------
# The pinned Python validators (§4.4) inspect staging, not the published tree, so a
# rejected page blocks publication rather than being served first (fail-closed §14).
set(_html_validation_stamp "${_output_root}/html-validation.stamp")
add_custom_command(
  OUTPUT "${_html_validation_stamp}"
  COMMAND "${_venv_python}"
          "${CMAKE_SOURCE_DIR}/website/tests/contract/test_generated_site_validation.py"
          --site "${_staging_dir}"
          --stamp "${_html_validation_stamp}"
          --asset-lock "${_static_dir}/third_party.lock.yaml"
  COMMAND "${_venv_python}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_setting_documentation.py"
          --model "${_site_model}" --site "${_staging_dir}"
          --manifest "${_bioseq_extension_sql_manifest}"
          --documentation-manifest "${_bioseq_settings_documentation_manifest}"
  COMMAND "${_venv_python}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_landing_content.py"
  COMMAND "${_venv_python}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_blog_content.py"
          --model "${_site_model}" --site "${_staging_dir}"
  DEPENDS "${_staging_manifest}" "${_venv_stamp}"
          "${CMAKE_SOURCE_DIR}/website/tests/contract/test_generated_site_validation.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_setting_documentation.py"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_landing_content.py"
          "${_landing_content}"
          "${CMAKE_CURRENT_SOURCE_DIR}/tests/contract/test_blog_content.py"
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
  set(_benchmark_browser_fixtures
      "${_benchmark_bundle_stamp}")
  set(_fixture_root_slash "${_output_root}/fixture-root")
  set(_fixture_root_bioseqdb "${_output_root}/fixture-bioseqdb")
  add_custom_command(
    OUTPUT "${_browser_stamp}"
    BYPRODUCTS "${_browser_deploy_stamp}" "${_browser_root_stamp}"
               "${_browser_bioseqdb_stamp}"
    COMMAND "${_gen}" website render --model "${_site_model}"
            --site-config "${_site_config}" --templates "${_templates_dir}"
            --static "${_render_static_dir}" --base-path "/"
            --source-revision "${_revision}" --output "${_fixture_root_slash}"
    COMMAND "${_gen}" website render --model "${_site_model}"
            --site-config "${_site_config}" --templates "${_templates_dir}"
            --static "${_render_static_dir}" --base-path "/bioseqdb/"
            --source-revision "${_revision}" --output "${_fixture_root_bioseqdb}"
    COMMAND "${_venv_python}"
            "${CMAKE_SOURCE_DIR}/website/tests/contract/test_generated_site_validation.py"
            --site "${_fixture_root_slash}"
            --stamp "${_output_root}/fixture-root-validation.stamp"
            --asset-lock "${_static_dir}/third_party.lock.yaml"
    COMMAND "${_venv_python}"
            "${CMAKE_SOURCE_DIR}/website/tests/contract/test_generated_site_validation.py"
            --site "${_fixture_root_bioseqdb}"
            --stamp "${_output_root}/fixture-bioseqdb-validation.stamp"
            --asset-lock "${_static_dir}/third_party.lock.yaml"
    COMMAND "${CMAKE_COMMAND}"
            -DPYTHON=${_venv_python}
            -DBROWSER_TEST=${CMAKE_SOURCE_DIR}/website/tests/integration/test_browser.py
            -DSITE=${_staging_dir} -DBASE_PATH=${_base_path} -DVERSION=${_version}
            -DMODEL=${_site_model}
            -DDESIGN_SOURCE=${_design_reference_dir}
            -DARTIFACT_DIR=${_output_root}/browser-artifacts/deployment-base
            -DSTAMP=${_browser_deploy_stamp}
            -DBROWSERS_PATH=${_output_root}/playwright-browsers
            -P "${CMAKE_SOURCE_DIR}/website/tests/support/browser_test_launcher.cmake"
    COMMAND "${CMAKE_COMMAND}"
            -DPYTHON=${_venv_python}
            -DBROWSER_TEST=${CMAKE_SOURCE_DIR}/website/tests/integration/test_browser.py
            -DSITE=${_fixture_root_slash} -DBASE_PATH=/ -DVERSION=${_version}
            -DMODEL=${_site_model}
            -DDESIGN_SOURCE=${_design_reference_dir}
            -DARTIFACT_DIR=${_output_root}/browser-artifacts/root-base
            -DSTAMP=${_browser_root_stamp}
            -DBROWSERS_PATH=${_output_root}/playwright-browsers
            -P "${CMAKE_SOURCE_DIR}/website/tests/support/browser_test_launcher.cmake"
    COMMAND "${CMAKE_COMMAND}"
            -DPYTHON=${_venv_python}
            -DBROWSER_TEST=${CMAKE_SOURCE_DIR}/website/tests/integration/test_browser.py
            -DSITE=${_fixture_root_bioseqdb} -DBASE_PATH=/bioseqdb/
            -DVERSION=${_version}
            -DMODEL=${_site_model}
            -DDESIGN_SOURCE=${_design_reference_dir}
            -DARTIFACT_DIR=${_output_root}/browser-artifacts/bioseqdb-base
            -DSTAMP=${_browser_bioseqdb_stamp}
            -DBROWSERS_PATH=${_output_root}/playwright-browsers
            -P "${CMAKE_SOURCE_DIR}/website/tests/support/browser_test_launcher.cmake"
    COMMAND "${CMAKE_COMMAND}" -E touch "${_browser_stamp}"
    DEPENDS ${_gen_dep} "${_html_validation_stamp}" "${_site_model}"
            "${_venv_stamp}"
            "${_design_translation_stamp}"
            "${CMAKE_SOURCE_DIR}/website/tests/integration/test_browser.py"
            "${CMAKE_SOURCE_DIR}/website/tests/contract/test_generated_site_validation.py"
            "${CMAKE_SOURCE_DIR}/website/tests/support/static_fixture_server.py"
            "${CMAKE_SOURCE_DIR}/website/tests/support/site_features_checks.py"
            "${CMAKE_SOURCE_DIR}/website/tests/support/blog_checks.py"
            "${CMAKE_SOURCE_DIR}/website/tests/support/browser_test_launcher.cmake"
            "${CMAKE_SOURCE_DIR}/website/tests/fixtures/search_tokens_golden.json"
            ${_benchmark_browser_fixtures}
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
  COMMAND "${Python3_EXECUTABLE}" "${_source_dir}/tools/export_site.py"
          --source "${_output_root}/site"
          --destination "${CMAKE_SOURCE_DIR}/artifacts/website"
  DEPENDS "${_website_success}" bioseq_private_postgres_tests
          bioseq_website_isolation_tests)

# Authored content is checked in so the normal build needs neither TeX nor
# Pandoc. This optional check verifies the conversion against its original
# README, recursively included project sources and benchmark observations.
find_program(_website_node_executable NAMES node nodejs)
if(_website_node_executable)
  set(_website_benchmark_contract_command
    "${_website_node_executable}"
    "${_source_dir}/tests/contract/test_project_benchmarks.mjs")
  set(_website_search_contract_command
    "${_website_node_executable}" --test
    "${_source_dir}/tests/contract/test_search_ranking.mjs")
  set(_website_features_contract_command
    "${_website_node_executable}" --test
    "${_source_dir}/tests/contract/test_theme_runtime.mjs"
    "${_source_dir}/tests/contract/test_analytics_runtime.mjs")
else()
  set(_website_benchmark_contract_command
    "${CMAKE_COMMAND}" -E echo
    "website: Node.js unavailable - skipping the benchmark interaction contract")
  set(_website_search_contract_command
    "${CMAKE_COMMAND}" -E echo
    "website: Node.js unavailable - skipping the search ranking contract")
  set(_website_features_contract_command
    "${CMAKE_COMMAND}" -E echo
    "website: Node.js unavailable - skipping the theme and analytics contracts")
endif()
add_custom_target(bioseq_website_content_check
  COMMAND "${Python3_EXECUTABLE}" "${_source_dir}/tools/theme_palette.py" --check
  COMMAND "${Python3_EXECUTABLE}" "${_source_dir}/tests/contract/test_theme_palette.py"
  COMMAND "${Python3_EXECUTABLE}" "${_source_dir}/tools/import_readme_tutorial.py" --check
  COMMAND "${Python3_EXECUTABLE}" "${_source_dir}/tools/import_project_chapters.py" --check
  COMMAND "${Python3_EXECUTABLE}" "${_source_dir}/tests/contract/test_project_bibliography.py"
  COMMAND "${Python3_EXECUTABLE}" "${_source_dir}/tools/generate_project_benchmarks.py" --check
  COMMAND ${_website_benchmark_contract_command}
  COMMAND ${_website_search_contract_command}
  COMMAND ${_website_features_contract_command}
  COMMAND "${_venv_python}" "${_source_dir}/tests/contract/test_landing_content.py"
  DEPENDS "${_venv_stamp}" "${_source_dir}/tests/contract/test_theme_palette.py"
  WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  COMMENT "website: check imported author content and benchmark observations"
  VERBATIM)

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
