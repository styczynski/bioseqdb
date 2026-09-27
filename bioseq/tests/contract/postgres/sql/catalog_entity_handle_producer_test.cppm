/**
 * @file catalog_entity_handle_producer_test.cppm
 * @brief Exports generated-like fixed and extension-schema catalog handles.
 */
module;

extern "C" {
#include "catalog/pg_class_d.h"
}

#include <string_view>

export module bioseq.postgres.sql.generated.core.catalog_entity_handle_producer_test;

export import bioseq.postgres.support.sql_catalog_entities;

export namespace bioseq::sql::generated::core::
    catalog_entity_handle_producer_test {

/**
 * Fixed-schema generated-like sequence handle used by importer contracts.
 */
inline constexpr bioseq::postgres::support::relation_entity_handle<
    RELKIND_SEQUENCE,
    bioseq::postgres::support::fixed_catalog_name{"fixed_contract_seq"},
    bioseq::postgres::support::sql_schema_binding::fixed,
    bioseq::postgres::support::fixed_catalog_name{"bioseq_internal"}>
    fixed_contract_seq{};

/**
 * Relocatable extension-schema generated-like sequence handle.
 */
inline constexpr bioseq::postgres::support::relation_entity_handle<
    RELKIND_SEQUENCE,
    bioseq::postgres::support::fixed_catalog_name{"extension_contract_seq"},
    bioseq::postgres::support::sql_schema_binding::extension,
    bioseq::postgres::support::fixed_catalog_name{"bioseq"}>
    extension_contract_seq{};

/**
 * Profile-neutral descriptor imported unchanged by both profile builds.
 */
inline constexpr std::string_view core_descriptor{
    "bioseq.postgres.sql.generated.core.catalog_entity_handle_producer_test\n"
    "revision=1\n"};

static_assert(fixed_contract_seq.leaf_name() == "fixed_contract_seq");
static_assert(
    fixed_contract_seq.schema_binding()
    == bioseq::postgres::support::sql_schema_binding::fixed);
static_assert(
    fixed_contract_seq.stable_schema_name() == "bioseq_internal");
static_assert(
    extension_contract_seq.leaf_name() == "extension_contract_seq");
static_assert(
    extension_contract_seq.schema_binding()
    == bioseq::postgres::support::sql_schema_binding::extension);
static_assert(
    extension_contract_seq.stable_schema_name() == "$extension");

}  // namespace bioseq::sql::generated::core::
   // catalog_entity_handle_producer_test
