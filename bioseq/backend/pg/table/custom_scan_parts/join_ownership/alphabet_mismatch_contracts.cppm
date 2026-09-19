/**
 * @file
 * @brief Declares the exact mmsearch alphabet-mismatch diagnostic boundary.
 */
export module bioseq.postgres.table:custom_scan_join_ownership_alphabet_mismatch_contracts;

import bioseq.sequences.compact_sequence_typmod;

/** @brief Raise a PostgreSQL datatype error for operands with different alphabets. */
export [[noreturn]] void raise_mmsearch_alphabet_mismatch(
    compact_sequence_stdc_type_params target,
    compact_sequence_stdc_type_params query,
    const char* stage);
