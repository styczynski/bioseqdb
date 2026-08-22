bioseq_add_postgresql_extension(
  postgresql_bioseq
  TYPE POSTGRES_EXTENSION
  CXX 26
  RELOCATABLE
  NAME bioseq
  COMMENT "BioSeqDB PG Extension"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/backend"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/safestringlib/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/fmtlog"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/fmt"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/seqan3/include"
  LINKS
    safestring_static
    fmt
    ZLIB::ZLIB
    reflectcpp::reflectcpp
    libassert::assert
    foonathan_memory
    TBB::tbb
    ${BIOSEQ_GLSLANG_STATIC_LIBS}
    spirv-cross-msl
    Vulkan::LayerSettings
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
    bioseq_sql_compile_contract_providers
  SOURCES
    ${SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${BIOSEQ_DISPATCH_PROCESSOR_TYPED_GENERATED_ROOT}"
  CXX_MOD_FILES
    ${SRC_MOD_FILES}
  GENERATED_INSTALL
    "${BIOSEQ_SQL_GENERATED_INSTALL}"
  SQL_MANIFEST
    "${BIOSEQ_SQL_GENERATED_MANIFEST}"
  DSO_CLOSURE_CONTRACT manifest
  DSO_ABI_EXPORTS
    Pg_magic_func
    _PG_init
    bioseq_scheduler_supervisor_main
    bioseq_hash_major_parallel_scan_probe_worker_main
  ${_bioseq_sql_completion_target_option}
  PG_ENTRYPOINT_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/extension/lifecycle.cppm"
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)
# These scheduler-owned pipeline entries are intentionally absent from the
# unit/integration executable variants created by the helper above.  Link the
# dedicated OBJECT target only into the production PostgreSQL DSO; placing it
# in SHARED_MODULE_LIBS would propagate its objects to every helper variant
# and restore the cross-target COMDAT/interposition hazard this boundary
# removes.
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_build_config
  bioseq_pipeline_dispatch_owners)
get_target_property(_bioseq_postgresql_direct_links
  postgresql_bioseq LINK_LIBRARIES)
set(_bioseq_pipeline_dispatch_owner_link_occurrences 0)
foreach(_bioseq_postgresql_direct_link IN LISTS
    _bioseq_postgresql_direct_links)
  if(_bioseq_postgresql_direct_link STREQUAL
      "bioseq_pipeline_dispatch_owners")
    math(EXPR _bioseq_pipeline_dispatch_owner_link_occurrences
      "${_bioseq_pipeline_dispatch_owner_link_occurrences} + 1")
  endif()
endforeach()
if(NOT _bioseq_pipeline_dispatch_owner_link_occurrences EQUAL 1)
  message(FATAL_ERROR
    "postgresql_bioseq must link the pipeline owner OBJECT target exactly "
    "once; found ${_bioseq_pipeline_dispatch_owner_link_occurrences}")
endif()
get_target_property(_bioseq_postgresql_attached_sources
  postgresql_bioseq SOURCES)
foreach(_bioseq_pipeline_dispatch_owner IN LISTS
    BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
  if(_bioseq_pipeline_dispatch_owner IN_LIST SRC_FILES
      OR _bioseq_pipeline_dispatch_owner IN_LIST SRC_MOD_FILES
      OR _bioseq_pipeline_dispatch_owner IN_LIST
        BIOSEQ_POSTGRES_ORDINARY_CPPM_UNITS
      OR _bioseq_pipeline_dispatch_owner IN_LIST BIOSEQ_NOPG_SOURCES
      OR _bioseq_pipeline_dispatch_owner IN_LIST
        BIOSEQ_NOPG_CPPM_IMPLEMENTATION_UNITS
      OR _bioseq_pipeline_dispatch_owner IN_LIST BIOSEQ_NOPG_OWNED_MODULES
      OR _bioseq_pipeline_dispatch_owner IN_LIST
        _bioseq_postgresql_attached_sources)
    message(FATAL_ERROR
      "Pipeline owner escaped its dedicated OBJECT target: "
      "${_bioseq_pipeline_dispatch_owner}")
  endif()
endforeach()
unset(_bioseq_pipeline_dispatch_owner)
unset(_bioseq_postgresql_attached_sources)
unset(_bioseq_pipeline_dispatch_owner_link_occurrences)
unset(_bioseq_postgresql_direct_link)
unset(_bioseq_postgresql_direct_links)
add_dependencies(postgresql_bioseq
  "${BIOSEQ_TYPED_GENERATION_TARGET}")

# Generated target primaries are the only exported providers. Their private
# binder implementation units and cold inventory are ordinary private sources.
# Algorithm partitions and the dispatch-processor registry are named module
# partition providers, so CMake must own them through a CXX_MODULES file set.
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PRIMARY_MODULES}
  ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS}
  ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PARTITIONS}
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY}"
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PROPERTIES GENERATED TRUE)
set_source_files_properties(
  ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS}
  "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PROPERTIES COMPILE_OPTIONS "-x;c++")
