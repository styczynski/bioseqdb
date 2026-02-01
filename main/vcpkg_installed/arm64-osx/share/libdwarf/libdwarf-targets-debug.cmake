#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "libdwarf::dwarf" for configuration "Debug"
set_property(TARGET libdwarf::dwarf APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(libdwarf::dwarf PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "C"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/lib/libdwarf.a"
  )

list(APPEND _cmake_import_check_targets libdwarf::dwarf )
list(APPEND _cmake_import_check_files_for_libdwarf::dwarf "${_IMPORT_PREFIX}/debug/lib/libdwarf.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
