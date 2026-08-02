module;

#include <climits>

export module bioseq.sql_guc_settings;

import bioseq.codegen.sql.annotation_model;
import bioseq.config.guc_storage;

export namespace bioseq {

enum class CompressionType : int {
    invalid [[bioseq_sql::guc_value({.omit = true})]] = -1,
    none = 0,
    pglz = 1,
#if defined(HAVE_CITUS_LIBLZ4) && HAVE_CITUS_LIBLZ4
    lz4 = 2,
#endif
#if defined(HAVE_LIBZSTD) && HAVE_LIBZSTD
    zstd = 3,
#endif
    count [[bioseq_sql::guc_value({.omit = true})]] = 4,
};

enum class PaperIndexFailure : int {
    none = 0,
    unsupported_vector_shape = 1,
};

enum class IndexPayloadStorageBackend : int {
    postgres_relation = 0,
    mapped_file = 1,
};

enum class PairRunSortImpl : int {
    std_sort = 0,
    simd_u128_vqsort = 1,
    simd_u128_vec_stream_1 = 2,
    simd_u128_vec_stream_2 = 3,
    simd_u128_vec_stream_4 = 4,
};

enum class PlannerDebugLevel : int {
    debug5 = 10,
    debug4 = 11,
    debug3 = 12,
    debug2 [[bioseq_sql::guc_value({
        .aliases = bioseq::sql::guc_aliases({
            .spelling = "debug",
            .hidden = true,
        }),
    })]] = 13,
    debug1 = 14,
    log = 15,
    info = 17,
    notice = 18,
    warning = 19,
};

inline constexpr int stripe_row_count_minimum = 1'000;
inline constexpr int stripe_row_count_maximum = 100'000'000;
inline constexpr int chunk_row_count_minimum = 1'000;
inline constexpr int chunk_row_count_maximum = 100'000'000;
inline constexpr int compression_level_minimum = 1;
inline constexpr int compression_level_maximum = 19;
inline constexpr int default_stripe_row_count = 150'000;
inline constexpr int default_chunk_row_count = 10'000;

#if defined(HAVE_LIBZSTD) && HAVE_LIBZSTD
inline constexpr CompressionType default_compression_type =
    CompressionType::zstd;
#elif defined(HAVE_CITUS_LIBLZ4) && HAVE_CITUS_LIBLZ4
inline constexpr CompressionType default_compression_type =
    CompressionType::lz4;
#else
inline constexpr CompressionType default_compression_type =
    CompressionType::pglz;
#endif

struct settings final {
    settings() = delete;

    /** @brief Enables extension diagnostic logging. */
    [[bioseq_sql::guc({
        .context = bioseq::sql::guc_context::superuser,
    })]]
    static inline bool logging = false;

