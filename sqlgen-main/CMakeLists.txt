cmake_minimum_required(VERSION 3.23)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(SQLGEN_BUILD_SHARED "Build shared library" ${BUILD_SHARED_LIBS})

option(SQLGEN_DUCKDB "Enable DuckDB support" OFF) 

option(SQLGEN_MYSQL "Enable MySQL support" OFF) 

option(SQLGEN_POSTGRES "Enable PostgreSQL support" ON) # enabled by default

option(SQLGEN_SQLITE3 "Enable SQLite3 support" ON) # enabled by default

option(SQLGEN_BUILD_TESTS "Build tests" OFF)

option(SQLGEN_BUILD_DRY_TESTS_ONLY "Build 'dry' tests only (those that do not require a database connection)" OFF)

option(SQLGEN_CHECK_HEADERS "Make sure that all headers are self-contained" OFF)

set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 20)
endif()

set(SQLGEN_USE_VCPKG_DEFAULT ON)

option(SQLGEN_USE_VCPKG "Use VCPKG to download and build dependencies" ${SQLGEN_USE_VCPKG_DEFAULT})

if (SQLGEN_USE_VCPKG)
    if (SQLGEN_BUILD_TESTS)
        list(APPEND VCPKG_MANIFEST_FEATURES "tests")
    endif()
    
    if (SQLGEN_DUCKDB OR SQLGEN_CHECK_HEADERS)
        list(APPEND VCPKG_MANIFEST_FEATURES "duckdb")
    endif()
    
    if (SQLGEN_MYSQL OR SQLGEN_CHECK_HEADERS)
        list(APPEND VCPKG_MANIFEST_FEATURES "mysql")
    endif()
    
    if (SQLGEN_POSTGRES OR SQLGEN_CHECK_HEADERS)
        list(APPEND VCPKG_MANIFEST_FEATURES "postgres")
    endif()
    
    if (SQLGEN_SQLITE3 OR SQLGEN_CHECK_HEADERS)
        list(APPEND VCPKG_MANIFEST_FEATURES "sqlite3")
    endif()

    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/vcpkg/scripts/buildsystems/vcpkg.cmake CACHE STRING "Vcpkg toolchain file")
endif ()

project(sqlgen VERSION 0.6.0 LANGUAGES CXX)

if (SQLGEN_BUILD_SHARED)
    add_library(sqlgen SHARED)
    set_target_properties(sqlgen PROPERTIES SOVERSION ${PROJECT_VERSION})
else()
    add_library(sqlgen STATIC)
endif()

add_library(sqlgen::sqlgen ALIAS sqlgen)

if (MSVC)
    target_compile_options(sqlgen PRIVATE 
        $<$<CONFIG:Debug>:
            -Wall
        >
        $<$<CONFIG:Release>:
            -DNDEBUG
        >
    )
else()
    target_compile_options(sqlgen PRIVATE
        $<$<CONFIG:Debug>:
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion
        >
        $<$<CONFIG:Release>:
            -DNDEBUG
        >
    )
endif()

if (SQLGEN_BUILD_SHARED)
    target_compile_definitions(sqlgen PUBLIC SQLGEN_BUILD_SHARED)
endif()

set(SQLGEN_SOURCES
    src/sqlgen.cpp
)

target_include_directories(
    sqlgen PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)


if (SQLGEN_DUCKDB OR SQLGEN_CHECK_HEADERS)
    list(APPEND SQLGEN_SOURCES src/sqlgen_duckdb.cpp)
    if (NOT TARGET DuckDB)
        find_package(DuckDB REQUIRED)
    endif()
    target_link_libraries(sqlgen PUBLIC $<IF:$<TARGET_EXISTS:duckdb>,duckdb,duckdb_static>)
endif()


if(SQLGEN_MYSQL OR SQLGEN_CHECK_HEADERS)
    list(APPEND SQLGEN_SOURCES src/sqlgen_mysql.cpp)
    if (SQLGEN_USE_VCPKG)
      if (NOT TARGET unofficial-libmariadb)
        find_package(unofficial-libmariadb CONFIG REQUIRED)
      endif()
      target_link_libraries(sqlgen PUBLIC unofficial::libmariadb)
    else()
      if (NOT TARGET mariadb-connector-c)
        find_package(mariadb-connector-c REQUIRED)
      endif()
      target_link_libraries(sqlgen PUBLIC mariadb-connector-c::mariadb-connector-c)
    endif()
