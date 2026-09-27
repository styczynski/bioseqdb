/**
 * @file bioseq_caching/pg/storage.cppm
 * @brief Stores cached column blocks in shared memory or private files.
 *
 * A manifest groups the blocks of one cached result. Readers pin the
 * manifest while using it, and the janitor frees retired results after
 * readers and waiters release them. Slot sequence numbers let the code
 * reject old handles after a slot is reused. The cache registries decide
 * when results expire or should be evicted.
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#if defined(BIOSEQ_CACHE_TESTING)
#include "fmgr.h"
#include "lib/stringinfo.h"
#endif
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/backend_status.h"
#include "utils/dsa.h"

#include "miscadmin.h"
#include "port/pg_crc32c.h"
#if defined(BIOSEQ_CACHE_TESTING)
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#endif
#include "utils/memutils.h"
#include "utils/timestamp.h"
}

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.cache.pg.storage;

import bioseq.cache.core;
import bioseq.cache.pg.shared_registry;
import bioseq.common.durable_file_io;
import bioseq.common.memory_budget;
import bioseq.postgres.support;
import bioseq.postgres.sql_boundary;

export namespace bioseq::cache::pg::storage {

inline constexpr std::uint32_t k_storage_abi_revision = 7;
inline constexpr std::size_t k_manifest_capacity = 256;
inline constexpr std::size_t k_blocks_per_manifest = 32;
inline constexpr std::size_t k_waiters_per_manifest = 64;
inline constexpr std::size_t k_reader_pins_per_manifest = 64;
inline constexpr std::size_t k_block_ledger_capacity =
    k_manifest_capacity * k_blocks_per_manifest + 256;
inline constexpr std::size_t k_shared_dsa_reservation_capacity = 1024;
inline constexpr std::uint64_t k_default_dsa_initial_bytes = 256ULL << 10;
inline constexpr std::uint64_t k_default_hot_ceiling_bytes = 256ULL << 20;
inline constexpr std::uint64_t k_default_file_ceiling_bytes = 128ULL << 30;
inline constexpr std::uint64_t k_default_maximum_block_bytes = 64ULL << 20;
inline constexpr std::uint32_t k_default_sweep_interval_ms = 1000;
inline constexpr std::uint32_t k_default_reclaim_batch = 64;

/** Storage limits and cleanup settings selected during PostgreSQL startup. */
struct storage_config {
    std::uint64_t dsa_initial_bytes = k_default_dsa_initial_bytes;
    std::uint64_t hot_ceiling_bytes = k_default_hot_ceiling_bytes;
    std::uint64_t file_ceiling_bytes = k_default_file_ceiling_bytes;
    std::uint64_t maximum_block_bytes = k_default_maximum_block_bytes;
    std::uint32_t sweep_interval_ms = k_default_sweep_interval_ms;
    std::uint32_t reclaim_batch = k_default_reclaim_batch;
    std::uint32_t soft_limit_percent = 80;
};

enum class storage_error : std::uint32_t {
    none = 0,
    not_preloaded,
    bootstrap_failed,
    invalid_argument,
    block_too_large,
    ledger_full,
    manifest_full,
    waiter_full,
    reader_full,
    duplicate_generation,
    memory_pressure,
    hot_tier_full,
    file_tier_full,
    filesystem_pressure,
    dsa_allocation_failed,
    unsafe_path,
    path_too_long,
    open_failed,
    write_failed,
    sync_failed,
    rename_failed,
    read_failed,
    short_read,
    checksum_mismatch,
    not_found,
    stale_token,
    invalid_state,
    pinned,
};

enum class storage_checksum_status : std::uint8_t {
    not_checked = 0,
    valid,
    invalid,
};

enum class storage_read_purpose : std::uint8_t {
    prevalidation = 0,
    replay,
};

/** Tracks reserved shared-memory space. Callers must release it in PostgreSQL cleanup paths. */
struct shared_dsa_reservation {
    std::uint32_t slot = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < k_shared_dsa_reservation_capacity && sequence != 0 && reserved == 0;
    }
};

struct shared_dsa_reserve_result {
    storage_error error = storage_error::none;
    shared_dsa_reservation reservation{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == storage_error::none && static_cast<bool>(reservation);
    }
};

enum class worker_lifecycle : std::uint32_t {
    not_registered = 0,
    registered,
    starting,
    running,
    stopping,
    stopped,
    failed,
};

enum class manifest_state : std::uint32_t {
    empty = 0,
    publishing,
    complete,
    retiring,
};

/** Identifies a backend by its process slot, PID and start time. */
struct storage_backend_identity {
    std::uint32_t proc_number = (std::numeric_limits<std::uint32_t>::max)();
    std::int32_t pid = 0;
    std::uint64_t backend_nonce = 0;
    std::int64_t backend_started_at_us = 0;

    [[nodiscard]] bool valid() const noexcept {
        return proc_number != (std::numeric_limits<std::uint32_t>::max)() && pid > 0 &&
               backend_nonce != 0 && backend_started_at_us != 0;
    }

    [[nodiscard]] bool operator==(const storage_backend_identity&) const noexcept = default;
};

static_assert(std::is_standard_layout_v<storage_backend_identity>);
static_assert(std::is_trivially_copyable_v<storage_backend_identity>);

/** Identifies a stored block and its owner, with a sequence number to reject reused slots. */
struct stored_block_token {
    std::uint32_t ledger_slot = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t sequence = 0;
    storage_backend_identity owner{};
    block_reference reference{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return sequence != 0 && ledger_slot < k_block_ledger_capacity && owner.valid() &&
               reference.length != 0;
    }
};

struct store_block_result {
    storage_error error = storage_error::none;
    stored_block_token token{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == storage_error::none && static_cast<bool>(token);
    }
};

/** Describes a cached result when its block manifest is published. */
struct manifest_descriptor {
    cache_entry_id id{};
    cache_entry_kind kind = cache_entry_kind::exact_final_result;
    std::uint32_t flags = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint32_t row_group_count = 0;
    std::uint32_t reserved2 = 0;
    std::int64_t created_at_us = 0;
    std::int64_t expires_at_us = 0;
    std::uint32_t reserved_waiter_count = 0;
    std::uint32_t reserved = 0;
};

struct manifest_handle {
    std::uint32_t slot = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t slot_sequence = 0;
    cache_entry_id id{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < k_manifest_capacity && slot_sequence != 0;
    }
};

struct publish_manifest_result {
    storage_error error = storage_error::none;
    manifest_handle handle{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == storage_error::none && static_cast<bool>(handle);
    }
};

/** Lets the named backend turn one registered waiter into a reader pin. */
struct manifest_waiter_token {
    manifest_handle handle{};
    std::uint32_t waiter_slot = (std::numeric_limits<std::uint32_t>::max)();
    storage_backend_identity owner{};
    std::uint64_t owner_sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(handle) && waiter_slot < k_waiters_per_manifest && owner.valid() &&
               owner_sequence != 0;
    }
};

struct register_manifest_waiter_result {
    storage_error error = storage_error::none;
    manifest_waiter_token token{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == storage_error::none && static_cast<bool>(token);
    }
};

/** Copies a manifest for one reader and keeps its blocks alive until the pin is released. */
struct manifest_pin {
    manifest_handle handle{};
    cache_entry_kind kind = cache_entry_kind::exact_final_result;
    std::uint32_t flags = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint32_t row_group_count = 0;
    std::uint32_t database_oid = 0;
    std::uint32_t block_count = 0;
    std::uint32_t reader_slot = (std::numeric_limits<std::uint32_t>::max)();
    storage_backend_identity owner{};
    std::uint64_t owner_sequence = 0;
    std::array<block_reference, k_blocks_per_manifest> blocks{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(handle) && block_count != 0 &&
               reader_slot < k_reader_pins_per_manifest && owner.valid() && owner_sequence != 0;
    }
};

struct pin_manifest_result {
    storage_error error = storage_error::none;
    manifest_pin pin{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == storage_error::none && static_cast<bool>(pin);
    }
};

struct janitor_sweep_result {
    std::uint32_t expired_manifests = 0;
    std::uint32_t lru_manifests = 0;
    std::uint32_t abandoned_blocks = 0;
    std::uint32_t reaped_waiters = 0;
    std::uint32_t reaped_readers = 0;
    std::uint32_t reclaimed_blocks = 0;
    std::uint32_t stale_partial_files = 0;
    std::uint64_t reclaimed_hot_bytes = 0;
    std::uint64_t reclaimed_file_bytes = 0;
};

struct storage_query_io_snapshot {
    std::uint64_t prevalidation_read_bytes = 0;
    std::uint64_t prevalidation_read_duration_us = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t read_duration_us = 0;
    std::uint64_t checksum_failure_count = 0;
    storage_checksum_status checksum_status = storage_checksum_status::not_checked;
};

struct storage_runtime_snapshot {
    bool bootstrap_ready = false;
    bool dsa_attached = false;
    bootstrap_state bootstrap_status = bootstrap_state::uninitialized;
    worker_lifecycle janitor_lifecycle = worker_lifecycle::not_registered;
    cache_correctness_readiness correctness_readiness =
        cache_correctness_readiness::passthrough_only;
    storage_error last_failure = storage_error::none;
    std::uint64_t boot_nonce = 0;
    std::uint64_t reserved_hot_bytes = 0;
    std::uint64_t fixed_hot_bytes = 0;
    std::uint64_t pending_hot_bytes = 0;
    bool pending_hot_bytes_valid = false;
    std::uint64_t live_hot_bytes = 0;
    std::uint64_t reserved_file_bytes = 0;
    std::uint64_t pending_file_bytes = 0;
    bool pending_file_bytes_valid = false;
    std::uint64_t dsa_stored_bytes = 0;
    std::uint64_t file_stored_bytes = 0;
    std::uint64_t row_group_count = 0;
    std::uint64_t prevalidation_read_bytes = 0;
    std::uint64_t prevalidation_read_duration_us = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t read_duration_us = 0;
    std::uint64_t checksum_failure_count = 0;
    storage_checksum_status checksum_status = storage_checksum_status::not_checked;
    std::uint64_t hot_ceiling_bytes = 0;
    std::uint64_t file_ceiling_bytes = 0;
    std::uint64_t manifest_count = 0;
    std::uint64_t waiter_reference_count = 0;
    std::uint64_t reader_reference_count = 0;
    std::uint64_t completed_sweeps = 0;
    std::int64_t last_sweep_at_us = 0;
    std::int64_t worker_heartbeat_at_us = 0;
    int worker_pid = 0;
    std::uint32_t worker_proc_number = (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t worker_backend_nonce = 0;
    std::int64_t worker_backend_started_at_us = 0;
};

#if defined(BIOSEQ_CACHE_TESTING)
/** Reports memory/file block counts and accounting checks for storage tests. */
struct storage_tier_test_snapshot {
    bool force_file_tier = false;
    bool coherent = false;
    std::uint64_t dsa_blocks = 0;
    std::uint64_t file_blocks = 0;
    std::uint64_t occupied_blocks = 0;
    std::uint64_t reserved_blocks = 0;
    std::uint64_t stored_blocks = 0;
    std::uint64_t referenced_blocks = 0;
    std::uint64_t retiring_blocks = 0;
    std::uint64_t release_committed_blocks = 0;
    std::uint64_t reader_pins = 0;
    std::uint64_t reserved_hot_bytes = 0;
    std::uint64_t fixed_hot_bytes = 0;
    std::uint64_t live_hot_bytes = 0;
    std::uint64_t pending_hot_bytes = 0;
    std::uint64_t dsa_stored_bytes = 0;
    std::uint64_t file_stored_bytes = 0;
    std::uint64_t row_group_count = 0;
    std::uint64_t reserved_file_bytes = 0;
};

/** Reports shared-area initialization and attachment state for storage tests. */
struct storage_bootstrap_test_snapshot {
    bootstrap_state state = bootstrap_state::uninitialized;
    shared_registry::shared_registry_error failure = shared_registry::shared_registry_error::none;
    std::uint64_t boot_nonce = 0;
    std::uint64_t initialization_attempt_count = 0;
    std::uint64_t takeover_count = 0;
    std::uint64_t cleanup_count = 0;
    std::uint64_t attach_count = 0;
    std::uint64_t validation_failure_count = 0;
    shared_registry::shared_registry_owner_token initializer{};
    shared_registry::shared_registry_owner_token last_initializer{};
    std::uint32_t takeover_used = 0;
    std::uint32_t live_unpublished_area_count = 0;
    std::uint32_t cleanup_handle_pinned = 0;
    std::uint32_t reusable_wait_cv_count = 0;
    std::uint32_t last_cleanup_handle = 0;
    bool cleanup_handle_published = false;
    bool ready_handle_published = false;
    bool root_published = false;
    bool mapping_pinned = false;
    bool pause_reached = false;
    bool defer_background_initializer = false;
    bool last_cleanup_handle_was_pinned = false;
    bool last_cleanup_release_verified = false;
    bool storage_attached = false;
    bool storage_metadata_in_dsa = false;
    bool reusable_wait_cvs_in_dsa = false;
    bool main_shmem_storage_region_requested = false;
};

/** Records whether two attachments reused the same shared area and root. */
struct storage_bootstrap_attach_twice_snapshot {
    bool first_attached = false;
    bool second_attached = false;
    bool same_area = false;
    bool same_root = false;
    bool mapping_pinned = false;
    std::uint64_t attempts_before = 0;
    std::uint64_t attempts_after = 0;
    std::uint64_t attaches_before = 0;
    std::uint64_t attaches_after = 0;
};

/**
 * Forces later test blocks into files. The test override skips the
 * production free-space reserve, while still checking available disk
 * space and the configured file-size limits.
 */
[[nodiscard]] bool set_test_force_file_tier(bool enabled) noexcept;

/** Copies block counts and accounting checks from the storage ledgers for tests. */
[[nodiscard]] storage_tier_test_snapshot test_storage_tier_snapshot() noexcept;

[[nodiscard]] storage_bootstrap_test_snapshot test_storage_bootstrap_snapshot() noexcept;

[[nodiscard]] bool test_arm_storage_bootstrap_pause() noexcept;
void test_release_storage_bootstrap_pause() noexcept;
[[nodiscard]] bool test_inject_storage_bootstrap_allocation_failure(std::uint32_t ordinal) noexcept;
[[nodiscard]] bool test_inject_storage_bootstrap_abi_mismatch(bool enabled) noexcept;
[[nodiscard]] storage_bootstrap_attach_twice_snapshot test_attach_storage_twice() noexcept;
[[nodiscard]] bool test_corrupt_storage_bootstrap_manifest(
    shared_registry::shared_registry_test_manifest_corruption corruption) noexcept;
[[nodiscard]] bool test_detach_storage_mapping() noexcept;
#endif

/** Returns the storage metadata size, or zero for invalid configuration. */
[[nodiscard]] std::size_t storage_shared_memory_size(const storage_config& config = {}) noexcept;

/** Reserves startup registry memory and requests the storage lock. */
void request_storage_resources(const storage_config& config = {});

/** Initializes the startup registry header. Shared block storage is created on first use. */
[[nodiscard]] bool startup_storage(std::uint64_t boot_nonce, const storage_config& config = {});

/** Attaches this process to shared storage, initializing it on first use. */
[[nodiscard]] bool attach_storage();

/** Checks that this process is attached to a ready shared storage area. */
[[nodiscard]] bool storage_ready() noexcept;

/** Stores a block with a checksum, trying shared memory before private files. */
[[nodiscard]] store_block_result store_block(std::span<const std::byte> bytes);

/** Reserves space in the shared memory budget before a caller allocates from DSA. */
[[nodiscard]] shared_dsa_reserve_result
reserve_shared_dsa_allocation(std::uint64_t payload_bytes) noexcept;

/** Releases this backend's accounting reservation and clears the token on completion. */
[[nodiscard]] bool release_shared_dsa_allocation(shared_dsa_reservation* reservation) noexcept;

/** Counts this backend's pending reservation as live memory. */
[[nodiscard]] bool publish_shared_dsa_allocation(shared_dsa_reservation* reservation) noexcept;

/** Releases a stored block that this backend has not published in a manifest. */
[[nodiscard]] storage_error discard_unpublished_block(const stored_block_token& token);

/**
 * Publishes a manifest after all its blocks are ready. A successful
 * publication transfers block ownership to the manifest. On failure,
 * the caller remains responsible for discarding unpublished blocks.
 */
[[nodiscard]] publish_manifest_result publish_manifest(const manifest_descriptor& descriptor,
                                                       std::span<const stored_block_token> blocks);

/** Writes combined column blocks through PostgreSQL storage and remembers storage errors. */
class storage_column_store_physical_sink {
  public:
    using token_type = stored_block_token;

    [[nodiscard]] std::expected<token_type, format_error>
    store_physical(std::span<const std::byte> bytes) {
        const store_block_result stored = store_block(bytes);
        if (!stored) {
            last_error_ = stored.error;
            return std::unexpected(format_error::sink_failed);
        }
        last_error_ = storage_error::none;
        return stored.token;
    }

    void discard_physical(const token_type& token) noexcept {
        const storage_error discarded = discard_unpublished_block(token);
        if (discarded != storage_error::none) {
            last_error_ = discarded;
        }
    }

    [[nodiscard]] storage_error last_error() const noexcept {
        return last_error_;
    }

  private:
    storage_error last_error_ = storage_error::none;
};

/**
 * Combines logical column blocks into larger storage blocks using one
 * bounded buffer. Callers use logical handles while this sink tracks the
 * shared-memory or file blocks that will form the manifest.
 */
class storage_column_store_block_sink {
  public:
    using token_type = column_store_logical_block_token;

    explicit storage_column_store_block_sink(
        std::uint64_t maximum_physical_block_bytes = k_default_maximum_block_bytes) noexcept
        : coalesced_(physical_,
                     maximum_physical_block_bytes,
                     static_cast<std::uint32_t>(k_blocks_per_manifest)) {}

    storage_column_store_block_sink(const storage_column_store_block_sink&) = delete;
    storage_column_store_block_sink& operator=(const storage_column_store_block_sink&) = delete;
    storage_column_store_block_sink(storage_column_store_block_sink&&) = delete;
    storage_column_store_block_sink& operator=(storage_column_store_block_sink&&) = delete;

    [[nodiscard]] std::expected<token_type, format_error>
    store(column_store_block_kind kind, std::uint64_t ordinal, std::span<const std::byte> bytes) {
        return coalesced_.store(kind, ordinal, bytes);
    }

    void discard(const token_type& token) noexcept {
        coalesced_.discard(token);
    }

    [[nodiscard]] std::expected<std::vector<stored_block_token>, format_error>
    manifest_tokens(std::span<const token_type> logical_order) const {
        return coalesced_.manifest_tokens(logical_order);
    }

    void mark_published() noexcept {
        coalesced_.mark_published();
    }

    [[nodiscard]] storage_error last_storage_error() const noexcept {
        return physical_.last_error();
    }

    [[nodiscard]] std::uint32_t physical_block_count() const noexcept {
        return coalesced_.physical_block_count();
    }

    [[nodiscard]] std::uint64_t retained_buffer_bytes() const noexcept {
        return coalesced_.retained_buffer_bytes();
    }

  private:
    storage_column_store_physical_sink physical_;
    coalescing_column_store_sink<storage_column_store_physical_sink> coalesced_;
};

/**
 * Publishes the combined column blocks and transfers their cleanup to
 * the manifest. If validation or publication returns an error, it asks
 * the sink to discard the unpublished blocks.
 */
[[nodiscard]] inline publish_manifest_result publish_streamed_column_store(
    manifest_descriptor descriptor,
    storage_column_store_block_sink& sink,
    streamed_column_store_blocks<storage_column_store_block_sink>& blocks) {
    auto physical_tokens = sink.manifest_tokens(blocks.tokens());
    if (!physical_tokens.has_value()) {
        const format_error token_format_failure = physical_tokens.error();
        const storage_error sink_error = sink.last_storage_error();
        blocks.discard();
        return publish_manifest_result{
            .error = sink_error != storage_error::none ? sink_error
                     : token_format_failure == format_error::physical_block_limit_exceeded
                         ? storage_error::manifest_full
                         : storage_error::invalid_argument,
        };
    }
    if (physical_tokens->size() > k_blocks_per_manifest) {
        blocks.discard();
        return publish_manifest_result{.error = storage_error::manifest_full};
    }
    descriptor.logical_bytes = blocks.logical_bytes();
    descriptor.row_count = blocks.row_count();
    descriptor.row_group_count = blocks.row_group_count();
    publish_manifest_result published = publish_manifest(descriptor, *physical_tokens);
    if (!published) {
        blocks.discard();
        return published;
    }
    sink.mark_published();
    blocks.mark_published();
    return published;
}

/** Pins an unexpired manifest for this backend, including one already marked for retirement. */
[[nodiscard]] pin_manifest_result pin_manifest(const cache_entry_id& id, std::int64_t now_us = 0);

/**
 * Registers a waiter that started while a complete manifest was valid.
 * The named backend can convert this token to a reader pin after expiry.
 * A publisher may register on another backend's behalf while coordinating
 * publication through the cache registry.
 */
[[nodiscard]] register_manifest_waiter_result
register_manifest_waiter(const manifest_handle& handle,
                         const storage_backend_identity& owner,
                         std::int64_t registered_at_us = 0);

/** Finds this backend's sole waiter for a complete manifest, rejecting missing or duplicate waiters. */
[[nodiscard]] register_manifest_waiter_result current_manifest_waiter(const cache_entry_id& id);

/** Converts this backend's waiter on a complete manifest into a reader pin, even after expiry. */
[[nodiscard]] pin_manifest_result convert_waiter_to_pin(const manifest_waiter_token& token);

/** Releases this backend's reader pin, rejecting stale or already released pins. */
[[nodiscard]] storage_error unpin_manifest(const manifest_pin& pin) noexcept;

/**
 * Releases a registered waiter without creating a reader pin. A
 * publisher can use the token to undo a registration made on another
 * backend's behalf.
 */
[[nodiscard]] storage_error unregister_manifest_waiter(const manifest_waiter_token& token) noexcept;

/** Copies one pinned block into an exactly sized buffer and checks its checksum. */
[[nodiscard]] storage_error
read_pinned_block(const manifest_pin& pin,
                  std::size_t block_index,
                  std::span<std::byte> destination,
                  storage_read_purpose purpose = storage_read_purpose::prevalidation);

/** Clears the process-local read counters at a query boundary. */
void reset_query_io_metrics() noexcept;
[[nodiscard]] storage_query_io_snapshot query_io_metrics() noexcept;

/** Marks a manifest for cleanup after its readers and waiters release it. */
[[nodiscard]] storage_error retire_manifest(const manifest_handle& handle) noexcept;

/** Runs a limited cleanup pass over retired manifests, unfinished publications and blocks left by dead producers. */
[[nodiscard]] janitor_sweep_result janitor_sweep(std::int64_t now_us = 0, bool aggressive = false);

/** Records a cleanup request for the janitor's next wakeup. */
void request_janitor_sweep(bool aggressive = false) noexcept;

/** Records the adapter's current cache-correctness level. */
[[nodiscard]] bool publish_correctness_readiness(cache_correctness_readiness readiness) noexcept;

/** Records the janitor's lifecycle state, owner and heartbeat. */
void publish_worker_lifecycle(worker_lifecycle lifecycle, int worker_pid = 0) noexcept;
void publish_worker_heartbeat(std::int64_t now_us = 0) noexcept;

/** Copies storage counters, worker state and this process's read statistics. */
[[nodiscard]] storage_runtime_snapshot runtime_snapshot() noexcept;

/** Returns the configured cleanup interval, using startup settings before attachment. */
[[nodiscard]] std::uint32_t janitor_interval_ms() noexcept;

} // namespace bioseq::cache::pg::storage

