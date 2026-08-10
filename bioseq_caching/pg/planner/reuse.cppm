/*-------------------------------------------------------------------------
 *
 * Reusable-result planning bridge ownership for bioseq_cache.
 *
 * Provider callbacks remain stable C-compatible function pointers. This
 * module owns their installation as one atomic integration seam so the
 * PostgreSQL primary interface does not know planner implementation details.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"

#include "nodes/extensible.h"
}

export module bioseq.cache.pg.planner:reuse;

import bioseq.cache.pg.provider;

export namespace bioseq::cache::pg::planner::reuse {

struct BridgeCallbacksV1 {
    provider_registry::RelationWriteObserverV2 relation_write = nullptr;
    provider_registry::PlannerBridgeObserverV2 relation_paths = nullptr;
    provider_registry::FinalizedPlanBridgeObserverV2 finalized_plan = nullptr;
};

/**
 * Own the reusable CustomPath callback table with a direct plan callback.
 * The template preserves the hot path without a dispatch trampoline.
 */
template<auto PlanCustomPath>
[[nodiscard]] const CustomPathMethods& coverage_path_methods() noexcept {
    static const CustomPathMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .PlanCustomPath = PlanCustomPath,
    };
    return methods;
}

/** Install all provider/cache bridges from one validated callback bundle. */
[[nodiscard]] bool install_provider_bridges(
        const BridgeCallbacksV1& callbacks) noexcept {
    if (callbacks.relation_write == nullptr
        || callbacks.relation_paths == nullptr
        || callbacks.finalized_plan == nullptr) {
        return false;
    }
    provider_registry::install_relation_write_observer_v2(
        callbacks.relation_write);
    provider_registry::install_planner_bridge_observer_v2(
        callbacks.relation_paths);
    provider_registry::install_finalized_plan_bridge_observer_v2(
        callbacks.finalized_plan);
    return true;
}

} // namespace bioseq::cache::pg::planner::reuse
