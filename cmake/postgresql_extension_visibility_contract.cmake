include_guard(GLOBAL)

# Configure-time contract for the Darwin PostgreSQL extension exact export
# allowlist.  The post-link DSO validator remains authoritative for the
# resulting binary; this catches producer/target wiring drift before linking.
function(
    bioseq_assert_postgresql_extension_macho_visibility_contract
    TARGET_NAME SYMBOL_FILE PRODUCER_TARGET EXPECTED_LINK_OPTION)
  if(NOT TARGET "${TARGET_NAME}")
    message(FATAL_ERROR
      "Mach-O visibility contract target does not exist: ${TARGET_NAME}")
  endif()

  get_target_property(_bioseq_target_type "${TARGET_NAME}" TYPE)
  if(NOT _bioseq_target_type STREQUAL "MODULE_LIBRARY")
    message(FATAL_ERROR
      "Mach-O visibility contract requires a MODULE_LIBRARY target: "
      "${TARGET_NAME} is ${_bioseq_target_type}")
  endif()

  if(NOT TARGET "${PRODUCER_TARGET}")
    message(FATAL_ERROR
      "Mach-O visibility contract producer target does not exist: "
      "${PRODUCER_TARGET}")
  endif()

  get_target_property(
    _bioseq_configured_symbol_file
    "${TARGET_NAME}"
    BIOSEQ_EXTENSION_DSO_MACHO_EXPORT_LIST)
  if(NOT _bioseq_configured_symbol_file STREQUAL "${SYMBOL_FILE}")
    message(FATAL_ERROR
      "Mach-O visibility contract target metadata does not name its "
      "generated exact export list")
  endif()
  get_target_property(
    _bioseq_configured_producer
    "${TARGET_NAME}"
    BIOSEQ_EXTENSION_DSO_MACHO_EXPORT_LIST_TARGET)
  if(NOT _bioseq_configured_producer STREQUAL "${PRODUCER_TARGET}")
    message(FATAL_ERROR
      "Mach-O visibility contract target metadata does not name its "
      "export-list producer")
  endif()

  get_target_property(_bioseq_link_options "${TARGET_NAME}" LINK_OPTIONS)
  if(NOT _bioseq_link_options)
    message(FATAL_ERROR
      "Mach-O visibility contract target has no linker options")
  endif()
  list(FIND
    _bioseq_link_options "${EXPECTED_LINK_OPTION}" _bioseq_link_option_index)
  if(_bioseq_link_option_index EQUAL -1)
    message(FATAL_ERROR
      "Mach-O visibility contract target does not consume its exact export "
      "list")
  endif()

  get_target_property(_bioseq_link_depends "${TARGET_NAME}" LINK_DEPENDS)
  if(NOT _bioseq_link_depends)
    message(FATAL_ERROR
      "Mach-O visibility contract target has no link dependencies")
  endif()
  list(FIND
    _bioseq_link_depends "${SYMBOL_FILE}" _bioseq_link_dependency_index)
  if(_bioseq_link_dependency_index EQUAL -1)
    message(FATAL_ERROR
      "Mach-O visibility contract export list is not a link dependency")
  endif()

  get_target_property(
    _bioseq_manual_dependencies
    "${TARGET_NAME}"
    MANUALLY_ADDED_DEPENDENCIES)
  if(NOT _bioseq_manual_dependencies)
    message(FATAL_ERROR
      "Mach-O visibility contract target has no producer dependency")
  endif()
  list(FIND
    _bioseq_manual_dependencies
    "${PRODUCER_TARGET}"
    _bioseq_producer_dependency_index)
  if(_bioseq_producer_dependency_index EQUAL -1)
    message(FATAL_ERROR
      "Mach-O visibility contract target does not depend on its export-list "
      "producer")
  endif()
endfunction()
