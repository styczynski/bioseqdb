# Render the reviewed PostgreSQL extension ABI as a Mach-O exported-symbols
# list while reusing the strict manifest inventory validation shared with ELF.
set(BIOSEQ_POSTGRESQL_EXTENSION_EXPORT_FORMAT MACHO)
include(
  "${CMAKE_CURRENT_LIST_DIR}/GeneratePostgreSQLExtensionElfVersionScript.cmake")
