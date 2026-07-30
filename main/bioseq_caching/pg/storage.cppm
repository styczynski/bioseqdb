/*-------------------------------------------------------------------------
 *
 * Generic PostgreSQL storage for cache column-store blocks.
 *
 * This module owns no planner or provider policy.  It supplies a bounded
 * postmaster-lifetime DSA hot tier, a private file overflow tier, an ABA-safe
 * block ledger, generation manifests, reader pins, and deterministic
 * reclamation primitives for the PostgreSQL adapter.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#if defined(BIOSEQ_CACHE_TESTING)
#include "fmgr.h"
#include "lib/stringinfo.h"
#endif
#include "miscadmin.h"
#include "port/pg_crc32c.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "utils/dsa.h"
#include "utils/backend_status.h"
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
import bioseq.common.memory_budget;
import bioseq.pg_sql_boundary;

export namespace bioseq::cache::pg::storage {

inline constexpr std::uint32_t kStorageAbiVersion = 7;
inline constexpr std::size_t kManifestCapacity = 256;
inline constexpr std::size_t kBlocksPerManifest = 32;
inline constexpr std::size_t kWaitersPerManifest = 64;
inline constexpr std::size_t kReaderPinsPerManifest = 64;
inline constexpr std::size_t kBlockLedgerCapacity =
    kManifestCapacity * kBlocksPerManifest + 256;
inline constexpr std::size_t kSharedDsaReservationCapacity = 1024;
inline constexpr std::uint64_t kDefaultDsaInitialBytes = 256ULL << 10;
inline constexpr std::uint64_t kDefaultHotCeilingBytes = 256ULL << 20;
inline constexpr std::uint64_t kDefaultFileCeilingBytes = 128ULL << 30;
inline constexpr std::uint64_t kDefaultMaximumBlockBytes = 64ULL << 20;
inline constexpr std::uint32_t kDefaultSweepIntervalMs = 1000;
inline constexpr std::uint32_t kDefaultReclaimBatch = 64;

/** Fixed postmaster configuration; no additional production GUC is needed. */
struct StorageConfig {
    std::uint64_t dsa_initial_bytes = kDefaultDsaInitialBytes;
    std::uint64_t hot_ceiling_bytes = kDefaultHotCeilingBytes;
    std::uint64_t file_ceiling_bytes = kDefaultFileCeilingBytes;
    std::uint64_t maximum_block_bytes = kDefaultMaximumBlockBytes;
    std::uint32_t sweep_interval_ms = kDefaultSweepIntervalMs;
    std::uint32_t reclaim_batch = kDefaultReclaimBatch;
    std::uint32_t soft_limit_percent = 80;
};

enum class StorageError : std::uint32_t {
    None = 0,
    NotPreloaded,
    BootstrapFailed,
    InvalidArgument,
    BlockTooLarge,
    LedgerFull,
    ManifestFull,
    WaiterFull,
    ReaderFull,
    DuplicateGeneration,
    MemoryPressure,
    HotTierFull,
    FileTierFull,
    FilesystemPressure,
    DsaAllocationFailed,
    UnsafePath,
    PathTooLong,
    OpenFailed,
    WriteFailed,
    SyncFailed,
    RenameFailed,
    ReadFailed,
    ShortRead,
    ChecksumMismatch,
    NotFound,
    StaleToken,
    InvalidState,
    Pinned,
};

enum class StorageChecksumStatus : std::uint8_t {
    NotChecked = 0,
    Valid,
    Invalid,
};

enum class StorageReadPhase : std::uint8_t {
    Prevalidation = 0,
    Replay,
};

/** Explicit reservation token; callers own release through PG cleanup paths. */
struct SharedDsaReservation {
    std::uint32_t slot = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < kSharedDsaReservationCapacity && sequence != 0 &&
            reserved == 0;
    }
};

struct SharedDsaReserveResult {
    StorageError error = StorageError::None;
    SharedDsaReservation reservation{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == StorageError::None &&
            static_cast<bool>(reservation);
    }
};

enum class WorkerLifecycle : std::uint32_t {
    NotRegistered = 0,
    Registered,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

enum class ManifestState : std::uint32_t {
    Empty = 0,
    Publishing,
    Complete,
    Retiring,
};

/** Full backend identity used for every storage-owned reference. */
struct StorageBackendIdentity {
    std::uint32_t proc_number =
        (std::numeric_limits<std::uint32_t>::max)();
    std::int32_t pid = 0;
    std::uint64_t backend_nonce = 0;
    std::int64_t backend_started_at_us = 0;

    [[nodiscard]] bool valid() const noexcept {
        return proc_number
                != (std::numeric_limits<std::uint32_t>::max)()
            && pid > 0
            && backend_nonce != 0
            && backend_started_at_us != 0;
    }

    [[nodiscard]] bool operator==(
        const StorageBackendIdentity&) const noexcept = default;
};

static_assert(std::is_standard_layout_v<StorageBackendIdentity>);
static_assert(std::is_trivially_copyable_v<StorageBackendIdentity>);

/** An ABA-safe capability for one stored but not necessarily published block. */
struct StoredBlockToken {
    std::uint32_t ledger_slot = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t sequence = 0;
    StorageBackendIdentity owner{};
    BlockReferenceV1 reference{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return sequence != 0
            && ledger_slot < kBlockLedgerCapacity
            && owner.valid()
            && reference.length != 0;
    }

};

struct StoreBlockResult {
    StorageError error = StorageError::None;
    StoredBlockToken token{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == StorageError::None && static_cast<bool>(token);
    }
};

/** Immutable generation information published with its block manifest. */
struct ManifestDescriptor {
    CacheEntryId id{};
    CacheEntryKind kind = CacheEntryKind::ExactFinalResult;
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

struct ManifestHandle {
    std::uint32_t slot = (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t slot_sequence = 0;
    CacheEntryId id{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < kManifestCapacity && slot_sequence != 0;
    }
};

struct PublishManifestResult {
    StorageError error = StorageError::None;
    ManifestHandle handle{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == StorageError::None && static_cast<bool>(handle);
    }
};

/** One ABA-safe, owner-bound entitlement to consume a manifest once. */
struct ManifestWaiterToken {
    ManifestHandle handle{};
    std::uint32_t waiter_slot =
        (std::numeric_limits<std::uint32_t>::max)();
    StorageBackendIdentity owner{};
    std::uint64_t owner_sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(handle)
            && waiter_slot < kWaitersPerManifest
            && owner.valid()
            && owner_sequence != 0;
    }
};

struct RegisterManifestWaiterResult {
    StorageError error = StorageError::None;
    ManifestWaiterToken token{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == StorageError::None && static_cast<bool>(token);
    }
};

/** A reader-owned snapshot.  The shared generation remains pinned until release. */
struct ManifestPin {
    ManifestHandle handle{};
    CacheEntryKind kind = CacheEntryKind::ExactFinalResult;
    std::uint32_t flags = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t row_count = 0;
    std::uint32_t row_group_count = 0;
    std::uint32_t database_oid = 0;
    std::uint32_t block_count = 0;
    std::uint32_t reader_slot =
        (std::numeric_limits<std::uint32_t>::max)();
    StorageBackendIdentity owner{};
    std::uint64_t owner_sequence = 0;
    std::array<BlockReferenceV1, kBlocksPerManifest> blocks{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return static_cast<bool>(handle)
            && block_count != 0
            && reader_slot < kReaderPinsPerManifest
            && owner.valid()
            && owner_sequence != 0;
    }
};

struct PinManifestResult {
    StorageError error = StorageError::None;
    ManifestPin pin{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == StorageError::None && static_cast<bool>(pin);
    }
};

struct JanitorSweepResult {
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

struct StorageQueryIoSnapshot {
    std::uint64_t prevalidation_read_bytes = 0;
    std::uint64_t prevalidation_read_duration_us = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t read_duration_us = 0;
    std::uint64_t checksum_failure_count = 0;
    StorageChecksumStatus checksum_status =
        StorageChecksumStatus::NotChecked;
};

struct StorageRuntimeSnapshot {
    bool bootstrap_ready = false;
    bool dsa_attached = false;
    BootstrapState bootstrap_state = BootstrapState::Uninitialized;
    WorkerLifecycle worker_lifecycle = WorkerLifecycle::NotRegistered;
    CacheCorrectnessReadiness correctness_readiness =
        CacheCorrectnessReadiness::PassthroughOnly;
    StorageError last_failure = StorageError::None;
    std::uint64_t boot_nonce = 0;
    std::uint64_t reserved_hot_bytes = 0;
    std::uint64_t fixed_hot_bytes = 0;
    std::uint64_t pending_hot_bytes = 0;
    std::uint64_t live_hot_bytes = 0;
    std::uint64_t reserved_file_bytes = 0;
    std::uint64_t dsa_stored_bytes = 0;
    std::uint64_t file_stored_bytes = 0;
    std::uint64_t row_group_count = 0;
    std::uint64_t prevalidation_read_bytes = 0;
    std::uint64_t prevalidation_read_duration_us = 0;
    std::uint64_t read_bytes = 0;
    std::uint64_t read_duration_us = 0;
    std::uint64_t checksum_failure_count = 0;
    StorageChecksumStatus checksum_status =
        StorageChecksumStatus::NotChecked;
    std::uint64_t hot_ceiling_bytes = 0;
    std::uint64_t file_ceiling_bytes = 0;
    std::uint64_t manifest_count = 0;
    std::uint64_t waiter_reference_count = 0;
    std::uint64_t reader_reference_count = 0;
    std::uint64_t completed_sweeps = 0;
    std::int64_t last_sweep_at_us = 0;
    std::int64_t worker_heartbeat_at_us = 0;
    int worker_pid = 0;
    std::uint32_t worker_proc_number =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint64_t worker_backend_nonce = 0;
    std::int64_t worker_backend_started_at_us = 0;
};

#if defined(BIOSEQ_CACHE_TESTING)
/** Test-only shared override and live ledger evidence for tier assertions. */
struct StorageTierTestSnapshot {
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

/** Pinned-root ownership evidence used by bootstrap/TAP tests. */
struct StorageBootstrapTestSnapshot {
    BootstrapState state = BootstrapState::Uninitialized;
    shared_registry::SharedRegistryErrorV1 failure =
        shared_registry::SharedRegistryErrorV1::None;
    std::uint64_t boot_nonce = 0;
    std::uint64_t initialization_attempt_count = 0;
    std::uint64_t takeover_count = 0;
    std::uint64_t cleanup_count = 0;
    std::uint64_t attach_count = 0;
    std::uint64_t validation_failure_count = 0;
    shared_registry::SharedRegistryOwnerTokenV1 initializer{};
    shared_registry::SharedRegistryOwnerTokenV1 last_initializer{};
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

/** One-backend proof that repeated attachment does not initialize twice. */
struct StorageBootstrapAttachTwiceSnapshot {
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

/** Force later block reservations to bypass DSA in testing builds only. */
[[nodiscard]] bool set_test_force_file_tier(bool enabled) noexcept;

/** Snapshot backing tiers without exposing paths or mutable ledger records. */
[[nodiscard]] StorageTierTestSnapshot test_storage_tier_snapshot() noexcept;

[[nodiscard]] StorageBootstrapTestSnapshot
test_storage_bootstrap_snapshot() noexcept;

[[nodiscard]] bool test_arm_storage_bootstrap_pause() noexcept;
void test_release_storage_bootstrap_pause() noexcept;
[[nodiscard]] bool test_inject_storage_bootstrap_allocation_failure(
    std::uint32_t ordinal) noexcept;
[[nodiscard]] bool test_inject_storage_bootstrap_abi_mismatch(
    bool enabled) noexcept;
[[nodiscard]] StorageBootstrapAttachTwiceSnapshot
test_attach_storage_twice() noexcept;
[[nodiscard]] bool test_corrupt_storage_bootstrap_manifest(
    shared_registry::SharedRegistryTestManifestCorruptionV1 corruption)
    noexcept;
[[nodiscard]] bool test_detach_storage_mapping() noexcept;
#endif

/** Return the fixed storage-metadata bytes allocated inside the pinned DSA. */
[[nodiscard]] std::size_t storage_shared_memory_size(
    const StorageConfig& config = {}) noexcept;

/** Request the bounded registry bootstrap/ledgers and named lock tranches. */
void request_storage_resources(const StorageConfig& config = {});

/** Initialize only the bounded bootstrap; dynamic DSA creation remains lazy. */
[[nodiscard]] bool startup_storage(
    std::uint64_t boot_nonce,
    const StorageConfig& config = {});

/** Attach this backend/worker to the postmaster-lifetime storage state. */
[[nodiscard]] bool attach_storage();

/** Return true only when shared bootstrap and this process's DSA attach succeeded. */
[[nodiscard]] bool storage_ready() noexcept;

/** Store a checksummed block, preferring DSA and falling back to a private file. */
[[nodiscard]] StoreBlockResult store_block(std::span<const std::byte> bytes);

/** Reserve one later caller-owned DSA allocation against the shared ceiling. */
[[nodiscard]] SharedDsaReserveResult reserve_shared_dsa_allocation(
    std::uint64_t payload_bytes) noexcept;

/** Release one explicit reservation exactly once. */
[[nodiscard]] bool release_shared_dsa_allocation(
    SharedDsaReservation* reservation) noexcept;

/** Transfer a pending reservation to live accounting exactly once. */
[[nodiscard]] bool publish_shared_dsa_allocation(
    SharedDsaReservation* reservation) noexcept;

/** Discard a stored block that was not transferred into a manifest. */
[[nodiscard]] StorageError discard_unpublished_block(
    const StoredBlockToken& token);

/** Atomically publish a generation and transfer all tokens into its manifest. */
[[nodiscard]] PublishManifestResult publish_manifest(
    const ManifestDescriptor& descriptor,
    std::span<const StoredBlockToken> blocks);

/** Adapt one physical coalescer flush to the generic storage block ledger. */
class StorageColumnStorePhysicalSink {
public:
    using token_type = StoredBlockToken;

    [[nodiscard]] std::expected<token_type, FormatError> store_physical(
            std::span<const std::byte> bytes) {
        const StoreBlockResult stored = store_block(bytes);
        if (!stored) {
            last_error_ = stored.error;
            return std::unexpected(FormatError::SinkFailed);
        }
        last_error_ = StorageError::None;
        return stored.token;
    }

    void discard_physical(const token_type& token) noexcept {
        const StorageError discarded = discard_unpublished_block(token);
        if (discarded != StorageError::None) {
            last_error_ = discarded;
        }
    }

    [[nodiscard]] StorageError last_error() const noexcept {
        return last_error_;
    }

private:
    StorageError last_error_ = StorageError::None;
};

/**
 * PostgreSQL-backed logical sink.  It retains one bounded physical buffer and
 * returns small logical handles while DSA/file capabilities stay internal.
 */
class StorageColumnStoreBlockSink {
public:
    using token_type = ColumnStoreLogicalBlockToken;

    explicit StorageColumnStoreBlockSink(
        std::uint64_t maximum_physical_block_bytes =
            kDefaultMaximumBlockBytes) noexcept
        : coalesced_(
            physical_,
            maximum_physical_block_bytes,
            static_cast<std::uint32_t>(kBlocksPerManifest)) {}

    StorageColumnStoreBlockSink(const StorageColumnStoreBlockSink&) = delete;
    StorageColumnStoreBlockSink& operator=(
        const StorageColumnStoreBlockSink&) = delete;
    StorageColumnStoreBlockSink(StorageColumnStoreBlockSink&&) = delete;
    StorageColumnStoreBlockSink& operator=(
        StorageColumnStoreBlockSink&&) = delete;

    [[nodiscard]] std::expected<token_type, FormatError> store(
        ColumnStoreBlockKind kind,
        std::uint64_t ordinal,
        std::span<const std::byte> bytes) {
        return coalesced_.store(kind, ordinal, bytes);
    }

    void discard(const token_type& token) noexcept {
        coalesced_.discard(token);
    }

    [[nodiscard]] std::expected<std::vector<StoredBlockToken>, FormatError>
    manifest_tokens(std::span<const token_type> logical_order) const {
        return coalesced_.manifest_tokens(logical_order);
    }

    void mark_published() noexcept {
        coalesced_.mark_published();
    }

    [[nodiscard]] StorageError last_storage_error() const noexcept {
        return physical_.last_error();
    }

    [[nodiscard]] std::uint32_t physical_block_count() const noexcept {
        return coalesced_.physical_block_count();
    }

