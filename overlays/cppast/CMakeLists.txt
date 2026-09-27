cmake_minimum_required(VERSION 3.25)

project(cppast VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(NOT CPPAST_SOURCE_DIR)
  message(FATAL_ERROR "CPPAST_SOURCE_DIR is required")
endif()
if(NOT CPPAST_LIBCLANG_INCLUDE_DIR)
  message(FATAL_ERROR "CPPAST_LIBCLANG_INCLUDE_DIR is required")
endif()
if(NOT CPPAST_LIBCLANG_VERSION)
  message(FATAL_ERROR "CPPAST_LIBCLANG_VERSION is required")
endif()
if(NOT EXISTS "${CPPAST_LIBCLANG_INCLUDE_DIR}/clang-c/Index.h")
  message(FATAL_ERROR
    "clang-c/Index.h is absent from CPPAST_LIBCLANG_INCLUDE_DIR")
endif()

find_package(Threads REQUIRED)
find_package(type_safe CONFIG REQUIRED)

add_library(cppast STATIC
  "${CPPAST_SOURCE_DIR}/external/tpl/process.cpp"
  "${CPPAST_SOURCE_DIR}/src/code_generator.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_alias_template.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_attribute.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_class.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_class_template.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_concept.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_entity.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_entity_index.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_entity_kind.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_enum.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_expression.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_file.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_forward_declarable.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_friend.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_function.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_function_template.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_language_linkage.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_member_function.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_member_variable.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_namespace.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_preprocessor.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_static_assert.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_template_parameter.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_token.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_type.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_type_alias.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_variable.cpp"
  "${CPPAST_SOURCE_DIR}/src/cpp_variable_template.cpp"
  "${CPPAST_SOURCE_DIR}/src/diagnostic_logger.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/class_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/concept_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/cxtokenizer.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/debug_helper.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/enum_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/expression_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/friend_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/function_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/language_linkage_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/libclang_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/namespace_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/parse_functions.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/preprocessor.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/template_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/type_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/libclang/variable_parser.cpp"
  "${CPPAST_SOURCE_DIR}/src/visitor.cpp"
)
if(WIN32)
  target_sources(cppast PRIVATE
    "${CPPAST_SOURCE_DIR}/external/tpl/process_win.cpp")
else()
  target_sources(cppast PRIVATE
    "${CPPAST_SOURCE_DIR}/external/tpl/process_unix.cpp")
endif()

set_target_properties(cppast PROPERTIES
  CXX_EXTENSIONS ON
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(cppast
  PUBLIC
    "$<BUILD_INTERFACE:${CPPAST_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
  PRIVATE
    "${CPPAST_LIBCLANG_INCLUDE_DIR}"
    "${CPPAST_SOURCE_DIR}/external/tpl"
    "${CPPAST_SOURCE_DIR}/src"
)
target_compile_definitions(cppast
  PUBLIC
    "CPPAST_VERSION_MAJOR=\"${PROJECT_VERSION_MAJOR}\""
    "CPPAST_VERSION_MINOR=\"${PROJECT_VERSION_MINOR}\""
    "CPPAST_VERSION_STRING=\"${PROJECT_VERSION}\""
  PRIVATE
    "CPPAST_CLANG_BINARY=\"clang\""
    "CPPAST_CLANG_VERSION_STRING=\"${CPPAST_LIBCLANG_VERSION}\""
)
target_link_libraries(cppast
  PUBLIC type_safe
  PRIVATE Threads::Threads
)

install(
  TARGETS cppast
  EXPORT cppast-targets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
  DIRECTORY "${CPPAST_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
  EXPORT cppast-targets
  FILE cppast-targets.cmake
  NAMESPACE cppast::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cppast"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cppast-config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/cppast-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cppast"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/cppast-config-version.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/cppast-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/cppast-config-version.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/cppast"
)
