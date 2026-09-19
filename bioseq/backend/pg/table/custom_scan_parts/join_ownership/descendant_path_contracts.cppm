/**
 * @file
 * @brief Declares descendant ownership and copy-on-write path operations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_descendant_path_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief Summarizes exact helper ownership found in a path subtree. */
export struct per_row_descendant_ownership {
    CustomPath* owner = nullptr;
    std::size_t matches = 0u;
    bool invalid_owner_abi = false;
    bool unsupported_wrapper = false;
};

/**
 * @brief Summarizes base-scan helper ownership below a reconstructed owner.
 *
 * A reconstructed transformed owner is deliberately narrower than the
 * incumbent join relation: helpers owned by the new PerRow node must not leak
 * into a descendant BioseqScan or an older BatchJoin.  PostgreSQL Path nodes
 * are shared, so normalize only after slice_project_clone_mutable_path_spine()
 * has made every supported container private.  Availability is scoped to the
 * current subtree: a descendant-owned helper is retained at its owner and
 * above it, but is removed from every child below that owner.
 */
export struct per_row_base_helper_ownership {
    CustomPath* owner = nullptr;
    std::size_t matches = 0u;
    bool invalid_owner_abi = false;
    bool unsupported_wrapper = false;
};

/** @brief Return a shallow restriction list without one pointer-owned clause. */
export [[nodiscard]] List* per_row_without_restriction(
    List* restrictions,
    RestrictInfo* owned);

/** @brief Recursively check whether a descendant retains one restriction pointer. */
export [[nodiscard]] bool per_row_descendant_owns_restriction(
    const Path* path,
    const RestrictInfo* owned) noexcept;

/** @brief Find the cheapest serial path whose descendants do not own a restriction. */
export [[nodiscard]] Path* per_row_path_without_owned_qual(
    RelOptInfo* rel,
    RestrictInfo* owned,
    bool require_bioseq_source) noexcept;

/** @brief Project required raw Vars through a private child-path wrapper. */
export [[nodiscard]] Path* per_row_project_required_child_vars(
    PlannerInfo* root,
    Path* child,
    List* required_vars);

/** @brief Find exact helper owners through supported planner path wrappers. */
export void per_row_analyze_descendant_exact_owner(
    Path* path,
    Node* helper_argument,
    per_row_descendant_ownership& ownership);

/** @brief Clone the path spine down to one helper owner and widen that spine. */
export [[nodiscard]] Path* per_row_clone_path_to_helper_owner(
    PlannerInfo* root,
    Path* path,
    CustomPath* owner,
    Node* identity,
    bool& applied,
    bool& invalid);

/** @brief Decode the match-helper function from a plain or PHV identity. */
export [[nodiscard]] FuncExpr* per_row_helper_identity_function(
    Node* identity,
    const bioseq_stub_oids& oids) noexcept;
