/**
 * @file core.cppm
 * @brief Declares PostgreSQL settings shared across Bioseq subsystems.
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

/** Minimum number of rows stored in one stripe. */
inline constexpr int k_minimum_stripe_row_count = 1'000;

/** Maximum number of rows stored in one stripe. */
inline constexpr int k_maximum_stripe_row_count = 100'000'000;

/** Minimum number of rows stored in one chunk group. */
inline constexpr int k_minimum_chunk_group_row_count = 1'000;

/** Maximum number of rows stored in one chunk group. */
inline constexpr int k_maximum_chunk_group_row_count = 100'000'000;

/** Minimum supported column-compression level. */
inline constexpr int k_minimum_compression_level = 1;

/** Maximum supported column-compression level. */
inline constexpr int k_maximum_compression_level = 19;

/** Default number of rows stored in one stripe. */
inline constexpr int k_default_stripe_row_count = 150'000;

/** Default number of rows stored in one chunk group. */
inline constexpr int k_default_chunk_group_row_count = 10'000;

#if defined(HAVE_LIBZSTD) && HAVE_LIBZSTD
/** Best available column-compression algorithm for this build. */
inline constexpr compression_algorithm k_default_compression_algorithm =
    compression_algorithm::zstd;
#elif defined(HAVE_CITUS_LIBLZ4) && HAVE_CITUS_LIBLZ4
/** Best available column-compression algorithm for this build. */
inline constexpr compression_algorithm k_default_compression_algorithm = compression_algorithm::lz4;
#else
/** Best available column-compression algorithm for this build. */
inline constexpr compression_algorithm k_default_compression_algorithm =
    compression_algorithm::pglz;
#endif

/** Owns PostgreSQL settings shared across Bioseq subsystems. */
struct settings final {
    settings() = delete;

    /** Owns diagnostic settings. */
    struct diagnostics final {
        diagnostics() = delete;

        /** Owns diagnostic logging settings. */
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
    /** Owns test-support-only fault and route controls. */
    struct testing final {
        testing() = delete;

        /** Owns synthetic chunked-scanner selection. */
        struct chunked_scanner final {
            chunked_scanner() = delete;

