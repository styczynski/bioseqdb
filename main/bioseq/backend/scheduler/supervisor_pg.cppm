/**
 * @file supervisor_pg.cppm
 * @brief PostgreSQL-facing scheduler supervision and stats bridge.
 */
module;
#include "common.h"
#include "columnar/columnar.h"

extern "C" {
#include "access/xact.h"
#include "access/parallel.h"
#include "executor/spi.h"
#include "libpq/pqsignal.h"
#include "miscadmin.h"
#include "postmaster/bgworker.h"
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/dsm_impl.h"
#include "storage/ipc.h"
#include "storage/latch.h"
#include "storage/proc.h"
#include "storage/shmem.h"
#include "tcop/tcopprot.h"
#include "utils/guc.h"
#include "utils/snapmgr.h"

void StartTransactionCommand(void);
void CommitTransactionCommand(void);
void AbortCurrentTransaction(void);
}

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <limits>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module bioseq_scheduler.supervisor_pg;
import mlogging;
import bioseq_instr;
import bioseq_parallelism.mode;
import bioseq_parallelism.concepts;
import bioseq_parallelism.pool;
import bioseq_parallelism.memory_admission;
import pg_utils;
import bioseq_indexing.type;
import bioseq_storage_indexing_pg;

namespace bioseq::parallelism::pg {

struct PgSchedulerStats {
    static constexpr std::size_t kInflightSlots =
        bioseq::parallelism::SchedulerStats::kInflightSlots;
    pg_atomic_uint64 batches_scheduled;
    pg_atomic_uint64 batches_processed;
    pg_atomic_uint64 batches_in_flight;
    pg_atomic_uint64 queries_waiting;
    pg_atomic_uint64 queries_processed;
    pg_atomic_uint64 rate_window_start_ns;
    pg_atomic_uint64 rate_window_start_queries;
    pg_atomic_uint64 restart_ns;
    pg_atomic_uint64 inflight_submit_ns[kInflightSlots];
    pg_atomic_uint32 inflight_generation[kInflightSlots];
    pg_atomic_uint64 inflight_seq;
};

struct PgPersistQueueSlot {
    pg_atomic_uint32 state;
    pg_atomic_uint32 generation;
    Oid db_oid;
    Oid user_relid;
    Oid tablespace_oid;
    Oid main_relfilenode;
    std::int16_t column_attno;
    std::uint8_t bucket_idx;
    std::uint32_t shard_idx;
    std::uint64_t sequence_rowno;
    std::uint64_t payload_bytes;
    dsm_handle dsm;
};

struct IndexPersistDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t chunk_count;
    std::uint32_t index_name_len;
    std::uint64_t payload_bytes;
};

struct IndexPersistDsmChunk {
    std::int64_t sub_key;
    std::uint32_t chunk_no;
    std::uint32_t reserved;
    std::uint64_t offset;
    std::uint64_t size;
};

struct IndexPersistFileDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t index_name_len;
    std::uint32_t path_len;
    std::uint64_t file_bytes;
    std::uint64_t chunk_count;
};

struct PersistFileChunkHeader {
    std::int64_t sub_key;
    std::uint32_t chunk_no;
    std::uint32_t reserved;
    std::uint64_t size;
};

inline constexpr std::uint32_t kIndexPersistDsmMagic = 0xB1051D58u;
inline constexpr std::uint32_t kIndexPersistDsmVersion = 1;
inline constexpr std::uint32_t kIndexPersistFileDsmMagic = 0xB1051D59u;
inline constexpr std::uint32_t kIndexPersistFileDsmVersion = 1;

struct BioseqSchedulerShared {
    pg_atomic_uint32 cfg_max_concurrency;
    pg_atomic_uint32 cfg_generation;
    pg_atomic_uint32 state;
    PgSchedulerStats stats;
    static constexpr std::size_t kPersistQueueSize = 64;
    PgPersistQueueSlot persist_queue[kPersistQueueSize];
};

inline BioseqSchedulerShared* g_shared = nullptr;
inline shmem_request_hook_type prev_shmem_request_hook = nullptr;
inline shmem_startup_hook_type prev_shmem_startup_hook = nullptr;
inline volatile sig_atomic_t got_sigterm = 0;
inline volatile sig_atomic_t got_sighup = 0;
inline bool g_persist_xact_callback_registered = false;

static_assert(alignof(BioseqSchedulerShared) <= MAXIMUM_ALIGNOF);

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
        }
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
    if (IsParallelWorker() && !bioseq_scheduler_allow_pg_parallel_composition) {
        concurrency = 1;
    }
    return std::max(1u, concurrency);
}

} // namespace bioseq::parallelism::pg

