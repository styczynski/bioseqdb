/**
 * @file
 * @brief Shares the alignment features found in scan expressions.
 */
export module bioseq.postgres.table:custom_scan_scan_feature_contracts;

/**
 * @brief Records the alignment results needed by scan expressions.
 *
 * The  expression walker checks both projections and filters. These flags enable
 * alignment scores, CIGAR, edit-distance, divergence and MAPQ work in saved parameters,
 * which also determine the stages shown by EXPLAIN.
 */
export struct scan_feature_flags {
    bool uses_alignment_score = false; ///< A score, dp_score or dp_max_score call needs DP.
    bool uses_cigar = false;      ///< A cigar or has_cigar call needs CIGAR output
    bool uses_edit = false;       ///< bioseq.edit(a) projection or exact predicate
    bool uses_divergence = false; ///< A divergence call needs divergence estimation.
    bool uses_mapq = false;       ///< A mapq call needs mapping quality.
    bool has_mmsearch = false;    ///< The expressions contain mmsearch.
    /* A match-result helper appears in the expressions. Ownership checks
     * must separately find the mmsearch that supplies its result. */
    bool any_match_projection = false;
};
