cmake_minimum_required(VERSION 3.25)

project(jinja2cpp VERSION 1.3.2 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

if(NOT JINJA2CPP_SOURCE_DIR)
  message(FATAL_ERROR "JINJA2CPP_SOURCE_DIR is required")
endif()

find_package(Boost REQUIRED COMPONENTS filesystem json regex)
find_package(expected-lite CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(optional-lite CONFIG REQUIRED)
find_package(string-view-lite CONFIG REQUIRED)
find_package(variant-lite CONFIG REQUIRED)

add_library(jinja2cpp
  "${JINJA2CPP_SOURCE_DIR}/src/binding/boost_json_serializer.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/error_info.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/expression_evaluator.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/expression_parser.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/filesystem_handler.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/filters.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/generic_list.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/internal_value.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/lexer.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/serialize_filters.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/statements.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/string_converter_filter.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/template.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/template_env.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/template_parser.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/testers.cpp"
  "${JINJA2CPP_SOURCE_DIR}/src/value.cpp"
)
set_target_properties(jinja2cpp PROPERTIES
  CXX_EXTENSIONS OFF
  CXX_STANDARD 17
  CXX_STANDARD_REQUIRED ON
  POSITION_INDEPENDENT_CODE ON
)
target_include_directories(jinja2cpp
  PUBLIC
    "$<BUILD_INTERFACE:${JINJA2CPP_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_compile_definitions(jinja2cpp
  PUBLIC
    BOOST_ERROR_CODE_HEADER_ONLY
    BOOST_SYSTEM_NO_DEPRECATED
    JINJA2CPP_WITH_JSON_BINDINGS_BOOST
    expected_CONFIG_SELECT_EXPECTED=expected_EXPECTED_NONSTD
    nssv_CONFIG_SELECT_STRING_VIEW=nssv_STRING_VIEW_NONSTD
    optional_CONFIG_SELECT_OPTIONAL=optional_OPTIONAL_NONSTD
    variant_CONFIG_SELECT_VARIANT=variant_VARIANT_NONSTD
)
target_link_libraries(jinja2cpp
  PUBLIC
    fmt::fmt
    nonstd::expected-lite
    nonstd::optional-lite
    nonstd::string-view-lite
    nonstd::variant-lite
  PRIVATE
    Boost::filesystem
    Boost::json
    Boost::regex
)

if(BUILD_SHARED_LIBS)
  target_compile_definitions(jinja2cpp
    PRIVATE JINJA2CPP_BUILD_AS_SHARED
    PUBLIC JINJA2CPP_LINK_AS_SHARED
  )
endif()

install(
  TARGETS jinja2cpp
  EXPORT jinja2cpp-targets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  DIRECTORY "${JINJA2CPP_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
  EXPORT jinja2cpp-targets
  FILE jinja2cpp-targets.cmake
  NAMESPACE jinja2cpp::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/jinja2cpp"
)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/jinja2cpp-config.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/jinja2cpp-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/jinja2cpp"
)
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/jinja2cpp-config-version.cmake"
  VERSION "${PROJECT_VERSION}"
  COMPATIBILITY SameMajorVersion
)
install(
  FILES
    "${CMAKE_CURRENT_BINARY_DIR}/jinja2cpp-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/jinja2cpp-config-version.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/jinja2cpp"
)
