/**
 * @file aligner.cppm
 * @brief PostgreSQL settings for minimizers, chaining and base alignment.
 */
export module bioseq.postgres.configuration.aligner;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.support.enum_storage;

export namespace bioseq::aligner {

/** Selects the algorithm used to chain seed anchors. */
enum class chaining_algorithm : int {
    scalar_minimap2 = 0,
    simd_minimap2 = 1,
    range_minimum_query = 2,
    vulkan_tiled_dynamic_programming = 3,
};

/** Selects the algorithm used for base-level alignment. */
enum class base_alignment_algorithm : int {
    ksw2 = 0,
    block_aligner = 1,
};

/** Selects the algorithm used to extract minimizers. */
enum class minimizer_algorithm : int {
    scalar = 0,
    simd_minimap2_reference = 1,
    two_stack_nthash = 2,
    vulkan_minimap_integer = 3,
    bounded_nthash_simd = 4,
};

/** Selects how stored minimizer fragments are searched. */
enum class fragment_minimizer_lookup_route : int {
    control_block_scan = 0,
    direct_compact_hash_lookup = 1,
};

/** PostgreSQL settings used to build alignment requests. */
struct settings final {
    settings() = delete;

    /** Settings for selecting minimizers from a sequence. */
    struct minimizer final {
        minimizer() = delete;

