/*-------------------------------------------------------------------------
 * Static maintenance worker for the generic PostgreSQL cache storage tier.
 *-------------------------------------------------------------------------
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/wait_event.h"
}

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>

export module bioseq.cache.pg.maintenance;

import bioseq.cache.pg.storage;

export namespace bioseq::cache::pg::maintenance {

inline constexpr char kJanitorWorkerName[] = "bioseq_cache janitor";
inline constexpr char kJanitorWorkerFunction[] =
    "bioseq_cache_storage_janitor_main";

/** Register exactly one postmaster-managed worker during shared preload. */
[[nodiscard]] bool register_static_janitor_worker() noexcept;

/** Return whether this process registered the static worker already. */
[[nodiscard]] bool static_janitor_registered() noexcept;

} // namespace bioseq::cache::pg::maintenance

namespace bioseq::cache::pg::maintenance {
namespace {

inline bool g_registered = false;
inline volatile sig_atomic_t g_terminate_requested = 0;
inline volatile sig_atomic_t g_reload_requested = 0;

void janitor_sigterm(SIGNAL_ARGS) {
    const int saved_errno = errno;
    g_terminate_requested = 1;
    SetLatch(MyLatch);
    errno = saved_errno;
}

void janitor_sighup(SIGNAL_ARGS) {
    const int saved_errno = errno;
    g_reload_requested = 1;
    SetLatch(MyLatch);
    errno = saved_errno;
}

} // namespace

bool register_static_janitor_worker() noexcept {
    if (g_registered) {
        return true;
    }
    if (!process_shared_preload_libraries_in_progress) {
        return false;
    }
    BackgroundWorker worker{};
    std::strncpy(
        worker.bgw_name, kJanitorWorkerName, sizeof(worker.bgw_name) - 1);
    std::strncpy(
        worker.bgw_type, kJanitorWorkerName, sizeof(worker.bgw_type) - 1);
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_ConsistentState;
    worker.bgw_restart_time = 5;
    std::strncpy(
        worker.bgw_library_name,
        "bioseq_cache",
        sizeof(worker.bgw_library_name) - 1);
    std::strncpy(
        worker.bgw_function_name,
        kJanitorWorkerFunction,
        sizeof(worker.bgw_function_name) - 1);
    worker.bgw_main_arg = Datum{0};
    worker.bgw_notify_pid = 0;
    RegisterBackgroundWorker(&worker);
    g_registered = true;
    return true;
}

bool static_janitor_registered() noexcept {
    return g_registered;
}

} // namespace bioseq::cache::pg::maintenance

extern "C" PGDLLEXPORT void bioseq_cache_storage_janitor_main(Datum) {
    using namespace bioseq::cache::pg::maintenance;
    using namespace bioseq::cache::pg::storage;

    pqsignal(SIGTERM, janitor_sigterm);
    pqsignal(SIGHUP, janitor_sighup);
    BackgroundWorkerUnblockSignals();
    publish_worker_lifecycle(WorkerLifecycle::Starting, MyProcPid);
    if (!attach_storage()) {
        publish_worker_lifecycle(WorkerLifecycle::Failed, 0);
        proc_exit(1);
    }
    publish_worker_lifecycle(WorkerLifecycle::Running, MyProcPid);

    while (!g_terminate_requested) {
        if (g_reload_requested) {
            g_reload_requested = 0;
            ProcessConfigFile(PGC_SIGHUP);
        }
        publish_worker_heartbeat();
        static_cast<void>(janitor_sweep());

        ResetLatch(MyLatch);
        const int events = WaitLatch(
            MyLatch,
            WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
            static_cast<long>(janitor_interval_ms()),
            PG_WAIT_EXTENSION);
        if ((events & WL_LATCH_SET) != 0) {
            ResetLatch(MyLatch);
        }
    }

    publish_worker_lifecycle(WorkerLifecycle::Stopping, MyProcPid);
    static_cast<void>(janitor_sweep());
    publish_worker_lifecycle(WorkerLifecycle::Stopped, 0);
    proc_exit(0);
}
