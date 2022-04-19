cmake_minimum_required(VERSION 3.14)

project(bioseqdb LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

find_package(PostgreSQL REQUIRED COMPONENTS Server)
find_program(PG_CONFIG pg_config)
execute_process(COMMAND ${PG_CONFIG} --pkglibdir OUTPUT_VARIABLE PG_CONFIG_PKGLIBDIR OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND ${PG_CONFIG} --sharedir OUTPUT_VARIABLE PG_CONFIG_SHAREDIR OUTPUT_STRIP_TRAILING_WHITESPACE)

find_package(BZip2 REQUIRED)

find_library(BWA_LIBRARIES bwa REQUIRED)
find_library(FML_LIBRARIES fml REQUIRED)
find_library(HTS_LIBRARIES hts REQUIRED)
find_library(SEQLIB_LIBRARIES seqlib REQUIRED)

add_library(bioseqdb_pg SHARED
        bioseqdb_pg/extension.cpp
        )

target_include_directories(bioseqdb_pg PRIVATE ${PostgreSQL_TYPE_INCLUDE_DIR})
target_link_libraries(bioseqdb_pg PRIVATE ${PostgreSQL_LIBRARIES})
target_link_libraries(bioseqdb_pg PRIVATE ${BZIP2_LIBRARIES})
target_link_libraries(bioseqdb_pg PRIVATE ${FML_LIBRARIES} ${HTS_LIBRARIES} ${SEQLIB_LIBRARIES} ${BWA_LIBRARIES})

install(TARGETS bioseqdb_pg DESTINATION ${PG_CONFIG_PKGLIBDIR})
install(FILES bioseqdb_pg/bioseqdb.control DESTINATION ${PG_CONFIG_SHAREDIR}/extension)
install(FILES bioseqdb_pg/bioseqdb--0.0.0.sql DESTINATION ${PG_CONFIG_SHAREDIR}/extension)
