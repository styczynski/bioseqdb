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
#include <utility>
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
import bioseq_indexing.minimizer_sequence;
import bioseq_storage;
import bioseq_storage_lazy;
import bioseq_storage_scanner_pg;
import bioseq_types.pg_compact_sequence;
import bioseq_sequences;
import bioseq_storage_indexing_pg;
import bioseq_storage_indexing_handles;

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

struct IndexBuildMpsDsmHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t index_name_len;
    std::uint8_t alphabet_code;
    std::uint8_t storage_unit_byte_width;
    std::uint16_t last_fragment_padding;
    std::int32_t pair_sort_impl;
    std::int32_t pipeline_mode;
    std::int32_t max_concurrency;
    std::int32_t index_memory_cap_mb;
    std::uint32_t reserved;
    bioseq::storage::seq_locator locator;
};
static_assert(sizeof(IndexBuildMpsDsmHeader) == 72);

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
inline constexpr std::uint32_t kIndexBuildMpsDsmMagic = 0xB1051D5Au;
inline constexpr std::uint32_t kIndexBuildMpsDsmVersion = 2;

struct PgMpsBuildStats {
    pg_atomic_uint64 generation;
    pg_atomic_uint64 backend_pid;
    pg_atomic_uint64 db_oid;
    pg_atomic_uint64 relfilenode;
    pg_atomic_uint64 sequence_rowno;
    pg_atomic_uint64 total_run_count;
    pg_atomic_uint64 resident_runs;
    pg_atomic_uint64 file_runs;
    pg_atomic_uint64 resident_run_bytes;
    pg_atomic_uint64 resident_peak_bytes;
    pg_atomic_uint64 spilled_run_bytes;
    pg_atomic_uint64 postgres_temp_file_bytes;
    pg_atomic_uint64 postgres_temp_file_count;
    pg_atomic_uint64 merge_reader_buffer_bytes;
    pg_atomic_uint64 merge_pass_count;
    pg_atomic_uint64 spill_count_from_pressure;
    pg_atomic_uint64 resident_demotions;
    pg_atomic_uint64 position_spill_bytes;
    pg_atomic_uint64 position_spill_count;
    pg_atomic_uint64 worker_build_ns;
    pg_atomic_uint64 pipeline_mode;
    pg_atomic_uint64 max_concurrency;
    pg_atomic_uint64 index_memory_cap_mb;
    pg_atomic_uint64 lazy_worker_shareable;
};

