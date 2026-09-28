/**
 * @file
 * @brief Declares the callback that prepares a BioseqScan for execution.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_runtime_scan_admission_contracts;

/** @brief Checks the BioseqScan plan and prepares its runtime, sequence sources, and  search inputs. */
export void bioseq_column_scan_begin_custom_scan_impl(CustomScanState* cscanstate,
                                                       EState* estate,
                                                       int eflags);
