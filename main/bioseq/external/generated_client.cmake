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
    "${_client_CLIENT_ROOT}/integration_tests/test_${_client_CLIENT_NAME}_client.cpp")
  set_source_files_properties("${_module}" "${_test}" PROPERTIES GENERATED TRUE)

  if(NOT TARGET bioseq_external_integration_tests_exec)
    if(NOT DEFINED BIOSEQ_EXTERNAL_TEST_RUNTIME_OUTPUT_DIRECTORY)
      set(BIOSEQ_EXTERNAL_TEST_RUNTIME_OUTPUT_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    find_package(Catch2 3 REQUIRED)
    add_executable(bioseq_external_integration_tests_exec)
    if(TARGET bioseq_external_codegen)
      add_dependencies(bioseq_external_integration_tests_exec
        bioseq_external_codegen)
    endif()
    set_target_properties(bioseq_external_integration_tests_exec PROPERTIES
      CXX_STANDARD 26
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS ON
      CXX_MODULE_STD ON
      INTERPROCEDURAL_OPTIMIZATION FALSE
      RUNTIME_OUTPUT_DIRECTORY
        "${BIOSEQ_EXTERNAL_TEST_RUNTIME_OUTPUT_DIRECTORY}")
    target_compile_definitions(bioseq_external_integration_tests_exec PRIVATE
      NO_POSTGRES=1)
    target_link_libraries(bioseq_external_integration_tests_exec PRIVATE
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

  target_sources(bioseq_external_integration_tests_exec PRIVATE "${_test}")
  target_link_libraries(bioseq_external_integration_tests_exec PRIVATE "${_target}")

  if(BUILD_TESTING)
    set(_test_name "bioseq_external_${_client_CLIENT_NAME}_integration")
    add_test(
      NAME "${_test_name}"
      COMMAND "$<TARGET_FILE:bioseq_external_integration_tests_exec>"
        "[external][${_client_CLIENT_NAME}][integration]"
        --reporter "console::out=-::colour-mode=ansi")
    set_tests_properties("${_test_name}" PROPERTIES
      LABELS "external;${_client_CLIENT_NAME};integration"
      WORKING_DIRECTORY "${_client_CLIENT_ROOT}")
  endif()
endfunction()
