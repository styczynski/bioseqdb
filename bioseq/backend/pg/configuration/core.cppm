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
    /// @brief Store column chunks without compression.
    none = 0,
    /// @brief Use Postgres built-in coded. This becomes the default when neither of previous
    /// two is available.
    pglz = 1,
#if defined(HAVE_CITUS_LIBLZ4) && HAVE_CITUS_LIBLZ4
    /// @brief Use LZ4 when compiled in. This becomes the default when Zstandard is
    /// unavailable.
    lz4 = 2,
#endif
#if defined(HAVE_LIBZSTD) && HAVE_LIBZSTD
    /// @brief Use Zstandard when compiled in. This is the preferred build-time default.
    zstd = 3,
#endif
    count [[bioseq_sql::guc_value({.omit = true})]] = 4,
};

#if defined(BIOSEQ_TEST_INDEXES)
/** Selects a test-only failure injected into persisted-index operations. */
enum class persisted_index_failure_mode : int {
    /// @brief Do nothing.
    none = 0,
    /// @brief Make the index reader report an unsupported vector shape (due to wrong w/k
    /// etc. parameters used during index build).
    unsupported_vector_shape = 1,
};
#endif

/** Selects the PostgreSQL severity used for planner diagnostics. */
enum class planner_log_level : int {
    /// @brief Emit them at DEBUG5 severity.
    debug5 = 10,
    /// @brief Emit them at DEBUG4 severity.
    debug4 = 11,
    /// @brief Emit them at DEBUG3 severity.
    debug3 = 12,
    /// @brief Emit them at DEBUG2 severity.
    debug2 = 13,
    /// @brief Emit them at DEBUG1 severity.
    debug1 = 14,
    /// @brief Emit them at LOG severity.
    log = 15,
    /// @brief Emit them at INFO severity.
    info = 17,
    /// @brief Emit them at NOTICE severity.
    notice = 18,
    /// @brief Emit them at WARNING severity.
    warning = 19,
};

/** Smallest allowed stripe row-limit setting. */
inline constexpr int k_minimum_stripe_row_count = 1'000;

/** Largest allowed stripe row-limit setting. */
inline constexpr int k_maximum_stripe_row_count = 100'000'000;

/** Smallest allowed chunk-group row-limit setting. */
inline constexpr int k_minimum_chunk_group_row_count = 1'000;

/** Largest allowed chunk-group row-limit setting */
inline constexpr int k_maximum_chunk_group_row_count = 100'000'000;

/** Minimum supported column-compression level. */
inline constexpr int k_minimum_compression_level = 1;

/** Maximum supported column-compression level. */
inline constexpr int k_maximum_compression_level = 19;

/** Default row limit for a stripe. */
inline constexpr int k_default_stripe_row_count = 150'000;

