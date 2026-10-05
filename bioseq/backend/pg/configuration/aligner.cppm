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
    /// @brief Minimap2 scalar chaining
    scalar_minimap2 = 0,
    /// @brief BioseqDB implementation of accelerated minimap2-inspired chaining
    simd_minimap2 = 1,
    /// @brief BioseqDB RMQ on AVL trees similar to what minimap2 uses
    range_minimum_query = 2,
    /// @brief Use tiled GPU chaining
    vulkan_tiled_dynamic_programming = 3,
};

/** Selects the algorithm used for base-level alignment. */
enum class base_alignment_algorithm : int {
    /// @brief Use KSW2 aligner
    ksw2 = 0,
    /// @brief Adaptive block aligner
    block_aligner = 1,
};

/** Selects the algorithm used to extract minimizers. */
enum class minimizer_algorithm : int {
    /// @brief Minimap2 scalar algorithm
    scalar = 0,
    /// @brief Use the naive version of SIMD implementation of minimap2's
    simd_minimap2_reference = 1,
    /// @brief Use rolling ntHash and two-stack window-minimum inspired by simd-minimizers
    /// paper.
    two_stack_nthash = 2,
    /// @brief Use BioseqDB GPU algorithm
    vulkan_minimap_integer = 3,
    /// @brief Use the bounded SIMD BioseqDB algorithm
    bounded_nthash_simd = 4,
};

/** Selects how stored minimizer fragments are searched. */
enum class fragment_minimizer_lookup_route : int {
    /// @brief Search the directory row for the hash bucket first
    control_block_scan = 0,
    /// @brief By default read hash directly from the hash bucket.
    direct_compact_hash_lookup = 1,
};

/** PostgreSQL settings used to build alignment requests. */
struct settings final {
    settings() = delete;

    /** Settings for selecting minimizers from a sequence. */
    struct minimizer final {
        minimizer() = delete;

        /** @brief Number of bases in each k-mer considered for a minimizer
         *
         * @remark Configuration: Sets the number of sequence symbols in each k-mer
         * @remark Display unit: symbols
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.minimizer.kmer_size;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.minimizer.kmer_size = '15';
         * SHOW bioseq.aligner.minimizer.kmer_size;
         * RESET bioseq.aligner.minimizer.kmer_size;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 64,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int kmer_size = 15;

        /** @brief Number of consecutive, k-mers in a  minimizer window.
         *
         * @remark Configuration: Sets the number of consecutive k-mers in one minimizer window
         * @remark Display unit: k-mers
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.minimizer.window_size;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.minimizer.window_size = '10';
         * SHOW bioseq.aligner.minimizer.window_size;
         * RESET bioseq.aligner.minimizer.window_size;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1'024,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int window_size = 10;

        /** @brief Selects the minimizer extraction algorithm.
         *
         * @remark Configuration: Chooses how minimizers are calculated Changing the algorithm can invalidate compatibility with saved indexes!
         * @remark Value `bounded_nthash_simd`: Use the bounded SIMD BioseqDB algorithm
         * @remark Value `scalar`: Minimap2 scalar algorithm
         * @remark Value `simd_minimap2_reference`: Use the naive version of SIMD implementation of minimap2's
         * @remark Value `two_stack_nthash`: Use rolling ntHash and two-stack window-minimum inspired by simd-minimizers paper.
         * @remark Value `vulkan_minimap_integer`: Use BioseqDB GPU algorithm
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.minimizer.algorithm;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.minimizer.algorithm = 'bounded_nthash_simd';
         * SHOW bioseq.aligner.minimizer.algorithm;
         * RESET bioseq.aligner.minimizer.algorithm;
         * ROLLBACK;
         * ```
         */
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

