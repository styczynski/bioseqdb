add_library(bioseq_pg_support STATIC)
set_target_properties(bioseq_pg_support PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE)
target_link_libraries(bioseq_pg_support PUBLIC PostgreSQL::PostgreSQL)
target_include_directories(bioseq_pg_support PUBLIC ${PostgreSQL_INCLUDE_DIRS})
target_sources(bioseq_pg_support
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES "${CMAKE_CURRENT_LIST_DIR}/index.cppm")
