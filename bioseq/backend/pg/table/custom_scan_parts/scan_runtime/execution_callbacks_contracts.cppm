/**
 * @file
 * @brief Declares base-scan executor callbacks without importing their implementations.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_scan_runtime_execution_callbacks_contracts;

import bioseq.postgres.table.scan_state;

/** @brief Install executor placeholders and advance the base-scan state machine. */
export TupleTableSlot* custom_exec_scan(scan_state* bioseq_scan_state,
                                        ExecScanAccessMtd access_mtd,
                                        ExecScanRecheckMtd recheck_mtd);

/** @brief Open the table-AM scan lazily and fetch its next scalar or vector slot. */
export TupleTableSlot* bioseq_column_scan_next(scan_state* bioseq_scan_state);

/** @brief Accept an EvalPlanQual tuple already rechecked by the table access method. */
export bool bioseq_column_scan_recheck(scan_state* node, TupleTableSlot* slot);

/** @brief Execute one base-scan step across the PostgreSQL/C++ error boundary. */
export TupleTableSlot* bioseq_column_scan_exec_custom_scan(CustomScanState* node);

/** @brief Release base-scan resources in executor ownership order. */
export void bioseq_column_scan_end_custom_scan(CustomScanState* node);

/** @brief Reset base-scan state and parameter bindings for another executor pass. */
export void bioseq_column_scan_rescan_custom_scan(CustomScanState* node);
