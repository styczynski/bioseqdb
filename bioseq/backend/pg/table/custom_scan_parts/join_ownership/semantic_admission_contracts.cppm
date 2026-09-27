/**
 * @file
 * @brief Declares checks for BioseqScan operands and PerRow join outputs.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_semantic_admission_contracts;

export import :custom_scan_join_ownership_cost_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief The join and sequence columns ued by an operand that may move to  a PerRow owner */
export struct per_row_operand_source_resolution {
    RelOptInfo* joinrel = nullptr;
    List* sequence_vars = NIL;
    bool invalid = false;
};

/** @brief Input columns and whole match helpers needed to bind a PerRow join's final output. */
export struct per_row_owner_support_validation {
    Index target_relid = 0;
    Relids join_relids = nullptr;
    const bioseq_stub_oids* oids = nullptr;
    Node* target_recipe = nullptr;
    List* passthrough_vars = NIL;
    List* helper_identities = NIL;
    /* A helper naming a different sequence expression may belong to a
     * descendant BatchJoin. Keep it as a candidate until the selected
     * path confirms that owner. */
    List* foreign_exact_helper_identities = NIL;
    List* helper_argument_vars = NIL;
    bool invalid = false;
};

/**
 * @brief A scan-local mmsearch operand, its source columns, and its sequence type.
 *
 * PostgreSQL evaluates the original ExprState at the scan, preserving the
 * order and short-circuit behavior of volatile functions, CASE, and
 * COALESCE. Every Var must belong to this non-nullable  relation. Reject
 * aggregates, window functions, subplans, PlaceHolderVars, and, set-returning
 * expressions because their evaluation belongs elsewhere.
 */
export struct base_local_mmsearch_operand_resolution {
    Node* expression = nullptr;
    List* sequence_vars = NIL;
    Var* primary_sequence_var = nullptr;
    int32 typmod = -1;
    bool evaluated = false;
};

/** @brief The target binding and the kind of query input for one BioseqScan search. */
export struct base_local_mmsearch_ownership {
    base_local_mmsearch_operand_resolution target{};
    std::optional<base_local_mmsearch_operand_resolution> local_query{};
    bool query_is_const = false;
    bool query_is_param = false;
    bool query_is_outer_var = false;
    bool any_evaluated_operand = false;
};

/** @brief The alphabet and storage width found for a scan-local sequence expression. */
export struct base_local_sequence_typmod_resolution {
    int32 typmod = -1;
    bool invalid = false;
};

/** @brief The next token used to pair BatchJoin plan components. */
export int next_batch_join_plan_token = 1;

/** @brief Stores a finite planner cost in a float8 Const for the plan. */
export [[nodiscard]] Const* make_batch_join_cost_const(Cost value);

/** @brief Appends a copy of the expression unless an equal expression is already present. */
export void per_row_append_unique_expression(List*& expressions, Node* node);

/** @brief Copies the join's output with matching  sort and group references. */
export [[nodiscard]] PathTarget* per_row_build_owner_target(
    PlannerInfo* root,
    PathTarget* original);

/** @brief Checks that the PerRow joyn can produce this output with the same Var and helper bindings. */
export [[nodiscard]] bool per_row_output_semantics_are_supported(
    PathTarget* target,
    Index target_relid,
    Relids join_relids,
    Node* target_recipe);

/** @brief Collects hidden input columns needed when PostgreSQL fixes plan references. */
export [[nodiscard]] bool per_row_collect_setrefs_carrier_vars(
    Node* target_recipe,
    List* helper_argument_vars,
    Relids join_relids,
    List*& carrier_vars);

/** @brief Collects the final output's input columns and match helpers for this PerRow join */
export [[nodiscard]] bool per_row_collect_owner_support(
    PlannerInfo* root,
    RelOptInfo* joinrel,
    Index target_relid,
    Node* target_recipe,
    per_row_owner_support_validation& validation);

/** @brief Rejects volatile join operands and collects the sequence columns they use. */
export [[nodiscard]] bool per_row_collect_operand_sources(
    Node* expression,
    RelOptInfo* joinrel,
    per_row_operand_source_resolution& resolution);

/** @brief Checks a scan-local sequence expression and determines its input columns and type. */
export [[nodiscard]] std::optional<base_local_mmsearch_operand_resolution>
resolve_base_local_mmsearch_operand(Node* expression,
                                    RelOptInfo* rel,
                                    const bioseq_stub_oids& oids,
                                    const bool require_sequence_var);

/** @brief Finds the sequence alphabet and storage width through supported expressions. */
export [[nodiscard]] base_local_sequence_typmod_resolution
resolve_base_local_sequence_typmod(Node* expression, const bioseq_stub_oids& oids);

/** @brief Checks whether a BioseqScan can own this mmsearch call at its current evaluation level. */
export [[nodiscard]] std::optional<base_local_mmsearch_ownership>
resolve_base_local_mmsearch_ownership(FuncExpr* function,
                                      RelOptInfo* rel,
                                      Relids param_relids,
                                      const bioseq_stub_oids& oids);