target_sources(postgresql_bioseq
  PRIVATE
    ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_ORDINARY_IMPLEMENTATIONS}
    "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_INVENTORY}"
  PRIVATE FILE_SET bioseq_highway_generated_primaries
    TYPE CXX_MODULES
    BASE_DIRS "${BIOSEQ_HIGHWAY_GENERATED_ROOT}"
    FILES
      ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PRIMARY_MODULES}
      ${BIOSEQ_HIGHWAY_GENERATED_PRODUCTION_PARTITIONS}
      "${BIOSEQ_HIGHWAY_GENERATED_DISPATCH_REGISTRY}")
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_highway_fixed_target)
target_link_libraries(bioseq_modules_nopg PRIVATE
  bioseq_highway_target_policy)
add_dependencies(postgresql_bioseq bioseq_highway_target_modules)

set_source_files_properties(
  ${BIOSEQ_SQL_ENTITY_HANDLE_MODULES}
  ${BIOSEQ_SQL_PRODUCTION_ROW_CODEC_MODULES}
  ${BIOSEQ_SQL_TEST_INDEXES_ROW_CODEC_MODULES}
  ${BIOSEQ_SQL_PRODUCTION_FMGR_ENTRYPOINT_MODULES}
  ${BIOSEQ_SQL_TEST_INDEXES_FMGR_ENTRYPOINT_MODULES}
  ${BIOSEQ_SQL_GUC_GENERATED_MODULES}
  PROPERTIES GENERATED TRUE)
target_sources(postgresql_bioseq
  PRIVATE
    FILE_SET bioseq_sql_generated_row_codecs
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/generated"
    FILES ${BIOSEQ_SQL_POSTGRES_ROW_CODEC_MODULES})
target_sources(postgresql_bioseq
  PRIVATE
    FILE_SET bioseq_sql_generated_fmgr_entrypoints
    TYPE CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_BINARY_DIR}/generated"
    FILES ${BIOSEQ_SQL_FMGR_ENTRYPOINT_MODULES})
target_compile_options(postgresql_bioseq PRIVATE
  "$<$<CXX_COMPILER_ID:GNU>:-Wno-attributes=bioseq_sql::>"
  "$<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wno-unknown-attributes>")
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  target_compile_definitions(
    postgresql_bioseq PRIVATE BIOSEQ_EXTERNAL_PG=1)
  target_compile_definitions(
    bioseq_pipeline_dispatch_owners PRIVATE BIOSEQ_EXTERNAL_PG=1)
endif()
# The Bioseq DSO always owns the provider half of the cache rendezvous ABI and
# the small PG lifecycle helpers, even when the sibling cache DSO is disabled.
target_link_libraries(postgresql_bioseq PRIVATE
  bioseq_cache_provider_api
  bioseq_pg_support
  bioseq_pg_sql_boundary
  bioseq_sql_annotation_model)

# Link validation consumes the generator-owned scan/modmap closure before the
# DSO link. A concrete link dependency expresses that ordering without making
# every source scan wait on its own validation.
set(BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP
  "${BIOSEQ_SQL_CODEGEN_DIR}/postgresql-bioseq-link-validation.stamp")
add_custom_command(
  OUTPUT "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory
    "${BIOSEQ_SQL_CODEGEN_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch
    "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}"
  DEPENDS
    bioseq_sql_generation_contract
    bioseq_sql_gcc_compile_contracts
  COMMENT
    "Validating SQL generation and module boundaries before linking bioseq"
  VERBATIM)
