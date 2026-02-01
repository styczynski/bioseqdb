#----------------------------------------------------------------
# Generated CMake target import file for configuration "Debug".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "reflectcpp::reflectcpp" for configuration "Debug"
set_property(TARGET reflectcpp::reflectcpp APPEND PROPERTY IMPORTED_CONFIGURATIONS DEBUG)
set_target_properties(reflectcpp::reflectcpp PROPERTIES
  IMPORTED_LINK_INTERFACE_LANGUAGES_DEBUG "CXX"
  IMPORTED_LOCATION_DEBUG "${_IMPORT_PREFIX}/debug/lib/libreflectcpp.a"
  )

list(APPEND _cmake_import_check_targets reflectcpp::reflectcpp )
list(APPEND _cmake_import_check_files_for_reflectcpp::reflectcpp "${_IMPORT_PREFIX}/debug/lib/libreflectcpp.a" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
