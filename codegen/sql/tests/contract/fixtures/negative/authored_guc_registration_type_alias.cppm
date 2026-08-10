// expected-failure: BIOSEQ_SQL_E_GUC_REGISTRATION_PROVENANCE

export module bioseq.authored.guc_registration_type_alias;

import bioseq.postgres.support.sql_boundary;
import bioseq.codegen.sql.annotation_model;

enum class reserved_mode : int {
    disabled = 0,
};

using reserved_registration =
    bioseq::postgres::support::postgres_enum_registration<reserved_mode>;

static_assert(sizeof(reserved_registration) > 0);