        /** @brief Maximum diqtance between anchors that chaining can join.
         *
         * @remark Configuration: Sets the requested maximum anchor gap used by chaining. Scalar (minimap2)chaining raises this limit to at least the bandwidth value.
         * @remark Display unit: positions
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.maximum_gap;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.maximum_gap = '5000';
         * SHOW bioseq.aligner.chaining.maximum_gap;
         * RESET bioseq.aligner.chaining.maximum_gap;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 30,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_gap = 5'000;

        /** @brief Store a reference-gap override in the request.
         * The current chaining code uses maximum_gap for both sequences.
         *
         * @remark Configuration: Currently unused
         * @remark Display unit: positions
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.maximum_reference_gap;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.maximum_reference_gap = '-1';
         * SHOW bioseq.aligner.chaining.maximum_reference_gap;
         * RESET bioseq.aligner.chaining.maximum_reference_gap;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = -1,
            .maximum = 1 << 30,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_reference_gap = -1;

        /** @brief Sets the chaining and alignment bandwidth.
         *
         * @remark Configuration: Despite the name it's also used by aligner stage, limits maximum distane to diagonal.
         * @remark Display unit: positions
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.bandwidth;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.bandwidth = '500';
         * SHOW bioseq.aligner.chaining.bandwidth;
         * RESET bioseq.aligner.chaining.bandwidth;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int bandwidth = 500;

        /** @brief Store a long-join bandwidth in the request.
         * The current pipeline does not use this setting.
         *
         * @remark Configuration: Currently unused.
         * @remark Display unit: positions
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.long_join_bandwidth;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.long_join_bandwidth = '0';
         * SHOW bioseq.aligner.chaining.long_join_bandwidth;
         * RESET bioseq.aligner.chaining.long_join_bandwidth;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int long_join_bandwidth = 0;

        /** @brief Stop searching earlier anchors after this many unhelpful candidates.
         *
         * @remark Configuration: Limit of consecutive updates for anchors that do not improve score (only for supported algorithms like scalar)
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.maximum_skipped_anchors;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.maximum_skipped_anchors = '25';
         * SHOW bioseq.aligner.chaining.maximum_skipped_anchors;
         * RESET bioseq.aligner.chaining.maximum_skipped_anchors;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_skipped_anchors = 25;

        /** @brief Limits predecessors examined for each anchor.
         *
         * @remark Configuration: Limits how many predecessors chainer can consider, but scalar chaining (depending on the actual algorithm) can examine one additional candidate. RMQ does not use this field.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.maximum_predecessors;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.maximum_predecessors = '5000';
         * SHOW bioseq.aligner.chaining.maximum_predecessors;
         * RESET bioseq.aligner.chaining.maximum_predecessors;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 22,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_predecessors = 5'000;

        /** @brief Sets the minimum seed count accepted for a chain.
         *
         * @remark Configuration: Sets the minimum seed count for an accepted chain.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.minimum_seed_count;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.minimum_seed_count = '3';
         * SHOW bioseq.aligner.chaining.minimum_seed_count;
         * RESET bioseq.aligner.chaining.minimum_seed_count;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int minimum_seed_count = 3;

        /** @brief Sets the minimum accepted chain score.
         *
         * @remark Configuration: Sets the minimum accepted chain score (the score is computed by the selected chaining algorithm)
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.minimum_score;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.minimum_score = '40';
         * SHOW bioseq.aligner.chaining.minimum_score;
         * RESET bioseq.aligner.chaining.minimum_score;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int minimum_score = 40;

        /** @brief Scales the chaining gap penalty.
         *
         * @remark Configuration: Scales the gap penalty used by chaining (larger values penalize that contribution more strongly)
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.gap_penalty_scale;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.gap_penalty_scale = '0.8';
         * SHOW bioseq.aligner.chaining.gap_penalty_scale;
         * RESET bioseq.aligner.chaining.gap_penalty_scale;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 100,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double gap_penalty_scale = 0.8;

        /** @brief Scales the chaining skip penalty.
         *
         * @remark Configuration: Scales the chaining skip-distance penalty.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.skip_penalty_scale;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.skip_penalty_scale = '0';
         * SHOW bioseq.aligner.chaining.skip_penalty_scale;
         * RESET bioseq.aligner.chaining.skip_penalty_scale;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 100,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double skip_penalty_scale = 0.0;

        /** @brief Selects the seed-anchor chaining algorithm.
         *
         * @remark Configuration: Selects algorithm used for chaining Selected algorithm might still be rejected if the hardware does not support it
         * @remark Value `simd_minimap2`: BioseqDB implementation of accelerated minimap2-inspired chaining
         * @remark Value `scalar_minimap2`: Minimap2 scalar chaining
         * @remark Value `range_minimum_query`: BioseqDB RMQ on AVL trees similar to what minimap2 uses
         * @remark Value `vulkan_tiled_dynamic_programming`: Use tiled GPU chaining
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.chaining.algorithm;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.chaining.algorithm = 'simd_minimap2';
         * SHOW bioseq.aligner.chaining.algorithm;
         * RESET bioseq.aligner.chaining.algorithm;
         * ROLLBACK;
         * ```
         */
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
         * The  comparison also accounts for query bases left uncovered by primaries.
         *
         * @remark Configuration: Sets the primary/secondary classification thresholds
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.secondary.overlap_threshold;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.secondary.overlap_threshold = '0.5';
         * SHOW bioseq.aligner.secondary.overlap_threshold;
         * RESET bioseq.aligner.secondary.overlap_threshold;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double overlap_threshold = 0.5;

