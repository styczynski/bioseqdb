/**
 * @file tbb_runtime_symbols.cppm
 * @brief GCC module-linkage definitions for oneTBB stolen-task helpers.
 */
module;

// GCC 16.1 can emit external references for oneTBB partitioner.h helpers when
// parallel_sort is instantiated through C++ modules. This implementation unit
// is linked only by the AWS GCC16 build and keeps the helper body identical to
// oneTBB.

#include <oneapi/tbb/detail/_task.h>

module bioseq.parallelism.pool;

namespace {

/** Evaluate oneTBB's stolen-task predicate from execution-slot identity. */
bool bioseq_tbb_is_stolen_task_impl(const tbb::detail::d1::execution_data& execution_data) {
    return tbb::detail::d1::execution_slot(execution_data) !=
           tbb::detail::d1::original_slot(execution_data);
}

}  // namespace

namespace tbb::detail::d1 {

/** Provide the external oneTBB helper referenced by GCC module output. */
bool is_stolen_task(const execution_data& execution_data) {
    return bioseq_tbb_is_stolen_task_impl(execution_data);
}

}  // namespace tbb::detail::d1

extern "C" bool
bioseq_tbb_d1_local_is_stolen_task(const tbb::detail::d1::execution_data& execution_data) asm(
    "_ZN3tbb6detail2d1L14is_stolen_taskERKNS1_14execution_dataE");

/** Define the local-linkage spelling emitted by GCC for the same predicate. */
extern "C" bool
bioseq_tbb_d1_local_is_stolen_task(const tbb::detail::d1::execution_data& execution_data) {
    return bioseq_tbb_is_stolen_task_impl(execution_data);
}
