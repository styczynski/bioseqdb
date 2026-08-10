/*-------------------------------------------------------------------------
 *
 * PostgreSQL CustomScan registration ownership for bioseq_cache.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"

#include "nodes/extensible.h"
}

export module bioseq.cache.pg.executor:exact_scan;

export namespace bioseq::cache::pg::executor::exact_scan {

/**
 * Own the exact executor callback table while retaining direct callback
 * pointers (no per-tuple dispatch layer).
 */
template<
    auto p_begin,
    auto p_execute,
    auto p_end,
    auto p_rescan,
    auto p_explain>
[[nodiscard]] const CustomExecMethods& exact_executor_methods() noexcept {
    static const CustomExecMethods methods{
        .CustomName = "BioseqCacheScan",
        .BeginCustomScan = p_begin,
        .ExecCustomScan = p_execute,
        .EndCustomScan = p_end,
        .ReScanCustomScan = p_rescan,
        .ExplainCustomScan = p_explain,
    };
    return methods;
}

/** Own the exact planner-to-executor state factory table. */
template<auto p_create_state>
[[nodiscard]] const CustomScanMethods& exact_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "BioseqCacheScan",
        .CreateCustomScanState = p_create_state,
    };
    return methods;
}

/** Own the companion reusable-coverage executor callback table. */
template<
    auto p_begin,
    auto p_execute,
    auto p_end,
    auto p_rescan,
    auto p_explain>
[[nodiscard]] const CustomExecMethods& coverage_executor_methods() noexcept {
    static const CustomExecMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .BeginCustomScan = p_begin,
        .ExecCustomScan = p_execute,
        .EndCustomScan = p_end,
        .ReScanCustomScan = p_rescan,
        .ExplainCustomScan = p_explain,
    };
    return methods;
}

/** Own the reusable-coverage planner-to-executor state factory table. */
template<auto p_create_state>
[[nodiscard]] const CustomScanMethods& coverage_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .CreateCustomScanState = p_create_state,
    };
    return methods;
}

/** Register the exact and reusable-coverage scan ABI method tables. */
void register_methods(
        const CustomScanMethods& exact,
        const CustomScanMethods& coverage) {
    RegisterCustomScanMethods(&exact);
    RegisterCustomScanMethods(&coverage);
}

} // namespace bioseq::cache::pg::executor::exact_scan