export namespace bioseq::parallelism::pg {

// DOC0301 iteration 2 — spawn a one-shot dynamic background worker that builds +
// persists the minimizers_per_sequence index for `relid` off the foreground read
// backend. Uses a dynamic bgworker (no shared_preload_libraries requirement, no
// libpq self-connection); the worker connects to `db_oid`, runs the existing
// `bioseq.update_index(relid, 'minimizers_per_sequence')` driver in its own
// transaction, and exits. Returns false if registration fails (e.g.
// max_worker_processes reached) so the caller can fall back to a foreground build.
[[nodiscard]] inline bool spawn_mps_async_build_worker(Oid db_oid, Oid relid) noexcept {
    if (!OidIsValid(db_oid) || !OidIsValid(relid)) {
        return false;
    }
    BackgroundWorker worker;
    std::memset(&worker, 0, sizeof(worker));
    pg_snprintf(worker.bgw_name, BGW_MAXLEN, "bioseq mps build");
    pg_snprintf(worker.bgw_type, BGW_MAXLEN, "bioseq mps build");
    worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
    worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
    worker.bgw_restart_time = BGW_NEVER_RESTART;
    pg_snprintf(worker.bgw_library_name, BGW_MAXLEN, "bioseq");
    pg_snprintf(worker.bgw_function_name, BGW_MAXLEN,
                "bioseq_mps_async_build_worker_main");
    const std::uint64_t packed =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(db_oid)) << 32)
        | static_cast<std::uint64_t>(static_cast<std::uint32_t>(relid));
    worker.bgw_main_arg = UInt64GetDatum(packed);
    worker.bgw_notify_pid = 0;
    BackgroundWorkerHandle* handle = nullptr;
    if (!RegisterDynamicBackgroundWorker(&worker, &handle)) {
        JustLog("[bioseq] index: could not register async mps build worker "
                "(db={} relid={}); max_worker_processes reached?",
                static_cast<unsigned>(db_oid), static_cast<unsigned>(relid));
        return false;
    }
    JustLog("[bioseq] index: spawned async mps build worker db={} relid={}",
            static_cast<unsigned>(db_oid), static_cast<unsigned>(relid));
    return true;
}

struct BatchTicket {
    bool shared = false;
    std::uint32_t generation = 0;
    std::size_t slot = 0;
    std::int64_t queries = 0;
    bioseq::parallelism::GlobalPool::BatchTicket local{};
};

struct SchedulerSnapshot {
    bool supervisor = false;
    std::uint32_t config_generation = 0;
    bioseq::parallelism::PipelineConcurrencyMode mode =
        bioseq::parallelism::PipelineConcurrencyMode::SINGLE;
    unsigned min_threads = 1;
    unsigned max_threads = 1;
    unsigned current_threads = 0;
    std::uint64_t batches_scheduled = 0;
    std::uint64_t batches_awaiting = 0;
    std::uint64_t batches_processed = 0;
    std::uint64_t queries_waiting = 0;
    std::uint64_t queries_processed = 0;
    double oldest_waiting_batch_age_secs = 0.0;
    double queries_per_sec = 0.0;
    double uptime_secs = 0.0;
    std::uint64_t async_persist_jobs_waiting = 0;
    std::uint64_t index_memory_reserved_bytes = 0;
};

struct PersistTarget {
    Oid db_oid = InvalidOid;
    Oid user_relid = InvalidOid;
    Oid tablespace_oid = InvalidOid;
    Oid main_relfilenode = InvalidOid;
    std::int16_t column_attno = 0;
    std::uint8_t bucket_idx = 0;
    std::uint32_t shard_idx = 0;
    std::uint64_t sequence_rowno = 0;
    std::string_view index_name{};
};

struct PersistChunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::span<const char> bytes{};
};

