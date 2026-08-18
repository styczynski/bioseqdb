/**
 * @file supervisor/index.cppm
 * @brief Narrow PostgreSQL-facing scheduler supervision API.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "storage/condition_variable.h"
#include "storage/dsm_impl.h"
#include "utils/snapshot.h"

#include "miscadmin.h"
#include "port/atomics.h"
}

export module bioseq.postgres.scheduler.supervisor;

export import :persist_queue_publish;
import std;
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

/**
 * @brief Tracks one scheduler batch admission until completion.
 *
 * A ticket is backend-local and must be returned once to `end_batch`.
 */
struct batch_ticket {
    bool shared = false;
    std::uint32_t generation = 0;
    std::size_t slot = 0;
    std::int64_t queries = 0;
    bioseq::parallelism::global_pool::batch_ticket local{};
};

/** @brief Captures scheduler configuration, queue depth, throughput, and memory use. */
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

/** @brief Captures backend-visible Vulkan runtime availability and device limits. */
struct vulkan_runtime_status_snapshot {
    bool available = false;
    std::string device_name{};
    std::int32_t max_workgroup_count_x = 0;
    bool scalar_block_layout = false;
    std::string initialization_error{};
    std::int64_t device_local_bytes = 0;
    bool software_device = false;
};

/** @brief Captures the most recently published asynchronous index-build statistics. */
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
 * @brief Selects a one-shot PostgreSQL failure in the async worker.
 *
 * Shared memory carries the selector from a normal backend to the background worker.
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

/** @brief Captures PostgreSQL leaf-call and unwind statistics for the async worker. */
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

/** @brief Selects a one-shot PostgreSQL failure in async build staging. */
enum class async_minimizer_sequence_stage_pg_failure : std::uint8_t {
    none = 0,
    register_xact_callback = 1,
    dsm_create = 2,
    dsm_pin = 3,
    dsm_detach = 4,
};

/**
 * @brief Arms one asynchronous build-staging failure point.
 * @param[in] point PostgreSQL leaf that should fail once.
 * @return True when the selector was accepted.
 */
[[nodiscard]] bool arm_async_minimizer_sequence_stage_pg_failure(
    async_minimizer_sequence_stage_pg_failure point) noexcept;
/** @brief Clears any armed asynchronous build-staging failure point. */
void disarm_async_minimizer_sequence_stage_pg_failure() noexcept;

/**
 * @brief Serializes and stages one minimizer-sequence build request.
 * @param[in] target Relation, index, and sequence identity.
 * @param[in] locator Durable fragment source identity.
 * @param[in] alphabet_code Serialized alphabet selector.
 * @param[in] storage_unit_byte_width Packed storage width in bytes.
 * @param[in] last_fragment_padding Padding symbols in the final fragment.
 * @param[in] pair_sort_impl Serialized pair sorter selector.
 * @param[in] pipeline_mode Serialized concurrency mode.
 * @param[in] max_concurrency Worker concurrency limit.
 * @param[in] index_memory_cap_mb Worker index-memory cap in MiB.
 * @param[in] index_sketch_backend Serialized minimizer backend selector.
 * @param[in] highway_executed_target Exact target bit selected before staging.
 * @return True when the request was attached to the current transaction.
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

/**
 * @brief Warms the backend-local shader cache and formats its status.
 * @return A human-readable cache status report.
 */
[[nodiscard]] std::string warmup_backend_local_shader_cache_report();
/** @brief Installs the PostgreSQL shader provider when shared supervision is available. */
void install_pg_shader_provider_if_available() noexcept;
/** @brief Ensures the backend has a usable Vulkan runtime or raises a PostgreSQL error. */
void prepare_pg_vulkan_runtime_ready();
/**
 * @brief Samples the backend-visible Vulkan runtime state.
 * @return The current runtime status snapshot.
 */
[[nodiscard]] vulkan_runtime_status_snapshot snapshot_vulkan_runtime_status();

/**
 * @brief Reports whether the shared scheduler supervisor can accept work.
 * @return True when the supervisor is initialized and reachable.
 */
[[nodiscard]] bool supervisor_available() noexcept;
/** @brief Synchronizes backend-local scheduler configuration with shared state. */
void backend_sync();
/**
 * @brief Admits a batch containing the requested query count.
 * @param[in] queries Query count, with negative values treated as zero.
 * @return A ticket that must be passed to `end_batch`.
 */
