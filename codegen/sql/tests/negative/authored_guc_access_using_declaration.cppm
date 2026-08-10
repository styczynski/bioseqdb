// expected-failure: BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE

export module bioseq.authored.guc_access_using_declaration;

import bioseq.config.guc_storage;
import bioseq.codegen.sql.annotation_model;

using bioseq::pg_utils::guc_registration_access;

enum class aliased_mode : int {
    disabled = 0,
};

struct aliased_registrar final {
    void register_variable(int*) const {}
};

inline void invalid_registration() {
    bioseq::pg_utils::postgres_enum_storage<aliased_mode> storage{
        aliased_mode::disabled};
    guc_registration_access::init(storage, aliased_registrar{});
}
