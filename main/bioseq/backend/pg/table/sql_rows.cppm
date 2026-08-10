module;

#include <cstdint>
#include <optional>
#include <type_traits>

export module bioseq.postgres.table.sql_rows;

import bioseq.codegen.sql.annotation_model;

export namespace bioseq::table::composite_rows {

/**
 * @brief Per-call aligner tuning options for the mmsearch similarity predicate.
 *
 * NULL fields inherit the corresponding session aligner setting.
 *
 * @ingroup bioseq_sql_public_api
 */
struct [[bioseq_sql::type({
            .schema = bioseq::sql::extension_schema,
        })]] bioseq_aligner_options {
    std::optional<std::int32_t> kmer_size;
    std::optional<std::int32_t> window_size;
    std::optional<std::int32_t> max_gap;
    std::optional<std::int32_t> max_gap_ref;
    std::optional<std::int32_t> bandwidth;
    std::optional<std::int32_t> bandwidth_long;
    std::optional<std::int32_t> max_chain_skip;
    std::optional<std::int32_t> max_chain_iter;
    std::optional<std::int32_t> min_seed_count;
    std::optional<std::int32_t> min_chain_score;
    std::optional<double> chain_gap_scale;
    std::optional<double> chain_skip_scale;
    std::optional<double> mask_level;
    std::optional<std::int32_t> mask_len;
    std::optional<double> pri_ratio;
    std::optional<std::int32_t> best_n;
    std::optional<std::int32_t> match_score;
    std::optional<std::int32_t> mismatch_pen;
    std::optional<std::int32_t> gap_open1;
    std::optional<std::int32_t> gap_ext1;
    std::optional<std::int32_t> gap_open2;
    std::optional<std::int32_t> gap_ext2;
    std::optional<std::int32_t> sc_ambi;
    std::optional<std::int32_t> zdrop;
    std::optional<std::int32_t> end_bonus;
    std::optional<std::int32_t> max_occ;
    std::optional<std::int32_t> mid_occ;
    std::optional<std::int32_t> block_min_size;
    std::optional<std::int32_t> block_max_size;
    std::optional<std::int32_t> chaining_algorithm;
    std::optional<std::int32_t> base_alignment_algorithm;
    std::optional<std::int32_t> minimizer_algorithm;
    std::optional<bool> vulkan_enabled;
    std::optional<bool> vulkan_seed_hits_enabled;
    std::optional<bool> vulkan_index_cache_enabled;
    std::optional<std::int32_t> index_read_requirement;
};

static_assert(std::is_aggregate_v<bioseq_aligner_options>);

}  // namespace bioseq::table::composite_rows

export namespace bioseq::table {

/** Counts observed while removing Bioseq relations. */
struct teardown_diagnostics_row {
    std::int64_t lock_fallback_classifications;
    std::int64_t missing_relation_observations;
};

static_assert(std::is_aggregate_v<teardown_diagnostics_row>);

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

static_assert(
    std::is_aggregate_v<batch_join_projection_guard_probe_row>);

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

static_assert(
    std::is_aggregate_v<
        match_helper_wrapper_propagation_probe_row>);

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

static_assert(
    std::is_aggregate_v<
        base_match_executor_translation_probe_row>);

struct match_owner_finalization_scope_diagnostics_row {
    std::int64_t scopes_entered;
    std::int64_t scopes_exited;
    std::int64_t scopes_active;
};

static_assert(
    std::is_aggregate_v<
        match_owner_finalization_scope_diagnostics_row>);

}  // namespace bioseq::table
