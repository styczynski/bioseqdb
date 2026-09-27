/**
 * @file generated_guc_producer_test.cppm
 * @brief Exports sample enum configuration  registration for an import test
 */
module;

#include <type_traits>

export module bioseq.postgres.sql.generated.core.generated_guc_producer_test;

export import bioseq.postgres.support.enum_storage;

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
 * Registers enum storage through the sape access helper used by generated code.
 */
inline void register_storage(
        storage& value,
        const registrar& registration) {
    bioseq::postgres::support::postgres_enum_registration_access::initialize(
        value, registration);
}

static_assert(std::is_enum_v<wire_mode>);
static_assert(std::is_nothrow_constructible_v<storage, wire_mode>);

}  // namespace bioseq::sql::generated::core::
   // generated_guc_producer_test
