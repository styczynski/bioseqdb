/**
 * @file
 * @brief Declares recursive sequence-typmod validation without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_sequence_typmod_validation_contracts;

export import :custom_scan_join_ownership_alphabet_mismatch_contracts;

import std;
import bioseq.sequences.compact_sequence_typmod;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
export import :custom_scan_join_ownership_sequence_typmod_contract;

/** @brief Carries a validated sequence typmod and whether its outer descriptor was erased. */
export struct per_row_sequence_typmod_validation {
    int32 typmod = -1;
    bool descriptor_erased = false;
};

/** @brief Derive one exact typmod through supported sequence-preserving expressions. */
export [[nodiscard]] std::optional<per_row_sequence_typmod_validation>
per_row_validate_sequence_typmod(Node* expression, const bioseq_stub_oids& oids);