namespace bioseq::cache::pg::storage {
namespace {

inline constexpr std::uint32_t k_storage_magic = 0x42535452U; /* BSTR */
inline constexpr char k_storage_lock_tranche[] = "bioseq_cache storage";
inline constexpr char k_store_subdirectory[] = "pg_bioseq_cache";
inline constexpr std::uint32_t k_invalid_slot = (std::numeric_limits<std::uint32_t>::max)();

enum class block_ledger_state : std::uint32_t {
    empty = 0,
    reserved,
    stored,
    referenced,
    retiring,
    release_committed,
};

enum class shared_dsa_reservation_state : std::uint32_t {
    empty = 0,
    pending,
    live,
    releasing,
};

struct shared_dsa_reservation_entry {
    shared_dsa_reservation_state state;
    std::uint32_t reserved;
    storage_backend_identity owner;
    std::uint64_t sequence;
    std::uint64_t accounted_bytes;
};

struct block_ledger_entry {
    block_ledger_state state;
    block_backing_tier reservation_tier;
    bool reservation_accounted;
    bool live_accounted;
    bool file_finalized;
    std::uint8_t reserved8;
    storage_backend_identity owner;
    std::uint32_t manifest_slot;
    std::uint32_t database_oid;
    std::uint64_t sequence;
    std::int64_t created_at_us;
    std::uint64_t payload_bytes;
    std::uint64_t reserved_bytes;
    cache_digest_256 file_digest;
    std::uint64_t file_generation;
    block_reference reference;
};

struct shared_manifest_waiter {
    bool active;
    std::uint8_t reserved8[3];
    storage_backend_identity owner;
    std::uint64_t owner_sequence;
};

struct shared_manifest_reader {
    bool active;
    std::uint8_t reserved8[3];
    storage_backend_identity owner;
    std::uint64_t owner_sequence;
};

/** Identifies the backend running a cleanup pass and that pass's sequence number. */
struct sweep_owner_token {
    storage_backend_identity owner{};
    std::uint64_t sequence = 0;

    [[nodiscard]] bool valid() const noexcept {
        return owner.valid() && sequence != 0;
    }

    [[nodiscard]] bool operator==(const sweep_owner_token&) const noexcept = default;
};

struct shared_manifest_entry {
    manifest_state state;
    cache_entry_kind kind;
    std::uint16_t block_count;
    std::uint32_t flags;
    bool manifest_counted;
    std::uint8_t reserved8[7];
    std::uint64_t slot_sequence;
    cache_entry_id id;
    std::uint64_t logical_bytes;
    std::uint64_t stored_bytes;
    std::uint64_t row_count;
    std::uint32_t row_group_count;
    std::uint32_t database_oid;
    std::int64_t created_at_us;
    std::int64_t expires_at_us;
    pg_atomic_uint64 last_access_at_us;
    pg_atomic_uint32 reader_pins;
    pg_atomic_uint32 waiter_references;
    std::array<shared_manifest_reader, k_reader_pins_per_manifest> readers;
    std::array<shared_manifest_waiter, k_waiters_per_manifest> waiters;
    std::array<std::uint32_t, k_blocks_per_manifest> ledger_slots;
    std::array<block_reference, k_blocks_per_manifest> blocks;
};

struct storage_shared_state {
    std::uint32_t magic;
    std::uint32_t abi_revision;
    std::uint64_t metadata_bytes;
    std::uint64_t dsa_initial_bytes;
    std::uint64_t hot_ceiling_bytes;
    std::uint64_t file_ceiling_bytes;
    std::uint64_t maximum_block_bytes;
    std::uint32_t sweep_interval_ms;
    std::uint32_t reclaim_batch;
    std::uint32_t soft_limit_percent;
    std::uint32_t reserved_config;
    pg_atomic_uint32 worker_lifecycle;
    pg_atomic_uint32 correctness_readiness;
    pg_atomic_uint32 last_failure;
    pg_atomic_uint64 boot_nonce;
    pg_atomic_uint64 next_block_sequence;
    pg_atomic_uint64 next_manifest_sequence;
    pg_atomic_uint64 next_waiter_sequence;
    pg_atomic_uint64 next_reader_sequence;
    pg_atomic_uint64 next_sweep_sequence;
    pg_atomic_uint64 next_file_id;
    pg_atomic_uint64 next_shared_reservation_sequence;
    pg_atomic_uint64 manifest_count;
    pg_atomic_uint64 waiter_reference_count;
    pg_atomic_uint64 reader_reference_count;
    pg_atomic_uint64 completed_sweeps;
    pg_atomic_uint64 last_sweep_at_us;
    pg_atomic_uint64 worker_heartbeat_at_us;
    pg_atomic_uint32 sweep_requested;
    pg_atomic_uint32 reserved_atomic;
#if defined(BIOSEQ_CACHE_TESTING)
    pg_atomic_uint32 force_file_tier;
#endif
    storage_backend_identity worker_owner;
    sweep_owner_token sweep_owner;
    std::uint32_t reference_scan_cursor;
    std::uint32_t block_scan_cursor;
    shared_dsa_reservation_entry shared_dsa_reservations[k_shared_dsa_reservation_capacity];
    block_ledger_entry block_ledger[k_block_ledger_capacity];
    shared_manifest_entry manifests[k_manifest_capacity];
};

static_assert(std::is_standard_layout_v<storage_shared_state>);
static_assert(alignof(storage_shared_state) <= MAXIMUM_ALIGNOF);

inline storage_shared_state* g_shared = nullptr;
inline LWLock* g_storage_lock = nullptr;
inline dsa_area* g_dsa = nullptr;
inline shared_registry::shared_registry_root* g_registry_root = nullptr;
inline shared_registry::shared_registry_accounting_access g_accounting{};
inline storage_config g_requested_config{};
inline shared_registry::shared_registry_preload_request g_registry_request{};
inline cache_correctness_readiness g_pending_correctness_readiness =
    cache_correctness_readiness::passthrough_only;
inline worker_lifecycle g_pending_worker_lifecycle = worker_lifecycle::not_registered;
inline bool g_exit_callback_registered = false;
inline storage_query_io_snapshot g_query_io_metrics{};

inline constexpr std::uint64_t k_pg_cache_minimum_available_bytes = 2ULL << 30U;

[[nodiscard]] std::uint64_t total_cache_ceiling(const storage_config& config) noexcept {
    const std::uint64_t physical = bioseq::common::memory::system_total_bytes();
    if (physical == 0) {
        return config.hot_ceiling_bytes;
    }
    const std::uint64_t physical_share =
        physical / k_cache_memory_fraction_denominator * k_cache_memory_fraction_numerator;
    return std::min(config.hot_ceiling_bytes, physical_share);
}

[[nodiscard]] constexpr std::uint64_t
fixed_registry_bytes(const shared_registry::shared_registry_preload_request& request) noexcept {
    return request.persistent.fixed_dsa_reserved_bytes;
}

[[nodiscard]] std::uint64_t
hot_payload_ceiling(const storage_config& config,
                    const shared_registry::shared_registry_preload_request& request) noexcept {
    const std::uint64_t total = total_cache_ceiling(config);
    return fixed_registry_bytes(request) <= total ? total : 0;
}

[[nodiscard]] bool config_valid(const storage_config& config) noexcept {
    return config.dsa_initial_bytes >= dsa_minimum_size() &&
           config.dsa_initial_bytes <=
               (std::numeric_limits<std::size_t>::max)() - MAXIMUM_ALIGNOF &&
           config.hot_ceiling_bytes != 0 && config.file_ceiling_bytes != 0 &&
           config.maximum_block_bytes != 0 &&
           config.maximum_block_bytes <= k_maximum_file_cache_bytes &&
           config.sweep_interval_ms >= 10 && config.reclaim_batch != 0 &&
           config.reclaim_batch <= k_manifest_capacity && config.soft_limit_percent >= 1 &&
           config.soft_limit_percent <= 100;
}

[[nodiscard]] constexpr bool same_config(const storage_config& left,
                                         const storage_config& right) noexcept {
    return left.dsa_initial_bytes == right.dsa_initial_bytes &&
           left.hot_ceiling_bytes == right.hot_ceiling_bytes &&
           left.file_ceiling_bytes == right.file_ceiling_bytes &&
           left.maximum_block_bytes == right.maximum_block_bytes &&
           left.sweep_interval_ms == right.sweep_interval_ms &&
           left.reclaim_batch == right.reclaim_batch &&
           left.soft_limit_percent == right.soft_limit_percent;
}

[[nodiscard]] shared_registry::shared_registry_preload_request
registry_preload_request(const storage_config& config) noexcept {
    shared_registry::shared_registry_preload_request failed{};
    if (!config_valid(config) || MaxBackends <= 0 ||
        static_cast<std::uint64_t>(MaxBackends) > k_maximum_transition_owners ||
        sizeof(storage_shared_state) > (std::numeric_limits<std::uint32_t>::max)()) {
        failed.error = shared_registry::shared_registry_error::invalid_configuration;
        return failed;
    }
    shared_registry::shared_registry_config registry_config{
        .max_backends = static_cast<std::uint32_t>(MaxBackends),
        .extension_metadata_bytes = static_cast<std::uint32_t>(sizeof(storage_shared_state)),
        .dsa_allocator_reserve_bytes = MAXALIGN(config.dsa_initial_bytes),
        .dsa_size_limit_bytes = (std::numeric_limits<std::uint64_t>::max)(),
    };
    auto preliminary = shared_registry::make_shared_registry_preload_request(registry_config);
    if (!preliminary) {
        return preliminary;
    }
    const std::uint64_t fixed_bytes = fixed_registry_bytes(preliminary);
    const std::uint64_t total_ceiling = total_cache_ceiling(config);
    if (fixed_bytes > total_ceiling) {
        preliminary.error = shared_registry::shared_registry_error::invalid_configuration;
        return preliminary;
    }
    registry_config.dsa_size_limit_bytes = total_ceiling;
    return shared_registry::make_shared_registry_preload_request(registry_config);
}

[[nodiscard]] pg_atomic_uint64& hot_reserved_bytes() noexcept {
    return *g_accounting.reserved_dsa_bytes;
}

[[nodiscard]] pg_atomic_uint64& file_reserved_bytes() noexcept {
    return *g_accounting.reserved_file_bytes;
}

[[nodiscard]] std::int64_t current_time_us(std::int64_t value = 0) noexcept {
    return value != 0 ? value : static_cast<std::int64_t>(GetCurrentTimestamp());
}

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept {
    pg_crc32c checksum = 0;
    INIT_CRC32C(checksum);
    if (!bytes.empty()) {
        COMP_CRC32C(checksum, bytes.data(), bytes.size());
    }
    FIN_CRC32C(checksum);
    return static_cast<std::uint32_t>(checksum);
}

struct pg_atomic_reservation_counter {
    pg_atomic_uint64* value = nullptr;

    [[nodiscard]] std::uint64_t load() noexcept {
        return pg_atomic_read_u64(value);
    }

    [[nodiscard]] bool compare_exchange(std::uint64_t& expected, std::uint64_t desired) noexcept {
        uint64 native_expected = static_cast<uint64>(expected);
        const bool exchanged =
            pg_atomic_compare_exchange_u64(value, &native_expected, static_cast<uint64>(desired));
        expected = static_cast<std::uint64_t>(native_expected);
        return exchanged;
    }

    [[nodiscard]] bool try_subtract(std::uint64_t bytes) noexcept {
        std::uint64_t observed = load();
        for (;;) {
            if (bytes > observed) {
                return false;
            }
            if (compare_exchange(observed, observed - bytes)) {
                return true;
            }
        }
    }

    void subtract(std::uint64_t bytes) noexcept {
        static_cast<void>(try_subtract(bytes));
    }
};

struct pg_system_memory_budget {
    std::uint64_t payload_ceiling = 0;
    pg_atomic_uint64* shared_reserved = nullptr;

    [[nodiscard]] std::uint64_t allocatable_bytes() const noexcept {
        const std::uint64_t available = bioseq::common::memory::system_available_bytes();
        if (available == 0) {
            return payload_ceiling;
        }
        const std::uint64_t free_headroom = available > k_pg_cache_minimum_available_bytes
                                                ? available - k_pg_cache_minimum_available_bytes
                                                : 0;
        return std::min(payload_ceiling, free_headroom);
    }

    [[nodiscard]] bool under_pressure() const noexcept {
        const std::uint64_t available = bioseq::common::memory::system_available_bytes();
        return (available != 0 && available < k_pg_cache_minimum_available_bytes) ||
               (shared_reserved != nullptr &&
                pg_atomic_read_u64(shared_reserved) > payload_ceiling);
    }

    [[nodiscard]] std::uint64_t total_bytes() const noexcept {
        return bioseq::common::memory::system_total_bytes();
    }
};

struct fixed_admission_budget {
    std::uint64_t ceiling = 0;
    bool pressure = false;

    [[nodiscard]] std::uint64_t allocatable_bytes() const noexcept {
        return ceiling;
    }

