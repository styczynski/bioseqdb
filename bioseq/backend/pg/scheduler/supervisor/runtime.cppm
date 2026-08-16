/**
 * @file scheduler/supervisor/runtime.cppm
 * @brief PostgreSQL-facing scheduler supervision and stats implementation.
 */
module;
#include <bioseq/pg_compat.h>

extern "C" {
#include "access/parallel.h"
#include "libpq/pqsignal.h"
#include "storage/condition_variable.h"
#include "storage/dsm_impl.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"

#include "miscadmin.h"
#include "port/atomics.h"
#include "postmaster/bgworker.h"
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>

module bioseq.postgres.scheduler.supervisor;
import :persist_queue_publish;
import bioseq.common.logging;
import bioseq.postgres.configuration.core;
import bioseq.postgres.configuration.scheduler;
import bioseq.instrumentation.clock;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;

namespace bioseq::postgres::scheduler {

/** @brief Previous shared-memory request hook retained for callback chaining. */
inline shmem_request_hook_type prev_shmem_request_hook = nullptr;
/** @brief Previous shared-memory startup hook retained for callback chaining. */
inline shmem_startup_hook_type prev_shmem_startup_hook = nullptr;
/** @brief Signal-safe termination flag consumed by the supervisor loop. */
inline volatile sig_atomic_t got_sigterm = 0;
/** @brief Signal-safe configuration reload flag consumed by the supervisor loop. */
inline volatile sig_atomic_t got_sighup = 0;

/**
 * @brief Resets scheduler counters and timing anchors in shared memory.
 * @param[out] stats Shared counters owned by the current cluster generation.
 * @param[in] now_ns Monotonic reset time in nanoseconds.
 */
void reset_shared_stats(pg_scheduler_stats& stats, std::uint64_t now_ns) {
    pg_atomic_write_u64(&stats.batches_scheduled, 0);
    pg_atomic_write_u64(&stats.batches_processed, 0);
    pg_atomic_write_u64(&stats.batches_in_flight, 0);
    pg_atomic_write_u64(&stats.queries_waiting, 0);
    pg_atomic_write_u64(&stats.queries_processed, 0);
    pg_atomic_write_u64(&stats.rate_window_start_ns, now_ns);
    pg_atomic_write_u64(&stats.rate_window_start_queries, 0);
    pg_atomic_write_u64(&stats.restart_ns, now_ns);
    pg_atomic_write_u64(&stats.inflight_seq, 0);
    for (std::size_t slot = 0; slot < pg_scheduler_stats::k_inflight_slots; ++slot) {
        pg_atomic_write_u64(&stats.inflight_submit_ns[slot], 0);
        pg_atomic_write_u32(&stats.inflight_generation[slot], 0);
    }
}

/** @brief Chains the prior request hook and reserves scheduler shared memory. */
void scheduler_shmem_request() {
    if (prev_shmem_request_hook != nullptr) {
        prev_shmem_request_hook();
    }
    RequestAddinShmemSpace(sizeof(scheduler_shared_state));
}

/** @brief Chains the prior startup hook and initializes scheduler shared state once. */
void scheduler_shmem_startup() {
    if (prev_shmem_startup_hook != nullptr) {
        prev_shmem_startup_hook();
    }

    bool found = false;
    g_shared = static_cast<scheduler_shared_state*>(
        ShmemInitStruct("bioseq scheduler shared state", sizeof(scheduler_shared_state), &found));
    if (!found) {
        pg_atomic_init_u32(&g_shared->cfg_max_concurrency, 0);
        pg_atomic_init_u32(&g_shared->cfg_generation, 1);
        pg_atomic_init_u32(&g_shared->state, 1);

        pg_atomic_init_u64(&g_shared->stats.batches_scheduled, 0);
        pg_atomic_init_u64(&g_shared->stats.batches_processed, 0);
        pg_atomic_init_u64(&g_shared->stats.batches_in_flight, 0);
        pg_atomic_init_u64(&g_shared->stats.queries_waiting, 0);
        pg_atomic_init_u64(&g_shared->stats.queries_processed, 0);
        pg_atomic_init_u64(&g_shared->stats.rate_window_start_ns, 0);
        pg_atomic_init_u64(&g_shared->stats.rate_window_start_queries, 0);
        pg_atomic_init_u64(&g_shared->stats.restart_ns, 0);
        pg_atomic_init_u64(&g_shared->stats.inflight_seq, 0);
        for (std::size_t slot = 0; slot < pg_scheduler_stats::k_inflight_slots; ++slot) {
            pg_atomic_init_u64(&g_shared->stats.inflight_submit_ns[slot], 0);
            pg_atomic_init_u32(&g_shared->stats.inflight_generation[slot], 0);
        }
        initialize_minimizer_sequence_build_stats(g_shared->minimizer_sequence_build_stats);
        initialize_async_minimizer_sequence_worker_boundary_stats(
            g_shared->async_minimizer_sequence_worker_boundary_stats);
        for (pg_persist_queue_slot& slot : g_shared->persist_queue) {
            pg_atomic_init_u32(&slot.state, 0);
            pg_atomic_init_u32(&slot.generation, 0);
            slot.db_oid = InvalidOid;
            slot.user_relid = InvalidOid;
            slot.tablespace_oid = InvalidOid;
            slot.main_relfilenode = InvalidOid;
            slot.column_attno = 0;
            slot.bucket_idx = 0;
            slot.shard_idx = 0;
            slot.sequence_rowno = 0;
            slot.payload_bytes = 0;
            slot.dsm = DSM_HANDLE_INVALID;
            slot.payload_kind = static_cast<std::uint32_t>(persist_payload_kind::none);
        }
        pg_atomic_init_u32(&g_shared->runtime_init_state,
                           static_cast<std::uint32_t>(runtime_initialization_state::pending));
        pg_atomic_init_u32(&g_shared->runtime_init_generation, 0);
        ConditionVariableInit(&g_shared->runtime_init_cv);
        pg_atomic_init_u32(&g_shared->runtime_init_errc, k_shader_cache_ok_wire_value);
        g_shared->cache_digest_hex[0] = '\0';
        pg_atomic_init_u32(&g_shared->supervisor_pgprocno,
                           static_cast<std::uint32_t>(INVALID_PROC_NUMBER));
        pg_atomic_init_u32(&g_shared->supervisor_generation, 0);
        for (shader_compile_slot& slot : g_shared->compile_queue) {
            pg_atomic_init_u32(&slot.state, static_cast<std::uint32_t>(compile_slot_state::free));
            pg_atomic_init_u32(&slot.generation, 0);
            slot.key = {};
            slot.requester_pid = 0;
            slot.errc = k_shader_cache_ok_wire_value;
        }
        ConditionVariableInit(&g_shared->compile_done_cv);
        reset_shared_stats(g_shared->stats,
                           static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns()));
    }
}

/** @brief Shuts down the process-local parallel pool during backend exit. */
void pool_shutdown_before_exit(int, Datum) {
    bioseq::parallelism::global_pool::instance().shutdown_at_exit();
}

/** @brief Records SIGTERM and wakes the supervisor latch without changing `errno`. */
void supervisor_sigterm(SIGNAL_ARGS) {
    const int save_errno = errno;
    got_sigterm = 1;
    if (MyProc != nullptr) {
        SetLatch(&MyProc->procLatch);
    }
    errno = save_errno;
}

/** @brief Records SIGHUP and wakes the supervisor latch without changing `errno`. */
void supervisor_sighup(SIGNAL_ARGS) {
    const int save_errno = errno;
    got_sighup = 1;
    if (MyProc != nullptr) {
        SetLatch(&MyProc->procLatch);
    }
    errno = save_errno;
}

/**
 * @brief Resolves zero to hardware concurrency and applies PostgreSQL worker limits.
 * @param[in] configured Requested concurrency, or zero for the hardware default.
 * @return A positive effective concurrency.
 */
[[nodiscard]] unsigned effective_max_concurrency(unsigned configured) {
    unsigned concurrency =
        configured == 0 ? bioseq::parallelism::default_concurrency_limit() : configured;
    if (IsParallelWorker() && !bioseq::scheduler::settings::postgres_parallel::enabled) {
        concurrency = 1;
    }
    return std::max(1u, concurrency);
}

} // namespace bioseq::postgres::scheduler

