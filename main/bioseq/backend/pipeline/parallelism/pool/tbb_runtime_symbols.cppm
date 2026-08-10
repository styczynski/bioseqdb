module;

// GCC 16.1 can emit external references for oneTBB partitioner.h helpers when
// parallel_sort is instantiated through C++ modules. This implementation unit
// is linked only by the AWS GCC16 build and keeps the helper body identical to
// oneTBB.

#include <oneapi/tbb/detail/_task.h>

module bioseq.parallelism.pool;

namespace {

bool bioseq_tbb_is_stolen_task_impl(const tbb::detail::d1::execution_data& ed) {
    return tbb::detail::d1::execution_slot(ed) != tbb::detail::d1::original_slot(ed);
}

}  // namespace

namespace tbb::detail::d1 {

bool is_stolen_task(const execution_data& ed) {
    return bioseq_tbb_is_stolen_task_impl(ed);
}

}  // namespace tbb::detail::d1

extern "C" bool bioseq_tbb_d1_local_is_stolen_task(
    const tbb::detail::d1::execution_data& ed)
    asm("_ZN3tbb6detail2d1L14is_stolen_taskERKNS1_14execution_dataE");

extern "C" bool bioseq_tbb_d1_local_is_stolen_task(
    const tbb::detail::d1::execution_data& ed) {
    return bioseq_tbb_is_stolen_task_impl(ed);
}