add_custom_target(bioseq_sql_postgresql_link_proof
  DEPENDS "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}")
set_property(TARGET postgresql_bioseq APPEND PROPERTY LINK_DEPENDS
  "${BIOSEQ_SQL_POSTGRES_EXTENSION_LINK_VALIDATION_STAMP}")

# Keep only compile-readiness dependencies target-wide. Validation targets belong
# to the link gate above; attaching them here would re-enter CXX.dd through the
# extension's .ddi object-order dependency.
add_dependencies(postgresql_bioseq bioseq_codegen_project)
add_dependencies(postgresql_bioseq bioseq_sql_annotation_probe)

if(TARGET bioseq_gpu_archive_builder)
  add_dependencies(postgresql_bioseq bioseq_gpu_archive_builder)
endif()

# Disable LTO on the PostgreSQL extension. ThinLTO repeats optimization across
# the dispatch-buffer template fan-out and can drop module-attached TLS-init
# wrappers. bioseq_modules_nopg uses the same non-LTO policy, so the extension
# retains consistent linkage and bounded template-instantiation work.
set_target_properties(postgresql_bioseq PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE
# The PostgreSQL extension must not link-load libvulkan in the
  # postmaster, because the macOS loader stack maps Metal/IOAccelerator before
  # backend fork. Keep the vcpkg rpath only so Runtime::init() can later
  # dlopen("@rpath/libvulkan...") inside the selected-GPU backend process.
  INSTALL_RPATH "${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/lib;${CMAKE_BINARY_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/debug/lib")

# Throttle the per-target heavy module compiles on low-RAM hosts. The
# postgresql_bioseq target owns the Domain P module fan-out — the modules
# that import the columnar storage / metadata / table-io stack and feel
# the most BMI-load + template-instantiation pressure (utils.cppm,
# access_method.cppm, custom_scan.cppm, aggregator_node.cppm, …). When
# BIOSEQ_HEAVY_JOBS is set the assignment caps these
# compiles against the same Ninja pool used by bioseq_modules_nopg, so
# the worst-case concurrent peak RSS is `heavy_compile × per-TU peak`
# instead of `min(--parallel, ncpu) × per-TU peak`. Same Ninja-generator
# caveat as the lib's annotation above; pool is unset (no-op) on other
# generators or when the user has not opted in.
if(BIOSEQ_HEAVY_JOBS AND BIOSEQ_HEAVY_JOBS GREATER 0)
  set_target_properties(postgresql_bioseq PROPERTIES
    JOB_POOL_COMPILE heavy_compile)
  if(TARGET bioseq_unit_tests)
    set_target_properties(bioseq_unit_tests PROPERTIES
      JOB_POOL_COMPILE heavy_compile)
  endif()
  if(TARGET bioseq_integration_tests)
    set_target_properties(bioseq_integration_tests PROPERTIES
      JOB_POOL_COMPILE heavy_compile)
  endif()
endif()

# Compile the test-only indexing descriptors
# (TestTableCountIndex, a CHUNKED_PER_TABLE index) into the server-loaded
# extension dylib AND the integration/unit test execs. Default OFF (production);
# the integration CI preset sets -DBIOSEQ_TEST_INDEXES=ON so the
# [indexing][per-table] test and the corruption-hook tests run.
if(BIOSEQ_TEST_INDEXES)
  target_compile_definitions(postgresql_bioseq PRIVATE BIOSEQ_TEST_INDEXES=1)
  target_compile_definitions(
    bioseq_pipeline_dispatch_owners PRIVATE BIOSEQ_TEST_INDEXES=1)
  target_compile_definitions(bioseq_modules_nopg PRIVATE BIOSEQ_TEST_INDEXES=1)
  target_compile_definitions(bioseq_testing_support PRIVATE BIOSEQ_TEST_INDEXES=1)
  if(TARGET bioseq_integration_tests)
    target_compile_definitions(bioseq_integration_tests PRIVATE BIOSEQ_TEST_INDEXES=1)
  endif()
  if(TARGET bioseq_unit_tests)
    target_compile_definitions(bioseq_unit_tests PRIVATE BIOSEQ_TEST_INDEXES=1)
  endif()
endif()

if(TARGET bioseq_integration_tests)
  add_dependencies(
    bioseq_integration_tests
    postgresql_bioseq)
endif()
