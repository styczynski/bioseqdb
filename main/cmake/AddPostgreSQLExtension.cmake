set(SUPPORTED_TARGET_TYPES
  "POSTGRES_EXTENSION"
  "EXEC"
  "TEST_EXEC"
)

# Allow forcing PostgreSQL version
set(POSTGRESQL_VERSION $ENV{POSTGRESQL_VERSION} CACHE STRING "PostgreSQL version major")
option(BIOSEQ_ENABLE_PGINDENT_FORMAT_TARGETS
  "Create explicit pgindent format targets for PostgreSQL extension sources."
  OFF)

# Find packages go here.
#
# Note: do NOT pass `PATHS` here. With CMake's full-signature semantics,
# supplying PATHS forces find_package into CONFIG-only mode, which bypasses
# our custom MODULE-mode FindPostgreSQL.cmake (under cmake/) and instead hits
# the vcpkg-provided libpq cmake-wrapper that has no Config file. The custom
# Find module already discovers Postgres via pg_config and honours
# PostgreSQL_ROOT / hints below for non-system install locations
# (e.g. Postgres.app, Homebrew's postgresql@N).
if(DEFINED VCPKG_TARGET_TRIPLET)
  find_package(OpenSSL REQUIRED)
endif()
find_package(PostgreSQL ${POSTGRESQL_VERSION} REQUIRED)

