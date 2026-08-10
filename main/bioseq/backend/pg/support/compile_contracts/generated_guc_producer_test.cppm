module;

#include <type_traits>

export module bioseq.postgres.sql.generated.core.generated_guc_producer_test;

export import bioseq.config.guc_storage;

export namespace bioseq::sql::generated::core::
    generated_guc_producer_test {

enum class wire_mode : int {
    disabled = -1,
    enabled = 7,
};

using storage = bioseq::postgres::support::postgres_enum_storage<wire_mode>;

struct registrar final {
    int** observed_wire;

    void register_variable(int* wire) const noexcept {
        *observed_wire = wire;
    }
};

/**
 * Generated infrastructure is the sole caller of the narrow registration
 * access shim; ordinary importers receive only this typed operation.
 */
inline void register_storage(
        storage& value,
        const registrar& registration) {
    bioseq::postgres::support::guc_registration_access::init(
        value, registration);
}

static_assert(std::is_enum_v<wire_mode>);
static_assert(std::is_nothrow_constructible_v<storage, wire_mode>);

}  // namespace bioseq::sql::generated::core::
   // generated_guc_producer_test
