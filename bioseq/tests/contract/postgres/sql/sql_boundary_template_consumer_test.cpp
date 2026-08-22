/**
 * @file sql_boundary_template_consumer_test.cpp
 * @brief Verifies generated SQL-boundary templates from an ordinary importer.
 */
#include <bioseq/pg_compat.h>

extern "C" {
#include "utils/xid8.h"
}

#include <concepts>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>

import bioseq.postgres.sql.generated.core.sql_boundary_template_producer_test;

namespace {

namespace contract = bioseq::sql::generated::core::
    sql_boundary_template_producer_test;

using chunk_group_row =
    contract::chunk_group_codec::row_type;
using chunk_row =
    contract::chunk_codec::row_type;
using options_row =
    contract::options_codec::row_type;
using row_mask_row =
    contract::row_mask_codec::row_type;
using index_policy_row =
    contract::index_policy_codec::row_type;
using metadata_age_log_row =
    contract::metadata_age_log_codec::row_type;
using minimizer_generation_storage_row =
    contract::minimizer_generation_storage_codec::row_type;

struct foreign_row final {
    std::int64_t storage_id;
};

template<class p_codec, auto p_member>
concept registered_codec_member = requires {
    typename p_codec::template member_type<p_member>;
    {
        p_codec::template attribute_number<p_member>()
    } -> std::same_as<AttrNumber>;
};

template<class p_encoded, class p_row>
concept publicly_constructible_encoded_row =
    std::constructible_from<p_encoded, const p_row&>;

static_assert(std::same_as<
              typename contract::setof_result::value_type,
              std::int32_t>);
static_assert(std::same_as<
              typename contract::table_result::value_type,
              contract::result_row>);
static_assert(std::same_as<
              typename contract::named_set_result::value_type,
              contract::result_row>);
static_assert(!std::is_copy_constructible_v<
              contract::setof_result>);
static_assert(!std::is_copy_constructible_v<
              contract::named_result>);
static_assert(contract::boundary_instantiation != nullptr);

// Every generated row-codec storage group must remain reachable through the
// imported producer BMI.
static_assert(std::same_as<
              contract::chunk_group_codec::member_type<
                  &chunk_group_row::storage_id>,
              std::int64_t>);
static_assert(std::same_as<
              contract::options_codec::member_type<
                  &options_row::regclass>,
              Oid>);
static_assert(std::same_as<
              contract::metadata_age_log_codec::member_type<
                  &metadata_age_log_row::last_updated>,
              TimestampTz>);
static_assert(std::same_as<
              contract::minimizer_generation_storage_codec::
                  member_type<
                      &minimizer_generation_storage_row::
                          payload_relation_oid>,
              Oid>);
static_assert(std::same_as<
              contract::minimizer_generation_storage_codec::
                  member_type<
                      &minimizer_generation_storage_row::
                          payload_relation_name>,
              NameData>);
static_assert(std::same_as<
              contract::minimizer_generation_storage_codec::
                  member_type<
                      &minimizer_generation_storage_row::
                          retire_full_xid>,
              std::optional<FullTransactionId>>);
static_assert(std::same_as<
              contract::options_codec::member_type<
                  &options_row::compression>,
              NameData>);
static_assert(std::same_as<
              contract::chunk_codec::member_type<
                  &chunk_row::minimum_value>,
              std::optional<bytea*>>);
static_assert(std::same_as<
              contract::index_policy_codec::member_type<
                  &index_policy_row::index_name>,
              text*>);

static_assert(registered_codec_member<
              contract::chunk_group_codec,
              &chunk_group_row::storage_id>);
static_assert(!registered_codec_member<
              contract::chunk_group_codec,
              &foreign_row::storage_id>);
static_assert(!publicly_constructible_encoded_row<
              contract::chunk_group_codec::encoded_row,
              chunk_group_row>);

// Instantiate scalar, OID, timestamp, XID8, NAME, bytea, and text update
// paths, both scan-key forms, and the explicit-context deformation surface.
static_assert(requires(
    contract::chunk_group_codec::update& update,
    std::int64_t scalar) {
    update.template set<&chunk_group_row::storage_id>(scalar);
});
static_assert(requires(
    contract::options_codec::update& update,
    Oid oid,
    NameData name,
    MemoryContext value_context) {
    update.template set<&options_row::regclass>(oid);
    update.template set<&options_row::compression>(
        name, value_context);
});
static_assert(requires(
    contract::metadata_age_log_codec::update& update,
    TimestampTz timestamp) {
    update.template set<&metadata_age_log_row::last_updated>(
        timestamp);
});
static_assert(requires(
    contract::minimizer_generation_storage_codec::update& update,
    std::optional<FullTransactionId> xid) {
    update.template set<
        &minimizer_generation_storage_row::retire_full_xid>(xid);
    update.template set_null<
        &minimizer_generation_storage_row::retire_full_xid>();
});
static_assert(requires(
    contract::row_mask_codec::update& update,
    std::optional<bytea*> bytes,
    MemoryContext value_context) {
    update.template set<&row_mask_row::mask>(
        bytes, value_context);
    update.template set_null<&row_mask_row::mask>();
});
static_assert(requires(
    contract::index_policy_codec::update& update,
    text* value,
    MemoryContext value_context) {
    update.template set<&index_policy_row::index_name>(
        value, value_context);
});
static_assert(requires {
    contract::chunk_group_codec::scan_key<
        &chunk_group_row::storage_id,
        bioseq::postgres::support::scan_comparison::less_equal>(
            std::int64_t{});
    contract::options_codec::scan_key<
        &options_row::regclass>(Oid{});
});
static_assert(requires(
    HeapTuple tuple,
    TupleDesc descriptor,
    MemoryContext result_context) {
    {
        contract::chunk_codec::deform(
            tuple, descriptor, result_context)
    } -> std::same_as<chunk_row>;
});

/**
 * Force imported template bodies to instantiate in an ordinary consumer TU.
 */
[[maybe_unused]] void instantiate_boundary_templates() {
    auto setof_rows = contract::make_setof_result();
    auto table_rows = contract::make_table_result();
    auto named_set_rows = contract::make_named_set_result();
    auto named_row = contract::make_named_result();
    static_cast<void>(setof_rows.size());
    static_cast<void>(table_rows.size());
    static_cast<void>(named_set_rows.size());
    static_cast<void>(named_row.value());
    static_cast<void>(
        contract::boundary_instantiation(
            contract::boundary_callable{}));
}

}  // namespace
