/**
 * @file
 * @brief Collects executor-owned sequence source routes for BatchSort.
 */
module;
#include <bioseq/pg_compat.h>

#include <cstdint>

export module bioseq.postgres.table:custom_scan_batch_sort_source_routes;

export import :custom_scan_batch_sort_source_route_contracts;
import bioseq.common.checked_arithmetic;
import bioseq.postgres.storage.scanner;
import bioseq.postgres.support;
import bioseq.postgres.table.scan_state;

import :custom_scan_execution_method_contracts;
import :custom_scan_scan_runtime_state_contracts;

using namespace bioseq::postgres::support;

export {
    /** Walks @p plan for lazy BioseqScan providers and adds each source route to @p routes. */
    void batch_sort_collect_source_routes(PlanState* plan,
                                          bioseq::storage::pg_sequence_source_route_set& routes,
                                          std::uint32_t& provider_count) {
        if (plan == nullptr)
            return;
        if (IsA(plan, CustomScanState)) {
            auto* custom = reinterpret_cast<CustomScanState*>(plan);
            if (custom->methods == bioseq_column_scan_execute_methods()) {
                auto* scan = scan_state::from_custom_scan_state(custom);
                bioseq_scan_runtime& runtime = bioseq_runtime(scan);
                if (!runtime.lazy_source_projection)
                    return;
                if (runtime.lazy_source_provider == nullptr ||
                    !bioseq::storage::add_pg_sequence_source_route(routes,
                                                                   runtime.lazy_source_provider)) {
                    ereport(ERROR,
                            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                             errmsg("BioseqBatchSort source route is unavailable")));
                }
                provider_count = bioseq::common::saturating_add(provider_count, 1u);
                return;
            }
            for (PlanState* child : postgres_list<PlanState>(custom->custom_ps)) {
                batch_sort_collect_source_routes(child, routes, provider_count);
            }
            return;
        }
        if (IsA(plan, AppendState)) {
            auto* append = reinterpret_cast<AppendState*>(plan);
            for (int index = 0; index < append->as_nplans; ++index) {
                batch_sort_collect_source_routes(
                    append->appendplans[index], routes, provider_count);
            }
            return;
        }
        if (IsA(plan, MergeAppendState)) {
            auto* append = reinterpret_cast<MergeAppendState*>(plan);
            for (int index = 0; index < append->ms_nplans; ++index) {
                batch_sort_collect_source_routes(append->mergeplans[index], routes, provider_count);
            }
            return;
        }
        PlanState* outer = outerPlanState(plan);
        PlanState* inner = innerPlanState(plan);
        batch_sort_collect_source_routes(outer, routes, provider_count);
        if (inner != outer) {
            batch_sort_collect_source_routes(inner, routes, provider_count);
        }
    }

    /** Collects BioseqScan source routes from @p child into @p routes and reports whether any provider was found. */
    [[nodiscard]] bool
    batch_sort_attach_source_routes(PlanState* child,
                                    bioseq::storage::pg_sequence_source_route_set& routes) {
        std::uint32_t provider_count = 0u;
        batch_sort_collect_source_routes(child, routes, provider_count);
        return provider_count != 0u;
    }
}
