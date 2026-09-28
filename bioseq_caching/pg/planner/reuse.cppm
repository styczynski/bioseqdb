/**
 * @file
 * @brief Connects provider planning callbacks and relation-write notifications to the cache.
 */
module;

extern "C" {
#include "postgres.h"

#include "nodes/extensible.h"
}

export module bioseq.cache.pg.planner:reuse;

import bioseq.cache.pg.provider;

export namespace bioseq::cache::pg::planner::reuse {

struct bridge_callbacks {
    provider_registry::relation_write_observer relation_write = nullptr;
    provider_registry::planner_bridge_observer relation_paths = nullptr;
    provider_registry::finalized_plan_bridge_observer finalized_plan = nullptr;
};

/** Return the callback table that turns a reusable coverage path into a plan. */
template <auto p_plan_custom_path>
[[nodiscard]] const CustomPathMethods& coverage_path_methods() noexcept {
    static const CustomPathMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .PlanCustomPath = p_plan_custom_path,
    };
    return methods;
}

/** Install the three provider callbacks after checking that each is present. */
[[nodiscard]] bool install_provider_bridges(const bridge_callbacks& callbacks) noexcept {
    if (callbacks.relation_write == nullptr || callbacks.relation_paths == nullptr ||
        callbacks.finalized_plan == nullptr) {
        return false;
    }
    provider_registry::install_relation_write_observer(callbacks.relation_write);
    provider_registry::install_planner_bridge_observer(callbacks.relation_paths);
    provider_registry::install_finalized_plan_bridge_observer(callbacks.finalized_plan);
    return true;
}

} // namespace bioseq::cache::pg::planner::reuse
