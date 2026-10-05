/**
 * @file
 * SQL row types for aligner options and planner diagnostics.
 */
module;

#include <cstdint>
#include <optional>
#include <type_traits>

export module bioseq.postgres.table.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::table::composite_rows {

/**
 * @brief Aligner settings for one mmsearch call.
 * NULL fields use the corresponding session setting.
 *
 * @ingroup bioseq_sql_public_api
 */
struct [[bioseq_sql::type({
    .schema = bioseq::sql::extension_schema,
})]] bioseq_aligner_options {
    /** @brief Number of bases in each k-mer considered for a minimizer.
     * NULL uses the session value of bioseq.aligner.minimizer.kmer_size.
     */
    std::optional<std::int32_t> kmer_size;
    /** @brief Number of consecutive, k-mers in a  minimizer window.
     * NULL uses the session value of bioseq.aligner.minimizer.window_size.
     */
    std::optional<std::int32_t> window_size;
    /** @brief Maximum diqtance between anchors that chaining can join.
     * NULL uses the session value of bioseq.aligner.chaining.maximum_gap.
     */
    std::optional<std::int32_t> max_gap;
    /** @brief Store a reference-gap override in the request. The current chaining code uses maximum_gap for both sequences.
     * NULL uses the session value of bioseq.aligner.chaining.maximum_reference_gap.
     */
    std::optional<std::int32_t> max_gap_ref;
    /** @brief Sets the chaining and alignment bandwidth.
     * NULL uses the session value of bioseq.aligner.chaining.bandwidth.
     */
    std::optional<std::int32_t> bandwidth;
    /** @brief Store a long-join bandwidth in the request. The current pipeline does not use this setting.
     * NULL uses the session value of bioseq.aligner.chaining.long_join_bandwidth.
     */
    std::optional<std::int32_t> bandwidth_long;
    /** @brief Stop searching earlier anchors after this many unhelpful candidates.
     * NULL uses the session value of bioseq.aligner.chaining.maximum_skipped_anchors.
     */
    std::optional<std::int32_t> max_chain_skip;
    /** @brief Limits predecessors examined for each anchor.
     * NULL uses the session value of bioseq.aligner.chaining.maximum_predecessors.
     */
    std::optional<std::int32_t> max_chain_iter;
    /** @brief Sets the minimum seed count accepted for a chain.
     * NULL uses the session value of bioseq.aligner.chaining.minimum_seed_count.
     */
    std::optional<std::int32_t> min_seed_count;
    /** @brief Sets the minimum accepted chain score.
     * NULL uses the session value of bioseq.aligner.chaining.minimum_score.
     */
    std::optional<std::int32_t> min_chain_score;
    /** @brief Scales the chaining gap penalty.
     * NULL uses the session value of bioseq.aligner.chaining.gap_penalty_scale.
     */
    std::optional<double> chain_gap_scale;
    /** @brief Scales the chaining skip penalty.
     * NULL uses the session value of bioseq.aligner.chaining.skip_penalty_scale.
     */
    std::optional<double> chain_skip_scale;
    /** @brief Required query overlap for assigning an alignment to a primary. The  comparison also accounts for query bases left uncovered by primaries.
     * NULL uses the session value of bioseq.aligner.secondary.overlap_threshold.
     */
    std::optional<double> mask_level;
    /** @brief Maximum uncovered query length allowed for a secondary alignment.
     * NULL uses the session value of bioseq.aligner.secondary.uncovered_length_limit.
     */
    std::optional<std::int32_t> mask_len;
    /** @brief Score ratio used to keep a secondary relative to its primary. A secondary can also pass within the allowed absolute score difference. Zero disables the CPU secondary filter.
     * NULL uses the session value of bioseq.aligner.secondary.score_ratio_threshold.
     */
    std::optional<double> pri_ratio;
    /** @brief Maximum secondary alignments kept across one query.
     * NULL uses the session value of bioseq.aligner.secondary.maximum_count.
     */
    std::optional<std::int32_t> best_n;
    /** @brief Sets the alignment match score.
     * NULL uses the session value of bioseq.aligner.scoring.match_score.
     */
    std::optional<std::int32_t> match_score;
    /** @brief Sets the alignment mismatch penalty.
     * NULL uses the session value of bioseq.aligner.scoring.mismatch_penalty.
     */
    std::optional<std::int32_t> mismatch_pen;
    /** @brief Sets the short-gap opening penalty.
     * NULL uses the session value of bioseq.aligner.scoring.short_gap_open_penalty.
     */
    std::optional<std::int32_t> gap_open1;
    /** @brief Sets the short-gap extension penalty.
     * NULL uses the session value of bioseq.aligner.scoring.short_gap_extension_penalty.
     */
    std::optional<std::int32_t> gap_ext1;
    /** @brief Sets the long-gap opening penalty.
     * NULL uses the session value of bioseq.aligner.scoring.long_gap_open_penalty.
     */
    std::optional<std::int32_t> gap_open2;
    /** @brief Sets the long-gap extension penalty.
     * NULL uses the session value of bioseq.aligner.scoring.long_gap_extension_penalty.
     */
    std::optional<std::int32_t> gap_ext2;
    /** @brief Sets the penalty for ambiguous bases.
     * NULL uses the session value of bioseq.aligner.scoring.ambiguous_base_penalty.
     */
    std::optional<std::int32_t> sc_ambi;
    /** @brief Stop  extension when its score falls too far below the besw score. Z-drop adjusts the comparison for gaps. Negative one disables it.
     * NULL uses the session value of bioseq.aligner.scoring.z_drop_threshold.
     */
    std::optional<std::int32_t> zdrop;
    /** @brief Bonus for an alignment that reaches the query end. Negative one disables the bonus.
     * NULL uses the session value of bioseq.aligner.scoring.end_bonus.
     */
    std::optional<std::int32_t> end_bonus;
    /** @brief Ignore minimizers with more target occurrences than this limit.
     * NULL uses the session value of bioseq.aligner.seeding.maximum_occurrence.
     */
    std::optional<std::int32_t> max_occ;
    /** @brief Store an intermediate occurrence threshold in the request. The current seeding code uses maximum_occurrence instead.
     * NULL uses the session value of bioseq.aligner.seeding.intermediate_occurrence_threshold.
     */
    std::optional<std::int32_t> mid_occ;
    /** @brief Sets the minimum block size used by the block aligner.
     * NULL uses the session value of bioseq.aligner.block_alignment.minimum_block_size.
     */
    std::optional<std::int32_t> block_min_size;
    /** @brief Sets the maximum block size used by the block aligner.
     * NULL uses the session value of bioseq.aligner.block_alignment.maximum_block_size.
     */
    std::optional<std::int32_t> block_max_size;
    /** @brief Selects the seed-anchor chaining algorithm.
     * Values: 0 scalar_minimap2, 1 simd_minimap2, 2 range_minimum_query, 3 vulkan_tiled_dynamic_programming.
     * NULL uses the session value of bioseq.aligner.chaining.algorithm.
     */
    std::optional<std::int32_t> chaining_algorithm;
    /** @brief Selects the base-level alignment algorithm.
     * Values: 0 ksw2, 1 block_aligner.
     * NULL uses the session value of bioseq.aligner.base_alignment.algorithm.
     */
    std::optional<std::int32_t> base_alignment_algorithm;
    /** @brief Selects the minimizer extraction algorithm.
     * Values: 0 scalar, 1 simd_minimap2_reference, 2 two_stack_nthash, 3 vulkan_minimap_integer, 4 bounded_nthash_simd.
     * NULL uses the session value of bioseq.aligner.minimizer.algorithm.
     */
    std::optional<std::int32_t> minimizer_algorithm;
    /** @brief Allows execution through Vulkan-backed algorithms.
     * NULL uses the session value of bioseq.aligner.vulkan.enabled.
     */
    std::optional<bool> vulkan_enabled;
    /** @brief Enables Vulkan seed-hit lookup.
     * NULL uses the session value of bioseq.aligner.vulkan.seed_lookup.enabled.
     */
    std::optional<bool> vulkan_seed_hits_enabled;
    /** @brief Keep uploaded target indexes for later GPU lookups.
     * NULL uses the session value of bioseq.aligner.vulkan.index_cache.enabled.
     */
    std::optional<bool> vulkan_index_cache_enabled;
    /** @brief Sets the requirement for using a stored minimizer index.
     * Values: 0 allows a compatible stored index or an index built for the query;
     * 1 requires the persisted index to be absent; 2 requires a compatible persisted index.
     * NULL keeps the planner's default index-read requirement.
     */
    std::optional<std::int32_t> index_read_requirement;
};

static_assert(std::is_aggregate_v<bioseq_aligner_options>);

} // namespace bioseq::table::composite_rows

