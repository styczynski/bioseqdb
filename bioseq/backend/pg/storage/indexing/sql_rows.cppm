/**
 * @file sql_rows.cppm
 * @brief Fixed catalog-row layouts for PostgreSQL index administration.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <type_traits>

export module bioseq.postgres.storage.indexing.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::indexing::catalog_rows {

/// @brief Per-column index policy persisted by the extension.
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
    /// User-visible relation governed by the policy.
    Oid user_relid;
    /// File identity used to reject stale policy bindings.
    Oid main_relfilenode;
    /// One-based indexed attribute number.
    std::int16_t column_attno;
    /// PostgreSQL-owned text naming the index policy.
    text* index_name;
    /// Whether maintenance may use this policy.
    bool enabled = true;
    /// Whether inserts trigger maintenance.
    bool update_on_insert = false;
    /// Whether this is the relation's active read policy.
    bool is_active_read = false;
};

/// @brief Last successful refresh time for each persisted index.
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
    /// User-visible relation whose metadata was refreshed.
    Oid user_relid;
    /// File identity of the refreshed relation.
    Oid main_relfilenode;
    /// One-based indexed attribute number.
    std::int16_t column_attno;
    /// PostgreSQL-owned index-policy name.
    text* index_name;

    [[bioseq_sql::column({
        .default_value = bioseq::sql::checked_sql{"now()"},
    })]]
    /// PostgreSQL timestamp of the successful refresh.
    TimestampTz last_updated;
};

/// @brief Physical relation allocated to one table-index shard.
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
    /// Database component of the parent physical identity.
    Oid database_oid;
    /// Tablespace component of the parent physical identity.
    Oid tablespace_oid;
    /// File component of the parent physical identity.
    Oid main_relfilenode;
    /// One-based indexed attribute number.
    std::int16_t column_attno;
    /// PostgreSQL-owned index-policy name.
    text* index_name;
    /// Zero-based physical shard number.
    std::int32_t shard_idx;
    /// Physical index relation OID.
    Oid index_relid;
    /// Persisted payload written to the shard in bytes.
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
