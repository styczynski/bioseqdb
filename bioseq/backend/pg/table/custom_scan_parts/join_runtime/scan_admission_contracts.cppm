/**
 * @file
 * @brief Declares base-scan admission without importing its implementation.
 */
module;
#include <bioseq/pg_compat.h>

export module bioseq.postgres.table:custom_scan_join_runtime_scan_admission_contracts;

/** @brief Validate a base-scan plan and construct its query-owned execution runtime. */
export void bioseq_column_scan_begin_custom_scan_impl(CustomScanState* cscanstate,
                                                       EState* estate,
                                                       int eflags);