endif()

if (SQLGEN_POSTGRES OR SQLGEN_CHECK_HEADERS)
    list(APPEND SQLGEN_SOURCES src/sqlgen_postgres.cpp)
    if (NOT TARGET PostgreSQL)
        find_package(PostgreSQL REQUIRED)
    endif()
    target_link_libraries(sqlgen PUBLIC PostgreSQL::PostgreSQL)
endif()

if (SQLGEN_SQLITE3 OR SQLGEN_CHECK_HEADERS)
    list(APPEND SQLGEN_SOURCES src/sqlgen_sqlite.cpp)

    if (SQLGEN_USE_VCPKG)
        if (NOT TARGET unofficial-sqlite3)
            find_package(unofficial-sqlite3 CONFIG REQUIRED)
        endif()
        target_link_libraries(sqlgen PUBLIC unofficial::sqlite3::sqlite3)
    else()
        if (NOT TARGET unofficial-sqlite3)
            find_package(SQLite3 CONFIG REQUIRED)
        endif()
        target_link_libraries(sqlgen PUBLIC SQLite::SQLite3)
    endif()
endif()

find_package(reflectcpp CONFIG REQUIRED)

target_link_libraries(sqlgen PUBLIC reflectcpp::reflectcpp)

set_target_properties(sqlgen PROPERTIES LINKER_LANGUAGE CXX)
target_sources(sqlgen PRIVATE ${SQLGEN_SOURCES})

if (SQLGEN_BUILD_TESTS)
    add_library(sqlgen_tests_crt INTERFACE)
    target_link_libraries(sqlgen_tests_crt INTERFACE sqlgen GTest::gtest_main)

    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    add_subdirectory(tests)
endif ()

if(SQLGEN_CHECK_HEADERS)
    file(GLOB_RECURSE PROJECT_HEADERS "include/*.hpp")
    find_package(reflectcpp CONFIG REQUIRED)

    set(TEST_DIR "${CMAKE_CURRENT_BINARY_DIR}/self_contained_tests")
    file(MAKE_DIRECTORY "${TEST_DIR}")

    foreach(HEADER_FILE ${PROJECT_HEADERS})
        string(MAKE_C_IDENTIFIER ${HEADER_FILE} HEADER_NAME)
        set(TEST_SOURCE_FILE "${TEST_DIR}/test_${HEADER_NAME}.cpp")

        file(GENERATE
            OUTPUT ${TEST_SOURCE_FILE}
            CONTENT "#include \"${HEADER_FILE}\"\n"
        )

        add_library(check_header_${HEADER_NAME} "${TEST_SOURCE_FILE}")
        
        target_include_directories(check_header_${HEADER_NAME} PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
            ${ADDITIONAL_INCLUDE_DIRS}
        )
        target_link_libraries(check_header_${HEADER_NAME} PUBLIC reflectcpp::reflectcpp)
        target_link_libraries(check_header_${HEADER_NAME} PUBLIC unofficial::libmariadb)
        target_link_libraries(check_header_${HEADER_NAME} PUBLIC PostgreSQL::PostgreSQL)
        target_link_libraries(check_header_${HEADER_NAME} PUBLIC unofficial::sqlite3::sqlite3)
        
        add_custom_target(check_${HEADER_NAME} ALL DEPENDS check_header_${HEADER_NAME})
    endforeach()
endif()

if (PROJECT_IS_TOP_LEVEL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)

    configure_package_config_file(sqlgen-config.cmake.in
      ${CMAKE_CURRENT_BINARY_DIR}/sqlgen-config.cmake
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sqlgen
      )

    install(
        FILES "${CMAKE_CURRENT_BINARY_DIR}/sqlgen-config.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/sqlgen"
    )

    file(GLOB_RECURSE SQLGEN_HEADERS RELATIVE ${CMAKE_CURRENT_LIST_DIR} "${CMAKE_CURRENT_LIST_DIR}/include/*" )

    target_sources(sqlgen
        PUBLIC
        FILE_SET sqlgen_headers
        TYPE HEADERS
        BASE_DIRS $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/include>
        FILES ${SQLGEN_HEADERS})

    install(
        TARGETS sqlgen
        EXPORT sqlgen-exports
        FILE_SET sqlgen_headers DESTINATION ${INCLUDE_INSTALL_DIR}
        )

    install(
        EXPORT sqlgen-exports
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/sqlgen
        NAMESPACE sqlgen::
    )
endif ()