namespace bioseq::postgres::scheduler {

/** @brief Installs scheduler shared-memory hooks while preserving the prior callbacks. */
void install_scheduler_shmem_hooks() {
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = scheduler_shmem_request;
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = scheduler_shmem_startup;
}

/** @brief Registers the process-lifetime scheduler background worker. */
void register_scheduler_supervisor() {
    BackgroundWorker worker;
    std::memset(&worker, 0, sizeof(worker));
    pg_snprintf(worker.bgw_name, BGW_MAXLEN, "bioseq scheduler supervisor");
    pg_snprintf(worker.bgw_type, BGW_MAXLEN, "bioseq scheduler");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 5;
    pg_snprintf(worker.bgw_library_name, BGW_MAXLEN, "bioseq");
    pg_snprintf(worker.bgw_function_name, BGW_MAXLEN, "bioseq_scheduler_supervisor_main");
    worker.bgw_main_arg = Datum{0};
    RegisterBackgroundWorker(&worker);
    bioseq_log<logging_level::debug>(
        "[bioseq] scheduler: shared supervisor background worker registered");
}

/**
 * @brief Reports whether this backend attached scheduler shared memory.
 * @return True when shared supervisor state is available.
 */
[[nodiscard]] bool supervisor_available() noexcept {
    return g_shared != nullptr;
}

/** @brief Applies the current shared configuration to backend-local runtime owners. */
void backend_sync() {
    static bool exit_hook_registered = false;
    if (!exit_hook_registered) {
        before_shmem_exit(pool_shutdown_before_exit, Datum{0});
        exit_hook_registered = true;
    }

    auto& pool = bioseq::parallelism::global_pool::instance();
    const std::uint32_t generation =
        g_shared != nullptr ? pg_atomic_read_u32(&g_shared->cfg_generation) : 0;
    const unsigned configured =
        g_shared != nullptr ? pg_atomic_read_u32(&g_shared->cfg_max_concurrency)
                            : static_cast<unsigned>(std::max(
                                  0, bioseq::scheduler::settings::backend::max_concurrency));
    const auto mode = bioseq::parallelism::pipeline_concurrency_mode_from_int(
        static_cast<int>(bioseq::scheduler::settings::pipeline::mode.get_value()));

    if (!pool.is_active() || pool.adopted_generation() != generation) {
        pool.configure(bioseq::parallelism::pool_config{
            .maximum_concurrency = effective_max_concurrency(configured),
            .mode = mode,
        });
        pool.set_adopted_generation(generation);
    } else {
        pool.set_mode(mode);
    }

    const std::uint64_t index_cap =
        bioseq::settings::index::memory_cap_mb > 0
            ? (static_cast<std::uint64_t>(bioseq::settings::index::memory_cap_mb) << 20)
            : 0;
    bioseq::parallelism::memory_admission_controller::instance().set_index_memory_cap_bytes(
        index_cap);
    install_pg_shader_provider_if_available();
}

/**
 * @brief Admits a batch into shared counters or the backend-local pool.
 * @param[in] queries Query count, with negative values treated as zero.
 * @return A ticket consumed once by `end_batch`.
 */
[[nodiscard]] batch_ticket begin_batch(std::int64_t queries) noexcept {
    if (queries < 0) {
        queries = 0;
    }
    batch_ticket ticket{};
    if (g_shared == nullptr) {
        ticket.local = bioseq::parallelism::global_pool::instance().begin_batch(queries);
        ticket.queries = queries;
        return ticket;
    }

    pg_scheduler_stats& stats = g_shared->stats;
    ticket.shared = true;
    ticket.generation = pg_atomic_read_u32(&g_shared->cfg_generation);
    ticket.queries = queries;
    const std::uint64_t seq = pg_atomic_fetch_add_u64(&stats.inflight_seq, 1);
    ticket.slot = static_cast<std::size_t>(seq % pg_scheduler_stats::k_inflight_slots);
    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    pg_atomic_write_u64(&stats.inflight_submit_ns[ticket.slot], now);
    pg_atomic_write_u32(&stats.inflight_generation[ticket.slot], ticket.generation);
    pg_atomic_fetch_add_u64(&stats.batches_scheduled, 1);
    pg_atomic_fetch_add_u64(&stats.batches_in_flight, 1);
    pg_atomic_fetch_add_u64(&stats.queries_waiting, static_cast<std::uint64_t>(queries));
    return ticket;
}

/**
 * @brief Completes or cancels one admitted scheduler batch.
 * @param[in] ticket Ticket returned by `begin_batch`.
 * @param[in] completed Whether to record completed work instead of cancellation.
 */
void end_batch(batch_ticket ticket, bool completed) noexcept {
    if (!ticket.shared) {
        if (completed) {
            bioseq::parallelism::global_pool::instance().end_batch(ticket.local);
        } else {
            bioseq::parallelism::global_pool::instance().cancel_batch(ticket.local);
        }
        return;
    }
    if (g_shared == nullptr || pg_atomic_read_u32(&g_shared->cfg_generation) != ticket.generation) {
        return;
    }
    pg_scheduler_stats& stats = g_shared->stats;
    pg_atomic_fetch_sub_u64(&stats.batches_in_flight, 1);
    pg_atomic_fetch_sub_u64(&stats.queries_waiting, static_cast<std::uint64_t>(ticket.queries));
    if (completed) {
        pg_atomic_fetch_add_u64(&stats.batches_processed, 1);
        pg_atomic_fetch_add_u64(&stats.queries_processed,
                                static_cast<std::uint64_t>(ticket.queries));
    }
    if (ticket.slot < pg_scheduler_stats::k_inflight_slots) {
        pg_atomic_write_u64(&stats.inflight_submit_ns[ticket.slot], 0);
        pg_atomic_write_u32(&stats.inflight_generation[ticket.slot], 0);
    }
}

/** @brief Starts a new scheduler generation with current configuration. */
void restart_cluster() {
    backend_sync();
    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    if (g_shared != nullptr) {
        pg_atomic_write_u32(&g_shared->cfg_max_concurrency,
                            static_cast<std::uint32_t>(std::max(
                                0, bioseq::scheduler::settings::backend::max_concurrency)));
        pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        reset_shared_stats(g_shared->stats, now);
    }
    auto& pool = bioseq::parallelism::global_pool::instance();
    pool.restart(bioseq::parallelism::pool_config{
        .maximum_concurrency = effective_max_concurrency(static_cast<unsigned>(
            std::max(0, bioseq::scheduler::settings::backend::max_concurrency))),
        .mode = bioseq::parallelism::pipeline_concurrency_mode_from_int(
            static_cast<int>(bioseq::scheduler::settings::pipeline::mode.get_value())),
    });
    pool.reset_stats(now);
}

/**
 * @brief Samples shared or backend-local scheduler counters.
 * @return Current queue, throughput, concurrency, and memory state.
 */
[[nodiscard]] scheduler_snapshot snapshot() {
    backend_sync();
    auto& pool = bioseq::parallelism::global_pool::instance();
    scheduler_snapshot snapshot_result{};
    snapshot_result.is_supervisor = g_shared != nullptr;
    snapshot_result.config_generation = g_shared != nullptr
                                            ? pg_atomic_read_u32(&g_shared->cfg_generation)
                                            : pool.adopted_generation();
    snapshot_result.mode = pool.get_mode();
    snapshot_result.min_threads = 1;
    snapshot_result.max_threads = pool.maximum_concurrency();
    snapshot_result.current_threads = pool.live_worker_count();
    snapshot_result.index_memory_reserved_bytes =
        bioseq::parallelism::memory_admission_controller::instance().reserved_bytes();

    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
    if (g_shared != nullptr) {
        pg_scheduler_stats& stats = g_shared->stats;
        snapshot_result.batches_scheduled = pg_atomic_read_u64(&stats.batches_scheduled);
        snapshot_result.batches_processed = pg_atomic_read_u64(&stats.batches_processed);
        snapshot_result.batches_awaiting = pg_atomic_read_u64(&stats.batches_in_flight);
        snapshot_result.queries_waiting = pg_atomic_read_u64(&stats.queries_waiting);
        snapshot_result.queries_processed = pg_atomic_read_u64(&stats.queries_processed);
        std::uint64_t oldest = 0;
        for (std::size_t slot = 0; slot < pg_scheduler_stats::k_inflight_slots; ++slot) {
            const std::uint64_t submit_ns = pg_atomic_read_u64(&stats.inflight_submit_ns[slot]);
            if (submit_ns != 0 && (oldest == 0 || submit_ns < oldest)) {
                oldest = submit_ns;
            }
        }
        snapshot_result.oldest_waiting_batch_age_secs =
            oldest == 0 ? 0.0 : static_cast<double>(now - oldest) / 1'000'000'000.0;
        const std::uint64_t start_ns = pg_atomic_read_u64(&stats.rate_window_start_ns);
        const std::uint64_t start_queries = pg_atomic_read_u64(&stats.rate_window_start_queries);
        const double elapsed =
            start_ns == 0 ? 0.0 : static_cast<double>(now - start_ns) / 1'000'000'000.0;
        if (elapsed > 0.0 && snapshot_result.queries_processed >= start_queries) {
            snapshot_result.queries_per_sec =
                static_cast<double>(snapshot_result.queries_processed - start_queries) / elapsed;
        }
        const std::uint64_t restart_ns = pg_atomic_read_u64(&stats.restart_ns);
        snapshot_result.uptime_secs =
            restart_ns == 0 ? 0.0 : static_cast<double>(now - restart_ns) / 1'000'000'000.0;
        for (pg_persist_queue_slot& slot : g_shared->persist_queue) {
            const std::uint32_t state = pg_atomic_read_u32(&slot.state);
            if (state == 1 || state == 2) {
                ++snapshot_result.async_persist_jobs_waiting;
            }
        }
        return snapshot_result;
    }

    bioseq::parallelism::scheduler_stats& stats = pool.get_scheduler_stats();
    snapshot_result.batches_scheduled = stats.batches_scheduled.load(std::memory_order_acquire);
    snapshot_result.batches_processed = stats.batches_processed.load(std::memory_order_acquire);
    snapshot_result.batches_awaiting = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, stats.batches_in_flight.load(std::memory_order_acquire)));
    snapshot_result.queries_waiting = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, stats.queries_waiting.load(std::memory_order_acquire)));
    snapshot_result.queries_processed = stats.queries_processed.load(std::memory_order_acquire);
    const std::uint64_t restart_ns = stats.restart_ns.load(std::memory_order_acquire);
    snapshot_result.uptime_secs =
        restart_ns == 0 ? 0.0 : static_cast<double>(now - restart_ns) / 1'000'000'000.0;
    return snapshot_result;
}

} // namespace bioseq::postgres::scheduler