[[nodiscard]] batch_ticket begin_batch(std::int64_t queries) noexcept;
/**
 * @brief Completes or cancels a previously admitted scheduler batch.
 * @param[in] ticket Ticket returned by `begin_batch`.
 * @param[in] completed Whether to record completed work instead of cancellation.
 */
void end_batch(batch_ticket ticket, bool completed) noexcept;
/** @brief Resets scheduler counters and start time for the current cluster. */
void restart_cluster();
/**
 * @brief Samples shared and backend-local scheduler state.
 * @return The current scheduler snapshot.
 */
[[nodiscard]] scheduler_snapshot snapshot();

/**
 * @brief Samples statistics from the last asynchronous minimizer-sequence build.
 * @return The current shared build-statistics snapshot.
 */
[[nodiscard]] async_minimizer_sequence_build_stats_snapshot
snapshot_async_minimizer_sequence_build_stats();
/**
 * @brief Samples PostgreSQL boundary statistics from the async build worker.
 * @return The current shared worker-boundary snapshot.
 */
[[nodiscard]] async_minimizer_sequence_worker_boundary_stats_snapshot
snapshot_async_minimizer_sequence_worker_boundary_stats() noexcept;
/**
 * @brief Arms one async-worker PostgreSQL failure point.
 * @param[in] point PostgreSQL leaf that should fail once.
 * @return True when the selector was accepted.
 */
[[nodiscard]] bool arm_async_minimizer_sequence_worker_pg_failure(
    async_minimizer_sequence_worker_pg_failure point) noexcept;
/** @brief Clears any armed async-worker PostgreSQL failure point. */
void disarm_async_minimizer_sequence_worker_pg_failure() noexcept;
/**
 * @brief Returns the stable diagnostic name for a worker failure point.
 * @param[in] point Failure selector.
 * @return A process-lifetime string owned by the module.
 */
[[nodiscard]] const char* async_minimizer_sequence_worker_failure_point_name(
    async_minimizer_sequence_worker_pg_failure point) noexcept;

} // namespace bioseq::postgres::scheduler

// Module-internal worker seam shared by the ordinary implementation units.
// These declarations are intentionally not exported to importers.
namespace bioseq::postgres::scheduler {

/**
 * @brief Detects standalone startup processes that cannot use postmaster state.
 * @return True only for a standalone backend outside a postmaster environment.
 */
[[nodiscard]] inline bool is_standalone_backend_process() noexcept {
    return MyBackendType == B_STANDALONE_BACKEND && !IsPostmasterEnvironment && !IsUnderPostmaster;
}

/** @brief Stores cross-backend scheduler counters in PostgreSQL shared memory. */
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
    // Raw arrays preserve lifetime-free PostgreSQL shared-memory layout.
    pg_atomic_uint64 inflight_submit_ns[k_inflight_slots];
    pg_atomic_uint32 inflight_generation[k_inflight_slots];
    pg_atomic_uint64 inflight_seq;
};

/**
 * @brief Describes shared Vulkan runtime initialization progress.
 *
 * The supervisor publishes exactly one transition from pending to ready or failed.
 */
enum class runtime_initialization_state : std::uint32_t {
    pending = 0,
    ready = 1,
    failed = 2,
};

/**
 * @brief Describes ownership and completion of one shared shader compile slot.
 *
 * Slots move from free through filling, requested, compiling, and one terminal state. The
 * waiter or an orphan-aware compiler then owns the terminal-to-free transition.
 */
enum class compile_slot_state : std::uint32_t {
    free = 0,
    filling = 1,
    requested = 2,
    compiling = 3,
    done_ok = 4,
    done_error = 5,
};

/** @brief Stores one cross-backend shader compilation request. */
struct shader_compile_slot {
    pg_atomic_uint32 state;
    pg_atomic_uint32 generation;
    bioseq::gpu::vulkan::shader_variant_key key{};
    std::uint32_t requester_pid = 0;
    std::uint32_t errc = 0;
};

/** @brief Wire value representing a successful shader-cache operation. */
inline constexpr std::uint32_t k_shader_cache_ok_wire_value = 0;

