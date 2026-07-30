add_library(bioseq_pg_support STATIC)
set_target_properties(bioseq_pg_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)
# This library is consumed by PostgreSQL server extensions.  It needs server
# headers, but must not propagate the libpq client archive into those modules;
# backend symbols are resolved by the postmaster at module load time.
target_include_directories(bioseq_pg_support PUBLIC ${PostgreSQL_INCLUDE_DIRS})
target_link_libraries(bioseq_pg_support PUBLIC PostgreSQL::ServerHeaders)
target_sources(bioseq_pg_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/safety_runtime_probe.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/error_boundary.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/lists.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/bitmapset.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/nodes.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/spi.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/index.cppm")

if(BUILD_TESTING)
  add_library(bioseq_pg_support_compile_contracts OBJECT
    "${CMAKE_CURRENT_LIST_DIR}/contract_tests.cpp")
  set_target_properties(bioseq_pg_support_compile_contracts PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)
  target_link_libraries(bioseq_pg_support_compile_contracts
    PRIVATE bioseq_pg_support)
endif()

# The source-neutral generated-SQL boundary is required by both extension
# DSOs.  Provision it alongside the PostgreSQL support target so cache-only
# builds do not need to enter the Bioseq source tree.
if(NOT TARGET bioseq_pg_sql_boundary)
  add_subdirectory(
    "${CMAKE_CURRENT_LIST_DIR}/../pg_sql_boundary"
    "${CMAKE_CURRENT_BINARY_DIR}/../pg_sql_boundary")
endif()
