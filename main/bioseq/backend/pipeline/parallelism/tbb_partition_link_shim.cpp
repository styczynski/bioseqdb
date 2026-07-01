// GCC 16 can emit an external reference for oneTBB's partitioner.h
// internal helper when TBB parallel_sort is instantiated through a C++ module.
// The body matches partitioner.h exactly: stolen means the current execution
// slot differs from the task's original slot.

#include <oneapi/tbb/detail/_task.h>

namespace tbb::detail::d1 {

bool is_stolen_task(const execution_data& ed) {
    return execution_slot(ed) != original_slot(ed);
}

}  // namespace tbb::detail::d1
