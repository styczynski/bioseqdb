/**
 * @file tbb_runtime_symbols.cppm
 * @brief Supply oneTBB helper symbols required by GCC module builds.
 */
module;

// GCC 16.1 module builds can leave unresolved references to these oneTBB
// partitioner helpers when instantiating parallel_sort. The AWS GCC16 build
// links these definitions, which use the same slt  comparison as oneTBB.

#include <oneapi/tbb/detail/_task.h>

module bioseq.parallelism.pool;

namespace {

/** Check whether a task runs in a different slot from the one that created it. */
bool bioseq_tbb_is_stolen_task_impl(const tbb::detail::d1::execution_data& execution_data) {
    return tbb::detail::d1::execution_slot(execution_data) !=
           tbb::detail::d1::original_slot(execution_data);
}

}  // namespace

namespace tbb::detail::d1 {

/** Supply the oneTBB helper symbol referenced by GCC module builds. */
bool is_stolen_task(const execution_data& execution_data) {
    return bioseq_tbb_is_stolen_task_impl(execution_data);
}

}  // namespace tbb::detail::d1

extern "C" bool
bioseq_tbb_d1_local_is_stolen_task(const tbb::detail::d1::execution_data& execution_data) asm(
    "_ZN3tbb6detail2d1L14is_stolen_taskERKNS1_14execution_dataE");

/** Supply the same helper under the local-linkage symbol emitted by GCC. */
extern "C" bool
bioseq_tbb_d1_local_is_stolen_task(const tbb::detail::d1::execution_data& execution_data) {
    return bioseq_tbb_is_stolen_task_impl(execution_data);
}
