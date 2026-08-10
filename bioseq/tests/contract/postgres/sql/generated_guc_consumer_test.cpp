#include <concepts>
#include <type_traits>

import bioseq.postgres.sql.generated.core.generated_guc_producer_test;

namespace {

namespace contract = bioseq::sql::generated::core::
    generated_guc_producer_test;

template<class p_storage, class p_registrar>
concept exposes_member_init = requires(
    p_storage& storage,
    const p_registrar& registrar) {
    storage.init(registrar);
};

template<class p_storage>
concept exposes_wire_cell = requires(p_storage& storage) {
    storage.wire_;
};

static_assert(!std::is_default_constructible_v<contract::storage>);
static_assert(!std::is_copy_constructible_v<contract::storage>);
static_assert(!std::is_move_constructible_v<contract::storage>);
static_assert(!exposes_member_init<
              contract::storage,
              contract::registrar>);
static_assert(!exposes_wire_cell<contract::storage>);
static_assert(requires(
    contract::storage& storage,
    const contract::registrar& registrar) {
    {
        contract::register_storage(storage, registrar)
    } -> std::same_as<void>;
});

/**
 * Instantiate the exported generated-like registration body from an ordinary
 * importer without exposing private storage.
 */
[[maybe_unused]] void instantiate_generated_guc_access() {
    contract::storage storage{contract::wire_mode::enabled};
    int* observed_wire = nullptr;
    const contract::registrar registration{&observed_wire};
    contract::register_storage(storage, registration);
    static_cast<void>(observed_wire);
}

}  // namespace
