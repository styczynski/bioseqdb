/*-------------------------------------------------------------------------
 *
 * PostgreSQL lifecycle-hook ownership for bioseq_cache.
 *
 * The runtime supplies cache callbacks; this module owns hook chaining and
 * the saved predecessor pointers. Keeping those pointers here makes hook
 * lifetime independent from planner and scan implementation details.
 *
 *-------------------------------------------------------------------------
 */
module;

extern "C" {
#include "postgres.h"

#include "executor/executor.h"
#include "optimizer/planner.h"
#include "storage/ipc.h"
#include "storage/shmem.h"
#include "tcop/utility.h"

#include "miscadmin.h"
}

export module bioseq.cache.pg.executor:lifecycle;

namespace bioseq::cache::pg::executor::lifecycle::detail {

shmem_request_hook_type g_previous_shmem_request = nullptr;
shmem_startup_hook_type g_previous_shmem_startup = nullptr;
planner_hook_type g_previous_planner = nullptr;
create_upper_paths_hook_type g_previous_create_upper_paths = nullptr;
ExecutorStart_hook_type g_previous_executor_start = nullptr;
ExecutorFinish_hook_type g_previous_executor_finish = nullptr;
ExecutorEnd_hook_type g_previous_executor_end = nullptr;
ProcessUtility_hook_type g_previous_process_utility = nullptr;
bool g_backend_hooks_installed = false;
bool g_shared_memory_hooks_installed = false;

} // namespace bioseq::cache::pg::executor::lifecycle::detail

export namespace bioseq::cache::pg::executor::lifecycle {

struct backend_hook_callbacks {
    planner_hook_type planner = nullptr;
    create_upper_paths_hook_type create_upper_paths = nullptr;
    ExecutorStart_hook_type executor_start = nullptr;
    ExecutorFinish_hook_type executor_finish = nullptr;
    ExecutorEnd_hook_type executor_end = nullptr;
    ProcessUtility_hook_type process_utility = nullptr;
};

/** Install the permanently chained planner, executor, and utility hooks. */
void install_backend_hooks(const backend_hook_callbacks& callbacks) noexcept {
    if (detail::g_backend_hooks_installed) {
        return;
    }
    detail::g_previous_planner = planner_hook;
    planner_hook = callbacks.planner;
    detail::g_previous_create_upper_paths = create_upper_paths_hook;
    create_upper_paths_hook = callbacks.create_upper_paths;
    detail::g_previous_executor_start = ExecutorStart_hook;
    ExecutorStart_hook = callbacks.executor_start;
    detail::g_previous_executor_finish = ExecutorFinish_hook;
    ExecutorFinish_hook = callbacks.executor_finish;
    detail::g_previous_executor_end = ExecutorEnd_hook;
    ExecutorEnd_hook = callbacks.executor_end;
    detail::g_previous_process_utility = ProcessUtility_hook;
    ProcessUtility_hook = callbacks.process_utility;
    detail::g_backend_hooks_installed = true;
}

/** Install the shared-memory hooks during shared-preload processing. */
void install_shared_memory_hooks(shmem_request_hook_type request,
                                 shmem_startup_hook_type startup) noexcept {
    if (detail::g_shared_memory_hooks_installed) {
        return;
    }
    detail::g_previous_shmem_request = shmem_request_hook;
    shmem_request_hook = request;
    detail::g_previous_shmem_startup = shmem_startup_hook;
    shmem_startup_hook = startup;
    detail::g_shared_memory_hooks_installed = true;
}

[[nodiscard]] shmem_request_hook_type previous_shmem_request() noexcept {
    return detail::g_previous_shmem_request;
}

[[nodiscard]] shmem_startup_hook_type previous_shmem_startup() noexcept {
    return detail::g_previous_shmem_startup;
}

[[nodiscard]] planner_hook_type previous_planner() noexcept {
    return detail::g_previous_planner;
}

[[nodiscard]] create_upper_paths_hook_type previous_create_upper_paths() noexcept {
    return detail::g_previous_create_upper_paths;
}

[[nodiscard]] ExecutorStart_hook_type previous_executor_start() noexcept {
    return detail::g_previous_executor_start;
}

[[nodiscard]] ExecutorFinish_hook_type previous_executor_finish() noexcept {
    return detail::g_previous_executor_finish;
}

[[nodiscard]] ExecutorEnd_hook_type previous_executor_end() noexcept {
    return detail::g_previous_executor_end;
}

[[nodiscard]] ProcessUtility_hook_type previous_process_utility() noexcept {
    return detail::g_previous_process_utility;
}

} // namespace bioseq::cache::pg::executor::lifecycle
