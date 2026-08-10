module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <type_traits>

export module bioseq.postgres.storage.indexing.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::storage::indexing::catalog_rows {

// Per-column index policy persisted by the extension.
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
    Oid user_relid;
    Oid main_relfilenode;
    std::int16_t column_attno;
    text* index_name;
    bool enabled = true;
    bool update_on_insert = false;
    bool is_active_read = false;
};

// Last successful refresh time for each persisted index.
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
    Oid user_relid;
    Oid main_relfilenode;
    std::int16_t column_attno;
    text* index_name;

    [[bioseq_sql::column({
        .default_value = bioseq::sql::checked_sql{"now()"},
    })]]
    TimestampTz last_updated;
};

// Physical relation allocated to one table-index shard.
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
    Oid database_oid;
    Oid tablespace_oid;
    Oid main_relfilenode;
    std::int16_t column_attno;
    text* index_name;
    std::int32_t shard_idx;
    Oid index_relid;
    std::int64_t bytes_written = 0;
    bool is_full = false;
};

static_assert(std::is_aggregate_v<index_policy>);
static_assert(std::is_aggregate_v<metadata_age_log>);
static_assert(std::is_aggregate_v<index_table_shard>);
static_assert(std::is_standard_layout_v<index_policy>);
static_assert(std::is_standard_layout_v<metadata_age_log>);
static_assert(std::is_standard_layout_v<index_table_shard>);

}  // namespace bioseq::storage::indexing::catalog_rows
