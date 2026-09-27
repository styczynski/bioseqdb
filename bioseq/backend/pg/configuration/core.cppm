/**
 * @file core.cppm
 * @brief PostgreSQL settings for Bioseq storage, query planning and execution.
 */
module;

extern "C" {
#include "postgres.h"
}

#include <limits>

export module bioseq.postgres.configuration.core;

import bioseq.codegen.sql.annotation_model;
import bioseq.indexing.type;
import bioseq.postgres.support.enum_storage;

export namespace bioseq {

/** Selects the compression algorithm used for column chunks. */
enum class compression_algorithm : int {
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

#if defined(BIOSEQ_TEST_INDEXES)
/** Selects a test-only failure injected into persisted-index operations. */
enum class persisted_index_failure_mode : int {
    none = 0,
    unsupported_vector_shape = 1,
};
#endif

/** Selects the PostgreSQL severity used for planner diagnostics. */
enum class planner_log_level : int {
    debug5 = 10,
    debug4 = 11,
    debug3 = 12,
    debug2 = 13,
    debug1 = 14,
    log = 15,
    info = 17,
    notice = 18,
    warning = 19,
};

/** Smallest allowed stripe row-limit setting. */
inline constexpr int k_minimum_stripe_row_count = 1'000;

/** Largest allowed stripe row-limit setting. */
inline constexpr int k_maximum_stripe_row_count = 100'000'000;

/** Smallest allowed chunk-group row-limit setting. */
inline constexpr int k_minimum_chunk_group_row_count = 1'000;

/** Largest allowed chunk-group row-limit setting. */
inline constexpr int k_maximum_chunk_group_row_count = 100'000'000;

/** Minimum supported column-compression level. */
inline constexpr int k_minimum_compression_level = 1;

/** Maximum supported column-compression level. */
inline constexpr int k_maximum_compression_level = 19;

/** Default row limit for a stripe. */
inline constexpr int k_default_stripe_row_count = 150'000;

/** Default row limit for a chunk group. */
inline constexpr int k_default_chunk_group_row_count = 10'000;

#if defined(HAVE_LIBZSTD) && HAVE_LIBZSTD
/** Use ZSTD by default when this build supports it. */
inline constexpr compression_algorithm k_default_compression_algorithm =
    compression_algorithm::zstd;
#elif defined(HAVE_CITUS_LIBLZ4) && HAVE_CITUS_LIBLZ4
/** Use LZ4 by default when ZSTD is unavailable. */
inline constexpr compression_algorithm k_default_compression_algorithm = compression_algorithm::lz4;
#else
/** Use PostgreSQL PGLZ when neither optional codec is available. */
inline constexpr compression_algorithm k_default_compression_algorithm =
    compression_algorithm::pglz;
#endif

/** PostgreSQL settings shared by Bioseq modules. */
struct settings final {
    settings() = delete;

    /** Extension diagnostics. */
    struct diagnostics final {
        diagnostics() = delete;

        /** Diagnostic logging controls. */
        struct logging final {
            logging() = delete;

            /** @brief Enables extension diagnostic logging. */
            [[bioseq_sql::guc({
                .context = bioseq::sql::guc_context::superuser,
            })]]
            static inline bool enabled = false;
        };
    };

#if defined(BIOSEQ_TEST_INDEXES)
    /** Scanner and failure controls available in test builds. */
    struct testing final {
        testing() = delete;

        /** Test setting for the chunked-scanner comparison. */
        struct chunked_scanner final {
            chunked_scanner() = delete;

