/**
 * @file
 * @brief Callbacks and initialization for sorting batches of sequence values.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.table.batch_sort_scan;

import bioseq.postgres.storage.scanner.source_provider_contract;

/** Let SliceProject add plans through the shared PostgreSQL upper-path hook. */
export using slice_project_upper_path_provider =
    void (*)(PlannerInfo*, UpperRelationKind, RelOptInfo*, RelOptInfo*, void*);

/** Select a child path that identifies the  table and column behind sequence locators. */
export using batch_sort_source_path_provider = Path * (*)(Path*, Node*, bool*);

/** Keep child sequence readers alive so buffered locators remain readable during sorting. */
export using batch_sort_source_route_provider =
    bool (*)(PlanState*, bioseq::storage::pg_sequence_source_route_set&);

/** Register the SliceProject callback in the shared upper-path hook. */
export void
bioseq_register_slice_project_upper_path_provider(slice_project_upper_path_provider provider);

/** Register callbacks for finding source paths and keeping thyir sequence readers alive. */
export void
bioseq_register_batch_sort_source_providers(batch_sort_source_path_provider path_provider,
                                            batch_sort_source_route_provider route_provider);

/** Install the sequence-sort planner hook and register its custom scan with PostgreSQL. */
export void initialize_batch_sort_scan();