    [[nodiscard]] bool under_pressure() const noexcept {
        return pressure;
    }
};

[[nodiscard]] std::uint32_t atomic_read(const pg_atomic_uint32& value) noexcept {
    return pg_atomic_read_u32(const_cast<pg_atomic_uint32*>(&value));
}

[[nodiscard]] std::uint64_t atomic_read(const pg_atomic_uint64& value) noexcept {
    return pg_atomic_read_u64(const_cast<pg_atomic_uint64*>(&value));
}

[[nodiscard]] bool release_reservation(pg_atomic_uint64& counter, std::uint64_t bytes) noexcept {
    pg_atomic_reservation_counter adapter{.value = &counter};
    return adapter.try_subtract(bytes);
}

void remember_failure(storage_error error) noexcept {
    if (g_shared != nullptr && error != storage_error::none) {
        pg_atomic_write_u32(&g_shared->last_failure, static_cast<std::uint32_t>(error));
    }
}

[[nodiscard]] bool build_store_directory(char* output, std::size_t capacity) noexcept {
    if (DataDir == nullptr) {
        return false;
    }
    const int length = snprintf(output, capacity, "%s/%s", DataDir, k_store_subdirectory);
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

[[nodiscard]] constexpr std::uint64_t
select_storage_boot_nonce(std::uint64_t unpublished_boot_nonce,
                          std::uint64_t attached_boot_nonce) noexcept {
    return unpublished_boot_nonce != 0 ? unpublished_boot_nonce : attached_boot_nonce;
}

static_assert(select_storage_boot_nonce(0x1234U, 0) == 0x1234U);
static_assert(select_storage_boot_nonce(0, 0) == 0);

[[nodiscard]] bool build_boot_directory(char* output,
                                        std::size_t capacity,
                                        std::uint64_t unpublished_boot_nonce = 0) noexcept {
    char root[MAXPGPATH]{};
    if (!build_store_directory(root, sizeof(root))) {
        return false;
    }
    const std::uint64_t attached_boot_nonce =
        g_shared == nullptr ? 0 : pg_atomic_read_u64(&g_shared->boot_nonce);
    const std::uint64_t boot_nonce =
        select_storage_boot_nonce(unpublished_boot_nonce, attached_boot_nonce);
    if (boot_nonce == 0) {
        return false;
    }
    const int length =
        snprintf(output, capacity, "%s/%016llx", root, static_cast<unsigned long long>(boot_nonce));
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

[[nodiscard]] bool
build_database_directory(std::uint32_t database_oid, char* output, std::size_t capacity) noexcept {
    char boot[MAXPGPATH]{};
    if (database_oid == 0 || !build_boot_directory(boot, sizeof(boot))) {
        return false;
    }
    const int length = snprintf(output, capacity, "%s/%u", boot, database_oid);
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

[[nodiscard]] bool safe_owned_directory(const char* path) noexcept {
    struct stat status{};
    if (::lstat(path, &status) != 0) {
        return false;
    }
    return S_ISDIR(status.st_mode) && status.st_uid == ::geteuid() &&
           (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) == S_IRWXU;
}

[[nodiscard]] storage_error ensure_owned_directory(const char* path) noexcept {
    if (MakePGDirectory(path) != 0 && errno != EEXIST) {
        return storage_error::open_failed;
    }
    return safe_owned_directory(path) ? storage_error::none : storage_error::unsafe_path;
}

[[nodiscard]] storage_error
ensure_store_directory(std::uint64_t unpublished_boot_nonce = 0) noexcept {
    char root[MAXPGPATH]{};
    char boot[MAXPGPATH]{};
    if (!build_store_directory(root, sizeof(root)) ||
        !build_boot_directory(boot, sizeof(boot), unpublished_boot_nonce)) {
        return storage_error::path_too_long;
    }
    storage_error error = ensure_owned_directory(root);
    if (error != storage_error::none) {
        return error;
    }
    return ensure_owned_directory(boot);
}

[[nodiscard]] storage_error ensure_database_directory(std::uint32_t database_oid) noexcept {
    char directory[MAXPGPATH]{};
    if (!build_database_directory(database_oid, directory, sizeof(directory))) {
        return storage_error::path_too_long;
    }
    const storage_error root_error = ensure_store_directory();
    return root_error == storage_error::none ? ensure_owned_directory(directory) : root_error;
}

[[nodiscard]] bool is_hex(char character) noexcept {
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
}

[[nodiscard]] bool boot_directory_name(const char* name) noexcept {
    if (std::strlen(name) != 16) {
        return false;
    }
    for (std::size_t index = 0; index < 16; ++index) {
        if (!is_hex(name[index])) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] storage_error clean_orphaned_store_files() noexcept {
    char root[MAXPGPATH]{};
    if (!build_store_directory(root, sizeof(root)) || !safe_owned_directory(root)) {
        return storage_error::unsafe_path;
    }
    DIR* stream = AllocateDir(root);
    if (stream == nullptr) {
        return storage_error::open_failed;
    }
    storage_error error = storage_error::none;
    while (struct dirent* item = ReadDir(stream, root)) {
        if (std::strcmp(item->d_name, ".") == 0 || std::strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (!boot_directory_name(item->d_name)) {
            error = storage_error::unsafe_path;
            break;
        }
        char path[MAXPGPATH]{};
        const int length = snprintf(path, sizeof(path), "%s/%s", root, item->d_name);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path) ||
            !safe_owned_directory(path) || !rmtree(path, true)) {
            error = storage_error::unsafe_path;
            break;
        }
    }
    static_cast<void>(FreeDir(stream));
    return error;
}

void digest_hex(const cache_digest_256& digest, char* output) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        output[index * 2U] = digits[digest.bytes[index] >> 4U];
        output[index * 2U + 1U] = digits[digest.bytes[index] & 0x0fU];
    }
    output[digest.bytes.size() * 2U] = '\0';
}

[[nodiscard]] bool build_staging_file_path(std::uint32_t database_oid,
                                           const storage_backend_identity& owner,
                                           std::uint64_t file_id,
                                           char* output,
                                           std::size_t capacity) noexcept {
    char directory[MAXPGPATH]{};
    if (!owner.valid() || !build_database_directory(database_oid, directory, sizeof(directory))) {
        return false;
    }
    const int length = snprintf(output,
                                capacity,
                                "%s/.stage-%08x-%08x-%016llx-%016llx-%016llx.partial",
                                directory,
                                owner.proc_number,
                                static_cast<std::uint32_t>(owner.pid),
                                static_cast<unsigned long long>(owner.backend_nonce),
                                static_cast<unsigned long long>(owner.backend_started_at_us),
                                static_cast<unsigned long long>(file_id));
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

[[nodiscard]] bool build_generation_file_paths(std::uint32_t database_oid,
                                               const cache_entry_id& id,
                                               const storage_backend_identity& owner,
                                               char* temporary,
                                               std::size_t temporary_capacity,
                                               char* final,
                                               std::size_t final_capacity) noexcept {
    char directory[MAXPGPATH]{};
    char digest[65]{};
    if (!owner.valid() || id.generation == 0 ||
        !build_database_directory(database_oid, directory, sizeof(directory))) {
        return false;
    }
    digest_hex(id.digest, digest);
    const int final_length = snprintf(final,
                                      final_capacity,
                                      "%s/%s.%016llx.cache",
                                      directory,
                                      digest,
                                      static_cast<unsigned long long>(id.generation));
    const int temporary_length =
        snprintf(temporary,
                 temporary_capacity,
                 "%s/.%s.%016llx-%08x-%08x-%016llx-%016llx.partial",
                 directory,
                 digest,
                 static_cast<unsigned long long>(id.generation),
                 owner.proc_number,
                 static_cast<std::uint32_t>(owner.pid),
                 static_cast<unsigned long long>(owner.backend_nonce),
                 static_cast<unsigned long long>(owner.backend_started_at_us));
    return final_length >= 0 && temporary_length >= 0 &&
           static_cast<std::size_t>(final_length) < final_capacity &&
           static_cast<std::size_t>(temporary_length) < temporary_capacity;
}

[[nodiscard]] bool unlink_cache_file(const char* path,
                                     std::uint64_t maximum_link_count = 1) noexcept {
    struct stat status{};
    if (::lstat(path, &status) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink < 1 ||
        static_cast<std::uint64_t>(status.st_nlink) > maximum_link_count ||
        (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != (S_IRUSR | S_IWUSR)) {
        return false;
    }
    return durable_unlink(path, LOG) == 0 || errno == ENOENT;
}

[[nodiscard]] storage_error filesystem_capacity(std::uint64_t& capacity,
                                                std::uint64_t& available) noexcept {
    char directory[MAXPGPATH]{};
    if (!build_store_directory(directory, sizeof(directory))) {
        return storage_error::path_too_long;
    }
    struct statvfs status{};
    if (::statvfs(directory, &status) != 0) {
        return storage_error::filesystem_pressure;
    }
    const std::uint64_t fragment_size = status.f_frsize != 0
                                            ? static_cast<std::uint64_t>(status.f_frsize)
                                            : static_cast<std::uint64_t>(status.f_bsize);
    const auto checked_capacity =
        checked_mul(fragment_size, static_cast<std::uint64_t>(status.f_blocks));
    const auto checked_available =
        checked_mul(fragment_size, static_cast<std::uint64_t>(status.f_bavail));
    if (!checked_capacity.has_value() || !checked_available.has_value()) {
        return storage_error::filesystem_pressure;
    }
    capacity = *checked_capacity;
    available = *checked_available;
    return storage_error::none;
}

/** Combines backend identity fields into a nonzero cache nonce. */
[[nodiscard]] constexpr std::uint64_t
storage_backend_nonce(std::uint32_t proc_number, int pid, std::int64_t started_at_us) noexcept {
    const std::uint64_t nonce = static_cast<std::uint64_t>(started_at_us) ^
                                (static_cast<std::uint64_t>(pid) << 32U) ^
                                static_cast<std::uint64_t>(proc_number);
    return nonce == 0 ? 1 : nonce;
}

/** Reads this backend's identity from PostgreSQL process-local state. */
[[nodiscard]] storage_backend_identity current_backend_identity() noexcept {
    const std::uint32_t proc_number = static_cast<std::uint32_t>(MyProcNumber);
    const std::int64_t started_at = static_cast<std::int64_t>(MyStartTimestamp);
    if (MyProcPid <= 0 || proc_number == static_cast<std::uint32_t>(INVALID_PROC_NUMBER) ||
        started_at == 0) {
        return {};
    }
    return storage_backend_identity{
        .proc_number = proc_number,
        .pid = MyProcPid,
        .backend_nonce = storage_backend_nonce(proc_number, MyProcPid, started_at),
        .backend_started_at_us = started_at,
    };
}

/**
 * Checks whether the backend in a copied owner record is alive. Callers
 * probe outside storage locks, then check that the shared record still
 * matches before changing it.
 */
[[nodiscard]] bool backend_identity_alive(const storage_backend_identity& owner) noexcept {
    if (ProcGlobal == nullptr || !owner.valid() ||
        owner.proc_number >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    volatile PgBackendStatus* status =
        pgstat_get_beentry_by_proc_number(static_cast<ProcNumber>(owner.proc_number));
    if (status == nullptr) {
        return false;
    }
    int status_pid = 0;
    TimestampTz status_started_at = 0;
    bool status_stable = false;
    for (std::uint32_t attempt = 0; attempt < 16; ++attempt) {
        int before_changecount = 0;
        int after_changecount = 0;
        pgstat_begin_read_activity(status, before_changecount);
        status_pid = status->st_procpid;
        status_started_at = status->st_proc_start_timestamp;
        pgstat_end_read_activity(status, after_changecount);
        if (pgstat_read_activity_complete(before_changecount, after_changecount)) {
            status_stable = true;
            break;
        }
    }
    if (!status_stable) {
        /* Treat an owner as alive when its status keeps changing during the read. */
        return true;
    }
    PGPROC* process = ProcNumberGetProc(static_cast<ProcNumber>(owner.proc_number));
    return process != nullptr && process->pid == owner.pid && status_pid == owner.pid &&
           status_started_at == owner.backend_started_at_us &&
           storage_backend_nonce(owner.proc_number, owner.pid, status_started_at) ==
               owner.backend_nonce;
}

[[nodiscard]] bool deactivate_waiter_locked(shared_manifest_entry& manifest,
                                            shared_manifest_waiter& waiter) noexcept {
    if (!waiter.active) {
        return false;
    }
    const std::uint32_t references = pg_atomic_read_u32(&manifest.waiter_references);
    const std::uint64_t global_references = pg_atomic_read_u64(&g_shared->waiter_reference_count);
    if (references == 0 || global_references == 0) {
        return false;
    }
    waiter = shared_manifest_waiter{};
    static_cast<void>(pg_atomic_fetch_sub_u32(&manifest.waiter_references, 1));
    static_cast<void>(pg_atomic_fetch_sub_u64(&g_shared->waiter_reference_count, 1));
    return true;
}

[[nodiscard]] bool deactivate_reader_locked(shared_manifest_entry& manifest,
                                            shared_manifest_reader& reader) noexcept {
    if (!reader.active) {
        return false;
    }
    const std::uint32_t pins = pg_atomic_read_u32(&manifest.reader_pins);
    const std::uint64_t global_pins = pg_atomic_read_u64(&g_shared->reader_reference_count);
    if (pins == 0 || global_pins == 0) {
        return false;
    }
    reader = shared_manifest_reader{};
    static_cast<void>(pg_atomic_fetch_sub_u32(&manifest.reader_pins, 1));
    static_cast<void>(pg_atomic_fetch_sub_u64(&g_shared->reader_reference_count, 1));
    return true;
}

struct reaped_backend_references {
    std::uint32_t readers = 0;
    std::uint32_t waiters = 0;
};

[[nodiscard]] reaped_backend_references
reap_backend_references(const storage_backend_identity& owner) noexcept {
    reaped_backend_references reaped{};
    if (g_shared == nullptr || g_storage_lock == nullptr || !owner.valid()) {
        return reaped;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (shared_manifest_entry& manifest : g_shared->manifests) {
        if (manifest.state == manifest_state::empty) {
            continue;
        }
        for (shared_manifest_reader& reader : manifest.readers) {
            if (reader.active && reader.owner == owner &&
                deactivate_reader_locked(manifest, reader)) {
                ++reaped.readers;
            }
        }
        for (shared_manifest_waiter& waiter : manifest.waiters) {
            if (waiter.active && waiter.owner == owner &&
                deactivate_waiter_locked(manifest, waiter)) {
                ++reaped.waiters;
            }
        }
    }
    LWLockRelease(g_storage_lock);
    return reaped;
}

[[nodiscard]] std::uint64_t next_nonzero_sweep_sequence() noexcept {
    std::uint64_t sequence = pg_atomic_fetch_add_u64(&g_shared->next_sweep_sequence, 1);
    if (sequence == 0) {
        sequence = pg_atomic_fetch_add_u64(&g_shared->next_sweep_sequence, 1);
    }
    return sequence == 0 ? 1 : sequence;
}

/** Claims the cleanup pass when it has no owner or its unchanged owner record belongs to a dead backend. */
[[nodiscard]] bool acquire_sweep_ownership(sweep_owner_token& acquired) noexcept {
    const storage_backend_identity current = current_backend_identity();
    if (!current.valid()) {
        return false;
    }
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
        LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
        const sweep_owner_token observed = g_shared->sweep_owner;
        if (!observed.valid()) {
            acquired = sweep_owner_token{
                .owner = current,
                .sequence = next_nonzero_sweep_sequence(),
            };
            g_shared->sweep_owner = acquired;
            LWLockRelease(g_storage_lock);
            return true;
        }
        LWLockRelease(g_storage_lock);

        if (observed.owner == current) {
            return false;
        }
        const bool owner_alive = backend_identity_alive(observed.owner);

        LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
        if (g_shared->sweep_owner == observed) {
            if (!owner_alive) {
                acquired = sweep_owner_token{
                    .owner = current,
                    .sequence = next_nonzero_sweep_sequence(),
                };
                g_shared->sweep_owner = acquired;
                LWLockRelease(g_storage_lock);
                return true;
            }
            LWLockRelease(g_storage_lock);
            return false;
        }
        LWLockRelease(g_storage_lock);
    }
    return false;
}

/** Releases cleanup ownership only if the owner and pass sequence still match. */
void release_sweep_ownership(const sweep_owner_token& acquired) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    if (acquired.valid() && g_shared->sweep_owner == acquired) {
        g_shared->sweep_owner = {};
    }
    LWLockRelease(g_storage_lock);
}

void initialize_shared_state(storage_shared_state& shared,
                             std::size_t metadata_bytes,
                             std::uint64_t boot_nonce,
                             const storage_config& config) noexcept {
    std::memset(&shared, 0, sizeof(shared));
    shared.magic = k_storage_magic;
    shared.abi_revision = k_storage_abi_revision;
    shared.metadata_bytes = metadata_bytes;
    shared.dsa_initial_bytes = MAXALIGN(config.dsa_initial_bytes);
    shared.hot_ceiling_bytes = hot_payload_ceiling(config, g_registry_request);
    shared.file_ceiling_bytes = config.file_ceiling_bytes;
    shared.maximum_block_bytes = config.maximum_block_bytes;
    shared.sweep_interval_ms = config.sweep_interval_ms;
    shared.reclaim_batch = config.reclaim_batch;
    shared.soft_limit_percent = config.soft_limit_percent;
    pg_atomic_init_u32(&shared.worker_lifecycle,
                       static_cast<std::uint32_t>(g_pending_worker_lifecycle));
    pg_atomic_init_u32(&shared.correctness_readiness,
                       static_cast<std::uint32_t>(g_pending_correctness_readiness));
    pg_atomic_init_u32(&shared.last_failure, static_cast<std::uint32_t>(storage_error::none));
    pg_atomic_init_u64(&shared.boot_nonce, boot_nonce == 0 ? 1 : boot_nonce);
    pg_atomic_init_u64(&shared.next_block_sequence, 1);
    pg_atomic_init_u64(&shared.next_manifest_sequence, 1);
    pg_atomic_init_u64(&shared.next_waiter_sequence, 1);
    pg_atomic_init_u64(&shared.next_reader_sequence, 1);
    pg_atomic_init_u64(&shared.next_sweep_sequence, 1);
    pg_atomic_init_u64(&shared.next_file_id, 1);
    pg_atomic_init_u64(&shared.next_shared_reservation_sequence, 1);
    pg_atomic_init_u64(&shared.manifest_count, 0);
    pg_atomic_init_u64(&shared.waiter_reference_count, 0);
    pg_atomic_init_u64(&shared.reader_reference_count, 0);
    pg_atomic_init_u64(&shared.completed_sweeps, 0);
    pg_atomic_init_u64(&shared.last_sweep_at_us, 0);
    pg_atomic_init_u64(&shared.worker_heartbeat_at_us, 0);
    pg_atomic_init_u32(&shared.sweep_requested, 0);
    pg_atomic_init_u32(&shared.reserved_atomic, 0);
#if defined(BIOSEQ_CACHE_TESTING)
    pg_atomic_init_u32(&shared.force_file_tier, 0);
#endif
    shared.worker_owner = {};
    shared.sweep_owner = {};
    shared.reference_scan_cursor = 0;
    shared.block_scan_cursor = 0;
    for (shared_dsa_reservation_entry& entry : shared.shared_dsa_reservations) {
        entry = shared_dsa_reservation_entry{};
    }
    for (block_ledger_entry& entry : shared.block_ledger) {
        entry.state = block_ledger_state::empty;
        entry.manifest_slot = k_invalid_slot;
    }
    for (shared_manifest_entry& entry : shared.manifests) {
        entry.state = manifest_state::empty;
        pg_atomic_init_u64(&entry.last_access_at_us, 0);
        pg_atomic_init_u32(&entry.reader_pins, 0);
        pg_atomic_init_u32(&entry.waiter_references, 0);
        entry.ledger_slots.fill(k_invalid_slot);
    }
}

[[nodiscard]] bool validate_shared_state(const storage_shared_state& shared,
                                         std::size_t metadata_bytes,
                                         std::uint64_t boot_nonce,
                                         const storage_config& config) noexcept {
    if (shared.magic != k_storage_magic || shared.abi_revision != k_storage_abi_revision ||
        shared.metadata_bytes != metadata_bytes || metadata_bytes != sizeof(storage_shared_state) ||
        shared.dsa_initial_bytes != MAXALIGN(config.dsa_initial_bytes) ||
        shared.hot_ceiling_bytes != hot_payload_ceiling(config, g_registry_request) ||
        shared.file_ceiling_bytes != config.file_ceiling_bytes ||
        shared.maximum_block_bytes != config.maximum_block_bytes ||
        shared.sweep_interval_ms != config.sweep_interval_ms ||
        shared.reclaim_batch != config.reclaim_batch ||
        shared.soft_limit_percent != config.soft_limit_percent || shared.reserved_config != 0 ||
        pg_atomic_read_u64(const_cast<pg_atomic_uint64*>(&shared.boot_nonce)) != boot_nonce ||
        pg_atomic_read_u32(const_cast<pg_atomic_uint32*>(&shared.worker_lifecycle)) >
            static_cast<std::uint32_t>(worker_lifecycle::failed) ||
        !known_readiness(static_cast<cache_correctness_readiness>(
            pg_atomic_read_u32(const_cast<pg_atomic_uint32*>(&shared.correctness_readiness))))) {
        return false;
    }
    const auto accounting = shared_registry::shared_registry_accounting();
    if (!accounting || pg_atomic_read_u64(accounting.reserved_dsa_bytes) <
                           g_registry_request.persistent.fixed_dsa_reserved_bytes) {
        return false;
    }
    for (const shared_dsa_reservation_entry& entry : shared.shared_dsa_reservations) {
        if (static_cast<std::uint32_t>(entry.state) >
                static_cast<std::uint32_t>(shared_dsa_reservation_state::releasing) ||
            entry.reserved != 0 ||
            (entry.state == shared_dsa_reservation_state::empty
                 ? entry.sequence != 0 || entry.accounted_bytes != 0 || entry.owner.valid()
                 : entry.sequence == 0 || entry.accounted_bytes == 0 || !entry.owner.valid())) {
            return false;
        }
    }
    for (const block_ledger_entry& entry : shared.block_ledger) {
        if (static_cast<std::uint32_t>(entry.state) >
                static_cast<std::uint32_t>(block_ledger_state::release_committed) ||
            (entry.manifest_slot != k_invalid_slot && entry.manifest_slot >= k_manifest_capacity)) {
            return false;
        }
    }
    for (const shared_manifest_entry& entry : shared.manifests) {
        if (static_cast<std::uint32_t>(entry.state) >
                static_cast<std::uint32_t>(manifest_state::retiring) ||
            entry.block_count > k_blocks_per_manifest ||
            atomic_read(entry.reader_pins) > k_reader_pins_per_manifest ||
            atomic_read(entry.waiter_references) > k_waiters_per_manifest) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool initialize_storage_extension(void* context,
                                                std::span<std::byte> metadata,
                                                std::uint64_t boot_nonce) noexcept {
    if (context == nullptr || metadata.size() != sizeof(storage_shared_state)) {
        return false;
    }
    const auto& config = *static_cast<const storage_config*>(context);
    const auto accounting = shared_registry::shared_registry_accounting();
    if (!accounting || g_registry_request.persistent.fixed_dsa_reserved_bytes == 0) {
        return false;
    }
    pg_atomic_reservation_counter fixed_counter{
        .value = accounting.reserved_dsa_bytes,
    };
    const pg_system_memory_budget fixed_budget{
        .payload_ceiling = total_cache_ceiling(config),
        .shared_reserved = accounting.reserved_dsa_bytes,
    };
    basic_cache_admission fixed_admission{fixed_counter, fixed_budget};
    auto fixed_permit =
        fixed_admission.try_reserve(g_registry_request.persistent.fixed_dsa_reserved_bytes);
    if (!fixed_permit.has_value()) {
        return false;
    }
    auto* shared = reinterpret_cast<storage_shared_state*>(metadata.data());
    initialize_shared_state(*shared, metadata.size(), boot_nonce, config);
    const storage_error directory_error = ensure_store_directory(boot_nonce);
    if (directory_error != storage_error::none) {
        pg_atomic_write_u32(&shared->last_failure, static_cast<std::uint32_t>(directory_error));
        return false;
    }
    const storage_error cleanup_error = clean_orphaned_store_files();
    const storage_error recreate_error =
        cleanup_error == storage_error::none ? ensure_store_directory(boot_nonce) : cleanup_error;
    if (recreate_error != storage_error::none) {
        pg_atomic_write_u32(&shared->last_failure, static_cast<std::uint32_t>(recreate_error));
        return false;
    }
    const bool valid = validate_shared_state(*shared, metadata.size(), boot_nonce, config);
    if (valid) {
        static_cast<void>(fixed_permit->disown());
    }
    return valid;
}

[[nodiscard]] bool validate_storage_extension(void* context,
                                              std::span<std::byte> metadata,
                                              std::uint64_t boot_nonce) noexcept {
    if (context == nullptr || metadata.size() != sizeof(storage_shared_state)) {
        return false;
    }
    if (g_storage_lock != nullptr) {
        LWLockAcquire(g_storage_lock, LW_SHARED);
    }
    const bool valid =
        validate_shared_state(*reinterpret_cast<const storage_shared_state*>(metadata.data()),
                              metadata.size(),
                              boot_nonce,
                              *static_cast<const storage_config*>(context));
    if (g_storage_lock != nullptr) {
        LWLockRelease(g_storage_lock);
    }
    return valid;
}

[[nodiscard]] shared_registry::shared_registry_callbacks storage_registry_callbacks() noexcept {
    return shared_registry::shared_registry_callbacks{
        .context = &g_requested_config,
        .initialize_extension = initialize_storage_extension,
        .validate_extension = validate_storage_extension,
    };
}

void storage_backend_exit(int, Datum) {
    if (g_shared != nullptr && g_storage_lock != nullptr) {
        static_cast<void>(reap_backend_references(current_backend_identity()));
    }
    g_shared = nullptr;
    g_registry_root = nullptr;
    g_accounting = {};
    g_dsa = nullptr;
}

[[nodiscard]] bool same_id(const cache_entry_id& left, const cache_entry_id& right) noexcept {
    return left == right;
}

[[nodiscard]] bool valid_stored_reference(const block_reference& reference,
                                          std::uint64_t payload_bytes) noexcept {
    const bool known_tier =
        reference.tier == block_backing_tier::dsa || reference.tier == block_backing_tier::file;
    const bool valid_handle =
        reference.tier == block_backing_tier::file
            ? reference.handle_or_file_id != 0
            : DsaPointerIsValid(static_cast<dsa_pointer>(reference.handle_or_file_id));
    const auto end = checked_add(reference.offset, reference.length);
    return known_tier && valid_handle && reference.flags == 0 && reference.reserved == 0 &&
           reference.reserved2 == 0 && end.has_value() &&
           (reference.tier == block_backing_tier::file || reference.offset == 0) &&
           reference.length != 0 && reference.length == payload_bytes;
}

[[nodiscard]] bool handle_matches(const shared_manifest_entry& entry,
                                  const manifest_handle& handle) noexcept {
    return handle.slot < k_manifest_capacity && entry.state != manifest_state::empty &&
           entry.slot_sequence == handle.slot_sequence && same_id(entry.id, handle.id);
}

[[nodiscard]] bool waiter_token_matches(const shared_manifest_waiter& waiter,
                                        const manifest_waiter_token& token) noexcept {
    return token.waiter_slot < k_waiters_per_manifest && waiter.owner == token.owner &&
           waiter.owner_sequence == token.owner_sequence;
}

[[nodiscard]] bool reader_pin_matches(const shared_manifest_reader& reader,
                                      const manifest_pin& pin) noexcept {
    return pin.reader_slot < k_reader_pins_per_manifest && reader.active &&
           reader.owner == pin.owner && reader.owner_sequence == pin.owner_sequence;
}

[[nodiscard]] storage_error allocate_ledger_slot(std::uint32_t& slot,
                                                 std::uint64_t& sequence,
                                                 const storage_backend_identity& owner,
                                                 std::uint64_t bytes) noexcept {
    if (!owner.valid()) {
        return storage_error::invalid_argument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (std::uint32_t index = 0; index < k_block_ledger_capacity; ++index) {
        block_ledger_entry& entry = g_shared->block_ledger[index];
        if (entry.state != block_ledger_state::empty) {
            continue;
        }
        sequence = pg_atomic_fetch_add_u64(&g_shared->next_block_sequence, 1);
        if (sequence == 0) {
            sequence = pg_atomic_fetch_add_u64(&g_shared->next_block_sequence, 1);
        }
        entry = block_ledger_entry{};
        entry.state = block_ledger_state::reserved;
        entry.owner = owner;
        entry.manifest_slot = k_invalid_slot;
        entry.sequence = sequence;
        entry.created_at_us = current_time_us();
        entry.database_oid = static_cast<std::uint32_t>(MyDatabaseId);
        entry.payload_bytes = bytes;
        slot = index;
        LWLockRelease(g_storage_lock);
        return storage_error::none;
    }
    LWLockRelease(g_storage_lock);
    return storage_error::ledger_full;
}

void clear_unreserved_ledger_slot(std::uint32_t slot,
                                  std::uint64_t sequence,
                                  const storage_backend_identity& owner) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    block_ledger_entry& entry = g_shared->block_ledger[slot];
    if (entry.sequence == sequence && entry.owner == owner &&
        entry.state == block_ledger_state::reserved && !entry.reservation_accounted &&
        entry.reference.length == 0) {
        entry = block_ledger_entry{};
        entry.manifest_slot = k_invalid_slot;
    }
    LWLockRelease(g_storage_lock);
}

[[nodiscard]] storage_error mark_ledger_reservation(std::uint32_t slot,
                                                    std::uint64_t sequence,
                                                    const storage_backend_identity& owner,
                                                    block_backing_tier tier,
                                                    std::uint64_t accounted_bytes) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    block_ledger_entry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence || entry.owner != owner ||
        entry.state != block_ledger_state::reserved || entry.reservation_accounted ||
        accounted_bytes == 0 || entry.reference.length != 0) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    entry.reservation_tier = tier;
    entry.reservation_accounted = true;
    entry.reserved_bytes = accounted_bytes;
    LWLockRelease(g_storage_lock);
    return storage_error::none;
}

[[nodiscard]] storage_error cancel_ledger_reservation(std::uint32_t slot,
                                                      std::uint64_t sequence,
                                                      const storage_backend_identity& owner,
                                                      block_backing_tier tier) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    block_ledger_entry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence || entry.owner != owner ||
        entry.state != block_ledger_state::reserved || !entry.reservation_accounted ||
        entry.reservation_tier != tier || entry.reference.length != 0) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    entry.reservation_accounted = false;
    entry.reserved_bytes = 0;
    LWLockRelease(g_storage_lock);
    return storage_error::none;
}

[[nodiscard]] storage_error publish_stored_ledger_block(std::uint32_t slot,
                                                        std::uint64_t sequence,
                                                        const storage_backend_identity& owner,
                                                        const block_reference& reference) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    block_ledger_entry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence || entry.owner != owner ||
        entry.state != block_ledger_state::reserved) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    if (entry.reference.length != 0 &&
        std::memcmp(&entry.reference, &reference, sizeof(reference)) != 0) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    entry.reference = reference;
    if (reference.tier == block_backing_tier::dsa && !entry.live_accounted) {
        const auto desired = checked_add(
            static_cast<std::uint64_t>(pg_atomic_read_u64(&g_registry_root->hot_live_bytes)),
            entry.reserved_bytes);
        if (!desired.has_value()) {
            LWLockRelease(g_storage_lock);
            return storage_error::hot_tier_full;
        }
        static_cast<void>(
            pg_atomic_fetch_add_u64(&g_registry_root->hot_live_bytes, entry.reserved_bytes));
        entry.live_accounted = true;
    }
    pg_write_barrier();
    entry.state = block_ledger_state::stored;
    LWLockRelease(g_storage_lock);
    return storage_error::none;
}

[[nodiscard]] storage_error record_reserved_block(std::uint32_t slot,
                                                  std::uint64_t sequence,
                                                  const storage_backend_identity& owner,
                                                  const block_reference& reference) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    block_ledger_entry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence || entry.owner != owner ||
        entry.state != block_ledger_state::reserved || entry.reference.length != 0) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    entry.reference = reference;
    LWLockRelease(g_storage_lock);
    return storage_error::none;
}

