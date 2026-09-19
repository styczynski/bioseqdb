/**
 * @file
 * @brief Declares finalized-owner semantic analysis without importing recursive walkers.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_match_analysis_semantic_analysis_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_match_finalization_contracts;

/** @brief Separate finalized BatchJoin and base-scan owner counts. */
export struct finalized_match_owner_site_counts {
    std::size_t batch = 0u;
    std::size_t base = 0u;
};

/** @brief Check that every helper in a base-scan target list has one finalized owner site. */
export [[nodiscard]] bool base_scan_targetlist_has_only_finalized_match_helpers(
    CustomScan* scan,
    const bioseq_stub_oids& oids);

/** @brief Count whether a finalized base owner supplies a helper identity. */
export [[nodiscard]] std::size_t finalized_base_owner_matching_site_count(
    CustomScan* scan,
    Node* identity,
    const bioseq_stub_oids& oids);

/** @brief Count descendant helper owners with saturation at ambiguity. */
export [[nodiscard]] std::size_t finalized_descendant_match_owner_site_count(
    Plan* plan,
    Node* identity,
    const bioseq_stub_oids& oids);

/** @brief Count finalized owner classes in the direct child subtrees of a plan. */
export [[nodiscard]] finalized_match_owner_site_counts finalized_child_match_owner_site_counts(
    Plan* plan,
    Node* identity,
    const bioseq_stub_oids& oids);

/** @brief Add owner counts while saturating at the ambiguous value two. */
export [[nodiscard]] constexpr std::size_t
saturated_match_owner_site_count_add(std::size_t left, std::size_t right) noexcept {
    return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
}

/** @brief Check that a dummy helper has exactly one current or descendant owner. */
export [[nodiscard]] constexpr bool
dummy_match_helper_has_unique_finalized_owner(std::size_t descendant_matches,
                                               std::size_t current_parent_matches) noexcept {
    return saturated_match_owner_site_count_add(descendant_matches, current_parent_matches) == 1u;
}

static_assert(dummy_match_helper_has_unique_finalized_owner(0u, 1u));
static_assert(!dummy_match_helper_has_unique_finalized_owner(1u, 1u));
static_assert(!dummy_match_helper_has_unique_finalized_owner(0u, 0u));
static_assert(!dummy_match_helper_has_unique_finalized_owner(2u, 0u));

/** @brief Check that a dummy helper is supplied only by one ancestor owner. */
export [[nodiscard]] constexpr bool
dummy_match_helper_is_unique_ancestor_carrier(std::size_t local_base_matches,
                                               std::size_t ancestor_matches) noexcept {
    return local_base_matches == 0u && ancestor_matches == 1u;
}

static_assert(dummy_match_helper_is_unique_ancestor_carrier(0u, 1u));
static_assert(!dummy_match_helper_is_unique_ancestor_carrier(1u, 0u));
static_assert(!dummy_match_helper_is_unique_ancestor_carrier(0u, 2u));

/** @brief Resolve and validate the finalization record for an ancestor BatchJoin scan. */
export [[nodiscard]] match_owner_finalization* finalized_batch_owner_for_ancestor_stack(
    CustomScan* scan,
    std::span<match_owner_finalization> owners);

