// expected-failure: BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE

export module bioseq.authored.guc_access_violation;

import bioseq.config.guc_storage;
import bioseq.codegen.sql.annotation_model;

enum class authored_mode : int {
    disabled = 0,
};

struct authored_registrar final {
    void register_variable(int*) const {}
};

inline void invalid_registration() {
    bioseq::pg_utils::postgres_enum_storage<authored_mode> storage{
        authored_mode::disabled};
    bioseq::pg_utils::guc_registration_access::init(
        storage, authored_registrar{});
}
