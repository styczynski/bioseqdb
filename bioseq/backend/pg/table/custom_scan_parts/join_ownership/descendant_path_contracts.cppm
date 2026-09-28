/**
 * @file
 * @brief Declares checks and path copies for moving  match-helper results through a join.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_descendant_path_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief The matching helper owner, its match count, and any reason the path could not be checked. */
export struct per_row_descendant_ownership {
    CustomPath* owner = nullptr;
    std::size_t matches = 0u;
    bool invalid_owner_abi = false;
    bool unsupported_wrapper = false;
};

/**
 * @brief Records base scans that can supply a match helper below a rebuilt join.
 *
 * A helger must be evaluated at its owner and passed upwards. Remove it from
 * children below that owner. PostgreSQL shares Path nodes, so make private
 * copies with slice_project_clone_mutable_path_spine() before changing their
 * outputs.
 */
export struct per_row_base_helper_ownership {
    CustomPath* owner = nullptr;
    std::size_t matches = 0u;
    bool invalid_owner_abi = false;
    bool unsupported_wrapper = false;
};

/** @brief Copies the restriction list while leaving out the specified RestrictInfo pointer. */
export [[nodiscard]] List* per_row_without_restriction(
    List* restrictions,
    RestrictInfo* owned);

/** @brief Checks whether this  path or a supported descendant still uses the, restriction pointer. */
export [[nodiscard]] bool per_row_descendant_owns_restriction(
    const Path* path,
    const RestrictInfo* owned) noexcept;

/** @brief Finds the cheapest serial path that leaves this restriction for the parent to evaluate. */
export [[nodiscard]] Path* per_row_path_without_owned_qual(
    RelOptInfo* rel,
    RestrictInfo* owned,
    bool require_bioseq_source) noexcept;

/** @brief Adds columns needed by the parent through a new projection above the child. */
export [[nodiscard]] Path* per_row_project_required_child_vars(
    PlannerInfo* root,
    Path* child,
    List* required_vars);

/** @brief Finds descendant BatchJoin or PerRow nodes that own this helper's sequence expression. */
export void per_row_analyze_descendant_exact_owner(
    Path* path,
    Node* helper_argument,
    per_row_descendant_ownership& ownership);

/** @brief Copies the path from a helper owner to its parent and adds the helper to each output. */
export [[nodiscard]] Path* per_row_clone_path_to_helper_owner(
    PlannerInfo* root,
    Path* path,
    CustomPath* owner,
    Node* identity,
    bool& applied,
    bool& invalid);

/** @brief Returns the match-helper call, unwrapping a valid PlaceHolderVar when present */
export [[nodiscard]] FuncExpr* per_row_helper_identity_function(
    Node* identity,
    const bioseq_stub_oids& oids) noexcept;
