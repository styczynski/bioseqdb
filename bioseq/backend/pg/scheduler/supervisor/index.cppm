/**
 * @file supervisor/index.cppm
 * @brief Coordinate PostgreSQL batch statistics, background builds, and shader setup.
 */
module;

extern "C" {
#include "postgres.h"

#include "storage/condition_variable.h"
#include "storage/dsm_impl.h"
#include "utils/snapshot.h"

#include "miscadmin.h"
#include "port/atomics.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

export module bioseq.postgres.scheduler.supervisor;

export import :persist_queue_publish;
import bioseq.indexing.type;
import bioseq.sequences.kmers.highway_minimizer_contract;
import bioseq.gpu.vulkan.shader_variants;
import bioseq.parallelism.concepts;
import bioseq.parallelism.mode;
import bioseq.parallelism.pool;
import bioseq.postgres.configuration.core;
import bioseq.storage.layout;
export import bioseq.postgres.storage.indexing.minimizer_build_stats;

export namespace bioseq::postgres::scheduler {

/** Track a started batch. Pass this backend-local ticket once to end_batch. */
struct batch_ticket {
    bool shared = false;
    std::uint32_t generation = 0;
    std::size_t slot = 0;
    std::int64_t queries = 0;
    bioseq::parallelism::global_pool::batch_ticket local{};
};

/** Current scheduler settings, batch counts, queue depth, and memory use. */
struct scheduler_snapshot {
    bool is_supervisor = false;
    std::uint32_t config_generation = 0;
    bioseq::parallelism::pipeline_concurrency_mode mode =
        bioseq::parallelism::pipeline_concurrency_mode::single_thread;
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

/** Vulkan device availability, limits, and any initialization error. */
struct vulkan_runtime_status_snapshot {
    bool available = false;
    std::string device_name{};
    std::int32_t max_workgroup_count_x = 0;
    bool scalar_block_layout = false;
    std::string initialization_error{};
    std::int64_t device_local_bytes = 0;
    bool software_device = false;
};

/** Counters and worker settings from the latest published asynchronous index build. */
struct async_minimizer_sequence_build_stats_snapshot {
    std::uint64_t generation = 0;
    bioseq::storage::indexing::last_minimizer_sequence_build_stats stats{};
    std::uint64_t worker_build_ns = 0;
    std::uint64_t pipeline_mode = 0;
    std::uint64_t max_concurrency = 0;
    std::uint64_t index_memory_cap_mb = 0;
    std::uint64_t shareable_lazy_workers = 0;
};

/**
 * Worker operations that tests can force to fail once. Shared memory carries
 * the selected operation from the requesting backend to the worker.
 */
enum class async_minimizer_sequence_worker_pg_failure : std::uint32_t {
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

/** Worker PostgreSQL call counts and results of error-cleanup tests. */
struct async_minimizer_sequence_worker_boundary_stats_snapshot {
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
    async_minimizer_sequence_worker_pg_failure last_failure_point =
        async_minimizer_sequence_worker_pg_failure::none;
    bool last_failure_injected = false;
    async_minimizer_sequence_worker_pg_failure armed_failure_point =
        async_minimizer_sequence_worker_pg_failure::none;
};

/** Job-staging operations that tests can force to fail once. */
enum class async_minimizer_sequence_stage_pg_failure : std::uint8_t {
    none = 0,
    register_xact_callback = 1,
    dsm_create = 2,
    dsm_pin = 3,
    dsm_detach = 4,
};

/** Select a staging operation to fail once. Return false outside test builds. */
[[nodiscard]] bool arm_async_minimizer_sequence_stage_pg_failure(
    async_minimizer_sequence_stage_pg_failure point) noexcept;
/** Cancel the pending staging failure injection. */
void disarm_async_minimizer_sequence_stage_pg_failure() noexcept;

/**
 * Save an index-build request with the current transaction.
 * Copy the source locator and build settings into pinned shared memory so a
 * worker can rebuild the index after commit. Unsupported requests or unavailable
 * queue resources return false. Invalid algorithm settings raise an error.
 * @param target Table, column, index, and sequence identity.
 * @param locator Stored sequence location used by the worker.
 * @param alphabet_code Sequence alphabet code.
 * @param storage_unit_byte_width Packed storage width in bytes.
 * @param last_fragment_padding Unused symbol positions in the last fragment.
 * @param pair_sort_impl Pair sorter selector.
 * @param pipeline_mode Worker concurrency mode.
 * @param max_concurrency Worker thread limit.
 * @param index_memory_cap_mb Worker memory cap in MiB.
 * @param index_sketch_backend Minimizer algorithm selector.
 * @param highway_executed_target Highway target selected before staging.
 * @return True after the transaction list takes ownership of the pinned segment.
 */
[[nodiscard]] bool
stage_async_minimizer_sequence_build(const persist_target& target,
                                     const bioseq::storage::sequence_locator& locator,
                                     std::uint8_t alphabet_code,
                                     std::uint8_t storage_unit_byte_width,
                                     std::uint16_t last_fragment_padding,
                                     std::int32_t pair_sort_impl,
                                     std::int32_t pipeline_mode,
                                     std::int32_t max_concurrency,
                                     std::int32_t index_memory_cap_mb,
                                     std::int32_t index_sketch_backend,
                                     std::int64_t highway_executed_target);

/** @brief Installs shared-memory request and startup hooks for the scheduler. */
void install_scheduler_shmem_hooks();
/** @brief Registers the scheduler supervisor background worker. */
void register_scheduler_supervisor();

/** Prepare the Apple shader cache and return a text report of its contents or errors. */
[[nodiscard]] std::string warmup_backend_local_shader_cache_report();
/** Install the Apple shader provider outside shared preloading and single-user mode. */
void install_pg_shader_provider_if_available() noexcept;
/** Prepare shader cache state before the backend first accesses the Vulkan runtime. */
void prepare_pg_vulkan_runtime_ready();
/** Prepare the runtime and report its device features and initialization error. */
[[nodiscard]] vulkan_runtime_status_snapshot snapshot_vulkan_runtime_status();

/** Check whether this backend has attached scheduler shared memory. */
[[nodiscard]] bool supervisor_available() noexcept;
/** Update this backend's pool settings, memory cap, and shader provider. */
void backend_sync();
/**
 * Start counting a batch and return its completion ticket.
 * @param queries Query count, with negative values treated as zero.
 * @return Ticket to pass once to end_batch.
 */
[[nodiscard]] batch_ticket begin_batch(std::int64_t queries) noexcept;
/**
 * Finish counting a batch, recording completed queries only when completed is true.
 * @param ticket Ticket returned by begin_batch, used once.
 */
void end_batch(batch_ticket ticket, bool completed) noexcept;
/** Apply current scheduler settings and reset shared and local batch statistics. */
void restart_cluster();
/** Read shared batch statistics and this backend's thread and memory use. */
[[nodiscard]] scheduler_snapshot snapshot();

/** Read the latest shared index-build counters, or zero values without shared state. */
[[nodiscard]] async_minimizer_sequence_build_stats_snapshot
snapshot_async_minimizer_sequence_build_stats();
/** Read the worker's shared PostgreSQL call, failure, and cleanup counters. */
[[nodiscard]] async_minimizer_sequence_worker_boundary_stats_snapshot
snapshot_async_minimizer_sequence_worker_boundary_stats() noexcept;
/**
 * Select a worker operation to fail once in tests. Return false when the
 * selector is invalid or shared state or test instrumentation is unavailable.
 */
[[nodiscard]] bool arm_async_minimizer_sequence_worker_pg_failure(
    async_minimizer_sequence_worker_pg_failure point) noexcept;
/** Cancel the worker's pending failure injection. */
void disarm_async_minimizer_sequence_worker_pg_failure() noexcept;
/** Return a worker failure point's diagnostic name, or "invalid". */
[[nodiscard]] const char* async_minimizer_sequence_worker_failure_point_name(
    async_minimizer_sequence_worker_pg_failure point) noexcept;

} // namespace bioseq::postgres::scheduler

// Internal declarations shared by this module's implementation files.
namespace bioseq::postgres::scheduler {

/** Check for a standalone backend running without the postmaster. */
[[nodiscard]] inline bool is_standalone_backend_process() noexcept {
    return MyBackendType == B_STANDALONE_BACKEND && !IsPostmasterEnvironment && !IsUnderPostmaster;
}

/** Scheduler counters shared by PostgreSQL backends. */
struct pg_scheduler_stats {
    static constexpr std::size_t k_inflight_slots =
        bioseq::parallelism::scheduler_stats::k_inflight_slots;
    pg_atomic_uint64 batches_scheduled;
    pg_atomic_uint64 batches_processed;
    pg_atomic_uint64 batches_in_flight;
    pg_atomic_uint64 queries_waiting;
    pg_atomic_uint64 queries_processed;
    pg_atomic_uint64 rate_window_start_ns;
    pg_atomic_uint64 rate_window_start_queries;
    pg_atomic_uint64 restart_ns;
    // Store the counters directly in PostgreSQL shared memory.
    pg_atomic_uint64 inflight_submit_ns[k_inflight_slots];
    pg_atomic_uint32 inflight_generation[k_inflight_slots];
    pg_atomic_uint64 inflight_seq;
};

/** Supervisor initialization state, reset to pending when the worker starts. */
enum class runtime_initialization_state : std::uint32_t {
    pending = 0,
    ready = 1,
    failed = 2,
};

/**
 * Ownership state of a shared shader request. After compilation finishes,
 * the waiter releases the slot. The compiler releases it when the waiter
 * has already cancelled.
 */
enum class compile_slot_state : std::uint32_t {
    free = 0,
    filling = 1,
    requested = 2,
    compiling = 3,
    done_ok = 4,
    done_error = 5,
};

/** Shader variant and completion status shared between a backend and the supervisor. */
struct shader_compile_slot {
    pg_atomic_uint32 state;
    pg_atomic_uint32 generation;
    bioseq::gpu::vulkan::shader_variant_key key{};
    std::uint32_t requester_pid = 0;
    std::uint32_t errc = 0;
};

/** Shared error code for a successful shader-cache operation. */
inline constexpr std::uint32_t k_shader_cache_ok_wire_value = 0;

/** Shared counters from the latest asynchronous minimizer-sequence build. */
struct pg_minimizer_sequence_build_stats {
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
    pg_atomic_uint64 shareable_lazy_workers;
};

/**
 * Share worker call and cleanup counters with test backends. The worker clears
 * a requested failure before raising it so each injection runs once.
 */
struct pg_async_minimizer_sequence_worker_boundary_stats {
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

/** Shared scheduler queues, statistics, and shader initialization state. */
struct scheduler_shared_state {
    pg_atomic_uint32 cfg_max_concurrency;
    pg_atomic_uint32 cfg_generation;
    pg_atomic_uint32 state;
    pg_scheduler_stats stats;
    pg_minimizer_sequence_build_stats minimizer_sequence_build_stats;
    pg_async_minimizer_sequence_worker_boundary_stats
        async_minimizer_sequence_worker_boundary_stats;
    static constexpr std::size_t k_persist_queue_size = 64;
    // Keep queue slots directly in PostgreSQL shared memory.
    pg_persist_queue_slot persist_queue[k_persist_queue_size];
    pg_atomic_uint32 runtime_init_state;
    pg_atomic_uint32 runtime_init_generation;
    ConditionVariable runtime_init_cv;
    pg_atomic_uint32 runtime_init_errc;
    // Share the digest bytes directly between PostgreSQL processes.
    char cache_digest_hex[33];
    pg_atomic_uint32 supervisor_pgprocno;
    pg_atomic_uint32 supervisor_generation;
    static constexpr std::size_t k_compile_queue_size = 64;
    // Keep shader requests directly in PostgreSQL shared memory.
    shader_compile_slot compile_queue[k_compile_queue_size];
    ConditionVariable compile_done_cv;
};

/** @brief Points to scheduler shared memory after backend attachment. */
inline scheduler_shared_state* g_shared = nullptr;

static_assert(alignof(scheduler_shared_state) <= MAXIMUM_ALIGNOF);

inline constexpr std::uint32_t k_minimizer_sequence_build_dsm_magic = 0xB1051D5Au;
inline constexpr std::uint32_t k_minimizer_sequence_build_wire_revision = 6;

/** Build settings and sequence location decoded from a shared-memory request. */
struct minimizer_sequence_build_metadata {
    std::string index_name{};
    bioseq::storage::sequence_locator locator{};
    std::uint8_t alphabet_code = 0;
    std::uint8_t storage_unit_byte_width = 0;
    std::uint16_t last_fragment_padding = 0;
    std::int32_t pair_sort_impl = 0;
    std::int32_t pipeline_mode = 0;
    std::int32_t max_concurrency = 0;
    std::int32_t index_memory_cap_mb = 0;
    std::int32_t index_sketch_backend = 0;
    std::int64_t highway_executed_target = 0;
};

/**
 * A build request claimed by the worker, including its pinned metadata segment.
 * Finishing the request releases the pin before making the slot reusable.
 */
struct persist_claim {
    std::size_t slot_index = 0;
    std::uint32_t generation = 0;
    persist_target target{};
    dsm_handle dsm = DSM_HANDLE_INVALID;
    std::uint64_t payload_bytes = 0;
    persist_payload_kind payload_kind = persist_payload_kind::none;
};

/**
 * Choose at least one worker thread, using the hardware default for zero.
 * Force one thread in PostgreSQL parallel workers when their Bioseq parallelism
 * is disabled.
 */
[[nodiscard]] unsigned effective_max_concurrency(unsigned configured);
/** Initialize the shared asynchronous build counters to zero. */
void initialize_minimizer_sequence_build_stats(pg_minimizer_sequence_build_stats& stats);
/** Initialize shared worker call counters and test failure selectors to zero. */
void initialize_async_minimizer_sequence_worker_boundary_stats(
    pg_async_minimizer_sequence_worker_boundary_stats& stats);
/** Release a shared-memory pin, ignoring an invalid handle or cleanup error. */
void release_pinned_dsm(dsm_handle handle) noexcept;
/** Register the callback that publishes committed jobs and releases aborted jobs. */
void register_persist_xact_callback_pg_leaf();
/**
 * Add a pinned build request to this transaction's pending jobs.
 * Ownership transfers after insertion succeeds.
 * @param payload_bytes Size of the mapped metadata in bytes.
 */
void enqueue_pending_minimizer_sequence_build(const persist_target& target,
                                              dsm_handle handle,
                                              std::uint64_t payload_bytes);
/**
 * Read a build request from mapped shared memory. Check the format and
 * algorithm selectors, locator version, nonzero source version, and name
 * bounds before copying the metadata. Return no request if a check fails.
 * @param base Start of the mapped bytes.
 * @param mapped Accessible byte count.
 */
[[nodiscard]] std::optional<minimizer_sequence_build_metadata>
parse_minimizer_sequence_build_metadata(const char* base, Size mapped);
/**
 * Find this worker's kernels for the Highway target saved with the job.
 * Return null for scalar or Vulkan backends. A missing requested SIMD kernel
 * raises std::logic_error.
 */
[[nodiscard]] const bioseq::minimizers::minimizer_kernels*
rebind_minimizer_kernels_for_backend(std::int32_t backend_value,
                                     std::int64_t highway_executed_target);
/** Check that the queued table identity still matches and insert maintenance is enabled. */
[[nodiscard]] bool worker_target_accepts_index_persist(const persist_target& target,
                                                       std::string_view index_name);
/**
 * Publish a completed build's counters and worker settings in shared memory.
 * @param worker_build_ns Build duration in nanoseconds.
 * @param index_memory_cap_mb Worker memory cap in MiB.
 * @param has_shareable_lazy_worker Whether the source allows parallel reads.
 */
void publish_async_minimizer_sequence_build_stats(
    const bioseq::storage::indexing::last_minimizer_sequence_build_stats& stats,
    std::uint64_t worker_build_ns,
    std::uint64_t pipeline_mode,
    std::uint64_t max_concurrency,
    std::uint64_t index_memory_cap_mb,
    bool has_shareable_lazy_worker) noexcept;
/** Count a worker call to the selected PostgreSQL operation. */
void record_async_minimizer_sequence_worker_pg_leaf_call(
    async_minimizer_sequence_worker_pg_failure point) noexcept;
/** Record a PostgreSQL operation failure and whether a test caused it. */
void record_async_minimizer_sequence_worker_pg_leaf_failure(
    async_minimizer_sequence_worker_pg_failure point) noexcept;
/** Raise the selected worker test failure once, inside a PostgreSQL error bridge. */
void inject_async_minimizer_sequence_worker_pg_failure_leaf(
    async_minimizer_sequence_worker_pg_failure point);
/** Count a worker target copy that received a memory reservation. */
void record_async_minimizer_sequence_materialize_admission() noexcept;
/**
 * Count a failed target copy and whether cleanup restored the memory budget.
 * @param admission_restored Reserved bytes equal their value before the copy.
 */
void record_async_minimizer_sequence_materialize_failure_unwind(bool admission_restored) noexcept;
/**
 * Build a claimed index using the worker transaction's snapshot. Convert
 * failures to PostgreSQL errors after C++ cleanup.
 * @param claim Claimed queue entry identifying the target.
 * @param base Mapped shared-memory metadata.
 * @param mapped Accessible metadata size in bytes.
 * @param registered_snapshot Snapshot owned by the worker transaction.
 */
void process_minimizer_sequence_build_claim(const persist_claim& claim,
                                            const char* base,
                                            Size mapped,
                                            Snapshot registered_snapshot);

/** Prepare the supervisor's Apple shader cache, returning any build or validation error. */
[[nodiscard]] std::expected<void, std::string> load_bioseq_runtime();
/** Publish shader initialization success or failure and wake waiting backends. */
void publish_runtime_init_result(std::expected<void, std::string> initialization_result) noexcept;
/** Compile up to four ready shader requests and wake their waiting backends. */
void service_pending_shader_compiles();
/** @brief Claims and processes at most one queued persistence job. */
void process_one_persist_job_if_ready();

} // namespace bioseq::postgres::scheduler