    [[nodiscard]] std::uint64_t retained_buffer_bytes() const noexcept {
        return coalesced_.retained_buffer_bytes();
    }

private:
    StorageColumnStorePhysicalSink physical_;
    CoalescingColumnStoreSink<StorageColumnStorePhysicalSink> coalesced_;
};

/**
 * Publish coalesced blocks atomically, or synchronously release every
 * unpublished physical capability on any validation/storage failure.
 */
[[nodiscard]] inline PublishManifestResult publish_streamed_column_store(
    ManifestDescriptor descriptor,
    StorageColumnStoreBlockSink& sink,
    StreamedColumnStoreBlocks<StorageColumnStoreBlockSink>& blocks) {
    auto physical_tokens = sink.manifest_tokens(blocks.tokens());
    if (!physical_tokens.has_value()) {
        const FormatError format_error = physical_tokens.error();
        const StorageError sink_error = sink.last_storage_error();
        blocks.discard();
        return PublishManifestResult{
            .error = sink_error != StorageError::None
                ? sink_error
                : format_error == FormatError::PhysicalBlockLimitExceeded
                    ? StorageError::ManifestFull
                    : StorageError::InvalidArgument,
        };
    }
    if (physical_tokens->size() > kBlocksPerManifest) {
        blocks.discard();
        return PublishManifestResult{.error = StorageError::ManifestFull};
    }
    descriptor.logical_bytes = blocks.logical_bytes();
    descriptor.row_count = blocks.row_count();
    descriptor.row_group_count = blocks.row_group_count();
    PublishManifestResult published = publish_manifest(
        descriptor, *physical_tokens);
    if (!published) {
        blocks.discard();
        return published;
    }
    sink.mark_published();
    blocks.mark_published();
    return published;
}

/** Pin one non-expired complete manifest for a reader. */
[[nodiscard]] PinManifestResult pin_manifest(
    const CacheEntryId& id,
    std::int64_t now_us = 0);

/**
 * Register a pre-expiry waiter whose entitlement can survive TTL expiry.
 *
 * Terminal publishers may name another live backend while holding the
 * registry's terminal-publication ordering.  The returned capability is
 * still bound to that backend and to this exact manifest ABA generation.
 */
[[nodiscard]] RegisterManifestWaiterResult register_manifest_waiter(
    const ManifestHandle& handle,
    const StorageBackendIdentity& owner,
    std::int64_t registered_at_us = 0);

/**
 * Recover this backend's unique published entitlement by generation id.
 * Retiring manifests remain discoverable so cancellation can release a token
 * after registry publication is rolled back; conversion still requires
 * Complete and therefore fails closed for that state.
 */
[[nodiscard]] RegisterManifestWaiterResult current_manifest_waiter(
    const CacheEntryId& id);

/** Convert one registered waiter to a reader pin even after expiry. */
[[nodiscard]] PinManifestResult convert_waiter_to_pin(
    const ManifestWaiterToken& token);

/** Release one reader pin exactly once. */
[[nodiscard]] StorageError unpin_manifest(const ManifestPin& pin) noexcept;

/**
 * Release one waiter reference without pinning.
 *
 * The caller may be the terminal publisher rolling back an explicit-owner
 * registration, so possession of the ABA capability is the authority.
 */
[[nodiscard]] StorageError unregister_manifest_waiter(
    const ManifestWaiterToken& token) noexcept;

/** Copy and checksum-verify one block from a pinned generation. */
[[nodiscard]] StorageError read_pinned_block(
    const ManifestPin& pin,
    std::size_t block_index,
    std::span<std::byte> destination,
    StorageReadPhase phase = StorageReadPhase::Prevalidation);

/** Reset/query process-local read telemetry at a query boundary. */
void reset_query_io_metrics() noexcept;
[[nodiscard]] StorageQueryIoSnapshot query_io_metrics() noexcept;

/** Mark a generation for reclamation. Existing pins drain before physical free. */
[[nodiscard]] StorageError retire_manifest(const ManifestHandle& handle) noexcept;

/** Run one bounded TTL/LRU/abandoned-producer reclamation pass. */
[[nodiscard]] JanitorSweepResult janitor_sweep(
    std::int64_t now_us = 0,
    bool aggressive = false);

/** Ask the static janitor for a normal or aggressive sweep. */
void request_janitor_sweep(bool aggressive = false) noexcept;

/** Publish the highest correctness level actually proven by the adapter. */
[[nodiscard]] bool publish_correctness_readiness(
    CacheCorrectnessReadiness readiness) noexcept;

/** Record worker lifecycle/heartbeat without exposing the shared header. */
void publish_worker_lifecycle(
    WorkerLifecycle lifecycle,
    int worker_pid = 0) noexcept;
void publish_worker_heartbeat(std::int64_t now_us = 0) noexcept;

/** Snapshot bounded counters and lifecycle diagnostics. */
[[nodiscard]] StorageRuntimeSnapshot runtime_snapshot() noexcept;

/** Authoritative interval copied into shared state at startup. */
[[nodiscard]] std::uint32_t janitor_interval_ms() noexcept;

} // namespace bioseq::cache::pg::storage

namespace bioseq::cache::pg::storage {
namespace {

inline constexpr std::uint32_t kStorageMagic = 0x42535452U; /* BSTR */
inline constexpr char kStorageLockTranche[] = "bioseq_cache storage";
inline constexpr char kStoreSubdirectory[] = "pg_bioseq_cache";
inline constexpr std::uint32_t kInvalidSlot =
    (std::numeric_limits<std::uint32_t>::max)();

enum class BlockLedgerState : std::uint32_t {
    Empty = 0,
    Reserved,
    Stored,
    Referenced,
    Retiring,
    ReleaseCommitted,
};

enum class SharedDsaReservationState : std::uint32_t {
    Empty = 0,
    Pending,
    Live,
    Releasing,
};

struct SharedDsaReservationEntry {
    SharedDsaReservationState state;
    std::uint32_t reserved;
    StorageBackendIdentity owner;
    std::uint64_t sequence;
    std::uint64_t accounted_bytes;
};

struct BlockLedgerEntry {
    BlockLedgerState state;
    BlockBackingTierV1 reservation_tier;
    bool reservation_accounted;
    bool live_accounted;
    bool file_finalized;
    std::uint8_t reserved8;
    StorageBackendIdentity owner;
    std::uint32_t manifest_slot;
    std::uint32_t database_oid;
    std::uint64_t sequence;
    std::int64_t created_at_us;
    std::uint64_t payload_bytes;
    std::uint64_t reserved_bytes;
    CacheDigest256 file_digest;
    std::uint64_t file_generation;
    BlockReferenceV1 reference;
};

struct SharedManifestWaiter {
    bool active;
    std::uint8_t reserved8[3];
    StorageBackendIdentity owner;
    std::uint64_t owner_sequence;
};

struct SharedManifestReader {
    bool active;
    std::uint8_t reserved8[3];
    StorageBackendIdentity owner;
    std::uint64_t owner_sequence;
};

/** ABA-safe exclusive ownership of one janitor sweep. */
struct SweepOwnerToken {
    StorageBackendIdentity owner{};
    std::uint64_t sequence = 0;

    [[nodiscard]] bool valid() const noexcept {
        return owner.valid() && sequence != 0;
    }

    [[nodiscard]] bool operator==(
        const SweepOwnerToken&) const noexcept = default;
};

struct SharedManifestEntry {
    ManifestState state;
    CacheEntryKind kind;
    std::uint16_t block_count;
    std::uint32_t flags;
    bool manifest_counted;
    std::uint8_t reserved8[7];
    std::uint64_t slot_sequence;
    CacheEntryId id;
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
    std::array<SharedManifestReader, kReaderPinsPerManifest> readers;
    std::array<SharedManifestWaiter, kWaitersPerManifest> waiters;
    std::array<std::uint32_t, kBlocksPerManifest> ledger_slots;
    std::array<BlockReferenceV1, kBlocksPerManifest> blocks;
};

struct StorageSharedState {
    std::uint32_t magic;
    std::uint32_t abi_version;
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
    StorageBackendIdentity worker_owner;
    SweepOwnerToken sweep_owner;
    std::uint32_t reference_scan_cursor;
    std::uint32_t block_scan_cursor;
    SharedDsaReservationEntry
        shared_dsa_reservations[kSharedDsaReservationCapacity];
    BlockLedgerEntry block_ledger[kBlockLedgerCapacity];
    SharedManifestEntry manifests[kManifestCapacity];
};

static_assert(std::is_standard_layout_v<StorageSharedState>);
static_assert(alignof(StorageSharedState) <= MAXIMUM_ALIGNOF);

inline StorageSharedState* g_shared = nullptr;
inline LWLock* g_storage_lock = nullptr;
inline dsa_area* g_dsa = nullptr;
inline shared_registry::SharedRegistryRootV1* g_registry_root = nullptr;
inline shared_registry::SharedRegistryAccountingAccessV1 g_accounting{};
inline StorageConfig g_requested_config{};
inline shared_registry::SharedRegistryPreloadRequestV1 g_registry_request{};
inline CacheCorrectnessReadiness g_pending_correctness_readiness =
    CacheCorrectnessReadiness::PassthroughOnly;
inline WorkerLifecycle g_pending_worker_lifecycle =
    WorkerLifecycle::NotRegistered;
inline bool g_exit_callback_registered = false;
inline StorageQueryIoSnapshot g_query_io_metrics{};

inline constexpr std::uint64_t kPgCacheMinimumAvailableBytes = 2ULL << 30U;

[[nodiscard]] std::uint64_t total_cache_ceiling(
        const StorageConfig& config) noexcept {
    const std::uint64_t physical =
        bioseq::common::memory::system_total_bytes();
    if (physical == 0) {
        return config.hot_ceiling_bytes;
    }
    const std::uint64_t physical_share =
        physical / kCacheMemoryFractionDenominator *
        kCacheMemoryFractionNumerator;
    return std::min(config.hot_ceiling_bytes, physical_share);
}

[[nodiscard]] constexpr std::uint64_t fixed_registry_bytes(
        const shared_registry::SharedRegistryPreloadRequestV1& request)
        noexcept {
    return request.persistent.fixed_dsa_reserved_bytes;
}

[[nodiscard]] std::uint64_t hot_payload_ceiling(
        const StorageConfig& config,
        const shared_registry::SharedRegistryPreloadRequestV1& request)
        noexcept {
    const std::uint64_t total = total_cache_ceiling(config);
    return fixed_registry_bytes(request) <= total ? total : 0;
}

[[nodiscard]] bool config_valid(const StorageConfig& config) noexcept {
    return config.dsa_initial_bytes >= dsa_minimum_size()
        && config.dsa_initial_bytes <=
            (std::numeric_limits<std::size_t>::max)() - MAXIMUM_ALIGNOF
        && config.hot_ceiling_bytes != 0
        && config.file_ceiling_bytes != 0
        && config.maximum_block_bytes != 0
        && config.maximum_block_bytes <= kMaxFileCacheBytes
        && config.sweep_interval_ms >= 10
        && config.reclaim_batch != 0
        && config.reclaim_batch <= kManifestCapacity
        && config.soft_limit_percent >= 1
        && config.soft_limit_percent <= 100;
}

[[nodiscard]] constexpr bool same_config(
        const StorageConfig& left,
        const StorageConfig& right) noexcept {
    return left.dsa_initial_bytes == right.dsa_initial_bytes
        && left.hot_ceiling_bytes == right.hot_ceiling_bytes
        && left.file_ceiling_bytes == right.file_ceiling_bytes
        && left.maximum_block_bytes == right.maximum_block_bytes
        && left.sweep_interval_ms == right.sweep_interval_ms
        && left.reclaim_batch == right.reclaim_batch
        && left.soft_limit_percent == right.soft_limit_percent;
}

[[nodiscard]] shared_registry::SharedRegistryPreloadRequestV1
registry_preload_request(const StorageConfig& config) noexcept {
    shared_registry::SharedRegistryPreloadRequestV1 failed{};
    if (!config_valid(config) || MaxBackends <= 0 ||
        static_cast<std::uint64_t>(MaxBackends) >
            kMaximumTransitionOwnersV1 ||
        sizeof(StorageSharedState) >
            (std::numeric_limits<std::uint32_t>::max)()) {
        failed.error =
            shared_registry::SharedRegistryErrorV1::InvalidConfiguration;
        return failed;
    }
    shared_registry::SharedRegistryConfigV1 registry_config{
        .max_backends = static_cast<std::uint32_t>(MaxBackends),
        .extension_metadata_bytes =
            static_cast<std::uint32_t>(sizeof(StorageSharedState)),
        .dsa_allocator_reserve_bytes = MAXALIGN(config.dsa_initial_bytes),
        .dsa_size_limit_bytes =
            (std::numeric_limits<std::uint64_t>::max)(),
    };
    auto preliminary =
        shared_registry::shared_registry_preload_request(registry_config);
    if (!preliminary) {
        return preliminary;
    }
    const std::uint64_t fixed_bytes = fixed_registry_bytes(preliminary);
    const std::uint64_t total_ceiling = total_cache_ceiling(config);
    if (fixed_bytes > total_ceiling) {
        preliminary.error =
            shared_registry::SharedRegistryErrorV1::InvalidConfiguration;
        return preliminary;
    }
    registry_config.dsa_size_limit_bytes = total_ceiling;
    return shared_registry::shared_registry_preload_request(registry_config);
}

[[nodiscard]] pg_atomic_uint64& hot_reserved_bytes() noexcept {
    return *g_accounting.reserved_dsa_bytes;
}

[[nodiscard]] pg_atomic_uint64& file_reserved_bytes() noexcept {
    return *g_accounting.reserved_file_bytes;
}

[[nodiscard]] std::int64_t current_time_us(std::int64_t value = 0) noexcept {
    return value != 0
        ? value
        : static_cast<std::int64_t>(GetCurrentTimestamp());
}

[[nodiscard]] std::uint32_t crc32c(
        std::span<const std::byte> bytes) noexcept {
    pg_crc32c checksum = 0;
    INIT_CRC32C(checksum);
    if (!bytes.empty()) {
        COMP_CRC32C(checksum, bytes.data(), bytes.size());
    }
    FIN_CRC32C(checksum);
    return static_cast<std::uint32_t>(checksum);
}

struct PgAtomicReservationCounter {
    pg_atomic_uint64* value = nullptr;

    [[nodiscard]] std::uint64_t load() noexcept {
        return pg_atomic_read_u64(value);
    }

