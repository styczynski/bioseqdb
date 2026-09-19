/**
 * @file
 * @brief Declares per-row semantic admission without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_semantic_admission_contracts;

export import :custom_scan_join_ownership_cost_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief Accumulates sequence Vars referenced by a movable per-row operand. */
export struct per_row_operand_source_resolution {
    RelOptInfo* joinrel = nullptr;
    List* sequence_vars = NIL;
    bool invalid = false;
};

/** @brief Collects passthrough Vars and helper identities needed by owner setrefs. */
export struct per_row_owner_support_validation {
    Index target_relid = 0;
    Relids join_relids = nullptr;
    const bioseq_stub_oids* oids = nullptr;
    Node* target_recipe = nullptr;
    List* passthrough_vars = NIL;
    List* helper_identities = NIL;
    /* Sequence-valued helpers which do not name this owner may still be exact
     * outputs of one versioned descendant BatchJoin.  They remain candidates
     * until each incumbent path validates that ownership structurally. */
    List* foreign_exact_helper_identities = NIL;
    List* helper_argument_vars = NIL;
    bool invalid = false;
};

/**
 * @brief Describes an admitted base-local mmsearch operand and its source Vars.
 *
 * Exact baserel operand validation for BioseqScan-owned mmsearch. Unlike the
 * join relocation validation above, this expression is not moved across a planner
 * frontier: its original post-setrefs ExprState remains authoritative at the
 * scan.  Volatile functions and opaque/erroring CASE/COALESCE arms are thus
 * legal and execute once in PostgreSQL order.  The remaining validation
 * requires that no aggregate/window/subplan/PHV/SRF frontier is crossed and that every
 * Var belongs to this non-nullable scan relation.
 */
export struct base_local_mmsearch_operand_resolution {
    Node* expression = nullptr;
    List* sequence_vars = NIL;
    Var* primary_sequence_var = nullptr;
    int32 typmod = -1;
    bool evaluated = false;
};

/** @brief Classifies the target and query bindings owned by one base-scan mmsearch. */
export struct base_local_mmsearch_ownership {
    base_local_mmsearch_operand_resolution target{};
    std::optional<base_local_mmsearch_operand_resolution> local_query{};
    bool query_is_const = false;
    bool query_is_param = false;
    bool query_is_outer_var = false;
    bool any_evaluated_operand = false;
};

/** @brief Carries the exact sequence typmod derived for a base-local expression. */
export struct base_local_sequence_typmod_resolution {
    int32 typmod = -1;
    bool invalid = false;
};

/** @brief Monotonic statement-local token used to pair BatchJoin plan components. */
export int next_batch_join_plan_token = 1;

/** @brief Encode a finite planner cost as a FLOAT8 Const. */
export [[nodiscard]] Const* make_batch_join_cost_const(Cost value);

/** @brief Append one expression when an equal expression is not already present. */
export void per_row_append_unique_expression(List*& expressions, Node* node);

/** @brief Build the projection target emitted by a per-row owner. */
export [[nodiscard]] PathTarget* per_row_build_owner_target(
    PlannerInfo* root,
    PathTarget* original);

/** @brief Check whether a join target can be emitted by a per-row owner. */
export [[nodiscard]] bool per_row_output_semantics_are_supported(
    PathTarget* target,
    Index target_relid,
    Relids join_relids,
    Node* target_recipe);

/** @brief Collect Plan-only Vars needed to bind a transformed target during setrefs. */
export [[nodiscard]] bool per_row_collect_setrefs_carrier_vars(
    Node* target_recipe,
    List* helper_argument_vars,
    Relids join_relids,
    List*& carrier_vars);

/** @brief Collect final-output support expressions for a candidate per-row owner. */
export [[nodiscard]] bool per_row_collect_owner_support(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    Index target_relid,
    Node* target_recipe,
    per_row_owner_support_validation& validation);

/** @brief Collect nonvolatile sequence sources for a movable join operand. */
export [[nodiscard]] bool per_row_collect_operand_sources(
    Node* expression,
    RelOptInfo* joinrel,
    per_row_operand_source_resolution& resolution);

/** @brief Resolve a base-local sequence expression that may be evaluated by the scan. */
export [[nodiscard]] std::optional<base_local_mmsearch_operand_resolution>
resolve_base_local_mmsearch_operand(Node* expression,
                                    RelOptInfo* rel,
                                    const bioseq_stub_oids& oids,
                                    const bool require_sequence_var);

/** @brief Derive a compatible sequence typmod through supported expression shapes. */
export [[nodiscard]] base_local_sequence_typmod_resolution
resolve_base_local_sequence_typmod(Node* expression, const bioseq_stub_oids& oids);

/** @brief Resolve an mmsearch call that a base relation can own without semantic movement. */
export [[nodiscard]] std::optional<base_local_mmsearch_ownership>
resolve_base_local_mmsearch_ownership(FuncExpr* function,
                                      RelOptInfo* rel,
                                      Relids param_relids,
                                      const bioseq_stub_oids& oids);
