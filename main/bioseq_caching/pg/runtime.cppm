/*-------------------------------------------------------------------------
 *
 * bioseq_cache PostgreSQL runtime composition.
 *
 * The sibling extension deliberately contains no sequence or Bioseq table-AM
 * knowledge.  Product integrations arrive through bioseq.cache.provider_api.
 * This implementation unit retains the behavior-preserving shared runtime
 * while the public primary interface composes dedicated PostgreSQL owners.
 * Storage and registry behavior is enabled incrementally behind the same
 * fail-open node.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "access/xact.h"
#include "access/transam.h"
#include "access/twophase.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_class.h"
#include "catalog/heap.h"
#include "catalog/pg_inherits.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_type_d.h"
#include "common/cryptohash.h"
#include "common/hashfn.h"
#include "commands/explain.h"
#include "executor/execdesc.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "executor/tuptable.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/params.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/planner.h"
#include "parser/parsetree.h"
#include "portability/instr_time.h"
#include "storage/ipc.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/array.h"
#include "utils/acl.h"
#include "utils/backend_status.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/rls.h"
#include "utils/ruleutils.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "port/pg_crc32c.h"
}

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <array>
#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

module bioseq.cache.pg;

import bioseq.cache.core;
import bioseq.cache.pg.dependency_runtime;
import bioseq.cache.pg.executor;
import bioseq.cache.pg.guc;
import bioseq.cache.pg.maintenance;
import bioseq.cache.pg.object_dependency_hooks;
import bioseq.cache.pg.planner;
import bioseq.cache.pg.provider;
import bioseq.cache.pg.shared_registry;
import bioseq.cache.pg.storage;
import bioseq.cache.pg.sql.rows;
import bioseq.pg_support;
import bioseq.pg_sql_boundary;

namespace bioseq::cache::pg {

using guc::QueryCacheMode;

inline constexpr std::uint32_t kSharedMagic = 0x42535143U; /* BSQC */
inline constexpr std::uint32_t kSharedAbiVersion = 10;
inline constexpr char kSharedName[] = "bioseq_cache shared state v10";
inline constexpr char kRegistryTrancheName[] = "bioseq_cache registry";
inline constexpr std::size_t kRegistryCapacity = 64;
inline constexpr std::size_t kMaximumKeyBytes = 64U * 1024U;
inline constexpr std::uint32_t kExactProviderRecipePlanFormatV1 = 1;
inline constexpr std::size_t kExactProviderRecipeNodeCountV1 = 6;
inline constexpr std::size_t kMaximumExactPlanWalkNodesV1 = 4096;
inline constexpr std::size_t kMaximumExactSubplansV1 = 1024;
inline constexpr std::size_t kExactDisplaySqlCapacityV1 = 4U * 1024U;
inline constexpr std::size_t kExactCanonicalPlanCapacityV1 = 16U * 1024U;
inline constexpr std::size_t kExactProducerPlanCapacityV1 = 16U * 1024U;
inline constexpr std::uint64_t kMaximumStoredResultBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumCoverageSelectionBitmapBytesV1 =
    64ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumCoverageOccurrenceOwnersV1 =
    1ULL << 20U;
inline constexpr std::uint64_t kMaximumCoverageProducerSpoolBytesV1 =
    256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaximumCoverageProducerSpoolRowsV1 =
    1ULL << 20U;
inline constexpr std::size_t kRelationEpochCapacity =
    static_cast<std::size_t>(kMaxDependenciesPerGeneration) * 4U;
inline constexpr std::size_t kMaximumRelationDependencies =
    static_cast<std::size_t>(kMaxDependenciesPerGeneration);

/** One exact-plan syscache dependency, matching PostgreSQL PlanInvalItem. */
struct ExactSyscacheDependencyV1 {
    std::int32_t cache_id = -1;
    std::uint32_t hash_value = 0;

    [[nodiscard]] bool operator==(
        const ExactSyscacheDependencyV1&) const noexcept = default;
};

static_assert(std::is_trivially_copyable_v<ExactSyscacheDependencyV1>);

/** PostgreSQL-backed checksums for the shared generic column-store format. */
struct PostgreSqlColumnChecksumPolicy {
    [[nodiscard]] std::expected<std::uint32_t, ChecksumError> block_crc32c(
            std::span<const std::byte> bytes) const noexcept {
        pg_crc32c checksum = 0;
        INIT_CRC32C(checksum);
        if (!bytes.empty()) {
            COMP_CRC32C(checksum, bytes.data(), bytes.size());
        }
        FIN_CRC32C(checksum);
        return static_cast<std::uint32_t>(checksum);
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError> sha256(
            std::span<const std::byte> bytes) const noexcept {
        pg_cryptohash_ctx* context = pg_cryptohash_create(PG_SHA256);
        if (context == nullptr) {
            return std::unexpected(ChecksumError::Unavailable);
        }
        CacheDigest256 digest{};
        const bool success = pg_cryptohash_init(context) >= 0
            && (bytes.empty()
                || pg_cryptohash_update(
                    context,
                    reinterpret_cast<const std::uint8_t*>(bytes.data()),
                    bytes.size()) >= 0)
            && pg_cryptohash_final(
                context, digest.bytes.data(), digest.bytes.size()) >= 0;
        pg_cryptohash_free(context);
        if (!success) {
            return std::unexpected(ChecksumError::Failed);
        }
        return digest;
    }

    [[nodiscard]] std::expected<CacheDigest256, ChecksumError>
    manifest_sha256(
            std::span<const CacheDigest256> digests) const noexcept {
        pg_cryptohash_ctx* context = pg_cryptohash_create(PG_SHA256);
        if (context == nullptr) {
            return std::unexpected(ChecksumError::Unavailable);
        }
        bool success = pg_cryptohash_init(context) >= 0;
        for (const CacheDigest256& digest : digests) {
            success = success
                && pg_cryptohash_update(
                    context, digest.bytes.data(), digest.bytes.size()) >= 0;
        }
        CacheDigest256 result{};
        success = success
            && pg_cryptohash_final(
                context, result.bytes.data(), result.bytes.size()) >= 0;
        pg_cryptohash_free(context);
        if (!success) {
            return std::unexpected(ChecksumError::Failed);
        }
        return result;
    }
};

/** Shared counters exposed by bioseq_cache.stats(). */
struct CacheSharedStats {
    pg_atomic_uint64 produced;
    pg_atomic_uint64 completed_hits;
    pg_atomic_uint64 inflight_waits;
    pg_atomic_uint64 reuse_hits;
    pg_atomic_uint64 fallbacks;
    pg_atomic_uint64 invalidations;
    pg_atomic_uint64 evictions;
    pg_atomic_uint64 logical_bytes;
    pg_atomic_uint64 stored_bytes;
    pg_atomic_uint64 planner_build_generation;
};

#if defined(BIOSEQ_CACHE_TESTING)
inline constexpr std::size_t kInjectedCoverageCapacity = 256;
inline constexpr std::size_t kInjectedDigestBytes = 64;

enum class TestPausePoint : std::uint32_t {
    AfterBuildingPublish = 0,
    AfterWaiterRegistered = 1,
    BeforeTerminalPublish = 2,
    AfterReaderPinned = 3,
    BeforeCoveragePreflight = 4,
    AfterCoverageReaderPinned = 5,
    BeforeDependencyVisibilityTicket = 6,
    AfterDependencyTransitionPublish = 7,
    Count = 8,
};

inline constexpr std::size_t kBackendPauseSlotCountV1 = 8;

enum class BackendPauseStateV1 : std::uint32_t {
    Free = 0,
    Armed = 1,
    Paused = 2,
    Released = 3,
};

struct CacheBackendPauseControlV1 {
    pg_atomic_uint32 state;
    std::uint32_t point;
    std::int32_t target_pid;
    std::uint32_t reserved;
};

struct CacheTestControl {
    pg_atomic_uint32 armed_mask;
    pg_atomic_uint32 paused_mask;
    pg_atomic_uint32 released_mask;
    pg_atomic_uint32 fail_next_write;
    pg_atomic_uint64 exact_child_executions;
    std::array<CacheBackendPauseControlV1,
        kBackendPauseSlotCountV1> backend_pauses;
    ConditionVariable changed;
};

struct InjectedCoverage {
    bool used;
    bool evictable;
    std::uint16_t domain_size;
    std::uint16_t predicate_size;
    std::uint16_t provider_digest_size;
    std::uint16_t forced_digest_size;
    Oid database_oid;
    Oid current_role_oid;
    Oid session_user_oid;
    std::uint32_t security_context_flags;
    std::uint64_t generation;
    unsigned char domain[kInjectedDigestBytes];
    unsigned char predicate[kInjectedDigestBytes];
    unsigned char provider_digest[kInjectedDigestBytes];
    unsigned char forced_digest[kInjectedDigestBytes];
};
#endif

enum class SharedEntryState : std::uint32_t {
    Empty = 0,
    Claimed = 1,
    Producing = 2,
    Complete = 3,
    Failed = 4,
    Retired = 5,
};

[[nodiscard]] constexpr bool exact_entry_in_flight(
        SharedEntryState state) noexcept {
    return state == SharedEntryState::Claimed
        || state == SharedEntryState::Producing;
}

struct RegistryWaiterOwner {
    bool active;
    std::uint8_t reserved8[3];
    int owner_pid;
    std::uint32_t owner_proc_number;
    std::uint64_t owner_backend_nonce;
    TimestampTz owner_backend_started_at;
    std::uint64_t registration_sequence;
    TimestampTz registered_at;
    storage::ManifestWaiterToken storage_token;
};

struct RegistryReaderOwner {
    bool active;
    std::uint8_t reserved8[3];
    int owner_pid;
    std::uint32_t owner_proc_number;
    std::uint64_t owner_backend_nonce;
    TimestampTz owner_backend_started_at;
    std::uint64_t registration_sequence;
};

/** One bounded registry record. Full key bytes make hash collisions harmless. */
struct CacheEntry {
    SharedEntryState state;
    std::uint32_t key_size;
    std::uint64_t generation;
    std::uint64_t claim_sequence;
    std::uint64_t key_hash;
    CacheDigest256 key_digest;
    std::uint64_t database_epoch;
    std::uint64_t completion_count;
    Oid database_oid;
    Oid current_role_oid;
    Oid session_user_oid;
    int producer_pid;
    ProcNumber producer_proc_number;
    std::uint64_t producer_backend_nonce;
    TimestampTz producer_backend_started_at;
    std::uint32_t waiter_count;
    std::uint32_t reader_count;
    RegistryReaderOwner readers[storage::kReaderPinsPerManifest];
    RegistryWaiterOwner waiters[storage::kWaitersPerManifest];
    TimestampTz created_at;
    TimestampTz completed_at;
    TimestampTz expires_at;
    std::uint64_t row_count;
    std::uint64_t logical_bytes;
    std::uint64_t stored_bytes;
    std::uint64_t hits;
    std::uint16_t relation_count;
    std::uint16_t syscache_dependency_count;
    Oid relation_oids[kMaximumRelationDependencies];
    ExactSyscacheDependencyV1
        syscache_dependencies[kMaxDependenciesPerGeneration];
    storage::ManifestHandle manifest_handle;
    std::array<char, kExactDisplaySqlCapacityV1> display_sql;
    std::uint32_t canonical_plan_bytes;
    std::uint32_t producer_plan_bytes;
    ReusablePlanSnapshotStatusV1 canonical_plan_status;
    ReusablePlanSnapshotStatusV1 producer_plan_status;
    bool display_sql_truncated;
    bool canonical_plan_truncated;
    bool producer_plan_truncated;
    std::uint8_t reserved_diagnostic;
    std::array<char, kExactCanonicalPlanCapacityV1> canonical_plan;
    std::array<char, kExactProducerPlanCapacityV1> producer_plan;
    ConditionVariable changed;
    unsigned char key[kMaximumKeyBytes];
};

/** Collision-checked bounded dependency epoch; protected by registry lock. */
struct RelationEpochSlot {
    Oid database_oid;
    Oid relation_oid;
    std::uint64_t committed_epoch;
    std::uint32_t transition_count;
    bool used;
    std::array<std::uint8_t, 3> reserved{};
    ConditionVariable changed;
};

/** Bounded postmaster-lifetime header. Variable cache state is added via DSA. */
struct CacheSharedState {
    std::uint32_t magic;
    std::uint32_t shared_abi_version;
    pg_atomic_uint32 ready;
    pg_atomic_uint32 bootstrap_failure;
    pg_atomic_uint64 boot_nonce;
    pg_atomic_uint64 database_write_epoch;
    pg_atomic_uint64 coarse_dependency_epoch;
    pg_atomic_uint32 last_committed_write_xid;
    pg_atomic_uint32 database_transition_count;
    pg_atomic_uint32 coarse_transition_count;
    pg_atomic_uint64 next_claim_sequence;
    pg_atomic_uint64 next_registry_waiter_sequence;
    pg_atomic_uint64 next_registry_reader_sequence;
    ConditionVariable database_changed;
    CacheSharedStats stats;
#if defined(BIOSEQ_CACHE_TESTING)
    CacheTestControl test_control;
    pg_atomic_uint64 next_injected_generation;
    InjectedCoverage injected_coverage[kInjectedCoverageCapacity];
#endif
    RelationEpochSlot relation_epochs[kRelationEpochCapacity];
    CacheEntry entries[kRegistryCapacity];
};

static_assert(std::is_standard_layout_v<CacheSharedState>);
static_assert(alignof(CacheSharedState) <= MAXIMUM_ALIGNOF);
static_assert(sizeof(CacheEntry) <= 256U * 1024U);
static_assert(sizeof(CacheSharedState) <= 32U * 1024U * 1024U);

inline CacheSharedState* g_shared = nullptr;
inline LWLock* g_registry_lock = nullptr;
inline ReusableRegistryAttachmentV1 g_reusable_registry{};
inline LWLock* g_reusable_membership_lock = nullptr;
inline LWLockPadded* g_reusable_exact_locks = nullptr;
inline LWLockPadded* g_reusable_coverage_locks = nullptr;
inline ConditionVariable* g_reusable_wait_slots = nullptr;
inline std::uint32_t g_reusable_wait_slot_count = 0;
inline bool g_callbacks_registered = false;
inline bool g_internal_planning = false;
#if defined(BIOSEQ_CACHE_TESTING)
inline bool g_forced_coverage_domain_digest_active = false;
inline CacheDigest256 g_forced_coverage_domain_digest{};
#endif

/** One allocation-free query-state link drained before DSA/storage detach. */
struct BackendQueryCleanupLinkV1 {
    BackendQueryCleanupLinkV1* previous = nullptr;
    BackendQueryCleanupLinkV1* next = nullptr;
    void (*cleanup)(void*) noexcept = nullptr;
    void* state = nullptr;
    bool linked = false;
    std::array<std::uint8_t, 7> reserved{};
};

inline BackendQueryCleanupLinkV1* g_backend_query_cleanup_head = nullptr;
inline bool g_backend_query_cleanup_exit_registered = false;

void unlink_backend_query_cleanup(
        BackendQueryCleanupLinkV1& link) noexcept {
    if (!link.linked) {
        return;
    }
    if (link.previous != nullptr) {
        link.previous->next = link.next;
    } else if (g_backend_query_cleanup_head == &link) {
        g_backend_query_cleanup_head = link.next;
    }
    if (link.next != nullptr) {
        link.next->previous = link.previous;
    }
    link = {};
}

void link_backend_query_cleanup(
        BackendQueryCleanupLinkV1& link,
        void* state,
        void (*cleanup)(void*) noexcept) noexcept {
    if (link.linked || state == nullptr || cleanup == nullptr) {
        return;
    }
    link.previous = nullptr;
    link.next = g_backend_query_cleanup_head;
    link.cleanup = cleanup;
    link.state = state;
    link.linked = true;
    if (g_backend_query_cleanup_head != nullptr) {
        g_backend_query_cleanup_head->previous = &link;
    }
    g_backend_query_cleanup_head = &link;
}

/** Drain query-owned capabilities while shared mappings are still attached. */
void cache_backend_query_cleanup_exit(int, Datum) noexcept {
    while (g_backend_query_cleanup_head != nullptr) {
        BackendQueryCleanupLinkV1* link = g_backend_query_cleanup_head;
        void (*cleanup)(void*) noexcept = link->cleanup;
        void* state = link->state;
        if (cleanup != nullptr && state != nullptr) {
            cleanup(state);
        }
        if (link->linked) {
            unlink_backend_query_cleanup(*link);
        }
    }
    g_reusable_registry = {};
    g_reusable_membership_lock = nullptr;
    g_reusable_exact_locks = nullptr;
    g_reusable_coverage_locks = nullptr;
    g_reusable_wait_slots = nullptr;
    g_reusable_wait_slot_count = 0;
}

void register_backend_query_cleanup_exit() noexcept {
    if (g_backend_query_cleanup_exit_registered) {
        return;
    }
    before_shmem_exit(cache_backend_query_cleanup_exit, Datum{0});
    g_backend_query_cleanup_exit_registered = true;
}

#if defined(BIOSEQ_CACHE_TESTING)
/** Backend-local verifier faults exercise unavailable third-party AM paths. */
enum class TestExactPlanFault : std::uint8_t {
    None,
    UnknownCustomScan,
    UnknownTableAccessMethod,
    UnknownIndexAccessMethod,
    DependencyFinalizationDeclined,
};

/** Corrupt only one recipe copied into each exact wrapper plan. */
enum class TestExactRecipeFault : std::uint8_t {
    None,
    ExpectedDigestMismatch,
};

/** Checksum-consistent wire faults exercise total descriptor validation. */
enum class TestCoverageDescriptorFault : std::uint8_t {
    None,
    RawProducerVar,
    ForeignMixedVar,
    WrongNodeType,
    MalformedSyntax,
    BindingSignatureMismatch,
    PlanPrivateShort,
    PlanPrivateBadVersion,
};

inline TestExactPlanFault g_test_exact_plan_fault = TestExactPlanFault::None;
inline TestExactRecipeFault g_test_exact_recipe_fault =
    TestExactRecipeFault::None;
inline TestCoverageDescriptorFault g_test_coverage_descriptor_fault =
    TestCoverageDescriptorFault::None;
#endif

/** Backend-local dirty state follows savepoint commit and rollback exactly. */
struct DirtyRelationObservationV1 {
    Oid relation_oid;
    bool physical_identity_valid;
    bool storage_parent;
    std::array<std::uint8_t, 2> reserved{};
    PhysicalIdentityV1 physical_identity{};
    WriteObservability observability = WriteObservability::ExecutorUtilityOnly;

    [[nodiscard]] bool operator==(
        const DirtyRelationObservationV1&) const noexcept = default;
};

inline constexpr std::size_t kMaximumDirtySubtransactionFramesV1 = 64;
using DirtyObservationLedgerV1 = TransactionWriteLedgerV1<
    DirtyRelationObservationV1,
    kMaximumRelationDependencies,
    kMaximumDirtySubtransactionFramesV1>;
inline DirtyObservationLedgerV1 g_dirty_observations{};
inline bool g_write_invalidation_deferred_to_emergency = false;
inline bool g_database_transition_published = false;
inline bool g_coarse_transition_published = false;
inline TransactionId g_database_transition_xid = InvalidTransactionId;
inline bool g_force_coarse_transition = false;
enum class DatabaseTransitionPurposeV1 : std::uint8_t {
    None = 0,
    TopLevelWrite = 1,
    PrepareHandoff = 2,
    CommitPreparedResolution = 3,
    RollbackPreparedResolution = 4,
    ImmediateWrite = 5,
};
enum class DatabaseTransitionPhaseV1 : std::uint8_t {
    Idle = 0,
    Publishing = 1,
    Published = 2,
    Finishing = 3,
};
inline DatabaseTransitionPurposeV1 g_database_transition_purpose =
    DatabaseTransitionPurposeV1::None;
inline DatabaseTransitionPhaseV1 g_database_transition_phase =
    DatabaseTransitionPhaseV1::Idle;
inline std::uint64_t g_database_transition_owner_token = 0;
inline std::array<Oid, kMaximumRelationDependencies>
    g_transition_relation_oids{};
inline std::size_t g_transition_relation_count = 0;

void observe_runtime_database_dependency_write() noexcept;
void observe_runtime_relation_write(
    const DirtyRelationObservationV1& observation) noexcept;
void observe_runtime_provider_relation_write(
    Oid relation_oid,
    const PhysicalIdentityV1& physical_identity) noexcept;
[[nodiscard]] bool bind_shared_reusable_registry() noexcept;
void mark_shared_cache_failed() noexcept;

/**
 * Writers must bind the reusable registry even when their cache GUC is
 * disabled.  Do this while normal executor/utility error handling is still
 * available; transaction callbacks are too late to attach shared storage.
 */
void bind_reusable_registry_for_write_invalidation() noexcept {
    if (!guc::preload_requested() || g_shared == nullptr
        || pg_atomic_read_u32(&g_shared->ready) == 0
        || pg_atomic_read_u32(&g_shared->bootstrap_failure) != 0) {
        return;
    }
    /*
     * A disabled writer must invalidate an existing reusable registry, but it
     * must not become or wait for the lazy initializer.  Before READY there is
     * no published reusable generation; the preallocated emergency write
     * dependency remains the conservative transition authority.
     */
    const BootstrapState bootstrap_state =
        shared_registry::shared_registry_bootstrap_state();
    if (bootstrap_state != BootstrapState::Ready) {
        if (bootstrap_state == BootstrapState::Uninitialized
            || bootstrap_state == BootstrapState::Initializing) {
            g_write_invalidation_deferred_to_emergency = true;
            dependency_runtime::force_emergency_fallback();
        } else if (bootstrap_state == BootstrapState::Failed) {
            mark_shared_cache_failed();
        }
        return;
    }
    if (!bind_shared_reusable_registry()) {
        /*
         * The source write must remain fail-open, but reusable reads cannot
         * remain enabled when this commit cannot invalidate their registry.
         */
        mark_shared_cache_failed();
    }
}

/** One query-local provider intent retained only for this planner invocation. */
struct NestedPlannerIntentV1 {
    Query* query = nullptr;
    const provider_registry::ProviderV2* provider = nullptr;
    reuse::PlannerOwnedIntentV1 intent{};
    bool supported = false;
};

inline constexpr std::size_t kMaximumSelectedCoverageGatesV1 =
    static_cast<std::size_t>(kMaxCoverageNodesPerPlan)
    * static_cast<std::size_t>(kMaxSourcesPerCoverageNode);

struct SelectedCoverageGateV1 {
    PlannerInfo* origin_root = nullptr;
    int param_id = -1;
};

enum class CoverageBypassMarkerV1 : int {
    Enabled = 0,
    Provisional = 1,
    Permanent = 2,
};

enum class ExactBypassMarkerV1 : int {
    Enabled = 0,
    UnsafeFinalizedPlan = 1,
    CoverageGateFinalizer = 2,
};

/** Per-planner invocation frame; PostgreSQL backends are single-threaded. */
struct PlannerFrame {
    PlannerFrame* previous = nullptr;
    Query* delegated_query = nullptr;
    char* canonical_query = nullptr;
    char* display_sql = nullptr;
    List* referenced_parameters = NIL;
    List* referenced_relation_oids = NIL;
    List* referenced_syscache_dependencies = NIL;
    std::array<const provider_registry::ProviderV2*,
        provider_registry::kMaximumRegisteredProvidersV2> providers{};
    std::array<reuse::PlannerOwnedIntentV1,
        provider_registry::kMaximumRegisteredProvidersV2> intents{};
    std::array<NestedPlannerIntentV1,
        static_cast<std::size_t>(kMaxCoverageNodesPerPlan)>
        nested_intents{};
    std::array<SelectedCoverageGateV1,
        kMaximumSelectedCoverageGatesV1> selected_coverage_gates{};
    std::size_t provider_intent_count = 0;
    std::size_t nested_intent_count = 0;
    std::size_t selected_coverage_gate_count = 0;
    PlannedStmt* bridge_finalized_statement = nullptr;
    bool selected_coverage_gate_overflow = false;
    bool nested_coverage_path_created = false;
    bool eligible = false;
};

inline PlannerFrame* g_planner_frame = nullptr;
inline bool g_producer_plan_capture_active = false;
inline MemoryContext g_plan_capture_context = nullptr;

/**
 * Remember each selected gate until the top root is created.  PostgreSQL
 * identifies outer Params before create_plan(), so adding a gate only to a
 * nested root is insufficient: its extParam later propagates into the top
 * SS_finalize_plan pass.  Re-applying the bounded records at the selected
 * top CustomPath is the first stable point after every identify pass.
 */
[[nodiscard]] bool remember_selected_coverage_gate(
        PlannerInfo& root, int param_id) {
    PlannerFrame* frame = g_planner_frame;
    if (frame == nullptr || root.glob == nullptr || param_id < 0
        || param_id >= list_length(root.glob->paramExecTypes)
        || list_nth_oid(root.glob->paramExecTypes, param_id) != BOOLOID
        || frame->selected_coverage_gate_overflow) {
        return false;
    }
    for (std::size_t index = 0;
         index < frame->selected_coverage_gate_count;
         ++index) {
        const SelectedCoverageGateV1& selected =
            frame->selected_coverage_gates[index];
        if (selected.param_id == param_id) {
            return selected.origin_root == &root;
        }
    }
    if (frame->selected_coverage_gate_count
            >= frame->selected_coverage_gates.size()) {
        frame->selected_coverage_gate_overflow = true;
        return false;
    }
    frame->selected_coverage_gates[
        frame->selected_coverage_gate_count++] = SelectedCoverageGateV1{
            .origin_root = &root,
            .param_id = param_id,
        };
    for (PlannerInfo* current = &root;
         current != nullptr;
         current = current->parent_root) {
        if (current->glob != root.glob) {
            frame->selected_coverage_gate_overflow = true;
            return false;
        }
        current->outer_params = bms_add_member(
            current->outer_params, param_id);
    }
    return true;
}

/** Re-union nested selected gates after all outer-Param identification. */
[[nodiscard]] bool finalize_selected_coverage_gates(
        PlannerInfo& root) {
    PlannerFrame* frame = g_planner_frame;
    if (frame == nullptr || root.glob == nullptr
        || frame->selected_coverage_gate_overflow) {
        return false;
    }
    if (root.parent_root != nullptr) {
        return true;
    }
    for (std::size_t index = 0;
         index < frame->selected_coverage_gate_count;
         ++index) {
        const SelectedCoverageGateV1& selected =
            frame->selected_coverage_gates[index];
        bool reached_top = false;
        for (PlannerInfo* current = selected.origin_root;
             current != nullptr;
             current = current->parent_root) {
            if (current->glob != root.glob) {
                return false;
            }
            current->outer_params = bms_add_member(
                current->outer_params, selected.param_id);
            if (current == &root) {
                reached_top = true;
            }
        }
        if (!reached_top) {
            return false;
        }
    }
    return true;
}

struct ReusableCoverageState;
struct BioseqCacheState;

inline constexpr std::size_t kMaximumExecutorStartProducerClaimsV1 = 256;

/** Query-context producers claimed while one chained ExecutorStart runs. */
struct ExecutorStartClaimFrameV1 {
    ExecutorStartClaimFrameV1* previous = nullptr;
    std::array<ReusableCoverageState*, kMaximumExecutorStartProducerClaimsV1>
        coverage_producers{};
    std::size_t coverage_producer_count = 0;
    std::array<BioseqCacheState*, kMaximumExecutorStartProducerClaimsV1>
        exact_producers{};
    std::size_t exact_producer_count = 0;
};

inline ExecutorStartClaimFrameV1* g_executor_start_claim_frame = nullptr;

[[nodiscard]] RelationEpochSlot* ensure_relation_epoch_locked(
    Oid relation_oid) noexcept;

/** Initialize all shared atomics exactly once. */
void initialize_shared_state(CacheSharedState& shared) noexcept {
    std::memset(&shared, 0, sizeof(shared));
    shared.magic = kSharedMagic;
    shared.shared_abi_version = kSharedAbiVersion;
    pg_atomic_init_u32(&shared.ready, 1);
    pg_atomic_init_u32(&shared.bootstrap_failure, 0);
    const std::uint64_t nonce =
        static_cast<std::uint64_t>(GetCurrentTimestamp())
        ^ (static_cast<std::uint64_t>(MyProcPid) << 32U);
    pg_atomic_init_u64(&shared.boot_nonce, nonce == 0 ? 1 : nonce);
    pg_atomic_init_u64(&shared.database_write_epoch, 1);
    pg_atomic_init_u64(&shared.coarse_dependency_epoch, 1);
    pg_atomic_init_u32(
        &shared.last_committed_write_xid, InvalidTransactionId);
    pg_atomic_init_u32(&shared.database_transition_count, 0);
    pg_atomic_init_u32(&shared.coarse_transition_count, 0);
    pg_atomic_init_u64(&shared.next_claim_sequence, 1);
    pg_atomic_init_u64(&shared.next_registry_waiter_sequence, 1);
    pg_atomic_init_u64(&shared.next_registry_reader_sequence, 1);
    ConditionVariableInit(&shared.database_changed);
    pg_atomic_init_u64(&shared.stats.produced, 0);
    pg_atomic_init_u64(&shared.stats.completed_hits, 0);
    pg_atomic_init_u64(&shared.stats.inflight_waits, 0);
    pg_atomic_init_u64(&shared.stats.reuse_hits, 0);
    pg_atomic_init_u64(&shared.stats.fallbacks, 0);
    pg_atomic_init_u64(&shared.stats.invalidations, 0);
    pg_atomic_init_u64(&shared.stats.evictions, 0);
    pg_atomic_init_u64(&shared.stats.logical_bytes, 0);
    pg_atomic_init_u64(&shared.stats.stored_bytes, 0);
    pg_atomic_init_u64(&shared.stats.planner_build_generation, 0);
#if defined(BIOSEQ_CACHE_TESTING)
    pg_atomic_init_u32(&shared.test_control.armed_mask, 0);
    pg_atomic_init_u32(&shared.test_control.paused_mask, 0);
    pg_atomic_init_u32(&shared.test_control.released_mask, 0);
    pg_atomic_init_u32(&shared.test_control.fail_next_write, 0);
    pg_atomic_init_u64(&shared.test_control.exact_child_executions, 0);
    for (CacheBackendPauseControlV1& pause
         : shared.test_control.backend_pauses) {
        pg_atomic_init_u32(
            &pause.state,
            static_cast<std::uint32_t>(BackendPauseStateV1::Free));
        pause.point = 0;
        pause.target_pid = 0;
        pause.reserved = 0;
    }
    ConditionVariableInit(&shared.test_control.changed);
    pg_atomic_init_u64(&shared.next_injected_generation, 1);
#endif
    for (RelationEpochSlot& dependency : shared.relation_epochs) {
        ConditionVariableInit(&dependency.changed);
    }
    for (CacheEntry& entry : shared.entries) {
        entry.state = SharedEntryState::Empty;
        ConditionVariableInit(&entry.changed);
    }
}

/** Remember a possible result-changing write until its transaction outcome. */
void mark_current_transaction_dirty() noexcept {
    if (IsTransactionState()) {
        bind_reusable_registry_for_write_invalidation();
        g_dirty_observations.observe_coarse(
            static_cast<std::uint32_t>(GetCurrentSubTransactionId()));
        observe_runtime_database_dependency_write();
    }
}

enum class RelationPhysicalIdentityStatusV1 : std::uint8_t {
    Unknown = 0,
    StorageParent = 1,
    Precise = 2,
};

inline constexpr std::size_t kPgRelFileLocatorIdentityBytesV1 =
    sizeof(Oid) + sizeof(Oid) + sizeof(RelFileNumber);

/** Encode PostgreSQL's full relfile locator exactly, without digest aliases. */
[[nodiscard]] RelationPhysicalIdentityStatusV1
relation_physical_identity(
        Oid relation_oid, PhysicalIdentityV1* destination) noexcept {
    if (!OidIsValid(relation_oid) || destination == nullptr) {
        return RelationPhysicalIdentityStatusV1::Unknown;
    }
    Relation relation = RelationIdGetRelation(relation_oid);
    if (!RelationIsValid(relation)) {
        return RelationPhysicalIdentityStatusV1::Unknown;
    }
    const bool storage_parent = relation->rd_rel->relkind
        == RELKIND_PARTITIONED_TABLE;
    const RelFileLocator locator = relation->rd_locator;
    RelationClose(relation);
    if (storage_parent) {
        return RelationPhysicalIdentityStatusV1::StorageParent;
    }
    if (!OidIsValid(locator.dbOid) || !OidIsValid(locator.spcOid)
        || !RelFileNumberIsValid(locator.relNumber)
        || kPgRelFileLocatorIdentityBytesV1
            > destination->bytes.size()) {
        return RelationPhysicalIdentityStatusV1::Unknown;
    }
    *destination = PhysicalIdentityV1{
        .kind = PhysicalIdentityKind::PgRelFileLocator,
        .length = static_cast<std::uint32_t>(
            kPgRelFileLocatorIdentityBytesV1),
    };
    std::size_t offset = 0;
    std::memcpy(
        destination->bytes.data() + offset,
        &locator.spcOid,
        sizeof(locator.spcOid));
    offset += sizeof(locator.spcOid);
    std::memcpy(
        destination->bytes.data() + offset,
        &locator.dbOid,
        sizeof(locator.dbOid));
    offset += sizeof(locator.dbOid);
    std::memcpy(
        destination->bytes.data() + offset,
        &locator.relNumber,
        sizeof(locator.relNumber));
    return RelationPhysicalIdentityStatusV1::Precise;
}

[[nodiscard]] bool copy_provider_physical_identity(
        const provider_registry::PhysicalIdentityV2* source,
        PhysicalIdentityV1* destination) noexcept {
    if (source == nullptr || destination == nullptr
        || source->struct_size < sizeof(*source) || source->reserved != 0
        || source->length == 0 || source->length > sizeof(source->bytes)
        || (source->kind
                != static_cast<std::uint32_t>(
                    PhysicalIdentityKind::PgRelFileLocator)
            && source->kind
                != static_cast<std::uint32_t>(
                    PhysicalIdentityKind::ProviderOpaque))) {
        return false;
    }
    if (source->kind == static_cast<std::uint32_t>(
            PhysicalIdentityKind::PgRelFileLocator)
        && source->length != kPgRelFileLocatorIdentityBytesV1) {
        return false;
    }
    for (std::size_t index = source->length;
         index < sizeof(source->bytes); ++index) {
        if (source->bytes[index] != 0) {
            return false;
        }
    }
    *destination = PhysicalIdentityV1{
        .kind = static_cast<PhysicalIdentityKind>(source->kind),
        .length = source->length,
    };
    std::memcpy(
        destination->bytes.data(), source->bytes, source->length);
    return true;
}

/** Record one relation/physical write, falling back when the set is full. */
void mark_current_transaction_relation_dirty(
        Oid relation_oid,
        const provider_registry::PhysicalIdentityV2* provider_identity = nullptr) {
    if (!IsTransactionState()) {
        return;
    }
    bind_reusable_registry_for_write_invalidation();
    if (g_dirty_observations.coarse()) {
        return;
    }
    if (!OidIsValid(relation_oid)) {
        mark_current_transaction_dirty();
        return;
    }
    PhysicalIdentityV1 physical_identity{};
    RelationPhysicalIdentityStatusV1 identity_status =
        RelationPhysicalIdentityStatusV1::Unknown;
    if (copy_provider_physical_identity(
            provider_identity, &physical_identity)) {
        identity_status = RelationPhysicalIdentityStatusV1::Precise;
    } else if (provider_identity == nullptr) {
        identity_status = relation_physical_identity(
            relation_oid, &physical_identity);
    }
    const bool physical_identity_valid = identity_status
        == RelationPhysicalIdentityStatusV1::Precise;
    const bool storage_parent = identity_status
        == RelationPhysicalIdentityStatusV1::StorageParent;
    const DirtyRelationObservationV1 observation{
        .relation_oid = relation_oid,
        .physical_identity_valid = physical_identity_valid,
        .storage_parent = storage_parent,
        .physical_identity = physical_identity,
        .observability = provider_identity == nullptr
            ? WriteObservability::ExecutorUtilityOnly
            : WriteObservability::ProviderComplete,
    };
    const WriteObservationResultV1 observed = g_dirty_observations.observe(
        static_cast<std::uint32_t>(GetCurrentSubTransactionId()),
        observation);
    if (observed == WriteObservationResultV1::CoarseFallback) {
        observe_runtime_database_dependency_write();
        return;
    }
    observe_runtime_database_dependency_write();
    if (provider_identity == nullptr) {
        observe_runtime_relation_write(observation);
    } else if (physical_identity_valid) {
        observe_runtime_provider_relation_write(
            relation_oid, physical_identity);
    }
    bool provisioned = false;
    if (g_shared != nullptr && g_registry_lock != nullptr) {
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        provisioned = ensure_relation_epoch_locked(relation_oid) != nullptr;
        LWLockRelease(g_registry_lock);
    }
    if (!provisioned) {
        g_dirty_observations.observe_coarse(
            static_cast<std::uint32_t>(GetCurrentSubTransactionId()));
    }
}

/** Provider write observations join the same savepoint-aware epoch ledger. */
void observe_provider_relation_write(
        std::uint32_t database_oid, std::uint32_t relation_oid,
        const provider_registry::PhysicalIdentityV2* physical_identity) noexcept {
    if (!IsTransactionState() || g_dirty_observations.coarse()) {
        return;
    }
    PhysicalIdentityV1 validated_identity{};
    if (database_oid != static_cast<std::uint32_t>(MyDatabaseId)
        || !OidIsValid(static_cast<Oid>(relation_oid))
        || !copy_provider_physical_identity(
            physical_identity, &validated_identity)) {
        mark_current_transaction_dirty();
        return;
    }
    mark_current_transaction_relation_dirty(
        static_cast<Oid>(relation_oid), physical_identity);
}

/** Implemented after the reusable-registry lock adapters are available. */
void consider_provider_reusable_paths(
    PlannerInfo*, RelOptInfo*, unsigned int, RangeTblEntry*,
    const provider_registry::ProviderV2*) noexcept;

/** Provider-owned path hook rendezvous; every optional failure is fail-open. */
void observe_provider_rel_paths(
        PlannerInfo* root, RelOptInfo* relation,
        unsigned int range_table_index, RangeTblEntry* range_entry,
        const provider_registry::ProviderV2* provider) noexcept {
    if (guc::mode() != QueryCacheMode::Reuse
        || !g_reusable_registry) {
        return;
    }
    consider_provider_reusable_paths(
        root, relation, range_table_index, range_entry, provider);
}

/** True when at least one non-aborted savepoint performed a write. */
[[nodiscard]] bool transaction_is_dirty() noexcept {
    return g_dirty_observations.transaction_dirty();
}

/** Drop all backend-local transaction frames without retaining stale pointers. */
void clear_dirty_frames() noexcept {
    g_dirty_observations.reset();
    g_write_invalidation_deferred_to_emergency = false;
}

/** Remove one subtransaction frame and optionally merge it into its parent. */
void finish_dirty_subtransaction(
        SubTransactionId child_id, SubTransactionId parent_id,
        bool committed) noexcept {
    static_cast<void>(g_dirty_observations.finish_subtransaction(
        static_cast<std::uint32_t>(child_id),
        static_cast<std::uint32_t>(parent_id),
        committed));
}

[[nodiscard]] constexpr std::size_t relation_epoch_start(
        Oid database_oid, Oid relation_oid) noexcept {
    const std::uint64_t mixed =
        static_cast<std::uint64_t>(database_oid) * 0x9e3779b185ebca87ULL
        ^ static_cast<std::uint64_t>(relation_oid) * 0xc2b2ae3d27d4eb4fULL;
    return static_cast<std::size_t>(mixed % kRelationEpochCapacity);
}

/** Find one provisioned dependency while the registry lock is held. */
[[nodiscard]] RelationEpochSlot* find_relation_epoch_locked(
        Oid relation_oid) noexcept {
    const std::size_t start = relation_epoch_start(
        MyDatabaseId, relation_oid);
    for (std::size_t probe = 0; probe < kRelationEpochCapacity; ++probe) {
        RelationEpochSlot& slot =
            g_shared->relation_epochs[(start + probe) % kRelationEpochCapacity];
        if (!slot.used) {
            return nullptr;
        }
        if (slot.database_oid == MyDatabaseId
            && slot.relation_oid == relation_oid) {
            return &slot;
        }
    }
    return nullptr;
}

/** Provision one fixed dependency slot while allocation and ERROR are legal. */
[[nodiscard]] RelationEpochSlot* ensure_relation_epoch_locked(
        Oid relation_oid) noexcept {
    const std::size_t start = relation_epoch_start(
        MyDatabaseId, relation_oid);
    for (std::size_t probe = 0; probe < kRelationEpochCapacity; ++probe) {
        RelationEpochSlot& slot =
            g_shared->relation_epochs[(start + probe) % kRelationEpochCapacity];
        if (!slot.used) {
            slot.database_oid = MyDatabaseId;
            slot.relation_oid = relation_oid;
            slot.committed_epoch = 1;
            slot.transition_count = 0;
            slot.used = true;
            return &slot;
        }
        if (slot.database_oid == MyDatabaseId
            && slot.relation_oid == relation_oid) {
            return &slot;
        }
    }
    return nullptr;
}

/** Read the current precise epoch; unseen relations begin at epoch one. */
[[nodiscard]] std::uint64_t relation_dependency_epoch(Oid relation_oid) noexcept {
    if (g_shared == nullptr || !OidIsValid(relation_oid)) {
        return 0;
    }
    std::uint64_t epoch = 1;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    const std::size_t start = relation_epoch_start(MyDatabaseId, relation_oid);
    for (std::size_t probe = 0; probe < kRelationEpochCapacity; ++probe) {
        const RelationEpochSlot& slot =
            g_shared->relation_epochs[(start + probe) % kRelationEpochCapacity];
        if (!slot.used) {
            break;
        }
        if (slot.database_oid == MyDatabaseId
            && slot.relation_oid == relation_oid) {
            epoch = slot.committed_epoch;
            break;
        }
    }
    LWLockRelease(g_registry_lock);
    return epoch;
}

/** Canonical conservative dependency used by every exact write bracket. */
[[nodiscard]] DependencyVersion database_write_dependency_version() noexcept {
    if (g_shared == nullptr || !OidIsValid(MyDatabaseId)) {
        return {};
    }
    return DependencyVersion{
        .key = {
            .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
            .object_oid = static_cast<std::uint32_t>(MyDatabaseId),
            .kind = DependencyKind::DatabaseWriteEpoch,
        },
        .committed_epoch = pg_atomic_read_u64(
            &g_shared->database_write_epoch),
        .observability = WriteObservability::DatabaseWideFallback,
    };
}

/** Convert one precise/topology observation into its directory capability. */
[[nodiscard]] DependencyVersion relation_dependency_version(
        const DirtyRelationObservationV1& observation) noexcept {
    if (!OidIsValid(observation.relation_oid)) {
        return {};
    }
    return DependencyVersion{
        .key = {
            .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
            .class_oid = static_cast<std::uint32_t>(RelationRelationId),
            .object_oid = static_cast<std::uint32_t>(
                observation.relation_oid),
            .object_sub_id = observation.observability
                    == WriteObservability::ProviderComplete
                ? 0 : -1,
            .kind = observation.storage_parent
                ? DependencyKind::RelationTopology
                : DependencyKind::RelationContent,
        },
        .committed_epoch = relation_dependency_epoch(
            observation.relation_oid),
        .physical_identity = observation.physical_identity_valid
            ? observation.physical_identity
            : PhysicalIdentityV1{},
        .observability = observation.observability,
    };
}

/** Provision the conservative owner capability during normal execution. */
void observe_runtime_database_dependency_write() noexcept {
    const dependency_runtime::DependencyRuntimeResultV1 result =
        dependency_runtime::observe_dirty_dependency(
            database_write_dependency_version());
    if (!result) {
        dependency_runtime::force_emergency_fallback();
    }
}

/** Ordinary executor/utility writes advance their exact -1 relation key. */
void observe_runtime_relation_write(
        const DirtyRelationObservationV1& observation) noexcept {
    const DependencyVersion version = relation_dependency_version(observation);
    const dependency_runtime::DependencyRuntimeResultV1 result =
        observation.physical_identity_valid
        ? dependency_runtime::observe_dirty_dependency(
            version, observation.physical_identity)
        : dependency_runtime::observe_dirty_dependency(version);
    if (!result) {
        dependency_runtime::force_emergency_fallback();
    }
}

/** Provider callbacks establish precise relation ownership before PRE_COMMIT. */
void observe_runtime_provider_relation_write(
        Oid relation_oid,
        const PhysicalIdentityV1& physical_identity) noexcept {
    const DirtyRelationObservationV1 observation{
        .relation_oid = relation_oid,
        .physical_identity_valid = true,
        .storage_parent = false,
        .physical_identity = physical_identity,
        .observability = WriteObservability::ProviderComplete,
    };
    const dependency_runtime::DependencyRuntimeResultV1 result =
        dependency_runtime::observe_dirty_dependency(
            relation_dependency_version(observation), physical_identity);
    if (!result) {
        dependency_runtime::force_emergency_fallback();
    }
}

/** Advance one precise dependency under the registry lock; false means full. */
[[nodiscard]] bool bump_relation_epoch_locked(Oid relation_oid) noexcept {
    RelationEpochSlot* slot = ensure_relation_epoch_locked(relation_oid);
    if (slot == nullptr) {
        return false;
    }
    ++slot->committed_epoch;
    if (slot->committed_epoch == 0) {
        slot->committed_epoch = 1;
    }
    return true;
}

/** Finalize this backend's precise transition prefix without allocation. */
[[nodiscard]] bool finish_dirty_dependency_transitions(
        bool committed) noexcept {
    bool coarse = g_force_coarse_transition || g_dirty_observations.coarse();
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t index = 0;
         index < g_transition_relation_count; ++index) {
        RelationEpochSlot* slot = find_relation_epoch_locked(
            g_transition_relation_oids[index]);
        if (slot == nullptr || slot->transition_count == 0) {
            coarse = true;
            continue;
        }
        if (committed) {
            ++slot->committed_epoch;
            if (slot->committed_epoch == 0) {
                slot->committed_epoch = 1;
            }
        }
        --slot->transition_count;
        ConditionVariableBroadcast(&slot->changed);
    }
    LWLockRelease(g_registry_lock);
    return coarse;
}

/** Defined after reusable lock/CV adapters; called only during commit cleanup. */
void invalidate_dirty_reusable_generations() noexcept;

/** Announce a commit transition so concurrent captures cannot race visibility. */
void begin_database_transition(
        DatabaseTransitionPurposeV1 purpose =
            DatabaseTransitionPurposeV1::TopLevelWrite) noexcept {
    if (g_database_transition_phase != DatabaseTransitionPhaseV1::Idle
        || g_database_transition_published || g_shared == nullptr) {
        return;
    }
    g_database_transition_owner_token =
        (static_cast<std::uint64_t>(static_cast<std::uint32_t>(MyProcPid))
            << 32U)
        | (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(MyProcNumber)) + 1U);
    if (g_database_transition_owner_token == 0) {
        g_database_transition_owner_token = 1;
    }
    g_database_transition_purpose = purpose;
    g_database_transition_phase = DatabaseTransitionPhaseV1::Publishing;
    pg_atomic_fetch_add_u32(&g_shared->database_transition_count, 1);
    g_database_transition_published = true;
    g_database_transition_xid = GetTopTransactionIdIfAny();
    g_transition_relation_count = 0;
    bool coarse = g_force_coarse_transition || g_dirty_observations.coarse();
    std::array<DirtyRelationObservationV1,
        kMaximumRelationDependencies> observations{};
    const std::size_t observation_count =
        g_dirty_observations.copy_unique(observations);
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t index = 0; index < observation_count; ++index) {
        if (!observations[index].physical_identity_valid
            || observations[index].storage_parent) {
            coarse = true;
        }
        const Oid relation_oid = observations[index].relation_oid;
        bool duplicate = false;
        for (std::size_t prior = 0;
             prior < g_transition_relation_count; ++prior) {
            duplicate = duplicate
                || g_transition_relation_oids[prior] == relation_oid;
        }
        if (duplicate) {
            continue;
        }
        RelationEpochSlot* slot = find_relation_epoch_locked(relation_oid);
        if (slot == nullptr
            || g_transition_relation_count
                >= g_transition_relation_oids.size()
            || slot->transition_count
                == (std::numeric_limits<std::uint32_t>::max)()) {
            coarse = true;
            continue;
        }
        ++slot->transition_count;
        g_transition_relation_oids[g_transition_relation_count++] =
            relation_oid;
        ConditionVariableBroadcast(&slot->changed);
    }
    LWLockRelease(g_registry_lock);
    if (coarse) {
        pg_atomic_fetch_add_u32(&g_shared->coarse_transition_count, 1);
        g_coarse_transition_published = true;
    }
    g_database_transition_phase = DatabaseTransitionPhaseV1::Published;
    ConditionVariableBroadcast(&g_shared->database_changed);
}

/** Complete or abandon an announced transition in visibility-safe order. */
void finish_database_transition(bool committed) noexcept {
    if (g_database_transition_owner_token == 0
        || g_database_transition_phase == DatabaseTransitionPhaseV1::Idle
        || !g_database_transition_published || g_shared == nullptr) {
        g_database_transition_published = false;
        g_coarse_transition_published = false;
        g_transition_relation_count = 0;
        g_database_transition_xid = InvalidTransactionId;
        g_force_coarse_transition = false;
        g_database_transition_purpose = DatabaseTransitionPurposeV1::None;
        g_database_transition_phase = DatabaseTransitionPhaseV1::Idle;
        g_database_transition_owner_token = 0;
        return;
    }
    g_database_transition_phase = DatabaseTransitionPhaseV1::Finishing;
    const bool coarse = finish_dirty_dependency_transitions(committed);
    if (committed) {
        if (coarse || g_coarse_transition_published) {
            pg_atomic_fetch_add_u64(
                &g_shared->coarse_dependency_epoch, 1);
        }
        if (committed && TransactionIdIsValid(g_database_transition_xid)) {
            pg_atomic_write_u32(
                &g_shared->last_committed_write_xid,
                g_database_transition_xid);
        }
        pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
        pg_atomic_fetch_add_u64(&g_shared->stats.invalidations, 1);
    }
    if (committed) {
        invalidate_dirty_reusable_generations();
    }
    if (g_coarse_transition_published) {
        pg_atomic_fetch_sub_u32(&g_shared->coarse_transition_count, 1);
    }
    pg_atomic_fetch_sub_u32(&g_shared->database_transition_count, 1);
    g_database_transition_published = false;
    g_coarse_transition_published = false;
    g_transition_relation_count = 0;
    g_database_transition_xid = InvalidTransactionId;
    g_force_coarse_transition = false;
    g_database_transition_purpose = DatabaseTransitionPurposeV1::None;
    g_database_transition_phase = DatabaseTransitionPhaseV1::Idle;
    g_database_transition_owner_token = 0;
    ConditionVariableBroadcast(&g_shared->database_changed);
}

/** Release this backend's shared transition ownership before shmem detach. */
void cache_backend_transition_exit(int, Datum) {
    if (g_database_transition_owner_token == 0
        || g_database_transition_phase == DatabaseTransitionPhaseV1::Idle) {
        return;
    }
    const bool possibly_committed = g_database_transition_purpose
            == DatabaseTransitionPurposeV1::CommitPreparedResolution
        || g_database_transition_purpose
            == DatabaseTransitionPurposeV1::TopLevelWrite
        || g_database_transition_purpose
            == DatabaseTransitionPurposeV1::ImmediateWrite;
    if (possibly_committed) {
        g_force_coarse_transition = true;
    }
    finish_database_transition(possibly_committed);
    clear_dirty_frames();
}

/** Publish write epochs only for successful top-level outcomes. */
void cache_transaction_callback(XactEvent event, void*) {
    switch (event) {
        case XACT_EVENT_PRE_COMMIT:
            if (transaction_is_dirty()) {
                begin_database_transition(
                    DatabaseTransitionPurposeV1::TopLevelWrite);
            }
            break;
        case XACT_EVENT_PRE_PREPARE:
            if (transaction_is_dirty()) {
                begin_database_transition(
                    DatabaseTransitionPurposeV1::PrepareHandoff);
            }
            break;
        case XACT_EVENT_COMMIT:
        case XACT_EVENT_PREPARE:
            finish_database_transition(true);
            clear_dirty_frames();
            break;
        case XACT_EVENT_ABORT:
            finish_database_transition(false);
            clear_dirty_frames();
            break;
        case XACT_EVENT_PARALLEL_COMMIT:
        case XACT_EVENT_PARALLEL_ABORT:
            /* Only the leader publishes the database-wide transition. */
            clear_dirty_frames();
            break;
        case XACT_EVENT_PARALLEL_PRE_COMMIT:
            break;
    }
}

/** Savepoint rollback discards dirty state; release merges it upward. */
void cache_subtransaction_callback(
        SubXactEvent event, SubTransactionId child_id,
        SubTransactionId parent_id, void*) {
    switch (event) {
        case SUBXACT_EVENT_COMMIT_SUB:
            finish_dirty_subtransaction(child_id, parent_id, true);
            break;
        case SUBXACT_EVENT_ABORT_SUB:
            finish_dirty_subtransaction(child_id, parent_id, false);
            break;
        case SUBXACT_EVENT_START_SUB:
        case SUBXACT_EVENT_PRE_COMMIT_SUB:
            break;
    }
}

[[nodiscard]] constexpr ReusableRegistryConfigV1
reusable_registry_config() noexcept {
    return ReusableRegistryConfigV1{};
}

void acquire_reusable_exact_partition(
        void*, std::uint32_t partition) noexcept {
    LWLockAcquire(&g_reusable_exact_locks[partition].lock, LW_EXCLUSIVE);
}

void release_reusable_exact_partition(
        void*, std::uint32_t partition) noexcept {
    LWLockRelease(&g_reusable_exact_locks[partition].lock);
}

void acquire_reusable_coverage_partition(
        void*, std::uint32_t partition) noexcept {
    LWLockAcquire(&g_reusable_coverage_locks[partition].lock, LW_EXCLUSIVE);
}

void release_reusable_coverage_partition(
        void*, std::uint32_t partition) noexcept {
    LWLockRelease(&g_reusable_coverage_locks[partition].lock);
}

[[nodiscard]] ReusableRegistryPartitionCallbacksV1
reusable_partition_callbacks() noexcept {
    return ReusableRegistryPartitionCallbacksV1{
        .acquire_exact = acquire_reusable_exact_partition,
        .release_exact = release_reusable_exact_partition,
        .acquire_coverage = acquire_reusable_coverage_partition,
        .release_coverage = release_reusable_coverage_partition,
    };
}

/**
 * The bridge definition remains before the implementation so provider hooks
 * can call it without exposing planner internals through the rendezvous ABI.
 */
void consider_provider_reusable_paths_impl(
    PlannerInfo*, RelOptInfo*, unsigned int, RangeTblEntry*,
    const provider_registry::ProviderV2*) noexcept;

void consider_provider_reusable_paths(
        PlannerInfo* root, RelOptInfo* relation,
        unsigned int range_table_index, RangeTblEntry* range_entry,
        const provider_registry::ProviderV2* provider) noexcept {
    consider_provider_reusable_paths_impl(
        root, relation, range_table_index, range_entry, provider);
}

[[nodiscard]] std::uint64_t reusable_monotonic_now_ns() noexcept {
    instr_time current{};
    INSTR_TIME_SET_CURRENT(current);
    return INSTR_TIME_GET_NANOSEC(current);
}

[[nodiscard]] constexpr std::uint64_t reusable_backend_nonce(
        std::uint32_t proc_number,
        int pid,
        TimestampTz started_at) noexcept {
    const std::uint64_t nonce = static_cast<std::uint64_t>(started_at)
        ^ (static_cast<std::uint64_t>(pid) << 32U)
        ^ static_cast<std::uint64_t>(proc_number);
    return nonce == 0 ? 1 : nonce;
}

[[nodiscard]] ReusableProducerIdentityV1
current_reusable_producer_identity() noexcept {
    const std::uint32_t proc_number =
        static_cast<std::uint32_t>(MyProcNumber);
    return ReusableProducerIdentityV1{
        .proc_number = proc_number,
        .pid = MyProcPid,
        .backend_nonce = reusable_backend_nonce(
            proc_number, MyProcPid, MyStartTimestamp),
        .backend_started_at_us = MyStartTimestamp,
    };
}

[[nodiscard]] ReusableManifestHandleV1 reusable_manifest_handle(
        const storage::ManifestHandle& handle) noexcept {
    return ReusableManifestHandleV1{
        .slot = handle.slot,
        .reserved = handle.reserved,
        .slot_sequence = handle.slot_sequence,
        .id = handle.id,
    };
}

[[nodiscard]] storage::ManifestHandle storage_manifest_handle(
        const ReusableManifestHandleV1& handle) noexcept {
    return storage::ManifestHandle{
        .slot = handle.slot,
        .reserved = handle.reserved,
        .slot_sequence = handle.slot_sequence,
        .id = handle.id,
    };
}

[[nodiscard]] bool reusable_producer_alive(
        void*, const ReusableProducerIdentityV1& producer) noexcept {
    if (ProcGlobal == nullptr || !producer.valid()
        || producer.backend_started_at_us == 0
        || reusable_backend_nonce(
            producer.proc_number,
            producer.pid,
            producer.backend_started_at_us) != producer.backend_nonce
        || producer.proc_number
            >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    int process_pid = 0;
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    PGPROC* process = ProcNumberGetProc(
        static_cast<ProcNumber>(producer.proc_number));
    if (process != nullptr) {
        process_pid = process->pid;
    }
    LWLockRelease(ProcArrayLock);
    if (process_pid != producer.pid) {
        return false;
    }
    volatile PgBackendStatus* status = pgstat_get_beentry_by_proc_number(
        static_cast<ProcNumber>(producer.proc_number));
    if (status == nullptr) {
        return true;
    }
    int status_pid = 0;
    TimestampTz status_started_at = 0;
    for (;;) {
        int before_changecount = 0;
        int after_changecount = 0;
        pgstat_begin_read_activity(status, before_changecount);
        status_pid = status->st_procpid;
        status_started_at = status->st_proc_start_timestamp;
        pgstat_end_read_activity(status, after_changecount);
        if (pgstat_read_activity_complete(
                before_changecount, after_changecount)) {
            break;
        }
        CHECK_FOR_INTERRUPTS();
    }
    if (status_pid != producer.pid) {
        return true;
    }
    return status_started_at == producer.backend_started_at_us
        && reusable_backend_nonce(
            producer.proc_number, producer.pid, status_started_at)
            == producer.backend_nonce;
}

inline constexpr std::size_t kMaximumExactLivenessIdentitiesV1 =
    kRegistryCapacity * (storage::kReaderPinsPerManifest
        + storage::kWaitersPerManifest + 1U);

struct BackendIdentityBatchV1 {
    std::array<ReusableProducerIdentityV1,
        kMaximumExactLivenessIdentitiesV1> identities{};
    std::size_t count = 0;
    bool complete = true;
};

struct BackendLivenessEntryV1 {
    ReusableProducerIdentityV1 identity{};
    bool alive = true;
};

struct BackendLivenessSampleV1 {
    std::array<BackendLivenessEntryV1,
        kMaximumExactLivenessIdentitiesV1> entries{};
    std::size_t count = 0;
};

[[nodiscard]] bool backend_identity_alive_in_sample(
        const BackendLivenessSampleV1& sample,
        const ReusableProducerIdentityV1& identity) noexcept {
    if (!identity.valid()) {
        return true;
    }
    for (std::size_t index = 0; index < sample.count; ++index) {
        if (sample.entries[index].identity == identity) {
            return sample.entries[index].alive;
        }
    }
    return true;
}

void append_backend_identity(
        BackendIdentityBatchV1& batch,
        const ReusableProducerIdentityV1& identity) noexcept {
    if (!identity.valid()) {
        return;
    }
    if (batch.count >= batch.identities.size()) {
        batch.complete = false;
        return;
    }
    batch.identities[batch.count++] = identity;
}

/** Deduplicate and probe only after every cache lock has been released. */
void probe_backend_liveness(
        const BackendIdentityBatchV1& batch,
        BackendLivenessSampleV1& sample) noexcept {
    if (!batch.complete) {
        return;
    }
    for (std::size_t index = 0; index < batch.count; ++index) {
        const ReusableProducerIdentityV1& identity = batch.identities[index];
        bool duplicate = false;
        for (std::size_t prior = 0; prior < sample.count; ++prior) {
            duplicate = duplicate
                || sample.entries[prior].identity == identity;
        }
        if (duplicate || sample.count >= sample.entries.size()) {
            continue;
        }
        sample.entries[sample.count++] = BackendLivenessEntryV1{
            .identity = identity,
            .alive = reusable_producer_alive(nullptr, identity),
        };
    }
}

struct ReusableOwnerLivenessSampleV1 {
    const ReusableJanitorOwnerObservationV1* observations = nullptr;
    const bool* alive = nullptr;
    std::size_t count = 0;
    bool complete = false;
};

struct ReusableJanitorLivenessWorkspaceV1 {
    std::array<ReusableJanitorOwnerObservationV1,
        kMaximumReusableJanitorLivenessObservationsV1> observations{};
    std::array<bool, kMaximumReusableJanitorLivenessObservationsV1> alive{};
    std::array<std::byte,
        dependency_vector_envelope_bytes(kMaxDependenciesPerGeneration)>
        dependency_vector{};
};

/** Cache-lock-safe lookup; an unsampled or changed observation stays live. */
[[nodiscard]] bool sampled_reusable_owner_alive(
        void* opaque,
        const ReusableJanitorOwnerObservationV1& observation) noexcept {
    const auto* sample = static_cast<const ReusableOwnerLivenessSampleV1*>(
        opaque);
    if (sample == nullptr || !sample->complete || !observation.valid()) {
        return true;
    }
    for (std::size_t index = 0; index < sample->count; ++index) {
        if (sample->observations[index] == observation) {
            return sample->alive[index];
        }
    }
    return true;
}

/** Deduplicate backend probes while retaining exact owner ABA observations. */
void probe_reusable_owner_liveness(
        std::span<const ReusableJanitorOwnerObservationV1> observations,
        std::span<bool> alive) noexcept {
    if (alive.size() < observations.size()) {
        return;
    }
    for (std::size_t index = 0; index < observations.size(); ++index) {
        alive[index] = true;
        if (!observations[index].valid()) {
            continue;
        }
        bool duplicate = false;
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (observations[prior].owner == observations[index].owner) {
                alive[index] = alive[prior];
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            alive[index] = reusable_producer_alive(
                nullptr, observations[index].owner);
        }
    }
}

/** Request the legacy exact header plus the single pinned-DSA shared root. */
void cache_shmem_request() {
    if (const auto previous =
            executor::lifecycle::previous_shmem_request();
        previous != nullptr) {
        previous();
    }
    RequestAddinShmemSpace(MAXALIGN(sizeof(CacheSharedState)));
    RequestNamedLWLockTranche(kRegistryTrancheName, 1);
    storage::request_storage_resources();
}

/** Attach or initialize the postmaster-lifetime cache header. */
void cache_shmem_startup() {
    if (const auto previous =
            executor::lifecycle::previous_shmem_startup();
        previous != nullptr) {
        previous();
    }

    bool found = false;
    g_shared = static_cast<CacheSharedState*>(
        ShmemInitStruct(kSharedName, sizeof(CacheSharedState), &found));
    if (!found) {
        initialize_shared_state(*g_shared);
    }
    g_registry_lock = &GetNamedLWLockTranche(kRegistryTrancheName)[0].lock;
    const std::uint64_t boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce);
    if (!storage::startup_storage(boot_nonce)) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    if (maintenance::static_janitor_registered()) {
        storage::publish_worker_lifecycle(
            storage::WorkerLifecycle::Registered, 0);
    }
    if (!storage::publish_correctness_readiness(
            CacheCorrectnessReadiness::ReusableCoverageReady)) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
    }
}

/** Inspect PostgreSQL's restart-reconstructed prepared-xact dummy procs. */
[[nodiscard]] bool database_has_prepared_xacts_locked(
        Oid database_oid) noexcept {
    if (max_prepared_xacts <= 0) {
        return false;
    }
    if (!OidIsValid(database_oid) || PreparedXactProcs == nullptr
        || ProcGlobal == nullptr || ProcGlobal->xids == nullptr) {
        return true;
    }
    const std::uint64_t dense_capacity =
        static_cast<std::uint64_t>(ProcGlobal->allProcCount)
        + static_cast<std::uint64_t>(max_prepared_xacts);
    for (int index = 0; index < max_prepared_xacts; ++index) {
        const PGPROC& prepared = PreparedXactProcs[index];
        const int offset = prepared.pgxactoff;
        if (prepared.databaseId == database_oid
            && TransactionIdIsValid(prepared.xid)
            && offset >= 0
            && static_cast<std::uint64_t>(offset) < dense_capacity
            && TransactionIdEquals(
                ProcGlobal->xids[offset], prepared.xid)) {
            return true;
        }
    }
    return false;
}

/** Dynamic 2PC gate survives restart and covers every prepared GID. */
[[nodiscard]] bool database_has_prepared_xacts(
        Oid database_oid) noexcept {
    if (max_prepared_xacts <= 0) {
        return false;
    }
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    const bool present = database_has_prepared_xacts_locked(database_oid);
    LWLockRelease(ProcArrayLock);
    return present;
}

/** Bind process-local PostgreSQL pointers from the one validated DSA root. */
[[nodiscard]] bool bind_shared_reusable_registry() noexcept {
    if (g_reusable_registry
        && g_reusable_membership_lock != nullptr
        && g_reusable_exact_locks != nullptr
        && g_reusable_coverage_locks != nullptr
        && g_reusable_wait_slots != nullptr
        && g_reusable_wait_slot_count != 0) {
        register_backend_query_cleanup_exit();
        return true;
    }
    if (!storage::storage_ready() && !storage::attach_storage()) {
        return false;
    }
    const shared_registry::SharedRegistryAttachmentV1* attachment =
        shared_registry::current_shared_registry_attachment();
    LWLockPadded* membership =
        shared_registry::shared_reusable_membership_lock();
    LWLockPadded* exact = shared_registry::shared_reusable_exact_locks();
    LWLockPadded* coverage =
        shared_registry::shared_reusable_coverage_locks();
    if (attachment == nullptr || !attachment->reusable
        || attachment->root == nullptr || membership == nullptr
        || exact == nullptr || coverage == nullptr
        || attachment->reusable_wait_cvs.empty()
        || attachment->reusable_wait_cvs.size()
            > (std::numeric_limits<std::uint32_t>::max)()
        || attachment->root->layout.reusable_max_generations
            != attachment->reusable_wait_cvs.size()
        || attachment->root->layout.reusable_wait_cv_capacity
            != attachment->reusable_wait_cvs.size()) {
        return false;
    }
    g_reusable_registry = attachment->reusable;
    g_reusable_membership_lock = &membership[0].lock;
    g_reusable_exact_locks = exact;
    g_reusable_coverage_locks = coverage;
    g_reusable_wait_slots = attachment->reusable_wait_cvs.data();
    g_reusable_wait_slot_count = static_cast<std::uint32_t>(
        attachment->reusable_wait_cvs.size());
    register_backend_query_cleanup_exit();
    return true;
}

/**
 * Administrative callers must not mistake an unbound process-local adapter
 * for an empty shared registry.  Fresh backends attach through the validated
 * shared root; once the cache header is ready, attachment failure is a
 * fail-closed administrative error rather than a silent zero-row result.
 */
[[nodiscard]] bool bind_admin_reusable_registry(const char* operation) {
    if (bind_shared_reusable_registry()) {
        return true;
    }
    const bool shared_registry_expected = guc::preload_requested()
        && g_shared != nullptr
        && g_shared->magic == kSharedMagic
        && g_shared->shared_abi_version == kSharedAbiVersion
        && pg_atomic_read_u32(&g_shared->ready) != 0
        && pg_atomic_read_u32(&g_shared->bootstrap_failure) == 0;
    if (shared_registry_expected) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("%s could not attach the bioseq_cache shared registry",
                    operation != nullptr ? operation : "bioseq_cache admin")));
    }
    return false;
}

/** True only when preload installed and the shared ABI is usable. */
[[nodiscard]] bool shared_cache_ready() noexcept {
    const bool header_ready = guc::preload_requested()
        && g_shared != nullptr
        && g_shared->magic == kSharedMagic
        && g_shared->shared_abi_version == kSharedAbiVersion
        && pg_atomic_read_u32(&g_shared->ready) != 0
        && pg_atomic_read_u32(&g_shared->bootstrap_failure) == 0;
    if (!header_ready) {
        return false;
    }
    if (!bind_shared_reusable_registry()) {
        return false;
    }
    const CacheCorrectnessReadiness required_readiness =
        guc::mode() == QueryCacheMode::Reuse
        ? CacheCorrectnessReadiness::ReusableCoverageReady
        : CacheCorrectnessReadiness::GenericMvccReady;
    return !database_has_prepared_xacts(MyDatabaseId)
        && storage::runtime_snapshot().correctness_readiness
            >= required_readiness
        && (required_readiness
                != CacheCorrectnessReadiness::ReusableCoverageReady
            || bind_shared_reusable_registry());
}

[[nodiscard]] bool cache_interrupt_error(int sqlstate) noexcept {
    return sqlstate == ERRCODE_QUERY_CANCELED
        || sqlstate == ERRCODE_ADMIN_SHUTDOWN
        || sqlstate == ERRCODE_CRASH_SHUTDOWN
        || ProcDiePending;
}

inline constexpr std::uint32_t kCoverageDescriptorMagicV1 = 0x42434431U;
inline constexpr std::uint32_t kCoverageDescriptorFormatV1 = 1;

/** Find one provider column by attno without trusting array position. */
[[nodiscard]] const BioseqCacheColumnSchemaV2* coverage_schema_column(
        const BioseqCacheColumnSchemaV2* columns,
        std::size_t column_count,
        std::uint32_t attno) noexcept {
    if (columns == nullptr || attno == 0) {
        return nullptr;
    }
    for (std::size_t index = 0; index < column_count; ++index) {
        if (columns[index].attno == attno) {
            return &columns[index];
        }
    }
    return nullptr;
}

enum class CoveragePredicateVarFormV1 : std::uint8_t {
    RawConsumer = 1,
    FragmentSentinel = 2,
};

struct CoveragePredicateContractV1 {
    const BioseqCachePathDomainV2* domain = nullptr;
    CoveragePredicateVarFormV1 var_form =
        CoveragePredicateVarFormV1::FragmentSentinel;
    Index raw_relation_index = 0;
    std::size_t var_count = 0;
    bool valid = true;
};

/** V1 admits only immutable built-in comparison operators. */
[[nodiscard]] bool coverage_comparison_operator_valid(
        const OpExpr& operation) noexcept {
    const Oid function_oid = get_opcode(operation.opno);
    return list_length(operation.args) == 2
        && !operation.opretset
        && operation.opresulttype == BOOLOID
        && OidIsValid(function_oid)
        && get_func_namespace(function_oid) == PG_CATALOG_NAMESPACE
        && func_volatile(function_oid) == PROVOLATILE_IMMUTABLE
        && OidIsValid(get_oprrest(operation.opno));
}

/** Check one Var against either the provider output or exact wire sentinel. */
[[nodiscard]] bool coverage_predicate_var_valid(
        const Var& variable,
        const CoveragePredicateContractV1& contract) noexcept {
    if (contract.domain == nullptr || variable.varattno <= 0
        || variable.varattno > MaxTupleAttributeNumber
        || variable.varlevelsup != 0
        || !bms_is_empty(variable.varnullingrels)) {
        return false;
    }
    const auto* column = coverage_schema_column(
        contract.domain->boundary_columns,
        contract.domain->boundary_column_count,
        static_cast<std::uint32_t>(variable.varattno));
    if (column == nullptr || variable.vartype != column->type_oid
        || variable.vartypmod != column->typmod
        || variable.varcollid != column->collation_oid) {
        return false;
    }
    if (contract.var_form == CoveragePredicateVarFormV1::RawConsumer) {
        const bool valid = contract.raw_relation_index != 0
            && !IS_SPECIAL_VARNO(contract.raw_relation_index)
            && variable.varno == contract.raw_relation_index
            && (variable.varnosyn == 0
                || variable.varnosyn == contract.raw_relation_index)
            && (variable.varattnosyn == 0
                || variable.varattnosyn == variable.varattno);
        return valid;
    }
    return variable.varno == INDEX_VAR
        && variable.varnosyn == INDEX_VAR
        && variable.varattnosyn == variable.varattno
        && variable.location == -1;
}

/** Total whitelist used after cache-artifact syntax decoding. */
bool validate_coverage_predicate_walker(Node* node, void* opaque) {
    if (node == nullptr) {
        return false;
    }
    auto* contract = static_cast<CoveragePredicateContractV1*>(opaque);
    if (contract == nullptr || !contract->valid) {
        return true;
    }
    if (IsA(node, Var)) {
        const auto* variable = reinterpret_cast<const Var*>(node);
        contract->valid = coverage_predicate_var_valid(*variable, *contract);
        if (contract->valid) {
            ++contract->var_count;
        }
        return !contract->valid;
    }
    if (IsA(node, Const)) {
        const auto* constant = reinterpret_cast<const Const*>(node);
        contract->valid = OidIsValid(constant->consttype);
        return !contract->valid;
    }
    if (IsA(node, OpExpr)) {
        const auto* operation = reinterpret_cast<const OpExpr*>(node);
        contract->valid = coverage_comparison_operator_valid(*operation)
            && (contract->var_form
                    != CoveragePredicateVarFormV1::FragmentSentinel
                || operation->location == -1);
    } else if (IsA(node, BoolExpr)) {
        const auto* boolean = reinterpret_cast<const BoolExpr*>(node);
        const int count = list_length(boolean->args);
        contract->valid = (boolean->boolop == NOT_EXPR
                ? count == 1
                : (boolean->boolop == AND_EXPR
                    || boolean->boolop == OR_EXPR) && count >= 2)
            && (contract->var_form
                    != CoveragePredicateVarFormV1::FragmentSentinel
                || boolean->location == -1);
    } else if (IsA(node, NullTest)) {
        const auto* test = reinterpret_cast<const NullTest*>(node);
        contract->valid = !test->argisrow
            && (test->nulltesttype == IS_NULL
                || test->nulltesttype == IS_NOT_NULL)
            && (contract->var_form
                    != CoveragePredicateVarFormV1::FragmentSentinel
                || test->location == -1);
    } else if (IsA(node, BooleanTest)) {
        const auto* test = reinterpret_cast<const BooleanTest*>(node);
        contract->valid = test->booltesttype >= IS_TRUE
            && test->booltesttype <= IS_NOT_UNKNOWN
            && (contract->var_form
                    != CoveragePredicateVarFormV1::FragmentSentinel
                || test->location == -1);
    } else if (IsA(node, RelabelType)) {
        const auto* relabel = reinterpret_cast<const RelabelType*>(node);
        contract->valid = relabel->arg != nullptr
            && relabel->resulttype == exprType(
                reinterpret_cast<const Node*>(relabel->arg))
            && (contract->var_form
                    != CoveragePredicateVarFormV1::FragmentSentinel
                || relabel->location == -1);
    } else {
        contract->valid = false;
    }
    if (!contract->valid) {
        return true;
    }
    return expression_tree_walker(
        node, validate_coverage_predicate_walker, opaque);
}

/** Validate the exact binding-neutral Node representation without repair. */
[[nodiscard]] bool normalized_coverage_predicate_valid(
        const BioseqCachePathDomainV2& domain,
        const Expr* predicate) {
    if (predicate == nullptr) {
        return false;
    }
    CoveragePredicateContractV1 contract{
        .domain = &domain,
        .var_form = CoveragePredicateVarFormV1::FragmentSentinel,
    };
    static_cast<void>(validate_coverage_predicate_walker(
        reinterpret_cast<Node*>(const_cast<Expr*>(predicate)), &contract));
    return contract.valid
        && exprType(reinterpret_cast<const Node*>(predicate)) == BOOLOID;
}

/** Copy provider Nodes while erasing all query-global binding/location state. */
Node* normalize_coverage_predicate_mutator(Node* node, void* opaque) {
    if (node == nullptr) {
        return nullptr;
    }
    auto* contract = static_cast<CoveragePredicateContractV1*>(opaque);
    if (contract == nullptr || !contract->valid) {
        return copyObject(node);
    }
    if (IsA(node, List)) {
        return expression_tree_mutator(
            node, normalize_coverage_predicate_mutator, opaque);
    }
    if (IsA(node, Var)) {
        const auto* variable = reinterpret_cast<const Var*>(node);
        if (!coverage_predicate_var_valid(*variable, *contract)) {
            contract->valid = false;
            return copyObject(node);
        }
        ++contract->var_count;
        auto* normalized = reinterpret_cast<Var*>(copyObject(node));
        normalized->varno = INDEX_VAR;
        normalized->varnosyn = INDEX_VAR;
        normalized->varattnosyn = normalized->varattno;
        normalized->location = -1;
        return reinterpret_cast<Node*>(normalized);
    }
    if (IsA(node, Const)) {
        const auto* constant = reinterpret_cast<const Const*>(node);
        if (!OidIsValid(constant->consttype)) {
            contract->valid = false;
        }
        return copyObject(node);
    }

    Node* result = nullptr;
    if (IsA(node, OpExpr)) {
        if (!coverage_comparison_operator_valid(
                *reinterpret_cast<const OpExpr*>(node))) {
            contract->valid = false;
            return copyObject(node);
        }
        result = expression_tree_mutator(
            node, normalize_coverage_predicate_mutator, opaque);
        reinterpret_cast<OpExpr*>(result)->location = -1;
    } else if (IsA(node, BoolExpr)) {
        const auto* boolean = reinterpret_cast<const BoolExpr*>(node);
        const int count = list_length(boolean->args);
        if (boolean->boolop == NOT_EXPR ? count != 1
                : !((boolean->boolop == AND_EXPR
                        || boolean->boolop == OR_EXPR) && count >= 2)) {
            contract->valid = false;
            return copyObject(node);
        }
        result = expression_tree_mutator(
            node, normalize_coverage_predicate_mutator, opaque);
        reinterpret_cast<BoolExpr*>(result)->location = -1;
    } else if (IsA(node, NullTest)) {
        const auto* test = reinterpret_cast<const NullTest*>(node);
        if (test->argisrow || (test->nulltesttype != IS_NULL
                && test->nulltesttype != IS_NOT_NULL)) {
            contract->valid = false;
            return copyObject(node);
        }
        result = expression_tree_mutator(
            node, normalize_coverage_predicate_mutator, opaque);
        reinterpret_cast<NullTest*>(result)->location = -1;
    } else if (IsA(node, BooleanTest)) {
        const auto* test = reinterpret_cast<const BooleanTest*>(node);
        if (test->booltesttype < IS_TRUE
            || test->booltesttype > IS_NOT_UNKNOWN) {
            contract->valid = false;
            return copyObject(node);
        }
        result = expression_tree_mutator(
            node, normalize_coverage_predicate_mutator, opaque);
        reinterpret_cast<BooleanTest*>(result)->location = -1;
    } else if (IsA(node, RelabelType)) {
        const auto* relabel = reinterpret_cast<const RelabelType*>(node);
        if (relabel->arg == nullptr
            || relabel->resulttype != exprType(
                reinterpret_cast<const Node*>(relabel->arg))) {
            contract->valid = false;
            return copyObject(node);
        }
        result = expression_tree_mutator(
            node, normalize_coverage_predicate_mutator, opaque);
        reinterpret_cast<RelabelType*>(result)->location = -1;
    } else {
        contract->valid = false;
        return copyObject(node);
    }
    return result;
}

/** Normalize the provider's current raw predicate in planner-owned memory. */
[[nodiscard]] Expr* normalize_coverage_predicate_fail_open(
        const BioseqCachePathDomainV2& domain,
        Index relation_index,
        MemoryContext planner_context) noexcept {
    if (domain.coverage_predicate == nullptr || relation_index == 0
        || IS_SPECIAL_VARNO(relation_index) || planner_context == nullptr) {
        return nullptr;
    }
    MemoryContext old_context = CurrentMemoryContext;
    volatile Expr* accepted = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(planner_context);
        CoveragePredicateContractV1 contract{
            .domain = &domain,
            .var_form = CoveragePredicateVarFormV1::RawConsumer,
            .raw_relation_index = relation_index,
        };
        auto* normalized = reinterpret_cast<Expr*>(
            normalize_coverage_predicate_mutator(
                reinterpret_cast<Node*>(domain.coverage_predicate),
                &contract));
        if (!contract.valid || normalized == nullptr
            || exprType(reinterpret_cast<const Node*>(normalized)) != BOOLOID
            || !normalized_coverage_predicate_valid(domain, normalized)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq cache coverage predicate cannot be normalized")));
        }
        accepted = normalized;
        MemoryContextSwitchTo(old_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = nullptr;
    }
    PG_END_TRY();
    return const_cast<Expr*>(accepted);
}

/** Compute the stable logical occurrence independently of raw RTI values. */
[[nodiscard]] std::optional<reuse::CoverageFragmentBindingSignatureV1>
coverage_fragment_binding_base(
        PlannerInfo* root,
        const RelOptInfo* relation,
        std::uint32_t provider_id) noexcept {
    if (root == nullptr || root->parse == nullptr || relation == nullptr
        || relation->relid == 0 || provider_id == 0) {
        return std::nullopt;
    }
    Index logical_index = relation->relid;
    if (!bms_is_empty(relation->top_parent_relids)) {
        if (bms_num_members(relation->top_parent_relids) != 1) {
            return std::nullopt;
        }
        logical_index = static_cast<Index>(bms_next_member(
            relation->top_parent_relids, -1));
    }
    if (logical_index == 0
        || logical_index > static_cast<Index>(
            list_length(root->parse->rtable))) {
        return std::nullopt;
    }
    const RangeTblEntry* logical = planner_rt_fetch(logical_index, root);
    const RangeTblEntry* selected = planner_rt_fetch(relation->relid, root);
    if (logical == nullptr || selected == nullptr
        || logical->rtekind != RTE_RELATION
        || selected->rtekind != RTE_RELATION
        || !OidIsValid(logical->relid) || !OidIsValid(selected->relid)) {
        return std::nullopt;
    }
    std::uint32_t occurrence_count = 0;
    std::uint32_t occurrence_ordinal = 0;
    bool found_current = false;
    Index index = 0;
    const ListCell* cell = nullptr;
    foreach(cell, root->parse->rtable) {
        ++index;
        const auto* candidate = static_cast<const RangeTblEntry*>(
            lfirst(cell));
        if (candidate == nullptr || candidate->rtekind != RTE_RELATION
            || candidate->relid != logical->relid
            || candidate->inh != logical->inh
            || candidate->lateral != logical->lateral) {
            continue;
        }
        if (index == logical_index) {
            occurrence_ordinal = occurrence_count;
            found_current = true;
        }
        ++occurrence_count;
    }
    reuse::CoverageFragmentBindingSignatureV1 binding{
        .provider_rte_occurrence_ordinal = occurrence_ordinal,
        .provider_rte_occurrence_count = occurrence_count,
        .logical_relation_oid = static_cast<std::uint32_t>(logical->relid),
        .selected_relation_oid = static_cast<std::uint32_t>(selected->relid),
        .provider_id = provider_id,
    };
    return found_current && reuse::valid_coverage_fragment_binding(binding)
        ? std::optional{binding}
        : std::nullopt;
}

/** Pointer-free persisted provider contract stored in the reusable registry. */
struct CoverageDescriptorHeaderV1 {
    std::uint32_t magic = kCoverageDescriptorMagicV1;
    std::uint32_t format_version = kCoverageDescriptorFormatV1;
    std::uint32_t provider_id = 0;
    std::uint32_t flags = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t capability_mask = 0;
    std::uint32_t provider_security_digest_format_version = 0;
    std::uint32_t provider_security_flags = 0;
    std::array<std::uint8_t, kProviderDigestBytes>
        provider_security_digest{};
    std::uint32_t boundary_count = 0;
    std::uint32_t lineage_count = 0;
    std::uint32_t required_boundary_count = 0;
    std::uint32_t required_lineage_count = 0;
    std::uint32_t predicate_bytes = 0;
    std::uint32_t dependency_node_bytes = 0;
    std::uint32_t provider_private_node_bytes = 0;
    std::uint32_t payload_crc32c = 0;
    reuse::CoverageFragmentBindingSignatureV1 binding{};
    std::uint32_t reserved = 0;
};

static_assert(std::is_standard_layout_v<CoverageDescriptorHeaderV1>);
static_assert(std::is_trivially_copyable_v<CoverageDescriptorHeaderV1>);

/** Planner-owned reconstruction of one pointer-free coverage descriptor. */
struct PlannerDecodedCoverageV1 {
    MemoryContext owner_context = nullptr;
    BioseqCachePathDomainV2 domain{};
    BioseqCacheReusableSourceV2 source{};
    reuse::CoverageFragmentBindingSignatureV1 binding{};
};

[[nodiscard]] bool add_coverage_descriptor_size(
        std::uint64_t& total, std::uint64_t count,
        std::uint64_t width) noexcept {
    if (width != 0
        && count > (std::numeric_limits<std::uint64_t>::max)() / width) {
        return false;
    }
    const std::uint64_t bytes = count * width;
    if (total > (std::numeric_limits<std::uint64_t>::max)() - bytes) {
        return false;
    }
    total += bytes;
    return total <= kMaxProviderRecipeBytes;
}

/** CRC covers the complete variable payload while allowing header inspection. */
[[nodiscard]] std::optional<std::uint32_t>
coverage_descriptor_payload_crc32c(
        std::span<const std::byte> descriptor) noexcept {
    if (descriptor.size() < sizeof(CoverageDescriptorHeaderV1)) {
        return std::nullopt;
    }
    PostgreSqlColumnChecksumPolicy checksum_policy;
    const auto checksum = checksum_policy.block_crc32c(
        descriptor.subspan(sizeof(CoverageDescriptorHeaderV1)));
    return checksum.has_value()
        ? std::optional{*checksum}
        : std::nullopt;
}

[[nodiscard]] bool coverage_descriptor_size(
        const CoverageDescriptorHeaderV1& header,
        std::uint64_t* total) noexcept {
    if (total == nullptr
        || header.magic != kCoverageDescriptorMagicV1
        || header.format_version != kCoverageDescriptorFormatV1
        || header.provider_id == 0 || header.flags != 0
        || header.compatibility_family_id == 0 || header.reserved != 0
        || !reuse::valid_coverage_fragment_binding(header.binding)
        || header.binding.provider_id != header.provider_id
        || header.provider_security_digest_format_version
            != BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1
        || header.provider_security_flags != 0
        || header.boundary_count > kMaxProviderBoundaryColumns
        || header.lineage_count > kMaxProviderBoundaryColumns
        || header.required_boundary_count > kMaxProviderBoundaryColumns
        || header.required_lineage_count > kMaxProviderBoundaryColumns
        || header.predicate_bytes == 0) {
        return false;
    }
    std::uint64_t bytes = sizeof(CoverageDescriptorHeaderV1);
    if (!add_coverage_descriptor_size(
            bytes, header.boundary_count,
            sizeof(BioseqCacheColumnSchemaV2))
        || !add_coverage_descriptor_size(
            bytes, header.lineage_count,
            sizeof(BioseqCacheColumnSchemaV2))
        || !add_coverage_descriptor_size(
            bytes, header.required_boundary_count,
            sizeof(std::uint32_t))
        || !add_coverage_descriptor_size(
            bytes, header.required_lineage_count,
            sizeof(std::uint32_t))
        || !add_coverage_descriptor_size(
            bytes, 1, header.predicate_bytes)
        || !add_coverage_descriptor_size(
            bytes, 1, header.dependency_node_bytes)
        || !add_coverage_descriptor_size(
            bytes, 1, header.provider_private_node_bytes)) {
        return false;
    }
    *total = bytes;
    return true;
}

/** Serialize validated provider POD and Nodes without retaining any pointer. */
[[nodiscard]] bytea* serialize_coverage_descriptor_fail_open(
        const BioseqCachePathDomainV2& domain,
        const reuse::CoverageFragmentBindingSignatureV1& binding,
        MemoryContext planner_context) noexcept {
    if (planner_context == nullptr
        || !reuse::valid_coverage_fragment_binding(binding)
        || binding.provider_id != domain.provider_id
        || !reuse::validate_path_domain(
                domain.provider_id, &domain).has_value()
        || !normalized_coverage_predicate_valid(
            domain, domain.coverage_predicate)) {
        return nullptr;
    }
    MemoryContext old_context = CurrentMemoryContext;
    volatile bytea* accepted = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(planner_context);
        char* predicate = nodeToString(domain.coverage_predicate);
        char* dependencies = nodeToString(domain.dependency_nodes);
        char* provider_private = nodeToString(domain.provider_private_nodes);
        if (predicate == nullptr || dependencies == nullptr
            || provider_private == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache provider descriptor is not serializable")));
        }
        const std::size_t predicate_bytes = std::strlen(predicate);
        const std::size_t dependency_bytes = std::strlen(dependencies);
        const std::size_t provider_private_bytes =
            std::strlen(provider_private);
        if (predicate_bytes > (std::numeric_limits<std::uint32_t>::max)()
            || dependency_bytes
                > (std::numeric_limits<std::uint32_t>::max)()
            || provider_private_bytes
                > (std::numeric_limits<std::uint32_t>::max)()) {
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq cache provider descriptor is too large")));
        }
        const CoverageDescriptorHeaderV1 header{
            .provider_id = domain.provider_id,
            .compatibility_family_id = domain.compatibility_family_id,
            .capability_mask = domain.capability_mask,
            .provider_security_digest_format_version =
                domain.provider_security_digest_format_version,
            .provider_security_flags = domain.provider_security_flags,
            .provider_security_digest = [&domain] {
                std::array<std::uint8_t, kProviderDigestBytes> digest{};
                std::copy_n(
                    domain.provider_security_digest,
                    digest.size(),
                    digest.begin());
                return digest;
            }(),
            .boundary_count = static_cast<std::uint32_t>(
                domain.boundary_column_count),
            .lineage_count = static_cast<std::uint32_t>(
                domain.lineage_column_count),
            .required_boundary_count = static_cast<std::uint32_t>(
                domain.required_boundary_attno_count),
            .required_lineage_count = static_cast<std::uint32_t>(
                domain.required_lineage_attno_count),
            .predicate_bytes = static_cast<std::uint32_t>(predicate_bytes),
            .dependency_node_bytes = static_cast<std::uint32_t>(
                dependency_bytes),
            .provider_private_node_bytes = static_cast<std::uint32_t>(
                provider_private_bytes),
            .binding = binding,
        };
        std::uint64_t raw_size = 0;
        if (!coverage_descriptor_size(header, &raw_size)
            || raw_size > static_cast<std::uint64_t>(MaxAllocSize - VARHDRSZ)) {
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq cache provider descriptor exceeds its bound")));
        }
        auto* descriptor = static_cast<bytea*>(
            palloc(VARHDRSZ + static_cast<std::size_t>(raw_size)));
        SET_VARSIZE(
            descriptor, VARHDRSZ + static_cast<std::size_t>(raw_size));
        std::byte* destination = reinterpret_cast<std::byte*>(
            VARDATA(descriptor));
        std::size_t offset = 0;
        const auto append = [destination, &offset](
                const void* source, std::size_t size) {
            if (size != 0) {
                std::memcpy(destination + offset, source, size);
                offset += size;
            }
        };
        append(&header, sizeof(header));
        append(
            domain.boundary_columns,
            domain.boundary_column_count
                * sizeof(BioseqCacheColumnSchemaV2));
        append(
            domain.lineage_columns,
            domain.lineage_column_count
                * sizeof(BioseqCacheColumnSchemaV2));
        append(
            domain.required_boundary_attnos,
            domain.required_boundary_attno_count * sizeof(std::uint32_t));
        append(
            domain.required_lineage_attnos,
            domain.required_lineage_attno_count * sizeof(std::uint32_t));
        append(predicate, predicate_bytes);
        append(dependencies, dependency_bytes);
        append(provider_private, provider_private_bytes);
        if (offset != raw_size) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache provider descriptor size changed")));
        }
        const auto payload_crc = coverage_descriptor_payload_crc32c(
            std::span<const std::byte>{destination, offset});
        if (!payload_crc.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq cache provider descriptor checksum is unavailable")));
        }
        CoverageDescriptorHeaderV1 finalized_header = header;
        finalized_header.payload_crc32c = *payload_crc;
        std::memcpy(destination, &finalized_header, sizeof(finalized_header));
        accepted = descriptor;
        MemoryContextSwitchTo(old_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = nullptr;
    }
    PG_END_TRY();
    return const_cast<bytea*>(accepted);
}

[[nodiscard]] void* parse_coverage_node_field(
        std::span<const std::byte> bytes,
        std::size_t& offset,
        std::uint32_t length) {
    if (length == 0 || offset > bytes.size()
        || length > bytes.size() - offset
        || std::memchr(bytes.data() + offset, 0, length) != nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq cache coverage descriptor node is malformed")));
    }
    auto* serialized = static_cast<char*>(palloc(length + 1U));
    std::memcpy(serialized, bytes.data() + offset, length);
    serialized[length] = '\0';
    offset += length;
    return stringToNode(serialized);
}

/** Reconstruct one source in disposable planner memory before releasing pin. */
[[nodiscard]] bool decode_coverage_descriptor_fail_open(
        std::span<const std::byte> bytes,
        MemoryContext planner_context,
        PlannerDecodedCoverageV1* output,
        const reuse::CoverageFragmentBindingSignatureV1*
            expected_binding = nullptr) noexcept {
    if (planner_context == nullptr || output == nullptr
        || bytes.size() < sizeof(CoverageDescriptorHeaderV1)
        || bytes.size() > kMaxProviderRecipeBytes) {
        return false;
    }
    CoverageDescriptorHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::uint64_t expected_size = 0;
    const auto payload_crc = coverage_descriptor_payload_crc32c(bytes);
    if (!coverage_descriptor_size(header, &expected_size)
        || expected_size != bytes.size()
        || !payload_crc.has_value()
        || *payload_crc != header.payload_crc32c
        || (expected_binding != nullptr
            && header.binding != *expected_binding)) {
        return false;
    }

    MemoryContext old_context = CurrentMemoryContext;
    volatile MemoryContext child_context = nullptr;
    volatile PlannerDecodedCoverageV1* accepted = nullptr;
    PG_TRY();
    {
        child_context = AllocSetContextCreate(
            planner_context,
            "bioseq cache coverage descriptor",
            ALLOCSET_SMALL_SIZES);
        MemoryContextSwitchTo(child_context);
        auto* decoded = static_cast<PlannerDecodedCoverageV1*>(
            palloc0(sizeof(PlannerDecodedCoverageV1)));
        std::size_t offset = sizeof(CoverageDescriptorHeaderV1);
        auto copy_array = [bytes, &offset](
                std::size_t count, std::size_t width) -> void* {
            if (count == 0) {
                return nullptr;
            }
            const std::size_t size = count * width;
            if (offset > bytes.size() || size > bytes.size() - offset) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq cache coverage descriptor is truncated")));
            }
            void* copy = palloc(size);
            std::memcpy(copy, bytes.data() + offset, size);
            offset += size;
            return copy;
        };
        auto* boundary = static_cast<BioseqCacheColumnSchemaV2*>(copy_array(
            header.boundary_count, sizeof(BioseqCacheColumnSchemaV2)));
        auto* lineage = static_cast<BioseqCacheColumnSchemaV2*>(copy_array(
            header.lineage_count, sizeof(BioseqCacheColumnSchemaV2)));
        auto* required_boundary = static_cast<std::uint32_t*>(copy_array(
            header.required_boundary_count, sizeof(std::uint32_t)));
        auto* required_lineage = static_cast<std::uint32_t*>(copy_array(
            header.required_lineage_count, sizeof(std::uint32_t)));
        void* predicate = parse_coverage_node_field(
            bytes, offset, header.predicate_bytes);
        void* dependencies = parse_coverage_node_field(
            bytes, offset, header.dependency_node_bytes);
        void* provider_private = parse_coverage_node_field(
            bytes, offset, header.provider_private_node_bytes);
        if (offset != bytes.size() || predicate == nullptr
            || (dependencies != nullptr && !IsA(dependencies, List))
            || (provider_private != nullptr && !IsA(provider_private, List))) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache coverage descriptor has invalid Nodes")));
        }
        decoded->owner_context = child_context;
        decoded->domain = BioseqCachePathDomainV2{
            .struct_size = sizeof(BioseqCachePathDomainV2),
            .provider_id = header.provider_id,
            .compatibility_family_id = header.compatibility_family_id,
            .capability_mask = header.capability_mask,
            .coverage_predicate = static_cast<Expr*>(predicate),
            .boundary_columns = boundary,
            .boundary_column_count = header.boundary_count,
            .lineage_columns = lineage,
            .lineage_column_count = header.lineage_count,
            .required_boundary_attnos = required_boundary,
            .required_boundary_attno_count = header.required_boundary_count,
            .required_lineage_attnos = required_lineage,
            .required_lineage_attno_count = header.required_lineage_count,
            .dependency_nodes = static_cast<List*>(dependencies),
            .provider_private_nodes = static_cast<List*>(provider_private),
            .provider_security_digest_format_version =
                header.provider_security_digest_format_version,
            .provider_security_flags = header.provider_security_flags,
            .provider_security_digest = {},
        };
        std::copy_n(
            header.provider_security_digest.begin(),
            header.provider_security_digest.size(),
            decoded->domain.provider_security_digest);
        decoded->source = BioseqCacheReusableSourceV2{
            .struct_size = sizeof(BioseqCacheReusableSourceV2),
            .provider_id = header.provider_id,
            .compatibility_family_id = header.compatibility_family_id,
            .capability_mask = header.capability_mask,
            .boundary_columns = boundary,
            .boundary_column_count = header.boundary_count,
            .lineage_columns = lineage,
            .lineage_column_count = header.lineage_count,
            .provider_private_nodes = static_cast<List*>(provider_private),
        };
        decoded->binding = header.binding;
        if (!reuse::validate_path_domain(
                header.provider_id, &decoded->domain).has_value()
            || !normalized_coverage_predicate_valid(
                decoded->domain, decoded->domain.coverage_predicate)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache coverage descriptor contract is invalid")));
        }
        accepted = decoded;
        MemoryContextSwitchTo(old_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (cache_interrupt_error(sqlstate)) {
            if (child_context != nullptr) {
                MemoryContextDelete(child_context);
            }
            PG_RE_THROW();
        }
        FlushErrorState();
        if (child_context != nullptr) {
            MemoryContextDelete(child_context);
        }
        child_context = nullptr;
        accepted = nullptr;
    }
    PG_END_TRY();
    if (accepted == nullptr) {
        return false;
    }
    *output = *const_cast<PlannerDecodedCoverageV1*>(accepted);
    return true;
}

[[nodiscard]] std::span<const std::byte> bytea_payload(
        const bytea* value) noexcept;
[[nodiscard]] std::span<const std::byte> binary_node_payload(
        const void* node) noexcept;

#if defined(BIOSEQ_CACHE_TESTING)
struct CoverageDescriptorFaultVarV1 {
    Index raw_relation_index = 0;
    bool changed = false;
};

[[nodiscard]] bool coverage_descriptor_wire_fault_active() noexcept {
    return g_test_coverage_descriptor_fault
            >= TestCoverageDescriptorFault::RawProducerVar
        && g_test_coverage_descriptor_fault
            <= TestCoverageDescriptorFault::BindingSignatureMismatch;
}

/** Replace exactly one sentinel Var so mixed-form faults remain deterministic. */
bool mutate_coverage_descriptor_var_walker(Node* node, void* opaque) {
    if (node == nullptr) {
        return false;
    }
    auto* fault = static_cast<CoverageDescriptorFaultVarV1*>(opaque);
    if (fault == nullptr || fault->changed) {
        return fault != nullptr && fault->changed;
    }
    if (IsA(node, Var)) {
        auto* variable = reinterpret_cast<Var*>(node);
        variable->varno = fault->raw_relation_index;
        variable->varnosyn = fault->raw_relation_index;
        fault->changed = true;
        return true;
    }
    return expression_tree_walker(
        node, mutate_coverage_descriptor_var_walker, opaque);
}

/** Rebuild one copied candidate with a valid outer CRC around bad inner data. */
[[nodiscard]] bytea* inject_coverage_descriptor_fault_fail_open(
        const bytea* original,
        MemoryContext planner_context) noexcept {
    if (original == nullptr || planner_context == nullptr
        || !coverage_descriptor_wire_fault_active()) {
        return const_cast<bytea*>(original);
    }
    const auto bytes = bytea_payload(original);
    if (bytes.size() < sizeof(CoverageDescriptorHeaderV1)) {
        return nullptr;
    }
    CoverageDescriptorHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::uint64_t original_size = 0;
    if (!coverage_descriptor_size(header, &original_size)
        || original_size != bytes.size()) {
        return nullptr;
    }
    std::uint64_t predicate_offset = sizeof(CoverageDescriptorHeaderV1);
    const auto add_offset = [&predicate_offset](
            std::uint64_t count, std::uint64_t width) {
        if (width != 0
            && count > (std::numeric_limits<std::uint64_t>::max)() / width) {
            return false;
        }
        const std::uint64_t size = count * width;
        if (predicate_offset
                > (std::numeric_limits<std::uint64_t>::max)() - size) {
            return false;
        }
        predicate_offset += size;
        return true;
    };
    if (!add_offset(header.boundary_count,
            sizeof(BioseqCacheColumnSchemaV2))
        || !add_offset(header.lineage_count,
            sizeof(BioseqCacheColumnSchemaV2))
        || !add_offset(header.required_boundary_count,
            sizeof(std::uint32_t))
        || !add_offset(header.required_lineage_count,
            sizeof(std::uint32_t))
        || predicate_offset > bytes.size()
        || header.predicate_bytes > bytes.size() - predicate_offset) {
        return nullptr;
    }

    MemoryContext old_context = CurrentMemoryContext;
    volatile bytea* accepted = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(planner_context);
        char* replacement = nullptr;
        if (g_test_coverage_descriptor_fault
                == TestCoverageDescriptorFault::MalformedSyntax) {
            replacement = pstrdup("(");
        } else if (g_test_coverage_descriptor_fault
                == TestCoverageDescriptorFault::WrongNodeType) {
            replacement = nodeToString(list_make1(makeInteger(1)));
        } else if (g_test_coverage_descriptor_fault
                == TestCoverageDescriptorFault::BindingSignatureMismatch) {
            header.binding.provider_rte_occurrence_count += 1U;
            replacement = static_cast<char*>(palloc(
                header.predicate_bytes + 1U));
            std::memcpy(
                replacement,
                bytes.data() + predicate_offset,
                header.predicate_bytes);
            replacement[header.predicate_bytes] = '\0';
        } else {
            std::size_t parse_offset = static_cast<std::size_t>(
                predicate_offset);
            void* parsed = parse_coverage_node_field(
                bytes, parse_offset, header.predicate_bytes);
            if (parsed == nullptr
                || !(IsA(parsed, Var) || IsA(parsed, Const)
                    || IsA(parsed, OpExpr) || IsA(parsed, BoolExpr)
                    || IsA(parsed, NullTest) || IsA(parsed, BooleanTest)
                    || IsA(parsed, RelabelType))) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq cache test descriptor has no predicate")));
            }
            auto* predicate = reinterpret_cast<Expr*>(parsed);
            CoverageDescriptorFaultVarV1 fault{
                .raw_relation_index = g_test_coverage_descriptor_fault
                        == TestCoverageDescriptorFault::RawProducerVar
                    ? static_cast<Index>(1)
                    : static_cast<Index>(2),
            };
            if (g_test_coverage_descriptor_fault
                    == TestCoverageDescriptorFault::ForeignMixedVar) {
                auto* foreign = reinterpret_cast<Expr*>(
                    copyObject(predicate));
                static_cast<void>(mutate_coverage_descriptor_var_walker(
                    reinterpret_cast<Node*>(foreign), &fault));
                predicate = make_andclause(list_make2(predicate, foreign));
            } else {
                static_cast<void>(mutate_coverage_descriptor_var_walker(
                    reinterpret_cast<Node*>(predicate), &fault));
            }
            if (!fault.changed) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("bioseq cache test descriptor has no Var")));
            }
            replacement = nodeToString(predicate);
        }
        if (replacement == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache test descriptor cannot be serialized")));
        }
        const std::size_t replacement_size = std::strlen(replacement);
        const std::size_t old_tail_offset = static_cast<std::size_t>(
            predicate_offset) + header.predicate_bytes;
        const std::size_t tail_size = bytes.size() - old_tail_offset;
        header.predicate_bytes = static_cast<std::uint32_t>(replacement_size);
        header.payload_crc32c = 0;
        std::uint64_t rebuilt_size = 0;
        if (!coverage_descriptor_size(header, &rebuilt_size)
            || rebuilt_size > static_cast<std::uint64_t>(MaxAllocSize)
            || rebuilt_size != static_cast<std::uint64_t>(
                predicate_offset) + replacement_size + tail_size) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq cache test descriptor size is invalid")));
        }
        auto* rebuilt = static_cast<bytea*>(palloc(
            VARHDRSZ + static_cast<std::size_t>(rebuilt_size)));
        SET_VARSIZE(
            rebuilt, VARHDRSZ + static_cast<std::size_t>(rebuilt_size));
        auto* destination = reinterpret_cast<std::byte*>(VARDATA(rebuilt));
        std::memcpy(destination, &header, sizeof(header));
        std::memcpy(
            destination + sizeof(header),
            bytes.data() + sizeof(header),
            static_cast<std::size_t>(predicate_offset) - sizeof(header));
        std::memcpy(
            destination + predicate_offset, replacement, replacement_size);
        std::memcpy(
            destination + predicate_offset + replacement_size,
            bytes.data() + old_tail_offset,
            tail_size);
        const auto payload_crc = coverage_descriptor_payload_crc32c(
            std::span<const std::byte>{
                destination, static_cast<std::size_t>(rebuilt_size)});
        if (!payload_crc.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq cache test descriptor checksum failed")));
        }
        header.payload_crc32c = *payload_crc;
        std::memcpy(destination, &header, sizeof(header));
        accepted = rebuilt;
        MemoryContextSwitchTo(old_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = nullptr;
    }
    PG_END_TRY();
    return const_cast<bytea*>(accepted);
}
#endif

struct PlannerSemanticKeysV1 {
    bytea* canonical_domain_key = nullptr;
    bytea* canonical_generation_key = nullptr;
    CacheDigest256 domain_digest{};
    CacheDigest256 generation_digest{};
    ReusableSecurityNamespaceV1 security_namespace{};
    reuse::CoverageFragmentBindingSignatureV1 binding{};
};

[[nodiscard]] ReusableSecurityNamespaceV1
current_reusable_security_namespace(
        const CacheDigest256& provider_security_digest) noexcept {
    Oid current_role = InvalidOid;
    int security_context = 0;
    GetUserIdAndSecContext(&current_role, &security_context);
    return ReusableSecurityNamespaceV1{
        .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
        .current_role_oid = static_cast<std::uint32_t>(current_role),
        .session_user_oid = static_cast<std::uint32_t>(GetSessionUserId()),
        .security_context_flags = static_cast<std::uint32_t>(security_context),
        .row_security_mode = row_security ? 1 : 0,
        .provider_security_digest = provider_security_digest,
    };
}

/** Capture both provider-defined canonical keys and their SHA-256 directories. */
[[nodiscard]] bool capture_planner_semantic_keys_fail_open(
        const provider_registry::ProviderV2* provider,
        const BioseqCachePathDomainV2& domain,
        const reuse::CoverageFragmentBindingSignatureV1& binding_base,
        MemoryContext planner_context,
        PlannerSemanticKeysV1* output) noexcept {
    if (provider == nullptr || planner_context == nullptr || output == nullptr
        || !reuse::valid_coverage_fragment_binding(binding_base)
        || binding_base.provider_id != provider->provider_id
        || !reuse::validate_path_domain(
            domain.provider_id, &domain).has_value()
        || !normalized_coverage_predicate_valid(
            domain, domain.coverage_predicate)) {
        return false;
    }
    const ReusableSecurityNamespaceV1 security_namespace =
        current_reusable_security_namespace(
            reuse::provider_security_digest(domain));
    if (!valid_reusable_security_namespace(security_namespace)) {
        return false;
    }
    MemoryContext old_context = CurrentMemoryContext;
    volatile PlannerSemanticKeysV1* accepted = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(planner_context);
        auto* keys = static_cast<PlannerSemanticKeysV1*>(
            palloc0(sizeof(PlannerSemanticKeysV1)));
        auto* domain_key = static_cast<bytea*>(palloc(
            VARHDRSZ + static_cast<std::size_t>(kMaxCoverageDomainKeyBytes)));
        auto* generation_key = static_cast<bytea*>(palloc(
            VARHDRSZ + static_cast<std::size_t>(kMaxCanonicalKeyBytes)));
        constexpr std::size_t domain_payload_capacity =
            static_cast<std::size_t>(kMaxCoverageDomainKeyBytes)
            - reuse::kCoverageCanonicalKeyFixedBytesV1;
        constexpr std::size_t generation_payload_capacity =
            static_cast<std::size_t>(kMaxCanonicalKeyBytes)
            - reuse::kCoverageCanonicalKeyFixedBytesV1;
        char* normalized_predicate = nodeToString(domain.coverage_predicate);
        if (normalized_predicate == nullptr) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq cache normalized predicate is unavailable")));
        }
        const std::size_t normalized_predicate_size =
            std::strlen(normalized_predicate);
        const std::size_t generation_binding_capacity =
            reuse::kCoverageBindingSemanticFixedBytesV1
            + 1U + sizeof(std::uint64_t) + normalized_predicate_size;
        if (domain_payload_capacity
                <= reuse::kCoverageBindingSemanticFixedBytesV1
            || generation_payload_capacity
                <= generation_binding_capacity) {
            ereport(ERROR,
                (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
                 errmsg("bioseq cache normalized predicate exceeds its key bound")));
        }
        auto* domain_payload = static_cast<std::byte*>(
            palloc(domain_payload_capacity));
        auto* generation_payload = static_cast<std::byte*>(
            palloc(generation_payload_capacity));
        std::size_t domain_payload_size = 0;
        std::size_t generation_payload_size = 0;
        reuse::ProviderCallDiagnosticV1 diagnostic{};
        if (!reuse::invoke_semantic_digest_fail_open(
                provider,
                &domain,
                reuse::SemanticDigestKindV1::Domain,
                std::span<std::byte>{
                    domain_payload,
                    domain_payload_capacity
                        - reuse::kCoverageBindingSemanticFixedBytesV1},
                &domain_payload_size,
                planner_context,
                &diagnostic)
            || !reuse::invoke_semantic_digest_fail_open(
                provider,
                &domain,
                reuse::SemanticDigestKindV1::Generation,
                std::span<std::byte>{
                    generation_payload,
                    generation_payload_capacity
                        - generation_binding_capacity},
                &generation_payload_size,
                planner_context,
                &diagnostic)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq cache provider semantic key is unavailable")));
        }
        PostgreSqlColumnChecksumPolicy checksum_policy;
        const auto role_digest = checksum_policy.sha256(
            std::span<const std::byte>{
                domain_payload, domain_payload_size});
        if (!role_digest.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq cache binding-role digest is unavailable")));
        }
        reuse::CoverageFragmentBindingSignatureV1 binding = binding_base;
        binding.binding_role_digest = *role_digest;
        const auto domain_binding_size =
            reuse::write_coverage_binding_semantics(
                reuse::CoverageCanonicalKeyKindV1::Domain,
                binding,
                {},
                std::span<std::byte>{
                    domain_payload + domain_payload_size,
                    domain_payload_capacity - domain_payload_size});
        const auto generation_binding_size =
            reuse::write_coverage_binding_semantics(
                reuse::CoverageCanonicalKeyKindV1::Generation,
                binding,
                std::span<const std::byte>{
                    reinterpret_cast<const std::byte*>(normalized_predicate),
                    normalized_predicate_size},
                std::span<std::byte>{
                    generation_payload + generation_payload_size,
                    generation_payload_capacity - generation_payload_size});
        if (!domain_binding_size.has_value()
            || !generation_binding_size.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq cache fragment binding key is unavailable")));
        }
        domain_payload_size += *domain_binding_size;
        generation_payload_size += *generation_binding_size;
        const auto domain_size = reuse::write_coverage_canonical_key(
            reuse::CoverageCanonicalKeyKindV1::Domain,
            security_namespace,
            std::span<const std::byte>{
                domain_payload, domain_payload_size},
            std::span<std::byte>{
                reinterpret_cast<std::byte*>(VARDATA(domain_key)),
                static_cast<std::size_t>(kMaxCoverageDomainKeyBytes)});
        const auto generation_size = reuse::write_coverage_canonical_key(
            reuse::CoverageCanonicalKeyKindV1::Generation,
            security_namespace,
            std::span<const std::byte>{
                generation_payload, generation_payload_size},
            std::span<std::byte>{
                reinterpret_cast<std::byte*>(VARDATA(generation_key)),
                static_cast<std::size_t>(kMaxCanonicalKeyBytes)});
        if (!domain_size.has_value() || !generation_size.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq cache canonical security key is unavailable")));
        }
        pfree(generation_payload);
        pfree(domain_payload);
        SET_VARSIZE(domain_key, VARHDRSZ + *domain_size);
        SET_VARSIZE(generation_key, VARHDRSZ + *generation_size);
        const auto domain_digest = checksum_policy.sha256(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(VARDATA(domain_key)),
                *domain_size});
        const auto generation_digest = checksum_policy.sha256(
            std::span<const std::byte>{
                reinterpret_cast<const std::byte*>(VARDATA(generation_key)),
                *generation_size});
        if (!domain_digest.has_value() || !generation_digest.has_value()) {
            ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                 errmsg("bioseq cache semantic digest is unavailable")));
        }
        keys->canonical_domain_key = domain_key;
        keys->canonical_generation_key = generation_key;
        keys->domain_digest = *domain_digest;
        keys->generation_digest = *generation_digest;
        keys->security_namespace = security_namespace;
        keys->binding = binding;
#if defined(BIOSEQ_CACHE_TESTING)
        if (g_forced_coverage_domain_digest_active) {
            keys->domain_digest = g_forced_coverage_domain_digest;
        }
#endif
        accepted = keys;
        MemoryContextSwitchTo(old_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(old_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = nullptr;
    }
    PG_END_TRY();
    if (accepted == nullptr) {
        return false;
    }
    *output = *const_cast<PlannerSemanticKeysV1*>(accepted);
    return true;
}

[[nodiscard]] reuse::CoverageSecurityNamespaceV1
planner_security_namespace(
        const ReusableSecurityNamespaceV1& source) noexcept {
    return source;
}

inline constexpr std::uint32_t kCoverageCurrentPlanMagicV1 = 0x42435031U;

/** Fixed prefix of executor metadata embedded as one bytea Const Node. */
struct CoverageCurrentPlanHeaderV1 {
    std::uint32_t magic = kCoverageCurrentPlanMagicV1;
    std::uint32_t format_version = reuse::kReusablePlanFormatVersionV1;
    std::uint32_t provider_id = 0;
    std::uint32_t provider_abi_version = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t capability_mask = 0;
    ReusableCoverageDirectoryKeyV1 coverage_directory_key{};
    CacheDigest256 generation_digest{};
    std::uint32_t canonical_domain_key_bytes = 0;
    std::uint32_t canonical_generation_key_bytes = 0;
    std::uint32_t coverage_descriptor_bytes = 0;
    std::uint32_t reserved = 0;
};

static_assert(std::is_standard_layout_v<CoverageCurrentPlanHeaderV1>);
static_assert(std::is_trivially_copyable_v<CoverageCurrentPlanHeaderV1>);

struct CoverageCurrentPlanViewV1 {
    CoverageCurrentPlanHeaderV1 header{};
    std::span<const std::byte> canonical_domain_key{};
    std::span<const std::byte> canonical_generation_key{};
    std::span<const std::byte> coverage_descriptor{};
};

[[nodiscard]] bool decode_current_coverage_plan(
        std::span<const std::byte> bytes,
        CoverageCurrentPlanViewV1* output) noexcept {
    if (output == nullptr || bytes.size() < sizeof(CoverageCurrentPlanHeaderV1)) {
        return false;
    }
    CoverageCurrentPlanHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kCoverageCurrentPlanMagicV1
        || header.format_version != reuse::kReusablePlanFormatVersionV1
        || header.provider_id == 0 || header.provider_abi_version == 0
        || header.compatibility_family_id == 0 || header.reserved != 0
        || header.canonical_domain_key_bytes == 0
        || header.canonical_generation_key_bytes == 0
        || header.coverage_descriptor_bytes == 0) {
        return false;
    }
    std::uint64_t expected = sizeof(header);
    if (!add_coverage_descriptor_size(
            expected, 1, header.canonical_domain_key_bytes)
        || !add_coverage_descriptor_size(
            expected, 1, header.canonical_generation_key_bytes)
        || !add_coverage_descriptor_size(
            expected, 1, header.coverage_descriptor_bytes)
        || expected != bytes.size()) {
        return false;
    }
    std::size_t offset = sizeof(header);
    const auto take = [bytes, &offset](std::uint32_t size) {
        const auto result = bytes.subspan(offset, size);
        offset += size;
        return result;
    };
    output->header = header;
    output->canonical_domain_key = take(header.canonical_domain_key_bytes);
    output->canonical_generation_key = take(
        header.canonical_generation_key_bytes);
    output->coverage_descriptor = take(header.coverage_descriptor_bytes);
    return offset == bytes.size()
        && output->header.coverage_directory_key.security_namespace
            == current_reusable_security_namespace(
                output->header.coverage_directory_key.security_namespace
                    .provider_security_digest);
}

struct CoverageDependencyTicketV1 {
    CacheDigest256 fingerprint{};
    PhysicalIdentityV1 physical_identity{};
    DependencyVersion version{};
    std::uint32_t dependency_count = 0;
    WriteObservability observability = WriteObservability::None;
    bool valid = false;
};

/** Refuse a reusable snapshot that can observe this backend's own writes. */
[[nodiscard]] bool current_transaction_touches_dependency(
        Oid relation_oid,
        const PhysicalIdentityV1& physical_identity) noexcept {
    if (g_force_coarse_transition || g_dirty_observations.coarse()) {
        return true;
    }
    std::array<DirtyRelationObservationV1,
        kMaximumRelationDependencies> observations{};
    const std::size_t observation_count =
        g_dirty_observations.copy_unique(observations);
    for (std::size_t index = 0; index < observation_count; ++index) {
        const DirtyRelationObservationV1& observation = observations[index];
        if (!observation.physical_identity_valid
            && !observation.storage_parent) {
            return true;
        }
        if (observation.storage_parent
            || observation.relation_oid == relation_oid
            || (observation.physical_identity_valid
                && observation.physical_identity == physical_identity)) {
            return true;
        }
    }
    return false;
}

/** Version one admits one precise provider leaf plus the coarse overflow epoch. */
[[nodiscard]] CoverageDependencyTicketV1 coverage_dependency_ticket(
        const BioseqCachePathDomainV2& contract,
        const provider_registry::ProviderV2* provider,
        std::uint64_t required_completion_count = 0) noexcept {
    CoverageDependencyTicketV1 ticket{};
    if (g_shared == nullptr || provider == nullptr
        || (provider->provider_flags
                & BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION) == 0
        || contract.dependency_nodes == NIL
        || list_length(contract.dependency_nodes) <= 0
        || list_length(contract.dependency_nodes)
            > static_cast<int>(kMaximumRelationDependencies)
        || pg_atomic_read_u32(&g_shared->coarse_transition_count) != 0) {
        return ticket;
    }
    std::array<Oid, kMaximumRelationDependencies> relation_oids{};
    std::size_t relation_count = 0;
    ListCell* cell = nullptr;
    foreach(cell, contract.dependency_nodes) {
        const auto* node = static_cast<const Node*>(lfirst(cell));
        if (node == nullptr || !IsA(node, Integer)) {
            return ticket;
        }
        const Oid relation_oid = static_cast<Oid>(
            static_cast<std::uint32_t>(intVal(node)));
        if (!OidIsValid(relation_oid)) {
            return ticket;
        }
        bool duplicate = false;
        for (std::size_t index = 0; index < relation_count; ++index) {
            duplicate = duplicate || relation_oids[index] == relation_oid;
        }
        if (!duplicate) {
            relation_oids[relation_count++] = relation_oid;
        }
    }
    if (relation_count != 1) {
        return ticket;
    }
    std::sort(relation_oids.begin(), relation_oids.begin() + relation_count);
    if (relation_physical_identity(
            relation_oids[0], &ticket.physical_identity)
            != RelationPhysicalIdentityStatusV1::Precise) {
        return ticket;
    }
    if (current_transaction_touches_dependency(
            relation_oids[0], ticket.physical_identity)) {
        return ticket;
    }
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    const bool dependency_provisioned =
        ensure_relation_epoch_locked(relation_oids[0]) != nullptr;
    LWLockRelease(g_registry_lock);
    if (!dependency_provisioned) {
        return ticket;
    }

    std::uint64_t coarse_epoch = 0;
    std::uint64_t relation_epoch = 0;
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    if (database_has_prepared_xacts_locked(MyDatabaseId)
        || (required_completion_count != 0
            && TransamVariables->xactCompletionCount
                != required_completion_count)) {
        LWLockRelease(ProcArrayLock);
        return ticket;
    }
    LWLockAcquire(g_registry_lock, LW_SHARED);
    const RelationEpochSlot* dependency = find_relation_epoch_locked(
        relation_oids[0]);
    const bool dependency_stable = dependency != nullptr
        && dependency->transition_count == 0
        && pg_atomic_read_u32(&g_shared->coarse_transition_count) == 0;
    if (dependency_stable) {
        coarse_epoch = pg_atomic_read_u64(
            &g_shared->coarse_dependency_epoch);
        relation_epoch = dependency->committed_epoch;
    }
    const bool snapshot_stable = required_completion_count == 0
        || TransamVariables->xactCompletionCount
            == required_completion_count;
    LWLockRelease(g_registry_lock);
    LWLockRelease(ProcArrayLock);
    if (!dependency_stable || !snapshot_stable
        || coarse_epoch == 0 || relation_epoch == 0) {
        return ticket;
    }

    std::array<std::byte,
        128U + kMaximumRelationDependencies
            * (sizeof(Oid) + sizeof(std::uint64_t))> bytes{};
    std::size_t offset = 0;
    const auto append = [&bytes, &offset](
            const void* source, std::size_t size) {
        if (source == nullptr || offset > bytes.size()
            || size > bytes.size() - offset) {
            return false;
        }
        std::memcpy(bytes.data() + offset, source, size);
        offset += size;
        return true;
    };
    const std::uint32_t version = 1;
    const std::uint32_t database_oid = MyDatabaseId;
    const std::uint32_t dependency_count = relation_count;
    if (!append(&version, sizeof(version))
        || !append(&database_oid, sizeof(database_oid))
        || !append(&coarse_epoch, sizeof(coarse_epoch))
        || !append(&dependency_count, sizeof(dependency_count))) {
        return ticket;
    }
    for (std::size_t index = 0; index < relation_count; ++index) {
        if (relation_epoch == 0
            || !append(&relation_oids[index], sizeof(relation_oids[index]))
            || !append(&relation_epoch, sizeof(relation_epoch))) {
            return ticket;
        }
    }
    const std::uint32_t identity_kind = static_cast<std::uint32_t>(
        ticket.physical_identity.kind);
    if (!append(&identity_kind, sizeof(identity_kind))
        || !append(
            &ticket.physical_identity.length,
            sizeof(ticket.physical_identity.length))
        || !append(
            ticket.physical_identity.bytes.data(),
            ticket.physical_identity.length)) {
        return ticket;
    }
    PostgreSqlColumnChecksumPolicy checksum;
    const auto digest = checksum.sha256(std::span<const std::byte>{
        bytes.data(), offset});
    if (!digest.has_value()) {
        return ticket;
    }
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    LWLockAcquire(g_registry_lock, LW_SHARED);
    dependency = find_relation_epoch_locked(relation_oids[0]);
    const bool still_stable = dependency != nullptr
        && dependency->transition_count == 0
        && dependency->committed_epoch == relation_epoch
        && pg_atomic_read_u32(&g_shared->coarse_transition_count) == 0
        && pg_atomic_read_u64(&g_shared->coarse_dependency_epoch)
            == coarse_epoch
        && !database_has_prepared_xacts_locked(MyDatabaseId)
        && (required_completion_count == 0
            || TransamVariables->xactCompletionCount
                == required_completion_count);
    LWLockRelease(g_registry_lock);
    LWLockRelease(ProcArrayLock);
    if (!still_stable || current_transaction_touches_dependency(
            relation_oids[0], ticket.physical_identity)) {
        return ticket;
    }
    ticket.fingerprint = *digest;
    ticket.version = DependencyVersion{
        .key = {
            .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
            .class_oid = static_cast<std::uint32_t>(RelationRelationId),
            .object_oid = static_cast<std::uint32_t>(relation_oids[0]),
            .kind = DependencyKind::RelationContent,
        },
        .committed_epoch = relation_epoch,
        .physical_identity = ticket.physical_identity,
        .observability = WriteObservability::ProviderComplete,
    };
    ticket.dependency_count = dependency_count;
    ticket.observability = WriteObservability::ProviderComplete;
    ticket.valid = true;
    return ticket;
}

[[nodiscard]] ConditionVariable* reusable_generation_changed(
        const ReusableGenerationReferenceV1& reference) noexcept {
    return reference
            && g_reusable_wait_slots != nullptr
            && reference.generation_slot < g_reusable_wait_slot_count
        ? &g_reusable_wait_slots[reference.generation_slot]
        : nullptr;
}

void broadcast_reusable_generation(
        const ReusableGenerationReferenceV1& reference) noexcept {
    if (ConditionVariable* changed = reusable_generation_changed(reference);
        changed != nullptr) {
        ConditionVariableBroadcast(changed);
    }
}

/** Commit-time invalidation is bounded, ABA-safe, and wakes after lock drop. */
void invalidate_dirty_reusable_generations() noexcept {
    if (MyDatabaseId == InvalidOid) {
        return;
    }
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr) {
        /*
         * A dirty preloaded backend should have bound during write
         * observation.  Failing closed here prevents stale COMPLETE replay if
         * that invariant is ever broken without attempting an unsafe attach
         * from XACT_EVENT_COMMIT.
         */
        if (guc::preload_requested() && g_shared != nullptr
            && !g_write_invalidation_deferred_to_emergency
            && shared_registry::shared_registry_bootstrap_state()
                == BootstrapState::Ready
            && transaction_is_dirty()) {
            mark_shared_cache_failed();
        }
        return;
    }
    std::array<PhysicalIdentityV1, kMaximumRelationDependencies> identities{};
    std::size_t identity_count = 0;
    bool coarse = g_force_coarse_transition || g_dirty_observations.coarse();
    bool storage_parent_seen = false;
    std::array<DirtyRelationObservationV1,
        kMaximumRelationDependencies> observations{};
    const std::size_t observation_count =
        g_dirty_observations.copy_unique(observations);
    for (std::size_t index = 0; index < observation_count; ++index) {
        const DirtyRelationObservationV1& observation = observations[index];
        if (!observation.physical_identity_valid) {
            if (observation.storage_parent) {
                storage_parent_seen = true;
            } else {
                coarse = true;
            }
            continue;
        }
        bool duplicate = false;
        for (std::size_t prior = 0; prior < identity_count; ++prior) {
            duplicate = duplicate
                || identities[prior] == observation.physical_identity;
        }
        if (duplicate) {
            continue;
        }
        if (identity_count >= identities.size()) {
            coarse = true;
            break;
        }
        identities[identity_count++] = observation.physical_identity;
    }
    coarse = coarse || storage_parent_seen;
    if (!coarse && identity_count == 0) {
        return;
    }

    std::array<ReusableGenerationReferenceV1,
        kDefaultReusableRegistryGenerationsV1> affected{};
    std::size_t affected_count = 0;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool invalidation_complete = true;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        const auto collect = [&affected, &affected_count](
                const ReusableInvalidationResultV1& result) {
            for (std::size_t index = 0;
                 index < result.affected_count; ++index) {
                const ReusableGenerationReferenceV1& reference =
                    result.affected_references[index];
                bool duplicate = false;
                for (std::size_t prior = 0;
                     prior < affected_count; ++prior) {
                    duplicate = duplicate || affected[prior] == reference;
                }
                if (!duplicate && affected_count < affected.size()) {
                    affected[affected_count++] = reference;
                }
            }
        };
        const auto run_selector = [&collect](
                ReusableInvalidationSelectorV1 selector) {
            for (;;) {
                const ReusableInvalidationResultV1 result =
                    invalidate_reusable_generations(
                        g_reusable_registry,
                        selector,
                        reusable_partition_callbacks());
                if (result.error != ReusableRegistryErrorV1::None) {
                    return false;
                }
                collect(result);
                if (result.done) {
                    return true;
                }
                if (result.next_cursor <= selector.cursor) {
                    return false;
                }
                selector.cursor = result.next_cursor;
            }
        };

        bool precise_ok = true;
        if (!coarse) {
            for (std::size_t index = 0;
                 precise_ok && index < identity_count; ++index) {
                precise_ok = run_selector(ReusableInvalidationSelectorV1{
                    .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
                    .physical_identity = identities[index],
                    .match_physical_identity = true,
                });
            }
            if (precise_ok) {
                precise_ok = run_selector(
                    ReusableInvalidationSelectorV1{
                        .database_oid = static_cast<std::uint32_t>(
                            MyDatabaseId),
                        .observability =
                            WriteObservability::DatabaseWideFallback,
                        .match_observability = true,
                    });
            }
        }
        if (coarse || !precise_ok) {
            invalidation_complete = run_selector(ReusableInvalidationSelectorV1{
                .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
                .coarse_database_fallback = true,
            });
        } else {
            invalidation_complete = precise_ok;
        }
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        FlushErrorState();
        affected_count = 0;
        invalidation_complete = false;
    }
    PG_END_TRY();
    for (std::size_t index = 0; index < affected_count; ++index) {
        broadcast_reusable_generation(affected[index]);
    }
    if (!invalidation_complete && g_shared != nullptr) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        ConditionVariableBroadcast(&g_shared->database_changed);
    }
}

/** Membership mutations are centralized for lock-order and future owner tokens. */
[[nodiscard]] ReusableClaimResultV1 claim_current_reusable_generation(
        const ReusableClaimRequestV1& request) noexcept {
    ReusableClaimResultV1 result{};
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        result = claim_reusable_generation(
            g_reusable_registry,
            request,
            reusable_partition_callbacks());
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        result = ReusableClaimResultV1{
            .error = ReusableRegistryErrorV1::InvalidState};
    }
    PG_END_TRY();
    const std::size_t retired_count = std::min<std::size_t>(
        result.retired_manifest_count,
        result.retired_manifests.size());
    for (std::size_t index = 0; index < retired_count; ++index) {
        const storage::ManifestHandle manifest = storage_manifest_handle(
            result.retired_manifests[index]);
        if (manifest) {
            static_cast<void>(storage::retire_manifest(manifest));
        }
    }
    if (result.retired_manifest_count > result.retired_manifests.size()
        || result.reserved != 0) {
        if (g_shared != nullptr) {
            pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
            pg_atomic_write_u32(&g_shared->ready, 0);
        }
        result.error = ReusableRegistryErrorV1::CorruptGeneration;
        result.disposition = ReusableClaimDispositionV1::Rejected;
    }
    return result;
}

struct RuntimeDependencyClaimContextV1 {
    ReusableClaimRequestV1 request{};
    ReusableClaimResultV1 claim{};
    bool rolled_back = false;
    bool wake_generation = false;
};

void mark_shared_cache_failed() noexcept {
    if (g_shared != nullptr) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
    }
}

/** Dependency -> membership -> partition is the single claim lock order. */
bool apply_runtime_dependency_claim(
        void* opaque,
        std::span<const DependencyVectorEntryV1> entries) noexcept {
    auto* context = static_cast<RuntimeDependencyClaimContextV1*>(opaque);
    if (context == nullptr
        || entries.size() != context->request.metadata.dependency_count
        || context->request.dependency_vector_envelope.empty()) {
        return false;
    }
    LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
    context->claim = claim_reusable_generation(
        g_reusable_registry,
        context->request,
        reusable_partition_callbacks());
    LWLockRelease(g_reusable_membership_lock);
    return static_cast<bool>(context->claim);
}

/** Undo pins or fail a newly linked producer after a locked recheck fails. */
void rollback_runtime_dependency_claim(void* opaque) noexcept {
    auto* context = static_cast<RuntimeDependencyClaimContextV1*>(opaque);
    if (context == nullptr || !context->claim) {
        return;
    }
    ReusableRegistryErrorV1 error = ReusableRegistryErrorV1::None;
    LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
    if (context->claim.disposition
            == ReusableClaimDispositionV1::ClaimedProducer) {
        error = fail_reusable_generation(
            g_reusable_registry,
            context->claim.reference,
            CacheFailureCode::DependencyChanged,
            reusable_partition_callbacks());
        context->wake_generation = error == ReusableRegistryErrorV1::None;
    } else {
        error = release_reusable_reference(
            g_reusable_registry,
            context->claim.token,
            reusable_partition_callbacks());
    }
    LWLockRelease(g_reusable_membership_lock);
    context->rolled_back = true;
    if (error != ReusableRegistryErrorV1::None) {
        mark_shared_cache_failed();
    }
}

void retire_runtime_claim_manifests(
        const ReusableClaimResultV1& claim) noexcept {
    const std::size_t count = std::min<std::size_t>(
        claim.retired_manifest_count, claim.retired_manifests.size());
    for (std::size_t index = 0; index < count; ++index) {
        const storage::ManifestHandle manifest = storage_manifest_handle(
            claim.retired_manifests[index]);
        if (manifest) {
            static_cast<void>(storage::retire_manifest(manifest));
        }
    }
    if (claim.retired_manifest_count > claim.retired_manifests.size()
        || claim.reserved != 0) {
        mark_shared_cache_failed();
    }
}

/** Atomically hand one canonical dependency capability into the registry. */
[[nodiscard]] ReusableClaimResultV1
claim_reusable_generation_with_dependencies(
        ReusableClaimRequestV1 request,
        std::span<const DependencyVersion> dependencies,
        std::uint64_t snapshot_binding_completion_count,
        const DependencyKey* conservative_guard_key,
        CacheDigest256* canonical_dependency_fingerprint = nullptr) noexcept {
    ReusableClaimResultV1 rejected{
        .error = ReusableRegistryErrorV1::InvalidState,
    };
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr
        || conservative_guard_key == nullptr
        || dependencies.size() > kMaxDependenciesPerGeneration) {
        return rejected;
    }
    dependency_runtime::DependencySnapshotTicketV1 ticket{};
    dependency_runtime::DependencyGenerationVectorV1 generation{};
    const bool requires_completion_guard =
        (conservative_guard_key != nullptr
            && database_guard_requires_completion_v1(
                *conservative_guard_key))
        || std::any_of(
            dependencies.begin(), dependencies.end(),
            [](const DependencyVersion& dependency) noexcept {
                return dependency.observability
                    == WriteObservability::DatabaseWideFallback;
            });
    const dependency_runtime::DependencyRuntimeResultV1 captured =
        dependency_runtime::capture_snapshot_ticket(
            dependencies,
            &ticket,
            conservative_guard_key,
            snapshot_binding_completion_count,
            requires_completion_guard
                ? snapshot_binding_completion_count : 0);
    if (!captured) {
        return rejected;
    }
    const dependency_runtime::DependencyRuntimeResultV1 retained =
        dependency_runtime::retain_generation_vector_preserving_ticket(
            ticket, &generation);
    if (!retained) {
        static_cast<void>(
            dependency_runtime::release_snapshot_ticket(&ticket));
        return rejected;
    }
    const std::span<const std::byte> envelope =
        dependency_runtime::dependency_generation_vector_envelope(
            generation);
    const bool has_conservative_guard =
        ticket.explicit_completion_guard;
    const std::span<const DependencyVectorEntryV1> guard{
        &ticket.conservative_guard_entry, 1};
    const std::size_t fingerprint_material_bytes =
        dependency_fingerprint_material_bytes(
            ticket.count, has_conservative_guard);
    auto* fingerprint_material = fingerprint_material_bytes == 0
            || fingerprint_material_bytes
                > static_cast<std::size_t>(MaxAllocSize)
        ? nullptr
        : static_cast<std::byte*>(palloc_extended(
            fingerprint_material_bytes, MCXT_ALLOC_NO_OOM));
    PostgreSqlColumnChecksumPolicy checksum_policy;
    std::optional<CacheDigest256> fingerprint;
    if (fingerprint_material != nullptr) {
        const std::span<std::byte> material{
            fingerprint_material, fingerprint_material_bytes};
        const auto initialized = initialize_dependency_fingerprint_material(
            material, envelope, guard);
        if (initialized) {
            const auto hashed = checksum_policy.sha256(material);
            if (hashed.has_value()) {
                fingerprint = *hashed;
            }
        }
        pfree(fingerprint_material);
    }
    if (envelope.empty() || !fingerprint.has_value()
        || !has_conservative_guard
        || ticket.count != dependencies.size()
        || ticket.count != generation.count
        || generation.explicit_completion_guard != has_conservative_guard
        || generation.conservative_guard_entry
            != ticket.conservative_guard_entry
        || generation.snapshot_xact_completion_count
            != ticket.snapshot_xact_completion_count) {
        static_cast<void>(
            dependency_runtime::release_generation_vector(&generation));
        static_cast<void>(
            dependency_runtime::release_snapshot_ticket(&ticket));
        return rejected;
    }
    request.dependency_vector_envelope = envelope;
    request.conservative_guard = ReusableConservativeGuardV1{
        .dependency = ticket.conservative_guard_entry,
        .snapshot_completion_count =
            ticket.snapshot_xact_completion_count,
        .active = true,
    };
    request.metadata.dependency_count = ticket.count;
    request.metadata.dependency_fingerprint = *fingerprint;
    if (canonical_dependency_fingerprint != nullptr) {
        *canonical_dependency_fingerprint = *fingerprint;
    }
    RuntimeDependencyClaimContextV1 context{
        .request = request,
    };
    const dependency_runtime::DependencyRuntimeResultV1 validated =
        dependency_runtime::validate_snapshot_ticket_locked(
            ticket,
            dependency_runtime::DependencyTicketLockedActionV1{
                .context = &context,
                .apply = apply_runtime_dependency_claim,
                .rollback = rollback_runtime_dependency_claim,
            });
    const dependency_runtime::DependencyRuntimeResultV1 ticket_released =
        dependency_runtime::release_snapshot_ticket(&ticket);
    const bool new_generation = context.claim.disposition
        == ReusableClaimDispositionV1::ClaimedProducer;
    dependency_runtime::DependencyRuntimeResultV1 vector_released{};
    if (new_generation) {
        vector_released =
            dependency_runtime::transfer_generation_vector_to_registry(
                &generation);
    } else {
        vector_released = dependency_runtime::release_generation_vector(
            &generation);
    }
    retire_runtime_claim_manifests(context.claim);

    const bool handoff_valid = validated && ticket_released
        && vector_released && !context.rolled_back;
    if (handoff_valid) {
        return context.claim;
    }
    if (!context.rolled_back && context.claim) {
        rollback_runtime_dependency_claim(&context);
    }
    if (context.wake_generation) {
        broadcast_reusable_generation(context.claim.reference);
    }
    if (!ticket_released || !vector_released) {
        mark_shared_cache_failed();
    }
    return rejected;
}

struct RuntimeDependencyPublishContextV1 {
    ReusablePublishRequestV1 request{};
    CacheEntryKind expected_kind = CacheEntryKind::ReusableCoverage;
    ReusableProducerIdentityV1 expected_producer{};
    std::uint32_t expected_dependency_count = 0;
    ReusableRegistryErrorV1 registry_result =
        ReusableRegistryErrorV1::InvalidState;
    ReusableRegistryErrorV1 rollback_result =
        ReusableRegistryErrorV1::None;
    bool action_applied = false;
    bool rolled_back = false;
};

static_assert(
    kReusableReferenceOwnersPerGenerationV1
    <= storage::kWaitersPerManifest);

[[nodiscard]] storage::StorageBackendIdentity storage_waiter_owner(
        const ReusableReferenceTokenV1& waiter) noexcept {
    if (!waiter.active || waiter.kind != ReusableReferenceKindV1::Waiter
        || !waiter.owner.valid()) {
        return {};
    }
    return storage::StorageBackendIdentity{
        .proc_number = waiter.owner.proc_number,
        .pid = waiter.owner.pid,
        .backend_nonce = waiter.owner.backend_nonce,
        .backend_started_at_us = waiter.owner.backend_started_at_us,
    };
}

[[nodiscard]] bool waiter_batch_contains(
        const ReusableWaiterOwnerBatchV1& batch,
        const ReusableReferenceTokenV1& waiter) noexcept {
    for (std::uint32_t index = 0; index < batch.count; ++index) {
        if (batch.waiters[index] == waiter) {
            return true;
        }
    }
    return false;
}

/** Transfer every still-live registry waiter before COMPLETE is visible. */
[[nodiscard]] bool register_runtime_manifest_waiters(
        const ReusableGenerationReferenceV1& reference,
        const storage::ManifestHandle& manifest,
        std::span<storage::ManifestWaiterToken> registered) noexcept {
    if (!reference || !manifest
        || registered.size()
            < kReusableReferenceOwnersPerGenerationV1) {
        return false;
    }
    const ReusableWaiterOwnerBatchV1 before =
        snapshot_reusable_waiter_owners(
            g_reusable_registry, reference,
            reusable_partition_callbacks());
    if (!before) {
        return false;
    }
    std::uint32_t registered_count = 0;
    bool complete = true;
    for (std::uint32_t index = 0; index < before.count; ++index) {
        const storage::StorageBackendIdentity owner =
            storage_waiter_owner(before.waiters[index]);
        const storage::RegisterManifestWaiterResult waiter =
            storage::register_manifest_waiter(manifest, owner);
        if (!waiter) {
            complete = false;
            break;
        }
        registered[index] = waiter.token;
        ++registered_count;
    }

    ReusableWaiterOwnerBatchV1 after{};
    if (complete) {
        after = snapshot_reusable_waiter_owners(
            g_reusable_registry, reference,
            reusable_partition_callbacks());
        complete = static_cast<bool>(after);
    }
    if (complete) {
        for (std::uint32_t index = 0; index < before.count; ++index) {
            if (waiter_batch_contains(after, before.waiters[index])) {
                continue;
            }
            static_cast<void>(storage::unregister_manifest_waiter(
                registered[index]));
            registered[index] = {};
        }
        for (std::uint32_t index = 0; index < after.count; ++index) {
            bool transferred = false;
            for (std::uint32_t prior = 0;
                 prior < before.count; ++prior) {
                transferred = transferred
                    || (after.waiters[index] == before.waiters[prior]
                        && static_cast<bool>(registered[prior]));
            }
            if (!transferred) {
                complete = false;
                break;
            }
        }
    }
    if (!complete) {
        for (std::uint32_t index = 0;
             index < registered_count; ++index) {
            if (registered[index]) {
                static_cast<void>(storage::unregister_manifest_waiter(
                    registered[index]));
                registered[index] = {};
            }
        }
    }
    return complete;
}

/** Dependency -> membership -> partition terminal publication. */
bool apply_runtime_dependency_publish(
        void* opaque,
        std::span<const DependencyVectorEntryV1> entries) noexcept {
    auto* context = static_cast<RuntimeDependencyPublishContextV1*>(opaque);
    if (context == nullptr || !context->request.reference
        || !context->request.manifest || !context->expected_producer.valid()
        || entries.size() != context->expected_dependency_count
        || g_reusable_membership_lock == nullptr) {
        return false;
    }
    LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
    std::array<storage::ManifestWaiterToken,
        kReusableReferenceOwnersPerGenerationV1> registered_waiters{};
    const auto current = wait_reusable_generation(
        g_reusable_registry,
        context->request.reference,
        reusable_partition_callbacks());
    if (current.has_value()
        && current->reference == context->request.reference
        && current->entry_kind == context->expected_kind
        && current->state == CacheEntryState::Building
        && current->producer == context->expected_producer
        && current->metadata.dependency_count
            == context->expected_dependency_count) {
        const bool waiters_registered = register_runtime_manifest_waiters(
            context->request.reference,
            storage_manifest_handle(context->request.manifest),
            registered_waiters);
        if (waiters_registered) {
            context->registry_result = publish_reusable_generation(
                g_reusable_registry,
                context->request,
                reusable_partition_callbacks());
            context->action_applied = context->registry_result
                == ReusableRegistryErrorV1::None;
        }
    }
    if (!context->action_applied) {
        for (storage::ManifestWaiterToken& waiter : registered_waiters) {
            if (waiter) {
                static_cast<void>(
                    storage::unregister_manifest_waiter(waiter));
                waiter = {};
            }
        }
    }
    LWLockRelease(g_reusable_membership_lock);
    return context->action_applied;
}

/** An emergency-epoch race after COMPLETE invalidates registry ownership. */
void rollback_runtime_dependency_publish(void* opaque) noexcept {
    auto* context = static_cast<RuntimeDependencyPublishContextV1*>(opaque);
    if (context == nullptr || !context->action_applied
        || g_reusable_membership_lock == nullptr) {
        return;
    }
    LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
    context->rollback_result = invalidate_reusable_generation(
        g_reusable_registry,
        context->request.reference,
        CacheFailureCode::DependencyChanged,
        reusable_partition_callbacks());
    LWLockRelease(g_reusable_membership_lock);
    context->rolled_back = true;
    if (context->rollback_result != ReusableRegistryErrorV1::None) {
        mark_shared_cache_failed();
    }
}

/** Validate the immutable vector borrowed through a live generation reference. */
[[nodiscard]] dependency_runtime::DependencyRuntimeResultV1
validate_reusable_generation_dependencies_locked(
        const ReusableGenerationSnapshotV1& snapshot,
        std::uint64_t snapshot_binding_completion_count,
        dependency_runtime::DependencyTicketLockedActionV1 action = {},
        std::uint64_t continuing_completion_count_override = 0)
        noexcept {
    dependency_runtime::DependencyRuntimeResultV1 rejected{
        .error = dependency_runtime::DependencyRuntimeErrorV1::InvalidArgument,
        .fail_open = true,
    };
    if (!snapshot.reference || snapshot_binding_completion_count == 0
        || snapshot.metadata.dependency_count
            > kMaxDependenciesPerGeneration
        || !g_reusable_registry
        || !snapshot.conservative_guard) {
        return rejected;
    }
    const std::span<const std::byte> envelope = reusable_shared_bytes(
        g_reusable_registry, snapshot.dependency_vector);
    if (envelope.empty()) {
        return rejected;
    }
    const DependencyVectorEntryV1* guard =
        &snapshot.conservative_guard.dependency;
    const bool in_flight = snapshot.state == CacheEntryState::Claimed
        || snapshot.state == CacheEntryState::Building;
    const std::uint64_t continuing_completion_count = in_flight
        ? continuing_completion_count_override != 0
            ? continuing_completion_count_override
            : snapshot.conservative_guard.snapshot_completion_count
        : 0;
    dependency_runtime::DependencyRuntimeResultV1 result =
        dependency_runtime::validate_dependency_vector_locked(
            envelope,
            guard,
            snapshot_binding_completion_count,
            continuing_completion_count,
            action);
    if (result
        && result.dependency_count != snapshot.metadata.dependency_count) {
        result.error =
            dependency_runtime::DependencyRuntimeErrorV1::VectorValidationFailed;
        result.fail_open = true;
    }
    return result;
}

[[nodiscard]] ReusableRegistryErrorV1 publish_current_reusable_generation(
        const ReusablePublishRequestV1& request) noexcept {
    ReusableRegistryErrorV1 result = ReusableRegistryErrorV1::InvalidState;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        result = publish_reusable_generation(
            g_reusable_registry,
            request,
            reusable_partition_callbacks());
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        result = ReusableRegistryErrorV1::InvalidState;
    }
    PG_END_TRY();
    if (result == ReusableRegistryErrorV1::None) {
        broadcast_reusable_generation(request.reference);
    }
    return result;
}

[[nodiscard]] ReusableRegistryErrorV1 begin_current_reusable_generation(
        const ReusableDiagnosticTextUpdateV1& diagnostics) noexcept {
    if (!diagnostics.reference || g_reusable_membership_lock == nullptr) {
        return ReusableRegistryErrorV1::InvalidArgument;
    }
    ReusableRegistryErrorV1 result = ReusableRegistryErrorV1::InvalidState;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        result = begin_reusable_build_with_diagnostics(
            g_reusable_registry,
            diagnostics,
            reusable_partition_callbacks());
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        result = ReusableRegistryErrorV1::InvalidState;
    }
    PG_END_TRY();
    if (result == ReusableRegistryErrorV1::None) {
        broadcast_reusable_generation(diagnostics.reference);
    }
    return result;
}

/** ABA-refresh producer diagnostics while membership remains stable. */
[[nodiscard]] ReusableRegistryErrorV1 store_current_reusable_diagnostics(
        const ReusableDiagnosticTextUpdateV1& diagnostics) noexcept {
    if (!diagnostics.reference || g_reusable_membership_lock == nullptr) {
        return ReusableRegistryErrorV1::InvalidArgument;
    }
    ReusableRegistryErrorV1 result = ReusableRegistryErrorV1::InvalidState;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        result = store_reusable_diagnostic_text(
            g_reusable_registry,
            diagnostics,
            reusable_partition_callbacks());
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        result = ReusableRegistryErrorV1::InvalidState;
    }
    PG_END_TRY();
    return result;
}

void fail_current_reusable_generation(
        const ReusableGenerationReferenceV1& reference,
        CacheFailureCode failure) noexcept {
    if (!reference || g_reusable_membership_lock == nullptr) {
        return;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool failed = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        failed = fail_reusable_generation(
            g_reusable_registry,
            reference,
            failure,
            reusable_partition_callbacks())
            == ReusableRegistryErrorV1::None;
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
    }
    PG_END_TRY();
    if (failed) {
        broadcast_reusable_generation(reference);
    }
}

[[nodiscard]] Const* make_binary_node(
        std::span<const std::byte> bytes) {
    if (bytes.empty() || bytes.size() > static_cast<std::size_t>(MaxAllocSize)) {
        return nullptr;
    }
    auto* value = static_cast<bytea*>(palloc(VARHDRSZ + bytes.size()));
    SET_VARSIZE(value, VARHDRSZ + bytes.size());
    std::memcpy(VARDATA(value), bytes.data(), bytes.size());
    return makeConst(
        BYTEAOID,
        -1,
        InvalidOid,
        -1,
        PointerGetDatum(value),
        false,
        false);
}

[[nodiscard]] std::span<const std::byte> bytea_payload(
        const bytea* value) noexcept {
    if (value == nullptr || VARSIZE_ANY_EXHDR(value) == 0) {
        return {};
    }
    return std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(VARDATA_ANY(value)),
        static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value)),
    };
}

[[nodiscard]] bytea* copy_bytea_payload(
        std::span<const std::byte> bytes) {
    if (bytes.empty()
        || bytes.size() > static_cast<std::size_t>(MaxAllocSize - VARHDRSZ)) {
        return nullptr;
    }
    auto* result = static_cast<bytea*>(palloc(VARHDRSZ + bytes.size()));
    SET_VARSIZE(result, VARHDRSZ + bytes.size());
    std::memcpy(VARDATA(result), bytes.data(), bytes.size());
    return result;
}

/** Persist current exact-generation identity for executor-time claiming. */
[[nodiscard]] Const* make_current_coverage_plan_node(
        const provider_registry::ProviderV2& provider,
        const BioseqCachePathDomainV2& domain,
        const PlannerSemanticKeysV1& keys,
        const bytea* descriptor) {
    const std::span<const std::byte> domain_key =
        bytea_payload(keys.canonical_domain_key);
    const std::span<const std::byte> generation_key =
        bytea_payload(keys.canonical_generation_key);
    const std::span<const std::byte> descriptor_bytes =
        bytea_payload(descriptor);
    if (domain_key.empty() || generation_key.empty()
        || descriptor_bytes.empty()
        || domain_key.size() > (std::numeric_limits<std::uint32_t>::max)()
        || generation_key.size() > (std::numeric_limits<std::uint32_t>::max)()
        || descriptor_bytes.size()
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return nullptr;
    }
    const CoverageCurrentPlanHeaderV1 header{
        .provider_id = provider.provider_id,
        .provider_abi_version = provider.abi_version,
        .compatibility_family_id = domain.compatibility_family_id,
        .capability_mask = domain.capability_mask,
        .coverage_directory_key = {
            .security_namespace = keys.security_namespace,
            .coverage_domain_digest = keys.domain_digest,
        },
        .generation_digest = keys.generation_digest,
        .canonical_domain_key_bytes = static_cast<std::uint32_t>(
            domain_key.size()),
        .canonical_generation_key_bytes = static_cast<std::uint32_t>(
            generation_key.size()),
        .coverage_descriptor_bytes = static_cast<std::uint32_t>(
            descriptor_bytes.size()),
    };
    std::uint64_t total = sizeof(header);
    if (!add_coverage_descriptor_size(total, 1, domain_key.size())
        || !add_coverage_descriptor_size(total, 1, generation_key.size())
        || !add_coverage_descriptor_size(total, 1, descriptor_bytes.size())) {
        return nullptr;
    }
    auto* packed = static_cast<std::byte*>(palloc(total));
    std::size_t offset = 0;
    const auto append = [packed, &offset](
            const void* source, std::size_t size) {
        std::memcpy(packed + offset, source, size);
        offset += size;
    };
    append(&header, sizeof(header));
    append(domain_key.data(), domain_key.size());
    append(generation_key.data(), generation_key.size());
    append(descriptor_bytes.data(), descriptor_bytes.size());
    return make_binary_node(
        std::span<const std::byte>{packed, static_cast<std::size_t>(total)});
}

inline constexpr std::uint32_t kSourceMappingPlanMagicV1 = 0x42534d31U;
inline constexpr std::uint32_t kSourceMappingPlanVersionV1 = 1;
inline constexpr std::size_t kSourceMappingEntryNodeCountV1 = 9;
inline constexpr std::uint32_t kSourceSchemaFingerprintTagV1 = 0x534d5331U;
inline constexpr std::uint32_t kConsumerContractFingerprintTagV1 = 0x534d4331U;
inline constexpr std::uint32_t kDecoratedSchemaFingerprintTagV1 = 0x534d4431U;
inline constexpr std::uint32_t kSourceMapFingerprintTagV1 = 0x534d4d31U;

struct DecodedSourceMappingPlanV1 {
    BioseqCacheColumnMapEntryV2* boundary_map = nullptr;
    BioseqCacheColumnMapEntryV2* lineage_map = nullptr;
    std::uint32_t boundary_map_count = 0;
    std::uint32_t lineage_map_count = 0;
    CacheDigest256 source_fingerprint{};
    CacheDigest256 consumer_fingerprint{};
    CacheDigest256 decorated_fingerprint{};
    CacheDigest256 map_fingerprint{};
};

void destroy_decoded_source_mapping_plan(
        DecodedSourceMappingPlanV1& plan) noexcept {
    if (plan.boundary_map != nullptr) {
        pfree(plan.boundary_map);
    }
    if (plan.lineage_map != nullptr) {
        pfree(plan.lineage_map);
    }
    plan = {};
}

[[nodiscard]] std::span<const std::byte> source_mapping_const_payload(
        const void* node) noexcept {
    if (node == nullptr || !IsA(node, Const)) {
        return {};
    }
    const auto* constant = static_cast<const Const*>(node);
    if (constant->constisnull || constant->consttype != BYTEAOID
        || constant->constlen != -1 || constant->consttypmod != -1
        || constant->constcollid != InvalidOid || constant->constbyval
        || constant->constvalue == static_cast<Datum>(0)) {
        return {};
    }
    const auto* value = reinterpret_cast<const bytea*>(
        DatumGetPointer(constant->constvalue));
    if (value == nullptr || VARSIZE_ANY_EXHDR(value) == 0) {
        return {};
    }
    return std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(VARDATA_ANY(value)),
        static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value)),
    };
}

/** NIL is the provider-independent sentinel; otherwise node zero is version. */
[[nodiscard]] bool source_mapping_private_version(
        const List* nodes,
        std::uint32_t* version) noexcept {
    if (version == nullptr) {
        return false;
    }
    *version = 0;
    if (nodes == nullptr) {
        return true;
    }
    const int count = list_length(nodes);
    if (count <= 0
        || static_cast<std::uint32_t>(count)
            > kMaxProviderBoundaryColumns
        || !IsA(list_nth(nodes, 0), Integer)) {
        return false;
    }
    const int raw_version = intVal(list_nth(nodes, 0));
    if (raw_version <= 0) {
        return false;
    }
    *version = static_cast<std::uint32_t>(raw_version);
    return true;
}

/** Canonicalize the opaque tail under the same generic Node byte budget. */
[[nodiscard]] bool append_source_mapping_private_bytes(
        std::vector<std::byte>& destination,
        const List* nodes) {
    std::uint32_t version = 0;
    if (!source_mapping_private_version(nodes, &version)) {
        return false;
    }
    append_little_endian(destination, nodes == nullptr
        ? std::uint32_t{0} : std::uint32_t{1});
    append_little_endian(destination, version);
    append_little_endian(destination, static_cast<std::uint32_t>(
        nodes == nullptr ? 0 : list_length(nodes)));
    if (nodes == nullptr) {
        append_little_endian(destination, std::uint64_t{0});
        return true;
    }

    MemoryContext caller_context = CurrentMemoryContext;
    volatile MemoryContext disposable = nullptr;
    volatile char* copied = nullptr;
    volatile std::size_t copied_size = 0;
    PG_TRY();
    {
        disposable = AllocSetContextCreate(
            caller_context,
            "bioseq cache mapping private canonicalization",
            ALLOCSET_SMALL_SIZES);
        MemoryContextSwitchTo(const_cast<MemoryContext>(disposable));
        List* tail = list_copy_tail(const_cast<List*>(nodes), 1);
        char* serialized = nodeToString(tail);
        const std::size_t byte_count = serialized == nullptr
            ? static_cast<std::size_t>(kMaxProviderRecipeBytes) + 1U
            : strnlen(
                serialized,
                static_cast<std::size_t>(kMaxProviderRecipeBytes) + 1U);
        if (byte_count <= kMaxProviderRecipeBytes) {
            MemoryContextSwitchTo(caller_context);
            copied = pnstrdup(serialized, byte_count);
            copied_size = byte_count;
        }
        MemoryContextSwitchTo(caller_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (disposable != nullptr) {
            MemoryContextDelete(const_cast<MemoryContext>(disposable));
            disposable = nullptr;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        copied = nullptr;
        copied_size = 0;
    }
    PG_END_TRY();
    if (disposable != nullptr) {
        MemoryContextDelete(const_cast<MemoryContext>(disposable));
    }
    if (copied == nullptr) {
        return false;
    }
    append_little_endian(
        destination, static_cast<std::uint64_t>(copied_size));
    const auto* first = reinterpret_cast<const std::byte*>(
        const_cast<const char*>(copied));
    destination.insert(destination.end(), first, first + copied_size);
    pfree(const_cast<char*>(copied));
    return destination.size() <= kMaxProviderRecipeBytes;
}

void append_source_mapping_schema_columns(
        std::vector<std::byte>& destination,
        const BioseqCacheColumnSchemaV2* columns,
        std::size_t count) {
    append_little_endian(destination, static_cast<std::uint32_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        const BioseqCacheColumnSchemaV2& column = columns[index];
        append_little_endian(destination, static_cast<std::uint32_t>(
            sizeof(BioseqCacheColumnSchemaV2)));
        append_little_endian(destination, column.attno);
        append_little_endian(destination, column.semantic_id_high);
        append_little_endian(destination, column.semantic_id_low);
        append_little_endian(destination, column.type_oid);
        append_little_endian(destination, column.typmod);
        append_little_endian(destination, column.collation_oid);
        append_little_endian(destination, column.flags);
    }
}

void append_source_mapping_required_attnos(
        std::vector<std::byte>& destination,
        const std::uint32_t* attnos,
        std::size_t count) {
    append_little_endian(destination, static_cast<std::uint32_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        append_little_endian(destination, attnos[index]);
    }
}

[[nodiscard]] std::optional<CacheDigest256> source_mapping_digest(
        const std::vector<std::byte>& bytes) noexcept {
    if (bytes.empty()) {
        return std::nullopt;
    }
    PostgreSqlColumnChecksumPolicy policy;
    const auto digest = policy.sha256(bytes);
    return digest.has_value()
        ? std::optional<CacheDigest256>{*digest}
        : std::nullopt;
}

[[nodiscard]] std::optional<CacheDigest256>
fingerprint_source_mapping_contract(
        const BioseqCacheReusableSourceV2& source) noexcept {
    const ProviderSchemaView boundary{
        .columns = source.boundary_columns,
        .column_count = source.boundary_column_count,
    };
    const ProviderSchemaView lineage{
        .columns = source.lineage_columns,
        .column_count = source.lineage_column_count,
    };
    std::uint32_t private_version = 0;
    if (!validate_provider_schema(boundary).has_value()
        || !validate_provider_schema(lineage).has_value()
        || !source_mapping_private_version(
            source.provider_private_nodes, &private_version)) {
        return std::nullopt;
    }
    try {
        std::vector<std::byte> bytes;
        append_little_endian(bytes, kSourceSchemaFingerprintTagV1);
        append_little_endian(bytes, kSourceMappingPlanVersionV1);
        append_little_endian(bytes, source.provider_id);
        append_little_endian(bytes, source.compatibility_family_id);
        append_little_endian(bytes, source.capability_mask);
        append_source_mapping_schema_columns(
            bytes, source.boundary_columns, source.boundary_column_count);
        append_source_mapping_schema_columns(
            bytes, source.lineage_columns, source.lineage_column_count);
        append_source_mapping_required_attnos(bytes, nullptr, 0);
        append_source_mapping_required_attnos(bytes, nullptr, 0);
        append_little_endian(bytes, private_version);
        return source_mapping_digest(bytes);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<CacheDigest256>
fingerprint_consumer_mapping_contract(
        const BioseqCachePathDomainV2& consumer) noexcept {
    const ProviderSchemaView boundary{
        .columns = consumer.boundary_columns,
        .column_count = consumer.boundary_column_count,
        .required_attnos = consumer.required_boundary_attnos,
        .required_attno_count = consumer.required_boundary_attno_count,
    };
    const ProviderSchemaView lineage{
        .columns = consumer.lineage_columns,
        .column_count = consumer.lineage_column_count,
        .required_attnos = consumer.required_lineage_attnos,
        .required_attno_count = consumer.required_lineage_attno_count,
    };
    std::uint32_t private_version = 0;
    if (!validate_provider_schema(boundary).has_value()
        || !validate_provider_schema(lineage).has_value()
        || !source_mapping_private_version(
            consumer.provider_private_nodes, &private_version)) {
        return std::nullopt;
    }
    try {
        std::vector<std::byte> bytes;
        append_little_endian(bytes, kConsumerContractFingerprintTagV1);
        append_little_endian(bytes, kSourceMappingPlanVersionV1);
        append_little_endian(bytes, consumer.provider_id);
        append_little_endian(bytes, consumer.compatibility_family_id);
        append_little_endian(bytes, consumer.capability_mask);
        append_source_mapping_schema_columns(
            bytes, consumer.boundary_columns, consumer.boundary_column_count);
        append_source_mapping_schema_columns(
            bytes, consumer.lineage_columns, consumer.lineage_column_count);
        append_source_mapping_required_attnos(
            bytes,
            consumer.required_boundary_attnos,
            consumer.required_boundary_attno_count);
        append_source_mapping_required_attnos(
            bytes,
            consumer.required_lineage_attnos,
            consumer.required_lineage_attno_count);
        append_little_endian(bytes, private_version);
        return source_mapping_digest(bytes);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] std::optional<CacheDigest256>
fingerprint_decorated_mapping_contract(
        const BioseqCachePathDomainV2& consumer,
        const BioseqCacheColumnSchemaV2* boundary_columns,
        std::size_t boundary_column_count,
        const BioseqCacheColumnSchemaV2* lineage_columns,
        std::size_t lineage_column_count,
        const List* provider_private_nodes) noexcept {
    const ProviderSchemaView boundary{
        .columns = boundary_columns,
        .column_count = boundary_column_count,
    };
    const ProviderSchemaView lineage{
        .columns = lineage_columns,
        .column_count = lineage_column_count,
    };
    std::uint32_t private_version = 0;
    if (!validate_provider_schema(boundary).has_value()
        || !validate_provider_schema(lineage).has_value()
        || !source_mapping_private_version(
            provider_private_nodes, &private_version)) {
        return std::nullopt;
    }
    try {
        std::vector<std::byte> bytes;
        append_little_endian(bytes, kDecoratedSchemaFingerprintTagV1);
        append_little_endian(bytes, kSourceMappingPlanVersionV1);
        append_little_endian(bytes, consumer.provider_id);
        append_little_endian(bytes, consumer.compatibility_family_id);
        append_little_endian(bytes, consumer.capability_mask);
        append_source_mapping_schema_columns(
            bytes, boundary_columns, boundary_column_count);
        append_source_mapping_schema_columns(
            bytes, lineage_columns, lineage_column_count);
        append_source_mapping_required_attnos(bytes, nullptr, 0);
        append_source_mapping_required_attnos(bytes, nullptr, 0);
        append_little_endian(bytes, private_version);
        return source_mapping_digest(bytes);
    } catch (...) {
        return std::nullopt;
    }
}

void append_source_mapping_entry_bytes(
        std::vector<std::byte>& destination,
        const BioseqCacheColumnMapEntryV2& entry) {
    append_little_endian(destination, static_cast<std::uint32_t>(
        sizeof(BioseqCacheColumnMapEntryV2)));
    append_little_endian(destination, entry.consumer_attno);
    append_little_endian(destination, entry.source_attno);
    append_little_endian(destination, entry.flags);
    append_little_endian(destination, entry.semantic_id_high);
    append_little_endian(destination, entry.semantic_id_low);
    append_little_endian(destination, entry.type_oid);
    append_little_endian(destination, entry.typmod);
    append_little_endian(destination, entry.collation_oid);
    append_little_endian(destination, entry.reserved);
}

[[nodiscard]] std::optional<CacheDigest256> fingerprint_source_mapping(
        const BioseqCacheSourceMappingV2& mapping) noexcept {
    if (mapping.boundary_map_count > kMaxProviderBoundaryColumns
        || mapping.lineage_map_count > kMaxProviderBoundaryColumns
        || (mapping.boundary_map_count != 0
            && mapping.boundary_map == nullptr)
        || (mapping.lineage_map_count != 0
            && mapping.lineage_map == nullptr)) {
        return std::nullopt;
    }
    try {
        std::vector<std::byte> bytes;
        append_little_endian(bytes, kSourceMapFingerprintTagV1);
        append_little_endian(bytes, kSourceMappingPlanVersionV1);
        append_little_endian(bytes, static_cast<std::uint32_t>(
            mapping.boundary_map_count));
        append_little_endian(bytes, static_cast<std::uint32_t>(
            mapping.lineage_map_count));
        for (std::size_t index = 0;
             index < mapping.boundary_map_count;
             ++index) {
            append_source_mapping_entry_bytes(bytes, mapping.boundary_map[index]);
        }
        for (std::size_t index = 0;
             index < mapping.lineage_map_count;
             ++index) {
            append_source_mapping_entry_bytes(bytes, mapping.lineage_map[index]);
        }
        if (!append_source_mapping_private_bytes(
                bytes, mapping.provider_private_nodes)) {
            return std::nullopt;
        }
        return source_mapping_digest(bytes);
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] bool source_mapping_private_versions_match(
        const BioseqCachePathDomainV2& consumer,
        const BioseqCacheReusableSourceV2& source,
        const List* decorated_private,
        const List* mapping_private) noexcept {
    std::uint32_t consumer_version = 0;
    std::uint32_t source_version = 0;
    std::uint32_t decorated_version = 0;
    std::uint32_t mapping_version = 0;
    return source_mapping_private_version(
            consumer.provider_private_nodes, &consumer_version)
        && source_mapping_private_version(
            source.provider_private_nodes, &source_version)
        && source_mapping_private_version(
            decorated_private, &decorated_version)
        && source_mapping_private_version(mapping_private, &mapping_version)
        && consumer_version == source_version
        && consumer_version == decorated_version
        && (mapping_private == nullptr
            || (consumer_version != 0
                && mapping_version == consumer_version));
}

[[nodiscard]] List* make_source_mapping_entry_node(
        const BioseqCacheColumnMapEntryV2& entry) {
    std::vector<std::byte> semantic;
    semantic.reserve(16);
    append_little_endian(semantic, entry.semantic_id_high);
    append_little_endian(semantic, entry.semantic_id_low);
    Const* semantic_node = make_binary_node(semantic);
    if (semantic_node == nullptr) {
        return nullptr;
    }
    List* result = list_make4(
        makeInteger(static_cast<int>(sizeof(BioseqCacheColumnMapEntryV2))),
        makeInteger(static_cast<std::int32_t>(entry.consumer_attno)),
        makeInteger(static_cast<std::int32_t>(entry.source_attno)),
        makeInteger(static_cast<std::int32_t>(entry.flags)));
    result = lappend(result, semantic_node);
    result = lappend(result,
        makeInteger(static_cast<std::int32_t>(entry.type_oid)));
    result = lappend(result, makeInteger(entry.typmod));
    result = lappend(result,
        makeInteger(static_cast<std::int32_t>(entry.collation_oid)));
    return lappend(result,
        makeInteger(static_cast<std::int32_t>(entry.reserved)));
}

/** Build the exact fixed-shape, self-authenticating mapping plan List. */
[[nodiscard]] List* make_source_mapping_plan_node(
        const BioseqCachePathDomainV2& consumer,
        const BioseqCacheReusableSourceV2& source,
        const BioseqCacheDecoratedPathV2& decorated,
        const BioseqCacheSourceMappingV2& mapping) {
    if (!validate_source_map(
            ProviderContractView{
                .boundary = {
                    .columns = consumer.boundary_columns,
                    .column_count = consumer.boundary_column_count,
                    .required_attnos = consumer.required_boundary_attnos,
                    .required_attno_count = consumer.required_boundary_attno_count,
                },
                .lineage = {
                    .columns = consumer.lineage_columns,
                    .column_count = consumer.lineage_column_count,
                    .required_attnos = consumer.required_lineage_attnos,
                    .required_attno_count = consumer.required_lineage_attno_count,
                },
                .capability_mask = consumer.capability_mask,
            },
            ProviderContractView{
                .boundary = {
                    .columns = source.boundary_columns,
                    .column_count = source.boundary_column_count,
                },
                .lineage = {
                    .columns = source.lineage_columns,
                    .column_count = source.lineage_column_count,
                },
                .capability_mask = source.capability_mask,
            },
            mapping).has_value()
        || !reuse::validate_decorated_path(&consumer, &decorated).has_value()
        || !source_mapping_private_versions_match(
            consumer,
            source,
            decorated.provider_private_nodes,
            mapping.provider_private_nodes)) {
        return nullptr;
    }
    const auto source_fingerprint =
        fingerprint_source_mapping_contract(source);
    const auto consumer_fingerprint =
        fingerprint_consumer_mapping_contract(consumer);
    const auto decorated_fingerprint =
        fingerprint_decorated_mapping_contract(
            consumer,
            decorated.actual_boundary_columns,
            decorated.actual_boundary_column_count,
            decorated.actual_lineage_columns,
            decorated.actual_lineage_column_count,
            decorated.provider_private_nodes);
    const auto map_fingerprint = fingerprint_source_mapping(mapping);
    if (!source_fingerprint.has_value()
        || !consumer_fingerprint.has_value()
        || !decorated_fingerprint.has_value()
        || !map_fingerprint.has_value()) {
        return nullptr;
    }
    List* result = list_make4(
        makeInteger(static_cast<std::int32_t>(kSourceMappingPlanMagicV1)),
        makeInteger(static_cast<int>(kSourceMappingPlanVersionV1)),
        makeInteger(static_cast<int>(mapping.boundary_map_count)),
        makeInteger(static_cast<int>(mapping.lineage_map_count)));
    for (std::size_t index = 0;
         index < mapping.boundary_map_count;
         ++index) {
        List* entry = make_source_mapping_entry_node(mapping.boundary_map[index]);
        if (entry == nullptr) {
            return nullptr;
        }
        result = lappend(result, entry);
    }
    for (std::size_t index = 0;
         index < mapping.lineage_map_count;
         ++index) {
        List* entry = make_source_mapping_entry_node(mapping.lineage_map[index]);
        if (entry == nullptr) {
            return nullptr;
        }
        result = lappend(result, entry);
    }
    const std::array<CacheDigest256, 4> fingerprints{
        *source_fingerprint,
        *consumer_fingerprint,
        *decorated_fingerprint,
        *map_fingerprint,
    };
    for (const CacheDigest256& fingerprint : fingerprints) {
        Const* node = make_binary_node(std::as_bytes(
            std::span<const std::uint8_t>{
                fingerprint.bytes.data(), fingerprint.bytes.size()}));
        if (node == nullptr) {
            return nullptr;
        }
        result = lappend(result, node);
    }
    return result;
}

[[nodiscard]] bool decode_source_mapping_entry_node(
        const void* node,
        BioseqCacheColumnMapEntryV2* output) noexcept {
    if (node == nullptr || output == nullptr || !IsA(node, List)) {
        return false;
    }
    const auto* fields = static_cast<const List*>(node);
    if (list_length(fields)
            != static_cast<int>(kSourceMappingEntryNodeCountV1)) {
        return false;
    }
    constexpr std::array<int, 8> integer_fields{0, 1, 2, 3, 5, 6, 7, 8};
    for (const int index : integer_fields) {
        if (!IsA(list_nth(fields, index), Integer)) {
            return false;
        }
    }
    const int struct_size = intVal(list_nth(fields, 0));
    const int consumer_attno = intVal(list_nth(fields, 1));
    const int source_attno = intVal(list_nth(fields, 2));
    const std::uint32_t flags = static_cast<std::uint32_t>(
        intVal(list_nth(fields, 3)));
    const std::uint32_t type_oid = static_cast<std::uint32_t>(
        intVal(list_nth(fields, 5)));
    const int typmod = intVal(list_nth(fields, 6));
    const std::uint32_t collation_oid = static_cast<std::uint32_t>(
        intVal(list_nth(fields, 7)));
    const std::uint32_t reserved = static_cast<std::uint32_t>(
        intVal(list_nth(fields, 8)));
    const auto semantic = source_mapping_const_payload(
        list_nth(fields, 4));
    if (struct_size != static_cast<int>(sizeof(BioseqCacheColumnMapEntryV2))
        || consumer_attno <= 0 || source_attno <= 0
        || flags != 0 || reserved != 0 || !OidIsValid(type_oid)
        || semantic.size() != 16) {
        return false;
    }
    std::size_t cursor = 0;
    const auto semantic_high = read_little_endian<std::uint64_t>(
        semantic, cursor);
    const auto semantic_low = read_little_endian<std::uint64_t>(
        semantic, cursor);
    if (!semantic_high.has_value() || !semantic_low.has_value()
        || cursor != semantic.size()) {
        return false;
    }
    *output = BioseqCacheColumnMapEntryV2{
        .struct_size = sizeof(BioseqCacheColumnMapEntryV2),
        .consumer_attno = static_cast<std::uint32_t>(consumer_attno),
        .source_attno = static_cast<std::uint32_t>(source_attno),
        .flags = flags,
        .semantic_id_high = *semantic_high,
        .semantic_id_low = *semantic_low,
        .type_oid = type_oid,
        .typmod = typmod,
        .collation_oid = collation_oid,
        .reserved = reserved,
    };
    return true;
}

/** Strictly decode fixed tags, lengths, counts, entry Nodes, and digests. */
[[nodiscard]] bool decode_source_mapping_plan_node(
        const void* node,
        DecodedSourceMappingPlanV1* output) {
    if (node == nullptr || output == nullptr || !IsA(node, List)) {
        return false;
    }
    const auto* plan = static_cast<const List*>(node);
    if (list_length(plan) < 8
        || !IsA(list_nth(plan, 0), Integer)
        || !IsA(list_nth(plan, 1), Integer)
        || !IsA(list_nth(plan, 2), Integer)
        || !IsA(list_nth(plan, 3), Integer)
        || static_cast<std::uint32_t>(intVal(list_nth(plan, 0)))
            != kSourceMappingPlanMagicV1
        || intVal(list_nth(plan, 1))
            != static_cast<int>(kSourceMappingPlanVersionV1)) {
        return false;
    }
    const int boundary_count = intVal(list_nth(plan, 2));
    const int lineage_count = intVal(list_nth(plan, 3));
    if (boundary_count < 0 || lineage_count < 0
        || static_cast<std::uint32_t>(boundary_count)
            > kMaxProviderBoundaryColumns
        || static_cast<std::uint32_t>(lineage_count)
            > kMaxProviderBoundaryColumns
        || list_length(plan) != 8 + boundary_count + lineage_count) {
        return false;
    }

    DecodedSourceMappingPlanV1 decoded{};
    decoded.boundary_map_count = static_cast<std::uint32_t>(boundary_count);
    decoded.lineage_map_count = static_cast<std::uint32_t>(lineage_count);
    if (boundary_count != 0) {
        decoded.boundary_map = static_cast<BioseqCacheColumnMapEntryV2*>(
            palloc0(sizeof(BioseqCacheColumnMapEntryV2)
                * static_cast<std::size_t>(boundary_count)));
    }
    if (lineage_count != 0) {
        decoded.lineage_map = static_cast<BioseqCacheColumnMapEntryV2*>(
            palloc0(sizeof(BioseqCacheColumnMapEntryV2)
                * static_cast<std::size_t>(lineage_count)));
    }
    int cursor = 4;
    for (int index = 0; index < boundary_count; ++index) {
        if (!decode_source_mapping_entry_node(
                list_nth(plan, cursor++), &decoded.boundary_map[index])) {
            destroy_decoded_source_mapping_plan(decoded);
            return false;
        }
    }
    for (int index = 0; index < lineage_count; ++index) {
        if (!decode_source_mapping_entry_node(
                list_nth(plan, cursor++), &decoded.lineage_map[index])) {
            destroy_decoded_source_mapping_plan(decoded);
            return false;
        }
    }
    std::array<CacheDigest256*, 4> fingerprints{
        &decoded.source_fingerprint,
        &decoded.consumer_fingerprint,
        &decoded.decorated_fingerprint,
        &decoded.map_fingerprint,
    };
    for (CacheDigest256* fingerprint : fingerprints) {
        const auto bytes = source_mapping_const_payload(
            list_nth(plan, cursor++));
        if (bytes.size() != fingerprint->bytes.size()) {
            destroy_decoded_source_mapping_plan(decoded);
            return false;
        }
        std::memcpy(
            fingerprint->bytes.data(), bytes.data(), bytes.size());
    }
    *output = decoded;
    return true;
}

/** Recompute every fingerprint and the generic semantic map validator. */
[[nodiscard]] bool validate_source_mapping_plan_node(
        const void* node,
        const BioseqCachePathDomainV2& consumer,
        const BioseqCacheReusableSourceV2& source,
        const BioseqCacheColumnSchemaV2* decorated_boundary_columns,
        std::size_t decorated_boundary_column_count,
        const BioseqCacheColumnSchemaV2* decorated_lineage_columns,
        std::size_t decorated_lineage_column_count,
        const List* decorated_private_nodes,
        const void* mapping_private_node,
        DecodedSourceMappingPlanV1* output) {
    if (output == nullptr
        || (mapping_private_node != nullptr
            && !IsA(mapping_private_node, List))) {
        return false;
    }
    const auto* mapping_private = static_cast<const List*>(
        mapping_private_node);
    DecodedSourceMappingPlanV1 decoded{};
    if (!decode_source_mapping_plan_node(node, &decoded)) {
        return false;
    }
    const BioseqCacheSourceMappingV2 mapping{
        .struct_size = sizeof(BioseqCacheSourceMappingV2),
        .flags = 0,
        .boundary_map = decoded.boundary_map,
        .boundary_map_count = decoded.boundary_map_count,
        .lineage_map = decoded.lineage_map,
        .lineage_map_count = decoded.lineage_map_count,
        .provider_private_nodes = const_cast<List*>(mapping_private),
    };
    const ProviderContractView consumer_contract{
        .boundary = {
            .columns = consumer.boundary_columns,
            .column_count = consumer.boundary_column_count,
            .required_attnos = consumer.required_boundary_attnos,
            .required_attno_count = consumer.required_boundary_attno_count,
        },
        .lineage = {
            .columns = consumer.lineage_columns,
            .column_count = consumer.lineage_column_count,
            .required_attnos = consumer.required_lineage_attnos,
            .required_attno_count = consumer.required_lineage_attno_count,
        },
        .capability_mask = consumer.capability_mask,
    };
    const ProviderContractView source_contract{
        .boundary = {
            .columns = source.boundary_columns,
            .column_count = source.boundary_column_count,
        },
        .lineage = {
            .columns = source.lineage_columns,
            .column_count = source.lineage_column_count,
        },
        .capability_mask = source.capability_mask,
    };
    const auto source_fingerprint =
        fingerprint_source_mapping_contract(source);
    const auto consumer_fingerprint =
        fingerprint_consumer_mapping_contract(consumer);
    const auto decorated_fingerprint =
        fingerprint_decorated_mapping_contract(
            consumer,
            decorated_boundary_columns,
            decorated_boundary_column_count,
            decorated_lineage_columns,
            decorated_lineage_column_count,
            decorated_private_nodes);
    const auto map_fingerprint = fingerprint_source_mapping(mapping);
    const bool valid = validate_source_map(
            consumer_contract, source_contract, mapping).has_value()
        && source_mapping_private_versions_match(
            consumer,
            source,
            decorated_private_nodes,
            mapping_private)
        && source_fingerprint.has_value()
        && consumer_fingerprint.has_value()
        && decorated_fingerprint.has_value()
        && map_fingerprint.has_value()
        && decoded.source_fingerprint == *source_fingerprint
        && decoded.consumer_fingerprint == *consumer_fingerprint
        && decoded.decorated_fingerprint == *decorated_fingerprint
        && decoded.map_fingerprint == *map_fingerprint;
    if (!valid) {
        destroy_decoded_source_mapping_plan(decoded);
        return false;
    }
    *output = decoded;
    return true;
}

struct PlannerPinReleaseV1 {
    ReusableReferenceTokenV1 token{};
};

struct PlannerCandidateBytesV1 {
    bytea* canonical_domain_key = nullptr;
    bytea* canonical_generation_key = nullptr;
    bytea* coverage_descriptor = nullptr;
};

struct PlannerMappedSourceV1 {
    Const* reference_node = nullptr;
    Const* generation_key_node = nullptr;
    Const* descriptor_node = nullptr;
    BioseqCacheReusableSourceV2 source{};
    BioseqCacheSourceMappingV2 mapping{};
    Expr* residual_predicate = nullptr;
    Expr* replay_predicate = nullptr;
    int gate_param_id = 0;
};

enum class CoveragePredicateInstanceRoleV1 : std::uint8_t {
    Residual = 1,
    Replay = 2,
};

struct CoveragePredicateRebindV1 {
    const BioseqCachePathDomainV2* source = nullptr;
    const BioseqCachePathDomainV2* consumer = nullptr;
    Index consumer_relation_index = 0;
    CoveragePredicateInstanceRoleV1 role =
        CoveragePredicateInstanceRoleV1::Residual;
    bool valid = true;
};

struct PlannerCoveragePredicateInstancesV1 {
    Expr* residual = nullptr;
    Expr* replay = nullptr;
};

/** Resolve exactly one consumer column with the provider's source meaning. */
[[nodiscard]] const BioseqCacheColumnSchemaV2*
coverage_consumer_column_for_source(
        const BioseqCachePathDomainV2& consumer,
        const BioseqCacheColumnSchemaV2& source) noexcept {
    const BioseqCacheColumnSchemaV2* match = nullptr;
    for (std::size_t index = 0;
         index < consumer.boundary_column_count;
         ++index) {
        const BioseqCacheColumnSchemaV2& candidate =
            consumer.boundary_columns[index];
        if (candidate.semantic_id_high != source.semantic_id_high
            || candidate.semantic_id_low != source.semantic_id_low
            || candidate.type_oid != source.type_oid
            || candidate.typmod != source.typmod
            || candidate.collation_oid != source.collation_oid
            || candidate.flags != source.flags) {
            continue;
        }
        if (match != nullptr && match->attno != candidate.attno) {
            return nullptr;
        }
        match = &candidate;
    }
    return match;
}

/** Dense replay attno is the rank of one real attno in sorted schema order. */
[[nodiscard]] AttrNumber coverage_synthetic_attno(
        const BioseqCachePathDomainV2& domain,
        std::uint32_t consumer_attno) noexcept {
    if (consumer_attno == 0
        || coverage_schema_column(
            domain.boundary_columns,
            domain.boundary_column_count,
            consumer_attno) == nullptr) {
        return InvalidAttrNumber;
    }
    std::size_t rank = 1;
    for (std::size_t index = 0;
         index < domain.boundary_column_count;
         ++index) {
        if (domain.boundary_columns[index].attno < consumer_attno) {
            ++rank;
        }
    }
    return rank <= static_cast<std::size_t>(MaxTupleAttributeNumber)
        ? static_cast<AttrNumber>(rank)
        : InvalidAttrNumber;
}

/** Deep-copy one exact sentinel into either executable binding role. */
Node* rebind_coverage_predicate_mutator(Node* node, void* opaque) {
    if (node == nullptr) {
        return nullptr;
    }
    auto* context = static_cast<CoveragePredicateRebindV1*>(opaque);
    if (context == nullptr || !context->valid
        || context->source == nullptr || context->consumer == nullptr) {
        return copyObject(node);
    }
    if (!IsA(node, Var)) {
        return expression_tree_mutator(
            node, rebind_coverage_predicate_mutator, opaque);
    }

    const auto* variable = reinterpret_cast<const Var*>(node);
    const auto* source_column = variable->varattno > 0
        ? coverage_schema_column(
            context->source->boundary_columns,
            context->source->boundary_column_count,
            static_cast<std::uint32_t>(variable->varattno))
        : nullptr;
    const auto* consumer_column = source_column == nullptr
        ? nullptr
        : coverage_consumer_column_for_source(
            *context->consumer, *source_column);
    const AttrNumber synthetic_attno = consumer_column == nullptr
        ? InvalidAttrNumber
        : coverage_synthetic_attno(
            *context->consumer, consumer_column->attno);
    if (variable->varno != INDEX_VAR
        || variable->varnosyn != INDEX_VAR
        || variable->varattnosyn != variable->varattno
        || variable->varlevelsup != 0
        || !bms_is_empty(variable->varnullingrels)
        || source_column == nullptr || consumer_column == nullptr
        || synthetic_attno == InvalidAttrNumber
        || variable->vartype != source_column->type_oid
        || variable->vartypmod != source_column->typmod
        || variable->varcollid != source_column->collation_oid) {
        context->valid = false;
        return copyObject(node);
    }
    auto* rebound = reinterpret_cast<Var*>(copyObject(node));
    if (context->role == CoveragePredicateInstanceRoleV1::Residual) {
        rebound->varno = context->consumer_relation_index;
        rebound->varnosyn = context->consumer_relation_index;
        rebound->varattno = static_cast<AttrNumber>(consumer_column->attno);
        rebound->varattnosyn = static_cast<AttrNumber>(
            consumer_column->attno);
    } else {
        rebound->varno = INDEX_VAR;
        rebound->varnosyn = INDEX_VAR;
        rebound->varattno = synthetic_attno;
        rebound->varattnosyn = synthetic_attno;
    }
    return reinterpret_cast<Node*>(rebound);
}

/** Instantiate independent residual and replay copies from immutable wire P. */
[[nodiscard]] bool instantiate_coverage_predicates(
        const BioseqCachePathDomainV2& source,
        const reuse::CoverageFragmentBindingSignatureV1& source_binding,
        const BioseqCachePathDomainV2& consumer,
        const reuse::CoverageFragmentBindingSignatureV1& consumer_binding,
        const Expr* normalized_predicate,
        Index consumer_relation_index,
        MemoryContext planner_context,
        PlannerCoveragePredicateInstancesV1* output) {
    if (normalized_predicate == nullptr || consumer_relation_index == 0
        || IS_SPECIAL_VARNO(consumer_relation_index)
        || planner_context == nullptr || output == nullptr
        || source_binding != consumer_binding
        || !reuse::valid_coverage_fragment_binding(source_binding)
        || !normalized_coverage_predicate_valid(
            source, normalized_predicate)) {
        return false;
    }
    CoveragePredicateRebindV1 residual_context{
        .source = &source,
        .consumer = &consumer,
        .consumer_relation_index = consumer_relation_index,
        .role = CoveragePredicateInstanceRoleV1::Residual,
    };
    CoveragePredicateRebindV1 replay_context{
        .source = &source,
        .consumer = &consumer,
        .consumer_relation_index = consumer_relation_index,
        .role = CoveragePredicateInstanceRoleV1::Replay,
    };
    MemoryContext previous = MemoryContextSwitchTo(planner_context);
    auto* residual = reinterpret_cast<Expr*>(
        rebind_coverage_predicate_mutator(
            reinterpret_cast<Node*>(const_cast<Expr*>(normalized_predicate)),
            &residual_context));
    auto* replay = reinterpret_cast<Expr*>(
        rebind_coverage_predicate_mutator(
            reinterpret_cast<Node*>(const_cast<Expr*>(normalized_predicate)),
            &replay_context));
    MemoryContextSwitchTo(previous);
    if (!residual_context.valid || !replay_context.valid
        || residual == nullptr || replay == nullptr
        || residual == replay) {
        return false;
    }
    *output = PlannerCoveragePredicateInstancesV1{
        .residual = residual,
        .replay = replay,
    };
    return true;
}

/** Planner-context callback target; idempotent token clearing prevents leaks. */
void release_planner_pin_noerror(
        void* opaque, const CacheEntryId&,
        reuse::CandidateReferenceKindV1 kind) noexcept {
    auto* owner = static_cast<PlannerPinReleaseV1*>(opaque);
    if (owner == nullptr
        || kind != reuse::CandidateReferenceKindV1::ProvisionalPin
        || !owner->token.active || !g_reusable_registry) {
        return;
    }
    static_cast<void>(release_reusable_reference(
        g_reusable_registry,
        owner->token,
        reusable_partition_callbacks()));
}

[[nodiscard]] const CustomPathMethods& coverage_path_methods();

/**
 * Build one coverage wrapper from a provider-owned path.  Candidate pins are
 * copied into planner-context cleanup records before any provider callback;
 * every callback then releases its pin after copying validated output.
 */
[[nodiscard]] bool wrap_one_provider_path(
        PlannerInfo* root,
        RelOptInfo* relation,
        Path* provider_path,
        const provider_registry::ProviderV2* provider,
        const BioseqCacheQueryIntentV2& intent,
        MemoryContext planner_context) {
    reuse::ProviderCallDiagnosticV1 diagnostic{};
    reuse::PlannerOwnedDomainV1 described{};
    if (!reuse::invoke_describe_path_fail_open(
            provider,
            root,
            relation,
            provider_path,
            &intent,
            planner_context,
            &described,
            &diagnostic)) {
        return false;
    }
    const auto binding_base = coverage_fragment_binding_base(
        root, relation, provider->provider_id);
    Expr* normalized_predicate = normalize_coverage_predicate_fail_open(
        described.value, relation->relid, planner_context);
    if (!binding_base.has_value() || normalized_predicate == nullptr) {
        return false;
    }
    described.value.coverage_predicate = normalized_predicate;
    PlannerSemanticKeysV1 keys{};
    if (!capture_planner_semantic_keys_fail_open(
            provider,
            described.value,
            *binding_base,
            planner_context,
            &keys)) {
        return false;
    }
    PlannerCoveragePredicateInstancesV1 current_predicates{};
    if (!instantiate_coverage_predicates(
            described.value,
            keys.binding,
            described.value,
            keys.binding,
            described.value.coverage_predicate,
            relation->relid,
            planner_context,
            &current_predicates)) {
        return false;
    }
    bytea* descriptor = serialize_coverage_descriptor_fail_open(
        described.value, keys.binding, planner_context);
    Const* current_plan = descriptor == nullptr
        ? nullptr
        : make_current_coverage_plan_node(
            *provider, described.value, keys, descriptor);
    if (current_plan == nullptr) {
        return false;
    }

    auto* release_owners = static_cast<PlannerPinReleaseV1*>(
        MemoryContextAllocZero(
            planner_context,
            sizeof(PlannerPinReleaseV1)
                * kMaxCoverageGenerationsPerDomain));
    auto* release_records = static_cast<
        reuse::PlannerCandidateCleanupRecordV1*>(MemoryContextAllocZero(
            planner_context,
            sizeof(reuse::PlannerCandidateCleanupRecordV1)
                * kMaxCoverageGenerationsPerDomain));
    for (std::size_t index = 0;
         index < kMaxCoverageGenerationsPerDomain;
         ++index) {
        if (reuse::register_candidate_context_cleanup(
                planner_context, &release_records[index]) == nullptr) {
            return false;
        }
    }

    const ReusableCandidateRequestV1 request{
        .coverage_directory_key = {
            .security_namespace = keys.security_namespace,
            .coverage_domain_digest = keys.domain_digest,
        },
        .canonical_domain_key = bytea_payload(keys.canonical_domain_key),
        .reference_owner = current_reusable_producer_identity(),
        .provider_id = provider->provider_id,
        .compatibility_family_id = described.value.compatibility_family_id,
        .required_capability_mask = described.value.capability_mask,
    };
    auto* candidate_bytes = static_cast<PlannerCandidateBytesV1*>(
        MemoryContextAllocZero(
            planner_context,
            sizeof(PlannerCandidateBytesV1)
                * kMaxCoverageGenerationsPerDomain));
    auto* candidates = static_cast<ReusableCandidateBatchV1*>(
        MemoryContextAllocZero(
            planner_context, sizeof(ReusableCandidateBatchV1)));
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool snapshot_valid = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
        membership_owned = true;
        auto candidate_result = snapshot_reusable_candidates(
            g_reusable_registry, request, reusable_partition_callbacks());
        if (candidate_result.has_value()) {
            *candidates = *candidate_result;
            snapshot_valid = true;
        }
        for (std::size_t index = 0;
             snapshot_valid && index < candidates->candidate_count;
             ++index) {
            release_owners[index].token =
                candidates->candidates[index].planner_pin;
            candidates->candidates[index].planner_pin.active = false;
            if (!reuse::activate_candidate_reference(
                    release_records[index],
                    candidates->candidates[index].generation.reference.id,
                    reuse::CandidateReferenceKindV1::ProvisionalPin,
                    &release_planner_pin_noerror,
                    &release_owners[index])) {
                static_cast<void>(release_reusable_reference(
                    g_reusable_registry,
                    release_owners[index].token,
                    reusable_partition_callbacks()));
                snapshot_valid = false;
                break;
            }
        }
        MemoryContextSwitchTo(planner_context);
        for (std::size_t index = 0;
             snapshot_valid && index < candidates->candidate_count;
             ++index) {
            const ReusableGenerationSnapshotV1& generation =
                candidates->candidates[index].generation;
            candidate_bytes[index].canonical_domain_key = copy_bytea_payload(
                reusable_shared_bytes(
                    g_reusable_registry,
                    generation.canonical_domain_key));
            candidate_bytes[index].canonical_generation_key =
                copy_bytea_payload(reusable_shared_bytes(
                    g_reusable_registry,
                    generation.canonical_generation_key));
            candidate_bytes[index].coverage_descriptor = copy_bytea_payload(
                reusable_shared_bytes(
                    g_reusable_registry,
                    generation.coverage_descriptor));
            snapshot_valid =
                candidate_bytes[index].canonical_domain_key != nullptr
                && candidate_bytes[index].canonical_generation_key != nullptr
                && candidate_bytes[index].coverage_descriptor != nullptr;
        }
        MemoryContextSwitchTo(caller_context);
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        for (std::size_t index = 0;
             index < candidates->candidate_count;
             ++index) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
        }
        static_cast<void>(release_reusable_candidate_batch(
            g_reusable_registry,
            *candidates,
            reusable_partition_callbacks()));
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        snapshot_valid = false;
    }
    PG_END_TRY();
    if (!snapshot_valid) {
        for (std::size_t index = 0;
             index < candidates->candidate_count;
             ++index) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
        }
        static_cast<void>(release_reusable_candidate_batch(
            g_reusable_registry,
            *candidates,
            reusable_partition_callbacks()));
        return false;
    }

    std::array<reuse::CoverageCandidateViewV1,
        kMaxCoverageGenerationsPerDomain> candidate_views{};
    for (std::size_t index = 0; index < candidates->candidate_count; ++index) {
        const ReusableCandidateSnapshotV1& candidate =
            candidates->candidates[index];
        const auto domain_bytes = bytea_payload(
            candidate_bytes[index].canonical_domain_key);
        const auto generation_bytes = bytea_payload(
            candidate_bytes[index].canonical_generation_key);
        const bool is_current_exact = reuse::exact_current_key_equal(
            keys.generation_digest,
            bytea_payload(keys.canonical_generation_key),
            candidate.generation.reference.id.digest,
            generation_bytes);
        candidate_views[index] = reuse::CoverageCandidateViewV1{
            .entry_id = candidate.generation.reference.id,
            .domain_digest = candidate.generation.coverage_directory_key
                .coverage_domain_digest,
            .canonical_domain_key = domain_bytes,
            .canonical_generation_key = generation_bytes,
            .security_namespace = planner_security_namespace(
                candidate.generation.coverage_directory_key
                    .security_namespace),
            .provider_id = candidate.generation.metadata.provider_id,
            .compatibility_family_id = candidate.generation.metadata
                .compatibility_family_id,
            .capability_mask = candidate.generation.metadata.capability_mask,
            .complete = candidate.complete && !is_current_exact,
            .in_flight = candidate.in_flight && !is_current_exact,
            .estimated_covered_rows = candidate.generation.metadata
                .estimated_covered_rows,
            .stored_bytes = candidate.generation.metadata.stored_bytes,
        };
    }
    const reuse::CoverageCandidateProbeV1 probe{
        .domain_digest = keys.domain_digest,
        .canonical_domain_key = bytea_payload(keys.canonical_domain_key),
        .security_namespace = planner_security_namespace(
            request.coverage_directory_key.security_namespace),
        .provider_id = provider->provider_id,
        .compatibility_family_id = described.value.compatibility_family_id,
        .required_capability_mask = described.value.capability_mask,
    };
    const auto selected = reuse::select_candidates(
        probe,
        std::span<const reuse::CoverageCandidateViewV1>{
            candidate_views.data(), candidates->candidate_count});
    std::array<bool, kMaxCoverageGenerationsPerDomain> selected_mask{};
    const std::size_t selected_count = std::min<std::size_t>(
        selected.selected_count, kMaxSourcesPerCoverageNode - 1U);
    for (std::size_t index = 0; index < selected_count; ++index) {
        selected_mask[selected.indices[index]] = true;
    }

    reuse::PlannerGateNodesV1 current_gate{};
    if (!reuse::build_planner_gate_nodes(
            root, current_predicates.residual, &current_gate)) {
        for (std::size_t index = 0;
             index < candidates->candidate_count;
             ++index) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
        }
        return false;
    }
    List* gate_expressions = list_make1(
        copyObject(current_gate.residual_gate));
    std::array<PlannerMappedSourceV1,
        kMaxCoverageGenerationsPerDomain> mapped_sources{};
    std::size_t mapped_source_count = 0;
    for (std::size_t index = 0; index < candidates->candidate_count; ++index) {
        if (!selected_mask[index]) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
            continue;
        }
        const ReusableCandidateSnapshotV1& candidate =
            candidates->candidates[index];
#if defined(BIOSEQ_CACHE_TESTING)
        if (coverage_descriptor_wire_fault_active()) {
            candidate_bytes[index].coverage_descriptor =
                inject_coverage_descriptor_fault_fail_open(
                    candidate_bytes[index].coverage_descriptor,
                    planner_context);
            if (candidate_bytes[index].coverage_descriptor == nullptr) {
                reuse::release_candidate_reference_noerror(
                    release_records[index]);
                continue;
            }
        }
#endif
        const auto descriptor_bytes = bytea_payload(
            candidate_bytes[index].coverage_descriptor);
        PlannerDecodedCoverageV1 decoded{};
        if (!decode_coverage_descriptor_fail_open(
                descriptor_bytes,
                planner_context,
                &decoded,
                &keys.binding)
            || reuse::provider_security_digest(decoded.domain)
                != candidate.generation.coverage_directory_key
                    .security_namespace.provider_security_digest) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
            continue;
        }
        Const* reference_node = make_binary_node(
            std::as_bytes(std::span{
                &candidate.generation.reference,
                std::size_t{1}}));
        Const* generation_key_node = make_binary_node(
            bytea_payload(candidate_bytes[index].canonical_generation_key));
        Const* descriptor_node = make_binary_node(descriptor_bytes);
        if (reference_node == nullptr || generation_key_node == nullptr
            || descriptor_node == nullptr) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
            continue;
        }
        reuse::PlannerOwnedSourceMappingV1 mapping{};
        if (!reuse::invoke_map_source_fail_open(
                provider,
                root,
                &described.value,
                &decoded.source,
                planner_context,
                &mapping,
                &diagnostic,
                &release_records[index])) {
            continue;
        }
        PlannerCoveragePredicateInstancesV1 source_predicates{};
        const bool predicates_instantiated = instantiate_coverage_predicates(
            decoded.domain,
            decoded.binding,
            described.value,
            keys.binding,
            decoded.domain.coverage_predicate,
            relation->relid,
            planner_context,
            &source_predicates);
        reuse::PlannerGateNodesV1 gate{};
        if (!predicates_instantiated
            || !reuse::build_planner_gate_nodes(
                root, source_predicates.residual, &gate)) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
            continue;
        }
        mapped_sources[mapped_source_count++] = PlannerMappedSourceV1{
            .reference_node = reference_node,
            .generation_key_node = generation_key_node,
            .descriptor_node = descriptor_node,
            .source = decoded.source,
            .mapping = mapping.value,
            .residual_predicate = source_predicates.residual,
            .replay_predicate = source_predicates.replay,
            .gate_param_id = gate.param_id,
        };
        gate_expressions = lappend(
            gate_expressions, copyObject(gate.residual_gate));
    }
    for (std::size_t index = 0; index < candidates->candidate_count; ++index) {
        reuse::release_candidate_reference_noerror(release_records[index]);
    }

    reuse::PlannerOwnedDecoratedPathV1 decorated{};
    if (!reuse::invoke_decorate_path_fail_open(
            provider,
            root,
            relation,
            provider_path,
            &described.value,
            gate_expressions,
            planner_context,
            &decorated,
            &diagnostic)) {
        return false;
    }
    List* candidate_private = NIL;
    for (std::size_t index = 0; index < mapped_source_count; ++index) {
        const PlannerMappedSourceV1& mapped = mapped_sources[index];
        List* mapping_node = make_source_mapping_plan_node(
            described.value,
            mapped.source,
            decorated.value,
            mapped.mapping);
        if (mapping_node == nullptr) {
            return false;
        }
        List* source_private = list_make5(
            mapped.reference_node,
            mapped.generation_key_node,
            mapped.descriptor_node,
            mapping_node,
            copyObject(mapped.mapping.provider_private_nodes));
        source_private = lappend(
            source_private, makeInteger(mapped.gate_param_id));
        source_private = lappend(
            source_private, copyObject(mapped.replay_predicate));
        candidate_private = lappend(candidate_private, source_private);
    }
    List* private_nodes = list_make5(
        makeInteger(reuse::kReusablePlanFormatVersionV1),
        makeInteger(static_cast<std::int32_t>(provider->provider_id)),
        current_plan,
        candidate_private,
        copyObject(current_predicates.replay));
    private_nodes = lappend(
        private_nodes, makeInteger(current_gate.param_id));
    /* Subplan roots remain bypassed unless the finalized main-tree walk clears. */
    private_nodes = lappend(private_nodes, makeInteger(
        static_cast<int>(CoverageBypassMarkerV1::Provisional)));
    const Cost overhead = cpu_tuple_cost
        * (provider_path->rows > 0 ? provider_path->rows : 1.0)
        * 0.0001;
    CustomPath* wrapper = reuse::make_coverage_cache_path(
        relation,
        provider_path,
        decorated.value.path,
        private_nodes,
        &coverage_path_methods(),
        reuse::CoveragePathCostsV1{
            .startup_cost = decorated.value.path->startup_cost,
            .total_cost = decorated.value.path->total_cost + overhead,
        });
#if defined(BIOSEQ_CACHE_TESTING)
    if (wrapper != nullptr
        && g_test_coverage_descriptor_fault
            == TestCoverageDescriptorFault::PlanPrivateShort) {
        wrapper->custom_private = list_truncate(
            wrapper->custom_private,
            list_length(wrapper->custom_private) - 1);
    } else if (wrapper != nullptr
        && g_test_coverage_descriptor_fault
            == TestCoverageDescriptorFault::PlanPrivateBadVersion) {
        lfirst(list_nth_cell(wrapper->custom_private, 0)) =
            makeInteger(INT_MAX);
    }
#endif
    const bool replaced = wrapper != nullptr
        && reuse::replace_provider_path(relation, provider_path, wrapper);
    if (replaced && root != nullptr && root->parent_root != nullptr
        && g_planner_frame != nullptr) {
        g_planner_frame->nested_coverage_path_created = true;
    }
    return replaced;
}

/** Resolve one copied intent for the current Query root without shared Nodes. */
[[nodiscard]] const BioseqCacheQueryIntentV2* planner_provider_intent(
        PlannerFrame& frame,
        PlannerInfo& root,
        const provider_registry::ProviderV2* provider) noexcept {
    if (provider == nullptr || root.parse == nullptr) {
        return nullptr;
    }
    if (root.parse == frame.delegated_query) {
        for (std::size_t index = 0;
             index < frame.provider_intent_count; ++index) {
            if (frame.providers[index] == provider) {
                return &frame.intents[index].value;
            }
        }
        return nullptr;
    }
    for (std::size_t index = 0;
         index < frame.nested_intent_count; ++index) {
        NestedPlannerIntentV1& cached = frame.nested_intents[index];
        if (cached.query == root.parse && cached.provider == provider) {
            return cached.supported ? &cached.intent.value : nullptr;
        }
    }
    if (frame.nested_intent_count >= frame.nested_intents.size()) {
        return nullptr;
    }
    NestedPlannerIntentV1& destination =
        frame.nested_intents[frame.nested_intent_count++];
    destination.query = root.parse;
    destination.provider = provider;
    reuse::ProviderCallDiagnosticV1 diagnostic{};
    destination.supported = reuse::invoke_analyze_query_fail_open(
        provider,
        root.parse,
        root.planner_cxt != nullptr ? root.planner_cxt : CurrentMemoryContext,
        &destination.intent,
        &diagnostic);
    return destination.supported ? &destination.intent.value : nullptr;
}

/** Discover, map, decorate, and replace paths owned by one provider bridge. */
void consider_provider_reusable_paths_impl(
        PlannerInfo* root,
        RelOptInfo* relation,
        unsigned int range_table_index,
        RangeTblEntry* range_entry,
        const provider_registry::ProviderV2* provider) noexcept {
    PlannerFrame* frame = g_planner_frame;
    if (frame == nullptr || !frame->eligible || root == nullptr
        || root->parse == nullptr || relation == nullptr
        || range_entry == nullptr
        || provider == nullptr || relation->pathlist == NIL
        || range_table_index == 0) {
        return;
    }
    const bool index_valid = root->simple_rte_array != nullptr
        ? range_table_index
            < static_cast<unsigned int>(root->simple_rel_array_size)
        : range_table_index <= static_cast<unsigned int>(
            list_length(root->parse->rtable));
    if (!index_valid
        || planner_rt_fetch(range_table_index, root) != range_entry) {
        return;
    }
    const BioseqCacheQueryIntentV2* intent = planner_provider_intent(
        *frame, *root, provider);
    if (intent == nullptr) {
        return;
    }
    MemoryContext planner_context = root->planner_cxt != nullptr
        ? root->planner_cxt
        : CurrentMemoryContext;
    List* original_paths = list_copy(relation->pathlist);
    ListCell* cell = nullptr;
    foreach(cell, original_paths) {
        Path* path = static_cast<Path*>(lfirst(cell));
        if (path != nullptr && list_member_ptr(relation->pathlist, path)) {
            static_cast<void>(wrap_one_provider_path(
                root,
                relation,
                path,
                provider,
                *intent,
                planner_context));
        }
    }
}

struct VisibilityTicket {
    std::uint64_t database_epoch = 0;
    std::uint64_t coarse_epoch = 0;
    std::uint64_t completion_count = 0;
    TransactionId last_committed_write_xid = InvalidTransactionId;
    WriteObservability observability = WriteObservability::None;
    bool valid = false;
};

enum class VisibilityTicketPurposeV1 : std::uint8_t {
    Admission,
    Continuation,
};

/** Bind the committed-write epoch to the statement's already-active snapshot. */
[[nodiscard]] VisibilityTicket mint_visibility_ticket(
        WriteObservability observability,
        VisibilityTicketPurposeV1 purpose =
            VisibilityTicketPurposeV1::Admission) noexcept {
    VisibilityTicket ticket{};
    if (!shared_cache_ready()) {
        return ticket;
    }
    Snapshot snapshot = GetActiveSnapshot();
    if (snapshot == nullptr || !IsMVCCSnapshot(snapshot)) {
        return ticket;
    }
    const bool provider_complete = observability
        == WriteObservability::ProviderComplete;
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    const std::uint64_t completion =
        TransamVariables->xactCompletionCount;
    const std::uint32_t transitions = provider_complete
        ? pg_atomic_read_u32(&g_shared->coarse_transition_count)
        : pg_atomic_read_u32(&g_shared->database_transition_count);
    const std::uint64_t database_epoch = pg_atomic_read_u64(
        &g_shared->database_write_epoch);
    const std::uint64_t coarse_epoch = pg_atomic_read_u64(
        &g_shared->coarse_dependency_epoch);
    const TransactionId last_write = static_cast<TransactionId>(
        pg_atomic_read_u32(&g_shared->last_committed_write_xid));
    const bool snapshot_completion_stable =
        purpose == VisibilityTicketPurposeV1::Continuation
        || snapshot->snapXactCompletionCount == 0
        || snapshot->snapXactCompletionCount == completion;
    const bool stable = snapshot_completion_stable
        && transitions == 0
        && !database_has_prepared_xacts_locked(MyDatabaseId);
    LWLockRelease(ProcArrayLock);
    const bool admission_crossed_committed_write =
        purpose == VisibilityTicketPurposeV1::Admission
        && TransactionIdIsValid(last_write)
        && XidInMVCCSnapshot(last_write, snapshot);
    if (!stable || admission_crossed_committed_write) {
        return ticket;
    }
    ticket.database_epoch = database_epoch;
    ticket.coarse_epoch = coarse_epoch;
    ticket.completion_count = completion;
    ticket.last_committed_write_xid = last_write;
    ticket.observability = observability;
    ticket.valid = true;
    return ticket;
}

/** Bind generic execution to committed writes, not harmless XID rollbacks. */
[[nodiscard]] bool visibility_ticket_still_bound(
        const VisibilityTicket& ticket) noexcept {
    if (!ticket.valid || g_shared == nullptr
        || ticket.observability
            != WriteObservability::DatabaseWideFallback) {
        return false;
    }
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    const bool valid = !database_has_prepared_xacts_locked(MyDatabaseId)
        && pg_atomic_read_u32(&g_shared->database_transition_count) == 0
        && pg_atomic_read_u64(&g_shared->database_write_epoch)
            == ticket.database_epoch
        && static_cast<TransactionId>(pg_atomic_read_u32(
            &g_shared->last_committed_write_xid))
            == ticket.last_committed_write_xid;
    LWLockRelease(ProcArrayLock);
    return valid;
}

/** Provider-complete execution ignores unrelated precise commits. */
[[nodiscard]] bool provider_visibility_ticket_still_bound(
        const VisibilityTicket& ticket) noexcept {
    if (!ticket.valid || g_shared == nullptr) {
        return false;
    }
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    const bool valid = ticket.observability
            == WriteObservability::ProviderComplete
        && !database_has_prepared_xacts_locked(MyDatabaseId)
        && pg_atomic_read_u32(&g_shared->coarse_transition_count) == 0
        && pg_atomic_read_u64(&g_shared->coarse_dependency_epoch)
            == ticket.coarse_epoch;
    LWLockRelease(ProcArrayLock);
    return valid;
}

/** Sample liveness outside cache locks, then ABA-recheck while sweeping. */
void reusable_registry_janitor_sweep_v1() noexcept {
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr
        || g_reusable_exact_locks == nullptr
        || g_reusable_coverage_locks == nullptr
        || g_reusable_wait_slots == nullptr
        || g_reusable_wait_slot_count == 0
        || ProcGlobal == nullptr) {
        return;
    }
    auto* workspace =
        executor::try_construct_context_owned<
            ReusableJanitorLivenessWorkspaceV1>(CurrentMemoryContext);
    if (workspace == nullptr) {
        return;
    }
    LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
    const ReusableJanitorLivenessSnapshotV1 liveness_snapshot =
        snapshot_reusable_janitor_liveness(
            g_reusable_registry,
            workspace->observations,
            kMaximumReusableJanitorBatchV1,
            reusable_partition_callbacks());
    LWLockRelease(g_reusable_membership_lock);
    const bool complete_sample =
        liveness_snapshot.error == ReusableRegistryErrorV1::None
        && !liveness_snapshot.truncated
        && liveness_snapshot.count <= workspace->observations.size();
    const std::size_t observed_count = complete_sample
        ? liveness_snapshot.count
        : 0;
    if (complete_sample) {
        probe_reusable_owner_liveness(
            std::span<const ReusableJanitorOwnerObservationV1>{
                workspace->observations.data(), observed_count},
            std::span<bool>{workspace->alive.data(), observed_count});
    }
    ReusableOwnerLivenessSampleV1 liveness{
        .observations = workspace->observations.data(),
        .alive = workspace->alive.data(),
        .count = observed_count,
        .complete = complete_sample,
    };

    LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
    const ReusableJanitorSweepResultV1 sweep = janitor_reusable_registry(
        g_reusable_registry,
        reusable_monotonic_now_ns(),
        kMaximumReusableJanitorBatchV1,
        ReusableJanitorCallbacksV1{
            .context = &liveness,
            .owner_alive = sampled_reusable_owner_alive,
        },
        reusable_partition_callbacks());
    LWLockRelease(g_reusable_membership_lock);

    for (std::size_t index = 0;
         index < sweep.affected_reference_count; ++index) {
        broadcast_reusable_generation(sweep.affected_references[index]);
    }
    for (std::size_t index = 0;
         index < sweep.retired_manifest_count; ++index) {
        const storage::ManifestHandle manifest = storage_manifest_handle(
            sweep.retired_manifests[index]);
        if (manifest) {
            static_cast<void>(storage::retire_manifest(manifest));
        }
    }

    /* Retired records keep Generation references until their copied vector
     * has been released with every registry lock dropped.  Persist an applied
     * prefix after each bounded attempt so errors are retryable and cannot
     * double-release an earlier dependency. */
    if (sweep.error == ReusableRegistryErrorV1::None) {
        for (std::size_t attempt = 0;
             attempt < kMaximumReusableJanitorBatchV1; ++attempt) {
            LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
            const ReusableDependencyReleasePreparationV1 prepared =
                prepare_reusable_dependency_release(
                    g_reusable_registry,
                    std::span<std::byte>{
                        workspace->dependency_vector.data(),
                        workspace->dependency_vector.size()});
            LWLockRelease(g_reusable_membership_lock);
            if (prepared.error
                    == ReusableRegistryErrorV1::NoReclaimableGeneration) {
                break;
            }
            if (!prepared
                || prepared.copied_vector_bytes
                    > workspace->dependency_vector.size()) {
                mark_shared_cache_failed();
                break;
            }

            const dependency_runtime::DependencyGenerationReleaseBatchV1
                released = dependency_runtime::release_generation_entries(
                    std::span<const std::byte>{
                        workspace->dependency_vector.data(),
                        prepared.copied_vector_bytes},
                    &prepared.ticket.conservative_guard.dependency,
                    prepared.ticket.first_reference);
            if (released.coarse_fallback_required) {
                dependency_runtime::force_emergency_fallback();
            }

            bool registry_progress_valid = false;
            LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
            const auto progress =
                record_reusable_dependency_release_progress(
                    g_reusable_registry,
                    prepared.ticket,
                    released.applied_count);
            if (progress.has_value()) {
                registry_progress_valid = true;
                if (*progress) {
                    const auto finalized =
                        finalize_reusable_generation_reclaim(
                            g_reusable_registry, prepared.ticket);
                    registry_progress_valid = finalized.has_value();
                }
            }
            LWLockRelease(g_reusable_membership_lock);
            if (!registry_progress_valid) {
                mark_shared_cache_failed();
                break;
            }
            if (!released.complete) {
                /* Pending cursor is authoritative; retry on a future sweep. */
                break;
            }
        }
    }
    if (sweep.error != ReusableRegistryErrorV1::None && g_shared != nullptr) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        for (std::uint32_t index = 0;
             index < g_reusable_wait_slot_count; ++index) {
            ConditionVariableBroadcast(&g_reusable_wait_slots[index]);
        }
    }
    executor::destroy_context_owned(workspace);
}

/** Snapshot reference ownership only while membership is fully quiesced. */
[[nodiscard]] ReusableRegistrySnapshotV1
runtime_reusable_registry_snapshot_v1() noexcept {
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr) {
        return {};
    }
    LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
    const ReusableRegistrySnapshotV1 snapshot =
        reusable_registry_snapshot(g_reusable_registry);
    LWLockRelease(g_reusable_membership_lock);
    return snapshot;
}

#if defined(BIOSEQ_CACHE_TESTING)

/** Snapshot test-drain progress through the production reference aggregate. */
[[nodiscard]] ReusableRegistrySnapshotV1
test_reusable_registry_snapshot_v1() noexcept {
    return runtime_reusable_registry_snapshot_v1();
}

[[nodiscard]] bool same_test_drain_progress_v1(
        const ReusableRegistrySnapshotV1& left,
        const ReusableRegistrySnapshotV1& right) noexcept {
    return left.indexed_generations == right.indexed_generations
        && left.retired_generations == right.retired_generations
        && left.free_generations == right.free_generations
        && left.inflight_generations == right.inflight_generations
        && left.complete_generations == right.complete_generations
        && left.failed_generations == right.failed_generations
        && left.invalidated_generations == right.invalidated_generations
        && left.waiter_references == right.waiter_references
        && left.planner_pins == right.planner_pins
        && left.reader_pins == right.reader_pins;
}

/**
 * Deterministically exercise the production unified janitor from SQL tests.
 * One cycle covers every generation slot when no references interfere.  A
 * bounded retry covers owner reaping and the later dependency-release pass;
 * two unchanged cycles prove that no immediately reclaimable work remains.
 */
void drain_reusable_registry_for_testing_v1() noexcept {
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr
        || g_reusable_wait_slot_count == 0) {
        return;
    }
    const std::uint32_t passes_per_cycle =
        (g_reusable_wait_slot_count + kMaximumReusableJanitorBatchV1 - 1U)
        / kMaximumReusableJanitorBatchV1;
    constexpr std::uint32_t maximum_cycles =
        kReusableReferenceOwnersPerGenerationV1 + 2U;
    ReusableRegistrySnapshotV1 previous =
        test_reusable_registry_snapshot_v1();
    std::uint32_t unchanged_cycles = 0;
    for (std::uint32_t cycle = 0;
         cycle < maximum_cycles && unchanged_cycles < 2U;
         ++cycle) {
        for (std::uint32_t pass = 0; pass < passes_per_cycle; ++pass) {
            reusable_registry_janitor_sweep_v1();
        }
        const ReusableRegistrySnapshotV1 current =
            test_reusable_registry_snapshot_v1();
        unchanged_cycles = same_test_drain_progress_v1(previous, current)
            ? unchanged_cycles + 1U
            : 0U;
        previous = current;
    }
}

#endif

/** Copy executor support OIDs while the aggregate syscache tuple is pinned. */
struct AggregateSupportFunctionsV1 {
    std::array<Oid, 8> functions{};
    bool found = false;
};

[[nodiscard]] AggregateSupportFunctionsV1 aggregate_support_functions(
        Oid aggregate_oid) {
    AggregateSupportFunctionsV1 result{};
    HeapTuple tuple = SearchSysCache1(
        AGGFNOID, ObjectIdGetDatum(aggregate_oid));
    if (!HeapTupleIsValid(tuple)) {
        return result;
    }
    const auto* metadata = reinterpret_cast<const FormData_pg_aggregate*>(
        GETSTRUCT(tuple));
    result.functions = {
        metadata->aggtransfn,
        metadata->aggfinalfn,
        metadata->aggcombinefn,
        metadata->aggserialfn,
        metadata->aggdeserialfn,
        metadata->aggmtransfn,
        metadata->aggminvtransfn,
        metadata->aggmfinalfn,
    };
    result.found = true;
    ReleaseSysCache(tuple);
    return result;
}

/** Aggregates are immutable only when every executor support function is. */
[[nodiscard]] bool aggregate_has_mutable_support(Oid aggregate_oid) {
    const AggregateSupportFunctionsV1 support =
        aggregate_support_functions(aggregate_oid);
    if (!support.found) {
        return true;
    }
    bool mutable_support = false;
    for (const Oid function_oid : support.functions) {
        mutable_support = mutable_support
            || (OidIsValid(function_oid)
                && func_volatile(function_oid) != PROVOLATILE_IMMUTABLE);
    }
    return mutable_support;
}

/** Reject execution state that cannot be represented by the exact key/epoch. */
bool query_contains_unsafe_state_walker(Node* node, void*) {
    if (node == nullptr) {
        return false;
    }
    if (IsA(node, Var)) {
        const Var* variable = reinterpret_cast<const Var*>(node);
        return variable->varattno < 0;
    }
    if (IsA(node, SQLValueFunction)
        || IsA(node, NextValueExpr)
        || IsA(node, CurrentOfExpr)) {
        return true;
    }
    if (IsA(node, Aggref)
        && aggregate_has_mutable_support(
            reinterpret_cast<const Aggref*>(node)->aggfnoid)) {
        return true;
    }
    if (IsA(node, WindowFunc)) {
        const auto* window = reinterpret_cast<const WindowFunc*>(node);
        if (window->winagg
            && aggregate_has_mutable_support(window->winfnoid)) {
            return true;
        }
    }
    if (IsA(node, RangeTblEntry)) {
        const auto* range = reinterpret_cast<const RangeTblEntry*>(node);
        if (range->tablesample != nullptr || range->securityQuals != NIL) {
            return true;
        }
        if (range->rtekind == RTE_NAMEDTUPLESTORE
            || range->rtekind == RTE_TABLEFUNC) {
            return true;
        }
        if (range->rtekind == RTE_RELATION) {
            const char relation_kind = get_rel_relkind(range->relid);
            if (get_rel_persistence(range->relid) == RELPERSISTENCE_TEMP
                || (relation_kind != RELKIND_RELATION
                    && relation_kind != RELKIND_PARTITIONED_TABLE
                    && relation_kind != RELKIND_MATVIEW)) {
                return true;
            }
        }
        return false;
    }
    if (IsA(node, Query)) {
        return query_tree_walker(
            reinterpret_cast<Query*>(node),
            query_contains_unsafe_state_walker,
            nullptr,
            QTW_EXAMINE_RTES_BEFORE);
    }
    return expression_tree_walker(
        node, query_contains_unsafe_state_walker, nullptr);
}

/** Append one full PlanInvalItem identity exactly once. */
[[nodiscard]] bool append_exact_syscache_dependency(
        List** dependencies, int cache_id, std::uint32_t hash_value) {
    if (dependencies == nullptr || cache_id < 0
        || cache_id >= SysCacheSize) {
        return false;
    }
    ListCell* cell = nullptr;
    foreach(cell, *dependencies) {
        const auto* dependency = static_cast<const PlanInvalItem*>(
            lfirst(cell));
        if (dependency == nullptr || !IsA(dependency, PlanInvalItem)) {
            return false;
        }
        if (dependency->cacheId == cache_id
            && dependency->hashValue == hash_value) {
            return true;
        }
    }
    if (list_length(*dependencies)
        >= static_cast<int>(kMaxDependenciesPerGeneration)) {
        return false;
    }
    auto* dependency = makeNode(PlanInvalItem);
    dependency->cacheId = cache_id;
    dependency->hashValue = hash_value;
    *dependencies = lappend(*dependencies, dependency);
    return true;
}

/** Add one user aggregate and every user support procedure definition. */
[[nodiscard]] bool append_aggregate_syscache_dependencies(
        List** dependencies, Oid aggregate_oid) {
    const AggregateSupportFunctionsV1 support =
        aggregate_support_functions(aggregate_oid);
    if (!support.found) {
        return false;
    }
    if (aggregate_oid >= FirstNormalObjectId
        && !append_exact_syscache_dependency(
            dependencies,
            AGGFNOID,
            GetSysCacheHashValue1(
                AGGFNOID, ObjectIdGetDatum(aggregate_oid)))) {
        return false;
    }
    for (const Oid function_oid : support.functions) {
        if (function_oid < FirstNormalObjectId) {
            continue;
        }
        if (!append_exact_syscache_dependency(
                dependencies,
                PROCOID,
                GetSysCacheHashValue1(
                    PROCOID, ObjectIdGetDatum(function_oid)))) {
            return false;
        }
    }
    return true;
}

struct ExactSyscacheCollectorV1 {
    List* dependencies = NIL;
    bool overflow = false;
};

/** Collect aggregate support definitions from Aggref and aggregate WindowFunc. */
bool collect_aggregate_syscache_walker(Node* node, void* context) {
    if (node == nullptr) {
        return false;
    }
    auto* collector = static_cast<ExactSyscacheCollectorV1*>(context);
    Oid aggregate_oid = InvalidOid;
    if (IsA(node, Aggref)) {
        aggregate_oid = reinterpret_cast<const Aggref*>(node)->aggfnoid;
    } else if (IsA(node, WindowFunc)) {
        const auto* window = reinterpret_cast<const WindowFunc*>(node);
        if (window->winagg) {
            aggregate_oid = window->winfnoid;
        }
    }
    if (OidIsValid(aggregate_oid)
        && !append_aggregate_syscache_dependencies(
            &collector->dependencies, aggregate_oid)) {
        collector->overflow = true;
        return true;
    }
    if (IsA(node, RangeTblEntry)) {
        return false;
    }
    if (IsA(node, Query)) {
        return query_tree_walker(
            reinterpret_cast<Query*>(node),
            collect_aggregate_syscache_walker,
            context,
            QTW_EXAMINE_RTES_BEFORE);
    }
    return expression_tree_walker(
        node, collect_aggregate_syscache_walker, context);
}

[[nodiscard]] ExactSyscacheCollectorV1 collect_aggregate_syscache_dependencies(
        Query& query) {
    ExactSyscacheCollectorV1 collector{};
    static_cast<void>(query_tree_walker(
        &query,
        collect_aggregate_syscache_walker,
        &collector,
        QTW_EXAMINE_RTES_BEFORE));
    return collector;
}

struct ExternalParameterCollectorV1 {
    List* parameters = NIL;
    bool valid = true;
};

[[nodiscard]] bool external_parameter_metadata_equal(
        const Param& left, const Param& right) noexcept {
    return left.paramid == right.paramid
        && left.paramtype == right.paramtype
        && left.paramtypmod == right.paramtypmod
        && left.paramcollid == right.paramcollid;
}

int compare_external_parameters(
        const ListCell* left, const ListCell* right) noexcept {
    const auto* left_parameter = static_cast<const Param*>(lfirst(left));
    const auto* right_parameter = static_cast<const Param*>(lfirst(right));
    if (left_parameter->paramid < right_parameter->paramid) {
        return -1;
    }
    return left_parameter->paramid > right_parameter->paramid ? 1 : 0;
}

/** Collect one typed metadata record for each referenced PARAM_EXTERN. */
bool collect_external_parameter_walker(Node* node, void* context) {
    if (node == nullptr) {
        return false;
    }
    if (IsA(node, Param)) {
        const auto* parameter = reinterpret_cast<const Param*>(node);
        if (parameter->paramkind == PARAM_EXTERN) {
            auto* collector = static_cast<ExternalParameterCollectorV1*>(
                context);
            if (parameter->paramid <= 0
                || !OidIsValid(parameter->paramtype)) {
                collector->valid = false;
                return true;
            }
            ListCell* cell = nullptr;
            foreach(cell, collector->parameters) {
                const auto* existing = static_cast<const Param*>(lfirst(cell));
                if (existing->paramid != parameter->paramid) {
                    continue;
                }
                if (!external_parameter_metadata_equal(
                        *existing, *parameter)) {
                    collector->valid = false;
                    return true;
                }
                return false;
            }
            collector->parameters = lappend(
                collector->parameters,
                copyObject(const_cast<Param*>(parameter)));
        }
        return false;
    }
    if (IsA(node, RangeTblEntry)) {
        // query_tree_walker supplies RTEs to the callback when
        // QTW_EXAMINE_RTES_BEFORE is set; it owns walking their expressions.
        return false;
    }
    if (IsA(node, Query)) {
        return query_tree_walker(
            reinterpret_cast<Query*>(node),
            collect_external_parameter_walker,
            context,
            QTW_EXAMINE_RTES_BEFORE);
    }
    return expression_tree_walker(
        node, collect_external_parameter_walker, context);
}

[[nodiscard]] ExternalParameterCollectorV1 collect_external_parameters(
        Query& query) {
    ExternalParameterCollectorV1 collector{};
    static_cast<void>(query_tree_walker(
        &query, collect_external_parameter_walker, &collector,
        QTW_EXAMINE_RTES_BEFORE));
    if (collector.valid) {
        list_sort(collector.parameters, compare_external_parameters);
    }
    return collector;
}

struct RelationCollector {
    List* relation_oids = NIL;
    bool overflow = false;
};

bool collect_relation_oid_walker(Node* node, void* context) {
    if (node == nullptr) {
        return false;
    }
    auto* collector = static_cast<RelationCollector*>(context);
    if (IsA(node, RangeTblEntry)) {
        const auto* range = reinterpret_cast<const RangeTblEntry*>(node);
        if (range->rtekind == RTE_RELATION
            && OidIsValid(range->relid)
            && !list_member_oid(collector->relation_oids, range->relid)) {
            if (list_length(collector->relation_oids)
                >= static_cast<int>(kMaximumRelationDependencies)) {
                collector->overflow = true;
                return true;
            }
            collector->relation_oids = lappend_oid(
                collector->relation_oids, range->relid);
        }
        return false;
    }
    if (IsA(node, Query)) {
        return query_tree_walker(
            reinterpret_cast<Query*>(node),
            collect_relation_oid_walker,
            context,
            QTW_EXAMINE_RTES_BEFORE);
    }
    return expression_tree_walker(node, collect_relation_oid_walker, context);
}

[[nodiscard]] List* collect_relation_oids(Query& query, bool& overflow) {
    RelationCollector collector{};
    static_cast<void>(query_tree_walker(
        &query,
        collect_relation_oid_walker,
        &collector,
        QTW_EXAMINE_RTES_BEFORE));
    overflow = collector.overflow;
    return collector.relation_oids;
}

/** Conservative v1 exact-cache eligibility before path decoration. */
[[nodiscard]] bool query_is_exact_cache_eligible(const Query& query) {
    if (query.commandType != CMD_SELECT
        || query.hasModifyingCTE
        || query.rowMarks != NIL
        || query.hasTargetSRFs
        || query.hasRowSecurity
        || query.hasForUpdate) {
        return false;
    }
    if (contain_mutable_functions(reinterpret_cast<Node*>(
            const_cast<Query*>(&query)))) {
        return false;
    }
    if (query_tree_walker(
            const_cast<Query*>(&query),
            query_contains_unsafe_state_walker,
            nullptr,
            QTW_EXAMINE_RTES_BEFORE)) {
        return false;
    }
    return true;
}

/** Canonical analyzed-tree bytes; source locations are omitted by nodeToString. */
[[nodiscard]] char* canonicalize_query(const Query& query) {
    Query* copy = static_cast<Query*>(
        copyObject(const_cast<Query*>(&query)));
    copy->queryId = 0;
    copy->stmt_location = -1;
    copy->stmt_len = 0;
    return nodeToString(copy);
}

/** Extract one bounded display statement for diagnostics only. */
[[nodiscard]] char* copy_display_sql(const char* query_string) {
    if (query_string == nullptr) {
        return pstrdup("<query text unavailable>");
    }
    constexpr std::size_t kMaxDisplayBytes = 1U << 20U;
    const std::size_t length = strnlen(query_string, kMaxDisplayBytes + 1U);
    if (length <= kMaxDisplayBytes) {
        return pnstrdup(query_string, length);
    }
    constexpr char suffix[] = " /* truncated */";
    char* result = static_cast<char*>(palloc(kMaxDisplayBytes + sizeof(suffix)));
    std::memcpy(result, query_string, kMaxDisplayBytes);
    std::memcpy(result + kMaxDisplayBytes, suffix, sizeof(suffix));
    return result;
}

/** Outer scan execution state; CustomScanState must remain first. */
enum class RuntimeRole : std::uint8_t {
    Bypass,
    Producer,
    Consumer,
};

struct RegistryReaderClaim {
    std::uint32_t slot =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t registration_sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < storage::kReaderPinsPerManifest
            && registration_sequence != 0;
    }
};

struct StoredAttribute {
    Oid type_oid;
    std::int32_t type_modifier;
    Oid collation_oid;
    std::uint32_t reserved;
};

static_assert(std::is_trivially_copyable_v<StoredAttribute>);

using PostgreSqlStreamingColumnStoreBuilder = StreamingColumnStoreBuilder<
    PostgreSqlColumnChecksumPolicy,
    storage::StorageColumnStoreBlockSink>;

struct BioseqCacheState {
    CustomScanState css;
    bool child_initialized;
    bool capture_failed;
    bool producer_completed;
    bool manifest_pin_owned;
    bool manifest_waiter_owned;
    bool registry_reader_owned;
    bool registry_reference_owned;
    bool registered_inflight_waiter;
    bool ttl_entitlement_consumed;
    bool runtime_recipe_valid;
    bool planned_diagnostics_published;
#if defined(BIOSEQ_CACHE_TESTING)
    bool child_execution_counted;
#endif
    RuntimeRole runtime_role;
    int registry_index;
    std::uint64_t generation;
    std::uint64_t visibility_database_epoch;
    std::uint64_t visibility_completion_count;
    std::uint64_t row_count;
    std::uint64_t payload_bytes;
    std::uint64_t logical_bytes;
    RegistryReaderClaim registry_reader_claim;
    CacheEntryId entry_id;
    ReusableGenerationReferenceV1 registry_reference;
    ReusableReferenceTokenV1 registry_reference_token;
    ReusableGenerationSnapshotV1 completed_snapshot;
    storage::ManifestHandle manifest_handle;
    storage::ManifestWaiterToken manifest_waiter_token;
    storage::ManifestPin manifest_pin;
    std::array<storage::StoredBlockToken,
        storage::kBlocksPerManifest> stored_tokens;
    std::uint32_t stored_token_count;
    std::array<provider_registry::ProviderLeaseV2,
        kMaxProviderRecipesPerExactPlan> provider_leases;
    std::uint32_t provider_lease_count;
    PostgreSqlColumnChecksumPolicy* column_checksum_policy;
    storage::StorageColumnStoreBlockSink* column_sink;
    PostgreSqlStreamingColumnStoreBuilder* column_builder;
    Oid* send_functions;
    Tuplestorestate* replay_store;
    TupleTableSlot* replay_slot;
    BackendQueryCleanupLinkV1 backend_cleanup;
    MemoryContextCallback cleanup_callback;
    bool cleanup_registered;
    bool cleanup_done;
    TimestampTz producer_started_at;
};

static_assert(std::is_standard_layout_v<BioseqCacheState>);
static_assert(offsetof(BioseqCacheState, css) == 0);

[[nodiscard]] bool register_exact_executor_start_claim(
        BioseqCacheState& state) noexcept {
    ExecutorStartClaimFrameV1* frame = g_executor_start_claim_frame;
    if (frame == nullptr) {
        return false;
    }
    for (std::size_t index = 0;
         index < frame->exact_producer_count;
         ++index) {
        if (frame->exact_producers[index] == &state) {
            return true;
        }
    }
    if (frame->exact_producer_count >= frame->exact_producers.size()) {
        return false;
    }
    frame->exact_producers[frame->exact_producer_count++] = &state;
    return true;
}

/** Read one String node from versioned custom-private metadata. */
[[nodiscard]] const char* private_string(const CustomScan& scan, int index) {
    if (scan.custom_private == NIL || list_length(scan.custom_private) <= index) {
        return "<metadata unavailable>";
    }
    Node* node = static_cast<Node*>(list_nth(scan.custom_private, index));
    return IsA(node, String) ? strVal(node) : "<metadata invalid>";
}

/** Read the post-setrefs fail-open bit; malformed metadata always bypasses. */
[[nodiscard]] bool private_force_bypass(const CustomScan& scan) noexcept {
    if (scan.custom_private == NIL
        || list_length(scan.custom_private) <= 2) {
        return true;
    }
    const Node* marker = static_cast<const Node*>(
        list_nth(scan.custom_private, 2));
    return marker == nullptr || !IsA(marker, Integer) || intVal(marker) != 0;
}

[[nodiscard]] int private_force_bypass_marker(
        const CustomScan& scan) noexcept {
    if (scan.custom_private == NIL
        || list_length(scan.custom_private) <= 2) {
        return -1;
    }
    const Node* marker = static_cast<const Node*>(
        list_nth(scan.custom_private, 2));
    return marker != nullptr && IsA(marker, Integer)
        ? intVal(marker)
        : -1;
}

/** Mutate only the preallocated scalar; never reshape a finalized Plan. */
[[nodiscard]] bool set_private_force_bypass(
        CustomScan& scan, bool force_bypass) noexcept {
    if (scan.custom_private == NIL
        || list_length(scan.custom_private) <= 2) {
        return false;
    }
    Node* marker = static_cast<Node*>(list_nth(scan.custom_private, 2));
    if (marker == nullptr || !IsA(marker, Integer)) {
        return false;
    }
    auto* value = reinterpret_cast<Integer*>(marker);
    if (value->ival
            == static_cast<int>(ExactBypassMarkerV1::CoverageGateFinalizer)) {
        return true;
    }
    value->ival = force_bypass
        ? static_cast<int>(ExactBypassMarkerV1::UnsafeFinalizedPlan)
        : static_cast<int>(ExactBypassMarkerV1::Enabled);
    return true;
}

/** Count one typed private list; -1 identifies malformed finalized metadata. */
[[nodiscard]] int private_list_length(
        const CustomScan& scan, int index, NodeTag expected_tag) noexcept {
    if (scan.custom_private == NIL
        || list_length(scan.custom_private) <= index) {
        return -1;
    }
    const auto* values = static_cast<const List*>(
        list_nth(scan.custom_private, index));
    if (values == NIL) {
        return 0;
    }
    return nodeTag(values) == expected_tag ? list_length(values) : -1;
}

/** Append a length-delimited field to an exact key without ambiguous joins. */
[[nodiscard]] bool append_key_field(
        StringInfo key, const void* bytes, std::size_t size) {
    if (size > static_cast<std::size_t>(kMaxCanonicalKeyBytes)
        || static_cast<std::size_t>(key->len) + sizeof(std::uint32_t) + size
            > static_cast<std::size_t>(kMaxCanonicalKeyBytes)) {
        return false;
    }
    const std::uint32_t length = static_cast<std::uint32_t>(size);
    appendBinaryStringInfo(
        key, reinterpret_cast<const char*>(&length), sizeof(length));
    if (size != 0) {
        appendBinaryStringInfo(
            key, static_cast<const char*>(bytes), static_cast<int>(size));
    }
    return true;
}

template <class Value>
[[nodiscard]] bool append_key_scalar(StringInfo key, const Value& value) {
    static_assert(std::is_trivially_copyable_v<Value>);
    return append_key_field(key, &value, sizeof(value));
}

/** Hash the exact output type/collation identity stored beside every result. */
[[nodiscard]] std::uint64_t output_schema_hash(const TupleDesc descriptor) {
    std::uint64_t value = 0x9e3779b97f4a7c15ULL;
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        const Form_pg_attribute metadata = TupleDescAttr(descriptor, attribute);
        const StoredAttribute stored{
            .type_oid = metadata->atttypid,
            .type_modifier = metadata->atttypmod,
            .collation_oid = metadata->attcollation,
            .reserved = 0,
        };
        value = hash_any_extended(
            reinterpret_cast<const unsigned char*>(&stored),
            sizeof(stored), value);
    }
    return value;
}

/** Confirm every output column has both binary directions before claiming. */
void validate_binary_schema(const TupleDesc descriptor) {
    if (descriptor->natts < 0
        || static_cast<std::uint32_t>(descriptor->natts)
            > kMaxProviderBoundaryColumns) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("cache result has too many columns")));
    }
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        const Form_pg_attribute metadata = TupleDescAttr(descriptor, attribute);
        Oid send_function = InvalidOid;
        Oid receive_function = InvalidOid;
        Oid io_parameter = InvalidOid;
        bool variable_length = false;
        getTypeBinaryOutputInfo(
            metadata->atttypid, &send_function, &variable_length);
        getTypeBinaryInputInfo(
            metadata->atttypid, &receive_function, &io_parameter);
        if (!OidIsValid(send_function) || !OidIsValid(receive_function)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("type %u lacks binary cache I/O", metadata->atttypid)));
        }
    }
}

[[nodiscard]] std::uint8_t type_alignment_bytes(char alignment) {
    switch (alignment) {
        case TYPALIGN_CHAR: return 1;
        case TYPALIGN_SHORT: return 2;
        case TYPALIGN_INT: return 4;
        case TYPALIGN_DOUBLE: return 8;
        default:
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("unknown PostgreSQL type alignment '%c'", alignment)));
    }
    pg_unreachable();
}

/** Freeze PostgreSQL tuple metadata into the portable column-store schema. */
[[nodiscard]] std::vector<ColumnSchema> build_column_schema(
        TupleDesc descriptor, Oid* send_functions) {
    std::vector<ColumnSchema> schema;
    schema.reserve(static_cast<std::size_t>(descriptor->natts));
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        const Form_pg_attribute metadata = TupleDescAttr(descriptor, attribute);
        Oid send_function = InvalidOid;
        Oid receive_function = InvalidOid;
        Oid io_parameter = InvalidOid;
        bool variable_length = false;
        getTypeBinaryOutputInfo(
            metadata->atttypid, &send_function, &variable_length);
        getTypeBinaryInputInfo(
            metadata->atttypid, &receive_function, &io_parameter);
        if (send_functions != nullptr) {
            send_functions[attribute] = send_function;
        }
        const std::uint64_t semantic_high =
            (static_cast<std::uint64_t>(metadata->atttypid) << 32U)
            | static_cast<std::uint32_t>(attribute + 1);
        const std::uint64_t semantic_low =
            (static_cast<std::uint64_t>(metadata->attcollation) << 32U)
            | static_cast<std::uint32_t>(metadata->atttypmod);
        schema.push_back(ColumnSchema{
            .name = NameStr(metadata->attname),
            .descriptor = ColumnDescriptorV1{
                .type_oid = metadata->atttypid,
                .typmod = metadata->atttypmod,
                .collation_oid = metadata->attcollation,
                .type_length = metadata->attlen,
                .by_value = static_cast<std::uint8_t>(metadata->attbyval),
                .type_alignment = type_alignment_bytes(metadata->attalign),
                .encoding = ColumnEncoding::BinarySend,
                .flags = kColumnFlagNullable,
                .send_function_oid = send_function,
                .receive_function_oid = receive_function,
                .receive_type_io_param = io_parameter,
                .reserved = 0,
                .name_offset = 0,
            },
            .semantic_id = SemanticId128{
                .high = semantic_high,
                .low = semantic_low,
            },
        });
    }
    return schema;
}

[[nodiscard]] ColumnStoreIdentity current_column_store_identity() noexcept {
    return ColumnStoreIdentity{
        .postgres_major = static_cast<std::uint32_t>(PG_VERSION_NUM / 10000),
        .boot_nonce = g_shared == nullptr
            ? 0
            : pg_atomic_read_u64(&g_shared->boot_nonce),
        .database_oid = MyDatabaseId,
        .owner_role_oid = GetUserId(),
        .datum_width = static_cast<std::uint8_t>(sizeof(Datum)),
    };
}

void destroy_column_builder(BioseqCacheState& state) noexcept {
    executor::destroy_context_owned(state.column_builder);
    executor::destroy_context_owned(state.column_sink);
    executor::destroy_context_owned(state.column_checksum_policy);
    if (state.send_functions != nullptr) {
        pfree(state.send_functions);
        state.send_functions = nullptr;
    }
}

/** Serialize one external parameter with analyzed and runtime identity. */
[[nodiscard]] bool append_parameter_key(
        StringInfo key, const Param& analyzed,
        const ParamExternData& parameter) {
    if (!append_key_scalar(key, analyzed.paramtype)
        || !append_key_scalar(key, analyzed.paramtypmod)
        || !append_key_scalar(key, analyzed.paramcollid)
        || !append_key_scalar(key, parameter.ptype)
        || !append_key_scalar(key, parameter.isnull)) {
        return false;
    }
    if (parameter.isnull) {
        return append_key_field(key, nullptr, 0);
    }
    Oid send_function = InvalidOid;
    bool variable_length = false;
    getTypeBinaryOutputInfo(
        parameter.ptype, &send_function, &variable_length);
    bytea* bytes = OidSendFunctionCall(send_function, parameter.value);
    const bool appended = append_key_field(
        key, VARDATA_ANY(bytes), VARSIZE_ANY_EXHDR(bytes));
    pfree(bytes);
    return appended;
}

/** Build the execution-time exact key, including parameters and environment. */
[[nodiscard]] StringInfo build_runtime_key(
        const CustomScan& scan, EState* estate, TupleDesc descriptor) {
    StringInfo key = makeStringInfo();
    const std::uint32_t key_format = 7;
    Oid effective_user = InvalidOid;
    int security_context = 0;
    GetUserIdAndSecContext(&effective_user, &security_context);
    const Oid session_user = GetSessionUserId();
    const std::uint64_t schema_hash = output_schema_hash(descriptor);
    const char* canonical_query = private_string(scan, 0);
    const std::size_t canonical_size = std::strlen(canonical_query);
    if (!append_key_scalar(key, key_format)
        || !append_key_scalar(key, MyDatabaseId)
        || !append_key_scalar(key, effective_user)
        || !append_key_scalar(key, session_user)
        || !append_key_scalar(key, security_context)
        || !append_key_scalar(key, XactIsoLevel)
        || !append_key_scalar(key, schema_hash)
        || !append_key_field(key, canonical_query, canonical_size)) {
        pfree(key->data);
        pfree(key);
        return nullptr;
    }

    static constexpr const char* environment_options[] = {
        "TimeZone", "DateStyle", "IntervalStyle", "extra_float_digits",
        "bytea_output", "search_path", "row_security",
        "default_table_access_method", "default_tablespace",
    };
    for (const char* option : environment_options) {
        const char* canonical_name = nullptr;
        char* value = GetConfigOptionByName(option, &canonical_name, false);
        if (!append_key_field(key, option, std::strlen(option))
            || !append_key_field(key, value, std::strlen(value))) {
            pfree(value);
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
        pfree(value);
    }

    ParamListInfo parameters = estate == nullptr
        ? nullptr
        : estate->es_param_list_info;
    List* referenced_parameters = scan.custom_private != NIL
            && list_length(scan.custom_private) > 3
        ? static_cast<List*>(list_nth(scan.custom_private, 3))
        : NIL;
    const int parameter_count = list_length(referenced_parameters);
    if (!append_key_scalar(key, parameter_count)) {
        pfree(key->data);
        pfree(key);
        return nullptr;
    }
    ListCell* parameter_cell = nullptr;
    foreach(parameter_cell, referenced_parameters) {
        const auto* analyzed = static_cast<const Param*>(
            lfirst(parameter_cell));
        if (analyzed == nullptr || !IsA(analyzed, Param)
            || analyzed->paramkind != PARAM_EXTERN
            || !OidIsValid(analyzed->paramtype)) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
        const int parameter_id = analyzed->paramid;
        if (parameters == nullptr
            || parameter_id <= 0
            || parameter_id > parameters->numParams
            || !append_key_scalar(key, parameter_id)) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
        ParamExternData workspace{};
        ParamExternData* parameter = parameters->paramFetch != nullptr
            ? parameters->paramFetch(
                parameters, parameter_id, true, &workspace)
            : &parameters->params[parameter_id - 1];
        if (parameter == nullptr || !OidIsValid(parameter->ptype)
            || !append_parameter_key(key, *analyzed, *parameter)) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
    }

    const int attribute_count = descriptor->natts;
    if (!append_key_scalar(key, attribute_count)) {
        pfree(key->data);
        pfree(key);
        return nullptr;
    }
    for (int attribute = 0; attribute < attribute_count; ++attribute) {
        const Form_pg_attribute metadata = TupleDescAttr(descriptor, attribute);
        const StoredAttribute stored{
            .type_oid = metadata->atttypid,
            .type_modifier = metadata->atttypmod,
            .collation_oid = metadata->attcollation,
            .reserved = 0,
        };
        if (!append_key_field(key, &stored, sizeof(stored))) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
    }
    return key;
}

const CustomScanMethods& coverage_scan_methods();

/** Stable semantic material carried by one finalized reusable coverage Plan. */
struct ExactCoverageSemanticFrameV1 {
    CoverageCurrentPlanViewV1 current{};
    CoverageDescriptorHeaderV1 descriptor{};
};

[[nodiscard]] bool exact_coverage_bytes_equal(
        std::span<const std::byte> left,
        std::span<const std::byte> right) noexcept {
    return left.size() == right.size()
        && (left.empty()
            || std::memcmp(left.data(), right.data(), left.size()) == 0);
}

/** A duplicate logical occurrence must describe byte-identical semantics. */
[[nodiscard]] bool exact_coverage_binding_anchor_equal(
        const reuse::CoverageFragmentBindingSignatureV1& left,
        const reuse::CoverageFragmentBindingSignatureV1& right) noexcept {
    return left.format_version == right.format_version
        && left.fragment_binding_ordinal == right.fragment_binding_ordinal
        && left.provider_rte_occurrence_ordinal
            == right.provider_rte_occurrence_ordinal
        && left.provider_rte_occurrence_count
            == right.provider_rte_occurrence_count
        && left.logical_relation_oid == right.logical_relation_oid
        && left.selected_relation_oid == right.selected_relation_oid
        && left.provider_id == right.provider_id
        && left.flags == right.flags;
}

[[nodiscard]] bool exact_coverage_frames_equal(
        const ExactCoverageSemanticFrameV1& left,
        const ExactCoverageSemanticFrameV1& right) noexcept {
    const auto& left_header = left.current.header;
    const auto& right_header = right.current.header;
    return left_header.provider_id == right_header.provider_id
        && left_header.provider_abi_version
            == right_header.provider_abi_version
        && left_header.compatibility_family_id
            == right_header.compatibility_family_id
        && left_header.capability_mask == right_header.capability_mask
        && left_header.coverage_directory_key
            == right_header.coverage_directory_key
        && left_header.generation_digest == right_header.generation_digest
        && left.descriptor.provider_security_digest_format_version
            == right.descriptor.provider_security_digest_format_version
        && left.descriptor.provider_security_flags
            == right.descriptor.provider_security_flags
        && left.descriptor.boundary_count == right.descriptor.boundary_count
        && left.descriptor.lineage_count == right.descriptor.lineage_count
        && left.descriptor.required_boundary_count
            == right.descriptor.required_boundary_count
        && left.descriptor.required_lineage_count
            == right.descriptor.required_lineage_count
        && left.descriptor.binding == right.descriptor.binding
        && exact_coverage_bytes_equal(
            left.current.canonical_domain_key,
            right.current.canonical_domain_key)
        && exact_coverage_bytes_equal(
            left.current.canonical_generation_key,
            right.current.canonical_generation_key);
}

/** Canonical order is the stable provider/relation occurrence identity. */
[[nodiscard]] bool exact_coverage_frame_less(
        const ExactCoverageSemanticFrameV1& left,
        const ExactCoverageSemanticFrameV1& right) noexcept {
    const auto& l = left.descriptor.binding;
    const auto& r = right.descriptor.binding;
    if (l.provider_id != r.provider_id) {
        return l.provider_id < r.provider_id;
    }
    if (l.logical_relation_oid != r.logical_relation_oid) {
        return l.logical_relation_oid < r.logical_relation_oid;
    }
    if (l.provider_rte_occurrence_ordinal
        != r.provider_rte_occurrence_ordinal) {
        return l.provider_rte_occurrence_ordinal
            < r.provider_rte_occurrence_ordinal;
    }
    if (l.provider_rte_occurrence_count
        != r.provider_rte_occurrence_count) {
        return l.provider_rte_occurrence_count
            < r.provider_rte_occurrence_count;
    }
    if (l.selected_relation_oid != r.selected_relation_oid) {
        return l.selected_relation_oid < r.selected_relation_oid;
    }
    if (l.fragment_binding_ordinal != r.fragment_binding_ordinal) {
        return l.fragment_binding_ordinal < r.fragment_binding_ordinal;
    }
    return l.format_version < r.format_version;
}

/** Walk all finalized Plan children without recursion cycles or unbounded work. */
struct ExactCoverageSemanticCollectorV1 {
    std::array<ExactCoverageSemanticFrameV1,
        kMaxProviderRecipesPerExactPlan> frames{};
    std::array<const Plan*, kMaximumExactPlanWalkNodesV1> visited{};
    std::size_t frame_count = 0;
    std::size_t visited_count = 0;
    bool valid = true;

    [[nodiscard]] bool collect_plan(const Plan* plan);

    [[nodiscard]] bool collect_plan_list(const List* plans) {
        const ListCell* cell = nullptr;
        foreach(cell, plans) {
            if (!collect_plan(static_cast<const Plan*>(lfirst(cell)))) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool insert(
            const ExactCoverageSemanticFrameV1& candidate) noexcept {
        const auto& candidate_binding = candidate.descriptor.binding;
        for (std::size_t index = 0; index < frame_count; ++index) {
            if (!exact_coverage_binding_anchor_equal(
                    frames[index].descriptor.binding,
                    candidate_binding)) {
                continue;
            }
            if (!exact_coverage_frames_equal(frames[index], candidate)) {
                valid = false;
                return false;
            }
            return true;
        }
        if (frame_count >= frames.size()) {
            valid = false;
            return false;
        }
        frames[frame_count++] = candidate;
        return true;
    }
};

/** Decode and validate one coverage node without retaining physical state. */
[[nodiscard]] bool collect_exact_coverage_frame(
        ExactCoverageSemanticCollectorV1& collector,
        const CustomScan& scan) noexcept {
    if (scan.custom_private == NIL || list_length(scan.custom_private) < 3
        || !IsA(list_nth(scan.custom_private, 0), Integer)
        || !IsA(list_nth(scan.custom_private, 1), Integer)
        || intVal(list_nth(scan.custom_private, 0))
            != static_cast<int>(reuse::kReusablePlanFormatVersionV1)) {
        return false;
    }
    ExactCoverageSemanticFrameV1 candidate{};
    const std::span<const std::byte> current_bytes = binary_node_payload(
        list_nth(scan.custom_private, 2));
    if (!decode_current_coverage_plan(current_bytes, &candidate.current)
        || intVal(list_nth(scan.custom_private, 1)) <= 0
        || static_cast<std::uint32_t>(
            intVal(list_nth(scan.custom_private, 1)))
            != candidate.current.header.provider_id
        || candidate.current.header.capability_mask == 0
        || candidate.current.coverage_descriptor.size()
            < sizeof(CoverageDescriptorHeaderV1)) {
        return false;
    }
    std::memcpy(
        &candidate.descriptor,
        candidate.current.coverage_descriptor.data(),
        sizeof(candidate.descriptor));
    std::uint64_t expected_descriptor_size = 0;
    const auto descriptor_crc = coverage_descriptor_payload_crc32c(
        candidate.current.coverage_descriptor);
    const auto& header = candidate.current.header;
    const auto& security = header.coverage_directory_key.security_namespace;
    const provider_registry::ProviderV2* provider =
        provider_registry::find_provider_v2(header.provider_id);
    if (!coverage_descriptor_size(
            candidate.descriptor, &expected_descriptor_size)
        || expected_descriptor_size
            != candidate.current.coverage_descriptor.size()
        || !descriptor_crc.has_value()
        || *descriptor_crc != candidate.descriptor.payload_crc32c
        || provider == nullptr
        || provider->abi_version != header.provider_abi_version
        || candidate.descriptor.provider_id != header.provider_id
        || candidate.descriptor.compatibility_family_id
            != header.compatibility_family_id
        || candidate.descriptor.capability_mask != header.capability_mask
        || candidate.descriptor.binding.provider_id != header.provider_id
        || candidate.descriptor.provider_security_digest_format_version
            != BIOSEQ_CACHE_PROVIDER_SECURITY_DIGEST_FORMAT_V1
        || candidate.descriptor.provider_security_flags != 0
        || std::memcmp(
            candidate.descriptor.provider_security_digest.data(),
            security.provider_security_digest.bytes.data(),
            security.provider_security_digest.bytes.size()) != 0) {
        return false;
    }
    return collector.insert(candidate);
}

bool ExactCoverageSemanticCollectorV1::collect_plan(const Plan* plan) {
    if (!valid || plan == nullptr) {
        return valid;
    }
    for (std::size_t index = 0; index < visited_count; ++index) {
        if (visited[index] == plan) {
            return true;
        }
    }
    if (visited_count >= visited.size()) {
        valid = false;
        return false;
    }
    visited[visited_count++] = plan;
    if (IsA(plan, CustomScan)) {
        const auto* custom = reinterpret_cast<const CustomScan*>(plan);
        if (custom->methods == &coverage_scan_methods()
            && !collect_exact_coverage_frame(*this, *custom)) {
            valid = false;
            return false;
        }
    }
    if (!collect_plan(outerPlan(plan)) || !collect_plan(innerPlan(plan))) {
        return false;
    }
    switch (nodeTag(plan)) {
        case T_SubqueryScan:
            return collect_plan(
                reinterpret_cast<const SubqueryScan*>(plan)->subplan);
        case T_CustomScan:
            return collect_plan_list(
                reinterpret_cast<const CustomScan*>(plan)->custom_plans);
        case T_Append:
            return collect_plan_list(
                reinterpret_cast<const Append*>(plan)->appendplans);
        case T_MergeAppend:
            return collect_plan_list(
                reinterpret_cast<const MergeAppend*>(plan)->mergeplans);
        case T_BitmapAnd:
            return collect_plan_list(
                reinterpret_cast<const BitmapAnd*>(plan)->bitmapplans);
        case T_BitmapOr:
            return collect_plan_list(
                reinterpret_cast<const BitmapOr*>(plan)->bitmapplans);
        case T_Agg:
            return collect_plan_list(
                reinterpret_cast<const Agg*>(plan)->chain);
        case T_Result:
        case T_ProjectSet:
        case T_ModifyTable:
        case T_RecursiveUnion:
        case T_SeqScan:
        case T_SampleScan:
        case T_IndexScan:
        case T_IndexOnlyScan:
        case T_BitmapIndexScan:
        case T_BitmapHeapScan:
        case T_TidScan:
        case T_TidRangeScan:
        case T_FunctionScan:
        case T_ValuesScan:
        case T_TableFuncScan:
        case T_CteScan:
        case T_NamedTuplestoreScan:
        case T_WorkTableScan:
        case T_ForeignScan:
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin:
        case T_Material:
        case T_Memoize:
        case T_Sort:
        case T_IncrementalSort:
        case T_Group:
        case T_WindowAgg:
        case T_Unique:
        case T_Gather:
        case T_GatherMerge:
        case T_Hash:
        case T_SetOp:
        case T_LockRows:
        case T_Limit:
            return true;
        default:
            valid = false;
            return false;
    }
}

/** Frame all reusable semantics that can affect one exact root's result. */
[[nodiscard]] bool append_exact_coverage_semantic_key(
        const CustomScan& scan,
        EState* estate,
        StringInfo key) {
    if (estate == nullptr || estate->es_plannedstmt == nullptr
        || key == nullptr || scan.custom_plans == NIL
        || list_length(scan.custom_plans) != 1
        || list_length(estate->es_plannedstmt->subplans)
            > static_cast<int>(kMaximumExactSubplansV1)) {
        return false;
    }
    ExactCoverageSemanticCollectorV1 collector{};
    if (!collector.collect_plan(static_cast<const Plan*>(
            linitial(scan.custom_plans)))
        || !collector.collect_plan_list(estate->es_plannedstmt->subplans)
        || !collector.valid) {
        return false;
    }
    std::sort(
        collector.frames.begin(),
        collector.frames.begin()
            + static_cast<std::ptrdiff_t>(collector.frame_count),
        exact_coverage_frame_less);
    constexpr std::uint32_t frame_format = 1;
    const auto frame_count = static_cast<std::uint32_t>(
        collector.frame_count);
    if (!append_key_scalar(key, frame_format)
        || !append_key_scalar(key, frame_count)) {
        return false;
    }
    for (std::size_t index = 0; index < collector.frame_count; ++index) {
        const ExactCoverageSemanticFrameV1& frame = collector.frames[index];
        const auto& header = frame.current.header;
        const auto& binding = frame.descriptor.binding;
        const auto& security =
            header.coverage_directory_key.security_namespace;
        if (!append_key_scalar(key, header.provider_id)
            || !append_key_scalar(key, header.provider_abi_version)
            || !append_key_scalar(key, header.compatibility_family_id)
            || !append_key_scalar(key, header.capability_mask)
            || !append_key_scalar(key, binding.format_version)
            || !append_key_scalar(key, binding.fragment_binding_ordinal)
            || !append_key_scalar(
                key, binding.provider_rte_occurrence_ordinal)
            || !append_key_scalar(
                key, binding.provider_rte_occurrence_count)
            || !append_key_scalar(key, binding.logical_relation_oid)
            || !append_key_scalar(key, binding.selected_relation_oid)
            || !append_key_scalar(key, binding.provider_id)
            || !append_key_scalar(key, binding.flags)
            || !append_key_field(
                key,
                binding.binding_role_digest.bytes.data(),
                binding.binding_role_digest.bytes.size())
            || !append_key_scalar(key, security.database_oid)
            || !append_key_scalar(key, security.current_role_oid)
            || !append_key_scalar(key, security.session_user_oid)
            || !append_key_scalar(key, security.security_context_flags)
            || !append_key_scalar(key, security.row_security_mode)
            || !append_key_scalar(key, security.reserved)
            || !append_key_field(
                key,
                security.provider_security_digest.bytes.data(),
                security.provider_security_digest.bytes.size())
            || !append_key_field(
                key,
                header.coverage_directory_key.coverage_domain_digest
                    .bytes.data(),
                header.coverage_directory_key.coverage_domain_digest
                    .bytes.size())
            || !append_key_field(
                key,
                header.generation_digest.bytes.data(),
                header.generation_digest.bytes.size())
            || !append_key_scalar(
                key,
                frame.descriptor
                    .provider_security_digest_format_version)
            || !append_key_scalar(
                key, frame.descriptor.provider_security_flags)
            || !append_key_scalar(key, frame.descriptor.boundary_count)
            || !append_key_scalar(key, frame.descriptor.lineage_count)
            || !append_key_scalar(
                key, frame.descriptor.required_boundary_count)
            || !append_key_scalar(
                key, frame.descriptor.required_lineage_count)
            || !append_key_field(
                key,
                frame.current.canonical_domain_key.data(),
                frame.current.canonical_domain_key.size())
            || !append_key_field(
                key,
                frame.current.canonical_generation_key.data(),
                frame.current.canonical_generation_key.size())) {
            return false;
        }
    }
    return true;
}

/** Exact replay may elide only providers that explicitly opt into it. */
[[nodiscard]] bool provider_allows_exact_source_elision(
        const provider_registry::ProviderV2* provider) noexcept {
    return provider != nullptr
        && (provider->provider_flags
                & BIOSEQ_CACHE_PROVIDER_EXACT_SOURCE_ELISION_SAFE) != 0;
}

/** Revalidate every finalized provider recipe and frame its semantic bytes. */
[[nodiscard]] bool append_exact_runtime_recipe_key(
        const CustomScan& scan,
        EState* estate,
        StringInfo key,
        BioseqCacheState& state) {
    if (estate == nullptr || key == nullptr || scan.custom_private == NIL
        || list_length(scan.custom_private) < 8
        || !IsA(list_nth(scan.custom_private, 7), Integer)) {
        return false;
    }
    const int expected_count = intVal(list_nth(scan.custom_private, 7));
    const void* recipes_node = list_nth(scan.custom_private, 6);
    const List* recipes = recipes_node == nullptr
        ? NIL
        : IsA(recipes_node, List)
            ? static_cast<const List*>(recipes_node)
            : nullptr;
    if (expected_count < 0
        || expected_count
            > static_cast<int>(kMaxProviderRecipesPerExactPlan)
        || (recipes == nullptr && recipes_node != nullptr)
        || list_length(recipes) != expected_count) {
        return false;
    }
    constexpr std::uint32_t key_recipe_format = 1;
    if (!append_key_scalar(key, key_recipe_format)
        || !append_key_scalar(key, expected_count)) {
        return false;
    }

    std::uint64_t serialized_total = 0;
    int prior_ordinal = -1;
    const ListCell* cell = nullptr;
    foreach(cell, recipes) {
        const auto* encoded = static_cast<const List*>(lfirst(cell));
        if (encoded == nullptr || !IsA(encoded, List)
            || list_length(encoded) != static_cast<int>(
                kExactProviderRecipeNodeCountV1)
            || !IsA(list_nth(encoded, 0), Integer)
            || !IsA(list_nth(encoded, 1), Integer)
            || !IsA(list_nth(encoded, 2), Integer)
            || !IsA(list_nth(encoded, 3), Integer)) {
            return false;
        }
        const int recipe_format = intVal(list_nth(encoded, 0));
        const int provider_id_value = intVal(list_nth(encoded, 1));
        const int provider_abi_value = intVal(list_nth(encoded, 2));
        const int ordinal = intVal(list_nth(encoded, 3));
        const std::span<const std::byte> expected = binary_node_payload(
            list_nth(encoded, 4));
        const void* plan_domain_node = list_nth(encoded, 5);
        if (recipe_format
                != static_cast<int>(kExactProviderRecipePlanFormatV1)
            || provider_id_value <= 0 || provider_abi_value <= 0
            || ordinal <= prior_ordinal
            || expected.size() != CacheDigest256{}.bytes.size()
            || plan_domain_node == nullptr
            || !IsA(plan_domain_node, List)) {
            return false;
        }
        const auto* provider = provider_registry::find_provider_v2(
            static_cast<std::uint32_t>(provider_id_value));
        if (provider == nullptr
            || !provider_allows_exact_source_elision(provider)
            || provider->abi_version
                != static_cast<std::uint32_t>(provider_abi_value)
            || state.provider_lease_count
                >= state.provider_leases.size()) {
            return false;
        }
        provider_registry::ProviderLeaseV2& provider_lease =
            state.provider_leases[state.provider_lease_count];
        if (!provider_registry::acquire_provider_lease_v2(
                provider, &provider_lease)) {
            return false;
        }
        ++state.provider_lease_count;
        char* serialized = nodeToString(plan_domain_node);
        if (serialized == nullptr) {
            return false;
        }
        const std::size_t serialized_bytes = strnlen(
            serialized,
            static_cast<std::size_t>(kMaxProviderRecipeBytes) + 1U);
        pfree(serialized);
        if (serialized_bytes
                > static_cast<std::size_t>(kMaxProviderRecipeBytes)
            || serialized_bytes
                > kMaxProviderRecipeBytes - serialized_total) {
            return false;
        }
        serialized_total += serialized_bytes;
        BioseqCachePlanDigestRecipeV2 recipe{
            .struct_size = sizeof(BioseqCachePlanDigestRecipeV2),
            .flags = 0,
            .digest_format_version = static_cast<std::uint32_t>(
                recipe_format),
            .reserved = 0,
            .plan_domain_nodes = const_cast<List*>(
                static_cast<const List*>(plan_domain_node)),
        };
        std::memcpy(
            recipe.expected_generation_digest,
            expected.data(),
            expected.size());
        reuse::ProviderCallDiagnosticV1 diagnostic{};
        if (!reuse::validate_runtime_recipe_fail_open(
                provider,
                &provider_lease,
                &recipe,
                estate,
                estate->es_query_cxt,
                &diagnostic)
            || !append_key_scalar(key, provider_id_value)
            || !append_key_scalar(key, provider_abi_value)
            || !append_key_scalar(key, ordinal)
            || !append_key_field(
                key, expected.data(), expected.size())) {
            return false;
        }
        prior_ordinal = ordinal;
    }
    return append_exact_coverage_semantic_key(scan, estate, key);
}

#if defined(BIOSEQ_CACHE_TESTING)
[[nodiscard]] constexpr std::uint32_t pause_bit(
        TestPausePoint point) noexcept {
    return 1U << static_cast<std::uint32_t>(point);
}

[[nodiscard]] std::int64_t pause_lock_key(
        TestPausePoint point, int target_pid = 0) noexcept {
    const std::uint64_t database = static_cast<std::uint64_t>(MyDatabaseId);
    const std::uint64_t ordinal = static_cast<std::uint32_t>(point) + 1U;
    return static_cast<std::int64_t>(
        0x4253514300000000ULL ^ (database << 24U)
        ^ (static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(target_pid)) << 8U)
        ^ ordinal);
}

[[nodiscard]] TestPausePoint parse_pause_point(const char* name) {
    if (std::strcmp(name, "after_building_publish") == 0) {
        return TestPausePoint::AfterBuildingPublish;
    }
    if (std::strcmp(name, "after_waiter_registered") == 0) {
        return TestPausePoint::AfterWaiterRegistered;
    }
    if (std::strcmp(name, "before_terminal_publish") == 0) {
        return TestPausePoint::BeforeTerminalPublish;
    }
    if (std::strcmp(name, "after_reader_pinned") == 0) {
        return TestPausePoint::AfterReaderPinned;
    }
    if (std::strcmp(name, "before_coverage_preflight") == 0) {
        return TestPausePoint::BeforeCoveragePreflight;
    }
    if (std::strcmp(name, "after_coverage_reader_pinned") == 0) {
        return TestPausePoint::AfterCoverageReaderPinned;
    }
    if (std::strcmp(name, "before_visibility_ticket") == 0
        || std::strcmp(name, "before_dependency_visibility_ticket") == 0) {
        return TestPausePoint::BeforeDependencyVisibilityTicket;
    }
    if (std::strcmp(name, "after_transition_publish") == 0
        || std::strcmp(name, "after_dependency_transition_publish") == 0) {
        return TestPausePoint::AfterDependencyTransitionPublish;
    }
    ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("unknown bioseq_cache pause point: %s", name)));
    pg_unreachable();
}

[[nodiscard]] CacheBackendPauseControlV1* find_backend_pause_control(
        TestPausePoint point, int target_pid) noexcept {
    if (g_shared == nullptr || target_pid <= 0) {
        return nullptr;
    }
    for (CacheBackendPauseControlV1& pause
         : g_shared->test_control.backend_pauses) {
        if (pg_atomic_read_u32(&pause.state)
                != static_cast<std::uint32_t>(BackendPauseStateV1::Free)
            && pause.point == static_cast<std::uint32_t>(point)
            && pause.target_pid == target_pid) {
            return &pause;
        }
    }
    return nullptr;
}

/** Deterministic test pause compiled out of production builds. */
void maybe_pause(TestPausePoint point) {
    if (g_shared == nullptr) {
        return;
    }
    CacheTestControl& control = g_shared->test_control;
    for (CacheBackendPauseControlV1& backend : control.backend_pauses) {
        if (backend.point != static_cast<std::uint32_t>(point)
            || backend.target_pid != MyProcPid) {
            continue;
        }
        std::uint32_t expected = static_cast<std::uint32_t>(
            BackendPauseStateV1::Armed);
        if (!pg_atomic_compare_exchange_u32(
                &backend.state,
                &expected,
                static_cast<std::uint32_t>(
                    BackendPauseStateV1::Paused))) {
            continue;
        }
        ConditionVariableBroadcast(&control.changed);
        volatile bool lock_acquired = false;
        PG_TRY();
        {
            static_cast<void>(DirectFunctionCall1(
                pg_advisory_lock_int8,
                Int64GetDatum(pause_lock_key(point, MyProcPid))));
            lock_acquired = true;
            static_cast<void>(DirectFunctionCall1(
                pg_advisory_unlock_int8,
                Int64GetDatum(pause_lock_key(point, MyProcPid))));
            lock_acquired = false;
        }
        PG_CATCH();
        {
            if (lock_acquired) {
                static_cast<void>(DirectFunctionCall1(
                    pg_advisory_unlock_int8,
                    Int64GetDatum(pause_lock_key(point, MyProcPid))));
            }
            pg_atomic_write_u32(
                &backend.state,
                static_cast<std::uint32_t>(BackendPauseStateV1::Free));
            ConditionVariableBroadcast(&control.changed);
            PG_RE_THROW();
        }
        PG_END_TRY();
        pg_atomic_write_u32(
            &backend.state,
            static_cast<std::uint32_t>(BackendPauseStateV1::Free));
        ConditionVariableBroadcast(&control.changed);
        return;
    }
    const std::uint32_t bit = pause_bit(point);
    if ((pg_atomic_read_u32(&control.armed_mask) & bit) == 0) {
        return;
    }
    pg_atomic_fetch_or_u32(&control.paused_mask, bit);
    ConditionVariableBroadcast(&control.changed);
    volatile bool lock_acquired = false;
    PG_TRY();
    {
        /* A heavyweight advisory lock makes the pause visible to
         * pg_isolation_regress's blocked-session detector. */
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_lock_int8,
            Int64GetDatum(pause_lock_key(point))));
        lock_acquired = true;
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_unlock_int8,
            Int64GetDatum(pause_lock_key(point))));
        lock_acquired = false;
    }
    PG_CATCH();
    {
        if (lock_acquired) {
            static_cast<void>(DirectFunctionCall1(
                pg_advisory_unlock_int8,
                Int64GetDatum(pause_lock_key(point))));
        }
        pg_atomic_fetch_and_u32(&control.armed_mask, ~bit);
        pg_atomic_fetch_and_u32(&control.paused_mask, ~bit);
        pg_atomic_fetch_and_u32(&control.released_mask, ~bit);
        ConditionVariableBroadcast(&control.changed);
        PG_RE_THROW();
    }
    PG_END_TRY();
    pg_atomic_fetch_and_u32(&control.armed_mask, ~bit);
    pg_atomic_fetch_and_u32(&control.paused_mask, ~bit);
    pg_atomic_fetch_and_u32(&control.released_mask, ~bit);
    ConditionVariableBroadcast(&control.changed);
}
#else
enum class TestPausePoint : std::uint32_t {
    AfterBuildingPublish,
    AfterWaiterRegistered,
    BeforeTerminalPublish,
    AfterReaderPinned,
    BeforeCoveragePreflight,
    AfterCoverageReaderPinned,
    BeforeDependencyVisibilityTicket,
    AfterDependencyTransitionPublish,
};
inline void maybe_pause(TestPausePoint) noexcept {}
#endif

#if defined(BIOSEQ_CACHE_TESTING)
/** Route dependency-runtime race windows through the shared pause control. */
void dependency_runtime_test_pause(
        void*,
        dependency_runtime::DependencyRuntimePausePointV1 point) noexcept {
    switch (point) {
        case dependency_runtime::DependencyRuntimePausePointV1::
                BeforeVisibilityTicket:
            maybe_pause(TestPausePoint::BeforeDependencyVisibilityTicket);
            break;
        case dependency_runtime::DependencyRuntimePausePointV1::
                AfterTransitionPublish:
            maybe_pause(TestPausePoint::AfterDependencyTransitionPublish);
            break;
    }
}
#endif

struct RegistryDecision {
    RuntimeRole role = RuntimeRole::Bypass;
    int index = -1;
    std::uint64_t generation = 0;
    RegistryReaderClaim reader_claim{};
    CacheEntryId entry_id{};
    ReusableGenerationReferenceV1 reference{};
    ReusableReferenceTokenV1 reference_token{};
    ReusableGenerationSnapshotV1 completed_snapshot{};
    storage::ManifestHandle manifest_handle{};
    storage::ManifestWaiterToken manifest_waiter_token{};
    bool registered_inflight_waiter = false;
    bool ttl_entitlement_consumed = false;
};

[[nodiscard]] bool snapshot_current_reusable_generation(
    const ReusableGenerationReferenceV1& reference,
    ReusableGenerationSnapshotV1* output) noexcept;
[[nodiscard]] bool convert_runtime_waiter_to_reader(
    ReusableReferenceTokenV1& token) noexcept;
void release_runtime_reusable_reference(
    ReusableReferenceTokenV1& token) noexcept;
[[nodiscard]] int exact_runtime_recipe_count(
    const CustomScan& scan) noexcept;

/** ABA-safe identity of one exact producer while its generation is live. */
struct ExactProducerWaitTokenV1 {
    CacheEntryId entry_id{};
    ReusableProducerIdentityV1 producer{};
    std::uint64_t database_epoch = 0;
    std::uint64_t completion_count = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return entry_id.boot_nonce != 0 && entry_id.generation != 0
            && entry_id.claim_sequence != 0 && producer.valid()
            && database_epoch != 0;
    }
};

struct RegistryWaiterClaim {
    std::uint32_t slot =
        (std::numeric_limits<std::uint32_t>::max)();
    std::uint32_t reserved = 0;
    std::uint64_t registration_sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot < storage::kWaitersPerManifest
            && registration_sequence != 0;
    }
};

[[nodiscard]] CacheEntryId cache_entry_id(const CacheEntry& entry) noexcept {
    return CacheEntryId{
        .digest = entry.key_digest,
        .boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce),
        .generation = entry.generation,
        .claim_sequence = entry.claim_sequence,
    };
}

[[nodiscard]] ReusableProducerIdentityV1 exact_producer_identity(
        const CacheEntry& entry) noexcept {
    return ReusableProducerIdentityV1{
        .proc_number = static_cast<std::uint32_t>(
            entry.producer_proc_number),
        .pid = entry.producer_pid,
        .backend_nonce = entry.producer_backend_nonce,
        .backend_started_at_us = entry.producer_backend_started_at,
    };
}

[[nodiscard]] bool exact_entry_matches_wait_token(
        const CacheEntry& entry,
        const ExactProducerWaitTokenV1& expected,
        bool require_producer) noexcept {
    return static_cast<bool>(expected)
        && pg_atomic_read_u64(&g_shared->boot_nonce)
            == expected.entry_id.boot_nonce
        && entry.key_digest == expected.entry_id.digest
        && entry.generation == expected.entry_id.generation
        && entry.claim_sequence == expected.entry_id.claim_sequence
        && entry.database_epoch == expected.database_epoch
        && entry.completion_count == expected.completion_count
        && (!require_producer
            || exact_producer_identity(entry) == expected.producer);
}

[[nodiscard]] std::uint64_t next_claim_sequence() noexcept {
    std::uint64_t sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_claim_sequence, 1);
    if (sequence == 0) {
        sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_claim_sequence, 1);
    }
    return sequence == 0 ? 1 : sequence;
}

/** Collect every reusable claim owned by this backend without truncation. */
[[nodiscard]] bool collect_local_reusable_claim_sequences(
        std::span<std::uint64_t> output,
        std::size_t* output_count) noexcept {
    if (output_count == nullptr) {
        return false;
    }
    *output_count = 0;
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr) {
        return true;
    }
    if (output.empty()
        || output.size() > static_cast<std::size_t>(MaxAllocSize)
            / sizeof(ReusableGenerationDiagnosticsRowV1)) {
        return false;
    }
    auto* rows = static_cast<ReusableGenerationDiagnosticsRowV1*>(
        palloc_extended(
            sizeof(ReusableGenerationDiagnosticsRowV1) * output.size(),
            MCXT_ALLOC_NO_OOM));
    if (rows == nullptr) {
        return false;
    }
    std::size_t count = 0;
    bool complete = false;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
        membership_owned = true;
        const auto diagnostics = snapshot_reusable_generation_diagnostics(
            g_reusable_registry,
            std::span<ReusableGenerationDiagnosticsRowV1>{
                rows, output.size()},
            reusable_partition_callbacks());
        complete = diagnostics.error == ReusableRegistryErrorV1::None
            && !diagnostics.truncated;
        if (complete) {
            const ReusableProducerIdentityV1 local =
                current_reusable_producer_identity();
            for (std::size_t index = 0;
                 index < diagnostics.count && count < output.size();
                 ++index) {
                if ((rows[index].state != CacheEntryState::Claimed
                        && rows[index].state != CacheEntryState::Building)
                    || rows[index].producer_pid != local.pid) {
                    continue;
                }
                const auto snapshot = wait_reusable_generation(
                    g_reusable_registry,
                    rows[index].reference,
                    reusable_partition_callbacks());
                if (!snapshot.has_value()
                    || snapshot->reference != rows[index].reference
                    || (snapshot->state != CacheEntryState::Claimed
                        && snapshot->state != CacheEntryState::Building)
                    || snapshot->producer != local
                    || snapshot->reference.id.claim_sequence == 0) {
                    continue;
                }
                output[count++] = snapshot->reference.id.claim_sequence;
            }
        }
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            pfree(rows);
            PG_RE_THROW();
        }
        FlushErrorState();
        complete = false;
        count = 0;
    }
    PG_END_TRY();
    pfree(rows);
    *output_count = complete ? count : 0;
    return complete;
}

/** Snapshot all exact and reusable claims owned by this backend. */
[[nodiscard]] bool collect_backend_owned_claim_sequences(
        std::span<std::uint64_t> output,
        std::size_t* exact_count,
        std::size_t* output_count) noexcept {
    if (exact_count == nullptr || output_count == nullptr
        || output.size() < kRegistryCapacity
        || g_shared == nullptr || g_registry_lock == nullptr) {
        return false;
    }
    *exact_count = 0;
    *output_count = 0;
    const ReusableProducerIdentityV1 local =
        current_reusable_producer_identity();
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const CacheEntry& entry : g_shared->entries) {
        if (exact_entry_in_flight(entry.state)
            && exact_producer_identity(entry) == local
            && entry.claim_sequence != 0) {
            output[(*output_count)++] = entry.claim_sequence;
        }
    }
    LWLockRelease(g_registry_lock);

    std::sort(output.begin(), output.begin() + *output_count);
    const auto exact_unique_end = std::unique(
        output.begin(), output.begin() + *output_count);
    *exact_count = static_cast<std::size_t>(
        exact_unique_end - output.begin());
    *output_count = *exact_count;

    std::size_t reusable_count = 0;
    if (!collect_local_reusable_claim_sequences(
            output.subspan(*output_count), &reusable_count)) {
        *output_count = 0;
        return false;
    }
    *output_count += reusable_count;
    std::sort(
        output.begin() + *exact_count,
        output.begin() + *output_count);
    const auto reusable_unique_end = std::unique(
        output.begin() + *exact_count,
        output.begin() + *output_count);
    *output_count = static_cast<std::size_t>(
        reusable_unique_end - output.begin());
    return true;
}

[[nodiscard]] ReusableProducerIdentityV1 registry_owner_identity(
        const RegistryReaderOwner& owner) noexcept {
    return ReusableProducerIdentityV1{
        .proc_number = owner.owner_proc_number,
        .pid = owner.owner_pid,
        .backend_nonce = owner.owner_backend_nonce,
        .backend_started_at_us = owner.owner_backend_started_at,
    };
}

[[nodiscard]] ReusableProducerIdentityV1 registry_owner_identity(
        const RegistryWaiterOwner& owner) noexcept {
    return ReusableProducerIdentityV1{
        .proc_number = owner.owner_proc_number,
        .pid = owner.owner_pid,
        .backend_nonce = owner.owner_backend_nonce,
        .backend_started_at_us = owner.owner_backend_started_at,
    };
}

struct ExactRegistryOwnerObservationV1 {
    ReusableProducerIdentityV1 owner{};
    std::uint64_t registration_sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner.valid() && registration_sequence != 0;
    }
};

struct ExactRegistrySweepWorkspaceV1 {
    std::array<ExactProducerWaitTokenV1, kRegistryCapacity>
        producers{};
    std::array<std::array<ExactRegistryOwnerObservationV1,
        storage::kReaderPinsPerManifest>, kRegistryCapacity> readers{};
    std::array<std::array<ExactRegistryOwnerObservationV1,
        storage::kWaitersPerManifest>, kRegistryCapacity> waiters{};
    BackendIdentityBatchV1 identities{};
    BackendLivenessSampleV1 liveness{};
};

[[nodiscard]] ExactRegistryOwnerObservationV1 registry_owner_observation(
        const RegistryReaderOwner& owner) noexcept {
    return ExactRegistryOwnerObservationV1{
        .owner = registry_owner_identity(owner),
        .registration_sequence = owner.registration_sequence,
    };
}

[[nodiscard]] ExactRegistryOwnerObservationV1 registry_owner_observation(
        const RegistryWaiterOwner& owner) noexcept {
    return ExactRegistryOwnerObservationV1{
        .owner = registry_owner_identity(owner),
        .registration_sequence = owner.registration_sequence,
    };
}

[[nodiscard]] bool registry_owner_matches_observation(
        const RegistryReaderOwner& owner,
        const ExactRegistryOwnerObservationV1& observation) noexcept {
    return owner.active && observation
        && owner.registration_sequence == observation.registration_sequence
        && registry_owner_identity(owner) == observation.owner;
}

[[nodiscard]] bool registry_owner_matches_observation(
        const RegistryWaiterOwner& owner,
        const ExactRegistryOwnerObservationV1& observation) noexcept {
    return owner.active && observation
        && owner.registration_sequence == observation.registration_sequence
        && registry_owner_identity(owner) == observation.owner;
}

void clear_registry_reader_locked(
        CacheEntry& entry,
        RegistryReaderOwner& reader) noexcept {
    if (!reader.active) {
        return;
    }
    reader = RegistryReaderOwner{};
    if (entry.reader_count > 0) {
        --entry.reader_count;
    }
}

void reap_dead_registry_readers_locked(
        CacheEntry& entry,
        std::span<const ExactRegistryOwnerObservationV1> observations,
        const BackendLivenessSampleV1& liveness) noexcept {
    if (observations.size() != storage::kReaderPinsPerManifest) {
        return;
    }
    for (std::size_t index = 0;
         index < storage::kReaderPinsPerManifest; ++index) {
        RegistryReaderOwner& reader = entry.readers[index];
        if (registry_owner_matches_observation(reader, observations[index])
            && !backend_identity_alive_in_sample(
                liveness, registry_owner_identity(reader))) {
            clear_registry_reader_locked(entry, reader);
        }
    }
}

[[nodiscard]] RegistryReaderClaim claim_registry_reader_locked(
        CacheEntry& entry) noexcept {
    for (std::uint32_t index = 0;
         index < storage::kReaderPinsPerManifest; ++index) {
        RegistryReaderOwner& reader = entry.readers[index];
        if (reader.active) {
            continue;
        }
        std::uint64_t sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_registry_reader_sequence, 1);
        if (sequence == 0) {
            sequence = pg_atomic_fetch_add_u64(
                &g_shared->next_registry_reader_sequence, 1);
        }
        if (sequence == 0) {
            sequence = 1;
        }
        const ReusableProducerIdentityV1 owner =
            current_reusable_producer_identity();
        reader = RegistryReaderOwner{
            .active = true,
            .owner_pid = MyProcPid,
            .owner_proc_number = static_cast<std::uint32_t>(MyProcNumber),
            .owner_backend_nonce = owner.backend_nonce,
            .owner_backend_started_at = owner.backend_started_at_us,
            .registration_sequence = sequence,
        };
        ++entry.reader_count;
        return RegistryReaderClaim{
            .slot = index,
            .registration_sequence = sequence,
        };
    }
    return {};
}

void clear_registry_waiter_locked(
        CacheEntry& entry,
        RegistryWaiterOwner& waiter) noexcept {
    if (!waiter.active) {
        return;
    }
    waiter = RegistryWaiterOwner{};
    if (entry.waiter_count > 0) {
        --entry.waiter_count;
    }
}

void reap_dead_registry_waiters_locked(
        CacheEntry& entry,
        bool storage_attached,
        std::span<const ExactRegistryOwnerObservationV1> observations,
        const BackendLivenessSampleV1& liveness) noexcept {
    if (observations.size() != storage::kWaitersPerManifest) {
        return;
    }
    for (std::size_t index = 0;
         index < storage::kWaitersPerManifest; ++index) {
        RegistryWaiterOwner& waiter = entry.waiters[index];
        if (!registry_owner_matches_observation(
                waiter, observations[index])
            || backend_identity_alive_in_sample(
                liveness, registry_owner_identity(waiter))) {
            continue;
        }
        if (waiter.storage_token) {
            if (!storage_attached) {
                continue;
            }
            static_cast<void>(storage::unregister_manifest_waiter(
                waiter.storage_token));
        }
        clear_registry_waiter_locked(entry, waiter);
    }
}

[[nodiscard]] RegistryWaiterClaim claim_registry_waiter_locked(
        CacheEntry& entry) noexcept {
    for (std::uint32_t index = 0;
         index < storage::kWaitersPerManifest; ++index) {
        RegistryWaiterOwner& waiter = entry.waiters[index];
        if (waiter.active) {
            continue;
        }
        std::uint64_t sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_registry_waiter_sequence, 1);
        if (sequence == 0) {
            sequence = pg_atomic_fetch_add_u64(
                &g_shared->next_registry_waiter_sequence, 1);
        }
        if (sequence == 0) {
            sequence = 1;
        }
        const ReusableProducerIdentityV1 owner =
            current_reusable_producer_identity();
        waiter = RegistryWaiterOwner{
            .active = true,
            .owner_pid = MyProcPid,
            .owner_proc_number = static_cast<std::uint32_t>(MyProcNumber),
            .owner_backend_nonce = owner.backend_nonce,
            .owner_backend_started_at = owner.backend_started_at_us,
            .registration_sequence = sequence,
            .registered_at = GetCurrentTimestamp(),
        };
        ++entry.waiter_count;
        return RegistryWaiterClaim{
            .slot = index,
            .registration_sequence = sequence,
        };
    }
    return {};
}

[[nodiscard]] bool take_registry_waiter_locked(
        CacheEntry& entry,
        const RegistryWaiterClaim& claim,
        storage::ManifestWaiterToken& token) noexcept {
    if (!claim) {
        return false;
    }
    RegistryWaiterOwner& waiter = entry.waiters[claim.slot];
    if (!waiter.active
        || waiter.registration_sequence != claim.registration_sequence
        || registry_owner_identity(waiter)
            != current_reusable_producer_identity()) {
        return false;
    }
    token = waiter.storage_token;
    clear_registry_waiter_locked(entry, waiter);
    return true;
}

/** Compare every key byte after the hash bucket shortcut. */
[[nodiscard]] bool entry_key_matches(
        const CacheEntry& entry, std::uint64_t key_hash,
        const StringInfo key) noexcept {
    return entry.key_hash == key_hash
        && entry.key_size == static_cast<std::uint32_t>(key->len)
        && std::memcmp(entry.key, key->data, key->len) == 0;
}

/** Complete entries are usable only in their captured database epoch. */
[[nodiscard]] bool entry_is_current(
        const CacheEntry& entry, std::uint64_t database_epoch,
        TimestampTz now) noexcept {
    return entry.state == SharedEntryState::Complete
        && static_cast<bool>(entry.manifest_handle)
        && entry.database_epoch == database_epoch
        && entry.expires_at > now;
}

/** Mark an abandoned producer failed while holding the registry lock. */
void fail_abandoned_producer(CacheEntry& entry) noexcept {
    entry.state = SharedEntryState::Failed;
    entry.producer_pid = 0;
    entry.producer_proc_number = INVALID_PROC_NUMBER;
    entry.producer_backend_nonce = 0;
    entry.producer_backend_started_at = 0;
    ConditionVariableBroadcast(&entry.changed);
}

/** Make one unreferenced terminal slot reusable without resetting its ABA id. */
void recycle_registry_entry_locked(CacheEntry& entry) noexcept {
    const std::uint64_t generation = entry.generation;
    entry.key_size = 0;
    entry.claim_sequence = 0;
    entry.key_hash = 0;
    entry.key_digest = {};
    entry.database_epoch = 0;
    entry.completion_count = 0;
    entry.database_oid = InvalidOid;
    entry.current_role_oid = InvalidOid;
    entry.session_user_oid = InvalidOid;
    entry.producer_pid = 0;
    entry.producer_proc_number = INVALID_PROC_NUMBER;
    entry.producer_backend_nonce = 0;
    entry.producer_backend_started_at = 0;
    entry.waiter_count = 0;
    entry.reader_count = 0;
    std::memset(entry.readers, 0, sizeof(entry.readers));
    std::memset(entry.waiters, 0, sizeof(entry.waiters));
    entry.created_at = 0;
    entry.completed_at = 0;
    entry.expires_at = 0;
    entry.row_count = 0;
    entry.logical_bytes = 0;
    entry.stored_bytes = 0;
    entry.hits = 0;
    entry.relation_count = 0;
    entry.syscache_dependency_count = 0;
    std::memset(entry.relation_oids, 0, sizeof(entry.relation_oids));
    std::memset(
        entry.syscache_dependencies, 0,
        sizeof(entry.syscache_dependencies));
    entry.manifest_handle = {};
    entry.display_sql.fill(0);
    entry.canonical_plan_bytes = 0;
    entry.producer_plan_bytes = 0;
    entry.canonical_plan_status =
        ReusablePlanSnapshotStatusV1::Unavailable;
    entry.producer_plan_status =
        ReusablePlanSnapshotStatusV1::Unavailable;
    entry.display_sql_truncated = false;
    entry.canonical_plan_truncated = false;
    entry.producer_plan_truncated = false;
    entry.reserved_diagnostic = 0;
    entry.canonical_plan.fill(0);
    entry.producer_plan.fill(0);
    std::memset(entry.key, 0, sizeof(entry.key));
    entry.generation = generation;
    pg_write_barrier();
    entry.state = SharedEntryState::Empty;
    ConditionVariableBroadcast(&entry.changed);
}

/** Wait for a matching producer; pre-existing waiters may consume TTL-zero. */
[[nodiscard]] RegistryDecision wait_for_producer(
        int index,
        const ExactProducerWaitTokenV1& expected,
        const RegistryWaiterClaim& waiter_claim,
        const VisibilityTicket& visibility,
        std::span<const std::uint64_t> locally_owned_exact_claim_sequences,
        std::span<const std::uint64_t> locally_owned_reusable_claim_sequences) {
    RegistryDecision decision{};
    CacheEntry& entry = g_shared->entries[index];
    storage::ManifestWaiterToken waiter_to_release{};
    ConditionVariablePrepareToSleep(&entry.changed);
    PG_TRY();
    {
        maybe_pause(TestPausePoint::AfterWaiterRegistered);
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            if (!visibility_ticket_still_bound(visibility)
                || expected.producer
                    == current_reusable_producer_identity()
                || !locally_owned_reusable_claim_sequences.empty()
                || !reuse::claim_order_allows_wait(
                    expected.entry_id.claim_sequence,
                    locally_owned_exact_claim_sequences)) {
                break;
            }
            LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
            const bool identity_matches = exact_entry_matches_wait_token(
                entry, expected, false);
            if (!identity_matches
                || entry.state == SharedEntryState::Failed
                || entry.state == SharedEntryState::Retired
                || entry.state == SharedEntryState::Empty) {
                if (identity_matches) {
                    static_cast<void>(take_registry_waiter_locked(
                        entry, waiter_claim, waiter_to_release));
                }
                LWLockRelease(g_registry_lock);
                break;
            }
            if (entry.state == SharedEntryState::Complete) {
                storage::ManifestWaiterToken waiter_token{};
                const bool claimed = take_registry_waiter_locked(
                    entry, waiter_claim, waiter_token);
                if (claimed && waiter_token) {
                    const RegistryReaderClaim reader_claim =
                        claim_registry_reader_locked(entry);
                    if (reader_claim) {
                        ++entry.hits;
                        decision.role = RuntimeRole::Consumer;
                        decision.index = index;
                        decision.generation = expected.entry_id.generation;
                        decision.reader_claim = reader_claim;
                        decision.entry_id = cache_entry_id(entry);
                        decision.manifest_handle = entry.manifest_handle;
                        decision.manifest_waiter_token = waiter_token;
                    } else {
                        waiter_to_release = waiter_token;
                    }
                } else if (waiter_token) {
                    waiter_to_release = waiter_token;
                }
                LWLockRelease(g_registry_lock);
                break;
            }
            const bool producer_matches = exact_entry_in_flight(entry.state)
                && exact_entry_matches_wait_token(entry, expected, true);
            if (!producer_matches) {
                LWLockRelease(g_registry_lock);
                break;
            }
            LWLockRelease(g_registry_lock);

            if (!reusable_producer_alive(nullptr, expected.producer)) {
                LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
                if (exact_entry_in_flight(entry.state)
                    && exact_entry_matches_wait_token(
                        entry, expected, true)) {
                    fail_abandoned_producer(entry);
                }
                LWLockRelease(g_registry_lock);
                continue;
            }
            static_cast<void>(ConditionVariableTimedSleep(
                &entry.changed, 1000L, PG_WAIT_EXTENSION));
        }
        if (decision.role != RuntimeRole::Consumer) {
            LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
            if (exact_entry_matches_wait_token(entry, expected, false)) {
                static_cast<void>(take_registry_waiter_locked(
                    entry, waiter_claim, waiter_to_release));
            }
            LWLockRelease(g_registry_lock);
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        if (exact_entry_matches_wait_token(entry, expected, false)) {
            static_cast<void>(take_registry_waiter_locked(
                entry, waiter_claim, waiter_to_release));
        }
        LWLockRelease(g_registry_lock);
        if (waiter_to_release) {
            static_cast<void>(
                storage::unregister_manifest_waiter(waiter_to_release));
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (waiter_to_release) {
        static_cast<void>(
            storage::unregister_manifest_waiter(waiter_to_release));
    }
    return decision;
}

/** Select an exact hit, wait on its producer, or claim one bounded slot. */
[[nodiscard]] RegistryDecision acquire_legacy_registry_entry(
        const StringInfo key,
        const VisibilityTicket& visibility,
        std::span<const std::uint64_t> locally_owned_exact_claim_sequences,
        std::span<const std::uint64_t> locally_owned_reusable_claim_sequences,
        const char* display_sql, List* relation_oids,
        List* syscache_dependencies) {
    RegistryDecision decision{};
    const std::uint64_t database_epoch = visibility.database_epoch;
    const std::uint64_t completion_count = visibility.completion_count;
    if (relation_oids == nullptr) {
        relation_oids = NIL;
    }
    if (syscache_dependencies == nullptr) {
        syscache_dependencies = NIL;
    }
    const int relation_count = list_length(relation_oids);
    const int syscache_dependency_count = list_length(
        syscache_dependencies);
    if (relation_count + syscache_dependency_count
            > static_cast<int>(kMaxDependenciesPerGeneration)
        || relation_count > static_cast<int>(kMaximumRelationDependencies)
        || syscache_dependency_count
            > static_cast<int>(kMaxDependenciesPerGeneration)) {
        return decision;
    }
    ListCell* validation_cell = nullptr;
    foreach(validation_cell, relation_oids) {
        if (!OidIsValid(lfirst_oid(validation_cell))) {
            return decision;
        }
    }
    foreach(validation_cell, syscache_dependencies) {
        const auto* dependency = static_cast<const PlanInvalItem*>(
            lfirst(validation_cell));
        if (dependency == nullptr || !IsA(dependency, PlanInvalItem)
            || dependency->cacheId < 0
            || dependency->cacheId >= SysCacheSize) {
            return decision;
        }
    }
    PostgreSqlColumnChecksumPolicy checksum_policy;
    const auto digest = checksum_policy.sha256(std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(key->data),
        static_cast<std::size_t>(key->len),
    });
    if (!digest.has_value()) {
        return decision;
    }
    const std::uint64_t key_hash = hash_any_extended(
        reinterpret_cast<const unsigned char*>(key->data), key->len,
        pg_atomic_read_u64(&g_shared->boot_nonce));
    const TimestampTz now = GetCurrentTimestamp();
    int reusable_index = -1;
    int lru_index = -1;
    TimestampTz oldest_completion = DT_NOEND;
    TimestampTz oldest_live_completion = DT_NOEND;
    storage::ManifestHandle retired_manifest{};

    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t offset = 0; offset < kRegistryCapacity; ++offset) {
        CacheEntry& entry = g_shared->entries[offset];
        if (entry_key_matches(entry, key_hash, key)) {
            if (entry_is_current(entry, database_epoch, now)) {
                const RegistryReaderClaim reader_claim =
                    claim_registry_reader_locked(entry);
                if (!reader_claim) {
                    LWLockRelease(g_registry_lock);
                    return decision;
                }
                ++entry.hits;
                decision.role = RuntimeRole::Consumer;
                decision.index = static_cast<int>(offset);
                decision.generation = entry.generation;
                decision.reader_claim = reader_claim;
                decision.entry_id = cache_entry_id(entry);
                decision.manifest_handle = entry.manifest_handle;
                LWLockRelease(g_registry_lock);
                return decision;
            }
            if (exact_entry_in_flight(entry.state)
                && entry.database_epoch == database_epoch
                && entry.completion_count == completion_count) {
                const ExactProducerWaitTokenV1 expected{
                    .entry_id = cache_entry_id(entry),
                    .producer = exact_producer_identity(entry),
                    .database_epoch = entry.database_epoch,
                    .completion_count = entry.completion_count,
                };
                if (!expected
                    || expected.producer
                        == current_reusable_producer_identity()
                    || !locally_owned_reusable_claim_sequences.empty()
                    || !reuse::claim_order_allows_wait(
                        expected.entry_id.claim_sequence,
                        locally_owned_exact_claim_sequences)) {
                    LWLockRelease(g_registry_lock);
                    return decision;
                }
                const RegistryWaiterClaim waiter_claim =
                    claim_registry_waiter_locked(entry);
                if (!waiter_claim) {
                    LWLockRelease(g_registry_lock);
                    return decision;
                }
                LWLockRelease(g_registry_lock);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.inflight_waits, 1);
                }
                return wait_for_producer(
                    static_cast<int>(offset),
                    expected,
                    waiter_claim,
                    visibility,
                    locally_owned_exact_claim_sequences,
                    locally_owned_reusable_claim_sequences);
            }
        }

        const bool unreferenced = entry.reader_count == 0
            && entry.waiter_count == 0;
        if (!unreferenced || exact_entry_in_flight(entry.state)) {
            continue;
        }
        if (entry.state == SharedEntryState::Empty
            || entry.state == SharedEntryState::Failed
            || entry.state == SharedEntryState::Retired) {
            if (reusable_index < 0) {
                reusable_index = static_cast<int>(offset);
            }
            continue;
        }
        if (entry.expires_at <= now
            && (reusable_index < 0
                || entry.completed_at < oldest_completion)) {
            reusable_index = static_cast<int>(offset);
            oldest_completion = entry.completed_at;
            continue;
        }
        if (entry.state == SharedEntryState::Complete
            && entry.completed_at < oldest_live_completion) {
            lru_index = static_cast<int>(offset);
            oldest_live_completion = entry.completed_at;
        }
    }

    if (reusable_index < 0) {
        reusable_index = lru_index;
    }

    if (reusable_index >= 0) {
        CacheEntry& entry = g_shared->entries[reusable_index];
        retired_manifest = entry.manifest_handle;
        const std::uint64_t next_generation = entry.generation + 1;
        entry.state = SharedEntryState::Claimed;
        entry.key_size = static_cast<std::uint32_t>(key->len);
        entry.generation = next_generation == 0 ? 1 : next_generation;
        entry.claim_sequence = next_claim_sequence();
        entry.key_hash = key_hash;
        entry.key_digest = *digest;
        entry.database_epoch = database_epoch;
        entry.completion_count = completion_count;
        entry.database_oid = MyDatabaseId;
        entry.current_role_oid = GetUserId();
        entry.session_user_oid = GetSessionUserId();
        entry.producer_pid = MyProcPid;
        entry.producer_proc_number = MyProcNumber;
        const ReusableProducerIdentityV1 producer =
            current_reusable_producer_identity();
        entry.producer_backend_nonce = producer.backend_nonce;
        entry.producer_backend_started_at = producer.backend_started_at_us;
        entry.waiter_count = 0;
        entry.reader_count = 0;
        std::memset(entry.readers, 0, sizeof(entry.readers));
        std::memset(entry.waiters, 0, sizeof(entry.waiters));
        entry.created_at = now;
        entry.completed_at = 0;
        entry.expires_at = 0;
        entry.row_count = 0;
        entry.logical_bytes = 0;
        entry.stored_bytes = 0;
        entry.hits = 0;
        entry.relation_count = 0;
        entry.syscache_dependency_count = 0;
        std::memset(entry.relation_oids, 0, sizeof(entry.relation_oids));
        std::memset(
            entry.syscache_dependencies, 0,
            sizeof(entry.syscache_dependencies));
        ListCell* relation_cell = nullptr;
        foreach(relation_cell, relation_oids) {
            entry.relation_oids[entry.relation_count++] =
                lfirst_oid(relation_cell);
        }
        ListCell* syscache_cell = nullptr;
        foreach(syscache_cell, syscache_dependencies) {
            const auto* dependency = static_cast<const PlanInvalItem*>(
                lfirst(syscache_cell));
            entry.syscache_dependencies[
                entry.syscache_dependency_count++] =
                ExactSyscacheDependencyV1{
                    .cache_id = dependency->cacheId,
                    .hash_value = dependency->hashValue,
                };
        }
        entry.manifest_handle = {};
        const std::size_t display_sql_size = strlcpy(
            entry.display_sql.data(),
            display_sql == nullptr ? "<query text unavailable>" : display_sql,
            entry.display_sql.size());
        entry.canonical_plan_bytes = 0;
        entry.producer_plan_bytes = 0;
        entry.canonical_plan_status =
            ReusablePlanSnapshotStatusV1::Unavailable;
        entry.producer_plan_status =
            ReusablePlanSnapshotStatusV1::Unavailable;
        entry.display_sql_truncated =
            display_sql_size >= entry.display_sql.size();
        entry.canonical_plan_truncated = false;
        entry.producer_plan_truncated = false;
        entry.reserved_diagnostic = 0;
        entry.canonical_plan.fill(0);
        entry.producer_plan.fill(0);
        std::memcpy(entry.key, key->data, key->len);
        decision.role = RuntimeRole::Producer;
        decision.index = reusable_index;
        decision.generation = entry.generation;
        decision.entry_id = cache_entry_id(entry);
    }
    LWLockRelease(g_registry_lock);

    if (retired_manifest) {
        static_cast<void>(storage::retire_manifest(retired_manifest));
        if (g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.evictions, 1);
        }
    }
    return decision;
}

/** Evaluate producer and consumer TTLs from the registry's monotonic clocks. */
[[nodiscard]] TtlDecision exact_reusable_ttl_decision(
        const ReusableGenerationSnapshotV1& snapshot,
        bool registered_inflight_waiter,
        bool entitlement_already_consumed) noexcept {
    if (snapshot.state != CacheEntryState::Complete
        || snapshot.completed_monotonic_ns == 0) {
        return {};
    }
    return completed_ttl_decision(
        CacheTimestamps{
            .started_ns = snapshot.started_monotonic_ns,
            .completed_ns = snapshot.completed_monotonic_ns,
        },
        snapshot.metadata.producer_ttl_ms,
        static_cast<std::uint64_t>(std::max(guc::ttl_ms(), 0)),
        reusable_monotonic_now_ns(),
        registered_inflight_waiter,
        entitlement_already_consumed);
}

/** Wait on one unified exact generation with full ABA and owner rechecks. */
[[nodiscard]] RegistryDecision wait_for_unified_exact_producer(
        ReusableGenerationReferenceV1 reference,
        ReusableReferenceTokenV1 waiter,
        const ReusableProducerIdentityV1& expected_producer,
        const VisibilityTicket& visibility,
        std::span<const std::uint64_t> locally_owned_exact_claim_sequences,
        std::span<const std::uint64_t> locally_owned_reusable_claim_sequences) {
    RegistryDecision decision{};
    ConditionVariable* changed = reusable_generation_changed(reference);
    if (!reference || !waiter.active
        || waiter.kind != ReusableReferenceKindV1::Waiter
        || changed == nullptr || !expected_producer.valid()
        || expected_producer == current_reusable_producer_identity()
        || !locally_owned_reusable_claim_sequences.empty()
        || !reuse::claim_order_allows_wait(
            reference.id.claim_sequence,
            locally_owned_exact_claim_sequences)) {
        release_runtime_reusable_reference(waiter);
        return decision;
    }

    storage::ManifestWaiterToken manifest_waiter{};
    ConditionVariablePrepareToSleep(changed);
    PG_TRY();
    {
        maybe_pause(TestPausePoint::AfterWaiterRegistered);
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            if (!visibility_ticket_still_bound(visibility)
                || expected_producer
                    == current_reusable_producer_identity()
                || !locally_owned_reusable_claim_sequences.empty()
                || !reuse::claim_order_allows_wait(
                    reference.id.claim_sequence,
                    locally_owned_exact_claim_sequences)) {
                break;
            }
            ReusableGenerationSnapshotV1 snapshot{};
            if (!snapshot_current_reusable_generation(reference, &snapshot)
                || snapshot.reference != reference
                || snapshot.entry_kind != CacheEntryKind::ExactFinalResult
                || snapshot.producer != expected_producer) {
                break;
            }
            if (snapshot.state == CacheEntryState::Complete) {
                const TtlDecision ttl = exact_reusable_ttl_decision(
                    snapshot, true, false);
                const storage::RegisterManifestWaiterResult entitlement =
                    snapshot.manifest
                    ? storage::current_manifest_waiter(snapshot.reference.id)
                    : storage::RegisterManifestWaiterResult{
                        .error = storage::StorageError::NotFound,
                    };
                if (snapshot.manifest && ttl.readable
                    && visibility_ticket_still_bound(visibility)
                    && entitlement
                    && convert_runtime_waiter_to_reader(waiter)) {
                    manifest_waiter = entitlement.token;
                    ReusableGenerationSnapshotV1 rebound{};
                    if (snapshot_current_reusable_generation(
                            reference, &rebound)
                        && rebound.reference == reference
                        && rebound.entry_kind
                            == CacheEntryKind::ExactFinalResult
                        && rebound.state == CacheEntryState::Complete
                        && rebound.manifest == snapshot.manifest
                        && exact_reusable_ttl_decision(
                            rebound, true, false).readable
                        && visibility_ticket_still_bound(visibility)) {
                        const VisibilityTicket refreshed =
                            mint_visibility_ticket(
                                rebound.metadata.observability,
                                VisibilityTicketPurposeV1::Continuation);
                        const bool dependencies_stable = refreshed.valid
                            && validate_reusable_generation_dependencies_locked(
                                rebound,
                                refreshed.completion_count);
                        if (!dependencies_stable) {
                            break;
                        }
                        decision.role = RuntimeRole::Consumer;
                        decision.entry_id = reference.id;
                        decision.reference = reference;
                        decision.reference_token = waiter;
                        decision.completed_snapshot = rebound;
                        decision.manifest_handle = storage_manifest_handle(
                            rebound.manifest);
                        decision.manifest_waiter_token = manifest_waiter;
                        decision.registered_inflight_waiter = true;
                        decision.ttl_entitlement_consumed = true;
                        manifest_waiter = {};
                        waiter.active = false;
                    }
                }
                break;
            }
            if (snapshot.state == CacheEntryState::Failed
                || snapshot.state == CacheEntryState::Invalidated
                || snapshot.state == CacheEntryState::Evicting
                || (snapshot.state != CacheEntryState::Claimed
                    && snapshot.state != CacheEntryState::Building)) {
                break;
            }
            if (!reusable_producer_alive(nullptr, expected_producer)) {
                fail_current_reusable_generation(
                    reference, CacheFailureCode::ProducerExited);
                continue;
            }
            static_cast<void>(ConditionVariableTimedSleep(
                changed, 1000L, PG_WAIT_EXTENSION));
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        if (manifest_waiter) {
            static_cast<void>(storage::unregister_manifest_waiter(
                manifest_waiter));
            manifest_waiter = {};
        }
        release_runtime_reusable_reference(waiter);
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (manifest_waiter) {
        static_cast<void>(storage::unregister_manifest_waiter(
            manifest_waiter));
    }
    release_runtime_reusable_reference(waiter);
    return decision;
}

using ExactDependencyEvidenceV1 = planner::ExactDependencyEvidenceV1;

/** Associate complete provider wrappers with one relation-bearing subtree. */
[[nodiscard]] ExactDependencyEvidenceV1 exact_dependency_evidence(
        const Plan* plan,
        EState* estate,
        Oid relation_oid,
        const provider_registry::ProviderSnapshotV2& providers,
        std::size_t& visited) noexcept {
    return planner::exact_dependency_evidence(
        plan,
        estate,
        relation_oid,
        providers,
        visited,
        kMaximumExactPlanWalkNodesV1);
}

struct ExactDependencyVersionsV1 {
    std::array<DependencyVersion, kMaxDependenciesPerGeneration> values{};
    std::uint32_t count = 0;
    std::uint32_t provider_id = 0;
    std::uint32_t provider_abi_version = 0;
    bool requires_completion_guard = false;
    bool valid = false;
};

/** Bounded canonical-set insertion; duplicate conflicts fail open. */
[[nodiscard]] bool insert_exact_dependency_version(
        ExactDependencyVersionsV1& result,
        const DependencyVersion& candidate) noexcept {
    for (std::uint32_t index = 0; index < result.count; ++index) {
        if (result.values[index].key == candidate.key) {
            return result.values[index] == candidate;
        }
    }
    if (result.count >= result.values.size()) {
        return false;
    }
    result.values[result.count++] = candidate;
    return true;
}

/** Build the canonical relation/object vector and its conservative backstop. */
[[nodiscard]] ExactDependencyVersionsV1 exact_dependency_versions(
        const CustomScan& scan,
        EState* estate,
        List* relation_oids,
        List* syscache_dependencies) noexcept {
    ExactDependencyVersionsV1 result{};
    if (estate == nullptr || !OidIsValid(MyDatabaseId)) {
        return result;
    }
    relation_oids = relation_oids == nullptr ? NIL : relation_oids;
    syscache_dependencies = syscache_dependencies == nullptr
        ? NIL : syscache_dependencies;
    const int relation_count = list_length(relation_oids);
    const int syscache_count = list_length(syscache_dependencies);
    if (relation_count < 0 || syscache_count < 0) {
        return result;
    }
    const provider_registry::ProviderSnapshotV2 providers =
        provider_registry::snapshot_providers_v2();
    const Plan* child = scan.custom_plans != NIL
            && list_length(scan.custom_plans) == 1
        ? static_cast<const Plan*>(linitial(scan.custom_plans))
        : nullptr;

    ListCell* cell = nullptr;
    foreach(cell, relation_oids) {
        const Oid relation_oid = lfirst_oid(cell);
        if (!OidIsValid(relation_oid)) {
            return {};
        }
        PhysicalIdentityV1 identity{};
        const RelationPhysicalIdentityStatusV1 identity_status =
            relation_physical_identity(relation_oid, &identity);
        std::size_t visited = 0;
        ExactDependencyEvidenceV1 evidence = exact_dependency_evidence(
            child, estate, relation_oid, providers, visited);
        if (estate->es_plannedstmt != nullptr) {
            const ListCell* subplan_cell = nullptr;
            foreach(subplan_cell, estate->es_plannedstmt->subplans) {
                ExactDependencyEvidenceV1 subplan = exact_dependency_evidence(
                    static_cast<const Plan*>(lfirst(subplan_cell)),
                    estate, relation_oid, providers, visited);
                evidence.references_relation = evidence.references_relation
                    || subplan.references_relation;
                evidence.complete_provider = evidence.complete_provider
                    || subplan.complete_provider;
                evidence.provider_ambiguous = evidence.provider_ambiguous
                    || subplan.provider_ambiguous;
                if (subplan.provider_id != 0) {
                    if (evidence.provider_id != 0
                        && evidence.provider_id != subplan.provider_id) {
                        evidence.provider_ambiguous = true;
                    } else {
                        evidence.provider_id = subplan.provider_id;
                        evidence.provider_abi_version =
                            subplan.provider_abi_version;
                    }
                }
            }
        }
        const bool topology = identity_status
            == RelationPhysicalIdentityStatusV1::StorageParent;
        const bool provider_complete = !topology
            && identity_status == RelationPhysicalIdentityStatusV1::Precise
            && evidence.references_relation && evidence.complete_provider
            && !evidence.provider_ambiguous && evidence.provider_id != 0;
        const DependencyVersion version{
            .key = {
                .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
                .class_oid = static_cast<std::uint32_t>(RelationRelationId),
                .object_oid = static_cast<std::uint32_t>(relation_oid),
                /* -1 is adapter-reserved and cannot alias a PG attnum. */
                .object_sub_id = provider_complete ? 0 : -1,
                .kind = topology
                    ? DependencyKind::RelationTopology
                    : DependencyKind::RelationContent,
            },
            .committed_epoch = relation_dependency_epoch(relation_oid),
            .physical_identity = identity_status
                    == RelationPhysicalIdentityStatusV1::Precise
                ? identity
                : PhysicalIdentityV1{},
            .observability = provider_complete
                ? WriteObservability::ProviderComplete
                : WriteObservability::ExecutorUtilityOnly,
        };
        if (!insert_exact_dependency_version(result, version)) {
            return {};
        }
        result.requires_completion_guard =
            result.requires_completion_guard || !provider_complete;
        if (provider_complete) {
            if (result.provider_id != 0
                && result.provider_id != evidence.provider_id) {
                result.requires_completion_guard = true;
            } else {
                result.provider_id = evidence.provider_id;
                result.provider_abi_version =
                    evidence.provider_abi_version;
            }
        }
    }

    foreach(cell, syscache_dependencies) {
        const auto* dependency = static_cast<const PlanInvalItem*>(
            lfirst(cell));
        if (dependency == nullptr || !IsA(dependency, PlanInvalItem)
            || dependency->cacheId < 0
            || dependency->cacheId >= SysCacheSize) {
            return {};
        }
        const DependencyVersion version{
            .key = {
                .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
                .class_oid = static_cast<std::uint32_t>(
                    dependency->cacheId),
                .object_oid = dependency->hashValue,
                .kind = DependencyKind::SyscacheHash,
            },
            .committed_epoch = 1,
            .observability = WriteObservability::ExecutorUtilityOnly,
        };
        if (!insert_exact_dependency_version(result, version)) {
            return {};
        }
    }

    if (result.count != 0 && !result.requires_completion_guard
        && result.provider_id == 0) {
        result.requires_completion_guard = true;
    }

    std::sort(
        result.values.begin(), result.values.begin() + result.count,
        [](const DependencyVersion& left,
           const DependencyVersion& right) noexcept {
            return dependency_key_less_v1(left.key, right.key);
        });
    for (std::uint32_t index = 0; index < result.count; ++index) {
        const DependencyVectorEntryV1 validation{
            .version = result.values[index],
            .record_generation = 1,
        };
        if (!dependency_vector_entry_valid_v1(validation)
            || (index != 0 && !dependency_key_less_v1(
                result.values[index - 1U].key,
                result.values[index].key))) {
            return {};
        }
    }
    result.valid = true;
    return result;
}

/** Frame dependency class/identity and the conservative completion oracle. */
[[nodiscard]] bool append_exact_dependency_key_frame(
        StringInfo key,
        const ExactDependencyVersionsV1& dependencies) {
    constexpr std::uint32_t frame_format = 1;
    const std::uint8_t conservative =
        dependencies.requires_completion_guard ? 1U : 0U;
    if (key == nullptr || !dependencies.valid
        || !append_key_scalar(key, frame_format)
        || !append_key_scalar(key, dependencies.count)) {
        return false;
    }
    for (std::uint32_t index = 0; index < dependencies.count; ++index) {
        const DependencyVersion& version = dependencies.values[index];
        const std::uint16_t kind = static_cast<std::uint16_t>(
            version.key.kind);
        const std::uint32_t observability = static_cast<std::uint32_t>(
            version.observability);
        if (!append_key_scalar(key, version.key.database_oid)
            || !append_key_scalar(key, version.key.class_oid)
            || !append_key_scalar(key, version.key.object_oid)
            || !append_key_scalar(key, version.key.object_sub_id)
            || !append_key_scalar(key, kind)
            || !append_key_scalar(key, observability)) {
            return false;
        }
    }
    return append_key_scalar(key, conservative)
        && append_key_scalar(key, dependencies.provider_id)
        && append_key_scalar(key, dependencies.provider_abi_version)
        && static_cast<std::size_t>(key->len)
            <= static_cast<std::size_t>(kMaxCanonicalKeyBytes);
}

[[nodiscard]] RegistryDecision acquire_registry_entry(
        const StringInfo key,
        const VisibilityTicket& visibility,
        std::span<const std::uint64_t> locally_owned_exact_claim_sequences,
        std::span<const std::uint64_t> locally_owned_reusable_claim_sequences,
        const CustomScan& scan, EState* estate,
        const char*, List* relation_oids,
        List* syscache_dependencies) {
    RegistryDecision decision{};
    if (key == nullptr || key->len <= 0
        || static_cast<std::size_t>(key->len)
            > static_cast<std::size_t>(kMaxCanonicalKeyBytes)
        || !g_reusable_registry || g_reusable_membership_lock == nullptr
        || !visibility_ticket_still_bound(visibility)) {
        return decision;
    }
    relation_oids = relation_oids == nullptr ? NIL : relation_oids;
    syscache_dependencies = syscache_dependencies == nullptr
        ? NIL : syscache_dependencies;
    const int relation_count = list_length(relation_oids);
    const int syscache_count = list_length(syscache_dependencies);
    if (relation_count < 0 || syscache_count < 0) {
        return decision;
    }
    ListCell* cell = nullptr;
    foreach(cell, relation_oids) {
        if (!OidIsValid(lfirst_oid(cell))) {
            return decision;
        }
    }
    foreach(cell, syscache_dependencies) {
        const auto* dependency = static_cast<const PlanInvalItem*>(
            lfirst(cell));
        if (dependency == nullptr || !IsA(dependency, PlanInvalItem)
            || dependency->cacheId < 0
            || dependency->cacheId >= SysCacheSize) {
            return decision;
        }
    }

    const ExactDependencyVersionsV1 dependencies = exact_dependency_versions(
        scan, estate, relation_oids, syscache_dependencies);
    if (!dependencies.valid
        || !append_exact_dependency_key_frame(key, dependencies)
        || !visibility_ticket_still_bound(visibility)) {
        return decision;
    }

    const std::span<const std::byte> key_bytes{
        reinterpret_cast<const std::byte*>(key->data),
        static_cast<std::size_t>(key->len)};
    PostgreSqlColumnChecksumPolicy checksum_policy;
    const auto digest = checksum_policy.sha256(key_bytes);
    if (!digest.has_value()) {
        return decision;
    }
    const ReusableProducerIdentityV1 owner =
        current_reusable_producer_identity();
    const std::uint64_t now_ns = reusable_monotonic_now_ns();
    const bool provider_complete = dependencies.count != 0
        && !dependencies.requires_completion_guard
        && dependencies.provider_id != 0
        && dependencies.provider_abi_version != 0;
    const ReusableClaimRequestV1 request{
        .entry_kind = CacheEntryKind::ExactFinalResult,
        .coverage_directory_key = {
            .security_namespace = current_reusable_security_namespace({}),
        },
        .generation_digest = *digest,
        .canonical_generation_key = key_bytes,
        .metadata = {
            .provider_id = provider_complete
                ? dependencies.provider_id : 0,
            .provider_abi_version = provider_complete
                ? dependencies.provider_abi_version : 0,
            .provider_domain_abi_version = provider_complete
                ? kExactProviderRecipePlanFormatV1 : 0,
            .dependency_count = dependencies.count,
            .dependency_fingerprint = *digest,
            .observability = dependencies.count == 0
                ? WriteObservability::None
                : provider_complete
                    ? WriteObservability::ProviderComplete
                    : WriteObservability::DatabaseWideFallback,
            .producer_ttl_ms = static_cast<std::uint64_t>(
                std::max(guc::ttl_ms(), 0)),
        },
        .producer = owner,
        .reference_owner = owner,
        .started_monotonic_ns = now_ns,
        .consumer_ttl_ms = static_cast<std::uint64_t>(
            std::max(guc::ttl_ms(), 0)),
        .pin_complete = true,
        .register_waiter = true,
    };
    const DependencyKey conservative_guard_key{
        .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
        .object_oid = static_cast<std::uint32_t>(MyDatabaseId),
        .kind = dependencies.requires_completion_guard
            ? DependencyKind::DatabaseWriteEpoch
            : DependencyKind::DatabaseDefinitionEpoch,
    };
    ReusableClaimResultV1 claim =
        claim_reusable_generation_with_dependencies(
            request,
            std::span<const DependencyVersion>{
                dependencies.values.data(), dependencies.count},
            visibility.completion_count,
            &conservative_guard_key);
    if (!claim || !claim.reference) {
        return decision;
    }
    if (claim.disposition == ReusableClaimDispositionV1::ClaimedProducer) {
        decision.role = RuntimeRole::Producer;
        decision.entry_id = claim.reference.id;
        decision.reference = claim.reference;
        return decision;
    }

    ReusableGenerationSnapshotV1 snapshot{};
    if (!snapshot_current_reusable_generation(claim.reference, &snapshot)
        || snapshot.reference != claim.reference
        || snapshot.entry_kind != CacheEntryKind::ExactFinalResult) {
        release_runtime_reusable_reference(claim.token);
        return decision;
    }
    if (claim.disposition == ReusableClaimDispositionV1::ExistingComplete) {
        const VisibilityTicket refreshed = mint_visibility_ticket(
            snapshot.metadata.observability,
            VisibilityTicketPurposeV1::Continuation);
        if (claim.token.active
            && claim.token.kind == ReusableReferenceKindV1::ReaderPin
            && snapshot.state == CacheEntryState::Complete
            && snapshot.manifest
            && exact_reusable_ttl_decision(snapshot, false, false).readable
            && refreshed.valid
            && validate_reusable_generation_dependencies_locked(
                snapshot, refreshed.completion_count)
            && visibility_ticket_still_bound(visibility)) {
            decision.role = RuntimeRole::Consumer;
            decision.entry_id = claim.reference.id;
            decision.reference = claim.reference;
            decision.reference_token = claim.token;
            decision.completed_snapshot = snapshot;
            decision.manifest_handle = storage_manifest_handle(
                snapshot.manifest);
            claim.token.active = false;
        }
        release_runtime_reusable_reference(claim.token);
        return decision;
    }
    if (claim.disposition != ReusableClaimDispositionV1::ExistingInFlight
        || !claim.token.active
        || claim.token.kind != ReusableReferenceKindV1::Waiter
        || snapshot.state != CacheEntryState::Claimed
            && snapshot.state != CacheEntryState::Building
        || !snapshot.producer.valid()) {
        release_runtime_reusable_reference(claim.token);
        return decision;
    }
    if (g_shared != nullptr) {
        pg_atomic_fetch_add_u64(&g_shared->stats.inflight_waits, 1);
    }
    return wait_for_unified_exact_producer(
        claim.reference,
        claim.token,
        snapshot.producer,
        visibility,
        locally_owned_exact_claim_sequences,
        locally_owned_reusable_claim_sequences);
}

/** Release a short reader pin after full preflight/materialization. */
void release_registry_reader(
        int index, const CacheEntryId& expected,
        const RegistryReaderClaim& reader_claim,
        bool successful,
        bool invalidate_failure = true,
        bool count_outcome = true) noexcept {
    if (g_shared == nullptr || index < 0
        || index >= static_cast<int>(kRegistryCapacity)
        || expected.boot_nonce == 0 || expected.generation == 0
        || expected.claim_sequence == 0 || !reader_claim) {
        return;
    }
    storage::ManifestHandle retired_manifest{};
    bool released = false;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[index];
    if (cache_entry_id(entry) == expected) {
        RegistryReaderOwner& reader = entry.readers[reader_claim.slot];
        if (reader.active
            && reader.registration_sequence
                == reader_claim.registration_sequence
            && registry_owner_identity(reader)
                == current_reusable_producer_identity()) {
            clear_registry_reader_locked(entry, reader);
            released = true;
        }
        if (released && entry.reader_count == 0
            && entry.state == SharedEntryState::Retired
            && entry.manifest_handle) {
            retired_manifest = entry.manifest_handle;
            entry.manifest_handle = {};
        }
        if (released && !successful && invalidate_failure
            && entry.state == SharedEntryState::Complete) {
            entry.state = SharedEntryState::Retired;
            if (entry.reader_count == 0 && entry.manifest_handle) {
                retired_manifest = entry.manifest_handle;
                entry.manifest_handle = {};
            }
            ConditionVariableBroadcast(&entry.changed);
        }
    }
    LWLockRelease(g_registry_lock);
    if (retired_manifest) {
        static_cast<void>(storage::retire_manifest(retired_manifest));
    }
    if (!released || !count_outcome) {
        return;
    }
    if (successful) {
        pg_atomic_fetch_add_u64(&g_shared->stats.completed_hits, 1);
    } else {
        pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
    }
}

/** Invalidate one exact generation after a failed manifest preflight. */
void invalidate_unified_exact_generation(
        const ReusableGenerationReferenceV1& reference) noexcept {
    if (!reference || !g_reusable_registry
        || g_reusable_membership_lock == nullptr) {
        return;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool invalidated = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        invalidated = invalidate_reusable_generation(
            g_reusable_registry,
            reference,
            CacheFailureCode::CorruptResult,
            reusable_partition_callbacks())
            == ReusableRegistryErrorV1::None;
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
    }
    PG_END_TRY();
    if (invalidated) {
        broadcast_reusable_generation(reference);
    }
}

/** Release one unified ReaderPin and account only a proven token release. */
void release_unified_exact_reader(
        ReusableReferenceTokenV1& token,
        bool successful,
        bool invalidate_failure = true,
        bool count_outcome = true) noexcept {
    if (!token.active || token.kind != ReusableReferenceKindV1::ReaderPin) {
        token = {};
        return;
    }
    const ReusableGenerationReferenceV1 reference = token.reference;
    const ReusableRegistryErrorV1 released = release_reusable_reference(
        g_reusable_registry, token, reusable_partition_callbacks());
    if (released != ReusableRegistryErrorV1::None) {
        return;
    }
    if (!successful && invalidate_failure) {
        invalidate_unified_exact_generation(reference);
    }
    if (!count_outcome || g_shared == nullptr) {
        return;
    }
    pg_atomic_fetch_add_u64(
        successful
            ? &g_shared->stats.completed_hits
            : &g_shared->stats.fallbacks,
        1);
}

void discard_unpublished_tokens(BioseqCacheState& state) noexcept {
    for (std::uint32_t index = 0;
         index < state.stored_token_count; ++index) {
        if (state.stored_tokens[index]) {
            static_cast<void>(storage::discard_unpublished_block(
                state.stored_tokens[index]));
            state.stored_tokens[index] = {};
        }
    }
    state.stored_token_count = 0;
}

void release_storage_reader_claims(BioseqCacheState& state) noexcept {
    if (state.manifest_pin_owned) {
        static_cast<void>(storage::unpin_manifest(state.manifest_pin));
        state.manifest_pin = {};
        state.manifest_pin_owned = false;
    }
    if (state.manifest_waiter_owned) {
        static_cast<void>(storage::unregister_manifest_waiter(
            state.manifest_waiter_token));
        state.manifest_waiter_token = {};
        state.manifest_waiter_owned = false;
    }
}

/** Retire every execution lease only after all exact callbacks have ended. */
void release_exact_provider_leases(BioseqCacheState& state) noexcept {
    for (std::uint32_t index = 0;
         index < state.provider_lease_count;
         ++index) {
        provider_registry::release_provider_lease_v2(
            &state.provider_leases[index]);
    }
    state.provider_leases = {};
    state.provider_lease_count = 0;
}

/** Fail a generation only when this backend still owns the producer token. */
void fail_registry_producer(BioseqCacheState& state) noexcept {
    storage::ManifestHandle retired_manifest{};
    if (state.registry_reference) {
        fail_current_reusable_generation(
            state.registry_reference, CacheFailureCode::ProducerError);
    } else if (g_shared != nullptr && state.registry_index >= 0) {
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        CacheEntry& entry = g_shared->entries[state.registry_index];
        if (cache_entry_id(entry) == state.entry_id
            && exact_entry_in_flight(entry.state)
            && exact_producer_identity(entry)
                == current_reusable_producer_identity()) {
            retired_manifest = entry.manifest_handle;
            entry.manifest_handle = {};
            fail_abandoned_producer(entry);
        }
        LWLockRelease(g_registry_lock);
    }
    if (state.registry_reference_owned) {
        release_runtime_reusable_reference(
            state.registry_reference_token);
        state.registry_reference_owned = false;
    }
    if (retired_manifest) {
        static_cast<void>(storage::retire_manifest(retired_manifest));
    }
    if (state.manifest_handle && !state.producer_completed) {
        static_cast<void>(storage::retire_manifest(state.manifest_handle));
        state.manifest_handle = {};
    }
    release_storage_reader_claims(state);
    discard_unpublished_tokens(state);
    destroy_column_builder(state);
    state.capture_failed = true;
    state.runtime_role = RuntimeRole::Bypass;
}

/** PostgreSQL memory-context cleanup covers ERROR paths that skip EndCustomScan. */
void cache_state_cleanup(void* argument) noexcept {
    auto* state = static_cast<BioseqCacheState*>(argument);
    if (state == nullptr) {
        return;
    }
    unlink_backend_query_cleanup(state->backend_cleanup);
    if (state->cleanup_done) {
        return;
    }
    state->cleanup_done = true;
    release_storage_reader_claims(*state);
    if (state->registry_reference_owned) {
        release_unified_exact_reader(
            state->registry_reference_token, false);
        state->registry_reference_owned = false;
    }
    if (state->registry_reader_owned) {
        release_registry_reader(
            state->registry_index,
            state->entry_id,
            state->registry_reader_claim,
            false);
        state->registry_reader_owned = false;
        state->registry_reader_claim = {};
    }
    if (state->runtime_role == RuntimeRole::Producer
        && !state->producer_completed) {
        fail_registry_producer(*state);
    }
    discard_unpublished_tokens(*state);
    if (state->replay_store != nullptr) {
        tuplestore_end(state->replay_store);
        state->replay_store = nullptr;
    }
    if (state->replay_slot != nullptr) {
        ExecDropSingleTupleTableSlot(state->replay_slot);
        state->replay_slot = nullptr;
    }
    destroy_column_builder(*state);
    release_exact_provider_leases(*state);
}

/** Prepare one generation-private self-validating column-store capture. */
[[nodiscard]] bool begin_producer_capture(
        BioseqCacheState& state, TupleDesc descriptor) {
    try {
        state.send_functions = static_cast<Oid*>(
            palloc0(sizeof(Oid) * static_cast<std::size_t>(descriptor->natts)));
        std::vector<ColumnSchema> schema = build_column_schema(
            descriptor, state.send_functions);
        state.column_checksum_policy =
            executor::try_construct_context_owned<
                PostgreSqlColumnChecksumPolicy>(CurrentMemoryContext);
        state.column_sink =
            executor::try_construct_context_owned<
                storage::StorageColumnStoreBlockSink>(CurrentMemoryContext);
        if (state.column_checksum_policy == nullptr
            || state.column_sink == nullptr) {
            destroy_column_builder(state);
            return false;
        }
        state.column_builder =
            executor::try_construct_context_owned<
                PostgreSqlStreamingColumnStoreBuilder>(
            CurrentMemoryContext,
            current_column_store_identity(),
            schema,
            *state.column_checksum_policy,
            *state.column_sink);
        return state.column_builder != nullptr;
    } catch (...) {
        destroy_column_builder(state);
        return false;
    }
}

/** Capture one raw child row through PostgreSQL's lossless binary send ABI. */
[[nodiscard]] bool capture_raw_child_row(
        BioseqCacheState& state, TupleTableSlot* slot) {
    if (state.column_builder == nullptr || state.send_functions == nullptr) {
        return false;
    }
    slot_getallattrs(slot);
    TupleDesc descriptor = slot->tts_tupleDescriptor;
    auto* values = static_cast<EncodedColumnValue*>(palloc0(
        sizeof(EncodedColumnValue)
        * static_cast<std::size_t>(descriptor->natts)));
    auto** encoded = static_cast<bytea**>(palloc0(
        sizeof(bytea*) * static_cast<std::size_t>(descriptor->natts)));
    bool success = false;
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        if (slot->tts_isnull[attribute]) {
            values[attribute].is_null = true;
            continue;
        }
        encoded[attribute] = OidSendFunctionCall(
            state.send_functions[attribute], slot->tts_values[attribute]);
        const std::size_t byte_count = VARSIZE_ANY_EXHDR(encoded[attribute]);
        values[attribute].bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(
                VARDATA_ANY(encoded[attribute])),
            byte_count,
        };
        state.logical_bytes += byte_count;
    }
    if (state.logical_bytes <= kMaximumStoredResultBytes) {
        try {
            const auto appended = state.column_builder->append_row(
                std::span<const EncodedColumnValue>{
                    values, static_cast<std::size_t>(descriptor->natts)});
            success = appended.has_value();
        } catch (...) {
            success = false;
        }
    }
    for (int attribute = 0; attribute < descriptor->natts; ++attribute) {
        if (encoded[attribute] != nullptr) {
            pfree(encoded[attribute]);
        }
    }
    pfree(encoded);
    pfree(values);
    if (success) {
        ++state.row_count;
    }
    return success;
}

/** Seal storage first and make unified-registry COMPLETE visible last. */
[[nodiscard]] bool complete_registry_producer(
        BioseqCacheState& state, TupleDesc descriptor) {
    if (descriptor == nullptr || !state.registry_reference
        || state.column_builder == nullptr
        || state.column_builder->row_count() != state.row_count
        || state.producer_started_at == 0
        || state.visibility_database_epoch == 0
        || state.visibility_completion_count == 0) {
        return false;
    }
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_shared != nullptr
        && pg_atomic_exchange_u32(
            &g_shared->test_control.fail_next_write, 0) != 0) {
        return false;
    }
#endif
    std::optional<PostgreSqlStreamingColumnStoreBuilder::result_type> blocks;
    try {
        auto result = state.column_builder->finalize();
        if (!result.has_value()) {
            return false;
        }
        blocks.emplace(std::move(*result));
    } catch (...) {
        return false;
    }
    state.payload_bytes = blocks->logical_bytes();
    if (state.payload_bytes == 0
        || state.payload_bytes > kMaximumStoredResultBytes
        || blocks->row_count() != state.row_count) {
        return false;
    }
    maybe_pause(TestPausePoint::BeforeTerminalPublish);

    ReusableGenerationSnapshotV1 storage_snapshot{};
    if (!snapshot_current_reusable_generation(
            state.registry_reference, &storage_snapshot)
        || storage_snapshot.reference != state.registry_reference
        || storage_snapshot.entry_kind != CacheEntryKind::ExactFinalResult
        || storage_snapshot.state != CacheEntryState::Building
        || storage_snapshot.producer
            != current_reusable_producer_identity()) {
        return false;
    }
    storage::ManifestDescriptor manifest{
        .id = state.registry_reference.id,
        .kind = CacheEntryKind::ExactFinalResult,
        .logical_bytes = state.logical_bytes,
        .row_count = state.row_count,
        .created_at_us = state.producer_started_at,
        /* Registry retirement owns expiry; zero also preserves every waiter
         * entitlement acquired before terminal publication. */
        .expires_at_us = 0,
    };
    storage::PublishManifestResult publication =
        storage::publish_streamed_column_store(
            manifest, *state.column_sink, *blocks);
    if (!publication) {
        return false;
    }
    destroy_column_builder(state);
    const VisibilityTicket refreshed = mint_visibility_ticket(
        storage_snapshot.metadata.observability,
        VisibilityTicketPurposeV1::Continuation);
    const std::uint64_t continuing_completion_count =
        storage_snapshot.conservative_guard.snapshot_completion_count;
    if (continuing_completion_count != 0
        && continuing_completion_count
            != state.visibility_completion_count) {
        static_cast<void>(storage::retire_manifest(publication.handle));
        mark_shared_cache_failed();
        return false;
    }
    RuntimeDependencyPublishContextV1 context{
        .request = {
            .reference = state.registry_reference,
            .manifest = reusable_manifest_handle(publication.handle),
            .completed_monotonic_ns = reusable_monotonic_now_ns(),
            .row_count = state.row_count,
            .logical_bytes = state.logical_bytes,
            .stored_bytes = state.payload_bytes,
        },
        .expected_kind = CacheEntryKind::ExactFinalResult,
        .expected_producer = current_reusable_producer_identity(),
        .expected_dependency_count =
            storage_snapshot.metadata.dependency_count,
    };
    const dependency_runtime::DependencyRuntimeResultV1 validated =
        refreshed.valid
            && (storage_snapshot.metadata.observability
                    != WriteObservability::DatabaseWideFallback
                || refreshed.database_epoch
                    == state.visibility_database_epoch)
        ? validate_reusable_generation_dependencies_locked(
            storage_snapshot,
            refreshed.completion_count,
            dependency_runtime::DependencyTicketLockedActionV1{
                .context = &context,
                .apply = apply_runtime_dependency_publish,
                .rollback = rollback_runtime_dependency_publish,
            },
            database_guard_requires_completion_v1(
                    storage_snapshot.conservative_guard.dependency)
                ? refreshed.completion_count
                : 0)
        : dependency_runtime::DependencyRuntimeResultV1{
            .error = dependency_runtime::DependencyRuntimeErrorV1::
                ReferenceValidationFailed,
            .fail_open = true,
        };
    if (context.action_applied) {
        broadcast_reusable_generation(state.registry_reference);
    }
    if (!validated || context.rolled_back
        || context.registry_result != ReusableRegistryErrorV1::None) {
        if (!context.action_applied) {
            static_cast<void>(storage::retire_manifest(publication.handle));
        }
        return false;
    }
    state.manifest_handle = publication.handle;
    state.producer_completed = true;
    if (g_shared != nullptr) {
        pg_atomic_fetch_add_u64(&g_shared->stats.produced, 1);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.logical_bytes, state.logical_bytes);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.stored_bytes, state.payload_bytes);
    }
    return true;
}

enum class CompleteResultLoadFailureV1 : std::uint8_t {
    None = 0,
    Schema,
    InvalidPin,
    InvalidSize,
    BlockRead,
    Validation,
    DescriptorMismatch,
    Decode,
};

[[nodiscard]] const char* complete_result_load_failure_name(
        CompleteResultLoadFailureV1 failure) noexcept {
    switch (failure) {
        case CompleteResultLoadFailureV1::None: return "none";
        case CompleteResultLoadFailureV1::Schema: return "schema";
        case CompleteResultLoadFailureV1::InvalidPin: return "invalid-pin";
        case CompleteResultLoadFailureV1::InvalidSize: return "invalid-size";
        case CompleteResultLoadFailureV1::BlockRead: return "block-read";
        case CompleteResultLoadFailureV1::Validation: return "validation";
        case CompleteResultLoadFailureV1::DescriptorMismatch:
            return "descriptor-mismatch";
        case CompleteResultLoadFailureV1::Decode: return "decode";
    }
    return "unknown";
}

/** Materialize and validate an entire shared result before exposing row one. */
[[nodiscard]] Tuplestorestate* load_complete_result(
        const storage::ManifestPin& pin, TupleDesc expected_descriptor,
        CacheEntryKind expected_kind = CacheEntryKind::ExactFinalResult,
        CompleteResultLoadFailureV1* failure = nullptr) {
    const auto reject = [failure](CompleteResultLoadFailureV1 reason) {
        if (failure != nullptr) {
            *failure = reason;
        }
        return static_cast<Tuplestorestate*>(nullptr);
    };
    if (failure != nullptr) {
        *failure = CompleteResultLoadFailureV1::None;
    }
    std::vector<ColumnSchema> expected_schema;
    try {
        expected_schema = build_column_schema(expected_descriptor, nullptr);
    } catch (...) {
        return reject(CompleteResultLoadFailureV1::Schema);
    }
    if (!pin || pin.kind != expected_kind
        || pin.block_count > storage::kBlocksPerManifest) {
        return reject(CompleteResultLoadFailureV1::InvalidPin);
    }
    std::uint64_t total_bytes = 0;
    for (std::uint32_t index = 0; index < pin.block_count; ++index) {
        const std::uint64_t length = pin.blocks[index].length;
        if (length == 0 || length > kMaximumStoredResultBytes
            || total_bytes > kMaximumStoredResultBytes - length) {
            return reject(CompleteResultLoadFailureV1::InvalidSize);
        }
        total_bytes += length;
    }
    if (total_bytes == 0 || total_bytes > kMaximumStoredResultBytes) {
        return reject(CompleteResultLoadFailureV1::InvalidSize);
    }
    const std::size_t store_size = static_cast<std::size_t>(total_bytes);
    auto* store = static_cast<std::byte*>(palloc(store_size));
    std::size_t offset = 0;
    for (std::uint32_t index = 0; index < pin.block_count; ++index) {
        const std::size_t length = static_cast<std::size_t>(
            pin.blocks[index].length);
        if (storage::read_pinned_block(
                pin, index,
                std::span<std::byte>{store + offset, length})
                != storage::StorageError::None) {
            pfree(store);
            return reject(CompleteResultLoadFailureV1::BlockRead);
        }
        offset += length;
    }

    ColumnStoreView* view = nullptr;
    try {
        PostgreSqlColumnChecksumPolicy policy;
        auto validated = validate_column_store(
            std::span<const std::byte>{store, store_size},
            current_column_store_identity(),
            expected_schema,
            policy);
        if (!validated.has_value()) {
            pfree(store);
            return reject(CompleteResultLoadFailureV1::Validation);
        }
        view = executor::try_construct_context_owned<ColumnStoreView>(
            CurrentMemoryContext, std::move(*validated));
        if (view == nullptr) {
            pfree(store);
            return reject(CompleteResultLoadFailureV1::Validation);
        }
    } catch (...) {
        pfree(store);
        return reject(CompleteResultLoadFailureV1::Validation);
    }
    if (view->column_count()
            != static_cast<std::size_t>(expected_descriptor->natts)
        || view->row_count() != pin.row_count) {
        executor::destroy_context_owned(view);
        pfree(store);
        return reject(CompleteResultLoadFailureV1::DescriptorMismatch);
    }
    Tuplestorestate* result = tuplestore_begin_heap(true, false, work_mem);
    MemoryContext row_context = AllocSetContextCreate(
        CurrentMemoryContext, "bioseq_cache replay row",
        ALLOCSET_DEFAULT_SIZES);
    Datum* values = static_cast<Datum*>(
        palloc0(sizeof(Datum)
            * static_cast<std::size_t>(expected_descriptor->natts)));
    bool* nulls = static_cast<bool*>(
        palloc0(sizeof(bool)
            * static_cast<std::size_t>(expected_descriptor->natts)));
    bool valid = true;
    MemoryContext caller_context = CurrentMemoryContext;
    PG_TRY();
    {
        for (std::uint64_t row = 0; row < view->row_count() && valid; ++row) {
            MemoryContextReset(row_context);
            MemoryContext old_context = MemoryContextSwitchTo(row_context);
            for (int attribute = 0;
                 attribute < expected_descriptor->natts; ++attribute) {
                auto cell = view->value(
                    row, static_cast<std::size_t>(attribute));
                if (!cell.has_value()) {
                    valid = false;
                    break;
                }
                if (cell->is_null) {
                    values[attribute] = static_cast<Datum>(0);
                    nulls[attribute] = true;
                    continue;
                }
                if (cell->bytes.size()
                    > static_cast<std::size_t>(
                        (std::numeric_limits<int>::max)())) {
                    valid = false;
                    break;
                }
                const ColumnDescriptorV1& codec =
                    expected_schema[static_cast<std::size_t>(attribute)]
                        .descriptor;
                StringInfoData input{
                    .data = const_cast<char*>(reinterpret_cast<const char*>(
                        cell->bytes.data())),
                    .len = static_cast<int>(cell->bytes.size()),
                    .maxlen = static_cast<int>(cell->bytes.size()),
                    .cursor = 0,
                };
                values[attribute] = OidReceiveFunctionCall(
                    codec.receive_function_oid,
                    &input,
                    codec.receive_type_io_param,
                    codec.typmod);
                nulls[attribute] = false;
                if (input.cursor != input.len) {
                    valid = false;
                    break;
                }
            }
            MemoryContextSwitchTo(old_context);
            if (valid) {
                tuplestore_putvalues(
                    result, expected_descriptor, values, nulls);
            }
        }
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            MemoryContextDelete(row_context);
            pfree(values);
            pfree(nulls);
            executor::destroy_context_owned(view);
            pfree(store);
            tuplestore_end(result);
            PG_RE_THROW();
        }
        FlushErrorState();
        valid = false;
    }
    PG_END_TRY();
    MemoryContextDelete(row_context);
    pfree(values);
    pfree(nulls);
    executor::destroy_context_owned(view);
    pfree(store);
    if (!valid) {
        tuplestore_end(result);
        return reject(CompleteResultLoadFailureV1::Decode);
    }
    tuplestore_rescan(result);
    return result;
}

/** Initialize the retained child exactly once, releasing claims on error. */
void initialize_retained_child(
        BioseqCacheState& state, CustomScanState* node,
        EState* estate, int eflags) {
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    if (scan->custom_plans == NIL || list_length(scan->custom_plans) != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqCacheScan requires exactly one retained child")));
    }
    Plan* child_plan = static_cast<Plan*>(linitial(scan->custom_plans));
    if (node->custom_ps != NIL) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqCacheScan child was initialized twice")));
    }
    node->custom_ps = list_make1(ExecInitNode(child_plan, estate, eflags));
    state.child_initialized = true;
}

[[nodiscard]] PlanState* retained_child(CustomScanState* node) noexcept {
    return node->custom_ps == NIL
        ? nullptr
        : static_cast<PlanState*>(linitial(node->custom_ps));
}

/** The CustomScan scan slot implements custom_scan_tlist, including resjunk. */
[[nodiscard]] TupleDesc exact_raw_tuple_descriptor(
        CustomScanState* node) noexcept {
    return node != nullptr && node->ss.ss_ScanTupleSlot != nullptr
        ? node->ss.ss_ScanTupleSlot->tts_tupleDescriptor
        : nullptr;
}

/** Reproject a raw cached/child slot to the visible CustomScan descriptor. */
[[nodiscard]] TupleTableSlot* project_exact_raw_slot(
        CustomScanState* node, TupleTableSlot* raw_slot) {
    if (node == nullptr || raw_slot == nullptr || TupIsNull(raw_slot)) {
        return node == nullptr
            ? raw_slot
            : ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    ExprContext* expression_context = node->ss.ps.ps_ExprContext;
    expression_context->ecxt_scantuple = raw_slot;
    return node->ss.ps.ps_ProjInfo == nullptr
        ? raw_slot
        : ExecProject(node->ss.ps.ps_ProjInfo);
}

/** Decide producer/consumer/bypass before touching the retained child. */
void cache_begin_custom_scan(CustomScanState* node, EState* estate, int eflags) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    bool fallback_outcome_counted = false;
    state->runtime_role = RuntimeRole::Bypass;
    state->registry_index = -1;
    state->visibility_database_epoch = 0;
    state->visibility_completion_count = 0;
    state->replay_store = nullptr;
    state->replay_slot = nullptr;
    state->column_checksum_policy = nullptr;
    state->column_sink = nullptr;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->capture_failed = false;
    state->producer_completed = false;
    state->manifest_pin_owned = false;
    state->manifest_waiter_owned = false;
    state->registry_reader_owned = false;
    state->registry_reference_owned = false;
    state->registered_inflight_waiter = false;
    state->ttl_entitlement_consumed = false;
    state->runtime_recipe_valid = false;
    state->planned_diagnostics_published = false;
#if defined(BIOSEQ_CACHE_TESTING)
    state->child_execution_counted = false;
#endif
    state->registry_reader_claim = {};
    state->entry_id = {};
    state->registry_reference = {};
    state->registry_reference_token = {};
    state->completed_snapshot = {};
    state->manifest_handle = {};
    state->manifest_waiter_token = {};
    state->manifest_pin = {};
    state->stored_tokens = {};
    state->stored_token_count = 0;
    state->provider_leases = {};
    state->provider_lease_count = 0;
    state->producer_started_at = 0;
    node->custom_ps = NIL;
    state->cleanup_done = false;
    if (!state->cleanup_registered) {
        state->cleanup_callback.func = cache_state_cleanup;
        state->cleanup_callback.arg = state;
        state->cleanup_callback.next = nullptr;
        MemoryContextRegisterResetCallback(
            estate->es_query_cxt, &state->cleanup_callback);
        state->cleanup_registered = true;
    }
    link_backend_query_cleanup(
        state->backend_cleanup, state, cache_state_cleanup);

    if (g_producer_plan_capture_active) {
        initialize_retained_child(*state, node, estate, eflags);
        return;
    }

    const bool explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
    const bool force_bypass = private_force_bypass(*scan);
    const int runtime_recipe_count = exact_runtime_recipe_count(*scan);
    const VisibilityTicket visibility = explain_only || force_bypass
        ? VisibilityTicket{}
        : mint_visibility_ticket(WriteObservability::DatabaseWideFallback);
    const bool cache_allowed = !force_bypass && visibility.valid
        && guc::mode() != QueryCacheMode::Disabled
        && XactIsoLevel == XACT_READ_COMMITTED
        && !transaction_is_dirty()
        && node->ss.ps.ps_ResultTupleDesc != nullptr;
    if (cache_allowed) {
        StringInfo key = nullptr;
        TupleDesc raw_descriptor = exact_raw_tuple_descriptor(node);
        MemoryContext caller_context = CurrentMemoryContext;
        PG_TRY();
        {
            validate_binary_schema(node->ss.ps.ps_ResultTupleDesc);
            if (raw_descriptor == nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqCacheScan raw child schema is unavailable")));
            }
            validate_binary_schema(raw_descriptor);
            key = build_runtime_key(
                *scan, estate, node->ss.ps.ps_ResultTupleDesc);
            state->runtime_recipe_valid = key != nullptr
                && append_exact_runtime_recipe_key(
                    *scan, estate, key, *state);
            if (key != nullptr && !state->runtime_recipe_valid) {
                pfree(key->data);
                pfree(key);
                key = nullptr;
            }
        }
        PG_CATCH();
        {
            const int sqlstate = geterrcode();
            MemoryContextSwitchTo(caller_context);
            if (cache_interrupt_error(sqlstate)) {
                PG_RE_THROW();
            }
            FlushErrorState();
            key = nullptr;
        }
        PG_END_TRY();

        if (key != nullptr
            && !visibility_ticket_still_bound(visibility)) {
            pfree(key->data);
            pfree(key);
            key = nullptr;
        }

        if (key != nullptr) {
            List* referenced_relations = scan->custom_private != NIL
                    && list_length(scan->custom_private) > 4
                ? static_cast<List*>(list_nth(scan->custom_private, 4))
                : NIL;
            List* referenced_syscache_dependencies =
                scan->custom_private != NIL
                    && list_length(scan->custom_private) > 5
                ? static_cast<List*>(list_nth(scan->custom_private, 5))
                : NIL;
            RegistryDecision decision{};
            const std::size_t owned_capacity = kRegistryCapacity
                + static_cast<std::size_t>(g_reusable_wait_slot_count);
            const bool owned_capacity_valid = owned_capacity
                    >= kRegistryCapacity
                && owned_capacity
                    <= static_cast<std::size_t>(MaxAllocSize)
                        / sizeof(std::uint64_t);
            auto* owned_claims = owned_capacity_valid
                ? static_cast<std::uint64_t*>(palloc_extended(
                    sizeof(std::uint64_t) * owned_capacity,
                    MCXT_ALLOC_NO_OOM))
                : nullptr;
            std::size_t owned_exact_count = 0;
            std::size_t owned_count = 0;
            if (owned_claims != nullptr
                && collect_backend_owned_claim_sequences(
                    std::span<std::uint64_t>{
                        owned_claims, owned_capacity},
                    &owned_exact_count,
                    &owned_count)) {
                decision = acquire_registry_entry(
                    key,
                    visibility,
                    std::span<const std::uint64_t>{
                        owned_claims, owned_exact_count},
                    std::span<const std::uint64_t>{
                        owned_claims + owned_exact_count,
                        owned_count - owned_exact_count},
                    *scan,
                    estate,
                    private_string(*scan, 1),
                    referenced_relations,
                    referenced_syscache_dependencies);
            }
            if (owned_claims != nullptr) {
                pfree(owned_claims);
            }
            state->runtime_role = decision.role;
            state->registry_index = decision.index;
            state->generation = decision.generation;
            state->registry_reader_claim = decision.reader_claim;
            state->entry_id = decision.entry_id;
            state->registry_reference = decision.reference;
            state->registry_reference_token = decision.reference_token;
            state->completed_snapshot = decision.completed_snapshot;
            state->manifest_handle = decision.manifest_handle;
            state->manifest_waiter_token =
                decision.manifest_waiter_token;
            state->manifest_waiter_owned =
                static_cast<bool>(decision.manifest_waiter_token);
            state->registry_reader_owned =
                static_cast<bool>(decision.reader_claim);
            state->registry_reference_owned =
                decision.reference_token.active;
            state->registered_inflight_waiter =
                decision.registered_inflight_waiter;
            state->ttl_entitlement_consumed =
                decision.ttl_entitlement_consumed;
            if (decision.role == RuntimeRole::Producer) {
                state->producer_started_at = GetCurrentTimestamp();
            }
            state->visibility_completion_count = visibility.completion_count;
            state->visibility_database_epoch = visibility.database_epoch;
        }
        if (key != nullptr) {
            pfree(key->data);
            pfree(key);
        }

        if (state->runtime_role == RuntimeRole::Consumer
            && !visibility_ticket_still_bound(visibility)) {
            if (state->registry_reference_owned) {
                release_unified_exact_reader(
                    state->registry_reference_token,
                    false,
                    false,
                    false);
                state->registry_reference_owned = false;
            }
            if (state->registry_reader_owned) {
                release_registry_reader(
                    state->registry_index,
                    state->entry_id,
                    state->registry_reader_claim,
                    false,
                    false,
                    false);
                state->registry_reader_owned = false;
                state->registry_reader_claim = {};
            }
            if (state->manifest_waiter_owned) {
                static_cast<void>(storage::unregister_manifest_waiter(
                    state->manifest_waiter_token));
                state->manifest_waiter_owned = false;
                state->manifest_waiter_token = {};
            }
            state->runtime_role = RuntimeRole::Bypass;
        }

        if (state->runtime_role == RuntimeRole::Consumer) {
            MemoryContext load_context = CurrentMemoryContext;
            storage::StorageError pin_error = storage::StorageError::None;
            CompleteResultLoadFailureV1 load_failure =
                CompleteResultLoadFailureV1::None;
            PG_TRY();
            {
                storage::PinManifestResult pinned =
                    state->manifest_waiter_owned
                    ? storage::convert_waiter_to_pin(
                        state->manifest_waiter_token)
                    : storage::pin_manifest(state->entry_id);
                pin_error = pinned.error;
                if (pinned) {
                    state->manifest_waiter_owned = false;
                    state->manifest_waiter_token = {};
                    state->manifest_pin = pinned.pin;
                    state->manifest_pin_owned = true;
                    maybe_pause(TestPausePoint::AfterReaderPinned);
                    state->replay_store = load_complete_result(
                        state->manifest_pin,
                        raw_descriptor,
                        CacheEntryKind::ExactFinalResult,
                        &load_failure);
                }
                if (state->replay_store != nullptr) {
                    state->replay_slot = MakeSingleTupleTableSlot(
                        raw_descriptor,
                        &TTSOpsMinimalTuple);
                }
            }
            PG_CATCH();
            {
                const int sqlstate = geterrcode();
                MemoryContextSwitchTo(load_context);
                if (state->replay_store != nullptr) {
                    tuplestore_end(state->replay_store);
                }
                if (state->replay_slot != nullptr) {
                    ExecDropSingleTupleTableSlot(state->replay_slot);
                }
                state->replay_store = nullptr;
                state->replay_slot = nullptr;
                if (cache_interrupt_error(sqlstate)) {
                    release_storage_reader_claims(*state);
                    if (state->registry_reference_owned) {
                        release_unified_exact_reader(
                            state->registry_reference_token,
                            false);
                        state->registry_reference_owned = false;
                    }
                    if (state->registry_reader_owned) {
                        release_registry_reader(
                            state->registry_index,
                            state->entry_id,
                            state->registry_reader_claim,
                            false,
                            false,
                            false);
                        state->registry_reader_owned = false;
                        state->registry_reader_claim = {};
                    }
                    PG_RE_THROW();
                }
                FlushErrorState();
            }
            PG_END_TRY();
            const bool loaded = state->replay_store != nullptr
                && state->replay_slot != nullptr;
            release_storage_reader_claims(*state);
            if (state->registry_reference_owned) {
                release_unified_exact_reader(
                    state->registry_reference_token, loaded);
                state->registry_reference_owned = false;
            }
            release_registry_reader(
                state->registry_index,
                state->entry_id,
                state->registry_reader_claim,
                loaded);
            fallback_outcome_counted = !loaded;
            state->registry_reader_owned = false;
            state->registry_reader_claim = {};
            if (loaded) {
                return;
            }
            ereport(LOG,
                (errmsg("bioseq_cache exact replay fell back"),
                 errdetail("manifest pin error %u, result load failure %s",
                     static_cast<unsigned>(pin_error),
                     complete_result_load_failure_name(load_failure))));
            state->runtime_role = RuntimeRole::Bypass;
        }

        if (state->runtime_role == RuntimeRole::Producer
            && !register_exact_executor_start_claim(*state)) {
            fail_registry_producer(*state);
        }
    }

    PG_TRY();
    {
        initialize_retained_child(*state, node, estate, eflags);
    }
    PG_CATCH();
    {
        if (state->runtime_role == RuntimeRole::Producer) {
            fail_registry_producer(*state);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();
    if (!explain_only
        && state->runtime_role == RuntimeRole::Bypass
        && !fallback_outcome_counted
        && g_shared != nullptr) {
        pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
    }
}

/** Execute the retained child and explicitly project its raw slot. */
TupleTableSlot* cache_exec_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    if (state->runtime_role == RuntimeRole::Consumer) {
        ResetExprContext(node->ss.ps.ps_ExprContext);
        TupleTableSlot* slot = state->replay_slot;
        ExecClearTuple(slot);
        if (!tuplestore_gettupleslot(
                state->replay_store, true, false, slot)) {
            return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
        }
        return project_exact_raw_slot(node, slot);
    }
    PlanState* child = retained_child(node);
    if (child == nullptr) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
#if defined(BIOSEQ_CACHE_TESTING)
    if (state->runtime_role == RuntimeRole::Producer
        && !state->child_execution_counted) {
        state->child_execution_counted = true;
        pg_atomic_fetch_add_u64(
            &g_shared->test_control.exact_child_executions, 1);
    }
#endif
    TupleTableSlot* raw_slot = ExecProcNode(child);
    if (TupIsNull(raw_slot)) {
        if (state->runtime_role == RuntimeRole::Producer) {
            MemoryContext caller_context = CurrentMemoryContext;
            bool completed = false;
            PG_TRY();
            {
                completed = complete_registry_producer(
                    *state, exact_raw_tuple_descriptor(node));
            }
            PG_CATCH();
            {
                const int sqlstate = geterrcode();
                MemoryContextSwitchTo(caller_context);
                if (cache_interrupt_error(sqlstate)) {
                    fail_registry_producer(*state);
                    PG_RE_THROW();
                }
                FlushErrorState();
                completed = false;
            }
            PG_END_TRY();
            if (!completed) {
                fail_registry_producer(*state);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.fallbacks, 1);
                }
            } else {
                state->runtime_role = RuntimeRole::Bypass;
            }
        }
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    if (state->runtime_role == RuntimeRole::Producer) {
        MemoryContext caller_context = CurrentMemoryContext;
        bool captured = false;
        PG_TRY();
        {
            captured = capture_raw_child_row(*state, raw_slot);
        }
        PG_CATCH();
        {
            const int sqlstate = geterrcode();
            MemoryContextSwitchTo(caller_context);
            if (cache_interrupt_error(sqlstate)) {
                fail_registry_producer(*state);
                PG_RE_THROW();
            }
            FlushErrorState();
            captured = false;
        }
        PG_END_TRY();
        if (!captured) {
            fail_registry_producer(*state);
            if (g_shared != nullptr) {
                pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
            }
        }
    }
    return project_exact_raw_slot(node, raw_slot);
}

/** End only the child this node initialized. */
void cache_end_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    if (state->runtime_role == RuntimeRole::Producer
        && !state->producer_completed) {
        fail_registry_producer(*state);
    }
    cache_state_cleanup(state);
    if (state->child_initialized && retained_child(node) != nullptr) {
        ExecEndNode(retained_child(node));
        node->custom_ps = NIL;
    }
    state->child_initialized = false;
}

/** Rewind the retained child for supported forward rescans. */
void cache_rescan_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    if (state->runtime_role == RuntimeRole::Consumer
        && state->replay_store != nullptr) {
        tuplestore_rescan(state->replay_store);
        return;
    }
    if (state->runtime_role == RuntimeRole::Producer) {
        fail_registry_producer(*state);
        if (g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
        }
    }
    if (retained_child(node) != nullptr) {
        ExecReScan(retained_child(node));
    }
}

/** Explain provisional and actual pass-through state without side effects. */
[[nodiscard]] const char* readiness_name(
        CacheCorrectnessReadiness readiness) noexcept {
    switch (readiness) {
        case CacheCorrectnessReadiness::PassthroughOnly:
            return "passthrough-only";
        case CacheCorrectnessReadiness::RelationFreeOnly:
            return "relation-free-only";
        case CacheCorrectnessReadiness::GenericMvccReady:
            return "generic-mvcc-ready";
        case CacheCorrectnessReadiness::ProviderCompleteReady:
            return "provider-complete-ready";
        case CacheCorrectnessReadiness::ReusableCoverageReady:
            return "reusable-coverage-ready";
    }
    return "unknown";
}

[[nodiscard]] const char* worker_lifecycle_name(
        storage::WorkerLifecycle lifecycle) noexcept {
    switch (lifecycle) {
        case storage::WorkerLifecycle::NotRegistered: return "not-registered";
        case storage::WorkerLifecycle::Registered: return "registered";
        case storage::WorkerLifecycle::Starting: return "starting";
        case storage::WorkerLifecycle::Running: return "running";
        case storage::WorkerLifecycle::Stopping: return "stopping";
        case storage::WorkerLifecycle::Stopped: return "stopped";
        case storage::WorkerLifecycle::Failed: return "failed";
    }
    return "unknown";
}

void cache_explain_custom_scan(
        CustomScanState* node, List*, ExplainState* explain_state) {
    const auto* scan = reinterpret_cast<const CustomScan*>(node->ss.ps.plan);
    const QueryCacheMode mode = guc::mode();
    const char* mode_name = mode == QueryCacheMode::Reuse
        ? "reuse"
        : mode == QueryCacheMode::Same ? "same" : "disabled";
    const bool force_bypass = private_force_bypass(*scan);
    const int force_bypass_marker = private_force_bypass_marker(*scan);
    const int runtime_recipe_count = exact_runtime_recipe_count(*scan);
    const int parameter_count = private_list_length(*scan, 3, T_List);
    const int relation_dependency_count =
        private_list_length(*scan, 4, T_OidList);
    const int syscache_dependency_count =
        private_list_length(*scan, 5, T_List);
    const int dependency_count = relation_dependency_count < 0
            || syscache_dependency_count < 0
        ? -1
        : relation_dependency_count + syscache_dependency_count;
    ExplainPropertyText("Cache Mode", mode_name, explain_state);
    ExplainPropertyText(
        "Planned Cache Action",
        force_bypass
            ? "[Bypass] unsafe finalized source path"
            : "provisional exact result",
        explain_state);
    ExplainPropertyBool("Cache Force Bypass", force_bypass, explain_state);
    if (force_bypass) {
        ExplainPropertyText(
            "Cache Bypass Reason",
            force_bypass_marker == static_cast<int>(
                    ExactBypassMarkerV1::CoverageGateFinalizer)
                ? "internal nested-coverage gate finalizer"
                : runtime_recipe_count < 0
                ? "provider runtime recipe unavailable"
                : "unsafe finalized source path",
            explain_state);
    }
    ExplainPropertyInteger(
        "Provider Runtime Recipes", nullptr, runtime_recipe_count,
        explain_state);
    if (explain_state->analyze) {
        const auto* state = reinterpret_cast<const BioseqCacheState*>(node);
        ExplainPropertyText(
            "Provider Runtime Recipe State",
            state->runtime_recipe_valid
                ? "validated"
                : "unavailable-exact-passthrough",
            explain_state);
    }
    ExplainPropertyInteger(
        "Cache Parameter Count", nullptr, parameter_count, explain_state);
    ExplainPropertyInteger(
        "Cache Dependency Count", nullptr, dependency_count, explain_state);
    char* dependency_diagnostic = dependency_count < 0
        ? pstrdup("metadata invalid")
        : dependency_count == 0
            ? pstrdup("none (relation-free exact)")
            : psprintf(
                "%d relation%s, %d syscache item%s",
                relation_dependency_count,
                relation_dependency_count == 1 ? "" : "s",
                syscache_dependency_count,
                syscache_dependency_count == 1 ? "" : "s");
    ExplainPropertyText(
        "Cache Dependencies", dependency_diagnostic, explain_state);
    pfree(dependency_diagnostic);
    if (explain_state->analyze) {
        const auto* state = reinterpret_cast<const BioseqCacheState*>(node);
        const char* action = state->replay_store != nullptr
            ? "[Hit] exact final result"
            : state->producer_completed
                ? "[Produce] exact final result"
                : "[Bypass] retained child";
        ExplainPropertyText("Actual Cache Action", action, explain_state);
    }
    ExplainPropertyText("SQL", private_string(*scan, 1), explain_state);
    ExplainPropertyText("Cache Entry Kind", "ExactFinalResult", explain_state);
    const storage::StorageRuntimeSnapshot storage_snapshot =
        storage::runtime_snapshot();
    const ReusableRegistrySnapshotV1 reference_snapshot =
        runtime_reusable_registry_snapshot_v1();
    char* pending_hot_diagnostic =
        storage_snapshot.pending_hot_bytes_valid
        ? psprintf(
            "%llu",
            static_cast<unsigned long long>(
                storage_snapshot.pending_hot_bytes))
        : pstrdup("unavailable");
    char* pending_file_diagnostic =
        storage_snapshot.pending_file_bytes_valid
        ? psprintf(
            "%llu",
            static_cast<unsigned long long>(
                storage_snapshot.pending_file_bytes))
        : pstrdup("unavailable");
    const bool reference_snapshot_valid =
        reference_snapshot.boot_nonce != 0;
    char* registry_waiter_diagnostic = reference_snapshot_valid
        ? psprintf(
            "%llu",
            static_cast<unsigned long long>(
                reference_snapshot.waiter_references))
        : pstrdup("unavailable");
    char* registry_reader_diagnostic = reference_snapshot_valid
        ? psprintf(
            "%llu",
            static_cast<unsigned long long>(
                reference_snapshot.reader_pins))
        : pstrdup("unavailable");
    char* planner_pin_diagnostic = reference_snapshot_valid
        ? psprintf(
            "%llu",
            static_cast<unsigned long long>(
                reference_snapshot.planner_pins))
        : pstrdup("unavailable");
    ExplainPropertyText(
        "Cache Readiness",
        readiness_name(storage_snapshot.correctness_readiness),
        explain_state);
    char* storage_diagnostic = psprintf(
        "%s; backend=%s; hot=%llu/%llu; file=%llu/%llu; "
        "manifests=%llu; waiters=%llu",
        storage_snapshot.bootstrap_ready ? "ready" : "unavailable",
        storage_snapshot.dsa_attached ? "attached" : "not-attached",
        static_cast<unsigned long long>(storage_snapshot.reserved_hot_bytes),
        static_cast<unsigned long long>(storage_snapshot.hot_ceiling_bytes),
        static_cast<unsigned long long>(storage_snapshot.reserved_file_bytes),
        static_cast<unsigned long long>(storage_snapshot.file_ceiling_bytes),
        static_cast<unsigned long long>(storage_snapshot.manifest_count),
        static_cast<unsigned long long>(
            storage_snapshot.waiter_reference_count));
    ExplainPropertyText("Cache Storage", storage_diagnostic, explain_state);
    pfree(storage_diagnostic);
    char* ownership_diagnostic = psprintf(
        "readers=%llu; pending_hot=%s; pending_file=%s; "
        "registry_waiters=%s; registry_readers=%s; planner_pins=%s",
        static_cast<unsigned long long>(
            storage_snapshot.reader_reference_count),
        pending_hot_diagnostic,
        pending_file_diagnostic,
        registry_waiter_diagnostic,
        registry_reader_diagnostic,
        planner_pin_diagnostic);
    ExplainPropertyText(
        "Cache Storage Ownership", ownership_diagnostic, explain_state);
    pfree(ownership_diagnostic);
    pfree(pending_hot_diagnostic);
    pfree(pending_file_diagnostic);
    pfree(registry_waiter_diagnostic);
    pfree(registry_reader_diagnostic);
    pfree(planner_pin_diagnostic);
    ExplainPropertyText(
        "Cache Janitor",
        worker_lifecycle_name(storage_snapshot.worker_lifecycle),
        explain_state);
}

const CustomExecMethods& cache_exec_methods() {
    return executor::exact_scan::exact_executor_methods<
        cache_begin_custom_scan,
        cache_exec_custom_scan,
        cache_end_custom_scan,
        cache_rescan_custom_scan,
        cache_explain_custom_scan>();
}

/** Allocate the standard-layout executor state. */
Node* cache_create_custom_scan_state(CustomScan*) {
    auto* state = reinterpret_cast<BioseqCacheState*>(
        newNode(sizeof(BioseqCacheState), T_CustomScanState));
    state->css.methods = &cache_exec_methods();
    state->child_initialized = false;
    state->runtime_role = RuntimeRole::Bypass;
    state->registry_index = -1;
    state->column_checksum_policy = nullptr;
    state->column_sink = nullptr;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->replay_slot = nullptr;
    return reinterpret_cast<Node*>(&state->css);
}

const CustomScanMethods& cache_scan_methods() {
    return executor::exact_scan::exact_scan_methods<
        cache_create_custom_scan_state>();
}

struct ExactProviderRecipeCollectorV1 {
    PlannerInfo* root = nullptr;
    const List* finalized_subplans = NIL;
    MemoryContext planner_context = nullptr;
    provider_registry::ProviderSnapshotV2 providers{};
    List* recipes = NIL;
    std::array<const Plan*, kMaximumExactPlanWalkNodesV1> visited{};
    std::array<bool, kMaximumExactSubplansV1> referenced_subplans{};
    std::array<bool, kMaximumExactSubplansV1> visited_subplans{};
    std::size_t visited_count = 0;
    std::uint64_t serialized_recipe_bytes = 0;
    std::uint32_t plan_ordinal = 0;
    std::uint32_t recipe_count = 0;
    bool valid = true;
#if defined(BIOSEQ_CACHE_TESTING)
    bool test_recipe_fault_applied = false;
#endif

    [[nodiscard]] bool remember_subplan(int plan_id) noexcept {
        const List* subplans = root != nullptr && root->glob != nullptr
            ? root->glob->subplans
            : finalized_subplans;
        if (plan_id <= 0 || subplans == nullptr
            || plan_id > list_length(subplans)
            || static_cast<std::size_t>(plan_id)
                > referenced_subplans.size()) {
            valid = false;
            return true;
        }
        referenced_subplans[static_cast<std::size_t>(plan_id - 1)] = true;
        return false;
    }

    [[nodiscard]] bool collect_expression(Node* node) noexcept;
    [[nodiscard]] bool collect_plan_expressions(const Plan* plan) noexcept;
    [[nodiscard]] bool collect_partition_prune_expressions(
        const PartitionPruneInfo* prune_info) noexcept;
    [[nodiscard]] bool collect_plan(const Plan* plan);

    [[nodiscard]] bool collect_plan_list(const List* plans) {
        const ListCell* cell = nullptr;
        foreach(cell, plans) {
            if (!collect_plan(static_cast<const Plan*>(lfirst(cell)))) {
                return false;
            }
        }
        return true;
    }
};

bool collect_exact_subplan_ids_walker(Node* node, void* opaque) {
    auto* collector = static_cast<ExactProviderRecipeCollectorV1*>(opaque);
    if (node == nullptr || collector == nullptr || !collector->valid) {
        return !collector || !collector->valid;
    }
    if (IsA(node, SubPlan)) {
        const auto* subplan = reinterpret_cast<const SubPlan*>(node);
        if (collector->remember_subplan(subplan->plan_id)) {
            return true;
        }
    }
    return expression_tree_walker(
        node, collect_exact_subplan_ids_walker, collector);
}

bool ExactProviderRecipeCollectorV1::collect_expression(
        Node* node) noexcept {
    return node == nullptr
        || !collect_exact_subplan_ids_walker(node, this);
}

/** Collect executor expressions hidden outside Plan's common expression fields. */
bool ExactProviderRecipeCollectorV1::collect_partition_prune_expressions(
        const PartitionPruneInfo* prune_info) noexcept {
    if (prune_info == nullptr) {
        return true;
    }
    if (!IsA(prune_info, PartitionPruneInfo)) {
        return false;
    }
    const ListCell* hierarchy_cell = nullptr;
    foreach(hierarchy_cell, prune_info->prune_infos) {
        const auto* hierarchy = static_cast<const List*>(
            lfirst(hierarchy_cell));
        if (hierarchy != NIL && !IsA(hierarchy, List)) {
            return false;
        }
        const ListCell* relation_cell = nullptr;
        foreach(relation_cell, hierarchy) {
            const auto* relation = static_cast<
                const PartitionedRelPruneInfo*>(lfirst(relation_cell));
            if (relation == nullptr
                || !IsA(relation, PartitionedRelPruneInfo)) {
                return false;
            }
            const List* step_lists[] = {
                relation->initial_pruning_steps,
                relation->exec_pruning_steps,
            };
            for (const List* steps : step_lists) {
                const ListCell* step_cell = nullptr;
                foreach(step_cell, steps) {
                    const auto* step = static_cast<
                        const PartitionPruneStep*>(lfirst(step_cell));
                    if (step == nullptr) {
                        return false;
                    }
                    switch (nodeTag(step)) {
                        case T_PartitionPruneStepOp:
                            if (!collect_expression(reinterpret_cast<Node*>(
                                    reinterpret_cast<
                                        const PartitionPruneStepOp*>(step)
                                        ->exprs))) {
                                return false;
                            }
                            break;
                        case T_PartitionPruneStepCombine:
                            break;
                        default:
                            return false;
                    }
                }
            }
        }
    }
    return true;
}

/** Collect every PG17 Plan-type field that can contain executable expressions. */
bool ExactProviderRecipeCollectorV1::collect_plan_expressions(
        const Plan* plan) noexcept {
    if (plan == nullptr
        || !collect_expression(reinterpret_cast<Node*>(plan->targetlist))
        || !collect_expression(reinterpret_cast<Node*>(plan->qual))
        || !collect_expression(reinterpret_cast<Node*>(plan->initPlan))) {
        return false;
    }

    switch (nodeTag(plan)) {
        case T_Result:
            return collect_expression(
                reinterpret_cast<const Result*>(plan)->resconstantqual);
        case T_ModifyTable: {
            const auto* modify = reinterpret_cast<const ModifyTable*>(plan);
            const ListCell* per_relation_cell = nullptr;
            foreach(per_relation_cell, modify->withCheckOptionLists) {
                const auto* options = static_cast<const List*>(
                    lfirst(per_relation_cell));
                if (options != NIL && !IsA(options, List)) {
                    return false;
                }
                const ListCell* option_cell = nullptr;
                foreach(option_cell, options) {
                    const auto* option = static_cast<const WithCheckOption*>(
                        lfirst(option_cell));
                    if (option == nullptr || !IsA(option, WithCheckOption)
                        || !collect_expression(option->qual)) {
                        return false;
                    }
                }
            }
            foreach(per_relation_cell, modify->mergeActionLists) {
                const auto* actions = static_cast<const List*>(
                    lfirst(per_relation_cell));
                if (actions != NIL && !IsA(actions, List)) {
                    return false;
                }
                const ListCell* action_cell = nullptr;
                foreach(action_cell, actions) {
                    const auto* action = static_cast<const MergeAction*>(
                        lfirst(action_cell));
                    if (action == nullptr || !IsA(action, MergeAction)
                        || !collect_expression(action->qual)
                        || !collect_expression(reinterpret_cast<Node*>(
                            action->targetList))) {
                        return false;
                    }
                }
            }
            return collect_expression(reinterpret_cast<Node*>(
                       modify->returningLists))
                && collect_expression(reinterpret_cast<Node*>(
                       modify->onConflictSet))
                && collect_expression(modify->onConflictWhere)
                && collect_expression(reinterpret_cast<Node*>(
                       modify->exclRelTlist))
                && collect_expression(reinterpret_cast<Node*>(
                       modify->mergeJoinConditions));
        }
        case T_Append:
            return collect_partition_prune_expressions(
                reinterpret_cast<const Append*>(plan)->part_prune_info);
        case T_MergeAppend:
            return collect_partition_prune_expressions(
                reinterpret_cast<const MergeAppend*>(plan)->part_prune_info);
        case T_SampleScan: {
            const auto* sample = reinterpret_cast<const SampleScan*>(plan);
            const auto* clause = sample->tablesample;
            return clause != nullptr && IsA(clause, TableSampleClause)
                && collect_expression(reinterpret_cast<Node*>(clause->args))
                && collect_expression(reinterpret_cast<Node*>(
                    clause->repeatable));
        }
        case T_IndexScan: {
            const auto* scan = reinterpret_cast<const IndexScan*>(plan);
            return collect_expression(reinterpret_cast<Node*>(scan->indexqual))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->indexqualorig))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->indexorderby))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->indexorderbyorig));
        }
        case T_IndexOnlyScan: {
            const auto* scan = reinterpret_cast<const IndexOnlyScan*>(plan);
            return collect_expression(reinterpret_cast<Node*>(scan->indexqual))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->recheckqual))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->indexorderby))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->indextlist));
        }
        case T_BitmapIndexScan: {
            const auto* scan = reinterpret_cast<const BitmapIndexScan*>(plan);
            return collect_expression(reinterpret_cast<Node*>(scan->indexqual))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->indexqualorig));
        }
        case T_BitmapHeapScan:
            return collect_expression(reinterpret_cast<Node*>(
                reinterpret_cast<const BitmapHeapScan*>(plan)
                    ->bitmapqualorig));
        case T_TidScan:
            return collect_expression(reinterpret_cast<Node*>(
                reinterpret_cast<const TidScan*>(plan)->tidquals));
        case T_TidRangeScan:
            return collect_expression(reinterpret_cast<Node*>(
                reinterpret_cast<const TidRangeScan*>(plan)->tidrangequals));
        case T_FunctionScan: {
            const List* functions = reinterpret_cast<const FunctionScan*>(plan)
                ->functions;
            const ListCell* function_cell = nullptr;
            foreach(function_cell, functions) {
                const auto* function = static_cast<const RangeTblFunction*>(
                    lfirst(function_cell));
                if (function == nullptr || !IsA(function, RangeTblFunction)
                    || !collect_expression(function->funcexpr)) {
                    return false;
                }
            }
            return true;
        }
        case T_ValuesScan:
            return collect_expression(reinterpret_cast<Node*>(
                reinterpret_cast<const ValuesScan*>(plan)->values_lists));
        case T_TableFuncScan: {
            const auto* table_func = reinterpret_cast<const TableFuncScan*>(plan)
                ->tablefunc;
            return table_func != nullptr && IsA(table_func, TableFunc)
                && collect_expression(reinterpret_cast<Node*>(
                       table_func->ns_uris))
                && collect_expression(table_func->docexpr)
                && collect_expression(table_func->rowexpr)
                && collect_expression(reinterpret_cast<Node*>(
                       table_func->colexprs))
                && collect_expression(reinterpret_cast<Node*>(
                       table_func->coldefexprs))
                && collect_expression(reinterpret_cast<Node*>(
                       table_func->colvalexprs))
                && collect_expression(reinterpret_cast<Node*>(
                       table_func->passingvalexprs));
        }
        case T_ForeignScan: {
            const auto* scan = reinterpret_cast<const ForeignScan*>(plan);
            return collect_expression(reinterpret_cast<Node*>(scan->fdw_exprs))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->fdw_scan_tlist))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->fdw_recheck_quals));
        }
        case T_CustomScan: {
            const auto* scan = reinterpret_cast<const CustomScan*>(plan);
            return collect_expression(reinterpret_cast<Node*>(
                       scan->custom_exprs))
                && collect_expression(reinterpret_cast<Node*>(
                       scan->custom_scan_tlist));
        }
        case T_NestLoop: {
            const auto* join = reinterpret_cast<const NestLoop*>(plan);
            if (!collect_expression(reinterpret_cast<Node*>(
                    join->join.joinqual))) {
                return false;
            }
            const ListCell* parameter_cell = nullptr;
            foreach(parameter_cell, join->nestParams) {
                const auto* parameter = static_cast<const NestLoopParam*>(
                    lfirst(parameter_cell));
                if (parameter == nullptr || !IsA(parameter, NestLoopParam)
                    || !collect_expression(reinterpret_cast<Node*>(
                        parameter->paramval))) {
                    return false;
                }
            }
            return true;
        }
        case T_MergeJoin: {
            const auto* join = reinterpret_cast<const MergeJoin*>(plan);
            return collect_expression(reinterpret_cast<Node*>(
                       join->join.joinqual))
                && collect_expression(reinterpret_cast<Node*>(
                       join->mergeclauses));
        }
        case T_HashJoin: {
            const auto* join = reinterpret_cast<const HashJoin*>(plan);
            return collect_expression(reinterpret_cast<Node*>(
                       join->join.joinqual))
                && collect_expression(reinterpret_cast<Node*>(
                       join->hashclauses))
                && collect_expression(reinterpret_cast<Node*>(
                       join->hashkeys));
        }
        case T_Memoize:
            return collect_expression(reinterpret_cast<Node*>(
                reinterpret_cast<const Memoize*>(plan)->param_exprs));
        case T_WindowAgg: {
            const auto* aggregate = reinterpret_cast<const WindowAgg*>(plan);
            return collect_expression(aggregate->startOffset)
                && collect_expression(aggregate->endOffset)
                && collect_expression(reinterpret_cast<Node*>(
                       aggregate->runCondition))
                && collect_expression(reinterpret_cast<Node*>(
                       aggregate->runConditionOrig));
        }
        case T_Hash:
            return collect_expression(reinterpret_cast<Node*>(
                reinterpret_cast<const Hash*>(plan)->hashkeys));
        case T_Limit: {
            const auto* limit = reinterpret_cast<const Limit*>(plan);
            return collect_expression(limit->limitOffset)
                && collect_expression(limit->limitCount);
        }
        case T_ProjectSet:
        case T_RecursiveUnion:
        case T_BitmapAnd:
        case T_BitmapOr:
        case T_SeqScan:
        case T_SubqueryScan:
        case T_CteScan:
        case T_NamedTuplestoreScan:
        case T_WorkTableScan:
        case T_Material:
        case T_Sort:
        case T_IncrementalSort:
        case T_Group:
        case T_Agg:
        case T_Unique:
        case T_Gather:
        case T_GatherMerge:
        case T_SetOp:
        case T_LockRows:
            return true;
        default:
            return false;
    }
}

/** Append one provider recipe and recurse over the finalized Plan in preorder. */
bool ExactProviderRecipeCollectorV1::collect_plan(const Plan* plan) {
    if (!valid || plan == nullptr) {
        return valid;
    }
    for (std::size_t index = 0; index < visited_count; ++index) {
        if (visited[index] == plan) {
            return true;
        }
    }
    if (visited_count >= visited.size()
        || plan_ordinal == (std::numeric_limits<std::uint32_t>::max)()) {
        valid = false;
        return false;
    }
    visited[visited_count++] = plan;
    const std::uint32_t current_ordinal = plan_ordinal++;
    if (!collect_plan_expressions(plan)) {
        valid = false;
        return false;
    }

    const provider_registry::ProviderV2* matched_provider = nullptr;
    for (std::uint32_t index = 0; index < providers.provider_count; ++index) {
        const provider_registry::ProviderV2* provider =
            providers.providers[index];
        if (provider != nullptr
            && reuse::provider_plan_matches(plan, provider)) {
            if (matched_provider != nullptr) {
                valid = false;
                return false;
            }
            matched_provider = provider;
        }
    }
    if (matched_provider != nullptr) {
        if (!provider_allows_exact_source_elision(matched_provider)
            || recipe_count >= kMaxProviderRecipesPerExactPlan
            || matched_provider->provider_id
                > static_cast<std::uint32_t>(INT_MAX)
            || matched_provider->abi_version
                > static_cast<std::uint32_t>(INT_MAX)
            || current_ordinal > static_cast<std::uint32_t>(INT_MAX)) {
            valid = false;
            return false;
        }
        reuse::PlannerOwnedDigestRecipeV1 recipe{};
        reuse::ProviderCallDiagnosticV1 diagnostic{};
        if (!reuse::invoke_build_recipe_fail_open(
                matched_provider,
                root,
                plan,
                planner_context,
                &recipe,
                &diagnostic)
            || recipe.serialized_bytes
                > kMaxProviderRecipeBytes - serialized_recipe_bytes) {
            valid = false;
            return false;
        }
#if defined(BIOSEQ_CACHE_TESTING)
        if (g_test_exact_recipe_fault
                == TestExactRecipeFault::ExpectedDigestMismatch
            && !test_recipe_fault_applied) {
            recipe.value.expected_generation_digest[0] ^= 0xa5U;
            test_recipe_fault_applied = true;
        }
#endif
        Const* expected = make_binary_node(std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(
                recipe.value.expected_generation_digest),
            sizeof(recipe.value.expected_generation_digest)});
        if (expected == nullptr
            || recipe.value.digest_format_version
                != kExactProviderRecipePlanFormatV1) {
            valid = false;
            return false;
        }
        List* encoded_recipe = list_make5(
                makeInteger(static_cast<int>(
                    recipe.value.digest_format_version)),
                makeInteger(static_cast<int>(matched_provider->provider_id)),
                makeInteger(static_cast<int>(matched_provider->abi_version)),
                makeInteger(static_cast<int>(current_ordinal)),
                expected);
        encoded_recipe = lappend(
            encoded_recipe, copyObject(recipe.value.plan_domain_nodes));
        recipes = lappend(recipes, encoded_recipe);
        serialized_recipe_bytes += recipe.serialized_bytes;
        ++recipe_count;
    }

    if (!collect_plan(outerPlan(plan)) || !collect_plan(innerPlan(plan))) {
        return false;
    }
    switch (nodeTag(plan)) {
        case T_SubqueryScan:
            return collect_plan(
                reinterpret_cast<const SubqueryScan*>(plan)->subplan);
        case T_CustomScan:
            return collect_plan_list(
                reinterpret_cast<const CustomScan*>(plan)->custom_plans);
        case T_Append:
            return collect_plan_list(
                reinterpret_cast<const Append*>(plan)->appendplans);
        case T_MergeAppend:
            return collect_plan_list(
                reinterpret_cast<const MergeAppend*>(plan)->mergeplans);
        case T_BitmapAnd:
            return collect_plan_list(
                reinterpret_cast<const BitmapAnd*>(plan)->bitmapplans);
        case T_BitmapOr:
            return collect_plan_list(
                reinterpret_cast<const BitmapOr*>(plan)->bitmapplans);
        case T_CteScan:
            return !remember_subplan(
                reinterpret_cast<const CteScan*>(plan)->ctePlanId);
        case T_Agg:
            return collect_plan_list(
                reinterpret_cast<const Agg*>(plan)->chain);
        case T_Result:
        case T_ProjectSet:
        case T_ModifyTable:
        case T_RecursiveUnion:
        case T_SeqScan:
        case T_SampleScan:
        case T_IndexScan:
        case T_IndexOnlyScan:
        case T_BitmapIndexScan:
        case T_BitmapHeapScan:
        case T_TidScan:
        case T_TidRangeScan:
        case T_FunctionScan:
        case T_ValuesScan:
        case T_TableFuncScan:
        case T_NamedTuplestoreScan:
        case T_WorkTableScan:
        case T_ForeignScan:
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin:
        case T_Material:
        case T_Memoize:
        case T_Sort:
        case T_IncrementalSort:
        case T_Group:
        case T_WindowAgg:
        case T_Unique:
        case T_Gather:
        case T_GatherMerge:
        case T_Hash:
        case T_SetOp:
        case T_LockRows:
        case T_Limit:
            return true;
        default:
            valid = false;
            return false;
    }
}

/** Capture main-tree recipes first, then referenced global plans by plan ID. */
[[nodiscard]] bool collect_exact_provider_recipes(
        PlannerInfo* root,
        const Plan* child,
        List** recipes,
        std::uint32_t* recipe_count) {
    if (root == nullptr || root->glob == nullptr || child == nullptr
        || recipes == nullptr || recipe_count == nullptr) {
        return false;
    }
    ExactProviderRecipeCollectorV1 collector{
        .root = root,
        .planner_context = root->planner_cxt,
        .providers = provider_registry::snapshot_providers_v2(),
    };
    if (!collector.collect_plan(child)) {
        return false;
    }
    /* Top-level InitPlans are attached after this CustomPath is lowered. */
    if (!collector.collect_expression(
            reinterpret_cast<Node*>(root->init_plans))) {
        return false;
    }
    bool progressed = true;
    while (progressed && collector.valid) {
        progressed = false;
        const int subplan_count = list_length(root->glob->subplans);
        if (subplan_count < 0
            || static_cast<std::size_t>(subplan_count)
                > collector.referenced_subplans.size()) {
            return false;
        }
        for (int plan_id = 1; plan_id <= subplan_count; ++plan_id) {
            const std::size_t index = static_cast<std::size_t>(plan_id - 1);
            if (!collector.referenced_subplans[index]
                || collector.visited_subplans[index]) {
                continue;
            }
            collector.visited_subplans[index] = true;
            progressed = true;
            if (!collector.collect_plan(static_cast<const Plan*>(
                    list_nth(root->glob->subplans, plan_id - 1)))) {
                return false;
            }
        }
    }
    if (!collector.valid) {
        return false;
    }
    *recipes = collector.recipes;
    *recipe_count = collector.recipe_count;
    return true;
}

/**
 * Rebuild provider recipes from the post-setrefs plan owned by PlannedStmt.
 *
 * The provider callback receives a null PlannerInfo in this phase by contract;
 * every Plan and referenced subplan is already in its final executor shape.
 */
[[nodiscard]] bool collect_finalized_exact_provider_recipes(
        const Plan* child,
        const List* finalized_subplans,
        MemoryContext planner_context,
        List** recipes,
        std::uint32_t* recipe_count) {
    if (child == nullptr || planner_context == nullptr
        || recipes == nullptr || recipe_count == nullptr
        || list_length(finalized_subplans)
            > static_cast<int>(kMaximumExactSubplansV1)) {
        return false;
    }
    ExactProviderRecipeCollectorV1 collector{
        .root = nullptr,
        .finalized_subplans = finalized_subplans,
        .planner_context = planner_context,
        .providers = provider_registry::snapshot_providers_v2(),
    };
    if (!collector.collect_plan(child)
        || !collector.collect_plan_list(finalized_subplans)
        || !collector.valid) {
        return false;
    }
    *recipes = collector.recipes;
    *recipe_count = collector.recipe_count;
    return true;
}

/** Convert one pre-setrefs outer CustomPath into a CustomScan. */
Plan* cache_plan_custom_path(
        PlannerInfo* root, RelOptInfo* relation, CustomPath* best_path,
        List* target_list, List*, List* custom_plans) {
    if (root == nullptr || relation == nullptr || best_path == nullptr
        || custom_plans == NIL
        || list_length(custom_plans) != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqCacheScan selected path is malformed")));
        pg_unreachable();
    }
    if (!finalize_selected_coverage_gates(*root)) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("Bioseq cache selected coverage gates are malformed")));
        pg_unreachable();
    }
    Plan* child = static_cast<Plan*>(linitial(custom_plans));
    List* runtime_recipes = NIL;
    std::uint32_t runtime_recipe_count = 0;
    const bool runtime_recipes_complete = collect_exact_provider_recipes(
        root, child, &runtime_recipes, &runtime_recipe_count);
    auto* scan = makeNode(CustomScan);
    scan->methods = &cache_scan_methods();
    scan->flags = best_path->flags;
    scan->scan.scanrelid = 0;
    scan->scan.plan.targetlist = target_list;
    scan->scan.plan.qual = NIL;
    scan->custom_plans = custom_plans;
    scan->custom_private = static_cast<List*>(copyObject(best_path->custom_private));
    if (!runtime_recipes_complete) {
        static_cast<void>(set_private_force_bypass(*scan, true));
    }
    scan->custom_private = lappend(
        scan->custom_private,
        runtime_recipes_complete
            ? static_cast<void*>(runtime_recipes)
            : static_cast<void*>(NIL));
    scan->custom_private = lappend(
        scan->custom_private,
        makeInteger(runtime_recipes_complete
            ? static_cast<int>(runtime_recipe_count)
            : -1));
    scan->custom_exprs = NIL;
    scan->custom_scan_tlist = static_cast<List*>(copyObject(child->targetlist));
    scan->custom_relids = bms_copy(relation->relids);
    return reinterpret_cast<Plan*>(scan);
}

const CustomPathMethods& cache_path_methods() {
    static const CustomPathMethods methods = {
        .CustomName = "BioseqCacheScan",
        .PlanCustomPath = cache_plan_custom_path,
    };
    return methods;
}

enum class CoverageRuntimeRoleV1 : std::uint8_t {
    Bypass = 0,
    Producer = 1,
    CurrentConsumer = 2,
};

enum class CoverageStoredColumnKindV1 : std::uint8_t {
    ResultOrdinal = 0,
    RteOrdinal = 1,
    LogicalRelationOid = 2,
    LeafRelationOid = 3,
    IdentityKind = 4,
    RowIdentity = 5,
    OccurrenceHigh = 6,
    OccurrenceLow = 7,
    CapabilityMask = 8,
    ProviderAttribute = 9,
};

inline constexpr std::size_t kCoverageOccurrenceColumnCountV1 = 9;

struct CoverageStoredColumnV1 {
    CoverageStoredColumnKindV1 kind;
    std::array<std::uint8_t, 3> reserved{};
    std::uint32_t provider_attno;
};

struct CoverageStoredRecipeV1 {
    TupleDesc descriptor = nullptr;
    CoverageStoredColumnV1* columns = nullptr;
    std::uint32_t column_count = 0;
};

struct CoverageOccurrenceKeyV1 {
    std::uint32_t rte_ordinal;
    std::uint32_t logical_relation_oid;
    std::uint32_t leaf_relation_oid;
    std::uint32_t identity_kind;
    std::uint64_t row_identity;
    std::uint64_t occurrence_high;
    std::uint64_t occurrence_low;
};

struct CoverageOccurrenceOwnerV1 {
    CoverageOccurrenceKeyV1 key;
    std::uint32_t source_index;
    std::uint32_t reserved;
};

struct CoverageRuntimeSourceV1 {
    bool valid;
    bool registry_reference_owned;
    bool manifest_pin_owned;
    bool waited;
    bool ttl_entitlement_consumed;
    std::array<std::uint8_t, 3> reserved_flags{};
    ReusableGenerationReferenceV1 reference;
    ReusableReferenceTokenV1 registry_reference;
    ReusableGenerationSnapshotV1 completed_snapshot;
    storage::ManifestPin manifest_pin;
    Tuplestorestate* replay_store;
    TupleTableSlot* replay_slot;
    ExprState* predicate_state;
    Bitmapset* predicate_attnos;
    PlannerDecodedCoverageV1 contract;
    TupleDesc stored_descriptor;
    CoverageStoredColumnV1* stored_columns;
    std::uint32_t stored_column_count;
    std::uint32_t boundary_map_count;
    std::uint32_t lineage_map_count;
    std::uint32_t reserved_count;
    BioseqCacheColumnMapEntryV2* boundary_map;
    BioseqCacheColumnMapEntryV2* lineage_map;
    List* provider_private_nodes;
    const void* generation_key_node;
    const void* descriptor_node;
    MemoryContext source_preflight_context;
    std::uint64_t row_count;
    std::uint64_t selected_row_count;
    std::uint64_t replay_row_index;
    std::uint8_t* selection_bitmap;
    std::size_t selection_bitmap_bytes;
};

struct ReusableCoverageState {
    CustomScanState css;
    bool child_initialized;
    bool runtime_recipe_valid;
    bool current_producer_owned;
    bool current_reference_owned;
    bool producer_completed;
    bool capture_failed;
    bool cleanup_registered;
    bool cleanup_done;
    bool current_manifest_pin_owned;
    bool current_waited;
    bool current_ttl_entitlement_consumed;
    bool producer_spool_failed;
    bool older_preflight_ready;
    bool planned_diagnostics_published;
    bool replaying_materialized_output;
    bool output_materialized_complete;
    std::uint16_t source_count;
    std::uint16_t active_source_count;
    std::uint16_t replay_source_index;
    std::uint16_t reserved;
    CoverageRuntimeRoleV1 runtime_role;
    std::array<std::uint8_t, 7> reserved_role;
    const provider_registry::ProviderV2* provider;
    provider_registry::ProviderLeaseV2 provider_lease;
    CoverageCurrentPlanHeaderV1 current_header;
    CacheDigest256 dependency_fingerprint;
    PhysicalIdentityV1 dependency_physical_identity;
    CacheDigest256 schema_fingerprint;
    CacheDigest256 mapping_fingerprint;
    CacheDigest256 lineage_fingerprint;
    PlannerDecodedCoverageV1 current_contract;
    ReusableGenerationReferenceV1 current_reference;
    ReusableReferenceTokenV1 current_reference_token;
    storage::ManifestHandle manifest_handle;
    storage::ManifestPin current_manifest_pin;
    std::array<storage::StoredBlockToken, storage::kBlocksPerManifest>
        stored_tokens;
    std::uint32_t stored_token_count;
    std::uint32_t reserved_count;
    std::uint64_t row_count;
    std::uint64_t logical_bytes;
    std::uint64_t payload_bytes;
    std::uint64_t producer_ttl_ms;
    std::uint64_t selection_bitmap_bytes_admitted;
    std::uint64_t occurrence_owner_count;
    std::uint64_t producer_spool_rows;
    std::uint64_t producer_spool_bytes;
    std::uint64_t visibility_completion_count;
    TimestampTz producer_started_at;
    PostgreSqlColumnChecksumPolicy* column_checksum_policy;
    storage::StorageColumnStoreBlockSink* column_sink;
    PostgreSqlStreamingColumnStoreBuilder* column_builder;
    Oid* send_functions;
    TupleDesc stored_descriptor;
    CoverageStoredColumnV1* stored_columns;
    std::uint32_t stored_column_count;
    std::uint32_t reserved_stored;
    List* plan_domain_nodes;
    Tuplestorestate* current_replay_store;
    TupleTableSlot* current_replay_slot;
    Tuplestorestate* producer_output_store;
    TupleTableSlot* producer_output_slot;
    TupleDesc predicate_descriptor;
    TupleTableSlot* predicate_slot;
    std::uint32_t* predicate_consumer_attnos;
    std::uint32_t predicate_column_count;
    std::uint32_t reserved_predicate;
    ExprState* requested_predicate_state;
    Bitmapset* requested_predicate_attnos;
    HTAB* occurrence_owners;
    std::array<CoverageRuntimeSourceV1, kMaxSourcesPerCoverageNode> sources;
    BackendQueryCleanupLinkV1 backend_cleanup;
    MemoryContextCallback cleanup_callback;
};

static_assert(std::is_standard_layout_v<ReusableCoverageState>);
static_assert(offsetof(ReusableCoverageState, css) == 0);

struct CoveragePredicateAttnoContextV1 {
    TupleDesc descriptor = nullptr;
    Bitmapset* attnos = nullptr;
    bool valid = true;
};

bool collect_coverage_predicate_attnos_walker(
        Node* node, void* opaque) {
    if (node == nullptr) {
        return false;
    }
    auto* context = static_cast<CoveragePredicateAttnoContextV1*>(opaque);
    if (context == nullptr || !context->valid) {
        return true;
    }
    if (IsA(node, Var)) {
        const auto* variable = reinterpret_cast<const Var*>(node);
        if (context->descriptor == nullptr || variable->varlevelsup != 0
            || variable->varattno <= 0
            || variable->varattno > context->descriptor->natts) {
            context->valid = false;
            return true;
        }
        context->attnos = bms_add_member(
            context->attnos, variable->varattno);
        return false;
    }
    if (IsA(node, Param)) {
        context->valid = false;
        return true;
    }
    return expression_tree_walker(
        node, collect_coverage_predicate_attnos_walker, opaque);
}

[[nodiscard]] bool collect_coverage_predicate_attnos(
        Node* expression,
        TupleDesc descriptor,
        Bitmapset** output) {
    if (expression == nullptr || descriptor == nullptr || output == nullptr) {
        return false;
    }
    CoveragePredicateAttnoContextV1 context{
        .descriptor = descriptor,
    };
    static_cast<void>(collect_coverage_predicate_attnos_walker(
        expression, &context));
    if (!context.valid) {
        bms_free(context.attnos);
        return false;
    }
    *output = context.attnos;
    return true;
}

/** Build the deterministic dense cache-expression slot from boundary schema. */
[[nodiscard]] bool initialize_coverage_predicate_slot(
        ReusableCoverageState& state,
        EState* estate,
        TupleDesc raw_descriptor) {
    const BioseqCachePathDomainV2& domain = state.current_contract.domain;
    if (estate == nullptr || raw_descriptor == nullptr
        || domain.boundary_columns == nullptr
        || domain.boundary_column_count == 0
        || domain.boundary_column_count
            > static_cast<std::size_t>(MaxTupleAttributeNumber)
        || domain.boundary_column_count
            > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    const std::size_t count = domain.boundary_column_count;
    auto* order = static_cast<std::size_t*>(palloc(
        count * sizeof(std::size_t)));
    auto* consumer_attnos = static_cast<std::uint32_t*>(palloc(
        count * sizeof(std::uint32_t)));
    for (std::size_t index = 0; index < count; ++index) {
        order[index] = index;
    }
    std::sort(order, order + count, [&domain](
            std::size_t left, std::size_t right) {
        return domain.boundary_columns[left].attno
            < domain.boundary_columns[right].attno;
    });
    TupleDesc descriptor = CreateTemplateTupleDesc(
        static_cast<int>(count));
    bool valid = descriptor != nullptr;
    for (std::size_t dense_index = 0;
         valid && dense_index < count;
         ++dense_index) {
        const BioseqCacheColumnSchemaV2& column =
            domain.boundary_columns[order[dense_index]];
        valid = column.attno != 0
            && column.attno <= static_cast<std::uint32_t>(
                raw_descriptor->natts);
        if (!valid) {
            break;
        }
        const Form_pg_attribute raw = TupleDescAttr(
            raw_descriptor, static_cast<int>(column.attno - 1U));
        valid = !raw->attisdropped
            && raw->atttypid == column.type_oid
            && raw->atttypmod == column.typmod
            && raw->attcollation == column.collation_oid;
        if (!valid) {
            break;
        }
        consumer_attnos[dense_index] = column.attno;
        TupleDescInitEntry(
            descriptor,
            static_cast<AttrNumber>(dense_index + 1U),
            nullptr,
            static_cast<Oid>(column.type_oid),
            column.typmod,
            0);
        TupleDescInitEntryCollation(
            descriptor,
            static_cast<AttrNumber>(dense_index + 1U),
            static_cast<Oid>(column.collation_oid));
    }
    pfree(order);
    if (!valid) {
        pfree(consumer_attnos);
        FreeTupleDesc(descriptor);
        return false;
    }
    TupleTableSlot* slot = ExecInitExtraTupleSlot(
        estate, descriptor, &TTSOpsVirtual);
    if (slot == nullptr) {
        pfree(consumer_attnos);
        FreeTupleDesc(descriptor);
        return false;
    }
    state.predicate_descriptor = descriptor;
    state.predicate_slot = slot;
    state.predicate_consumer_attnos = consumer_attnos;
    state.predicate_column_count = static_cast<std::uint32_t>(count);
    return true;
}

/** Copy consumer-shaped values into the deterministic dense replay slot. */
[[nodiscard]] bool populate_coverage_predicate_slot(
        ReusableCoverageState& state,
        TupleTableSlot* raw) {
    if (raw == nullptr || raw->tts_tupleDescriptor == nullptr
        || state.predicate_slot == nullptr
        || state.predicate_consumer_attnos == nullptr
        || state.predicate_column_count == 0
        || state.predicate_descriptor == nullptr
        || state.predicate_descriptor->natts
            != static_cast<int>(state.predicate_column_count)) {
        return false;
    }
    slot_getallattrs(raw);
    ExecClearTuple(state.predicate_slot);
    for (std::uint32_t dense_index = 0;
         dense_index < state.predicate_column_count;
         ++dense_index) {
        const std::uint32_t consumer_attno =
            state.predicate_consumer_attnos[dense_index];
        if (consumer_attno == 0
            || consumer_attno > static_cast<std::uint32_t>(
                raw->tts_tupleDescriptor->natts)) {
            ExecClearTuple(state.predicate_slot);
            return false;
        }
        state.predicate_slot->tts_values[dense_index] =
            raw->tts_values[consumer_attno - 1U];
        state.predicate_slot->tts_isnull[dense_index] =
            raw->tts_isnull[consumer_attno - 1U];
    }
    state.predicate_slot->tts_nvalid = static_cast<int>(
        state.predicate_column_count);
    ExecStoreVirtualTuple(state.predicate_slot);
    return true;
}

struct CoverageSetrefsReplayContextV1 {
    AttrNumber raw_column_count = InvalidAttrNumber;
    TupleDesc predicate_descriptor = nullptr;
    bool valid = true;
};

/** Restore setrefs-mapped replay Vars to the executor's dense predicate slot. */
Node* restore_dense_coverage_predicate_mutator(
        Node* node, void* opaque) {
    if (node == nullptr) {
        return nullptr;
    }
    auto* context = static_cast<CoverageSetrefsReplayContextV1*>(opaque);
    if (context == nullptr || !context->valid
        || context->predicate_descriptor == nullptr
        || context->raw_column_count < 0) {
        return copyObject(node);
    }
    if (IsA(node, List)) {
        return expression_tree_mutator(
            node, restore_dense_coverage_predicate_mutator, opaque);
    }
    if (!IsA(node, Var)) {
        return expression_tree_mutator(
            node, restore_dense_coverage_predicate_mutator, opaque);
    }

    const auto* variable = reinterpret_cast<const Var*>(node);
    const int dense_attno = static_cast<int>(variable->varattno)
        - static_cast<int>(context->raw_column_count);
    if (variable->varno != INDEX_VAR || variable->varlevelsup != 0
        || !bms_is_empty(variable->varnullingrels)
        || dense_attno <= 0
        || dense_attno > context->predicate_descriptor->natts) {
        context->valid = false;
        return copyObject(node);
    }
    const Form_pg_attribute expected = TupleDescAttr(
        context->predicate_descriptor, dense_attno - 1);
    if (expected->attisdropped || variable->vartype != expected->atttypid
        || variable->vartypmod != expected->atttypmod
        || variable->varcollid != expected->attcollation) {
        context->valid = false;
        return copyObject(node);
    }
    auto* restored = reinterpret_cast<Var*>(copyObject(node));
    restored->varno = INDEX_VAR;
    restored->varattno = static_cast<AttrNumber>(dense_attno);
    restored->varnosyn = INDEX_VAR;
    restored->varattnosyn = static_cast<AttrNumber>(dense_attno);
    restored->location = -1;
    return reinterpret_cast<Node*>(restored);
}

/** Compile every frozen predicate only after setrefs produced consumer Vars. */
[[nodiscard]] bool compile_coverage_predicates(
        ReusableCoverageState& state,
        const CustomScan& scan,
        TupleDesc predicate_descriptor) {
    if (predicate_descriptor == nullptr || scan.custom_exprs == NIL
        || list_length(scan.custom_exprs)
            != static_cast<int>(state.source_count)
        || state.css.ss.ss_ScanTupleSlot == nullptr
        || state.css.ss.ss_ScanTupleSlot->tts_tupleDescriptor == nullptr
        || state.css.ss.ps.state == nullptr
        || state.css.ss.ps.state->es_query_cxt == nullptr
        || scan.custom_scan_tlist == NIL
        || list_length(scan.custom_scan_tlist)
            != state.css.ss.ss_ScanTupleSlot->tts_tupleDescriptor->natts
        || list_length(scan.custom_scan_tlist)
            <= predicate_descriptor->natts) {
        return false;
    }
    const int raw_column_count = list_length(scan.custom_scan_tlist)
        - predicate_descriptor->natts;
    if (raw_column_count <= 0
        || raw_column_count > MaxTupleAttributeNumber) {
        return false;
    }
    MemoryContext previous = MemoryContextSwitchTo(
        state.css.ss.ps.state->es_query_cxt);
    const auto restore = [raw_column_count, predicate_descriptor](
            Node* expression) -> Node* {
        CoverageSetrefsReplayContextV1 context{
            .raw_column_count = static_cast<AttrNumber>(raw_column_count),
            .predicate_descriptor = predicate_descriptor,
        };
        Node* restored = restore_dense_coverage_predicate_mutator(
            expression, &context);
        return context.valid ? restored : nullptr;
    };
    Bitmapset* attnos = nullptr;
    Node* requested = restore(
        static_cast<Node*>(linitial(scan.custom_exprs)));
    if (!collect_coverage_predicate_attnos(
            requested, predicate_descriptor, &attnos)) {
        MemoryContextSwitchTo(previous);
        return false;
    }
    state.requested_predicate_attnos = attnos;
    state.requested_predicate_state = ExecInitExpr(
        reinterpret_cast<Expr*>(requested), &state.css.ss.ps);
    if (state.requested_predicate_state == nullptr) {
        MemoryContextSwitchTo(previous);
        return false;
    }
    for (std::size_t index = 0;
         index + 1U < state.source_count;
         ++index) {
        Node* predicate = restore(static_cast<Node*>(list_nth(
            scan.custom_exprs, static_cast<int>(index + 1U))));
        attnos = nullptr;
        if (!collect_coverage_predicate_attnos(
                predicate, predicate_descriptor, &attnos)) {
            MemoryContextSwitchTo(previous);
            return false;
        }
        state.sources[index].predicate_attnos = attnos;
        state.sources[index].predicate_state = ExecInitExpr(
            reinterpret_cast<Expr*>(predicate), &state.css.ss.ps);
        if (state.sources[index].predicate_state == nullptr) {
            MemoryContextSwitchTo(previous);
            return false;
        }
    }
    MemoryContextSwitchTo(previous);
    return true;
}

[[nodiscard]] CoverageOccurrenceKeyV1 coverage_occurrence_key(
        const BioseqCacheOccurrenceV2& occurrence) noexcept {
    return CoverageOccurrenceKeyV1{
        .rte_ordinal = occurrence.rte_ordinal,
        .logical_relation_oid = occurrence.logical_relation_oid,
        .leaf_relation_oid = occurrence.leaf_relation_oid,
        .identity_kind = occurrence.identity_kind,
        .row_identity = occurrence.row_identity,
        .occurrence_high = occurrence.occurrence_high,
        .occurrence_low = occurrence.occurrence_low,
    };
}

[[nodiscard]] bool coverage_selection_bit(
        const CoverageRuntimeSourceV1& source,
        std::uint64_t row) noexcept {
    const std::uint64_t byte = row >> 3U;
    return source.selection_bitmap != nullptr
        && byte < source.selection_bitmap_bytes
        && (source.selection_bitmap[byte]
            & static_cast<std::uint8_t>(1U << (row & 7U))) != 0;
}

void set_coverage_selection_bit(
        CoverageRuntimeSourceV1& source,
        std::uint64_t row,
        bool selected) noexcept {
    const std::uint64_t byte = row >> 3U;
    if (source.selection_bitmap == nullptr
        || byte >= source.selection_bitmap_bytes) {
        return;
    }
    const std::uint8_t mask = static_cast<std::uint8_t>(
        1U << (row & 7U));
    if (selected) {
        source.selection_bitmap[byte] |= mask;
    } else {
        source.selection_bitmap[byte] &= static_cast<std::uint8_t>(~mask);
    }
}

/** Register a new CLAIMED producer for the enclosing ExecutorStart cleanup. */
[[nodiscard]] bool register_executor_start_claim(
        ReusableCoverageState& state) noexcept {
    ExecutorStartClaimFrameV1* frame = g_executor_start_claim_frame;
    if (frame == nullptr) {
        return false;
    }
    for (std::size_t index = 0;
         index < frame->coverage_producer_count;
         ++index) {
        if (frame->coverage_producers[index] == &state) {
            return true;
        }
    }
    if (frame->coverage_producer_count
            >= frame->coverage_producers.size()) {
        return false;
    }
    frame->coverage_producers[frame->coverage_producer_count++] = &state;
    return true;
}

struct CoverageSemanticFingerprintsV1 {
    CacheDigest256 schema{};
    CacheDigest256 mapping{};
    CacheDigest256 lineage{};
    bool valid = false;
};

[[nodiscard]] std::optional<CacheDigest256> tagged_coverage_digest(
        std::uint32_t tag,
        std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return std::nullopt;
    }
    try {
        std::vector<std::byte> tagged(sizeof(tag) + bytes.size());
        std::memcpy(tagged.data(), &tag, sizeof(tag));
        std::memcpy(tagged.data() + sizeof(tag), bytes.data(), bytes.size());
        PostgreSqlColumnChecksumPolicy policy;
        const auto digest = policy.sha256(tagged);
        return digest.has_value()
            ? std::optional<CacheDigest256>{*digest}
            : std::nullopt;
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] CoverageSemanticFingerprintsV1
coverage_semantic_fingerprints(
        const BioseqCachePathDomainV2& contract) noexcept;

[[nodiscard]] std::span<const std::byte> binary_node_payload(
        const void* node) noexcept {
    if (node == nullptr || !IsA(node, Const)) {
        return {};
    }
    const auto* constant = static_cast<const Const*>(node);
    if (constant->constisnull || constant->consttype != BYTEAOID
        || constant->constbyval) {
        return {};
    }
    const auto* value = DatumGetByteaPP(constant->constvalue);
    return bytea_payload(value);
}

[[nodiscard]] PlanState* coverage_retained_child(
        CustomScanState* node) noexcept {
    return node->custom_ps == NIL
        ? nullptr
        : static_cast<PlanState*>(linitial(node->custom_ps));
}

[[nodiscard]] const BioseqCacheColumnSchemaV2* coverage_contract_column(
        const BioseqCachePathDomainV2& contract,
        std::uint32_t attno) noexcept {
    for (std::size_t index = 0;
         index < contract.boundary_column_count; ++index) {
        if (contract.boundary_columns[index].attno == attno) {
            return &contract.boundary_columns[index];
        }
    }
    for (std::size_t index = 0;
         index < contract.lineage_column_count; ++index) {
        if (contract.lineage_columns[index].attno == attno) {
            return &contract.lineage_columns[index];
        }
    }
    return nullptr;
}

[[nodiscard]] bool append_unique_coverage_attno(
        std::vector<std::uint32_t>& attnos,
        const BioseqCachePathDomainV2& contract,
        std::uint32_t attno) {
    if (attno == 0 || coverage_contract_column(contract, attno) == nullptr) {
        return false;
    }
    if (std::find(attnos.begin(), attnos.end(), attno) == attnos.end()) {
        attnos.push_back(attno);
    }
    return true;
}

[[nodiscard]] bool coverage_column_semantic_digest(
        const BioseqCachePathDomainV2& contract,
        std::uint32_t attno,
        CacheDigest256* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    constexpr std::size_t schema_scalar_bytes =
        6U * sizeof(std::uint32_t) + 2U * sizeof(std::uint64_t);
    std::array<std::byte,
        sizeof(std::uint32_t)
            + 2U * (sizeof(std::uint32_t) + schema_scalar_bytes)> bytes{};
    std::size_t offset = 0;
    const auto append = [&bytes, &offset](
            const void* source, std::size_t size) {
        std::memcpy(bytes.data() + offset, source, size);
        offset += size;
    };
    append(&attno, sizeof(attno));
    const auto append_matching = [attno, &append](std::uint32_t role,
            const BioseqCacheColumnSchemaV2* columns,
            std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            if (columns[index].attno == attno) {
                const std::uint32_t normalized_struct_size =
                    sizeof(BioseqCacheColumnSchemaV2);
                append(&role, sizeof(role));
                append(
                    &normalized_struct_size,
                    sizeof(normalized_struct_size));
                append(&columns[index].attno, sizeof(columns[index].attno));
                append(
                    &columns[index].semantic_id_high,
                    sizeof(columns[index].semantic_id_high));
                append(
                    &columns[index].semantic_id_low,
                    sizeof(columns[index].semantic_id_low));
                append(
                    &columns[index].type_oid,
                    sizeof(columns[index].type_oid));
                append(&columns[index].typmod, sizeof(columns[index].typmod));
                append(
                    &columns[index].collation_oid,
                    sizeof(columns[index].collation_oid));
                append(&columns[index].flags, sizeof(columns[index].flags));
                return true;
            }
        }
        return false;
    };
    const bool boundary = append_matching(
        1U, contract.boundary_columns, contract.boundary_column_count);
    const bool lineage = append_matching(
        2U, contract.lineage_columns, contract.lineage_column_count);
    if (!boundary && !lineage) {
        return false;
    }
    PostgreSqlColumnChecksumPolicy policy;
    const auto digest = policy.sha256(std::span<const std::byte>{
        bytes.data(), offset});
    if (!digest.has_value()) {
        return false;
    }
    *output = *digest;
    return true;
}

/** Hash explicit ABI scalars only; provider struct padding is never semantic. */
[[nodiscard]] CoverageSemanticFingerprintsV1
coverage_semantic_fingerprints(
        const BioseqCachePathDomainV2& contract) noexcept {
    CoverageSemanticFingerprintsV1 result{};
    try {
        std::vector<std::byte> schema_bytes;
        std::vector<std::byte> lineage_bytes;
        std::vector<std::byte> mapping_bytes;
        schema_bytes.reserve(8192);
        lineage_bytes.reserve(8192);
        mapping_bytes.reserve(4096);
        const auto append = [](std::vector<std::byte>& output,
                const void* source, std::size_t size) {
            if (source == nullptr && size != 0) {
                return false;
            }
            const std::size_t offset = output.size();
            output.resize(offset + size);
            if (size != 0) {
                std::memcpy(output.data() + offset, source, size);
            }
            return true;
        };
        const auto append_column = [&append](
                std::vector<std::byte>& output,
                std::uint32_t role, std::uint32_t ordinal,
                const BioseqCacheColumnSchemaV2& column) {
            const std::uint32_t normalized_struct_size =
                sizeof(BioseqCacheColumnSchemaV2);
            return append(output, &role, sizeof(role))
                && append(output, &ordinal, sizeof(ordinal))
                && append(output, &normalized_struct_size,
                    sizeof(normalized_struct_size))
                && append(output, &column.attno, sizeof(column.attno))
                && append(output, &column.semantic_id_high,
                    sizeof(column.semantic_id_high))
                && append(output, &column.semantic_id_low,
                    sizeof(column.semantic_id_low))
                && append(output, &column.type_oid, sizeof(column.type_oid))
                && append(output, &column.typmod, sizeof(column.typmod))
                && append(output, &column.collation_oid,
                    sizeof(column.collation_oid))
                && append(output, &column.flags, sizeof(column.flags));
        };
        const std::uint32_t format = 1;
        const std::uint32_t boundary_count =
            contract.boundary_column_count;
        const std::uint32_t lineage_count =
            contract.lineage_column_count;
        if (!append(schema_bytes, &format, sizeof(format))
            || !append(schema_bytes, &contract.compatibility_family_id,
                sizeof(contract.compatibility_family_id))
            || !append(schema_bytes, &contract.capability_mask,
                sizeof(contract.capability_mask))
            || !append(schema_bytes, &boundary_count,
                sizeof(boundary_count))) {
            return result;
        }
        for (std::uint32_t index = 0; index < boundary_count; ++index) {
            if (!append_column(
                    schema_bytes, 1U, index,
                    contract.boundary_columns[index])) {
                return result;
            }
        }
        if (!append(schema_bytes, &lineage_count, sizeof(lineage_count))) {
            return result;
        }
        for (std::uint32_t index = 0; index < lineage_count; ++index) {
            if (!append_column(
                    schema_bytes, 2U, index,
                    contract.lineage_columns[index])) {
                return result;
            }
        }

        const std::uint32_t required_boundary_count =
            contract.required_boundary_attno_count;
        const std::uint32_t required_lineage_count =
            contract.required_lineage_attno_count;
        if (!append(lineage_bytes, &format, sizeof(format))
            || !append(lineage_bytes, &contract.compatibility_family_id,
                sizeof(contract.compatibility_family_id))
            || !append(lineage_bytes, &contract.capability_mask,
                sizeof(contract.capability_mask))
            || !append(lineage_bytes, &required_boundary_count,
                sizeof(required_boundary_count))
            || !append(lineage_bytes, contract.required_boundary_attnos,
                sizeof(std::uint32_t) * required_boundary_count)
            || !append(lineage_bytes, &required_lineage_count,
                sizeof(required_lineage_count))
            || !append(lineage_bytes, contract.required_lineage_attnos,
                sizeof(std::uint32_t) * required_lineage_count)) {
            return result;
        }
        for (std::uint32_t index = 0; index < lineage_count; ++index) {
            if (!append_column(
                    lineage_bytes, 2U, index,
                    contract.lineage_columns[index])) {
                return result;
            }
        }

        std::vector<std::uint32_t> stored_attnos;
        stored_attnos.reserve(boundary_count + lineage_count);
        const auto append_attnos = [&stored_attnos, &contract](
                const std::uint32_t* values,
                std::uint32_t count) {
            for (std::uint32_t index = 0; index < count; ++index) {
                if (!append_unique_coverage_attno(
                        stored_attnos, contract, values[index])) {
                    return false;
                }
            }
            return true;
        };
        if (!append_attnos(
                contract.required_lineage_attnos, required_lineage_count)
            || !append_attnos(
                contract.required_boundary_attnos, required_boundary_count)) {
            return result;
        }
        for (std::uint32_t index = 0; index < lineage_count; ++index) {
            if (!append_unique_coverage_attno(
                    stored_attnos, contract,
                    contract.lineage_columns[index].attno)) {
                return result;
            }
        }
        for (std::uint32_t index = 0; index < boundary_count; ++index) {
            if (!append_unique_coverage_attno(
                    stored_attnos, contract,
                    contract.boundary_columns[index].attno)) {
                return result;
            }
        }
        const std::uint32_t occurrence_columns =
            kCoverageOccurrenceColumnCountV1;
        const std::uint32_t stored_count = stored_attnos.size();
        if (!append(mapping_bytes, &format, sizeof(format))
            || !append(mapping_bytes, &occurrence_columns,
                sizeof(occurrence_columns))
            || !append(mapping_bytes, &stored_count, sizeof(stored_count))) {
            return result;
        }
        for (const std::uint32_t attno : stored_attnos) {
            CacheDigest256 semantic{};
            if (!coverage_column_semantic_digest(
                    contract, attno, &semantic)
                || !append(mapping_bytes, &attno, sizeof(attno))
                || !append(mapping_bytes, semantic.bytes.data(),
                    semantic.bytes.size())) {
                return result;
            }
        }
        const auto schema = tagged_coverage_digest(
            0x53434831U, schema_bytes);
        const auto mapping = tagged_coverage_digest(
            0x4d415031U, mapping_bytes);
        const auto lineage = tagged_coverage_digest(
            0x4c494e31U, lineage_bytes);
        if (!schema.has_value() || !mapping.has_value()
            || !lineage.has_value()) {
            return result;
        }
        result.schema = *schema;
        result.mapping = *mapping;
        result.lineage = *lineage;
        result.valid = true;
    } catch (...) {
        result = CoverageSemanticFingerprintsV1{};
    }
    return result;
}

/** Build the deterministic provider boundary/lineage + occurrence schema. */
[[nodiscard]] bool build_coverage_stored_recipe(
        const BioseqCachePathDomainV2& contract,
        CoverageStoredRecipeV1* output) {
    if (output == nullptr
        || !reuse::validate_path_domain(
            contract.provider_id, &contract).has_value()) {
        return false;
    }
    std::vector<std::uint32_t> attnos;
    attnos.reserve(
        contract.boundary_column_count + contract.lineage_column_count);
    const auto append_list = [&attnos, &contract](
            const std::uint32_t* values,
            std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            if (!append_unique_coverage_attno(
                    attnos, contract, values[index])) {
                return false;
            }
        }
        return true;
    };
    if (!append_list(
            contract.required_lineage_attnos,
            contract.required_lineage_attno_count)
        || !append_list(
            contract.required_boundary_attnos,
            contract.required_boundary_attno_count)) {
        return false;
    }
    const std::size_t column_count =
        kCoverageOccurrenceColumnCountV1 + attnos.size();
    if (column_count == kCoverageOccurrenceColumnCountV1
        || column_count > static_cast<std::size_t>(MaxTupleAttributeNumber)) {
        return false;
    }

    TupleDesc descriptor = CreateTemplateTupleDesc(
        static_cast<int>(column_count));
    auto* columns = static_cast<CoverageStoredColumnV1*>(palloc0(
        sizeof(CoverageStoredColumnV1) * column_count));
    struct OccurrenceColumnSpecV1 {
        CoverageStoredColumnKindV1 kind;
        const char* name;
        Oid type_oid;
    };
    static constexpr std::array<OccurrenceColumnSpecV1,
        kCoverageOccurrenceColumnCountV1> occurrence_columns{{
        {CoverageStoredColumnKindV1::ResultOrdinal,
            "cache_result_ordinal", INT8OID},
        {CoverageStoredColumnKindV1::RteOrdinal,
            "cache_rte_ordinal", INT4OID},
        {CoverageStoredColumnKindV1::LogicalRelationOid,
            "cache_logical_relation_oid", OIDOID},
        {CoverageStoredColumnKindV1::LeafRelationOid,
            "cache_leaf_relation_oid", OIDOID},
        {CoverageStoredColumnKindV1::IdentityKind,
            "cache_identity_kind", INT4OID},
        {CoverageStoredColumnKindV1::RowIdentity,
            "cache_row_identity", INT8OID},
        {CoverageStoredColumnKindV1::OccurrenceHigh,
            "cache_occurrence_high", INT8OID},
        {CoverageStoredColumnKindV1::OccurrenceLow,
            "cache_occurrence_low", INT8OID},
        {CoverageStoredColumnKindV1::CapabilityMask,
            "cache_capability_mask", INT8OID},
    }};
    for (std::size_t index = 0;
         index < occurrence_columns.size(); ++index) {
        const auto& spec = occurrence_columns[index];
        TupleDescInitEntry(
            descriptor,
            static_cast<AttrNumber>(index + 1U),
            spec.name,
            spec.type_oid,
            -1,
            0);
        columns[index] = CoverageStoredColumnV1{
            .kind = spec.kind,
            .provider_attno = 0,
        };
    }
    static constexpr char hex[] = "0123456789abcdef";
    for (std::size_t index = 0; index < attnos.size(); ++index) {
        const std::uint32_t attno = attnos[index];
        const BioseqCacheColumnSchemaV2* schema =
            coverage_contract_column(contract, attno);
        CacheDigest256 semantic_digest{};
        if (schema == nullptr
            || !coverage_column_semantic_digest(
                contract, attno, &semantic_digest)) {
            FreeTupleDesc(descriptor);
            pfree(columns);
            return false;
        }
        char name[NAMEDATALEN]{};
        const int prefix = snprintf(
            name, sizeof(name), "provider_%u_", attno);
        if (prefix <= 0 || prefix >= static_cast<int>(sizeof(name))) {
            FreeTupleDesc(descriptor);
            pfree(columns);
            return false;
        }
        std::size_t name_offset = static_cast<std::size_t>(prefix);
        for (std::size_t byte_index = 0;
             byte_index < 24U
                && name_offset + 2U < sizeof(name);
             ++byte_index) {
            const std::uint8_t value = semantic_digest.bytes[byte_index];
            name[name_offset++] = hex[value >> 4U];
            name[name_offset++] = hex[value & 0x0fU];
        }
        name[name_offset] = '\0';
        const std::size_t physical_index =
            kCoverageOccurrenceColumnCountV1 + index;
        TupleDescInitEntry(
            descriptor,
            static_cast<AttrNumber>(physical_index + 1U),
            name,
            schema->type_oid,
            schema->typmod,
            0);
        TupleDescInitEntryCollation(
            descriptor,
            static_cast<AttrNumber>(physical_index + 1U),
            schema->collation_oid);
        columns[physical_index] = CoverageStoredColumnV1{
            .kind = CoverageStoredColumnKindV1::ProviderAttribute,
            .provider_attno = attno,
        };
    }
    output->descriptor = descriptor;
    output->columns = columns;
    output->column_count = static_cast<std::uint32_t>(column_count);
    return true;
}

void destroy_coverage_stored_recipe(
        CoverageStoredRecipeV1& recipe) noexcept {
    if (recipe.descriptor != nullptr) {
        FreeTupleDesc(recipe.descriptor);
        recipe.descriptor = nullptr;
    }
    if (recipe.columns != nullptr) {
        pfree(recipe.columns);
        recipe.columns = nullptr;
    }
    recipe.column_count = 0;
}

void destroy_coverage_builder(ReusableCoverageState& state) noexcept {
    executor::destroy_context_owned(state.column_builder);
    executor::destroy_context_owned(state.column_sink);
    executor::destroy_context_owned(state.column_checksum_policy);
    if (state.send_functions != nullptr) {
        pfree(state.send_functions);
        state.send_functions = nullptr;
    }
    if (state.stored_descriptor != nullptr) {
        FreeTupleDesc(state.stored_descriptor);
        state.stored_descriptor = nullptr;
    }
    if (state.stored_columns != nullptr) {
        pfree(state.stored_columns);
        state.stored_columns = nullptr;
    }
    state.stored_column_count = 0;
}

void discard_coverage_blocks(ReusableCoverageState& state) noexcept {
    for (std::uint32_t index = 0;
         index < state.stored_token_count;
         ++index) {
        if (state.stored_tokens[index]) {
            static_cast<void>(storage::discard_unpublished_block(
                state.stored_tokens[index]));
            state.stored_tokens[index] = {};
        }
    }
    state.stored_token_count = 0;
}

void release_runtime_reusable_reference(
        ReusableReferenceTokenV1& token) noexcept {
    const ReusableReferenceTokenV1 owned = token;
    if (token.active && g_reusable_registry) {
        static_cast<void>(release_reusable_reference(
            g_reusable_registry,
            token,
            reusable_partition_callbacks()));
    }
    if (owned.active && owned.kind == ReusableReferenceKindV1::Waiter
        && owned.reference) {
        const storage::RegisterManifestWaiterResult waiter =
            storage::current_manifest_waiter(owned.reference.id);
        if (waiter) {
            static_cast<void>(
                storage::unregister_manifest_waiter(waiter.token));
        }
    }
}

void release_coverage_source(CoverageRuntimeSourceV1& source) noexcept {
    MemoryContext source_preflight_context =
        source.source_preflight_context;
    source.source_preflight_context = nullptr;
    if (source.manifest_pin_owned) {
        static_cast<void>(storage::unpin_manifest(source.manifest_pin));
        source.manifest_pin = {};
        source.manifest_pin_owned = false;
    }
    if (source.registry_reference_owned) {
        release_runtime_reusable_reference(source.registry_reference);
        source.registry_reference_owned = false;
    }
    if (source.replay_store != nullptr) {
        tuplestore_end(source.replay_store);
        source.replay_store = nullptr;
    }
    if (source.replay_slot != nullptr) {
        ExecDropSingleTupleTableSlot(source.replay_slot);
        source.replay_slot = nullptr;
    }
    if (source.stored_descriptor != nullptr) {
        FreeTupleDesc(source.stored_descriptor);
        source.stored_descriptor = nullptr;
    }
    if (source.stored_columns != nullptr) {
        pfree(source.stored_columns);
        source.stored_columns = nullptr;
    }
    if (source.boundary_map != nullptr) {
        pfree(source.boundary_map);
        source.boundary_map = nullptr;
    }
    if (source.lineage_map != nullptr) {
        pfree(source.lineage_map);
        source.lineage_map = nullptr;
    }
    if (source.selection_bitmap != nullptr) {
        pfree(source.selection_bitmap);
        source.selection_bitmap = nullptr;
    }
    bms_free(source.predicate_attnos);
    source.predicate_attnos = nullptr;
    source.stored_column_count = 0;
    source.boundary_map_count = 0;
    source.lineage_map_count = 0;
    source.provider_private_nodes = NIL;
    source.generation_key_node = nullptr;
    source.descriptor_node = nullptr;
    source.completed_snapshot = {};
    source.row_count = 0;
    source.selected_row_count = 0;
    source.replay_row_index = 0;
    source.selection_bitmap_bytes = 0;
    source.waited = false;
    source.ttl_entitlement_consumed = false;
    source.valid = false;
    if (source_preflight_context != nullptr) {
        if (CurrentMemoryContext == source_preflight_context) {
            MemoryContextSwitchTo(
                MemoryContextGetParent(source_preflight_context));
        }
        MemoryContextDelete(source_preflight_context);
    }
}

void abort_coverage_capture(ReusableCoverageState& state) noexcept {
    if (state.current_producer_owned && !state.producer_completed) {
        fail_current_reusable_generation(
            state.current_reference, CacheFailureCode::ProducerExited);
    }
    state.current_producer_owned = false;
    if (state.manifest_handle && !state.producer_completed) {
        static_cast<void>(storage::retire_manifest(state.manifest_handle));
        state.manifest_handle = {};
    }
    discard_coverage_blocks(state);
    destroy_coverage_builder(state);
    state.capture_failed = true;
}

void fail_coverage_producer(ReusableCoverageState& state) noexcept {
    abort_coverage_capture(state);
    state.runtime_role = CoverageRuntimeRoleV1::Bypass;
}

void coverage_state_cleanup(void* opaque) noexcept {
    auto* state = static_cast<ReusableCoverageState*>(opaque);
    if (state == nullptr) {
        return;
    }
    unlink_backend_query_cleanup(state->backend_cleanup);
    if (state->cleanup_done) {
        return;
    }
    state->cleanup_done = true;
    if (state->current_manifest_pin_owned) {
        static_cast<void>(storage::unpin_manifest(
            state->current_manifest_pin));
        state->current_manifest_pin = {};
        state->current_manifest_pin_owned = false;
    }
    if (state->current_reference_owned) {
        release_runtime_reusable_reference(state->current_reference_token);
        state->current_reference_owned = false;
    }
    for (CoverageRuntimeSourceV1& source : state->sources) {
        release_coverage_source(source);
    }
    bms_free(state->requested_predicate_attnos);
    state->requested_predicate_attnos = nullptr;
    if (state->occurrence_owners != nullptr) {
        hash_destroy(state->occurrence_owners);
        state->occurrence_owners = nullptr;
    }
    state->occurrence_owner_count = 0;
    state->selection_bitmap_bytes_admitted = 0;
    if (state->current_replay_store != nullptr) {
        tuplestore_end(state->current_replay_store);
        state->current_replay_store = nullptr;
    }
    if (state->current_replay_slot != nullptr) {
        ExecDropSingleTupleTableSlot(state->current_replay_slot);
        state->current_replay_slot = nullptr;
    }
    if (state->producer_output_store != nullptr) {
        tuplestore_end(state->producer_output_store);
        state->producer_output_store = nullptr;
    }
    if (state->producer_output_slot != nullptr) {
        ExecDropSingleTupleTableSlot(state->producer_output_slot);
        state->producer_output_slot = nullptr;
    }
    state->producer_spool_rows = 0;
    state->producer_spool_bytes = 0;
    if (state->current_producer_owned && !state->producer_completed) {
        fail_coverage_producer(*state);
    }
    discard_coverage_blocks(*state);
    destroy_coverage_builder(*state);
    provider_registry::release_provider_lease_v2(
        &state->provider_lease);
    state->provider = nullptr;
}

[[nodiscard]] bool snapshot_current_reusable_generation(
        const ReusableGenerationReferenceV1& reference,
        ReusableGenerationSnapshotV1* output) noexcept {
    if (!reference || output == nullptr || !g_reusable_registry
        || g_reusable_membership_lock == nullptr) {
        return false;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool accepted = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
        membership_owned = true;
        const auto snapshot = wait_reusable_generation(
            g_reusable_registry,
            reference,
            reusable_partition_callbacks());
        if (snapshot.has_value()) {
            *output = *snapshot;
            accepted = true;
        }
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = false;
    }
    PG_END_TRY();
    return accepted;
}

[[nodiscard]] TtlDecision older_reusable_ttl_decision(
        const ReusableGenerationSnapshotV1& snapshot,
        std::uint64_t consumer_ttl_ms,
        bool registered_inflight_waiter,
        bool entitlement_already_consumed) noexcept {
    if (snapshot.state != CacheEntryState::Complete
        || snapshot.completed_monotonic_ns == 0) {
        return {};
    }
    return completed_ttl_decision(
        CacheTimestamps{
            .started_ns = snapshot.started_monotonic_ns,
            .completed_ns = snapshot.completed_monotonic_ns,
        },
        snapshot.metadata.producer_ttl_ms,
        consumer_ttl_ms,
        reusable_monotonic_now_ns(),
        registered_inflight_waiter,
        entitlement_already_consumed);
}

[[nodiscard]] TtlDecision current_reusable_ttl_decision(
        const ReusableCoverageState& state,
        const ReusableGenerationSnapshotV1& snapshot) noexcept {
    return older_reusable_ttl_decision(
        snapshot,
        state.producer_ttl_ms,
        state.current_waited,
        state.current_ttl_entitlement_consumed);
}

[[nodiscard]] bool older_reusable_identity_matches(
    const ReusableCoverageState& state,
    const CoverageRuntimeSourceV1& source,
    const CoverageCurrentPlanViewV1& current_plan,
    const ReusableGenerationSnapshotV1& snapshot) noexcept;

/** Acquire one ABA-bound reader or waiter while membership cannot change. */
[[nodiscard]] bool acquire_older_reusable_reference(
        const ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        std::span<const std::uint64_t> locally_owned_claim_sequences,
        const VisibilityTicket& visibility,
        std::uint64_t consumer_ttl_ms) noexcept {
    if (!source.reference || !g_reusable_registry
        || g_reusable_membership_lock == nullptr
        || !provider_visibility_ticket_still_bound(visibility)) {
        return false;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool accepted = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
        membership_owned = true;
        const auto observed = wait_reusable_generation(
            g_reusable_registry,
            source.reference,
            reusable_partition_callbacks());
        if (observed.has_value()
            && observed->reference == source.reference) {
            ReusableReferenceKindV1 kind =
                ReusableReferenceKindV1::None;
            if (observed->state == CacheEntryState::Complete
                && observed->manifest
                && older_reusable_ttl_decision(
                    *observed, consumer_ttl_ms, false, false).readable) {
                kind = ReusableReferenceKindV1::ReaderPin;
            } else if ((observed->state == CacheEntryState::Claimed
                        || observed->state == CacheEntryState::Building)
                       && observed->producer
                            != current_reusable_producer_identity()) {
                if (reuse::claim_order_allows_wait(
                        source.reference.id.claim_sequence,
                        locally_owned_claim_sequences)) {
                    kind = ReusableReferenceKindV1::Waiter;
                }
            }
            if (kind != ReusableReferenceKindV1::None) {
                auto pinned = pin_reusable_generation(
                    g_reusable_registry,
                    source.reference,
                    kind,
                    current_reusable_producer_identity(),
                    reusable_partition_callbacks());
                if (pinned.has_value()) {
                    const auto rebound = wait_reusable_generation(
                        g_reusable_registry,
                        source.reference,
                        reusable_partition_callbacks());
                    const bool state_matches = rebound.has_value()
                        && rebound->reference == source.reference
                        && ((kind == ReusableReferenceKindV1::ReaderPin
                                && rebound->state
                                    == CacheEntryState::Complete
                                && rebound->manifest
                                && older_reusable_ttl_decision(
                                    *rebound,
                                    consumer_ttl_ms,
                                    false,
                                    false).readable)
                            || (kind == ReusableReferenceKindV1::Waiter
                                && (rebound->state
                                        == CacheEntryState::Claimed
                                    || rebound->state
                                        == CacheEntryState::Building)
                                && rebound->producer
                                    != current_reusable_producer_identity()
                                && reuse::claim_order_allows_wait(
                                    rebound->reference.id.claim_sequence,
                                    locally_owned_claim_sequences)));
                    if (state_matches
                        && provider_visibility_ticket_still_bound(visibility)) {
                        source.registry_reference = *pinned;
                        source.registry_reference_owned = true;
                        source.completed_snapshot = *rebound;
                        source.waited =
                            kind == ReusableReferenceKindV1::Waiter;
                        if (source.waited && g_shared != nullptr) {
                            pg_atomic_fetch_add_u64(
                                &g_shared->stats.inflight_waits, 1);
                        }
                        accepted = true;
                    } else {
                        ReusableReferenceTokenV1 rejected = *pinned;
                        static_cast<void>(release_reusable_reference(
                            g_reusable_registry,
                            rejected,
                            reusable_partition_callbacks()));
                    }
                }
            }
        }
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = false;
    }
    PG_END_TRY();
    if (accepted && !source.waited) {
        const VisibilityTicket refreshed = mint_visibility_ticket(
            source.completed_snapshot.metadata.observability,
            VisibilityTicketPurposeV1::Continuation);
        const bool semantic_identity_matches =
            older_reusable_identity_matches(
                state, source, current_plan, source.completed_snapshot);
        const bool ttl_readable = older_reusable_ttl_decision(
            source.completed_snapshot,
            consumer_ttl_ms,
            false,
            false).readable;
        const dependency_runtime::DependencyRuntimeResultV1 validated =
            refreshed.valid
            ? validate_reusable_generation_dependencies_locked(
                source.completed_snapshot,
                refreshed.completion_count)
            : dependency_runtime::DependencyRuntimeResultV1{
                .error = dependency_runtime::DependencyRuntimeErrorV1::
                    ReferenceValidationFailed,
                .fail_open = true,
            };
        if (!semantic_identity_matches || !ttl_readable
            || !refreshed.valid || !validated) {
            release_runtime_reusable_reference(source.registry_reference);
            source.registry_reference_owned = false;
            source.registry_reference = {};
            source.completed_snapshot = {};
            accepted = false;
        }
    }
    return accepted;
}

[[nodiscard]] bool reusable_bytes_equal(
        ReusableSharedByteRangeV1 shared_range,
        std::span<const std::byte> expected) noexcept {
    const std::span<const std::byte> shared = reusable_shared_bytes(
        g_reusable_registry, shared_range);
    return !expected.empty() && shared.size() == expected.size()
        && std::memcmp(shared.data(), expected.data(), expected.size()) == 0;
}

[[nodiscard]] bool decode_reusable_reference_node(
        const void* node,
        ReusableGenerationReferenceV1* output) noexcept {
    const std::span<const std::byte> bytes = binary_node_payload(node);
    if (output == nullptr
        || bytes.size() != sizeof(ReusableGenerationReferenceV1)) {
        return false;
    }
    ReusableGenerationReferenceV1 reference{};
    std::memcpy(&reference, bytes.data(), sizeof(reference));
    if (!reference || reference.reserved != 0) {
        return false;
    }
    *output = reference;
    return true;
}

[[nodiscard]] ProviderContractView coverage_provider_contract(
        const BioseqCachePathDomainV2& domain) noexcept {
    return ProviderContractView{
        .boundary = {
            .columns = domain.boundary_columns,
            .column_count = domain.boundary_column_count,
            .required_attnos = domain.required_boundary_attnos,
            .required_attno_count = domain.required_boundary_attno_count,
        },
        .lineage = {
            .columns = domain.lineage_columns,
            .column_count = domain.lineage_column_count,
            .required_attnos = domain.required_lineage_attnos,
            .required_attno_count = domain.required_lineage_attno_count,
        },
        .capability_mask = domain.capability_mask,
    };
}

[[nodiscard]] ProviderContractView coverage_provider_contract(
        const BioseqCacheReusableSourceV2& source) noexcept {
    return ProviderContractView{
        .boundary = {
            .columns = source.boundary_columns,
            .column_count = source.boundary_column_count,
        },
        .lineage = {
            .columns = source.lineage_columns,
            .column_count = source.lineage_column_count,
        },
        .capability_mask = source.capability_mask,
    };
}

[[nodiscard]] bool tuple_descriptor_matches_mapping_schema(
        TupleDesc descriptor,
        const BioseqCacheColumnSchemaV2* columns,
        std::size_t column_count) noexcept {
    if (descriptor == nullptr || column_count > kMaxProviderBoundaryColumns
        || (column_count != 0 && columns == nullptr)) {
        return false;
    }
    for (std::size_t index = 0; index < column_count; ++index) {
        const BioseqCacheColumnSchemaV2& column = columns[index];
        if (column.attno == 0
            || column.attno > static_cast<std::uint32_t>(descriptor->natts)) {
            return false;
        }
        Form_pg_attribute attribute = TupleDescAttr(
            descriptor, static_cast<int>(column.attno - 1U));
        if (attribute->attisdropped || attribute->atttypid != column.type_oid
            || attribute->atttypmod != column.typmod
            || attribute->attcollation != column.collation_oid) {
            return false;
        }
    }
    return true;
}

/** Decode the frozen mapping List and recompute all four fingerprints. */
[[nodiscard]] bool decode_and_validate_runtime_source_mapping(
        CoverageRuntimeSourceV1& source,
        const BioseqCachePathDomainV2& consumer,
        const void* mapping_node,
        const void* provider_private_nodes) {
    DecodedSourceMappingPlanV1 decoded{};
    if (!validate_source_mapping_plan_node(
            mapping_node,
            consumer,
            source.contract.source,
            consumer.boundary_columns,
            consumer.boundary_column_count,
            consumer.lineage_columns,
            consumer.lineage_column_count,
            consumer.provider_private_nodes,
            provider_private_nodes,
            &decoded)) {
        return false;
    }
    source.boundary_map = decoded.boundary_map;
    source.lineage_map = decoded.lineage_map;
    source.boundary_map_count = decoded.boundary_map_count;
    source.lineage_map_count = decoded.lineage_map_count;
    decoded.boundary_map = nullptr;
    decoded.lineage_map = nullptr;
    source.provider_private_nodes = const_cast<List*>(
        static_cast<const List*>(provider_private_nodes));
    return true;
}

/** Reprove immutable semantics and collision bytes for one completed source. */
[[nodiscard]] bool older_reusable_identity_matches(
        const ReusableCoverageState& state,
        const CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        const ReusableGenerationSnapshotV1& snapshot) noexcept {
    if (!source.reference || snapshot.reference != source.reference
        || state.provider == nullptr
        || source.contract.domain.provider_id != state.provider->provider_id
        || source.contract.domain.compatibility_family_id
            != state.current_header.compatibility_family_id
        || (source.contract.domain.capability_mask
                & state.current_header.capability_mask)
            != state.current_header.capability_mask
        || reuse::provider_security_digest(source.contract.domain)
            != state.current_header.coverage_directory_key
                .security_namespace.provider_security_digest
        || snapshot.coverage_directory_key
            != state.current_header.coverage_directory_key) {
        return false;
    }
    const CoverageSemanticFingerprintsV1 fingerprints =
        coverage_semantic_fingerprints(source.contract.domain);
    const ReusableGenerationMetadataV1& metadata = snapshot.metadata;
    return fingerprints.valid
        && metadata.provider_id == state.provider->provider_id
        && metadata.provider_abi_version == state.provider->abi_version
        && metadata.provider_domain_abi_version
            == kCoverageDescriptorFormatV1
        && metadata.dependency_count == 1
        && metadata.compatibility_family_id
            == source.contract.domain.compatibility_family_id
        && metadata.capability_mask
            == source.contract.domain.capability_mask
        && metadata.schema_fingerprint == fingerprints.schema
        && metadata.mapping_fingerprint == fingerprints.mapping
        && metadata.lineage_fingerprint == fingerprints.lineage
        && metadata.observability == WriteObservability::ProviderComplete
        && reusable_bytes_equal(
            snapshot.canonical_domain_key,
            current_plan.canonical_domain_key)
        && reusable_bytes_equal(
            snapshot.canonical_generation_key,
            binary_node_payload(source.generation_key_node))
        && reusable_bytes_equal(
            snapshot.coverage_descriptor,
            binary_node_payload(source.descriptor_node));
}

/** In-flight identity is proved by the stored canonical vector and guard. */
[[nodiscard]] bool older_reusable_inflight_identity_matches(
        const ReusableCoverageState& state,
        const CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        const ReusableGenerationSnapshotV1& snapshot) noexcept {
    const VisibilityTicket refreshed = mint_visibility_ticket(
        snapshot.metadata.observability,
        VisibilityTicketPurposeV1::Continuation);
    const CoverageDependencyTicketV1 dependency = refreshed.valid
        ? coverage_dependency_ticket(
            source.contract.domain,
            state.provider,
            refreshed.completion_count)
        : CoverageDependencyTicketV1{};
    return older_reusable_identity_matches(
            state, source, current_plan, snapshot)
        && refreshed.valid
        && dependency.valid
        && snapshot.metadata.dependency_count == dependency.dependency_count
        && snapshot.metadata.physical_identity == dependency.physical_identity
        && validate_reusable_generation_dependencies_locked(
            snapshot, refreshed.completion_count);
}

/** A completed older source additionally requires one published manifest. */
[[nodiscard]] bool older_reusable_snapshot_matches(
        const ReusableCoverageState& state,
        const CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        const ReusableGenerationSnapshotV1& snapshot) noexcept {
    return snapshot.state == CacheEntryState::Complete
        && static_cast<bool>(snapshot.manifest)
        && older_reusable_identity_matches(
            state, source, current_plan, snapshot);
}

/** Wait only on an older foreign token and reprove it after every timed wake. */
[[nodiscard]] bool wait_for_older_reusable_generation(
        const ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        std::span<const std::uint64_t> locally_owned_claim_sequences,
        const VisibilityTicket& visibility,
        std::uint64_t consumer_ttl_ms) {
    if (!source.registry_reference_owned
        || source.registry_reference.kind != ReusableReferenceKindV1::Waiter
        || source.reference.id.claim_sequence == 0
        || !provider_visibility_ticket_still_bound(visibility)
        || !reuse::claim_order_allows_wait(
            source.reference.id.claim_sequence,
            locally_owned_claim_sequences)) {
        return false;
    }
    ConditionVariable* changed = reusable_generation_changed(source.reference);
    const ReusableProducerIdentityV1 expected_producer =
        source.completed_snapshot.producer;
    if (changed == nullptr || !expected_producer.valid()
        || expected_producer == current_reusable_producer_identity()
        || !older_reusable_inflight_identity_matches(
            state, source, current_plan, source.completed_snapshot)) {
        return false;
    }
    volatile bool accepted = false;
    ConditionVariablePrepareToSleep(changed);
    PG_TRY();
    {
        maybe_pause(TestPausePoint::AfterWaiterRegistered);
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            if (!provider_visibility_ticket_still_bound(visibility)
                || expected_producer
                    == current_reusable_producer_identity()
                || !reuse::claim_order_allows_wait(
                    source.reference.id.claim_sequence,
                    locally_owned_claim_sequences)) {
                break;
            }
            ReusableGenerationSnapshotV1 snapshot{};
            if (!snapshot_current_reusable_generation(
                    source.reference, &snapshot)
                || snapshot.reference != source.reference
                || snapshot.producer != expected_producer) {
                break;
            }
            if (snapshot.state == CacheEntryState::Complete) {
                const VisibilityTicket refreshed = mint_visibility_ticket(
                    snapshot.metadata.observability,
                    VisibilityTicketPurposeV1::Continuation);
                if (older_reusable_identity_matches(
                        state, source, current_plan, snapshot)
                    && snapshot.manifest
                    && older_reusable_ttl_decision(
                        snapshot,
                        consumer_ttl_ms,
                        true,
                        source.ttl_entitlement_consumed).readable
                    && provider_visibility_ticket_still_bound(visibility)
                    && refreshed.valid
                    && validate_reusable_generation_dependencies_locked(
                        snapshot, refreshed.completion_count)) {
                    source.completed_snapshot = snapshot;
                    accepted = true;
                }
                break;
            }
            if (snapshot.state == CacheEntryState::Failed
                || snapshot.state == CacheEntryState::Invalidated
                || snapshot.state == CacheEntryState::Evicting
                || (snapshot.state != CacheEntryState::Claimed
                    && snapshot.state != CacheEntryState::Building)) {
                break;
            }
            if (!older_reusable_inflight_identity_matches(
                    state, source, current_plan, snapshot)) {
                break;
            }
            if (!reusable_producer_alive(nullptr, expected_producer)) {
                fail_current_reusable_generation(
                    source.reference, CacheFailureCode::ProducerExited);
                continue;
            }
            static_cast<void>(ConditionVariableTimedSleep(
                changed, 1000L, PG_WAIT_EXTENSION));
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        PG_RE_THROW();
    }
    PG_END_TRY();
    return accepted;
}

/** Completed identity excludes legacy liveness fields proved by the vector. */
[[nodiscard]] bool current_reusable_snapshot_matches(
        const ReusableGenerationSnapshotV1& snapshot,
        const CoverageCurrentPlanViewV1& plan,
        const ReusableGenerationMetadataV1& expected_metadata) noexcept {
    const ReusableGenerationMetadataV1& actual = snapshot.metadata;
    return snapshot.reference
        && snapshot.reference.id.digest == plan.header.generation_digest
        && snapshot.coverage_directory_key
            == plan.header.coverage_directory_key
        && actual.provider_id == expected_metadata.provider_id
        && actual.provider_abi_version
            == expected_metadata.provider_abi_version
        && actual.provider_domain_abi_version
            == expected_metadata.provider_domain_abi_version
        && actual.dependency_count == expected_metadata.dependency_count
        && actual.compatibility_family_id
            == expected_metadata.compatibility_family_id
        && actual.capability_mask == expected_metadata.capability_mask
        && actual.schema_fingerprint == expected_metadata.schema_fingerprint
        && actual.mapping_fingerprint == expected_metadata.mapping_fingerprint
        && actual.lineage_fingerprint == expected_metadata.lineage_fingerprint
        && actual.observability == WriteObservability::ProviderComplete
        && reusable_bytes_equal(
            snapshot.canonical_domain_key, plan.canonical_domain_key)
        && reusable_bytes_equal(
            snapshot.canonical_generation_key,
            plan.canonical_generation_key)
        && reusable_bytes_equal(
            snapshot.coverage_descriptor, plan.coverage_descriptor);
}

/** Provisional generations retain their captured liveness fingerprint. */
[[nodiscard]] bool current_reusable_inflight_snapshot_matches(
        const ReusableGenerationSnapshotV1& snapshot,
        const CoverageCurrentPlanViewV1& plan,
        const ReusableGenerationMetadataV1& expected_metadata) noexcept {
    return current_reusable_snapshot_matches(snapshot, plan, expected_metadata)
        && snapshot.metadata.dependency_fingerprint
            == expected_metadata.dependency_fingerprint
        && snapshot.metadata.physical_identity
            == expected_metadata.physical_identity;
}

[[nodiscard]] bool current_dependency_ticket_matches(
        const ReusableCoverageState& state) noexcept {
    const CoverageDependencyTicketV1 ticket = coverage_dependency_ticket(
        state.current_contract.domain, state.provider);
    return ticket.valid
        && ticket.dependency_count != 0
        && ticket.observability == WriteObservability::ProviderComplete
        && ticket.fingerprint == state.dependency_fingerprint
        && ticket.physical_identity == state.dependency_physical_identity
        && pg_atomic_read_u32(&g_shared->coarse_transition_count) == 0;
}

/** Wake-driven, ABA-keyed wait; cancellation releases through query cleanup. */
[[nodiscard]] bool wait_for_current_reusable_generation(
        ReusableCoverageState& state,
        const CoverageCurrentPlanViewV1& plan,
        const ReusableGenerationMetadataV1& expected_metadata,
        const VisibilityTicket& visibility,
        ReusableGenerationSnapshotV1* completed) {
    ConditionVariable* changed = reusable_generation_changed(
        state.current_reference);
    if (changed == nullptr || completed == nullptr
        || !state.current_reference_owned
        || state.current_reference_token.kind
            != ReusableReferenceKindV1::Waiter) {
        return false;
    }
    const std::size_t owned_capacity = kRegistryCapacity
        + static_cast<std::size_t>(g_reusable_wait_slot_count);
    if (owned_capacity < kRegistryCapacity
        || owned_capacity > static_cast<std::size_t>(MaxAllocSize)
            / sizeof(std::uint64_t)) {
        return false;
    }
    auto* locally_owned = static_cast<std::uint64_t*>(palloc_extended(
        sizeof(std::uint64_t) * owned_capacity,
        MCXT_ALLOC_NO_OOM));
    std::size_t locally_owned_exact_count = 0;
    std::size_t locally_owned_count = 0;
    if (locally_owned == nullptr
        || !collect_backend_owned_claim_sequences(
            std::span<std::uint64_t>{locally_owned, owned_capacity},
            &locally_owned_exact_count,
            &locally_owned_count)) {
        if (locally_owned != nullptr) {
            pfree(locally_owned);
        }
        return false;
    }
    const std::span<const std::uint64_t> locally_owned_exact_claims{
        locally_owned, locally_owned_exact_count};
    const std::span<const std::uint64_t> locally_owned_reusable_claims{
        locally_owned + locally_owned_exact_count,
        locally_owned_count - locally_owned_exact_count};
    ReusableGenerationSnapshotV1 initial{};
    if (!snapshot_current_reusable_generation(
            state.current_reference, &initial)
        || initial.reference != state.current_reference
        || initial.producer == current_reusable_producer_identity()
        || !initial.producer.valid()
        || !current_reusable_snapshot_matches(
            initial, plan, expected_metadata)) {
        pfree(locally_owned);
        return false;
    }
    if (initial.state == CacheEntryState::Complete) {
        const VisibilityTicket refreshed = mint_visibility_ticket(
            initial.metadata.observability,
            VisibilityTicketPurposeV1::Continuation);
        const bool accepted = initial.manifest
            && current_reusable_ttl_decision(state, initial).readable
            && provider_visibility_ticket_still_bound(visibility)
            && refreshed.valid
            && validate_reusable_generation_dependencies_locked(
                initial, refreshed.completion_count);
        if (accepted) {
            *completed = initial;
        }
        pfree(locally_owned);
        return accepted;
    }
    if ((initial.state != CacheEntryState::Claimed
            && initial.state != CacheEntryState::Building)
        || !current_reusable_inflight_snapshot_matches(
            initial, plan, expected_metadata)
        || !current_dependency_ticket_matches(state)
        || !locally_owned_exact_claims.empty()
        || !reuse::claim_order_allows_wait(
            initial.reference.id.claim_sequence,
            locally_owned_reusable_claims)) {
        pfree(locally_owned);
        return false;
    }
    const ReusableProducerIdentityV1 expected_producer = initial.producer;
    volatile bool accepted = false;
    ConditionVariablePrepareToSleep(changed);
    PG_TRY();
    {
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            if (!provider_visibility_ticket_still_bound(visibility)
                || expected_producer
                    == current_reusable_producer_identity()
                || !locally_owned_exact_claims.empty()
                || !reuse::claim_order_allows_wait(
                    state.current_reference.id.claim_sequence,
                    locally_owned_reusable_claims)) {
                break;
            }
            ReusableGenerationSnapshotV1 snapshot{};
            if (!snapshot_current_reusable_generation(
                    state.current_reference, &snapshot)
                || snapshot.reference != state.current_reference
                || snapshot.producer != expected_producer
                || !current_reusable_snapshot_matches(
                    snapshot, plan, expected_metadata)) {
                break;
            }
            if (snapshot.state == CacheEntryState::Complete) {
                const VisibilityTicket refreshed = mint_visibility_ticket(
                    snapshot.metadata.observability,
                    VisibilityTicketPurposeV1::Continuation);
                if (snapshot.manifest
                    && current_reusable_ttl_decision(
                        state, snapshot).readable
                    && provider_visibility_ticket_still_bound(visibility)
                    && refreshed.valid
                    && validate_reusable_generation_dependencies_locked(
                        snapshot, refreshed.completion_count)) {
                    *completed = snapshot;
                    accepted = true;
                }
                break;
            }
            if (snapshot.state == CacheEntryState::Failed
                || snapshot.state == CacheEntryState::Invalidated
                || snapshot.state == CacheEntryState::Evicting
                || (snapshot.state != CacheEntryState::Claimed
                    && snapshot.state != CacheEntryState::Building)) {
                break;
            }
            if (!current_reusable_inflight_snapshot_matches(
                    snapshot, plan, expected_metadata)
                || !current_dependency_ticket_matches(state)) {
                break;
            }
            if (!reusable_producer_alive(nullptr, snapshot.producer)) {
                fail_current_reusable_generation(
                    state.current_reference,
                    CacheFailureCode::ProducerExited);
                continue;
            }
            static_cast<void>(ConditionVariableTimedSleep(
                changed, 1000L, PG_WAIT_EXTENSION));
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        pfree(locally_owned);
        PG_RE_THROW();
    }
    PG_END_TRY();
    pfree(locally_owned);
    return accepted;
}

[[nodiscard]] bool begin_coverage_capture(
        ReusableCoverageState& state) {
    try {
        CoverageStoredRecipeV1 recipe{};
        if (!build_coverage_stored_recipe(
                state.current_contract.domain, &recipe)) {
            return false;
        }
        state.stored_descriptor = recipe.descriptor;
        state.stored_columns = recipe.columns;
        state.stored_column_count = recipe.column_count;
        state.send_functions = static_cast<Oid*>(palloc0(
            sizeof(Oid) * state.stored_column_count));
        std::vector<ColumnSchema> schema = build_column_schema(
            state.stored_descriptor, state.send_functions);
        state.column_checksum_policy =
            executor::try_construct_context_owned<
                PostgreSqlColumnChecksumPolicy>(CurrentMemoryContext);
        state.column_sink =
            executor::try_construct_context_owned<
                storage::StorageColumnStoreBlockSink>(CurrentMemoryContext);
        if (state.column_checksum_policy == nullptr
            || state.column_sink == nullptr) {
            destroy_coverage_builder(state);
            return false;
        }
        state.column_builder =
            executor::try_construct_context_owned<
                PostgreSqlStreamingColumnStoreBuilder>(
            CurrentMemoryContext,
            current_column_store_identity(),
            schema,
            *state.column_checksum_policy,
            *state.column_sink);
        return state.column_builder != nullptr;
    } catch (...) {
        destroy_coverage_builder(state);
        return false;
    }
}

void release_coverage_output_spool(ReusableCoverageState& state) noexcept {
    if (state.producer_output_store != nullptr) {
        tuplestore_end(state.producer_output_store);
        state.producer_output_store = nullptr;
    }
    if (state.producer_output_slot != nullptr) {
        ExecDropSingleTupleTableSlot(state.producer_output_slot);
        state.producer_output_slot = nullptr;
    }
    state.producer_spool_rows = 0;
    state.producer_spool_bytes = 0;
}

/** Allocate the optional rescan spool inside a cache-only ERROR boundary. */
[[nodiscard]] bool begin_coverage_output_spool(
        ReusableCoverageState& state,
        TupleDesc raw_descriptor) {
    if (raw_descriptor == nullptr || state.producer_output_store != nullptr
        || state.producer_output_slot != nullptr) {
        return false;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool initialized = false;
    PG_TRY();
    {
        state.producer_output_store = tuplestore_begin_heap(
            true, false, work_mem);
        if (state.producer_output_store != nullptr) {
            state.producer_output_slot = MakeSingleTupleTableSlot(
                raw_descriptor, &TTSOpsMinimalTuple);
        }
        initialized = state.producer_output_store != nullptr
            && state.producer_output_slot != nullptr;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        initialized = false;
    }
    PG_END_TRY();
    if (!initialized) {
        release_coverage_output_spool(state);
        state.producer_spool_failed = true;
        return false;
    }
    state.producer_spool_failed = false;
    state.producer_spool_rows = 0;
    state.producer_spool_bytes = 0;
    return true;
}

[[nodiscard]] bool convert_runtime_waiter_to_reader(
        ReusableReferenceTokenV1& token) noexcept {
    if (!token.active
        || token.kind != ReusableReferenceKindV1::Waiter
        || g_reusable_membership_lock == nullptr) {
        return false;
    }
    ReusableRegistryErrorV1 result =
        ReusableRegistryErrorV1::InvalidState;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
        membership_owned = true;
        result = convert_reusable_waiter_to_reader(
            g_reusable_registry,
            token,
            reusable_partition_callbacks());
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        result = ReusableRegistryErrorV1::InvalidState;
    }
    PG_END_TRY();
    return result == ReusableRegistryErrorV1::None;
}

[[nodiscard]] bool convert_current_waiter_to_reader(
        ReusableCoverageState& state) noexcept {
    if (!state.current_reference_owned
        || !convert_runtime_waiter_to_reader(
            state.current_reference_token)) {
        return false;
    }
    state.current_ttl_entitlement_consumed = true;
    return true;
}

void release_current_coverage_consumer(
        ReusableCoverageState& state) noexcept {
    if (state.current_manifest_pin_owned) {
        static_cast<void>(storage::unpin_manifest(
            state.current_manifest_pin));
        state.current_manifest_pin = {};
        state.current_manifest_pin_owned = false;
    }
    if (state.current_reference_owned) {
        release_runtime_reusable_reference(state.current_reference_token);
        state.current_reference_owned = false;
    }
    if (state.current_replay_store != nullptr) {
        tuplestore_end(state.current_replay_store);
        state.current_replay_store = nullptr;
    }
    if (state.current_replay_slot != nullptr) {
        ExecDropSingleTupleTableSlot(state.current_replay_slot);
        state.current_replay_slot = nullptr;
    }
    state.current_waited = false;
    state.current_ttl_entitlement_consumed = false;
    destroy_coverage_builder(state);
    state.runtime_role = CoverageRuntimeRoleV1::Bypass;
}

[[nodiscard]] bool coverage_stored_row_valid(
        const CoverageStoredColumnV1* columns,
        std::uint32_t column_count,
        std::uint64_t required_capability_mask,
        TupleTableSlot* stored,
        std::uint64_t expected_ordinal,
        BioseqCacheOccurrenceV2* occurrence = nullptr) {
    if (columns == nullptr || stored == nullptr
        || column_count < kCoverageOccurrenceColumnCountV1) {
        return false;
    }
    slot_getallattrs(stored);
    const auto scalar = [stored](int attribute, Datum* value) {
        bool is_null = false;
        *value = slot_getattr(stored, attribute, &is_null);
        return !is_null;
    };
    Datum result_ordinal = 0;
    Datum rte_ordinal = 0;
    Datum logical_relation = 0;
    Datum leaf_relation = 0;
    Datum identity_kind = 0;
    Datum row_identity = 0;
    Datum occurrence_high = 0;
    Datum occurrence_low = 0;
    Datum capability_mask = 0;
    const bool valid = scalar(1, &result_ordinal)
        && scalar(2, &rte_ordinal)
        && scalar(3, &logical_relation)
        && scalar(4, &leaf_relation)
        && scalar(5, &identity_kind)
        && scalar(6, &row_identity)
        && scalar(7, &occurrence_high)
        && scalar(8, &occurrence_low)
        && scalar(9, &capability_mask)
        && static_cast<std::uint64_t>(DatumGetInt64(result_ordinal))
            == expected_ordinal
        && DatumGetInt32(rte_ordinal) > 0
        && OidIsValid(DatumGetObjectId(logical_relation))
        && OidIsValid(DatumGetObjectId(leaf_relation))
        && DatumGetInt32(identity_kind) > 0
        && (static_cast<std::uint64_t>(DatumGetInt64(capability_mask))
                & required_capability_mask)
            == required_capability_mask;
    if (valid && occurrence != nullptr) {
        *occurrence = BioseqCacheOccurrenceV2{
            .struct_size = sizeof(BioseqCacheOccurrenceV2),
            .rte_ordinal = static_cast<std::uint32_t>(
                DatumGetInt32(rte_ordinal)),
            .logical_relation_oid = static_cast<std::uint32_t>(
                DatumGetObjectId(logical_relation)),
            .leaf_relation_oid = static_cast<std::uint32_t>(
                DatumGetObjectId(leaf_relation)),
            .identity_kind = static_cast<std::uint32_t>(
                DatumGetInt32(identity_kind)),
            .reserved = 0,
            .row_identity = static_cast<std::uint64_t>(
                DatumGetInt64(row_identity)),
            .occurrence_high = static_cast<std::uint64_t>(
                DatumGetInt64(occurrence_high)),
            .occurrence_low = static_cast<std::uint64_t>(
                DatumGetInt64(occurrence_low)),
            .capability_mask = static_cast<std::uint64_t>(
                DatumGetInt64(capability_mask)),
        };
    }
    return valid;
}

[[nodiscard]] bool coverage_stored_row_valid(
        ReusableCoverageState& state,
        TupleTableSlot* stored,
        std::uint64_t expected_ordinal) {
    return coverage_stored_row_valid(
        state.stored_columns,
        state.stored_column_count,
        state.current_header.capability_mask,
        stored,
        expected_ordinal);
}

/** Prove every stored row and semantic attno mapping before gate publication. */
[[nodiscard]] bool preflight_current_coverage_store(
        ReusableCoverageState& state,
        TupleDesc raw_descriptor,
        std::uint64_t expected_rows) {
    if (raw_descriptor == nullptr || state.current_replay_store == nullptr
        || state.current_replay_slot == nullptr
        || state.stored_descriptor == nullptr
        || state.stored_descriptor->natts
            != static_cast<int>(state.stored_column_count)) {
        return false;
    }
    for (std::uint32_t index = 0;
         index < state.stored_column_count; ++index) {
        const CoverageStoredColumnV1& stored = state.stored_columns[index];
        if (stored.kind != CoverageStoredColumnKindV1::ProviderAttribute) {
            continue;
        }
        if (stored.provider_attno == 0
            || stored.provider_attno
                > static_cast<std::uint32_t>(raw_descriptor->natts)) {
            return false;
        }
        const BioseqCacheColumnSchemaV2* semantic =
            coverage_contract_column(
                state.current_contract.domain, stored.provider_attno);
        const Form_pg_attribute raw = TupleDescAttr(
            raw_descriptor,
            static_cast<int>(stored.provider_attno - 1U));
        if (semantic == nullptr || raw->attisdropped
            || raw->atttypid != semantic->type_oid
            || raw->atttypmod != semantic->typmod
            || raw->attcollation != semantic->collation_oid) {
            return false;
        }
    }
    const auto required_mapped_once = [&state](
            const std::uint32_t* attnos,
            std::size_t count) {
        if (count != 0 && attnos == nullptr) {
            return false;
        }
        for (std::size_t required = 0; required < count; ++required) {
            std::uint32_t mapping_count = 0;
            for (std::uint32_t stored_index = 0;
                 stored_index < state.stored_column_count; ++stored_index) {
                const CoverageStoredColumnV1& stored =
                    state.stored_columns[stored_index];
                if (stored.kind
                        == CoverageStoredColumnKindV1::ProviderAttribute
                    && stored.provider_attno == attnos[required]) {
                    ++mapping_count;
                }
            }
            if (mapping_count != 1) {
                return false;
            }
        }
        return true;
    };
    if (!required_mapped_once(
            state.current_contract.domain.required_lineage_attnos,
            state.current_contract.domain.required_lineage_attno_count)
        || !required_mapped_once(
            state.current_contract.domain.required_boundary_attnos,
            state.current_contract.domain.required_boundary_attno_count)) {
        return false;
    }
    tuplestore_rescan(state.current_replay_store);
    std::uint64_t row_count = 0;
    while (tuplestore_gettupleslot(
            state.current_replay_store,
            true,
            false,
            state.current_replay_slot)) {
        if (!coverage_stored_row_valid(
                state, state.current_replay_slot, row_count)) {
            tuplestore_rescan(state.current_replay_store);
            ExecClearTuple(state.current_replay_slot);
            return false;
        }
        ++row_count;
    }
    tuplestore_rescan(state.current_replay_store);
    ExecClearTuple(state.current_replay_slot);
    return row_count == expected_rows;
}

[[nodiscard]] bool source_mapping_matches_raw_descriptor(
        const CoverageRuntimeSourceV1& source,
        TupleDesc raw_descriptor) noexcept {
    if (raw_descriptor == nullptr || source.stored_columns == nullptr
        || source.stored_column_count < kCoverageOccurrenceColumnCountV1) {
        return false;
    }
    const auto validate_half = [&source, raw_descriptor](
            const BioseqCacheColumnMapEntryV2* mappings,
            std::uint32_t count) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const BioseqCacheColumnMapEntryV2& mapping = mappings[index];
            if (mapping.consumer_attno == 0
                || mapping.consumer_attno
                    > static_cast<std::uint32_t>(raw_descriptor->natts)) {
                return false;
            }
            const Form_pg_attribute raw = TupleDescAttr(
                raw_descriptor,
                static_cast<int>(mapping.consumer_attno - 1U));
            if (raw->attisdropped || raw->atttypid != mapping.type_oid
                || raw->atttypmod != mapping.typmod
                || raw->attcollation != mapping.collation_oid) {
                return false;
            }
            std::uint32_t stored_matches = 0;
            for (std::uint32_t stored_index = 0;
                 stored_index < source.stored_column_count;
                 ++stored_index) {
                const CoverageStoredColumnV1& stored =
                    source.stored_columns[stored_index];
                if (stored.kind
                        == CoverageStoredColumnKindV1::ProviderAttribute
                    && stored.provider_attno == mapping.source_attno) {
                    ++stored_matches;
                }
            }
            if (stored_matches != 1) {
                return false;
            }
        }
        return true;
    };
    return validate_half(source.boundary_map, source.boundary_map_count)
        && validate_half(source.lineage_map, source.lineage_map_count);
}

[[nodiscard]] bool preflight_older_coverage_store(
        CoverageRuntimeSourceV1& source,
        std::uint64_t required_capability_mask,
        std::uint64_t expected_rows) {
    if (source.replay_store == nullptr || source.replay_slot == nullptr
        || source.stored_descriptor == nullptr
        || source.stored_descriptor->natts
            != static_cast<int>(source.stored_column_count)) {
        return false;
    }
    tuplestore_rescan(source.replay_store);
    std::uint64_t row_count = 0;
    while (tuplestore_gettupleslot(
            source.replay_store, true, false, source.replay_slot)) {
        if (!coverage_stored_row_valid(
                source.stored_columns,
                source.stored_column_count,
                required_capability_mask,
                source.replay_slot,
                row_count)) {
            tuplestore_rescan(source.replay_store);
            ExecClearTuple(source.replay_slot);
            return false;
        }
        ++row_count;
    }
    tuplestore_rescan(source.replay_store);
    ExecClearTuple(source.replay_slot);
    source.row_count = row_count;
    return row_count == expected_rows;
}

[[nodiscard]] bool source_stored_column_index(
        const CoverageRuntimeSourceV1& source,
        std::uint32_t provider_attno,
        std::uint32_t* output) noexcept {
    if (provider_attno == 0 || output == nullptr) {
        return false;
    }
    std::uint32_t matches = 0;
    std::uint32_t matched_index = 0;
    for (std::uint32_t index = 0;
         index < source.stored_column_count;
         ++index) {
        const CoverageStoredColumnV1& column = source.stored_columns[index];
        if (column.kind == CoverageStoredColumnKindV1::ProviderAttribute
            && column.provider_attno == provider_attno) {
            ++matches;
            matched_index = index;
        }
    }
    if (matches != 1) {
        return false;
    }
    *output = matched_index;
    return true;
}

[[nodiscard]] bool source_supplies_consumer_attno(
        const CoverageRuntimeSourceV1& source,
        std::uint32_t consumer_attno) noexcept {
    const auto mapped = [&source, consumer_attno](
            const BioseqCacheColumnMapEntryV2* mappings,
            std::uint32_t count) {
        for (std::uint32_t index = 0; index < count; ++index) {
            if (mappings[index].consumer_attno == consumer_attno) {
                std::uint32_t stored_index = 0;
                return source_stored_column_index(
                    source, mappings[index].source_attno, &stored_index);
            }
        }
        return false;
    };
    return mapped(source.boundary_map, source.boundary_map_count)
        || mapped(source.lineage_map, source.lineage_map_count);
}

/** Reconstruct one consumer-shaped raw slot from the provider source maps. */
[[nodiscard]] bool populate_consumer_slot_from_source(
        const CoverageRuntimeSourceV1& source,
        TupleTableSlot* stored,
        TupleTableSlot* raw) {
    if (stored == nullptr || raw == nullptr
        || raw->tts_tupleDescriptor == nullptr) {
        return false;
    }
    slot_getallattrs(stored);
    ExecClearTuple(raw);
    for (int index = 0; index < raw->tts_tupleDescriptor->natts; ++index) {
        raw->tts_values[index] = static_cast<Datum>(0);
        raw->tts_isnull[index] = true;
    }
    const auto apply = [&source, stored, raw](
            const BioseqCacheColumnMapEntryV2* mappings,
            std::uint32_t count) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const BioseqCacheColumnMapEntryV2& mapping = mappings[index];
            std::uint32_t stored_index = 0;
            if (mapping.consumer_attno == 0
                || mapping.consumer_attno
                    > static_cast<std::uint32_t>(
                        raw->tts_tupleDescriptor->natts)
                || !source_stored_column_index(
                    source, mapping.source_attno, &stored_index)) {
                return false;
            }
            const std::size_t raw_index = mapping.consumer_attno - 1U;
            raw->tts_values[raw_index] = stored->tts_values[stored_index];
            raw->tts_isnull[raw_index] = stored->tts_isnull[stored_index];
        }
        return true;
    };
    if (!apply(source.boundary_map, source.boundary_map_count)
        || !apply(source.lineage_map, source.lineage_map_count)) {
        ExecClearTuple(raw);
        return false;
    }
    raw->tts_nvalid = raw->tts_tupleDescriptor->natts;
    ExecStoreVirtualTuple(raw);
    return true;
}

[[nodiscard]] SqlBool evaluate_coverage_predicate(
        ExprState* predicate,
        ExprContext* context,
        TupleTableSlot* raw) {
    if (predicate == nullptr || context == nullptr || raw == nullptr) {
        return SqlBool::Unknown;
    }
    context->ecxt_scantuple = raw;
    bool is_null = false;
    const Datum result = ExecEvalExprSwitchContext(
        predicate, context, &is_null);
    if (is_null) {
        return SqlBool::Unknown;
    }
    return DatumGetBool(result)
        ? SqlBool::TrueValue
        : SqlBool::FalseValue;
}

inline constexpr std::size_t kCoverageLateFetchBatchRowsV1 = 256;

/** Batch late-fetch missing consumer fields and evaluate both SQL predicates. */
[[nodiscard]] bool evaluate_older_source_rows(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        TupleTableSlot* raw,
        const AttrNumber* missing_attnos,
        std::size_t missing_attno_count) {
    if (raw == nullptr || raw->tts_tupleDescriptor == nullptr
        || source.replay_store == nullptr || source.replay_slot == nullptr
        || source.source_preflight_context == nullptr
        || (missing_attno_count != 0
            && (missing_attnos == nullptr || state.provider == nullptr
                || state.provider->late_fetch_batch == nullptr))) {
        return false;
    }
    if (missing_attno_count != 0
        && kCoverageLateFetchBatchRowsV1
            > (std::numeric_limits<std::size_t>::max)()
                / missing_attno_count) {
        return false;
    }
    const std::size_t maximum_value_count =
        kCoverageLateFetchBatchRowsV1 * missing_attno_count;
    if (maximum_value_count != 0
        && (maximum_value_count > MaxAllocSize / sizeof(Datum)
            || maximum_value_count > MaxAllocSize / sizeof(bool)
            || maximum_value_count > MaxAllocSize / sizeof(std::uint8_t))) {
        return false;
    }
    MemoryContext batch_context = AllocSetContextCreate(
        source.source_preflight_context,
        "bioseq reusable coverage late fetch batch",
        ALLOCSET_DEFAULT_SIZES);
    ExprContext* expression_context = state.css.ss.ps.ps_ExprContext;
    tuplestore_rescan(source.replay_store);
    std::uint64_t row = 0;
    bool success = true;
    for (;;) {
        MemoryContext previous = MemoryContextSwitchTo(batch_context);
        auto** tuples = static_cast<MinimalTuple*>(palloc0(
            sizeof(MinimalTuple) * kCoverageLateFetchBatchRowsV1));
        auto* occurrences = static_cast<BioseqCacheOccurrenceV2*>(palloc0(
            sizeof(BioseqCacheOccurrenceV2)
                * kCoverageLateFetchBatchRowsV1));
        std::size_t batch_count = 0;
        while (batch_count < kCoverageLateFetchBatchRowsV1
            && tuplestore_gettupleslot(
                source.replay_store, true, false, source.replay_slot)) {
            if (!coverage_stored_row_valid(
                    source.stored_columns,
                    source.stored_column_count,
                    state.current_header.capability_mask,
                    source.replay_slot,
                    row + batch_count,
                    &occurrences[batch_count])) {
                success = false;
                break;
            }
            tuples[batch_count] = ExecCopySlotMinimalTuple(
                source.replay_slot);
            ExecClearTuple(source.replay_slot);
            ++batch_count;
        }
        MemoryContextSwitchTo(previous);
        if (!success || batch_count == 0) {
            break;
        }

        Datum* fetched_values = nullptr;
        bool* fetched_nulls = nullptr;
        if (missing_attno_count != 0) {
            const std::size_t value_count =
                batch_count * missing_attno_count;
            previous = MemoryContextSwitchTo(batch_context);
            auto* terminal = static_cast<std::uint8_t*>(palloc(
                batch_count * sizeof(std::uint8_t)));
            auto* seen = static_cast<std::uint8_t*>(palloc0(
                value_count * sizeof(std::uint8_t)));
            auto* raw_values = static_cast<std::uintptr_t*>(palloc0(
                value_count * sizeof(std::uintptr_t)));
            auto* raw_nulls = static_cast<std::uint8_t*>(palloc0(
                value_count * sizeof(std::uint8_t)));
            fetched_values = static_cast<Datum*>(palloc0(
                value_count * sizeof(Datum)));
            fetched_nulls = static_cast<bool*>(palloc0(
                value_count * sizeof(bool)));
            reuse::LateFetchSinkStateV1 sink_state{
                .reference_count = batch_count,
                .attribute_count = missing_attno_count,
                .terminal_status = std::span<std::uint8_t>{
                    terminal, batch_count},
                .value_seen = std::span<std::uint8_t>{
                    seen, value_count},
                .values = std::span<std::uintptr_t>{
                    raw_values, value_count},
                .nulls = std::span<std::uint8_t>{
                    raw_nulls, value_count},
            };
            reuse::ProviderCallDiagnosticV1 diagnostic{};
            const bool fetched =
                reuse::invoke_late_fetch_batch_fail_open(
                    state.provider,
                    &state.provider_lease,
                    state.plan_domain_nodes,
                    state.css.ss.ps.state,
                    occurrences,
                    batch_count,
                    missing_attnos,
                    missing_attno_count,
                    &sink_state,
                    raw->tts_tupleDescriptor,
                    batch_context,
                    fetched_values,
                    fetched_nulls,
                    batch_context,
                    &diagnostic);
            MemoryContextSwitchTo(previous);
            if (!fetched) {
                success = false;
                break;
            }
        }

        for (std::size_t batch_index = 0;
             batch_index < batch_count;
             ++batch_index) {
            ResetExprContext(expression_context);
            ExecStoreMinimalTuple(
                tuples[batch_index], source.replay_slot, false);
            if (!populate_consumer_slot_from_source(
                    source, source.replay_slot, raw)) {
                success = false;
                break;
            }
            for (std::size_t attribute = 0;
                 attribute < missing_attno_count;
                 ++attribute) {
                const AttrNumber attno = missing_attnos[attribute];
                const std::size_t value_index =
                    batch_index * missing_attno_count + attribute;
                raw->tts_values[attno - 1] = fetched_values[value_index];
                raw->tts_isnull[attno - 1] = fetched_nulls[value_index];
            }
            if (!populate_coverage_predicate_slot(state, raw)) {
                success = false;
                break;
            }
            const SqlBool requested = evaluate_coverage_predicate(
                state.requested_predicate_state,
                expression_context,
                state.predicate_slot);
            const SqlBool source_predicate = evaluate_coverage_predicate(
                source.predicate_state,
                expression_context,
                state.predicate_slot);
            if (passes_where(requested) && passes_where(source_predicate)) {
                set_coverage_selection_bit(
                    source, row + batch_index, true);
            }
            ExecClearTuple(state.predicate_slot);
            ExecClearTuple(raw);
            ExecClearTuple(source.replay_slot);
        }
        if (!success) {
            break;
        }
        row += batch_count;
        MemoryContextReset(batch_context);
    }
    ExecClearTuple(raw);
    if (state.predicate_slot != nullptr) {
        ExecClearTuple(state.predicate_slot);
    }
    ExecClearTuple(source.replay_slot);
    tuplestore_rescan(source.replay_store);
    MemoryContextDelete(batch_context);
    return success && row == source.row_count;
}

void remove_source_occurrence_owners(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        std::size_t source_index) noexcept {
    if (state.occurrence_owners == nullptr || source.replay_store == nullptr
        || source.replay_slot == nullptr) {
        return;
    }
    tuplestore_rescan(source.replay_store);
    std::uint64_t row = 0;
    while (tuplestore_gettupleslot(
            source.replay_store, true, false, source.replay_slot)) {
        if (coverage_selection_bit(source, row)) {
            BioseqCacheOccurrenceV2 occurrence{};
            if (coverage_stored_row_valid(
                    source.stored_columns,
                    source.stored_column_count,
                    0,
                    source.replay_slot,
                    row,
                    &occurrence)) {
                const CoverageOccurrenceKeyV1 key =
                    coverage_occurrence_key(occurrence);
                auto* owner = static_cast<CoverageOccurrenceOwnerV1*>(
                    hash_search(
                        state.occurrence_owners,
                        &key,
                        HASH_FIND,
                        nullptr));
                if (owner != nullptr && owner->source_index == source_index) {
                    void* removed = hash_search(
                        state.occurrence_owners,
                        &key,
                        HASH_REMOVE,
                        nullptr);
                    if (removed != nullptr
                        && state.occurrence_owner_count != 0) {
                        --state.occurrence_owner_count;
                    }
                }
            }
        }
        ExecClearTuple(source.replay_slot);
        ++row;
    }
    tuplestore_rescan(source.replay_store);
    ExecClearTuple(source.replay_slot);
}

/** Evaluate P and Pi for all materialized rows, then assign earliest owner. */
[[nodiscard]] bool select_older_coverage_source(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        std::size_t source_index,
        TupleTableSlot* raw) {
    if (!source.valid || source.predicate_state == nullptr
        || state.requested_predicate_state == nullptr
        || state.occurrence_owners == nullptr || raw == nullptr
        || state.predicate_slot == nullptr
        || state.predicate_descriptor == nullptr
        || state.predicate_consumer_attnos == nullptr) {
        return false;
    }
    Bitmapset* requested_attnos = bms_copy(
        state.requested_predicate_attnos);
    requested_attnos = bms_add_members(
        requested_attnos, source.predicate_attnos);
    const int predicate_attno_count = bms_num_members(requested_attnos);
    auto* missing_attnos = predicate_attno_count == 0
        ? nullptr
        : static_cast<AttrNumber*>(palloc(
            static_cast<std::size_t>(predicate_attno_count)
                * sizeof(AttrNumber)));
    std::size_t missing_attno_count = 0;
    int attno = -1;
    while ((attno = bms_next_member(requested_attnos, attno)) >= 0) {
        if (attno <= 0
            || attno > static_cast<int>(state.predicate_column_count)) {
            bms_free(requested_attnos);
            if (missing_attnos != nullptr) {
                pfree(missing_attnos);
            }
            return false;
        }
        const std::uint32_t consumer_attno =
            state.predicate_consumer_attnos[attno - 1];
        if (!source_supplies_consumer_attno(
                source, consumer_attno)) {
            missing_attnos[missing_attno_count++] =
                static_cast<AttrNumber>(consumer_attno);
        }
    }
    bms_free(requested_attnos);

    if (source.row_count
            > (std::numeric_limits<std::uint64_t>::max)() - 7U) {
        if (missing_attnos != nullptr) {
            pfree(missing_attnos);
        }
        return false;
    }
    const std::uint64_t bitmap_bytes = (source.row_count + 7U) >> 3U;
    if (bitmap_bytes > static_cast<std::uint64_t>(MaxAllocSize)
        || bitmap_bytes
            > (std::numeric_limits<std::size_t>::max)()
        || state.selection_bitmap_bytes_admitted
            > kMaximumCoverageSelectionBitmapBytesV1
        || bitmap_bytes > kMaximumCoverageSelectionBitmapBytesV1
            - state.selection_bitmap_bytes_admitted) {
        if (missing_attnos != nullptr) {
            pfree(missing_attnos);
        }
        return false;
    }
    state.selection_bitmap_bytes_admitted += bitmap_bytes;
    source.selection_bitmap_bytes = static_cast<std::size_t>(bitmap_bytes);
    source.selection_bitmap = bitmap_bytes == 0
        ? nullptr
        : static_cast<std::uint8_t*>(palloc0(
            static_cast<std::size_t>(bitmap_bytes)));
    const bool evaluated = evaluate_older_source_rows(
        state,
        source,
        raw,
        missing_attnos,
        missing_attno_count);
    if (missing_attnos != nullptr) {
        pfree(missing_attnos);
    }
    if (!evaluated) {
        return false;
    }

    tuplestore_rescan(source.replay_store);
    std::uint64_t row = 0;
    bool duplicate_within_source = false;
    while (tuplestore_gettupleslot(
            source.replay_store, true, false, source.replay_slot)) {
        if (coverage_selection_bit(source, row)) {
            BioseqCacheOccurrenceV2 occurrence{};
            if (!coverage_stored_row_valid(
                    source.stored_columns,
                    source.stored_column_count,
                    state.current_header.capability_mask,
                    source.replay_slot,
                    row,
                    &occurrence)) {
                duplicate_within_source = true;
                break;
            }
            const CoverageOccurrenceKeyV1 key =
                coverage_occurrence_key(occurrence);
            auto* owner = static_cast<CoverageOccurrenceOwnerV1*>(
                hash_search(
                    state.occurrence_owners,
                    &key,
                    HASH_FIND,
                    nullptr));
            if (owner == nullptr) {
                if (state.occurrence_owner_count
                        >= kMaximumCoverageOccurrenceOwnersV1) {
                    duplicate_within_source = true;
                    break;
                }
                bool found = false;
                owner = static_cast<CoverageOccurrenceOwnerV1*>(
                    hash_search(
                        state.occurrence_owners,
                        &key,
                        HASH_ENTER,
                        &found));
                if (owner == nullptr || found) {
                    duplicate_within_source = true;
                    break;
                }
                owner->source_index = static_cast<std::uint32_t>(
                    source_index);
                owner->reserved = 0;
                ++state.occurrence_owner_count;
                ++source.selected_row_count;
            } else if (owner->source_index == source_index) {
                duplicate_within_source = true;
                break;
            } else {
                set_coverage_selection_bit(source, row, false);
            }
        }
        ExecClearTuple(source.replay_slot);
        ++row;
    }
    tuplestore_rescan(source.replay_store);
    ExecClearTuple(source.replay_slot);
    if (duplicate_within_source) {
        remove_source_occurrence_owners(state, source, source_index);
        source.selected_row_count = 0;
        return false;
    }
    return row == source.row_count;
}

/** Pin, load, checksum, map, and revalidate one completed older generation. */
[[nodiscard]] bool materialize_older_coverage_source(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        TupleDesc raw_descriptor,
        const VisibilityTicket& visibility) {
    const ReusableGenerationSnapshotV1 completed =
        source.completed_snapshot;
    const bool initial_semantic = older_reusable_snapshot_matches(
        state, source, current_plan, completed);
    const bool initial_ttl = older_reusable_ttl_decision(
            completed,
            state.producer_ttl_ms,
            source.waited,
            source.ttl_entitlement_consumed).readable;
    if (!provider_visibility_ticket_still_bound(visibility)
        || !initial_ttl || !initial_semantic) {
        return false;
    }
    CoverageStoredRecipeV1 recipe{};
    if (!build_coverage_stored_recipe(source.contract.domain, &recipe)) {
        return false;
    }
    source.stored_descriptor = recipe.descriptor;
    source.stored_columns = recipe.columns;
    source.stored_column_count = recipe.column_count;
    if (!source_mapping_matches_raw_descriptor(source, raw_descriptor)) {
        return false;
    }
    const storage::ManifestHandle expected_handle =
        storage_manifest_handle(completed.manifest);
    storage::PinManifestResult pinned{};
    if (source.registry_reference.kind
            == ReusableReferenceKindV1::Waiter) {
        const storage::RegisterManifestWaiterResult entitlement =
            storage::current_manifest_waiter(completed.reference.id);
        if (entitlement) {
            pinned = storage::convert_waiter_to_pin(entitlement.token);
            if (!pinned) {
                static_cast<void>(storage::unregister_manifest_waiter(
                    entitlement.token));
            }
        } else {
            pinned.error = entitlement.error;
        }
    } else {
        pinned = storage::pin_manifest(completed.reference.id);
    }
    const bool handle_matches = pinned
        && pinned.pin.handle.slot == expected_handle.slot
        && pinned.pin.handle.slot_sequence == expected_handle.slot_sequence
        && pinned.pin.handle.id == expected_handle.id;
    if (!handle_matches
        || pinned.pin.kind != CacheEntryKind::ReusableCoverage
        || pinned.pin.row_count != completed.row_count) {
        if (pinned) {
            static_cast<void>(storage::unpin_manifest(pinned.pin));
        }
        return false;
    }
    source.manifest_pin = pinned.pin;
    source.manifest_pin_owned = true;
    source.replay_store = load_complete_result(
        source.manifest_pin,
        source.stored_descriptor,
        CacheEntryKind::ReusableCoverage);
    if (source.replay_store != nullptr) {
        source.replay_slot = MakeSingleTupleTableSlot(
            source.stored_descriptor, &TTSOpsMinimalTuple);
    }
    ReusableGenerationSnapshotV1 final_snapshot{};
    const bool store_loaded = source.replay_store != nullptr
        && source.replay_slot != nullptr;
    const bool store_preflight = store_loaded
        && preflight_older_coverage_store(
            source,
            state.current_header.capability_mask,
            completed.row_count);
    const bool final_snapshotted = store_preflight
        && snapshot_current_reusable_generation(
            source.reference, &final_snapshot);
    const bool final_semantic = final_snapshotted
        && final_snapshot.reference == completed.reference
        && final_snapshot.manifest == completed.manifest
        && older_reusable_snapshot_matches(
            state, source, current_plan, final_snapshot);
    const bool final_ttl = final_semantic
        && older_reusable_ttl_decision(
            final_snapshot,
            state.producer_ttl_ms,
            source.waited,
            source.ttl_entitlement_consumed).readable;
    const VisibilityTicket refreshed = final_semantic
        ? mint_visibility_ticket(
            final_snapshot.metadata.observability,
            VisibilityTicketPurposeV1::Continuation)
        : VisibilityTicket{};
    const dependency_runtime::DependencyRuntimeResultV1 final_vector =
        refreshed.valid
        ? validate_reusable_generation_dependencies_locked(
            final_snapshot, refreshed.completion_count)
        : dependency_runtime::DependencyRuntimeResultV1{
            .error = dependency_runtime::DependencyRuntimeErrorV1::
                ReferenceValidationFailed,
            .fail_open = true,
        };
    const bool materialized = store_preflight && final_semantic && final_ttl
        && provider_visibility_ticket_still_bound(visibility)
        && refreshed.valid && final_vector;
    if (!materialized) {
        return false;
    }
    if (source.registry_reference.kind == ReusableReferenceKindV1::Waiter) {
        if (!convert_runtime_waiter_to_reader(source.registry_reference)) {
            return false;
        }
        source.ttl_entitlement_consumed = true;
    }
    if (source.registry_reference.kind
            != ReusableReferenceKindV1::ReaderPin) {
        return false;
    }
    source.completed_snapshot = final_snapshot;
    source.valid = true;
    return true;
}

void release_all_older_coverage_sources(
        ReusableCoverageState& state) noexcept {
    for (CoverageRuntimeSourceV1& source : state.sources) {
        release_coverage_source(source);
    }
    if (state.occurrence_owners != nullptr) {
        hash_destroy(state.occurrence_owners);
        state.occurrence_owners = nullptr;
    }
    state.occurrence_owner_count = 0;
    state.selection_bitmap_bytes_admitted = 0;
    state.active_source_count = 0;
    state.older_preflight_ready = false;
}

void release_source_selection_bitmap_admission(
        ReusableCoverageState& state,
        const CoverageRuntimeSourceV1& source) noexcept {
    if (source.selection_bitmap_bytes
            > state.selection_bitmap_bytes_admitted) {
        state.selection_bitmap_bytes_admitted = 0;
    } else {
        state.selection_bitmap_bytes_admitted -=
            source.selection_bitmap_bytes;
    }
}

[[nodiscard]] bool select_older_coverage_source_fail_open(
    ReusableCoverageState& state,
    CoverageRuntimeSourceV1& source,
    std::size_t source_index,
    TupleTableSlot* raw);

[[nodiscard]] bool select_all_older_coverage_sources(
        ReusableCoverageState& state,
        std::size_t candidate_count,
        TupleTableSlot* raw) {
    if (candidate_count == 0 || candidate_count > state.sources.size()
        || raw == nullptr || raw->tts_tupleDescriptor == nullptr
        || state.css.ss.ps.ps_ExprContext == nullptr
        || state.css.ss.ps.state == nullptr) {
        return false;
    }
    HASHCTL control{};
    control.keysize = sizeof(CoverageOccurrenceKeyV1);
    control.entrysize = sizeof(CoverageOccurrenceOwnerV1);
    control.hcxt = state.css.ss.ps.state->es_query_cxt;
    state.occurrence_owners = hash_create(
        "bioseq reusable coverage occurrence owners",
        128,
        &control,
        HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
    if (state.occurrence_owners == nullptr) {
        return false;
    }
    std::uint16_t active = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        CoverageRuntimeSourceV1& source = state.sources[index];
        if (!source.valid) {
            continue;
        }
        if (select_older_coverage_source_fail_open(
                state, source, index, raw)) {
            ++active;
        } else {
            remove_source_occurrence_owners(state, source, index);
            release_source_selection_bitmap_admission(state, source);
            release_coverage_source(source);
        }
    }
    ExecClearTuple(raw);
    state.active_source_count = active;
    if (state.occurrence_owners != nullptr) {
        hash_destroy(state.occurrence_owners);
        state.occurrence_owners = nullptr;
    }
    state.occurrence_owner_count = 0;
    return active != 0;
}

[[nodiscard]] bool reusable_reference_precedes(
        const ReusableGenerationReferenceV1& left,
        const ReusableGenerationReferenceV1& right) noexcept {
    if (left.id.claim_sequence != right.id.claim_sequence) {
        return left.id.claim_sequence < right.id.claim_sequence;
    }
    if (left.id.generation != right.id.generation) {
        return left.id.generation < right.id.generation;
    }
    if (left.id.boot_nonce != right.id.boot_nonce) {
        return left.id.boot_nonce < right.id.boot_nonce;
    }
    if (left.id.digest.bytes != right.id.digest.bytes) {
        return std::lexicographical_compare(
            left.id.digest.bytes.begin(), left.id.digest.bytes.end(),
            right.id.digest.bytes.begin(), right.id.digest.bytes.end());
    }
    if (left.generation_slot != right.generation_slot) {
        return left.generation_slot < right.generation_slot;
    }
    return left.slot_sequence < right.slot_sequence;
}

/** Decode one planned source in its own ERROR-isolated lifetime context. */
[[nodiscard]] bool decode_older_coverage_source_fail_open(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& runtime,
        const List* candidate,
        const CoverageCurrentPlanViewV1& current_plan,
        std::size_t planner_index) {
    if (candidate == nullptr || !IsA(candidate, List)
        || list_length(candidate) != 7
        || state.css.ss.ps.state == nullptr
        || state.css.ss.ps.state->es_query_cxt == nullptr
        || runtime.source_preflight_context != nullptr) {
        return false;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool decoded = false;
    PG_TRY();
    {
        runtime.source_preflight_context = AllocSetContextCreate(
            state.css.ss.ps.state->es_query_cxt,
            "bioseq reusable source preflight",
            ALLOCSET_DEFAULT_SIZES);
        MemoryContextSwitchTo(runtime.source_preflight_context);
        decoded = decode_reusable_reference_node(
            list_nth(candidate, 0), &runtime.reference);
        const std::span<const std::byte> generation_key = decoded
            ? binary_node_payload(list_nth(candidate, 1))
            : std::span<const std::byte>{};
        const std::span<const std::byte> descriptor = decoded
            ? binary_node_payload(list_nth(candidate, 2))
            : std::span<const std::byte>{};
        if (decoded) {
            runtime.generation_key_node = list_nth(candidate, 1);
            runtime.descriptor_node = list_nth(candidate, 2);
            decoded = !generation_key.empty() && !descriptor.empty()
                && runtime.reference != state.current_reference
                && runtime.reference.id.claim_sequence
                    < state.current_reference.id.claim_sequence
                && decode_coverage_descriptor_fail_open(
                    descriptor,
                    runtime.source_preflight_context,
                    &runtime.contract,
                    &state.current_contract.binding)
                && runtime.contract.domain.provider_id
                    == state.current_header.provider_id
                && runtime.contract.source.provider_id
                    == state.current_header.provider_id
                && runtime.contract.domain.compatibility_family_id
                    == state.current_header.compatibility_family_id
                && runtime.contract.source.compatibility_family_id
                    == state.current_header.compatibility_family_id
                && (runtime.contract.domain.capability_mask
                        & state.current_header.capability_mask)
                    == state.current_header.capability_mask
                && (runtime.contract.source.capability_mask
                        & state.current_header.capability_mask)
                    == state.current_header.capability_mask
                && decode_and_validate_runtime_source_mapping(
                    runtime,
                    state.current_contract.domain,
                    list_nth(candidate, 3),
                    list_nth(candidate, 4));
        }
        if (decoded
            && runtime.reference.id.digest
                == state.current_header.generation_digest
            && generation_key.size()
                == current_plan.canonical_generation_key.size()
            && std::memcmp(
                generation_key.data(),
                current_plan.canonical_generation_key.data(),
                generation_key.size()) == 0) {
            decoded = false;
        }
        for (std::size_t prior = 0;
             decoded && prior < planner_index;
             ++prior) {
            decoded = state.sources[prior].reference != runtime.reference;
        }
        MemoryContextSwitchTo(caller_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        release_coverage_source(runtime);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        decoded = false;
    }
    PG_END_TRY();
    if (!decoded) {
        release_coverage_source(runtime);
    }
    return decoded;
}

/** Recheck/pin/wait one source without allowing its ERROR to drop siblings. */
[[nodiscard]] bool acquire_older_coverage_source_fail_open(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        std::span<const std::uint64_t> owned_claims,
        const VisibilityTicket& visibility,
        bool wait_for_completion) {
    if (source.source_preflight_context == nullptr) {
        return false;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool accepted = false;
    PG_TRY();
    {
        MemoryContextSwitchTo(source.source_preflight_context);
        accepted = wait_for_completion
            ? source.registry_reference_owned
                && source.registry_reference.kind
                    == ReusableReferenceKindV1::Waiter
                && wait_for_older_reusable_generation(
                    state,
                    source,
                    current_plan,
                    owned_claims,
                    visibility,
                    state.producer_ttl_ms)
            : acquire_older_reusable_reference(
                state,
                source,
                current_plan,
                owned_claims,
                visibility,
                state.producer_ttl_ms);
        MemoryContextSwitchTo(caller_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            release_coverage_source(source);
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = false;
    }
    PG_END_TRY();
    return accepted;
}

/** Load and validate one source under its source-local ERROR boundary. */
[[nodiscard]] bool materialize_older_coverage_source_fail_open(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        TupleDesc raw_descriptor,
        const VisibilityTicket& visibility) {
    if (source.source_preflight_context == nullptr
        || !source.registry_reference_owned) {
        return false;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool materialized = false;
    PG_TRY();
    {
        MemoryContextSwitchTo(source.source_preflight_context);
        materialized = materialize_older_coverage_source(
            state, source, current_plan, raw_descriptor, visibility);
        MemoryContextSwitchTo(caller_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            release_coverage_source(source);
            PG_RE_THROW();
        }
        FlushErrorState();
        materialized = false;
    }
    PG_END_TRY();
    return materialized;
}

/** Evaluate one source bitmap/provider batch without suppressing later sources. */
[[nodiscard]] bool select_older_coverage_source_fail_open(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        std::size_t source_index,
        TupleTableSlot* raw) {
    if (source.source_preflight_context == nullptr) {
        return false;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool selected = false;
    PG_TRY();
    {
        MemoryContextSwitchTo(source.source_preflight_context);
        selected = select_older_coverage_source(
            state, source, source_index, raw);
        MemoryContextSwitchTo(caller_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            release_coverage_source(source);
            PG_RE_THROW();
        }
        FlushErrorState();
        selected = false;
    }
    PG_END_TRY();
    return selected;
}

/** Decide every planned older source before any later gate publication. */
[[nodiscard]] bool prepare_older_coverage_sources(
        ReusableCoverageState& state,
        const CustomScan& scan,
        const CoverageCurrentPlanViewV1& current_plan,
        TupleDesc raw_descriptor,
        const VisibilityTicket& visibility) {
    const auto* candidates = scan.custom_private != NIL
            && list_length(scan.custom_private) >= 4
        ? static_cast<const List*>(list_nth(scan.custom_private, 3))
        : NIL;
    const std::size_t candidate_count = static_cast<std::size_t>(
        list_length(candidates));
    if (candidate_count == 0
        || candidate_count > state.sources.size()
        || !state.current_reference
        || state.current_reference.id.claim_sequence == 0
        || raw_descriptor == nullptr
        || !tuple_descriptor_matches_mapping_schema(
            raw_descriptor,
            state.current_contract.domain.boundary_columns,
            state.current_contract.domain.boundary_column_count)
        || !tuple_descriptor_matches_mapping_schema(
            raw_descriptor,
            state.current_contract.domain.lineage_columns,
            state.current_contract.domain.lineage_column_count)
        || !provider_visibility_ticket_still_bound(visibility)) {
        return false;
    }

    std::array<std::size_t, kMaxSourcesPerCoverageNode> acquisition_order{};
    std::size_t acquisition_count = 0;
    std::size_t planner_index = 0;
    ListCell* cell = nullptr;
    foreach(cell, candidates) {
        CoverageRuntimeSourceV1& runtime = state.sources[planner_index];
        const auto* candidate = static_cast<const List*>(lfirst(cell));
        if (decode_older_coverage_source_fail_open(
                state,
                runtime,
                candidate,
                current_plan,
                planner_index)) {
            acquisition_order[acquisition_count++] = planner_index;
        }
        ++planner_index;
    }
    if (acquisition_count == 0) {
        return false;
    }
    std::sort(
        acquisition_order.begin(),
        acquisition_order.begin() + acquisition_count,
        [&state](std::size_t left, std::size_t right) {
            return reusable_reference_precedes(
                state.sources[left].reference,
                state.sources[right].reference);
        });

    const std::size_t owned_capacity = kRegistryCapacity
        + static_cast<std::size_t>(g_reusable_wait_slot_count);
    if (g_reusable_wait_slot_count == 0
        || owned_capacity < kRegistryCapacity
        || owned_capacity > static_cast<std::size_t>(MaxAllocSize)
            / sizeof(std::uint64_t)) {
        release_all_older_coverage_sources(state);
        return false;
    }
    auto* locally_owned = static_cast<std::uint64_t*>(palloc_extended(
        sizeof(std::uint64_t) * owned_capacity,
        MCXT_ALLOC_NO_OOM));
    std::size_t locally_owned_exact_count = 0;
    std::size_t locally_owned_count = 0;
    if (locally_owned == nullptr
        || !collect_backend_owned_claim_sequences(
            std::span<std::uint64_t>{locally_owned, owned_capacity},
            &locally_owned_exact_count,
            &locally_owned_count)) {
        if (locally_owned != nullptr) {
            pfree(locally_owned);
        }
        release_all_older_coverage_sources(state);
        return false;
    }
    const auto* owned_begin = locally_owned + locally_owned_exact_count;
    const auto* owned_end = locally_owned + locally_owned_count;
    if (locally_owned_exact_count != 0
        || owned_begin == owned_end
        || std::find(
            owned_begin,
            owned_end,
            state.current_reference.id.claim_sequence) == owned_end) {
        pfree(locally_owned);
        release_all_older_coverage_sources(state);
        return false;
    }
    const std::span<const std::uint64_t> owned_claims{
        owned_begin, static_cast<std::size_t>(owned_end - owned_begin)};
    for (std::size_t order = 0; order < acquisition_count; ++order) {
        CoverageRuntimeSourceV1& source =
            state.sources[acquisition_order[order]];
        if (!acquire_older_coverage_source_fail_open(
                state,
                source,
                current_plan,
                owned_claims,
                visibility,
                false)) {
            release_coverage_source(source);
        }
    }
    for (std::size_t order = 0; order < acquisition_count; ++order) {
        CoverageRuntimeSourceV1& source =
            state.sources[acquisition_order[order]];
        if (source.registry_reference_owned
            && source.registry_reference.kind
                == ReusableReferenceKindV1::Waiter
            && !acquire_older_coverage_source_fail_open(
                state,
                source,
                current_plan,
                owned_claims,
                visibility,
                true)) {
            release_coverage_source(source);
        }
    }
    pfree(locally_owned);
    std::uint16_t materialized_count = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        CoverageRuntimeSourceV1& source = state.sources[index];
        if (source.registry_reference_owned
            && materialize_older_coverage_source_fail_open(
                state,
                source,
                current_plan,
                raw_descriptor,
                visibility)) {
            ++materialized_count;
        } else {
            release_coverage_source(source);
        }
    }
    state.active_source_count = materialized_count;
    state.older_preflight_ready = materialized_count != 0
        && select_all_older_coverage_sources(
            state,
            candidate_count,
            state.css.ss.ss_ScanTupleSlot)
        && provider_visibility_ticket_still_bound(visibility);
    if (!state.older_preflight_ready) {
        release_all_older_coverage_sources(state);
    }
    return state.older_preflight_ready;
}

[[nodiscard]] bool prepare_older_coverage_sources_fail_open(
        ReusableCoverageState& state,
        const CustomScan& scan,
        const CoverageCurrentPlanViewV1& current_plan,
        TupleDesc raw_descriptor,
        const VisibilityTicket& visibility) {
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool prepared = false;
    PG_TRY();
    {
        prepared = prepare_older_coverage_sources(
            state, scan, current_plan, raw_descriptor, visibility);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        release_all_older_coverage_sources(state);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        prepared = false;
    }
    PG_END_TRY();
    return prepared;
}

/** Validate and materialize all stored bytes before publishing one valid gate. */
[[nodiscard]] bool prepare_current_coverage_consumer(
        ReusableCoverageState& state,
        const CoverageCurrentPlanViewV1& plan,
        const ReusableGenerationMetadataV1& expected_metadata,
        const ReusableGenerationSnapshotV1& completed,
        TupleDesc raw_descriptor) {
    const VisibilityTicket initial_dependency_visibility =
        mint_visibility_ticket(
            completed.metadata.observability,
            VisibilityTicketPurposeV1::Continuation);
    const bool initial_semantic =
        completed.state == CacheEntryState::Complete
        && current_reusable_snapshot_matches(
            completed, plan, expected_metadata);
    const bool initial_ttl = current_reusable_ttl_decision(
        state, completed).readable;
    const dependency_runtime::DependencyRuntimeResultV1 initial_vector =
        initial_dependency_visibility.valid
        ? validate_reusable_generation_dependencies_locked(
            completed,
            initial_dependency_visibility.completion_count)
        : dependency_runtime::DependencyRuntimeResultV1{
            .error = dependency_runtime::DependencyRuntimeErrorV1::
                ReferenceValidationFailed,
            .fail_open = true,
        };
    if (!completed.manifest || !initial_semantic || !initial_ttl
        || !initial_dependency_visibility.valid || !initial_vector) {
        return false;
    }
    CoverageStoredRecipeV1 recipe{};
    if (!build_coverage_stored_recipe(
            state.current_contract.domain, &recipe)) {
        return false;
    }
    state.stored_descriptor = recipe.descriptor;
    state.stored_columns = recipe.columns;
    state.stored_column_count = recipe.column_count;

    const storage::ManifestHandle expected_handle =
        storage_manifest_handle(completed.manifest);
    storage::PinManifestResult pinned{};
    if (state.current_reference_token.kind
            == ReusableReferenceKindV1::Waiter) {
        const storage::RegisterManifestWaiterResult entitlement =
            storage::current_manifest_waiter(completed.reference.id);
        if (entitlement) {
            pinned = storage::convert_waiter_to_pin(entitlement.token);
            if (!pinned) {
                static_cast<void>(storage::unregister_manifest_waiter(
                    entitlement.token));
            }
        } else {
            pinned.error = entitlement.error;
        }
    } else {
        pinned = storage::pin_manifest(completed.reference.id);
    }
    const bool handle_matches = pinned
        && pinned.pin.handle.slot == expected_handle.slot
        && pinned.pin.handle.slot_sequence == expected_handle.slot_sequence
        && pinned.pin.handle.id == expected_handle.id;
    if (!handle_matches
        || pinned.pin.kind != CacheEntryKind::ReusableCoverage
        || pinned.pin.row_count != completed.row_count) {
        if (pinned) {
            static_cast<void>(storage::unpin_manifest(pinned.pin));
        }
        destroy_coverage_builder(state);
        return false;
    }
    state.current_manifest_pin = pinned.pin;
    state.current_manifest_pin_owned = true;
    state.current_replay_store = load_complete_result(
        state.current_manifest_pin,
        state.stored_descriptor,
        CacheEntryKind::ReusableCoverage);
    if (state.current_replay_store != nullptr) {
        state.current_replay_slot = MakeSingleTupleTableSlot(
            state.stored_descriptor, &TTSOpsMinimalTuple);
    }
    ReusableGenerationSnapshotV1 final_snapshot{};
    const bool store_loaded = state.current_replay_store != nullptr
        && state.current_replay_slot != nullptr;
    const bool store_preflight = store_loaded
        && preflight_current_coverage_store(
            state, raw_descriptor, completed.row_count);
    const bool final_snapshotted = store_preflight
        && snapshot_current_reusable_generation(
            state.current_reference, &final_snapshot);
    const bool final_semantic = final_snapshotted
        && final_snapshot.state == CacheEntryState::Complete
        && final_snapshot.reference == completed.reference
        && final_snapshot.manifest == completed.manifest
        && current_reusable_snapshot_matches(
            final_snapshot, plan, expected_metadata);
    const bool final_ttl = final_semantic
        && current_reusable_ttl_decision(state, final_snapshot).readable;
    const VisibilityTicket final_dependency_visibility = final_semantic
        ? mint_visibility_ticket(
            final_snapshot.metadata.observability,
            VisibilityTicketPurposeV1::Continuation)
        : VisibilityTicket{};
    const dependency_runtime::DependencyRuntimeResultV1 final_vector =
        final_dependency_visibility.valid
        ? validate_reusable_generation_dependencies_locked(
                final_snapshot,
                final_dependency_visibility.completion_count)
        : dependency_runtime::DependencyRuntimeResultV1{
            .error = dependency_runtime::DependencyRuntimeErrorV1::
                ReferenceValidationFailed,
            .fail_open = true,
        };
    const bool materialized = store_preflight && final_semantic && final_ttl
        && final_dependency_visibility.valid && final_vector;
    if (!materialized) {
        release_current_coverage_consumer(state);
        return false;
    }
    if (state.current_reference_token.kind
            == ReusableReferenceKindV1::Waiter
        && !convert_current_waiter_to_reader(state)) {
        release_current_coverage_consumer(state);
        return false;
    }
    if (state.current_reference_token.kind
            != ReusableReferenceKindV1::ReaderPin) {
        release_current_coverage_consumer(state);
        return false;
    }
    state.runtime_role = CoverageRuntimeRoleV1::CurrentConsumer;
    return true;
}

[[nodiscard]] bool capture_coverage_row_with_occurrence(
        ReusableCoverageState& state,
        TupleTableSlot* slot,
        const BioseqCacheOccurrenceV2& occurrence) {
    if (state.column_builder == nullptr || state.send_functions == nullptr
        || state.stored_columns == nullptr
        || state.stored_descriptor == nullptr
        || state.stored_column_count == 0 || slot == nullptr
        || slot->tts_tupleDescriptor == nullptr
        || occurrence.struct_size < sizeof(BioseqCacheOccurrenceV2)
        || occurrence.rte_ordinal == 0 || occurrence.logical_relation_oid == 0
        || occurrence.leaf_relation_oid == 0 || occurrence.identity_kind == 0
        || occurrence.reserved != 0
        || (occurrence.capability_mask & state.current_header.capability_mask)
            != state.current_header.capability_mask) {
        return false;
    }
    const std::size_t column_count = state.stored_column_count;
    auto* values = static_cast<EncodedColumnValue*>(palloc0(
        sizeof(EncodedColumnValue) * column_count));
    auto** encoded = static_cast<bytea**>(palloc0(
        sizeof(bytea*) * column_count));
    bool success = false;
    std::uint64_t row_logical_bytes = 0;
    for (std::size_t index = 0; index < column_count; ++index) {
        Datum datum = static_cast<Datum>(0);
        bool is_null = false;
        const CoverageStoredColumnV1& column = state.stored_columns[index];
        switch (column.kind) {
            case CoverageStoredColumnKindV1::ResultOrdinal:
                datum = Int64GetDatum(
                    static_cast<std::int64_t>(state.row_count));
                break;
            case CoverageStoredColumnKindV1::RteOrdinal:
                datum = Int32GetDatum(
                    static_cast<std::int32_t>(occurrence.rte_ordinal));
                break;
            case CoverageStoredColumnKindV1::LogicalRelationOid:
                datum = ObjectIdGetDatum(
                    static_cast<Oid>(occurrence.logical_relation_oid));
                break;
            case CoverageStoredColumnKindV1::LeafRelationOid:
                datum = ObjectIdGetDatum(
                    static_cast<Oid>(occurrence.leaf_relation_oid));
                break;
            case CoverageStoredColumnKindV1::IdentityKind:
                datum = Int32GetDatum(
                    static_cast<std::int32_t>(occurrence.identity_kind));
                break;
            case CoverageStoredColumnKindV1::RowIdentity:
                datum = Int64GetDatum(
                    static_cast<std::int64_t>(occurrence.row_identity));
                break;
            case CoverageStoredColumnKindV1::OccurrenceHigh:
                datum = Int64GetDatum(
                    static_cast<std::int64_t>(occurrence.occurrence_high));
                break;
            case CoverageStoredColumnKindV1::OccurrenceLow:
                datum = Int64GetDatum(
                    static_cast<std::int64_t>(occurrence.occurrence_low));
                break;
            case CoverageStoredColumnKindV1::CapabilityMask:
                datum = Int64GetDatum(
                    static_cast<std::int64_t>(occurrence.capability_mask));
                break;
            case CoverageStoredColumnKindV1::ProviderAttribute:
                if (column.provider_attno == 0
                    || column.provider_attno
                        > static_cast<std::uint32_t>(
                            slot->tts_tupleDescriptor->natts)) {
                    is_null = true;
                    success = false;
                    goto capture_cleanup;
                }
                datum = slot_getattr(
                    slot,
                    static_cast<int>(column.provider_attno),
                    &is_null);
                break;
        }
        if (is_null) {
            values[index].is_null = true;
            continue;
        }
        encoded[index] = OidSendFunctionCall(
            state.send_functions[index], datum);
        const std::size_t byte_count = VARSIZE_ANY_EXHDR(encoded[index]);
        values[index].bytes = std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(
                VARDATA_ANY(encoded[index])),
            byte_count};
        if (row_logical_bytes > kMaximumStoredResultBytes - byte_count) {
            goto capture_cleanup;
        }
        row_logical_bytes += byte_count;
    }
    if (state.logical_bytes
            <= kMaximumStoredResultBytes - row_logical_bytes) {
        try {
            success = state.column_builder->append_row(
                std::span<const EncodedColumnValue>{
                    values, column_count})
                .has_value();
        } catch (...) {
            success = false;
        }
    }
capture_cleanup:
    for (std::size_t index = 0; index < column_count; ++index) {
        if (encoded[index] != nullptr) {
            pfree(encoded[index]);
        }
    }
    pfree(encoded);
    pfree(values);
    if (success) {
        state.logical_bytes += row_logical_bytes;
        ++state.row_count;
    }
    return success;
}

[[nodiscard]] bool capture_coverage_row(
        ReusableCoverageState& state, TupleTableSlot* slot) {
    if (state.provider == nullptr || state.plan_domain_nodes == NIL
        || state.provider->extract_occurrence == nullptr || slot == nullptr) {
        return false;
    }
    BioseqCacheOccurrenceV2 occurrence{
        .struct_size = sizeof(BioseqCacheOccurrenceV2),
    };
    reuse::ProviderCallDiagnosticV1 diagnostic{};
    return reuse::invoke_extract_occurrence_fail_open(
                state.provider,
                &state.provider_lease,
                state.plan_domain_nodes,
                slot,
                &occurrence,
                state.css.ss.ps.state->es_query_cxt,
                &diagnostic)
        && capture_coverage_row_with_occurrence(
            state, slot, occurrence);
}

/** Seal full merged output, publish storage, then make registry COMPLETE last. */
[[nodiscard]] bool complete_coverage_producer(
        ReusableCoverageState& state) {
    const CoverageDependencyTicketV1 dependency_ticket =
        coverage_dependency_ticket(
            state.current_contract.domain, state.provider);
    if (!state.current_producer_owned || state.column_builder == nullptr
        || state.column_builder->row_count() != state.row_count
        || !dependency_ticket.valid
        || state.dependency_fingerprint != dependency_ticket.fingerprint
        || state.dependency_physical_identity
            != dependency_ticket.physical_identity
        || pg_atomic_read_u32(&g_shared->coarse_transition_count) != 0) {
        return false;
    }
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_shared != nullptr
        && pg_atomic_exchange_u32(
            &g_shared->test_control.fail_next_write, 0) != 0) {
        return false;
    }
#endif
    std::optional<PostgreSqlStreamingColumnStoreBuilder::result_type> blocks;
    try {
        auto result = state.column_builder->finalize();
        if (!result.has_value()) {
            return false;
        }
        blocks.emplace(std::move(*result));
    } catch (...) {
        return false;
    }
    state.payload_bytes = blocks->logical_bytes();
    if (state.payload_bytes == 0
        || state.payload_bytes > kMaximumStoredResultBytes
        || blocks->row_count() != state.row_count) {
        return false;
    }
    ReusableGenerationSnapshotV1 storage_snapshot{};
    if (!snapshot_current_reusable_generation(
            state.current_reference, &storage_snapshot)
        || storage_snapshot.reference != state.current_reference
        || storage_snapshot.entry_kind != CacheEntryKind::ReusableCoverage
        || storage_snapshot.state != CacheEntryState::Building
        || storage_snapshot.producer
            != current_reusable_producer_identity()) {
        return false;
    }
    const storage::ManifestDescriptor descriptor{
        .id = state.current_reference.id,
        .kind = CacheEntryKind::ReusableCoverage,
        .logical_bytes = state.logical_bytes,
        .row_count = state.row_count,
        .created_at_us = state.producer_started_at,
        .expires_at_us = 0,
    };
    storage::PublishManifestResult publication =
        storage::publish_streamed_column_store(
            descriptor, *state.column_sink, *blocks);
    if (!publication) {
        return false;
    }
    destroy_coverage_builder(state);
    const VisibilityTicket refreshed = mint_visibility_ticket(
        storage_snapshot.metadata.observability,
        VisibilityTicketPurposeV1::Continuation);
    const std::uint64_t continuing_completion_count =
        storage_snapshot.conservative_guard.snapshot_completion_count;
    if (continuing_completion_count != 0
        && continuing_completion_count
            != state.visibility_completion_count) {
        static_cast<void>(storage::retire_manifest(publication.handle));
        mark_shared_cache_failed();
        return false;
    }
    RuntimeDependencyPublishContextV1 context{
        .request = {
            .reference = state.current_reference,
            .manifest = reusable_manifest_handle(publication.handle),
            .completed_monotonic_ns = reusable_monotonic_now_ns(),
            .row_count = state.row_count,
            .logical_bytes = state.logical_bytes,
            .stored_bytes = state.payload_bytes,
        },
        .expected_kind = CacheEntryKind::ReusableCoverage,
        .expected_producer = current_reusable_producer_identity(),
        .expected_dependency_count =
            storage_snapshot.metadata.dependency_count,
    };
    const dependency_runtime::DependencyRuntimeResultV1 validated =
        refreshed.valid
        ? validate_reusable_generation_dependencies_locked(
            storage_snapshot,
            refreshed.completion_count,
            dependency_runtime::DependencyTicketLockedActionV1{
                .context = &context,
                .apply = apply_runtime_dependency_publish,
                .rollback = rollback_runtime_dependency_publish,
            })
        : dependency_runtime::DependencyRuntimeResultV1{
            .error = dependency_runtime::DependencyRuntimeErrorV1::
                ReferenceValidationFailed,
            .fail_open = true,
        };
    if (context.action_applied) {
        broadcast_reusable_generation(state.current_reference);
    }
    if (!validated || context.rolled_back
        || context.registry_result != ReusableRegistryErrorV1::None) {
        if (!context.action_applied) {
            static_cast<void>(storage::retire_manifest(publication.handle));
        }
        return false;
    }
    state.manifest_handle = publication.handle;
    state.producer_completed = true;
    state.current_producer_owned = false;
    state.runtime_role = CoverageRuntimeRoleV1::Bypass;
    if (g_shared != nullptr) {
        pg_atomic_fetch_add_u64(&g_shared->stats.produced, 1);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.logical_bytes, state.logical_bytes);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.stored_bytes, state.payload_bytes);
    }
    return true;
}

struct BoundedDiagnosticTextV1 {
    char* data = nullptr;
    std::size_t size = 0;
    bool available = false;
    bool truncated = false;
    ReusablePlanSnapshotStatusV1 status =
        ReusablePlanSnapshotStatusV1::Unavailable;
};

inline constexpr int kDiagnosticDisplaySqlTruncatedV1 = 1 << 0;
inline constexpr int kDiagnosticCanonicalPlanTruncatedV1 = 1 << 1;
inline constexpr int kDiagnosticCanonicalPlanUnavailableV1 = 1 << 2;

/** Copy diagnostics into planner/query memory with an explicit byte bound. */
[[nodiscard]] bool copy_bounded_diagnostic_text(
        const char* source,
        std::size_t capacity,
        BoundedDiagnosticTextV1* output) {
    if (source == nullptr || capacity == 0 || output == nullptr) {
        return false;
    }
    const std::size_t observed = strnlen(source, capacity + 1U);
    const std::size_t copied = std::min(observed, capacity);
    output->data = pnstrdup(source, copied);
    output->size = copied;
    output->available = copied != 0;
    output->truncated = observed > capacity;
    return output->available;
}

/** Reuse one resettable backend context for transient serialization trees. */
[[nodiscard]] MemoryContext reusable_plan_capture_context() {
    if (g_plan_capture_context == nullptr) {
        MemoryContext previous = MemoryContextSwitchTo(TopMemoryContext);
        g_plan_capture_context = AllocSetContextCreate(
            TopMemoryContext,
            "bioseq reusable plan capture",
            ALLOCSET_SMALL_SIZES);
        MemoryContextSwitchTo(previous);
    }
    return g_plan_capture_context;
}

/** Publish status only; unavailable diagnostics never expose partial bytes. */
void set_plan_capture_unavailable(
        BoundedDiagnosticTextV1* output) noexcept {
    if (output == nullptr) {
        return;
    }
    *output = {};
    output->status = ReusablePlanSnapshotStatusV1::Unavailable;
}

/** Canonical child Plan text is diagnostics only and fails open. */
[[nodiscard]] bool capture_canonical_child_plan_fail_open(
        const Plan* child,
        std::size_t capacity,
        BoundedDiagnosticTextV1* output) {
    if (child == nullptr || capacity == 0 || output == nullptr
        || g_producer_plan_capture_active) {
        return false;
    }
    *output = {};
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool capture_active = false;
    PG_TRY();
    {
        MemoryContext capture_context = reusable_plan_capture_context();
        MemoryContextReset(capture_context);
        MemoryContextSwitchTo(capture_context);
        g_producer_plan_capture_active = true;
        capture_active = true;
        char* serialized = nodeToString(child);
        MemoryContextSwitchTo(caller_context);
        const bool copied = copy_bounded_diagnostic_text(
            serialized,
            capacity,
            output);
        if (copied) {
            output->status = ReusablePlanSnapshotStatusV1::Planned;
        }
        g_producer_plan_capture_active = false;
        capture_active = false;
        MemoryContextReset(capture_context);
        if (!copied) {
            set_plan_capture_unavailable(output);
        }
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (capture_active) {
            g_producer_plan_capture_active = false;
            capture_active = false;
        }
        if (g_plan_capture_context != nullptr) {
            MemoryContextReset(g_plan_capture_context);
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        set_plan_capture_unavailable(output);
    }
    PG_END_TRY();
    return output->status == ReusablePlanSnapshotStatusV1::Planned;
}

/** Render an initialized producer child without mutating the live QueryDesc. */
[[nodiscard]] bool capture_initialized_child_plan_fail_open(
        CustomScanState* node,
        EState* estate,
        const char* display_sql,
        bool analyze,
        std::size_t capacity,
        BoundedDiagnosticTextV1* output) {
    PlanState* child_state = node == nullptr
        ? nullptr
        : coverage_retained_child(node);
    auto* scan = node == nullptr
        ? nullptr
        : reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    Plan* child_plan = scan == nullptr || scan->custom_plans == NIL
        ? nullptr
        : static_cast<Plan*>(linitial(scan->custom_plans));
    if (child_state == nullptr || child_plan == nullptr || estate == nullptr
        || estate->es_plannedstmt == nullptr || capacity == 0
        || output == nullptr
        || g_producer_plan_capture_active) {
        return false;
    }
    *output = {};
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool capture_active = false;
    PG_TRY();
    {
        MemoryContext capture_context = reusable_plan_capture_context();
        MemoryContextReset(capture_context);
        MemoryContextSwitchTo(capture_context);
        g_producer_plan_capture_active = true;
        capture_active = true;
        PlannedStmt planned = *estate->es_plannedstmt;
        planned.planTree = child_plan;
        QueryDesc view{};
        view.operation = planned.commandType;
        view.plannedstmt = &planned;
        view.sourceText = display_sql;
        view.snapshot = estate->es_snapshot;
        view.crosscheck_snapshot = estate->es_crosscheck_snapshot;
        view.params = estate->es_param_list_info;
        view.queryEnv = estate->es_queryEnv;
        view.instrument_options = estate->es_instrument;
        view.tupDesc = child_state->ps_ResultTupleDesc;
        view.estate = estate;
        view.planstate = child_state;
        view.already_executed = analyze;

        ExplainState* explain = NewExplainState();
        explain->format = EXPLAIN_FORMAT_TEXT;
        explain->verbose = true;
        explain->costs = true;
        explain->analyze = analyze;
        explain->buffers = analyze
            && (estate->es_instrument & INSTRUMENT_BUFFERS) != 0;
        explain->wal = analyze
            && (estate->es_instrument & INSTRUMENT_WAL) != 0;
        explain->timing = analyze
            && (estate->es_instrument & INSTRUMENT_TIMER) != 0;
        explain->summary = false;
        explain->settings = false;
        ExplainBeginOutput(explain);
        ExplainPrintPlan(explain, &view);
        ExplainEndOutput(explain);
        MemoryContextSwitchTo(caller_context);
        bool copied = false;
        if (explain->str != nullptr && explain->str->data != nullptr) {
            copied = copy_bounded_diagnostic_text(
                explain->str->data,
                capacity,
                output);
        }
        if (copied) {
            output->status = analyze
                ? ReusablePlanSnapshotStatusV1::Analyzed
                : ReusablePlanSnapshotStatusV1::Planned;
        }
        g_producer_plan_capture_active = false;
        capture_active = false;
        MemoryContextReset(capture_context);
        if (!copied) {
            set_plan_capture_unavailable(output);
        }
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (capture_active) {
            g_producer_plan_capture_active = false;
            capture_active = false;
        }
        if (g_plan_capture_context != nullptr) {
            MemoryContextReset(g_plan_capture_context);
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        set_plan_capture_unavailable(output);
    }
    PG_END_TRY();
    return output->status == (analyze
        ? ReusablePlanSnapshotStatusV1::Analyzed
        : ReusablePlanSnapshotStatusV1::Planned);
}

void initialize_coverage_child(
        ReusableCoverageState& state,
        CustomScanState* node,
        EState* estate,
        int eflags) {
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    if (scan->custom_plans == NIL || list_length(scan->custom_plans) != 1
        || node->custom_ps != NIL) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan retained child is malformed")));
    }
    Plan* child_plan = static_cast<Plan*>(linitial(scan->custom_plans));
    node->custom_ps = list_make1(ExecInitNode(child_plan, estate, eflags));
    state.child_initialized = true;
}

/** Claim exact-current work before any retained provider row can execute. */
/** Coverage defaults to bypass until main-tree finalization clears its marker. */
[[nodiscard]] bool coverage_force_bypass(
        const CustomScan* scan) noexcept {
    return scan == nullptr || scan->custom_private == NIL
        || list_length(scan->custom_private) < 11
        || !IsA(list_nth(scan->custom_private, 10), Integer)
        || intVal(list_nth(scan->custom_private, 10))
            != static_cast<int>(CoverageBypassMarkerV1::Enabled);
}

void coverage_begin_custom_scan(
        CustomScanState* node, EState* estate, int eflags) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(node);
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    state->child_initialized = false;
    state->runtime_recipe_valid = false;
    state->current_producer_owned = false;
    state->current_reference_owned = false;
    state->producer_completed = false;
    state->capture_failed = false;
    state->cleanup_done = false;
    state->current_manifest_pin_owned = false;
    state->current_waited = false;
    state->current_ttl_entitlement_consumed = false;
    state->producer_spool_failed = false;
    state->older_preflight_ready = false;
    state->planned_diagnostics_published = false;
    state->replaying_materialized_output = false;
    state->output_materialized_complete = false;
    state->source_count = 0;
    state->active_source_count = 0;
    state->replay_source_index = 0;
    state->runtime_role = CoverageRuntimeRoleV1::Bypass;
    state->provider = nullptr;
    state->provider_lease = {};
    state->current_header = {};
    state->dependency_fingerprint = {};
    state->dependency_physical_identity = {};
    state->schema_fingerprint = {};
    state->mapping_fingerprint = {};
    state->lineage_fingerprint = {};
    state->current_contract = {};
    state->current_reference = {};
    state->current_reference_token = {};
    state->manifest_handle = {};
    state->current_manifest_pin = {};
    state->stored_tokens = {};
    state->stored_token_count = 0;
    state->row_count = 0;
    state->logical_bytes = 0;
    state->payload_bytes = 0;
    state->producer_ttl_ms = 0;
    state->selection_bitmap_bytes_admitted = 0;
    state->occurrence_owner_count = 0;
    state->producer_spool_rows = 0;
    state->producer_spool_bytes = 0;
    state->visibility_completion_count = 0;
    state->producer_started_at = 0;
    state->column_checksum_policy = nullptr;
    state->column_sink = nullptr;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->stored_descriptor = nullptr;
    state->stored_columns = nullptr;
    state->stored_column_count = 0;
    state->plan_domain_nodes = NIL;
    state->current_replay_store = nullptr;
    state->current_replay_slot = nullptr;
    state->producer_output_store = nullptr;
    state->producer_output_slot = nullptr;
    state->predicate_descriptor = nullptr;
    state->predicate_slot = nullptr;
    state->predicate_consumer_attnos = nullptr;
    state->predicate_column_count = 0;
    state->reserved_predicate = 0;
    state->requested_predicate_state = nullptr;
    state->requested_predicate_attnos = nullptr;
    state->occurrence_owners = nullptr;
    for (CoverageRuntimeSourceV1& source : state->sources) {
        source = CoverageRuntimeSourceV1{};
    }
    node->custom_ps = NIL;
    if (!state->cleanup_registered) {
        state->cleanup_callback.func = coverage_state_cleanup;
        state->cleanup_callback.arg = state;
        state->cleanup_callback.next = nullptr;
        MemoryContextRegisterResetCallback(
            estate->es_query_cxt, &state->cleanup_callback);
        state->cleanup_registered = true;
    }
    link_backend_query_cleanup(
        state->backend_cleanup, state, coverage_state_cleanup);

    const bool explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
    if (coverage_force_bypass(scan)) {
        initialize_coverage_child(*state, node, estate, eflags);
        if (!explain_only && g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
        }
        return;
    }

    maybe_pause(TestPausePoint::BeforeCoveragePreflight);

    List* candidates = scan->custom_private != NIL
            && list_length(scan->custom_private) >= 4
        ? static_cast<List*>(list_nth(scan->custom_private, 3))
        : NIL;
    const int older_source_count = list_length(candidates);
    const int source_count = older_source_count + 1;
    if (older_source_count < 0
        || source_count > static_cast<int>(kMaxSourcesPerCoverageNode)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan source count is invalid")));
    }
    state->source_count = static_cast<std::uint16_t>(source_count);

    std::array<int, kMaxSourcesPerCoverageNode> gate_ids{};
    if (scan->custom_private == NIL
        || list_length(scan->custom_private) < 7
        || !IsA(list_nth(scan->custom_private, 5), Integer)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan current gate is invalid")));
    }
    int gate_index = 0;
    gate_ids[gate_index++] = intVal(list_nth(scan->custom_private, 5));
    ListCell* cell = nullptr;
    foreach(cell, candidates) {
        const auto* source = static_cast<const List*>(lfirst(cell));
        if (source == nullptr || list_length(source) != 7
            || !IsA(list_nth(source, 5), Integer)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqReusableCoverageScan source metadata is invalid")));
        }
        gate_ids[gate_index++] = intVal(list_nth(source, 5));
    }

    reuse::ReuseGateFreezeV1 closed_gates{};
    if (!reuse::begin_gate_preflight(closed_gates, source_count)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan gates are malformed")));
    }
    reuse::freeze_all_sources_invalid(closed_gates);
    const std::span<const int> gate_span{
        gate_ids.data(), static_cast<std::size_t>(source_count)};
    if (!reuse::publish_frozen_gates(estate, gate_span, closed_gates)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan gates cannot be frozen")));
    }

    if (g_producer_plan_capture_active) {
        initialize_coverage_child(*state, node, estate, eflags);
        return;
    }

    const int provider_id = scan->custom_private != NIL
            && list_length(scan->custom_private) >= 2
            && IsA(list_nth(scan->custom_private, 1), Integer)
        ? intVal(list_nth(scan->custom_private, 1))
        : 0;
    const provider_registry::ProviderV2* provider = provider_id > 0
        ? provider_registry::find_provider_v2(
            static_cast<std::uint32_t>(provider_id))
        : nullptr;
    if (provider != nullptr
        && !provider_registry::acquire_provider_lease_v2(
            provider, &state->provider_lease)) {
        provider = nullptr;
    }
    state->provider = provider;
    List* recipe_private = scan->custom_private != NIL
            && list_length(scan->custom_private) >= 7
        ? static_cast<List*>(list_nth(scan->custom_private, 6))
        : NIL;
    if (provider != nullptr && recipe_private != NIL
        && list_length(recipe_private) == 4
        && IsA(list_nth(recipe_private, 0), Integer)
        && intVal(list_nth(recipe_private, 0)) != 0
        && IsA(list_nth(recipe_private, 3), Integer)) {
        const auto expected = binary_node_payload(
            list_nth(recipe_private, 2));
        if (expected.size() == CacheDigest256{}.bytes.size()) {
            BioseqCachePlanDigestRecipeV2 recipe{
                .struct_size = sizeof(BioseqCachePlanDigestRecipeV2),
                .flags = 0,
                .digest_format_version = static_cast<std::uint32_t>(
                    intVal(list_nth(recipe_private, 3))),
                .reserved = 0,
                .plan_domain_nodes = static_cast<List*>(
                    list_nth(recipe_private, 1)),
            };
            std::memcpy(
                recipe.expected_generation_digest,
                expected.data(),
                expected.size());
            state->plan_domain_nodes = recipe.plan_domain_nodes;
            reuse::ProviderCallDiagnosticV1 diagnostic{};
            state->runtime_recipe_valid =
                reuse::validate_runtime_recipe_fail_open(
                    provider,
                    &state->provider_lease,
                    &recipe,
                    estate,
                    estate->es_query_cxt,
                &diagnostic);
        }
    }

    const bool diagnostic_private_valid = scan->custom_private != NIL
        && list_length(scan->custom_private) >= 11
        && IsA(list_nth(scan->custom_private, 7), String)
        && IsA(list_nth(scan->custom_private, 8), String)
        && IsA(list_nth(scan->custom_private, 9), Integer)
        && IsA(list_nth(scan->custom_private, 10), Integer);
    const bool force_bypass = coverage_force_bypass(scan);

    TupleDesc raw_descriptor = node->ss.ss_ScanTupleSlot == nullptr
        ? nullptr
        : node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
    CoverageCurrentPlanViewV1 current_plan{};
    const std::span<const std::byte> current_plan_bytes =
        scan->custom_private != NIL && list_length(scan->custom_private) >= 3
        ? binary_node_payload(list_nth(scan->custom_private, 2))
        : std::span<const std::byte>{};
    const bool current_contract_ready = !explain_only
        && provider != nullptr
        && decode_current_coverage_plan(current_plan_bytes, &current_plan)
        && current_plan.header.provider_id == provider->provider_id
        && current_plan.header.provider_abi_version
            == provider->abi_version
        && current_plan.header.compatibility_family_id != 0
        && current_plan.header.capability_mask != 0
        && decode_coverage_descriptor_fail_open(
            current_plan.coverage_descriptor,
            estate->es_query_cxt,
            &state->current_contract)
        && reuse::provider_security_digest(state->current_contract.domain)
            == current_plan.header.coverage_directory_key
                .security_namespace.provider_security_digest
        && state->current_contract.domain.provider_id
            == current_plan.header.provider_id
        && state->current_contract.domain.compatibility_family_id
            == current_plan.header.compatibility_family_id
        && state->current_contract.domain.capability_mask
            == current_plan.header.capability_mask;
    const bool predicate_slot_ready = current_contract_ready
        && initialize_coverage_predicate_slot(
            *state, estate, raw_descriptor);
    const bool predicate_states_ready = !explain_only
        && predicate_slot_ready
        && compile_coverage_predicates(
            *state, *scan, state->predicate_descriptor);
    const bool runtime_allowed = !explain_only
        && !force_bypass
        && current_contract_ready
        && predicate_states_ready
        && state->runtime_recipe_valid
        && state->plan_domain_nodes != NIL
        && provider != nullptr
        && (provider->provider_flags & BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE)
            != 0
        && (provider->provider_flags
                & BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION) != 0
        && provider->extract_occurrence != nullptr
        && guc::mode() == QueryCacheMode::Reuse
        && XactIsoLevel == XACT_READ_COMMITTED
        && shared_cache_ready()
        && scan->custom_private != NIL
        && diagnostic_private_valid;
    const VisibilityTicket visibility = runtime_allowed
        ? mint_visibility_ticket(WriteObservability::ProviderComplete)
        : VisibilityTicket{};
    bool runtime_selected = false;
    if (runtime_allowed && visibility.valid) {
        if (current_contract_ready) {
            state->current_header = current_plan.header;
            const CoverageDependencyTicketV1 dependency =
                coverage_dependency_ticket(
                    state->current_contract.domain,
                    provider,
                    visibility.completion_count);
            const CoverageSemanticFingerprintsV1 fingerprints =
                coverage_semantic_fingerprints(
                    state->current_contract.domain);
            if (dependency.valid && fingerprints.valid
                && pg_atomic_read_u32(
                    &g_shared->coarse_transition_count) == 0
                && provider_visibility_ticket_still_bound(visibility)) {
                state->dependency_fingerprint = dependency.fingerprint;
                state->dependency_physical_identity =
                    dependency.physical_identity;
                state->visibility_completion_count =
                    visibility.completion_count;
                state->schema_fingerprint = fingerprints.schema;
                state->mapping_fingerprint = fingerprints.mapping;
                state->lineage_fingerprint = fingerprints.lineage;
                const std::uint64_t ttl_ms = static_cast<std::uint64_t>(
                    guc::ttl_ms());
                state->producer_ttl_ms = ttl_ms;
                const ReusableProducerIdentityV1 owner =
                    current_reusable_producer_identity();
                ReusableGenerationMetadataV1 metadata{
                    .provider_id = provider->provider_id,
                    .provider_abi_version = provider->abi_version,
                    .provider_domain_abi_version =
                        kCoverageDescriptorFormatV1,
                    .dependency_count = dependency.dependency_count,
                    .compatibility_family_id =
                        current_plan.header.compatibility_family_id,
                    .capability_mask =
                        current_plan.header.capability_mask,
                    .schema_fingerprint = fingerprints.schema,
                    .mapping_fingerprint = fingerprints.mapping,
                    .lineage_fingerprint = fingerprints.lineage,
                    .dependency_fingerprint = dependency.fingerprint,
                    .physical_identity = dependency.physical_identity,
                    .observability = WriteObservability::ProviderComplete,
                    .estimated_covered_rows = static_cast<std::uint64_t>(
                        scan->scan.plan.plan_rows > 0
                            ? scan->scan.plan.plan_rows
                            : 0),
                    .producer_ttl_ms = ttl_ms,
                };
                const ReusableClaimRequestV1 request{
                    .coverage_directory_key =
                        current_plan.header.coverage_directory_key,
                    .generation_digest =
                        current_plan.header.generation_digest,
                    .canonical_domain_key =
                        current_plan.canonical_domain_key,
                    .canonical_generation_key =
                        current_plan.canonical_generation_key,
                    .coverage_descriptor =
                        current_plan.coverage_descriptor,
                    .metadata = metadata,
                    .producer = owner,
                    .reference_owner = owner,
                    .started_monotonic_ns = reusable_monotonic_now_ns(),
                    .consumer_ttl_ms = ttl_ms,
                    .pin_complete = true,
                    .register_waiter = true,
                };
                const DependencyKey definition_guard_key{
                    .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
                    .object_oid = static_cast<std::uint32_t>(MyDatabaseId),
                    .kind = DependencyKind::DatabaseDefinitionEpoch,
                };
                CacheDigest256 canonical_dependency_fingerprint{};
                const ReusableClaimResultV1 claimed =
                    provider_visibility_ticket_still_bound(visibility)
                    ? claim_reusable_generation_with_dependencies(
                        request,
                        std::span<const DependencyVersion>{
                            &dependency.version, 1},
                        visibility.completion_count,
                        &definition_guard_key,
                        &canonical_dependency_fingerprint)
                    : ReusableClaimResultV1{};
                if (claimed) {
                    metadata.dependency_fingerprint =
                        canonical_dependency_fingerprint;
                    state->current_reference = claimed.reference;
                    state->current_reference_token = claimed.token;
                    state->current_reference_owned =
                        claimed.token.active;
                    if (claimed.disposition
                            == ReusableClaimDispositionV1::ClaimedProducer) {
                        state->runtime_role =
                            CoverageRuntimeRoleV1::Producer;
                        state->current_producer_owned = true;
                        state->producer_started_at = GetCurrentTimestamp();
                        if (register_executor_start_claim(*state)
                            && begin_coverage_capture(*state)
                            && begin_coverage_output_spool(
                                *state, raw_descriptor)) {
                            PG_TRY();
                            {
                                const bool older_ready =
                                    prepare_older_coverage_sources_fail_open(
                                        *state,
                                        *scan,
                                        current_plan,
                                        raw_descriptor,
                                        visibility);
                                bool reused_older = false;
                                if (older_ready) {
                                    reuse::ReuseGateFreezeV1 source_gates{};
                                    bool staged = reuse::begin_gate_preflight(
                                        source_gates, source_count);
                                    for (std::size_t source_index = 0;
                                         staged
                                            && source_index
                                                < state->sources.size();
                                         ++source_index) {
                                        if (state->sources[source_index].valid) {
                                            staged = reuse::stage_valid_source(
                                                source_gates,
                                                source_index + 1U);
                                        }
                                    }
                                    staged = staged
                                        && reuse::freeze_source_decisions(
                                            source_gates)
                                        && provider_visibility_ticket_still_bound(
                                            visibility)
                                        && reuse::publish_frozen_gates(
                                            estate, gate_span, source_gates);
                                    if (!staged) {
                                        release_all_older_coverage_sources(
                                            *state);
                                    } else {
                                        reused_older = true;
                                    }
                                }
                                if (reused_older) {
                                    maybe_pause(
                                        TestPausePoint::
                                            AfterCoverageReaderPinned);
                                }
                                initialize_coverage_child(
                                    *state, node, estate, eflags);
                                if (reused_older && g_shared != nullptr) {
                                        pg_atomic_fetch_add_u64(
                                            &g_shared->stats.reuse_hits, 1);
                                }
                            }
                            PG_CATCH();
                            {
                                fail_coverage_producer(*state);
                                PG_RE_THROW();
                            }
                            PG_END_TRY();
                            runtime_selected = true;
                        } else {
                            if (state->current_producer_owned) {
                                fail_coverage_producer(*state);
                            }
                        }
                    } else {
                        ReusableGenerationSnapshotV1 completed{};
                        bool complete = false;
                        if (claimed.disposition
                                == ReusableClaimDispositionV1::ExistingComplete) {
                            const bool snapshotted =
                                snapshot_current_reusable_generation(
                                    state->current_reference, &completed);
                            const bool semantic_identity_matches =
                                snapshotted
                                && completed.state
                                    == CacheEntryState::Complete
                                && current_reusable_snapshot_matches(
                                    completed, current_plan, metadata);
                            const bool ttl_readable = snapshotted
                                && current_reusable_ttl_decision(
                                    *state, completed).readable;
                            const VisibilityTicket refreshed = snapshotted
                                ? mint_visibility_ticket(
                                    completed.metadata.observability,
                                    VisibilityTicketPurposeV1::Continuation)
                                : VisibilityTicket{};
                            const dependency_runtime::
                                DependencyRuntimeResultV1 validated =
                                semantic_identity_matches && refreshed.valid
                                ? validate_reusable_generation_dependencies_locked(
                                    completed,
                                    refreshed.completion_count)
                                : dependency_runtime::
                                    DependencyRuntimeResultV1{
                                        .error = dependency_runtime::
                                            DependencyRuntimeErrorV1::
                                                ReferenceValidationFailed,
                                        .fail_open = true,
                                    };
                            complete = semantic_identity_matches
                                && ttl_readable
                                && refreshed.valid
                                && validated;
                        } else if (claimed.disposition
                                == ReusableClaimDispositionV1::ExistingInFlight) {
                            state->current_waited = true;
                            if (g_shared != nullptr) {
                                pg_atomic_fetch_add_u64(
                                    &g_shared->stats.inflight_waits, 1);
                            }
                            maybe_pause(
                                TestPausePoint::AfterWaiterRegistered);
                            complete = wait_for_current_reusable_generation(
                                *state,
                                current_plan,
                                metadata,
                                visibility,
                                &completed);
                        }
                        if (complete) {
                            MemoryContext caller_context = CurrentMemoryContext;
                            volatile bool prepared = false;
                            PG_TRY();
                            {
                                prepared = prepare_current_coverage_consumer(
                                    *state,
                                    current_plan,
                                    metadata,
                                    completed,
                                    raw_descriptor);
                            }
                            PG_CATCH();
                            {
                                const int sqlstate = geterrcode();
                                MemoryContextSwitchTo(caller_context);
                                release_current_coverage_consumer(*state);
                                if (cache_interrupt_error(sqlstate)) {
                                    PG_RE_THROW();
                                }
                                FlushErrorState();
                                prepared = false;
                            }
                            PG_END_TRY();
                            if (prepared
                                && provider_visibility_ticket_still_bound(
                                    visibility)) {
                                reuse::ReuseGateFreezeV1 hit_gates{};
                                if (reuse::begin_gate_preflight(
                                        hit_gates, source_count)
                                    && reuse::stage_valid_source(hit_gates, 0)
                                    && reuse::freeze_source_decisions(hit_gates)
                                    && reuse::publish_frozen_gates(
                                        estate, gate_span, hit_gates)) {
                                    if (g_shared != nullptr) {
                                        pg_atomic_fetch_add_u64(
                                            &g_shared->stats.reuse_hits, 1);
                                    }
                                    runtime_selected = true;
                                }
                            }
                        }
                        if (!runtime_selected) {
                            release_current_coverage_consumer(*state);
                        }
                    }
                }
            }
        }
    }
    if (!runtime_selected && !state->child_initialized) {
        initialize_coverage_child(*state, node, estate, eflags);
    }
    if (!explain_only && !runtime_selected && g_shared != nullptr) {
        pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
    }
}

[[nodiscard]] TupleTableSlot* project_coverage_tuple(
        CustomScanState* node, TupleTableSlot* raw) {
    ExprContext* expression_context = node->ss.ps.ps_ExprContext;
    ResetExprContext(expression_context);
    expression_context->ecxt_scantuple = raw;
    return node->ss.ps.ps_ProjInfo == nullptr
        ? raw
        : ExecProject(node->ss.ps.ps_ProjInfo);
}

[[nodiscard]] TupleTableSlot* next_current_coverage_row(
        ReusableCoverageState& state,
        CustomScanState* node) {
    if (state.current_replay_store == nullptr
        || state.current_replay_slot == nullptr
        || node->ss.ss_ScanTupleSlot == nullptr
        || !tuplestore_gettupleslot(
            state.current_replay_store,
            true,
            false,
            state.current_replay_slot)) {
        return nullptr;
    }
    if (!coverage_stored_row_valid(
            state, state.current_replay_slot, state.row_count)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan replay invariant failed")));
    }
    TupleTableSlot* raw = node->ss.ss_ScanTupleSlot;
    TupleDesc raw_descriptor = raw->tts_tupleDescriptor;
    slot_getallattrs(state.current_replay_slot);
    ExecClearTuple(raw);
    for (int index = 0; index < raw_descriptor->natts; ++index) {
        raw->tts_values[index] = static_cast<Datum>(0);
        raw->tts_isnull[index] = true;
    }
    for (std::uint32_t index = 0;
         index < state.stored_column_count; ++index) {
        const CoverageStoredColumnV1& column = state.stored_columns[index];
        if (column.kind != CoverageStoredColumnKindV1::ProviderAttribute) {
            continue;
        }
        const std::size_t raw_index = column.provider_attno - 1U;
        raw->tts_values[raw_index] =
            state.current_replay_slot->tts_values[index];
        raw->tts_isnull[raw_index] =
            state.current_replay_slot->tts_isnull[index];
    }
    raw->tts_nvalid = raw_descriptor->natts;
    ExecStoreVirtualTuple(raw);
    ++state.row_count;
    return raw;
}

[[nodiscard]] TupleTableSlot* next_selected_older_coverage_row(
        ReusableCoverageState& state,
        CustomScanState* node,
        BioseqCacheOccurrenceV2* occurrence) {
    if (node->ss.ss_ScanTupleSlot == nullptr || occurrence == nullptr) {
        return nullptr;
    }
    const std::size_t older_source_count = state.source_count == 0
        ? 0
        : static_cast<std::size_t>(state.source_count - 1U);
    while (state.replay_source_index < older_source_count) {
        CoverageRuntimeSourceV1& source =
            state.sources[state.replay_source_index];
        if (!source.valid) {
            ++state.replay_source_index;
            continue;
        }
        while (tuplestore_gettupleslot(
                source.replay_store, true, false, source.replay_slot)) {
            const std::uint64_t row = source.replay_row_index++;
            if (!coverage_selection_bit(source, row)) {
                ExecClearTuple(source.replay_slot);
                continue;
            }
            if (!coverage_stored_row_valid(
                    source.stored_columns,
                    source.stored_column_count,
                    state.current_header.capability_mask,
                    source.replay_slot,
                    row,
                    occurrence)
                || !populate_consumer_slot_from_source(
                    source,
                    source.replay_slot,
                    node->ss.ss_ScanTupleSlot)) {
                ereport(ERROR,
                    (errcode(ERRCODE_DATA_CORRUPTED),
                     errmsg("BioseqReusableCoverageScan older replay invariant failed")));
            }
            return node->ss.ss_ScanTupleSlot;
        }
        if (source.replay_row_index != source.row_count) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqReusableCoverageScan older row count changed")));
        }
        ExecClearTuple(source.replay_slot);
        ++state.replay_source_index;
    }
    return nullptr;
}

[[nodiscard]] TupleTableSlot* next_materialized_coverage_output(
        ReusableCoverageState& state) {
    if (!state.replaying_materialized_output
        || state.producer_output_store == nullptr
        || state.producer_output_slot == nullptr
        || !tuplestore_gettupleslot(
            state.producer_output_store,
            true,
            false,
            state.producer_output_slot)) {
        return nullptr;
    }
    return state.producer_output_slot;
}

/** Copy one visible raw tuple into a bounded optional rescan spool. */
[[nodiscard]] bool spool_coverage_output(
        ReusableCoverageState& state,
        TupleTableSlot* raw) {
    if (state.producer_spool_failed
        || state.producer_output_store == nullptr || raw == nullptr
        || TupIsNull(raw)) {
        release_coverage_output_spool(state);
        state.producer_spool_failed = true;
        return false;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile MinimalTuple measured_tuple = nullptr;
    volatile bool spooled = false;
    PG_TRY();
    {
        measured_tuple = ExecCopySlotMinimalTuple(raw);
        const std::uint64_t tuple_bytes = measured_tuple == nullptr
            ? 0
            : static_cast<std::uint64_t>(measured_tuple->t_len);
        const bool admitted = tuple_bytes != 0
            && state.producer_spool_rows
                < kMaximumCoverageProducerSpoolRowsV1
            && state.producer_spool_bytes
                <= kMaximumCoverageProducerSpoolBytesV1
            && tuple_bytes <= kMaximumCoverageProducerSpoolBytesV1
                - state.producer_spool_bytes;
        if (admitted) {
            tuplestore_puttupleslot(state.producer_output_store, raw);
            ++state.producer_spool_rows;
            state.producer_spool_bytes += tuple_bytes;
            spooled = true;
        }
        if (measured_tuple != nullptr) {
            heap_free_minimal_tuple(measured_tuple);
            measured_tuple = nullptr;
        }
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (measured_tuple != nullptr) {
            heap_free_minimal_tuple(measured_tuple);
            measured_tuple = nullptr;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        spooled = false;
    }
    PG_END_TRY();
    if (!spooled) {
        release_coverage_output_spool(state);
        state.producer_spool_failed = true;
    }
    return spooled;
}

[[nodiscard]] bool capture_coverage_row_fail_open(
        ReusableCoverageState& state,
        TupleTableSlot* raw) {
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool captured = false;
    PG_TRY();
    {
        captured = capture_coverage_row(state, raw);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            fail_coverage_producer(state);
            PG_RE_THROW();
        }
        FlushErrorState();
        captured = false;
    }
    PG_END_TRY();
    return captured;
}

[[nodiscard]] bool capture_cached_coverage_row_fail_open(
        ReusableCoverageState& state,
        TupleTableSlot* raw,
        const BioseqCacheOccurrenceV2& occurrence) {
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool captured = false;
    PG_TRY();
    {
        captured = capture_coverage_row_with_occurrence(
            state, raw, occurrence);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            abort_coverage_capture(state);
            PG_RE_THROW();
        }
        FlushErrorState();
        captured = false;
    }
    PG_END_TRY();
    return captured;
}

[[nodiscard]] bool complete_coverage_producer_fail_open(
        ReusableCoverageState& state) {
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool completed = false;
    PG_TRY();
    {
        completed = complete_coverage_producer(state);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            fail_coverage_producer(state);
            PG_RE_THROW();
        }
        FlushErrorState();
        completed = false;
    }
    PG_END_TRY();
    return completed;
}

TupleTableSlot* coverage_exec_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(node);
    if (state->replaying_materialized_output) {
        TupleTableSlot* raw = next_materialized_coverage_output(*state);
        return raw == nullptr
            ? ExecClearTuple(node->ss.ps.ps_ResultTupleSlot)
            : project_coverage_tuple(node, raw);
    }
    if (state->runtime_role == CoverageRuntimeRoleV1::CurrentConsumer) {
        TupleTableSlot* raw = next_current_coverage_row(*state, node);
        return raw == nullptr
            ? ExecClearTuple(node->ss.ps.ps_ResultTupleSlot)
            : project_coverage_tuple(node, raw);
    }
    if (state->runtime_role == CoverageRuntimeRoleV1::Producer
        && state->active_source_count != 0) {
        BioseqCacheOccurrenceV2 occurrence{};
        TupleTableSlot* raw = next_selected_older_coverage_row(
            *state, node, &occurrence);
        if (raw != nullptr) {
            if (!state->capture_failed
                && !capture_cached_coverage_row_fail_open(
                    *state, raw, occurrence)) {
                abort_coverage_capture(*state);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.fallbacks, 1);
                }
            }
            if (!spool_coverage_output(*state, raw)
                && !state->capture_failed) {
                abort_coverage_capture(*state);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.fallbacks, 1);
                }
            }
            return project_coverage_tuple(node, raw);
        }
    }
    PlanState* child = coverage_retained_child(node);
    if (child == nullptr) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    TupleTableSlot* raw = ExecProcNode(child);
    if (TupIsNull(raw)) {
        if (state->runtime_role == CoverageRuntimeRoleV1::Producer) {
            state->output_materialized_complete =
                !state->producer_spool_failed
                && state->producer_output_store != nullptr;
            if (state->output_materialized_complete) {
                tuplestore_rescan(state->producer_output_store);
            }
            if (!state->capture_failed
                && !complete_coverage_producer_fail_open(*state)) {
                abort_coverage_capture(*state);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.fallbacks, 1);
                }
            }
        }
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    if (state->runtime_role == CoverageRuntimeRoleV1::Producer) {
        if (!spool_coverage_output(*state, raw)
            && !state->capture_failed) {
            abort_coverage_capture(*state);
            if (g_shared != nullptr) {
                pg_atomic_fetch_add_u64(
                    &g_shared->stats.fallbacks, 1);
            }
        }
    }
    if (state->runtime_role == CoverageRuntimeRoleV1::Producer
        && !state->capture_failed
        && !capture_coverage_row_fail_open(*state, raw)) {
        abort_coverage_capture(*state);
        if (g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
        }
    }
    return project_coverage_tuple(node, raw);
}

void coverage_end_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(node);
    if (state->runtime_role == CoverageRuntimeRoleV1::Producer
        && state->current_producer_owned && !state->producer_completed) {
        fail_coverage_producer(*state);
    }
    coverage_state_cleanup(state);
    if (state->child_initialized && coverage_retained_child(node) != nullptr) {
        ExecEndNode(coverage_retained_child(node));
        node->custom_ps = NIL;
    }
    state->child_initialized = false;
}

void coverage_rescan_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(node);
    if (state->runtime_role == CoverageRuntimeRoleV1::CurrentConsumer
        && state->current_replay_store != nullptr) {
        tuplestore_rescan(state->current_replay_store);
        state->row_count = 0;
        return;
    }
    if (state->output_materialized_complete
        && state->producer_output_store != nullptr) {
        tuplestore_rescan(state->producer_output_store);
        state->replaying_materialized_output = true;
        return;
    }
    if (state->runtime_role == CoverageRuntimeRoleV1::Producer) {
        if (!state->capture_failed) {
            abort_coverage_capture(*state);
            if (g_shared != nullptr) {
                pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
            }
        }
        state->replay_source_index = 0;
        const std::size_t older_source_count = state->source_count == 0
            ? 0
            : static_cast<std::size_t>(state->source_count - 1U);
        for (std::size_t index = 0;
             index < older_source_count;
             ++index) {
            CoverageRuntimeSourceV1& source = state->sources[index];
            if (source.valid && source.replay_store != nullptr) {
                tuplestore_rescan(source.replay_store);
                source.replay_row_index = 0;
            }
        }
        if (state->producer_output_store != nullptr) {
            tuplestore_clear(state->producer_output_store);
            state->producer_spool_rows = 0;
            state->producer_spool_bytes = 0;
        }
        state->replaying_materialized_output = false;
        state->output_materialized_complete = false;
    }
    if (coverage_retained_child(node) != nullptr) {
        ExecReScan(coverage_retained_child(node));
    }
}

[[nodiscard]] const char* coverage_explain_state_name(
        CacheEntryState state) noexcept {
    switch (state) {
        case CacheEntryState::Absent: return "ABSENT";
        case CacheEntryState::Claimed: return "CLAIMED";
        case CacheEntryState::Building: return "BUILDING";
        case CacheEntryState::Complete: return "COMPLETE";
        case CacheEntryState::Failed: return "FAILED";
        case CacheEntryState::Invalidated: return "INVALIDATED";
        case CacheEntryState::Evicting: return "EVICTING";
    }
    return "UNKNOWN";
}

[[nodiscard]] const char* coverage_actual_action_name(
        const ReusableCoverageState& state) noexcept {
    if (state.runtime_role == CoverageRuntimeRoleV1::CurrentConsumer) {
        return "[Read] Read current reusable generation";
    }
    if (state.producer_completed && state.active_source_count != 0) {
        return "[Reuse] Reused selected branches and produced residual";
    }
    if (state.producer_completed
        || state.runtime_role == CoverageRuntimeRoleV1::Producer) {
        return "[Produce] Produce current reusable generation";
    }
    return "[Bypass] Execute retained provider child";
}

[[nodiscard]] std::array<char, 17> coverage_digest_prefix(
        const CacheDigest256& digest) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 17> output{};
    for (std::size_t index = 0; index < 8; ++index) {
        output[index * 2] = digits[digest.bytes[index] >> 4U];
        output[index * 2 + 1] = digits[digest.bytes[index] & 0x0fU];
    }
    return output;
}

[[nodiscard]] char* deparse_cache_expression_fail_open(
        Node* expression,
        ExplainState* explain) {
    if (expression == nullptr || explain == nullptr
        || explain->deparse_cxt == NIL) {
        return nullptr;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile char* rendered = nullptr;
    PG_TRY();
    {
        rendered = deparse_expression(
            expression, explain->deparse_cxt, true, false);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        rendered = nullptr;
    }
    PG_END_TRY();
    return const_cast<char*>(rendered);
}

void explain_provider_domain_fail_open(
        const ReusableCoverageState& state,
        ExplainState* explain) {
    if (state.provider == nullptr || state.provider->explain_domain == nullptr
        || state.plan_domain_nodes == NIL || explain == nullptr) {
        return;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool group_open = false;
    PG_TRY();
    {
        ExplainOpenGroup("Provider", "Provider", true, explain);
        group_open = true;
        reuse::ProviderCallDiagnosticV1 diagnostic{};
        static_cast<void>(reuse::invoke_explain_domain_fail_open(
            state.provider,
            &state.provider_lease,
            state.plan_domain_nodes,
            explain,
            state.css.ss.ps.state != nullptr
                ? state.css.ss.ps.state->es_query_cxt
                : CurrentMemoryContext,
            &diagnostic));
        ExplainCloseGroup("Provider", "Provider", true, explain);
        group_open = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)
            || g_producer_plan_capture_active) {
            PG_RE_THROW();
        }
        FlushErrorState();
        if (group_open) {
            ExplainCloseGroup("Provider", "Provider", true, explain);
            group_open = false;
        }
    }
    PG_END_TRY();
}

void coverage_explain_custom_scan(
        CustomScanState* node, List*, ExplainState* explain) {
    const auto* state = reinterpret_cast<const ReusableCoverageState*>(node);
    const auto* scan = node == nullptr
        ? nullptr
        : reinterpret_cast<const CustomScan*>(node->ss.ps.plan);
    CoverageCurrentPlanViewV1 current_plan{};
    const bool current_plan_valid = scan != nullptr
        && scan->custom_private != NIL
        && list_length(scan->custom_private) >= 11
        && decode_current_coverage_plan(
            binary_node_payload(list_nth(scan->custom_private, 2)),
            &current_plan);
    const auto digest = coverage_digest_prefix(
        current_plan_valid
            ? current_plan.header.generation_digest
            : state->current_header.generation_digest);
    const char* display_sql = scan != nullptr
            && scan->custom_private != NIL
            && list_length(scan->custom_private) >= 8
            && IsA(list_nth(scan->custom_private, 7), String)
        ? strVal(list_nth(scan->custom_private, 7))
        : "<query text unavailable>";
    const List* candidates = scan != nullptr
            && scan->custom_private != NIL
            && list_length(scan->custom_private) >= 4
        ? static_cast<const List*>(list_nth(scan->custom_private, 3))
        : NIL;
    const std::uint64_t planned_branches = static_cast<std::uint64_t>(
        std::max(list_length(candidates), 0));

    ExplainPropertyText("Cache Entry Kind", "ReusableCoverage", explain);
    ExplainPropertyText("Cache Mode", "reuse", explain);
    ExplainPropertyBool(
        "Cache Force Bypass", coverage_force_bypass(scan), explain);
    ExplainPropertyText(
        "Planned Cache Action",
        planned_branches == 0
            ? "[Provisional] Revalidate current generation or produce"
            : "[Provisional] Revalidate cache branches and current generation",
        explain);
    if (explain->analyze) {
        ExplainPropertyText(
            "Actual Cache Action", coverage_actual_action_name(*state), explain);
    }
    ExplainPropertyText("Cache Entry", digest.data(), explain);
    ExplainPropertyText("SQL", display_sql, explain);
    ExplainPropertyInteger(
        "Reusable Sources", nullptr, state->source_count, explain);
    ExplainPropertyUInteger(
        "Selected Cache Branches",
        nullptr,
        explain->analyze
            ? state->active_source_count
            : planned_branches,
        explain);
    ExplainPropertyBool(
        "Provider Runtime Recipe Valid",
        state->runtime_recipe_valid,
        explain);
    ReusableGenerationSnapshotV1 snapshot{};
    const bool snapshot_valid = state->current_reference
        && snapshot_current_reusable_generation(
            state->current_reference, &snapshot);
    if (snapshot_valid) {
        ExplainPropertyText(
            "State", coverage_explain_state_name(snapshot.state), explain);
        const Oid role = static_cast<Oid>(snapshot.coverage_directory_key
            .security_namespace.current_role_oid);
        const Oid user = GetUserId();
        const bool producer_visible = cache_diagnostic_disclosure(
                superuser_arg(user),
                OidIsValid(role) && user == role,
                has_privs_of_role(user, ROLE_PG_READ_ALL_STATS))
            == CacheDiagnosticDisclosure::FullText;
        if (producer_visible && snapshot.producer.pid > 0) {
            ExplainPropertyInteger(
                "Producer PID", nullptr, snapshot.producer.pid, explain);
        }
        const std::uint64_t now_ns = reusable_monotonic_now_ns();
        const std::uint64_t age_ms = snapshot.started_monotonic_ns != 0
                && snapshot.started_monotonic_ns <= now_ns
            ? (now_ns - snapshot.started_monotonic_ns) / 1'000'000U
            : 0;
        ExplainPropertyUInteger("Age", "ms", age_ms, explain);
        ExplainPropertyUInteger(
            "Effective TTL",
            "ms",
            snapshot.metadata.producer_ttl_ms,
            explain);
        ExplainPropertyUInteger("Rows", nullptr, snapshot.row_count, explain);
        ExplainPropertyUInteger(
            "Logical Bytes", nullptr, snapshot.metadata.logical_bytes, explain);
        ExplainPropertyUInteger(
            "Stored Bytes", nullptr, snapshot.metadata.stored_bytes, explain);
        ExplainPropertyUInteger(
            "Dependencies",
            nullptr,
            snapshot.metadata.dependency_count,
            explain);
    } else {
        ExplainPropertyText("State", "planned/provisional", explain);
        if (explain->analyze) {
            ExplainPropertyUInteger("Rows", nullptr, state->row_count, explain);
            ExplainPropertyUInteger(
                "Logical Bytes", nullptr, state->logical_bytes, explain);
            ExplainPropertyUInteger(
                "Stored Bytes", nullptr, state->payload_bytes, explain);
        }
    }

    const int expression_count = scan == nullptr
        ? 0
        : list_length(scan->custom_exprs);
    int emitted_branch = 0;
    for (int index = 0; index < static_cast<int>(planned_branches); ++index) {
        const bool selected = !explain->analyze
            || (index < static_cast<int>(state->sources.size())
                && state->sources[index].valid);
        if (!selected) {
            continue;
        }
        ++emitted_branch;
        char* label = psprintf("Cache Branch %d", emitted_branch);
        ExplainOpenGroup("Cache Branch", label, true, explain);
        ExplainPropertyText(
            "Action",
            explain->analyze
                ? "[Read] Read selected reusable source"
                : "[Provisional] Revalidate reusable source",
            explain);
        if (index + 1 < expression_count) {
            char* recheck = deparse_cache_expression_fail_open(
                static_cast<Node*>(list_nth(scan->custom_exprs, index + 1)),
                explain);
            if (recheck != nullptr) {
                ExplainPropertyText("Recheck", recheck, explain);
            }
        }
        ExplainCloseGroup("Cache Branch", label, true, explain);
        pfree(label);
    }
    ExplainOpenGroup("Residual Child", "Residual Child", true, explain);
    if (expression_count != 0) {
        char* residual = deparse_cache_expression_fail_open(
            static_cast<Node*>(linitial(scan->custom_exprs)), explain);
        if (residual != nullptr) {
            ExplainPropertyText("Predicate", residual, explain);
        }
    }
    ExplainCloseGroup("Residual Child", "Residual Child", true, explain);
    explain_provider_domain_fail_open(*state, explain);
}

const CustomExecMethods& coverage_exec_methods() {
    return executor::exact_scan::coverage_executor_methods<
        coverage_begin_custom_scan,
        coverage_exec_custom_scan,
        coverage_end_custom_scan,
        coverage_rescan_custom_scan,
        coverage_explain_custom_scan>();
}

Node* coverage_create_custom_scan_state(CustomScan*) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(
        newNode(sizeof(ReusableCoverageState), T_CustomScanState));
    state->css.methods = &coverage_exec_methods();
    return reinterpret_cast<Node*>(&state->css);
}

const CustomScanMethods& coverage_scan_methods() {
    return executor::exact_scan::coverage_scan_methods<
        coverage_create_custom_scan_state>();
}

/** Re-decode every mapping after the final decorated child Plan exists. */
[[nodiscard]] bool planned_source_mappings_valid(
        PlannerInfo* root,
        const CustomPath& best_path) {
    const auto current_bytes = binary_node_payload(
        list_nth(best_path.custom_private, 2));
    CoverageCurrentPlanViewV1 current_plan{};
    PlannerDecodedCoverageV1 consumer{};
    if (root == nullptr
        || !decode_current_coverage_plan(current_bytes, &current_plan)
        || !decode_coverage_descriptor_fail_open(
            current_plan.coverage_descriptor, root->planner_cxt, &consumer)) {
        return false;
    }
    const void* candidates_node = list_nth(best_path.custom_private, 3);
    if (candidates_node != nullptr && !IsA(candidates_node, List)) {
        return false;
    }
    ListCell* cell = nullptr;
    foreach(cell, static_cast<List*>(const_cast<void*>(candidates_node))) {
        const auto* candidate = static_cast<const List*>(lfirst(cell));
        if (candidate == nullptr || !IsA(candidate, List)
            || list_length(candidate) != 7) {
            return false;
        }
        PlannerDecodedCoverageV1 source{};
        const auto descriptor = binary_node_payload(list_nth(candidate, 2));
        DecodedSourceMappingPlanV1 mapping{};
        if (!decode_coverage_descriptor_fail_open(
                descriptor,
                root->planner_cxt,
                &source,
                &consumer.binding)
            || !validate_source_mapping_plan_node(
                list_nth(candidate, 3),
                consumer.domain,
                source.source,
                consumer.domain.boundary_columns,
                consumer.domain.boundary_column_count,
                consumer.domain.lineage_columns,
                consumer.domain.lineage_column_count,
                consumer.domain.provider_private_nodes,
                list_nth(candidate, 4),
                &mapping)) {
            return false;
        }
        destroy_decoded_source_mapping_plan(mapping);
    }
    return true;
}

/** Register only selected cache gates after PostgreSQL identifies outer Params. */
[[nodiscard]] bool register_selected_gate_params(
        PlannerInfo* root,
        const CustomPath& best_path) noexcept {
    if (root == nullptr || root->glob == nullptr
        || list_length(best_path.custom_private) < 6) {
        return false;
    }
    const void* sources_node = list_nth(best_path.custom_private, 3);
    if (sources_node != nullptr && !IsA(sources_node, List)) {
        return false;
    }
    const auto* sources = static_cast<const List*>(sources_node);
    const std::size_t source_count = static_cast<std::size_t>(
        list_length(sources));
    if (source_count + 1U > kMaxSourcesPerCoverageNode) {
        return false;
    }
    std::array<int, kMaxSourcesPerCoverageNode> gate_param_ids{};
    const auto record_gate = [root, &gate_param_ids](
            const void* node, std::size_t index) {
        if (node == nullptr || !IsA(node, Integer)) {
            return false;
        }
        const int param_id = intVal(node);
        if (param_id < 0
            || param_id >= list_length(root->glob->paramExecTypes)
            || list_nth_oid(root->glob->paramExecTypes, param_id)
                != BOOLOID) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (gate_param_ids[prior] == param_id) {
                return false;
            }
        }
        gate_param_ids[index] = param_id;
        return true;
    };
    if (!record_gate(list_nth(best_path.custom_private, 5), 0)) {
        return false;
    }
    std::size_t gate_index = 1;
    const ListCell* cell = nullptr;
    foreach(cell, sources) {
        const auto* source = static_cast<const List*>(lfirst(cell));
        if (source == nullptr || !IsA(source, List)
            || list_length(source) != 7
            || !record_gate(list_nth(source, 5), gate_index++)) {
            return false;
        }
    }
    for (std::size_t index = 0; index < gate_index; ++index) {
        if (!remember_selected_coverage_gate(
                *root, gate_param_ids[index])) {
            return false;
        }
    }
    return finalize_selected_coverage_gates(*root);
}

/** Add a planner-only dense replay namespace after the provider raw tuple. */
[[nodiscard]] List* make_coverage_scan_tlist(
        PlannerInfo* root,
        const CustomPath& best_path,
        const Plan& child) {
    if (root == nullptr || root->planner_cxt == nullptr
        || list_length(best_path.custom_private) < 6
        || child.targetlist == NIL) {
        return NIL;
    }
    CoverageCurrentPlanViewV1 current_plan{};
    PlannerDecodedCoverageV1 consumer{};
    if (!decode_current_coverage_plan(
            binary_node_payload(list_nth(best_path.custom_private, 2)),
            &current_plan)
        || !decode_coverage_descriptor_fail_open(
            current_plan.coverage_descriptor,
            root->planner_cxt,
            &consumer)) {
        return NIL;
    }
    const BioseqCachePathDomainV2& domain = consumer.domain;
    const std::size_t raw_count = static_cast<std::size_t>(
        list_length(child.targetlist));
    const std::size_t predicate_count = domain.boundary_column_count;
    bool valid = predicate_count != 0
        && domain.boundary_columns != nullptr
        && raw_count <= static_cast<std::size_t>(MaxTupleAttributeNumber)
        && predicate_count <= static_cast<std::size_t>(MaxTupleAttributeNumber)
        && raw_count + predicate_count
            <= static_cast<std::size_t>(MaxTupleAttributeNumber);
    AttrNumber expected_resno = 1;
    const ListCell* cell = nullptr;
    foreach(cell, child.targetlist) {
        const auto* entry = static_cast<const TargetEntry*>(lfirst(cell));
        if (entry == nullptr || !IsA(entry, TargetEntry)
            || entry->resno != expected_resno++) {
            valid = false;
            break;
        }
    }
    auto* order = valid
        ? static_cast<std::size_t*>(palloc(
            predicate_count * sizeof(std::size_t)))
        : nullptr;
    for (std::size_t index = 0; valid && index < predicate_count; ++index) {
        const BioseqCacheColumnSchemaV2& column =
            domain.boundary_columns[index];
        valid = column.attno != 0
            && column.attno
                <= static_cast<std::uint32_t>(MaxTupleAttributeNumber)
            && OidIsValid(column.type_oid);
        order[index] = index;
    }
    if (valid) {
        std::sort(order, order + predicate_count, [&domain](
                std::size_t left, std::size_t right) {
            return domain.boundary_columns[left].attno
                < domain.boundary_columns[right].attno;
        });
        for (std::size_t index = 1; index < predicate_count; ++index) {
            if (domain.boundary_columns[order[index - 1U]].attno
                    == domain.boundary_columns[order[index]].attno) {
                valid = false;
                break;
            }
        }
    }

    List* scan_tlist = valid
        ? static_cast<List*>(copyObject(child.targetlist))
        : NIL;
    for (std::size_t dense = 0;
         valid && dense < predicate_count;
         ++dense) {
        const BioseqCacheColumnSchemaV2& column =
            domain.boundary_columns[order[dense]];
        auto* variable = makeVar(
            INDEX_VAR,
            static_cast<AttrNumber>(dense + 1U),
            static_cast<Oid>(column.type_oid),
            column.typmod,
            static_cast<Oid>(column.collation_oid),
            0);
        variable->varnosyn = INDEX_VAR;
        variable->varattnosyn = static_cast<AttrNumber>(dense + 1U);
        variable->location = -1;
        scan_tlist = lappend(
            scan_tlist,
            makeTargetEntry(
                reinterpret_cast<Expr*>(variable),
                static_cast<AttrNumber>(raw_count + dense + 1U),
                nullptr,
                true));
    }
    if (order != nullptr) {
        pfree(order);
    }
    if (consumer.owner_context != nullptr) {
        MemoryContextDelete(consumer.owner_context);
    }
    return valid ? scan_tlist : NIL;
}

/** A selected CustomPath must always lower to a valid semantics-first Plan. */
[[nodiscard]] Plan* make_force_bypassed_coverage_plan(
        PlannerInfo& root,
        RelOptInfo& relation,
        const CustomPath& best_path,
        List* target_list,
        List* custom_plans) {
    Plan* child = static_cast<Plan*>(linitial(custom_plans));
    List* private_nodes = list_make5(
        makeInteger(reuse::kReusablePlanFormatVersionV1),
        makeInteger(0),
        makeInteger(0),
        static_cast<void*>(NIL),
        static_cast<void*>(NIL));
    private_nodes = lappend(private_nodes, makeInteger(-1));
    private_nodes = lappend(
        private_nodes,
        list_make4(
            makeInteger(0),
            static_cast<void*>(NIL),
            makeInteger(0),
            makeInteger(0)));
    private_nodes = lappend(
        private_nodes, makeString(pstrdup("<query text unavailable>")));
    private_nodes = lappend(private_nodes, makeString(pstrdup("")));
    private_nodes = lappend(
        private_nodes, makeInteger(kDiagnosticCanonicalPlanUnavailableV1));
    private_nodes = lappend(private_nodes, makeInteger(
        static_cast<int>(CoverageBypassMarkerV1::Permanent)));

    List* scan_tlist = make_coverage_scan_tlist(
        &root, best_path, *child);
    if (scan_tlist == NIL) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan fallback projection is malformed")));
        pg_unreachable();
    }
    List* expressions = list_make1(
        copyObject(list_nth(best_path.custom_private, 4)));
    const auto* sources = static_cast<const List*>(
        list_nth(best_path.custom_private, 3));
    ListCell* cell = nullptr;
    foreach(cell, sources) {
        const auto* source = static_cast<const List*>(lfirst(cell));
        if (source == nullptr || list_length(source) != 7) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqReusableCoverageScan fallback source is malformed")));
            pg_unreachable();
        }
        expressions = lappend(
            expressions, copyObject(list_nth(source, 6)));
    }

    auto* scan = makeNode(CustomScan);
    scan->methods = &coverage_scan_methods();
    scan->flags = best_path.flags;
    scan->scan.scanrelid = relation.relid;
    scan->scan.plan.targetlist = target_list;
    scan->scan.plan.qual = NIL;
    scan->custom_plans = custom_plans;
    scan->custom_private = private_nodes;
    scan->custom_exprs = expressions;
    scan->custom_scan_tlist = scan_tlist;
    scan->custom_relids = bms_copy(relation.relids);
    return &scan->scan.plan;
}

/** Finalize the provider recipe only after the decorated child Plan exists. */
Plan* coverage_plan_custom_path(
        PlannerInfo* root,
        RelOptInfo* relation,
        CustomPath* best_path,
        List* target_list,
        List*,
        List* custom_plans) {
    if (root == nullptr || relation == nullptr || best_path == nullptr
        || list_length(custom_plans) != 1) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("BioseqReusableCoverageScan selected path is malformed")));
        pg_unreachable();
    }
    if (list_length(best_path->custom_private) != 7
        || !IsA(list_nth(best_path->custom_private, 0), Integer)
        || intVal(list_nth(best_path->custom_private, 0))
            != static_cast<int>(reuse::kReusablePlanFormatVersionV1)
        || !IsA(list_nth(best_path->custom_private, 1), Integer)
        || !IsA(list_nth(best_path->custom_private, 6), Integer)) {
        if (!register_selected_gate_params(root, *best_path)) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("BioseqReusableCoverageScan fallback gates are malformed")));
            pg_unreachable();
        }
        return make_force_bypassed_coverage_plan(
            *root, *relation, *best_path, target_list, custom_plans);
    }
    const int provider_id = intVal(list_nth(best_path->custom_private, 1));
    const provider_registry::ProviderV2* provider = provider_id > 0
        ? provider_registry::find_provider_v2(
            static_cast<std::uint32_t>(provider_id))
        : nullptr;
    Plan* child = static_cast<Plan*>(linitial(custom_plans));
    if (!register_selected_gate_params(root, *best_path)) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("BioseqReusableCoverageScan selected gates are malformed")));
        pg_unreachable();
    }
    const bool mappings_valid = planned_source_mappings_valid(
        root, *best_path);
    reuse::PlannerOwnedDigestRecipeV1 recipe{};
    reuse::ProviderCallDiagnosticV1 diagnostic{};
    const bool recipe_valid = mappings_valid && provider != nullptr
        && reuse::invoke_build_recipe_fail_open(
            provider,
            root,
            child,
            root->planner_cxt,
            &recipe,
            &diagnostic);
    Const* expected_digest = recipe_valid
        ? make_binary_node(std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(
                recipe.value.expected_generation_digest),
            sizeof(recipe.value.expected_generation_digest)})
        : nullptr;
    List* recipe_private = list_make4(
        makeInteger(recipe_valid && expected_digest != nullptr ? 1 : 0),
        recipe_valid
            ? copyObject(recipe.value.plan_domain_nodes)
            : static_cast<void*>(NIL),
        expected_digest != nullptr
            ? static_cast<void*>(expected_digest)
            : static_cast<void*>(makeInteger(0)),
        makeInteger(recipe_valid
            ? static_cast<int>(recipe.value.digest_format_version)
            : 0));
    BoundedDiagnosticTextV1 display_sql{};
    static_cast<void>(copy_bounded_diagnostic_text(
        g_planner_frame == nullptr ? nullptr : g_planner_frame->display_sql,
        kReusableDisplaySqlCapacityV1,
        &display_sql));
    if (!display_sql.available) {
        static_cast<void>(copy_bounded_diagnostic_text(
            "<query text unavailable>",
            kReusableDisplaySqlCapacityV1,
            &display_sql));
    }
    BoundedDiagnosticTextV1 canonical_plan{};
    const bool canonical_plan_captured = capture_canonical_child_plan_fail_open(
        child, kReusableCanonicalPlanCapacityV1, &canonical_plan);
    int diagnostic_flags = 0;
    if (display_sql.truncated) {
        diagnostic_flags |= kDiagnosticDisplaySqlTruncatedV1;
    }
    if (canonical_plan.truncated) {
        diagnostic_flags |= kDiagnosticCanonicalPlanTruncatedV1;
    }
    if (!canonical_plan_captured) {
        diagnostic_flags |= kDiagnosticCanonicalPlanUnavailableV1;
    }

    auto* scan = makeNode(CustomScan);
    scan->methods = &coverage_scan_methods();
    scan->flags = best_path->flags;
    /* Coverage decorates one base relation; preserve its distinct self-join RTI. */
    scan->scan.scanrelid = relation->relid;
    scan->scan.plan.targetlist = target_list;
    scan->scan.plan.qual = NIL;
    scan->custom_plans = custom_plans;
    scan->custom_private = static_cast<List*>(
        copyObject(best_path->custom_private));
    void* force_bypass = list_nth(scan->custom_private, 6);
    scan->custom_private = list_truncate(scan->custom_private, 6);
    scan->custom_private = lappend(scan->custom_private, recipe_private);
    scan->custom_private = lappend(
        scan->custom_private, makeString(display_sql.data));
    scan->custom_private = lappend(
        scan->custom_private,
        makeString(canonical_plan.available
            ? canonical_plan.data
            : pstrdup("")));
    scan->custom_private = lappend(
        scan->custom_private, makeInteger(diagnostic_flags));
    scan->custom_private = lappend(scan->custom_private, force_bypass);
    List* scan_tlist = make_coverage_scan_tlist(
        root, *best_path, *child);
    scan->custom_exprs = NIL;
    if (scan_tlist != NIL) {
        scan->custom_exprs = list_make1(
            copyObject(list_nth(best_path->custom_private, 4)));
        const auto* sources = static_cast<const List*>(
            list_nth(best_path->custom_private, 3));
        ListCell* cell = nullptr;
        foreach(cell, sources) {
            const auto* source = static_cast<const List*>(lfirst(cell));
            if (source == nullptr || list_length(source) != 7) {
                scan->custom_exprs = NIL;
                scan_tlist = NIL;
                break;
            }
            scan->custom_exprs = lappend(
                scan->custom_exprs,
                copyObject(list_nth(source, 6)));
        }
    }
    if (scan_tlist == NIL) {
        scan_tlist = static_cast<List*>(copyObject(child->targetlist));
    }
    scan->custom_scan_tlist = scan_tlist;
    scan->custom_relids = bms_copy(relation->relids);
    return &scan->scan.plan;
}

const CustomPathMethods& coverage_path_methods() {
    return planner::reuse::coverage_path_methods<
        coverage_plan_custom_path>();
}

/** Bounded, cycle-safe inspection of one selected Path tree. */
struct ReusableCoveragePathProbeV1 {
    std::array<const Path*, kMaximumExactPlanWalkNodesV1> visited{};
    std::size_t visited_count = 0;
    bool valid = true;

    [[nodiscard]] bool contains(const Path* path);

    /** Lists here are selected children, never relation path alternatives. */
    [[nodiscard]] bool contains_list(const List* paths) {
        const ListCell* cell = nullptr;
        foreach(cell, paths) {
            if (contains(static_cast<const Path*>(lfirst(cell)))) {
                return true;
            }
            if (!valid) {
                return false;
            }
        }
        return false;
    }
};

/** Recognize a cache-owned coverage node anywhere in one chosen Path tree. */
bool ReusableCoveragePathProbeV1::contains(const Path* path) {
    if (!valid || path == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < visited_count; ++index) {
        if (visited[index] == path) {
            return false;
        }
    }
    if (visited_count >= visited.size()) {
        valid = false;
        return false;
    }
    visited[visited_count++] = path;
    switch (nodeTag(path)) {
        case T_BitmapHeapPath:
            return contains(
                reinterpret_cast<const BitmapHeapPath*>(path)->bitmapqual);
        case T_BitmapAndPath:
            return contains_list(
                reinterpret_cast<const BitmapAndPath*>(path)->bitmapquals);
        case T_BitmapOrPath:
            return contains_list(
                reinterpret_cast<const BitmapOrPath*>(path)->bitmapquals);
        case T_SubqueryScanPath:
            return contains(
                reinterpret_cast<const SubqueryScanPath*>(path)->subpath);
        case T_ForeignPath:
            return contains(
                reinterpret_cast<const ForeignPath*>(path)->fdw_outerpath);
        case T_CustomPath: {
            const auto* custom = reinterpret_cast<const CustomPath*>(path);
            return custom->methods == &coverage_path_methods()
                || contains_list(custom->custom_paths);
        }
        case T_AppendPath:
            return contains_list(
                reinterpret_cast<const AppendPath*>(path)->subpaths);
        case T_MergeAppendPath:
            return contains_list(
                reinterpret_cast<const MergeAppendPath*>(path)->subpaths);
        case T_MaterialPath:
            return contains(
                reinterpret_cast<const MaterialPath*>(path)->subpath);
        case T_MemoizePath:
            return contains(
                reinterpret_cast<const MemoizePath*>(path)->subpath);
        case T_UniquePath:
            return contains(
                reinterpret_cast<const UniquePath*>(path)->subpath);
        case T_GatherPath:
            return contains(
                reinterpret_cast<const GatherPath*>(path)->subpath);
        case T_GatherMergePath:
            return contains(
                reinterpret_cast<const GatherMergePath*>(path)->subpath);
        case T_NestPath:
        case T_MergePath:
        case T_HashPath: {
            const auto* join = reinterpret_cast<const JoinPath*>(path);
            return contains(join->outerjoinpath)
                || contains(join->innerjoinpath);
        }
        case T_ProjectionPath:
            return contains(
                reinterpret_cast<const ProjectionPath*>(path)->subpath);
        case T_ProjectSetPath:
            return contains(
                reinterpret_cast<const ProjectSetPath*>(path)->subpath);
        case T_SortPath:
            return contains(
                reinterpret_cast<const SortPath*>(path)->subpath);
        case T_IncrementalSortPath:
            return contains(
                reinterpret_cast<const IncrementalSortPath*>(path)
                    ->spath.subpath);
        case T_GroupPath:
            return contains(
                reinterpret_cast<const GroupPath*>(path)->subpath);
        case T_UpperUniquePath:
            return contains(
                reinterpret_cast<const UpperUniquePath*>(path)->subpath);
        case T_AggPath:
            return contains(
                reinterpret_cast<const AggPath*>(path)->subpath);
        case T_GroupingSetsPath:
            return contains(
                reinterpret_cast<const GroupingSetsPath*>(path)->subpath);
        case T_MinMaxAggPath: {
            const auto* aggregate = reinterpret_cast<const MinMaxAggPath*>(
                path);
            const ListCell* cell = nullptr;
            foreach(cell, aggregate->mmaggregates) {
                const auto* info = static_cast<const MinMaxAggInfo*>(
                    lfirst(cell));
                if (info != nullptr
                    && contains(info->path)) {
                    return true;
                }
                if (!valid) {
                    return false;
                }
            }
            return false;
        }
        case T_WindowAggPath:
            return contains(
                reinterpret_cast<const WindowAggPath*>(path)->subpath);
        case T_SetOpPath:
            return contains(
                reinterpret_cast<const SetOpPath*>(path)->subpath);
        case T_RecursiveUnionPath: {
            const auto* recursive =
                reinterpret_cast<const RecursiveUnionPath*>(path);
            return contains(recursive->leftpath)
                || contains(recursive->rightpath);
        }
        case T_LockRowsPath:
            return contains(
                reinterpret_cast<const LockRowsPath*>(path)->subpath);
        case T_ModifyTablePath:
            return contains(
                reinterpret_cast<const ModifyTablePath*>(path)->subpath);
        case T_LimitPath:
            return contains(
                reinterpret_cast<const LimitPath*>(path)->subpath);
        default:
            return false;
    }
}

[[nodiscard]] bool path_contains_reusable_coverage(const Path* path) {
    ReusableCoveragePathProbeV1 probe{};
    const bool found = probe.contains(path);
    return probe.valid && found;
}

/** Add one finalized relation dependency without losing topology roots. */
[[nodiscard]] bool append_exact_relation_dependency(
        List** relation_oids, Oid relation_oid) {
    if (relation_oids == nullptr || !OidIsValid(relation_oid)) {
        return false;
    }
    if (list_member_oid(*relation_oids, relation_oid)) {
        return true;
    }
    if (list_length(*relation_oids)
        >= static_cast<int>(kMaxDependenciesPerGeneration)) {
        return false;
    }
    *relation_oids = lappend_oid(*relation_oids, relation_oid);
    return true;
}

/** Walk only the selected Path tree, switching roots for subquery domains. */
struct ExactPathDependencyCollectorV1 {
    List** relation_oids = nullptr;

    [[nodiscard]] bool collect_list(
            const List* paths, PlannerInfo* root) {
        const ListCell* cell = nullptr;
        foreach(cell, paths) {
            if (!collect(
                    static_cast<const Path*>(lfirst(cell)), root)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool collect(const Path* path, PlannerInfo* root) {
        if (path == nullptr || root == nullptr) {
            return path == nullptr;
        }
        if (path->parent != nullptr && path->parent->relid > 0) {
            const Index range_table_index = path->parent->relid;
            const bool index_valid = root->simple_rte_array != nullptr
                ? range_table_index
                    < static_cast<Index>(root->simple_rel_array_size)
                : range_table_index
                    <= static_cast<Index>(list_length(root->parse->rtable));
            if (!index_valid) {
                return false;
            }
            const RangeTblEntry* range = planner_rt_fetch(
                range_table_index, root);
            if (range != nullptr && range->rtekind == RTE_RELATION
                && !append_exact_relation_dependency(
                    relation_oids, range->relid)) {
                return false;
            }
        }
        switch (nodeTag(path)) {
            case T_Path:
            case T_IndexPath:
            case T_TidPath:
            case T_TidRangePath:
            case T_GroupResultPath:
                return true;
            case T_BitmapHeapPath:
                return collect(
                    reinterpret_cast<const BitmapHeapPath*>(path)->bitmapqual,
                    root);
            case T_BitmapAndPath:
                return collect_list(
                    reinterpret_cast<const BitmapAndPath*>(path)->bitmapquals,
                    root);
            case T_BitmapOrPath:
                return collect_list(
                    reinterpret_cast<const BitmapOrPath*>(path)->bitmapquals,
                    root);
            case T_SubqueryScanPath: {
                const auto* subquery =
                    reinterpret_cast<const SubqueryScanPath*>(path);
                PlannerInfo* subroot = path->parent == nullptr
                    ? nullptr
                    : path->parent->subroot;
                return collect(subquery->subpath, subroot);
            }
            case T_ForeignPath:
                return collect(
                    reinterpret_cast<const ForeignPath*>(path)->fdw_outerpath,
                    root);
            case T_CustomPath:
                return collect_list(
                    reinterpret_cast<const CustomPath*>(path)->custom_paths,
                    root);
            case T_AppendPath:
                return collect_list(
                    reinterpret_cast<const AppendPath*>(path)->subpaths,
                    root);
            case T_MergeAppendPath:
                return collect_list(
                    reinterpret_cast<const MergeAppendPath*>(path)->subpaths,
                    root);
            case T_MaterialPath:
                return collect(
                    reinterpret_cast<const MaterialPath*>(path)->subpath,
                    root);
            case T_MemoizePath:
                return collect(
                    reinterpret_cast<const MemoizePath*>(path)->subpath,
                    root);
            case T_UniquePath:
                return collect(
                    reinterpret_cast<const UniquePath*>(path)->subpath,
                    root);
            case T_GatherPath:
                return collect(
                    reinterpret_cast<const GatherPath*>(path)->subpath,
                    root);
            case T_GatherMergePath:
                return collect(
                    reinterpret_cast<const GatherMergePath*>(path)->subpath,
                    root);
            case T_NestPath:
            case T_MergePath:
            case T_HashPath: {
                const auto* join = reinterpret_cast<const JoinPath*>(path);
                return collect(join->outerjoinpath, root)
                    && collect(join->innerjoinpath, root);
            }
            case T_ProjectionPath:
                return collect(
                    reinterpret_cast<const ProjectionPath*>(path)->subpath,
                    root);
            case T_ProjectSetPath:
                return collect(
                    reinterpret_cast<const ProjectSetPath*>(path)->subpath,
                    root);
            case T_SortPath:
                return collect(
                    reinterpret_cast<const SortPath*>(path)->subpath,
                    root);
            case T_IncrementalSortPath:
                return collect(
                    reinterpret_cast<const IncrementalSortPath*>(path)
                        ->spath.subpath,
                    root);
            case T_GroupPath:
                return collect(
                    reinterpret_cast<const GroupPath*>(path)->subpath,
                    root);
            case T_UpperUniquePath:
                return collect(
                    reinterpret_cast<const UpperUniquePath*>(path)->subpath,
                    root);
            case T_AggPath:
                return collect(
                    reinterpret_cast<const AggPath*>(path)->subpath,
                    root);
            case T_GroupingSetsPath:
                return collect(
                    reinterpret_cast<const GroupingSetsPath*>(path)->subpath,
                    root);
            case T_MinMaxAggPath: {
                const auto* aggregate =
                    reinterpret_cast<const MinMaxAggPath*>(path);
                const ListCell* cell = nullptr;
                foreach(cell, aggregate->mmaggregates) {
                    const auto* info = static_cast<const MinMaxAggInfo*>(
                        lfirst(cell));
                    if (info == nullptr
                        || !collect(info->path, info->subroot)) {
                        return false;
                    }
                }
                return true;
            }
            case T_WindowAggPath:
                return collect(
                    reinterpret_cast<const WindowAggPath*>(path)->subpath,
                    root);
            case T_SetOpPath:
                return collect(
                    reinterpret_cast<const SetOpPath*>(path)->subpath,
                    root);
            case T_RecursiveUnionPath: {
                const auto* recursive =
                    reinterpret_cast<const RecursiveUnionPath*>(path);
                return collect(recursive->leftpath, root)
                    && collect(recursive->rightpath, root);
            }
            case T_LockRowsPath:
                return collect(
                    reinterpret_cast<const LockRowsPath*>(path)->subpath,
                    root);
            case T_ModifyTablePath:
                return collect(
                    reinterpret_cast<const ModifyTablePath*>(path)->subpath,
                    root);
            case T_LimitPath:
                return collect(
                    reinterpret_cast<const LimitPath*>(path)->subpath,
                    root);
            default:
                return false;
        }
    }
};

/**
 * Freeze the post-partition-expansion dependency closure before decoration.
 * Query-tree roots preserve topology identity; the selected Path contributes
 * candidate physical leaves while aggregate support items remain full
 * cacheId/hashValue identities collected from the analyzed expression tree.
 */
[[nodiscard]] bool finalize_exact_plan_dependencies(
        PlannerInfo& root, const Path& selected_path,
        PlannerFrame& frame) {
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_test_exact_plan_fault
            == TestExactPlanFault::DependencyFinalizationDeclined) {
        return false;
    }
#endif
    if (root.glob == nullptr) {
        return false;
    }
    ListCell* relation_cell = nullptr;
    foreach(relation_cell, root.glob->relationOids) {
        if (!append_exact_relation_dependency(
                &frame.referenced_relation_oids,
                lfirst_oid(relation_cell))) {
            return false;
        }
        if (list_length(frame.referenced_relation_oids)
                + list_length(frame.referenced_syscache_dependencies)
            > static_cast<int>(kMaxDependenciesPerGeneration)) {
            return false;
        }
    }
    ExactPathDependencyCollectorV1 path_collector{
        .relation_oids = &frame.referenced_relation_oids,
    };
    if (!path_collector.collect(&selected_path, &root)) {
        return false;
    }
    return list_length(frame.referenced_relation_oids)
            + list_length(frame.referenced_syscache_dependencies)
        <= static_cast<int>(kMaxDependenciesPerGeneration);
}

/** Create an exact-result wrapper around one final path. */
[[nodiscard]] CustomPath* make_exact_cache_path(
        RelOptInfo& output_relation, Path& input_path,
        const PlannerFrame& frame,
        ExactBypassMarkerV1 bypass_marker =
            ExactBypassMarkerV1::Enabled) {
    auto* path = makeNode(CustomPath);
    path->path.pathtype = T_CustomScan;
    path->path.parent = &output_relation;
    path->path.pathtarget = input_path.pathtarget;
    path->path.param_info = nullptr;
#if PG_VERSION_NUM >= 180000
    path->path.disabled_nodes = input_path.disabled_nodes;
#endif
    path->path.parallel_aware = false;
    path->path.parallel_safe = false;
    path->path.parallel_workers = 0;
    path->path.rows = input_path.rows;
    path->path.startup_cost = input_path.startup_cost;
    path->path.total_cost = input_path.total_cost
        + cpu_tuple_cost * input_path.rows * 0.01;
    path->path.pathkeys = input_path.pathkeys;
    path->flags = CUSTOMPATH_SUPPORT_PROJECTION;
    path->custom_paths = list_make1(&input_path);
    path->custom_private = list_make3(
        makeString(pstrdup(frame.canonical_query)),
        makeString(pstrdup(frame.display_sql)),
        makeInteger(static_cast<int>(bypass_marker)));
    path->custom_private = lappend(
        path->custom_private,
        copyObject(frame.referenced_parameters));
    path->custom_private = lappend(
        path->custom_private,
        copyObject(frame.referenced_relation_oids));
    path->custom_private = lappend(
        path->custom_private,
        copyObject(frame.referenced_syscache_dependencies));
    path->custom_restrictinfo = NIL;
    path->methods = &cache_path_methods();
    return path;
}

/** Predecessor-first top-level FINAL path decoration before setrefs. */
void cache_create_upper_paths(
        PlannerInfo* root, UpperRelationKind stage,
        RelOptInfo* input_relation, RelOptInfo* output_relation, void* extra) {
    if (const auto previous =
            executor::lifecycle::previous_create_upper_paths();
        previous != nullptr) {
        previous(
            root, stage, input_relation, output_relation, extra);
    }
    PlannerFrame* frame = g_planner_frame;
    if (frame == nullptr
        || !frame->eligible
        || stage != UPPERREL_FINAL
        || root->parent_root != nullptr
        || root->parse != frame->delegated_query
        || output_relation == nullptr
        || output_relation->pathlist == NIL) {
        return;
    }
    List* original_paths = output_relation->pathlist;
    List* replacements = NIL;
    bool exact_path_added = false;
    ListCell* path_cell = nullptr;
    foreach(path_cell, original_paths) {
        Path* input_path = static_cast<Path*>(lfirst(path_cell));
        if (input_path->param_info != nullptr
            || path_contains_reusable_coverage(input_path)) {
            replacements = lappend(replacements, input_path);
        } else {
            PlannerFrame path_frame = *frame;
            path_frame.referenced_relation_oids = static_cast<List*>(
                copyObject(frame->referenced_relation_oids));
            path_frame.referenced_syscache_dependencies =
                static_cast<List*>(copyObject(
                    frame->referenced_syscache_dependencies));
            if (finalize_exact_plan_dependencies(
                    *root, *input_path, path_frame)) {
                replacements = lappend(
                    replacements,
                    make_exact_cache_path(
                        *output_relation, *input_path, path_frame));
                exact_path_added = true;
            } else if (frame->nested_coverage_path_created) {
                /*
                 * A selected nested coverage Plan will export its private
                 * Boolean gate through subplan extParam.  Retain one top
                 * PlanCustomPath callback even when exact dependencies are
                 * unavailable so those IDs can be re-unioned after
                 * SS_identify_outer_params.  The wrapper is permanently
                 * passthrough and can never touch the registry.
                 */
                replacements = lappend(
                    replacements,
                    make_exact_cache_path(
                        *output_relation,
                        *input_path,
                        path_frame,
                        ExactBypassMarkerV1::CoverageGateFinalizer));
                exact_path_added = true;
            } else {
                replacements = lappend(replacements, input_path);
            }
        }
    }
    if (!exact_path_added) {
        return;
    }

    output_relation->pathlist = NIL;
    output_relation->cheapest_startup_path = nullptr;
    output_relation->cheapest_total_path = nullptr;
    output_relation->cheapest_unique_path = nullptr;
    output_relation->cheapest_parameterized_paths = NIL;
    foreach(path_cell, replacements) {
        add_path(output_relation, static_cast<Path*>(lfirst(path_cell)));
    }
    set_cheapest(output_relation);
    if (g_shared != nullptr) {
        pg_atomic_fetch_add_u64(
            &g_shared->stats.planner_build_generation, 1);
    }
}

struct ExactPlanVerification {
    const PlannedStmt* statement = nullptr;
    const List* relation_oids = NIL;
    bool relation_free = false;
    bool saw_relation_free_source = false;
    std::uint32_t provider_scan_count = 0;
};

[[nodiscard]] const RangeTblEntry* exact_plan_range_entry(
        const ExactPlanVerification& verification,
        Index range_table_index) noexcept {
    if (verification.statement == nullptr || range_table_index == 0
        || range_table_index > static_cast<Index>(
            list_length(verification.statement->rtable))) {
        return nullptr;
    }
    return static_cast<const RangeTblEntry*>(list_nth(
        verification.statement->rtable,
        static_cast<int>(range_table_index - 1)));
}

/** V1 admits only PostgreSQL's built-in heap table AM. */
[[nodiscard]] bool exact_table_access_method_safe(Oid relation_oid) {
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_test_exact_plan_fault
        == TestExactPlanFault::UnknownTableAccessMethod) {
        return false;
    }
#endif
    return OidIsValid(relation_oid)
        && get_rel_relam(relation_oid) == HEAP_TABLE_AM_OID;
}

/** V1 admits the built-in index AMs whose executor paths are reviewed. */
[[nodiscard]] bool exact_index_access_method_safe(Oid index_oid) {
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_test_exact_plan_fault
        == TestExactPlanFault::UnknownIndexAccessMethod) {
        return false;
    }
#endif
    if (!OidIsValid(index_oid)) {
        return false;
    }
    switch (get_rel_relam(index_oid)) {
        case BTREE_AM_OID:
        case HASH_AM_OID:
        case GIST_AM_OID:
        case GIN_AM_OID:
        case SPGIST_AM_OID:
        case BRIN_AM_OID:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] bool exact_relation_scan_safe(
        ExactPlanVerification& verification, const Scan& scan) {
    const RangeTblEntry* range = exact_plan_range_entry(
        verification, scan.scanrelid);
    if (range == nullptr || range->rtekind != RTE_RELATION
        || !OidIsValid(range->relid) || verification.relation_free
        || !list_member_oid(verification.relation_oids, range->relid)) {
        return false;
    }
    const char relation_kind = get_rel_relkind(range->relid);
    return (relation_kind == RELKIND_RELATION
            || relation_kind == RELKIND_MATVIEW)
        && exact_table_access_method_safe(range->relid);
}

[[nodiscard]] bool exact_synthetic_scan_safe(
        const ExactPlanVerification& verification,
        const Scan& scan, RTEKind expected_kind) noexcept {
    const RangeTblEntry* range = exact_plan_range_entry(
        verification, scan.scanrelid);
    return range != nullptr && range->rtekind == expected_kind;
}

[[nodiscard]] bool exact_plan_tree_safe(
    ExactPlanVerification& verification, const Plan* plan);

[[nodiscard]] bool exact_plan_list_safe(
        ExactPlanVerification& verification, const List* plans) {
    const ListCell* cell = nullptr;
    foreach(cell, plans) {
        if (!exact_plan_tree_safe(
                verification, static_cast<const Plan*>(lfirst(cell)))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool exact_common_plan_expressions_safe(const Plan& plan) {
    return !contain_mutable_functions(
            reinterpret_cast<Node*>(plan.targetlist))
        && !contain_mutable_functions(reinterpret_cast<Node*>(plan.qual))
        && !contain_mutable_functions(reinterpret_cast<Node*>(plan.initPlan));
}

/** Positive allowlist for finalized executor sources and structural nodes. */
[[nodiscard]] bool exact_plan_tree_safe(
        ExactPlanVerification& verification, const Plan* plan) {
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_test_exact_plan_fault == TestExactPlanFault::UnknownCustomScan) {
        return false;
    }
#endif
    if (plan == nullptr || !exact_common_plan_expressions_safe(*plan)) {
        return false;
    }

    switch (nodeTag(plan)) {
        case T_Result: {
            const auto* result = reinterpret_cast<const Result*>(plan);
            if (contain_mutable_functions(result->resconstantqual)) {
                return false;
            }
            if (verification.relation_free && outerPlan(plan) == nullptr
                && innerPlan(plan) == nullptr) {
                verification.saw_relation_free_source = true;
            }
            break;
        }
        case T_ValuesScan: {
            const auto* scan = reinterpret_cast<const ValuesScan*>(plan);
            if (!exact_synthetic_scan_safe(
                    verification, scan->scan, RTE_VALUES)
                || contain_mutable_functions(
                    reinterpret_cast<Node*>(scan->values_lists))) {
                return false;
            }
            if (verification.relation_free) {
                verification.saw_relation_free_source = true;
            }
            break;
        }
        case T_SubqueryScan: {
            const auto* scan = reinterpret_cast<const SubqueryScan*>(plan);
            if (!exact_synthetic_scan_safe(
                    verification, scan->scan, RTE_SUBQUERY)
                || !exact_plan_tree_safe(verification, scan->subplan)) {
                return false;
            }
            break;
        }
        case T_SeqScan:
        case T_BitmapHeapScan:
        case T_TidScan:
        case T_TidRangeScan:
            if (!exact_relation_scan_safe(
                    verification,
                    *reinterpret_cast<const Scan*>(plan))) {
                return false;
            }
            break;
        case T_IndexScan: {
            const auto* scan = reinterpret_cast<const IndexScan*>(plan);
            if (!exact_relation_scan_safe(verification, scan->scan)
                || !exact_index_access_method_safe(scan->indexid)) {
                return false;
            }
            break;
        }
        case T_IndexOnlyScan: {
            const auto* scan = reinterpret_cast<const IndexOnlyScan*>(plan);
            if (!exact_relation_scan_safe(verification, scan->scan)
                || !exact_index_access_method_safe(scan->indexid)) {
                return false;
            }
            break;
        }
        case T_BitmapIndexScan: {
            const auto* scan = reinterpret_cast<const BitmapIndexScan*>(plan);
            if (!exact_relation_scan_safe(verification, scan->scan)
                || !exact_index_access_method_safe(scan->indexid)) {
                return false;
            }
            break;
        }
        case T_Append:
            if (!exact_plan_list_safe(
                    verification,
                    reinterpret_cast<const Append*>(plan)->appendplans)) {
                return false;
            }
            break;
        case T_MergeAppend:
            if (!exact_plan_list_safe(
                    verification,
                    reinterpret_cast<const MergeAppend*>(plan)->mergeplans)) {
                return false;
            }
            break;
        case T_BitmapAnd:
            if (!exact_plan_list_safe(
                    verification,
                    reinterpret_cast<const BitmapAnd*>(plan)->bitmapplans)) {
                return false;
            }
            break;
        case T_BitmapOr:
            if (!exact_plan_list_safe(
                    verification,
                    reinterpret_cast<const BitmapOr*>(plan)->bitmapplans)) {
                return false;
            }
            break;
        case T_CustomScan: {
            const auto* scan = reinterpret_cast<const CustomScan*>(plan);
            if (contain_mutable_functions(
                    reinterpret_cast<Node*>(scan->custom_exprs))) {
                return false;
            }
            if (scan->methods == &coverage_scan_methods()) {
                if (list_length(scan->custom_plans) != 1
                    || !exact_plan_list_safe(
                        verification, scan->custom_plans)) {
                    return false;
                }
                break;
            }
            const auto providers = provider_registry::snapshot_providers_v2();
            const provider_registry::ProviderV2* matched_provider = nullptr;
            for (std::uint32_t index = 0;
                 index < providers.provider_count; ++index) {
                const provider_registry::ProviderV2* provider =
                    providers.providers[index];
                if (provider == nullptr
                    || !reuse::provider_plan_matches(plan, provider)) {
                    continue;
                }
                if (matched_provider != nullptr) {
                    return false;
                }
                matched_provider = provider;
            }
            if (!provider_allows_exact_source_elision(matched_provider)
                || verification.provider_scan_count
                    == (std::numeric_limits<std::uint32_t>::max)()
                || !exact_plan_list_safe(
                    verification, scan->custom_plans)) {
                return false;
            }
            ++verification.provider_scan_count;
            break;
        }
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin:
        case T_Material:
        case T_Memoize:
        case T_Sort:
        case T_IncrementalSort:
        case T_Group:
        case T_Agg:
        case T_WindowAgg:
        case T_Unique:
        case T_Gather:
        case T_GatherMerge:
        case T_Hash:
        case T_SetOp:
        case T_Limit:
            break;
        case T_ProjectSet:
        case T_ModifyTable:
        case T_RecursiveUnion:
        case T_SampleScan:
        case T_FunctionScan:
        case T_TableFuncScan:
        case T_CteScan:
        case T_NamedTuplestoreScan:
        case T_WorkTableScan:
        case T_ForeignScan:
        case T_LockRows:
        default:
            return false;
    }

    return (outerPlan(plan) == nullptr
            || exact_plan_tree_safe(verification, outerPlan(plan)))
        && (innerPlan(plan) == nullptr
            || exact_plan_tree_safe(verification, innerPlan(plan)));
}

[[nodiscard]] int exact_runtime_recipe_count(
        const CustomScan& scan) noexcept {
    if (scan.custom_private == NIL
        || list_length(scan.custom_private) < 8
        || !IsA(list_nth(scan.custom_private, 7), Integer)) {
        return -1;
    }
    const int count = intVal(list_nth(scan.custom_private, 7));
    const void* recipes_node = list_nth(scan.custom_private, 6);
    const bool recipes_valid = recipes_node == nullptr
        ? count == 0
        : IsA(recipes_node, List)
            && list_length(static_cast<const List*>(recipes_node)) == count;
    return count >= 0
            && count <= static_cast<int>(kMaxProviderRecipesPerExactPlan)
            && recipes_valid
        ? count
        : -1;
}

[[nodiscard]] bool exact_dependency_metadata(
        const CustomScan& scan, const List** relation_oids,
        const List** syscache_dependencies) noexcept {
    if (relation_oids == nullptr || syscache_dependencies == nullptr
        || scan.custom_private == NIL
        || list_length(scan.custom_private) <= 5) {
        return false;
    }
    const auto* dependencies = static_cast<const List*>(
        list_nth(scan.custom_private, 4));
    if (dependencies != NIL && nodeTag(dependencies) != T_OidList) {
        return false;
    }
    const auto* syscache = static_cast<const List*>(
        list_nth(scan.custom_private, 5));
    if (syscache != NIL && nodeTag(syscache) != T_List) {
        return false;
    }
    ListCell* cell = nullptr;
    foreach(cell, syscache) {
        const auto* item = static_cast<const PlanInvalItem*>(lfirst(cell));
        if (item == nullptr || !IsA(item, PlanInvalItem)
            || item->cacheId < 0 || item->cacheId >= SysCacheSize) {
            return false;
        }
    }
    *relation_oids = dependencies;
    *syscache_dependencies = syscache;
    return true;
}

/** Merge the planner's finalized invalidation closure into the exact node. */
[[nodiscard]] bool merge_finalized_exact_dependencies(
        PlannedStmt& statement) {
    if (statement.planTree == nullptr
        || !IsA(statement.planTree, CustomScan)) {
        return true;
    }
    auto* exact = reinterpret_cast<CustomScan*>(statement.planTree);
    if (exact->methods != &cache_scan_methods()) {
        return true;
    }
    if (!set_private_force_bypass(*exact, true)) {
        return false;
    }
    if (statement.transientPlan || exact->custom_private == NIL
        || list_length(exact->custom_private) <= 5) {
        return false;
    }
    auto* dependencies = static_cast<List*>(
        list_nth(exact->custom_private, 5));
    if (dependencies != NIL && nodeTag(dependencies) != T_List) {
        return false;
    }
    auto* relation_oids = static_cast<List*>(
        list_nth(exact->custom_private, 4));
    if (relation_oids != NIL && nodeTag(relation_oids) != T_OidList) {
        return false;
    }
    const ListCell* cell = nullptr;
    foreach(cell, statement.relationOids) {
        if (!append_exact_relation_dependency(
                &relation_oids, lfirst_oid(cell))) {
            return false;
        }
    }
    foreach(cell, statement.invalItems) {
        const auto* item = static_cast<const PlanInvalItem*>(lfirst(cell));
        if (item == nullptr || !IsA(item, PlanInvalItem)
            || !append_exact_syscache_dependency(
                &dependencies, item->cacheId, item->hashValue)) {
            return false;
        }
    }
    if (statement.dependsOnRole) {
        Oid effective_user = InvalidOid;
        int security_context = 0;
        GetUserIdAndSecContext(&effective_user, &security_context);
        const Oid session_user = GetSessionUserId();
        if (!OidIsValid(effective_user)
            || !append_exact_syscache_dependency(
                &dependencies,
                AUTHOID,
                GetSysCacheHashValue1(
                    AUTHOID, ObjectIdGetDatum(effective_user)))
            || (session_user != effective_user
                && (!OidIsValid(session_user)
                    || !append_exact_syscache_dependency(
                        &dependencies,
                        AUTHOID,
                        GetSysCacheHashValue1(
                            AUTHOID, ObjectIdGetDatum(session_user)))))) {
            return false;
        }
    }
    lfirst(list_nth_cell(exact->custom_private, 4)) = relation_oids;
    lfirst(list_nth_cell(exact->custom_private, 5)) = dependencies;
    return true;
}

/** Verify the exact retained tree after setrefs and flip only its marker. */
void verify_exact_cache_plan_fail_open(PlannedStmt& statement) {
    if (statement.planTree == nullptr
        || !IsA(statement.planTree, CustomScan)) {
        return;
    }
    auto* exact = reinterpret_cast<CustomScan*>(statement.planTree);
    if (exact->methods != &cache_scan_methods()) {
        return;
    }
    if (!set_private_force_bypass(*exact, true)) {
        return;
    }

    bool safe = false;
    MemoryContext caller_context = CurrentMemoryContext;
    PG_TRY();
    {
        const List* relation_oids = NIL;
        const List* syscache_dependencies = NIL;
        ExactPlanVerification verification{
            .statement = &statement,
            .relation_oids = NIL,
            .relation_free = false,
            .saw_relation_free_source = false,
        };
        const int runtime_recipe_count = exact_runtime_recipe_count(*exact);
        const bool dependency_metadata_valid = exact_dependency_metadata(
            *exact, &relation_oids, &syscache_dependencies);
        if (dependency_metadata_valid
            && runtime_recipe_count >= 0
            && list_length(exact->custom_plans) == 1) {
            verification.relation_oids = relation_oids;
            verification.relation_free = relation_oids == NIL;
            safe = exact_plan_tree_safe(
                verification,
                static_cast<const Plan*>(linitial(exact->custom_plans)));
            ListCell* subplan_cell = nullptr;
            foreach(subplan_cell, statement.subplans) {
                safe = safe && exact_plan_tree_safe(
                    verification,
                    static_cast<const Plan*>(lfirst(subplan_cell)));
            }
            safe = safe && (!verification.relation_free
                || verification.saw_relation_free_source)
                && verification.provider_scan_count
                    == static_cast<std::uint32_t>(runtime_recipe_count);
        }
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        safe = false;
    }
    PG_END_TRY();
    if (safe) {
        static_cast<void>(set_private_force_bypass(*exact, false));
    }
}

/** Collect selected coverage markers before mutating any finalized Plan. */
struct MainTreeCoverageMarkerCollectorV1 {
    std::array<const Plan*, kMaximumExactPlanWalkNodesV1> visited{};
    std::array<Integer*, static_cast<std::size_t>(kMaxCoverageNodesPerPlan)>
        markers{};
    std::size_t visited_count = 0;
    std::size_t marker_count = 0;
    bool include_enabled = false;
    bool valid = true;

    [[nodiscard]] bool collect(Plan* plan);

    [[nodiscard]] bool collect_list(List* plans) {
        ListCell* cell = nullptr;
        foreach(cell, plans) {
            if (!collect(static_cast<Plan*>(lfirst(cell)))) {
                return false;
            }
        }
        return true;
    }
};

bool MainTreeCoverageMarkerCollectorV1::collect(Plan* plan) {
    if (!valid || plan == nullptr) {
        return valid;
    }
    for (std::size_t index = 0; index < visited_count; ++index) {
        if (visited[index] == plan) {
            return true;
        }
    }
    if (visited_count >= visited.size()) {
        valid = false;
        return false;
    }
    visited[visited_count++] = plan;
    if (IsA(plan, CustomScan)) {
        auto* custom = reinterpret_cast<CustomScan*>(plan);
        if (custom->methods == &coverage_scan_methods()) {
            if (custom->custom_private == NIL
                || list_length(custom->custom_private) < 11
                || !IsA(list_nth(custom->custom_private, 10), Integer)) {
                valid = false;
                return false;
            }
            auto* marker = static_cast<Integer*>(
                list_nth(custom->custom_private, 10));
            const int value = marker->ival;
            if (value < static_cast<int>(CoverageBypassMarkerV1::Enabled)
                || value > static_cast<int>(
                    CoverageBypassMarkerV1::Permanent)) {
                valid = false;
                return false;
            }
            if (value == static_cast<int>(
                    CoverageBypassMarkerV1::Provisional)
                || (include_enabled
                    && value == static_cast<int>(
                        CoverageBypassMarkerV1::Enabled))) {
                if (marker_count >= markers.size()) {
                    valid = false;
                    return false;
                }
                markers[marker_count++] = marker;
            }
        }
    }
    if (!collect(outerPlan(plan)) || !collect(innerPlan(plan))) {
        return false;
    }
    switch (nodeTag(plan)) {
        case T_SubqueryScan:
            return collect(reinterpret_cast<SubqueryScan*>(plan)->subplan);
        case T_CustomScan:
            return collect_list(
                reinterpret_cast<CustomScan*>(plan)->custom_plans);
        case T_Append:
            return collect_list(reinterpret_cast<Append*>(plan)->appendplans);
        case T_MergeAppend:
            return collect_list(
                reinterpret_cast<MergeAppend*>(plan)->mergeplans);
        case T_BitmapAnd:
            return collect_list(
                reinterpret_cast<BitmapAnd*>(plan)->bitmapplans);
        case T_BitmapOr:
            return collect_list(
                reinterpret_cast<BitmapOr*>(plan)->bitmapplans);
        case T_Agg:
            return collect_list(reinterpret_cast<Agg*>(plan)->chain);
        case T_Result:
        case T_ProjectSet:
        case T_ModifyTable:
        case T_RecursiveUnion:
        case T_SeqScan:
        case T_SampleScan:
        case T_IndexScan:
        case T_IndexOnlyScan:
        case T_BitmapIndexScan:
        case T_BitmapHeapScan:
        case T_TidScan:
        case T_TidRangeScan:
        case T_FunctionScan:
        case T_ValuesScan:
        case T_TableFuncScan:
        case T_CteScan:
        case T_NamedTuplestoreScan:
        case T_WorkTableScan:
        case T_ForeignScan:
        case T_NestLoop:
        case T_MergeJoin:
        case T_HashJoin:
        case T_Material:
        case T_Memoize:
        case T_Sort:
        case T_IncrementalSort:
        case T_Group:
        case T_WindowAgg:
        case T_Unique:
        case T_Gather:
        case T_GatherMerge:
        case T_Hash:
        case T_SetOp:
        case T_LockRows:
        case T_Limit:
            return true;
        default:
            valid = false;
            return false;
    }
}

/**
 * Replace every exact scan's provisional recipes with post-setrefs recipes.
 *
 * The traversal covers both the main tree and separately initialized subplans.
 * Every exact node is forced to bypass before callbacks run and is re-enabled
 * only by the ordinary finalized-plan verifier later in the planner hook.
 */
[[nodiscard]] bool refresh_finalized_exact_provider_recipes(
        PlannedStmt& statement,
        MemoryContext planner_context) {
    MainTreeCoverageMarkerCollectorV1 plans{};
    const bool traversal_complete =
        plans.collect(statement.planTree)
        && plans.collect_list(statement.subplans)
        && plans.valid;
    bool recipes_complete = traversal_complete;
    for (std::size_t index = 0; index < plans.visited_count; ++index) {
        auto* plan = const_cast<Plan*>(plans.visited[index]);
        if (plan == nullptr || !IsA(plan, CustomScan)) {
            continue;
        }
        auto* scan = reinterpret_cast<CustomScan*>(plan);
        if (scan->methods != &cache_scan_methods()) {
            continue;
        }
        if (!set_private_force_bypass(*scan, true)
            || scan->custom_private == NIL
            || list_length(scan->custom_private) < 8
            || scan->custom_plans == NIL
            || list_length(scan->custom_plans) != 1
            || !IsA(list_nth(scan->custom_private, 7), Integer)) {
            recipes_complete = false;
            continue;
        }
        List* recipes = NIL;
        std::uint32_t recipe_count = 0;
        if (!collect_finalized_exact_provider_recipes(
                static_cast<const Plan*>(linitial(scan->custom_plans)),
                statement.subplans,
                planner_context,
                &recipes,
                &recipe_count)) {
            recipes_complete = false;
            continue;
        }
        lfirst(list_nth_cell(scan->custom_private, 6)) = recipes;
        lfirst(list_nth_cell(scan->custom_private, 7)) =
            makeInteger(static_cast<int>(recipe_count));
    }
    return recipes_complete;
}

/** Permanently bypass every separately initialized CTE/InitPlan/SubPlan root. */
[[nodiscard]] bool mark_subplan_coverage_permanent_fail_open(
        PlannedStmt& statement) noexcept {
    MainTreeCoverageMarkerCollectorV1 collector{
        .include_enabled = true,
    };
    if (!collector.collect_list(statement.subplans) || !collector.valid) {
        return false;
    }
    for (std::size_t index = 0; index < collector.marker_count; ++index) {
        collector.markers[index]->ival = static_cast<int>(
            CoverageBypassMarkerV1::Permanent);
    }
    return true;
}

/** Enable only provisional main-tree nodes; permanent bypass is sticky. */
void enable_main_tree_coverage_fail_open(PlannedStmt& statement) noexcept {
    MainTreeCoverageMarkerCollectorV1 collector{};
    if (!collector.collect(statement.planTree) || !collector.valid) {
        return;
    }
    for (std::size_t index = 0; index < collector.marker_count; ++index) {
        collector.markers[index]->ival = static_cast<int>(
            CoverageBypassMarkerV1::Enabled);
    }
}

/**
 * Finalize all cache-owned plan metadata after the current outer provider has
 * completed its post-setrefs mutations.
 *
 * Repeating this operation is intentional: when cache is the inner planner
 * hook it first sees the post-setrefs tree before an outer provider mutates
 * it, and the provider bridge then authoritatively replaces those provisional
 * recipes.  Every refresh forces exact nodes to bypass before rebuilding.
 */
void finalize_cache_planned_statement(
        PlannedStmt& statement,
        MemoryContext planner_context) {
    if (mark_subplan_coverage_permanent_fail_open(statement)) {
        enable_main_tree_coverage_fail_open(statement);
    }
    if (refresh_finalized_exact_provider_recipes(
            statement, planner_context)
        && merge_finalized_exact_dependencies(statement)) {
        verify_exact_cache_plan_fail_open(statement);
    }
}

/** V2 planner-bridge endpoint invoked only after provider Plan mutation. */
void observe_finalized_planned_statement(
        PlannedStmt* statement) noexcept {
    if (statement == nullptr) {
        return;
    }
    finalize_cache_planned_statement(*statement, CurrentMemoryContext);
    if (g_planner_frame != nullptr) {
        /*
         * Cache is the outer hook in this load order.  The bridge refresh is
         * authoritative; do not immediately rebuild the same recipes again
         * after the provider returns.
         */
        g_planner_frame->bridge_finalized_statement = statement;
    }
}

/** Reentrant planner wrapper that owns canonical bytes for upper-path hooks. */
PlannedStmt* cache_planner(
        Query* query, const char* query_string,
        int cursor_options, ParamListInfo bound_parameters) {
    const planner_hook_type previous_planner =
        executor::lifecycle::previous_planner();
    if (g_internal_planning || g_producer_plan_capture_active
        || guc::mode() == QueryCacheMode::Disabled
        || !shared_cache_ready()) {
        return previous_planner != nullptr
            ? previous_planner(
                query, query_string, cursor_options, bound_parameters)
            : standard_planner(
                query, query_string, cursor_options, bound_parameters);
    }

    MemoryContext planner_context = CurrentMemoryContext;
    PlannerFrame frame{
        .previous = g_planner_frame,
        .delegated_query = query,
        .canonical_query = nullptr,
        .display_sql = nullptr,
        .referenced_parameters = NIL,
        .referenced_relation_oids = NIL,
        .referenced_syscache_dependencies = NIL,
        .eligible = false,
    };
    g_planner_frame = &frame;
    PlannedStmt* result = nullptr;
    PG_TRY();
    {
        MemoryContextSwitchTo(planner_context);
        frame.eligible = query_is_exact_cache_eligible(*query);
        const bool current_statement_text = query_string != nullptr
            && (debug_query_string == nullptr
                || query_string == debug_query_string
                || std::strcmp(query_string, debug_query_string) == 0);
        if (frame.eligible && query->rtable == NIL
            && !current_statement_text) {
            /* PL/pgSQL simple-expression plans must retain a Result root. */
            frame.eligible = false;
        }
        if (frame.eligible) {
            bool relation_overflow = false;
            frame.canonical_query = canonicalize_query(*query);
            frame.display_sql = copy_display_sql(query_string);
            ExternalParameterCollectorV1 parameters =
                collect_external_parameters(*query);
            frame.referenced_parameters = parameters.parameters;
            frame.referenced_relation_oids = collect_relation_oids(
                *query, relation_overflow);
            ExactSyscacheCollectorV1 syscache_dependencies =
                collect_aggregate_syscache_dependencies(*query);
            frame.referenced_syscache_dependencies =
                syscache_dependencies.dependencies;
            frame.eligible = parameters.valid
                && !relation_overflow
                && !syscache_dependencies.overflow
                && list_length(frame.referenced_relation_oids)
                    + list_length(frame.referenced_syscache_dependencies)
                    <= static_cast<int>(kMaxDependenciesPerGeneration);
            if (frame.eligible
                && guc::mode() == QueryCacheMode::Reuse) {
                const auto providers =
                    provider_registry::snapshot_providers_v2();
                for (std::uint32_t provider_index = 0;
                     provider_index < providers.provider_count
                     && frame.provider_intent_count
                         < frame.providers.size();
                     ++provider_index) {
                    const provider_registry::ProviderV2* provider =
                        providers.providers[provider_index];
                    reuse::ProviderCallDiagnosticV1 diagnostic{};
                    reuse::PlannerOwnedIntentV1 intent{};
                    if (!reuse::invoke_analyze_query_fail_open(
                            provider,
                            query,
                            planner_context,
                            &intent,
                            &diagnostic)) {
                        continue;
                    }
                    const std::size_t destination =
                        frame.provider_intent_count++;
                    frame.providers[destination] = provider;
                    frame.intents[destination] = intent;
                }
            }
        }
        result = previous_planner != nullptr
            ? previous_planner(
                query, query_string, cursor_options, bound_parameters)
            : standard_planner(
                query, query_string, cursor_options, bound_parameters);
        if (result != nullptr
            && frame.bridge_finalized_statement != result) {
            finalize_cache_planned_statement(*result, planner_context);
        }
        g_planner_frame = frame.previous;
    }
    PG_CATCH();
    {
        g_planner_frame = frame.previous;
        PG_RE_THROW();
    }
    PG_END_TRY();
    return result;
}

/** Preserve write observability even when a writer session disables caching. */
void mark_planned_result_relations(PlannedStmt& statement) {
    if (g_dirty_observations.coarse()) {
        return;
    }
    bool precise = false;
    ListCell* relation_cell = nullptr;
    foreach(relation_cell, statement.resultRelations) {
        const int range_index = lfirst_int(relation_cell);
        if (range_index <= 0 || range_index > list_length(statement.rtable)) {
            mark_current_transaction_dirty();
            return;
        }
        const auto* range = static_cast<const RangeTblEntry*>(
            list_nth(statement.rtable, range_index - 1));
        if (range == nullptr || range->rtekind != RTE_RELATION
            || !OidIsValid(range->relid)) {
            mark_current_transaction_dirty();
            return;
        }
        mark_current_transaction_relation_dirty(range->relid);
        if (g_dirty_observations.coarse()) {
            return;
        }
        precise = true;
    }
    if (!precise) {
        mark_current_transaction_dirty();
    }
}

/** Planner metadata is a backstop; malformed/ERROR paths become coarse. */
void mark_planned_result_relations_fail_open(PlannedStmt& statement) {
    MemoryContext caller_context = CurrentMemoryContext;
    PG_TRY();
    {
        mark_planned_result_relations(statement);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        mark_current_transaction_dirty();
    }
    PG_END_TRY();
}

/** Observe one initialized ResultRelInfo without retaining executor pointers. */
void mark_result_relation_info(ResultRelInfo* result_relation) {
    if (g_dirty_observations.coarse()) {
        return;
    }
    if (result_relation == nullptr
        || !RelationIsValid(result_relation->ri_RelationDesc)) {
        mark_current_transaction_dirty();
        return;
    }
    const Oid relation_oid = RelationGetRelid(
        result_relation->ri_RelationDesc);
    if (!OidIsValid(relation_oid)) {
        mark_current_transaction_dirty();
        return;
    }
    mark_current_transaction_relation_dirty(relation_oid);
}

/**
 * Capture opened, tuple-routed, and trigger-only targets while EState is live.
 * Ordinary observer failures switch this transaction to sticky coarse mode;
 * they never fail a successful source write.
 */
void mark_executor_result_relations_fail_open(QueryDesc* query_description) {
    EState* estate = query_description == nullptr
        ? nullptr
        : query_description->estate;
    if (estate == nullptr) {
        return;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    PG_TRY();
    {
        bool coarse = false;
        ListCell* cell = nullptr;
        foreach(cell, estate->es_opened_result_relations) {
            mark_result_relation_info(
                static_cast<ResultRelInfo*>(lfirst(cell)));
            if (g_dirty_observations.coarse()) {
                coarse = true;
                break;
            }
        }
        if (!coarse) {
            foreach(cell, estate->es_tuple_routing_result_relations) {
                mark_result_relation_info(
                    static_cast<ResultRelInfo*>(lfirst(cell)));
                if (g_dirty_observations.coarse()) {
                    coarse = true;
                    break;
                }
            }
        }
        if (!coarse) {
            foreach(cell, estate->es_trig_target_relations) {
                mark_result_relation_info(
                    static_cast<ResultRelInfo*>(lfirst(cell)));
                if (g_dirty_observations.coarse()) {
                    break;
                }
            }
        }
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        mark_current_transaction_dirty();
    }
    PG_END_TRY();
}

/** Copy one bounded diagnostic into an exact shared record under its lock. */
template <std::size_t Capacity>
void assign_exact_plan_diagnostic(
        std::array<char, Capacity>& destination,
        std::uint32_t& destination_bytes,
        ReusablePlanSnapshotStatusV1& destination_status,
        bool& destination_truncated,
        const BoundedDiagnosticTextV1& source) noexcept {
    destination.fill(0);
    const std::size_t copied = std::min(source.size, destination.size());
    if (source.available && source.data != nullptr && copied != 0) {
        std::memcpy(destination.data(), source.data, copied);
    }
    destination_bytes = static_cast<std::uint32_t>(
        source.available ? copied : 0);
    destination_status = source.status;
    destination_truncated = source.truncated || source.size > copied;
}

/** Finalize one exact claim only after its retained PlanState is initialized. */
[[nodiscard]] bool publish_legacy_planned_exact_diagnostics(
        BioseqCacheState& state) {
    CustomScanState* node = &state.css;
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    PlanState* child_state = retained_child(node);
    Plan* child_plan = scan == nullptr || scan->custom_plans == NIL
        ? nullptr
        : static_cast<Plan*>(linitial(scan->custom_plans));
    if (state.runtime_role != RuntimeRole::Producer
        || state.producer_completed || state.planned_diagnostics_published
        || !state.child_initialized || state.registry_index < 0
        || child_state == nullptr || child_plan == nullptr
        || node->ss.ps.ps_ResultTupleDesc == nullptr) {
        return false;
    }
    BoundedDiagnosticTextV1 canonical_plan{};
    static_cast<void>(capture_canonical_child_plan_fail_open(
        child_plan, kExactCanonicalPlanCapacityV1, &canonical_plan));
    BoundedDiagnosticTextV1 human_plan{};
    static_cast<void>(capture_initialized_child_plan_fail_open(
        node,
        node->ss.ps.state,
        private_string(*scan, 1),
        false,
        kExactProducerPlanCapacityV1,
        &human_plan));
    TupleDesc raw_descriptor = exact_raw_tuple_descriptor(node);
    if (raw_descriptor == nullptr
        || !begin_producer_capture(state, raw_descriptor)) {
        return false;
    }

    bool published = false;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[state.registry_index];
    if (cache_entry_id(entry) == state.entry_id
        && entry.state == SharedEntryState::Claimed
        && exact_producer_identity(entry)
            == current_reusable_producer_identity()) {
        assign_exact_plan_diagnostic(
            entry.canonical_plan,
            entry.canonical_plan_bytes,
            entry.canonical_plan_status,
            entry.canonical_plan_truncated,
            canonical_plan);
        assign_exact_plan_diagnostic(
            entry.producer_plan,
            entry.producer_plan_bytes,
            entry.producer_plan_status,
            entry.producer_plan_truncated,
            human_plan);
        entry.state = SharedEntryState::Producing;
        state.planned_diagnostics_published = true;
        ConditionVariableBroadcast(&entry.changed);
        published = true;
    }
    LWLockRelease(g_registry_lock);
    if (!published) {
        destroy_column_builder(state);
        return false;
    }
    maybe_pause(TestPausePoint::AfterBuildingPublish);
    return true;
}

/** Publish exact BUILDING and its initialized-child diagnostics atomically. */
[[nodiscard]] bool publish_planned_exact_diagnostics(
        BioseqCacheState& state) {
    CustomScanState* node = &state.css;
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    PlanState* child_state = retained_child(node);
    Plan* child_plan = scan == nullptr || scan->custom_plans == NIL
        ? nullptr
        : static_cast<Plan*>(linitial(scan->custom_plans));
    if (state.runtime_role != RuntimeRole::Producer
        || state.producer_completed || state.planned_diagnostics_published
        || !state.child_initialized || !state.registry_reference
        || child_state == nullptr || child_plan == nullptr
        || node->ss.ps.ps_ResultTupleDesc == nullptr) {
        return false;
    }
    BoundedDiagnosticTextV1 canonical_plan{};
    static_cast<void>(capture_canonical_child_plan_fail_open(
        child_plan, kExactCanonicalPlanCapacityV1, &canonical_plan));
    BoundedDiagnosticTextV1 human_plan{};
    static_cast<void>(capture_initialized_child_plan_fail_open(
        node,
        node->ss.ps.state,
        private_string(*scan, 1),
        false,
        kExactProducerPlanCapacityV1,
        &human_plan));
    TupleDesc raw_descriptor = exact_raw_tuple_descriptor(node);
    if (raw_descriptor == nullptr
        || !begin_producer_capture(state, raw_descriptor)) {
        return false;
    }
    const char* display_sql = private_string(*scan, 1);
    const std::size_t display_bytes = strnlen(
        display_sql, kReusableDisplaySqlCapacityV1);
    const ReusableDiagnosticTextUpdateV1 diagnostics{
        .reference = state.registry_reference,
        .producer = current_reusable_producer_identity(),
        .display_sql = std::span<const char>{display_sql, display_bytes},
        .canonical_plan = canonical_plan.available
            ? std::span<const char>{canonical_plan.data, canonical_plan.size}
            : std::span<const char>{},
        .producer_plan = human_plan.available
            ? std::span<const char>{human_plan.data, human_plan.size}
            : std::span<const char>{},
        .canonical_plan_status = canonical_plan.status,
        .plan_status = human_plan.status,
        .display_sql_truncated = display_bytes
            == kReusableDisplaySqlCapacityV1,
        .canonical_plan_truncated = canonical_plan.truncated,
        .producer_plan_truncated = human_plan.truncated,
    };
    if (begin_current_reusable_generation(diagnostics)
            != ReusableRegistryErrorV1::None) {
        destroy_column_builder(state);
        return false;
    }
    state.planned_diagnostics_published = true;
    maybe_pause(TestPausePoint::AfterBuildingPublish);
    return true;
}

/** Upgrade one exact producer plan only from live Analyze instrumentation. */
void refresh_legacy_analyzed_exact_diagnostics(BioseqCacheState& state) {
    PlanState* child = retained_child(&state.css);
    EState* estate = state.css.ss.ps.state;
    auto* scan = reinterpret_cast<CustomScan*>(state.css.ss.ps.plan);
    if (!state.planned_diagnostics_published || state.capture_failed
        || (!state.producer_completed
            && state.runtime_role != RuntimeRole::Producer)
        || state.registry_index < 0 || child == nullptr
        || child->instrument == nullptr || estate == nullptr
        || estate->es_instrument == 0 || scan == nullptr) {
        return;
    }
    BoundedDiagnosticTextV1 human_plan{};
    if (!capture_initialized_child_plan_fail_open(
            &state.css,
            estate,
            private_string(*scan, 1),
            true,
            kExactProducerPlanCapacityV1,
            &human_plan)) {
        return;
    }
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[state.registry_index];
    const bool owned_build = entry.state == SharedEntryState::Producing
        && exact_producer_identity(entry)
            == current_reusable_producer_identity();
    const bool owned_complete = entry.state == SharedEntryState::Complete
        && state.producer_completed;
    if (cache_entry_id(entry) == state.entry_id
        && (owned_build || owned_complete)) {
        assign_exact_plan_diagnostic(
            entry.producer_plan,
            entry.producer_plan_bytes,
            entry.producer_plan_status,
            entry.producer_plan_truncated,
            human_plan);
    }
    LWLockRelease(g_registry_lock);
}

/** Replace exact producer diagnostics from live Analyze instrumentation. */
void refresh_analyzed_exact_diagnostics(BioseqCacheState& state) {
    PlanState* child = retained_child(&state.css);
    EState* estate = state.css.ss.ps.state;
    auto* scan = reinterpret_cast<CustomScan*>(state.css.ss.ps.plan);
    if (!state.planned_diagnostics_published || state.capture_failed
        || (!state.producer_completed
            && state.runtime_role != RuntimeRole::Producer)
        || !state.registry_reference || child == nullptr
        || child->instrument == nullptr || estate == nullptr
        || estate->es_instrument == 0 || scan == nullptr
        || scan->custom_plans == NIL) {
        return;
    }
    BoundedDiagnosticTextV1 human_plan{};
    if (!capture_initialized_child_plan_fail_open(
            &state.css,
            estate,
            private_string(*scan, 1),
            true,
            kExactProducerPlanCapacityV1,
            &human_plan)) {
        return;
    }
    BoundedDiagnosticTextV1 canonical_plan{};
    static_cast<void>(capture_canonical_child_plan_fail_open(
        static_cast<Plan*>(linitial(scan->custom_plans)),
        kExactCanonicalPlanCapacityV1,
        &canonical_plan));
    const char* display_sql = private_string(*scan, 1);
    const std::size_t display_bytes = strnlen(
        display_sql, kReusableDisplaySqlCapacityV1);
    static_cast<void>(store_current_reusable_diagnostics(
        ReusableDiagnosticTextUpdateV1{
            .reference = state.registry_reference,
            .producer = current_reusable_producer_identity(),
            .display_sql = std::span<const char>{
                display_sql, display_bytes},
            .canonical_plan = canonical_plan.available
                ? std::span<const char>{
                    canonical_plan.data, canonical_plan.size}
                : std::span<const char>{},
            .producer_plan = std::span<const char>{
                human_plan.data, human_plan.size},
            .canonical_plan_status = canonical_plan.status,
            .plan_status = ReusablePlanSnapshotStatusV1::Analyzed,
            .display_sql_truncated = display_bytes
                == kReusableDisplaySqlCapacityV1,
            .canonical_plan_truncated = canonical_plan.truncated,
            .producer_plan_truncated = human_plan.truncated,
        }));
}

struct CoverageDiagnosticPrivateV1 {
    const char* display_sql = nullptr;
    const char* canonical_plan = nullptr;
    int flags = 0;
};

[[nodiscard]] bool coverage_diagnostic_private(
        const ReusableCoverageState& state,
        CoverageDiagnosticPrivateV1* output) noexcept {
    const auto* scan = reinterpret_cast<const CustomScan*>(
        state.css.ss.ps.plan);
    if (scan == nullptr || output == nullptr
        || scan->custom_private == NIL
        || list_length(scan->custom_private) < 10
        || !IsA(list_nth(scan->custom_private, 7), String)
        || !IsA(list_nth(scan->custom_private, 8), String)
        || !IsA(list_nth(scan->custom_private, 9), Integer)) {
        return false;
    }
    *output = CoverageDiagnosticPrivateV1{
        .display_sql = strVal(list_nth(scan->custom_private, 7)),
        .canonical_plan = strVal(list_nth(scan->custom_private, 8)),
        .flags = intVal(list_nth(scan->custom_private, 9)),
    };
    return output->display_sql != nullptr
        && output->display_sql[0] != '\0'
        && output->canonical_plan != nullptr;
}

/** Publish BUILDING only after the complete chained PlanState tree exists. */
[[nodiscard]] bool publish_planned_coverage_diagnostics(
        ReusableCoverageState& state) {
    if (!state.current_producer_owned || state.producer_completed
        || state.planned_diagnostics_published
        || !state.child_initialized || !state.current_reference) {
        return false;
    }
    CoverageDiagnosticPrivateV1 private_text{};
    if (!coverage_diagnostic_private(state, &private_text)) {
        return false;
    }
    BoundedDiagnosticTextV1 human_plan{};
    static_cast<void>(capture_initialized_child_plan_fail_open(
        &state.css,
        state.css.ss.ps.state,
        private_text.display_sql,
        false,
        kReusableProducerPlanCapacityV1,
        &human_plan));
    const std::size_t display_sql_bytes = strnlen(
        private_text.display_sql, kReusableDisplaySqlCapacityV1);
    const std::size_t canonical_plan_bytes = strnlen(
        private_text.canonical_plan, kReusableCanonicalPlanCapacityV1);
    const bool canonical_unavailable = canonical_plan_bytes == 0
        || (private_text.flags
            & kDiagnosticCanonicalPlanUnavailableV1) != 0;
    const ReusableDiagnosticTextUpdateV1 diagnostics{
        .reference = state.current_reference,
        .producer = current_reusable_producer_identity(),
        .display_sql = std::span<const char>{
            private_text.display_sql, display_sql_bytes},
        .canonical_plan = std::span<const char>{
            private_text.canonical_plan, canonical_plan_bytes},
        .producer_plan = human_plan.available
            ? std::span<const char>{human_plan.data, human_plan.size}
            : std::span<const char>{},
        .canonical_plan_status = canonical_unavailable
            ? ReusablePlanSnapshotStatusV1::Unavailable
            : ReusablePlanSnapshotStatusV1::Planned,
        .plan_status = human_plan.status,
        .display_sql_truncated =
            (private_text.flags & kDiagnosticDisplaySqlTruncatedV1) != 0,
        .canonical_plan_truncated =
            (private_text.flags & kDiagnosticCanonicalPlanTruncatedV1) != 0,
        .producer_plan_truncated = human_plan.truncated,
    };
    if (begin_current_reusable_generation(diagnostics)
            != ReusableRegistryErrorV1::None) {
        return false;
    }
    state.planned_diagnostics_published = true;
    maybe_pause(TestPausePoint::AfterBuildingPublish);
    return true;
}

/** Upgrade Planned text only when EXPLAIN ANALYZE instrumentation is live. */
void refresh_analyzed_coverage_diagnostics(
        ReusableCoverageState& state) {
    PlanState* child = coverage_retained_child(&state.css);
    EState* estate = state.css.ss.ps.state;
    if (!state.planned_diagnostics_published || state.capture_failed
        || (!state.current_producer_owned && !state.producer_completed)
        || !state.current_reference || child == nullptr
        || child->instrument == nullptr || estate == nullptr
        || estate->es_instrument == 0) {
        return;
    }
    CoverageDiagnosticPrivateV1 private_text{};
    if (!coverage_diagnostic_private(state, &private_text)) {
        return;
    }
    BoundedDiagnosticTextV1 human_plan{};
    if (!capture_initialized_child_plan_fail_open(
            &state.css,
            estate,
            private_text.display_sql,
            true,
            kReusableProducerPlanCapacityV1,
            &human_plan)) {
        return;
    }
    const std::size_t display_sql_bytes = strnlen(
        private_text.display_sql, kReusableDisplaySqlCapacityV1);
    const std::size_t canonical_plan_bytes = strnlen(
        private_text.canonical_plan, kReusableCanonicalPlanCapacityV1);
    const bool canonical_unavailable = canonical_plan_bytes == 0
        || (private_text.flags
            & kDiagnosticCanonicalPlanUnavailableV1) != 0;
    static_cast<void>(store_current_reusable_diagnostics(
        ReusableDiagnosticTextUpdateV1{
            .reference = state.current_reference,
            .producer = current_reusable_producer_identity(),
            .display_sql = std::span<const char>{
                private_text.display_sql, display_sql_bytes},
            .canonical_plan = std::span<const char>{
                private_text.canonical_plan, canonical_plan_bytes},
            .producer_plan = std::span<const char>{
                human_plan.data, human_plan.size},
            .canonical_plan_status = canonical_unavailable
                ? ReusablePlanSnapshotStatusV1::Unavailable
                : ReusablePlanSnapshotStatusV1::Planned,
            .plan_status = ReusablePlanSnapshotStatusV1::Analyzed,
            .display_sql_truncated =
                (private_text.flags & kDiagnosticDisplaySqlTruncatedV1) != 0,
            .canonical_plan_truncated =
                (private_text.flags
                    & kDiagnosticCanonicalPlanTruncatedV1) != 0,
            .producer_plan_truncated = human_plan.truncated,
        }));
}

enum class ProducerPlanWalkPhaseV1 : std::uint8_t {
    Planned,
    Analyzed,
};

inline constexpr std::size_t kMaximumProducerPlanWalkNodesV1 = 1024;

struct ProducerPlanWalkContextV1 {
    ProducerPlanWalkPhaseV1 phase = ProducerPlanWalkPhaseV1::Planned;
    std::array<PlanState*, kMaximumProducerPlanWalkNodesV1> visited{};
    std::size_t visited_count = 0;
    bool overflow = false;
};

/** Traverse init plans, subplans, and custom children without revisiting DAGs. */
bool visit_reusable_producer_planstate(
        PlanState* planstate, void* opaque) {
    auto* context = static_cast<ProducerPlanWalkContextV1*>(opaque);
    if (planstate == nullptr || context == nullptr) {
        return false;
    }
    for (std::size_t index = 0; index < context->visited_count; ++index) {
        if (context->visited[index] == planstate) {
            return false;
        }
    }
    if (context->visited_count >= context->visited.size()) {
        context->overflow = true;
        return true;
    }
    context->visited[context->visited_count++] = planstate;
    if (IsA(planstate, CustomScanState)) {
        auto* custom = reinterpret_cast<CustomScanState*>(planstate);
        if (custom->methods == &cache_exec_methods()) {
            auto* state = reinterpret_cast<BioseqCacheState*>(custom);
            if (context->phase == ProducerPlanWalkPhaseV1::Planned) {
                if (state->runtime_role == RuntimeRole::Producer
                    && !publish_planned_exact_diagnostics(*state)) {
                    fail_registry_producer(*state);
                    if (g_shared != nullptr) {
                        pg_atomic_fetch_add_u64(
                            &g_shared->stats.fallbacks, 1);
                    }
                }
            } else {
                refresh_analyzed_exact_diagnostics(*state);
            }
        } else if (custom->methods == &coverage_exec_methods()) {
            auto* state = reinterpret_cast<ReusableCoverageState*>(custom);
            if (context->phase == ProducerPlanWalkPhaseV1::Planned) {
                if (state->current_producer_owned
                    && !publish_planned_coverage_diagnostics(*state)) {
                    fail_coverage_producer(*state);
                    if (g_shared != nullptr) {
                        pg_atomic_fetch_add_u64(
                            &g_shared->stats.fallbacks, 1);
                    }
                }
            } else {
                refresh_analyzed_coverage_diagnostics(*state);
            }
        }
    }
    return planstate_tree_walker(
        planstate, visit_reusable_producer_planstate, context);
}

void walk_reusable_producer_planstates_unchecked(
        QueryDesc* query_description,
        ProducerPlanWalkPhaseV1 phase) {
    if (query_description == nullptr || query_description->planstate == nullptr) {
        return;
    }
    ProducerPlanWalkContextV1 context{.phase = phase};
    static_cast<void>(visit_reusable_producer_planstate(
        query_description->planstate, &context));
}

/** Diagnostic traversal never blocks the source query on ordinary errors. */
[[nodiscard]] bool walk_reusable_producer_planstates_fail_open(
        QueryDesc* query_description,
        ProducerPlanWalkPhaseV1 phase) {
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool completed = false;
    PG_TRY();
    {
        walk_reusable_producer_planstates_unchecked(
            query_description, phase);
        completed = true;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        completed = false;
    }
    PG_END_TRY();
    return completed;
}

void fail_executor_start_claims(
        ExecutorStartClaimFrameV1& frame,
        bool only_unpublished) noexcept {
    for (std::size_t index = 0;
         index < frame.coverage_producer_count;
         ++index) {
        ReusableCoverageState* state = frame.coverage_producers[index];
        if (state != nullptr && state->current_producer_owned
            && (!only_unpublished
                || !state->planned_diagnostics_published)
            && !state->producer_completed) {
            fail_coverage_producer(*state);
            if (only_unpublished && g_shared != nullptr) {
                pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
            }
        }
    }
    for (std::size_t index = 0;
         index < frame.exact_producer_count;
         ++index) {
        BioseqCacheState* state = frame.exact_producers[index];
        if (state != nullptr
            && state->runtime_role == RuntimeRole::Producer
            && (!only_unpublished
                || !state->planned_diagnostics_published)
            && !state->producer_completed) {
            fail_registry_producer(*state);
            if (only_unpublished && g_shared != nullptr) {
                pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
            }
        }
    }
}

void cache_executor_start(QueryDesc* query_description, int eflags) {
    ExecutorStartClaimFrameV1 frame{
        .previous = g_executor_start_claim_frame,
    };
    g_executor_start_claim_frame = &frame;
    const bool observes_execution =
        (eflags & EXEC_FLAG_EXPLAIN_ONLY) == 0;
    PG_TRY();
    {
        if (observes_execution && query_description != nullptr
            && query_description->plannedstmt != nullptr
            && (query_description->plannedstmt->commandType != CMD_SELECT
                || query_description->plannedstmt->hasModifyingCTE)) {
            mark_planned_result_relations_fail_open(
                *query_description->plannedstmt);
        }
        if (const auto previous =
                executor::lifecycle::previous_executor_start();
            previous != nullptr) {
            previous(query_description, eflags);
        } else {
            standard_ExecutorStart(query_description, eflags);
        }
        if (observes_execution) {
            mark_executor_result_relations_fail_open(query_description);
        }
        static_cast<void>(walk_reusable_producer_planstates_fail_open(
            query_description, ProducerPlanWalkPhaseV1::Planned));
        fail_executor_start_claims(frame, true);
        g_executor_start_claim_frame = frame.previous;
    }
    PG_CATCH();
    {
        g_executor_start_claim_frame = frame.previous;
        fail_executor_start_claims(frame, false);
        PG_RE_THROW();
    }
    PG_END_TRY();
}

void cache_executor_finish(QueryDesc* query_description) {
    const bool observes_execution = query_description != nullptr
        && query_description->estate != nullptr
        && (query_description->estate->es_top_eflags
                & EXEC_FLAG_EXPLAIN_ONLY) == 0;
    if (observes_execution) {
        mark_executor_result_relations_fail_open(query_description);
    }
    if (observes_execution
        && !query_description->estate->es_finished) {
        static_cast<void>(walk_reusable_producer_planstates_fail_open(
            query_description, ProducerPlanWalkPhaseV1::Analyzed));
    }
    if (const auto previous =
            executor::lifecycle::previous_executor_finish();
        previous != nullptr) {
        previous(query_description);
    } else {
        standard_ExecutorFinish(query_description);
    }
}

/** Last-chance collection covers tuple routing and trigger targets from Run. */
void cache_executor_end(QueryDesc* query_description) {
    if (query_description != nullptr && query_description->estate != nullptr
        && (query_description->estate->es_top_eflags
                & EXEC_FLAG_EXPLAIN_ONLY) == 0) {
        mark_executor_result_relations_fail_open(query_description);
    }
    if (const auto previous =
            executor::lifecycle::previous_executor_end();
        previous != nullptr) {
        previous(query_description);
    } else {
        standard_ExecutorEnd(query_description);
    }
}

/** Exclude utility commands that cannot change a reusable SQL result. */
[[nodiscard]] bool utility_may_change_database(const Node* statement) noexcept {
    if (statement == nullptr) {
        return false;
    }
    switch (nodeTag(statement)) {
        case T_CopyStmt:
            return reinterpret_cast<const CopyStmt*>(statement)->is_from;
        case T_TransactionStmt:
        case T_VariableSetStmt:
        case T_VariableShowStmt:
        case T_PrepareStmt:
        case T_ExecuteStmt: /* Any executed DML is observed by ExecutorStart. */
        case T_DeallocateStmt:
        case T_ExplainStmt: /* EXPLAIN ANALYZE DML is observed by ExecutorStart. */
        case T_DeclareCursorStmt:
        case T_ClosePortalStmt:
        case T_FetchStmt:
        case T_ListenStmt:
        case T_UnlistenStmt:
        case T_NotifyStmt:
        case T_CheckPointStmt:
        case T_DiscardStmt:
            return false;
        default:
            return true;
    }
}

inline constexpr std::size_t kMaximumTruncateReplacementTargetsV1 =
    dependency_runtime::kMaximumDirtyDependenciesV1 - 1U;

struct TruncateRelationReplacementV1 {
    Oid relation_oid = InvalidOid;
    DependencyVersion old_version{};
};

/** Fixed-stack utility handoff; PostgreSQL owns every retained relation lock. */
struct TruncateReplacementCaptureV1 {
    std::array<TruncateRelationReplacementV1,
        kMaximumTruncateReplacementTargetsV1> targets{};
    std::size_t count = 0;
    bool active = false;
    bool closure_complete = false;
};

[[nodiscard]] bool truncate_target_already_captured(
        const TruncateReplacementCaptureV1& capture,
        Oid relation_oid) noexcept {
    return std::any_of(
        capture.targets.begin(),
        capture.targets.begin() + capture.count,
        [relation_oid](const TruncateRelationReplacementV1& target) noexcept {
            return target.relation_oid == relation_oid;
        });
}

/** Snapshot and own one pre-TRUNCATE logical-key/old-locator capability. */
[[nodiscard]] bool capture_truncate_target(
        Oid relation_oid, TruncateReplacementCaptureV1& capture) {
    if (!OidIsValid(relation_oid)) {
        return false;
    }
    if (truncate_target_already_captured(capture, relation_oid)) {
        return true;
    }
    if (capture.count >= capture.targets.size()) {
        return false;
    }
    PhysicalIdentityV1 old_identity{};
    const RelationPhysicalIdentityStatusV1 identity_status =
        relation_physical_identity(relation_oid, &old_identity);
    const DirtyRelationObservationV1 observation{
        .relation_oid = relation_oid,
        .physical_identity_valid = identity_status
            == RelationPhysicalIdentityStatusV1::Precise,
        .storage_parent = identity_status
            == RelationPhysicalIdentityStatusV1::StorageParent,
        .physical_identity = old_identity,
        .observability = WriteObservability::ExecutorUtilityOnly,
    };
    const DependencyVersion old_version =
        relation_dependency_version(observation);
    if (old_version.key.database_oid == 0
        || old_version.committed_epoch == 0
        || !physical_identity_valid_v1(old_version.physical_identity)) {
        return false;
    }
    mark_current_transaction_relation_dirty(relation_oid);
    if (g_dirty_observations.coarse()) {
        return false;
    }
    capture.targets[capture.count++] = TruncateRelationReplacementV1{
        .relation_oid = relation_oid,
        .old_version = old_version,
    };
    return identity_status != RelationPhysicalIdentityStatusV1::Unknown;
}

/** Expand inheritance before execution while old locators remain observable. */
[[nodiscard]] bool capture_truncate_replacements(
        const TruncateStmt& truncate,
        TruncateReplacementCaptureV1& capture) {
    capture = {};
    capture.active = true;
    bool complete = truncate.relations != NIL
        && truncate.behavior != DROP_CASCADE;
    ListCell* cell = nullptr;
    foreach(cell, truncate.relations) {
        auto* range = static_cast<RangeVar*>(lfirst(cell));
        const Oid relation_oid = range == nullptr
            ? InvalidOid
            : RangeVarGetRelid(range, AccessExclusiveLock, true);
        if (!OidIsValid(relation_oid)) {
            complete = false;
            continue;
        }
        if (range->inh) {
            List* inheritors = find_all_inheritors(
                relation_oid, AccessExclusiveLock, nullptr);
            if (inheritors == NIL) {
                complete = false;
                continue;
            }
            ListCell* inherited = nullptr;
            foreach(inherited, inheritors) {
                if (!capture_truncate_target(
                        lfirst_oid(inherited), capture)) {
                    complete = false;
                }
            }
            list_free(inheritors);
        } else if (!capture_truncate_target(relation_oid, capture)) {
            complete = false;
        }
    }

    bool cascade_closure_complete = true;
    if (truncate.behavior == DROP_CASCADE && capture.count != 0) {
        List* relation_ids = NIL;
        for (std::size_t index = 0; index < capture.count; ++index) {
            relation_ids = lappend_oid(
                relation_ids, capture.targets[index].relation_oid);
        }
        for (;;) {
            List* referencing = heap_truncate_find_FKs(relation_ids);
            if (referencing == NIL) {
                break;
            }
            bool progressed = false;
            ListCell* referenced = nullptr;
            foreach(referenced, referencing) {
                const Oid relation_oid = lfirst_oid(referenced);
                if (truncate_target_already_captured(
                        capture, relation_oid)) {
                    continue;
                }
                if (capture.count >= capture.targets.size()) {
                    cascade_closure_complete = false;
                    break;
                }
                LockRelationOid(relation_oid, AccessExclusiveLock);
                if (!capture_truncate_target(relation_oid, capture)) {
                    cascade_closure_complete = false;
                    break;
                }
                relation_ids = lappend_oid(relation_ids, relation_oid);
                progressed = true;
            }
            list_free(referencing);
            if (!cascade_closure_complete || !progressed) {
                break;
            }
        }
        list_free(relation_ids);
    }
    capture.closure_complete = complete && cascade_closure_complete;
    /* CASCADE always carries a database guard in addition to the exact
     * pre/post physical replacement set: extension-owned or otherwise
     * undiscoverable targets must never retain a readable stale result. */
    return complete && truncate.behavior != DROP_CASCADE;
}

/** Stage each freshly swapped locator; transaction callbacks publish or abort. */
void stage_truncate_replacements_after_success(
        const TruncateReplacementCaptureV1& capture) noexcept {
    if (!capture.active || !IsTransactionState()) {
        return;
    }
    for (std::size_t index = 0; index < capture.count; ++index) {
        const TruncateRelationReplacementV1& target =
            capture.targets[index];
        PhysicalIdentityV1 new_identity{};
        const RelationPhysicalIdentityStatusV1 identity_status =
            relation_physical_identity(target.relation_oid, &new_identity);
        if (identity_status == RelationPhysicalIdentityStatusV1::Unknown) {
            mark_current_transaction_dirty();
            dependency_runtime::force_emergency_fallback();
            continue;
        }
        const DirtyRelationObservationV1 observation{
            .relation_oid = target.relation_oid,
            .physical_identity_valid = identity_status
                == RelationPhysicalIdentityStatusV1::Precise,
            .storage_parent = identity_status
                == RelationPhysicalIdentityStatusV1::StorageParent,
            .physical_identity = new_identity,
            .observability = WriteObservability::ExecutorUtilityOnly,
        };
        const DependencyVersion new_version =
            relation_dependency_version(observation);
        const dependency_runtime::DependencyRuntimeResultV1 staged =
            dependency_runtime::observe_dirty_dependency(
                target.old_version, new_version);
        if (!staged) {
            mark_current_transaction_dirty();
            dependency_runtime::force_emergency_fallback();
        }
    }
}

/** Resolve bounded utility targets while normal allocation/errors are legal. */
[[nodiscard]] bool mark_precise_utility_targets(const Node* statement) {
    if (g_dirty_observations.coarse()) {
        return true;
    }
    if (statement == nullptr) {
        return false;
    }
    if (IsA(statement, CopyStmt)) {
        const auto* copy = reinterpret_cast<const CopyStmt*>(statement);
        if (!copy->is_from || copy->relation == nullptr) {
            return false;
        }
        const Oid relation_oid = RangeVarGetRelid(
            copy->relation, RowExclusiveLock, true);
        if (!OidIsValid(relation_oid)) {
            return false;
        }
        mark_current_transaction_relation_dirty(relation_oid);
        return true;
    }
    return false;
}

/** Unknown utility classes remain conservatively coarse and fail open. */
void mark_utility_write_fail_open(
        const Node* statement,
        TruncateReplacementCaptureV1* truncate_capture = nullptr) {
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool precise = false;
    PG_TRY();
    {
        precise = statement != nullptr && IsA(statement, TruncateStmt)
            && truncate_capture != nullptr
            ? capture_truncate_replacements(
                *reinterpret_cast<const TruncateStmt*>(statement),
                *truncate_capture)
            : mark_precise_utility_targets(statement);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        precise = false;
    }
    PG_END_TRY();
    if (!precise) {
        mark_current_transaction_dirty();
    }
}

/** Conservatively publish one database-wide epoch for utility writes. */
void cache_process_utility(
        PlannedStmt* statement, const char* query_string, bool read_only_tree,
        ProcessUtilityContext context, ParamListInfo parameters,
        QueryEnvironment* environment, DestReceiver* destination,
        QueryCompletion* completion) {
    const ProcessUtility_hook_type previous_process_utility =
        executor::lifecycle::previous_process_utility();
    const auto* transaction = statement != nullptr
            && IsA(statement->utilityStmt, TransactionStmt)
        ? reinterpret_cast<const TransactionStmt*>(statement->utilityStmt)
        : nullptr;
    const bool commit_prepared = transaction != nullptr
        && transaction->kind == TRANS_STMT_COMMIT_PREPARED;
    const bool rollback_prepared = transaction != nullptr
        && transaction->kind == TRANS_STMT_ROLLBACK_PREPARED;
    const bool resolving_prepared = commit_prepared || rollback_prepared;
    dependency_runtime::ExternalPreparedResolutionTokenV1
        external_prepared_resolution{};
    bool external_prepared_resolution_started = false;
    if (resolving_prepared) {
        const bool registry_bound = bind_shared_reusable_registry();
        const std::uint64_t resolution_identity = registry_bound
            ? shared_registry::next_shared_registry_claim_sequence()
            : 0;
        if (registry_bound && resolution_identity != 0) {
            const dependency_runtime::DependencyRuntimeResultV1 begun =
                dependency_runtime::begin_external_prepared_resolution(
                    commit_prepared
                        ? dependency_runtime::
                            ExternalPreparedResolutionKindV1::CommitPrepared
                        : dependency_runtime::
                            ExternalPreparedResolutionKindV1::RollbackPrepared,
                    static_cast<std::uint32_t>(MyDatabaseId),
                    resolution_identity,
                    &external_prepared_resolution);
            external_prepared_resolution_started = begun
                && static_cast<bool>(external_prepared_resolution);
        }
        if (!external_prepared_resolution_started) {
            dependency_runtime::force_emergency_fallback();
            mark_shared_cache_failed();
        }
        g_force_coarse_transition = true;
        begin_database_transition(commit_prepared
            ? DatabaseTransitionPurposeV1::CommitPreparedResolution
            : DatabaseTransitionPurposeV1::RollbackPreparedResolution);
    }
    const bool dirty = statement != nullptr
        && utility_may_change_database(statement->utilityStmt);
    const bool outside_transaction = dirty && !IsTransactionState();
    TruncateReplacementCaptureV1 truncate_capture{};
    if (dirty && !outside_transaction) {
        mark_utility_write_fail_open(
            statement->utilityStmt, &truncate_capture);
    }
    if (resolving_prepared) {
        PG_TRY();
        {
            if (previous_process_utility != nullptr) {
                previous_process_utility(
                    statement, query_string, read_only_tree, context,
                    parameters, environment, destination, completion);
            } else {
                standard_ProcessUtility(
                    statement, query_string, read_only_tree, context,
                    parameters, environment, destination, completion);
            }
        }
        PG_CATCH();
        {
            MemoryContextSwitchTo(TopMemoryContext);
            ErrorData* error = CopyErrorData();
            FlushErrorState();
            if (external_prepared_resolution_started) {
                const dependency_runtime::DependencyRuntimeResultV1 finished =
                    dependency_runtime::finish_external_prepared_resolution(
                        &external_prepared_resolution,
                        dependency_runtime::
                            ExternalPreparedResolutionOutcomeV1::Ambiguous);
                if (!finished) {
                    mark_shared_cache_failed();
                }
            }
            if (commit_prepared) {
                g_force_coarse_transition = true;
                finish_database_transition(true);
            } else {
                finish_database_transition(false);
            }
            ReThrowError(error);
        }
        PG_END_TRY();
        if (external_prepared_resolution_started) {
            const dependency_runtime::DependencyRuntimeResultV1 finished =
                dependency_runtime::finish_external_prepared_resolution(
                    &external_prepared_resolution,
                    commit_prepared
                        ? dependency_runtime::
                            ExternalPreparedResolutionOutcomeV1::Committed
                        : dependency_runtime::
                            ExternalPreparedResolutionOutcomeV1::RolledBack);
            if (!finished) {
                mark_shared_cache_failed();
            }
        }
        finish_database_transition(commit_prepared);
    } else if (previous_process_utility != nullptr) {
        previous_process_utility(
            statement, query_string, read_only_tree, context, parameters,
            environment, destination, completion);
    } else {
        standard_ProcessUtility(
            statement, query_string, read_only_tree, context, parameters,
            environment, destination, completion);
    }
    stage_truncate_replacements_after_success(truncate_capture);
    if (outside_transaction) {
        g_force_coarse_transition = true;
        begin_database_transition(
            DatabaseTransitionPurposeV1::ImmediateWrite);
        finish_database_transition(true);
    }
}

/** Read one shared counter without exposing shared-memory pointers. */
[[nodiscard]] std::uint64_t read_counter(pg_atomic_uint64& counter) noexcept {
    return g_shared == nullptr ? 0 : pg_atomic_read_u64(&counter);
}

/** Enforce the documented schema-USAGE plus function-EXECUTE boundary. */
void require_cache_diagnostics_reader(Oid function_oid) {
    const Oid user_id = GetUserId();
    const Oid namespace_oid = OidIsValid(function_oid)
        ? get_func_namespace(function_oid)
        : InvalidOid;
    if (!superuser_arg(user_id)
        && (!OidIsValid(function_oid) || !OidIsValid(namespace_oid)
            || object_aclcheck(
                ProcedureRelationId,
                function_oid,
                user_id,
                ACL_EXECUTE) != ACLCHECK_OK
            || object_aclcheck(
                NamespaceRelationId,
                namespace_oid,
                user_id,
                ACL_USAGE) != ACLCHECK_OK)) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("permission denied to inspect bioseq_cache"),
             errhint("Grant USAGE on schema bioseq_cache and EXECUTE on the monitoring function.")));
    }
}

/** Apply the per-row producer/superuser/policy-monitor text rule. */
[[nodiscard]] CacheDiagnosticDisclosure diagnostic_disclosure_for_role(
        Oid producer_role_oid) noexcept {
    const Oid user_id = GetUserId();
    return cache_diagnostic_disclosure(
        superuser_arg(user_id),
        OidIsValid(producer_role_oid) && user_id == producer_role_oid,
        has_privs_of_role(user_id, ROLE_PG_READ_ALL_STATS));
}

struct EntrySnapshot {
    SharedEntryState state;
    std::uint32_t registry_index;
    std::uint64_t key_hash;
    std::uint64_t generation;
    std::uint64_t claim_sequence;
    std::uint64_t database_epoch;
    Oid database_oid;
    Oid current_role_oid;
    int producer_pid;
    std::uint32_t waiter_count;
    std::uint32_t reader_count;
    TimestampTz created_at;
    TimestampTz completed_at;
    TimestampTz expires_at;
    std::uint64_t row_count;
    std::uint64_t logical_bytes;
    std::uint64_t stored_bytes;
    std::uint64_t hits;
    ReusablePlanSnapshotStatusV1 canonical_plan_status;
    ReusablePlanSnapshotStatusV1 producer_plan_status;
    bool display_sql_truncated;
    bool canonical_plan_truncated;
    bool producer_plan_truncated;
    std::uint16_t relation_count;
    std::uint16_t syscache_dependency_count;
    Oid relation_oids[kMaximumRelationDependencies];
    ExactSyscacheDependencyV1
        syscache_dependencies[kMaxDependenciesPerGeneration];
};

[[nodiscard]] const char* entry_state_name(SharedEntryState state) noexcept {
    switch (state) {
        case SharedEntryState::Empty: return "EMPTY";
        case SharedEntryState::Claimed: return "CLAIMED";
        case SharedEntryState::Producing: return "BUILDING";
        case SharedEntryState::Complete: return "COMPLETE";
        case SharedEntryState::Failed: return "FAILED";
        case SharedEntryState::Retired: return "RETIRED";
    }
    return "UNKNOWN";
}

/** Render one reusable core state without exposing registry internals. */
[[nodiscard]] const char* reusable_entry_state_name(
        CacheEntryState state) noexcept {
    switch (state) {
        case CacheEntryState::Absent: return "ABSENT";
        case CacheEntryState::Claimed: return "CLAIMED";
        case CacheEntryState::Building: return "BUILDING";
        case CacheEntryState::Complete: return "COMPLETE";
        case CacheEntryState::Failed: return "FAILED";
        case CacheEntryState::Invalidated: return "INVALIDATED";
        case CacheEntryState::Evicting: return "EVICTING";
    }
    return "UNKNOWN";
}

/** Derive the current administrative action from durable reusable state. */
[[nodiscard]] const char* reusable_entry_action_name(
        CacheEntryState state) noexcept {
    switch (state) {
        case CacheEntryState::Claimed: return "claim";
        case CacheEntryState::Building: return "produce";
        case CacheEntryState::Complete: return "read";
        case CacheEntryState::Failed: return "fallback";
        case CacheEntryState::Invalidated: return "invalidate";
        case CacheEntryState::Evicting: return "evict";
        case CacheEntryState::Absent: return "absent";
    }
    return "unknown";
}

/** Render a stable failure label for redacted diagnostics JSON. */
[[nodiscard]] const char* reusable_failure_name(
        CacheFailureCode failure) noexcept {
    switch (failure) {
        case CacheFailureCode::None: return "None";
        case CacheFailureCode::ProducerError: return "ProducerError";
        case CacheFailureCode::ProducerCancelled: return "ProducerCancelled";
        case CacheFailureCode::ProducerExited: return "ProducerExited";
        case CacheFailureCode::DependencyChanged: return "DependencyChanged";
        case CacheFailureCode::SnapshotMismatch: return "SnapshotMismatch";
        case CacheFailureCode::AdmissionDenied: return "AdmissionDenied";
        case CacheFailureCode::StorageFailure: return "StorageFailure";
        case CacheFailureCode::CorruptResult: return "CorruptResult";
        case CacheFailureCode::UnsupportedType: return "UnsupportedType";
        case CacheFailureCode::RuntimeSemanticDigestUnavailable:
            return "RuntimeSemanticDigestUnavailable";
    }
    return "Unknown";
}

/** Render the bounded write-observability class. */
[[nodiscard]] const char* reusable_observability_name(
        WriteObservability observability) noexcept {
    switch (observability) {
        case WriteObservability::None: return "None";
        case WriteObservability::ProviderComplete: return "ProviderComplete";
        case WriteObservability::ExecutorUtilityOnly:
            return "ExecutorUtilityOnly";
        case WriteObservability::DatabaseWideFallback:
            return "DatabaseWideFallback";
    }
    return "Unknown";
}

/** Render one bounded producer-plan capture status. */
[[nodiscard]] const char* reusable_plan_snapshot_status_name(
        ReusablePlanSnapshotStatusV1 status) noexcept {
    switch (status) {
        case ReusablePlanSnapshotStatusV1::Unavailable:
            return "Unavailable";
        case ReusablePlanSnapshotStatusV1::Planned:
            return "Planned";
        case ReusablePlanSnapshotStatusV1::Analyzed:
            return "Analyzed";
    }
    return "Unknown";
}

/** Format a collision-index digest without reading canonical identity bytes. */
[[nodiscard]] std::array<char, 65> reusable_digest_hex(
        const CacheDigest256& digest) noexcept {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 65> output{};
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        output[index * 2] = digits[digest.bytes[index] >> 4U];
        output[index * 2 + 1] = digits[digest.bytes[index] & 0x0fU];
    }
    return output;
}

/** Convert one monotonic event to an approximate wall timestamp at snapshot. */
[[nodiscard]] std::optional<TimestampTz> reusable_wall_timestamp(
        TimestampTz now_wall,
        std::uint64_t now_monotonic_ns,
        std::uint64_t event_monotonic_ns) noexcept {
    if (event_monotonic_ns == 0 || event_monotonic_ns > now_monotonic_ns) {
        return std::nullopt;
    }
    const std::uint64_t delta_us =
        (now_monotonic_ns - event_monotonic_ns) / 1'000U;
    if (delta_us > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())) {
        return std::nullopt;
    }
    return now_wall - static_cast<std::int64_t>(delta_us);
}

/** Convert a bounded monotonic age to PostgreSQL interval microseconds. */
[[nodiscard]] std::int64_t reusable_age_us(
        std::uint64_t now_monotonic_ns,
        std::uint64_t started_monotonic_ns) noexcept {
    if (started_monotonic_ns == 0 || started_monotonic_ns > now_monotonic_ns) {
        return 0;
    }
    return static_cast<std::int64_t>(std::min<std::uint64_t>(
        (now_monotonic_ns - started_monotonic_ns) / 1'000U,
        static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())));
}

/** Convert a reusable TTL to a saturated PostgreSQL interval. */
[[nodiscard]] std::int64_t reusable_ttl_us(std::uint64_t ttl_ms) noexcept {
    constexpr std::uint64_t maximum = static_cast<std::uint64_t>(
        (std::numeric_limits<std::int64_t>::max)());
    return static_cast<std::int64_t>(
        ttl_ms > maximum / 1'000U ? maximum : ttl_ms * 1'000U);
}

/** Reap dead owners and converge unreferenced terminal slots to EMPTY. */
[[nodiscard]] std::uint64_t sweep_registry(bool expire_completed) noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
    const bool storage_attached = storage::storage_ready()
        || storage::attach_storage();
    auto* workspace =
        executor::try_construct_context_owned<
            ExactRegistrySweepWorkspaceV1>(CurrentMemoryContext);
    if (workspace == nullptr) {
        return 0;
    }
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (std::size_t index = 0; index < kRegistryCapacity; ++index) {
        const CacheEntry& entry = g_shared->entries[index];
        if (exact_entry_in_flight(entry.state)) {
            workspace->producers[index] = ExactProducerWaitTokenV1{
                .entry_id = cache_entry_id(entry),
                .producer = exact_producer_identity(entry),
                .database_epoch = entry.database_epoch,
                .completion_count = entry.completion_count,
            };
            append_backend_identity(
                workspace->identities,
                workspace->producers[index].producer);
        }
        for (std::size_t owner_index = 0;
             owner_index < storage::kReaderPinsPerManifest; ++owner_index) {
            const RegistryReaderOwner& reader = entry.readers[owner_index];
            if (reader.active) {
                workspace->readers[index][owner_index] =
                    registry_owner_observation(reader);
                append_backend_identity(
                    workspace->identities,
                    workspace->readers[index][owner_index].owner);
            }
        }
        for (std::size_t owner_index = 0;
             owner_index < storage::kWaitersPerManifest; ++owner_index) {
            const RegistryWaiterOwner& waiter = entry.waiters[owner_index];
            if (waiter.active) {
                workspace->waiters[index][owner_index] =
                    registry_owner_observation(waiter);
                append_backend_identity(
                    workspace->identities,
                    workspace->waiters[index][owner_index].owner);
            }
        }
    }
    LWLockRelease(g_registry_lock);
    probe_backend_liveness(workspace->identities, workspace->liveness);
    storage::ManifestHandle manifests[kRegistryCapacity]{};
    std::size_t manifest_count = 0;
    std::uint64_t swept = 0;
    const TimestampTz now = GetCurrentTimestamp();
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t index = 0; index < kRegistryCapacity; ++index) {
        CacheEntry& entry = g_shared->entries[index];
        reap_dead_registry_readers_locked(
            entry, workspace->readers[index], workspace->liveness);
        reap_dead_registry_waiters_locked(
            entry, storage_attached, workspace->waiters[index],
            workspace->liveness);
        const bool dead_producer = exact_entry_in_flight(entry.state)
            && workspace->producers[index]
            && !backend_identity_alive_in_sample(
                workspace->liveness,
                workspace->producers[index].producer)
            && exact_entry_matches_wait_token(
                entry, workspace->producers[index], true);
        const bool expired = expire_completed
            && entry.state == SharedEntryState::Complete
            && entry.expires_at <= now
            && entry.waiter_count == 0
            && entry.reader_count == 0;
        const bool terminal_reclaimable =
            entry.state == SharedEntryState::Retired
            && entry.waiter_count == 0
            && entry.reader_count == 0;
        if (!dead_producer && !expired && !terminal_reclaimable) {
            continue;
        }
        if (dead_producer) {
            fail_abandoned_producer(entry);
        } else if (expired) {
            entry.state = SharedEntryState::Retired;
            ConditionVariableBroadcast(&entry.changed);
        }
        const bool reclaimable =
            entry.state == SharedEntryState::Retired
            && entry.waiter_count == 0
            && entry.reader_count == 0;
        if (storage_attached && reclaimable && entry.manifest_handle) {
            manifests[manifest_count++] = entry.manifest_handle;
            entry.manifest_handle = {};
        }
        if (reclaimable && !entry.manifest_handle) {
            recycle_registry_entry_locked(entry);
        }
        if (expired) {
            pg_atomic_fetch_add_u64(&g_shared->stats.evictions, 1);
        }
        ++swept;
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < manifest_count; ++index) {
        static_cast<void>(storage::retire_manifest(manifests[index]));
    }
    executor::destroy_context_owned(workspace);
    return swept;
}

/** Unpublish registry identities before storage reclaims retired manifests. */
void cache_registry_janitor_sweep_v1() noexcept {
    static_cast<void>(sweep_registry(true));
    reusable_registry_janitor_sweep_v1();
}

/** Copy bounded diagnostics under lock; never hold shared locks during SQL I/O. */
[[nodiscard]] int snapshot_entries(EntrySnapshot* snapshots) noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
    int count = 0;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (std::size_t offset = 0; offset < kRegistryCapacity; ++offset) {
        const CacheEntry& entry = g_shared->entries[offset];
        if (entry.state == SharedEntryState::Empty) {
            continue;
        }
        EntrySnapshot& snapshot = snapshots[count++];
        snapshot.state = entry.state;
        snapshot.registry_index = static_cast<std::uint32_t>(offset);
        snapshot.key_hash = entry.key_hash;
        snapshot.generation = entry.generation;
        snapshot.claim_sequence = entry.claim_sequence;
        snapshot.database_epoch = entry.database_epoch;
        snapshot.database_oid = entry.database_oid;
        snapshot.current_role_oid = entry.current_role_oid;
        snapshot.producer_pid = entry.producer_pid;
        snapshot.waiter_count = entry.waiter_count;
        snapshot.reader_count = entry.reader_count;
        snapshot.created_at = entry.created_at;
        snapshot.completed_at = entry.completed_at;
        snapshot.expires_at = entry.expires_at;
        snapshot.row_count = entry.row_count;
        snapshot.logical_bytes = entry.logical_bytes;
        snapshot.stored_bytes = entry.stored_bytes;
        snapshot.hits = entry.hits;
        snapshot.canonical_plan_status = entry.canonical_plan_status;
        snapshot.producer_plan_status = entry.producer_plan_status;
        snapshot.display_sql_truncated = entry.display_sql_truncated;
        snapshot.canonical_plan_truncated =
            entry.canonical_plan_truncated;
        snapshot.producer_plan_truncated = entry.producer_plan_truncated;
        snapshot.relation_count = entry.relation_count
                <= kMaximumRelationDependencies
            ? entry.relation_count
            : static_cast<std::uint16_t>(kMaximumRelationDependencies);
        snapshot.syscache_dependency_count =
            entry.syscache_dependency_count
                <= kMaxDependenciesPerGeneration
            ? entry.syscache_dependency_count
            : static_cast<std::uint16_t>(
                kMaxDependenciesPerGeneration);
        std::memcpy(
            snapshot.relation_oids,
            entry.relation_oids,
            static_cast<std::size_t>(snapshot.relation_count) * sizeof(Oid));
        std::memcpy(
            snapshot.syscache_dependencies,
            entry.syscache_dependencies,
            static_cast<std::size_t>(snapshot.syscache_dependency_count)
                * sizeof(ExactSyscacheDependencyV1));
    }
    LWLockRelease(g_registry_lock);
    return count;
}

struct DiagnosticTextWorkspaceV1 {
    void* allocation = nullptr;
    std::span<char> display_sql{};
    std::span<char> canonical_plan{};
    std::span<char> producer_plan{};
};

/** Allocate one row-local diagnostic workspace without raising on OOM. */
[[nodiscard]] bool allocate_diagnostic_text_workspace(
        std::size_t display_sql_capacity,
        std::size_t canonical_plan_capacity,
        std::size_t producer_plan_capacity,
        DiagnosticTextWorkspaceV1* output) noexcept {
    if (output == nullptr) {
        return false;
    }
    *output = {};
    const std::size_t maximum = static_cast<std::size_t>(MaxAllocSize);
    if (display_sql_capacity == 0 || canonical_plan_capacity == 0
        || producer_plan_capacity == 0
        || display_sql_capacity > maximum
        || canonical_plan_capacity > maximum - display_sql_capacity
        || producer_plan_capacity
            > maximum - display_sql_capacity - canonical_plan_capacity) {
        return false;
    }
    const std::size_t total = display_sql_capacity
        + canonical_plan_capacity + producer_plan_capacity;
    auto* allocation = static_cast<char*>(palloc_extended(
        total, MCXT_ALLOC_NO_OOM));
    if (allocation == nullptr) {
        return false;
    }
    output->allocation = allocation;
    output->display_sql = std::span<char>{
        allocation, display_sql_capacity};
    output->canonical_plan = std::span<char>{
        allocation + display_sql_capacity, canonical_plan_capacity};
    output->producer_plan = std::span<char>{
        allocation + display_sql_capacity + canonical_plan_capacity,
        producer_plan_capacity};
    return true;
}

void release_diagnostic_text_workspace(
        DiagnosticTextWorkspaceV1* workspace) noexcept {
    if (workspace == nullptr) {
        return;
    }
    if (workspace->allocation != nullptr) {
        pfree(workspace->allocation);
    }
    *workspace = {};
}

struct ExactDiagnosticTextSnapshotV1 {
    std::span<char> display_sql{};
    std::span<char> canonical_plan{};
    std::span<char> producer_plan{};
    std::uint32_t display_sql_bytes = 0;
    std::uint32_t canonical_plan_bytes = 0;
    std::uint32_t producer_plan_bytes = 0;
    bool copied = false;
};

/** Copy exact text only after row-level authorization and an ABA recheck. */
[[nodiscard]] bool copy_exact_entry_text(
        const EntrySnapshot& snapshot,
        ExactDiagnosticTextSnapshotV1* output) noexcept {
    if (output == nullptr || g_shared == nullptr
        || snapshot.registry_index >= kRegistryCapacity
        || output->display_sql.empty() || output->canonical_plan.empty()
        || output->producer_plan.empty()) {
        return false;
    }
    output->display_sql_bytes = 0;
    output->canonical_plan_bytes = 0;
    output->producer_plan_bytes = 0;
    output->copied = false;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    const CacheEntry& entry = g_shared->entries[snapshot.registry_index];
    const bool matches = entry.generation == snapshot.generation
        && entry.claim_sequence == snapshot.claim_sequence
        && entry.state == snapshot.state;
    if (matches) {
        const std::size_t display_sql_bytes = strnlen(
            entry.display_sql.data(), entry.display_sql.size());
        output->display_sql_bytes = static_cast<std::uint32_t>(std::min(
            display_sql_bytes, output->display_sql.size()));
        output->canonical_plan_bytes = std::min<std::uint32_t>(
            entry.canonical_plan_bytes,
            static_cast<std::uint32_t>(output->canonical_plan.size()));
        output->producer_plan_bytes = std::min<std::uint32_t>(
            entry.producer_plan_bytes,
            static_cast<std::uint32_t>(output->producer_plan.size()));
        std::memcpy(
            output->display_sql.data(),
            entry.display_sql.data(),
            output->display_sql_bytes);
        std::memcpy(
            output->canonical_plan.data(),
            entry.canonical_plan.data(),
            output->canonical_plan_bytes);
        std::memcpy(
            output->producer_plan.data(),
            entry.producer_plan.data(),
            output->producer_plan_bytes);
        output->copied = true;
    }
    LWLockRelease(g_registry_lock);
    return output->copied;
}

/** Copy redacted reusable rows under membership shared plus exact partitions. */
[[nodiscard]] ReusableGenerationDiagnosticsResultV1
snapshot_reusable_entries(
        std::span<ReusableGenerationDiagnosticsRowV1> rows) noexcept {
    ReusableGenerationDiagnosticsResultV1 result{};
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr
        || g_reusable_exact_locks == nullptr
        || g_reusable_coverage_locks == nullptr) {
        return result;
    }
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
        membership_owned = true;
        result = snapshot_reusable_generation_diagnostics(
            g_reusable_registry, rows, reusable_partition_callbacks());
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        result = ReusableGenerationDiagnosticsResultV1{
            .error = ReusableRegistryErrorV1::InvalidState,
        };
    }
    PG_END_TRY();
    return result;
}

struct ReusableDiagnosticTextSnapshotV1 {
    ReusableDiagnosticTextCopyV1 metadata{};
    std::span<char> display_sql{};
    std::span<char> canonical_plan{};
    std::span<char> producer_plan{};
    bool copied = false;
};

/** Copy row text only after the SQL caller passed row-level authorization. */
[[nodiscard]] bool copy_reusable_entry_text(
        const ReusableGenerationReferenceV1& reference,
        ReusableDiagnosticTextSnapshotV1* output) noexcept {
    if (!reference || output == nullptr || !g_reusable_registry
        || output->display_sql.empty() || output->canonical_plan.empty()
        || output->producer_plan.empty()
        || g_reusable_membership_lock == nullptr) {
        return false;
    }
    output->metadata = {};
    output->copied = false;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool accepted = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
        membership_owned = true;
        const auto copied = copy_reusable_diagnostic_text(
            g_reusable_registry,
            reference,
            output->display_sql,
            output->canonical_plan,
            output->producer_plan,
            reusable_partition_callbacks());
        if (copied.has_value()) {
            output->metadata = *copied;
            output->copied = true;
            accepted = true;
        }
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        output->metadata = {};
        output->copied = false;
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = false;
    }
    PG_END_TRY();
    return accepted;
}

/** Append one affected reusable identity exactly once to caller-owned storage. */
[[nodiscard]] bool append_reusable_invalidation_reference(
        std::span<ReusableGenerationReferenceV1> references,
        std::size_t& count,
        const ReusableGenerationReferenceV1& reference) noexcept {
    for (std::size_t index = 0; index < count; ++index) {
        if (references[index] == reference) {
            return true;
        }
    }
    if (count >= references.size()) {
        return false;
    }
    references[count++] = reference;
    return true;
}

/** Run every bounded cursor page for one selector while membership is exclusive. */
[[nodiscard]] bool invalidate_reusable_selector_locked(
        ReusableInvalidationSelectorV1 selector,
        std::span<ReusableGenerationReferenceV1> affected,
        std::size_t& affected_count,
        std::uint64_t& invalidated) noexcept {
    for (;;) {
        const ReusableInvalidationResultV1 page =
            invalidate_reusable_generations(
                g_reusable_registry,
                selector,
                reusable_partition_callbacks());
        if (page.error != ReusableRegistryErrorV1::None) {
            return false;
        }
        invalidated += static_cast<std::uint64_t>(page.failed_inflight)
            + static_cast<std::uint64_t>(page.invalidated_complete);
        for (std::size_t index = 0; index < page.affected_count; ++index) {
            if (!append_reusable_invalidation_reference(
                    affected, affected_count,
                    page.affected_references[index])) {
                return false;
            }
        }
        if (page.done) {
            return true;
        }
        if (page.next_cursor <= selector.cursor) {
            return false;
        }
        selector.cursor = page.next_cursor;
    }
}

/**
 * Invalidate reusable generations without retiring storage under registry
 * locks. A null identity means every database represented in the snapshot.
 */
[[nodiscard]] std::uint64_t invalidate_reusable_registry(
        const std::optional<PhysicalIdentityV1>& physical_identity) {
    if (!bind_admin_reusable_registry("bioseq_cache invalidation")
        || !g_reusable_registry || g_reusable_membership_lock == nullptr
        || g_reusable_wait_slot_count == 0) {
        return 0;
    }
    const std::size_t capacity = g_reusable_wait_slot_count;
    auto* affected = static_cast<ReusableGenerationReferenceV1*>(palloc0(
        sizeof(ReusableGenerationReferenceV1) * capacity));
    auto* diagnostics = physical_identity.has_value()
        ? nullptr
        : static_cast<ReusableGenerationDiagnosticsRowV1*>(palloc0(
            sizeof(ReusableGenerationDiagnosticsRowV1) * capacity));
    std::size_t affected_count = 0;
    std::uint64_t invalidated = 0;
    bool complete = true;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        if (physical_identity.has_value()) {
            complete = invalidate_reusable_selector_locked(
                ReusableInvalidationSelectorV1{
                    .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
                    .physical_identity = *physical_identity,
                    .match_physical_identity = true,
                },
                std::span<ReusableGenerationReferenceV1>{
                    affected, capacity},
                affected_count,
                invalidated);
            if (complete) {
                complete = invalidate_reusable_selector_locked(
                    ReusableInvalidationSelectorV1{
                        .database_oid = static_cast<std::uint32_t>(
                            MyDatabaseId),
                        .observability =
                            WriteObservability::DatabaseWideFallback,
                        .match_observability = true,
                    },
                    std::span<ReusableGenerationReferenceV1>{
                        affected, capacity},
                    affected_count,
                    invalidated);
            }
        } else {
            const auto snapshot = snapshot_reusable_generation_diagnostics(
                g_reusable_registry,
                std::span<ReusableGenerationDiagnosticsRowV1>{
                    diagnostics, capacity},
                reusable_partition_callbacks());
            complete = snapshot.error == ReusableRegistryErrorV1::None
                && !snapshot.truncated;
            for (std::size_t index = 0;
                 complete && index < snapshot.count;
                 ++index) {
                const std::uint32_t database_oid =
                    diagnostics[index].database_oid;
                if (database_oid == 0) {
                    complete = false;
                    break;
                }
                bool duplicate = false;
                for (std::size_t prior = 0; prior < index; ++prior) {
                    duplicate = duplicate
                        || diagnostics[prior].database_oid == database_oid;
                }
                if (!duplicate) {
                    complete = invalidate_reusable_selector_locked(
                        ReusableInvalidationSelectorV1{
                            .database_oid = database_oid,
                            .coarse_database_fallback = true,
                        },
                        std::span<ReusableGenerationReferenceV1>{
                            affected, capacity},
                        affected_count,
                        invalidated);
                }
            }
        }
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        pfree(affected);
        if (diagnostics != nullptr) {
            pfree(diagnostics);
        }
        PG_RE_THROW();
    }
    PG_END_TRY();

    for (std::size_t index = 0; index < affected_count; ++index) {
        broadcast_reusable_generation(affected[index]);
    }
    pfree(affected);
    if (diagnostics != nullptr) {
        pfree(diagnostics);
    }
    if (!complete) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq_cache reusable invalidation did not complete")));
    }
    if (invalidated != 0) {
        pg_atomic_fetch_add_u64(&g_shared->stats.invalidations, invalidated);
        storage::request_janitor_sweep(false);
    }
    return invalidated;
}

/** Precisely invalidate a reusable physical leaf, conservatively if absent. */
[[nodiscard]] std::uint64_t invalidate_reusable_relation_registry(
        Oid relation_oid) {
    if (!bind_admin_reusable_registry(
            "bioseq_cache relation invalidation")) {
        return 0;
    }
    PhysicalIdentityV1 physical_identity{};
    if (relation_physical_identity(relation_oid, &physical_identity)
            == RelationPhysicalIdentityStatusV1::Precise) {
        return invalidate_reusable_registry(physical_identity);
    }
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr
        || g_reusable_wait_slot_count == 0) {
        return 0;
    }
    const std::size_t capacity = g_reusable_wait_slot_count;
    auto* affected = static_cast<ReusableGenerationReferenceV1*>(palloc0(
        sizeof(ReusableGenerationReferenceV1) * capacity));
    std::size_t affected_count = 0;
    std::uint64_t invalidated = 0;
    bool complete = true;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        complete = invalidate_reusable_selector_locked(
            ReusableInvalidationSelectorV1{
                .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
                .coarse_database_fallback = true,
            },
            std::span<ReusableGenerationReferenceV1>{affected, capacity},
            affected_count,
            invalidated);
        LWLockRelease(g_reusable_membership_lock);
        membership_owned = false;
    }
    PG_CATCH();
    {
        MemoryContextSwitchTo(caller_context);
        if (membership_owned) {
            LWLockRelease(g_reusable_membership_lock);
            membership_owned = false;
        }
        pfree(affected);
        PG_RE_THROW();
    }
    PG_END_TRY();
    for (std::size_t index = 0; index < affected_count; ++index) {
        broadcast_reusable_generation(affected[index]);
    }
    pfree(affected);
    if (!complete) {
        ereport(ERROR,
            (errcode(ERRCODE_DATA_CORRUPTED),
             errmsg("bioseq_cache reusable relation invalidation failed")));
    }
    if (invalidated != 0) {
        pg_atomic_fetch_add_u64(&g_shared->stats.invalidations, invalidated);
        storage::request_janitor_sweep(false);
    }
    return invalidated;
}

/** Retire all live generations and wake waiters; returns affected entries. */
[[nodiscard]] std::uint64_t invalidate_registry() noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
    const bool storage_attached = storage::storage_ready()
        || storage::attach_storage();
    storage::ManifestHandle manifests[kRegistryCapacity]{};
    std::size_t manifest_count = 0;
    std::uint64_t invalidated = 0;
    pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
    pg_atomic_fetch_add_u64(&g_shared->coarse_dependency_epoch, 1);
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (CacheEntry& entry : g_shared->entries) {
        if (exact_entry_in_flight(entry.state)) {
            fail_abandoned_producer(entry);
            ++invalidated;
            if (storage_attached && entry.reader_count == 0
                && entry.manifest_handle) {
                manifests[manifest_count++] = entry.manifest_handle;
                entry.manifest_handle = {};
            }
            continue;
        }
        if (entry.state != SharedEntryState::Complete) {
            continue;
        }
        entry.state = SharedEntryState::Retired;
        ++invalidated;
        if (storage_attached && entry.reader_count == 0
            && entry.manifest_handle) {
            manifests[manifest_count++] = entry.manifest_handle;
            entry.manifest_handle = {};
        }
        ConditionVariableBroadcast(&entry.changed);
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < manifest_count; ++index) {
        static_cast<void>(storage::retire_manifest(manifests[index]));
    }
    if (invalidated != 0) {
        pg_atomic_fetch_add_u64(
            &g_shared->stats.invalidations, invalidated);
    }
    ConditionVariableBroadcast(&g_shared->database_changed);
    return invalidated;
}

[[nodiscard]] bool entry_depends_on_relation(
        const CacheEntry& entry, Oid relation_oid) noexcept {
    if (entry.database_oid != MyDatabaseId) {
        return false;
    }
    for (std::uint16_t index = 0; index < entry.relation_count; ++index) {
        if (entry.relation_oids[index] == relation_oid) {
            return true;
        }
    }
    return false;
}

/** Match the full cacheId/hashValue identity; zero hash means cache-wide. */
[[nodiscard]] bool entry_depends_on_syscache(
        const CacheEntry& entry, int cache_id,
        std::uint32_t hash_value) noexcept {
    if (entry.database_oid != MyDatabaseId || cache_id < 0) {
        return false;
    }
    const std::size_t dependency_count = std::min<std::size_t>(
        entry.syscache_dependency_count,
        kMaxDependenciesPerGeneration);
    for (std::size_t index = 0; index < dependency_count; ++index) {
        const ExactSyscacheDependencyV1& dependency =
            entry.syscache_dependencies[index];
        if (dependency.cache_id == cache_id
            && (hash_value == 0 || dependency.hash_value == hash_value)) {
            return true;
        }
    }
    return false;
}

/** Retire one exact generation only after its full ABA identity recheck. */
[[nodiscard]] bool retire_exact_dependency_entry_locked(
        CacheEntry& entry, const CacheEntryId& expected) noexcept {
    if (cache_entry_id(entry) != expected) {
        return false;
    }
    if (exact_entry_in_flight(entry.state)) {
        fail_abandoned_producer(entry);
        return true;
    }
    if (entry.state != SharedEntryState::Complete) {
        return false;
    }
    entry.state = SharedEntryState::Retired;
    ConditionVariableBroadcast(&entry.changed);
    return true;
}

/** Retire exact generations named by a PostgreSQL syscache invalidation. */
void cache_exact_syscache_invalidation(
        Datum, int cache_id, std::uint32_t hash_value) noexcept {
    if (g_shared == nullptr || g_registry_lock == nullptr
        || (cache_id != PROCOID && cache_id != AGGFNOID)) {
        return;
    }
    std::uint64_t invalidated = 0;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (CacheEntry& entry : g_shared->entries) {
        if (!entry_depends_on_syscache(entry, cache_id, hash_value)) {
            continue;
        }
        const CacheEntryId expected = cache_entry_id(entry);
        if (retire_exact_dependency_entry_locked(entry, expected)) {
            ++invalidated;
        }
    }
    LWLockRelease(g_registry_lock);
    if (invalidated != 0) {
        pg_atomic_fetch_add_u64(
            &g_shared->stats.invalidations, invalidated);
        ConditionVariableBroadcast(&g_shared->database_changed);
        storage::request_janitor_sweep(false);
    }
}

/** Advance and retire only generations that reference one relation. */
[[nodiscard]] std::uint64_t invalidate_relation_registry(
        Oid relation_oid) noexcept {
    if (g_shared == nullptr || !OidIsValid(relation_oid)) {
        return 0;
    }
    const bool storage_attached = storage::storage_ready()
        || storage::attach_storage();
    storage::ManifestHandle manifests[kRegistryCapacity]{};
    std::size_t manifest_count = 0;
    std::uint64_t invalidated = 0;
    pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    if (!bump_relation_epoch_locked(relation_oid)) {
        pg_atomic_fetch_add_u64(&g_shared->coarse_dependency_epoch, 1);
    }
    for (CacheEntry& entry : g_shared->entries) {
        if (!entry_depends_on_relation(entry, relation_oid)) {
            continue;
        }
        if (exact_entry_in_flight(entry.state)) {
            fail_abandoned_producer(entry);
            ++invalidated;
            if (storage_attached && entry.reader_count == 0
                && entry.manifest_handle) {
                manifests[manifest_count++] = entry.manifest_handle;
                entry.manifest_handle = {};
            }
            continue;
        }
        if (entry.state != SharedEntryState::Complete) {
            continue;
        }
        entry.state = SharedEntryState::Retired;
        ++invalidated;
        if (storage_attached && entry.reader_count == 0
            && entry.manifest_handle) {
            manifests[manifest_count++] = entry.manifest_handle;
            entry.manifest_handle = {};
        }
        ConditionVariableBroadcast(&entry.changed);
    }
    LWLockRelease(g_registry_lock);
    for (std::size_t index = 0; index < manifest_count; ++index) {
        static_cast<void>(storage::retire_manifest(manifests[index]));
    }
    if (invalidated != 0) {
        pg_atomic_fetch_add_u64(
            &g_shared->stats.invalidations, invalidated);
    }
    ConditionVariableBroadcast(&g_shared->database_changed);
    return invalidated;
}

/** Convert one PostgreSQL wall timestamp to its owning SQL representation. */
[[nodiscard]] bioseq::pg_utils::sql_timestamptz owning_timestamp(
        TimestampTz value) noexcept {
    if (TIMESTAMP_IS_NOBEGIN(value)) {
        return {
            .kind =
                bioseq::pg_utils::sql_timestamp_kind::negative_infinity,
        };
    }
    if (TIMESTAMP_IS_NOEND(value)) {
        return {
            .kind =
                bioseq::pg_utils::sql_timestamp_kind::positive_infinity,
        };
    }
    return {
        .kind = bioseq::pg_utils::sql_timestamp_kind::finite,
        .postgres_microseconds = static_cast<std::int64_t>(value),
    };
}

/** Format one legacy exact-result key without PostgreSQL allocation. */
[[nodiscard]] std::string exact_entry_key(std::uint64_t key_hash) {
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 16> encoded{};
    for (std::size_t index = 0; index < encoded.size(); ++index) {
        const std::size_t shift = (encoded.size() - index - 1U) * 4U;
        encoded[index] = digits[(key_hash >> shift) & 0x0fU];
    }
    return std::string{encoded.data(), encoded.size()};
}

/** Build the owning dependency JSON for one exact-result generation. */
[[nodiscard]] std::string exact_dependency_json(
        const EntrySnapshot& entry) {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [entry_pointer = &entry] {
            const EntrySnapshot& snapshot = *entry_pointer;
            StringInfoData dependencies{};
            initStringInfo(&dependencies);
            appendStringInfo(
                &dependencies,
                "{\"database_epoch\":%llu,"
                "\"canonical_plan_status\":\"%s\","
                "\"producer_plan_status\":\"%s\","
                "\"display_sql_truncated\":%s,"
                "\"canonical_plan_truncated\":%s,"
                "\"producer_plan_truncated\":%s,"
                "\"relation_oids\":[",
                static_cast<unsigned long long>(snapshot.database_epoch),
                reusable_plan_snapshot_status_name(
                    snapshot.canonical_plan_status),
                reusable_plan_snapshot_status_name(
                    snapshot.producer_plan_status),
                snapshot.display_sql_truncated ? "true" : "false",
                snapshot.canonical_plan_truncated ? "true" : "false",
                snapshot.producer_plan_truncated ? "true" : "false");
            for (std::uint16_t index = 0;
                 index < snapshot.relation_count;
                 ++index) {
                appendStringInfo(
                    &dependencies,
                    "%s%u",
                    index == 0 ? "" : ",",
                    snapshot.relation_oids[index]);
            }
            appendStringInfoString(
                &dependencies, "],\"syscache_items\":[");
            for (std::uint16_t index = 0;
                 index < snapshot.syscache_dependency_count;
                 ++index) {
                const ExactSyscacheDependencyV1& dependency =
                    snapshot.syscache_dependencies[index];
                appendStringInfo(
                    &dependencies,
                    "%s{\"cache_id\":%d,\"hash_value\":%u}",
                    index == 0 ? "" : ",",
                    dependency.cache_id,
                    dependency.hash_value);
            }
            appendStringInfoString(&dependencies, "]}");
            std::string result{
                dependencies.data,
                static_cast<std::size_t>(dependencies.len)};
            pfree(dependencies.data);
            return result;
        });
}

/** Build the owning dependency JSON for one reusable generation. */
[[nodiscard]] std::string reusable_dependency_json(
        const ReusableGenerationDiagnosticsRowV1& entry) {
    return bioseq::pg_utils::invoke_pg_as_cpp(
        [entry_pointer = &entry] {
            const ReusableGenerationDiagnosticsRowV1& snapshot =
                *entry_pointer;
            const auto dependency = reusable_digest_hex(
                snapshot.dependency_fingerprint);
            StringInfoData dependencies{};
            initStringInfo(&dependencies);
            appendStringInfo(
                &dependencies,
                "{\"action\":\"%s\",\"failure\":\"%s\","
                "\"provider_id\":%u,\"provider_abi_version\":%u,"
                "\"provider_domain_abi_version\":%u,"
                "\"compatibility_family_id\":%llu,"
                "\"capability_mask\":%llu,"
                "\"dependency_count\":%u,"
                "\"dependency_fingerprint\":\"%s\","
                "\"observability\":\"%s\",\"planner_pins\":%u,"
                "\"canonical_plan_status\":\"%s\","
                "\"producer_plan_status\":\"%s\","
                "\"display_sql_truncated\":%s,"
                "\"canonical_plan_truncated\":%s,"
                "\"producer_plan_truncated\":%s",
                reusable_entry_action_name(snapshot.state),
                reusable_failure_name(snapshot.failure),
                snapshot.provider_id,
                snapshot.provider_abi_version,
                snapshot.provider_domain_abi_version,
                static_cast<unsigned long long>(
                    snapshot.compatibility_family_id),
                static_cast<unsigned long long>(snapshot.capability_mask),
                snapshot.dependency_count,
                dependency.data(),
                reusable_observability_name(snapshot.observability),
                snapshot.planner_pin_count,
                reusable_plan_snapshot_status_name(
                    snapshot.canonical_plan_snapshot_status),
                reusable_plan_snapshot_status_name(
                    snapshot.plan_snapshot_status),
                snapshot.display_sql_truncated ? "true" : "false",
                snapshot.canonical_plan_truncated ? "true" : "false",
                snapshot.producer_plan_truncated ? "true" : "false");
            if (snapshot.manifest) {
                appendStringInfo(
                    &dependencies,
                    ",\"manifest\":{\"slot\":%u,"
                    "\"slot_sequence\":%llu,\"generation\":%llu}",
                    snapshot.manifest.slot,
                    static_cast<unsigned long long>(
                        snapshot.manifest.slot_sequence),
                    static_cast<unsigned long long>(
                        snapshot.manifest.id.generation));
            } else {
                appendStringInfoString(
                    &dependencies, ",\"manifest\":null");
            }
            appendStringInfoChar(&dependencies, '}');
            std::string result{
                dependencies.data,
                static_cast<std::size_t>(dependencies.len)};
            pfree(dependencies.data);
            return result;
        });
}

/** Copy exact diagnostic text into one owning result row. */
void populate_exact_diagnostic_text(
        const EntrySnapshot& entry,
        bioseq::cache::pg::sql_rows::cache_entry_row& row) {
    DiagnosticTextWorkspaceV1 workspace{};
    if (!allocate_diagnostic_text_workspace(
            kExactDisplaySqlCapacityV1,
            kExactCanonicalPlanCapacityV1,
            kExactProducerPlanCapacityV1,
            &workspace)) {
        return;
    }
    ExactDiagnosticTextSnapshotV1 diagnostic_text{
        .display_sql = workspace.display_sql,
        .canonical_plan = workspace.canonical_plan,
        .producer_plan = workspace.producer_plan,
    };
    if (copy_exact_entry_text(entry, &diagnostic_text)) {
        if (diagnostic_text.display_sql_bytes != 0) {
            row.sql = std::string{
                diagnostic_text.display_sql.data(),
                diagnostic_text.display_sql_bytes};
        }
        if (diagnostic_text.producer_plan_bytes != 0) {
            row.producer_plan = std::string{
                diagnostic_text.producer_plan.data(),
                diagnostic_text.producer_plan_bytes};
        }
    }
    release_diagnostic_text_workspace(&workspace);
}

/** Copy reusable diagnostic text into one owning result row. */
void populate_reusable_diagnostic_text(
        const ReusableGenerationDiagnosticsRowV1& entry,
        bioseq::cache::pg::sql_rows::cache_entry_row& row) {
    DiagnosticTextWorkspaceV1 workspace{};
    if (!allocate_diagnostic_text_workspace(
            kReusableDisplaySqlCapacityV1,
            kReusableCanonicalPlanCapacityV1,
            kReusableProducerPlanCapacityV1,
            &workspace)) {
        return;
    }
    ReusableDiagnosticTextSnapshotV1 diagnostic_text{
        .display_sql = workspace.display_sql,
        .canonical_plan = workspace.canonical_plan,
        .producer_plan = workspace.producer_plan,
    };
    if (copy_reusable_entry_text(entry.reference, &diagnostic_text)) {
        if (diagnostic_text.metadata.display_sql_bytes != 0) {
            row.sql = std::string{
                diagnostic_text.display_sql.data(),
                diagnostic_text.metadata.display_sql_bytes};
        }
        if (diagnostic_text.metadata.producer_plan_bytes != 0) {
            row.producer_plan = std::string{
                diagnostic_text.producer_plan.data(),
                diagnostic_text.metadata.producer_plan_bytes};
        }
    }
    release_diagnostic_text_workspace(&workspace);
}

}  // namespace bioseq::cache::pg

namespace bioseq::cache::pg::sql_backend {

/** Return one owning snapshot of the public cache counters. */
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::stats_row>
stats(const bioseq::pg_utils::sql_call_context& call) {
    require_cache_diagnostics_reader(call.function_oid());
    sql_rows::stats_row row{};
    if (g_shared != nullptr) {
        row = {
            .produced = static_cast<std::int64_t>(
                read_counter(g_shared->stats.produced)),
            .completed_hits = static_cast<std::int64_t>(
                read_counter(g_shared->stats.completed_hits)),
            .inflight_waits = static_cast<std::int64_t>(
                read_counter(g_shared->stats.inflight_waits)),
            .reuse_hits = static_cast<std::int64_t>(
                read_counter(g_shared->stats.reuse_hits)),
            .fallbacks = static_cast<std::int64_t>(
                read_counter(g_shared->stats.fallbacks)),
            .invalidations = static_cast<std::int64_t>(
                read_counter(g_shared->stats.invalidations)),
            .evictions = static_cast<std::int64_t>(
                read_counter(g_shared->stats.evictions)),
            .logical_bytes = static_cast<std::int64_t>(
                read_counter(g_shared->stats.logical_bytes)),
            .stored_bytes = static_cast<std::int64_t>(
                read_counter(g_shared->stats.stored_bytes)),
        };
    }
    return bioseq::pg_utils::returns_table<sql_rows::stats_row>{
        std::vector<sql_rows::stats_row>{row}};
}

/** Return owning exact and reusable diagnostic rows. */
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::cache_entry_row>
entries(const bioseq::pg_utils::sql_call_context& call) {
    using namespace bioseq::cache;

    require_cache_diagnostics_reader(call.function_oid());
    static_cast<void>(bind_admin_reusable_registry(
        "bioseq_cache.entries()"));

    EntrySnapshot* const exact_snapshots =
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            return static_cast<EntrySnapshot*>(
                palloc0(sizeof(EntrySnapshot) * kRegistryCapacity));
        });
    const int exact_count = snapshot_entries(exact_snapshots);
    const TimestampTz now = GetCurrentTimestamp();

    std::vector<sql_rows::cache_entry_row> rows;
    rows.reserve(
        static_cast<std::size_t>(exact_count)
        + static_cast<std::size_t>(g_reusable_wait_slot_count));
    for (int index = 0; index < exact_count; ++index) {
        const EntrySnapshot& entry = exact_snapshots[index];
        const bool full_disclosure =
            bioseq::pg_utils::invoke_pg_as_cpp(
                [role_oid = entry.current_role_oid] {
                    return diagnostic_disclosure_for_role(role_oid)
                        == CacheDiagnosticDisclosure::FullText;
                });
        sql_rows::cache_entry_row row{
            .entry_key = exact_entry_key(entry.key_hash),
            .generation = static_cast<std::int64_t>(entry.generation),
            .kind = "ExactFinalResult",
            .state = entry_state_name(entry.state),
            .database_oid =
                static_cast<std::uint32_t>(entry.database_oid),
            .owner_role = full_disclosure
                ? std::optional<std::uint32_t>{
                    static_cast<std::uint32_t>(entry.current_role_oid)}
                : std::nullopt,
            .producer_pid = full_disclosure
                ? std::optional<std::int32_t>{
                    static_cast<std::int32_t>(entry.producer_pid)}
                : std::nullopt,
            .created_at = owning_timestamp(entry.created_at),
            .completed_at = entry.completed_at == 0
                ? std::nullopt
                : std::optional<bioseq::pg_utils::sql_timestamptz>{
                    owning_timestamp(entry.completed_at)},
            .age = {
                .time = static_cast<std::int64_t>(
                    now - entry.created_at),
            },
            .ttl = {
                .time = entry.completed_at == 0
                    ? static_cast<std::int64_t>(
                        guc::ttl_ms()) * 1'000
                    : static_cast<std::int64_t>(
                        entry.expires_at - entry.completed_at),
            },
            .rows = static_cast<std::int64_t>(entry.row_count),
            .logical_bytes =
                static_cast<std::int64_t>(entry.logical_bytes),
            .stored_bytes =
                static_cast<std::int64_t>(entry.stored_bytes),
            .waiters = static_cast<std::int32_t>(entry.waiter_count),
            .readers = static_cast<std::int32_t>(entry.reader_count),
            .hits = static_cast<std::int64_t>(entry.hits),
            .sql = std::nullopt,
            .producer_plan = std::nullopt,
            .dependencies = std::nullopt,
        };
        if (full_disclosure) {
            populate_exact_diagnostic_text(entry, row);
            row.dependencies.emplace(exact_dependency_json(entry));
        }
        rows.push_back(std::move(row));
    }
    pfree(exact_snapshots);

    if (g_reusable_wait_slot_count != 0) {
        const std::size_t capacity = g_reusable_wait_slot_count;
        auto* const reusable_snapshots =
            bioseq::pg_utils::invoke_pg_as_cpp([capacity] {
                return static_cast<
                    ReusableGenerationDiagnosticsRowV1*>(
                        palloc0(
                            sizeof(ReusableGenerationDiagnosticsRowV1)
                            * capacity));
            });
        const ReusableGenerationDiagnosticsResultV1 snapshot =
            snapshot_reusable_entries(
                std::span<ReusableGenerationDiagnosticsRowV1>{
                    reusable_snapshots, capacity});
        if (snapshot.error != ReusableRegistryErrorV1::None
            || snapshot.truncated) {
            pfree(reusable_snapshots);
            bioseq::pg_utils::raise_sql_error(
                ERRCODE_DATA_CORRUPTED,
                {
                    .message =
                        "bioseq_cache reusable diagnostics are inconsistent",
                });
        }
        const std::uint64_t now_monotonic_ns =
            reusable_monotonic_now_ns();
        for (std::size_t index = 0;
             index < snapshot.count;
             ++index) {
            const ReusableGenerationDiagnosticsRowV1& entry =
                reusable_snapshots[index];
            const bool full_disclosure =
                bioseq::pg_utils::invoke_pg_as_cpp(
                    [role_oid = static_cast<Oid>(
                         entry.current_role_oid)] {
                        return diagnostic_disclosure_for_role(role_oid)
                            == CacheDiagnosticDisclosure::FullText;
                    });
            const auto key =
                reusable_digest_hex(entry.reference.id.digest);
            const auto created_at = reusable_wall_timestamp(
                now,
                now_monotonic_ns,
                entry.started_monotonic_ns);
            const auto completed_at = reusable_wall_timestamp(
                now,
                now_monotonic_ns,
                entry.completed_monotonic_ns);
            sql_rows::cache_entry_row row{
                .entry_key = std::string{key.data()},
                .generation = static_cast<std::int64_t>(
                    entry.reference.id.generation),
                .kind =
                    entry.entry_kind
                            == CacheEntryKind::ExactFinalResult
                        ? "ExactFinalResult"
                        : "ReusableCoverage",
                .state = reusable_entry_state_name(entry.state),
                .database_oid = entry.database_oid == 0
                    ? std::nullopt
                    : std::optional<std::uint32_t>{
                        entry.database_oid},
                .owner_role =
                    entry.current_role_oid != 0 && full_disclosure
                    ? std::optional<std::uint32_t>{
                        entry.current_role_oid}
                    : std::nullopt,
                .producer_pid =
                    entry.producer_pid > 0 && full_disclosure
                    ? std::optional<std::int32_t>{
                        entry.producer_pid}
                    : std::nullopt,
                .created_at = created_at.has_value()
                    ? std::optional<bioseq::pg_utils::sql_timestamptz>{
                        owning_timestamp(*created_at)}
                    : std::nullopt,
                .completed_at = completed_at.has_value()
                    ? std::optional<bioseq::pg_utils::sql_timestamptz>{
                        owning_timestamp(*completed_at)}
                    : std::nullopt,
                .age = {
                    .time = reusable_age_us(
                        now_monotonic_ns,
                        entry.started_monotonic_ns),
                },
                .ttl = {
                    .time = reusable_ttl_us(
                        entry.producer_ttl_ms),
                },
                .rows = static_cast<std::int64_t>(
                    entry.row_count),
                .logical_bytes = static_cast<std::int64_t>(
                    entry.logical_bytes),
                .stored_bytes = static_cast<std::int64_t>(
                    entry.stored_bytes),
                .waiters = static_cast<std::int32_t>(
                    entry.waiter_count),
                .readers = static_cast<std::int32_t>(
                    entry.reader_pin_count),
                .hits = 0,
                .sql = std::nullopt,
                .producer_plan = std::nullopt,
                .dependencies = std::nullopt,
            };
            if (full_disclosure) {
                populate_reusable_diagnostic_text(entry, row);
                row.dependencies.emplace(
                    reusable_dependency_json(entry));
            }
            rows.push_back(std::move(row));
        }
        pfree(reusable_snapshots);
    }

    return bioseq::pg_utils::returns_table<
        sql_rows::cache_entry_row>{std::move(rows)};
}

/** Retire all cache generations after the SQL privilege check. */
[[nodiscard]] std::int64_t invalidate_all() {
    if (!superuser()) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            {.message = "must be superuser to invalidate bioseq_cache"});
    }
    static_cast<void>(bind_admin_reusable_registry(
        "bioseq_cache.invalidate_all()"));
    return static_cast<std::int64_t>(
        invalidate_registry()
        + invalidate_reusable_registry(std::nullopt));
}

/** Retire every cache generation which depends on one relation. */
[[nodiscard]] std::int64_t invalidate_relation(
        std::uint32_t relation_oid) {
    if (!superuser()) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            {.message = "must be superuser to invalidate bioseq_cache"});
    }
    static_cast<void>(bind_admin_reusable_registry(
        "bioseq_cache.invalidate_relation()"));
    return static_cast<std::int64_t>(
        invalidate_relation_registry(static_cast<Oid>(relation_oid))
        + invalidate_reusable_relation_registry(
            static_cast<Oid>(relation_oid)));
}

/** Reset all public and test exact-result counters. */
void reset_stats() {
    if (!superuser()) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            {
                .message =
                    "must be superuser to reset bioseq_cache statistics",
            });
    }
    if (g_shared == nullptr) {
        return;
    }
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    pg_atomic_write_u64(&g_shared->stats.produced, 0);
    pg_atomic_write_u64(&g_shared->stats.completed_hits, 0);
    pg_atomic_write_u64(&g_shared->stats.inflight_waits, 0);
    pg_atomic_write_u64(&g_shared->stats.reuse_hits, 0);
    pg_atomic_write_u64(&g_shared->stats.fallbacks, 0);
    pg_atomic_write_u64(&g_shared->stats.invalidations, 0);
    pg_atomic_write_u64(&g_shared->stats.evictions, 0);
    pg_atomic_write_u64(&g_shared->stats.logical_bytes, 0);
    pg_atomic_write_u64(&g_shared->stats.stored_bytes, 0);
#if defined(BIOSEQ_CACHE_TESTING)
    pg_atomic_write_u64(
        &g_shared->test_control.exact_child_executions, 0);
#endif
    LWLockRelease(g_registry_lock);
}

}  // namespace bioseq::cache::pg::sql_backend

#if defined(BIOSEQ_CACHE_TESTING)
namespace bioseq::cache::pg {

/** Require the private-cluster superuser test surface and shared state. */
inline void require_cache_test_superuser_typed() {
    if (!superuser()) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INSUFFICIENT_PRIVILEGE,
            {
                .message =
                    "must be superuser to use bioseq_cache test controls",
            });
    }
    if (g_shared == nullptr) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            {
                .message =
                    "bioseq_cache test shared memory is unavailable",
            });
    }
}

/** Copy one bounded owning byte sequence into an injected registry field. */
inline void copy_injected_bytes_typed(
        unsigned char* destination,
        std::uint16_t* destination_size,
        std::span<const std::byte> source,
        std::string_view field_name) {
    if (source.size() > kInjectedDigestBytes) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            {
                .message = std::string{field_name}
                    + " exceeds "
                    + std::to_string(kInjectedDigestBytes)
                    + " bytes",
            });
    }
    *destination_size =
        static_cast<std::uint16_t>(source.size());
    if (!source.empty()) {
        std::memcpy(
            destination, source.data(), source.size());
    }
}

/** Copy one injected byte field into its owning SQL representation. */
[[nodiscard]] inline bioseq::pg_utils::sql_bytea injected_bytes_typed(
        const unsigned char* source,
        std::size_t size) {
    std::vector<std::byte> bytes(size);
    if (size != 0) {
        std::memcpy(bytes.data(), source, size);
    }
    return bioseq::pg_utils::sql_bytea{std::move(bytes)};
}

}  // namespace bioseq::cache::pg

namespace bioseq::cache::pg::test_sql_backend {

/** Prove planner infrastructure health without prepared-xact policy gates. */
[[nodiscard]] bool cache_infrastructure_ready() {
    require_cache_test_superuser_typed();
    const bool header_ready =
        guc::preload_requested()
        && g_shared != nullptr
        && g_shared->magic == kSharedMagic
        && g_shared->shared_abi_version == kSharedAbiVersion
        && pg_atomic_read_u32(&g_shared->ready) != 0
        && pg_atomic_read_u32(&g_shared->bootstrap_failure) == 0;
    return header_ready
        && shared_registry::shared_registry_bootstrap_state()
            == BootstrapState::Ready
        && bind_shared_reusable_registry()
        && storage::runtime_snapshot().correctness_readiness
            >= CacheCorrectnessReadiness::ReusableCoverageReady;
}

/** Select one exact-plan verifier fault. */
void set_exact_plan_fault(const std::string& name) {
    require_cache_test_superuser_typed();
    if (name == "none") {
        g_test_exact_plan_fault = TestExactPlanFault::None;
    } else if (name == "unknown_custom_scan") {
        g_test_exact_plan_fault = TestExactPlanFault::UnknownCustomScan;
    } else if (name == "unknown_table_am") {
        g_test_exact_plan_fault =
            TestExactPlanFault::UnknownTableAccessMethod;
    } else if (name == "unknown_index_am") {
        g_test_exact_plan_fault =
            TestExactPlanFault::UnknownIndexAccessMethod;
    } else if (name == "dependency_finalization_declined") {
        g_test_exact_plan_fault =
            TestExactPlanFault::DependencyFinalizationDeclined;
    } else {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "unknown exact-plan verifier fault: " + name,
            });
    }
}

/** Select one exact-recipe verifier fault and invalidate saved plans. */
void set_exact_recipe_fault(const std::string& name) {
    require_cache_test_superuser_typed();
    if (name == "none") {
        g_test_exact_recipe_fault = TestExactRecipeFault::None;
    } else if (name == "expected_digest_mismatch") {
        g_test_exact_recipe_fault =
            TestExactRecipeFault::ExpectedDigestMismatch;
    } else {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "unknown exact-recipe fault: " + name});
    }
    ResetPlanCache();
}

/** Select one reusable coverage-descriptor fault. */
void set_coverage_descriptor_fault(const std::string& name) {
    require_cache_test_superuser_typed();
    if (name == "none") {
        g_test_coverage_descriptor_fault =
            TestCoverageDescriptorFault::None;
    } else if (name == "raw_producer_var") {
        g_test_coverage_descriptor_fault =
            TestCoverageDescriptorFault::RawProducerVar;
    } else if (name == "foreign_mixed_var") {
        g_test_coverage_descriptor_fault =
            TestCoverageDescriptorFault::ForeignMixedVar;
    } else if (name == "wrong_node_type") {
        g_test_coverage_descriptor_fault =
            TestCoverageDescriptorFault::WrongNodeType;
    } else if (name == "malformed_syntax") {
        g_test_coverage_descriptor_fault =
            TestCoverageDescriptorFault::MalformedSyntax;
    } else if (name == "binding_signature_mismatch") {
        g_test_coverage_descriptor_fault =
            TestCoverageDescriptorFault::BindingSignatureMismatch;
    } else if (name == "plan_private_short") {
        g_test_coverage_descriptor_fault =
            TestCoverageDescriptorFault::PlanPrivateShort;
    } else if (name == "plan_private_bad_version") {
        g_test_coverage_descriptor_fault =
            TestCoverageDescriptorFault::PlanPrivateBadVersion;
    } else {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "unknown coverage-descriptor fault: " + name,
            });
    }
}

/** Arm one global deterministic executor pause point. */
void arm_pause(const std::string& name) {
    require_cache_test_superuser_typed();
    const TestPausePoint point = parse_pause_point(name.c_str());
    const std::uint32_t bit = pause_bit(point);
    CacheTestControl& control = g_shared->test_control;
    bioseq::pg_utils::invoke_pg_as_cpp([point] {
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_lock_int8,
            Int64GetDatum(pause_lock_key(point))));
    });
    pg_atomic_fetch_and_u32(&control.paused_mask, ~bit);
    pg_atomic_fetch_and_u32(&control.released_mask, ~bit);
    pg_atomic_fetch_or_u32(&control.armed_mask, bit);
    ConditionVariableBroadcast(&control.changed);
}

/** Arm one pause point for a selected backend PID. */
void arm_backend_pause(
        const std::string& name,
        std::int32_t target_pid) {
    require_cache_test_superuser_typed();
    const TestPausePoint point = parse_pause_point(name.c_str());
    if (target_pid <= 0
        || point
            != TestPausePoint::AfterDependencyTransitionPublish) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "invalid backend-targeted bioseq_cache pause"});
    }
    bioseq::pg_utils::invoke_pg_as_cpp([point, target_pid] {
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_lock_int8,
            Int64GetDatum(pause_lock_key(point, target_pid))));
    });
    CacheBackendPauseControlV1* selected = nullptr;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    if (find_backend_pause_control(point, target_pid) == nullptr) {
        for (CacheBackendPauseControlV1& pause
             : g_shared->test_control.backend_pauses) {
            if (pg_atomic_read_u32(&pause.state)
                != static_cast<std::uint32_t>(
                    BackendPauseStateV1::Free)) {
                continue;
            }
            pause.point = static_cast<std::uint32_t>(point);
            pause.target_pid = target_pid;
            pause.reserved = 0;
            pg_atomic_write_u32(
                &pause.state,
                static_cast<std::uint32_t>(
                    BackendPauseStateV1::Armed));
            selected = &pause;
            break;
        }
    }
    LWLockRelease(g_registry_lock);
    if (selected == nullptr) {
        bioseq::pg_utils::invoke_pg_as_cpp([point, target_pid] {
            static_cast<void>(DirectFunctionCall1(
                pg_advisory_unlock_int8,
                Int64GetDatum(pause_lock_key(point, target_pid))));
        });
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_PROGRAM_LIMIT_EXCEEDED,
            {
                .message =
                    "backend-targeted bioseq_cache pause slots are full",
            });
    }
    ConditionVariableBroadcast(&g_shared->test_control.changed);
}

/** Wait at most thirty seconds for one global pause point. */
void wait_until_paused(const std::string& name) {
    require_cache_test_superuser_typed();
    const std::uint32_t bit =
        pause_bit(parse_pause_point(name.c_str()));
    CacheTestControl* const control = &g_shared->test_control;
    const TimestampTz started = GetCurrentTimestamp();
    bioseq::pg_utils::invoke_pg_as_cpp([bit, control, started] {
        ConditionVariablePrepareToSleep(&control->changed);
        PG_TRY();
        {
            while ((pg_atomic_read_u32(&control->paused_mask) & bit)
                   == 0) {
                CHECK_FOR_INTERRUPTS();
                if (TimestampDifferenceExceeds(
                        started, GetCurrentTimestamp(), 30'000)) {
                    ereport(ERROR,
                        (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                         errmsg(
                             "timed out waiting for bioseq_cache pause")));
                }
                static_cast<void>(ConditionVariableTimedSleep(
                    &control->changed,
                    100L,
                    PG_WAIT_EXTENSION));
            }
            ConditionVariableCancelSleep();
        }
        PG_CATCH();
        {
            ConditionVariableCancelSleep();
            PG_RE_THROW();
        }
        PG_END_TRY();
    });
}

/** Wait at most thirty seconds for one backend-specific pause point. */
void wait_until_backend_paused(
        const std::string& name,
        std::int32_t target_pid) {
    require_cache_test_superuser_typed();
    const TestPausePoint point = parse_pause_point(name.c_str());
    CacheTestControl* const control = &g_shared->test_control;
    const TimestampTz started = GetCurrentTimestamp();
    bioseq::pg_utils::invoke_pg_as_cpp(
        [control, point, started, target_pid] {
            ConditionVariablePrepareToSleep(&control->changed);
            PG_TRY();
            {
                for (;;) {
                    CHECK_FOR_INTERRUPTS();
                    CacheBackendPauseControlV1* const pause =
                        find_backend_pause_control(
                            point, target_pid);
                    if (pause != nullptr
                        && pg_atomic_read_u32(&pause->state)
                            == static_cast<std::uint32_t>(
                                BackendPauseStateV1::Paused)) {
                        break;
                    }
                    if (TimestampDifferenceExceeds(
                            started,
                            GetCurrentTimestamp(),
                            30'000)) {
                        ereport(ERROR,
                            (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                             errmsg(
                                 "timed out waiting for backend "
                                 "bioseq_cache pause")));
                    }
                    static_cast<void>(
                        ConditionVariableTimedSleep(
                            &control->changed,
                            100L,
                            PG_WAIT_EXTENSION));
                }
                ConditionVariableCancelSleep();
            }
            PG_CATCH();
            {
                ConditionVariableCancelSleep();
                PG_RE_THROW();
            }
            PG_END_TRY();
        });
}

/** Release one global pause point. */
void release_pause(const std::string& name) {
    require_cache_test_superuser_typed();
    const TestPausePoint point = parse_pause_point(name.c_str());
    const std::uint32_t bit = pause_bit(point);
    pg_atomic_fetch_or_u32(
        &g_shared->test_control.released_mask, bit);
    bioseq::pg_utils::invoke_pg_as_cpp([point] {
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_unlock_int8,
            Int64GetDatum(pause_lock_key(point))));
    });
    ConditionVariableBroadcast(&g_shared->test_control.changed);
}

/** Release one backend-specific pause point. */
void release_backend_pause(
        const std::string& name,
        std::int32_t target_pid) {
    require_cache_test_superuser_typed();
    const TestPausePoint point = parse_pause_point(name.c_str());
    CacheBackendPauseControlV1* const pause =
        find_backend_pause_control(point, target_pid);
    if (pause == nullptr) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "backend bioseq_cache pause is not armed"});
    }
    pg_atomic_write_u32(
        &pause->state,
        static_cast<std::uint32_t>(
            BackendPauseStateV1::Released));
    bioseq::pg_utils::invoke_pg_as_cpp([point, target_pid] {
        static_cast<void>(DirectFunctionCall1(
            pg_advisory_unlock_int8,
            Int64GetDatum(pause_lock_key(point, target_pid))));
    });
    ConditionVariableBroadcast(&g_shared->test_control.changed);
}

/** Fail the next cache result write through the selected path. */
void fail_next_write(const std::string& failure) {
    require_cache_test_superuser_typed();
    if (failure != "encoder_error" && failure != "enospc") {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "unknown bioseq_cache write failure: " + failure,
            });
    }
    pg_atomic_write_u32(
        &g_shared->test_control.fail_next_write, 1);
}

/** Force or release the test-only file-tier policy. */
void set_force_file_tier(bool enabled) {
    require_cache_test_superuser_typed();
    if (!storage::set_test_force_file_tier(enabled)) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE,
            {.message = "bioseq_cache storage is unavailable"});
    }
}

/** Return one storage-ledger snapshot. */
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::storage_tier_stats_row>
storage_tier_stats() {
    require_cache_test_superuser_typed();
    const storage::StorageTierTestSnapshot snapshot =
        storage::test_storage_tier_snapshot();
    sql_rows::storage_tier_stats_row row{
        .force_file_tier = snapshot.force_file_tier,
        .dsa_blocks = static_cast<std::int64_t>(snapshot.dsa_blocks),
        .file_blocks =
            static_cast<std::int64_t>(snapshot.file_blocks),
        .reserved_hot_bytes =
            static_cast<std::int64_t>(snapshot.reserved_hot_bytes),
        .reserved_file_bytes =
            static_cast<std::int64_t>(snapshot.reserved_file_bytes),
        .coherent = snapshot.coherent,
        .occupied_blocks =
            static_cast<std::int64_t>(snapshot.occupied_blocks),
        .reserved_blocks =
            static_cast<std::int64_t>(snapshot.reserved_blocks),
        .stored_blocks =
            static_cast<std::int64_t>(snapshot.stored_blocks),
        .referenced_blocks =
            static_cast<std::int64_t>(snapshot.referenced_blocks),
        .retiring_blocks =
            static_cast<std::int64_t>(snapshot.retiring_blocks),
        .release_committed_blocks = static_cast<std::int64_t>(
            snapshot.release_committed_blocks),
        .reader_pins =
            static_cast<std::int64_t>(snapshot.reader_pins),
    };
    return bioseq::pg_utils::returns_table<
        sql_rows::storage_tier_stats_row>{
            std::vector<sql_rows::storage_tier_stats_row>{row}};
}

/** Return exact-result states held in the reusable registry. */
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::entry_state_row>
entry_states() {
    using namespace bioseq::cache;
    require_cache_test_superuser_typed();
    std::vector<sql_rows::entry_state_row> rows;
    if (!bind_admin_reusable_registry(
            "bioseq_cache_test.entry_states()")
        || g_reusable_wait_slot_count == 0) {
        return bioseq::pg_utils::returns_table<
            sql_rows::entry_state_row>{std::move(rows)};
    }
    const std::size_t capacity = g_reusable_wait_slot_count;
    auto* const snapshots =
        bioseq::pg_utils::invoke_pg_as_cpp([capacity] {
            return static_cast<ReusableGenerationDiagnosticsRowV1*>(
                palloc0(
                    sizeof(ReusableGenerationDiagnosticsRowV1)
                    * capacity));
        });
    const ReusableGenerationDiagnosticsResultV1 copied =
        snapshot_reusable_entries(
            std::span<ReusableGenerationDiagnosticsRowV1>{
                snapshots, capacity});
    if (copied.error == ReusableRegistryErrorV1::None
        && !copied.truncated) {
        rows.reserve(copied.count);
        for (std::size_t index = 0; index < copied.count; ++index) {
            const ReusableGenerationDiagnosticsRowV1& entry =
                snapshots[index];
            if (entry.entry_kind
                != CacheEntryKind::ExactFinalResult) {
                continue;
            }
            const auto digest =
                reusable_digest_hex(entry.reference.id.digest);
            rows.push_back({
                .entry_key = std::string{digest.data(), 16},
                .generation = static_cast<std::int64_t>(
                    entry.reference.id.generation),
                .state = reusable_entry_state_name(entry.state),
                .waiter_count = static_cast<std::int32_t>(
                    entry.waiter_count),
                .reader_count = static_cast<std::int32_t>(
                    entry.reader_pin_count),
                .producer_pid = entry.producer_pid,
                .claim_sequence = static_cast<std::int64_t>(
                    entry.reference.id.claim_sequence),
            });
        }
    }
    pfree(snapshots);
    return bioseq::pg_utils::returns_table<
        sql_rows::entry_state_row>{std::move(rows)};
}

/** Return the exact child-execution counter. */
[[nodiscard]] std::int64_t exact_child_executions() {
    require_cache_test_superuser_typed();
    return static_cast<std::int64_t>(
        pg_atomic_read_u64(
            &g_shared->test_control.exact_child_executions));
}

/** Return transaction-transition state. */
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::transition_state_row>
transition_state() {
    require_cache_test_superuser_typed();
    const Snapshot snapshot = GetActiveSnapshot();
    LWLockAcquire(ProcArrayLock, LW_SHARED);
    const bool prepared =
        database_has_prepared_xacts_locked(MyDatabaseId);
    const std::uint64_t global_completion =
        TransamVariables->xactCompletionCount;
    const std::uint64_t snapshot_completion =
        snapshot == nullptr
        ? 0
        : snapshot->snapXactCompletionCount;
    LWLockRelease(ProcArrayLock);
    return bioseq::pg_utils::returns_table<
        sql_rows::transition_state_row>{
            std::vector<sql_rows::transition_state_row>{{
                .database_transitions =
                    static_cast<std::int64_t>(pg_atomic_read_u32(
                        &g_shared->database_transition_count)),
                .coarse_transitions =
                    static_cast<std::int64_t>(pg_atomic_read_u32(
                        &g_shared->coarse_transition_count)),
                .prepared_xacts = prepared,
                .snapshot_completion =
                    static_cast<std::int64_t>(snapshot_completion),
                .global_completion =
                    static_cast<std::int64_t>(global_completion),
            }}};
}

/** Return one relation's precise transition count and committed epoch. */
[[nodiscard]]
bioseq::pg_utils::returns_table<
    sql_rows::relation_transition_state_row>
relation_transition_state(std::uint32_t relation_oid) {
    require_cache_test_superuser_typed();
    if (!OidIsValid(static_cast<Oid>(relation_oid))) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {.message = "relation transition state requires a valid OID"});
    }
    std::uint64_t committed_epoch = 1;
    std::uint32_t transition_count = 0;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    const RelationEpochSlot* const slot =
        find_relation_epoch_locked(static_cast<Oid>(relation_oid));
    if (slot != nullptr) {
        committed_epoch = slot->committed_epoch;
        transition_count = slot->transition_count;
    }
    LWLockRelease(g_registry_lock);
    return bioseq::pg_utils::returns_table<
        sql_rows::relation_transition_state_row>{
            std::vector<sql_rows::relation_transition_state_row>{{
                .relation_transitions =
                    static_cast<std::int64_t>(transition_count),
                .relation_epoch =
                    static_cast<std::int64_t>(committed_epoch),
            }}};
}

/** Return aggregate planner reference counts. */
[[nodiscard]]
bioseq::pg_utils::returns_table<
    sql_rows::planner_reference_counts_row>
planner_reference_counts() {
    using namespace bioseq::cache;
    require_cache_test_superuser_typed();
    std::uint64_t readers = 0;
    std::uint64_t waiters = 0;
    std::uint64_t planner_pins = 0;
    if (bind_admin_reusable_registry(
            "bioseq_cache_test.planner_reference_counts()")
        && g_reusable_wait_slot_count != 0) {
        const std::size_t capacity = g_reusable_wait_slot_count;
        auto* const snapshots =
            bioseq::pg_utils::invoke_pg_as_cpp([capacity] {
                return static_cast<
                    ReusableGenerationDiagnosticsRowV1*>(
                        palloc0(
                            sizeof(
                                ReusableGenerationDiagnosticsRowV1)
                            * capacity));
            });
        const ReusableGenerationDiagnosticsResultV1 copied =
            snapshot_reusable_entries(
                std::span<ReusableGenerationDiagnosticsRowV1>{
                    snapshots, capacity});
        if (copied.error == ReusableRegistryErrorV1::None
            && !copied.truncated) {
            for (std::size_t index = 0;
                 index < copied.count;
                 ++index) {
                readers += snapshots[index].reader_pin_count;
                waiters += snapshots[index].waiter_count;
                planner_pins += snapshots[index].planner_pin_count;
            }
        }
        pfree(snapshots);
    }
    return bioseq::pg_utils::returns_table<
        sql_rows::planner_reference_counts_row>{
            std::vector<sql_rows::planner_reference_counts_row>{{
                .reader_pins =
                    static_cast<std::int64_t>(readers),
                .waiter_references =
                    static_cast<std::int64_t>(waiters),
                .admission_reservations =
                    static_cast<std::int64_t>(planner_pins),
            }}};
}

/** Return the currently implemented provider callback counters. */
[[nodiscard]]
bioseq::pg_utils::returns_table<
    sql_rows::provider_callback_stats_row>
provider_callback_stats() {
    require_cache_test_superuser_typed();
    return bioseq::pg_utils::returns_table<
        sql_rows::provider_callback_stats_row>{
            std::vector<sql_rows::provider_callback_stats_row>{}};
}

/** Return cache-plan construction counters. */
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::plan_build_stats_row>
plan_build_stats() {
    require_cache_test_superuser_typed();
    const std::uint64_t generation =
        read_counter(g_shared->stats.planner_build_generation);
    const bool same = guc::mode() == QueryCacheMode::Same;
    const bool reuse = guc::mode() == QueryCacheMode::Reuse;
    return bioseq::pg_utils::returns_table<
        sql_rows::plan_build_stats_row>{
            std::vector<sql_rows::plan_build_stats_row>{{
                .planning_generation =
                    static_cast<std::int64_t>(generation),
                .disabled_plans = 0,
                .same_exact_wrappers = static_cast<std::int64_t>(
                    same ? generation : 0),
                .reuse_exact_wrappers = static_cast<std::int64_t>(
                    reuse ? generation : 0),
                .reuse_coverage_nodes = 0,
            }}};
}

/** Preserve the historical mapping-fault control as a typed no-op. */
void set_mapping_fault(const std::string& name) {
    require_cache_test_superuser_typed();
    static_cast<void>(name);
}

/** Clear the historical mapping-fault control. */
void clear_mapping_fault() {
    require_cache_test_superuser_typed();
}

/** Preserve the historical runtime-digest control as a typed no-op. */
void set_runtime_digest_fault(const std::string& name) {
    require_cache_test_superuser_typed();
    static_cast<void>(name);
}

/** Clear the historical runtime-digest control. */
void clear_runtime_digest_fault() {
    require_cache_test_superuser_typed();
}

/** Force one exact coverage-domain digest. */
void set_forced_coverage_domain_digest(
        const bioseq::pg_utils::sql_bytea& value) {
    require_cache_test_superuser_typed();
    if (value.bytes().size()
        != g_forced_coverage_domain_digest.bytes.size()) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "forced coverage domain digest must contain 32 bytes",
            });
    }
    std::memcpy(
        g_forced_coverage_domain_digest.bytes.data(),
        value.bytes().data(),
        value.bytes().size());
    g_forced_coverage_domain_digest_active = true;
}

/** Clear the forced exact coverage-domain digest. */
void clear_forced_coverage_domain_digest() {
    require_cache_test_superuser_typed();
    g_forced_coverage_domain_digest = {};
    g_forced_coverage_domain_digest_active = false;
}

/** Inject one complete reusable coverage generation. */
[[nodiscard]] std::int64_t inject_complete_coverage(
        const bioseq::pg_utils::sql_bytea& domain_key,
        const bioseq::pg_utils::sql_bytea& predicate_key,
        const bioseq::pg_utils::sql_bytea& provider_security_digest,
        const std::optional<bioseq::pg_utils::sql_bytea>&
            forced_domain_digest) {
    require_cache_test_superuser_typed();
    int selected = -1;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t index = 0;
         index < kInjectedCoverageCapacity;
         ++index) {
        if (!g_shared->injected_coverage[index].used
            || (g_shared->injected_coverage[index].evictable
                && g_shared->injected_coverage[index].database_oid
                    == MyDatabaseId)) {
            selected = static_cast<int>(index);
            break;
        }
    }
    if (selected < 0) {
        LWLockRelease(g_registry_lock);
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INSUFFICIENT_RESOURCES,
            {
                .message =
                    "bioseq_cache injected coverage registry is full",
            });
    }
    InjectedCoverage& entry =
        g_shared->injected_coverage[selected];
    std::memset(&entry, 0, sizeof(entry));
    entry.used = true;
    entry.database_oid = MyDatabaseId;
    int security_context = 0;
    GetUserIdAndSecContext(
        &entry.current_role_oid, &security_context);
    entry.security_context_flags =
        static_cast<std::uint32_t>(security_context);
    entry.session_user_oid = GetSessionUserId();
    entry.generation = pg_atomic_fetch_add_u64(
        &g_shared->next_injected_generation, 1);
    copy_injected_bytes_typed(
        entry.domain,
        &entry.domain_size,
        domain_key.bytes(),
        "domain key");
    copy_injected_bytes_typed(
        entry.predicate,
        &entry.predicate_size,
        predicate_key.bytes(),
        "predicate key");
    copy_injected_bytes_typed(
        entry.provider_digest,
        &entry.provider_digest_size,
        provider_security_digest.bytes(),
        "provider security digest");
    if (forced_domain_digest.has_value()) {
        copy_injected_bytes_typed(
            entry.forced_digest,
            &entry.forced_digest_size,
            forced_domain_digest->bytes(),
            "forced domain digest");
    }
    const std::uint64_t generation = entry.generation;
    LWLockRelease(g_registry_lock);
    ConditionVariableBroadcast(&g_shared->test_control.changed);
    return static_cast<std::int64_t>(generation);
}

/** Find injected complete coverage for one domain. */
[[nodiscard]]
bioseq::pg_utils::returns_table<sql_rows::injected_coverage_row>
lookup_injected_coverage(
        const bioseq::pg_utils::sql_bytea& domain_key) {
    require_cache_test_superuser_typed();
    std::vector<sql_rows::injected_coverage_row> rows;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const InjectedCoverage& entry
         : g_shared->injected_coverage) {
        if (!entry.used
            || entry.database_oid != MyDatabaseId
            || entry.domain_size != domain_key.bytes().size()
            || std::memcmp(
                   entry.domain,
                   domain_key.bytes().data(),
                   domain_key.bytes().size()) != 0) {
            continue;
        }
        rows.push_back({
            .generation =
                static_cast<std::int64_t>(entry.generation),
            .predicate_key = injected_bytes_typed(
                entry.predicate, entry.predicate_size),
            .state = "COMPLETE",
        });
    }
    LWLockRelease(g_registry_lock);
    return bioseq::pg_utils::returns_table<
        sql_rows::injected_coverage_row>{std::move(rows)};
}

/** Return injected security/domain coverage buckets. */
[[nodiscard]]
bioseq::pg_utils::returns_table<
    sql_rows::coverage_bucket_state_row>
coverage_bucket_states() {
    require_cache_test_superuser_typed();
    std::vector<sql_rows::coverage_bucket_state_row> rows;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const InjectedCoverage& entry
         : g_shared->injected_coverage) {
        if (!entry.used || entry.database_oid != MyDatabaseId) {
            continue;
        }
        const unsigned char* const digest =
            entry.forced_digest_size == 0
            ? entry.domain
            : entry.forced_digest;
        const std::size_t digest_size =
            entry.forced_digest_size == 0
            ? entry.domain_size
            : entry.forced_digest_size;
        rows.push_back({
            .database_oid = entry.database_oid,
            .current_role_oid = entry.current_role_oid,
            .session_user_oid = entry.session_user_oid,
            .security_context_flags =
                static_cast<std::int32_t>(
                    entry.security_context_flags),
            .row_security_mode = 0,
            .provider_security_digest = injected_bytes_typed(
                entry.provider_digest,
                entry.provider_digest_size),
            .coverage_domain_digest = injected_bytes_typed(
                digest, digest_size),
            .generation_count = 1,
            .generation_ids =
                bioseq::pg_utils::sql_int64_array{
                    std::vector<std::int64_t>{
                        static_cast<std::int64_t>(
                            entry.generation)}},
        });
    }
    LWLockRelease(g_registry_lock);
    return bioseq::pg_utils::returns_table<
        sql_rows::coverage_bucket_state_row>{std::move(rows)};
}

/** Mutate one injected coverage descriptor. */
void mutate_coverage_descriptor(
        std::int64_t generation,
        const std::string& mutation) {
    require_cache_test_superuser_typed();
    static_cast<void>(mutation);
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used
            && entry.generation
                == static_cast<std::uint64_t>(generation)) {
            entry.predicate_size = 0;
            break;
        }
    }
    LWLockRelease(g_registry_lock);
}

/** Mark one injected generation as evictable. */
void mark_generation_evictable(std::int64_t generation) {
    require_cache_test_superuser_typed();
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used
            && entry.generation
                == static_cast<std::uint64_t>(generation)) {
            entry.evictable = true;
        }
    }
    LWLockRelease(g_registry_lock);
}

/** Run one synchronous test janitor sweep. */
void request_janitor_sweep() {
    require_cache_test_superuser_typed();
    static_cast<void>(bind_admin_reusable_registry(
        "bioseq_cache_test.request_janitor_sweep()"));
    storage::request_janitor_sweep(true);
    static_cast<void>(sweep_registry(true));
    drain_reusable_registry_for_testing_v1();
    static_cast<void>(storage::janitor_sweep(0, true));
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used && entry.evictable) {
            entry.used = false;
        }
    }
    LWLockRelease(g_registry_lock);
    ConditionVariableBroadcast(&g_shared->test_control.changed);
}

/** Wait for one generation to disappear before the supplied deadline. */
[[nodiscard]] bool wait_for_generation_absent(
        std::int64_t generation,
        const bioseq::pg_utils::sql_interval& timeout) {
    require_cache_test_superuser_typed();
    const TimestampTz deadline =
        GetCurrentTimestamp()
        + timeout.time
        + static_cast<std::int64_t>(timeout.day) * USECS_PER_DAY;
    for (;;) {
        bool absent = true;
        LWLockAcquire(g_registry_lock, LW_SHARED);
        for (const InjectedCoverage& entry
             : g_shared->injected_coverage) {
            if (entry.used
                && entry.generation
                    == static_cast<std::uint64_t>(generation)) {
                absent = false;
                break;
            }
        }
        LWLockRelease(g_registry_lock);
        if (absent || GetCurrentTimestamp() >= deadline) {
            return absent;
        }
        bioseq::pg_utils::invoke_pg_as_cpp([] {
            CHECK_FOR_INTERRUPTS();
        });
        static_cast<void>(ConditionVariableTimedSleep(
            &g_shared->test_control.changed,
            10L,
            PG_WAIT_EXTENSION));
        ConditionVariableCancelSleep();
    }
}

/** Insert one heap tuple through SPI without cache write observation. */
void direct_heap_insert(
        std::uint32_t relation_oid,
        std::int32_t identifier,
        const std::string& value) {
    require_cache_test_superuser_typed();
    struct PreparedInsertV1 {
        char* namespace_name = nullptr;
        char* qualified_name = nullptr;
        char* command = nullptr;
        Oid argument_types[2]{};
        Datum arguments[2]{};
        char nulls[2]{};
    };
    PreparedInsertV1 prepared =
        bioseq::pg_utils::invoke_pg_as_cpp(
            [identifier,
             relation_oid,
             value_pointer = &value] {
            char* const relation_name =
                get_rel_name(static_cast<Oid>(relation_oid));
            char* const namespace_name = get_namespace_name(
                get_rel_namespace(static_cast<Oid>(relation_oid)));
            if (relation_name == nullptr
                || namespace_name == nullptr) {
                ereport(ERROR,
                    (errcode(ERRCODE_UNDEFINED_TABLE),
                     errmsg(
                         "relation %u does not exist",
                         relation_oid)));
            }
            char* const qualified = quote_qualified_identifier(
                namespace_name, relation_name);
            char* const command = psprintf(
                "INSERT INTO %s VALUES ($1, $2)", qualified);
            return PreparedInsertV1{
                .namespace_name = namespace_name,
                .qualified_name = qualified,
                .command = command,
                .argument_types = {INT4OID, TEXTOID},
                .arguments = {
                    Int32GetDatum(identifier),
                    CStringGetTextDatum(value_pointer->c_str()),
                },
                .nulls = {' ', ' '},
            };
        });
    int status = 0;
    {
        bioseq::pg_support::ScopedSpiConnection connection;
        status = connection.execute_with_args(
                prepared.command,
                2,
                prepared.argument_types,
                prepared.arguments,
                prepared.nulls,
                false,
                0);
        static_cast<void>(connection.finish());
    }
    bioseq::pg_utils::invoke_pg_as_cpp(
        [command = prepared.command,
         namespace_name = prepared.namespace_name,
         qualified_name = prepared.qualified_name] {
            pfree(command);
            pfree(qualified_name);
            pfree(namespace_name);
        });
    if (status != SPI_OK_INSERT) {
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INTERNAL_ERROR,
            {
                .message =
                    "direct test insert failed with SPI status "
                    + std::to_string(status),
            });
    }
    mark_current_transaction_dirty();
}

}  // namespace bioseq::cache::pg::test_sql_backend
#endif

namespace bioseq::cache::pg::detail {

void initialize_extension() {
    using namespace bioseq::cache::pg;
    if (!provider_registry::preflight_provider_registry_v2()) {
        ereport(ERROR,
            (errmsg("bioseq_cache provider rendezvous is owned by another library")));
    }
    guc::register_variables();
    if (!maintenance::register_reusable_janitor_callback_v1(
            cache_registry_janitor_sweep_v1)) {
        ereport(ERROR,
            (errmsg("bioseq_cache could not register reusable maintenance")));
    }
    if (!planner::reuse::install_provider_bridges(
            planner::reuse::BridgeCallbacksV1{
                .relation_write = observe_provider_relation_write,
                .relation_paths = observe_provider_rel_paths,
                .finalized_plan = observe_finalized_planned_statement,
            })) {
        ereport(ERROR,
            (errmsg("bioseq_cache provider bridges are incomplete")));
    }
    if (process_shared_preload_libraries_in_progress) {
        guc::mark_preload_requested();
        if (!maintenance::register_static_janitor_worker()) {
            ereport(WARNING,
                (errmsg("bioseq_cache could not register its janitor worker")));
        }
        executor::lifecycle::install_shared_memory_hooks(
            cache_shmem_request, cache_shmem_startup);
    }
    object_dependency_hooks::install_object_dependency_hooks();
    executor::lifecycle::install_backend_hooks(
        executor::lifecycle::BackendHookCallbacksV1{
            .planner = cache_planner,
            .create_upper_paths = cache_create_upper_paths,
            .executor_start = cache_executor_start,
            .executor_finish = cache_executor_finish,
            .executor_end = cache_executor_end,
            .process_utility = cache_process_utility,
        });
    /*
     * PostgreSQL invokes transaction callbacks in reverse registration order.
     * Register the dependency runtime first so the legacy cache callback
     * publishes its event-specific transition before the dependency runtime
     * publishes and reaches the test-only after-transition pause.
     */
    dependency_runtime::register_dependency_runtime_callbacks();
    if (!g_callbacks_registered) {
        RegisterXactCallback(cache_transaction_callback, nullptr);
        RegisterSubXactCallback(cache_subtransaction_callback, nullptr);
        CacheRegisterSyscacheCallback(
            PROCOID, cache_exact_syscache_invalidation, 0);
        CacheRegisterSyscacheCallback(
            AGGFNOID, cache_exact_syscache_invalidation, 0);
        before_shmem_exit(cache_backend_transition_exit, 0);
        g_callbacks_registered = true;
    }
#if defined(BIOSEQ_CACHE_TESTING)
    dependency_runtime::set_dependency_runtime_test_hooks(
        dependency_runtime::DependencyRuntimeTestHooksV1{
            .pause = dependency_runtime_test_pause,
        });
#endif
    executor::exact_scan::register_methods(
        cache_scan_methods(), coverage_scan_methods());
    if (!provider_registry::initialize_provider_registry_v2()) {
        ereport(ERROR,
            (errmsg("bioseq_cache provider rendezvous is owned by another library")));
    }
}

} // namespace bioseq::cache::pg::detail
