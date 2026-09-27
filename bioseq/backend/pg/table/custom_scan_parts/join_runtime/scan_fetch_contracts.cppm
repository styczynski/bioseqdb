/**
 * @file
 * @brief Fetches scan rows, including replacement rows used by PostgreSQL for rechecks.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_runtime_scan_fetch_contracts;

import bioseq.common.logging;

/**
 * @brief Returns the next scan row or the row supplied for an EvalPlanQual recheck.
 *
 * During a recheck, PostgreSQL may supply a replacement row after a concurrent
 * update. Test that row with recheck_mtd and return it at most once for its
 * relation. Otherwise, fetch the next row through access_mtd.
 */
export inline TupleTableSlot* exec_scan_fetch(ScanState* node,
                                              ExecScanAccessMtd access_mtd,
                                              ExecScanRecheckMtd recheck_mtd) {
    bioseq_log<logging_level::debug>("[bioseq] pg: ExecScanFetch() entered");
    EState* estate = node->ps.state;

    CHECK_FOR_INTERRUPTS();

    if (estate->es_epq_active != nullptr) {
        EPQState* epqstate = estate->es_epq_active;
        Index scanrelid = reinterpret_cast<Scan*>(node->ps.plan)->scanrelid;

        if (scanrelid == 0) {
            TupleTableSlot* slot = node->ss_ScanTupleSlot;
            if (!(*recheck_mtd)(node, slot))
                ExecClearTuple(slot);
            return slot;
        } else if (epqstate->relsubs_done[scanrelid - 1]) {
            TupleTableSlot* slot = node->ss_ScanTupleSlot;
            return ExecClearTuple(slot);
        } else if (epqstate->relsubs_slot[scanrelid - 1] != nullptr) {
            TupleTableSlot* slot = epqstate->relsubs_slot[scanrelid - 1];

            Assert(epqstate->relsubs_rowmark[scanrelid - 1] == nullptr);
            epqstate->relsubs_done[scanrelid - 1] = true;

            if (TupIsNull(slot))
                return nullptr;
            if (!(*recheck_mtd)(node, slot))
                return ExecClearTuple(slot);
            return slot;
        } else if (epqstate->relsubs_rowmark[scanrelid - 1] != nullptr) {
            TupleTableSlot* slot = node->ss_ScanTupleSlot;
            epqstate->relsubs_done[scanrelid - 1] = true;

            if (!EvalPlanQualFetchRowMark(epqstate, scanrelid, slot))
                return nullptr;
            if (TupIsNull(slot))
                return nullptr;
            if (!(*recheck_mtd)(node, slot))
                return ExecClearTuple(slot);
            return slot;
        }
    }

    return (*access_mtd)(node);
}