        /** @brief Number of bases in each k-mer considered for a minimizer. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 64,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int kmer_size = 15;

        /** @brief Number of consecutive k-mers in a minimizer window. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1'024,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int window_size = 10;

        /** @brief Selects the minimizer extraction algorithm. */
        [[bioseq_sql::guc({
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline bioseq::postgres::support::postgres_enum_storage<minimizer_algorithm>
            algorithm = bioseq::postgres::support::postgres_enum_storage<minimizer_algorithm>{
                minimizer_algorithm::bounded_nthash_simd};
    };

    /** Settings for joining matching seeds into candidate alignments. */
    struct chaining final {
        chaining() = delete;

        /** @brief Maximum distance between anchors that chaining can join. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 30,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_gap = 5'000;

        /** @brief Store a reference-gap override in the request.
         * The current chaining code uses maximum_gap for both sequences.
         */
        [[bioseq_sql::guc({
            .minimum = -1,
            .maximum = 1 << 30,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_reference_gap = -1;

        /** @brief Sets the chaining and alignment bandwidth. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int bandwidth = 500;

        /** @brief Store a long-join bandwidth in the request.
         * The current pipeline does not use this setting.
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int long_join_bandwidth = 0;

        /** @brief Stop searching earlier anchors after this many unhelpful candidates. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_skipped_anchors = 25;

        /** @brief Limits predecessors examined for each anchor. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 22,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_predecessors = 5'000;

        /** @brief Sets the minimum seed count accepted for a chain. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int minimum_seed_count = 3;

        /** @brief Sets the minimum accepted chain score. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int minimum_score = 40;

        /** @brief Scales the chaining gap penalty. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 100,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double gap_penalty_scale = 0.8;

        /** @brief Scales the chaining skip penalty. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 100,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double skip_penalty_scale = 0.0;

        /** @brief Selects the seed-anchor chaining algorithm. */
        [[bioseq_sql::guc({
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline bioseq::postgres::support::postgres_enum_storage<chaining_algorithm>
            algorithm = bioseq::postgres::support::postgres_enum_storage<chaining_algorithm>{
                chaining_algorithm::simd_minimap2};
    };

    /** Settings for choosing secondary alignments. */
    struct secondary final {
        secondary() = delete;

        /** @brief Required query overlap for assigning an alignment to a primary.
         * The comparison also accounts for query bases left uncovered by primaries.
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double overlap_threshold = 0.5;

        /** @brief Maximum uncovered query length allowed for a secondary alignment. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 2'147'483'647,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int uncovered_length_limit = 2'147'483'647;

        /** @brief Score ratio used to keep a secondary relative to its primary.
         * A secondary can also pass within the allowed absolute score difference.
         * Zero disables the CPU secondary filter.
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double score_ratio_threshold = 0.8;

        /** @brief Maximum secondary alignments kept across one query. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1'024,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_count = 5;
    };

    /** Scores and penalties used during base alignment. */
    struct scoring final {
        scoring() = delete;

        /** @brief Sets the alignment match score. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int match_score = 2;

        /** @brief Sets the alignment mismatch penalty. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int mismatch_penalty = 4;

        /** @brief Sets the short-gap opening penalty. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int short_gap_open_penalty = 4;

        /** @brief Sets the short-gap extension penalty. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int short_gap_extension_penalty = 2;

        /** @brief Sets the long-gap opening penalty. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int long_gap_open_penalty = 24;

        /** @brief Sets the long-gap extension penalty. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int long_gap_extension_penalty = 1;

        /** @brief Sets the penalty for ambiguous bases. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int ambiguous_base_penalty = 1;

        /** @brief Stop extension when its score falls too far below the best score.
         * Z-drop adjusts the comparison for gaps. Negative one disables it.
         */
        [[bioseq_sql::guc({
            .minimum = -1,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int z_drop_threshold = 400;

        /** @brief Bonus for an alignment that reaches the query end.
         * Negative one disables the bonus.
         */
        [[bioseq_sql::guc({
            .minimum = -1,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int end_bonus = -1;
    };

    /** Settings for ignoring minimizers that occur too often. */
    struct seeding final {
        seeding() = delete;

        /** @brief Ignore minimizers with more target occurrences than this limit. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 24,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_occurrence = 500;

        /** @brief Store an intermediate occurrence threshold in the request.
         * The current seeding code uses maximum_occurrence instead.
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 24,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int intermediate_occurrence_threshold = 500;
    };

    /** Block sizes used by Block Aligner during extension. */
    struct block_alignment final {
        block_alignment() = delete;

        /** @brief Sets the minimum block size used by the block aligner. */
        [[bioseq_sql::guc({
            .minimum = 8,
            .maximum = 4'096,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int minimum_block_size = 32;

        /** @brief Sets the maximum block size used by the block aligner. */
        [[bioseq_sql::guc({
            .minimum = 8,
            .maximum = 16'384,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_block_size = 256;
    };

    /** Choice of algorithm for aligning individual bases. */
    struct base_alignment final {
        base_alignment() = delete;

        /** @brief Selects the base-level alignment algorithm. */
        [[bioseq_sql::guc({
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline bioseq::postgres::support::postgres_enum_storage<base_alignment_algorithm>
            algorithm = bioseq::postgres::support::postgres_enum_storage<base_alignment_algorithm>{
                base_alignment_algorithm::block_aligner};
    };

    /** Settings for reading stored minimizer fragments. */
    struct fragment_minimizer final {
        fragment_minimizer() = delete;

        /**
         * @brief Choose control-table scans or direct compact hash lookup.
         *
         * Direct lookup applies to stored fragments with 32-bit hashes.
         * Opening that route requires a verified compact index image.
         */
        [[bioseq_sql::guc]]
        static inline bioseq::postgres::support::postgres_enum_storage<
            fragment_minimizer_lookup_route> lookup_route =
            bioseq::postgres::support::postgres_enum_storage<fragment_minimizer_lookup_route>{
                fragment_minimizer_lookup_route::direct_compact_hash_lookup};
    };

    /** Settings for storing minimizers in grouped arrays. */
    struct grouped_soa_generation final {
        grouped_soa_generation() = delete;

        /** @brief Build minimizer generations in grouped arrays with source locators. */
        [[bioseq_sql::guc]]
        static inline bool enabled = true;
    };

    /** Settings for GPU alignment through Vulkan. */
    struct vulkan final {
        vulkan() = delete;

        /** @brief Allows execution through Vulkan-backed algorithms. */
        [[bioseq_sql::guc({
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline bool enabled = true;

        /** GPU lookup of matching seed positions. */
        struct seed_lookup final {
            seed_lookup() = delete;

            /** @brief Enables Vulkan seed-hit lookup. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** Reuse target minimizer indexes already uploaded to the GPU. */
        struct index_cache final {
            index_cache() = delete;

            /** @brief Keep uploaded target indexes for later GPU lookups. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = true;
        };

        /** Run minimizer extraction and seeding together on the GPU. */
        struct fused_frontend final {
            fused_frontend() = delete;

            /** @brief Enable the GPU path that combines sketching and seed lookup. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** GPU construction of target minimizer indexes. */
        struct index_build final {
            index_build() = delete;

            /** @brief Builds the target minimizer index on the GPU. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** GPU extension of chains into base alignments. */
        struct alignment_extension final {
            alignment_extension() = delete;

            /** @brief Enables GPU base-alignment extension. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** @brief Sets the ordered-sketch tile size in base pairs. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 24,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int ordered_sketch_tile_bp = 2'048;

        /** @brief Store the requested radix digit width in the request.
         * The current GPU radix kernels process eight bits per pass.
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 16,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int radix_sort_digit_bits = 8;
    };

    /** Timing and counters for alignment stages. */
    struct profiling final {
        profiling() = delete;

        /** @brief Enables runtime profiling for the aligner pipeline. */
        [[bioseq_sql::guc]]
        static inline bool enabled = false;
    };
};

} // namespace bioseq::aligner
