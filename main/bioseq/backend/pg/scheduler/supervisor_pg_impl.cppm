/**
 * @file supervisor_pg_impl.cppm
 * @brief PostgreSQL-facing scheduler supervision and stats implementation.
 */
module;
#include "common.h"

extern "C" {
#include "access/parallel.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "port/atomics.h"
#include "storage/dsm_impl.h"
#include "storage/condition_variable.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <csignal>
#include <cstdint>
#include <cstring>

module bioseq.scheduler.supervisor_pg;
import bioseq.common.mlogging;
import bioseq.config;                  // DOC0322 §9.3 — bioseq_scheduler_*, bioseq::settings::index_async_persist, bioseq::settings::index_memory_cap_mb GUCs
import bioseq.instr;
import bioseq.parallelism.mode;
import bioseq.parallelism.concepts;
import bioseq.parallelism.pool;
import bioseq.parallelism.memory_admission;

namespace bioseq::parallelism::pg {

inline shmem_request_hook_type prev_shmem_request_hook = nullptr;
inline shmem_startup_hook_type prev_shmem_startup_hook = nullptr;
inline volatile sig_atomic_t got_sigterm = 0;
inline volatile sig_atomic_t got_sighup = 0;


void reset_shared_stats(PgSchedulerStats& stats, std::uint64_t now_ns) {
    pg_atomic_write_u64(&stats.batches_scheduled, 0);
    pg_atomic_write_u64(&stats.batches_processed, 0);
    pg_atomic_write_u64(&stats.batches_in_flight, 0);
    pg_atomic_write_u64(&stats.queries_waiting, 0);
    pg_atomic_write_u64(&stats.queries_processed, 0);
    pg_atomic_write_u64(&stats.rate_window_start_ns, now_ns);
    pg_atomic_write_u64(&stats.rate_window_start_queries, 0);
    pg_atomic_write_u64(&stats.restart_ns, now_ns);
    pg_atomic_write_u64(&stats.inflight_seq, 0);
    for (std::size_t slot = 0; slot < PgSchedulerStats::kInflightSlots; ++slot) {
        pg_atomic_write_u64(&stats.inflight_submit_ns[slot], 0);
        pg_atomic_write_u32(&stats.inflight_generation[slot], 0);
    }
}

void scheduler_shmem_request() {
    if (prev_shmem_request_hook != nullptr) {
        prev_shmem_request_hook();
    }
    RequestAddinShmemSpace(sizeof(BioseqSchedulerShared));
}

void scheduler_shmem_startup() {
    if (prev_shmem_startup_hook != nullptr) {
        prev_shmem_startup_hook();
    }

    bool found = false;
    g_shared = static_cast<BioseqSchedulerShared*>(
        ShmemInitStruct("bioseq scheduler shared state",
                        sizeof(BioseqSchedulerShared),
                        &found));
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
        for (std::size_t slot = 0; slot < PgSchedulerStats::kInflightSlots; ++slot) {
            pg_atomic_init_u64(&g_shared->stats.inflight_submit_ns[slot], 0);
            pg_atomic_init_u32(&g_shared->stats.inflight_generation[slot], 0);
        }
        init_mps_build_stats(g_shared->mps_build_stats);
        init_async_mps_worker_boundary_stats(
            g_shared->async_mps_worker_boundary_stats);
        for (PgPersistQueueSlot& slot : g_shared->persist_queue) {
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
            slot.payload_kind = static_cast<std::uint32_t>(
                PersistPayloadKind::none);
            slot.requested_provider =
                index_payload_storage_backend_wire_value(
                    IndexPayloadStorageBackend::postgres_relation);
        }
        pg_atomic_init_u32(
            &g_shared->runtime_init_state,
            static_cast<std::uint32_t>(RuntimeInitState::Pending));
        pg_atomic_init_u32(&g_shared->runtime_init_generation, 0);
        ConditionVariableInit(&g_shared->runtime_init_cv);
        pg_atomic_init_u32(
            &g_shared->runtime_init_errc,
            kShaderCacheOkWireValue);
        g_shared->cache_digest_hex[0] = '\0';
        pg_atomic_init_u32(
            &g_shared->supervisor_pgprocno,
            static_cast<std::uint32_t>(INVALID_PROC_NUMBER));
        pg_atomic_init_u32(&g_shared->supervisor_generation, 0);
        for (ShaderCompileSlot& slot : g_shared->compile_queue) {
            pg_atomic_init_u32(
                &slot.state,
                static_cast<std::uint32_t>(CompileSlotState::Free));
            pg_atomic_init_u32(&slot.generation, 0);
            slot.key = {};
            slot.requester_pid = 0;
            slot.errc = kShaderCacheOkWireValue;
        }
        ConditionVariableInit(&g_shared->compile_done_cv);
        reset_shared_stats(g_shared->stats, static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()));
    }
}