            /** @brief Materialize sequence payloads for scanner tests.
             * Recipe-backed batches keep their source readers.
             */
            [[bioseq_sql::guc({
                .context = bioseq::sql::guc_context::superuser,
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = false;
        };

        /** Test setting for scans with materialized sequence input. */
        struct in_memory_scanner final {
            in_memory_scanner() = delete;

            /** @brief Materialize sequence input before dispatch in test scans.
             * Recipe-backed batches keep their source readers.
             */
            [[bioseq_sql::guc({
                .context = bioseq::sql::guc_context::superuser,
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = false;
        };

        /** Failures injected into stored-index operations for tests. */
        struct persisted_index final {
            persisted_index() = delete;

            /** @brief Injects the selected persisted-index adapter failure. */
            [[bioseq_sql::guc({
                .context = bioseq::sql::guc_context::superuser,
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bioseq::postgres::support::postgres_enum_storage<
                persisted_index_failure_mode> failure_mode =
                bioseq::postgres::support::postgres_enum_storage<persisted_index_failure_mode>{
                    persisted_index_failure_mode::none};
        };
    };
#endif

    /** Column storage settings. */
    struct storage final {
        storage() = delete;

        /** Compression applied to column chunks. */
        struct compression final {
            compression() = delete;

            /** @brief Selects the compression algorithm used for column chunks. */
            [[bioseq_sql::guc]]
            static inline bioseq::postgres::support::postgres_enum_storage<compression_algorithm>
                algorithm = bioseq::postgres::support::postgres_enum_storage<compression_algorithm>{
                    k_default_compression_algorithm};

            /** @brief ZSTD compression level. PGLZ and LZ4 ignore this setting. */
            [[bioseq_sql::guc({
                .minimum = k_minimum_compression_level,
                .maximum = k_maximum_compression_level,
            })]]
            static inline int level = 3;
        };

        /** Maximum size of a columnar stripe. */
        struct stripe final {
            stripe() = delete;

            /** @brief Limits the number of rows stored in one stripe. */
            [[bioseq_sql::guc({
                .minimum = k_minimum_stripe_row_count,
                .maximum = k_maximum_stripe_row_count,
            })]]
            static inline int row_limit = k_default_stripe_row_count;
        };

        /** Maximum size of a chunk group within a stripe. */
        struct chunk_group final {
            chunk_group() = delete;

            /** @brief Limits the number of rows stored in one chunk group. */
            [[bioseq_sql::guc({
                .minimum = k_minimum_chunk_group_row_count,
                .maximum = k_maximum_chunk_group_row_count,
            })]]
            static inline int row_limit = k_default_chunk_group_row_count;
        };

        /** Visibility of rows marked as deleted. */
        struct dml final {
            dml() = delete;

            /** @brief Apply deletion masks when scanning Bioseq tables. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;
        };
    };

    /** Query execution settings. */
    struct execution final {
        execution() = delete;

        /** PostgreSQL workers used for parallel column scans. */
        struct parallel final {
            parallel() = delete;

            /** @brief Allow parallel PostgreSQL plans for Bioseq column scans. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;

            /** @brief Number of workers requested for a parallel column scan. */
            [[bioseq_sql::guc({
                .minimum = 1,
                .maximum = 32,
            })]]
            static inline int minimum_processes = 8;
        };
    };

    /** Batch processing of column values. */
    struct vectorization final {
        vectorization() = delete;

        /** @brief Allow supported expressions and aggregates to process column batches. */
        [[bioseq_sql::guc({
            .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
        })]]
        static inline bool enabled = true;
    };

    /** Cache of decompressed column chunks. */
    struct column_cache final {
        column_cache() = delete;

        /** @brief Reuse decompressed column chunks within the backend. */
        [[bioseq_sql::guc]]
        static inline bool enabled = true;

        /** @brief Memory limit for cached column chunks, in MiB. */
        [[bioseq_sql::guc({
            .flags = bioseq::sql::guc_flag::unit_mb,
            .minimum = 20,
            .maximum = 20'000,
        })]]
        static inline int capacity_mb = 200;
    };

    /** Bioseq query-planning settings. */
    struct planner final {
        planner() = delete;

        /** Bioseq scans through PostgreSQL indexes. */
        struct index_scan final {
            index_scan() = delete;

            /** @brief Enables Bioseq index scan paths. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;
        };

        /** Bioseq custom scans and their planner limits. */
        struct custom_scan final {
            custom_scan() = delete;

            /** @brief Enables custom scan paths for Bioseq tables. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;

            /** @brief Limits custom scan alternatives generated for one relation. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
                .minimum = 1,
                .maximum = 1'024,
            })]]
            static inline int max_paths = 64;
        };

        /** Filtering rows and skipping chunks inside a custom scan. */
        struct qual_pushdown final {
            qual_pushdown() = delete;

            /** @brief Pushes supported filter predicates into custom scans. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;

            /** @brief Minimum absolute column correlation for chunk-filter pushdown.
             * Columns ordered with their row positions are useful for min/max pruning.
             */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
                .minimum = 0,
                .maximum = 1,
            })]]
            static inline double correlation_threshold = 0.4;
        };

        /** Messages emitted while planning Bioseq scans. */
        struct diagnostics final {
            diagnostics() = delete;

            /** @brief Sets the message level for planner diagnostics. */
            [[bioseq_sql::guc]]
            static inline bioseq::postgres::support::postgres_enum_storage<planner_log_level>
                log_level = bioseq::postgres::support::postgres_enum_storage<planner_log_level>{
                    planner_log_level::debug3};
        };

        /** Batch sorting of sequence values in a custom scan. */
        struct sequence_sort_scan final {
            sequence_sort_scan() = delete;

            /** @brief Routes sequence ordering through the batch-sort custom scan. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = false;
        };
    };

    /** Reading and building sequence indexes. */
    struct index final {
        index() = delete;

        /** Index writes performed during INSERT. */
        struct insert_maintenance final {
            insert_maintenance() = delete;

            /**
             * @brief Update indexes during INSERT using the older index format.
             *
             * Grouped minimizer indexes require bioseq.update_index after the
             * source rows commit. This INSERT writer cannot publish that format.
             * Rows without stored indexes remain searchable using temporary
             * indexes built by the selected alignment algorithm.
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /** Reading an index stored for one sequence. */
        struct sequence_read final {
            sequence_read() = delete;

            /** @brief Reads stored per-sequence indexes when they are available. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = true;
        };

        /** Saving indexes in the background. */
        struct async_persistence final {
            async_persistence() = delete;

            /**
             * @brief Save indexes built during searches in the background.
             *
             * Disabled by default. The background writer cannot publish grouped
             * minimizer generations, which require bioseq.update_index.
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /** Saving rebuilt indexes in the background. */
        struct recompute_async_persistence final {
            recompute_async_persistence() = delete;

            /** @brief Persists an index asynchronously after recomputation. */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /**
         * @brief Limit memory used by loaded indexes, in MiB.
         * Zero lets the runtime choose its memory budget.
         */
        [[bioseq_sql::guc({
            .flags = bioseq::sql::guc_flag::unit_mb,
            .minimum = 0,
            .maximum = std::numeric_limits<int>::max(),
        })]]
        static inline int memory_cap_mb = 0;

        /** Sorting minimizer/sequence pairs for an index. */
        struct sequence_minimizer_sort final {
            sequence_minimizer_sort() = delete;

            /** @brief Selects the sorter used for minimizer sequence pairs. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bioseq::postgres::support::postgres_enum_storage<
                bioseq::indexing::pair_run_sort_implementation> algorithm =
                bioseq::postgres::support::postgres_enum_storage<
                    bioseq::indexing::pair_run_sort_implementation>{
                    bioseq::indexing::pair_run_sort_implementation::std_sort};
        };
    };

    /** Building stored sequence indexes. */
    struct index_build final {
        index_build() = delete;

        /**
         * @brief Size in bytes of an in-memory run for external sorting.
         * Zero keeps the index builder's default buffer size.
         */
        [[bioseq_sql::guc({
            .context = bioseq::sql::guc_context::superuser,
            .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            .minimum = 0,
            .maximum = std::numeric_limits<int>::max(),
        })]]
        static inline int run_buffer_bytes = 0;
    };

    /** Loading sequences from input files. */
    struct load final {
        load() = delete;

        /** Decoding the next input batch while writing the current batch. */
        struct write_overlap final {
            write_overlap() = delete;

            /** @brief Allow input decoding to overlap column writes.
             * Requires a scheduler mode and worker count that allow parallel dispatch.
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };
    };
};

} // namespace bioseq