# Helper command to add extensions
function(AddCommonTarget LIBRARY_NAME ENABLE_TEST_TARGET)
  message("%%%% PREPARSE CXX_MODULES FOR '${LIBRARY_NAME}' is <${EXTENSION_CXX_MOD_FILES}>")
  set(options
    RELOCATABLE
    SUPPORT_CXX_MODULES
    REQUIRE_COMPLETE_CODEGEN)
  set(oneValueArgs
    NAME TYPE COMMENT VERSION COMPONENT CXX HWY MAIN SCHEMA
    GENERATED_INSTALL SQL_MANIFEST)
  # SHARED_MODULE_LIBS: object/static libraries that export FILE_SET
  # CXX_MODULES PUBLIC. Linking them into the consumer makes the BMIs they
  # produced visible without rebuilding the module sources for every target —
  # see main/DOCS/IMPL_PLANS/BUILD_TIME_REDUCTION_..._DOC0269.md §5.
  set(multiValueArgs REQUIRES BOOST SOURCES INSTALLS UPDATES CXX_MOD_BASE_DIRS CXX_MOD_FILES INCLUDE_DIRS LINKS TESTS_MAIN TESTS INTEGRATION_TESTS INTEGRATION_TESTS_LINKS INTEGRATION_TESTS_MODULES SHARED_MODULE_LIBS PG_ENTRYPOINT_SOURCES)
  cmake_parse_arguments(EXTENSION "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  message("%%%% POSTPARSE CXX_MODULES FOR '${LIBRARY_NAME}' is <${EXTENSION_CXX_MOD_FILES}> <${EXTENSION_TESTS}> <${EXTENSION_INTEGRATION_TESTS}>")

  set(EXTENSION_X_INTERNAL_TESTS ${EXTENSION_TESTS})
  set(EXTENSION_X_INTERNAL_INT_TESTS ${EXTENSION_INTEGRATION_TESTS})

  if("${ENABLE_TEST_TARGET}" STREQUAL "OFF" OR "${ENABLE_TEST_TARGET}" STREQUAL "NEVER")
    unset(EXTENSION_X_INTERNAL_TESTS)
  endif()

  if("${ENABLE_TEST_TARGET}" STREQUAL "ON" OR "${ENABLE_TEST_TARGET}" STREQUAL "INTEGRATION")
    if(EXTENSION_TYPE STREQUAL "EXEC")
      if(NOT EXTENSION_MAIN)
        message("You need to specify MAIN argument (name of the source file containing main function) when you use TESTS for TYPE EXEC")
      endif()
    endif()
    set(EXTENSION_TYPE "EXEC")
    if("${ENABLE_TEST_TARGET}" STREQUAL "ON")
      list(APPEND EXTENSION_SOURCES ${EXTENSION_X_INTERNAL_TESTS})
    endif()
    if("${ENABLE_TEST_TARGET}" STREQUAL "INTEGRATION")
      set(EXTENSION_SOURCES ${EXTENSION_X_INTERNAL_INT_TESTS})
      message("HUJEMUJE => ${EXTENSION_X_INTERNAL_INT_TESTS}")
    endif()

    message("Source exclude '${EXTENSION_MAIN}' from '${EXTENSION_SOURCES}'")
    list(FILTER EXTENSION_SOURCES EXCLUDE REGEX ${EXTENSION_MAIN})

    if(EXTENSION_TESTS_MAIN)
      if("${ENABLE_TEST_TARGET}" STREQUAL "ON")
        list(APPEND EXTENSION_SOURCES ${EXTENSION_TESTS_MAIN})
      endif()
    endif()
  endif()
    # else()
  #   if(EXTENSION_TYPE STREQUAL "EXEC")
  #   list(APPEND EXTENSION_SOURCES ${EXTENSION_MAIN})
  #   endif()
  # endif()

  if(NOT EXTENSION_TYPE)
    message(FATAL_ERROR "TYPE setting not given to the AddCommonTarget() function. Must be one of ${SUPPORTED_TARGET_TYPES}")
  endif()

  if (NOT "${EXTENSION_TYPE}" IN_LIST SUPPORTED_TARGET_TYPES)
    message(FATAL_ERROR "TYPE setting was '${EXTENSION_TYPE}'. Must be one of ${SUPPORTED_TARGET_TYPES}")
  else()
    set("IS_${EXTENSION_TYPE}" 1)
  endif()

  if(IS_TEST_EXEC)
    set(IS_EXEC 1)
  endif()

  set(HAS_SOURCES 1)

  # Default extension name to same as library name
  if(NOT EXTENSION_NAME)
    set(EXTENSION_NAME ${LIBRARY_NAME})
  endif()

  # Override tests target name
  if("${ENABLE_TEST_TARGET}" STREQUAL "ON" OR "${ENABLE_TEST_TARGET}" STREQUAL "INTEGRATION")
    set(EXTENSION_NAME ${LIBRARY_NAME})
  endif()

  message("Adding target '${EXTENSION_NAME}' (library name: '${LIBRARY_NAME}') (${EXTENSION_X_INTERNAL_TESTS}) <${EXTENSION_SOURCES}>")

  # Executable target
  if(IS_EXEC)
    add_executable(${LIBRARY_NAME} ${EXTENSION_SOURCES})
  endif()

  # Generic setup postgres
  if(IS_POSTGRES_EXTENSION)
    # Add pg_regress binary
    find_program(PostgreSQL_REGRESS pg_regress
      HINTS
        "${PostgreSQL_PKG_LIBRARY_DIR}/pgxs/src/test/regress/"
        "${PostgreSQL_BIN_DIR}"
    )

    # Add pg_validate_extupgrade binary
    find_program(PostgreSQL_VALIDATE_EXTUPGRADE pg_validate_extupgrade)

    # Add pgindent binary
    find_program(PostgreSQL_INDENT pgindent)

    if(HAS_SOURCES)
      # Add extension as a dynamically linked library
      add_library(${LIBRARY_NAME} MODULE ${EXTENSION_SOURCES})

      # A PostgreSQL server extension on ELF platforms must not link libpq.
      # Its backend API symbols are resolved by the postmaster when the module
      # is loaded.  Linking a static client library here can pull frontend
      # pgcommon objects into the DSO and leave symbols such as
      # pg_log_generic unresolved at server startup.  Apple/Windows retain the
      # platform-specific import-library behavior below.
      if(APPLE OR WIN32)
        target_link_libraries(${LIBRARY_NAME} PRIVATE PostgreSQL::PostgreSQL)
      endif()

      # Handle macOS specifics
      if(APPLE)
        # Fix apple missing symbols
        set_target_properties(${LIBRARY_NAME} PROPERTIES LINK_FLAGS ${PostgreSQL_LINK_FLAGS})

        # Since Postgres 16, the shared library extension on macOS is `dylib`, not `so`.
        # Ref https://github.com/postgres/postgres/commit/b55f62abb2c2e07dfae99e19a2b3d7ca9e58dc1a
        if (${PostgreSQL_VERSION_MAJOR} VERSION_GREATER_EQUAL "16")
          set_target_properties (${LIBRARY_NAME} PROPERTIES SUFFIX ".dylib")
        endif()
      endif()
    endif()
  else()
    if("${ENABLE_TEST_TARGET}" STREQUAL "INTEGRATION")
      target_include_directories(${LIBRARY_NAME} PRIVATE ${PostgreSQL_INCLUDE_DIRS})
      target_link_libraries(${LIBRARY_NAME} PRIVATE PostgreSQL::PostgreSQL)
      target_compile_definitions(${LIBRARY_NAME} PUBLIC -DNO_POSTGRES=1)
    else()
      target_compile_definitions(${LIBRARY_NAME} PUBLIC -DNO_POSTGRES=1)
    endif()
  endif() # End postgress setup

  # Debug
  if(APPLE)
    add_custom_command(
      TARGET ${LIBRARY_NAME}
      POST_BUILD
      COMMAND dsymutil $<TARGET_FILE:${LIBRARY_NAME}>
    )
  endif()

  # Allow extensions without sources
  if(HAS_SOURCES)
    target_compile_definitions(${LIBRARY_NAME} PUBLIC -DPOSTGRESQL_VERSION_MAJOR=${PostgreSQL_VERSION_MAJOR})

    # Final touches on output file
    set_target_properties(${LIBRARY_NAME} PROPERTIES
      OUTPUT_NAME ${EXTENSION_NAME}
      INTERPROCEDURAL_OPTIMIZATION TRUE
      #C_VISIBILITY_PRESET hidden # @TODO: how to get this working?
      PREFIX "" # Avoid lib* prefix on output file
    )

    # Support CXX
    if(EXTENSION_CXX)
      message("Target ${LIBRARY_NAME} support C++ version ${EXTENSION_CXX}")
      set_source_files_properties(${EXTENSION_SOURCES} PROPERTIES LANGUAGE CXX )
      set_property(TARGET ${LIBRARY_NAME} PROPERTY CXX_STANDARD ${EXTENSION_CXX})
    endif()

    # Include dirs
    if(EXTENSION_INCLUDE_DIRS)
      target_include_directories(
        ${LIBRARY_NAME} PRIVATE ${EXTENSION_INCLUDE_DIRS}
      )
    endif()

    # Tests
    if(NOT "${ENABLE_TEST_TARGET}" STREQUAL "NEVER")
      find_package(Catch2 3 REQUIRED)
      target_link_libraries(${LIBRARY_NAME} PRIVATE Catch2::Catch2WithMain)
      target_compile_definitions(${LIBRARY_NAME} PUBLIC CATCH_CONFIG_CONSOLE_WIDTH=1024 CATCH_CONFIG_DEFAULT_REPORTER=console CATCH_CONFIG_EXPERIMENTAL_REDIRECT=1)
    endif()

    if(IS_TEST_EXEC)
      find_package(Catch2 3 REQUIRED)
      target_link_libraries(${LIBRARY_NAME} PRIVATE Catch2::Catch2)
      target_compile_definitions(${LIBRARY_NAME} PUBLIC CATCH_CONFIG_CONSOLE_WIDTH=1024 CATCH_CONFIG_DEFAULT_REPORTER=console CATCH_CONFIG_EXPERIMENTAL_REDIRECT=1)
      target_compile_definitions(${LIBRARY_NAME} PUBLIC -DNO_POSTGRES=1)
    endif()

    # Boost
    if(EXTENSION_BOOST)
      find_package(Boost 1.90.0 REQUIRED COMPONENTS ${EXTENSION_BOOST})
      target_include_directories(
        ${LIBRARY_NAME} PRIVATE ${Boost_INCLUDE_DIRS}
      )
      target_link_libraries(${LIBRARY_NAME} PRIVATE ${Boost_LIBRARIES})
    endif()

    # Google highway
    if(EXTENSION_HWY)
      find_package(HWY ${EXTENSION_HWY} REQUIRED NO_MODULE)
      target_link_libraries(${LIBRARY_NAME} PRIVATE hwy::hwy)
      target_include_directories(
        ${LIBRARY_NAME} PRIVATE ${HWY_INCLUDE_DIRS}
      )
      # target_link_libraries(${LIBRARY_NAME} PRIVATE hwy)
      # target_include_directories(
      #   ${LIBRARY_NAME} PRIVATE $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
      # )
    endif()

    # Link libraries
    if(EXTENSION_LINKS)
      target_link_libraries(${LIBRARY_NAME} PRIVATE ${EXTENSION_LINKS})
    endif()

    # Cross-target BMI sharing (DOC0269 §5). The libraries listed here own a
    # FILE_SET CXX_MODULES whose BMI files are reused across all consumers
    # instead of being rebuilt once per target. The objects from OBJECT/STATIC
    # libraries also flow into the link line, so we only link them on the
    # consumer once.
    if(EXTENSION_SHARED_MODULE_LIBS)
      target_link_libraries(${LIBRARY_NAME} PRIVATE ${EXTENSION_SHARED_MODULE_LIBS})
    endif()

    if(EXTENSION_INTEGRATION_TESTS_LINKS)
      if("${ENABLE_TEST_TARGET}" STREQUAL "INTEGRATION")
        target_link_libraries(${LIBRARY_NAME} PRIVATE ${EXTENSION_INTEGRATION_TESTS_LINKS})
      endif()
    endif()

    if(EXTENSION_INTEGRATION_TESTS_MODULES)
      if("${ENABLE_TEST_TARGET}" STREQUAL "INTEGRATION")
        set(EXTENSION_CXX_MOD_FILES ${EXTENSION_INTEGRATION_TESTS_MODULES})
      endif()
    endif()

    # Suport CXX modules
    if(EXTENSION_SUPPORT_CXX_MODULES)
      if(EXTENSION_CXX)
        # Modules enables
        if(EXTENSION_CXX_MOD_BASE_DIRS)
          message("Enable CXX modules for the target ${LIBRARY_NAME}")
          set(PG_EXT_FILE_SET_NAME "${LIBRARY_NAME}_CXX_MODULES_FILE_SET")
          message("%%%% CHECKING CXX_MODULES FOR '${LIBRARY_NAME}' is <${EXTENSION_CXX_MOD_FILES}>")
          target_sources(
            ${LIBRARY_NAME} PRIVATE
            FILE_SET "${PG_EXT_FILE_SET_NAME}"
            TYPE CXX_MODULES
            BASE_DIRS ${EXTENSION_CXX_MOD_BASE_DIRS}
            FILES ${EXTENSION_CXX_MOD_FILES}
          )
          # This is required to write your own modules in your project.
          if(CMAKE_VERSION VERSION_LESS "3.28.0")
            if(CMAKE_VERSION VERSION_LESS "3.27.0")
              set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API "2182bf5c-ef0d-489a-91da-49dbc3090d2a")
            else()
              set(CMAKE_EXPERIMENTAL_CXX_MODULE_CMAKE_API "aa1f7df0-828a-4fcd-9afc-2dc80491aca7")
            endif()
            set(CMAKE_EXPERIMENTAL_CXX_MODULE_DYNDEP 1)
          else()
            cmake_policy(VERSION 3.28)
          endif()

          set_target_properties(
            ${LIBRARY_NAME} PROPERTIES
              CXX_MODULE_STD ON
          )

        else()
          message("Cannot enable CXX modules, because either CXX_MOD_BASE_DIRS or CXX_MOD_FILES variables are missing. Please specify base dirs and module files list.")
        endif()
      else()
        message("Cannot enable CXX modules, because EXTENSION_CXX setting is not provided. Please specify it like so: 'EXTENSION_CXX 20'")
      endif()
    endif()
  endif()

  if(IS_POSTGRES_EXTENSION)

    # Install .so/.dll to pkglib-dir
    if(HAS_SOURCES)
      install(
        TARGETS ${LIBRARY_NAME}
        LIBRARY DESTINATION "${PostgreSQL_PKG_LIBRARY_DIR}"
        COMPONENT ${EXTENSION_COMPONENT}
      )
    endif()
    # Generate .control file. Independent extensions omit `requires` entirely;
    # fixed-schema extensions emit the control property explicitly.
    if(EXTENSION_REQUIRES)
      string(REPLACE ";" ", " EXTENSION_REQUIRES "${EXTENSION_REQUIRES}")
      set(EXTENSION_REQUIRES_CONTROL_LINE
        "requires = '${EXTENSION_REQUIRES}'")
    else()
      set(EXTENSION_REQUIRES_CONTROL_LINE "")
    endif()
    if(EXTENSION_SCHEMA)
      set(EXTENSION_SCHEMA_CONTROL_LINE
        "schema = '${EXTENSION_SCHEMA}'")
    else()
      set(EXTENSION_SCHEMA_CONTROL_LINE "")
    endif()
    configure_file(
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/control.in"
      "${EXTENSION_NAME}.control"
    )

    # Select the extension install script authority. Authored fragments retain
    # the legacy configure-time concatenation path. Code-generated extensions
    # publish one build-time artifact directly; CMake must never copy the
    # checked source golden into a second package-authoritative file.
    if(EXTENSION_GENERATED_INSTALL)
      if(EXTENSION_INSTALLS)
        message(FATAL_ERROR
          "${LIBRARY_NAME}: GENERATED_INSTALL and INSTALLS are mutually exclusive")
      endif()
      if(NOT IS_ABSOLUTE "${EXTENSION_GENERATED_INSTALL}")
        message(FATAL_ERROR
          "${LIBRARY_NAME}: GENERATED_INSTALL must be an absolute build-tree path")
      endif()
      get_filename_component(
        EXTENSION_GENERATED_INSTALL_NAME
        "${EXTENSION_GENERATED_INSTALL}" NAME)
      if(NOT EXTENSION_GENERATED_INSTALL_NAME STREQUAL
          "${EXTENSION_NAME}--${EXTENSION_VERSION}.sql")
        message(FATAL_ERROR
          "${LIBRARY_NAME}: generated install script must be named "
          "${EXTENSION_NAME}--${EXTENSION_VERSION}.sql, got "
          "${EXTENSION_GENERATED_INSTALL_NAME}")
      endif()
      set(EXTENSION_INSTALL "${EXTENSION_GENERATED_INSTALL}")
      set_source_files_properties(
        "${EXTENSION_INSTALL}" PROPERTIES GENERATED TRUE)
    else()
      set(EXTENSION_INSTALL
        ${CMAKE_CURRENT_BINARY_DIR}/${EXTENSION_NAME}--${EXTENSION_VERSION}.sql)
      file(WRITE "${EXTENSION_INSTALL}.in" "")
      foreach(file ${EXTENSION_INSTALLS})
        file(READ ${file} CONTENTS)
        file(APPEND "${EXTENSION_INSTALL}.in" "${CONTENTS}")
      endforeach()
      configure_file("${EXTENSION_INSTALL}.in" "${EXTENSION_INSTALL}" COPYONLY)
    endif()

    if(EXTENSION_SQL_MANIFEST)
      if(NOT IS_ABSOLUTE "${EXTENSION_SQL_MANIFEST}")
        message(FATAL_ERROR
          "${LIBRARY_NAME}: SQL_MANIFEST must be an absolute build-tree path")
      endif()
      set_source_files_properties(
        "${EXTENSION_SQL_MANIFEST}" PROPERTIES GENERATED TRUE)

      # Validate the real linked DSO against the generated fmgr ownership
      # manifest. Making the manifest and validator link dependencies ensures
      # an explicit build of the extension relinks and runs POST_BUILD when
      # either input changes; a failed validation therefore fails the actual
      # extension target rather than an optional sibling target.
      set(_extension_dso_validator
        "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../codegen/safety/validate_evidence.py")
      if(NOT EXISTS "${_extension_dso_validator}")
        message(FATAL_ERROR
          "${LIBRARY_NAME}: missing DSO validator ${_extension_dso_validator}")
      endif()
      find_package(Python3 REQUIRED COMPONENTS Interpreter)
      set(_extension_dso_report
        "${CMAKE_CURRENT_BINARY_DIR}/generated/sql/${EXTENSION_NAME}-dso-closure.json")
      set(_extension_dso_completion_args)
      if(EXTENSION_REQUIRE_COMPLETE_CODEGEN)
        list(APPEND _extension_dso_completion_args
          --require-complete-codegen)
      endif()
      set(_extension_safety_validator_args --dso-only)
      set(_extension_safety_dependencies)
      set(_extension_safety_validation_description "fmgr ownership")
      set_property(TARGET ${LIBRARY_NAME} APPEND PROPERTY LINK_DEPENDS
        "${EXTENSION_SQL_MANIFEST}"
        "${_extension_dso_validator}"
        ${_extension_safety_dependencies})
      add_custom_command(
        TARGET ${LIBRARY_NAME}
        POST_BUILD
        BYPRODUCTS "${_extension_dso_report}"
        COMMAND "${Python3_EXECUTABLE}"
          "${_extension_dso_validator}"
          ${_extension_safety_validator_args}
          --sql-manifest "${EXTENSION_SQL_MANIFEST}"
          --dso "$<TARGET_FILE:${LIBRARY_NAME}>"
          ${_extension_dso_completion_args}
          --report "${_extension_dso_report}"
        COMMENT
          "Validating ${EXTENSION_NAME} ${_extension_safety_validation_description} against the linked DSO"
        VERBATIM)
      add_custom_target("${LIBRARY_NAME}_dso_closure"
        COMMAND "${Python3_EXECUTABLE}"
          "${_extension_dso_validator}"
          ${_extension_safety_validator_args}
          --sql-manifest "${EXTENSION_SQL_MANIFEST}"
          --dso "$<TARGET_FILE:${LIBRARY_NAME}>"
          ${_extension_dso_completion_args}
          --report "${_extension_dso_report}"
        DEPENDS
          ${LIBRARY_NAME}
          "${EXTENSION_SQL_MANIFEST}"
          "${_extension_dso_validator}"
          ${_extension_safety_dependencies}
        COMMENT
          "Revalidating ${EXTENSION_NAME} ${_extension_safety_validation_description} report"
        VERBATIM)
      set_target_properties(${LIBRARY_NAME} PROPERTIES
        BIOSEQ_EXTENSION_DSO_CLOSURE_REPORT
          "${_extension_dso_report}")
    endif()

    # Install everything else into share-dir
    install(
      FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${EXTENSION_NAME}.control
        ${EXTENSION_INSTALL}
        ${EXTENSION_UPDATES}
      DESTINATION "${PostgreSQL_SHARE_DIR}/extension"
      COMPONENT ${EXTENSION_COMPONENT}
    )

    # Publish the generated control/install-SQL/DSO artifacts as generic
    # read-only target properties (§13.2) so the website relocatable-extension
    # stage copies the *current build target's* files rather than reading a
    # globally installed Bioseq. The library filename stays a genex so consumers
    # resolve the real DSO name (bioseq.so) when they expand $<TARGET_PROPERTY>.
    if(HAS_SOURCES)
      set_target_properties(${LIBRARY_NAME} PROPERTIES
        BIOSEQ_EXTENSION_CONTROL_FILE
          "${CMAKE_CURRENT_BINARY_DIR}/${EXTENSION_NAME}.control"
        BIOSEQ_EXTENSION_INSTALL_SQL
          "${EXTENSION_INSTALL}"
        BIOSEQ_EXTENSION_SQL_MANIFEST
          "${EXTENSION_SQL_MANIFEST}"
        BIOSEQ_EXTENSION_LIBRARY_FILENAME
          "$<TARGET_FILE_NAME:${LIBRARY_NAME}>"
        BIOSEQ_EXTENSION_PG_ENTRYPOINT_SOURCES
          "${EXTENSION_PG_ENTRYPOINT_SOURCES}")
    endif()

    # Formatting is intentionally opt-in. pgindent may need local typedefs
    # configuration, and build targets must not mutate or reformat sources.
    if(BIOSEQ_ENABLE_PGINDENT_FORMAT_TARGETS AND PostgreSQL_INDENT)
      add_custom_target("format_${EXTENSION_NAME}"
        COMMAND ${PostgreSQL_INDENT} ${EXTENSION_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Formatting ${EXTENSION_NAME} sources"
      )
    endif()
  endif()

  message("Added target ${EXTENSION_NAME}")
endfunction()

function(AddCommonTargetX LIBRARY_NAME)
  set(SUBARGS ${ARGN})
  set(options)
  set(oneValueArgs)
  set(multiValueArgs TESTS INTEGRATION_TESTS)
  cmake_parse_arguments(EXTENSION "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})
  message("TESTEESTSTSTS ${EXTENSION_TESTS}")
  if(EXTENSION_TESTS OR EXTENSION_INTEGRATION_TESTS)
   if(EXTENSION_TESTS)
    AddCommonTarget("${LIBRARY_NAME}_tests_exec" "ON" ${SUBARGS})
    # clang bug: ThinLTO drops TLS init wrappers (_ZTHW…) for module-attached
    # thread_local template static data members. Triggers on mlogging's
    # fmtlogDetailT<0>::sbc when linking the test exec. Disable LTO here only.
    set_target_properties("${LIBRARY_NAME}_tests_exec" PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
    add_custom_target(
      "test_${LIBRARY_NAME}"
      COMMAND "./${LIBRARY_NAME}_tests_exec" "--reporter" "console::out=-::colour-mode=ansi"
      DEPENDS "${LIBRARY_NAME}_tests_exec"
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
   endif()
   message("CHECK INT => '${EXTENSION_INTEGRATION_TESTS}'")
   if(EXTENSION_INTEGRATION_TESTS)
    message("add int target tests")
    AddCommonTarget("${LIBRARY_NAME}_int_tests_exec" "INTEGRATION" ${SUBARGS})
    # clang bug: ThinLTO drops TLS init wrappers (_ZTHW…) for module-attached
    # thread_local template static data members. Triggers on mlogging's
    # fmtlogDetailT<0>::sbc when linking the test exec. Disable LTO here only.
    set_target_properties("${LIBRARY_NAME}_int_tests_exec" PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
    add_custom_target(
      "integration_test_${LIBRARY_NAME}"
      COMMAND "./${LIBRARY_NAME}_int_tests_exec" "--reporter" "console::out=-::colour-mode=ansi"
      DEPENDS "${LIBRARY_NAME}_int_tests_exec"
      WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    )
   endif()
    AddCommonTarget("${LIBRARY_NAME}" "OFF" ${SUBARGS})
  else()
    AddCommonTarget("${LIBRARY_NAME}" "NEVER" ${SUBARGS})
  endif()
endfunction()