/** Default row  limit for a chunk group. */
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

        /** Diagnostic loggng controls. */
        struct logging final {
            logging() = delete;

            /** @brief Enables extension diagnostic logging.
             *
             * @remark Configuration: Enable or dissable internal logging. BioseqDB internally supports logging to file, but current builds are all configured to use only postgres logging mechanisms.
             * @remark Value `false`: Disable internal logger.
             * @remark Value `true`: Enable internal logger.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.diagnostics.logging.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state. Requires permission to change this superuser setting.
             * BEGIN;
             * SET LOCAL bioseq.diagnostics.logging.enabled = 'off';
             * SHOW bioseq.diagnostics.logging.enabled;
             * RESET bioseq.diagnostics.logging.enabled;
             * ROLLBACK;
             * ```
             */
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

            /** @brief Materialize, sequence payloads for scanner tests.
             * Recipe-backed batches keep their source readers.
             *
             * @remark Configuration: Forces BioseqDB to load entire sequence into memory and create 'fake' lazy-loading interface that accesses it. Used by me to test C++ interface for lazy-loading.
             * @remark Value `false`: Use the normal scanner.
             * @remark Value `true`: Force load entire sequence into memory and create lazy-loader interface for that RAM sequence.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.testing.chunked_scanner.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state. Requires permission to change this superuser setting.
             * BEGIN;
             * SET LOCAL bioseq.testing.chunked_scanner.enabled = 'off';
             * SHOW bioseq.testing.chunked_scanner.enabled;
             * RESET bioseq.testing.chunked_scanner.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .context = bioseq::sql::guc_context::superuser,
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = false;
        };

        /** Test setting  for scans with materialized sequence input. */
        struct in_memory_scanner final {
            in_memory_scanner() = delete;

            /** @brief Materialize sequence input before dispatch in test scans.
             * Recipe-backed batches keep their source readers.
             *
             * @remark Configuration: Forces BioseqDB to load entire sequence into memory and passes it directly to the pipeline. (Note that this is the same for user as bioseq.testing.chunked_scanner.enabled with the only difference being internally used C++ API). Used by me to test C++ interface for lazy-loading.
             * @remark Value `false`: Use the normal scanner.
             * @remark Value `true`: Force load entire sequence into memory and pass it directly to the pipeline.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.testing.in_memory_scanner.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state. Requires permission to change this superuser setting.
             * BEGIN;
             * SET LOCAL bioseq.testing.in_memory_scanner.enabled = 'off';
             * SHOW bioseq.testing.in_memory_scanner.enabled;
             * RESET bioseq.testing.in_memory_scanner.enabled;
             * ROLLBACK;
             * ```
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

            /** @brief Injects the selected persisted-index adapter failure.
             *
             * @remark Configuration: Cause 'fake' index reader failure to be reported during queries.
             * @remark Value `none`: Do nothing.
             * @remark Value `unsupported_vector_shape`: Make the index reader report an unsupported vector shape (due to wrong w/k etc. parameters used during index build).
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.testing.persisted_index.failure_mode;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state. Requires permission to change this superuser setting.
             * BEGIN;
             * SET LOCAL bioseq.testing.persisted_index.failure_mode = 'none';
             * SHOW bioseq.testing.persisted_index.failure_mode;
             * RESET bioseq.testing.persisted_index.failure_mode;
             * ROLLBACK;
             * ```
             */
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

            /** @brief Selects the compression algorithm used for column chunks.
             *
             * @remark Configuration: Choose the compression algorithm used for storing data columns Changing the default does not rewrite any existing indices!
             * @remark Value `zstd`: Use Zstandard when compiled in. This is the preferred build-time default.
             * @remark Value `lz4`: Use LZ4 when compiled in. This becomes the default when Zstandard is unavailable.
             * @remark Value `pglz`: Use Postgres built-in coded. This becomes the default when neither of previous two is available.
             * @remark Value `none`: Store column chunks without compression.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.storage.compression.algorithm;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.storage.compression.algorithm = DEFAULT;
             * SHOW bioseq.storage.compression.algorithm;
             * RESET bioseq.storage.compression.algorithm;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc]]
            static inline bioseq::postgres::support::postgres_enum_storage<compression_algorithm>
                algorithm = bioseq::postgres::support::postgres_enum_storage<compression_algorithm>{
                    k_default_compression_algorithm};

            /** @brief ZSTD compression level. PGLZ and LZ4 ignore this setting.
             *
             * @remark Configuration: Set the zstd compression level if zstd is used.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.storage.compression.level;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.storage.compression.level = '3';
             * SHOW bioseq.storage.compression.level;
             * RESET bioseq.storage.compression.level;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .minimum = k_minimum_compression_level,
                .maximum = k_maximum_compression_level,
            })]]
            static inline int level = 3;
        };

        /** Maximum size of a columnar stripe. */
        struct stripe final {
            stripe() = delete;

            /** @brief Limits the number of rows stored in one stripe.
             *
             * @remark Configuration: Sets the maximum rows in a new table's column stripe, when writing rows. Existing table options and stored stripes are unchanged.
             * @remark Display unit: rows
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.storage.stripe.row_limit;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.storage.stripe.row_limit = '150000';
             * SHOW bioseq.storage.stripe.row_limit;
             * RESET bioseq.storage.stripe.row_limit;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .minimum = k_minimum_stripe_row_count,
                .maximum = k_maximum_stripe_row_count,
            })]]
            static inline int row_limit = k_default_stripe_row_count;
        };

        /** Maximum size of a chunk group within a stripe. */
        struct chunk_group final {
            chunk_group() = delete;

            /** @brief Limits the number of rows stored in one chunk group.
             *
             * @remark Configuration: Sets the maximum rows in a new table's chunk group, when writing rows. Chunk groups divide a stripe into smaller column-reading units.
             * @remark Display unit: rows
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.storage.chunk_group.row_limit;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.storage.chunk_group.row_limit = '10000';
             * SHOW bioseq.storage.chunk_group.row_limit;
             * RESET bioseq.storage.chunk_group.row_limit;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .minimum = k_minimum_chunk_group_row_count,
                .maximum = k_maximum_chunk_group_row_count,
            })]]
            static inline int row_limit = k_default_chunk_group_row_count;
        };

        /** Visibility of rows marked as deleted. */
        struct dml final {
            dml() = delete;

            /** @brief Apply deletion masks when scanning Bioseq tables.
             *
             * @remark Configuration: Special setting that controls whether reads apply deleted-row masks.
             * @remark Value `true`: Default, shouldn't be changed. Normally deleted rows that are marked as such, should not be returned in queries.
             * @remark Value `false`: Bypass any deletion filtering. Useful for debugging, the queries will return all rows including deleted ones. Note that if the table is rewritten using VACCUM FULL, then deleted rows will physically dissapear!
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.storage.dml.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.storage.dml.enabled = 'on';
             * SHOW bioseq.storage.dml.enabled;
             * RESET bioseq.storage.dml.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;
        };
    };

    /** Query  execution settings. */
    struct execution final {
        execution() = delete;

        /** PostgreSQL workers used for parallel column scans. */
        struct parallel final {
            parallel() = delete;

            /** @brief Allow parallel PostgreSQL plans for Bioseq column scans.
             *
             * @remark Configuration: Enable Postgres parallel queries Alignment won't use parallel scans even with this setting enabled.
             * @remark Value `true`: Allow possible parallel scan paths (actual worker availability still depends on PostgreSQL planner, so this setting does not force the planner to use parallel scans). Additionally, any query using mmsearch won't be parallelized, due to current code architecture. The parallel scans however are possible for metadata queries.
             * @remark Value `false`: Disable all parallel scans on BioseqDB tables.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.execution.parallel.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.execution.parallel.enabled = 'on';
             * SHOW bioseq.execution.parallel.enabled;
             * RESET bioseq.execution.parallel.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;

            /** @brief Number of workers requested for a parallel column scan.
             *
             * @remark Configuration: Requests the PostgreSQL worker count for possbible Bioseq parallel scan paths. This is soft minimum and the actual worker count is determined by the PostgreSQL planner.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.execution.parallel.minimum_processes;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.execution.parallel.minimum_processes = '8';
             * SHOW bioseq.execution.parallel.minimum_processes;
             * RESET bioseq.execution.parallel.minimum_processes;
             * ROLLBACK;
             * ```
             */
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

        /** @brief Allow supported expressions and aggregates to process column batches.
         *
         * @remark Configuration: Can disable vectorized SQL operations for aggregates like SUM(), COUNT(), etc. Vectorized operations only apply to non-sequence aggregates, not alignment pipeline.
         * @remark Value `true`: Use supported vectorized SQL operations when the plan allows them.
         * @remark Value `false`: Use normal SQL processing for those operations.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.vectorization.enabled;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.vectorization.enabled = 'on';
         * SHOW bioseq.vectorization.enabled;
         * RESET bioseq.vectorization.enabled;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
        })]]
        static inline bool enabled = true;
    };

    /** Cache of decompressed column chunks. */
    struct column_cache final {
        column_cache() = delete;

        /** @brief Reuse decompressed column chunks within the backend.
         *
         * @remark Configuration: Controls the backend's decompressed column-chunk cache. Decompression cache only applies to one query. After completion of the query, the cache is cleared.
         * @remark Value `true`: After decompression save the buffer, so it's avaialable for reuse
         * @remark Value `false`: Read and decompress but do not save buffers in cache.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.column_cache.enabled;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.column_cache.enabled = 'on';
         * SHOW bioseq.column_cache.enabled;
         * RESET bioseq.column_cache.enabled;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc]]
        static inline bool enabled = true;

        /** @brief Memory limit for cached column chunks, in MiB.
         *
         * @remark Configuration: Limits the decompressed column-chunk cache BioseqDB will still allocate buffers for decompression, so this setting does not control the total memory used during decompression
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.column_cache.capacity_mb;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.column_cache.capacity_mb = '200';
         * SHOW bioseq.column_cache.capacity_mb;
         * RESET bioseq.column_cache.capacity_mb;
         * ROLLBACK;
         * ```
         */
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

            /** @brief Enables Bioseq index scan paths.
             *
             * @remark Configuration: Can allow usage of non-BioseqDB indices with BioseqDB tables. This settings does not control any BioseqDB indices like MPF/MPS etc.
             * @remark Value `true`: Allow using non-BioseqDB indices during queries, for example it becomes possible to use B-tree index on metadata column.
             * @remark Value `false`: Do not allow using any non-BioseqDB indices on BioseqDB tables. This can heavily degrade performance as Postgres will select plan that ignores existing B-trees, hash indexes etc.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.planner.index_scan.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.planner.index_scan.enabled = 'on';
             * SHOW bioseq.planner.index_scan.enabled;
             * RESET bioseq.planner.index_scan.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;
        };

        /** Bioseq custom scans and their planner limits. */
        struct custom_scan final {
            custom_scan() = delete;

            /** @brief Enables custom scan paths for Bioseq tables.
             *
             * @remark Configuration: Entirely disable all BioseqDB custom scans. Useful to force Postgres to select non-BioseqDB plans. Do not use this unless you know what you're doing.
             * @remark Value `true`: Allow the planner to install Bioseq custom scans.
             * @remark Value `false`: Disable custom scans. This means that it won't be possible to use 'mmsearch' function anymore as it requires custom scan path. All other metadata queries still will be possible.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.planner.custom_scan.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.planner.custom_scan.enabled = 'on';
             * SHOW bioseq.planner.custom_scan.enabled;
             * RESET bioseq.planner.custom_scan.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;

            /** @brief Limits custom scan alternatives generated for one relation.
             *
             * @remark Configuration: Limit paths explored by GQO for BioseqDB custom scans. Increasing the number may yield better plans, but increase planning time.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.planner.custom_scan.max_paths;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.planner.custom_scan.max_paths = '64';
             * SHOW bioseq.planner.custom_scan.max_paths;
             * RESET bioseq.planner.custom_scan.max_paths;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
                .minimum = 1,
                .maximum = 1'024,
            })]]
            static inline int max_paths = 64;
        };

        /** Filtering rows and skipping chunks inside a custom scan */
        struct qual_pushdown final {
            qual_pushdown() = delete;

            /** @brief Pushes supported filter predicates into custom scans.
             *
             * @remark Configuration: Controls behaviour of predicate pushdown. Pushdown uses correlation statistics for column reported by Postgres to decide if this predicate can be useful to avoid loading chunks
             * @remark Value `true`: Normally filters can be pushed down into the scan, which improved performance. The predicates that Postgres reports as highly-correlated will be used to possibly reduce number of loaded chunks (min/max prunning mechanism)
             * @remark Value `false`: Leave predicates outside the scan, which may severely degrade performance.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.planner.qual_pushdown.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.planner.qual_pushdown.enabled = 'on';
             * SHOW bioseq.planner.qual_pushdown.enabled;
             * RESET bioseq.planner.qual_pushdown.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .flags = bioseq::sql::guc_flag::no_show_all | bioseq::sql::guc_flag::not_in_sample,
            })]]
            static inline bool enabled = true;

            /** @brief Minimum absolute column correlation for  chunk-filter pushdown.
             * Columns ordered with their row positions are nseful for min/max pruning.
             *
             * @remark Configuration: Sets the minimum absolute column correlation value (reported by Postgres table statistics) used by chunk prunning logic described for bioseq.planner.qual_pushdown.enabled. Values closer to one, mean stronger correlation.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.planner.qual_pushdown.correlation_threshold;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.planner.qual_pushdown.correlation_threshold = '0.4';
             * SHOW bioseq.planner.qual_pushdown.correlation_threshold;
             * RESET bioseq.planner.qual_pushdown.correlation_threshold;
             * ROLLBACK;
             * ```
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

            /** @brief Sets the message level for planner diagnostics.
             *
             * @remark Configuration: Chooses the PostgreSQL severity for planner diagnostic messages (logging levels are Postgres specific). PostgreSQL's message thresholds determine where the messages appear.
             * @remark Value `debug3`: Emit them at DEBUG3 severity.
             * @remark Value `debug5`: Emit them at DEBUG5 severity.
             * @remark Value `debug4`: Emit them at DEBUG4 severity.
             * @remark Value `debug2`: Emit them at DEBUG2 severity.
             * @remark Value `debug1`: Emit them at DEBUG1 severity.
             * @remark Value `log`: Emit them at LOG severity.
             * @remark Value `info`: Emit them at INFO severity.
             * @remark Value `notice`: Emit them at NOTICE severity.
             * @remark Value `warning`: Emit them at WARNING severity.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.planner.diagnostics.log_level;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.planner.diagnostics.log_level = 'debug3';
             * SHOW bioseq.planner.diagnostics.log_level;
             * RESET bioseq.planner.diagnostics.log_level;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc]]
            static inline bioseq::postgres::support::postgres_enum_storage<planner_log_level>
                log_level = bioseq::postgres::support::postgres_enum_storage<planner_log_level>{
                    planner_log_level::debug3};
        };

        /** Batch sorting of sequence values in a custom scan. */
        struct sequence_sort_scan final {
            sequence_sort_scan() = delete;

            /** @brief Routes sequence ordering through the batch-sort custom scan.
             *
             * @remark Configuration: Enable or disable custom sort for BioseqDB sequences. Sort works only if there's single sorting key that is sequence column itself.
             * @remark Value `false`: Use default Postgres sort.
             * @remark Value `true`: Consider the custom batch-sort where possible.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.planner.sequence_sort_scan.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.planner.sequence_sort_scan.enabled = 'off';
             * SHOW bioseq.planner.sequence_sort_scan.enabled;
             * RESET bioseq.planner.sequence_sort_scan.enabled;
             * ROLLBACK;
             * ```
             */
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
             * source rows commit. This INSERT writer cannot, publish that format.
             * Rows without stored indexes remain searchable using temporary
             * indexes built by the selected alignment  algorithm.
             *
             * @remark Configuration: Enables automatic index incremental update when row is inserted into the table. You can always still manually update index via update_index function.
             * @remark Value `false`: Save new source rows without automatic index builds.
             * @remark Value `true`: Currently grouped MPF index (the default one) does not support this function and reports an error.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.index.insert_maintenance.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.index.insert_maintenance.enabled = 'off';
             * SHOW bioseq.index.insert_maintenance.enabled;
             * RESET bioseq.index.insert_maintenance.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /** Reading an index stored for one sequence. */
        struct sequence_read final {
            sequence_read() = delete;

            /** @brief Reads stored per-sequence indexes when they are available.
             *
             * @remark Configuration: Controls whether the pipeline executor can read indexes. If the option is disabled, then pipeline will always recompute all seeds from scratch. There's also per-search option passed via aligner_options: index_read_requirement codes 1 and 2 override this setting.
             * @remark Value `true`: Normally allow reading BioseqDB indices.
             * @remark Value `false`: Force pipeline to always recompute all seeds from scratch.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.index.sequence_read.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.index.sequence_read.enabled = 'on';
             * SHOW bioseq.index.sequence_read.enabled;
             * RESET bioseq.index.sequence_read.enabled;
             * ROLLBACK;
             * ```
             */
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
             *
             * @remark Configuration: Very experimental setting that allows main process to asynchronously update missing indices for sequences: If query does not have seeds in index for certain sequence and needs to compute them, after the query the index will be updated. Grouped MPF index (default) does not implement this feature and using it with this option enabled will produce an error. Additionally you need to enable bioseq.index.recompute_async_persistence.enabled as well.
             * @remark Value `false`: Do not stage asynchronous persistence work.
             * @remark Value `true`: Allow the limited ordinary-MPS submission path when its other requirements are met.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.index.async_persistence.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.index.async_persistence.enabled = 'off';
             * SHOW bioseq.index.async_persistence.enabled;
             * RESET bioseq.index.async_persistence.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /** Saving rebuilt indexes in the background. */
        struct recompute_async_persistence final {
            recompute_async_persistence() = delete;

            /** @brief Persists an index asynchronously after recomputation.
             *
             * @remark Configuration: Separate setting, but effectively to enable async persistence you need to enable bioseq.index.async_persistence.enabled together with this one. Set bioseq.index.async_persistence.enabled to the same value as this one.
             * @remark Value `false`: Same as bioseq.index.async_persistence.enabled
             * @remark Value `true`: Same as bioseq.index.async_persistence.enabled
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.index.recompute_async_persistence.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.index.recompute_async_persistence.enabled = 'off';
             * SHOW bioseq.index.recompute_async_persistence.enabled;
             * RESET bioseq.index.recompute_async_persistence.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };

        /**
         * @brief Limit memory used by loaded indexes, in MiB.
         * Zero lets the runtime choose its memory budget.
         *
         * @remark Configuration: Specialized limit on the memory allocated by index builders/readers in MB. Using 0 means that they share the same memory budget as other query buffers.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.index.memory_cap_mb;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.index.memory_cap_mb = '0';
         * SHOW bioseq.index.memory_cap_mb;
         * RESET bioseq.index.memory_cap_mb;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .flags = bioseq::sql::guc_flag::unit_mb,
            .minimum = 0,
            .maximum = std::numeric_limits<int>::max(),
        })]]
        static inline int memory_cap_mb = 0;

        /** Sorting minimizer/sequence pairs for an ipdex. */
        struct sequence_minimizer_sort final {
            sequence_minimizer_sort() = delete;

            /** @brief Selects the sorter used for minimizer sequence pairs.
             *
             * @remark Configuration: Change sorting algorithm used for non-grouped MPF index builds SIMD versions weren't tested by me in terms of correctness and they were slower than the default
             * @remark Value `std_sort`: Use the standard-library sort
             * @remark Value `simd_u128_vqsort`: Use Highway VQSort on packed 128-bit keys (implementation from C++ Highway repo).
             * @remark Value `simd_u128_vec_stream_1`: Use the custom SIMD sorter with one vector stream.
             * @remark Value `simd_u128_vec_stream_2`: Use the custom SIMD sorter with two vector streams.
             * @remark Value `simd_u128_vec_stream_4`: Use the custom SIMD sorter with four vector streams.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.index.sequence_minimizer_sort.algorithm;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.index.sequence_minimizer_sort.algorithm = 'std_sort';
             * SHOW bioseq.index.sequence_minimizer_sort.algorithm;
             * RESET bioseq.index.sequence_minimizer_sort.algorithm;
             * ROLLBACK;
             * ```
             */
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
         * Zero keeps the index builder's default  buffer size.
         *
         * @remark Configuration: For MPS overrides the external-sort run buffer size. Zero computes a size from the common BioseqDB memory budget.
         * @remark Display unit: bytes
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.index_build.run_buffer_bytes;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state. Requires permission to change this superuser setting.
         * BEGIN;
         * SET LOCAL bioseq.index_build.run_buffer_bytes = '0';
         * SHOW bioseq.index_build.run_buffer_bytes;
         * RESET bioseq.index_build.run_buffer_bytes;
         * ROLLBACK;
         * ```
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
             * Requires a scheduler mode and worker count that allow parallel dispatch
             *
             * @remark Configuration: Experimental setting to use parallelized writing and decoding of data chunks through queue when loading FASTA/FASTQ files.
             * @remark Value `false`: Default reader perform decoding and writing (placing sequences in buffer) sequentially.
             * @remark Value `true`: Enable parallelized writing and decoding of data chunks through queue.
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.load.write_overlap.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.load.write_overlap.enabled = 'off';
             * SHOW bioseq.load.write_overlap.enabled;
             * RESET bioseq.load.write_overlap.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc]]
            static inline bool enabled = false;
        };
    };
};

} // namespace bioseq
