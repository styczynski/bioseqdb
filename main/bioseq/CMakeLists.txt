message(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
file(GLOB SRC_FILES ${CMAKE_CURRENT_LIST_DIR}/backend/*.c ${CMAKE_CURRENT_LIST_DIR}/include/*.h)
message("SOURCES ${SRC_FILES}")

find_package(Intl)

PostgreSQL_add_extension(postgresql_bioseq
  RELOCATABLE
  NAME bioseq
  COMMENT "BioSeqDB PG Extension"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  SOURCES
    ${SRC_FILES}
  INSTALLS
    sql/citus_columnar--13.2-1--14.0-1.sql
)

# configure
#configure_file(src/config.h.in src/config.h)

# include
target_include_directories(postgresql_bioseq PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/../config
  ${CMAKE_CURRENT_LIST_DIR}/include
  ${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include
)

target_link_libraries(postgresql_bioseq PRIVATE safestring_shared)

# safestring_shared

# link
# target_link_libraries(postgresql_h3 PRIVATE
#   postgresql_h3_shared
#   h3
# )

# test
# if(BUILD_TESTING)
#   add_subdirectory(test)
# endif()