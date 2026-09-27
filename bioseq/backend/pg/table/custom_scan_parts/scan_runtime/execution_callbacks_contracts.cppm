/**
 * @file
 * @brief Declares base-scan fetch, execution and cleanup callbacks.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_scan_runtime_execution_callbacks_contracts;

import bioseq.postgres.table.scan_state;

/** @brief Prepares result placeholders on first use and produces the next scan result. */
export TupleTableSlot* custom_exec_scan(scan_state* bioseq_scan_state,
                                        ExecScanAccessMtd access_mtd,
                                        ExecScanRecheckMtd recheck_mtd);

/** @brief Open the table-AM scan lazily and fetch its next scalar or vector slot. */
export TupleTableSlot* bioseq_column_scan_next(scan_state* bioseq_scan_state);

/** @brief Leaves EvalPlanQual replacement rows to the scan's normal filter evaluation. */
export bool bioseq_column_scan_recheck(scan_state* node, TupleTableSlot* slot);

/** @brief Produces the next scan result and reports C++ failures as PostgreSQL errors. */
export TupleTableSlot* bioseq_column_scan_exec_custom_scan(CustomScanState* node);

/** @brief Releases scan resources while PostgreSQL still owns their tuple references. */
export void bioseq_column_scan_end_custom_scan(CustomScanState* node);

/** @brief Reset base-scan state and parameter bindings for another executor pass. */
export void bioseq_column_scan_rescan_custom_scan(CustomScanState* node);