export namespace bioseq::table {

/** Counts observed while removing Bioseq relations. */
struct teardown_diagnostics_row {
    std::int64_t lock_fallback_classifications;
    std::int64_t missing_relation_observations;
};

static_assert(std::is_aggregate_v<teardown_diagnostics_row>);

/** Test results for projections that a batched mmsearch join can safely read. */
struct batch_join_projection_guard_probe_row {
    bool accepts_exact_helper;
    bool rejects_unrelated_phv;
    bool rejects_volatile_mixed_expression;
    bool preserves_wrapper_cost_owner;
    bool accepts_plain_target_var;
    bool rejects_plain_target_sequence_var;
    bool rejects_whole_row_var;
    bool rejects_system_var;
    bool rejects_nulling_var;
};

static_assert(std::is_aggregate_v<batch_join_projection_guard_probe_row>);

/** Test results for carrying a match helper through copied planner wrappers. */
struct match_helper_wrapper_propagation_probe_row {
    bool exact_owner_found;
    bool clone_applied;
    bool owner_cloned;
    bool projection_cloned;
    bool material_cloned;
    bool memoize_cloned;
    bool unique_cloned;
    bool append_cloned;
    bool helper_on_owner;
    bool helper_on_projection;
    bool helper_on_material;
    bool helper_on_memoize;
    bool helper_on_unique;
    bool helper_on_append;
    bool originals_unchanged;
    bool sibling_unchanged;
    bool duplicate_owner_rejected;
    bool unknown_wrapper_rejected;
    bool group_result_qual_rejected;
};

static_assert(std::is_aggregate_v<match_helper_wrapper_propagation_probe_row>);

/** Test results for keeping match arguments tied to the correct  executor columns. */
struct base_match_executor_translation_probe_row {
    bool target_offset_accepted;
    bool target_attno_disagreement_rejected;
    bool exact_site_offset_accepted;
    bool lineage_alias_disagreement_rejected;
    bool outer_slot_accepted;
    bool outer_wrong_slot_rejected;
    bool inner_slot_accepted;
    bool inner_wrong_slot_rejected;
    bool index_slot_accepted;
    bool index_wrong_slot_rejected;
    bool special_out_of_range_rejected;
};

static_assert(std::is_aggregate_v<base_match_executor_translation_probe_row>);

/** Counts of entered, exited, and still-active match finalization scopes. */
struct match_owner_finalization_scope_diagnostics_row {
    std::int64_t scopes_entered;
    std::int64_t scopes_exited;
    std::int64_t scopes_active;
};

static_assert(std::is_aggregate_v<match_owner_finalization_scope_diagnostics_row>);

} // namespace bioseq::table
