/**
 * @file sql_rows.cppm
 * @brief Fixed catalog-row layouts for sequence and generation storage.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>

export module bioseq.postgres.storage.manager.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::catalog_rows {

/**
 * @brief Immutable relation-backed minimizer generation descriptor. The
 * serialized provider discriminator is fixed at 0; state 1 is live and state
 * 2 is retired.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .persistence = bioseq::sql::persistence::unlogged,
            .access_method = "heap",
        }),
        bioseq_sql::revoke({
            .privileges = bioseq::sql::privileges::all,
            .from = bioseq::sql::public_role,
        }),
        bioseq_sql::primary_key({
            .columns = {
                "database_oid",
                "tablespace_oid",
                "main_relfilenode",
                "user_relid",
                "column_attno",
                "index_name",
                "query_key",
                "generation_id",
            },
        }),
        bioseq_sql::index({
            .name = "minimizer_generation_storage_one_live",
            .unique = true,
            .expression = bioseq::sql::checked_sql{
                R"SQL(database_oid, tablespace_oid, main_relfilenode,
user_relid, column_attno, index_name, query_key)SQL"},
            .predicate =
                bioseq::sql::checked_sql{"state = smallint '1'"},
        }),
        bioseq_sql::index({
            .name = "minimizer_generation_storage_retired",
            .expression = bioseq::sql::checked_sql{
                R"SQL(retire_full_xid, database_oid, tablespace_oid,
main_relfilenode, user_relid, column_attno, index_name, query_key,
generation_id)SQL"},
            .predicate =
                bioseq::sql::checked_sql{"state = smallint '2'"},
        }),
        bioseq_sql::config_dump]] minimizer_generation_storage {
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"database_oid <> 0"},
    })]]
    Oid database_oid;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"tablespace_oid <> 0"},
    })]]
    Oid tablespace_oid;

    [[bioseq_sql::check({
        .expression =
            bioseq::sql::checked_sql{"main_relfilenode <> 0"},
    })]]
    Oid main_relfilenode;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"user_relid <> 0"},
    })]]
    Oid user_relid;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"column_attno > 0"},
    })]]
    std::int16_t column_attno;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            "index_name = 'minimizers_per_fragment'"},
    })]]
    NameData index_name;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"query_key >= 0"},
    })]]
    std::int64_t query_key;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"generation_id > 0"},
    })]]
    std::int64_t generation_id;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"provider = 0"},
    })]]
    std::int16_t provider;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            R"SQL(payload_relation_oid <> 0
AND payload_database_oid = database_oid
AND payload_tablespace_oid <> 0
AND payload_relfilenode <> 0)SQL"},
    })]]
    Oid payload_relation_oid;

    Oid payload_database_oid;
    Oid payload_tablespace_oid;
    Oid payload_relfilenode;
    NameData payload_relation_name;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"image_bytes >= 4096"},
    })]]
    std::int64_t image_bytes;

    std::int64_t header_checksum;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            R"SQL((state = 1 AND retire_full_xid IS NULL)
OR (state = 2 AND retire_full_xid IS NOT NULL))SQL"},
    })]]
    std::optional<FullTransactionId> retire_full_xid;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"state IN (1, 2)"},
    })]]
    std::int16_t state;
};

/**
 * @brief Cursor for bounded generation-image orphan reconciliation.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .persistence = bioseq::sql::persistence::unlogged,
            .access_method = "heap",
        }),
        bioseq_sql::revoke({
            .privileges = bioseq::sql::privileges::all,
            .from = bioseq::sql::public_role,
        }),
        bioseq_sql::primary_key({
            .placement =
                bioseq::sql::constraint_placement::column_inline,
            .columns = {"database_oid"},
        }),
        bioseq_sql::config_dump]] minimizer_generation_reaper_state {
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"database_oid <> 0"},
    })]]
    Oid database_oid;

    [[bioseq_sql::check({
        .expression =
            bioseq::sql::checked_sql{"tablespace_root_ordinal >= 0"},
    })]]
    std::int32_t tablespace_root_ordinal = 0;

    [[bioseq_sql::check({
        .expression =
            bioseq::sql::checked_sql{
                "shard_ordinal BETWEEN 0 AND 255"},
    })]]
    std::int16_t shard_ordinal = 0;

    std::string last_canonical_filename = std::string{};

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"pass >= 0"},
    })]]
    std::int64_t pass = 0;
};

/**
 * @brief PostgreSQL-owned MVCC source epoch and insertion-time content checksum for one immutable sequence locator range; state 0=UNPUBLISHED, 1=READY, 2=terminal INVALID.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .access_method = "heap",
        }),
        bioseq_sql::revoke({
            .privileges = bioseq::sql::privileges::all,
            .from = bioseq::sql::public_role,
        }),
        bioseq_sql::primary_key({
            .name = "sequence_source_version_pkey",
            .columns = {"source_epoch"},
        }),
        bioseq_sql::index({
            .name = "sequence_source_version_locator_key",
            .unique = true,
            .expression = bioseq::sql::checked_sql{
                R"SQL(fragment_relid, fragment_storage_id,
first_sub_row_no)SQL"},
        }),
        bioseq_sql::index({
            .name = "sequence_source_version_fragment_row_idx",
            .expression = bioseq::sql::checked_sql{
                "fragment_relid, first_sub_row_no"},
        }),
        bioseq_sql::config_dump]] sequence_source_version {
    [[bioseq_sql::check({
        .name = "sequence_source_version_source_epoch_check",
        .expression = bioseq::sql::checked_sql{"source_epoch > 0"},
    })]]
    std::int64_t source_epoch;

    Oid user_relid;
    Oid database_oid;
    Oid tablespace_oid;
    Oid main_relfilenode;

    [[bioseq_sql::check({
        .name = "sequence_source_version_main_storage_id_check",
        .expression = bioseq::sql::checked_sql{"main_storage_id > 0"},
    })]]
    std::int64_t main_storage_id;

    [[bioseq_sql::check({
        .name = "sequence_source_version_column_attno_check",
        .expression = bioseq::sql::checked_sql{"column_attno > 0"},
    })]]
    std::int16_t column_attno;

    [[bioseq_sql::check({
        .name = "sequence_source_version_locator_version_check",
        .expression = bioseq::sql::checked_sql{"locator_version = 3"},
    })]]
    std::int16_t locator_version;

    [[bioseq_sql::check({
        .name = "sequence_source_version_bucket_idx_check",
        .expression =
            bioseq::sql::checked_sql{"bucket_idx BETWEEN 0 AND 255"},
    })]]
    std::int16_t bucket_idx;

    [[bioseq_sql::check({
        .name = "sequence_source_version_shard_idx_check",
        .expression = bioseq::sql::checked_sql{"shard_idx >= 0"},
    })]]
    std::int32_t shard_idx;

    [[bioseq_sql::check({
        .name = "sequence_source_version_fragment_shape_check",
        .expression = bioseq::sql::checked_sql{
            R"SQL((num_fragments = 0
AND fragment_relid IS NULL
AND fragment_storage_id = 0
AND first_sub_row_no = 0)
OR (num_fragments > 0
AND fragment_relid IS NOT NULL
AND fragment_storage_id > 0))SQL"},
    })]]
    std::optional<Oid> fragment_relid;

    [[bioseq_sql::check({
        .name =
            "sequence_source_version_fragment_storage_id_check",
        .expression =
            bioseq::sql::checked_sql{"fragment_storage_id >= 0"},
    })]]
    std::int64_t fragment_storage_id;

    [[bioseq_sql::check({
        .name = "sequence_source_version_first_sub_row_no_check",
        .expression =
            bioseq::sql::checked_sql{"first_sub_row_no >= 0"},
    })]]
    std::int64_t first_sub_row_no;

    [[bioseq_sql::check({
        .name = "sequence_source_version_num_fragments_check",
        .expression = bioseq::sql::checked_sql{
            "num_fragments BETWEEN 0 AND 4294967295"},
    })]]
    std::int64_t num_fragments;

    [[bioseq_sql::check({
        .name = "sequence_source_version_total_bp_size_check",
        .expression =
            bioseq::sql::checked_sql{"total_bp_size >= 0"},
    })]]
    std::int64_t total_bp_size;

    [[bioseq_sql::check({
        .name = "sequence_source_version_alphabet_code_check",
        .expression = bioseq::sql::checked_sql{
            "alphabet_code BETWEEN 0 AND 255"},
    })]]
    std::int16_t alphabet_code;

    [[bioseq_sql::check({
        .name =
            "sequence_source_version_storage_unit_byte_width_check",
        .expression = bioseq::sql::checked_sql{
            "storage_unit_byte_width BETWEEN 1 AND 15"},
    })]]
    std::int16_t storage_unit_byte_width;

    [[bioseq_sql::check({
        .name = "sequence_source_version_padding_letters_check",
        .expression = bioseq::sql::checked_sql{
            "padding_letters BETWEEN 0 AND 65535"},
    })]]
    std::int32_t padding_letters;

    std::int64_t source_content_checksum;

    [[bioseq_sql::check({
        .name = "sequence_source_version_state_check",
        .expression = bioseq::sql::checked_sql{"state IN (0, 1, 2)"},
    })]]
    std::int16_t state;
};

static_assert(std::is_aggregate_v<minimizer_generation_storage>);
static_assert(std::is_aggregate_v<minimizer_generation_reaper_state>);
static_assert(std::is_aggregate_v<sequence_source_version>);

}  // namespace bioseq::storage::catalog_rows

export namespace bioseq::storage {

/** Result of one orphaned sequence-storage cleanup attempt. */
struct reaper_event {
    std::string action;
    std::string target;
};

static_assert(std::is_aggregate_v<reaper_event>);

}  // namespace bioseq::storage
