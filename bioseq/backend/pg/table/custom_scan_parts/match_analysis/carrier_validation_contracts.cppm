/**
 * @file
 * @brief Declares finalized carrier validation without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_match_analysis_carrier_validation_contracts;

export import :custom_scan_match_analysis_match_owner_descriptor_contracts;

import std;
import :custom_scan_oid_expression_support_oid_catalog_contracts;

/** @brief Mutable base-scan runtime state defined by the scan-runtime contract. */
export struct bioseq_scan_runtime;

/** @brief Captures the first finalized-carrier comparison for planner diagnostics. */
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

/** @brief Validate the base-owner executor image and install its runtime bindings. */
export [[nodiscard]] Node* initialize_base_match_owner_runtime(
    CustomScan* scan,
    Relation relation,
    bioseq_scan_runtime& runtime);

/** @brief Count finalized sites in one BatchJoin that own a helper carrier identity. */
export [[nodiscard]] std::size_t finalized_batch_owner_matching_site_count(
    CustomScan* scan,
    Node* identity,
    const bioseq_stub_oids& oids,
    finalized_match_helper_carrier_audit* audit = nullptr);
