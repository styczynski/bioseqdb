module;

#include <bioseq/pg_compat.h>

#include <cstdint>
#include <type_traits>

export module bioseq.postgres.sql.functions.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::sql_functions::result_rows {

struct list_seq_storage_shard final {
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::regclass,
    })]]
    Oid sub_relation;

    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::regclass,
    })]]
    Oid sidecar_relation;

    std::int16_t column_attno;
    std::int16_t bucket_idx;
    std::int32_t shard_idx;
    std::int64_t bytes_written;
    bool is_full;
};

static_assert(std::is_aggregate_v<list_seq_storage_shard>);

}  // namespace bioseq::sql_functions::result_rows