struct PendingPersistJob {
    PersistTarget target{};
    std::string index_name{};
    std::string temp_file_path{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
    bool published = false;
    bool file_backed = false;
    bioseq::parallelism::MemoryPermit permit{};
};

[[nodiscard]] std::vector<PendingPersistJob>& pending_persist_jobs() {
    static std::vector<PendingPersistJob> jobs;
    return jobs;
}

[[nodiscard]] std::uint64_t align_up_u64(std::uint64_t value, std::uint64_t alignment) noexcept {
    return alignment == 0
        ? value
        : ((value + alignment - 1) / alignment) * alignment;
}

void release_pinned_dsm(dsm_handle handle) noexcept {
    if (handle == DSM_HANDLE_INVALID) {
        return;
    }
    dsm_unpin_segment(handle);
}

void remove_temp_file_noexcept(std::string_view path) noexcept {
    if (path.empty()) {
        return;
    }
    std::string owned{path};
    std::remove(owned.c_str());
}

[[nodiscard]] std::string file_path_from_metadata_dsm(dsm_handle handle) {
    if (handle == DSM_HANDLE_INVALID) {
        return {};
    }
    dsm_segment* seg = dsm_attach(handle);
    if (seg == nullptr) {
        return {};
    }
    const Size mapped = dsm_segment_map_length(seg);
    std::string path;
    if (mapped >= sizeof(IndexPersistFileDsmHeader)) {
        char* base = static_cast<char*>(dsm_segment_address(seg));
        IndexPersistFileDsmHeader header{};
        std::memcpy(&header, base, sizeof(header));
        if (header.magic == kIndexPersistFileDsmMagic
            && header.version == kIndexPersistFileDsmVersion
            && header.index_name_len > 0
            && header.path_len > 0
            && sizeof(IndexPersistFileDsmHeader) + header.index_name_len
                    <= static_cast<std::uint64_t>(mapped)
            && sizeof(IndexPersistFileDsmHeader) + header.index_name_len
                    + header.path_len
                    <= static_cast<std::uint64_t>(mapped)) {
            const char* path_begin =
                base + sizeof(IndexPersistFileDsmHeader) + header.index_name_len;
            path.assign(path_begin, path_begin + header.path_len);
        }
    }
    dsm_detach(seg);
    return path;
}

void remove_file_payload_if_present_noexcept(dsm_handle handle) noexcept {
    try {
        remove_temp_file_noexcept(file_path_from_metadata_dsm(handle));
    } catch (...) {
    }
}

struct PersistFileMetadata {
    std::string index_name{};
    std::string path{};
    std::uint64_t file_bytes = 0;
    std::uint64_t chunk_count = 0;
};

[[nodiscard]] std::optional<PersistFileMetadata>
parse_persist_file_metadata(const char* base, Size mapped) {
    if (mapped < sizeof(IndexPersistFileDsmHeader)) {
        return std::nullopt;
    }
    IndexPersistFileDsmHeader header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != kIndexPersistFileDsmMagic
        || header.version != kIndexPersistFileDsmVersion
        || header.index_name_len == 0
        || header.path_len == 0) {
        return std::nullopt;
    }
    const std::uint64_t names_offset = sizeof(IndexPersistFileDsmHeader);
    const std::uint64_t path_offset = names_offset + header.index_name_len;
    const std::uint64_t path_end = path_offset + header.path_len;
    if (path_offset < names_offset
        || path_end < path_offset
        || path_end > static_cast<std::uint64_t>(mapped)) {
        return std::nullopt;
    }
    PersistFileMetadata meta{};
    meta.index_name.assign(base + names_offset, base + path_offset);
    meta.path.assign(base + path_offset, base + path_end);
    meta.file_bytes = header.file_bytes;
    meta.chunk_count = header.chunk_count;
    return meta;
}

[[nodiscard]] bool publish_persist_job_to_shared_queue(PendingPersistJob& job) noexcept {
    if (g_shared == nullptr || job.dsm == DSM_HANDLE_INVALID) {
        return false;
    }
    for (PgPersistQueueSlot& slot : g_shared->persist_queue) {
        std::uint32_t expected = 0;
        if (!pg_atomic_compare_exchange_u32(&slot.state, &expected, 3)) {
            continue;
        }
        slot.db_oid = job.target.db_oid;
        slot.user_relid = job.target.user_relid;
        slot.tablespace_oid = job.target.tablespace_oid;
        slot.main_relfilenode = job.target.main_relfilenode;
        slot.column_attno = job.target.column_attno;
        slot.bucket_idx = job.target.bucket_idx;
        slot.shard_idx = job.target.shard_idx;
        slot.sequence_rowno = job.target.sequence_rowno;
        slot.payload_bytes = job.payload_bytes;
        slot.dsm = job.dsm;
        pg_atomic_fetch_add_u32(&slot.generation, 1);
        pg_atomic_write_u32(&slot.state, 1);
        JustLog("[bioseq] index: published async persist job row={} index={} bytes={}",
                static_cast<unsigned long long>(job.target.sequence_rowno),
                job.index_name,
                static_cast<unsigned long long>(job.payload_bytes));
        return true;
    }
    return false;
}

extern "C++" void bioseq_persist_xact_callback(XactEvent event, void*) {
    std::vector<PendingPersistJob>& jobs = pending_persist_jobs();
    const bool commit =
        event == XACT_EVENT_COMMIT
        || event == XACT_EVENT_PARALLEL_COMMIT;
    const bool abort =
        event == XACT_EVENT_ABORT
        || event == XACT_EVENT_PARALLEL_ABORT
        || event == XACT_EVENT_PREPARE;
    if (!commit && !abort) {
        return;
    }
    if (commit) {
        for (PendingPersistJob& job : jobs) {
            if (publish_persist_job_to_shared_queue(job)) {
                job.published = true;
            } else {
                if (job.file_backed) {
                    remove_temp_file_noexcept(job.temp_file_path);
                }
                release_pinned_dsm(job.dsm);
                job.dsm = DSM_HANDLE_INVALID;
            }
            job.permit.release();
        }
    } else {
        for (PendingPersistJob& job : jobs) {
            if (!job.published) {
                if (job.file_backed) {
                    remove_temp_file_noexcept(job.temp_file_path);
                }
                release_pinned_dsm(job.dsm);
            }
            job.permit.release();
            job.dsm = DSM_HANDLE_INVALID;
        }
    }
    jobs.clear();
}

