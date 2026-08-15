/**
 * @file functions.cppm
 * @brief Declares generated SQL function bodies owned by the PostgreSQL extension.
 */
module;

#include <cstdint>
#include <optional>
#include <tuple>

export module bioseq.postgres.sql.functions;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.sql.functions.sql_rows;
import bioseq.postgres.table.sql_rows;

export namespace bioseq::sql_functions {

/**
 * @brief Lists storage shard and sidecar relations for a Bioseq table.
 * @return The checked SQL body for the set-returning function.
 */
[[bioseq_sql::sql_function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::stable,
})]]
[[bioseq_sql::grant({})]]
[[nodiscard]]
bioseq::sql::sql_function_body<
    bioseq::postgres::support::table_rows<
        bioseq::sql_functions::result_rows::list_seq_storage_shard>>
list_seq_storage_shards(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::regclass,
        })]]
        std::optional<std::uint32_t> rel) {
    static_cast<void>(rel);
    return {
        .body = bioseq::sql::checked_sql{R"SQL(
WITH parent AS (
    SELECT
        (SELECT oid FROM pg_database WHERE datname = current_database())
            AS db_oid,
        COALESCE(NULLIF(c.reltablespace, 0),
                 (SELECT dattablespace FROM pg_database
                  WHERE datname = current_database()))
            AS spc_oid,
        c.relfilenode AS rfn
    FROM pg_class c
    WHERE c.oid = rel
)
SELECT
    s.sub_table_relid::regclass AS sub_relation,
    s.sidecar_relid::regclass AS sidecar_relation,
    s.column_attno,
    s.bucket_idx,
    s.shard_idx,
    s.bytes_written,
    s.is_full
FROM bioseq_internal.seq_sub_table s, parent p
WHERE s.database_oid = p.db_oid
  AND s.tablespace_oid = p.spc_oid
  AND s.main_relfilenode = p.rfn
ORDER BY s.column_attno, s.bucket_idx, s.shard_idx;
)SQL"},
    };
}

/**
 * @brief Builds an aligner-options value from nullable per-call settings.
 * @return The checked SQL body that constructs the composite value.
 */
[[bioseq_sql::sql_function({
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
})]]
[[nodiscard]]
bioseq::sql::sql_function_body<
    bioseq::table::composite_rows::bioseq_aligner_options>
aligner_options(
        std::optional<std::int32_t> kmer_size = std::nullopt,
        std::optional<std::int32_t> window_size = std::nullopt,
        std::optional<std::int32_t> max_gap = std::nullopt,
        std::optional<std::int32_t> max_gap_ref = std::nullopt,
        std::optional<std::int32_t> bandwidth = std::nullopt,
        std::optional<std::int32_t> bandwidth_long = std::nullopt,
        std::optional<std::int32_t> max_chain_skip = std::nullopt,
        std::optional<std::int32_t> max_chain_iter = std::nullopt,
        std::optional<std::int32_t> min_seed_count = std::nullopt,
        std::optional<std::int32_t> min_chain_score = std::nullopt,
        std::optional<double> chain_gap_scale = std::nullopt,
        std::optional<double> chain_skip_scale = std::nullopt,
        std::optional<double> mask_level = std::nullopt,
        std::optional<std::int32_t> mask_len = std::nullopt,
        std::optional<double> pri_ratio = std::nullopt,
        std::optional<std::int32_t> best_n = std::nullopt,
        std::optional<std::int32_t> match_score = std::nullopt,
        std::optional<std::int32_t> mismatch_pen = std::nullopt,
        std::optional<std::int32_t> gap_open1 = std::nullopt,
        std::optional<std::int32_t> gap_ext1 = std::nullopt,
        std::optional<std::int32_t> gap_open2 = std::nullopt,
        std::optional<std::int32_t> gap_ext2 = std::nullopt,
        std::optional<std::int32_t> sc_ambi = std::nullopt,
        std::optional<std::int32_t> zdrop = std::nullopt,
        std::optional<std::int32_t> end_bonus = std::nullopt,
        std::optional<std::int32_t> max_occ = std::nullopt,
        std::optional<std::int32_t> mid_occ = std::nullopt,
        std::optional<std::int32_t> block_min_size = std::nullopt,
        std::optional<std::int32_t> block_max_size = std::nullopt,
        std::optional<std::int32_t> chaining_algorithm =
            std::nullopt,
        std::optional<std::int32_t> base_alignment_algorithm =
            std::nullopt,
        std::optional<std::int32_t> minimizer_algorithm =
            std::nullopt,
        std::optional<bool> vulkan_enabled = std::nullopt,
        std::optional<bool> vulkan_seed_hits_enabled =
            std::nullopt,
        std::optional<bool> vulkan_index_cache_enabled =
            std::nullopt,
        std::optional<std::int32_t> index_read_requirement =
            std::nullopt) {
    static_cast<void>(std::tie(
        kmer_size,
        window_size,
        max_gap,
        max_gap_ref,
        bandwidth,
        bandwidth_long,
        max_chain_skip,
        max_chain_iter,
        min_seed_count,
        min_chain_score,
        chain_gap_scale,
        chain_skip_scale,
        mask_level,
        mask_len,
        pri_ratio,
        best_n,
        match_score,
        mismatch_pen,
        gap_open1,
        gap_ext1,
        gap_open2,
        gap_ext2,
        sc_ambi,
        zdrop,
        end_bonus,
        max_occ,
        mid_occ,
        block_min_size,
        block_max_size,
        chaining_algorithm,
        base_alignment_algorithm,
        minimizer_algorithm,
        vulkan_enabled,
        vulkan_seed_hits_enabled,
        vulkan_index_cache_enabled,
        index_read_requirement));
    return {
        .body = bioseq::sql::checked_sql{R"SQL(
SELECT
    kmer_size, window_size, max_gap, max_gap_ref, bandwidth, bandwidth_long,
    max_chain_skip, max_chain_iter, min_seed_count, min_chain_score,
    chain_gap_scale, chain_skip_scale, mask_level, mask_len, pri_ratio,
    best_n, match_score, mismatch_pen, gap_open1, gap_ext1, gap_open2,
    gap_ext2, sc_ambi, zdrop, end_bonus, max_occ, mid_occ,
    block_min_size, block_max_size,
    chaining_algorithm, base_alignment_algorithm, minimizer_algorithm,
    vulkan_enabled, vulkan_seed_hits_enabled, vulkan_index_cache_enabled,
    index_read_requirement
)SQL"},
    };
}

}  // namespace bioseq::sql_functions