        /** @brief Maximum uncovered query length allowed for a secondary alignment.
         *
         * @remark Configuration: Limits the uncovered query length used when classifying an alignment as secondary to an existing primary.
         * @remark Display unit: symbols
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.secondary.uncovered_length_limit;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.secondary.uncovered_length_limit = '2147483647';
         * SHOW bioseq.aligner.secondary.uncovered_length_limit;
         * RESET bioseq.aligner.secondary.uncovered_length_limit;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 2'147'483'647,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int uncovered_length_limit = 2'147'483'647;

        /** @brief Score ratio used to keep a secondary relative to its primary.
         * A secondary can also pass within the allowed absolute score difference.
         * Zero disables the CPU secondary filter.
         *
         * @remark Configuration: Sets the secondary-to-primary score-ratio threshold. Zero bypasses this filtering step, including its secondary-count limit.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.secondary.score_ratio_threshold;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.secondary.score_ratio_threshold = '0.8';
         * SHOW bioseq.aligner.secondary.score_ratio_threshold;
         * RESET bioseq.aligner.secondary.score_ratio_threshold;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 1,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline double score_ratio_threshold = 0.8;

        /** @brief Maximum secondary alignments kept across one query.
         *
         * @remark Configuration: Limits retained secondary alignments across the complete list passed to the filtering stage
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.secondary.maximum_count;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.secondary.maximum_count = '5';
         * SHOW bioseq.aligner.secondary.maximum_count;
         * RESET bioseq.aligner.secondary.maximum_count;
         * ROLLBACK;
         * ```
         */
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

