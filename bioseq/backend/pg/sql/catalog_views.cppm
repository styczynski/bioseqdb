/**
 * @file catalog_views.cppm
 * @brief Declares public SQL catalog views for Bioseq storage metadata.
 */
export module bioseq.postgres.sql.catalog_views;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::sql_entities::catalog_views {

/**
 * @brief Summarizes physical sequence-shard storage relations.
 * @remark Missing shard relations appear as NULL while concurrent DDL is in
 * flight.
 */
[[bioseq_sql::view({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
}),
  bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::select,
})]]
inline constexpr bioseq::sql::checked_sql seq_sub_table_summary{R"SQL(
SELECT
    s.database_oid,
    s.tablespace_oid,
    s.main_relfilenode,
    s.column_attno,
    s.bucket_idx,
    s.shard_idx,
    c1.relname AS shard_relation_name,
    c2.relname AS sidecar_relation_name,
    s.bytes_written,
    pg_size_pretty(s.bytes_written) AS payload_pretty_size,
    s.is_full,
    s.total_payload_bytes
FROM bioseq_internal.seq_sub_table s
LEFT JOIN pg_class c1 ON c1.oid = s.sub_table_relid
LEFT JOIN pg_class c2 ON c2.oid = s.sidecar_relid
ORDER BY
    s.database_oid,
    s.tablespace_oid,
    s.main_relfilenode,
    s.column_attno,
    s.bucket_idx,
    s.shard_idx
)SQL"};

[[bioseq_sql::view({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .security_barrier = true,
}),
  bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::select,
})]]
/**
 * @brief Bioseq relation ID to storage ID mapping.
 */
inline constexpr bioseq::sql::checked_sql storage{R"SQL(
SELECT
    c.oid::regclass AS relation,
    bioseq.get_storage_id(c.oid) AS storage_id
FROM pg_class c, pg_am am
WHERE c.relam = am.oid
  AND am.amname = 'bioseq'
  AND pg_has_role(c.relowner, 'USAGE')
)SQL"};

[[bioseq_sql::view({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .security_barrier = true,
}),
  bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::select,
})]]
/**
 * @brief Bioseq options for tables owned by the current user.
 */
inline constexpr bioseq::sql::checked_sql options{R"SQL(
SELECT
    regclass AS relation,
    chunk_group_row_limit,
    stripe_row_limit,
    compression,
    compression_level
FROM bioseq_internal.options o, pg_class c
WHERE o.regclass = c.oid
  AND pg_has_role(c.relowner, 'USAGE')
)SQL"};

[[bioseq_sql::view({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .security_barrier = true,
}),
  bioseq_sql::depends(
      bioseq::sql_entities::catalog_views::storage),
  bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::select,
})]]
/**
 * @brief Bioseq stripe information for tables owned by the current user.
 */
inline constexpr bioseq::sql::checked_sql stripe{R"SQL(
SELECT
    relation,
    storage.storage_id,
    stripe_num,
    file_offset,
    data_length,
    column_count,
    chunk_row_count,
    row_count,
    chunk_group_count,
    first_row_number
FROM bioseq_internal.stripe stripe, bioseq.storage storage
WHERE stripe.storage_id = storage.storage_id
)SQL"};

[[bioseq_sql::view({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .security_barrier = true,
}),
  bioseq_sql::depends(
      bioseq::sql_entities::catalog_views::storage),
  bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::select,
})]]
/**
 * @brief Bioseq chunk-group information for tables owned by the current user.
 */
inline constexpr bioseq::sql::checked_sql chunk_group{R"SQL(
SELECT
    relation,
    storage.storage_id,
    stripe_num,
    chunk_group_num,
    row_count
FROM bioseq_internal.chunk_group cg, bioseq.storage storage
WHERE cg.storage_id = storage.storage_id
)SQL"};

[[bioseq_sql::view({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .security_barrier = true,
}),
  bioseq_sql::depends(
      bioseq::sql_entities::catalog_views::storage),
  bioseq_sql::grant({
    .privileges = bioseq::sql::privileges::select,
})]]
/**
 * @brief Bioseq chunk information for tables owned by the current user.
 */
inline constexpr bioseq::sql::checked_sql chunk{R"SQL(
SELECT
    relation,
    storage.storage_id,
    stripe_num,
    attr_num,
    chunk_group_num,
    minimum_value,
    maximum_value,
    value_stream_offset,
    value_stream_length,
    exists_stream_offset,
    exists_stream_length,
    value_compression_type,
    value_compression_level,
    value_decompressed_length,
    value_count
FROM bioseq_internal.chunk chunk, bioseq.storage storage
WHERE chunk.storage_id = storage.storage_id
)SQL"};

}  // namespace bioseq::sql_entities::catalog_views