            /** @brief Routes sequence input through the synthetic chunked scanner. */
            [[bioseq_sql::guc({
                .context = bioseq::sql::guc_context::superuser,
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = false;
        };

        /** Owns in-memory scanner selection. */
        struct in_memory_scanner final {
            in_memory_scanner() = delete;

            /** @brief Materializes sequence input before scanner dispatch. */
            [[bioseq_sql::guc({
                .context = bioseq::sql::guc_context::superuser,
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = false;
        };

        /** Owns persisted-index failure injection. */
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

    /** Owns column-storage settings. */
    struct storage final {
        storage() = delete;

        /** Owns column-compression settings. */
        struct compression final {
            compression() = delete;

            /** @brief Selects the compression algorithm used for column chunks. */
            [[bioseq_sql::guc]]
            static inline bioseq::postgres::support::postgres_enum_storage<compression_algorithm>
                algorithm = bioseq::postgres::support::postgres_enum_storage<compression_algorithm>{
                    k_default_compression_algorithm};

            /** @brief Sets the compression level used for compressed column chunks.
             */
            [[bioseq_sql::guc({
                .minimum = k_minimum_compression_level,
                .maximum = k_maximum_compression_level,
            })]]
            static inline int level = 3;
        };

        /** Owns stripe-size settings. */
        struct stripe final {
            stripe() = delete;

            /** @brief Limits the number of rows stored in one stripe. */
            [[bioseq_sql::guc({
                .minimum = k_minimum_stripe_row_count,
                .maximum = k_maximum_stripe_row_count,
            })]]
            static inline int row_limit = k_default_stripe_row_count;
        };

        /** Owns chunk-group-size settings. */
        struct chunk_group final {
            chunk_group() = delete;

            /** @brief Limits the number of rows stored in one chunk group. */
            [[bioseq_sql::guc({
                .minimum = k_minimum_chunk_group_row_count,
                .maximum = k_maximum_chunk_group_row_count,
            })]]
            static inline int row_limit = k_default_chunk_group_row_count;
        };

        /** Owns data-modification settings. */
        struct dml final {
            dml() = delete;

            /** @brief Enables data-modification operations on Bioseq tables. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;
        };
    };

    /** Owns execution settings. */
    struct execution final {
        execution() = delete;

        /** Owns parallel-execution settings. */
        struct parallel final {
            parallel() = delete;

            /** @brief Allows execution to use parallel workers. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;

            /** @brief Sets the minimum worker count for parallel execution. */
            [[bioseq_sql::guc({
                .minimum = 1,
                .maximum = 32,
            })]]
            static inline int minimum_processes = 8;
        };
    };

    /** Owns vectorized-execution settings. */
    struct vectorization final {
        vectorization() = delete;

        /** @brief Enables vectorized execution paths. */
        [[bioseq_sql::guc({
            .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
        })]]
        static inline bool enabled = true;
    };

    /** Owns column-cache settings. */
    struct column_cache final {
        column_cache() = delete;

        /** @brief Enables the column page cache. */
        [[bioseq_sql::guc]]
        static inline bool enabled = true;

        /** @brief Sets the column page cache capacity in megabytes. */
        [[bioseq_sql::guc({
            .flags = bioseq::sql::guc_flag::unit_mb,
            .minimum = 20,
            .maximum = 20'000,
        })]]
        static inline int capacity_mb = 200;
    };

    /** Owns planner settings. */
    struct planner final {
        planner() = delete;

        /** Owns Bioseq index-scan selection. */
        struct index_scan final {
            index_scan() = delete;

            /** @brief Enables Bioseq index scan paths. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;
        };

        /** Owns custom-scan selection and path limits. */
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

        /** Owns predicate-pushdown settings. */
        struct qual_pushdown final {
            qual_pushdown() = delete;

            /** @brief Pushes supported filter predicates into custom scans. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;

            /** @brief Sets the minimum column correlation used for predicate
             * pushdown. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
                .minimum = 0,
                .maximum = 1,
            })]]
            static inline double correlation_threshold = 0.4;
        };

        /** Owns planner diagnostic settings. */
        struct diagnostics final {
            diagnostics() = delete;

            /** @brief Sets the message level for planner diagnostics. */
            [[bioseq_sql::guc]]
            static inline bioseq::postgres::support::postgres_enum_storage<planner_log_level>
                log_level = bioseq::postgres::support::postgres_enum_storage<planner_log_level>{
                    planner_log_level::debug3};
        };

        /** Owns sequence-sort custom-scan selection. */
        struct sequence_sort_scan final {
            sequence_sort_scan() = delete;

            /** @brief Routes sequence ordering through the batch-sort custom scan. */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = false;
        };
    };

    /** Owns sequence-index settings. */
    struct index final {
        index() = delete;

        /** Owns insert-time index maintenance. */
        struct insert_maintenance final {
            insert_maintenance() = delete;

            /**
             * @brief Enables legacy insert-time index maintenance.
             *
             * The default grouped-SoA minimizer format is published after the
             * source rows commit, through bioseq.update_index. It cannot use
             * this scheme-untagged INSERT writer. Unindexed rows remain
             * searchable through the selected algorithm's transient indexes.
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /** Owns per-sequence index reads. */
        struct sequence_read final {
            sequence_read() = delete;

            /** @brief Reads stored per-sequence indexes when they are available. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = true;
        };

        /** Owns asynchronous index persistence. */
        struct async_persistence final {
            async_persistence() = delete;

            /**
             * @brief Persists opportunistically built indexes asynchronously.
             *
             * Off by default because grouped-SoA generations require explicit
             * publication; the legacy asynchronous writer cannot publish them.
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /** Owns asynchronous persistence after recomputation. */
        struct recompute_async_persistence final {
            recompute_async_persistence() = delete;

            /** @brief Persists an index asynchronously after recomputation. */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /**
         * @brief Caps resident index memory in megabytes.
         *
         * Zero uses the runtime memory budget.
         */
        [[bioseq_sql::guc({
            .flags = bioseq::sql::guc_flag::unit_mb,
            .minimum = 0,
            .maximum = std::numeric_limits<int>::max(),
        })]]
        static inline int memory_cap_mb = 0;

        /** Owns minimizer-sequence pair sorting. */
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

    /** Owns index-build settings. */
    struct index_build final {
        index_build() = delete;

        /**
         * @brief Overrides the external-sort run buffer size.
         *
         * Zero uses the computed production size.
         */
        [[bioseq_sql::guc({
            .context = bioseq::sql::guc_context::superuser,
            .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            .minimum = 0,
            .maximum = std::numeric_limits<int>::max(),
        })]]
        static inline int run_buffer_bytes = 0;
    };

    /** Owns sequence-loading settings. */
    struct load final {
        load() = delete;

        /** Owns overlap between decoding and storage writes. */
        struct write_overlap final {
            write_overlap() = delete;

            /** @brief Overlaps input decoding with column writes during loading. */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };
    };
};

} // namespace bioseq