        /** @brief Sets the alignment match score.
         *
         * @remark Configuration: Sets the score added for an aligned matching symbol.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.match_score;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.match_score = '2';
         * SHOW bioseq.aligner.scoring.match_score;
         * RESET bioseq.aligner.scoring.match_score;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int match_score = 2;

        /** @brief Sets the alignment mismatch penalty.
         *
         * @remark Configuration: Sets the score penalty for an aligned mismatch.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.mismatch_penalty;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.mismatch_penalty = '4';
         * SHOW bioseq.aligner.scoring.mismatch_penalty;
         * RESET bioseq.aligner.scoring.mismatch_penalty;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int mismatch_penalty = 4;

        /** @brief Sets the short-gap opening penalty.
         *
         * @remark Configuration: Sets the first gap-opening penalty. KSW2 and BlockAligner use this pair. BlockAligner requires positive opening and extension penalties whose sum is at most 128.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.short_gap_open_penalty;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.short_gap_open_penalty = '4';
         * SHOW bioseq.aligner.scoring.short_gap_open_penalty;
         * RESET bioseq.aligner.scoring.short_gap_open_penalty;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int short_gap_open_penalty = 4;

        /** @brief Sets the short-gap extension penalty.
         *
         * @remark Configuration: Sets the first gap-extension penalty. BlockAligner requires it to be positive and its sum with the first opening penalty to be at most 128.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.short_gap_extension_penalty;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.short_gap_extension_penalty = '2';
         * SHOW bioseq.aligner.scoring.short_gap_extension_penalty;
         * RESET bioseq.aligner.scoring.short_gap_extension_penalty;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int short_gap_extension_penalty = 2;

        /** @brief Sets the long-gap opening penalty.
         *
         * @remark Configuration: Sets KSW2's second gap-opening penalty. Native BlockAligner uses only the first gap-penalty pair.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.long_gap_open_penalty;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.long_gap_open_penalty = '24';
         * SHOW bioseq.aligner.scoring.long_gap_open_penalty;
         * RESET bioseq.aligner.scoring.long_gap_open_penalty;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int long_gap_open_penalty = 24;

        /** @brief Sets the long-gap extension penalty.
         *
         * @remark Configuration: Sets KSW2's second gap-extension penalty. Native BlockAligner does not use this second pair.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.long_gap_extension_penalty;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.long_gap_extension_penalty = '1';
         * SHOW bioseq.aligner.scoring.long_gap_extension_penalty;
         * RESET bioseq.aligner.scoring.long_gap_extension_penalty;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int long_gap_extension_penalty = 1;

        /** @brief Sets the penalty for ambiguous bases.
         *
         * @remark Configuration: Sets the alignment penalty for ambiguous symbols on paths that support them.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.ambiguous_base_penalty;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.ambiguous_base_penalty = '1';
         * SHOW bioseq.aligner.scoring.ambiguous_base_penalty;
         * RESET bioseq.aligner.scoring.ambiguous_base_penalty;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 0,
            .maximum = 127,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int ambiguous_base_penalty = 1;

        /** @brief Stop  extension when its score falls too far below the besw score.
         * Z-drop adjusts the comparison for gaps. Negative one disables it.
         *
         * @remark Configuration: Sets KSW2's Z-drop threshold or BlockAligner's X-drop threshold for end extensions. BlockAligner extensions require a nonnegative value.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.z_drop_threshold;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.z_drop_threshold = '400';
         * SHOW bioseq.aligner.scoring.z_drop_threshold;
         * RESET bioseq.aligner.scoring.z_drop_threshold;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = -1,
            .maximum = 1 << 20,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int z_drop_threshold = 400;

        /** @brief Bonus for an alignment that reaches the query end.
         * Negative one disables the bonus
         *
         * @remark Configuration: Sets KSW2's sequence-end bonus. Negative one disables the bonus. Native BlockAligner does not use it.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.scoring.end_bonus;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.scoring.end_bonus = '-1';
         * SHOW bioseq.aligner.scoring.end_bonus;
         * RESET bioseq.aligner.scoring.end_bonus;
         * ROLLBACK;
         * ```
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

        /** @brief Ignore minimizers with more target occurrences than this limit.
         *
         * @remark Configuration: Excludes minimizer keys with more target occurrences than this limit.
         * @remark Display unit: positions
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.seeding.maximum_occurrence;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.seeding.maximum_occurrence = '500';
         * SHOW bioseq.aligner.seeding.maximum_occurrence;
         * RESET bioseq.aligner.seeding.maximum_occurrence;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 24,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int maximum_occurrence = 500;

        /** @brief Store an intermediate occurrence threshold in the request.
         * The current seeding code uses maximum_occurrence instead.
         *
         * @remark Configuration: Currently unused
         * @remark Display unit: positions
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.seeding.intermediate_occurrence_threshold;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.seeding.intermediate_occurrence_threshold = '500';
         * SHOW bioseq.aligner.seeding.intermediate_occurrence_threshold;
         * RESET bioseq.aligner.seeding.intermediate_occurrence_threshold;
         * ROLLBACK;
         * ```
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

        /** @brief Sets the minimum block size used by the block aligner.
         *
         * @remark Configuration: Sets BlockAligner's minimum adaptive block size. (block sizes must be powers of two and the minimum cannot exceed the maximum)
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.block_alignment.minimum_block_size;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.block_alignment.minimum_block_size = '32';
         * SHOW bioseq.aligner.block_alignment.minimum_block_size;
         * RESET bioseq.aligner.block_alignment.minimum_block_size;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 8,
            .maximum = 4'096,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int minimum_block_size = 32;

        /** @brief Sets the maximum block size used by the block aligner.
         *
         * @remark Configuration: Sets BlockAligner's maximum adaptive block size. (block sizes must be powers of two and the minimum cannot exceed the maximum)
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.block_alignment.maximum_block_size;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.block_alignment.maximum_block_size = '256';
         * SHOW bioseq.aligner.block_alignment.maximum_block_size;
         * RESET bioseq.aligner.block_alignment.maximum_block_size;
         * ROLLBACK;
         * ```
         */
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

        /** @brief Selects the base-level alignment algorithm.
         *
         * @remark Configuration: Selects the base alignment algorithm
         * @remark Value `block_aligner`: Adaptive block aligner
         * @remark Value `ksw2`: Use KSW2 aligner
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.base_alignment.algorithm;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.base_alignment.algorithm = 'block_aligner';
         * SHOW bioseq.aligner.base_alignment.algorithm;
         * RESET bioseq.aligner.base_alignment.algorithm;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline bioseq::postgres::support::postgres_enum_storage<base_alignment_algorithm>
            algorithm = bioseq::postgres::support::postgres_enum_storage<base_alignment_algorithm>{
                base_alignment_algorithm::block_aligner};
    };

    /** Settings for  reading stored minimizer fragments. */
    struct fragment_minimizer final {
        fragment_minimizer() = delete;