void ensure_persist_xact_callback_registered() {
    if (g_persist_xact_callback_registered) {
        return;
    }
    RegisterXactCallback(bioseq_persist_xact_callback, nullptr);
    g_persist_xact_callback_registered = true;
}

[[nodiscard]] bool stage_async_index_persist(
        const PersistTarget& target,
        std::span<const PersistChunk> chunks) {
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    ensure_persist_xact_callback_registered();
    if (!bioseq_index_async_persist || chunks.empty() || target.index_name.empty()) {
        return false;
    }
    std::uint64_t bytes_total = sizeof(IndexPersistDsmHeader);
    bytes_total += target.index_name.size();
    bytes_total = align_up_u64(bytes_total, alignof(IndexPersistDsmChunk));
    const std::uint64_t directory_offset = bytes_total;
    bytes_total += static_cast<std::uint64_t>(chunks.size()) * sizeof(IndexPersistDsmChunk);
    bytes_total = align_up_u64(bytes_total, alignof(std::max_align_t));
    const std::uint64_t payload_offset = bytes_total;
    for (const PersistChunk& chunk : chunks) {
        bytes_total = align_up_u64(bytes_total, alignof(std::max_align_t));
        bytes_total += chunk.bytes.size();
    }
    if (bytes_total == 0 || bytes_total > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        return false;
    }

    auto permit =
        bioseq::parallelism::MemoryAdmissionController::instance()
            .try_reserve(bytes_total);
    if (!permit) {
        JustLog("[bioseq] index: async persist staging skipped by memory admission (bytes={})",
                static_cast<unsigned long long>(bytes_total));
        return false;
    }

    dsm_segment* seg = dsm_create(static_cast<Size>(bytes_total), DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (seg == nullptr) {
        return false;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    IndexPersistDsmHeader header{
        kIndexPersistDsmMagic,
        kIndexPersistDsmVersion,
        static_cast<std::uint32_t>(chunks.size()),
        static_cast<std::uint32_t>(target.index_name.size()),
        bytes_total,
    };
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(base + sizeof(header), target.index_name.data(), target.index_name.size());
    auto* directory = reinterpret_cast<IndexPersistDsmChunk*>(base + directory_offset);
    std::uint64_t cursor = payload_offset;
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const PersistChunk& chunk = chunks[i];
        cursor = align_up_u64(cursor, alignof(std::max_align_t));
        directory[i] = IndexPersistDsmChunk{
            .sub_key = static_cast<std::int64_t>(chunk.sub_key),
            .chunk_no = chunk.chunk_no,
            .reserved = 0,
            .offset = cursor,
            .size = static_cast<std::uint64_t>(chunk.bytes.size()),
        };
        if (!chunk.bytes.empty()) {
            std::memcpy(base + cursor, chunk.bytes.data(), chunk.bytes.size());
        }
        cursor += chunk.bytes.size();
    }
    dsm_pin_segment(seg);
    const dsm_handle handle = dsm_segment_handle(seg);
    dsm_detach(seg);
    const std::uint64_t dsm_ready_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());

    PendingPersistJob job{};
    job.target = target;
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.target.index_name = job.index_name;
    job.dsm = handle;
    job.payload_bytes = bytes_total;
    job.permit = std::move(*permit);
    pending_persist_jobs().push_back(std::move(job));
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    JustLog("[bioseq] index: staged async persist job row={} index={} chunks={} bytes={} dsm_copy_ns={} total_stage_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            pending_persist_jobs().back().index_name,
            chunks.size(),
            static_cast<unsigned long long>(bytes_total),
            static_cast<unsigned long long>(dsm_ready_ns - stage_start_ns),
            static_cast<unsigned long long>(queued_ns - stage_start_ns));
    return true;
}

