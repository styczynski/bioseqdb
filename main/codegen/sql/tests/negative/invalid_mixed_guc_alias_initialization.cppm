// expected-failure: BIOSEQ_SQL_E_MIXED_INITIALIZER

export module bioseq.codegen.sql.invalid_mixed_guc_alias_initialization;

import bioseq.codegen.sql.annotation_model;

enum class invalid_alias_level : int {
    debug [[bioseq_sql::guc_value({
        .aliases = bioseq::sql::guc_aliases({
            "debug",
            .hidden = true,
        }),
    })]] = 1,
};
