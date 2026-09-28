/**
 * @file
 * @brief Keep child sequence readers alive for batch sorting.
 */
module;
#include <bioseq/pg_compat.h>

#include <cstdint>

export module bioseq.postgres.table:custom_scan_batch_sort_source_route_contracts;

import bioseq.postgres.storage.scanner.source_provider_contract;

/** Find lazy Bioseq scans below @p plan and add their sequence-reader references to @p routes. */
export void batch_sort_collect_source_routes(
    PlanState* plan,
    bioseq::storage::pg_sequence_source_route_set& routes,
    std::uint32_t& provider_count);

/** Keep @p child sequence readers alive and return whether any provider was found. */
export [[nodiscard]] bool batch_sort_attach_source_routes(
    PlanState* child,
    bioseq::storage::pg_sequence_source_route_set& routes);
