// expected-failure: BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE

export module bioseq.authored.guc_access_using_declaration;

import bioseq.postgres.support.enum_storage;
import bioseq.codegen.sql.annotation_model;

using bioseq::postgres::support::postgres_enum_registration_access;

enum class aliased_mode : int {
    disabled = 0,
};

struct aliased_registrar final {
    void register_variable(int*) const {}
};

inline void invalid_registration() {
    bioseq::postgres::support::postgres_enum_storage<aliased_mode> storage{
        aliased_mode::disabled};
    postgres_enum_registration_access::initialize(
        storage, aliased_registrar{});
}
