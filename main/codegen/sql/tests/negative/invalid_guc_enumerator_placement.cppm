// expected-failure: BIOSEQ_SQL_E_GUC_ENUMERATOR_PLACEMENT

export module bioseq.codegen.sql.invalid_guc_enumerator_placement;

import bioseq.codegen.sql.annotation_model;

enum class invalid_level : int {
    [[bioseq_sql::guc_value({})]]
    debug = 1,
};