struct BioseqSchedulerShared {
    pg_atomic_uint32 cfg_max_concurrency;
    pg_atomic_uint32 cfg_generation;
    pg_atomic_uint32 state;
    PgSchedulerStats stats;
    PgMpsBuildStats mps_build_stats;
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

void init_mps_build_stats(PgMpsBuildStats& stats) {
    pg_atomic_init_u64(&stats.generation, 0);
    pg_atomic_init_u64(&stats.backend_pid, 0);
    pg_atomic_init_u64(&stats.db_oid, 0);
    pg_atomic_init_u64(&stats.relfilenode, 0);
    pg_atomic_init_u64(&stats.sequence_rowno, 0);
    pg_atomic_init_u64(&stats.total_run_count, 0);
    pg_atomic_init_u64(&stats.resident_runs, 0);
    pg_atomic_init_u64(&stats.file_runs, 0);
    pg_atomic_init_u64(&stats.resident_run_bytes, 0);
    pg_atomic_init_u64(&stats.resident_peak_bytes, 0);
    pg_atomic_init_u64(&stats.spilled_run_bytes, 0);
    pg_atomic_init_u64(&stats.postgres_temp_file_bytes, 0);
    pg_atomic_init_u64(&stats.postgres_temp_file_count, 0);
    pg_atomic_init_u64(&stats.merge_reader_buffer_bytes, 0);
    pg_atomic_init_u64(&stats.merge_pass_count, 0);
    pg_atomic_init_u64(&stats.spill_count_from_pressure, 0);
    pg_atomic_init_u64(&stats.resident_demotions, 0);
    pg_atomic_init_u64(&stats.position_spill_bytes, 0);
    pg_atomic_init_u64(&stats.position_spill_count, 0);
    pg_atomic_init_u64(&stats.worker_build_ns, 0);
    pg_atomic_init_u64(&stats.pipeline_mode, 0);
    pg_atomic_init_u64(&stats.max_concurrency, 0);
    pg_atomic_init_u64(&stats.index_memory_cap_mb, 0);
    pg_atomic_init_u64(&stats.lazy_worker_shareable, 0);
}

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

struct AsyncMpsBuildStatsSnapshot {
    std::uint64_t generation = 0;
    bioseq::storage::indexing::LastMpsBuildStats stats{};
    std::uint64_t worker_build_ns = 0;
    std::uint64_t pipeline_mode = 0;
    std::uint64_t max_concurrency = 0;
    std::uint64_t index_memory_cap_mb = 0;
    std::uint64_t lazy_worker_shareable = 0;
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

void publish_async_mps_build_stats(
        const bioseq::storage::indexing::LastMpsBuildStats& stats,
        std::uint64_t worker_build_ns,
        std::uint64_t pipeline_mode,
        std::uint64_t max_concurrency,
        std::uint64_t index_memory_cap_mb,
        bool lazy_worker_shareable) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    PgMpsBuildStats& dst = g_shared->mps_build_stats;
    pg_atomic_write_u64(&dst.backend_pid, stats.backend_pid);
    pg_atomic_write_u64(&dst.db_oid, stats.db_oid);
    pg_atomic_write_u64(&dst.relfilenode, stats.relfilenode);
    pg_atomic_write_u64(&dst.sequence_rowno, stats.sequence_rowno);
    pg_atomic_write_u64(&dst.total_run_count, stats.total_run_count);
    pg_atomic_write_u64(&dst.resident_runs, stats.resident_runs);
    pg_atomic_write_u64(&dst.file_runs, stats.file_runs);
    pg_atomic_write_u64(&dst.resident_run_bytes, stats.resident_run_bytes);
    pg_atomic_write_u64(&dst.resident_peak_bytes, stats.resident_peak_bytes);
    pg_atomic_write_u64(&dst.spilled_run_bytes, stats.spilled_run_bytes);
    pg_atomic_write_u64(&dst.postgres_temp_file_bytes, stats.postgres_temp_file_bytes);
    pg_atomic_write_u64(&dst.postgres_temp_file_count, stats.postgres_temp_file_count);
    pg_atomic_write_u64(&dst.merge_reader_buffer_bytes, stats.merge_reader_buffer_bytes);
    pg_atomic_write_u64(&dst.merge_pass_count, stats.merge_pass_count);
    pg_atomic_write_u64(&dst.spill_count_from_pressure, stats.spill_count_from_pressure);
    pg_atomic_write_u64(&dst.resident_demotions, stats.resident_demotions);
    pg_atomic_write_u64(&dst.position_spill_bytes, stats.position_spill_bytes);
    pg_atomic_write_u64(&dst.position_spill_count, stats.position_spill_count);
    pg_atomic_write_u64(&dst.worker_build_ns, worker_build_ns);
    pg_atomic_write_u64(&dst.pipeline_mode, pipeline_mode);
    pg_atomic_write_u64(&dst.max_concurrency, max_concurrency);
    pg_atomic_write_u64(&dst.index_memory_cap_mb, index_memory_cap_mb);
    pg_atomic_write_u64(&dst.lazy_worker_shareable, lazy_worker_shareable ? 1u : 0u);
    pg_atomic_fetch_add_u64(&dst.generation, 1);
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

struct MpsBuildMetadata {
    std::string index_name{};
    bioseq::storage::seq_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t last_fragment_padding = 0;
    std::int32_t pair_sort_impl = 0;
    std::int32_t pipeline_mode = 0;
    std::int32_t max_concurrency = 0;
    std::int32_t index_memory_cap_mb = 0;
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

[[nodiscard]] std::optional<MpsBuildMetadata>
parse_mps_build_metadata(const char* base, Size mapped) {
    if (mapped < sizeof(IndexBuildMpsDsmHeader)) {
        return std::nullopt;
    }
    IndexBuildMpsDsmHeader header{};
    std::memcpy(&header, base, sizeof(header));
    if (header.magic != kIndexBuildMpsDsmMagic
        || header.version != kIndexBuildMpsDsmVersion
        || header.index_name_len == 0
        || header.locator.version != bioseq::storage::kLocatorVersion) {
        return std::nullopt;
    }
    const std::uint64_t name_offset = sizeof(IndexBuildMpsDsmHeader);
    const std::uint64_t name_end = name_offset + header.index_name_len;
    if (name_end < name_offset || name_end > static_cast<std::uint64_t>(mapped)) {
        return std::nullopt;
    }
    MpsBuildMetadata meta{};
    meta.index_name.assign(base + name_offset, base + name_end);
    meta.locator = header.locator;
    meta.alphabet_code = header.alphabet_code;
    meta.storage_unit_byte_width = header.storage_unit_byte_width;
    meta.last_fragment_padding = header.last_fragment_padding;
    meta.pair_sort_impl = header.pair_sort_impl;
    meta.pipeline_mode = header.pipeline_mode;
    meta.max_concurrency = header.max_concurrency;
    meta.index_memory_cap_mb = header.index_memory_cap_mb;
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
                JustLog("[bioseq] index: async persist publish failed row={} index={} dsm={} shared_queue_available={}",
                        static_cast<unsigned long long>(job.target.sequence_rowno),
                        job.index_name,
                        static_cast<unsigned>(job.dsm),
                        g_shared != nullptr);
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

[[nodiscard]] bool async_persist_queue_available() noexcept {
    return g_shared != nullptr;
}

[[nodiscard]] bool stage_async_index_persist(
        const PersistTarget& target,
        std::span<const PersistChunk> chunks) {
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq_index_async_persist || chunks.empty() || target.index_name.empty()) {
        return false;
    }
    if (!async_persist_queue_available()) {
        JustLog("[bioseq] index: async persist skipped row={} index={} because shared supervisor queue is unavailable",
                static_cast<unsigned long long>(target.sequence_rowno),
                target.index_name);
        return false;
    }
    ensure_persist_xact_callback_registered();
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
    if (!bioseq_index_async_persist
        || target.index_name.empty()
        || temp_file_path.empty()
        || chunk_count == 0) {
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    if (!async_persist_queue_available()) {
        JustLog("[bioseq] index: file async persist skipped row={} index={} chunks={} file_bytes={} because shared supervisor queue is unavailable",
                static_cast<unsigned long long>(target.sequence_rowno),
                target.index_name,
                static_cast<unsigned long long>(chunk_count),
                static_cast<unsigned long long>(file_bytes));
        remove_temp_file_noexcept(temp_file_path);
        return false;
    }
    ensure_persist_xact_callback_registered();

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

[[nodiscard]] bool stage_async_mps_build(
        const PersistTarget& target,
        const bioseq::storage::seq_locator& locator,
        std::uint8_t alphabet_code,
        std::uint8_t storage_unit_byte_width,
        std::uint16_t last_fragment_padding,
        std::int32_t pair_sort_impl,
        std::int32_t pipeline_mode,
        std::int32_t max_concurrency,
        std::int32_t index_memory_cap_mb) {
    const std::uint64_t stage_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    if (!bioseq_index_async_persist
        || target.index_name.empty()
        || locator.version != bioseq::storage::kLocatorVersion
        || locator.num_fragments == 0
        || locator.total_bp_size == 0) {
        return false;
    }
    if (!async_persist_queue_available()) {
        JustLog("[bioseq] index: async mps build skipped row={} index={} target_bp={} fragments={} because shared supervisor queue is unavailable",
                static_cast<unsigned long long>(target.sequence_rowno),
                target.index_name,
                static_cast<unsigned long long>(locator.total_bp_size),
                static_cast<unsigned long long>(locator.num_fragments));
        return false;
    }
    ensure_persist_xact_callback_registered();

    const std::uint64_t bytes_total =
        sizeof(IndexBuildMpsDsmHeader) + target.index_name.size();
    if (bytes_total == 0
        || bytes_total
            > static_cast<std::uint64_t>((std::numeric_limits<Size>::max)())) {
        return false;
    }

    dsm_segment* seg = dsm_create(
        static_cast<Size>(bytes_total),
        DSM_CREATE_NULL_IF_MAXSEGMENTS);
    if (seg == nullptr) {
        return false;
    }
    char* base = static_cast<char*>(dsm_segment_address(seg));
    IndexBuildMpsDsmHeader header{
        .magic = kIndexBuildMpsDsmMagic,
        .version = kIndexBuildMpsDsmVersion,
        .index_name_len = static_cast<std::uint32_t>(target.index_name.size()),
        .alphabet_code = alphabet_code,
        .storage_unit_byte_width = storage_unit_byte_width,
        .last_fragment_padding = last_fragment_padding,
        .pair_sort_impl = pair_sort_impl,
        .pipeline_mode = pipeline_mode,
        .max_concurrency = max_concurrency,
        .index_memory_cap_mb = index_memory_cap_mb,
        .reserved = 0,
        .locator = locator,
    };
    std::memcpy(base, &header, sizeof(header));
    std::memcpy(base + sizeof(header), target.index_name.data(), target.index_name.size());
    dsm_pin_segment(seg);
    const dsm_handle handle = dsm_segment_handle(seg);
    dsm_detach(seg);

    PendingPersistJob job{};
    job.target = target;
    job.index_name.assign(target.index_name.begin(), target.index_name.end());
    job.target.index_name = job.index_name;
    job.dsm = handle;
    job.payload_bytes = bytes_total;
    pending_persist_jobs().push_back(std::move(job));
    const std::uint64_t queued_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    JustLog("[bioseq] index: staged async mps build job row={} index={} target_bp={} fragments={} mode={} max_concurrency={} index_memory_cap_mb={} meta_bytes={} total_stage_ns={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            pending_persist_jobs().back().index_name,
            static_cast<unsigned long long>(locator.total_bp_size),
            static_cast<unsigned long long>(locator.num_fragments),
            pipeline_mode,
            max_concurrency,
            index_memory_cap_mb,
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

[[nodiscard]] AsyncMpsBuildStatsSnapshot async_mps_build_stats_snapshot() {
    AsyncMpsBuildStatsSnapshot out{};
    if (g_shared == nullptr) {
        return out;
    }
    PgMpsBuildStats& src = g_shared->mps_build_stats;
    out.generation = pg_atomic_read_u64(&src.generation);
    out.stats.backend_pid = pg_atomic_read_u64(&src.backend_pid);
    out.stats.db_oid = pg_atomic_read_u64(&src.db_oid);
    out.stats.relfilenode = pg_atomic_read_u64(&src.relfilenode);
    out.stats.sequence_rowno = pg_atomic_read_u64(&src.sequence_rowno);
    out.stats.total_run_count = pg_atomic_read_u64(&src.total_run_count);
    out.stats.resident_runs = pg_atomic_read_u64(&src.resident_runs);
    out.stats.file_runs = pg_atomic_read_u64(&src.file_runs);
    out.stats.resident_run_bytes = pg_atomic_read_u64(&src.resident_run_bytes);
    out.stats.resident_peak_bytes = pg_atomic_read_u64(&src.resident_peak_bytes);
    out.stats.spilled_run_bytes = pg_atomic_read_u64(&src.spilled_run_bytes);
    out.stats.postgres_temp_file_bytes = pg_atomic_read_u64(&src.postgres_temp_file_bytes);
    out.stats.postgres_temp_file_count = pg_atomic_read_u64(&src.postgres_temp_file_count);
    out.stats.merge_reader_buffer_bytes = pg_atomic_read_u64(&src.merge_reader_buffer_bytes);
    out.stats.merge_pass_count = pg_atomic_read_u64(&src.merge_pass_count);
    out.stats.spill_count_from_pressure = pg_atomic_read_u64(&src.spill_count_from_pressure);
    out.stats.resident_demotions = pg_atomic_read_u64(&src.resident_demotions);
    out.stats.position_spill_bytes = pg_atomic_read_u64(&src.position_spill_bytes);
    out.stats.position_spill_count = pg_atomic_read_u64(&src.position_spill_count);
    out.worker_build_ns = pg_atomic_read_u64(&src.worker_build_ns);
    out.pipeline_mode = pg_atomic_read_u64(&src.pipeline_mode);
    out.max_concurrency = pg_atomic_read_u64(&src.max_concurrency);
    out.index_memory_cap_mb = pg_atomic_read_u64(&src.index_memory_cap_mb);
    out.lazy_worker_shareable = pg_atomic_read_u64(&src.lazy_worker_shareable);
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

template <class R>
struct AsyncMpsResidentTarget {
    using traits = typename compact_sequence_range<R>::traits;
    using storage_unit = typename traits::storage_unit;
    using resident_config = sequence_custom_alphabet<
        typename traits::alphabet_type,
        storage_unit,
        traits::bits_per_char>;
    using resident_range = compact_sequence_range<resident_config>;

    bioseq::parallelism::MemoryPermit permit;
    resident_range range;
    std::uint64_t packed_bytes = 0;
};

template <class R, class Scanner>
[[nodiscard]] std::optional<AsyncMpsResidentTarget<R>>
try_materialize_async_mps_worker_target(
        const bioseq::storage::lazy_compact_sequence<R, Scanner>& seq,
        std::uint64_t sequence_rowno,
        std::string_view index_name) {
    if constexpr (!Scanner::supports_fragments()) {
        return std::nullopt;
    } else {
        auto& pool = bioseq::parallelism::GlobalPool::instance();
        if (!bioseq::parallelism::task_parallel(pool.mode())
            || pool.max_concurrency() <= 1) {
            return std::nullopt;
        }

        using resident_t = AsyncMpsResidentTarget<R>;
        using storage_unit = typename resident_t::storage_unit;
        const std::uint64_t total_units64 =
            static_cast<std::uint64_t>(seq.size());
        if (total_units64
            > static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)())) {
            pg_log(ERROR,
                   "bioseq async mps build: row {} index {} has too many "
                   "packed units to materialize in one worker",
                   static_cast<unsigned long long>(sequence_rowno),
                   std::string(index_name).c_str());
        }
        const std::size_t total_units =
            static_cast<std::size_t>(total_units64);
        const std::uint64_t packed_bytes =
            bioseq::parallelism::saturating_mul(
                total_units64,
                static_cast<std::uint64_t>(sizeof(storage_unit)));
        constexpr std::uint64_t context_reserve_bytes = std::uint64_t{8} << 20;
        const std::uint64_t reserve_bytes =
            packed_bytes
                    > (std::numeric_limits<std::uint64_t>::max)()
                        - context_reserve_bytes
                ? (std::numeric_limits<std::uint64_t>::max)()
                : packed_bytes + context_reserve_bytes;

        auto permit =
            bioseq::parallelism::MemoryAdmissionController::instance()
                .try_reserve(reserve_bytes);
        if (!permit) {
            JustLog("[bioseq] index: async mps worker row={} index={} target_bp={} "
                    "fragments={} packed_bytes={} not admitted for resident "
                    "materialization; using scanner_pg",
                    static_cast<unsigned long long>(sequence_rowno),
                    index_name,
                    static_cast<unsigned long long>(seq.bp_size()),
                    seq.number_of_fragments(),
                    static_cast<unsigned long long>(packed_bytes));
            return std::nullopt;
        }

        typename resident_t::resident_range::base_container packed(total_units);
        const std::size_t fragment_units = Scanner::fragment_storage_units;
        for (std::size_t fragment_no = 0;
             fragment_no < seq.number_of_fragments();
             ++fragment_no) {
            if ((fragment_no & std::size_t{0x3ff}) == 0) {
                CHECK_FOR_INTERRUPTS();
            }
            const std::span<const storage_unit> fragment =
                seq.scanner().read_fragment(fragment_no);
            const std::size_t dst_offset = fragment_no * fragment_units;
            if (dst_offset >= packed.size()) {
                seq.scanner().release_fragment(fragment_no);
                break;
            }
            const std::size_t copy_units =
                std::min(fragment.size(), packed.size() - dst_offset);
            std::copy_n(fragment.data(), copy_units, packed.data() + dst_offset);
            seq.scanner().release_fragment(fragment_no);
        }

        JustLog("[bioseq] index: async mps worker row={} index={} target_bp={} "
                "fragments={} packed_bytes={} materialized resident target",
                static_cast<unsigned long long>(sequence_rowno),
                index_name,
                static_cast<unsigned long long>(seq.bp_size()),
                seq.number_of_fragments(),
                static_cast<unsigned long long>(packed_bytes));

        return resident_t{
            std::move(*permit),
            resident_t::resident_range::from_packed(
                std::move(packed), seq.padding()),
            packed_bytes};
    }
}

void process_mps_build_claim(
        const PersistClaim& claim,
        const char* base,
        Size mapped) {
    const std::uint64_t worker_start_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const std::optional<MpsBuildMetadata> meta =
        parse_mps_build_metadata(base, mapped);
    if (!meta.has_value()) {
        JustLog("[bioseq] index: discarded invalid async mps build metadata row={}",
                static_cast<unsigned long long>(claim.target.sequence_rowno));
        return;
    }

    PersistTarget target = claim.target;
    target.index_name = meta->index_name;
    if (!worker_target_accepts_index_persist(target, meta->index_name)) {
        JustLog("[bioseq] index: discarded stale async mps build job row={} index={}",
                static_cast<unsigned long long>(target.sequence_rowno),
                meta->index_name);
        return;
    }

    MemoryContext work = AllocSetContextCreate(
        CurrentMemoryContext,
        "bioseq_async_mps_build",
        ALLOCSET_DEFAULT_SIZES);
    bioseq::storage::seq_caches* caches = bioseq::storage::alloc_seq_caches(work);
    caches->side = bioseq::storage::seq_caches::Side::Read;
    caches->lock_mode = AccessShareLock;
    caches->read_state_mctx = work;
    caches->main_database_oid = target.db_oid;
    caches->main_tablespace_oid = target.tablespace_oid;
    caches->main_relfilenode = target.main_relfilenode;

    const auto previous_sort_impl =
        bioseq::indexing::detail::pair_run_sort_impl();
    bioseq::indexing::detail::set_pair_run_sort_impl(
        static_cast<bioseq::indexing::detail::PairRunSortImpl>(
            meta->pair_sort_impl));
    const auto worker_mode =
        bioseq::parallelism::mode_from_int(meta->pipeline_mode);
    const unsigned worker_concurrency = effective_max_concurrency(
        static_cast<unsigned>(std::max(0, meta->max_concurrency)));
    bioseq::parallelism::GlobalPool::instance().configure(
        bioseq::parallelism::PoolConfig{
            .max_concurrency = worker_concurrency,
            .mode = worker_mode,
        });
    const std::uint64_t worker_index_cap_bytes =
        meta->index_memory_cap_mb > 0
            ? (static_cast<std::uint64_t>(meta->index_memory_cap_mb) << 20)
            : 0;
    bioseq::parallelism::MemoryAdmissionController::instance()
        .set_index_memory_cap_bytes(worker_index_cap_bytes);

    bool built = false;
    bool lazy_worker_shareable = false;
    JustLog("[bioseq] index: async mps build worker starting row={} index={} target_bp={} mode={} max_concurrency={} index_memory_cap_mb={} sort_impl={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            meta->index_name,
            static_cast<unsigned long long>(meta->locator.total_bp_size),
            std::string_view(bioseq::parallelism::to_string(worker_mode)),
            static_cast<unsigned long long>(worker_concurrency),
            meta->index_memory_cap_mb,
            meta->pair_sort_impl);
    PG_TRY();
    {
        pgseq_t::dispatch_instance(
            meta->alphabet_code,
            meta->storage_unit_byte_width,
            [&]<class S>() {
                auto store = bioseq::storage::indexing::open_fragment_storage(
                    target.user_relid,
                    target.db_oid,
                    target.tablespace_oid,
                    target.main_relfilenode,
                    target.column_attno,
                    meta->index_name,
                    meta->locator,
                    target.sequence_rowno,
                    caches,
                    work);
                bioseq::storage::with_lazy_postgres<S>(
                    target.db_oid,
                    target.tablespace_oid,
                    target.main_relfilenode,
                    target.column_attno,
                    meta->locator,
                    meta->last_fragment_padding,
                    caches,
                    work,
                    [&](auto& lazy) {
                        const bool pg_lazy_worker_shareable =
                            bioseq::storage::lazy_is_worker_shareable(lazy);
                        lazy_worker_shareable = pg_lazy_worker_shareable;
                        if (!pg_lazy_worker_shareable) {
                            if (auto resident =
                                    try_materialize_async_mps_worker_target(
                                        lazy,
                                        target.sequence_rowno,
                                        meta->index_name);
                                resident.has_value()) {
                                auto resident_lazy =
                                    bioseq::storage::make_lazy_in_memory(
                                        resident->range);
                                lazy_worker_shareable =
                                    bioseq::storage::lazy_is_worker_shareable(
                                        resident_lazy);
                                JustLog("[bioseq] index: async mps worker row={} index={} "
                                        "using resident target packed_bytes={} "
                                        "lazy_worker_shareable={}",
                                        static_cast<unsigned long long>(
                                            target.sequence_rowno),
                                        meta->index_name,
                                        static_cast<unsigned long long>(
                                            resident->packed_bytes),
                                        lazy_worker_shareable);
                                bioseq::indexing::MinimizerSequenceMergedIndex<
                                    bioseq::storage::indexing::IndexStorageFragmentPg>::rebuild(
                                        store, resident_lazy);
                                return;
                            }
                        }
                        JustLog("[bioseq] index: async mps worker row={} index={} "
                                "using scanner_pg target lazy_worker_shareable={}",
                                static_cast<unsigned long long>(
                                    target.sequence_rowno),
                                meta->index_name,
                                lazy_worker_shareable);
                        bioseq::indexing::MinimizerSequenceMergedIndex<
                            bioseq::storage::indexing::IndexStorageFragmentPg>::rebuild(
                                store, lazy);
                    });
            });
        built = true;
    }
    PG_CATCH();
    {
        bioseq::indexing::detail::set_pair_run_sort_impl(previous_sort_impl);
        caches->close_all(/*commit=*/false);
        bioseq::storage::destroy_seq_caches(caches);
        MemoryContextDelete(work);
        PG_RE_THROW();
    }
    PG_END_TRY();

    bioseq::indexing::detail::set_pair_run_sort_impl(previous_sort_impl);
    caches->close_all(/*commit=*/false);
    bioseq::storage::destroy_seq_caches(caches);
    MemoryContextDelete(work);

    if (built) {
        bioseq::storage::indexing::mark_serialized_index_write_successful_pg(
            target.user_relid,
            target.main_relfilenode,
            target.column_attno,
            meta->index_name);
    }

    const std::uint64_t worker_end_ns =
        static_cast<std::uint64_t>(bioseq::instr::clock::now_ns());
    const auto stats = bioseq::storage::indexing::g_last_mps_build_stats;
    publish_async_mps_build_stats(
        stats,
        worker_end_ns - worker_start_ns,
        static_cast<std::uint64_t>(worker_mode),
        static_cast<std::uint64_t>(worker_concurrency),
        static_cast<std::uint64_t>(std::max(0, meta->index_memory_cap_mb)),
        lazy_worker_shareable);
    JustLog("[bioseq] index: async mps build worker built row={} index={} target_bp={} worker_build_ns={} mode={} max_concurrency={} lazy_worker_shareable={} runs={} resident_runs={} file_runs={} resident_peak_bytes={} spilled_run_bytes={} postgres_temp_file_bytes={} merge_passes={} position_spill_bytes={}",
            static_cast<unsigned long long>(target.sequence_rowno),
            meta->index_name,
            static_cast<unsigned long long>(meta->locator.total_bp_size),
            static_cast<unsigned long long>(worker_end_ns - worker_start_ns),
            std::string_view(bioseq::parallelism::to_string(worker_mode)),
            static_cast<unsigned long long>(worker_concurrency),
            lazy_worker_shareable,
            static_cast<unsigned long long>(stats.total_run_count),
            static_cast<unsigned long long>(stats.resident_runs),
            static_cast<unsigned long long>(stats.file_runs),
            static_cast<unsigned long long>(stats.resident_peak_bytes),
            static_cast<unsigned long long>(stats.spilled_run_bytes),
            static_cast<unsigned long long>(stats.postgres_temp_file_bytes),
            static_cast<unsigned long long>(stats.merge_pass_count),
            static_cast<unsigned long long>(stats.position_spill_bytes));
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
        if (magic == kIndexBuildMpsDsmMagic) {
            process_mps_build_claim(claim, base, mapped);
            dsm_detach(seg);
            PopActiveSnapshot();
            CommitTransactionCommand();
            return;
        }
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