    [[nodiscard]] bool compare_exchange(
            std::uint64_t& expected,
            std::uint64_t desired) noexcept {
        uint64 native_expected = static_cast<uint64>(expected);
        const bool exchanged = pg_atomic_compare_exchange_u64(
            value, &native_expected, static_cast<uint64>(desired));
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

struct PgSystemMemoryBudget {
    std::uint64_t payload_ceiling = 0;
    pg_atomic_uint64* shared_reserved = nullptr;

    [[nodiscard]] std::uint64_t allocatable_bytes() const noexcept {
        const std::uint64_t available =
            bioseq::common::memory::system_available_bytes();
        if (available == 0) {
            return payload_ceiling;
        }
        const std::uint64_t free_headroom =
            available > kPgCacheMinimumAvailableBytes
                ? available - kPgCacheMinimumAvailableBytes
                : 0;
        return std::min(payload_ceiling, free_headroom);
    }

    [[nodiscard]] bool under_pressure() const noexcept {
        const std::uint64_t available =
            bioseq::common::memory::system_available_bytes();
        return (available != 0 &&
                available < kPgCacheMinimumAvailableBytes) ||
            (shared_reserved != nullptr &&
             pg_atomic_read_u64(shared_reserved) > payload_ceiling);
    }

    [[nodiscard]] std::uint64_t total_bytes() const noexcept {
        return bioseq::common::memory::system_total_bytes();
    }
};

struct FixedAdmissionBudget {
    std::uint64_t ceiling = 0;
    bool pressure = false;

    [[nodiscard]] std::uint64_t allocatable_bytes() const noexcept {
        return ceiling;
    }

    [[nodiscard]] bool under_pressure() const noexcept {
        return pressure;
    }
};

[[nodiscard]] std::uint32_t atomic_read(
        const pg_atomic_uint32& value) noexcept {
    return pg_atomic_read_u32(
        const_cast<pg_atomic_uint32*>(&value));
}

[[nodiscard]] std::uint64_t atomic_read(
        const pg_atomic_uint64& value) noexcept {
    return pg_atomic_read_u64(
        const_cast<pg_atomic_uint64*>(&value));
}

[[nodiscard]] bool release_reservation(
        pg_atomic_uint64& counter,
        std::uint64_t bytes) noexcept {
    PgAtomicReservationCounter adapter{.value = &counter};
    return adapter.try_subtract(bytes);
}

void remember_failure(StorageError error) noexcept {
    if (g_shared != nullptr && error != StorageError::None) {
        pg_atomic_write_u32(
            &g_shared->last_failure,
            static_cast<std::uint32_t>(error));
    }
}

[[nodiscard]] bool build_store_directory(
        char* output, std::size_t capacity) noexcept {
    if (DataDir == nullptr) {
        return false;
    }
    const int length = snprintf(
        output, capacity, "%s/%s", DataDir, kStoreSubdirectory);
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

[[nodiscard]] constexpr std::uint64_t select_storage_boot_nonce(
        std::uint64_t unpublished_boot_nonce,
        std::uint64_t attached_boot_nonce) noexcept {
    return unpublished_boot_nonce != 0
        ? unpublished_boot_nonce
        : attached_boot_nonce;
}

static_assert(select_storage_boot_nonce(0x1234U, 0) == 0x1234U);
static_assert(select_storage_boot_nonce(0, 0) == 0);

[[nodiscard]] bool build_boot_directory(
        char* output,
        std::size_t capacity,
        std::uint64_t unpublished_boot_nonce = 0) noexcept {
    char root[MAXPGPATH]{};
    if (!build_store_directory(root, sizeof(root))) {
        return false;
    }
    const std::uint64_t attached_boot_nonce = g_shared == nullptr
        ? 0
        : pg_atomic_read_u64(&g_shared->boot_nonce);
    const std::uint64_t boot_nonce = select_storage_boot_nonce(
        unpublished_boot_nonce, attached_boot_nonce);
    if (boot_nonce == 0) {
        return false;
    }
    const int length = snprintf(
        output, capacity, "%s/%016llx", root,
        static_cast<unsigned long long>(boot_nonce));
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

[[nodiscard]] bool build_database_directory(
        std::uint32_t database_oid,
        char* output,
        std::size_t capacity) noexcept {
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
    return S_ISDIR(status.st_mode)
        && status.st_uid == ::geteuid()
        && (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) == S_IRWXU;
}

[[nodiscard]] StorageError ensure_owned_directory(const char* path) noexcept {
    if (MakePGDirectory(path) != 0 && errno != EEXIST) {
        return StorageError::OpenFailed;
    }
    return safe_owned_directory(path)
        ? StorageError::None
        : StorageError::UnsafePath;
}

[[nodiscard]] StorageError ensure_store_directory(
        std::uint64_t unpublished_boot_nonce = 0) noexcept {
    char root[MAXPGPATH]{};
    char boot[MAXPGPATH]{};
    if (!build_store_directory(root, sizeof(root)) ||
        !build_boot_directory(
            boot, sizeof(boot), unpublished_boot_nonce)) {
        return StorageError::PathTooLong;
    }
    StorageError error = ensure_owned_directory(root);
    if (error != StorageError::None) {
        return error;
    }
    return ensure_owned_directory(boot);
}

[[nodiscard]] StorageError ensure_database_directory(
        std::uint32_t database_oid) noexcept {
    char directory[MAXPGPATH]{};
    if (!build_database_directory(
            database_oid, directory, sizeof(directory))) {
        return StorageError::PathTooLong;
    }
    const StorageError root_error = ensure_store_directory();
    return root_error == StorageError::None
        ? ensure_owned_directory(directory)
        : root_error;
}

[[nodiscard]] bool is_hex(char character) noexcept {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f');
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

[[nodiscard]] StorageError clean_orphaned_store_files() noexcept {
    char root[MAXPGPATH]{};
    if (!build_store_directory(root, sizeof(root)) ||
        !safe_owned_directory(root)) {
        return StorageError::UnsafePath;
    }
    DIR* stream = AllocateDir(root);
    if (stream == nullptr) {
        return StorageError::OpenFailed;
    }
    StorageError error = StorageError::None;
    while (struct dirent* item = ReadDir(stream, root)) {
        if (std::strcmp(item->d_name, ".") == 0 ||
            std::strcmp(item->d_name, "..") == 0) {
            continue;
        }
        if (!boot_directory_name(item->d_name)) {
            error = StorageError::UnsafePath;
            break;
        }
        char path[MAXPGPATH]{};
        const int length = snprintf(
            path, sizeof(path), "%s/%s", root, item->d_name);
        if (length < 0 || static_cast<std::size_t>(length) >= sizeof(path) ||
            !safe_owned_directory(path) || !rmtree(path, true)) {
            error = StorageError::UnsafePath;
            break;
        }
    }
    static_cast<void>(FreeDir(stream));
    return error;
}

void digest_hex(const CacheDigest256& digest, char* output) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        output[index * 2U] = digits[digest.bytes[index] >> 4U];
        output[index * 2U + 1U] = digits[digest.bytes[index] & 0x0fU];
    }
    output[digest.bytes.size() * 2U] = '\0';
}

[[nodiscard]] bool build_staging_file_path(
        std::uint32_t database_oid,
        const StorageBackendIdentity& owner,
        std::uint64_t file_id,
        char* output,
        std::size_t capacity) noexcept {
    char directory[MAXPGPATH]{};
    if (!owner.valid() || !build_database_directory(
            database_oid, directory, sizeof(directory))) {
        return false;
    }
    const int length = snprintf(
        output, capacity,
        "%s/.stage-%08x-%08x-%016llx-%016llx-%016llx.partial",
        directory, owner.proc_number, static_cast<std::uint32_t>(owner.pid),
        static_cast<unsigned long long>(owner.backend_nonce),
        static_cast<unsigned long long>(owner.backend_started_at_us),
        static_cast<unsigned long long>(file_id));
    return length >= 0 && static_cast<std::size_t>(length) < capacity;
}

[[nodiscard]] bool build_generation_file_paths(
        std::uint32_t database_oid,
        const CacheEntryId& id,
        const StorageBackendIdentity& owner,
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
    const int final_length = snprintf(
        final, final_capacity, "%s/%s.%016llx.cache", directory, digest,
        static_cast<unsigned long long>(id.generation));
    const int temporary_length = snprintf(
        temporary, temporary_capacity,
        "%s/.%s.%016llx-%08x-%08x-%016llx-%016llx.partial",
        directory, digest, static_cast<unsigned long long>(id.generation),
        owner.proc_number, static_cast<std::uint32_t>(owner.pid),
        static_cast<unsigned long long>(owner.backend_nonce),
        static_cast<unsigned long long>(owner.backend_started_at_us));
    return final_length >= 0 && temporary_length >= 0 &&
        static_cast<std::size_t>(final_length) < final_capacity &&
        static_cast<std::size_t>(temporary_length) < temporary_capacity;
}

[[nodiscard]] bool unlink_cache_file(
        const char* path,
        std::uint64_t maximum_link_count = 1) noexcept {
    struct stat status{};
    if (::lstat(path, &status) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
        status.st_nlink < 1 ||
        static_cast<std::uint64_t>(status.st_nlink) > maximum_link_count ||
        (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
            (S_IRUSR | S_IWUSR)) {
        return false;
    }
    return durable_unlink(path, LOG) == 0 || errno == ENOENT;
}

[[nodiscard]] StorageError filesystem_capacity(
        std::uint64_t& capacity,
        std::uint64_t& available) noexcept {
    char directory[MAXPGPATH]{};
    if (!build_store_directory(directory, sizeof(directory))) {
        return StorageError::PathTooLong;
    }
    struct statvfs status{};
    if (::statvfs(directory, &status) != 0) {
        return StorageError::FilesystemPressure;
    }
    const std::uint64_t fragment_size = status.f_frsize != 0
        ? static_cast<std::uint64_t>(status.f_frsize)
        : static_cast<std::uint64_t>(status.f_bsize);
    const auto checked_capacity = checked_mul(
        fragment_size, static_cast<std::uint64_t>(status.f_blocks));
    const auto checked_available = checked_mul(
        fragment_size, static_cast<std::uint64_t>(status.f_bavail));
    if (!checked_capacity.has_value() || !checked_available.has_value()) {
        return StorageError::FilesystemPressure;
    }
    capacity = *checked_capacity;
    available = *checked_available;
    return StorageError::None;
}

/** Derive the cache nonce carried alongside the independently checked fields. */
[[nodiscard]] constexpr std::uint64_t storage_backend_nonce(
        std::uint32_t proc_number,
        int pid,
        std::int64_t started_at_us) noexcept {
    const std::uint64_t nonce = static_cast<std::uint64_t>(started_at_us)
        ^ (static_cast<std::uint64_t>(pid) << 32U)
        ^ static_cast<std::uint64_t>(proc_number);
    return nonce == 0 ? 1 : nonce;
}

/** Capture this process without consulting PostgreSQL's backend arrays. */
[[nodiscard]] StorageBackendIdentity current_backend_identity() noexcept {
    const std::uint32_t proc_number =
        static_cast<std::uint32_t>(MyProcNumber);
    const std::int64_t started_at =
        static_cast<std::int64_t>(MyStartTimestamp);
    if (MyProcPid <= 0
        || proc_number == static_cast<std::uint32_t>(INVALID_PROC_NUMBER)
        || started_at == 0) {
        return {};
    }
    return StorageBackendIdentity{
        .proc_number = proc_number,
        .pid = MyProcPid,
        .backend_nonce = storage_backend_nonce(
            proc_number, MyProcPid, started_at),
        .backend_started_at_us = started_at,
    };
}

/**
 * Probe one copied owner token.  Every caller invokes this after releasing
 * storage/cache locks and later rechecks the complete observation before any
 * mutation.
 */
[[nodiscard]] bool backend_identity_alive(
        const StorageBackendIdentity& owner) noexcept {
    if (ProcGlobal == nullptr || !owner.valid()
        || owner.proc_number
            >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    volatile PgBackendStatus* status = pgstat_get_beentry_by_proc_number(
        static_cast<ProcNumber>(owner.proc_number));
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
        if (pgstat_read_activity_complete(
                before_changecount, after_changecount)) {
            status_stable = true;
            break;
        }
    }
    if (!status_stable) {
        /* A racing status writer is not proof of death. */
        return true;
    }
    PGPROC* process = ProcNumberGetProc(
        static_cast<ProcNumber>(owner.proc_number));
    return process != nullptr
        && process->pid == owner.pid
        && status_pid == owner.pid
        && status_started_at == owner.backend_started_at_us
        && storage_backend_nonce(
            owner.proc_number, owner.pid, status_started_at)
            == owner.backend_nonce;
}

[[nodiscard]] bool deactivate_waiter_locked(
        SharedManifestEntry& manifest,
        SharedManifestWaiter& waiter) noexcept {
    if (!waiter.active) {
        return false;
    }
    const std::uint32_t references = pg_atomic_read_u32(
        &manifest.waiter_references);
    const std::uint64_t global_references = pg_atomic_read_u64(
        &g_shared->waiter_reference_count);
    if (references == 0 || global_references == 0) {
        return false;
    }
    waiter = SharedManifestWaiter{};
    static_cast<void>(pg_atomic_fetch_sub_u32(
        &manifest.waiter_references, 1));
    static_cast<void>(pg_atomic_fetch_sub_u64(
        &g_shared->waiter_reference_count, 1));
    return true;
}

[[nodiscard]] bool deactivate_reader_locked(
        SharedManifestEntry& manifest,
        SharedManifestReader& reader) noexcept {
    if (!reader.active) {
        return false;
    }
    const std::uint32_t pins = pg_atomic_read_u32(&manifest.reader_pins);
    const std::uint64_t global_pins = pg_atomic_read_u64(
        &g_shared->reader_reference_count);
    if (pins == 0 || global_pins == 0) {
        return false;
    }
    reader = SharedManifestReader{};
    static_cast<void>(pg_atomic_fetch_sub_u32(
        &manifest.reader_pins, 1));
    static_cast<void>(pg_atomic_fetch_sub_u64(
        &g_shared->reader_reference_count, 1));
    return true;
}

struct ReapedBackendReferences {
    std::uint32_t readers = 0;
    std::uint32_t waiters = 0;
};

[[nodiscard]] ReapedBackendReferences reap_backend_references(
        const StorageBackendIdentity& owner) noexcept {
    ReapedBackendReferences reaped{};
    if (g_shared == nullptr || g_storage_lock == nullptr
        || !owner.valid()) {
        return reaped;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (SharedManifestEntry& manifest : g_shared->manifests) {
        if (manifest.state == ManifestState::Empty) {
            continue;
        }
        for (SharedManifestReader& reader : manifest.readers) {
            if (reader.active
                && reader.owner == owner
                && deactivate_reader_locked(manifest, reader)) {
                ++reaped.readers;
            }
        }
        for (SharedManifestWaiter& waiter : manifest.waiters) {
            if (waiter.active
                && waiter.owner == owner
                && deactivate_waiter_locked(manifest, waiter)) {
                ++reaped.waiters;
            }
        }
    }
    LWLockRelease(g_storage_lock);
    return reaped;
}

[[nodiscard]] std::uint64_t next_nonzero_sweep_sequence() noexcept {
    std::uint64_t sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_sweep_sequence, 1);
    if (sequence == 0) {
        sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_sweep_sequence, 1);
    }
    return sequence == 0 ? 1 : sequence;
}

/** Claim an unowned/dead sweep token through copy, probe, exact recheck. */
[[nodiscard]] bool acquire_sweep_ownership(
        SweepOwnerToken& acquired) noexcept {
    const StorageBackendIdentity current = current_backend_identity();
    if (!current.valid()) {
        return false;
    }
    for (std::uint32_t attempt = 0; attempt < 2; ++attempt) {
        LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
        const SweepOwnerToken observed = g_shared->sweep_owner;
        if (!observed.valid()) {
            acquired = SweepOwnerToken{
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
                acquired = SweepOwnerToken{
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

/** Release only the exact sweep generation this backend acquired. */
void release_sweep_ownership(const SweepOwnerToken& acquired) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    if (acquired.valid() && g_shared->sweep_owner == acquired) {
        g_shared->sweep_owner = {};
    }
    LWLockRelease(g_storage_lock);
}

void initialize_shared_state(
        StorageSharedState& shared,
        std::size_t metadata_bytes,
        std::uint64_t boot_nonce,
        const StorageConfig& config) noexcept {
    std::memset(&shared, 0, sizeof(shared));
    shared.magic = kStorageMagic;
    shared.abi_version = kStorageAbiVersion;
    shared.metadata_bytes = metadata_bytes;
    shared.dsa_initial_bytes = MAXALIGN(config.dsa_initial_bytes);
    shared.hot_ceiling_bytes =
        hot_payload_ceiling(config, g_registry_request);
    shared.file_ceiling_bytes = config.file_ceiling_bytes;
    shared.maximum_block_bytes = config.maximum_block_bytes;
    shared.sweep_interval_ms = config.sweep_interval_ms;
    shared.reclaim_batch = config.reclaim_batch;
    shared.soft_limit_percent = config.soft_limit_percent;
    pg_atomic_init_u32(
        &shared.worker_lifecycle,
        static_cast<std::uint32_t>(g_pending_worker_lifecycle));
    pg_atomic_init_u32(
        &shared.correctness_readiness,
        static_cast<std::uint32_t>(g_pending_correctness_readiness));
    pg_atomic_init_u32(
        &shared.last_failure,
        static_cast<std::uint32_t>(StorageError::None));
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
    for (SharedDsaReservationEntry& entry :
         shared.shared_dsa_reservations) {
        entry = SharedDsaReservationEntry{};
    }
    for (BlockLedgerEntry& entry : shared.block_ledger) {
        entry.state = BlockLedgerState::Empty;
        entry.manifest_slot = kInvalidSlot;
    }
    for (SharedManifestEntry& entry : shared.manifests) {
        entry.state = ManifestState::Empty;
        pg_atomic_init_u64(&entry.last_access_at_us, 0);
        pg_atomic_init_u32(&entry.reader_pins, 0);
        pg_atomic_init_u32(&entry.waiter_references, 0);
        entry.ledger_slots.fill(kInvalidSlot);
    }
}

[[nodiscard]] bool validate_shared_state(
        const StorageSharedState& shared,
        std::size_t metadata_bytes,
        std::uint64_t boot_nonce,
        const StorageConfig& config) noexcept {
    if (shared.magic != kStorageMagic ||
        shared.abi_version != kStorageAbiVersion ||
        shared.metadata_bytes != metadata_bytes ||
        metadata_bytes != sizeof(StorageSharedState) ||
        shared.dsa_initial_bytes != MAXALIGN(config.dsa_initial_bytes) ||
        shared.hot_ceiling_bytes !=
            hot_payload_ceiling(config, g_registry_request) ||
        shared.file_ceiling_bytes != config.file_ceiling_bytes ||
        shared.maximum_block_bytes != config.maximum_block_bytes ||
        shared.sweep_interval_ms != config.sweep_interval_ms ||
        shared.reclaim_batch != config.reclaim_batch ||
        shared.soft_limit_percent != config.soft_limit_percent ||
        shared.reserved_config != 0 ||
        pg_atomic_read_u64(
            const_cast<pg_atomic_uint64*>(&shared.boot_nonce)) != boot_nonce ||
        pg_atomic_read_u32(
            const_cast<pg_atomic_uint32*>(&shared.worker_lifecycle)) >
            static_cast<std::uint32_t>(WorkerLifecycle::Failed) ||
        !known_readiness(static_cast<CacheCorrectnessReadiness>(
            pg_atomic_read_u32(const_cast<pg_atomic_uint32*>(
                &shared.correctness_readiness))))) {
        return false;
    }
    const auto accounting = shared_registry::shared_registry_accounting();
    if (!accounting ||
        pg_atomic_read_u64(accounting.reserved_dsa_bytes) <
            g_registry_request.persistent.fixed_dsa_reserved_bytes) {
        return false;
    }
    for (const SharedDsaReservationEntry& entry :
         shared.shared_dsa_reservations) {
        if (static_cast<std::uint32_t>(entry.state) >
                static_cast<std::uint32_t>(
                    SharedDsaReservationState::Releasing) ||
            entry.reserved != 0 ||
            (entry.state == SharedDsaReservationState::Empty
                ? entry.sequence != 0 || entry.accounted_bytes != 0 ||
                    entry.owner.valid()
                : entry.sequence == 0 || entry.accounted_bytes == 0 ||
                    !entry.owner.valid())) {
            return false;
        }
    }
    for (const BlockLedgerEntry& entry : shared.block_ledger) {
        if (static_cast<std::uint32_t>(entry.state) >
                static_cast<std::uint32_t>(
                    BlockLedgerState::ReleaseCommitted) ||
            (entry.manifest_slot != kInvalidSlot &&
             entry.manifest_slot >= kManifestCapacity)) {
            return false;
        }
    }
    for (const SharedManifestEntry& entry : shared.manifests) {
        if (static_cast<std::uint32_t>(entry.state) >
                static_cast<std::uint32_t>(ManifestState::Retiring) ||
            entry.block_count > kBlocksPerManifest ||
            atomic_read(entry.reader_pins) > kReaderPinsPerManifest ||
            atomic_read(entry.waiter_references) > kWaitersPerManifest) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool initialize_storage_extension(
        void* context,
        std::span<std::byte> metadata,
        std::uint64_t boot_nonce) noexcept {
    if (context == nullptr || metadata.size() != sizeof(StorageSharedState)) {
        return false;
    }
    const auto& config = *static_cast<const StorageConfig*>(context);
    const auto accounting = shared_registry::shared_registry_accounting();
    if (!accounting || g_registry_request.persistent.fixed_dsa_reserved_bytes == 0) {
        return false;
    }
    PgAtomicReservationCounter fixed_counter{
        .value = accounting.reserved_dsa_bytes,
    };
    const PgSystemMemoryBudget fixed_budget{
        .payload_ceiling = total_cache_ceiling(config),
        .shared_reserved = accounting.reserved_dsa_bytes,
    };
    BasicCacheAdmission fixed_admission{fixed_counter, fixed_budget};
    auto fixed_permit = fixed_admission.try_reserve(
        g_registry_request.persistent.fixed_dsa_reserved_bytes);
    if (!fixed_permit.has_value()) {
        return false;
    }
    auto* shared = reinterpret_cast<StorageSharedState*>(metadata.data());
    initialize_shared_state(*shared, metadata.size(), boot_nonce, config);
    const StorageError directory_error = ensure_store_directory(boot_nonce);
    if (directory_error != StorageError::None) {
        pg_atomic_write_u32(
            &shared->last_failure,
            static_cast<std::uint32_t>(directory_error));
        return false;
    }
    const StorageError cleanup_error = clean_orphaned_store_files();
    const StorageError recreate_error = cleanup_error == StorageError::None
        ? ensure_store_directory(boot_nonce)
        : cleanup_error;
    if (recreate_error != StorageError::None) {
        pg_atomic_write_u32(
            &shared->last_failure,
            static_cast<std::uint32_t>(recreate_error));
        return false;
    }
    const bool valid = validate_shared_state(
        *shared, metadata.size(), boot_nonce, config);
    if (valid) {
        static_cast<void>(fixed_permit->disown());
    }
    return valid;
}

[[nodiscard]] bool validate_storage_extension(
        void* context,
        std::span<std::byte> metadata,
        std::uint64_t boot_nonce) noexcept {
    if (context == nullptr || metadata.size() != sizeof(StorageSharedState)) {
        return false;
    }
    if (g_storage_lock != nullptr) {
        LWLockAcquire(g_storage_lock, LW_SHARED);
    }
    const bool valid = validate_shared_state(
        *reinterpret_cast<const StorageSharedState*>(metadata.data()),
        metadata.size(), boot_nonce,
        *static_cast<const StorageConfig*>(context));
    if (g_storage_lock != nullptr) {
        LWLockRelease(g_storage_lock);
    }
    return valid;
}

[[nodiscard]] shared_registry::SharedRegistryCallbacksV1
storage_registry_callbacks() noexcept {
    return shared_registry::SharedRegistryCallbacksV1{
        .context = &g_requested_config,
        .initialize_extension = initialize_storage_extension,
        .validate_extension = validate_storage_extension,
    };
}

void storage_backend_exit(int, Datum) {
    if (g_shared != nullptr && g_storage_lock != nullptr) {
        static_cast<void>(
            reap_backend_references(current_backend_identity()));
    }
    g_shared = nullptr;
    g_registry_root = nullptr;
    g_accounting = {};
    g_dsa = nullptr;
}

[[nodiscard]] bool same_id(
        const CacheEntryId& left,
        const CacheEntryId& right) noexcept {
    return left == right;
}

[[nodiscard]] bool valid_stored_reference(
        const BlockReferenceV1& reference,
        std::uint64_t payload_bytes) noexcept {
    const bool known_tier = reference.tier == BlockBackingTierV1::Dsa
        || reference.tier == BlockBackingTierV1::File;
    const bool valid_handle = reference.tier == BlockBackingTierV1::File
        ? reference.handle_or_file_id != 0
        : DsaPointerIsValid(
            static_cast<dsa_pointer>(reference.handle_or_file_id));
    const auto end = checked_add(reference.offset, reference.length);
    return known_tier
        && valid_handle
        && reference.flags == 0
        && reference.reserved == 0
        && reference.reserved2 == 0
        && end.has_value()
        && (reference.tier == BlockBackingTierV1::File ||
            reference.offset == 0)
        && reference.length != 0
        && reference.length == payload_bytes;
}

[[nodiscard]] bool handle_matches(
        const SharedManifestEntry& entry,
        const ManifestHandle& handle) noexcept {
    return handle.slot < kManifestCapacity
        && entry.state != ManifestState::Empty
        && entry.slot_sequence == handle.slot_sequence
        && same_id(entry.id, handle.id);
}

[[nodiscard]] bool waiter_token_matches(
        const SharedManifestWaiter& waiter,
        const ManifestWaiterToken& token) noexcept {
    return token.waiter_slot < kWaitersPerManifest
        && waiter.owner == token.owner
        && waiter.owner_sequence == token.owner_sequence;
}

[[nodiscard]] bool reader_pin_matches(
        const SharedManifestReader& reader,
        const ManifestPin& pin) noexcept {
    return pin.reader_slot < kReaderPinsPerManifest
        && reader.active
        && reader.owner == pin.owner
        && reader.owner_sequence == pin.owner_sequence;
}

[[nodiscard]] StorageError allocate_ledger_slot(
        std::uint32_t& slot,
        std::uint64_t& sequence,
        const StorageBackendIdentity& owner,
        std::uint64_t bytes) noexcept {
    if (!owner.valid()) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (std::uint32_t index = 0; index < kBlockLedgerCapacity; ++index) {
        BlockLedgerEntry& entry = g_shared->block_ledger[index];
        if (entry.state != BlockLedgerState::Empty) {
            continue;
        }
        sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_block_sequence, 1);
        if (sequence == 0) {
            sequence = pg_atomic_fetch_add_u64(
                &g_shared->next_block_sequence, 1);
        }
        entry = BlockLedgerEntry{};
        entry.state = BlockLedgerState::Reserved;
        entry.owner = owner;
        entry.manifest_slot = kInvalidSlot;
        entry.sequence = sequence;
        entry.created_at_us = current_time_us();
        entry.database_oid = static_cast<std::uint32_t>(MyDatabaseId);
        entry.payload_bytes = bytes;
        slot = index;
        LWLockRelease(g_storage_lock);
        return StorageError::None;
    }
    LWLockRelease(g_storage_lock);
    return StorageError::LedgerFull;
}

void clear_unreserved_ledger_slot(
        std::uint32_t slot,
        std::uint64_t sequence,
        const StorageBackendIdentity& owner) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence == sequence
        && entry.owner == owner
        && entry.state == BlockLedgerState::Reserved
        && !entry.reservation_accounted
        && entry.reference.length == 0) {
        entry = BlockLedgerEntry{};
        entry.manifest_slot = kInvalidSlot;
    }
    LWLockRelease(g_storage_lock);
}

[[nodiscard]] StorageError mark_ledger_reservation(
        std::uint32_t slot,
        std::uint64_t sequence,
        const StorageBackendIdentity& owner,
        BlockBackingTierV1 tier,
        std::uint64_t accounted_bytes) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence
        || entry.owner != owner
        || entry.state != BlockLedgerState::Reserved
        || entry.reservation_accounted
        || accounted_bytes == 0
        || entry.reference.length != 0) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    entry.reservation_tier = tier;
    entry.reservation_accounted = true;
    entry.reserved_bytes = accounted_bytes;
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

[[nodiscard]] StorageError cancel_ledger_reservation(
        std::uint32_t slot,
        std::uint64_t sequence,
        const StorageBackendIdentity& owner,
        BlockBackingTierV1 tier) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence
        || entry.owner != owner
        || entry.state != BlockLedgerState::Reserved
        || !entry.reservation_accounted
        || entry.reservation_tier != tier
        || entry.reference.length != 0) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    entry.reservation_accounted = false;
    entry.reserved_bytes = 0;
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

[[nodiscard]] StorageError publish_stored_ledger_block(
        std::uint32_t slot,
        std::uint64_t sequence,
        const StorageBackendIdentity& owner,
        const BlockReferenceV1& reference) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence
        || entry.owner != owner
        || entry.state != BlockLedgerState::Reserved) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    if (entry.reference.length != 0
        && std::memcmp(
            &entry.reference, &reference, sizeof(reference)) != 0) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    entry.reference = reference;
    if (reference.tier == BlockBackingTierV1::Dsa &&
        !entry.live_accounted) {
        const auto desired = checked_add(
            static_cast<std::uint64_t>(
                pg_atomic_read_u64(&g_registry_root->hot_live_bytes)),
            entry.reserved_bytes);
        if (!desired.has_value()) {
            LWLockRelease(g_storage_lock);
            return StorageError::HotTierFull;
        }
        static_cast<void>(pg_atomic_fetch_add_u64(
            &g_registry_root->hot_live_bytes, entry.reserved_bytes));
        entry.live_accounted = true;
    }
    pg_write_barrier();
    entry.state = BlockLedgerState::Stored;
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

[[nodiscard]] StorageError record_reserved_block(
        std::uint32_t slot,
        std::uint64_t sequence,
        const StorageBackendIdentity& owner,
        const BlockReferenceV1& reference) noexcept {
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence
        || entry.owner != owner
        || entry.state != BlockLedgerState::Reserved
        || entry.reference.length != 0) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    entry.reference = reference;
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

[[nodiscard]] StorageError write_all_at(
        int descriptor,
        std::span<const std::byte> bytes,
        std::uint64_t offset = 0) noexcept {
    std::size_t written = 0;
    while (written < bytes.size()) {
        CHECK_FOR_INTERRUPTS();
        if (offset > static_cast<std::uint64_t>(
                (std::numeric_limits<off_t>::max)()) - written) {
            return StorageError::InvalidArgument;
        }
        const ssize_t count = pg_pwrite(
            descriptor,
            bytes.data() + written,
            bytes.size() - written,
            static_cast<off_t>(offset + written));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            return StorageError::WriteFailed;
        }
        written += static_cast<std::size_t>(count);
    }
    return StorageError::None;
}

[[nodiscard]] StorageError store_file_block(
        std::span<const std::byte> bytes,
        std::uint32_t database_oid,
        const StorageBackendIdentity& owner,
        std::uint64_t file_id,
        BlockReferenceV1& reference) noexcept {
    if (ensure_database_directory(database_oid) != StorageError::None) {
        return StorageError::UnsafePath;
    }
    char temporary[MAXPGPATH]{};
    if (!build_staging_file_path(
            database_oid, owner, file_id, temporary, sizeof(temporary))) {
        return StorageError::PathTooLong;
    }
    int flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    volatile int descriptor = -1;
    StorageError error = StorageError::None;
    PG_TRY();
    {
        descriptor = OpenTransientFilePerm(
            temporary, flags, S_IRUSR | S_IWUSR);
        if (descriptor < 0) {
            error = StorageError::OpenFailed;
        }
        struct stat status{};
        if (error == StorageError::None &&
            (::fstat(descriptor, &status) != 0 ||
             !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
             status.st_nlink != 1 ||
             (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
                (S_IRUSR | S_IWUSR))) {
            error = StorageError::UnsafePath;
        }
        if (error == StorageError::None) {
            error = write_all_at(descriptor, bytes);
        }
        if (descriptor >= 0 && CloseTransientFile(descriptor) != 0 &&
            error == StorageError::None) {
            error = StorageError::SyncFailed;
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
    if (error != StorageError::None) {
        static_cast<void>(durable_unlink(temporary, LOG));
        return error;
    }
    reference.tier = BlockBackingTierV1::File;
    reference.handle_or_file_id = file_id;
    reference.offset = 0;
    reference.length = bytes.size();
    reference.crc32c = crc32c(bytes);
    return StorageError::None;
}

[[nodiscard]] StorageError read_all_at(
        int descriptor,
        std::uint64_t offset,
        std::span<std::byte> destination) noexcept {
    std::size_t read = 0;
    while (read < destination.size()) {
        CHECK_FOR_INTERRUPTS();
        if (offset > static_cast<std::uint64_t>((std::numeric_limits<off_t>::max)())
            - read) {
            return StorageError::InvalidArgument;
        }
        const ssize_t count = pg_pread(
            descriptor,
            destination.data() + read,
            destination.size() - read,
            static_cast<off_t>(offset + read));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return StorageError::ReadFailed;
        }
        if (count == 0) {
            return StorageError::ShortRead;
        }
        read += static_cast<std::size_t>(count);
    }
    return StorageError::None;
}

[[nodiscard]] StorageError read_file_block(
        std::uint32_t database_oid,
        const CacheEntryId& id,
        const BlockReferenceV1& reference,
        std::span<std::byte> destination) noexcept {
    char temporary[MAXPGPATH]{};
    char final[MAXPGPATH]{};
    if (!build_generation_file_paths(
            database_oid, id, current_backend_identity(),
            temporary, sizeof(temporary), final, sizeof(final))) {
        return StorageError::PathTooLong;
    }
    int flags = O_RDONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    flags |= O_NOFOLLOW;
#endif
    volatile int descriptor = OpenTransientFile(final, flags);
    if (descriptor < 0) {
        return errno == ENOENT ? StorageError::NotFound : StorageError::OpenFailed;
    }
    StorageError error = StorageError::None;
    PG_TRY();
    {
        struct stat status{};
        const std::uint64_t end = reference.offset <=
                (std::numeric_limits<std::uint64_t>::max)() - reference.length
            ? reference.offset + reference.length
            : (std::numeric_limits<std::uint64_t>::max)();
        if (::fstat(descriptor, &status) != 0
            || !S_ISREG(status.st_mode)
            || status.st_uid != ::geteuid()
            || status.st_nlink != 1
            || (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
                (S_IRUSR | S_IWUSR)
            || status.st_size < 0
            || static_cast<std::uint64_t>(status.st_size) < end) {
            error = StorageError::UnsafePath;
        } else {
            error = read_all_at(descriptor, reference.offset, destination);
        }
        if (CloseTransientFile(descriptor) != 0 &&
            error == StorageError::None) {
            error = StorageError::ReadFailed;
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

[[nodiscard]] StorageError release_block_slot(
        std::uint32_t slot,
        std::uint64_t sequence,
        JanitorSweepResult* result = nullptr) noexcept {
    if (slot >= kBlockLedgerCapacity) {
        return StorageError::InvalidArgument;
    }
    BlockReferenceV1 reference{};
    std::uint64_t reserved_bytes = 0;
    StorageBackendIdentity owner{};
    std::uint32_t database_oid = 0;
    CacheDigest256 file_digest{};
    std::uint64_t file_generation = 0;
    bool file_finalized = false;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& entry = g_shared->block_ledger[slot];
    if (entry.sequence != sequence
        || (entry.state != BlockLedgerState::Stored
            && entry.state != BlockLedgerState::Retiring
            && !(entry.state == BlockLedgerState::Reserved
                && entry.reference.length != 0))) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    if (!entry.reservation_accounted
        || entry.reservation_tier != entry.reference.tier
        || !valid_stored_reference(
            entry.reference, entry.payload_bytes)) {
        LWLockRelease(g_storage_lock);
        return StorageError::InvalidState;
    }
    /*
     * Commit the one-way release before touching the physical tier.  If this
     * process exits inside dsa_free()/unlink(), a later janitor leaves the
     * tombstone fail-closed instead of risking a second DSA free.
     */
    entry.state = BlockLedgerState::ReleaseCommitted;
    reference = entry.reference;
    reserved_bytes = entry.reserved_bytes;
    owner = entry.owner;
    database_oid = entry.database_oid;
    file_digest = entry.file_digest;
    file_generation = entry.file_generation;
    file_finalized = entry.file_finalized;
    const BlockBackingTierV1 reservation_tier = entry.reservation_tier;
    const bool reservation_accounted = entry.reservation_accounted;
    const bool live_accounted = entry.live_accounted;
    LWLockRelease(g_storage_lock);

    /* ReleaseCommitted prevents a second janitor while physical I/O runs. */
    if (reference.tier == BlockBackingTierV1::Dsa
        && DsaPointerIsValid(static_cast<dsa_pointer>(reference.handle_or_file_id))) {
        dsa_free(g_dsa, static_cast<dsa_pointer>(reference.handle_or_file_id));
        if (live_accounted) {
            static_cast<void>(release_reservation(
                g_registry_root->hot_live_bytes, reserved_bytes));
        }
        if (reservation_accounted
            && reservation_tier == BlockBackingTierV1::Dsa) {
            static_cast<void>(release_reservation(
                hot_reserved_bytes(), reserved_bytes));
        }
        if (result != nullptr && reservation_accounted
            && reservation_tier == BlockBackingTierV1::Dsa) {
            result->reclaimed_hot_bytes += reserved_bytes;
        }
    } else if (reference.tier == BlockBackingTierV1::File) {
        char temporary[MAXPGPATH]{};
        char final[MAXPGPATH]{};
        bool released = false;
        if (file_finalized) {
            const CacheEntryId id{
                .digest = file_digest,
                .boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce),
                .generation = file_generation,
                .claim_sequence = 1,
            };
            released = build_generation_file_paths(
                database_oid, id, owner, temporary, sizeof(temporary),
                final, sizeof(final));
            if (released) {
                static_cast<void>(unlink_cache_file(temporary));
                released = unlink_cache_file(final);
            }
        } else {
            released = build_staging_file_path(
                database_oid, owner, reference.handle_or_file_id,
                temporary, sizeof(temporary));
            if (released) {
                released = unlink_cache_file(temporary);
            }
        }
        if (!released) {
            LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
            BlockLedgerEntry& failed = g_shared->block_ledger[slot];
            if (failed.sequence == sequence
                && failed.state == BlockLedgerState::ReleaseCommitted) {
                failed.state = BlockLedgerState::Retiring;
            }
            LWLockRelease(g_storage_lock);
            return StorageError::WriteFailed;
        }
        if (reservation_accounted
            && reservation_tier == BlockBackingTierV1::File) {
            static_cast<void>(release_reservation(
                file_reserved_bytes(), reserved_bytes));
        }
        if (result != nullptr && reservation_accounted
            && reservation_tier == BlockBackingTierV1::File) {
            result->reclaimed_file_bytes += reserved_bytes;
        }
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    BlockLedgerEntry& completed = g_shared->block_ledger[slot];
    if (completed.sequence != sequence
        || completed.state != BlockLedgerState::ReleaseCommitted) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    completed = BlockLedgerEntry{};
    completed.manifest_slot = kInvalidSlot;
    LWLockRelease(g_storage_lock);
    if (result != nullptr) {
        ++result->reclaimed_blocks;
    }
    return StorageError::None;
}

struct ManifestFileFinalization {
    StorageError error = StorageError::None;
    std::array<BlockReferenceV1, kBlocksPerManifest> references{};
};

/** Reject every forged/stale/duplicate capability before filesystem access. */
[[nodiscard]] StorageError validate_manifest_tokens_before_io(
        const ManifestDescriptor& descriptor,
        std::span<const StoredBlockToken> blocks,
        const StorageBackendIdentity& owner) noexcept {
    std::uint64_t stored_bytes = 0;
    LWLockAcquire(g_storage_lock, LW_SHARED);
    for (const SharedManifestEntry& manifest : g_shared->manifests) {
        if (manifest.state != ManifestState::Empty &&
            (same_id(manifest.id, descriptor.id) ||
             (manifest.id.digest == descriptor.id.digest &&
              manifest.id.generation == descriptor.id.generation))) {
            LWLockRelease(g_storage_lock);
            return StorageError::DuplicateGeneration;
        }
    }
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const StoredBlockToken& token = blocks[index];
        if (!token || token.ledger_slot >= kBlockLedgerCapacity ||
            token.owner != owner) {
            LWLockRelease(g_storage_lock);
            return StorageError::InvalidArgument;
        }
        for (std::size_t previous = 0; previous < index; ++previous) {
            if (blocks[previous].ledger_slot == token.ledger_slot) {
                LWLockRelease(g_storage_lock);
                return StorageError::InvalidArgument;
            }
        }
        const BlockLedgerEntry& entry =
            g_shared->block_ledger[token.ledger_slot];
        const auto next_stored = checked_add(
            stored_bytes, entry.payload_bytes);
        const bool file_identity_clear = std::all_of(
            entry.file_digest.bytes.begin(), entry.file_digest.bytes.end(),
            [](std::uint8_t value) noexcept { return value == 0; });
        if (entry.sequence != token.sequence || entry.owner != owner ||
            entry.state != BlockLedgerState::Stored ||
            entry.manifest_slot != kInvalidSlot ||
            entry.database_oid != static_cast<std::uint32_t>(MyDatabaseId) ||
            !entry.reservation_accounted || entry.file_finalized ||
            entry.file_generation != 0 || !file_identity_clear ||
            entry.reserved_bytes == 0 ||
            entry.live_accounted !=
                (entry.reservation_tier == BlockBackingTierV1::Dsa) ||
            entry.reservation_tier != entry.reference.tier ||
            !valid_stored_reference(entry.reference, entry.payload_bytes) ||
            std::memcmp(
                &entry.reference, &token.reference,
                sizeof(BlockReferenceV1)) != 0 ||
            !next_stored.has_value() ||
            *next_stored > kMaximumColumnStoreBytes) {
            LWLockRelease(g_storage_lock);
            return StorageError::StaleToken;
        }
        stored_bytes = *next_stored;
    }
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

/** Consolidate staged file blocks in manifest order before final publication. */
[[nodiscard]] ManifestFileFinalization finalize_manifest_file(
        const ManifestDescriptor& descriptor,
        std::span<const StoredBlockToken> blocks,
        const StorageBackendIdentity& owner) {
    ManifestFileFinalization result{};
    bool has_file = false;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        result.references[index] = blocks[index].reference;
        has_file = has_file ||
            blocks[index].reference.tier == BlockBackingTierV1::File;
    }
    if (!has_file) {
        return result;
    }
    const std::uint32_t database_oid = static_cast<std::uint32_t>(MyDatabaseId);
    if (database_oid == 0) {
        result.error = StorageError::UnsafePath;
        return result;
    }
    char directory[MAXPGPATH]{};
    char temporary[MAXPGPATH]{};
    char final[MAXPGPATH]{};
    if (!build_database_directory(
            database_oid, directory, sizeof(directory)) ||
        !build_generation_file_paths(
            database_oid, descriptor.id, owner,
            temporary, sizeof(temporary), final, sizeof(final))) {
        result.error = StorageError::PathTooLong;
        return result;
    }
    struct stat existing{};
    if (::lstat(final, &existing) == 0 || errno != ENOENT) {
        result.error = StorageError::DuplicateGeneration;
        return result;
    }
    if (ensure_database_directory(database_oid) != StorageError::None) {
        result.error = StorageError::UnsafePath;
        return result;
    }

    int destination_flags = O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC;
    int source_flags = O_RDONLY | O_CLOEXEC;
#if defined(O_NOFOLLOW)
    destination_flags |= O_NOFOLLOW;
    source_flags |= O_NOFOLLOW;
#endif
    volatile int destination = -1;
    volatile int source = -1;
    std::uint64_t file_offset = 0;
    PG_TRY();
    {
        destination = OpenTransientFilePerm(
            temporary, destination_flags, S_IRUSR | S_IWUSR);
        if (destination < 0) {
            result.error = StorageError::OpenFailed;
        }
        std::array<std::byte, 64U << 10U> buffer{};
        for (std::size_t index = 0;
             result.error == StorageError::None && index < blocks.size();
             ++index) {
            const StoredBlockToken& token = blocks[index];
            if (token.reference.tier != BlockBackingTierV1::File) {
                continue;
            }
            char staging[MAXPGPATH]{};
            if (!build_staging_file_path(
                    database_oid, owner, token.reference.handle_or_file_id,
                    staging, sizeof(staging))) {
                result.error = StorageError::PathTooLong;
                break;
            }
            source = OpenTransientFile(staging, source_flags);
            if (source < 0) {
                result.error = StorageError::OpenFailed;
                break;
            }
            struct stat status{};
            if (::fstat(source, &status) != 0 ||
                !S_ISREG(status.st_mode) || status.st_uid != ::geteuid() ||
                status.st_nlink != 1 ||
                (status.st_mode & (S_IRWXU | S_IRWXG | S_IRWXO)) !=
                    (S_IRUSR | S_IWUSR) ||
                status.st_size < 0 ||
                static_cast<std::uint64_t>(status.st_size) !=
                    token.reference.length) {
                result.error = StorageError::UnsafePath;
            }
            pg_crc32c checksum = 0;
            INIT_CRC32C(checksum);
            std::uint64_t copied = 0;
            while (result.error == StorageError::None &&
                   copied < token.reference.length) {
                CHECK_FOR_INTERRUPTS();
                const std::size_t chunk = static_cast<std::size_t>(std::min(
                    static_cast<std::uint64_t>(buffer.size()),
                    token.reference.length - copied));
                const ssize_t count = pg_pread(
                    source, buffer.data(), chunk, static_cast<off_t>(copied));
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    result.error = count == 0
                        ? StorageError::ShortRead
                        : StorageError::ReadFailed;
                    break;
                }
                const auto bytes_read = static_cast<std::size_t>(count);
                COMP_CRC32C(checksum, buffer.data(), bytes_read);
                result.error = write_all_at(
                    destination,
                    std::span<const std::byte>{buffer.data(), bytes_read},
                    file_offset + copied);
                copied += bytes_read;
            }
            FIN_CRC32C(checksum);
            if (result.error == StorageError::None &&
                static_cast<std::uint32_t>(checksum) !=
                    token.reference.crc32c) {
                result.error = StorageError::ChecksumMismatch;
            }
            if (CloseTransientFile(source) != 0 &&
                result.error == StorageError::None) {
                result.error = StorageError::ReadFailed;
            }
            source = -1;
            if (result.error != StorageError::None) {
                break;
            }
            BlockReferenceV1 reference = token.reference;
            reference.handle_or_file_id = descriptor.id.generation;
            reference.offset = file_offset;
            result.references[index] = reference;
            const auto next = checked_add(file_offset, reference.length);
            if (!next.has_value() || *next > kMaximumColumnStoreBytes) {
                result.error = StorageError::BlockTooLarge;
                break;
            }
            file_offset = *next;
        }
        if (destination >= 0 && CloseTransientFile(destination) != 0 &&
            result.error == StorageError::None) {
            result.error = StorageError::SyncFailed;
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
        static_cast<void>(unlink_cache_file(temporary));
        for (const StoredBlockToken& token : blocks) {
            static_cast<void>(release_block_slot(
                token.ledger_slot, token.sequence));
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (result.error != StorageError::None) {
        static_cast<void>(unlink_cache_file(temporary));
        return result;
    }
    if (fsync_fname_ext(temporary, false, false, WARNING) != 0) {
        static_cast<void>(unlink_cache_file(temporary));
        result.error = StorageError::SyncFailed;
        return result;
    }
    if (::link(temporary, final) != 0) {
        result.error = errno == EEXIST
            ? StorageError::DuplicateGeneration
            : StorageError::RenameFailed;
        static_cast<void>(unlink_cache_file(temporary));
        return result;
    }
    if (fsync_fname_ext(directory, true, false, WARNING) != 0) {
        static_cast<void>(unlink_cache_file(final, 2));
        static_cast<void>(unlink_cache_file(temporary));
        result.error = StorageError::SyncFailed;
        return result;
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    bool unchanged = true;
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const StoredBlockToken& token = blocks[index];
        const BlockLedgerEntry& entry =
            g_shared->block_ledger[token.ledger_slot];
        unchanged = unchanged && entry.sequence == token.sequence &&
            entry.owner == owner && entry.state == BlockLedgerState::Stored &&
            entry.manifest_slot == kInvalidSlot &&
            std::memcmp(
                &entry.reference, &token.reference,
                sizeof(BlockReferenceV1)) == 0;
    }
    if (unchanged) {
        for (std::size_t index = 0; index < blocks.size(); ++index) {
            const StoredBlockToken& token = blocks[index];
            if (token.reference.tier != BlockBackingTierV1::File) {
                continue;
            }
            BlockLedgerEntry& entry =
                g_shared->block_ledger[token.ledger_slot];
            entry.reference = result.references[index];
            entry.file_digest = descriptor.id.digest;
            entry.file_generation = descriptor.id.generation;
            entry.file_finalized = true;
        }
    }
    LWLockRelease(g_storage_lock);
    if (!unchanged) {
        static_cast<void>(unlink_cache_file(final, 2));
        static_cast<void>(unlink_cache_file(temporary));
        result.error = StorageError::StaleToken;
        return result;
    }

    for (const StoredBlockToken& token : blocks) {
        if (token.reference.tier != BlockBackingTierV1::File) {
            continue;
        }
        char staging[MAXPGPATH]{};
        if (build_staging_file_path(
                database_oid, owner, token.reference.handle_or_file_id,
                staging, sizeof(staging))) {
            static_cast<void>(unlink_cache_file(staging));
        }
    }
    static_cast<void>(unlink_cache_file(temporary, 2));
    return result;
}

[[nodiscard]] bool manifest_reclaimable(
        const SharedManifestEntry& entry) noexcept {
    return entry.state != ManifestState::Empty
        && atomic_read(entry.reader_pins) == 0
        && atomic_read(entry.waiter_references) == 0;
}

[[nodiscard]] StorageError reclaim_manifest_slot(
        std::uint32_t slot,
        std::uint64_t sequence,
        JanitorSweepResult& result) noexcept {
    if (slot >= kManifestCapacity) {
        return StorageError::InvalidArgument;
    }
    std::array<std::uint32_t, kBlocksPerManifest> ledger_slots{};
    std::array<std::uint64_t, kBlocksPerManifest> ledger_sequences{};
    std::uint32_t block_count = 0;
    bool decrement_manifest_count = false;

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& manifest = g_shared->manifests[slot];
    if (manifest.slot_sequence != sequence
        || !manifest_reclaimable(manifest)) {
        LWLockRelease(g_storage_lock);
        return StorageError::Pinned;
    }
    decrement_manifest_count = manifest.manifest_counted;
    manifest.manifest_counted = false;
    manifest.state = ManifestState::Retiring;
    block_count = manifest.block_count;
    for (std::uint32_t index = 0; index < block_count; ++index) {
        ledger_slots[index] = manifest.ledger_slots[index];
        if (ledger_slots[index] < kBlockLedgerCapacity) {
            BlockLedgerEntry& block = g_shared->block_ledger[ledger_slots[index]];
            ledger_sequences[index] = block.sequence;
            if (block.state == BlockLedgerState::Referenced
                && block.manifest_slot == slot) {
                block.state = BlockLedgerState::Stored;
                block.manifest_slot = kInvalidSlot;
            }
        }
    }
    LWLockRelease(g_storage_lock);

    for (std::uint32_t index = 0; index < block_count; ++index) {
        if (ledger_slots[index] < kBlockLedgerCapacity) {
            static_cast<void>(release_block_slot(
                ledger_slots[index], ledger_sequences[index], &result));
        }
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& current = g_shared->manifests[slot];
    if (current.slot_sequence == sequence
        && current.state == ManifestState::Retiring
        && pg_atomic_read_u32(&current.reader_pins) == 0
        && pg_atomic_read_u32(&current.waiter_references) == 0) {
        current = SharedManifestEntry{};
        current.state = ManifestState::Empty;
        current.ledger_slots.fill(kInvalidSlot);
        pg_atomic_init_u64(&current.last_access_at_us, 0);
        pg_atomic_init_u32(&current.reader_pins, 0);
        pg_atomic_init_u32(&current.waiter_references, 0);
        if (decrement_manifest_count) {
            static_cast<void>(pg_atomic_fetch_sub_u64(
                &g_shared->manifest_count, 1));
        }
    }
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

[[nodiscard]] StorageError claim_manifest_reader_locked(
        SharedManifestEntry& manifest,
        const ManifestHandle& handle,
        const StorageBackendIdentity& owner,
        ManifestPin& pin,
        std::int64_t accessed_at_us) noexcept {
    if (!owner.valid()) {
        return StorageError::InvalidArgument;
    }
    std::uint32_t reader_slot = kInvalidSlot;
    for (std::uint32_t index = 0; index < kReaderPinsPerManifest; ++index) {
        if (!manifest.readers[index].active) {
            reader_slot = index;
            break;
        }
    }
    if (reader_slot == kInvalidSlot
        || pg_atomic_read_u32(&manifest.reader_pins)
            == (std::numeric_limits<std::uint32_t>::max)()) {
        return StorageError::ReaderFull;
    }
    std::uint64_t owner_sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_reader_sequence, 1);
    if (owner_sequence == 0) {
        owner_sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_reader_sequence, 1);
    }
    if (owner_sequence == 0) {
        owner_sequence = 1;
    }
    SharedManifestReader& reader = manifest.readers[reader_slot];
    reader = SharedManifestReader{
        .active = true,
        .owner = owner,
        .owner_sequence = owner_sequence,
    };
    static_cast<void>(pg_atomic_fetch_add_u32(&manifest.reader_pins, 1));
    static_cast<void>(pg_atomic_fetch_add_u64(
        &g_shared->reader_reference_count, 1));
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
    return StorageError::None;
}

inline constexpr std::size_t kOwnerLivenessScanBudget = 1024;
inline constexpr std::size_t kManifestReferenceSlotCount =
    kManifestCapacity * (kReaderPinsPerManifest + kWaitersPerManifest);

enum class ManifestReferenceKind : std::uint8_t {
    Reader = 1,
    Waiter = 2,
};

/** Exact immutable observation copied before a backend-status probe. */
struct ManifestReferenceObservation {
    ManifestHandle handle{};
    ManifestReferenceKind kind = ManifestReferenceKind::Reader;
    std::uint32_t reference_slot = kInvalidSlot;
    StorageBackendIdentity owner{};
    std::uint64_t owner_sequence = 0;
    bool requires_liveness = true;
};

/** Exact block-ledger observation copied before a backend-status probe. */
struct BlockOwnerObservation {
    std::uint32_t ledger_slot = kInvalidSlot;
    BlockLedgerEntry entry{};
    StorageBackendIdentity owner{};
    bool requires_liveness = true;
};

template <typename Observation>
void probe_observation_owners(
        std::span<const Observation> observations,
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
        const StorageBackendIdentity& owner = observations[index].owner;
        if (!owner.valid()) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (observations[prior].requires_liveness
                && observations[prior].owner == owner) {
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

/** Copy a rotating bounded slice of reader/waiter ABA tokens under lock. */
[[nodiscard]] std::size_t collect_manifest_reference_observations(
        std::span<ManifestReferenceObservation> observations) noexcept {
    if (observations.empty()) {
        return 0;
    }
    std::size_t count = 0;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t cursor = g_shared->reference_scan_cursor
        % static_cast<std::uint32_t>(kManifestReferenceSlotCount);
    const std::size_t scan_count = observations.size()
            < kManifestReferenceSlotCount
        ? observations.size()
        : kManifestReferenceSlotCount;
    for (std::size_t scanned = 0; scanned < scan_count; ++scanned) {
        const std::uint32_t position = cursor;
        cursor = (cursor + 1U)
            % static_cast<std::uint32_t>(kManifestReferenceSlotCount);
        const std::uint32_t per_manifest =
            kReaderPinsPerManifest + kWaitersPerManifest;
        const std::uint32_t manifest_slot = position / per_manifest;
        const std::uint32_t local_slot = position % per_manifest;
        const SharedManifestEntry& manifest =
            g_shared->manifests[manifest_slot];
        if (manifest.state == ManifestState::Empty) {
            continue;
        }
        const ManifestHandle handle{
            .slot = manifest_slot,
            .slot_sequence = manifest.slot_sequence,
            .id = manifest.id,
        };
        if (local_slot < kReaderPinsPerManifest) {
            const SharedManifestReader& reader =
                manifest.readers[local_slot];
            if (reader.active) {
                observations[count++] = ManifestReferenceObservation{
                    .handle = handle,
                    .kind = ManifestReferenceKind::Reader,
                    .reference_slot = local_slot,
                    .owner = reader.owner,
                    .owner_sequence = reader.owner_sequence,
                };
            }
        } else {
            const std::uint32_t waiter_slot =
                local_slot - kReaderPinsPerManifest;
            const SharedManifestWaiter& waiter =
                manifest.waiters[waiter_slot];
            if (waiter.active) {
                observations[count++] = ManifestReferenceObservation{
                    .handle = handle,
                    .kind = ManifestReferenceKind::Waiter,
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

/** Reap dead references only when their complete observation is unchanged. */
void reap_dead_manifest_references(JanitorSweepResult& result) noexcept {
    std::array<ManifestReferenceObservation,
        kOwnerLivenessScanBudget> observations{};
    const std::size_t count = collect_manifest_reference_observations(
        std::span<ManifestReferenceObservation>{observations});
    std::array<bool, kOwnerLivenessScanBudget> alive{};
    probe_observation_owners(
        std::span<const ManifestReferenceObservation>{
            observations.data(), count},
        std::span<bool>{alive.data(), count});

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (std::size_t index = 0;
         index < count
            && result.reaped_readers + result.reaped_waiters
                < g_shared->reclaim_batch;
         ++index) {
        if (alive[index]) {
            continue;
        }
        const ManifestReferenceObservation& observation =
            observations[index];
        SharedManifestEntry& manifest =
            g_shared->manifests[observation.handle.slot];
        if (!handle_matches(manifest, observation.handle)) {
            continue;
        }
        if (observation.kind == ManifestReferenceKind::Reader
            && observation.reference_slot < kReaderPinsPerManifest) {
            SharedManifestReader& reader =
                manifest.readers[observation.reference_slot];
            if (reader.active
                && reader.owner == observation.owner
                && reader.owner_sequence == observation.owner_sequence
                && deactivate_reader_locked(manifest, reader)) {
                ++result.reaped_readers;
            }
        } else if (observation.kind == ManifestReferenceKind::Waiter
            && observation.reference_slot < kWaitersPerManifest) {
            SharedManifestWaiter& waiter =
                manifest.waiters[observation.reference_slot];
            if (waiter.active
                && waiter.owner == observation.owner
                && waiter.owner_sequence == observation.owner_sequence
                && deactivate_waiter_locked(manifest, waiter)) {
                ++result.reaped_waiters;
            }
        }
    }
    LWLockRelease(g_storage_lock);
}

/** Compare all block fields that may change while its owner is active. */
[[nodiscard]] bool block_observation_matches(
        const BlockLedgerEntry& current,
        const BlockLedgerEntry& observed) noexcept {
    return current.state == observed.state
        && current.reservation_tier == observed.reservation_tier
        && current.reservation_accounted == observed.reservation_accounted
        && current.live_accounted == observed.live_accounted
        && current.file_finalized == observed.file_finalized
        && current.owner == observed.owner
        && current.manifest_slot == observed.manifest_slot
        && current.database_oid == observed.database_oid
        && current.sequence == observed.sequence
        && current.created_at_us == observed.created_at_us
        && current.payload_bytes == observed.payload_bytes
        && current.reserved_bytes == observed.reserved_bytes
        && current.file_digest == observed.file_digest
        && current.file_generation == observed.file_generation
        && std::memcmp(
            &current.reference,
            &observed.reference,
            sizeof(BlockReferenceV1)) == 0;
}

/** Copy a rotating bounded slice of unowned block states under lock. */
[[nodiscard]] std::size_t collect_block_owner_observations(
        std::span<BlockOwnerObservation> observations) noexcept {
    if (observations.empty()) {
        return 0;
    }
    std::size_t count = 0;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t cursor = g_shared->block_scan_cursor
        % static_cast<std::uint32_t>(kBlockLedgerCapacity);
    const std::size_t scan_count = observations.size()
            < kBlockLedgerCapacity
        ? observations.size()
        : kBlockLedgerCapacity;
    for (std::size_t scanned = 0; scanned < scan_count; ++scanned) {
        const std::uint32_t slot = cursor;
        cursor = (cursor + 1U)
            % static_cast<std::uint32_t>(kBlockLedgerCapacity);
        const BlockLedgerEntry& entry = g_shared->block_ledger[slot];
        const bool explicit_retirement =
            entry.state == BlockLedgerState::Retiring;
        const bool producer_owned = entry.state == BlockLedgerState::Reserved
            || (entry.state == BlockLedgerState::Stored
                && entry.manifest_slot == kInvalidSlot);
        if (!explicit_retirement && !producer_owned) {
            continue;
        }
        observations[count++] = BlockOwnerObservation{
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
 * Reclaim abandoned producer blocks through copy/probe/recheck.  The unknown
 * DSA-allocation window deliberately retains its reservation counter exactly
 * as before; ReleaseCommitted remains the one-way physical-free tombstone.
 */
void reclaim_abandoned_blocks(JanitorSweepResult& result) noexcept {
    std::array<BlockOwnerObservation,
        kOwnerLivenessScanBudget> observations{};
    const std::size_t count = collect_block_owner_observations(
        std::span<BlockOwnerObservation>{observations});
    std::array<bool, kOwnerLivenessScanBudget> alive{};
    probe_observation_owners(
        std::span<const BlockOwnerObservation>{observations.data(), count},
        std::span<bool>{alive.data(), count});

    for (std::size_t index = 0;
         index < count
            && result.abandoned_blocks < g_shared->reclaim_batch;
         ++index) {
        if (alive[index]) {
            continue;
        }
        const BlockOwnerObservation& observation = observations[index];
        bool release_physical_block = false;
        LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
        BlockLedgerEntry& current =
            g_shared->block_ledger[observation.ledger_slot];
        if (!block_observation_matches(current, observation.entry)) {
            LWLockRelease(g_storage_lock);
            continue;
        }
        if (current.state == BlockLedgerState::Reserved
            && current.reference.length == 0) {
            if (current.reservation_accounted) {
                if (current.reservation_tier == BlockBackingTierV1::File &&
                    release_reservation(
                        file_reserved_bytes(), current.reserved_bytes)) {
                    result.reclaimed_file_bytes += current.reserved_bytes;
                } else if (
                    current.reservation_tier == BlockBackingTierV1::Dsa &&
                    !current.live_accounted && release_reservation(
                        hot_reserved_bytes(), current.reserved_bytes)) {
                    result.reclaimed_hot_bytes += current.reserved_bytes;
                }
            }
            current = BlockLedgerEntry{};
            current.manifest_slot = kInvalidSlot;
            ++result.abandoned_blocks;
            LWLockRelease(g_storage_lock);
            continue;
        }
        current.state = BlockLedgerState::Retiring;
        release_physical_block = true;
        LWLockRelease(g_storage_lock);
        if (release_physical_block
            && release_block_slot(
                observation.ledger_slot,
                observation.entry.sequence,
                &result) == StorageError::None) {
            ++result.abandoned_blocks;
        }
    }
}

} // namespace

std::size_t storage_shared_memory_size(const StorageConfig& config) noexcept {
    return config_valid(config) ? sizeof(StorageSharedState) : 0;
}

void request_storage_resources(const StorageConfig& config) {
    g_requested_config = config;
    g_registry_request = registry_preload_request(config);
    if (!g_registry_request) {
        return;
    }
    shared_registry::request_shared_registry_resources(g_registry_request);
    RequestNamedLWLockTranche(kStorageLockTranche, 1);
}

bool startup_storage(
        std::uint64_t boot_nonce,
        const StorageConfig& config) {
    if (!config_valid(config) || !same_config(config, g_requested_config) ||
        !g_registry_request) {
        return false;
    }
    g_storage_lock = &GetNamedLWLockTranche(kStorageLockTranche)[0].lock;
    return g_storage_lock != nullptr &&
        shared_registry::startup_shared_registry(
            g_registry_request, boot_nonce);
}

bool attach_storage() {
    if (storage_ready()) {
        return true;
    }
    if (!g_registry_request || !config_valid(g_requested_config)) {
        return false;
    }
    if (g_storage_lock == nullptr) {
        g_storage_lock = &GetNamedLWLockTranche(kStorageLockTranche)[0].lock;
    }
    const auto attached = shared_registry::attach_shared_registry(
        g_registry_request, storage_registry_callbacks(), true);
    if (!attached || attached.attachment.extension_metadata.size() !=
            sizeof(StorageSharedState)) {
        return false;
    }
    g_dsa = attached.attachment.area;
    g_registry_root = attached.attachment.root;
    g_accounting = shared_registry::shared_registry_accounting();
    g_shared = reinterpret_cast<StorageSharedState*>(
        attached.attachment.extension_metadata.data());
    if (!g_accounting || !validate_shared_state(
            *g_shared, attached.attachment.extension_metadata.size(),
            g_registry_root->boot_nonce, g_requested_config)) {
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
    return g_shared != nullptr
        && g_dsa != nullptr
        && g_registry_root != nullptr
        && shared_registry::shared_registry_ready()
        && g_shared->magic == kStorageMagic
        && g_shared->abi_version == kStorageAbiVersion;
}

SharedDsaReserveResult reserve_shared_dsa_allocation(
        std::uint64_t payload_bytes) noexcept {
    SharedDsaReserveResult result{};
    if (payload_bytes == 0 || !attach_storage()) {
        result.error = payload_bytes == 0
            ? StorageError::InvalidArgument
            : StorageError::NotPreloaded;
        return result;
    }
    const auto accounted = conservative_dsa_reservation_bytes(payload_bytes);
    if (!accounted.has_value()) {
        result.error = StorageError::HotTierFull;
        return result;
    }
    const StorageBackendIdentity owner = current_backend_identity();
    if (!owner.valid()) {
        result.error = StorageError::InvalidState;
        return result;
    }
    PgAtomicReservationCounter counter{.value = &hot_reserved_bytes()};
    const PgSystemMemoryBudget budget{
        .payload_ceiling = g_shared->hot_ceiling_bytes,
        .shared_reserved = &hot_reserved_bytes(),
    };
    BasicCacheAdmission admission{counter, budget};
    auto permit = admission.try_reserve(*accounted);
    if (!permit.has_value()) {
        result.error = permit.error() == AdmissionError::SystemPressure
            ? StorageError::MemoryPressure
            : StorageError::HotTierFull;
        return result;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t slot = kInvalidSlot;
    for (std::uint32_t index = 0;
         index < kSharedDsaReservationCapacity; ++index) {
        if (g_shared->shared_dsa_reservations[index].state ==
                SharedDsaReservationState::Empty) {
            slot = index;
            break;
        }
    }
    if (slot == kInvalidSlot) {
        LWLockRelease(g_storage_lock);
        result.error = StorageError::LedgerFull;
        return result;
    }
    std::uint64_t sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_shared_reservation_sequence, 1);
    if (sequence == 0) {
        sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_shared_reservation_sequence, 1);
    }
    if (sequence == 0) {
        LWLockRelease(g_storage_lock);
        result.error = StorageError::InvalidState;
        return result;
    }
    g_shared->shared_dsa_reservations[slot] = SharedDsaReservationEntry{
        .state = SharedDsaReservationState::Pending,
        .owner = owner,
        .sequence = sequence,
        .accounted_bytes = permit->bytes(),
    };
    LWLockRelease(g_storage_lock);
    static_cast<void>(permit->disown());
    result.reservation = SharedDsaReservation{
        .slot = slot,
        .sequence = sequence,
    };
    return result;
}

bool publish_shared_dsa_allocation(
        SharedDsaReservation* reservation) noexcept {
    const StorageBackendIdentity owner = current_backend_identity();
    if (!storage_ready() || reservation == nullptr || !*reservation ||
        !owner.valid()) {
        return false;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedDsaReservationEntry& entry =
        g_shared->shared_dsa_reservations[reservation->slot];
    if (entry.state != SharedDsaReservationState::Pending ||
        entry.sequence != reservation->sequence || entry.owner != owner ||
        entry.accounted_bytes == 0) {
        LWLockRelease(g_storage_lock);
        return false;
    }
    const auto desired = checked_add(
        static_cast<std::uint64_t>(
            pg_atomic_read_u64(&g_registry_root->hot_live_bytes)),
        entry.accounted_bytes);
    if (!desired.has_value()) {
        LWLockRelease(g_storage_lock);
        return false;
    }
    static_cast<void>(pg_atomic_fetch_add_u64(
        &g_registry_root->hot_live_bytes, entry.accounted_bytes));
    entry.state = SharedDsaReservationState::Live;
    LWLockRelease(g_storage_lock);
    return true;
}

bool release_shared_dsa_allocation(
        SharedDsaReservation* reservation) noexcept {
    const StorageBackendIdentity owner = current_backend_identity();
    if (!storage_ready() || reservation == nullptr || !*reservation ||
        !owner.valid()) {
        return false;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedDsaReservationEntry& entry =
        g_shared->shared_dsa_reservations[reservation->slot];
    if ((entry.state != SharedDsaReservationState::Pending &&
         entry.state != SharedDsaReservationState::Live) ||
        entry.sequence != reservation->sequence || entry.owner != owner ||
        entry.accounted_bytes == 0) {
        LWLockRelease(g_storage_lock);
        return false;
    }
    const bool live = entry.state == SharedDsaReservationState::Live;
    const std::uint64_t accounted_bytes = entry.accounted_bytes;
    entry.state = SharedDsaReservationState::Releasing;
    LWLockRelease(g_storage_lock);
    bool released = true;
    if (live) {
        released = release_reservation(
            g_registry_root->hot_live_bytes,
            accounted_bytes);
    }
    released = release_reservation(
        hot_reserved_bytes(), accounted_bytes) && released;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedDsaReservationEntry& completed =
        g_shared->shared_dsa_reservations[reservation->slot];
    if (completed.state == SharedDsaReservationState::Releasing &&
        completed.sequence == reservation->sequence &&
        completed.owner == owner) {
        completed = SharedDsaReservationEntry{};
    } else {
        released = false;
    }
    LWLockRelease(g_storage_lock);
    *reservation = SharedDsaReservation{};
    return released;
}

StoreBlockResult store_block(std::span<const std::byte> bytes) {
    StoreBlockResult result{};
    if (!attach_storage()) {
        result.error = StorageError::NotPreloaded;
        return result;
    }
    if (bytes.empty()) {
        result.error = StorageError::InvalidArgument;
        return result;
    }
    if (bytes.size() > g_shared->maximum_block_bytes) {
        result.error = StorageError::BlockTooLarge;
        return result;
    }
    const StorageBackendIdentity owner = current_backend_identity();
    if (!owner.valid()) {
        result.error = StorageError::InvalidState;
        return result;
    }

    std::uint32_t ledger_slot = kInvalidSlot;
    std::uint64_t sequence = 0;
    result.error = allocate_ledger_slot(
        ledger_slot,
        sequence,
        owner,
        static_cast<std::uint64_t>(bytes.size()));
    if (result.error != StorageError::None) {
        remember_failure(result.error);
        return result;
    }

    BlockReferenceV1 reference{};
    bool hot_reserved = false;
    const bool force_file_tier =
#if defined(BIOSEQ_CACHE_TESTING)
        pg_atomic_read_u32(&g_shared->force_file_tier) != 0;
#else
        false;
#endif
    if (!force_file_tier) {
        PgAtomicReservationCounter hot_counter{
            .value = &hot_reserved_bytes(),
        };
        const PgSystemMemoryBudget memory_budget{
            .payload_ceiling = g_shared->hot_ceiling_bytes,
            .shared_reserved = &hot_reserved_bytes(),
        };
        BasicCacheAdmission hot_admission{hot_counter, memory_budget};
        const auto hot_accounted = conservative_dsa_reservation_bytes(
            static_cast<std::uint64_t>(bytes.size()));
        auto hot_permit = hot_admission.try_reserve(
            hot_accounted.value_or(
                (std::numeric_limits<std::uint64_t>::max)()));
        hot_reserved = hot_accounted.has_value() && hot_permit.has_value();
        if (hot_reserved) {
            result.error = mark_ledger_reservation(
                ledger_slot, sequence, owner, BlockBackingTierV1::Dsa,
                hot_permit->bytes());
            if (result.error != StorageError::None) {
                clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
                remember_failure(result.error);
                return result;
            }
            const dsa_pointer allocation = dsa_allocate_extended(
                g_dsa,
                bytes.size(),
                DSA_ALLOC_NO_OOM);
            if (DsaPointerIsValid(allocation)) {
                reference.tier = BlockBackingTierV1::Dsa;
                reference.handle_or_file_id =
                    static_cast<std::uint64_t>(allocation);
                reference.offset = 0;
                reference.length = bytes.size();
                reference.crc32c = crc32c(bytes);
                result.error = record_reserved_block(
                    ledger_slot, sequence, owner, reference);
                if (result.error != StorageError::None) {
                    dsa_free(g_dsa, allocation);
                    static_cast<void>(cancel_ledger_reservation(
                        ledger_slot,
                        sequence,
                        owner,
                        BlockBackingTierV1::Dsa));
                    clear_unreserved_ledger_slot(
                        ledger_slot, sequence, owner);
                    remember_failure(result.error);
                    return result;
                }
                static_cast<void>(hot_permit->disown());
                void* address = dsa_get_address(g_dsa, allocation);
                std::memcpy(address, bytes.data(), bytes.size());
                pg_write_barrier();
                result.error = publish_stored_ledger_block(
                    ledger_slot, sequence, owner, reference);
                if (result.error == StorageError::None) {
                    result.token = StoredBlockToken{
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
            static_cast<void>(pg_atomic_fetch_add_u64(
                &g_registry_root->hot_allocation_failures, 1));
            static_cast<void>(cancel_ledger_reservation(
                ledger_slot,
                sequence,
                owner,
                BlockBackingTierV1::Dsa));
        }
    }

    std::uint64_t filesystem_bytes = 0;
    std::uint64_t filesystem_available = 0;
    result.error = filesystem_capacity(
        filesystem_bytes, filesystem_available);
    if (result.error != StorageError::None) {
        clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
        remember_failure(result.error);
        return result;
    }
    const std::uint64_t policy_ceiling = FileAdmissionPolicy::ceiling_bytes(
        filesystem_bytes);
    const std::uint64_t ceiling = g_shared->file_ceiling_bytes < policy_ceiling
        ? g_shared->file_ceiling_bytes
        : policy_ceiling;
    const std::uint64_t observed = pg_atomic_read_u64(
        &file_reserved_bytes());
    const std::uint64_t free_floor = FileAdmissionPolicy::free_floor_bytes(
        filesystem_bytes);
    const std::uint64_t free_headroom = filesystem_available > free_floor
        ? filesystem_available - free_floor
        : 0;
    const std::uint64_t concurrency_ceiling = observed <=
            (std::numeric_limits<std::uint64_t>::max)() - free_headroom
        ? observed + free_headroom
        : (std::numeric_limits<std::uint64_t>::max)();
    const std::uint64_t reservation_ceiling = ceiling < concurrency_ceiling
        ? ceiling
        : concurrency_ceiling;
    PgAtomicReservationCounter file_counter{
        .value = &file_reserved_bytes(),
    };
    const FixedAdmissionBudget file_budget{
        .ceiling = reservation_ceiling,
    };
    BasicCacheAdmission file_admission{file_counter, file_budget};
    auto file_permit = file_admission.try_reserve(bytes.size());
    if (!FileAdmissionPolicy::can_reserve(
            observed,
            bytes.size(),
            filesystem_bytes,
            filesystem_available)
        || !file_permit.has_value()) {
        clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
        result.error = StorageError::FileTierFull;
        remember_failure(result.error);
        return result;
    }
    result.error = mark_ledger_reservation(
        ledger_slot, sequence, owner, BlockBackingTierV1::File,
        file_permit->bytes());
    if (result.error != StorageError::None) {
        clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
        remember_failure(result.error);
        return result;
    }
    std::uint64_t file_id = pg_atomic_fetch_add_u64(
        &g_shared->next_file_id, 1);
    if (file_id == 0) {
        file_id = pg_atomic_fetch_add_u64(&g_shared->next_file_id, 1);
    }
    reference.tier = BlockBackingTierV1::File;
    reference.handle_or_file_id = file_id;
    reference.offset = 0;
    reference.length = bytes.size();
    reference.crc32c = crc32c(bytes);
    result.error = record_reserved_block(
        ledger_slot, sequence, owner, reference);
    if (result.error != StorageError::None) {
        static_cast<void>(cancel_ledger_reservation(
            ledger_slot,
            sequence,
            owner,
            BlockBackingTierV1::File));
        clear_unreserved_ledger_slot(ledger_slot, sequence, owner);
        remember_failure(result.error);
        return result;
    }
    static_cast<void>(file_permit->disown());
    result.error = store_file_block(
        bytes,
        static_cast<std::uint32_t>(MyDatabaseId),
        owner,
        file_id,
        reference);
    if (result.error != StorageError::None) {
        static_cast<void>(release_block_slot(ledger_slot, sequence));
        remember_failure(result.error);
        return result;
    }
    result.error = publish_stored_ledger_block(
        ledger_slot, sequence, owner, reference);
    if (result.error != StorageError::None) {
        static_cast<void>(release_block_slot(ledger_slot, sequence));
        remember_failure(result.error);
        return result;
    }
    result.token = StoredBlockToken{
        .ledger_slot = ledger_slot,
        .sequence = sequence,
        .owner = owner,
        .reference = reference,
    };
    return result;
}

StorageError discard_unpublished_block(const StoredBlockToken& token) {
    const StorageBackendIdentity current = current_backend_identity();
    if (!storage_ready() || !token || token.owner != current) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    const BlockLedgerEntry& entry = g_shared->block_ledger[token.ledger_slot];
    const bool valid = entry.sequence == token.sequence
        && entry.owner == token.owner
        && entry.state == BlockLedgerState::Stored
        && entry.manifest_slot == kInvalidSlot;
    LWLockRelease(g_storage_lock);
    return valid
        ? release_block_slot(token.ledger_slot, token.sequence)
        : StorageError::StaleToken;
}

PublishManifestResult publish_manifest(
        const ManifestDescriptor& descriptor,
        std::span<const StoredBlockToken> blocks) {
    PublishManifestResult result{};
    if (!storage_ready()) {
        result.error = StorageError::NotPreloaded;
        return result;
    }
    if (descriptor.id.boot_nonce == 0
        || descriptor.id.boot_nonce
            != pg_atomic_read_u64(&g_shared->boot_nonce)
        || descriptor.id.generation == 0
        || descriptor.id.claim_sequence == 0
        || (descriptor.kind != CacheEntryKind::ExactFinalResult
            && descriptor.kind != CacheEntryKind::ReusableCoverage)
        || descriptor.reserved != 0
        || descriptor.reserved2 != 0
        || descriptor.reserved_waiter_count != 0
        || descriptor.flags != 0
        || descriptor.logical_bytes > kMaximumColumnStoreBytes
        || descriptor.row_group_count > kMaximumColumnStoreRowGroups
        || blocks.empty()
        || blocks.size() > kBlocksPerManifest) {
        result.error = StorageError::InvalidArgument;
        return result;
    }
    const StorageBackendIdentity current = current_backend_identity();
    if (!current.valid()) {
        result.error = StorageError::InvalidState;
        return result;
    }

    result.error = validate_manifest_tokens_before_io(
        descriptor, blocks, current);
    if (result.error != StorageError::None) {
        return result;
    }

    const ManifestFileFinalization finalized = finalize_manifest_file(
        descriptor, blocks, current);
    if (finalized.error != StorageError::None) {
        result.error = finalized.error;
        return result;
    }

    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    std::uint32_t free_slot = kInvalidSlot;
    for (std::uint32_t index = 0; index < kManifestCapacity; ++index) {
        SharedManifestEntry& manifest = g_shared->manifests[index];
        if (manifest.state == ManifestState::Empty && free_slot == kInvalidSlot) {
            free_slot = index;
        } else if (manifest.state != ManifestState::Empty &&
            (same_id(manifest.id, descriptor.id) ||
             (manifest.id.digest == descriptor.id.digest &&
              manifest.id.generation == descriptor.id.generation))) {
            LWLockRelease(g_storage_lock);
            result.error = StorageError::DuplicateGeneration;
            return result;
        }
    }
    if (free_slot == kInvalidSlot) {
        LWLockRelease(g_storage_lock);
        result.error = StorageError::ManifestFull;
        return result;
    }
    std::uint64_t stored_bytes = 0;
    for (std::size_t token_index = 0;
         token_index < blocks.size();
         ++token_index) {
        const StoredBlockToken& token = blocks[token_index];
        if (!token || token.ledger_slot >= kBlockLedgerCapacity
            || token.owner != current) {
            LWLockRelease(g_storage_lock);
            result.error = StorageError::InvalidArgument;
            return result;
        }
        const BlockLedgerEntry& block = g_shared->block_ledger[token.ledger_slot];
        const BlockReferenceV1& expected_reference =
            finalized.references[token_index];
        if (block.sequence != token.sequence
            || block.owner != token.owner
            || block.state != BlockLedgerState::Stored
            || block.manifest_slot != kInvalidSlot
            || !block.reservation_accounted
            || block.reservation_tier != block.reference.tier
            || !valid_stored_reference(
                block.reference, block.payload_bytes)
            || std::memcmp(
                &block.reference,
                &expected_reference,
                sizeof(BlockReferenceV1)) != 0
            || stored_bytes > (std::numeric_limits<std::uint64_t>::max)()
                - block.reference.length) {
            LWLockRelease(g_storage_lock);
            result.error = StorageError::StaleToken;
            return result;
        }
        for (std::size_t previous = 0;
             previous < token_index;
             ++previous) {
            if (blocks[previous].ledger_slot == token.ledger_slot) {
                LWLockRelease(g_storage_lock);
                result.error = StorageError::InvalidArgument;
                return result;
            }
        }
        stored_bytes += block.reference.length;
    }

    SharedManifestEntry& manifest = g_shared->manifests[free_slot];
    manifest = SharedManifestEntry{};
    manifest.state = ManifestState::Empty;
    manifest.kind = descriptor.kind;
    manifest.block_count = static_cast<std::uint16_t>(blocks.size());
    manifest.flags = descriptor.flags;
    manifest.slot_sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_manifest_sequence, 1);
    if (manifest.slot_sequence == 0) {
        manifest.slot_sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_manifest_sequence, 1);
    }
    manifest.id = descriptor.id;
    manifest.logical_bytes = descriptor.logical_bytes;
    manifest.stored_bytes = stored_bytes;
    manifest.row_count = descriptor.row_count;
    manifest.row_group_count = descriptor.row_group_count;
    manifest.database_oid = static_cast<std::uint32_t>(MyDatabaseId);
    manifest.created_at_us = current_time_us(descriptor.created_at_us);
    manifest.expires_at_us = descriptor.expires_at_us;
    if (manifest.expires_at_us != 0
        && manifest.expires_at_us < manifest.created_at_us) {
        manifest = SharedManifestEntry{};
        manifest.state = ManifestState::Empty;
        manifest.ledger_slots.fill(kInvalidSlot);
        pg_atomic_init_u64(&manifest.last_access_at_us, 0);
        pg_atomic_init_u32(&manifest.reader_pins, 0);
        pg_atomic_init_u32(&manifest.waiter_references, 0);
        LWLockRelease(g_storage_lock);
        result.error = StorageError::InvalidArgument;
        return result;
    }
    manifest.ledger_slots.fill(kInvalidSlot);
    pg_atomic_init_u64(&manifest.last_access_at_us, manifest.created_at_us);
    pg_atomic_init_u32(&manifest.reader_pins, 0);
    pg_atomic_init_u32(&manifest.waiter_references, 0);
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const StoredBlockToken& token = blocks[index];
        manifest.ledger_slots[index] = token.ledger_slot;
        manifest.blocks[index] = finalized.references[index];
    }
    /* Complete is published last; janitor repairs a crashed Publishing slot. */
    manifest.state = ManifestState::Publishing;
    pg_write_barrier();
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        const StoredBlockToken& token = blocks[index];
        BlockLedgerEntry& block = g_shared->block_ledger[token.ledger_slot];
        block.state = BlockLedgerState::Referenced;
        block.manifest_slot = free_slot;
    }
    pg_write_barrier();
    static_cast<void>(pg_atomic_fetch_add_u64(&g_shared->manifest_count, 1));
    manifest.manifest_counted = true;
    manifest.state = ManifestState::Complete;
    pg_write_barrier();
    result.handle = ManifestHandle{
        .slot = free_slot,
        .slot_sequence = manifest.slot_sequence,
        .id = manifest.id,
    };
    LWLockRelease(g_storage_lock);
    return result;
}

PinManifestResult pin_manifest(
        const CacheEntryId& id,
        std::int64_t now_us) {
    PinManifestResult result{};
    if (!storage_ready()) {
        result.error = StorageError::NotPreloaded;
        return result;
    }
    const StorageBackendIdentity owner = current_backend_identity();
    if (!owner.valid()) {
        result.error = StorageError::InvalidState;
        return result;
    }
    const std::int64_t now = current_time_us(now_us);
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    for (std::uint32_t index = 0; index < kManifestCapacity; ++index) {
        SharedManifestEntry& manifest = g_shared->manifests[index];
        if ((manifest.state != ManifestState::Complete
                && manifest.state != ManifestState::Retiring)
            || !same_id(manifest.id, id)) {
            continue;
        }
        if (manifest.expires_at_us != 0 && now >= manifest.expires_at_us) {
            LWLockRelease(g_storage_lock);
            result.error = StorageError::NotFound;
            return result;
        }
        const ManifestHandle handle{
            .slot = index,
            .slot_sequence = manifest.slot_sequence,
            .id = manifest.id,
        };
        result.error = claim_manifest_reader_locked(
            manifest, handle, owner, result.pin, now);
        LWLockRelease(g_storage_lock);
        return result;
    }
    LWLockRelease(g_storage_lock);
    result.error = StorageError::NotFound;
    return result;
}

RegisterManifestWaiterResult register_manifest_waiter(
        const ManifestHandle& handle,
        const StorageBackendIdentity& owner,
        std::int64_t registered_at_us) {
    RegisterManifestWaiterResult result{};
    if (!storage_ready() || !handle || !owner.valid()) {
        result.error = StorageError::InvalidArgument;
        return result;
    }
    const std::int64_t now = current_time_us();
    const bool preexisting = registered_at_us != 0;
    const std::int64_t registered_at = preexisting
        ? registered_at_us
        : now;
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& manifest = g_shared->manifests[handle.slot];
    const bool expired_registration = manifest.expires_at_us != 0
        && (preexisting
            ? registered_at > manifest.expires_at_us
            : registered_at >= manifest.expires_at_us);
    if (!handle_matches(manifest, handle)
        || manifest.state != ManifestState::Complete
        || registered_at < manifest.created_at_us
        || registered_at > now
        || expired_registration) {
        LWLockRelease(g_storage_lock);
        result.error = StorageError::NotFound;
        return result;
    }
    std::uint32_t waiter_slot = kInvalidSlot;
    for (std::uint32_t index = 0; index < kWaitersPerManifest; ++index) {
        if (!manifest.waiters[index].active) {
            waiter_slot = index;
            break;
        }
    }
    if (waiter_slot == kInvalidSlot) {
        LWLockRelease(g_storage_lock);
        result.error = StorageError::WaiterFull;
        return result;
    }
    std::uint64_t owner_sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_waiter_sequence, 1);
    if (owner_sequence == 0) {
        owner_sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_waiter_sequence, 1);
    }
    if (owner_sequence == 0) {
        owner_sequence = 1;
    }
    SharedManifestWaiter& waiter = manifest.waiters[waiter_slot];
    waiter = SharedManifestWaiter{
        .active = true,
        .owner = owner,
        .owner_sequence = owner_sequence,
    };
    static_cast<void>(pg_atomic_fetch_add_u32(
        &manifest.waiter_references, 1));
    static_cast<void>(pg_atomic_fetch_add_u64(
        &g_shared->waiter_reference_count, 1));
    result.token = ManifestWaiterToken{
        .handle = handle,
        .waiter_slot = waiter_slot,
        .owner = owner,
        .owner_sequence = owner_sequence,
    };
    LWLockRelease(g_storage_lock);
    return result;
}

RegisterManifestWaiterResult current_manifest_waiter(
        const CacheEntryId& id) {
    RegisterManifestWaiterResult result{};
    const StorageBackendIdentity current = current_backend_identity();
    if (!storage_ready() || id.boot_nonce == 0 || id.generation == 0
        || id.claim_sequence == 0 || !current.valid()
        || id.boot_nonce != pg_atomic_read_u64(&g_shared->boot_nonce)) {
        result.error = StorageError::InvalidArgument;
        return result;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    bool found = false;
    for (std::uint32_t manifest_slot = 0;
         manifest_slot < kManifestCapacity; ++manifest_slot) {
        const SharedManifestEntry& manifest =
            g_shared->manifests[manifest_slot];
        if (manifest.state != ManifestState::Complete
            || !same_id(manifest.id, id)) {
            continue;
        }
        const ManifestHandle handle{
            .slot = manifest_slot,
            .slot_sequence = manifest.slot_sequence,
            .id = manifest.id,
        };
        for (std::uint32_t waiter_slot = 0;
             waiter_slot < kWaitersPerManifest; ++waiter_slot) {
            const SharedManifestWaiter& waiter =
                manifest.waiters[waiter_slot];
            if (!waiter.active || waiter.owner != current) {
                continue;
            }
            if (waiter.owner_sequence == 0) {
                LWLockRelease(g_storage_lock);
                result.error = StorageError::InvalidState;
                return result;
            }
            if (found) {
                LWLockRelease(g_storage_lock);
                result.token = {};
                result.error = StorageError::InvalidState;
                return result;
            }
            result.token = ManifestWaiterToken{
                .handle = handle,
                .waiter_slot = waiter_slot,
                .owner = waiter.owner,
                .owner_sequence = waiter.owner_sequence,
            };
            found = true;
        }
    }
    LWLockRelease(g_storage_lock);
    result.error = found ? StorageError::None : StorageError::NotFound;
    return result;
}

PinManifestResult convert_waiter_to_pin(
        const ManifestWaiterToken& token) {
    PinManifestResult result{};
    const StorageBackendIdentity current = current_backend_identity();
    if (!storage_ready() || !token
        || token.owner != current) {
        result.error = StorageError::InvalidArgument;
        return result;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& manifest =
        g_shared->manifests[token.handle.slot];
    SharedManifestWaiter& waiter = manifest.waiters[token.waiter_slot];
    if (!handle_matches(manifest, token.handle)
        || manifest.state != ManifestState::Complete
        || !waiter_token_matches(waiter, token)
        || !waiter.active) {
        LWLockRelease(g_storage_lock);
        result.error = StorageError::NotFound;
        return result;
    }
    const std::int64_t accessed_at = current_time_us();
    result.error = claim_manifest_reader_locked(
        manifest, token.handle, token.owner, result.pin, accessed_at);
    if (result.error != StorageError::None) {
        LWLockRelease(g_storage_lock);
        return result;
    }
    static_cast<void>(deactivate_waiter_locked(manifest, waiter));
    LWLockRelease(g_storage_lock);
    return result;
}

StorageError unpin_manifest(const ManifestPin& pin) noexcept {
    const StorageBackendIdentity current = current_backend_identity();
    if (!storage_ready() || !pin || pin.handle.slot >= kManifestCapacity
        || pin.owner != current) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& manifest = g_shared->manifests[pin.handle.slot];
    SharedManifestReader& reader = manifest.readers[pin.reader_slot];
    if (!handle_matches(manifest, pin.handle)
        || !reader_pin_matches(reader, pin)) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    static_cast<void>(deactivate_reader_locked(manifest, reader));
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

StorageError unregister_manifest_waiter(
        const ManifestWaiterToken& token) noexcept {
    if (!storage_ready() || !token) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& manifest =
        g_shared->manifests[token.handle.slot];
    SharedManifestWaiter& waiter = manifest.waiters[token.waiter_slot];
    if (!handle_matches(manifest, token.handle)
        || !waiter_token_matches(waiter, token)) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    if (waiter.active) {
        static_cast<void>(deactivate_waiter_locked(manifest, waiter));
    }
    LWLockRelease(g_storage_lock);
    return StorageError::None;
}

StorageError read_pinned_block(
        const ManifestPin& pin,
        std::size_t block_index,
        std::span<std::byte> destination,
        StorageReadPhase phase) {
    const StorageBackendIdentity current = current_backend_identity();
    if (!storage_ready() || !pin
        || pin.owner != current
        || block_index >= pin.block_count
        || destination.size() != pin.blocks[block_index].length) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_SHARED);
    const SharedManifestEntry& manifest =
        g_shared->manifests[pin.handle.slot];
    const SharedManifestReader& reader =
        manifest.readers[pin.reader_slot];
    const bool valid_pin = handle_matches(manifest, pin.handle)
        && reader_pin_matches(reader, pin)
        && manifest.block_count == pin.block_count
        && std::memcmp(
            &manifest.blocks[block_index],
            &pin.blocks[block_index],
            sizeof(BlockReferenceV1)) == 0;
    LWLockRelease(g_storage_lock);
    if (!valid_pin) {
        return StorageError::StaleToken;
    }
    const TimestampTz started_at = GetCurrentTimestamp();
    const BlockReferenceV1& reference = pin.blocks[block_index];
    StorageError error = StorageError::None;
    if (reference.tier == BlockBackingTierV1::Dsa) {
        if (!DsaPointerIsValid(
                static_cast<dsa_pointer>(reference.handle_or_file_id))) {
            return StorageError::InvalidArgument;
        }
        const auto* source = static_cast<const std::byte*>(
            dsa_get_address(
                g_dsa,
                static_cast<dsa_pointer>(reference.handle_or_file_id)));
        if (source == nullptr) {
            return StorageError::NotFound;
        }
        std::memcpy(
            destination.data(),
            source + reference.offset,
            destination.size());
    } else if (reference.tier == BlockBackingTierV1::File) {
        error = read_file_block(
            pin.database_oid, pin.handle.id, reference, destination);
    } else {
        return StorageError::InvalidArgument;
    }
    if (error == StorageError::None
        && crc32c(destination) != reference.crc32c) {
        error = StorageError::ChecksumMismatch;
    }
    const TimestampTz finished_at = GetCurrentTimestamp();
    const std::uint64_t duration = finished_at >= started_at
        ? static_cast<std::uint64_t>(finished_at - started_at)
        : 0;
    const auto saturating_add = [](
            std::uint64_t current, std::uint64_t increment) noexcept {
        return increment <=
                (std::numeric_limits<std::uint64_t>::max)() - current
            ? current + increment
            : (std::numeric_limits<std::uint64_t>::max)();
    };
    g_query_io_metrics.read_bytes = saturating_add(
        g_query_io_metrics.read_bytes, destination.size());
    g_query_io_metrics.read_duration_us = saturating_add(
        g_query_io_metrics.read_duration_us, duration);
    if (phase == StorageReadPhase::Prevalidation) {
        g_query_io_metrics.prevalidation_read_bytes = saturating_add(
            g_query_io_metrics.prevalidation_read_bytes, destination.size());
        g_query_io_metrics.prevalidation_read_duration_us = saturating_add(
            g_query_io_metrics.prevalidation_read_duration_us, duration);
    }
    g_query_io_metrics.checksum_status = error == StorageError::None
        ? StorageChecksumStatus::Valid
        : StorageChecksumStatus::Invalid;
    if (error == StorageError::ChecksumMismatch) {
        g_query_io_metrics.checksum_failure_count = saturating_add(
            g_query_io_metrics.checksum_failure_count, 1);
    }
    if (error != StorageError::None) {
        remember_failure(error);
    }
    return error;
}

StorageError retire_manifest(const ManifestHandle& handle) noexcept {
    if (!storage_ready() || !handle) {
        return StorageError::InvalidArgument;
    }
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    SharedManifestEntry& manifest = g_shared->manifests[handle.slot];
    if (!handle_matches(manifest, handle)) {
        LWLockRelease(g_storage_lock);
        return StorageError::StaleToken;
    }
    manifest.state = ManifestState::Retiring;
    LWLockRelease(g_storage_lock);
    request_janitor_sweep(false);
    return StorageError::None;
}

JanitorSweepResult janitor_sweep(
        std::int64_t now_us,
        bool aggressive) {
    JanitorSweepResult result{};
    if (!attach_storage()) {
        return result;
    }
    SweepOwnerToken sweep_owner{};
    if (!acquire_sweep_ownership(sweep_owner)) {
        if (aggressive) {
            request_janitor_sweep(true);
        }
        return result;
    }
    const std::int64_t now = current_time_us(now_us);
    const std::uint32_t requested = pg_atomic_exchange_u32(
        &g_shared->sweep_requested, 0);
    aggressive = aggressive || requested > 1;

    /* Every backend-status lookup is outside the storage lock. */
    reap_dead_manifest_references(result);
    reclaim_abandoned_blocks(result);

    /* Publishing repair and explicit registry retirement are the only
     * manifest-level reclamation inputs owned by storage. */
    for (std::uint32_t index = 0;
         index < kManifestCapacity
            && result.expired_manifests < g_shared->reclaim_batch;
         ++index) {
        std::uint64_t sequence = 0;
        bool expired = false;
        LWLockAcquire(g_storage_lock, LW_SHARED);
        const SharedManifestEntry& manifest = g_shared->manifests[index];
        if (manifest_reclaimable(manifest)) {
            expired = manifest.state == ManifestState::Publishing
                || manifest.state == ManifestState::Retiring;
            sequence = manifest.slot_sequence;
        }
        LWLockRelease(g_storage_lock);
        if (expired
            && reclaim_manifest_slot(index, sequence, result)
                == StorageError::None) {
            ++result.expired_manifests;
        }
    }

    /* The exact/reusable registries own TTL and LRU decisions.  Storage may
     * reclaim COMPLETE only after the owning registry calls retire_manifest;
     * otherwise a registry-visible handle could become stale between lookup
     * and its provisional manifest pin.  Pressure therefore denies new
     * admission until a registry-first eviction marks a manifest Retiring. */
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
    while (observed < desired
        && !pg_atomic_compare_exchange_u32(
            &g_shared->sweep_requested, &observed, desired)) {
    }
    /* The worker's one-second timed latch wake is the notification bound.
     * Avoid a PostgreSQL liveness lookup here because callers may still own
     * an exact/reusable registry lock when they queue physical retirement. */
}

bool publish_correctness_readiness(
        CacheCorrectnessReadiness readiness) noexcept {
    if (!known_readiness(readiness)) {
        return false;
    }
    g_pending_correctness_readiness = readiness;
    if (g_shared == nullptr) {
        return true;
    }
    pg_atomic_write_u32(
        &g_shared->correctness_readiness,
        static_cast<std::uint32_t>(readiness));
    return true;
}

void publish_worker_lifecycle(
        WorkerLifecycle lifecycle,
        int worker_pid) noexcept {
    g_pending_worker_lifecycle = lifecycle;
    if (g_shared == nullptr) {
        return;
    }
    pg_atomic_write_u32(
        &g_shared->worker_lifecycle,
        static_cast<std::uint32_t>(lifecycle));
    const StorageBackendIdentity current = current_backend_identity();
    LWLockAcquire(g_storage_lock, LW_EXCLUSIVE);
    g_shared->worker_owner = worker_pid > 0
            && current.valid()
            && current.pid == worker_pid
        ? current
        : StorageBackendIdentity{};
    LWLockRelease(g_storage_lock);
    publish_worker_heartbeat();
}

void publish_worker_heartbeat(std::int64_t now_us) noexcept {
    if (g_shared != nullptr) {
        pg_atomic_write_u64(
            &g_shared->worker_heartbeat_at_us,
            static_cast<std::uint64_t>(current_time_us(now_us)));
    }
}

void reset_query_io_metrics() noexcept {
    g_query_io_metrics = StorageQueryIoSnapshot{};
}

StorageQueryIoSnapshot query_io_metrics() noexcept {
    return g_query_io_metrics;
}

StorageRuntimeSnapshot runtime_snapshot() noexcept {
    StorageRuntimeSnapshot snapshot{};
    snapshot.dsa_attached = g_dsa != nullptr;
    const auto registry = shared_registry::shared_registry_diagnostics();
    snapshot.bootstrap_state = registry.state;
    snapshot.bootstrap_ready = registry.state == BootstrapState::Ready;
    if (g_shared == nullptr) {
        return snapshot;
    }
    snapshot.worker_lifecycle = static_cast<WorkerLifecycle>(
        pg_atomic_read_u32(&g_shared->worker_lifecycle));
    snapshot.correctness_readiness = static_cast<CacheCorrectnessReadiness>(
        pg_atomic_read_u32(&g_shared->correctness_readiness));
    snapshot.last_failure = static_cast<StorageError>(
        pg_atomic_read_u32(&g_shared->last_failure));
    snapshot.boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce);
    snapshot.reserved_hot_bytes = !g_accounting
        ? 0
        : pg_atomic_read_u64(g_accounting.reserved_dsa_bytes);
    snapshot.fixed_hot_bytes = registry.fixed_dsa_reserved_bytes;
    snapshot.live_hot_bytes = g_registry_root == nullptr
        ? 0
        : pg_atomic_read_u64(&g_registry_root->hot_live_bytes);
    const auto fixed_and_live = checked_add(
        snapshot.fixed_hot_bytes, snapshot.live_hot_bytes);
    snapshot.pending_hot_bytes = fixed_and_live.has_value() &&
            snapshot.reserved_hot_bytes >= *fixed_and_live
        ? snapshot.reserved_hot_bytes - *fixed_and_live
        : 0;
    snapshot.reserved_file_bytes = pg_atomic_read_u64(
        g_accounting.reserved_file_bytes);
    snapshot.hot_ceiling_bytes = g_shared->hot_ceiling_bytes;
    snapshot.file_ceiling_bytes = g_shared->file_ceiling_bytes;
    snapshot.manifest_count = pg_atomic_read_u64(&g_shared->manifest_count);
    snapshot.waiter_reference_count = pg_atomic_read_u64(
        &g_shared->waiter_reference_count);
    snapshot.reader_reference_count = pg_atomic_read_u64(
        &g_shared->reader_reference_count);
    snapshot.completed_sweeps = pg_atomic_read_u64(
        &g_shared->completed_sweeps);
    snapshot.last_sweep_at_us = static_cast<std::int64_t>(
        pg_atomic_read_u64(&g_shared->last_sweep_at_us));
    snapshot.worker_heartbeat_at_us = static_cast<std::int64_t>(
        pg_atomic_read_u64(&g_shared->worker_heartbeat_at_us));
    LWLockAcquire(g_storage_lock, LW_SHARED);
    const StorageBackendIdentity worker = g_shared->worker_owner;
    for (const BlockLedgerEntry& entry : g_shared->block_ledger) {
        if (!entry.reservation_accounted ||
            entry.state == BlockLedgerState::Empty) {
            continue;
        }
        if (entry.reservation_tier == BlockBackingTierV1::Dsa) {
            snapshot.dsa_stored_bytes += entry.payload_bytes;
        } else if (entry.reservation_tier == BlockBackingTierV1::File) {
            snapshot.file_stored_bytes += entry.payload_bytes;
        }
    }
    for (const SharedManifestEntry& manifest : g_shared->manifests) {
        if (manifest.state != ManifestState::Empty) {
            snapshot.row_group_count += manifest.row_group_count;
        }
    }
    LWLockRelease(g_storage_lock);
    snapshot.worker_pid = worker.pid;
    snapshot.worker_proc_number = worker.proc_number;
    snapshot.worker_backend_nonce = worker.backend_nonce;
    snapshot.worker_backend_started_at_us =
        worker.backend_started_at_us;
    const StorageQueryIoSnapshot query_metrics = query_io_metrics();
    snapshot.prevalidation_read_bytes =
        query_metrics.prevalidation_read_bytes;
    snapshot.prevalidation_read_duration_us =
        query_metrics.prevalidation_read_duration_us;
    snapshot.read_bytes = query_metrics.read_bytes;
    snapshot.read_duration_us = query_metrics.read_duration_us;
    snapshot.checksum_failure_count =
        query_metrics.checksum_failure_count;
    snapshot.checksum_status = query_metrics.checksum_status;
    return snapshot;
}

std::uint32_t janitor_interval_ms() noexcept {
    return g_shared != nullptr
        ? g_shared->sweep_interval_ms
        : g_requested_config.sweep_interval_ms;
}

} // namespace bioseq::cache::pg::storage

#if defined(BIOSEQ_CACHE_TESTING)
namespace bioseq::cache::pg::storage {

/** Add a test-ledger byte count without allowing diagnostic wraparound. */
[[nodiscard]] bool add_test_reserved_bytes(
        std::uint64_t& total, std::uint64_t bytes) noexcept {
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

StorageBootstrapTestSnapshot test_storage_bootstrap_snapshot() noexcept {
    StorageBootstrapTestSnapshot snapshot{};
    const auto diagnostics =
        shared_registry::shared_registry_diagnostics();
    snapshot.state = diagnostics.state;
    snapshot.failure = diagnostics.failure;
    snapshot.boot_nonce = diagnostics.boot_nonce;
    snapshot.initialization_attempt_count =
        diagnostics.initialization_attempt_count;
    snapshot.takeover_count = diagnostics.takeover_count;
    snapshot.cleanup_count = diagnostics.cleanup_count;
    snapshot.attach_count = diagnostics.attach_count;
    snapshot.validation_failure_count =
        diagnostics.validation_failure_count;
    snapshot.initializer = diagnostics.initializer;
    snapshot.last_initializer = diagnostics.last_initializer;
    snapshot.takeover_used = diagnostics.takeover_used;
    snapshot.live_unpublished_area_count =
        diagnostics.live_unpublished_area_count;
    snapshot.cleanup_handle_pinned = diagnostics.cleanup_handle_pinned;
    snapshot.cleanup_handle_published =
        diagnostics.cleanup_handle_published;
    snapshot.ready_handle_published = diagnostics.ready_handle_published;
    snapshot.root_published = diagnostics.root_published;
    const auto test_status = shared_registry::shared_registry_test_status();
    snapshot.pause_reached = test_status.reached;
    snapshot.defer_background_initializer =
        test_status.defer_background_initializer;
    snapshot.last_cleanup_handle_was_pinned =
        test_status.last_cleanup_handle_was_pinned;
    snapshot.last_cleanup_release_verified =
        test_status.last_cleanup_release_verified;
    snapshot.last_cleanup_handle = test_status.last_cleanup_handle;
    const auto* attachment =
        shared_registry::current_shared_registry_attachment();
    snapshot.storage_attached = storage_ready();
    if (attachment == nullptr || attachment->root == nullptr ||
        attachment->area == nullptr) {
        return snapshot;
    }
    snapshot.mapping_pinned = attachment->mapping_pinned;
    snapshot.reusable_wait_cv_count = static_cast<std::uint32_t>(
        attachment->reusable_wait_cvs.size());
    snapshot.storage_metadata_in_dsa =
        g_shared != nullptr &&
        dsa_get_address(
            attachment->area,
            attachment->root->extension_metadata) == g_shared;
    snapshot.reusable_wait_cvs_in_dsa =
        !attachment->reusable_wait_cvs.empty() &&
        dsa_get_address(
            attachment->area,
            attachment->root->reusable_wait_cvs) ==
            attachment->reusable_wait_cvs.data();
    return snapshot;
}

bool test_arm_storage_bootstrap_pause() noexcept {
    return shared_registry::arm_shared_registry_test_pause(
        shared_registry::SharedRegistryTestPointV1::BeforeHandlePublish);
}

void test_release_storage_bootstrap_pause() noexcept {
    shared_registry::release_shared_registry_test_pause();
}

bool test_inject_storage_bootstrap_allocation_failure(
        std::uint32_t ordinal) noexcept {
    return shared_registry::inject_shared_registry_allocation_failure(
        ordinal);
}

bool test_inject_storage_bootstrap_abi_mismatch(bool enabled) noexcept {
    return shared_registry::inject_shared_registry_root_abi_mismatch(enabled);
}

StorageBootstrapAttachTwiceSnapshot test_attach_storage_twice() noexcept {
    StorageBootstrapAttachTwiceSnapshot snapshot{};
    const auto before = shared_registry::shared_registry_diagnostics();
    snapshot.attempts_before = before.initialization_attempt_count;
    snapshot.attaches_before = before.attach_count;
    snapshot.first_attached = attach_storage();
    const auto* first =
        shared_registry::current_shared_registry_attachment();
    dsa_area* first_area = first == nullptr ? nullptr : first->area;
    auto* first_root = first == nullptr ? nullptr : first->root;
    snapshot.second_attached = attach_storage();
    const auto* second =
        shared_registry::current_shared_registry_attachment();
    snapshot.same_area = first_area != nullptr && second != nullptr &&
        first_area == second->area;
    snapshot.same_root = first_root != nullptr && second != nullptr &&
        first_root == second->root;
    snapshot.mapping_pinned = second != nullptr && second->mapping_pinned;
    const auto after = shared_registry::shared_registry_diagnostics();
    snapshot.attempts_after = after.initialization_attempt_count;
    snapshot.attaches_after = after.attach_count;
    return snapshot;
}

bool test_corrupt_storage_bootstrap_manifest(
        shared_registry::SharedRegistryTestManifestCorruptionV1 corruption)
        noexcept {
    if (!attach_storage() ||
        !shared_registry::corrupt_shared_registry_manifest_for_test(
            corruption) ||
        !test_detach_storage_mapping()) {
        return false;
    }
    const bool rejected = !attach_storage();
    return rejected &&
        shared_registry::shared_registry_diagnostics().state ==
            BootstrapState::Failed;
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

StorageTierTestSnapshot test_storage_tier_snapshot() noexcept {
    StorageTierTestSnapshot snapshot{};
    if (!attach_storage()) {
        return snapshot;
    }
    bool known_states = true;
    std::uint64_t accounted_blocks = 0;
    std::uint64_t dynamic_hot_reservations = 0;
    LWLockAcquire(g_storage_lock, LW_SHARED);
    snapshot.force_file_tier =
        pg_atomic_read_u32(&g_shared->force_file_tier) != 0;
    for (const BlockLedgerEntry& entry : g_shared->block_ledger) {
        if (entry.state == BlockLedgerState::Empty) {
            continue;
        }
        ++snapshot.occupied_blocks;
        switch (entry.state) {
            case BlockLedgerState::Reserved:
                ++snapshot.reserved_blocks;
                break;
            case BlockLedgerState::Stored:
                ++snapshot.stored_blocks;
                break;
            case BlockLedgerState::Referenced:
                ++snapshot.referenced_blocks;
                break;
            case BlockLedgerState::Retiring:
                ++snapshot.retiring_blocks;
                break;
            case BlockLedgerState::ReleaseCommitted:
                ++snapshot.release_committed_blocks;
                break;
            case BlockLedgerState::Empty:
                break;
            default:
                known_states = false;
                break;
        }
        if (!entry.reservation_accounted) {
            continue;
        }
        ++accounted_blocks;
        if (entry.reservation_tier == BlockBackingTierV1::Dsa) {
            ++snapshot.dsa_blocks;
            known_states = add_test_reserved_bytes(
                dynamic_hot_reservations, entry.reserved_bytes)
                && known_states;
            known_states = add_test_reserved_bytes(
                snapshot.dsa_stored_bytes, entry.payload_bytes)
                && known_states;
        } else if (entry.reservation_tier == BlockBackingTierV1::File) {
            ++snapshot.file_blocks;
            known_states = add_test_reserved_bytes(
                snapshot.reserved_file_bytes, entry.reserved_bytes)
                && known_states;
            known_states = add_test_reserved_bytes(
                snapshot.file_stored_bytes, entry.payload_bytes)
                && known_states;
        } else {
            known_states = false;
        }
    }
    for (const SharedManifestEntry& manifest : g_shared->manifests) {
        if (manifest.state != ManifestState::Empty) {
            snapshot.reader_pins += atomic_read(manifest.reader_pins);
            known_states = add_test_reserved_bytes(
                snapshot.row_group_count, manifest.row_group_count)
                && known_states;
        }
    }
    const std::uint64_t state_total = snapshot.reserved_blocks
        + snapshot.stored_blocks
        + snapshot.referenced_blocks
        + snapshot.retiring_blocks
        + snapshot.release_committed_blocks;
    snapshot.coherent = known_states
        && state_total == snapshot.occupied_blocks
        && snapshot.dsa_blocks + snapshot.file_blocks == accounted_blocks
        && snapshot.reader_pins
            == pg_atomic_read_u64(&g_shared->reader_reference_count);
    LWLockRelease(g_storage_lock);
    const StorageRuntimeSnapshot runtime = runtime_snapshot();
    snapshot.reserved_hot_bytes = runtime.reserved_hot_bytes;
    snapshot.fixed_hot_bytes = runtime.fixed_hot_bytes;
    snapshot.live_hot_bytes = runtime.live_hot_bytes;
    snapshot.pending_hot_bytes = runtime.pending_hot_bytes;
    snapshot.coherent = snapshot.coherent &&
        runtime.reserved_hot_bytes >= runtime.fixed_hot_bytes &&
        dynamic_hot_reservations <= runtime.reserved_hot_bytes;
    return snapshot;
}

} // namespace bioseq::cache::pg::storage

export namespace bioseq::cache::pg::storage::bootstrap_test_api {

/** Test controls are private-cluster superuser operations only. */
inline void require_superuser() {
    if (!superuser()) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            {
                .message =
                    "bioseq_cache bootstrap test controls require superuser",
            });
    }
}

inline void append_json_member(
        std::string& json,
        std::string_view key,
        std::string_view encoded_value) {
    if (json.size() != 1) {
        json.push_back(',');
    }
    json.push_back('"');
    json.append(key);
    json.append("\":");
    json.append(encoded_value);
}

template<class Integer>
    requires std::is_integral_v<Integer>
inline void append_json_integer(
        std::string& json,
        std::string_view key,
        Integer value) {
    append_json_member(json, key, std::to_string(value));
}

inline void append_json_boolean(
        std::string& json,
        std::string_view key,
        bool value) {
    append_json_member(json, key, value ? "true" : "false");
}

/** Return one owning snapshot of shared bootstrap state. */
[[nodiscard]] inline bioseq::pg_utils::sql_jsonb bootstrap_status() {
    require_superuser();
    const StorageBootstrapTestSnapshot snapshot =
        test_storage_bootstrap_snapshot();
    std::string json{"{"};
    json.reserve(1'024);
    append_json_integer(
        json, "state", static_cast<std::uint32_t>(snapshot.state));
    append_json_integer(
        json, "failure", static_cast<std::uint32_t>(snapshot.failure));
    append_json_integer(json, "boot_nonce", snapshot.boot_nonce);
    append_json_integer(
        json,
        "initialization_attempts",
        snapshot.initialization_attempt_count);
    append_json_integer(json, "takeovers", snapshot.takeover_count);
    append_json_integer(json, "cleanups", snapshot.cleanup_count);
    append_json_integer(json, "attaches", snapshot.attach_count);
    append_json_integer(
        json,
        "validation_failures",
        snapshot.validation_failure_count);
    append_json_integer(
        json, "takeover_used", snapshot.takeover_used);
    append_json_integer(
        json,
        "live_unpublished_areas",
        snapshot.live_unpublished_area_count);
    append_json_integer(
        json,
        "cleanup_handle_pinned",
        snapshot.cleanup_handle_pinned);
    append_json_boolean(
        json,
        "cleanup_handle_published",
        snapshot.cleanup_handle_published);
    append_json_boolean(
        json,
        "ready_handle_published",
        snapshot.ready_handle_published);
    append_json_boolean(
        json, "root_published", snapshot.root_published);
    append_json_boolean(
        json, "storage_attached", snapshot.storage_attached);
    append_json_boolean(
        json, "mapping_pinned", snapshot.mapping_pinned);
    append_json_boolean(
        json,
        "storage_metadata_in_dsa",
        snapshot.storage_metadata_in_dsa);
    append_json_boolean(
        json,
        "reusable_wait_cvs_in_dsa",
        snapshot.reusable_wait_cvs_in_dsa);
    append_json_integer(
        json,
        "reusable_wait_cv_count",
        snapshot.reusable_wait_cv_count);
    append_json_boolean(
        json, "pause_reached", snapshot.pause_reached);
    append_json_boolean(
        json,
        "defer_background_initializer",
        snapshot.defer_background_initializer);
    append_json_integer(
        json,
        "last_cleanup_handle",
        snapshot.last_cleanup_handle);
    append_json_boolean(
        json,
        "last_cleanup_was_pinned",
        snapshot.last_cleanup_handle_was_pinned);
    append_json_boolean(
        json,
        "last_cleanup_release_verified",
        snapshot.last_cleanup_release_verified);
    append_json_integer(
        json,
        "initializer_proc",
        static_cast<std::uint32_t>(
            snapshot.initializer.owner.proc_number));
    append_json_integer(
        json, "initializer_pid", snapshot.initializer.owner.pid);
    append_json_integer(
        json,
        "initializer_nonce",
        snapshot.initializer.owner.backend_nonce);
    append_json_integer(
        json,
        "initializer_started_at",
        snapshot.initializer.owner.backend_started_at_us);
    append_json_integer(
        json,
        "initializer_claim",
        snapshot.initializer.claim_sequence);
    append_json_integer(
        json,
        "last_initializer_proc",
        static_cast<std::uint32_t>(
            snapshot.last_initializer.owner.proc_number));
    append_json_integer(
        json,
        "last_initializer_pid",
        snapshot.last_initializer.owner.pid);
    append_json_integer(
        json,
        "last_initializer_nonce",
        snapshot.last_initializer.owner.backend_nonce);
    append_json_integer(
        json,
        "last_initializer_started_at",
        snapshot.last_initializer.owner.backend_started_at_us);
    append_json_integer(
        json,
        "last_initializer_claim",
        snapshot.last_initializer.claim_sequence);
    json.push_back('}');
    return bioseq::pg_utils::sql_jsonb{std::move(json)};
}

/** Prove that repeated attachment selects one shared area and root. */
[[nodiscard]] inline bioseq::pg_utils::sql_jsonb
bootstrap_attach_twice() {
    require_superuser();
    const StorageBootstrapAttachTwiceSnapshot snapshot =
        test_attach_storage_twice();
    std::string json{"{"};
    json.reserve(384);
    append_json_boolean(
        json, "first_attached", snapshot.first_attached);
    append_json_boolean(
        json, "second_attached", snapshot.second_attached);
    append_json_boolean(json, "same_area", snapshot.same_area);
    append_json_boolean(json, "same_root", snapshot.same_root);
    append_json_boolean(
        json, "mapping_pinned", snapshot.mapping_pinned);
    append_json_integer(
        json, "attempts_before", snapshot.attempts_before);
    append_json_integer(
        json, "attempts_after", snapshot.attempts_after);
    append_json_integer(
        json, "attaches_before", snapshot.attaches_before);
    append_json_integer(
        json, "attaches_after", snapshot.attaches_after);
    json.push_back('}');
    return bioseq::pg_utils::sql_jsonb{std::move(json)};
}

/** Corrupt one named bootstrap-manifest field for recovery testing. */
[[nodiscard]] inline bool corrupt_bootstrap_manifest(
        const std::string& name) {
    require_superuser();
    shared_registry::SharedRegistryTestManifestCorruptionV1 corruption{};
    if (name == "magic") {
        corruption =
            shared_registry::SharedRegistryTestManifestCorruptionV1::Magic;
    } else if (name == "abi") {
        corruption = shared_registry::
            SharedRegistryTestManifestCorruptionV1::AbiVersion;
    } else if (name == "boot_nonce") {
        corruption = shared_registry::
            SharedRegistryTestManifestCorruptionV1::BootNonce;
    } else {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "unknown bootstrap manifest corruption: " + name,
            });
    }
    return test_corrupt_storage_bootstrap_manifest(corruption);
}

} // namespace bioseq::cache::pg::storage::bootstrap_test_api

#endif
