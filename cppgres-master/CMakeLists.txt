cmake_minimum_required(VERSION 3.22)
project(cppgres)

set(CMAKE_CXX_STANDARD 20)

list(PREPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_LIST_DIR}/cmake)

include(FindPostgres)

macro(pg_config VAR OPT)
    execute_process(
            COMMAND ${PG_CONFIG} ${OPT}
            OUTPUT_VARIABLE ${VAR}
            OUTPUT_STRIP_TRAILING_WHITESPACE)
endmacro()

pg_config(PG_PKG_INCLUDE_DIR --pkgincludedir)
pg_config(PG_INCLUDE_DIR_SERVER --includedir-server)

include(FindIntl)

pg_config(_pg_cflags --cflags)

add_library(cppgres INTERFACE)
target_include_directories(cppgres INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${PG_PKG_INCLUDE_DIR} ${PG_INCLUDE_DIR_SERVER} ${Intl_INCLUDE_DIR})
string(REPLACE " " ";" _pg_cflags_list "${_pg_cflags}")
target_compile_options(cppgres INTERFACE ${_pg_cflags_list})

pg_config(_pg_bindir --bindir)
pg_config(_pg_includedir --includedir)
pg_config(_pg_pkgincludedir --pkgincludedir)
pg_config(_pg_sharedir --sharedir)
pg_config(_pg_includedir_server --includedir-server)
pg_config(_pg_libs --libs)
pg_config(_pg_ldflags --ldflags)
pg_config(_pg_ldflags_sl --ldflags_sl)
pg_config(_pg_ldflags_ex --ldflags_ex)
pg_config(_pg_pkglibdir --pkglibdir)
pg_config(_pg_libdir --libdir)
pg_config(_pg_version --version)

separate_arguments(_pg_ldflags)
separate_arguments(_pg_ldflags_sl)
separate_arguments(_pg_ldflags_ex)

set(_server_lib_dirs ${_pg_libdir} ${_pg_pkglibdir})
set(_server_inc_dirs ${_pg_pkgincludedir} ${_pg_includedir_server})
string(REPLACE ";" " " _shared_link_options
        "${_pg_ldflags};${_pg_ldflags_sl}")
set(_link_options ${_pg_ldflags})

if (_pg_ldflags_ex)
    string(APPEND " ${_shared_link_options}" ${_pg_ldflags_ex})
endif ()

string(REPLACE ";" " " _link_options_1 "${_link_options}")

set(_link_flags "${_link_options_1}")

foreach (_dir ${_pg_libdir})
    set(_link_flags "${_link_flags} -L${_dir}")
endforeach ()

if (APPLE)
    set(_link_flags "${_link_flags} -bundle -bundle_loader ${_pg_bindir}/postgres")
endif ()

#### TESTS

add_library(cppgres_tests MODULE tests/tests.cpp tests/dummy.cpp)
add_dependencies(cppgres_tests cppgres)
target_link_libraries(cppgres_tests cppgres)
target_include_directories(cppgres_tests PRIVATE tests)

set_target_properties(cppgres_tests PROPERTIES LINK_FLAGS "${_link_flags}")
target_compile_features(cppgres_tests PUBLIC cxx_std_20)

enable_testing()

add_test(
        NAME cppgres_tests
        COMMAND env PG_CONFIG=${PG_CONFIG} TEST_MODULE_PATH=$<TARGET_FILE:cppgres_tests>
        ${CMAKE_CURRENT_LIST_DIR}/test.sh
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR})

##### ABI check

find_program(_clang clang)
find_program(_clangxx clang++)

find_package(Python COMPONENTS Interpreter)

if (NOT "${_clang}" STREQUAL _clang-NOTFOUND AND NOT "${_clangxx}" STREQUAL _clangxx-NOTFOUND AND Python_FOUND)
    add_custom_target(cppgres_abi
            COMMAND ${_clang} -Xclang -fdump-record-layouts-complete
            ${CMAKE_SOURCE_DIR}/src/cppgres/imports.h
            -I${PG_PKG_INCLUDE_DIR} -I${PG_INCLUDE_DIR_SERVER} $<IF:$<BOOL:${Intl_INCLUDE_DIR}>,-I${Intl_INCLUDE_DIR},> > ${CMAKE_BINARY_DIR}/c_abi.txt
            COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/abi_layout.py ${CMAKE_BINARY_DIR}/c_abi.txt ${CMAKE_BINARY_DIR}/c_abi.json
            COMMAND ${_clangxx} -Xclang -fdump-record-layouts-complete -x c++-header
            ${CMAKE_SOURCE_DIR}/src/cppgres/imports.h
            -I${PG_PKG_INCLUDE_DIR} -I${PG_INCLUDE_DIR_SERVER} $<IF:$<BOOL:${Intl_INCLUDE_DIR}>,-I${Intl_INCLUDE_DIR},> > ${CMAKE_BINARY_DIR}/cxx_abi.txt
            COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/abi_layout.py ${CMAKE_BINARY_DIR}/cxx_abi.txt ${CMAKE_BINARY_DIR}/cxx_abi.json
            COMMAND ${Python_EXECUTABLE} ${CMAKE_SOURCE_DIR}/scripts/check_abi.py ${CMAKE_BINARY_DIR}/c_abi.json ${CMAKE_BINARY_DIR}/cxx_abi.json
    )

    add_dependencies(cppgres cppgres_abi)
endif ()
