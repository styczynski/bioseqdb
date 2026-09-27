/**
 * @file
 * @brief Defines the lightweight feature set shared by planner and executor scans.
 */
export module bioseq.postgres.table:custom_scan_scan_feature_contracts;

/**
 * @brief Records which alignment results a scan must compute.
 *
 * Walked from the plan target list once
 * per BeginCustomScan and pushed into minimap2_parameters so the minimap2
 * pipeline can skip expensive stages (divergence estimation, MAPQ, CIGAR
 * base alignment) whose outputs are never projected to the caller.
 *
 * Keeping this in one place also makes EXPLAIN consistent: the same flags
 * drive both what-to-run at execution time and which pipeline-step sections
 * are surfaced in BioseqColumnScan_ExplainCustomScan output.
 */
export struct scan_feature_flags {
    bool uses_cigar = false;      ///< bioseq.cigar(a) in target list
    bool uses_edit = false;       ///< bioseq.edit(a) projection or exact predicate
    bool uses_divergence = false; ///< bioseq.divergence(a) in target list
    bool uses_mapq = false;       ///< bioseq.mapq(a) in target list
    bool has_mmsearch = false;    ///< WHERE mmsearch(seq, 'query'::bioseq.seq) present
    /*
     * True when any placeholder projection that is resolved from a
     * match_result appears in the target list. Distinct from has_mmsearch
     * because a projection like bioseq.cigar(a) without a matching
     * mmsearch() qual is theoretically possible but yields NULLs today.
     */
    bool any_match_projection = false;
};
