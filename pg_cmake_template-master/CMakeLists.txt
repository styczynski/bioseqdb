# Apache 2 Licensed
# Code modified from:
# TimescaleDB (https://www.timescale.com/)

cmake_minimum_required(VERSION 3.4)

set(PROJECT_NAME demopgextension)
set(PROEJCT_VERSION "0.0.1")
project(${PROJECT_NAME} VERSION ${VERSION} LANGUAGES C)

if (NOT CMAKE_BUILD_TYPE)
    # Default to Release builds
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build, options are: None Debug Release RelWithDebInfo MinSizeRel" FORCE)
endif ()

message(STATUS "Build type is ${CMAKE_BUILD_TYPE}")

# Search paths for Postgres binaries
if (WIN32)
    find_path(PG_PATH
            bin/postgres
            HINTS
            "C:/PostgreSQL"
            "C:/Program Files/PostgreSQL"
            PATH_SUFFIXES
            bin
            10/bin
            96/bin
            pg96/bin
            DOC
            "The path to a PostgreSQL installation")
endif (WIN32)

if (UNIX)
    find_path(PG_PATH
            bin/postgres
            HINTS
            $ENV{HOME}
            /opt/local/pgsql
            /usr/local/pgsql
            /usr/lib/postgresql
            PATH_SUFFIXES
            bin
            10/bin
            9.6/bin
            96/bin
            pg96/bin
            DOC
            "The path to a PostgreSQL installation")
endif (UNIX)

find_program(PG_CONFIG pg_config
        HINTS
        ${PG_PATH}
        PATH_SUFFIXES
        bin
        DOC
        "The path to the pg_config of the PostgreSQL version to compile against"
        REQUIRED)

if (NOT PG_CONFIG)
    message(FATAL_ERROR "Unable to find 'pg_config'")
endif ()

find_package(Git)

message(STATUS "Using pg_config ${PG_CONFIG}")

# Check PostgreSQL version
execute_process(
        COMMAND ${PG_CONFIG} --version
        OUTPUT_VARIABLE PG_VERSION_STRING
        OUTPUT_STRIP_TRAILING_WHITESPACE)

if (NOT ${PG_VERSION_STRING} MATCHES "^PostgreSQL[ ]+([0-9]+)\\.([0-9]+)(\\.([0-9]+))*$")
    message(FATAL_ERROR "Could not parse PostgreSQL version ${PG_VERSION}")
endif ()

set(PG_VERSION_MAJOR ${CMAKE_MATCH_1})
set(PG_VERSION_MINOR ${CMAKE_MATCH_2})
set(PG_VERSION_PATCH ${CMAKE_MATCH_4})

if (NOT ${PG_VERSION_PATCH} OR ${PG_VERSION_PATCH} EQUAL "")
    set(PG_VERSION "${PG_VERSION_MAJOR}.${PG_VERSION_MINOR}")
else ()
    set(PG_VERSION "${PG_VERSION_MAJOR}.${PG_VERSION_MINOR}.${PG_VERSION_PATCH}")
endif ()

message(STATUS "Compiling against PostgreSQL version ${PG_VERSION}")

if ((${PG_VERSION} VERSION_LESS "9.6")
        OR (${PG_VERSION} VERSION_EQUAL "11")
        OR (${PG_VERSION} VERSION_GREATER "11"))
endif ()

# Get PostgreSQL configuration from pg_config
execute_process(
        COMMAND ${PG_CONFIG} --includedir
        OUTPUT_VARIABLE PG_INCLUDEDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --includedir-server
        OUTPUT_VARIABLE PG_INCLUDEDIR_SERVER
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --libdir
        OUTPUT_VARIABLE PG_LIBDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --pkglibdir
        OUTPUT_VARIABLE PG_PKGLIBDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --sharedir
        OUTPUT_VARIABLE PG_SHAREDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --bindir
        OUTPUT_VARIABLE PG_BINDIR
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --cppflags
        OUTPUT_VARIABLE PG_CPPFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --cflags
        OUTPUT_VARIABLE PG_CFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --ldflags
        OUTPUT_VARIABLE PG_LDFLAGS
        OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(
        COMMAND ${PG_CONFIG} --libs
        OUTPUT_VARIABLE PG_LIBS
        OUTPUT_STRIP_TRAILING_WHITESPACE)

find_path(PG_SOURCE_DIR
        src/include/pg_config.h.in
        HINTS
        $ENV{HOME}
        $ENV{HOME}/projects
        $ENV{HOME}/Projects
        $ENV{HOME}/development
        $ENV{HOME}/Development
        PATH_SUFFIXES
        postgres
        postgresql
        pgsql
        DOC
        "The path to the PostgreSQL source tree")

if (PG_SOURCE_DIR)
    message(STATUS "Found PostgreSQL source in ${PG_SOURCE_DIR}")
endif (PG_SOURCE_DIR)

add_subdirectory(sql)
add_subdirectory(src)

set(EXT_CONTROL_FILE ${PROJECT_NAME}.control)
configure_file(${EXT_CONTROL_FILE} ${EXT_CONTROL_FILE})

install(
        FILES ${CMAKE_CURRENT_BINARY_DIR}/${EXT_CONTROL_FILE}
        DESTINATION "${PG_SHAREDIR}/extension")