/**
 * @brief Runs the registered scheduler supervisor background worker.
 *
 * The external callback spelling and unnamed PostgreSQL argument are ABI-mandated.
 */
extern "C" PGDLLEXPORT void bioseq_scheduler_supervisor_main(Datum) {
    using namespace bioseq::postgres::scheduler;

    pqsignal(SIGTERM, supervisor_sigterm);
    pqsignal(SIGHUP, supervisor_sighup);
    BackgroundWorkerUnblockSignals();

    while (g_shared == nullptr && !got_sigterm) {
        WaitLatch(
            MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1000L, PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
    }
    if (g_shared == nullptr) {
        proc_exit(0);
    }

    pg_atomic_write_u32(&g_shared->supervisor_pgprocno,
                        static_cast<std::uint32_t>(MyProc->vxid.procNumber));
    pg_atomic_fetch_add_u32(&g_shared->supervisor_generation, 1);
    pg_atomic_write_u32(&g_shared->runtime_init_state,
                        static_cast<std::uint32_t>(runtime_initialization_state::pending));
    pg_atomic_fetch_add_u32(&g_shared->runtime_init_generation, 1);
    publish_runtime_init_result(load_bioseq_runtime());

    pg_atomic_write_u32(&g_shared->cfg_max_concurrency,
                        static_cast<std::uint32_t>(
                            std::max(0, bioseq::scheduler::settings::backend::max_concurrency)));
    pg_atomic_write_u32(&g_shared->state, 1);
    pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
    reset_shared_stats(g_shared->stats,
                       static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns()));

    while (!got_sigterm) {
        const int rc = WaitLatch(
            MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, 1000L, PG_WAIT_EXTENSION);
        if ((rc & WL_LATCH_SET) != 0) {
            ResetLatch(MyLatch);
        }
        CHECK_FOR_INTERRUPTS();
        if (got_sighup) {
            got_sighup = 0;
            ProcessConfigFile(PGC_SIGHUP);
            pg_atomic_write_u32(&g_shared->cfg_max_concurrency,
                                static_cast<std::uint32_t>(std::max(
                                    0, bioseq::scheduler::settings::backend::max_concurrency)));
            pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        }

        service_pending_shader_compiles();
        process_one_persist_job_if_ready();

        pg_scheduler_stats& stats = g_shared->stats;
        const std::uint64_t now =
            static_cast<std::uint64_t>(bioseq::instrumentation::clock::now_ns());
        const std::uint64_t start_ns = pg_atomic_read_u64(&stats.rate_window_start_ns);
        if (start_ns == 0 || now - start_ns > 5'000'000'000ull) {
            pg_atomic_write_u64(&stats.rate_window_start_ns, now);
            pg_atomic_write_u64(&stats.rate_window_start_queries,
                                pg_atomic_read_u64(&stats.queries_processed));
        }
    }

    pg_atomic_write_u32(&g_shared->state, 0);
    pg_atomic_write_u32(&g_shared->supervisor_pgprocno,
                        static_cast<std::uint32_t>(INVALID_PROC_NUMBER));
    ConditionVariableBroadcast(&g_shared->runtime_init_cv);
    ConditionVariableBroadcast(&g_shared->compile_done_cv);
    proc_exit(0);
}
