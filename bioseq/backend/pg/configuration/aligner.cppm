/**
 * @file aligner.cppm
 * @brief Declares PostgreSQL settings for sequence alignment execution.
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

/** Selects the lookup topology for persisted minimizer fragments. */
enum class fragment_minimizer_lookup_route : int {
    control_block_scan = 0,
    direct_compact_hash_lookup = 1,
};

/** Owns PostgreSQL settings for alignment execution. */
struct settings final {
    settings() = delete;

    /** Owns minimizer extraction settings. */
    struct minimizer final {
        minimizer() = delete;

        /** @brief Sets the minimizer k-mer size. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 64,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int kmer_size = 15;

        /** @brief Sets the minimizer window size. */
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
                minimizer_algorithm::scalar};
    };

    /** Owns seed-anchor chaining settings. */
    struct chaining final {
        chaining() = delete;

        /** @brief Sets the maximum chaining gap. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 30,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_gap = 5'000;

        /** @brief Overrides the reference gap limit. Negative one uses maximum_gap.
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

        /** @brief Sets the long-join bandwidth. Zero disables long joins. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int long_join_bandwidth = 0;

        /** @brief Limits skipped anchors before chaining terminates early. */
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
                chaining_algorithm::scalar_minimap2};
    };

    /** Owns secondary-alignment selection settings. */
    struct secondary final {
        secondary() = delete;

        /** @brief Sets the overlap threshold for secondary alignments. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double overlap_threshold = 0.5;

        /** @brief Sets the uncovered-length limit for secondary alignments. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 2'147'483'647,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int uncovered_length_limit = 2'147'483'647;

        /** @brief Sets the secondary-to-primary score ratio threshold. */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double score_ratio_threshold = 0.8;

        /** @brief Limits secondary alignments retained for each primary alignment.
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1'024,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_count = 5;
    };

    /** Owns base-alignment scoring settings. */
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

        /** @brief Sets the Z-drop threshold. Negative one disables it. */
        [[bioseq_sql::guc({
            .minimum = -1,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int z_drop_threshold = 400;

        /** @brief Sets the sequence-end bonus. Negative one disables it. */
        [[bioseq_sql::guc({
            .minimum = -1,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int end_bonus = -1;
    };

    /** Owns seed-frequency settings. */
    struct seeding final {
        seeding() = delete;

        /** @brief Sets the maximum seed occurrence retained. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 24,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_occurrence = 500;

        /** @brief Sets the intermediate seed occurrence threshold. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 24,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int intermediate_occurrence_threshold = 500;
    };

    /** Owns Block Aligner sizing settings. */
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

    /** Owns base-alignment algorithm selection. */
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

    /** Owns persisted minimizer-fragment lookup settings. */
    struct fragment_minimizer final {
        fragment_minimizer() = delete;

        /**
         * @brief Selects the persisted minimizer-fragment lookup topology.
         *
         * Direct compact lookup fails when its authenticated image is unavailable.
         */
        [[bioseq_sql::guc]]
        static inline bioseq::postgres::support::postgres_enum_storage<
            fragment_minimizer_lookup_route> lookup_route =
            bioseq::postgres::support::postgres_enum_storage<fragment_minimizer_lookup_route>{
                fragment_minimizer_lookup_route::control_block_scan};
    };

    /** Owns grouped-SoA generation selection. */
    struct grouped_soa_generation final {
        grouped_soa_generation() = delete;

        /** @brief Enables locator-backed grouped-SoA minimizer generations. */
        [[bioseq_sql::guc]]
        static inline bool enabled = false;
    };

    /** Owns Vulkan-backed alignment settings. */
    struct vulkan final {
        vulkan() = delete;

        /** @brief Allows execution through Vulkan-backed algorithms. */
        [[bioseq_sql::guc({
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline bool enabled = true;

        /** Owns Vulkan seed-lookup selection. */
        struct seed_lookup final {
            seed_lookup() = delete;

            /** @brief Enables Vulkan seed-hit lookup. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** Owns GPU-resident minimizer-index cache selection. */
        struct index_cache final {
            index_cache() = delete;

            /** @brief Caches GPU-resident minimizer index uploads. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = true;
        };

        /** Owns fused Vulkan frontend selection. */
        struct fused_frontend final {
            fused_frontend() = delete;

            /** @brief Enables the resident fused Vulkan frontend. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** Owns Vulkan target-index build selection. */
        struct index_build final {
            index_build() = delete;

            /** @brief Builds the target minimizer index on the GPU. */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** Owns GPU base-alignment extension selection. */
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

        /** @brief Sets the device radix-sort digit width. */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 16,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int radix_sort_digit_bits = 8;
    };

    /** Owns aligner profiling settings. */
    struct profiling final {
        profiling() = delete;

        /** @brief Enables runtime profiling for the aligner pipeline. */
        [[bioseq_sql::guc]]
        static inline bool enabled = false;
    };
};

} // namespace bioseq::aligner
