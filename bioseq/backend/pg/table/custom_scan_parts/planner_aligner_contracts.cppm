/**
 * @file
 * @brief Declares how PostgreSQL settings and mmsearch options configure alignment.
 */
module;
struct FuncExpr;
struct Const;

export module bioseq.postgres.table:custom_scan_planner_aligner_contracts;

import bioseq.indexing.type;
import bioseq.pipeline.minimap_parameters_contract;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Copies the current PostgreSQL aligner settings into a parameter set. */
export [[nodiscard]] bioseq::simd::minimap2_parameters build_params_from_gucs();

/** @brief Checks whether the selected minimizer can read packed sequence regions. */
export [[nodiscard]] bool mmsearch_plans_admitted_packed_regions(
    const bioseq::simd::minimap2_parameters& params) noexcept;

/**
 * @brief Returns a plan-time options constant when one is present
 *
 * A null call or two-argument mmsearch has no options. Raise a PostgreSQL error
 * for an invlid call shape, an, incorrect options type, or nonconstant options.
 */
export [[nodiscard]] Const* require_aligner_options_const(
    FuncExpr* mmsearch_fe,
    const bioseq_stub_oids& oids);

/**
 * @brief Applies the non-NULL fields of an mmsearch options constant.
 *
 * Leave unspecified settings unchanged. Report an explicit Vulkan setting
 * through vulkan_enabled_explicit when supplied.
 *
 * @return Whether  the options explicitly set the index-read requirement.
 */
export [[nodiscard]] bool apply_aligner_options_override(
    FuncExpr* mmsearch_fe,
    bioseq::simd::minimap2_parameters& params,
    bioseq::indexing::index_read_requirement& index_read_requirement,
    const bioseq_stub_oids& oids,
    bool* vulkan_enabled_explicit = nullptr);

/**
 * @brief Sets the chaining policy after applying the call's options.
 *
 * SIMD minimap2 uses bounded_batch_approximate. The other supported algorithms
 * use exact_visited. Reject an unknown chaining algorithm.
 */
export void finalize_simd_chaining_policy(bioseq::simd::minimap2_parameters& params);
