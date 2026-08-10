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

# Helper command to add extensions
function(PostgreSQL_add_extension LIBRARY_NAME)
  set(options RELOCATABLE SUPPORT_CXX_MODULES)
  set(oneValueArgs NAME COMMENT VERSION COMPONENT CXX)
  set(multiValueArgs REQUIRES SOURCES INSTALLS UPDATES CXX_MOD_BASE_DIRS CXX_MOD_FILES INCLUDE_DIRS LINKS)
  cmake_parse_arguments(EXTENSION "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

  # Default extension name to same as library name
  if(NOT EXTENSION_NAME)
    set(EXTENSION_NAME ${LIBRARY_NAME})
  endif()

  # Allow extensions without sources
  if(EXTENSION_SOURCES)
    # Add extension as a dynamically linked library
    add_library(${LIBRARY_NAME} MODULE ${EXTENSION_SOURCES})

    # Link extension to PostgreSQL
    target_link_libraries(${LIBRARY_NAME} PRIVATE PostgreSQL::PostgreSQL)

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

    # Final touches on output file
    set_target_properties(${LIBRARY_NAME} PROPERTIES
      OUTPUT_NAME ${EXTENSION_NAME}
      INTERPROCEDURAL_OPTIMIZATION TRUE
      #C_VISIBILITY_PRESET hidden # @TODO: how to get this working?
      PREFIX "" # Avoid lib* prefix on output file
    )

    # Support CXX
    if(EXTENSION_CXX)
      message("PostgreSQL extension support C++ version ${EXTENSION_CXX}")
      set_source_files_properties(${EXTENSION_SOURCES} PROPERTIES LANGUAGE CXX )
      set_property(TARGET ${LIBRARY_NAME} PROPERTY CXX_STANDARD ${EXTENSION_CXX})
    endif()

    # Include dirs
    if(EXTENSION_INCLUDE_DIRS)
      target_include_directories(
        ${LIBRARY_NAME} PRIVATE ${EXTENSION_INCLUDE_DIRS}
      )
    endif()

    # Link libraries
    if(EXTENSION_LINKS)
      target_link_libraries(${LIBRARY_NAME} PRIVATE ${EXTENSION_LINKS})
    endif()

    # Suport CXX modules
    if(EXTENSION_SUPPORT_CXX_MODULES)
      if(EXTENSION_CXX)
        # Modules enables
        if(EXTENSION_CXX_MOD_BASE_DIRS)
          message("Enable CXX modules for the target PostgreSQL extension")
          set(PG_EXT_FILE_SET_NAME "${LIBRARY_NAME}_CXX_MODULES_FILE_SET")
          target_sources(
            ${LIBRARY_NAME} PRIVATE
            FILE_SET "${PG_EXT_FILE_SET_NAME}"
            TYPE CXX_MODULES
            BASE_DIRS ${EXTENSION_CXX_MOD_BASE_DIRS}
            FILES ${EXTENSION_CXX_MOD_FILES}
          )

          #
          #
          # Enable modules in CMake
          #

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

          #
          # Import the modules from libc++
          #
          # set(CMAKE_CXX_EXTENSIONS OFF)
          # include(FetchContent)
          # FetchContent_Declare(
          #   std
          #   URL "file://${LIBCXX_BUILD}/modules/c++/v1/"
          #   DOWNLOAD_EXTRACT_TIMESTAMP TRUE
          #   SYSTEM
          # )
          # FetchContent_MakeAvailable(std)
          # add_dependencies(${LIBRARY_NAME} std.compat)
          # target_link_libraries(${LIBRARY_NAME} std.compat)
          # set(CMAKE_CXX_EXTENSIONS ON)
          #set(CMAKE_CXX_MODULE_STD ON)
          
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

    # Install .so/.dll to pkglib-dir
    install(
      TARGETS ${LIBRARY_NAME}
      LIBRARY DESTINATION "${PostgreSQL_PKG_LIBRARY_DIR}"
      COMPONENT ${EXTENSION_COMPONENT}
    )
  endif()

  # Generate .control file
  string(REPLACE ";" ", " EXTENSION_REQUIRES "${EXTENSION_REQUIRES}")
  configure_file(
    ${CMAKE_SOURCE_DIR}/cmake/control.in
    ${EXTENSION_NAME}.control
  )

  # Generate .sql install file
  set(EXTENSION_INSTALL ${CMAKE_CURRENT_BINARY_DIR}/${EXTENSION_NAME}--${EXTENSION_VERSION}.sql)
  file(WRITE "${EXTENSION_INSTALL}.in" "")
  foreach(file ${EXTENSION_INSTALLS})
    file(READ ${file} CONTENTS)
    file(APPEND "${EXTENSION_INSTALL}.in" "${CONTENTS}")
  endforeach()
  configure_file("${EXTENSION_INSTALL}.in" "${EXTENSION_INSTALL}" COPYONLY)

  # Install everything else into share-dir
  install(
    FILES
      ${CMAKE_CURRENT_BINARY_DIR}/${EXTENSION_NAME}.control
      ${EXTENSION_INSTALL}
      ${EXTENSION_UPDATES}
    DESTINATION "${PostgreSQL_SHARE_DIR}/extension"
    COMPONENT ${EXTENSION_COMPONENT}
  )

  # Setup auto-format
  if(PostgreSQL_INDENT)
    add_custom_target("format_${EXTENSION_NAME}"
      COMMAND ${PostgreSQL_INDENT} ${EXTENSION_SOURCES}
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      COMMENT "Formatting ${EXTENSION_NAME} sources"
    )
    add_dependencies(${LIBRARY_NAME} "format_${EXTENSION_NAME}")
  endif()
endfunction()
