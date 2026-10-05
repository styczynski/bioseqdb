/**
 * @file catalog_views.cppm
 * @brief SQL views showing Bioseq storage metadata.
 */
export module bioseq.postgres.sql.catalog_views;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::sql_entities::catalog_views {

/**
 * Show sequence shards, their relation names, and stored byte counts.
 * A missing pg_class entry leaves the corresponding relation name NULL.
 *
 * @remark website.field database_oid: OID of the database containing the parent table.
 * @remark website.field tablespace_oid: OID of the tablespace containing the parent table.
 * @remark website.field main_relfilenode: Physical file number identifying the parent table.
 * @remark website.field column_attno: One-based attribute number of the parent sequence column.
 * @remark website.field bucket_idx: Length-bucket index containing this sequence shard.
 * @remark website.field shard_idx: Shard number within the length bucket.
 * @remark website.field shard_relation_name: Name of the sequence-fragment relation, or NULL if its pg_class entry is missing.
 * @remark website.field sidecar_relation_name: Name of the sidecar relation, or NULL if its pg_class entry is missing.
 * @remark website.field bytes_written: Number of payload bytes written to the shard.
 * @remark website.field payload_pretty_size: Human-readable formatting of bytes_written.
 * @remark website.field is_full: Whether new writes must be directed to a later shard.
 * @remark website.field total_payload_bytes: Recorded total payload size for the shard, in bytes.
 *
 * ```sql
 * -- Inspect sequence shards and their payload sizes without resolving relation OIDs manually.
 * SELECT shard_relation_name, sidecar_relation_name,
 *        payload_pretty_size, is_full
 * FROM bioseq.seq_sub_table_summary
 * ORDER BY database_oid, main_relfilenode, column_attno, bucket_idx, shard_idx;
 * ```
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
/** Map Bioseq tables to storage IDs when the current user can use their owner  role
 *
 * @remark website.field relation: Bioseq table whose owner role is available to the current user.
 * @remark website.field storage_id: Current columnar storage generation of the table.
 *
 * ```sql
 * -- List Bioseq tables whose owner role is available to the current user.
 * SELECT relation, storage_id
 * FROM bioseq.storage
 * ORDER BY relation;
 * ```
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
/** Show storage options for tables whose owner role is available to the, current user.
 *
 * @remark website.field relation: User table whose owner role is available to the current user.
 * @remark website.field chunk_group_row_limit: Maximum number of rows in one chunk group.
 * @remark website.field stripe_row_limit: Maximum number of rows in one stripe.
 * @remark website.field compression_level: Compression level used for column value streams.
 * @remark website.field compression: Name of the column compression algorithm.
 *
 * ```sql
 * -- Inspect row limits and compression for tables visible to the current user.
 * SELECT relation, chunk_group_row_limit, stripe_row_limit,
 *        compression, compression_level
 * FROM bioseq.options
 * ORDER BY relation;
 * ```
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
/** Show stripe sizes and row ranges for the tables visible through bioseq.storage.
 *
 * @remark website.field relation: User table visible through bioseq.storage.
 * @remark website.field storage_id: Storage generation containing this stripe.
 * @remark website.field stripe_num: Identifier of the stripe within the storage generation.
 * @remark website.field file_offset: Byte offset of the stripe data in columnar storage.
 * @remark website.field data_length: Length of the stripe data in bytes.
 * @remark website.field column_count: Number of columns stored in the stripe.
 * @remark website.field chunk_row_count: Configured row count for each chunk group in the stripe.
 * @remark website.field row_count: Total number of rows stored in the stripe.
 * @remark website.field chunk_group_count: Number of chunk groups stored in the stripe.
 * @remark website.field first_row_number: First logical row number assigned to the stripe.
 *
 * ```sql
 * -- Compare row counts and stored bytes for each visible table stripe.
 * SELECT relation, stripe_num, row_count, data_length
 * FROM bioseq.stripe
 * ORDER BY relation, stripe_num;
 * ```
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
/** Show eamh stripe's row groups for the tables visible through bioseq.storage.
 *
 * @remark website.field relation: User table visible through bioseq.storage.
 * @remark website.field storage_id: Storage generation containing this chunk group.
 * @remark website.field stripe_num: Stripe containing this chunk group.
 * @remark website.field chunk_group_num: Identifier of the chunk group within its stripe.
 * @remark website.field row_count: Total number of rows in the chunk group.
 *
 * ```sql
 * -- Inspect the row groups belonging to each visible table stripe.
 * SELECT relation, stripe_num, chunk_group_num, row_count
 * FROM bioseq.chunk_group
 * ORDER BY relation, stripe_num, chunk_group_num;
 * ```
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
/** Show column chunk statistics and compression for tables visible through bioseq.storage.
 *
 * @remark website.field relation: User table visible through bioseq.storage.
 * @remark website.field storage_id: Storage generation containing this column chunk.
 * @remark website.field stripe_num: Stripe containing this column chunk.
 * @remark website.field attr_num: PostgreSQL attribute number of the stored column.
 * @remark website.field chunk_group_num: Chunk group containing this column chunk.
 * @remark website.field minimum_value: Serialized minimum value used to skip chunks during filtering, when available.
 * @remark website.field maximum_value: Serialized maximum value used to skip chunks during filtering, when available.
 * @remark website.field value_stream_offset: Byte offset locating the stored value stream.
 * @remark website.field value_stream_length: Length of the stored value stream in bytes.
 * @remark website.field exists_stream_offset: Byte offset locating the presence-bit stream for NULL handling.
 * @remark website.field exists_stream_length: Length of the presence-bit stream in bytes.
 * @remark website.field value_compression_type: Numeric identifier of the value-stream compression algorithm.
 * @remark website.field value_compression_level: Compression level used for the value stream.
 * @remark website.field value_decompressed_length: Size of the value buffer before compression, in bytes.
 * @remark website.field value_count: Number of rows represented by this column chunk.
 *
 * ```sql
 * -- Compare stored and decompressed value-stream sizes for visible column chunks.
 * SELECT relation, attr_num, chunk_group_num,
 *        value_stream_length, value_decompressed_length
 * FROM bioseq.chunk
 * ORDER BY relation, stripe_num, attr_num, chunk_group_num;
 * ```
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
