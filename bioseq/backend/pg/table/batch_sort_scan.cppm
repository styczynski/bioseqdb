/**
 * @file
 * @brief Public callback and initialization contract for the blocking sequence batch sort scan.
 */
module;

#include <bioseq/pg_compat.h>

export module bioseq.postgres.table.batch_sort_scan;

import bioseq.postgres.storage.scanner.source_provider_contract;

/** Lets SliceProject contribute paths through the shared upper-path hook. */
export using slice_project_upper_path_provider =
    void (*)(PlannerInfo*, UpperRelationKind, RelOptInfo*, RelOptInfo*, void*);

/** Supplies the child path carrying exact sequence-source provenance. */
export using batch_sort_source_path_provider = Path * (*)(Path*, Node*, bool*);

/** Acquires the sequence-source routes needed to materialize locator values. */
export using batch_sort_source_route_provider =
    bool (*)(PlanState*, bioseq::storage::pg_sequence_source_route_set&);

/** Register the SliceProject participant in the shared upper-path dispatcher. */
export void
bioseq_register_slice_project_upper_path_provider(slice_project_upper_path_provider provider);

/** Register source-path tagging and source-route acquisition callbacks. */
export void
bioseq_register_batch_sort_source_providers(batch_sort_source_path_provider path_provider,
                                            batch_sort_source_route_provider route_provider);

/** Install the ordered-path hook and register the stable custom-scan methods. */
export void initialize_batch_sort_scan();
