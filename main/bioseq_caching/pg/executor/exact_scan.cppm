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
    auto Begin,
    auto Execute,
    auto End,
    auto Rescan,
    auto Explain>
[[nodiscard]] const CustomExecMethods& exact_executor_methods() noexcept {
    static const CustomExecMethods methods{
        .CustomName = "BioseqCacheScan",
        .BeginCustomScan = Begin,
        .ExecCustomScan = Execute,
        .EndCustomScan = End,
        .ReScanCustomScan = Rescan,
        .ExplainCustomScan = Explain,
    };
    return methods;
}

/** Own the exact planner-to-executor state factory table. */
template<auto CreateState>
[[nodiscard]] const CustomScanMethods& exact_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "BioseqCacheScan",
        .CreateCustomScanState = CreateState,
    };
    return methods;
}

/** Own the companion reusable-coverage executor callback table. */
template<
    auto Begin,
    auto Execute,
    auto End,
    auto Rescan,
    auto Explain>
[[nodiscard]] const CustomExecMethods& coverage_executor_methods() noexcept {
    static const CustomExecMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .BeginCustomScan = Begin,
        .ExecCustomScan = Execute,
        .EndCustomScan = End,
        .ReScanCustomScan = Rescan,
        .ExplainCustomScan = Explain,
    };
    return methods;
}

/** Own the reusable-coverage planner-to-executor state factory table. */
template<auto CreateState>
[[nodiscard]] const CustomScanMethods& coverage_scan_methods() noexcept {
    static const CustomScanMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .CreateCustomScanState = CreateState,
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
