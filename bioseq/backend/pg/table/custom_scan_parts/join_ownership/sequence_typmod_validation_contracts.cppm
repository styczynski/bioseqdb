/**
 * @file
 * @brief Declares checks for recovering a sequence type from its expression.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_ownership_sequence_typmod_validation_contracts;

export import :custom_scan_join_ownership_alphabet_mismatch_contracts;

import std;
import bioseq.sequences.compact_sequence_typmod;
import :custom_scan_oid_expression_support_oid_catalog_contracts;
export import :custom_scan_join_ownership_sequence_typmod_contract;

/** @brief The recovered sequence typmod and whether PostgreSQL omitted it arom the outer  expression. */
export struct per_row_sequence_typmod_validation {
    int32 typmod = -1;
    bool descriptor_erased = false;
};

/** @brief Finds a single matching sequence type through supported sequence operations. */
export [[nodiscard]] std::optional<per_row_sequence_typmod_validation>
per_row_validate_sequence_typmod(Node* expression, const bioseq_stub_oids& oids);