        /**
         * @brief Choose control-table scans or direct compact, hash lookup.
         *
         * Direct lookup applies to stored fragments with 32-bit hashes.
         * Opening that route requires a verified compact index image.
         *
         * @remark Configuration: Changes how grouped MPF works when reading is performed The directory row should be faster in theory, but it's disabled, because I'm not sure if it's correct
         * @remark Value `direct_compact_hash_lookup`: By default read hash directly from the hash bucket.
         * @remark Value `control_block_scan`: Search the directory row for the hash bucket first
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.fragment_minimizer.lookup_route;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.fragment_minimizer.lookup_route = 'direct_compact_hash_lookup';
         * SHOW bioseq.aligner.fragment_minimizer.lookup_route;
         * RESET bioseq.aligner.fragment_minimizer.lookup_route;
         * ROLLBACK;
         * ```
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

        /** @brief Build minimizer generations in grouped arrays with source locators.
         *
         * @remark Configuration: Selects if we want to use grouped MPF version of index or non-grouped one (for writing only) Changing this does not rewrite existing indexes, only applies to new ones and does not affect reading!
         * @remark Value `true`: Prefer grouped version of MPF index over non-grouped (for writing only)
         * @remark Value `false`: Use only non-grouped MPF when possible (for writing only)
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.grouped_soa_generation.enabled;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.grouped_soa_generation.enabled = 'on';
         * SHOW bioseq.aligner.grouped_soa_generation.enabled;
         * RESET bioseq.aligner.grouped_soa_generation.enabled;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc]]
        static inline bool enabled = true;
    };

    /** Settings for GPU alignment through Vulkan. */
    struct vulkan final {
        vulkan() = delete;

        /** @brief Allows execution through Vulkan-backed algorithms.
         *
         * @remark Configuration: Enables GPU execution GPU algorithms currently are not optimised and severly increase the query time. Enabling execution doesn't mean the GPU will be used (for example software-emulated GPUs are disabled by default). The correct GPU algorithm also need to be selected by setting respective GUCs for aligner/chaining.
         * @remark Value `true`: Allow supported GPU work
         * @remark Value `false`: Setting aligner/chaining algorithms (or other) to Vulkan will immediately fail.
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.vulkan.enabled;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.vulkan.enabled = 'on';
         * SHOW bioseq.aligner.vulkan.enabled;
         * RESET bioseq.aligner.vulkan.enabled;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline bool enabled = true;

        /** GPU lookup of matching seed positions. */
        struct seed_lookup final {
            seed_lookup() = delete;

            /** @brief Enables Vulkan seed-hit lookup.
             *
             * @remark Configuration: Enabled GPU-acclelerated minimizers (reading)
             * @remark Value `false`: Disable GPU for reading minimizer indexes
             * @remark Value `true`: Enable GPU minimizer lookup (but other GPU settings apply)
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.aligner.vulkan.seed_lookup.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.aligner.vulkan.seed_lookup.enabled = 'off';
             * SHOW bioseq.aligner.vulkan.seed_lookup.enabled;
             * RESET bioseq.aligner.vulkan.seed_lookup.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** Reuse target minimizer indexes already uploaded to the GPU. */
        struct index_cache final {
            index_cache() = delete;

            /** @brief Keep uploaded target indexes for later GPU lookups.
             *
             * @remark Configuration: Cache minimizer index data to be reused across GPU calls (applies only to single pipeline execution and transfers between GPU/CPU memory)
             * @remark Value `true`: Save buffer with minimizers
             * @remark Value `false`: GPU doesn't reuse minimizers during computation
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.aligner.vulkan.index_cache.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.aligner.vulkan.index_cache.enabled = 'on';
             * SHOW bioseq.aligner.vulkan.index_cache.enabled;
             * RESET bioseq.aligner.vulkan.index_cache.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = true;
        };

        /** Run minimizer extraction and seeding toether  on the GPU. */
        struct fused_frontend final {
            fused_frontend() = delete;