[[nodiscard]] inline ssize_t postgres_read_at(
    void*, int descriptor, void* destination, std::size_t bytes, off_t offset) noexcept {
    return pg_pread(descriptor, destination, bytes, offset);
}

[[nodiscard]] inline ssize_t postgres_write_at(
    void*, int descriptor, const void* source, std::size_t bytes, off_t offset) noexcept {
    return pg_pwrite(descriptor, source, bytes, offset);
}

inline void poll_postgres_interrupts(void*) noexcept {
    CHECK_FOR_INTERRUPTS();
}

[[nodiscard]] inline constexpr bioseq::common::durable_file_io::positional_io_callbacks
postgres_positional_io() noexcept {
    return {
        .context = nullptr,
        .read_at = postgres_read_at,
        .write_at = postgres_write_at,
        .poll = poll_postgres_interrupts,
    };
}

[[nodiscard]] inline storage_error
write_all_at(int descriptor, std::span<const std::byte> bytes, std::uint64_t offset = 0) noexcept {
    const auto written = bioseq::common::durable_file_io::write_all_at(
        descriptor, bytes, offset, postgres_positional_io());
    if (written.has_value()) {
        return storage_error::none;
    }
    return written.error().code == bioseq::common::durable_file_io::file_error_code::offset_overflow
               ? storage_error::invalid_argument
               : storage_error::write_failed;
}

