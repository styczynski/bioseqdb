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
 *
 * ```sql
 * -- Inspect deleted-row counts in stored chunk groups as a catalog owner.
 * SELECT storage_id, stripe_id, chunk_id, deleted_rows
 * FROM bioseq_internal.row_mask
 * WHERE deleted_rows > 0
 * ORDER BY storage_id, stripe_id, chunk_id;
 * ```
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
    /// @brief Identifier allocated for this deletion-mask record.
    std::int64_t id;
    /// @brief Storage generation containing the masked rows.
    std::int64_t storage_id;
    /// @brief Stripe containing the masked row range.
    std::int64_t stripe_id;
    /// @brief Chunk group containing the masked row range.
    std::int32_t chunk_id;
    /// @brief First logical row number covered by the mask.
    std::int64_t start_row_number;
    /// @brief Last logical row number covered by the mask, inclusive.
    std::int64_t end_row_number;
    /// @brief Number of deleted rows in the covered range.
    std::int32_t deleted_rows;

    /// @brief Deletion bitmap for the covered row range.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    std::optional<bytea*> mask;
};

/**
 * @brief Per-table Bioseq storage options.
 *
 * ```sql
 * -- Inspect the compression and row limits recorded for Bioseq tables.
 * SELECT regclass AS relation, compression, compression_level,
 *        chunk_group_row_limit, stripe_row_limit
 * FROM bioseq_internal.options
 * ORDER BY regclass;
 * ```
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
    /// @brief OID of the user table whose storage options are recorded.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::regclass,
    })]]
    Oid regclass;

    /// @brief Maximum number of rows in one chunk group.
    std::int32_t chunk_group_row_limit;
    /// @brief Maximum number of rows in one stripe.
    std::int32_t stripe_row_limit;
    /// @brief Compression level used for column value streams.
    std::int32_t compression_level;

    /// @brief Name of the column compression algorithm.
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
 *
 * ```sql
 * -- Inspect the external entity and sequence layout configured for each table.
 * SELECT regclass AS relation, entity, alphabet, storage_width, chunk_bases
 * FROM bioseq_internal.external_relation_options
 * ORDER BY regclass;
 * ```
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
    /// @brief OID of the external table whose options are recorded.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::regclass,
    })]]
    Oid regclass;

    /// @brief External entity kind: taxonomy, catalog, record_gene, record_location, sequence, or sequence_chunk.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            R"SQL(entity IS NULL OR entity IN (
  'taxonomy', 'catalog', 'record_gene', 'record_location',
  'sequence', 'sequence_chunk'))SQL"},
    })]]
    std::optional<text*> entity;

    /// @brief Alphabet selected for external sequence values.
    std::optional<text*> alphabet;

    /// @brief Packed sequence storage width; the catalog permits values from 0 through 64.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{
            "storage_width BETWEEN 0 AND 64"},
    })]]
    std::int32_t storage_width = 0;

    /// @brief Configured number of bases per external sequence chunk; must be nonnegative.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"chunk_bases >= 0"},
    })]]
    std::int32_t chunk_bases = 0;
};

/**
 * @brief A stripe's byte range, logical row range and chunk-group counts.
 *
 * ```sql
 * -- Compare stripe row counts with their stored byte lengths.
 * SELECT storage_id, stripe_num, row_count, data_length
 * FROM bioseq_internal.stripe
 * ORDER BY storage_id, stripe_num;
 * ```
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
    /// @brief Storage generation containing this stripe.
    std::int64_t storage_id;
    /// @brief Identifier of the stripe within the storage generation.
    std::int64_t stripe_num;
    /// @brief Byte offset of the stripe data in columnar storage.
    std::int64_t file_offset;
    /// @brief Length of the stripe data in bytes.
    std::int64_t data_length;
    /// @brief Number of columns stored in the stripe.
    std::int32_t column_count;
    /// @brief Configured row count for each chunk group in the stripe.
    std::int32_t chunk_row_count;
    /// @brief Total number of rows stored in the stripe.
    std::int64_t row_count;
    /// @brief Number of chunk groups stored in the stripe.
    std::int32_t chunk_group_count;
    /// @brief First logical row number assigned to the stripe.
    std::int64_t first_row_number;
};

/**
 * @brief Total and deleted-row counts for one chunk group.
 *
 * ```sql
 * -- Count rows that have not been marked deleted in each stored chunk group.
 * SELECT storage_id, stripe_num, chunk_group_num,
 *        row_count - deleted_rows AS live_rows
 * FROM bioseq_internal.chunk_group
 * ORDER BY storage_id, stripe_num, chunk_group_num;
 * ```
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
    /// @brief Storage generation containing this chunk group.
    std::int64_t storage_id;
    /// @brief Stripe containing this chunk group.
    std::int64_t stripe_num;
    /// @brief Identifier of the chunk group within its stripe.
    std::int32_t chunk_group_num;
    /// @brief Total number of rows in the chunk group.
    std::int64_t row_count;
    /// @brief Number of rows marked deleted in the chunk group.
    std::int64_t deleted_rows = 0;
};

/**
 * @brief Locations, compression details and min/max values for one column chunk.
 *
 * ```sql
 * -- Compare the stored and decompressed sizes of each column value stream.
 * SELECT storage_id, stripe_num, attr_num,
 *        value_stream_length, value_decompressed_length
 * FROM bioseq_internal.chunk
 * ORDER BY storage_id, stripe_num, attr_num, chunk_group_num;
 * ```
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
    /// @brief Storage generation containing this column chunk.
    std::int64_t storage_id;
    /// @brief Stripe containing this column chunk.
    std::int64_t stripe_num;
    /// @brief PostgreSQL attribute number of the stored column.
    std::int32_t attr_num;
    /// @brief Chunk group containing this column chunk.
    std::int32_t chunk_group_num;

    /// @brief Serialized minimum value used to skip chunks during filtering, when available.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    std::optional<bytea*> minimum_value;

    /// @brief Serialized maximum value used to skip chunks during filtering, when available.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    std::optional<bytea*> maximum_value;

    /// @brief Byte offset locating the stored value stream.
    std::int64_t value_stream_offset;
    /// @brief Length of the stored value stream in bytes.
    std::int64_t value_stream_length;
    /// @brief Byte offset locating the presence-bit stream for NULL handling.
    std::int64_t exists_stream_offset;
    /// @brief Length of the presence-bit stream in bytes.
    std::int64_t exists_stream_length;
    /// @brief Numeric identifier of the value-stream compression algorithm.
    std::int32_t value_compression_type;
    /// @brief Compression level used for the value stream.
    std::int32_t value_compression_level;
    /// @brief Size of the value buffer before compression, in bytes.
    std::int64_t value_decompressed_length;
    /// @brief Number of rows represented by this column chunk.
    std::int64_t value_count;
};

/**
 * @brief Map, sequence-colurn buckets to shard tables and track their byte counts.
 *
 * ```sql
 * -- Inspect shard relation names and determine which shards still accept writes.
 * SELECT sub_table_relid::regclass AS shard,
 *        sidecar_relid::regclass AS sidecar, bytes_written, is_full
 * FROM bioseq_internal.seq_sub_table
 * ORDER BY database_oid, main_relfilenode, column_attno, bucket_idx, shard_idx;
 * ```
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
    /// @brief OID of the database containing the parent table.
    Oid database_oid;
    /// @brief OID of the tablespace containing the parent table.
    Oid tablespace_oid;
    /// @brief Physical file number identifying the parent table.
    Oid main_relfilenode;
    /// @brief One-based attribute number of the parent sequence column.
    std::int16_t column_attno;
    /// @brief Length-bucket index containing this sequence shard.
    std::int16_t bucket_idx;
    /// @brief Shard number within the length bucket.
    std::int32_t shard_idx;
    /// @brief OID of the table storing the sequence fragments.
    Oid sub_table_relid;
    /// @brief OID of the shard sidecar relation.
    Oid sidecar_relid;

    /// @brief Number of payload bytes written to the shard.
    [[bioseq_sql::check({
        .expression = bioseq::sql::checked_sql{"bytes_written >= 0"},
    })]]
    std::int64_t bytes_written = 0;

    /// @brief Whether new writes must be directed to a later shard.
    bool is_full = false;

    /// @brief Recorded total payload size for the shard, in bytes.
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
