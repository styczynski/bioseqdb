include_guard(GLOBAL)

function(bioseq_register_generated_api_client)
  cmake_parse_arguments(PARSE_ARGV 0 _client "" "CLIENT_NAME;CLIENT_ROOT" "")
  if(NOT _client_CLIENT_NAME OR NOT _client_CLIENT_ROOT)
    message(FATAL_ERROR
      "bioseq_register_generated_api_client requires CLIENT_NAME and CLIENT_ROOT")
  endif()
  if(NOT _client_CLIENT_NAME MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
    message(FATAL_ERROR
      "Invalid generated external API name '${_client_CLIENT_NAME}'")
  endif()

  set(_target "bioseq_external_api_${_client_CLIENT_NAME}")
  if(TARGET "${_target}")
    return()
  endif()

  set(_module "${_client_CLIENT_ROOT}/client/index.cppm")
  set(_test
    "${_client_CLIENT_ROOT}/tests/integration/${_client_CLIENT_NAME}_client_test.cpp")
  set_source_files_properties("${_module}" "${_test}" PROPERTIES GENERATED TRUE)

  if(NOT TARGET bioseq_external_api_integration_tests)
    if(NOT DEFINED BIOSEQ_EXTERNAL_TEST_RUNTIME_OUTPUT_DIRECTORY)
      set(BIOSEQ_EXTERNAL_TEST_RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    find_package(Catch2 3 REQUIRED)
    add_executable(bioseq_external_api_integration_tests)
    if(TARGET bioseq_external_codegen)
      add_dependencies(bioseq_external_api_integration_tests
        bioseq_external_codegen)
    endif()
    set_target_properties(bioseq_external_api_integration_tests PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS ON
      CXX_MODULE_STD ON
      INTERPROCEDURAL_OPTIMIZATION FALSE
      RUNTIME_OUTPUT_DIRECTORY
        "${BIOSEQ_EXTERNAL_TEST_RUNTIME_OUTPUT_DIRECTORY}")
    target_compile_definitions(bioseq_external_api_integration_tests PRIVATE
      NO_POSTGRES=1)
    target_link_libraries(bioseq_external_api_integration_tests PRIVATE
      Catch2::Catch2WithMain)
  endif()

  add_library("${_target}" OBJECT)
  if(TARGET bioseq_external_codegen)
    add_dependencies("${_target}" bioseq_external_codegen)
  endif()
  set_target_properties("${_target}" PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    POSITION_INDEPENDENT_CODE ON)
  target_compile_definitions("${_target}" PRIVATE NO_POSTGRES=1)
  target_sources("${_target}"
    PUBLIC FILE_SET CXX_MODULES
      BASE_DIRS "${_client_CLIENT_ROOT}/client"
      FILES "${_module}")
  target_link_libraries("${_target}" PUBLIC
    bioseq_runtime_core
    bioseq_external_https
    oatpp::oatpp
    OpenSSL::SSL
    OpenSSL::Crypto
    TBB::tbb)

  target_sources(bioseq_external_api_integration_tests PRIVATE "${_test}")
  target_link_libraries(bioseq_external_api_integration_tests PRIVATE "${_target}")
  set_property(
    TARGET bioseq_external_api_integration_tests
    APPEND PROPERTY BIOSEQ_GENERATED_CATCH_TEST_SOURCES "${_test}")

  get_property(
    _bioseq_external_ctest_registered
    GLOBAL PROPERTY BIOSEQ_EXTERNAL_API_CTEST_REGISTERED)
  if(BUILD_TESTING AND NOT _bioseq_external_ctest_registered)
    add_test(
      NAME bioseq_external_api_integration_tests
      COMMAND "$<TARGET_FILE:bioseq_external_api_integration_tests>"
        "[component:external_api][feature:external_source]"
        --reporter "console::out=-::colour-mode=ansi")
    set_tests_properties(
      bioseq_external_api_integration_tests
      PROPERTIES
        LABELS
          "component:external_api;layer:unit;layer:contract;layer:integration;feature:external_source;resource:filesystem;resource:network"
        TIMEOUT 1800)
    set_property(
      GLOBAL PROPERTY BIOSEQ_EXTERNAL_API_CTEST_REGISTERED TRUE)
  endif()
endfunction()
