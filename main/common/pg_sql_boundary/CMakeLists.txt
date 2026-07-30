add_library(bioseq_pg_sql_boundary STATIC)
set_target_properties(bioseq_pg_sql_boundary PROPERTIES
  CXX_STANDARD 26
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS ON
  CXX_MODULE_STD ON
  POSITION_INDEPENDENT_CODE ON
  INTERPROCEDURAL_OPTIMIZATION FALSE
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN YES)

target_include_directories(
  bioseq_pg_sql_boundary PUBLIC ${PostgreSQL_INCLUDE_DIRS})
target_sources(bioseq_pg_sql_boundary
  PUBLIC FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_LIST_DIR}"
    FILES
      "${CMAKE_CURRENT_LIST_DIR}/error_boundary.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/owning_values.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/shape.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/call_context.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/scalar_conversion.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/result_materialization.cppm"
      "${CMAKE_CURRENT_LIST_DIR}/index.cppm")
target_link_libraries(bioseq_pg_sql_boundary PUBLIC
  PostgreSQL::ServerHeaders
  bioseq_pg_support)
