/**
 * @file sql_rows.cppm
 * @brief Catalog rows for, index settings, refresh times and table shards.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <type_traits>

export module bioseq.postgres.storage.indexing.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::indexing::catalog_rows {

/// @brief Saved settings for one index on a sequence colmn.
///
/// ```sql
/// -- Inspect which saved sequence indexes are enabled and selected for reads.
/// SELECT user_relid::regclass AS relation, column_attno, index_name,
///        enabled, is_active_read
/// FROM bioseq_internal.index_policy
/// ORDER BY user_relid, column_attno, index_name;
/// ```
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
            .access_method = "heap",
        }),
        bioseq_sql::primary_key({
            .columns = {
                "user_relid",
                "main_relfilenode",
                "column_attno",
                "index_name",
            },
        }),
        bioseq_sql::index({
            .name = "index_policy_one_active_read",
            .create = bioseq::sql::create_mode::if_not_exists,
            .unique = true,
            .expression = bioseq::sql::checked_sql{
                "user_relid, main_relfilenode, column_attno"},
            .predicate =
                bioseq::sql::checked_sql{"is_active_read"},
        }),
        bioseq_sql::config_dump]] index_policy {
    /// User table OID.
    Oid user_relid;
    /// Table file  number used to detect settings left over from a table rewrite.
    Oid main_relfilenode;
    /// Indexed column number, starting at one.
    std::int16_t column_attno;
    /// Index name stored as PostgreSQL text.
    text* index_name;
    /// Whether index maintenance may build this index.
    bool enabled = true;
    /// Whether inserts trigger maintenance.
    bool update_on_insert = false;
    /// Whether queries should use this index for the sequence column
    /// @brief Selects this index for reads of the sequence column.
    bool is_active_read = false;
};

/// @brief Last recorded refresh time for an index on a sequence column.
///
/// ```sql
/// -- Inspect the most recently recorded index metadata refreshes.
/// SELECT user_relid::regclass AS relation, column_attno, index_name, last_updated
/// FROM bioseq_internal.metadata_age_log
/// ORDER BY last_updated DESC;
/// ```
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
            .access_method = "heap",
        }),
        bioseq_sql::primary_key({
            .columns = {
                "user_relid",
                "main_relfilenode",
                "column_attno",
                "index_name",
            },
        }),
        bioseq_sql::config_dump]] metadata_age_log {
    /// User table whose index was refreshed.
    Oid user_relid;
    /// Table file number recorded for this refresh.
    Oid main_relfilenode;
    /// Indexed  column number, starting at one.
    std::int16_t column_attno;
    /// Index name stored as PostgreSQL text.
    text* index_name;

    [[bioseq_sql::column({
        .default_value = bioseq::sql::checked_sql{"now()"},
    })]]
    /// Time recorded when the index metadata was refreshed.
    TimestampTz last_updated;
};

/// @brief Location and size of one shard of a table index.
///
/// ```sql
/// -- Inspect physical index shard sizes and whether they accept more writes.
/// SELECT index_relid::regclass AS shard, index_name, bytes_written, is_full
/// FROM bioseq_internal.index_table_shard
/// ORDER BY bytes_written DESC;
/// ```
struct [[bioseq_sql::table({
            .schema =
                bioseq::sql::fixed_schema<"bioseq_internal">,
            .create = bioseq::sql::create_mode::if_not_exists,
            .access_method = "heap",
        }),
        bioseq_sql::primary_key({
            .columns = {
                "database_oid",
                "tablespace_oid",
                "main_relfilenode",
                "column_attno",
                "index_name",
                "shard_idx",
            },
        }),
        bioseq_sql::config_dump]] index_table_shard {
    /// Database containing the indexed table.
    Oid database_oid;
    /// Tablespace containing the indexed table.
    Oid tablespace_oid;
    /// File number of the indexed table.
    Oid main_relfilenode;
    /// Indexed column number, starting at one.
    std::int16_t column_attno;
    /// Index name stored as PostgreSQL text.
    text* index_name;
    /// Zero-based physical shard number.
    std::int32_t shard_idx;
    /// OID of the table holding this shard's index data.
    Oid index_relid;
    /// Number of payload bytes written to this shamd.
    std::int64_t bytes_written = 0;
    /// Whether new writes must use a later shard.
    bool is_full = false;
};

static_assert(std::is_aggregate_v<index_policy>);
static_assert(std::is_aggregate_v<metadata_age_log>);
static_assert(std::is_aggregate_v<index_table_shard>);
static_assert(std::is_standard_layout_v<index_policy>);
static_assert(std::is_standard_layout_v<metadata_age_log>);
static_assert(std::is_standard_layout_v<index_table_shard>);

}  // namespace bioseq::storage::indexing::catalog_rows
