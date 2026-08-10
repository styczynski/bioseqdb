// expected-failure: BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE

export module bioseq.authored.guc_access_violation;

import bioseq.postgres.support.enum_storage;
import bioseq.codegen.sql.annotation_model;

enum class authored_mode : int {
    disabled = 0,
};

struct authored_registrar final {
    void register_variable(int*) const {}
};

inline void invalid_registration() {
    bioseq::postgres::support::postgres_enum_storage<authored_mode> storage{
        authored_mode::disabled};
    bioseq::postgres::support::postgres_enum_registration_access::initialize(
        storage, authored_registrar{});
}
