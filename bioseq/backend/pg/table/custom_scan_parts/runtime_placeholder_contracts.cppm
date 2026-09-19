/**
 * @file
 * @brief Defines mutable projection-placeholder ownership without importing scan runtimes.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_runtime_placeholder_contracts;

import std;

export {
    /**
     * @brief Binds one rewritten symbol-count call to its mutable result placeholder.
     *
     * The Const*
     * replaces the FuncExpr in runtime.newtargetlist; `rank` is resolved once at
     * plan time via seqan3 so the per-row populate path is a single array
     * index into the meta_sidecar counts[]. `letter` + `column_name` are
     * retained for the EXPLAIN-time "Fetched from meta:" string.
     */
    struct symbol_count_call_site {
        Const* placeholder = nullptr;
        int rank = -1;
        char letter = '\0';
        int16_t column_attno = 0; // SEQ column whose symbols are counted
        std::string column_name;  // attname for EXPLAIN output
    };

    /**
     * @brief Owns mutable Const nodes populated immediately before PostgreSQL projection.
     *
     * The nodes are injected by VarInjectorMutator into the
     * rewritten target list. Each one stands in for a bioseq.<field>(...)
     * FuncExpr; per-row values are written into constvalue/constisnull
     * before ExecProject runs. All pointers start as nullptr in a fresh
     * bioseq_scan_runtime; CustomExecScan's first-call init allocates them
     * and VarInjectorMutator wires them into runtime.newtargetlist.
     *
     * symbol_count_sites is populated dynamically by VarInjectorMutator as
     * it discovers `symbol_count(Var, 'L')` call sites in the target list.
     * Multiple call sites per scan are supported (different letters or
     * different columns resolve to independent placeholders), and the
     * vector is cleared at the top of every scan init so stale entries
     * from a prior query cannot bleed across.
     */
    struct placeholder_constants {
        Const* dp_score_node = nullptr; // bioseq.score / bioseq.dp_score
        Const* query_len_node = nullptr;
        Const* query_start_node = nullptr;
        Const* query_end_node = nullptr;
        Const* target_len_node = nullptr;
        Const* target_start_node = nullptr;
        Const* target_end_node = nullptr;
        Const* num_matches_node = nullptr;
        Const* block_length_node = nullptr;
        Const* edit_distance_node = nullptr;
        Const* mapq_node = nullptr;
        Const* chain_score_node = nullptr;
        Const* dp_max_score_node = nullptr;
        Const* seed_count_node = nullptr;
        Const* secondary_chain_score_node = nullptr;
        Const* is_reverse_node = nullptr;
        Const* is_primary_node = nullptr;
        Const* has_cigar_node = nullptr;
        Const* divergence_node = nullptr;
        Const* cigar_node = nullptr;
        std::vector<symbol_count_call_site> symbol_count_sites;
    };
}