[[nodiscard]] bool stage_async_index_persist_file(
        const PersistTarget& target,
        std::string temp_file_path,
        std::uint64_t file_bytes,
        std::uint64_t chunk_count) {
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    ensure_persist_xact_callback_registered();
    if (!bioseq_index_async_persist
        || target.index_name.empty()
        || temp_file_path.empty()
        || chunk_count == 0) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }

    std::uint64_t bytes_total = sizeof(IndexPersistFileDsmHeader);
    bytes_total += target.index_name.size();
    bytes_total += temp_file_path.size();
    if (bytes_total == 0
        || bytes_total
            > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }

    dsm_segment* seg = dsm_create(
        static_cast<Size>(bytes_total),
        DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (seg == nullptr) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    IndexPersistFileDsmHeader header{
        kIndexPersistFileDsmMagic,
        kIndexPersistFileDsmVersion,
        static_cast<std::uint32_t>(target.index_name.size()),
        static_cast<std::uint32_t>(temp_file_path.size()),
        file_bytes,
        chunk_count,
    };
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(
        base + sizeof(header),
        target.index_name.data(),
        target.index_name.size());
    std::memcpy(
        base + sizeof(header) + target.index_name.size(),
        temp_file_path.data(),
        temp_file_path.size());
    dsm_pin_segment(seg);
    const dsm_handle handle = dsm_segment_handle(seg);
    dsm_detach(seg);

    PendingPersistJob job{};
    job.target = target;
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.target.index_name = job.index_name;
    job.temp_file_path = std::move(temp_file_path);
    job.dsm = handle;
    job.payload_bytes = file_bytes;
    job.file_backed = true;
    pending_persist_jobs().push_back(std::move(job));
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    JustLog("[bioseq] index: staged file async persist job row={} index={} chunks={} file_bytes={} meta_bytes={} total_stage_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            pending_persist_jobs().back().index_name,
            static_cast<unsigned long long>(chunk_count),
            static_cast<unsigned long long>(file_bytes),
            static_cast<unsigned long long>(bytes_total),
            static_cast<unsigned long long>(queued_ns - stage_start_ns));
    return true;
}

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
    JustLog("[bioseq] scheduler: shared supervisor background worker registered");
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
        : static_cast<unsigned>(std::max(0, bioseq_scheduler_max_concurrency));
    const auto mode = bioseq::parallelism::mode_from_int(
        bioseq_scheduler_pipeline_concurrency_mode);

    if (!pool.active() || pool.adopted_generation() != generation) {
        pool.configure(bioseq::parallelism::PoolConfig{
            .max_concurrency = effective_max_concurrency(configured),
            .mode = mode,
        });
        pool.set_adopted_generation(generation);
    } else {
        pool.set_mode(mode);
    }

    const std::uint64_t index_cap = bioseq_index_memory_cap_mb > 0
        ? (static_cast<std::uint64_t>(bioseq_index_memory_cap_mb) << 20)
        : 0;
    bioseq::parallelism::MemoryAdmissionController::instance()
        .set_index_memory_cap_bytes(index_cap);
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
            static_cast<std::uint32_t>(std::max(0, bioseq_scheduler_max_concurrency)));
        pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        reset_shared_stats(g_shared->stats, now);
    }
    auto& pool = bioseq::parallelism::GlobalPool::instance();
    pool.restart(bioseq::parallelism::PoolConfig{
        .max_concurrency = effective_max_concurrency(
            static_cast<unsigned>(std::max(0, bioseq_scheduler_max_concurrency))),
        .mode = bioseq::parallelism::mode_from_int(
            bioseq_scheduler_pipeline_concurrency_mode),
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
            if (pg_atomic_read_u32(&slot.state) == 1) {
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

struct PersistClaim {
    std::size_t slot_index = 0;
    std::uint32_t generation = 0;
    PersistTarget target{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
};

[[nodiscard]] std::optional<PersistClaim> claim_persist_job() noexcept {
    if (g_shared == nullptr) {
        return std::nullopt;
    }
    for (std::size_t slot_index = 0;
         slot_index < BioseqSchedulerShared::kPersistQueueSize;
         ++slot_index) {
        PgPersistQueueSlot& slot = g_shared->persist_queue[slot_index];
        std::uint32_t expected = 1;
        if (!pg_atomic_compare_exchange_u32(&slot.state, &expected, 2)) {
            continue;
        }
        PersistClaim claim{};
        claim.slot_index = slot_index;
        claim.generation = pg_atomic_read_u32(&slot.generation);
        claim.target.db_oid = slot.db_oid;
        claim.target.user_relid = slot.user_relid;
        claim.target.tablespace_oid = slot.tablespace_oid;
        claim.target.main_relfilenode = slot.main_relfilenode;
        claim.target.column_attno = slot.column_attno;
        claim.target.bucket_idx = slot.bucket_idx;
        claim.target.shard_idx = slot.shard_idx;
        claim.target.sequence_rowno = slot.sequence_rowno;
        claim.dsm = slot.dsm;
        claim.payload_bytes = slot.payload_bytes;
        return claim;
    }
    return std::nullopt;
}

void finish_persist_claim(const PersistClaim& claim) noexcept {
    remove_file_payload_if_present_noexcept(claim.dsm);
    if (g_shared == nullptr || claim.slot_index >= BioseqSchedulerShared::kPersistQueueSize) {
        release_pinned_dsm(claim.dsm);
        return;
    }
    PgPersistQueueSlot& slot = g_shared->persist_queue[claim.slot_index];
    release_pinned_dsm(claim.dsm);
    slot.dsm = DSM_HANDLE_INVALID;
    slot.payload_bytes = 0;
    slot.sequence_rowno = 0;
    pg_atomic_fetch_add_u32(&slot.generation, 1);
    pg_atomic_write_u32(&slot.state, 0);
}

[[nodiscard]] bool worker_target_accepts_index_persist(
        const PersistTarget& target,
        std::string_view index_name) {
    Relation rel = try_safe_relation_open(target.user_relid, AccessShareLock);
    if (rel == nullptr) {
        return false;
    }
    const Oid tablespace = OidIsValid(rel->rd_rel->reltablespace)
        ? rel->rd_rel->reltablespace
        : MyDatabaseTableSpace;
    Oid relfilenode = static_cast<Oid>(rel->rd_locator.relNumber);
    if (!OidIsValid(relfilenode)) {
        relfilenode = RelationGetRelid(rel);
    }
    relation_close(rel, AccessShareLock);
    if (tablespace != target.tablespace_oid
        || relfilenode != target.main_relfilenode) {
        return false;
    }

    bool enabled = true;
    bool update_on_insert =
        bioseq::indexing::default_update_on_insert_contains(index_name);
    if (SPI_connect() == SPI_OK_CONNECT) {
        const std::string sql = fmt::format(
            "SELECT enabled, update_on_insert FROM bioseq_internal.index_policy "
            "WHERE user_relid={}::oid AND main_relfilenode={}::oid "
            "AND column_attno={} AND index_name={} LIMIT 1",
            static_cast<unsigned>(target.user_relid),
            static_cast<unsigned>(target.main_relfilenode),
            static_cast<int>(target.column_attno),
            quote_literal_cstr(std::string(index_name).c_str()));
        if (SPI_execute(sql.c_str(), true, 0) == SPI_OK_SELECT && SPI_processed > 0) {
            bool isnull = false;
            enabled = DatumGetBool(SPI_getbinval(
                SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull));
            update_on_insert = DatumGetBool(SPI_getbinval(
                SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull));
        }
        SPI_finish();
    }
    return enabled && update_on_insert;
}

void process_persist_file_claim(
        const PersistClaim& claim,
        const char* base,
        Size mapped) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const std::optional<PersistFileMetadata> meta =
        parse_persist_file_metadata(base, mapped);
    if (!meta.has_value()) {
        JustLog("[bioseq] index: discarded invalid file async persist metadata row={}",
                static_cast<unsigned long long>(claim.target.sequence_rowno));
        return;
    }

    PersistTarget target = claim.target;
    target.index_name = meta->index_name;
    if (!worker_target_accepts_index_persist(target, meta->index_name)) {
        remove_temp_file_noexcept(meta->path);
        JustLog("[bioseq] index: discarded stale file async persist job row={} index={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                meta->index_name);
        return;
    }

    std::ifstream input(meta->path, std::ios::binary);
    if (!input) {
        remove_temp_file_noexcept(meta->path);
        JustLog("[bioseq] index: discarded missing file async persist payload row={} index={} path={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                meta->index_name,
                meta->path);
        return;
    }

    std::vector<char> payload;
    std::uint64_t bytes_read = 0;
    std::uint64_t chunks_written = 0;
    for (std::uint64_t i = 0; i < meta->chunk_count; ++i) {
        PersistFileChunkHeader chunk{};
        input.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
        if (!input) {
            pg_log(ERROR,
                   "bioseq async persist: truncated file chunk header at chunk {}",
                   static_cast<unsigned long long>(i));
        }
        bytes_read += sizeof(chunk);
        if (chunk.size > bioseq::indexing::MAX_INDEX_CHUNK_SIZE_BYTES) {
            pg_log(ERROR,
                   "bioseq async persist: file chunk too large (sub_key={} chunk={} bytes={})",
                   static_cast<long long>(chunk.sub_key),
                   static_cast<unsigned>(chunk.chunk_no),
                   static_cast<unsigned long long>(chunk.size));
        }

        payload.assign(static_cast<std::size_t>(chunk.size), char{0});
        if (chunk.size != 0) {
            input.read(payload.data(), static_cast<std::streamsize>(chunk.size));
            if (!input) {
                pg_log(ERROR,
                       "bioseq async persist: truncated file chunk payload at chunk {}",
                       static_cast<unsigned long long>(i));
            }
        }
        bytes_read += chunk.size;
        bioseq::storage::indexing::write_serialized_index_chunk_pg(
            target.user_relid,
            target.db_oid,
            target.tablespace_oid,
            target.main_relfilenode,
            target.column_attno,
            meta->index_name,
            target.bucket_idx,
            target.shard_idx,
            target.sequence_rowno,
            static_cast<bioseq::indexing::index_sub_key_t>(chunk.sub_key),
            chunk.chunk_no,
            std::span<const char>(payload.data(), payload.size()));
        ++chunks_written;
    }
    if (bytes_read != meta->file_bytes) {
        pg_log(ERROR,
               "bioseq async persist: file byte count mismatch (expected={} actual={})",
               static_cast<unsigned long long>(meta->file_bytes),
               static_cast<unsigned long long>(bytes_read));
    }
    bioseq::storage::indexing::mark_serialized_index_write_successful_pg(
        target.user_relid,
        target.main_relfilenode,
        target.column_attno,
        meta->index_name);
    input.close();
    remove_temp_file_noexcept(meta->path);

    const std::uint64_t worker_end_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    JustLog("[bioseq] index: async persist file worker wrote row={} index={} chunks={} bytes={} worker_write_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            meta->index_name,
            static_cast<unsigned long long>(chunks_written),
            static_cast<unsigned long long>(meta->file_bytes),
            static_cast<unsigned long long>(worker_end_ns - worker_start_ns));
}

void process_persist_claim(const PersistClaim& claim) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    StartTransactionCommand();
    PushActiveSnapshot(GetTransactionSnapshot());

    dsm_segment* seg = dsm_attach(claim.dsm);
    if (seg == nullptr) {
        PopActiveSnapshot();
        CommitTransactionCommand();
        return;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    const Size mapped = dsm_segment_map_length(seg);
    if (mapped >= sizeof(std::uint32_t)) {
        std::uint32_t magic = 0;
        std::memcpy(&magic, base, sizeof(magic));
        if (magic == kIndexPersistFileDsmMagic) {
            process_persist_file_claim(claim, base, mapped);
            dsm_detach(seg);
            PopActiveSnapshot();
            CommitTransactionCommand();
            return;
        }
    }
    if (mapped < sizeof(IndexPersistDsmHeader)) {
        dsm_detach(seg);
        PopActiveSnapshot();
        CommitTransactionCommand();
        return;
    }
    IndexPersistDsmHeader header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != kIndexPersistDsmMagic
        || header.version != kIndexPersistDsmVersion
        || header.payload_bytes > mapped
        || header.index_name_len == 0) {
        dsm_detach(seg);
        PopActiveSnapshot();
        CommitTransactionCommand();
        return;
    }
    const std::uint64_t directory_offset = align_up_u64(
        sizeof(IndexPersistDsmHeader) + header.index_name_len,
        alignof(IndexPersistDsmChunk));
    const std::uint64_t directory_bytes =
        static_cast<std::uint64_t>(header.chunk_count) * sizeof(IndexPersistDsmChunk);
    if (directory_offset > header.payload_bytes
        || directory_bytes > header.payload_bytes - directory_offset) {
        dsm_detach(seg);
        PopActiveSnapshot();
        CommitTransactionCommand();
        return;
    }
    std::string index_name(
        base + sizeof(IndexPersistDsmHeader),
        base + sizeof(IndexPersistDsmHeader) + header.index_name_len);
    PersistTarget target = claim.target;
    target.index_name = index_name;
    if (!worker_target_accepts_index_persist(target, index_name)) {
        dsm_detach(seg);
        PopActiveSnapshot();
        CommitTransactionCommand();
        JustLog("[bioseq] index: discarded stale async persist job row={} index={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                index_name);
        return;
    }

    const auto* directory =
        reinterpret_cast<const IndexPersistDsmChunk*>(base + directory_offset);
    for (std::uint32_t i = 0; i < header.chunk_count; ++i) {
        const IndexPersistDsmChunk& chunk = directory[i];
        if (chunk.offset > header.payload_bytes
            || chunk.size > header.payload_bytes - chunk.offset) {
            dsm_detach(seg);
            PopActiveSnapshot();
            CommitTransactionCommand();
            return;
        }
        bioseq::storage::indexing::write_serialized_index_chunk_pg(
            target.user_relid,
            target.db_oid,
            target.tablespace_oid,
            target.main_relfilenode,
            target.column_attno,
            index_name,
            target.bucket_idx,
            target.shard_idx,
            target.sequence_rowno,
            static_cast<bioseq::indexing::index_sub_key_t>(chunk.sub_key),
            chunk.chunk_no,
            std::span<const char>(base + chunk.offset, static_cast<std::size_t>(chunk.size)));
    }
    bioseq::storage::indexing::mark_serialized_index_write_successful_pg(
        target.user_relid,
        target.main_relfilenode,
        target.column_attno,
        index_name);
    dsm_detach(seg);
    PopActiveSnapshot();
    CommitTransactionCommand();
    const std::uint64_t worker_end_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    JustLog("[bioseq] index: async persist worker wrote row={} index={} chunks={} bytes={} worker_write_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            index_name,
            header.chunk_count,
            static_cast<unsigned long long>(header.payload_bytes),
            static_cast<unsigned long long>(worker_end_ns - worker_start_ns));
}

inline Oid g_persist_worker_connected_db = InvalidOid;

void process_one_persist_job_if_ready() {
    auto claim = claim_persist_job();
    if (!claim) {
        return;
    }
    if (!OidIsValid(claim->target.db_oid)
        || claim->dsm == DSM_HANDLE_INVALID) {
        finish_persist_claim(*claim);
        return;
    }
    if (!OidIsValid(g_persist_worker_connected_db)) {
        BackgroundWorkerInitializeConnectionByOid(claim->target.db_oid, InvalidOid, 0);
        g_persist_worker_connected_db = claim->target.db_oid;
    } else if (g_persist_worker_connected_db != claim->target.db_oid) {
        JustLog("[bioseq] index: discarded async persist job for db={} because worker is connected to db={}",
                static_cast<unsigned>(claim->target.db_oid),
                static_cast<unsigned>(g_persist_worker_connected_db));
        finish_persist_claim(*claim);
        return;
    }

    PG_TRY();
    {
        process_persist_claim(*claim);
    }
    PG_CATCH();
    {
        AbortCurrentTransaction();
        FlushErrorState();
        JustLog("[bioseq] index: async persist worker dropped failed job row={}",
                static_cast<unsigned long long>(claim->target.sequence_rowno));
    }
    PG_END_TRY();
    finish_persist_claim(*claim);
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
        &g_shared->cfg_max_concurrency,
        static_cast<std::uint32_t>(std::max(0, bioseq_scheduler_max_concurrency)));
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
                    std::max(0, bioseq_scheduler_max_concurrency)));
            pg_atomic_fetch_add_u32(&g_shared->cfg_generation, 1);
        }

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
    proc_exit(0);
}

