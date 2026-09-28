/**
 * @file sql_rows.cppm
 * @brief Declare SQL catalog tables and row types for Bioseq storage metadata.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <optional>
#include <type_traits>

export module bioseq.postgres.storage.metadata.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::metadata::catalog_rows {

/**
 * @brief Deletion bits and deleted-row count for a range within a chunk group.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .user_catalog_table = true,
        }),
        bioseq_sql::primary_key({
            .columns = {
                "id",
                "storage_id",
                "start_row_number",
                "end_row_number",
            },
        }),
        bioseq_sql::unique({
            .name = "row_mask_stripe_unique",
            .placement =
                bioseq::sql::constraint_placement::alter_table,
            .columns = {
                "storage_id",
                "stripe_id",
                "start_row_number",
            },
        }),
        bioseq_sql::unique({
            .name = "row_mask_chunk_unique",
            .placement =
                bioseq::sql::constraint_placement::alter_table,
            .columns = {
                "storage_id",
                "stripe_id",
                "chunk_id",
                "start_row_number",
            },
        })]] row_mask {
    std::int64_t id;
    std::int64_t storage_id;
    std::int64_t stripe_id;
    std::int32_t chunk_id;
    std::int64_t start_row_number;
    std::int64_t end_row_number;
    std::int32_t deleted_rows;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    std::optional<bytea*> mask;
};

/**
 * @brief Per-table Bioseq storage options.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
            .user_catalog_table = true,
        }),
        bioseq_sql::primary_key({
            .placement =
                bioseq::sql::constraint_placement::column_inline,
            .columns = {"regclass"},
        })]] options {
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::regclass,
    })]]
    Oid regclass;

    std::int32_t chunk_group_row_limit;
    std::int32_t stripe_row_limit;
    std::int32_t compression_level;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::name,
    })]]
    NameData compression;
};

/**
 * @brief Store custom options for a bioseq_external table.
 *
 * PostgreSQL 17 provides no custom table-access callback for these relation
 * options. Keep the external entity and sequence settings in this catalog,
 * where Bioseq can validate  and restore them with the table.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .access_method = "heap",
            .user_catalog_table = true,
        }),
        bioseq_sql::primary_key({
            .placement =
                bioseq::sql::constraint_placement::column_inline,
            .columns = {"regclass"},
        }),
        bioseq_sql::config_dump]] external_relation_options {
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::regclass,
    })]]
    Oid regclass;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            R"SQL(entity IS NULL OR entity IN (
  'taxonomy', 'catalog', 'record_gene', 'record_location',
  'sequence', 'sequence_chunk'))SQL"},
    })]]
    std::optional<text*> entity;

    std::optional<text*> alphabet;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            "storage_width BETWEEN 0 AND 64"},
    })]]
    std::int32_t storage_width = 0;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"chunk_bases >= 0"},
    })]]
    std::int32_t chunk_bases = 0;
};

/**
 * @brief A stripe's byte range, logical row range and chunk-group counts.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
            .user_catalog_table = true,
        }),
        bioseq_sql::primary_key({
            .columns = {
                "storage_id",
                "stripe_num",
            },
        }),
        bioseq_sql::unique({
            .name = "stripe_first_row_number_idx",
            .columns = {
                "storage_id",
                "first_row_number",
            },
        })]] stripe {
    std::int64_t storage_id;
    std::int64_t stripe_num;
    std::int64_t file_offset;
    std::int64_t data_length;
    std::int32_t column_count;
    std::int32_t chunk_row_count;
    std::int64_t row_count;
    std::int32_t chunk_group_count;
    std::int64_t first_row_number;
};

/**
 * @brief Total and deleted-row counts for one chunk group.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
        }),
        bioseq_sql::primary_key({
            .columns = {
                "storage_id",
                "stripe_num",
                "chunk_group_num",
            },
        })]] chunk_group {
    std::int64_t storage_id;
    std::int64_t stripe_num;
    std::int32_t chunk_group_num;
    std::int64_t row_count;
    std::int64_t deleted_rows = 0;
};

/**
 * @brief Locations, compression details and min/max values for one column chunk.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
            .user_catalog_table = true,
        }),
        bioseq_sql::primary_key({
            .columns = {
                "storage_id",
                "stripe_num",
                "attr_num",
                "chunk_group_num",
            },
        })]] chunk {
    std::int64_t storage_id;
    std::int64_t stripe_num;
    std::int32_t attr_num;
    std::int32_t chunk_group_num;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    std::optional<bytea*> minimum_value;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    std::optional<bytea*> maximum_value;

    std::int64_t value_stream_offset;
    std::int64_t value_stream_length;
    std::int64_t exists_stream_offset;
    std::int64_t exists_stream_length;
    std::int32_t value_compression_type;
    std::int32_t value_compression_level;
    std::int64_t value_decompressed_length;
    std::int64_t value_count;
};

/**
 * @brief Map, sequence-colurn buckets to shard tables and track their byte counts.
 */
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
        }),
        bioseq_sql::primary_key({
            .columns = {
                "database_oid",
                "tablespace_oid",
                "main_relfilenode",
                "column_attno",
                "bucket_idx",
                "shard_idx",
            },
        }),
        bioseq_sql::index({
            .name = "seq_sub_table_active_shard_idx",
            .create = bioseq::sql::create_mode::if_not_exists,
            .expression = bioseq::sql::checked_sql{
                R"SQL(database_oid, tablespace_oid, main_relfilenode,
column_attno, bucket_idx)SQL"},
            .predicate =
                bioseq::sql::checked_sql{"is_full = false"},
        }),
        bioseq_sql::index({
            .name = "seq_sub_table_by_main_idx",
            .create = bioseq::sql::create_mode::if_not_exists,
            .expression = bioseq::sql::checked_sql{
                "database_oid, tablespace_oid, main_relfilenode"},
        }),
        bioseq_sql::config_dump]] seq_sub_table {
    Oid database_oid;
    Oid tablespace_oid;
    Oid main_relfilenode;
    std::int16_t column_attno;
    std::int16_t bucket_idx;
    std::int32_t shard_idx;
    Oid sub_table_relid;
    Oid sidecar_relid;

    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"bytes_written >= 0"},
    })]]
    std::int64_t bytes_written = 0;

    bool is_full = false;

    [[bioseq_sql::check({
        .expression =
            bioseq::sql::checked_sql{"total_payload_bytes >= 0"},
    })]]
    std::int64_t total_payload_bytes = 0;
};

static_assert(std::is_aggregate_v<row_mask>);
static_assert(std::is_aggregate_v<options>);
static_assert(std::is_aggregate_v<external_relation_options>);
static_assert(std::is_aggregate_v<stripe>);
static_assert(std::is_aggregate_v<chunk_group>);
static_assert(std::is_aggregate_v<chunk>);
static_assert(std::is_aggregate_v<seq_sub_table>);
static_assert(std::is_standard_layout_v<options>);
static_assert(std::is_standard_layout_v<external_relation_options>);
static_assert(std::is_standard_layout_v<stripe>);
static_assert(std::is_standard_layout_v<chunk_group>);
static_assert(std::is_standard_layout_v<seq_sub_table>);

} // namespace bioseq::storage::metadata::catalog_rows
