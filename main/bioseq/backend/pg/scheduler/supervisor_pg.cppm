/**
 * @file supervisor_pg.cppm
 * @brief Narrow PostgreSQL-facing scheduler supervision API.
 */
module;

#include "common.h"

extern "C" {
#include "storage/dsm_impl.h"
#include "utils/snapshot.h"
}

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

export module bioseq.scheduler.supervisor_pg;

import bioseq.indexing.type;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
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

enum class PersistPayloadKind : std::uint32_t {
    none = 0,
    prohibited_legacy_dsm = 1,
    prohibited_legacy_file = 2,
    mps_build_identity = 3,
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
    std::int32_t index_sketch_backend = 0;
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
[[nodiscard]] std::optional<MpsBuildMetadata>
parse_mps_build_metadata(const char* base, Size mapped);
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

} // namespace bioseq::parallelism::pg
