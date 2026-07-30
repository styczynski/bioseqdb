include_guard(GLOBAL)

# Provision one compiler-native BMI owner for the authored SQL annotation/value
# model. Extension DSOs consume this target, while the isolated host code
# generator compiles the same authored sources in its own build tree.
function(bioseq_ensure_sql_annotation_model)
  if(TARGET bioseq_sql_annotation_model)
    return()
  endif()

  set(_bioseq_sql_annotation_root
    "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../codegen")
  set(_bioseq_sql_annotation_modules
    "${_bioseq_sql_annotation_root}/sql/annotation_model/entities.cppm"
    "${_bioseq_sql_annotation_root}/sql/annotation_model/migration.cppm"
    "${_bioseq_sql_annotation_root}/sql/annotation_model/options.cppm"
    "${_bioseq_sql_annotation_root}/sql/annotation_model/validation.cppm"
    "${_bioseq_sql_annotation_root}/sql/annotation_model/index.cppm")

  add_library(bioseq_sql_annotation_model STATIC)
  target_sources(bioseq_sql_annotation_model
    PUBLIC FILE_SET CXX_MODULES
      BASE_DIRS "${_bioseq_sql_annotation_root}"
      FILES ${_bioseq_sql_annotation_modules})
  set_target_properties(bioseq_sql_annotation_model PROPERTIES
    CXX_STANDARD 26
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS ON
    CXX_MODULE_STD ON
    POSITION_INDEPENDENT_CODE ON
    INTERPROCEDURAL_OPTIMIZATION FALSE)
endfunction()