/** @brief Stores the latest async minimizer-sequence build counters in shared memory. */
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
 * @brief Stores cross-backend PostgreSQL boundary counters for the async worker.
 *
 * The backend that arms a failure is distinct from the background worker that
 * consumes it. The worker clears the one-shot selector before raising ERROR.
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

/** @brief Owns scheduler queues, counters, runtime state, and condition variables. */
struct scheduler_shared_state {
    pg_atomic_uint32 cfg_max_concurrency;
    pg_atomic_uint32 cfg_generation;
    pg_atomic_uint32 state;
    pg_scheduler_stats stats;
    pg_minimizer_sequence_build_stats minimizer_sequence_build_stats;
    pg_async_minimizer_sequence_worker_boundary_stats
        async_minimizer_sequence_worker_boundary_stats;
    static constexpr std::size_t k_persist_queue_size = 64;
    // Raw arrays preserve lifetime-free PostgreSQL shared-memory layout.
    pg_persist_queue_slot persist_queue[k_persist_queue_size];
    pg_atomic_uint32 runtime_init_state;
    pg_atomic_uint32 runtime_init_generation;
    ConditionVariable runtime_init_cv;
    pg_atomic_uint32 runtime_init_errc;
    // The fixed C buffer is copied across PostgreSQL processes without an owning object.
    char cache_digest_hex[33];
    pg_atomic_uint32 supervisor_pgprocno;
    pg_atomic_uint32 supervisor_generation;
    static constexpr std::size_t k_compile_queue_size = 64;
    // Raw arrays preserve lifetime-free PostgreSQL shared-memory layout.
    shader_compile_slot compile_queue[k_compile_queue_size];
    ConditionVariable compile_done_cv;
};

/** @brief Points to scheduler shared memory after backend attachment. */
inline scheduler_shared_state* g_shared = nullptr;

static_assert(alignof(scheduler_shared_state) <= MAXIMUM_ALIGNOF);

inline constexpr std::uint32_t k_minimizer_sequence_build_dsm_magic = 0xB1051D5Au;
inline constexpr std::uint32_t k_minimizer_sequence_build_wire_revision = 6;

/** @brief Decoded metadata carried by an asynchronous index-build DSM segment. */
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
 * @brief Claims one shared persistence slot and its pinned DSM payload.
 *
 * The generation identifies one ready-to-claimed transition. Worker completion releases the
 * pin before publishing the slot as free.
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
 * @brief Resolves zero to hardware concurrency and clamps explicit values.
 * @param[in] configured Requested concurrency, or zero for the hardware default.
 * @return A positive scheduler concurrency.
 */
[[nodiscard]] unsigned effective_max_concurrency(unsigned configured);
/**
 * @brief Initializes shared async-build counters and their atomics.
 * @param[out] stats Newly allocated shared-memory counters.
 */
void initialize_minimizer_sequence_build_stats(pg_minimizer_sequence_build_stats& stats);
/**
 * @brief Initializes shared PostgreSQL-boundary counters and selectors.
 * @param[out] stats Newly allocated shared-memory boundary state.
 */
void initialize_async_minimizer_sequence_worker_boundary_stats(
    pg_async_minimizer_sequence_worker_boundary_stats& stats);
/**
 * @brief Unpins and releases a DSM segment without throwing.
 * @param[in] handle DSM handle, or `DSM_HANDLE_INVALID` when already released.
 */
void release_pinned_dsm(dsm_handle handle) noexcept;
/** @brief Registers the transaction callback that owns pending persistence jobs. */
void register_persist_xact_callback_pg_leaf();
/**
 * @brief Adds a pinned async-build payload to transaction-local pending work.
 * @param[in] target Relation, index, and sequence identity.
 * @param[in] handle Pinned metadata DSM handle.
 * @param[in] payload_bytes Mapped metadata size in bytes.
 */
void enqueue_pending_minimizer_sequence_build(const persist_target& target,
                                              dsm_handle handle,
                                              std::uint64_t payload_bytes);
/**
 * @brief Validates and decodes async-build metadata from a DSM mapping.
 * @param[in] base Start of the mapped bytes.
 * @param[in] mapped Accessible byte count.
 * @return Decoded metadata or absence when the mapping is invalid.
 */
