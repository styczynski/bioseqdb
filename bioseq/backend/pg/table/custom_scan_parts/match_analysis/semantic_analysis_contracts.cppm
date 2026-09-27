/**
 * @file
 * @brief Declares checks for the scans that supply each match-helper result.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_match_analysis_semantic_analysis_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
import :custom_scan_match_finalization_contracts;

/** @brief Keeps join-owner and base-scan counts separate for diagnostics. */
export struct finalized_match_owner_site_counts {
    std::size_t batch = 0u;
    std::size_t base = 0u;
};

/** @brief Checks that base-scan helpers and saved bindings account for each other. */
export [[nodiscard]] bool base_scan_targetlist_has_only_finalized_match_helpers(
    CustomScan* scan,
    const bioseq_stub_oids& oids);

/**
 * @brief Checks whether this base scan supplies a helper result.
 *
 * Return 1 for an owned helper, 0 for no match, or 2 for malformed site data.
 */
export [[nodiscard]] std::size_t finalized_base_owner_matching_site_count(
    CustomScan* scan,
    Node* identity,
    const bioseq_stub_oids& oids);

/**
 * @brief Counts matching helper owners in a plan subtree, stopping at subqueries.
 *
 * Counts stop at 2 because callers only need to test uniqueness.
 */
export [[nodiscard]] std::size_t finalized_descendant_match_owner_site_count(
    Plan* plan,
    Node* identity,
    const bioseq_stub_oids& oids);

/** @brief Counts join and base-scan helper owners in the child subtrees for diagnostics. */
export [[nodiscard]] finalized_match_owner_site_counts finalized_child_match_owner_site_counts(
    Plan* plan,
    Node* identity,
    const bioseq_stub_oids& oids);

/** @brief Adds owner counts, returning 2 once ownership is ambiguous. */
export [[nodiscard]] constexpr std::size_t
saturated_match_owner_site_count_add(std::size_t left, std::size_t right) noexcept {
    return left > 1u || right > 1u || left + right > 1u ? std::size_t{2u} : left + right;
}

/** @brief Checks that exactly one parent or descendant scan supplies the dummy helper result. */
export [[nodiscard]] constexpr bool
dummy_match_helper_has_unique_finalized_owner(std::size_t descendant_matches,
                                               std::size_t current_parent_matches) noexcept {
    return saturated_match_owner_site_count_add(descendant_matches, current_parent_matches) == 1u;
}

static_assert(dummy_match_helper_has_unique_finalized_owner(0u, 1u));
static_assert(!dummy_match_helper_has_unique_finalized_owner(1u, 1u));
static_assert(!dummy_match_helper_has_unique_finalized_owner(0u, 0u));
static_assert(!dummy_match_helper_has_unique_finalized_owner(2u, 0u));

/** @brief Checks that one ancestor supplies the helper and the base scan does not. */
export [[nodiscard]] constexpr bool
dummy_match_helper_is_unique_ancestor_carrier(std::size_t local_base_matches,
                                               std::size_t ancestor_matches) noexcept {
    return local_base_matches == 0u && ancestor_matches == 1u;
}

static_assert(dummy_match_helper_is_unique_ancestor_carrier(0u, 1u));
static_assert(!dummy_match_helper_is_unique_ancestor_carrier(1u, 0u));
static_assert(!dummy_match_helper_is_unique_ancestor_carrier(0u, 2u));

/** @brief Finds and validates the saved match-owner record for an ancestor join. */
export [[nodiscard]] match_owner_finalization* finalized_batch_owner_for_ancestor_stack(
    CustomScan* scan,
    std::span<match_owner_finalization> owners);