void pool_shutdown_before_exit(int, Datum) {
    bioseq::parallelism::GlobalPool::instance().shutdown_at_exit();
}

void supervisor_sigterm(SIGNAL_ARGS) {
    const int save_errno = errno;
    got_sigterm = 1;
    if (MyProc != nullptr) {
        SetLatch(&MyProc->procLatch);
    }
    errno = save_errno;
}

void supervisor_sighup(SIGNAL_ARGS) {
    const int save_errno = errno;
    got_sighup = 1;
    if (MyProc != nullptr) {
        SetLatch(&MyProc->procLatch);
    }
    errno = save_errno;
}

[[nodiscard]] unsigned effective_max_concurrency(unsigned configured) {
    unsigned concurrency = configured == 0
        ? bioseq::parallelism::default_concurrency()
        : configured;
    if (IsParallelWorker() && !bioseq::scheduler::settings::allow_pg_parallel_composition) {
        concurrency = 1;
    }
    return std::max(1u, concurrency);
}

} // namespace bioseq::parallelism::pg

namespace bioseq::parallelism::pg {

void install_scheduler_shmem_hooks() {
    prev_shmem_request_hook = shmem_request_hook;
    shmem_request_hook = scheduler_shmem_request;
    prev_shmem_startup_hook = shmem_startup_hook;
    shmem_startup_hook = scheduler_shmem_startup;
}

void register_scheduler_supervisor() {
    BackgroundWorker worker;
    std::memset(&worker, 0, sizeof(worker));
    pg_snprintf(worker.bgw_name, BGW_MAXLEN, "bioseq scheduler supervisor");
    pg_snprintf(worker.bgw_type, BGW_MAXLEN, "bioseq scheduler");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = 5;
    pg_snprintf(worker.bgw_library_name, BGW_MAXLEN, "bioseq");
    pg_snprintf(worker.bgw_function_name, BGW_MAXLEN,
                "bioseq_scheduler_supervisor_main");
    worker.bgw_main_arg = Datum{0};
    RegisterBackgroundWorker(&worker);
    bioseq_log<DBG>("[bioseq] scheduler: shared supervisor background worker registered");
}


[[nodiscard]] bool supervisor_available() noexcept {
    return g_shared != nullptr;
}

void backend_sync() {
    static bool exit_hook_registered = false;
    if (!exit_hook_registered) {
        before_shmem_exit(pool_shutdown_before_exit, Datum{0});
        exit_hook_registered = true;
    }

    auto& pool = bioseq::parallelism::GlobalPool::instance();
    const std::uint32_t generation = g_shared != nullptr
        ? pg_atomic_read_u32(&g_shared->cfg_generation)
        : 0;
    const unsigned configured = g_shared != nullptr
        ? pg_atomic_read_u32(&g_shared->cfg_max_concurrency)
        : static_cast<unsigned>(std::max(0, bioseq::scheduler::settings::max_concurrency));
    const auto mode = bioseq::parallelism::mode_from_int(
        static_cast<int>(bioseq::scheduler::settings::pipeline_concurrency_mode.value()));

    if (!pool.active() || pool.adopted_generation() != generation) {
        pool.configure(bioseq::parallelism::PoolConfig{
            .max_concurrency = effective_max_concurrency(configured),
            .mode = mode,
        });
        pool.set_adopted_generation(generation);
    } else {
        pool.set_mode(mode);
    }

    const std::uint64_t index_cap = bioseq::settings::index_memory_cap_mb > 0
        ? (static_cast<std::uint64_t>(bioseq::settings::index_memory_cap_mb) << 20)
        : 0;
    bioseq::parallelism::MemoryAdmissionController::instance()
        .set_index_memory_cap_bytes(index_cap);
    install_pg_shader_provider_if_available();
}

[[nodiscard]] BatchTicket begin_batch(std::int64_t queries) noexcept {
    if (queries < 0) {
        queries = 0;
    }
    BatchTicket ticket{};
    if (g_shared == nullptr) {
        ticket.local = bioseq::parallelism::GlobalPool::instance().begin_batch(queries);
        ticket.queries = queries;
        return ticket;
    }

    PgSchedulerStats& stats = g_shared->stats;
    ticket.shared = true;
    ticket.generation = pg_atomic_read_u32(&g_shared->cfg_generation);
    ticket.queries = queries;
    const std::uint64_t seq = pg_atomic_fetch_add_u64(&stats.inflight_seq, 1);
    ticket.slot = static_cast<std::size_t>(seq % PgSchedulerStats::kInflightSlots);
    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    pg_atomic_write_u64(&stats.inflight_submit_ns[ticket.slot], now);
    pg_atomic_write_u32(&stats.inflight_generation[ticket.slot], ticket.generation);
    pg_atomic_fetch_add_u64(&stats.batches_scheduled, 1);
    pg_atomic_fetch_add_u64(&stats.batches_in_flight, 1);
    pg_atomic_fetch_add_u64(&stats.queries_waiting, static_cast<std::uint64_t>(queries));
    return ticket;
}

void end_batch(BatchTicket ticket, bool completed) noexcept {
    if (!ticket.shared) {
        if (completed) {
            bioseq::parallelism::GlobalPool::instance().end_batch(ticket.local);
        } else {
            bioseq::parallelism::GlobalPool::instance().cancel_batch(ticket.local);
        }
        return;
    }
    if (g_shared == nullptr
        || pg_atomic_read_u32(&g_shared->cfg_generation) != ticket.generation) {
        return;
    }
    PgSchedulerStats& stats = g_shared->stats;
    pg_atomic_fetch_sub_u64(&stats.batches_in_flight, 1);
    pg_atomic_fetch_sub_u64(&stats.queries_waiting, static_cast<std::uint64_t>(ticket.queries));
    if (completed) {
        pg_atomic_fetch_add_u64(&stats.batches_processed, 1);
        pg_atomic_fetch_add_u64(&stats.queries_processed, static_cast<std::uint64_t>(ticket.queries));
    }
    if (ticket.slot < PgSchedulerStats::kInflightSlots) {
        pg_atomic_write_u64(&stats.inflight_submit_ns[ticket.slot], 0);
        pg_atomic_write_u32(&stats.inflight_generation[ticket.slot], 0);
    }
}

void restart_cluster() {
    backend_sync();
    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (g_shared != nullptr) {
        pg_atomic_write_u32(
            &g_shared->cfg_max_concurrency,
            static_cast<std::uint32_t>(std::max(0, bioseq::scheduler::settings::max_concurrency)));
        pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        reset_shared_stats(g_shared->stats, now);
    }
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    pool.restart(bioseq::parallelism::PoolConfig{
        .max_concurrency = effective_max_concurrency(
            static_cast<unsigned>(std::max(0, bioseq::scheduler::settings::max_concurrency))),
        .mode = bioseq::parallelism::mode_from_int(
            static_cast<int>(bioseq::scheduler::settings::pipeline_concurrency_mode.value())),
    });
    pool.reset_stats(now);
}

[[nodiscard]] SchedulerSnapshot snapshot() {
    backend_sync();
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    SchedulerSnapshot out{};
    out.supervisor = g_shared != nullptr;
    out.config_generation = g_shared != nullptr
        ? pg_atomic_read_u32(&g_shared->cfg_generation)
        : pool.adopted_generation();
    out.mode = pool.mode();
    out.min_threads = 1;
    out.max_threads = pool.max_concurrency();
    out.current_threads = pool.live_workers();
    out.index_memory_reserved_bytes =
        bioseq::parallelism::MemoryAdmissionController::instance().reserved_bytes();

    const std::uint64_t now = static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (g_shared != nullptr) {
        PgSchedulerStats& stats = g_shared->stats;
        out.batches_scheduled = pg_atomic_read_u64(&stats.batches_scheduled);
        out.batches_processed = pg_atomic_read_u64(&stats.batches_processed);
        out.batches_awaiting = pg_atomic_read_u64(&stats.batches_in_flight);
        out.queries_waiting = pg_atomic_read_u64(&stats.queries_waiting);
        out.queries_processed = pg_atomic_read_u64(&stats.queries_processed);
        std::uint64_t oldest = 0;
        for (std::size_t slot = 0; slot < PgSchedulerStats::kInflightSlots; ++slot) {
            const std::uint64_t submit_ns =
                pg_atomic_read_u64(&stats.inflight_submit_ns[slot]);
            if (submit_ns != 0 && (oldest == 0 || submit_ns < oldest)) {
                oldest = submit_ns;
            }
        }
        out.oldest_waiting_batch_age_secs = oldest == 0
            ? 0.0
            : static_cast<double>(now - oldest) / 1'000'000'000.0;
        const std::uint64_t start_ns = pg_atomic_read_u64(&stats.rate_window_start_ns);
        const std::uint64_t start_queries = pg_atomic_read_u64(&stats.rate_window_start_queries);
        const double elapsed = start_ns == 0
            ? 0.0
            : static_cast<double>(now - start_ns) / 1'000'000'000.0;
        if (elapsed > 0.0 && out.queries_processed >= start_queries) {
            out.queries_per_sec =
                static_cast<double>(out.queries_processed - start_queries) / elapsed;
        }
        const std::uint64_t restart_ns = pg_atomic_read_u64(&stats.restart_ns);
        out.uptime_secs = restart_ns == 0
            ? 0.0
            : static_cast<double>(now - restart_ns) / 1'000'000'000.0;
        for (PgPersistQueueSlot& slot : g_shared->persist_queue) {
            const std::uint32_t state = pg_atomic_read_u32(&slot.state);
            if (state == 1 || state == 2) {
                ++out.async_persist_jobs_waiting;
            }
        }
        return out;
    }

    SchedulerStats& stats = pool.stats();
    out.batches_scheduled = stats.batches_scheduled.load(std::memory_order_acquire);
    out.batches_processed = stats.batches_processed.load(std::memory_order_acquire);
    out.batches_awaiting = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, stats.batches_in_flight.load(std::memory_order_acquire)));
    out.queries_waiting = static_cast<std::uint64_t>(
        std::max<std::int64_t>(0, stats.queries_waiting.load(std::memory_order_acquire)));
    out.queries_processed = stats.queries_processed.load(std::memory_order_acquire);
    const std::uint64_t restart_ns = stats.restart_ns.load(std::memory_order_acquire);
    out.uptime_secs = restart_ns == 0
        ? 0.0
        : static_cast<double>(now - restart_ns) / 1'000'000'000.0;
    return out;
}

} // namespace bioseq::parallelism::pg

