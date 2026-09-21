set(BIOSEQ_TEST_TARGET_ARGUMENTS
  TYPE EXEC
  CXX 26
  RELOCATABLE
  COMMENT "BioSeq tests"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/backend/pg/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/backend"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/safestringlib/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/fmtlog"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/fmt"
    "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/seqan3/include"
  TESTS
    ${BIOSEQ_UNIT_TEST_SUPPORT_SOURCES}
    ${BIOSEQ_UNIT_TEST_SOURCES}
  INTEGRATION_TESTS
    ${BIOSEQ_INTEGRATION_TEST_SOURCES}
  INTEGRATION_TESTS_LINKS
    sqlgen::sqlgen
    bioseq_integration_test_support
    kseq++::kseq++
  LINKS
    safestring_static
    fmt
    ZLIB::ZLIB
    reflectcpp::reflectcpp
    libassert::assert
    foonathan_memory
    TBB::tbb
    Vulkan::Vulkan
    ${BIOSEQ_GLSLANG_STATIC_LIBS}
    spirv-cross-msl
    Vulkan::LayerSettings
    bioseq_pg_support
    bioseq_bench_prefilter_metrics
    bioseq_testing_support
  SHARED_MODULE_LIBS
    bioseq_modules_nopg
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}"
  BOOST
    program_options regex serialization iterator integer
  HWY 1.3.0
)

bioseq_add_postgresql_extension_variant(
  bioseq_unit_tests
  ON
  ${BIOSEQ_TEST_TARGET_ARGUMENTS})
set_target_properties(bioseq_unit_tests PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE)

bioseq_add_postgresql_extension_variant(
  bioseq_integration_tests
  INTEGRATION
  ${BIOSEQ_TEST_TARGET_ARGUMENTS})
set_target_properties(bioseq_integration_tests PROPERTIES
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_definitions(bioseq_integration_tests PRIVATE
  "BIOSEQ_IO_TEST_DATA_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../data\"")
unset(BIOSEQ_TEST_TARGET_ARGUMENTS)

if(BUILD_TESTING)
  add_test(
    NAME bioseq_unit_tests
    COMMAND "$<TARGET_FILE:bioseq_unit_tests>" "[component:bioseq]")
  set_tests_properties(bioseq_unit_tests PROPERTIES
    LABELS
      "component:bioseq;layer:unit;feature:alignment;feature:alphabet;feature:chaining;feature:configuration;feature:gpu_runtime;feature:index;feature:io;feature:minimizer;feature:pipeline;feature:planner;feature:prefilter;feature:scheduler;feature:seeding;feature:sequence;feature:simd_primitives;feature:sql_api;feature:storage;resource:filesystem;resource:gpu;resource:highway;resource:large_memory;resource:subprocess")
  add_test(
    NAME bioseq_integration_tests
    COMMAND "$<TARGET_FILE:bioseq_integration_tests>"
      "[component:bioseq][layer:integration]")
  set_tests_properties(bioseq_integration_tests PROPERTIES
    LABELS
      "component:bioseq;layer:integration;feature:alignment;feature:alphabet;feature:configuration;feature:extension_lifecycle;feature:index;feature:io;feature:pipeline;feature:planner;feature:prefilter;feature:scheduler;feature:sequence;feature:sql_api;feature:storage;resource:filesystem;resource:highway;resource:large_memory;resource:postgres;resource:subprocess"
    RESOURCE_LOCK "bioseq_private_postgres")
endif()

# Unit tests use one generator-proven fixed target, chosen from the compiler's
# HWY_STATIC_TARGET record. The test provider imports the same generated
# canonical partitions as production and never performs runtime dispatch.
if(TARGET bioseq_unit_tests)
  if(BIOSEQ_TEST_INDEXES)
    set_source_files_properties(
      ${BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS}
      PROPERTIES GENERATED TRUE COMPILE_OPTIONS "-x;c++")
    target_sources(bioseq_unit_tests PRIVATE
      ${BIOSEQ_HIGHWAY_GENERATED_UNIT_PROBE_IMPLEMENTATIONS})
  endif()
  target_link_libraries(bioseq_unit_tests PRIVATE
    bioseq_highway_test_support)
  add_dependencies(
    bioseq_unit_tests bioseq_highway_target_modules)
endif()
