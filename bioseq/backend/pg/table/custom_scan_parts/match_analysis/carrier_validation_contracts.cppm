/**
 * @file
 * @brief Declares checks for saved match bindings and their executor expressions.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_match_analysis_carrier_validation_contracts;

export import :custom_scan_match_analysis_match_owner_descriptor_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief Holds the base scan state  filled by initialize_base_match_owner_runtime. */
export struct bioseq_scan_runtime;

/** @brief Records the first helper comparison and any early, rejection for diagnostics. */
export struct finalized_match_helper_carrier_audit {
    bool inspected = false;
    int early_reject = 0;
    int version = -1;
    int private_fields = -1;
    int expected_fields = -1;
    int descriptor_tag = -1;
    int sites_tag = -1;
    int site_count = -1;
    int supplied_tag = -1;
    int carrier_tag = -1;
    int supplied_argument_tag = -1;
    int carrier_argument_tag = -1;
    int canonical_argument_tag = -1;
    int site_resno = -1;
    int site_phid = -1;
    int supplied_phid = 0;
    int carrier_phid = 0;
    bool identity_equal = false;
    bool supplied_argument_canonical = false;
    bool carrier_argument_canonical = false;
    bool equivalent = false;
};

/** @brief Checks and loads the base scan's saved match target and helper bindings. */
export [[nodiscard]] Node* initialize_base_match_owner_runtime(
    CustomScan* scan,
    Relation relation,
    bioseq_scan_runtime& runtime);

/**
 * @brief Counts matching helper bindings in a batch or per-row join scan.
 *
 * The result is 0 for no match, 1 for a unique binding, or 2 for ambiguity or
 * malformed site data.
 */
export [[nodiscard]] std::size_t finalized_batch_owner_matching_site_count(
    CustomScan* scan,
    Node* identity,
    const bioseq_stub_oids& oids,
    finalized_match_helper_carrier_audit* audit = nullptr);