extern "C" PGDLLEXPORT void bioseq_scheduler_supervisor_main(Datum) {
    using namespace bioseq::parallelism::pg;

    pqsignal(SIGTERM, supervisor_sigterm);
    pqsignal(SIGHUP, supervisor_sighup);
    BackgroundWorkerUnblockSignals();

    while (g_shared == nullptr && !got_sigterm) {
        WaitLatch(MyLatch,
                  WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                  1000L,
                  PG_WAIT_EXTENSION);
        ResetLatch(MyLatch);
    }
    if (g_shared == nullptr) {
        proc_exit(0);
    }

    pg_atomic_write_u32(
        &g_shared->supervisor_pgprocno,
        static_cast<std::uint32_t>(getProcNo_compat(MyProc)));
    pg_atomic_fetch_add_u32(&g_shared->supervisor_generation, 1);
    pg_atomic_write_u32(
        &g_shared->runtime_init_state,
        static_cast<std::uint32_t>(RuntimeInitState::Pending));
    pg_atomic_fetch_add_u32(&g_shared->runtime_init_generation, 1);
    publish_runtime_init_result(load_bioseq_runtime());

    pg_atomic_write_u32(
        &g_shared->cfg_max_concurrency,
        static_cast<std::uint32_t>(std::max(0, bioseq::scheduler::settings::max_concurrency)));
    pg_atomic_write_u32(&g_shared->state, 1);
    pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
    reset_shared_stats(
        g_shared->stats,
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns()));

    while (!got_sigterm) {
        const int rc =
            WaitLatch(MyLatch,
                      WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
                      1000L,
                      PG_WAIT_EXTENSION);
        if ((rc & WL_LATCH_SET) != 0) {
            ResetLatch(MyLatch);
        }
        CHECK_FOR_INTERRUPTS();
        if (got_sighup) {
            got_sighup = 0;
            ProcessConfigFile(PGC_SIGHUP);
            pg_atomic_write_u32(
                &g_shared->cfg_max_concurrency,
                static_cast<std::uint32_t>(
                    std::max(0, bioseq::scheduler::settings::max_concurrency)));
            pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        }

        service_pending_shader_compiles();
        process_one_persist_job_if_ready();

        PgSchedulerStats& stats = g_shared->stats;
        const std::uint64_t now =
            static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
        const std::uint64_t start_ns =
            pg_atomic_read_u64(&stats.rate_window_start_ns);
        if (start_ns == 0 || now - start_ns > 5'000'000'000ull) {
            pg_atomic_write_u64(&stats.rate_window_start_ns, now);
            pg_atomic_write_u64(
                &stats.rate_window_start_queries,
                pg_atomic_read_u64(&stats.queries_processed));
        }
    }

    pg_atomic_write_u32(&g_shared->state, 0);
    pg_atomic_write_u32(
        &g_shared->supervisor_pgprocno,
        static_cast<std::uint32_t>(INVALID_PROC_NUMBER));
    ConditionVariableBroadcast(&g_shared->runtime_init_cv);
    ConditionVariableBroadcast(&g_shared->compile_done_cv);
    proc_exit(0);
}