[[nodiscard]] std::optional<minimizer_sequence_build_metadata>
parse_minimizer_sequence_build_metadata(const char* base, Size mapped);
/**
 * @brief Rebinds minimizer kernels to the scheduler-selected Highway target.
 * @param[in] backend_value Serialized minimizer backend selector.
 * @param[in] highway_executed_target Exact target bit selected before queueing.
 * @return The exact-target kernel table or null when the coordinate is unsupported.
 */
[[nodiscard]] const bioseq::minimizers::minimizer_kernels*
rebind_minimizer_kernels_for_backend(std::int32_t backend_value,
                                     std::int64_t highway_executed_target);
/**
 * @brief Validates that a claimed target still accepts the selected persisted index.
 * @param[in] target Relation identity captured before commit.
 * @param[in] index_name Current index policy key.
 * @return True when relation identity and policy still match.
 */
[[nodiscard]] bool worker_target_accepts_index_persist(const persist_target& target,
                                                       std::string_view index_name);
/**
 * @brief Publishes async-build counters to scheduler shared memory.
 * @param[in] stats Storage-build counters and byte totals.
 * @param[in] worker_build_ns Total worker build duration in nanoseconds.
 * @param[in] pipeline_mode Serialized concurrency mode.
 * @param[in] max_concurrency Effective worker concurrency.
 * @param[in] index_memory_cap_mb Configured index-memory cap in MiB.
 * @param[in] has_shareable_lazy_worker Whether the source supports parallel reads.
 */
void publish_async_minimizer_sequence_build_stats(
    const bioseq::storage::indexing::last_minimizer_sequence_build_stats& stats,
    std::uint64_t worker_build_ns,
    std::uint64_t pipeline_mode,
    std::uint64_t max_concurrency,
    std::uint64_t index_memory_cap_mb,
    bool has_shareable_lazy_worker) noexcept;
/**
 * @brief Records entry into one worker PostgreSQL leaf boundary.
 * @param[in] point Leaf about to execute.
 */
void record_async_minimizer_sequence_worker_pg_leaf_call(
    async_minimizer_sequence_worker_pg_failure point) noexcept;
/**
 * @brief Records a PostgreSQL failure raised by one worker leaf boundary.
 * @param[in] point Leaf whose error bridge caught the failure.
 */
void record_async_minimizer_sequence_worker_pg_leaf_failure(
    async_minimizer_sequence_worker_pg_failure point) noexcept;
/**
 * @brief Consumes and raises an armed worker failure at the selected leaf.
 * @param[in] point Leaf about to execute inside one PostgreSQL callback.
 */
void inject_async_minimizer_sequence_worker_pg_failure_leaf(
    async_minimizer_sequence_worker_pg_failure point);
/** @brief Records admission of a worker-side target materialization. */
void record_async_minimizer_sequence_materialize_admission() noexcept;
/**
 * @brief Records materialization unwind and whether admission was restored.
 * @param[in] admission_restored Whether reserved bytes returned to their entry value.
 */
void record_async_minimizer_sequence_materialize_failure_unwind(bool admission_restored) noexcept;
/**
 * @brief Processes one claimed async-build payload within a registered snapshot.
 * @param[in] claim Claimed queue identity.
 * @param[in] base Start of the mapped metadata DSM.
 * @param[in] mapped Accessible metadata bytes.
 * @param[in] registered_snapshot Snapshot owned by the worker transaction.
 */
void process_minimizer_sequence_build_claim(const persist_claim& claim,
                                            const char* base,
                                            Size mapped,
                                            Snapshot registered_snapshot);

/**
 * @brief Initializes the Bioseq Vulkan runtime and shader-cache state.
 * @return Success or a diagnostic safe to publish through shared memory.
 */
[[nodiscard]] std::expected<void, std::string> load_bioseq_runtime();
/**
 * @brief Publishes terminal runtime initialization state and wakes waiters.
 * @param[in] initialization_result Runtime initialization outcome safe for shared publication.
 */
void publish_runtime_init_result(std::expected<void, std::string> initialization_result) noexcept;
/** @brief Services every shader compilation request currently ready to claim. */
void service_pending_shader_compiles();
/** @brief Claims and processes at most one queued persistence job. */
void process_one_persist_job_if_ready();

} // namespace bioseq::postgres::scheduler