// DOC0301 iteration 2 — one-shot dynamic background worker entry. Builds +
// persists the minimizers_per_sequence index for one user relation off the
// foreground read backend, then exits. `arg` packs (db_oid << 32 | relid).
// Routes the build through the SQL executor (SELECT bioseq.update_index(...))
// rather than calling update_index_impl() directly, which is the proven-clean
// path (DOC0286: a direct executor-frame call leaks relcache/typcache churn).
extern "C" PGDLLEXPORT void bioseq_mps_async_build_worker_main(Datum arg) {
    pqsignal(SIGTERM, die);
    BackgroundWorkerUnblockSignals();

    const std::uint64_t packed = DatumGetUInt64(arg);
    const Oid db_oid = static_cast<Oid>(static_cast<std::uint32_t>(packed >> 32));
    const Oid relid =
        static_cast<Oid>(static_cast<std::uint32_t>(packed & 0xFFFFFFFFu));

    BackgroundWorkerInitializeConnectionByOid(db_oid, InvalidOid, 0);

    const std::uint64_t t0 =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    bool ok = false;
    PG_TRY();
    {
        StartTransactionCommand();
        PushActiveSnapshot(GetTransactionSnapshot());
        if (SPI_connect() != SPI_OK_CONNECT) {
            elog(ERROR, "bioseq mps build worker: SPI_connect failed");
        }
        // DOC0301 iteration 3 — in-flight dedup: take a session advisory lock keyed
        // by relid; if another build worker already holds it, skip the rebuild
        // (CASE only evaluates update_index when the try-lock succeeds). This bounds
        // redundant whole-table rebuilds from repeated/multi-row recompute spawns to
        // one fast no-op each instead of N serialized full rebuilds. The session
        // lock auto-releases when this one-shot worker exits.
        char sql[256];
        snprintf(sql, sizeof(sql),
                 "SELECT CASE WHEN pg_try_advisory_lock(%u::bigint) "
                 "THEN bioseq.update_index(%u::oid::regclass, "
                 "'minimizers_per_sequence') ELSE -1 END",
                 static_cast<unsigned>(relid), static_cast<unsigned>(relid));
        const int rc = SPI_execute(sql, false, 0);
        SPI_finish();
        PopActiveSnapshot();
        CommitTransactionCommand();
        ok = (rc == SPI_OK_SELECT);
    }
    PG_CATCH();
    {
        AbortCurrentTransaction();
        FlushErrorState();
        ok = false;
    }
    PG_END_TRY();
    const std::uint64_t t1 =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    JustLog("[bioseq] index: async mps build worker finished db={} relid={} "
            "ok={} build_ns={}",
            static_cast<unsigned>(db_oid), static_cast<unsigned>(relid), ok,
            static_cast<unsigned long long>(t1 - t0));
    proc_exit(0);
}
