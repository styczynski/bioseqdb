/**
 * @file
 * @brief Declares storage-pruning qualifier filtering without planner walk contracts.
 */
module;
extern "C" {
#include "postgres.h"
#include "nodes/pg_list.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

export module bioseq.postgres.table:custom_scan_oid_expression_support_storage_pruning_contracts;

import :custom_scan_oid_expression_support_oid_catalog_core_contracts;

/** @brief Remove all mmsearch expressions from storage-pruning qualifiers. */
export [[nodiscard]] List* storage_prunable_quals_without_mmsearch(
    List* quals,
    const bioseq_stub_oids& oids);
