include_guard(GLOBAL)

# Configure-time contract for the Darwin PostgreSQL extension export filter.
# The post-link DSO validator remains authoritative for the resulting binary;
# this catches target wiring or generated-pattern drift before the linker runs.
function(
    bioseq_assert_postgresql_extension_macho_visibility_contract
    TARGET_NAME SYMBOL_FILE EXPECTED_LINK_OPTION)
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

  if(NOT EXISTS "${SYMBOL_FILE}")
    message(FATAL_ERROR
      "Mach-O visibility contract symbol list is missing: ${SYMBOL_FILE}")
  endif()
  file(READ "${SYMBOL_FILE}" _bioseq_symbol_file_content)
  if(NOT _bioseq_symbol_file_content STREQUAL "__ZGIW*\n")
    message(FATAL_ERROR
      "Mach-O visibility contract must hide exactly the C++ named-module "
      "initializer family")
  endif()

  get_target_property(
    _bioseq_configured_symbol_file
    "${TARGET_NAME}"
    BIOSEQ_EXTENSION_DSO_UNEXPORTED_SYMBOLS)
  if(NOT _bioseq_configured_symbol_file STREQUAL "${SYMBOL_FILE}")
    message(FATAL_ERROR
      "Mach-O visibility contract target metadata does not name its "
      "generated symbol list")
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
      "Mach-O visibility contract target does not consume its unexported "
      "symbols list")
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
      "Mach-O visibility contract symbol list is not a link dependency")
  endif()
endfunction()
