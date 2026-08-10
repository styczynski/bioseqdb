#include <bioseq/pg_compat.h>

#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

import bioseq.postgres.sql.generated.core.catalog_entity_handle_producer_test;

namespace {

namespace contract = bioseq::sql::generated::core::
    catalog_entity_handle_producer_test;

using fixed_handle =
    std::remove_cvref_t<decltype(contract::fixed_contract_seq)>;
using extension_handle =
    std::remove_cvref_t<decltype(contract::extension_contract_seq)>;

template<class p_handle>
concept complete_sequence_handle = requires(
    bioseq::postgres::support::catalog_missing_policy policy) {
    { p_handle::leaf_name() } -> std::same_as<std::string_view>;
    {
        p_handle::schema_binding()
    } -> std::same_as<bioseq::postgres::support::sql_schema_binding>;
    {
        p_handle::stable_schema_name()
    } -> std::same_as<std::string_view>;
    { p_handle::stable_qualified_name() } -> std::same_as<std::string>;
    {
        p_handle::installed_schema_name(policy)
    } -> std::same_as<std::optional<std::string>>;
    {
        p_handle::installed_qualified_name(policy)
    } -> std::same_as<std::optional<std::string>>;
    { p_handle::resolve_oid(policy) } -> std::same_as<Oid>;
};

static_assert(std::is_empty_v<fixed_handle>);
static_assert(std::is_empty_v<extension_handle>);
static_assert(complete_sequence_handle<fixed_handle>);
static_assert(complete_sequence_handle<extension_handle>);
static_assert(fixed_handle::leaf_name() == "fixed_contract_seq");
static_assert(
    fixed_handle::stable_schema_name() == "bioseq_internal");
static_assert(
    fixed_handle::schema_binding()
    == bioseq::postgres::support::sql_schema_binding::fixed);
static_assert(
    extension_handle::leaf_name() == "extension_contract_seq");
static_assert(extension_handle::stable_schema_name() == "$extension");
static_assert(
    extension_handle::schema_binding()
    == bioseq::postgres::support::sql_schema_binding::extension);
static_assert(
    contract::core_descriptor
    == "bioseq.postgres.sql.generated.core."
       "catalog_entity_handle_producer_test\n"
       "revision=1\n");

/**
 * Instantiate every runtime name/OID body from the imported producer BMI.
 */
[[maybe_unused]] void instantiate_runtime_handle_methods() {
    constexpr auto policy =
        bioseq::postgres::support::catalog_missing_policy::return_invalid;
    static_cast<void>(fixed_handle::stable_qualified_name());
    static_cast<void>(fixed_handle::installed_schema_name(policy));
    static_cast<void>(fixed_handle::installed_qualified_name(policy));
    static_cast<void>(fixed_handle::resolve_oid(policy));
    static_cast<void>(extension_handle::stable_qualified_name());
    static_cast<void>(extension_handle::installed_schema_name(policy));
    static_cast<void>(
        extension_handle::installed_qualified_name(policy));
    static_cast<void>(extension_handle::resolve_oid(policy));
}

}  // namespace
