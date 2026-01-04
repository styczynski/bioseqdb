# set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
# set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)

message(PROJECT_SOURCE_DIR="${CMAKE_CURRENT_LIST_DIR}")
file(GLOB_RECURSE SRC_FILES ${CMAKE_CURRENT_LIST_DIR}/backend/*.c ${CMAKE_CURRENT_LIST_DIR}/backend/**/*.c ${CMAKE_CURRENT_LIST_DIR}/include/*.h)
message("SOURCES ${SRC_FILES}")

find_package(Intl)

PostgreSQL_add_extension(postgresql_bioseq
  CXX 26
  RELOCATABLE
  NAME bioseq
  COMMENT "BioSeqDB PG Extension"
  VERSION ${INSTALL_VERSION}
  COMPONENT ${PROJECT_NAME}
  INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../config"
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include"
    "${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog"
  LINKS
    safestring_static
    fmt
  SOURCES
    ${SRC_FILES}
  SUPPORT_CXX_MODULES
  CXX_MOD_BASE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
  INSTALLS
    sql/bioseq--1.0.sql
)
#target_compile_features(postgresql_bioseq PRIVATE cxx_std_17)
# set_source_files_properties(${SRC_FILES} PROPERTIES LANGUAGE CXX )
# set_property(TARGET postgresql_bioseq PROPERTY CXX_STANDARD 26)

# configure
#configure_file(src/config.h.in src/config.h)

# include
# target_include_directories(postgresql_bioseq PRIVATE
#   ${CMAKE_CURRENT_LIST_DIR}/../config
#   ${CMAKE_CURRENT_LIST_DIR}/include
#   ${CMAKE_CURRENT_LIST_DIR}/../safestringlib/include
# )

# target_link_libraries(postgresql_bioseq PRIVATE safestring_static fmt)


# include_directories(${CMAKE_CURRENT_LIST_DIR}/../vendor/fmtlog)

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