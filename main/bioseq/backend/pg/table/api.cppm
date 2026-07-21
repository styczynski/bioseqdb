module;

#include "common.h"

export module bioseq.table:api;

/*
 * Public table-module surface used outside bioseq.table.
 *
 * C++ module partitions are only importable by units of the same named module,
 * so external modules import bioseq.table and see this small API partition
 * re-exported by the primary interface. The heavy implementation partitions stay
 * out of the primary module's re-export graph for GCC 16.1.
 */
export const TableAmRoutine* GetBioseqColumnTableAmRoutine(void);
export void bioseq_tableam_init(void);

export const CustomScanMethods* bioseq_customscan_methods(void);
export bool bioseq_expr_uses_bioseq_projection(Node* node);
export void bioseq_customscan_init(void);
export void bioseq_validate_raw_mmsearch_options(Query* query);
export void bioseq_finalize_match_helper_bindings(PlannedStmt* statement);

export CustomScan* bioseq_create_indexscan_node(void);
export void bioseq_register_indexscan_node(void);