[[nodiscard]] storage_error store_file_block(std::span<const std::byte> bytes,
                                             std::uint32_t database_oid,
                                             const storage_backend_identity& owner,
                                             std::uint64_t file_id,
                                             block_reference& reference) noexcept {
    if (ensure_database_directory(database_oid) != storage_error::none) {
        return storage_error::unsafe_path;
    }
    char temporary[MAXPGPATH]{};
    if (!build_staging_file_path(database_oid, owner, file_id, temporary, sizeof(temporary))) {
        return storage_error::path_too_long;
    }
    int flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    volatile int descriptor = -1;
    storage_error error = storage_error::none;
    PG_TRY();
    {
        descriptor = OpenTransientFilePerm(temporary, flags, S_IRUSR | S_IWUSR);
        if (descriptor < 0) {
            error = storage_error::open_failed;
        }
        struct stat status{};
        if (error == storage_error::none &&
            (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
             status.st_uid != ::geteuid() || status.st_nlink != 1 ||
             (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != (S_IRUSR | S_IWUSR))) {
            error = storage_error::unsafe_path;
        }
        if (error == storage_error::none) {
            error = write_all_at(descriptor, bytes);
        }
        if (descriptor >= 0 && CloseTransientFile(descriptor) != 0 &&
            error == storage_error::none) {
            error = storage_error::sync_failed;
        }
        descriptor = -1;
    }
    PG_CATCH();
    {
        if (descriptor >= 0) {
            static_cast<void>(CloseTransientFile(descriptor));
        }
        static_cast<void>(durable_unlink(temporary, LOG));
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (error != storage_error::none) {
        static_cast<void>(durable_unlink(temporary, LOG));
        return error;
    }
    reference.tier = block_backing_tier::file;
    reference.handle_or_file_id = file_id;
    reference.offset = 0;
    reference.length = bytes.size();
    reference.crc32c = crc32c(bytes);
    return storage_error::none;
}

[[nodiscard]] inline storage_error
read_all_at(int descriptor, std::uint64_t offset, std::span<std::byte> destination) noexcept {
    const auto read = bioseq::common::durable_file_io::read_exact_at(
        descriptor, offset, destination, postgres_positional_io());
    if (read.has_value()) {
        return storage_error::none;
    }
    switch (read.error().code) {
    case bioseq::common::durable_file_io::file_error_code::offset_overflow:
        return storage_error::invalid_argument;
    case bioseq::common::durable_file_io::file_error_code::unexpected_end_of_file:
        return storage_error::short_read;
    default:
        return storage_error::read_failed;
    }
}

[[nodiscard]] storage_error read_file_block(std::uint32_t database_oid,
                                            const cache_entry_id& id,
                                            const block_reference& reference,
                                            std::span<std::byte> destination) noexcept {
    char temporary[MAXPGPATH]{};
    char final[MAXPGPATH]{};
    if (!build_generation_file_paths(database_oid,
                                     id,
                                     current_backend_identity(),
                                     temporary,
                                     sizeof(temporary),
                                     final,
                                     sizeof(final))) {
        return storage_error::path_too_long;
    }
    int flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    volatile int descriptor = OpenTransientFile(final, flags);
    if (descriptor < 0) {
        return errno == ENOENT ? storage_error::not_found : storage_error::open_failed;
    }
    storage_error error = storage_error::none;
    bioseq::common::durable_file_io::regular_file_identity identity_before{};
    bool captured_identity = false;
    PG_TRY();
    {
        const std::uint64_t end =
            reference.offset <= (std::numeric_limits<std::uint64_t>::max)() - reference.length
                ? reference.offset + reference.length
                : (std::numeric_limits<std::uint64_t>::max)();
        {
            const auto captured =
                bioseq::common::durable_file_io::capture_regular_file_identity(descriptor);
            if (!captured.has_value() || captured->owner != ::geteuid() ||
                captured->link_count != 1 ||
                (captured->mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != (S_IRUSR | S_IWUSR) ||
                captured->size < end) {
                error = storage_error::unsafe_path;
            } else {
                identity_before = *captured;
                captured_identity = true;
            }
        }
        if (error == storage_error::none) {
            error = read_all_at(descriptor, reference.offset, destination);
        }
        if (error == storage_error::none && captured_identity) {
            const auto captured =
                bioseq::common::durable_file_io::capture_regular_file_identity(descriptor);
            if (!captured.has_value() || *captured != identity_before) {
                error = storage_error::read_failed;
            }
        }
        if (CloseTransientFile(descriptor) != 0 && error == storage_error::none) {
            error = storage_error::read_failed;
        }
        descriptor = -1;
    }
    PG_CATCH();
    {
        if (descriptor >= 0) {
            static_cast<void>(CloseTransientFile(descriptor));
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    return error;
}

[[nodiscard]] storage_error release_block_slot(std::uint32_t slot,
                                               std::uint64_t sequence,
                                               janitor_sweep_result* result = nullptr) noexcept {
    if (slot >= k_block_ledger_capacity) {
        return storage_error::invalid_argument;
    }
    block_reference reference{};
    std::uint64_t reserved_bytes = 0;
    storage_backend_identity owner{};
    std::uint32_t database_oid = 0;
    cache_digest_256 file_digest{};
    std::uint64_t file_generation = 0;
    bool file_finalized = false;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    block_ledger_entry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence ||
        (entry.state != block_ledger_state::stored && entry.state != block_ledger_state::retiring &&
         !(entry.state == block_ledger_state::reserved && entry.reference.length != 0))) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    if (!entry.reservation_accounted || entry.reservation_tier != entry.reference.tier ||
        !valid_stored_reference(entry.reference, entry.payload_bytes)) {
        LWLockRelease(g_storage_lock);
        return storage_error::invalid_state;
    }
    /* Mark the release before freeing memory or deleting files. If the
     * process dies during the release, later cleanup leaves this record
     * alone to avoid freeing the same allocation twice. */
    entry.state = block_ledger_state::release_committed;
    reference = entry.reference;
    reserved_bytes = entry.reserved_bytes;
    owner = entry.owner;
    database_oid = entry.database_oid;
    file_digest = entry.file_digest;
    file_generation = entry.file_generation;
    file_finalized = entry.file_finalized;
    const block_backing_tier reservation_tier = entry.reservation_tier;
    const bool reservation_accounted = entry.reservation_accounted;
    const bool live_accounted = entry.live_accounted;
    LWLockRelease(g_storage_lock);

    /* Other cleanup passes skip this block while its release is in progress. */
    if (reference.tier == block_backing_tier::dsa &&
        DsaPointerIsValid(static_cast<dsa_pointer>(reference.handle_or_file_id))) {
        dsa_free(g_dsa, static_cast<dsa_pointer>(reference.handle_or_file_id));
        if (live_accounted) {
            static_cast<void>(release_reservation(g_registry_root->hot_live_bytes, reserved_bytes));
        }
        if (reservation_accounted && reservation_tier == block_backing_tier::dsa) {
            static_cast<void>(release_reservation(hot_reserved_bytes(), reserved_bytes));
        }
        if (result != nullptr && reservation_accounted &&
            reservation_tier == block_backing_tier::dsa) {
            result->reclaimed_hot_bytes += reserved_bytes;
        }
    } else if (reference.tier == block_backing_tier::file) {
        char temporary[MAXPGPATH]{};
        char final[MAXPGPATH]{};
        bool released = false;
        if (file_finalized) {
            const cache_entry_id id{
                .digest = file_digest,
                .boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce),
                .generation = file_generation,
                .claim_sequence = 1,
            };
            released = build_generation_file_paths(
                database_oid, id, owner, temporary, sizeof(temporary), final, sizeof(final));
            if (released) {
                static_cast<void>(unlink_cache_file(temporary));
                released = unlink_cache_file(final);
            }
        } else {
            released = build_staging_file_path(
                database_oid, owner, reference.handle_or_file_id, temporary, sizeof(temporary));
            if (released) {
                released = unlink_cache_file(temporary);
            }
        }
        if (!released) {
            LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
            block_ledger_entry& failed = g_shared->block_ledger[slot];
            if (failed.sequence == sequence &&
                failed.state == block_ledger_state::release_committed) {
                failed.state = block_ledger_state::retiring;
            }
            LWLockRelease(g_storage_lock);
            return storage_error::write_failed;
        }
        if (reservation_accounted && reservation_tier == block_backing_tier::file) {
            static_cast<void>(release_reservation(file_reserved_bytes(), reserved_bytes));
        }
        if (result != nullptr && reservation_accounted &&
            reservation_tier == block_backing_tier::file) {
            result->reclaimed_file_bytes += reserved_bytes;
        }
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    block_ledger_entry& completed = g_shared->block_ledger[slot];
    if (completed.sequence != sequence ||
        completed.state != block_ledger_state::release_committed) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    completed = block_ledger_entry{};
    completed.manifest_slot = k_invalid_slot;
    LWLockRelease(g_storage_lock);
    if (result != nullptr) {
        ++result->reclaimed_blocks;
    }
    return storage_error::none;
}

struct manifest_file_finalization {
    storage_error error = storage_error::none;
    std::array<block_reference, k_blocks_per_manifest> references{};
};

/**
 * Publishes an already-synced file without replacing an existing result.
 * C++ filesystem objects stay inside this helper, which makes no
 * PostgreSQL calls. They are destroyed before the caller resumes work
 * that could raise a PostgreSQL error. Staging files belong to one
 * backend, and ledger state controls when the janitor may remove them.
 */
[[nodiscard]] inline storage_error publish_manifest_file_no_replace(
    std::string_view directory,
    std::string_view temporary,
    std::string_view final,
    const bioseq::common::durable_file_io::regular_file_identity& expected_identity) noexcept {
    try {
        auto directory_opened =
            bioseq::common::durable_file_io::open_directory_descriptor(std::filesystem::path{directory});
        if (!directory_opened.has_value()) {
            return storage_error::unsafe_path;
        }
        bioseq::common::durable_file_io::file_descriptor directory_descriptor =
            std::move(*directory_opened);
        const std::string temporary_name = std::filesystem::path{temporary}.filename().string();
        const std::string final_name = std::filesystem::path{final}.filename().string();
        const auto identity = bioseq::common::durable_file_io::matches_regular_file_identity_at(
            directory_descriptor.get(), temporary_name, expected_identity);
        if (!identity.has_value() || !*identity) {
            return storage_error::unsafe_path;
        }
        const auto linked =
            bioseq::common::durable_file_io::link_file_no_replace_at(directory_descriptor.get(),
                                                             temporary_name,
                                                             directory_descriptor.get(),
                                                             final_name,
                                                             {},
                                                             &expected_identity);
        if (!linked.has_value()) {
            const auto temporary_removed = bioseq::common::durable_file_io::remove_file_if_same_inode_at(
                directory_descriptor.get(), temporary_name, expected_identity);
            const auto cleanup_synced =
                bioseq::common::durable_file_io::fsync_directory_descriptor(directory_descriptor.get());
            if (!temporary_removed.has_value() ||
                *temporary_removed !=
                    bioseq::common::durable_file_io::conditional_remove_disposition::removed ||
                !cleanup_synced.has_value()) {
                return storage_error::unsafe_path;
            }
            if (linked.error().publication !=
                    bioseq::common::durable_file_io::publication_state::unchanged ||
                linked.error().code == bioseq::common::durable_file_io::file_error_code::identity_changed) {
                return storage_error::unsafe_path;
            }
            return linked.error().code == bioseq::common::durable_file_io::file_error_code::destination_exists
                       ? storage_error::duplicate_generation
                       : storage_error::rename_failed;
        }
        const auto temporary_removed = bioseq::common::durable_file_io::remove_file_if_same_inode_at(
            directory_descriptor.get(),
            temporary_name,
            expected_identity,
            bioseq::common::durable_file_io::publication_state::visible_not_durable);
        if (!temporary_removed.has_value() ||
            *temporary_removed !=
                bioseq::common::durable_file_io::conditional_remove_disposition::removed) {
            const auto final_removed = bioseq::common::durable_file_io::remove_file_if_same_inode_at(
                directory_descriptor.get(),
                final_name,
                expected_identity,
                bioseq::common::durable_file_io::publication_state::visible_not_durable);
            const auto reconciled = bioseq::common::durable_file_io::fsync_directory_descriptor(
                directory_descriptor.get(),
                {},
                bioseq::common::durable_file_io::publication_state::visible_not_durable);
            if (!final_removed.has_value() ||
                *final_removed ==
                    bioseq::common::durable_file_io::conditional_remove_disposition::identity_mismatch ||
                !reconciled.has_value()) {
                return storage_error::unsafe_path;
            }
            return storage_error::unsafe_path;
        }
        const auto synced = bioseq::common::durable_file_io::fsync_directory_descriptor(
            directory_descriptor.get(),
            {},
            bioseq::common::durable_file_io::publication_state::visible_not_durable);
        if (!synced.has_value()) {
            // Keep the first writeback failure. A later successful fsync cannot
            // establish that the failed publication was durable.
            const auto removed = bioseq::common::durable_file_io::remove_file_if_same_inode_at(
                directory_descriptor.get(),
                final_name,
                expected_identity,
                bioseq::common::durable_file_io::publication_state::visible_not_durable);
            const auto rollback_synced =
                bioseq::common::durable_file_io::fsync_directory_descriptor(directory_descriptor.get());
            if (!removed.has_value() ||
                *removed ==
                    bioseq::common::durable_file_io::conditional_remove_disposition::identity_mismatch ||
                !rollback_synced.has_value()) {
                return storage_error::unsafe_path;
            }
            return storage_error::sync_failed;
        }
        return storage_error::none;
    } catch (...) {
        return storage_error::unsafe_path;
    }
}

[[nodiscard]] inline storage_error remove_cache_file_durably(
    std::string_view path,
    std::uint64_t maximum_link_count = 1,
    const bioseq::common::durable_file_io::regular_file_identity* expected_identity = nullptr) noexcept {
    try {
        const std::filesystem::path file_path{path};
        const std::filesystem::path parent =
            file_path.parent_path().empty() ? std::filesystem::path{"."} : file_path.parent_path();
        const std::string name = file_path.filename().string();
        auto directory_opened = bioseq::common::durable_file_io::open_directory_descriptor(parent);
        if (!directory_opened.has_value()) {
            return storage_error::unsafe_path;
        }
        bioseq::common::durable_file_io::file_descriptor directory_descriptor =
            std::move(*directory_opened);
        const auto identity = bioseq::common::durable_file_io::capture_regular_file_identity_at(
            directory_descriptor.get(), name);
        if (!identity.has_value()) {
            if (identity.error().system_error != ENOENT) {
                return storage_error::unsafe_path;
            }
            return bioseq::common::durable_file_io::fsync_directory_descriptor(directory_descriptor.get())
                           .has_value()
                       ? storage_error::none
                       : storage_error::unsafe_path;
        }
        if (identity->owner != ::geteuid() || identity->link_count < 1 ||
            identity->link_count > maximum_link_count ||
            (identity->mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != (S_IRUSR | S_IWUSR) ||
            (expected_identity != nullptr && (identity->device != expected_identity->device ||
                                              identity->inode != expected_identity->inode))) {
            return storage_error::unsafe_path;
        }
        const auto removed = bioseq::common::durable_file_io::remove_file_if_same_inode_at(
            directory_descriptor.get(),
            name,
            *identity,
            bioseq::common::durable_file_io::publication_state::visible_not_durable);
        const auto synced = bioseq::common::durable_file_io::fsync_directory_descriptor(
            directory_descriptor.get(),
            {},
            bioseq::common::durable_file_io::publication_state::visible_not_durable);
        if (!removed.has_value() ||
            *removed ==
                bioseq::common::durable_file_io::conditional_remove_disposition::identity_mismatch ||
            !synced.has_value()) {
            return storage_error::unsafe_path;
        }
        return storage_error::none;
    } catch (...) {
        return storage_error::unsafe_path;
    }
}

/** Checks block ownership, slot sequences and duplicates before opening manifest files. */
[[nodiscard]] storage_error
validate_manifest_tokens_before_io(const manifest_descriptor& descriptor,
                                   std::span<const stored_block_token> blocks,
                                   const storage_backend_identity& owner) noexcept {
    std::uint64_t stored_bytes = 0;
    LWLockAcquire(g_storage_lock, LW_SHARED);
    for (const shared_manifest_entry& manifest : g_shared->manifests) {
        if (manifest.state != manifest_state::empty &&
            (same_id(manifest.id, descriptor.id) ||
             (manifest.id.digest == descriptor.id.digest &&
              manifest.id.generation == descriptor.id.generation))) {
            LWLockRelease(g_storage_lock);
            return storage_error::duplicate_generation;
        }
    }
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const stored_block_token& token = blocks[index];
        if (!token || token.ledger_slot >= k_block_ledger_capacity || token.owner != owner) {
            LWLockRelease(g_storage_lock);
            return storage_error::invalid_argument;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (blocks[previous].ledger_slot == token.ledger_slot) {
                LWLockRelease(g_storage_lock);
                return storage_error::invalid_argument;
            }
        }
        const block_ledger_entry& entry = g_shared->block_ledger[token.ledger_slot];
        const auto next_stored = checked_add(stored_bytes, entry.payload_bytes);
        const bool file_identity_clear =
            std::all_of(entry.file_digest.bytes.begin(),
                        entry.file_digest.bytes.end(),
                        [](std::uint8_t value) noexcept { return value == 0; });
        if (entry.sequence != token.sequence || entry.owner != owner ||
            entry.state != block_ledger_state::stored || entry.manifest_slot != k_invalid_slot ||
            entry.database_oid != static_cast<std::uint32_t>(MyDatabaseId) ||
            !entry.reservation_accounted || entry.file_finalized || entry.file_generation != 0 ||
            !file_identity_clear || entry.reserved_bytes == 0 ||
            entry.live_accounted != (entry.reservation_tier == block_backing_tier::dsa) ||
            entry.reservation_tier != entry.reference.tier ||
            !valid_stored_reference(entry.reference, entry.payload_bytes) ||
            std::memcmp(&entry.reference, &token.reference, sizeof(block_reference)) != 0 ||
            !next_stored.has_value() || *next_stored > k_maximum_column_store_bytes) {
            LWLockRelease(g_storage_lock);
            return storage_error::stale_token;
        }
        stored_bytes = *next_stored;
    }
    LWLockRelease(g_storage_lock);
    return storage_error::none;
}

/** Combines staged file blocks in manifest order, checks their checksums and publishes the result file. */
[[nodiscard]] manifest_file_finalization
finalize_manifest_file(const manifest_descriptor& descriptor,
                       std::span<const stored_block_token> blocks,
                       const storage_backend_identity& owner) {
    manifest_file_finalization result{};
    bool has_file = false;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        result.references[index] = blocks[index].reference;
        has_file = has_file || blocks[index].reference.tier == block_backing_tier::file;
    }
    if (!has_file) {
        return result;
    }
    const std::uint32_t database_oid = static_cast<std::uint32_t>(MyDatabaseId);
    if (database_oid == 0) {
        result.error = storage_error::unsafe_path;
        return result;
    }
    char directory[MAXPGPATH]{};
    char temporary[MAXPGPATH]{};
    char final[MAXPGPATH]{};
    if (!build_database_directory(database_oid, directory, sizeof(directory)) ||
        !build_generation_file_paths(database_oid,
                                     descriptor.id,
                                     owner,
                                     temporary,
                                     sizeof(temporary),
                                     final,
                                     sizeof(final))) {
        result.error = storage_error::path_too_long;
        return result;
    }
    struct stat existing{};
    if (::lstat(final, &existing) == 0 || errno != ENOENT) {
        result.error = storage_error::duplicate_generation;
        return result;
    }
    if (ensure_database_directory(database_oid) != storage_error::none) {
        result.error = storage_error::unsafe_path;
        return result;
    }

    int destination_flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
    int source_flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK;
#if defined(O_NOFOLLOW)
    destination_flags |= O_NOFOLLOW;
    source_flags |= O_NOFOLLOW;
#endif
    volatile int destination = -1;
    volatile int source = -1;
    std::uint64_t file_offset = 0;
    bioseq::common::durable_file_io::regular_file_identity destination_identity{};
    bool destination_identity_captured = false;
    PG_TRY();
    {
        destination = OpenTransientFilePerm(temporary, destination_flags, S_IRUSR | S_IWUSR);
        if (destination < 0) {
            result.error = storage_error::open_failed;
        }
        std::array<std::byte, 64U << 10U> buffer{};
        for (std::size_t index = 0; result.error == storage_error::none && index < blocks.size();
             ++index) {
            const stored_block_token& token = blocks[index];
            if (token.reference.tier != block_backing_tier::file) {
                continue;
            }
            char staging[MAXPGPATH]{};
            if (!build_staging_file_path(database_oid,
                                         owner,
                                         token.reference.handle_or_file_id,
                                         staging,
                                         sizeof(staging))) {
                result.error = storage_error::path_too_long;
                break;
            }
            source = OpenTransientFile(staging, source_flags);
            if (source < 0) {
                result.error = storage_error::open_failed;
                break;
            }
            bioseq::common::durable_file_io::regular_file_identity source_identity{};
            bool source_identity_captured = false;
            if (result.error == storage_error::none) {
                const auto captured =
                    bioseq::common::durable_file_io::capture_regular_file_identity(source);
                if (!captured.has_value() || captured->owner != ::geteuid() ||
                    captured->link_count != 1 ||
                    (captured->mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != (S_IRUSR | S_IWUSR) ||
                    captured->size != token.reference.length) {
                    result.error = storage_error::unsafe_path;
                } else {
                    source_identity = *captured;
                    source_identity_captured = true;
                }
            }
            pg_crc32c checksum = 0;
            INIT_CRC32C(checksum);
            std::uint64_t copied = 0;
            while (result.error == storage_error::none && copied < token.reference.length) {
                CHECK_FOR_INTERRUPTS();
                const std::size_t chunk = static_cast<std::size_t>(std::min(
                    static_cast<std::uint64_t>(buffer.size()), token.reference.length - copied));
                result.error =
                    read_all_at(source, copied, std::span<std::byte>{buffer.data(), chunk});
                if (result.error != storage_error::none) {
                    break;
                }
                COMP_CRC32C(checksum, buffer.data(), chunk);
                result.error = write_all_at(destination,
                                            std::span<const std::byte>{buffer.data(), chunk},
                                            file_offset + copied);
                copied += chunk;
            }
            if (result.error == storage_error::none && source_identity_captured) {
                const auto captured =
                    bioseq::common::durable_file_io::capture_regular_file_identity(source);
                if (!captured.has_value() || *captured != source_identity) {
                    result.error = storage_error::read_failed;
                }
            }
            FIN_CRC32C(checksum);
            if (result.error == storage_error::none &&
                static_cast<std::uint32_t>(checksum) != token.reference.crc32c) {
                result.error = storage_error::checksum_mismatch;
            }
            if (CloseTransientFile(source) != 0 && result.error == storage_error::none) {
                result.error = storage_error::read_failed;
            }
            source = -1;
            if (result.error != storage_error::none) {
                break;
            }
            block_reference reference = token.reference;
            reference.handle_or_file_id = descriptor.id.generation;
            reference.offset = file_offset;
            result.references[index] = reference;
            const auto next = checked_add(file_offset, reference.length);
            if (!next.has_value() || *next > k_maximum_column_store_bytes) {
                result.error = storage_error::block_too_large;
                break;
            }
            file_offset = *next;
        }
        if (result.error == storage_error::none) {
            const auto captured =
                bioseq::common::durable_file_io::capture_regular_file_identity(destination);
            if (!captured.has_value() || captured->owner != ::geteuid() ||
                captured->link_count != 1 ||
                (captured->mode & (S_IRWXU | S_IRWXG | S_IRWXO)) != (S_IRUSR | S_IWUSR) ||
                captured->size != file_offset) {
                result.error = storage_error::unsafe_path;
            } else {
                destination_identity = *captured;
                destination_identity_captured = true;
            }
        }
        if (result.error == storage_error::none &&
            !bioseq::common::durable_file_io::fsync_file_descriptor(destination).has_value()) {
            result.error = storage_error::sync_failed;
        }
        if (destination >= 0 && CloseTransientFile(destination) != 0 &&
            result.error == storage_error::none) {
            result.error = storage_error::sync_failed;
        }
        destination = -1;
    }
    PG_CATCH();
    {
        if (source >= 0) {
            static_cast<void>(CloseTransientFile(source));
        }
        if (destination >= 0) {
            static_cast<void>(CloseTransientFile(destination));
        }
        static_cast<void>(remove_cache_file_durably(temporary));
        for (const stored_block_token& token : blocks) {
            static_cast<void>(release_block_slot(token.ledger_slot, token.sequence));
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (result.error != storage_error::none) {
        if (remove_cache_file_durably(temporary) != storage_error::none) {
            result.error = storage_error::unsafe_path;
        }
        return result;
    }
    result.error =
        destination_identity_captured
            ? publish_manifest_file_no_replace(directory, temporary, final, destination_identity)
            : storage_error::unsafe_path;
    if (result.error != storage_error::none) {
        if (remove_cache_file_durably(temporary, 1, &destination_identity) != storage_error::none) {
            result.error = storage_error::unsafe_path;
        }
        return result;
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    bool unchanged = true;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const stored_block_token& token = blocks[index];
        const block_ledger_entry& entry = g_shared->block_ledger[token.ledger_slot];
        unchanged = unchanged && entry.sequence == token.sequence && entry.owner == owner &&
                    entry.state == block_ledger_state::stored &&
                    entry.manifest_slot == k_invalid_slot &&
                    std::memcmp(&entry.reference, &token.reference, sizeof(block_reference)) == 0;
    }
    if (unchanged) {
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const stored_block_token& token = blocks[index];
            if (token.reference.tier != block_backing_tier::file) {
                continue;
            }
            block_ledger_entry& entry = g_shared->block_ledger[token.ledger_slot];
            entry.reference = result.references[index];
            entry.file_digest = descriptor.id.digest;
            entry.file_generation = descriptor.id.generation;
            entry.file_finalized = true;
        }
    }
    LWLockRelease(g_storage_lock);
    if (!unchanged) {
        result.error =
            remove_cache_file_durably(final, 1, &destination_identity) == storage_error::none
                ? storage_error::stale_token
                : storage_error::unsafe_path;
        return result;
    }

    for (const stored_block_token& token : blocks) {
        if (token.reference.tier != block_backing_tier::file) {
            continue;
        }
        char staging[MAXPGPATH]{};
        if (build_staging_file_path(
                database_oid, owner, token.reference.handle_or_file_id, staging, sizeof(staging))) {
            if (remove_cache_file_durably(staging) != storage_error::none) {
                result.error = storage_error::unsafe_path;
                return result;
            }
        } else {
            result.error = storage_error::path_too_long;
            return result;
        }
    }
    return result;
}

[[nodiscard]] bool manifest_reclaimable(const shared_manifest_entry& entry) noexcept {
    return entry.state != manifest_state::empty && atomic_read(entry.reader_pins) == 0 &&
           atomic_read(entry.waiter_references) == 0;
}

[[nodiscard]] storage_error reclaim_manifest_slot(std::uint32_t slot,
                                                  std::uint64_t sequence,
                                                  janitor_sweep_result& result) noexcept {
    if (slot >= k_manifest_capacity) {
        return storage_error::invalid_argument;
    }
    std::array<std::uint32_t, k_blocks_per_manifest> ledger_slots{};
    std::array<std::uint64_t, k_blocks_per_manifest> ledger_sequences{};
    std::uint32_t block_count = 0;
    bool decrement_manifest_count = false;

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_manifest_entry& manifest = g_shared->manifests[slot];
    if (manifest.slot_sequence != sequence || !manifest_reclaimable(manifest)) {
        LWLockRelease(g_storage_lock);
        return storage_error::pinned;
    }
    decrement_manifest_count = manifest.manifest_counted;
    manifest.manifest_counted = false;
    manifest.state = manifest_state::retiring;
    block_count = manifest.block_count;
    for (std::uint32_t index = 0; index < block_count; ++index) {
        ledger_slots[index] = manifest.ledger_slots[index];
        if (ledger_slots[index] < k_block_ledger_capacity) {
            block_ledger_entry& block = g_shared->block_ledger[ledger_slots[index]];
            ledger_sequences[index] = block.sequence;
            if (block.state == block_ledger_state::referenced && block.manifest_slot == slot) {
                block.state = block_ledger_state::stored;
                block.manifest_slot = k_invalid_slot;
            }
        }
    }
    LWLockRelease(g_storage_lock);

    for (std::uint32_t index = 0; index < block_count; ++index) {
        if (ledger_slots[index] < k_block_ledger_capacity) {
            static_cast<void>(
                release_block_slot(ledger_slots[index], ledger_sequences[index], &result));
        }
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_manifest_entry& current = g_shared->manifests[slot];
    if (current.slot_sequence == sequence && current.state == manifest_state::retiring &&
        pg_atomic_read_u32(&current.reader_pins) == 0 &&
        pg_atomic_read_u32(&current.waiter_references) == 0) {
        current = shared_manifest_entry{};
        current.state = manifest_state::empty;
        current.ledger_slots.fill(k_invalid_slot);
        pg_atomic_init_u64(&current.last_access_at_us, 0);
        pg_atomic_init_u32(&current.reader_pins, 0);
        pg_atomic_init_u32(&current.waiter_references, 0);
        if (decrement_manifest_count) {
            static_cast<void>(pg_atomic_fetch_sub_u64(&g_shared->manifest_count, 1));
        }
    }
    LWLockRelease(g_storage_lock);
    return storage_error::none;
}

[[nodiscard]] storage_error claim_manifest_reader_locked(shared_manifest_entry& manifest,
                                                         const manifest_handle& handle,
                                                         const storage_backend_identity& owner,
                                                         manifest_pin& pin,
                                                         std::int64_t accessed_at_us) noexcept {
    if (!owner.valid()) {
        return storage_error::invalid_argument;
    }
    std::uint32_t reader_slot = k_invalid_slot;
    for (std::uint32_t index = 0; index < k_reader_pins_per_manifest; ++index) {
        if (!manifest.readers[index].active) {
            reader_slot = index;
            break;
        }
    }
    if (reader_slot == k_invalid_slot ||
        pg_atomic_read_u32(&manifest.reader_pins) == (std::numeric_limits<std::uint32_t>::max)()) {
        return storage_error::reader_full;
    }
    std::uint64_t owner_sequence = pg_atomic_fetch_add_u64(&g_shared->next_reader_sequence, 1);
    if (owner_sequence == 0) {
        owner_sequence = pg_atomic_fetch_add_u64(&g_shared->next_reader_sequence, 1);
    }
    if (owner_sequence == 0) {
        owner_sequence = 1;
    }
    shared_manifest_reader& reader = manifest.readers[reader_slot];
    reader = shared_manifest_reader{
        .active = true,
        .owner = owner,
        .owner_sequence = owner_sequence,
    };
    static_cast<void>(pg_atomic_fetch_add_u32(&manifest.reader_pins, 1));
    static_cast<void>(pg_atomic_fetch_add_u64(&g_shared->reader_reference_count, 1));
    pg_atomic_write_u64(&manifest.last_access_at_us, accessed_at_us);
    pin.handle = handle;
    pin.kind = manifest.kind;
    pin.flags = manifest.flags;
    pin.logical_bytes = manifest.logical_bytes;
    pin.row_count = manifest.row_count;
    pin.row_group_count = manifest.row_group_count;
    pin.database_oid = manifest.database_oid;
    pin.block_count = manifest.block_count;
    pin.reader_slot = reader_slot;
    pin.owner = reader.owner;
    pin.owner_sequence = reader.owner_sequence;
    for (std::uint32_t index = 0; index < manifest.block_count; ++index) {
        pin.blocks[index] = manifest.blocks[index];
    }
    return storage_error::none;
}

inline constexpr std::size_t k_owner_liveness_scan_budget = 1024;
inline constexpr std::size_t k_manifest_reference_slot_count =
    k_manifest_capacity * (k_reader_pins_per_manifest + k_waiters_per_manifest);

enum class manifest_reference_kind : std::uint8_t {
    reader = 1,
    waiter = 2,
};

/** Saves a reader or waiter record for a liveness check outside the storage lock. */
struct manifest_reference_observation {
    manifest_handle handle{};
    manifest_reference_kind kind = manifest_reference_kind::reader;
    std::uint32_t reference_slot = k_invalid_slot;
    storage_backend_identity owner{};
    std::uint64_t owner_sequence = 0;
    bool requires_liveness = true;
};

/** Saves a block record for a liveness check outside the storage lock. */
struct block_owner_observation {
    std::uint32_t ledger_slot = k_invalid_slot;
    block_ledger_entry entry{};
    storage_backend_identity owner{};
    bool requires_liveness = true;
};

template <typename p_observation_type>
void probe_observation_owners(std::span<const p_observation_type> observations,
                              std::span<bool> alive) noexcept {
    if (alive.size() < observations.size()) {
        return;
    }
    for (std::size_t index = 0; index < observations.size(); ++index) {
        alive[index] = true;
        if (!observations[index].requires_liveness) {
            alive[index] = false;
            continue;
        }
        const storage_backend_identity& owner = observations[index].owner;
        if (!owner.valid()) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (observations[prior].requires_liveness && observations[prior].owner == owner) {
                alive[index] = alive[prior];
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            alive[index] = backend_identity_alive(owner);
        }
    }
}

/** Copies a limited set of reader and waiter records, resuming after the previous scan. */
[[nodiscard]] std::size_t collect_manifest_reference_observations(
    std::span<manifest_reference_observation> observations) noexcept {
    if (observations.empty()) {
        return 0;
    }
    std::size_t count = 0;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t cursor = g_shared->reference_scan_cursor %
                           static_cast<std::uint32_t>(k_manifest_reference_slot_count);
    const std::size_t scan_count = observations.size() < k_manifest_reference_slot_count
                                       ? observations.size()
                                       : k_manifest_reference_slot_count;
    for (std::size_t scanned = 0; scanned < scan_count; ++scanned) {
        const std::uint32_t position = cursor;
        cursor = (cursor + 1U) % static_cast<std::uint32_t>(k_manifest_reference_slot_count);
        const std::uint32_t per_manifest = k_reader_pins_per_manifest + k_waiters_per_manifest;
        const std::uint32_t manifest_slot = position / per_manifest;
        const std::uint32_t local_slot = position % per_manifest;
        const shared_manifest_entry& manifest = g_shared->manifests[manifest_slot];
        if (manifest.state == manifest_state::empty) {
            continue;
        }
        const manifest_handle handle{
            .slot = manifest_slot,
            .slot_sequence = manifest.slot_sequence,
            .id = manifest.id,
        };
        if (local_slot < k_reader_pins_per_manifest) {
            const shared_manifest_reader& reader = manifest.readers[local_slot];
            if (reader.active) {
                observations[count++] = manifest_reference_observation{
                    .handle = handle,
                    .kind = manifest_reference_kind::reader,
                    .reference_slot = local_slot,
                    .owner = reader.owner,
                    .owner_sequence = reader.owner_sequence,
                };
            }
        } else {
            const std::uint32_t waiter_slot = local_slot - k_reader_pins_per_manifest;
            const shared_manifest_waiter& waiter = manifest.waiters[waiter_slot];
            if (waiter.active) {
                observations[count++] = manifest_reference_observation{
                    .handle = handle,
                    .kind = manifest_reference_kind::waiter,
                    .reference_slot = waiter_slot,
                    .owner = waiter.owner,
                    .owner_sequence = waiter.owner_sequence,
                };
            }
        }
    }
    g_shared->reference_scan_cursor = cursor;
    LWLockRelease(g_storage_lock);
    return count;
}

/** Removes dead backends' references after checking that their saved records still match. */
void reap_dead_manifest_references(janitor_sweep_result& result) noexcept {
    std::array<manifest_reference_observation, k_owner_liveness_scan_budget> observations{};
    const std::size_t count = collect_manifest_reference_observations(
        std::span<manifest_reference_observation>{observations});
    std::array<bool, k_owner_liveness_scan_budget> alive{};
    probe_observation_owners(
        std::span<const manifest_reference_observation>{observations.data(), count},
        std::span<bool>{alive.data(), count});

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (std::size_t index = 0;
         index < count && result.reaped_readers + result.reaped_waiters < g_shared->reclaim_batch;
         ++index) {
        if (alive[index]) {
            continue;
        }
        const manifest_reference_observation& observation = observations[index];
        shared_manifest_entry& manifest = g_shared->manifests[observation.handle.slot];
        if (!handle_matches(manifest, observation.handle)) {
            continue;
        }
        if (observation.kind == manifest_reference_kind::reader &&
            observation.reference_slot < k_reader_pins_per_manifest) {
            shared_manifest_reader& reader = manifest.readers[observation.reference_slot];
            if (reader.active && reader.owner == observation.owner &&
                reader.owner_sequence == observation.owner_sequence &&
                deactivate_reader_locked(manifest, reader)) {
                ++result.reaped_readers;
            }
        } else if (observation.kind == manifest_reference_kind::waiter &&
                   observation.reference_slot < k_waiters_per_manifest) {
            shared_manifest_waiter& waiter = manifest.waiters[observation.reference_slot];
            if (waiter.active && waiter.owner == observation.owner &&
                waiter.owner_sequence == observation.owner_sequence &&
                deactivate_waiter_locked(manifest, waiter)) {
                ++result.reaped_waiters;
            }
        }
    }
    LWLockRelease(g_storage_lock);
}

/** Checks that a block's state, owner and storage details have not changed since it was copied. */
[[nodiscard]] bool block_observation_matches(const block_ledger_entry& current,
                                             const block_ledger_entry& observed) noexcept {
    return current.state == observed.state &&
           current.reservation_tier == observed.reservation_tier &&
           current.reservation_accounted == observed.reservation_accounted &&
           current.live_accounted == observed.live_accounted &&
           current.file_finalized == observed.file_finalized && current.owner == observed.owner &&
           current.manifest_slot == observed.manifest_slot &&
           current.database_oid == observed.database_oid && current.sequence == observed.sequence &&
           current.created_at_us == observed.created_at_us &&
           current.payload_bytes == observed.payload_bytes &&
           current.reserved_bytes == observed.reserved_bytes &&
           current.file_digest == observed.file_digest &&
           current.file_generation == observed.file_generation &&
           std::memcmp(&current.reference, &observed.reference, sizeof(block_reference)) == 0;
}

/** Copies a limited set of producer-owned or retiring blocks for cleanup checks. */
[[nodiscard]] std::size_t
collect_block_owner_observations(std::span<block_owner_observation> observations) noexcept {
    if (observations.empty()) {
        return 0;
    }
    std::size_t count = 0;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t cursor =
        g_shared->block_scan_cursor % static_cast<std::uint32_t>(k_block_ledger_capacity);
    const std::size_t scan_count = observations.size() < k_block_ledger_capacity
                                       ? observations.size()
                                       : k_block_ledger_capacity;
    for (std::size_t scanned = 0; scanned < scan_count; ++scanned) {
        const std::uint32_t slot = cursor;
        cursor = (cursor + 1U) % static_cast<std::uint32_t>(k_block_ledger_capacity);
        const block_ledger_entry& entry = g_shared->block_ledger[slot];
        const bool explicit_retirement = entry.state == block_ledger_state::retiring;
        const bool producer_owned =
            entry.state == block_ledger_state::reserved ||
            (entry.state == block_ledger_state::stored && entry.manifest_slot == k_invalid_slot);
        if (!explicit_retirement && !producer_owned) {
            continue;
        }
        observations[count++] = block_owner_observation{
            .ledger_slot = slot,
            .entry = entry,
            .owner = entry.owner,
            .requires_liveness = !explicit_retirement,
        };
    }
    g_shared->block_scan_cursor = cursor;
    LWLockRelease(g_storage_lock);
    return count;
}

/**
 * Reclaims blocks left by dead producers and blocks marked for retirement.
 * The owner is checked outside the lock, then the saved block record is
 * compared again before cleanup. For reservations with no recorded block,
 * it clears the record and returns reserved bytes to the storage budget
 * when possible.
 */
void reclaim_abandoned_blocks(janitor_sweep_result& result) noexcept {
    std::array<block_owner_observation, k_owner_liveness_scan_budget> observations{};
    const std::size_t count =
        collect_block_owner_observations(std::span<block_owner_observation>{observations});
    std::array<bool, k_owner_liveness_scan_budget> alive{};
    probe_observation_owners(std::span<const block_owner_observation>{observations.data(), count},
                             std::span<bool>{alive.data(), count});

    for (std::size_t index = 0; index < count && result.abandoned_blocks < g_shared->reclaim_batch;
         ++index) {
        if (alive[index]) {
            continue;
        }
        const block_owner_observation& observation = observations[index];
        bool release_physical_block = false;
        LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
        block_ledger_entry& current = g_shared->block_ledger[observation.ledger_slot];
        if (!block_observation_matches(current, observation.entry)) {
            LWLockRelease(g_storage_lock);
            continue;
        }
        if (current.state == block_ledger_state::reserved && current.reference.length == 0) {
            if (current.reservation_accounted) {
                if (current.reservation_tier == block_backing_tier::file &&
                    release_reservation(file_reserved_bytes(), current.reserved_bytes)) {
                    result.reclaimed_file_bytes += current.reserved_bytes;
                } else if (current.reservation_tier == block_backing_tier::dsa &&
                           !current.live_accounted &&
                           release_reservation(hot_reserved_bytes(), current.reserved_bytes)) {
                    result.reclaimed_hot_bytes += current.reserved_bytes;
                }
            }
            current = block_ledger_entry{};
            current.manifest_slot = k_invalid_slot;
            ++result.abandoned_blocks;
            LWLockRelease(g_storage_lock);
            continue;
        }
        current.state = block_ledger_state::retiring;
        release_physical_block = true;
        LWLockRelease(g_storage_lock);
        if (release_physical_block &&
            release_block_slot(observation.ledger_slot, observation.entry.sequence, &result) ==
                storage_error::none) {
            ++result.abandoned_blocks;
        }
    }
}

} // namespace

std::size_t storage_shared_memory_size(const storage_config& config) noexcept {
    return config_valid(config) ? sizeof(storage_shared_state) : 0;
}

void request_storage_resources(const storage_config& config) {
    g_requested_config = config;
    g_registry_request = registry_preload_request(config);
    if (!g_registry_request) {
        return;
    }
    shared_registry::request_shared_registry_resources(g_registry_request);
    RequestNamedLWLockTranche(k_storage_lock_tranche, 1);
}

bool startup_storage(std::uint64_t boot_nonce, const storage_config& config) {
    if (!config_valid(config) || !same_config(config, g_requested_config) || !g_registry_request) {
        return false;
    }
    g_storage_lock = &GetNamedLWLockTranche(k_storage_lock_tranche)[0].lock;
    return g_storage_lock != nullptr &&
           shared_registry::startup_shared_registry(g_registry_request, boot_nonce);
}

bool attach_storage() {
    if (storage_ready()) {
        return true;
    }
    if (!g_registry_request || !config_valid(g_requested_config)) {
        return false;
    }
    if (g_storage_lock == nullptr) {
        g_storage_lock = &GetNamedLWLockTranche(k_storage_lock_tranche)[0].lock;
    }
    const auto attached = shared_registry::attach_shared_registry(
        g_registry_request, storage_registry_callbacks(), true);
    if (!attached ||
        attached.attachment.extension_metadata.size() != sizeof(storage_shared_state)) {
        return false;
    }
    g_dsa = attached.attachment.area;
    g_registry_root = attached.attachment.root;
    g_accounting = shared_registry::shared_registry_accounting();
    g_shared =
        reinterpret_cast<storage_shared_state*>(attached.attachment.extension_metadata.data());
    if (!g_accounting || !validate_shared_state(*g_shared,
                                                attached.attachment.extension_metadata.size(),
                                                g_registry_root->boot_nonce,
                                                g_requested_config)) {
        shared_registry::detach_shared_registry_local_mapping();
        g_shared = nullptr;
        g_registry_root = nullptr;
        g_accounting = {};
        g_dsa = nullptr;
        return false;
    }
    if (!g_exit_callback_registered) {
        before_shmem_exit(storage_backend_exit, Datum{0});
        g_exit_callback_registered = true;
    }
    return true;
}

bool storage_ready() noexcept {
    return g_shared != nullptr && g_dsa != nullptr && g_registry_root != nullptr &&
           shared_registry::shared_registry_ready() && g_shared->magic == k_storage_magic &&
           g_shared->abi_revision == k_storage_abi_revision;
}

shared_dsa_reserve_result reserve_shared_dsa_allocation(std::uint64_t payload_bytes) noexcept {
    shared_dsa_reserve_result result{};
    if (payload_bytes == 0 || !attach_storage()) {
        result.error =
            payload_bytes == 0 ? storage_error::invalid_argument : storage_error::not_preloaded;
        return result;
    }
    const auto accounted = conservative_dsa_reservation_bytes(payload_bytes);
    if (!accounted.has_value()) {
        result.error = storage_error::hot_tier_full;
        return result;
    }
    const storage_backend_identity owner = current_backend_identity();
    if (!owner.valid()) {
        result.error = storage_error::invalid_state;
        return result;
    }
    pg_atomic_reservation_counter counter{.value = &hot_reserved_bytes()};
    const pg_system_memory_budget budget{
        .payload_ceiling = g_shared->hot_ceiling_bytes,
        .shared_reserved = &hot_reserved_bytes(),
    };
    basic_cache_admission admission{counter, budget};
    auto permit = admission.try_reserve(*accounted);
    if (!permit.has_value()) {
        result.error = permit.error() == admission_error::system_pressure
                           ? storage_error::memory_pressure
                           : storage_error::hot_tier_full;
        return result;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t slot = k_invalid_slot;
    for (std::uint32_t index = 0; index < k_shared_dsa_reservation_capacity; ++index) {
        if (g_shared->shared_dsa_reservations[index].state == shared_dsa_reservation_state::empty) {
            slot = index;
            break;
        }
    }
    if (slot == k_invalid_slot) {
        LWLockRelease(g_storage_lock);
        result.error = storage_error::ledger_full;
        return result;
    }
    std::uint64_t sequence =
        pg_atomic_fetch_add_u64(&g_shared->next_shared_reservation_sequence, 1);
    if (sequence == 0) {
        sequence = pg_atomic_fetch_add_u64(&g_shared->next_shared_reservation_sequence, 1);
    }
    if (sequence == 0) {
        LWLockRelease(g_storage_lock);
        result.error = storage_error::invalid_state;
        return result;
    }
    g_shared->shared_dsa_reservations[slot] = shared_dsa_reservation_entry{
        .state = shared_dsa_reservation_state::pending,
        .owner = owner,
        .sequence = sequence,
        .accounted_bytes = permit->bytes(),
    };
    LWLockRelease(g_storage_lock);
    static_cast<void>(permit->disown());
    result.reservation = shared_dsa_reservation{
        .slot = slot,
        .sequence = sequence,
    };
    return result;
}

bool publish_shared_dsa_allocation(shared_dsa_reservation* reservation) noexcept {
    const storage_backend_identity owner = current_backend_identity();
    if (!storage_ready() || reservation == nullptr || !*reservation || !owner.valid()) {
        return false;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_dsa_reservation_entry& entry = g_shared->shared_dsa_reservations[reservation->slot];
    if (entry.state != shared_dsa_reservation_state::pending ||
        entry.sequence != reservation->sequence || entry.owner != owner ||
        entry.accounted_bytes == 0) {
        LWLockRelease(g_storage_lock);
        return false;
    }
    const auto desired = checked_add(
        static_cast<std::uint64_t>(pg_atomic_read_u64(&g_registry_root->hot_live_bytes)),
        entry.accounted_bytes);
    if (!desired.has_value()) {
        LWLockRelease(g_storage_lock);
        return false;
    }
    static_cast<void>(
        pg_atomic_fetch_add_u64(&g_registry_root->hot_live_bytes, entry.accounted_bytes));
    entry.state = shared_dsa_reservation_state::live;
    LWLockRelease(g_storage_lock);
    return true;
}

bool release_shared_dsa_allocation(shared_dsa_reservation* reservation) noexcept {
    const storage_backend_identity owner = current_backend_identity();
    if (!storage_ready() || reservation == nullptr || !*reservation || !owner.valid()) {
        return false;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_dsa_reservation_entry& entry = g_shared->shared_dsa_reservations[reservation->slot];
    if ((entry.state != shared_dsa_reservation_state::pending &&
         entry.state != shared_dsa_reservation_state::live) ||
        entry.sequence != reservation->sequence || entry.owner != owner ||
        entry.accounted_bytes == 0) {
        LWLockRelease(g_storage_lock);
        return false;
    }
    const bool live = entry.state == shared_dsa_reservation_state::live;
    const std::uint64_t accounted_bytes = entry.accounted_bytes;
    entry.state = shared_dsa_reservation_state::releasing;
    LWLockRelease(g_storage_lock);
    bool released = true;
    if (live) {
        released = release_reservation(g_registry_root->hot_live_bytes, accounted_bytes);
    }
    released = release_reservation(hot_reserved_bytes(), accounted_bytes) && released;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_dsa_reservation_entry& completed = g_shared->shared_dsa_reservations[reservation->slot];
    if (completed.state == shared_dsa_reservation_state::releasing &&
        completed.sequence == reservation->sequence && completed.owner == owner) {
        completed = shared_dsa_reservation_entry{};
    } else {
        released = false;
    }
    LWLockRelease(g_storage_lock);
    *reservation = shared_dsa_reservation{};
    return released;
}

store_block_result store_block(std::span<const std::byte> bytes) {
    store_block_result result{};
    if (!attach_storage()) {
        result.error = storage_error::not_preloaded;
        return result;
    }
    if (bytes.empty()) {
        result.error = storage_error::invalid_argument;
        return result;
    }
    if (bytes.size() > g_shared->maximum_block_bytes) {
        result.error = storage_error::block_too_large;
        return result;
    }
    const storage_backend_identity owner = current_backend_identity();
    if (!owner.valid()) {
        result.error = storage_error::invalid_state;
        return result;
    }

    std::uint32_t ledger_slot = k_invalid_slot;
    std::uint64_t sequence = 0;
    result.error = allocate_ledger_slot(
        ledger_slot, sequence, owner, static_cast<std::uint64_t>(bytes.size()));
    if (result.error != storage_error::none) {
        remember_failure(result.error);
        return result;
    }

    block_reference reference{};
    bool hot_reserved = false;
    const bool force_file_tier =
#if defined(BIOSEQ_CACHE_TESTING)
        pg_atomic_read_u32(&g_shared->force_file_tier) != 0;
#else
        false;
#endif
    if (!force_file_tier) {
        pg_atomic_reservation_counter hot_counter{
            .value = &hot_reserved_bytes(),
        };
        const pg_system_memory_budget memory_budget{
            .payload_ceiling = g_shared->hot_ceiling_bytes,
            .shared_reserved = &hot_reserved_bytes(),
        };
        basic_cache_admission hot_admission{hot_counter, memory_budget};
        const auto hot_accounted =
            conservative_dsa_reservation_bytes(static_cast<std::uint64_t>(bytes.size()));
        auto hot_permit = hot_admission.try_reserve(
            hot_accounted.value_or((std::numeric_limits<std::uint64_t>::max)()));
        hot_reserved = hot_accounted.has_value() && hot_permit.has_value();
        if (hot_reserved) {
            result.error = mark_ledger_reservation(
                ledger_slot, sequence, owner, block_backing_tier::dsa, hot_permit->bytes());
            if (result.error != storage_error::none) {
                clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
                remember_failure(result.error);
                return result;
            }
            const dsa_pointer allocation =
                dsa_allocate_extended(g_dsa, bytes.size(), DSA_ALLOC_NO_OOM);
            if (DsaPointerIsValid(allocation)) {
                reference.tier = block_backing_tier::dsa;
                reference.handle_or_file_id = static_cast<std::uint64_t>(allocation);
                reference.offset = 0;
                reference.length = bytes.size();
                reference.crc32c = crc32c(bytes);
                result.error = record_reserved_block(ledger_slot, sequence, owner, reference);
                if (result.error != storage_error::none) {
                    dsa_free(g_dsa, allocation);
                    static_cast<void>(cancel_ledger_reservation(
                        ledger_slot, sequence, owner, block_backing_tier::dsa));
                    clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
                    remember_failure(result.error);
                    return result;
                }
                static_cast<void>(hot_permit->disown());
                void* address = dsa_get_address(g_dsa, allocation);
                std::memcpy(address, bytes.data(), bytes.size());
                pg_write_barrier();
                result.error = publish_stored_ledger_block(ledger_slot, sequence, owner, reference);
                if (result.error == storage_error::none) {
                    result.token = stored_block_token{
                        .ledger_slot = ledger_slot,
                        .sequence = sequence,
                        .owner = owner,
                        .reference = reference,
                    };
                    return result;
                }
                static_cast<void>(release_block_slot(ledger_slot, sequence));
                remember_failure(result.error);
                return result;
            }
            static_cast<void>(
                pg_atomic_fetch_add_u64(&g_registry_root->hot_allocation_failures, 1));
            static_cast<void>(
                cancel_ledger_reservation(ledger_slot, sequence, owner, block_backing_tier::dsa));
        }
    }

    std::uint64_t filesystem_bytes = 0;
    std::uint64_t filesystem_available = 0;
    result.error = filesystem_capacity(filesystem_bytes, filesystem_available);
    if (result.error != storage_error::none) {
        clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
        remember_failure(result.error);
        return result;
    }
    const std::uint64_t policy_ceiling = file_admission_policy::ceiling_bytes(filesystem_bytes);
    const std::uint64_t ceiling = g_shared->file_ceiling_bytes < policy_ceiling
                                      ? g_shared->file_ceiling_bytes
                                      : policy_ceiling;
    const std::uint64_t observed = pg_atomic_read_u64(&file_reserved_bytes());
    /* File-tier tests skip the production free-space reserve so unrelated
     * disk use does not prevent the test. Available-space and file-size
     * limits still apply. */
    const std::uint64_t free_floor =
        force_file_tier ? 0 : file_admission_policy::free_floor_bytes(filesystem_bytes);
    const std::uint64_t free_headroom =
        filesystem_available > free_floor ? filesystem_available - free_floor : 0;
    const std::uint64_t concurrency_ceiling =
        observed <= (std::numeric_limits<std::uint64_t>::max)() - free_headroom
            ? observed + free_headroom
            : (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t reservation_ceiling =
        ceiling < concurrency_ceiling ? ceiling : concurrency_ceiling;
    pg_atomic_reservation_counter file_counter{
        .value = &file_reserved_bytes(),
    };
    const fixed_admission_budget file_budget{
        .ceiling = reservation_ceiling,
    };
    basic_cache_admission file_admission{file_counter, file_budget};
    auto file_permit = file_admission.try_reserve(bytes.size());
    const bool file_policy_allows_reservation = file_admission_policy::can_reserve_with_free_floor(
        observed, bytes.size(), filesystem_bytes, filesystem_available, free_floor);
    if (!file_policy_allows_reservation || !file_permit.has_value()) {
        clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
        result.error = storage_error::file_tier_full;
        remember_failure(result.error);
        return result;
    }
    result.error = mark_ledger_reservation(
        ledger_slot, sequence, owner, block_backing_tier::file, file_permit->bytes());
    if (result.error != storage_error::none) {
        clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
        remember_failure(result.error);
        return result;
    }
    std::uint64_t file_id = pg_atomic_fetch_add_u64(&g_shared->next_file_id, 1);
    if (file_id == 0) {
        file_id = pg_atomic_fetch_add_u64(&g_shared->next_file_id, 1);
    }
    reference.tier = block_backing_tier::file;
    reference.handle_or_file_id = file_id;
    reference.offset = 0;
    reference.length = bytes.size();
    reference.crc32c = crc32c(bytes);
    result.error = record_reserved_block(ledger_slot, sequence, owner, reference);
    if (result.error != storage_error::none) {
        static_cast<void>(
            cancel_ledger_reservation(ledger_slot, sequence, owner, block_backing_tier::file));
        clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
        remember_failure(result.error);
        return result;
    }
    static_cast<void>(file_permit->disown());
    result.error = store_file_block(
        bytes, static_cast<std::uint32_t>(MyDatabaseId), owner, file_id, reference);
    if (result.error != storage_error::none) {
        static_cast<void>(release_block_slot(ledger_slot, sequence));
        remember_failure(result.error);
        return result;
    }
    result.error = publish_stored_ledger_block(ledger_slot, sequence, owner, reference);
    if (result.error != storage_error::none) {
        static_cast<void>(release_block_slot(ledger_slot, sequence));
        remember_failure(result.error);
        return result;
    }
    result.token = stored_block_token{
        .ledger_slot = ledger_slot,
        .sequence = sequence,
        .owner = owner,
        .reference = reference,
    };
    return result;
}

storage_error discard_unpublished_block(const stored_block_token& token) {
    const storage_backend_identity current = current_backend_identity();
    if (!storage_ready() || !token || token.owner != current) {
        return storage_error::invalid_argument;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    const block_ledger_entry& entry = g_shared->block_ledger[token.ledger_slot];
    const bool valid = entry.sequence == token.sequence && entry.owner == token.owner &&
                       entry.state == block_ledger_state::stored &&
                       entry.manifest_slot == k_invalid_slot;
    LWLockRelease(g_storage_lock);
    return valid ? release_block_slot(token.ledger_slot, token.sequence)
                 : storage_error::stale_token;
}

publish_manifest_result publish_manifest(const manifest_descriptor& descriptor,
                                         std::span<const stored_block_token> blocks) {
    publish_manifest_result result{};
    if (!storage_ready()) {
        result.error = storage_error::not_preloaded;
        return result;
    }
    if (descriptor.id.boot_nonce == 0 ||
        descriptor.id.boot_nonce != pg_atomic_read_u64(&g_shared->boot_nonce) ||
        descriptor.id.generation == 0 || descriptor.id.claim_sequence == 0 ||
        (descriptor.kind != cache_entry_kind::exact_final_result &&
         descriptor.kind != cache_entry_kind::reusable_coverage) ||
        descriptor.reserved != 0 || descriptor.reserved2 != 0 ||
        descriptor.reserved_waiter_count != 0 || descriptor.flags != 0 ||
        descriptor.logical_bytes > k_maximum_column_store_bytes ||
        descriptor.row_group_count > k_maximum_column_store_row_groups || blocks.empty() ||
        blocks.size() > k_blocks_per_manifest) {
        result.error = storage_error::invalid_argument;
        return result;
    }
    const storage_backend_identity current = current_backend_identity();
    if (!current.valid()) {
        result.error = storage_error::invalid_state;
        return result;
    }

    result.error = validate_manifest_tokens_before_io(descriptor, blocks, current);
    if (result.error != storage_error::none) {
        return result;
    }

    const manifest_file_finalization finalized = bioseq::postgres::support::invoke_pg_as_cpp(
        [descriptor_pointer = &descriptor, blocks, current] {
            return finalize_manifest_file(*descriptor_pointer, blocks, current);
        });
    if (finalized.error != storage_error::none) {
        result.error = finalized.error;
        return result;
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t free_slot = k_invalid_slot;
    for (std::uint32_t index = 0; index < k_manifest_capacity; ++index) {
        shared_manifest_entry& manifest = g_shared->manifests[index];
        if (manifest.state == manifest_state::empty && free_slot == k_invalid_slot) {
            free_slot = index;
        } else if (manifest.state != manifest_state::empty &&
                   (same_id(manifest.id, descriptor.id) ||
                    (manifest.id.digest == descriptor.id.digest &&
                     manifest.id.generation == descriptor.id.generation))) {
            LWLockRelease(g_storage_lock);
            result.error = storage_error::duplicate_generation;
            return result;
        }
    }
    if (free_slot == k_invalid_slot) {
        LWLockRelease(g_storage_lock);
        result.error = storage_error::manifest_full;
        return result;
    }
    std::uint64_t stored_bytes = 0;
    for (std::size_t token_index = 0; token_index < blocks.size(); ++token_index) {
        const stored_block_token& token = blocks[token_index];
        if (!token || token.ledger_slot >= k_block_ledger_capacity || token.owner != current) {
            LWLockRelease(g_storage_lock);
            result.error = storage_error::invalid_argument;
            return result;
        }
        const block_ledger_entry& block = g_shared->block_ledger[token.ledger_slot];
        const block_reference& expected_reference = finalized.references[token_index];
        if (block.sequence != token.sequence || block.owner != token.owner ||
            block.state != block_ledger_state::stored || block.manifest_slot != k_invalid_slot ||
            !block.reservation_accounted || block.reservation_tier != block.reference.tier ||
            !valid_stored_reference(block.reference, block.payload_bytes) ||
            std::memcmp(&block.reference, &expected_reference, sizeof(block_reference)) != 0 ||
            stored_bytes > (std::numeric_limits<std::uint64_t>::max)() - block.reference.length) {
            LWLockRelease(g_storage_lock);
            result.error = storage_error::stale_token;
            return result;
        }
        for (std::size_t previous = 0; previous < token_index; ++previous) {
            if (blocks[previous].ledger_slot == token.ledger_slot) {
                LWLockRelease(g_storage_lock);
                result.error = storage_error::invalid_argument;
                return result;
            }
        }
        stored_bytes += block.reference.length;
    }

    shared_manifest_entry& manifest = g_shared->manifests[free_slot];
    manifest = shared_manifest_entry{};
    manifest.state = manifest_state::empty;
    manifest.kind = descriptor.kind;
    manifest.block_count = static_cast<std::uint16_t>(blocks.size());
    manifest.flags = descriptor.flags;
    manifest.slot_sequence = pg_atomic_fetch_add_u64(&g_shared->next_manifest_sequence, 1);
    if (manifest.slot_sequence == 0) {
        manifest.slot_sequence = pg_atomic_fetch_add_u64(&g_shared->next_manifest_sequence, 1);
    }
    manifest.id = descriptor.id;
    manifest.logical_bytes = descriptor.logical_bytes;
    manifest.stored_bytes = stored_bytes;
    manifest.row_count = descriptor.row_count;
    manifest.row_group_count = descriptor.row_group_count;
    manifest.database_oid = static_cast<std::uint32_t>(MyDatabaseId);
    manifest.created_at_us = current_time_us(descriptor.created_at_us);
    manifest.expires_at_us = descriptor.expires_at_us;
    if (manifest.expires_at_us != 0 && manifest.expires_at_us < manifest.created_at_us) {
        manifest = shared_manifest_entry{};
        manifest.state = manifest_state::empty;
        manifest.ledger_slots.fill(k_invalid_slot);
        pg_atomic_init_u64(&manifest.last_access_at_us, 0);
        pg_atomic_init_u32(&manifest.reader_pins, 0);
        pg_atomic_init_u32(&manifest.waiter_references, 0);
        LWLockRelease(g_storage_lock);
        result.error = storage_error::invalid_argument;
        return result;
    }
    manifest.ledger_slots.fill(k_invalid_slot);
    pg_atomic_init_u64(&manifest.last_access_at_us, manifest.created_at_us);
    pg_atomic_init_u32(&manifest.reader_pins, 0);
    pg_atomic_init_u32(&manifest.waiter_references, 0);
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const stored_block_token& token = blocks[index];
        manifest.ledger_slots[index] = token.ledger_slot;
        manifest.blocks[index] = finalized.references[index];
    }
    /* Publish Complete after assigning all blocks. The janitor can clean
     * a Publishing record left by an interrupted publication. */
    manifest.state = manifest_state::publishing;
    pg_write_barrier();
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const stored_block_token& token = blocks[index];
        block_ledger_entry& block = g_shared->block_ledger[token.ledger_slot];
        block.state = block_ledger_state::referenced;
        block.manifest_slot = free_slot;
    }
    pg_write_barrier();
    static_cast<void>(pg_atomic_fetch_add_u64(&g_shared->manifest_count, 1));
    manifest.manifest_counted = true;
    manifest.state = manifest_state::complete;
    pg_write_barrier();
    result.handle = manifest_handle{
        .slot = free_slot,
        .slot_sequence = manifest.slot_sequence,
        .id = manifest.id,
    };
    LWLockRelease(g_storage_lock);
    return result;
}

pin_manifest_result pin_manifest(const cache_entry_id& id, std::int64_t now_us) {
    pin_manifest_result result{};
    if (!storage_ready()) {
        result.error = storage_error::not_preloaded;
        return result;
    }
    const storage_backend_identity owner = current_backend_identity();
    if (!owner.valid()) {
        result.error = storage_error::invalid_state;
        return result;
    }
    const std::int64_t now = current_time_us(now_us);
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (std::uint32_t index = 0; index < k_manifest_capacity; ++index) {
        shared_manifest_entry& manifest = g_shared->manifests[index];
        if ((manifest.state != manifest_state::complete &&
             manifest.state != manifest_state::retiring) ||
            !same_id(manifest.id, id)) {
            continue;
        }
        if (manifest.expires_at_us != 0 && now >= manifest.expires_at_us) {
            LWLockRelease(g_storage_lock);
            result.error = storage_error::not_found;
            return result;
        }
        const manifest_handle handle{
            .slot = index,
            .slot_sequence = manifest.slot_sequence,
            .id = manifest.id,
        };
        result.error = claim_manifest_reader_locked(manifest, handle, owner, result.pin, now);
        LWLockRelease(g_storage_lock);
        return result;
    }
    LWLockRelease(g_storage_lock);
    result.error = storage_error::not_found;
    return result;
}

register_manifest_waiter_result register_manifest_waiter(const manifest_handle& handle,
                                                         const storage_backend_identity& owner,
                                                         std::int64_t registered_at_us) {
    register_manifest_waiter_result result{};
    if (!storage_ready() || !handle || !owner.valid()) {
        result.error = storage_error::invalid_argument;
        return result;
    }
    const std::int64_t now = current_time_us();
    const bool preexisting = registered_at_us != 0;
    const std::int64_t registered_at = preexisting ? registered_at_us : now;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_manifest_entry& manifest = g_shared->manifests[handle.slot];
    const bool expired_registration =
        manifest.expires_at_us != 0 && (preexisting ? registered_at > manifest.expires_at_us
                                                    : registered_at >= manifest.expires_at_us);
    if (!handle_matches(manifest, handle) || manifest.state != manifest_state::complete ||
        registered_at < manifest.created_at_us || registered_at > now || expired_registration) {
        LWLockRelease(g_storage_lock);
        result.error = storage_error::not_found;
        return result;
    }
    std::uint32_t waiter_slot = k_invalid_slot;
    for (std::uint32_t index = 0; index < k_waiters_per_manifest; ++index) {
        if (!manifest.waiters[index].active) {
            waiter_slot = index;
            break;
        }
    }
    if (waiter_slot == k_invalid_slot) {
        LWLockRelease(g_storage_lock);
        result.error = storage_error::waiter_full;
        return result;
    }
    std::uint64_t owner_sequence = pg_atomic_fetch_add_u64(&g_shared->next_waiter_sequence, 1);
    if (owner_sequence == 0) {
        owner_sequence = pg_atomic_fetch_add_u64(&g_shared->next_waiter_sequence, 1);
    }
    if (owner_sequence == 0) {
        owner_sequence = 1;
    }
    shared_manifest_waiter& waiter = manifest.waiters[waiter_slot];
    waiter = shared_manifest_waiter{
        .active = true,
        .owner = owner,
        .owner_sequence = owner_sequence,
    };
    static_cast<void>(pg_atomic_fetch_add_u32(&manifest.waiter_references, 1));
    static_cast<void>(pg_atomic_fetch_add_u64(&g_shared->waiter_reference_count, 1));
    result.token = manifest_waiter_token{
        .handle = handle,
        .waiter_slot = waiter_slot,
        .owner = owner,
        .owner_sequence = owner_sequence,
    };
    LWLockRelease(g_storage_lock);
    return result;
}

register_manifest_waiter_result current_manifest_waiter(const cache_entry_id& id) {
    register_manifest_waiter_result result{};
    const storage_backend_identity current = current_backend_identity();
    if (!storage_ready() || id.boot_nonce == 0 || id.generation == 0 || id.claim_sequence == 0 ||
        !current.valid() || id.boot_nonce != pg_atomic_read_u64(&g_shared->boot_nonce)) {
        result.error = storage_error::invalid_argument;
        return result;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    bool found = false;
    for (std::uint32_t manifest_slot = 0; manifest_slot < k_manifest_capacity; ++manifest_slot) {
        const shared_manifest_entry& manifest = g_shared->manifests[manifest_slot];
        if (manifest.state != manifest_state::complete || !same_id(manifest.id, id)) {
            continue;
        }
        const manifest_handle handle{
            .slot = manifest_slot,
            .slot_sequence = manifest.slot_sequence,
            .id = manifest.id,
        };
        for (std::uint32_t waiter_slot = 0; waiter_slot < k_waiters_per_manifest; ++waiter_slot) {
            const shared_manifest_waiter& waiter = manifest.waiters[waiter_slot];
            if (!waiter.active || waiter.owner != current) {
                continue;
            }
            if (waiter.owner_sequence == 0) {
                LWLockRelease(g_storage_lock);
                result.error = storage_error::invalid_state;
                return result;
            }
            if (found) {
                LWLockRelease(g_storage_lock);
                result.token = {};
                result.error = storage_error::invalid_state;
                return result;
            }
            result.token = manifest_waiter_token{
                .handle = handle,
                .waiter_slot = waiter_slot,
                .owner = waiter.owner,
                .owner_sequence = waiter.owner_sequence,
            };
            found = true;
        }
    }
    LWLockRelease(g_storage_lock);
    result.error = found ? storage_error::none : storage_error::not_found;
    return result;
}

pin_manifest_result convert_waiter_to_pin(const manifest_waiter_token& token) {
    pin_manifest_result result{};
    const storage_backend_identity current = current_backend_identity();
    if (!storage_ready() || !token || token.owner != current) {
        result.error = storage_error::invalid_argument;
        return result;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_manifest_entry& manifest = g_shared->manifests[token.handle.slot];
    shared_manifest_waiter& waiter = manifest.waiters[token.waiter_slot];
    if (!handle_matches(manifest, token.handle) || manifest.state != manifest_state::complete ||
        !waiter_token_matches(waiter, token) || !waiter.active) {
        LWLockRelease(g_storage_lock);
        result.error = storage_error::not_found;
        return result;
    }
    const std::int64_t accessed_at = current_time_us();
    result.error =
        claim_manifest_reader_locked(manifest, token.handle, token.owner, result.pin, accessed_at);
    if (result.error != storage_error::none) {
        LWLockRelease(g_storage_lock);
        return result;
    }
    static_cast<void>(deactivate_waiter_locked(manifest, waiter));
    LWLockRelease(g_storage_lock);
    return result;
}

storage_error unpin_manifest(const manifest_pin& pin) noexcept {
    const storage_backend_identity current = current_backend_identity();
    if (!storage_ready() || !pin || pin.handle.slot >= k_manifest_capacity ||
        pin.owner != current) {
        return storage_error::invalid_argument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_manifest_entry& manifest = g_shared->manifests[pin.handle.slot];
    shared_manifest_reader& reader = manifest.readers[pin.reader_slot];
    if (!handle_matches(manifest, pin.handle) || !reader_pin_matches(reader, pin)) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    static_cast<void>(deactivate_reader_locked(manifest, reader));
    LWLockRelease(g_storage_lock);
    return storage_error::none;
}

storage_error unregister_manifest_waiter(const manifest_waiter_token& token) noexcept {
    if (!storage_ready() || !token) {
        return storage_error::invalid_argument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_manifest_entry& manifest = g_shared->manifests[token.handle.slot];
    shared_manifest_waiter& waiter = manifest.waiters[token.waiter_slot];
    if (!handle_matches(manifest, token.handle) || !waiter_token_matches(waiter, token)) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    if (waiter.active) {
        static_cast<void>(deactivate_waiter_locked(manifest, waiter));
    }
    LWLockRelease(g_storage_lock);
    return storage_error::none;
}

storage_error read_pinned_block(const manifest_pin& pin,
                                std::size_t block_index,
                                std::span<std::byte> destination,
                                storage_read_purpose purpose) {
    const storage_backend_identity current = current_backend_identity();
    if (!storage_ready() || !pin || pin.owner != current || block_index >= pin.block_count ||
        destination.size() != pin.blocks[block_index].length) {
        return storage_error::invalid_argument;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    const shared_manifest_entry& manifest = g_shared->manifests[pin.handle.slot];
    const shared_manifest_reader& reader = manifest.readers[pin.reader_slot];
    const bool valid_pin =
        handle_matches(manifest, pin.handle) && reader_pin_matches(reader, pin) &&
        manifest.block_count == pin.block_count &&
        std::memcmp(
            &manifest.blocks[block_index], &pin.blocks[block_index], sizeof(block_reference)) == 0;
    LWLockRelease(g_storage_lock);
    if (!valid_pin) {
        return storage_error::stale_token;
    }
    const TimestampTz started_at = GetCurrentTimestamp();
    const block_reference& reference = pin.blocks[block_index];
    storage_error error = storage_error::none;
    if (reference.tier == block_backing_tier::dsa) {
        if (!DsaPointerIsValid(static_cast<dsa_pointer>(reference.handle_or_file_id))) {
            return storage_error::invalid_argument;
        }
        const auto* source = static_cast<const std::byte*>(
            dsa_get_address(g_dsa, static_cast<dsa_pointer>(reference.handle_or_file_id)));
        if (source == nullptr) {
            return storage_error::not_found;
        }
        std::memcpy(destination.data(), source + reference.offset, destination.size());
    } else if (reference.tier == block_backing_tier::file) {
        error = read_file_block(pin.database_oid, pin.handle.id, reference, destination);
    } else {
        return storage_error::invalid_argument;
    }
    if (error == storage_error::none && crc32c(destination) != reference.crc32c) {
        error = storage_error::checksum_mismatch;
    }
    const TimestampTz finished_at = GetCurrentTimestamp();
    const std::uint64_t duration =
        finished_at >= started_at ? static_cast<std::uint64_t>(finished_at - started_at) : 0;
    const auto saturating_add = [](std::uint64_t current, std::uint64_t increment) noexcept {
        return increment <= (std::numeric_limits<std::uint64_t>::max)() - current
                   ? current + increment
                   : (std::numeric_limits<std::uint64_t>::max)();
    };
    g_query_io_metrics.read_bytes =
        saturating_add(g_query_io_metrics.read_bytes, destination.size());
    g_query_io_metrics.read_duration_us =
        saturating_add(g_query_io_metrics.read_duration_us, duration);
    if (purpose == storage_read_purpose::prevalidation) {
        g_query_io_metrics.prevalidation_read_bytes =
            saturating_add(g_query_io_metrics.prevalidation_read_bytes, destination.size());
        g_query_io_metrics.prevalidation_read_duration_us =
            saturating_add(g_query_io_metrics.prevalidation_read_duration_us, duration);
    }
    g_query_io_metrics.checksum_status = error == storage_error::none
                                             ? storage_checksum_status::valid
                                             : storage_checksum_status::invalid;
    if (error == storage_error::checksum_mismatch) {
        g_query_io_metrics.checksum_failure_count =
            saturating_add(g_query_io_metrics.checksum_failure_count, 1);
    }
    if (error != storage_error::none) {
        remember_failure(error);
    }
    return error;
}

storage_error retire_manifest(const manifest_handle& handle) noexcept {
    if (!storage_ready() || !handle) {
        return storage_error::invalid_argument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    shared_manifest_entry& manifest = g_shared->manifests[handle.slot];
    if (!handle_matches(manifest, handle)) {
        LWLockRelease(g_storage_lock);
        return storage_error::stale_token;
    }
    manifest.state = manifest_state::retiring;
    LWLockRelease(g_storage_lock);
    request_janitor_sweep(false);
    return storage_error::none;
}

janitor_sweep_result janitor_sweep(std::int64_t now_us, bool aggressive) {
    janitor_sweep_result result{};
    if (!attach_storage()) {
        return result;
    }
    sweep_owner_token sweep_owner{};
    if (!acquire_sweep_ownership(sweep_owner)) {
        if (aggressive) {
            request_janitor_sweep(true);
        }
        return result;
    }
    const std::int64_t now = current_time_us(now_us);
    const std::uint32_t requested = pg_atomic_exchange_u32(&g_shared->sweep_requested, 0);
    aggressive = aggressive || requested > 1;

    /* Check backend liveness outside the storage lock. */
    reap_dead_manifest_references(result);
    reclaim_abandoned_blocks(result);

    /* Storage cleans unfinished publications and manifests retired by
     * the cache registries. */
    for (std::uint32_t index = 0;
         index < k_manifest_capacity && result.expired_manifests < g_shared->reclaim_batch;
         ++index) {
        std::uint64_t sequence = 0;
        bool expired = false;
        LWLockAcquire(g_storage_lock, LW_SHARED);
        const shared_manifest_entry& manifest = g_shared->manifests[index];
        if (manifest_reclaimable(manifest)) {
            expired = manifest.state == manifest_state::publishing ||
                      manifest.state == manifest_state::retiring;
            sequence = manifest.slot_sequence;
        }
        LWLockRelease(g_storage_lock);
        if (expired && reclaim_manifest_slot(index, sequence, result) == storage_error::none) {
            ++result.expired_manifests;
        }
    }

    /* The cache registries decide expiry and eviction. They must retire a
     * manifest before storage frees it, so a lookup cannot lose its blocks
     * before acquiring a reader pin. Memory pressure alone does not let
     * this pass evict a complete manifest. */
    static_cast<void>(aggressive);
    if (g_dsa != nullptr && result.reclaimed_hot_bytes != 0) {
        dsa_trim(g_dsa);
    }
    pg_atomic_write_u64(&g_shared->last_sweep_at_us, now);
    static_cast<void>(pg_atomic_fetch_add_u64(&g_shared->completed_sweeps, 1));
    release_sweep_ownership(sweep_owner);
    return result;
}

void request_janitor_sweep(bool aggressive) noexcept {
    if (g_shared == nullptr) {
        return;
    }
    const std::uint32_t desired = aggressive ? 2U : 1U;
    std::uint32_t observed = pg_atomic_read_u32(&g_shared->sweep_requested);
    while (observed < desired &&
           !pg_atomic_compare_exchange_u32(&g_shared->sweep_requested, &observed, desired)) {
    }
    /* The janitor sees this request on its next timed wakeup. Avoid a
     * backend-status lookup here because the caller may hold a cache
     * registry lock. */
}

bool publish_correctness_readiness(cache_correctness_readiness readiness) noexcept {
    if (!known_readiness(readiness)) {
        return false;
    }
    g_pending_correctness_readiness = readiness;
    if (g_shared == nullptr) {
        return true;
    }
    pg_atomic_write_u32(&g_shared->correctness_readiness, static_cast<std::uint32_t>(readiness));
    return true;
}

void publish_worker_lifecycle(worker_lifecycle lifecycle, int worker_pid) noexcept {
    g_pending_worker_lifecycle = lifecycle;
    if (g_shared == nullptr) {
        return;
    }
    pg_atomic_write_u32(&g_shared->worker_lifecycle, static_cast<std::uint32_t>(lifecycle));
    const storage_backend_identity current = current_backend_identity();
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    g_shared->worker_owner = worker_pid > 0 && current.valid() && current.pid == worker_pid
                                 ? current
                                 : storage_backend_identity{};
    LWLockRelease(g_storage_lock);
    publish_worker_heartbeat();
}

void publish_worker_heartbeat(std::int64_t now_us) noexcept {
    if (g_shared != nullptr) {
        pg_atomic_write_u64(&g_shared->worker_heartbeat_at_us,
                            static_cast<std::uint64_t>(current_time_us(now_us)));
    }
}

void reset_query_io_metrics() noexcept {
    g_query_io_metrics = storage_query_io_snapshot{};
}

storage_query_io_snapshot query_io_metrics() noexcept {
    return g_query_io_metrics;
}

storage_runtime_snapshot runtime_snapshot() noexcept {
    storage_runtime_snapshot snapshot{};
    snapshot.dsa_attached = g_dsa != nullptr;
    const auto registry = shared_registry::snapshot_shared_registry_diagnostics();
    snapshot.bootstrap_status = registry.state;
    snapshot.bootstrap_ready = registry.state == bootstrap_state::ready;
    if (g_shared == nullptr || !g_accounting || g_storage_lock == nullptr) {
        return snapshot;
    }
    snapshot.pending_file_bytes_valid = true;
    snapshot.janitor_lifecycle =
        static_cast<worker_lifecycle>(pg_atomic_read_u32(&g_shared->worker_lifecycle));
    snapshot.correctness_readiness = static_cast<cache_correctness_readiness>(
        pg_atomic_read_u32(&g_shared->correctness_readiness));
    snapshot.last_failure = static_cast<storage_error>(pg_atomic_read_u32(&g_shared->last_failure));
    snapshot.boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce);
    snapshot.reserved_hot_bytes =
        !g_accounting ? 0 : pg_atomic_read_u64(g_accounting.reserved_dsa_bytes);
    snapshot.fixed_hot_bytes = registry.fixed_dsa_reserved_bytes;
    snapshot.live_hot_bytes =
        g_registry_root == nullptr ? 0 : pg_atomic_read_u64(&g_registry_root->hot_live_bytes);
    const auto fixed_and_live = checked_add(snapshot.fixed_hot_bytes, snapshot.live_hot_bytes);
    snapshot.pending_hot_bytes_valid =
        fixed_and_live.has_value() && snapshot.reserved_hot_bytes >= *fixed_and_live;
    if (snapshot.pending_hot_bytes_valid) {
        snapshot.pending_hot_bytes = snapshot.reserved_hot_bytes - *fixed_and_live;
    }
    snapshot.reserved_file_bytes = pg_atomic_read_u64(g_accounting.reserved_file_bytes);
    snapshot.hot_ceiling_bytes = g_shared->hot_ceiling_bytes;
    snapshot.file_ceiling_bytes = g_shared->file_ceiling_bytes;
    snapshot.manifest_count = pg_atomic_read_u64(&g_shared->manifest_count);
    snapshot.waiter_reference_count = pg_atomic_read_u64(&g_shared->waiter_reference_count);
    snapshot.reader_reference_count = pg_atomic_read_u64(&g_shared->reader_reference_count);
    snapshot.completed_sweeps = pg_atomic_read_u64(&g_shared->completed_sweeps);
    snapshot.last_sweep_at_us =
        static_cast<std::int64_t>(pg_atomic_read_u64(&g_shared->last_sweep_at_us));
    snapshot.worker_heartbeat_at_us =
        static_cast<std::int64_t>(pg_atomic_read_u64(&g_shared->worker_heartbeat_at_us));
    LWLockAcquire(g_storage_lock, LW_SHARED);
    const storage_backend_identity worker = g_shared->worker_owner;
    for (const block_ledger_entry& entry : g_shared->block_ledger) {
        if (!entry.reservation_accounted || entry.state == block_ledger_state::empty) {
            continue;
        }
        if (entry.state == block_ledger_state::reserved &&
            entry.reservation_tier == block_backing_tier::file &&
            snapshot.pending_file_bytes_valid) {
            const auto next_pending_file =
                checked_add(snapshot.pending_file_bytes, entry.reserved_bytes);
            if (next_pending_file.has_value()) {
                snapshot.pending_file_bytes = *next_pending_file;
            } else {
                snapshot.pending_file_bytes_valid = false;
            }
        }
        if (entry.reservation_tier == block_backing_tier::dsa) {
            snapshot.dsa_stored_bytes += entry.payload_bytes;
        } else if (entry.reservation_tier == block_backing_tier::file) {
            snapshot.file_stored_bytes += entry.payload_bytes;
        }
    }
    for (const shared_manifest_entry& manifest : g_shared->manifests) {
        if (manifest.state != manifest_state::empty) {
            snapshot.row_group_count += manifest.row_group_count;
        }
    }
    LWLockRelease(g_storage_lock);
    snapshot.worker_pid = worker.pid;
    snapshot.worker_proc_number = worker.proc_number;
    snapshot.worker_backend_nonce = worker.backend_nonce;
    snapshot.worker_backend_started_at_us = worker.backend_started_at_us;
    const storage_query_io_snapshot query_metrics = query_io_metrics();
    snapshot.prevalidation_read_bytes = query_metrics.prevalidation_read_bytes;
    snapshot.prevalidation_read_duration_us = query_metrics.prevalidation_read_duration_us;
    snapshot.read_bytes = query_metrics.read_bytes;
    snapshot.read_duration_us = query_metrics.read_duration_us;
    snapshot.checksum_failure_count = query_metrics.checksum_failure_count;
    snapshot.checksum_status = query_metrics.checksum_status;
    return snapshot;
}

std::uint32_t janitor_interval_ms() noexcept {
    return g_shared != nullptr ? g_shared->sweep_interval_ms : g_requested_config.sweep_interval_ms;
}

} // namespace bioseq::cache::pg::storage

#if defined(BIOSEQ_CACHE_TESTING)
namespace bioseq::cache::pg::storage {

/** Adds a test byte count, returning false and leaving the total unchanged on overflow. */
[[nodiscard]] bool add_test_reserved_bytes(std::uint64_t& total, std::uint64_t bytes) noexcept {
    if (total > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
        return false;
    }
    total += bytes;
    return true;
}

bool set_test_force_file_tier(bool enabled) noexcept {
    if (!attach_storage()) {
        return false;
    }
    pg_atomic_write_u32(&g_shared->force_file_tier, enabled ? 1U : 0U);
    return true;
}

storage_bootstrap_test_snapshot test_storage_bootstrap_snapshot() noexcept {
    storage_bootstrap_test_snapshot snapshot{};
    const auto diagnostics = shared_registry::snapshot_shared_registry_diagnostics();
    snapshot.state = diagnostics.state;
    snapshot.failure = diagnostics.failure;
    snapshot.boot_nonce = diagnostics.boot_nonce;
    snapshot.initialization_attempt_count = diagnostics.initialization_attempt_count;
    snapshot.takeover_count = diagnostics.takeover_count;
    snapshot.cleanup_count = diagnostics.cleanup_count;
    snapshot.attach_count = diagnostics.attach_count;
    snapshot.validation_failure_count = diagnostics.validation_failure_count;
    snapshot.initializer = diagnostics.initializer;
    snapshot.last_initializer = diagnostics.last_initializer;
    snapshot.takeover_used = diagnostics.takeover_used;
    snapshot.live_unpublished_area_count = diagnostics.live_unpublished_area_count;
    snapshot.cleanup_handle_pinned = diagnostics.cleanup_handle_pinned;
    snapshot.cleanup_handle_published = diagnostics.cleanup_handle_published;
    snapshot.ready_handle_published = diagnostics.ready_handle_published;
    snapshot.root_published = diagnostics.root_published;
    const auto test_status = shared_registry::snapshot_shared_registry_test_status();
    snapshot.pause_reached = test_status.reached;
    snapshot.defer_background_initializer = test_status.defer_background_initializer;
    snapshot.last_cleanup_handle_was_pinned = test_status.last_cleanup_handle_was_pinned;
    snapshot.last_cleanup_release_verified = test_status.last_cleanup_release_verified;
    snapshot.last_cleanup_handle = test_status.last_cleanup_handle;
    const auto* attachment = shared_registry::current_shared_registry_attachment();
    snapshot.storage_attached = storage_ready();
    if (attachment == nullptr || attachment->root == nullptr || attachment->area == nullptr) {
        return snapshot;
    }
    snapshot.mapping_pinned = attachment->mapping_pinned;
    snapshot.reusable_wait_cv_count =
        static_cast<std::uint32_t>(attachment->reusable_wait_cvs.size());
    snapshot.storage_metadata_in_dsa =
        g_shared != nullptr &&
        dsa_get_address(attachment->area, attachment->root->extension_metadata) == g_shared;
    snapshot.reusable_wait_cvs_in_dsa =
        !attachment->reusable_wait_cvs.empty() &&
        dsa_get_address(attachment->area, attachment->root->reusable_wait_cvs) ==
            attachment->reusable_wait_cvs.data();
    return snapshot;
}

bool test_arm_storage_bootstrap_pause() noexcept {
    return shared_registry::arm_shared_registry_test_pause(
        shared_registry::shared_registry_test_point::before_handle_publish);
}

void test_release_storage_bootstrap_pause() noexcept {
    shared_registry::release_shared_registry_test_pause();
}

bool test_inject_storage_bootstrap_allocation_failure(std::uint32_t ordinal) noexcept {
    return shared_registry::inject_shared_registry_allocation_failure(ordinal);
}

bool test_inject_storage_bootstrap_abi_mismatch(bool enabled) noexcept {
    return shared_registry::inject_shared_registry_root_abi_mismatch(enabled);
}

storage_bootstrap_attach_twice_snapshot test_attach_storage_twice() noexcept {
    storage_bootstrap_attach_twice_snapshot snapshot{};
    const auto before = shared_registry::snapshot_shared_registry_diagnostics();
    snapshot.attempts_before = before.initialization_attempt_count;
    snapshot.attaches_before = before.attach_count;
    snapshot.first_attached = attach_storage();
    const auto* first = shared_registry::current_shared_registry_attachment();
    dsa_area* first_area = first == nullptr ? nullptr : first->area;
    auto* first_root = first == nullptr ? nullptr : first->root;
    snapshot.second_attached = attach_storage();
    const auto* second = shared_registry::current_shared_registry_attachment();
    snapshot.same_area = first_area != nullptr && second != nullptr && first_area == second->area;
    snapshot.same_root = first_root != nullptr && second != nullptr && first_root == second->root;
    snapshot.mapping_pinned = second != nullptr && second->mapping_pinned;
    const auto after = shared_registry::snapshot_shared_registry_diagnostics();
    snapshot.attempts_after = after.initialization_attempt_count;
    snapshot.attaches_after = after.attach_count;
    return snapshot;
}

bool test_corrupt_storage_bootstrap_manifest(
    shared_registry::shared_registry_test_manifest_corruption corruption) noexcept {
    if (!attach_storage() ||
        !shared_registry::corrupt_shared_registry_manifest_for_test(corruption) ||
        !test_detach_storage_mapping()) {
        return false;
    }
    const bool rejected = !attach_storage();
    return rejected &&
           shared_registry::snapshot_shared_registry_diagnostics().state ==
               bootstrap_state::failed;
}

bool test_detach_storage_mapping() noexcept {
    if (!storage_ready()) {
        return false;
    }
    static_cast<void>(reap_backend_references(current_backend_identity()));
    g_shared = nullptr;
    g_registry_root = nullptr;
    g_accounting = {};
    g_dsa = nullptr;
    shared_registry::detach_shared_registry_local_mapping();
    return !shared_registry::shared_registry_ready();
}

storage_tier_test_snapshot test_storage_tier_snapshot() noexcept {
    storage_tier_test_snapshot snapshot{};
    if (!attach_storage()) {
        return snapshot;
    }
    bool known_states = true;
    std::uint64_t accounted_blocks = 0;
    std::uint64_t dynamic_hot_reservations = 0;
    LWLockAcquire(g_storage_lock, LW_SHARED);
    snapshot.force_file_tier = pg_atomic_read_u32(&g_shared->force_file_tier) != 0;
    for (const block_ledger_entry& entry : g_shared->block_ledger) {
        if (entry.state == block_ledger_state::empty) {
            continue;
        }
        ++snapshot.occupied_blocks;
        switch (entry.state) {
        case block_ledger_state::reserved:
            ++snapshot.reserved_blocks;
            break;
        case block_ledger_state::stored:
            ++snapshot.stored_blocks;
            break;
        case block_ledger_state::referenced:
            ++snapshot.referenced_blocks;
            break;
        case block_ledger_state::retiring:
            ++snapshot.retiring_blocks;
            break;
        case block_ledger_state::release_committed:
            ++snapshot.release_committed_blocks;
            break;
        case block_ledger_state::empty:
            break;
        default:
            known_states = false;
            break;
        }
        if (!entry.reservation_accounted) {
            continue;
        }
        ++accounted_blocks;
        if (entry.reservation_tier == block_backing_tier::dsa) {
            ++snapshot.dsa_blocks;
            known_states =
                add_test_reserved_bytes(dynamic_hot_reservations, entry.reserved_bytes) &&
                known_states;
            known_states =
                add_test_reserved_bytes(snapshot.dsa_stored_bytes, entry.payload_bytes) &&
                known_states;
        } else if (entry.reservation_tier == block_backing_tier::file) {
            ++snapshot.file_blocks;
            known_states =
                add_test_reserved_bytes(snapshot.reserved_file_bytes, entry.reserved_bytes) &&
                known_states;
            known_states =
                add_test_reserved_bytes(snapshot.file_stored_bytes, entry.payload_bytes) &&
                known_states;
        } else {
            known_states = false;
        }
    }
    for (const shared_manifest_entry& manifest : g_shared->manifests) {
        if (manifest.state != manifest_state::empty) {
            snapshot.reader_pins += atomic_read(manifest.reader_pins);
            known_states =
                add_test_reserved_bytes(snapshot.row_group_count, manifest.row_group_count) &&
                known_states;
        }
    }
    const std::uint64_t state_total = snapshot.reserved_blocks + snapshot.stored_blocks +
                                      snapshot.referenced_blocks + snapshot.retiring_blocks +
                                      snapshot.release_committed_blocks;
    snapshot.coherent =
        known_states && state_total == snapshot.occupied_blocks &&
        snapshot.dsa_blocks + snapshot.file_blocks == accounted_blocks &&
        snapshot.reader_pins == pg_atomic_read_u64(&g_shared->reader_reference_count);
    LWLockRelease(g_storage_lock);
    const storage_runtime_snapshot runtime = runtime_snapshot();
    snapshot.reserved_hot_bytes = runtime.reserved_hot_bytes;
    snapshot.fixed_hot_bytes = runtime.fixed_hot_bytes;
    snapshot.live_hot_bytes = runtime.live_hot_bytes;
    snapshot.pending_hot_bytes = runtime.pending_hot_bytes;
    snapshot.coherent = snapshot.coherent && runtime.pending_hot_bytes_valid &&
                        runtime.pending_file_bytes_valid &&
                        runtime.reserved_hot_bytes >= runtime.fixed_hot_bytes &&
                        dynamic_hot_reservations <= runtime.reserved_hot_bytes;
    return snapshot;
}

} // namespace bioseq::cache::pg::storage

export namespace bioseq::cache::pg::storage::bootstrap_test_api {

/** Requires superuser access to the bootstrap test controls. */
inline void require_superuser() {
    if (!superuser()) {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            {
                .message = "bioseq_cache bootstrap test controls require superuser",
            });
    }
}

inline void
append_json_member(std::string& json, std::string_view key, std::string_view encoded_value) {
    if (json.size() != 1) {
        json.push_back(',');
    }
    json.push_back('"');
    json.append(key);
    json.append("\":");
    json.append(encoded_value);
}

template <class p_integer_type>
    requires std::is_integral_v<p_integer_type>
inline void append_json_integer(std::string& json, std::string_view key, p_integer_type value) {
    append_json_member(json, key, std::to_string(value));
}

inline void append_json_boolean(std::string& json, std::string_view key, bool value) {
    append_json_member(json, key, value ? "true" : "false");
}

/** Returns bootstrap counters and attachment state as a JSON value. */
[[nodiscard]] inline bioseq::postgres::support::jsonb_value bootstrap_status() {
    require_superuser();
    const storage_bootstrap_test_snapshot snapshot = test_storage_bootstrap_snapshot();
    std::string json{"{"};
    json.reserve(1'024);
    append_json_integer(json, "state", static_cast<std::uint32_t>(snapshot.state));
    append_json_integer(json, "failure", static_cast<std::uint32_t>(snapshot.failure));
    append_json_integer(json, "boot_nonce", snapshot.boot_nonce);
    append_json_integer(json, "initialization_attempts", snapshot.initialization_attempt_count);
    append_json_integer(json, "takeovers", snapshot.takeover_count);
    append_json_integer(json, "cleanups", snapshot.cleanup_count);
    append_json_integer(json, "attaches", snapshot.attach_count);
    append_json_integer(json, "validation_failures", snapshot.validation_failure_count);
    append_json_integer(json, "takeover_used", snapshot.takeover_used);
    append_json_integer(json, "live_unpublished_areas", snapshot.live_unpublished_area_count);
    append_json_integer(json, "cleanup_handle_pinned", snapshot.cleanup_handle_pinned);
    append_json_boolean(json, "cleanup_handle_published", snapshot.cleanup_handle_published);
    append_json_boolean(json, "ready_handle_published", snapshot.ready_handle_published);
    append_json_boolean(json, "root_published", snapshot.root_published);
    append_json_boolean(json, "storage_attached", snapshot.storage_attached);
    append_json_boolean(json, "mapping_pinned", snapshot.mapping_pinned);
    append_json_boolean(json, "storage_metadata_in_dsa", snapshot.storage_metadata_in_dsa);
    append_json_boolean(json, "reusable_wait_cvs_in_dsa", snapshot.reusable_wait_cvs_in_dsa);
    append_json_integer(json, "reusable_wait_cv_count", snapshot.reusable_wait_cv_count);
    append_json_boolean(json, "pause_reached", snapshot.pause_reached);
    append_json_boolean(
        json, "defer_background_initializer", snapshot.defer_background_initializer);
    append_json_integer(json, "last_cleanup_handle", snapshot.last_cleanup_handle);
    append_json_boolean(json, "last_cleanup_was_pinned", snapshot.last_cleanup_handle_was_pinned);
    append_json_boolean(
        json, "last_cleanup_release_verified", snapshot.last_cleanup_release_verified);
    append_json_integer(json,
                        "initializer_proc",
                        static_cast<std::uint32_t>(snapshot.initializer.owner.proc_number));
    append_json_integer(json, "initializer_pid", snapshot.initializer.owner.pid);
    append_json_integer(json, "initializer_nonce", snapshot.initializer.owner.backend_nonce);
    append_json_integer(
        json, "initializer_started_at", snapshot.initializer.owner.backend_started_at_us);
    append_json_integer(json, "initializer_claim", snapshot.initializer.claim_sequence);
    append_json_integer(json,
                        "last_initializer_proc",
                        static_cast<std::uint32_t>(snapshot.last_initializer.owner.proc_number));
    append_json_integer(json, "last_initializer_pid", snapshot.last_initializer.owner.pid);
    append_json_integer(
        json, "last_initializer_nonce", snapshot.last_initializer.owner.backend_nonce);
    append_json_integer(
        json, "last_initializer_started_at", snapshot.last_initializer.owner.backend_started_at_us);
    append_json_integer(json, "last_initializer_claim", snapshot.last_initializer.claim_sequence);
    json.push_back('}');
    return bioseq::postgres::support::jsonb_value{std::move(json)};
}

/** Attaches twice and returns the area/root comparisons and initialization counters. */
[[nodiscard]] inline bioseq::postgres::support::jsonb_value bootstrap_attach_twice() {
    require_superuser();
    const storage_bootstrap_attach_twice_snapshot snapshot = test_attach_storage_twice();
    std::string json{"{"};
    json.reserve(384);
    append_json_boolean(json, "first_attached", snapshot.first_attached);
    append_json_boolean(json, "second_attached", snapshot.second_attached);
    append_json_boolean(json, "same_area", snapshot.same_area);
    append_json_boolean(json, "same_root", snapshot.same_root);
    append_json_boolean(json, "mapping_pinned", snapshot.mapping_pinned);
    append_json_integer(json, "attempts_before", snapshot.attempts_before);
    append_json_integer(json, "attempts_after", snapshot.attempts_after);
    append_json_integer(json, "attaches_before", snapshot.attaches_before);
    append_json_integer(json, "attaches_after", snapshot.attaches_after);
    json.push_back('}');
    return bioseq::postgres::support::jsonb_value{std::move(json)};
}

/** Corrupts a named bootstrap field and checks that reattachment rejects it. */
[[nodiscard]] inline bool corrupt_bootstrap_manifest(const std::string& name) {
    require_superuser();
    shared_registry::shared_registry_test_manifest_corruption corruption{};
    if (name == "magic") {
        corruption = shared_registry::shared_registry_test_manifest_corruption::magic;
    } else if (name == "abi") {
        corruption = shared_registry::shared_registry_test_manifest_corruption::abi_revision;
    } else if (name == "boot_nonce") {
        corruption = shared_registry::shared_registry_test_manifest_corruption::boot_nonce;
    } else {
        bioseq::postgres::support::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message = "unknown bootstrap manifest corruption: " + name,
            });
    }
    return test_corrupt_storage_bootstrap_manifest(corruption);
}

} // namespace bioseq::cache::pg::storage::bootstrap_test_api

#endif
