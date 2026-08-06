/**
 * @file supervisor_pg.cppm
 * @brief Narrow PostgreSQL-facing scheduler supervision API.
 */
module;

#include "common.h"

extern "C" {
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/condition_variable.h"
#include "storage/dsm_impl.h"
#include "utils/snapshot.h"
}

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>

export module bioseq.scheduler.supervisor_pg;

import bioseq.indexing.type;
import bioseq.sequences.kmers.highway_contract;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.sql_guc_settings;
import bioseq.storage.bucket_math;
import bioseq.storage.indexing_pg.chunked_storage;

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

struct VulkanRuntimeStatusSnapshot {
    bool available = false;
    std::string device_name{};
    std::int32_t max_workgroup_count_x = 0;
    bool scalar_block_layout = false;
    std::string initialization_error{};
    std::int64_t device_local_bytes = 0;
    bool software_device = false;
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

/**
 * One-shot PostgreSQL failure points in the asynchronous MPS worker.  The
 * selector is stored in scheduler shared memory, so a normal backend can arm
 * it for the background worker without relying on process-local test state.
 */
enum class AsyncMpsWorkerPgFailure : std::uint32_t {
    none = 0,
    relation_open = 1,
    relation_identity = 2,
    relation_close = 3,
    spi_connect = 4,
    spi_execute = 5,
    spi_read = 6,
    spi_finish = 7,
    materialize_error_log = 8,
    materialize_interrupt = 9,
    materialize_scanner_read = 10,
};

struct AsyncMpsWorkerBoundaryStatsSnapshot {
    std::uint64_t relation_leaf_calls = 0;
    std::uint64_t spi_leaf_calls = 0;
    std::uint64_t materialize_interrupt_leaf_calls = 0;
    std::uint64_t materialize_scanner_read_leaf_calls = 0;
    std::uint64_t pg_leaf_failures = 0;
    std::uint64_t injected_pg_leaf_failures = 0;
    std::uint64_t materialize_admissions = 0;
    std::uint64_t materialize_failure_unwinds = 0;
    std::uint64_t materialize_admission_restored = 0;
    std::uint64_t materialize_admission_mismatches = 0;
    AsyncMpsWorkerPgFailure last_failure_point =
        AsyncMpsWorkerPgFailure::none;
    bool last_failure_injected = false;
    AsyncMpsWorkerPgFailure armed_failure_point =
        AsyncMpsWorkerPgFailure::none;
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

enum class AsyncMpsStagePgFailure : std::uint8_t {
    none = 0,
    register_xact_callback = 1,
    dsm_create = 2,
    dsm_pin = 3,
    dsm_detach = 4,
};

struct PersistChunk {
    bioseq::indexing::index_sub_key_t sub_key = 0;
    std::uint32_t chunk_no = 0;
    std::span<const char> bytes{};
};

[[nodiscard]] bool arm_async_mps_stage_pg_failure(
    AsyncMpsStagePgFailure point) noexcept;
void disarm_async_mps_stage_pg_failure() noexcept;

[[nodiscard]] bool stage_async_index_persist(
    const PersistTarget& target,
    std::span<const PersistChunk> chunks);

[[nodiscard]] bool stage_async_mps_build(
    const PersistTarget& target,
    const bioseq::storage::seq_locator& locator,
    std::uint8_t alphabet_code,
    std::uint8_t storage_unit_byte_width,
    std::uint16_t last_fragment_padding,
    std::int32_t pair_sort_impl,
    std::int32_t pipeline_mode,
    std::int32_t max_concurrency,
    std::int32_t index_memory_cap_mb,
    std::int32_t index_sketch_backend,
    std::int64_t highway_executed_target,
    int requested_provider_wire_value);

void install_scheduler_shmem_hooks();
void register_scheduler_supervisor();

[[nodiscard]] std::string warmup_backend_local_shader_cache_report();
void install_pg_shader_provider_if_available() noexcept;
void prepare_pg_vulkan_runtime_ready();
[[nodiscard]] VulkanRuntimeStatusSnapshot vulkan_runtime_status_snapshot();

[[nodiscard]] bool supervisor_available() noexcept;
void backend_sync();
[[nodiscard]] BatchTicket begin_batch(std::int64_t queries) noexcept;
void end_batch(BatchTicket ticket, bool completed) noexcept;
void restart_cluster();
[[nodiscard]] SchedulerSnapshot snapshot();

[[nodiscard]] AsyncMpsBuildStatsSnapshot async_mps_build_stats_snapshot();
[[nodiscard]] AsyncMpsWorkerBoundaryStatsSnapshot
async_mps_worker_boundary_stats_snapshot() noexcept;
[[nodiscard]] bool arm_async_mps_worker_pg_failure(
    AsyncMpsWorkerPgFailure point) noexcept;
void disarm_async_mps_worker_pg_failure() noexcept;
[[nodiscard]] const char* async_mps_worker_failure_point_name(
    AsyncMpsWorkerPgFailure point) noexcept;

} // namespace bioseq::parallelism::pg

// Module-internal worker seam shared by the ordinary implementation units.
// These declarations are intentionally not exported to importers.
namespace bioseq::parallelism::pg {

[[nodiscard]] inline bool is_standalone_backend_process() noexcept {
    return MyBackendType == B_STANDALONE_BACKEND
        && !IsPostmasterEnvironment
        && !IsUnderPostmaster;
}

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
    std::uint32_t payload_kind;
    std::int32_t requested_provider;
};

enum class RuntimeInitState : std::uint32_t {
    Pending = 0,
    Ready = 1,
    Failed = 2,
};

enum class CompileSlotState : std::uint32_t {
    Free = 0,
    Filling = 1,
    Requested = 2,
    Compiling = 3,
    DoneOk = 4,
    DoneErr = 5,
};

struct ShaderCompileSlot {
    pg_atomic_uint32 state;
    pg_atomic_uint32 generation;
    bioseq::gpu::vulkan::ShaderVariantKey key{};
    std::uint32_t requester_pid = 0;
    std::uint32_t errc = 0;
};

inline constexpr std::uint32_t kShaderCacheOkWireValue = 0;

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

/*
 * Cross-backend evidence for the asynchronous MPS worker's PostgreSQL leaf
 * seams.  These counters live in scheduler shared memory because the backend
 * that arms an integration failure is not the background worker that consumes
 * it.  The injection selector is one-shot: the worker atomically clears it
 * before raising the requested ERROR.
 */
struct PgAsyncMpsWorkerBoundaryStats {
    pg_atomic_uint64 relation_leaf_calls;
    pg_atomic_uint64 spi_leaf_calls;
    pg_atomic_uint64 materialize_interrupt_leaf_calls;
    pg_atomic_uint64 materialize_scanner_read_leaf_calls;
    pg_atomic_uint64 pg_leaf_failures;
    pg_atomic_uint64 injected_pg_leaf_failures;
    pg_atomic_uint64 materialize_admissions;
    pg_atomic_uint64 materialize_failure_unwinds;
    pg_atomic_uint64 materialize_admission_restored;
    pg_atomic_uint64 materialize_admission_mismatches;
    pg_atomic_uint32 last_failure_point;
    pg_atomic_uint32 armed_failure_point;
};

struct BioseqSchedulerShared {
    pg_atomic_uint32 cfg_max_concurrency;
    pg_atomic_uint32 cfg_generation;
    pg_atomic_uint32 state;
    PgSchedulerStats stats;
    PgMpsBuildStats mps_build_stats;
    PgAsyncMpsWorkerBoundaryStats async_mps_worker_boundary_stats;
    static constexpr std::size_t kPersistQueueSize = 64;
    PgPersistQueueSlot persist_queue[kPersistQueueSize];
    pg_atomic_uint32 runtime_init_state;
    pg_atomic_uint32 runtime_init_generation;
    ConditionVariable runtime_init_cv;
    pg_atomic_uint32 runtime_init_errc;
    char cache_digest_hex[33];
    pg_atomic_uint32 supervisor_pgprocno;
    pg_atomic_uint32 supervisor_generation;
    static constexpr std::size_t kCompileQueueSize = 64;
    ShaderCompileSlot compile_queue[kCompileQueueSize];
    ConditionVariable compile_done_cv;
};

inline BioseqSchedulerShared* g_shared = nullptr;

static_assert(alignof(BioseqSchedulerShared) <= MAXIMUM_ALIGNOF);

enum class PersistPayloadKind : std::uint32_t {
    none = 0,
    prohibited_legacy_dsm = 1,
    prohibited_legacy_file = 2,
    mps_build_identity = 3,
};

inline constexpr std::uint32_t kIndexBuildMpsDsmMagic = 0xB1051D5Au;
inline constexpr std::uint32_t kIndexBuildMpsDsmVersion = 5;

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
    std::int32_t index_sketch_backend = 0;
    std::int64_t highway_executed_target = 0;
    IndexPayloadStorageBackend requested_provider =
        IndexPayloadStorageBackend::postgres_relation;
};

