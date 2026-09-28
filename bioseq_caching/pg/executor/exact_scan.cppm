/**
 * @file
 * @brief Registers PostgreSQL callbacks for cache scans.
 */
module;

extern "C" {
#include "postgres.h"

#include "nodes/extensible.h"
}

export module bioseq.cache.pg.executor:exact_scan;

export namespace bioseq::cache::pg::executor::exact_scan {

/** Return the executor callbacks for reading or producing an exact cached result. */
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

/** Return the callback that creates executor state for an exact cache scan */
template<auto p_create_state>
[[nodiscard]] const CustomScanMethods& exact_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "BioseqCacheScan",
        .CreateCustomScanState = p_create_state,
    };
    return methods;
}

/** Return the executor callbacks for combining reusable cached results. */
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

/** Return the callback that creates executor state for a reusable coverage scan. */
template<auto p_create_state>
[[nodiscard]] const CustomScanMethods& coverage_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .CreateCustomScanState = p_create_state,
    };
    return methods;
}

/** Register both cache scan types with PostgreSQL. */
void register_methods(
        const CustomScanMethods& exact,
        const CustomScanMethods& coverage) {
    RegisterCustomScanMethods(&exact);
    RegisterCustomScanMethods(&coverage);
}

} // namespace bioseq::cache::pg::executor::exact_scan
