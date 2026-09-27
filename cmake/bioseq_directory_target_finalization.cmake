# External API descriptors are build-time inputs. Keep their generated clients
# outside BIOSEQ_NOPG_MODULES so changing an OpenAPI schema does not rebuild the
# shared Domain-N module library. The dedicated build mode enables this option;
# normal configurations must not require the host's code-generation toolchain.
# BIOSEQ_BUILD_EXTERNAL_API_CLIENTS is declared at the repository root so the
# isolated generator can be provisioned once before add_subdirectory(bioseq).
if(BIOSEQ_BUILD_EXTERNAL_API_CLIENTS)
  set(BIOSEQ_EXTERNAL_GENERATED_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/generated/external")
  add_subdirectory(external)
  if(TARGET bioseq_external_api_integration_tests)
    add_dependencies(
      bioseq_external_api_integration_tests
      bioseq_sql_external_feature_contract)
  endif()
  if(TARGET bioseq_external_codegen)
    add_dependencies(
      bioseq_sql_prepare_final_production
      bioseq_external_codegen)
    add_dependencies(
      bioseq_sql_prepare_final_test_indexes
      bioseq_external_codegen)
  endif()

  # Link the compile-time provider pack into the extension so the external
  # CustomScan and row writer resolve bioseq.external.providers.run
  # and the provider model, and expose those module BMIs to the extension's
  # module compiles. BIOSEQ_EXTERNAL_PG gates the _PG_init registration and the
  # conditional import of the scan module in backend/pg/column/index.cppm.
  if(TARGET bioseq_external_provider_adapters)
    # The PG-free streaming library is the single compiled owner of the
    # transport/archive module providers. Linking that owner publishes its
    # module file set to CustomScan while retaining its private crypto edge.
    target_link_libraries(postgresql_bioseq PRIVATE
      bioseq_external_provider_adapters
      bioseq_external_io)
  endif()
endif()

# No executable or reusable library in this directory may acquire the
# scheduler-only owner objects. The single allowed consumer is the PostgreSQL
# DSO; the owner target itself is excluded from the source-leak check.
get_property(_bioseq_directory_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_bioseq_directory_target IN LISTS _bioseq_directory_targets)
  if(_bioseq_directory_target STREQUAL "postgresql_bioseq"
      OR _bioseq_directory_target STREQUAL
        "bioseq_pipeline_dispatch_owners")
    continue()
  endif()
  get_target_property(_bioseq_directory_target_links
    ${_bioseq_directory_target} LINK_LIBRARIES)
  foreach(_bioseq_directory_target_link IN LISTS
      _bioseq_directory_target_links)
    string(FIND "${_bioseq_directory_target_link}"
      "bioseq_pipeline_dispatch_owners"
      _bioseq_pipeline_dispatch_owner_link_offset)
    if(NOT _bioseq_pipeline_dispatch_owner_link_offset EQUAL -1)
      message(FATAL_ERROR
        "Scheduler-only pipeline owner target leaked into "
        "${_bioseq_directory_target}")
    endif()
  endforeach()
  get_target_property(_bioseq_directory_target_sources
    ${_bioseq_directory_target} SOURCES)
  foreach(_bioseq_pipeline_dispatch_owner IN LISTS
      BIOSEQ_PIPELINE_DISPATCH_OWNER_ALL_IMPLEMENTATION_UNITS)
    if(_bioseq_pipeline_dispatch_owner IN_LIST
        _bioseq_directory_target_sources)
      message(FATAL_ERROR
        "Scheduler-only pipeline owner source leaked into "
        "${_bioseq_directory_target}: ${_bioseq_pipeline_dispatch_owner}")
    endif()
  endforeach()
endforeach()
unset(_bioseq_pipeline_dispatch_owner)
unset(_bioseq_directory_target_sources)
unset(_bioseq_pipeline_dispatch_owner_link_offset)
unset(_bioseq_directory_target_link)
unset(_bioseq_directory_target_links)
unset(_bioseq_directory_target)
unset(_bioseq_directory_targets)
