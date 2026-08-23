/**
 * @file
 * @brief Declares planner-facing aligner policy without importing runtime implementations.
 */
module;
struct FuncExpr;
struct Const;

export module bioseq.postgres.table:custom_scan_planner_aligner_contracts;

import bioseq.indexing.type;
import bioseq.pipeline.minimap_parameters_contract;
import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Snapshot backend aligner settings into one planner parameter object. */
export [[nodiscard]] bioseq::simd::minimap2_parameters build_params_from_gucs();

/** @brief Report whether the selected minimizer accepts packed sequence regions. */
export [[nodiscard]] bool mmsearch_plans_admitted_packed_regions(
    const bioseq::simd::minimap2_parameters& params) noexcept;

/** @brief Return the optional plan-time aligner-options constant from an mmsearch call. */
export [[nodiscard]] Const* require_aligner_options_const(
    FuncExpr* mmsearch_fe,
    const bioseq_stub_oids& oids);

/** @brief Overlay an optional mmsearch aligner-options value on planner parameters. */
export [[nodiscard]] bool apply_aligner_options_override(
    FuncExpr* mmsearch_fe,
    bioseq::simd::minimap2_parameters& params,
    bioseq::indexing::index_read_requirement& index_read_requirement,
    const bioseq_stub_oids& oids,
    bool* vulkan_enabled_explicit = nullptr);

/** @brief Resolve the final SIMD chaining policy after all planner overrides. */
export void finalize_simd_chaining_policy(bioseq::simd::minimap2_parameters& params);