    /** @brief Routes sequence input through the synthetic chunked scanner. */
    [[bioseq_sql::guc({
        .context = bioseq::sql::guc_context::superuser,
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool test_force_chunked_scanner = false;

    /** @brief Materializes sequence input before scanner dispatch. */
    [[bioseq_sql::guc({
        .context = bioseq::sql::guc_context::superuser,
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool test_force_inmemory_scanner = false;

    /** @brief Selects the compression method used for column chunks. */
    [[bioseq_sql::guc]]
    static inline bioseq::pg_utils::postgres_enum_storage<CompressionType>
        compression =
            bioseq::pg_utils::postgres_enum_storage<CompressionType>{
                default_compression_type};

    /** @brief Sets the compression level used for compressed column chunks. */
    [[bioseq_sql::guc({
        .minimum = compression_level_minimum,
        .maximum = compression_level_maximum,
    })]]
    static inline int compression_level = 3;

    /** @brief Limits the number of rows stored in one stripe. */
    [[bioseq_sql::guc({
        .minimum = stripe_row_count_minimum,
        .maximum = stripe_row_count_maximum,
    })]]
    static inline int stripe_row_limit = default_stripe_row_count;

    /** @brief Limits the number of rows stored in one chunk group. */
    [[bioseq_sql::guc({
        .minimum = chunk_row_count_minimum,
        .maximum = chunk_row_count_maximum,
    })]]
    static inline int chunk_group_row_limit = default_chunk_row_count;

    /** @brief Allows execution to use parallel workers. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool enable_parallel_execution = true;

    /** @brief Sets the minimum worker count for parallel execution. */
    [[bioseq_sql::guc({
        .minimum = 1,
        .maximum = 32,
    })]]
    static inline int min_parallel_processes = 8;

    /** @brief Enables vectorized execution paths. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool enable_vectorization = true;

    /** @brief Enables data-modification operations on BioSeq tables. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool enable_dml = true;

    /** @brief Enables the column page cache. */
    [[bioseq_sql::guc]]
    static inline bool enable_column_cache = false;

    /** @brief Sets the column page cache capacity in megabytes. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::unit_mb,
        .minimum = 20,
        .maximum = 20'000,
    })]]
    static inline int column_cache_size = 200;

    /** @brief Enables BioSeq index scan paths. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool enable_bioseq_index_scan = true;

    /** @brief Maintains update-on-insert indexes when sequence values are written. */
    [[bioseq_sql::guc]]
    static inline bool update_index_on_insert = true;

    /** @brief Reads stored per-sequence indexes when they are available. */
    [[bioseq_sql::guc({
        .plan_cache = bioseq::sql::plan_cache_policy::reset,
    })]]
    static inline bool read_sequence_index = true;

    /**
     * @brief Overrides the external-sort run buffer size.
     *
     * Zero uses the computed production size.
     */
    [[bioseq_sql::guc({
        .context = bioseq::sql::guc_context::superuser,
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
        .minimum = 0,
        .maximum = INT_MAX,
    })]]
    static inline int index_run_buffer_bytes = 0;

    /** @brief Overlaps input decoding with column writes during loading. */
    [[bioseq_sql::guc]]
    static inline bool load_overlap = false;

    /** @brief Persists opportunistically built indexes asynchronously. */
    [[bioseq_sql::guc]]
    static inline bool index_async_persist = true;

    /** @brief Persists an index asynchronously after a no-index recomputation. */
    [[bioseq_sql::guc]]
    static inline bool recompute_async_persist = false;

    /**
     * @brief Caps resident index memory in megabytes.
     *
     * Zero uses the runtime memory budget.
     */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::unit_mb,
        .minimum = 0,
        .maximum = INT_MAX,
    })]]
    static inline int index_memory_cap_mb = 0;

#if defined(BIOSEQ_TEST_INDEXES)
    /** @brief Injects the selected paper-index adapter failure. */
    [[bioseq_sql::guc({
        .context = bioseq::sql::guc_context::superuser,
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bioseq::pg_utils::postgres_enum_storage<PaperIndexFailure>
        test_force_paper_index_failure =
            bioseq::pg_utils::postgres_enum_storage<PaperIndexFailure>{
                PaperIndexFailure::none};
#endif

    /** @brief Selects the storage provider for persisted index payloads. */
    [[bioseq_sql::guc]]
    static inline
        bioseq::pg_utils::postgres_enum_storage<IndexPayloadStorageBackend>
            index_payload_storage_backend =
                bioseq::pg_utils::postgres_enum_storage<
                    IndexPayloadStorageBackend>{
                    IndexPayloadStorageBackend::postgres_relation};

    /** @brief Selects the sorter used for minimizer pair runs. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bioseq::pg_utils::postgres_enum_storage<PairRunSortImpl>
        mps_pair_sort_impl =
            bioseq::pg_utils::postgres_enum_storage<PairRunSortImpl>{
                PairRunSortImpl::std_sort};

    /** @brief Enables custom scan paths for BioSeq tables. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool enable_custom_scan = true;

    /** @brief Pushes supported filter predicates into custom scans. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool enable_qual_pushdown = true;

    /** @brief Sets the minimum column correlation used for predicate pushdown. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
        .minimum = 0,
        .maximum = 1,
    })]]
    static inline double qual_pushdown_correlation_threshold = 0.4;

    /** @brief Limits custom scan alternatives generated for one relation. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
        .minimum = 1,
        .maximum = 1'024,
    })]]
    static inline int max_custom_scan_paths = 64;

    /** @brief Sets the message level for planner diagnostics. */
    [[bioseq_sql::guc]]
    static inline bioseq::pg_utils::postgres_enum_storage<PlannerDebugLevel>
        planner_debug_level =
            bioseq::pg_utils::postgres_enum_storage<PlannerDebugLevel>{
                PlannerDebugLevel::debug3};

    /** @brief Routes sequence ordering through the batch-sort custom scan. */
    [[bioseq_sql::guc({
        .flags = bioseq::sql::guc_flag::no_show_all
            | bioseq::sql::guc_flag::not_in_sample,
    })]]
    static inline bool enable_sequence_sort_scan = false;
};

}  // namespace bioseq
