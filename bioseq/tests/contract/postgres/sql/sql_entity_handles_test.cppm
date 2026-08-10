module;

#include <bioseq/pg_compat.h>

#include <concepts>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

export module bioseq.tests.contract.postgres.sql.sql_entity_handles;

import bioseq.postgres.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import bioseq.postgres.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
import bioseq.postgres.sql.generated.core.storage_metadata_core.entity_handles;
import bioseq.postgres.storage.indexing.sql_row_codecs;
import bioseq.postgres.storage.io.sql_row_codecs;
import bioseq.postgres.storage.manager.sql_row_codecs;
import bioseq.postgres.storage.metadata.sql_row_codecs;
import bioseq.postgres.table.sql_row_codecs;

namespace {

template<class p_handle>
[[nodiscard]] consteval bool generated_handle_contract(
        const p_handle& handle,
        std::string_view expected_leaf) {
    return handle.leaf_name() == expected_leaf
        && handle.stable_schema_name() == "bioseq_internal"
        && handle.schema_binding()
            == bioseq::postgres::support::sql_schema_binding::fixed
        && requires {
            handle.stable_qualified_name();
            handle.installed_schema_name(
                bioseq::postgres::support::catalog_missing_policy::return_invalid);
            handle.installed_qualified_name(
                bioseq::postgres::support::catalog_missing_policy::return_invalid);
            handle.resolve_oid(
                bioseq::postgres::support::catalog_missing_policy::return_invalid);
        };
}

namespace metadata =
    bioseq::sql::generated::core::storage_metadata_core::entity_handles;
namespace indexing =
    bioseq::sql::generated::core::
        storage_indexing_pg_chunked_storage::entity_handles;
namespace storage_manager =
    bioseq::sql::generated::core::
        storage_manager_sequence_storage_manager::entity_handles;

static_assert(generated_handle_contract(
    metadata::row_mask_seq, "row_mask_seq"));
static_assert(generated_handle_contract(
    metadata::storageid_seq, "storageid_seq"));
static_assert(generated_handle_contract(
    indexing::minimizer_generation_id_seq,
    "minimizer_generation_id_seq"));
static_assert(generated_handle_contract(
    storage_manager::sequence_source_epoch_seq,
    "sequence_source_epoch_seq"));

using chunk_group_codec =
    bioseq::storage::metadata::sql_row_codecs::chunk_group_codec;
using chunk_group_row = chunk_group_codec::row_type;
using row_mask_codec =
    bioseq::storage::metadata::sql_row_codecs::row_mask_codec;
using row_mask_row = row_mask_codec::row_type;
using options_codec =
    bioseq::storage::metadata::sql_row_codecs::options_codec;
using options_row = options_codec::row_type;
using stripe_codec =
    bioseq::storage::metadata::sql_row_codecs::stripe_codec;
using stripe_row = stripe_codec::row_type;
using chunk_codec =
    bioseq::storage::metadata::sql_row_codecs::chunk_codec;
using chunk_row = chunk_codec::row_type;
using index_policy_codec =
    bioseq::storage::indexing_pg::sql_row_codecs::
        index_policy_codec;
using index_policy_row = index_policy_codec::row_type;
using metadata_age_log_codec =
    bioseq::storage::indexing_pg::sql_row_codecs::
        metadata_age_log_codec;
using metadata_age_log_row = metadata_age_log_codec::row_type;
using index_table_shard_codec =
    bioseq::storage::indexing_pg::sql_row_codecs::
        index_table_shard_codec;
using index_table_shard_row = index_table_shard_codec::row_type;
using seq_sub_table_codec =
    bioseq::storage::metadata::sql_row_codecs::
        seq_sub_table_codec;
using seq_sub_table_row = seq_sub_table_codec::row_type;
using minimizer_generation_storage_codec =
    bioseq::storage::manager::sql_row_codecs::
        minimizer_generation_storage_codec;
using minimizer_generation_storage_row =
    minimizer_generation_storage_codec::row_type;
using minimizer_generation_reaper_codec =
    bioseq::storage::manager::sql_row_codecs::
        minimizer_generation_reaper_state_codec;
using minimizer_generation_reaper_row =
    minimizer_generation_reaper_codec::row_type;
using sequence_source_version_codec =
    bioseq::storage::manager::sql_row_codecs::
        sequence_source_version_codec;
using sequence_source_version_row =
    sequence_source_version_codec::row_type;
using aligner_options_codec =
    bioseq::table::sql_row_codecs::
        bioseq_aligner_options_codec;
using aligner_options_row = aligner_options_codec::row_type;
using column_mapping_codec =
    bioseq::storage::load_pg::sql_row_codecs::
        column_mapping_codec;
using column_mapping_row = column_mapping_codec::row_type;

template<class p_update>
concept nullable_deleted_rows = requires(p_update& update) {
    update.template set_null<&chunk_group_row::deleted_rows>();
};

static_assert(chunk_group_codec::fields_count == 5);
static_assert(
    chunk_group_codec::attribute_number<
        &chunk_group_row::storage_id>() == 1);
static_assert(
    chunk_group_codec::attribute_number<
        &chunk_group_row::deleted_rows>() == 5);
static_assert(std::same_as<
              chunk_group_codec::member_type<
                  &chunk_group_row::chunk_group_num>,
              std::int32_t>);
static_assert(
    chunk_group_codec::expected_descriptor[0].name
    == "storage_id");
static_assert(
    chunk_group_codec::expected_descriptor[4].name
    == "deleted_rows");
static_assert(
    !chunk_group_codec::expected_descriptor[4].nullable);
static_assert(
    !nullable_deleted_rows<chunk_group_codec::update>);

static_assert(row_mask_codec::fields_count == 8);
static_assert(options_codec::fields_count == 5);
static_assert(stripe_codec::fields_count == 9);
static_assert(chunk_codec::fields_count == 14);
static_assert(index_policy_codec::fields_count == 7);
static_assert(metadata_age_log_codec::fields_count == 5);
static_assert(index_table_shard_codec::fields_count == 9);
static_assert(seq_sub_table_codec::fields_count == 11);
static_assert(minimizer_generation_storage_codec::fields_count == 18);
static_assert(minimizer_generation_reaper_codec::fields_count == 5);
static_assert(sequence_source_version_codec::fields_count == 20);
static_assert(aligner_options_codec::fields_count == 36);
static_assert(column_mapping_codec::fields_count == 2);
static_assert(
    row_mask_codec::expected_descriptor[7].name == "mask");
static_assert(
    row_mask_codec::expected_descriptor[7].type_oid == BYTEAOID);
static_assert(row_mask_codec::expected_descriptor[7].nullable);
static_assert(
    options_codec::expected_descriptor[0].type_oid == REGCLASSOID);
static_assert(
    options_codec::expected_descriptor[4].type_oid == NAMEOID);
static_assert(
    stripe_codec::expected_descriptor[8].name
    == "first_row_number");
static_assert(
    chunk_codec::expected_descriptor[4].name
    == "minimum_value");
static_assert(
    chunk_codec::expected_descriptor[4].nullable);
static_assert(
    chunk_codec::expected_descriptor[5].nullable);
static_assert(std::same_as<
              options_codec::member_type<&options_row::regclass>,
              Oid>);
static_assert(std::same_as<
              options_codec::member_type<&options_row::compression>,
              NameData>);
static_assert(std::same_as<
              chunk_codec::member_type<&chunk_row::minimum_value>,
              std::optional<bytea*>>);
static_assert(std::same_as<
              index_policy_codec::member_type<
                  &index_policy_row::user_relid>,
              Oid>);
static_assert(std::same_as<
              index_policy_codec::member_type<
                  &index_policy_row::index_name>,
              text*>);
static_assert(std::same_as<
              metadata_age_log_codec::member_type<
                  &metadata_age_log_row::last_updated>,
              TimestampTz>);
static_assert(
    index_policy_codec::expected_descriptor[6].name
    == "is_active_read");
static_assert(
    metadata_age_log_codec::expected_descriptor[4].type_oid
    == TIMESTAMPTZOID);
static_assert(
    index_table_shard_codec::expected_descriptor[7].name
    == "bytes_written");
static_assert(
    index_table_shard_codec::attribute_number<
        &index_table_shard_row::is_full>() == 9);
static_assert(
    seq_sub_table_codec::attribute_number<
        &seq_sub_table_row::total_payload_bytes>() == 11);
static_assert(
    seq_sub_table_codec::expected_descriptor[8].name
    == "bytes_written");
static_assert(
    !minimizer_generation_storage_codec::expected_descriptor[9].nullable);
static_assert(
    minimizer_generation_storage_codec::expected_descriptor[16]
        .type_oid == XID8OID);
static_assert(std::same_as<
              minimizer_generation_storage_codec::member_type<
                  &minimizer_generation_storage_row::
                      payload_relation_oid>,
              Oid>);
static_assert(std::same_as<
              minimizer_generation_storage_codec::member_type<
                  &minimizer_generation_storage_row::
                      payload_relation_name>,
              NameData>);
static_assert(std::same_as<
              minimizer_generation_storage_codec::member_type<
                  &minimizer_generation_storage_row::
                      retire_full_xid>,
              std::optional<FullTransactionId>>);
static_assert(
    minimizer_generation_reaper_codec::expected_descriptor[3].name
    == "last_canonical_filename");
static_assert(std::same_as<
              minimizer_generation_reaper_codec::member_type<
                  &minimizer_generation_reaper_row::
                      last_canonical_filename>,
              std::string>);
static_assert(
    sequence_source_version_codec::attribute_number<
        &sequence_source_version_row::fragment_relid>() == 11);
static_assert(
    sequence_source_version_codec::expected_descriptor[10].nullable);
static_assert(
    sequence_source_version_codec::expected_descriptor[19].name
    == "state");
static_assert(
    aligner_options_codec::expected_descriptor[0].name
    == "kmer_size");
static_assert(
    aligner_options_codec::expected_descriptor[10].type_oid
    == FLOAT8OID);
static_assert(
    aligner_options_codec::expected_descriptor[32].type_oid
    == BOOLOID);
static_assert(
    aligner_options_codec::expected_descriptor[35].name
    == "index_read_requirement");
static_assert(std::same_as<
              aligner_options_codec::member_type<
                  &aligner_options_row::chain_gap_scale>,
              std::optional<double>>);
static_assert(
    column_mapping_codec::expected_descriptor[0].name
    == "column_name");
static_assert(
    column_mapping_codec::expected_descriptor[1].name
    == "data_source");
static_assert(
    column_mapping_codec::expected_descriptor[0].nullable);
static_assert(
    column_mapping_codec::expected_descriptor[1].nullable);
static_assert(std::same_as<
              column_mapping_codec::member_type<
                  &column_mapping_row::column_name>,
              std::optional<text*>>);
static_assert(requires(
    const column_mapping_row& row,
    MemoryContext value_context) {
    column_mapping_codec::encode(row, value_context);
});
static_assert(requires(
    column_mapping_codec::update& update) {
    update.template set_null<
        &column_mapping_row::column_name>();
    update.template set_null<
        &column_mapping_row::data_source>();
});
static_assert(requires(
    const row_mask_row& row,
    MemoryContext value_context) {
    row_mask_codec::encode(row, value_context);
});
static_assert(requires(
    const options_row& row,
    MemoryContext value_context) {
    options_codec::encode(row, value_context);
});
static_assert(requires(
    row_mask_codec::update& update,
    std::optional<bytea*> value,
    MemoryContext value_context) {
    update.template set<&row_mask_row::mask>(
        value, value_context);
    update.template set_null<&row_mask_row::mask>();
});
static_assert(requires {
    row_mask_codec::scan_key<
        &row_mask_row::start_row_number,
        bioseq::postgres::support::scan_comparison::less_equal>(
            std::int64_t{});
});
static_assert(requires {
    index_policy_codec::scan_key<
        &index_policy_row::user_relid>(Oid{});
    index_policy_codec::scan_key<
        &index_policy_row::column_attno>(std::int16_t{});
});
static_assert(requires(
    const index_policy_row& row,
    MemoryContext value_context) {
    index_policy_codec::encode(row, value_context);
});
static_assert(std::same_as<
              decltype(
                  std::declval<
                      chunk_group_codec::encoded_row&>()
                      .tuple_data(std::declval<TupleDesc>())
                      .values),
              Datum*>);
static_assert(std::same_as<
              decltype(
                  std::declval<
                      const chunk_group_codec::encoded_row&>()
                      .tuple_data(std::declval<TupleDesc>())
                      .values),
              const Datum*>);

} // namespace