struct PersistClaim {
    std::size_t slot_index = 0;
    std::uint32_t generation = 0;
    PersistTarget target{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
    PersistPayloadKind payload_kind = PersistPayloadKind::none;
    IndexPayloadStorageBackend requested_provider =
        IndexPayloadStorageBackend::postgres_relation;
};

[[nodiscard]] unsigned effective_max_concurrency(unsigned configured);
void init_mps_build_stats(PgMpsBuildStats& stats);
void init_async_mps_worker_boundary_stats(
    PgAsyncMpsWorkerBoundaryStats& stats);
void release_pinned_dsm(dsm_handle handle) noexcept;
void record_prohibited_legacy_persist_rejection(
    std::string_view route) noexcept;
void remove_temp_file_noexcept(std::string_view path) noexcept;
[[nodiscard]] bool publish_persist_job_to_shared_queue(
    const PersistTarget& target,
    std::string_view index_name,
    dsm_handle dsm,
    std::uint64_t payload_bytes,
    PersistPayloadKind payload_kind,
    IndexPayloadStorageBackend requested_provider) noexcept;
void register_persist_xact_callback_pg_leaf();
void enqueue_pending_mps_build(
    const PersistTarget& target,
    dsm_handle handle,
    std::uint64_t payload_bytes,
    IndexPayloadStorageBackend requested_provider);
[[nodiscard]] std::optional<MpsBuildMetadata>
parse_mps_build_metadata(const char* base, Size mapped);
[[nodiscard]] const bioseq::minimizers::MinimizerKernels*
rebind_minimizer_kernels_for_backend(
    std::int32_t backend_value,
    std::int64_t highway_executed_target);
[[nodiscard]] bool worker_target_accepts_index_persist(
    const PersistTarget& target,
    std::string_view index_name);
void publish_async_mps_build_stats(
    const bioseq::storage::indexing::LastMpsBuildStats& stats,
    std::uint64_t worker_build_ns,
    std::uint64_t pipeline_mode,
    std::uint64_t max_concurrency,
    std::uint64_t index_memory_cap_mb,
    bool lazy_worker_shareable) noexcept;
void record_async_mps_worker_pg_leaf_call(
    AsyncMpsWorkerPgFailure point) noexcept;
void record_async_mps_worker_pg_leaf_failure(
    AsyncMpsWorkerPgFailure point) noexcept;
void inject_async_mps_worker_pg_failure_leaf(
    AsyncMpsWorkerPgFailure point);
void record_async_mps_materialize_admission() noexcept;
void record_async_mps_materialize_failure_unwind(
    bool admission_restored) noexcept;
void process_mps_build_claim(
    const PersistClaim& claim,
    const char* base,
    Size mapped,
    Snapshot registered_snapshot);

[[nodiscard]] std::expected<void, std::string> load_bioseq_runtime();
void publish_runtime_init_result(
    std::expected<void, std::string> result) noexcept;
void service_pending_shader_compiles();
void process_one_persist_job_if_ready();

} // namespace bioseq::parallelism::pg