            /** @brief Enable the GPU path that combines sketching and seed lookup.
             *
             * @remark Configuration: Enable experimental GPU fused kernel (this doesn't work correctly)
             * @remark Value `false`: Leave this frontend unselected.
             * @remark Value `true`: Enable special optimisation for GPU kernels
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.aligner.vulkan.fused_frontend.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.aligner.vulkan.fused_frontend.enabled = 'off';
             * SHOW bioseq.aligner.vulkan.fused_frontend.enabled;
             * RESET bioseq.aligner.vulkan.fused_frontend.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** GPU construction of target minimizer indexes. */
        struct index_build final {
            index_build() = delete;

            /** @brief Builds the target minimizer index on the GPU.
             *
             * @remark Configuration: Enabled GPU-acclelerated minimizers (writing)
             * @remark Value `false`: Disable GPU for wrikting minimizer indexes
             * @remark Value `true`: Enable GPU minimizer index building (but other GPU settings apply)
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.aligner.vulkan.index_build.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.aligner.vulkan.index_build.enabled = 'off';
             * SHOW bioseq.aligner.vulkan.index_build.enabled;
             * RESET bioseq.aligner.vulkan.index_build.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** GPU extension of chains into base alignments. */
        struct alignment_extension final {
            alignment_extension() = delete;

            /** @brief Enables GPU base-alignment extension.
             *
             * @remark Configuration: Enabled CIGAR computation on GPU
             * @remark Value `false`: Disbable CIGAR computation on GPU
             * @remark Value `true`: Allow computing CIGAR on GPU (but other GPU settings apply)
             *
             * ```sql
             * -- Inspect the current value without changing the session.
             * SHOW bioseq.aligner.vulkan.alignment_extension.enabled;
             * ```
             *
             * ```sql
             * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
             * BEGIN;
             * SET LOCAL bioseq.aligner.vulkan.alignment_extension.enabled = 'off';
             * SHOW bioseq.aligner.vulkan.alignment_extension.enabled;
             * RESET bioseq.aligner.vulkan.alignment_extension.enabled;
             * ROLLBACK;
             * ```
             */
            [[bioseq_sql::guc({
                .plan_cache = bioseq::sql::plan_cache_policy::reset,
            })]]
            static inline bool enabled = false;
        };

        /** @brief Sets the ordered-sketch tile size in base pairs.
         *
         * @remark Configuration: Currently unused
         * @remark Display unit: symbols
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.vulkan.ordered_sketch_tile_bp;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.vulkan.ordered_sketch_tile_bp = '2048';
         * SHOW bioseq.aligner.vulkan.ordered_sketch_tile_bp;
         * RESET bioseq.aligner.vulkan.ordered_sketch_tile_bp;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc({
            .minimum = 1,
            .maximum = 1 << 24,
            .plan_cache = bioseq::sql::plan_cache_policy::reset,
        })]]
        static inline int ordered_sketch_tile_bp = 2'048;

        /** @brief Store the requested radix digit width in the request.
         * The current GPU radix kernels process eight bits per pass.
         *
         * @remark Configuration: Currently unused
         * @remark Display unit: bits
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.vulkan.radix_sort_digit_bits;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.vulkan.radix_sort_digit_bits = '8';
         * SHOW bioseq.aligner.vulkan.radix_sort_digit_bits;
         * RESET bioseq.aligner.vulkan.radix_sort_digit_bits;
         * ROLLBACK;
         * ```
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

        /** @brief Enables runtime profiling for the aligner pipeline.
         *
         * @remark Configuration: Enables internal profiling of the pipeline execution
         * @remark Value `false`: profiling measures statistics like number of minimizers/chains/timings per pipeline stage etc.
         * @remark Value `true`: Profiling can increase execution time
         *
         * ```sql
         * -- Inspect the current value without changing the session.
         * SHOW bioseq.aligner.profiling.enabled;
         * ```
         *
         * ```sql
         * -- Use the compiled default for one transaction; RESET removes the override and ROLLBACK restores the previous session state.
         * BEGIN;
         * SET LOCAL bioseq.aligner.profiling.enabled = 'off';
         * SHOW bioseq.aligner.profiling.enabled;
         * RESET bioseq.aligner.profiling.enabled;
         * ROLLBACK;
         * ```
         */
        [[bioseq_sql::guc]]
        static inline bool enabled = false;
    };
};

} // namespace bioseq::aligner
