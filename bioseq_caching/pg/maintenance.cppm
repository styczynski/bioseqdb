/**
 * @file
 * @brief PostgreSQL background worker that cleans cache entries and storage.
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "libpq/pqsignal.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/procsignal.h"
#include "utils/guc.h"
#include "utils/wait_event.h"

#include "miscadmin.h"
#include "postmaster/bgworker.h"
}

#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>

export module bioseq.cache.pg.maintenance;

import bioseq.cache.pg.shared_registry;
import bioseq.cache.pg.storage;

export namespace bioseq::cache::pg::maintenance {

inline constexpr char janitor_worker_name[] = "bioseq_cache janitor";
inline constexpr char janitor_worker_function[] = "bioseq_cache_storage_janitor_main";

/** Registry cleanup callback run before each storage sweep. */
using reusable_janitor_callback = void (*)() noexcept;

/** Register the janitor with PostgreSQL during shared preload. Repeated calls are harmless. */
[[nodiscard]] bool register_static_janitor_worker() noexcept;

/** Report whether this process has registered the janitor. */
[[nodiscard]] bool static_janitor_registered() noexcept;

/** Set the registry cleanup callback if none is set, or accept the same callback again. */
[[nodiscard]] bool register_reusable_janitor_callback(reusable_janitor_callback callback) noexcept;

/** Clear the registry cleanup callback only if it matches the supplied function. */
void unregister_reusable_janitor_callback(reusable_janitor_callback callback) noexcept;

} // namespace bioseq::cache::pg::maintenance

namespace bioseq::cache::pg::maintenance {
namespace {

inline bool g_registered = false;
inline volatile sig_atomic_t g_terminate_requested = 0;
inline volatile sig_atomic_t g_reload_requested = 0;
inline reusable_janitor_callback g_reusable_janitor_callback = nullptr;

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

/** Run the registered registry cleanup callback, if present. */
void run_reusable_janitor_callback() noexcept {
    const reusable_janitor_callback callback = g_reusable_janitor_callback;
    if (callback != nullptr) {
        callback();
    }
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
    std::strncpy(worker.bgw_name, janitor_worker_name, sizeof(worker.bgw_name) - 1);
    std::strncpy(worker.bgw_type, janitor_worker_name, sizeof(worker.bgw_type) - 1);
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS;
    worker.bgw_start_time = BgWorkerStart_ConsistentState;
    worker.bgw_restart_time = 5;
    std::strncpy(worker.bgw_library_name, "bioseq_cache", sizeof(worker.bgw_library_name) - 1);
    std::strncpy(
        worker.bgw_function_name, janitor_worker_function, sizeof(worker.bgw_function_name) - 1);
    worker.bgw_main_arg = Datum{0};
    worker.bgw_notify_pid = 0;
    RegisterBackgroundWorker(&worker);
    g_registered = true;
    return true;
}

bool static_janitor_registered() noexcept {
    return g_registered;
}

bool register_reusable_janitor_callback(reusable_janitor_callback callback) noexcept {
    if (callback == nullptr) {
        return false;
    }
    if (g_reusable_janitor_callback == nullptr) {
        g_reusable_janitor_callback = callback;
        return true;
    }
    return g_reusable_janitor_callback == callback;
}

void unregister_reusable_janitor_callback(reusable_janitor_callback callback) noexcept {
    if (callback != nullptr && g_reusable_janitor_callback == callback) {
        g_reusable_janitor_callback = nullptr;
    }
}

} // namespace bioseq::cache::pg::maintenance

extern "C" PGDLLEXPORT void bioseq_cache_storage_janitor_main(Datum) {
    using namespace bioseq::cache::pg::maintenance;
    using namespace bioseq::cache::pg::storage;

    pqsignal(SIGTERM, janitor_sigterm);
    pqsignal(SIGHUP, janitor_sighup);
    BackgroundWorkerUnblockSignals();
    publish_worker_lifecycle(worker_lifecycle::starting, MyProcPid);
    if (!attach_storage()) {
        publish_worker_lifecycle(worker_lifecycle::failed, 0);
        proc_exit(1);
    }
    publish_worker_lifecycle(worker_lifecycle::running, MyProcPid);

    while (!g_terminate_requested) {
        if (g_reload_requested) {
            g_reload_requested = 0;
            ProcessConfigFile(PGC_SIGHUP);
        }
        publish_worker_heartbeat();
        run_reusable_janitor_callback();
        static_cast<void>(janitor_sweep());

        ResetLatch(MyLatch);
        const int events = WaitLatch(MyLatch,
                                     WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                                     static_cast<long>(janitor_interval_ms()),
                                     PG_WAIT_EXTENSION);
        if ((events & WL_LATCH_SET) != 0) {
            ResetLatch(MyLatch);
        }
    }

    publish_worker_lifecycle(worker_lifecycle::stopping, MyProcPid);
    run_reusable_janitor_callback();
    static_cast<void>(janitor_sweep());
    publish_worker_lifecycle(worker_lifecycle::stopped, 0);
    /* The janitor releases the pin that keeps shared storage alive for this server
     * run. Exit callbacks then detach the storage references and local mapping. */
    static_cast<void>(bioseq::cache::pg::shared_registry::orderly_unpin_shared_registry());
    proc_exit(0);
}
