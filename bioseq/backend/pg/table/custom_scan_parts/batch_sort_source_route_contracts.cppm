/**
 * @file
 * @brief Declares BatchSort source-route collection without importing executor state.
 */
module;
#include <bioseq/pg_compat.h>

#include <cstdint>

export module bioseq.postgres.table:custom_scan_batch_sort_source_route_contracts;

import bioseq.postgres.storage.scanner.source_provider_contract;

/** Walk @p plan for lazy BioseqScan providers and append each source route. */
export void batch_sort_collect_source_routes(
    PlanState* plan,
    bioseq::storage::pg_sequence_source_route_set& routes,
    std::uint32_t& provider_count);

/** Attach BioseqScan source routes from @p child and report whether any provider was found. */
export [[nodiscard]] bool batch_sort_attach_source_routes(
    PlanState* child,
    bioseq::storage::pg_sequence_source_route_set& routes);
