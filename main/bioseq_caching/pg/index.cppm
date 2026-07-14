/*-------------------------------------------------------------------------
 *
 * bioseq_cache PostgreSQL adapter.
 *
 * The sibling extension deliberately contains no sequence or Bioseq table-AM
 * knowledge.  Product integrations arrive through bioseq.cache.provider_api.
 * This primary unit owns the PostgreSQL hooks, outer CustomScan, shared
 * bootstrap, GUCs, and SQL administration surface.  Storage and registry
 * behavior is enabled incrementally behind the same fail-open node.
 *
 *-------------------------------------------------------------------------
 */
module;

#include <cstdlib>

extern "C" {
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_am_d.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_class.h"
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
#include "portability/instr_time.h"
#include "storage/ipc.h"
#include "storage/condition_variable.h"
#include "storage/fd.h"
#include "storage/lwlock.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "storage/shmem.h"
#include "tcop/utility.h"
#include "utils/builtins.h"
#include "utils/array.h"
#include "utils/acl.h"
#include "utils/fmgrprotos.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/plancache.h"
#include "utils/rls.h"
#include "utils/relcache.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplestore.h"
#include "utils/wait_event.h"

#include "port/pg_crc32c.h"

PG_MODULE_MAGIC;
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
#include <new>
#include <optional>
#include <span>
#include <string>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

export module bioseq.cache.pg;

import bioseq.cache.core;
import bioseq.cache.pg.maintenance;
import bioseq.cache.pg.provider_registry;
import bioseq.cache.pg.reusable_adapter;
import bioseq.cache.pg.reusable_planning;
import bioseq.cache.pg.storage;

namespace bioseq::cache::pg {

inline constexpr std::uint32_t kSharedMagic = 0x42535143U; /* BSQC */
inline constexpr std::uint32_t kSharedAbiVersion = 5;
inline constexpr char kSharedName[] = "bioseq_cache shared state v5";
inline constexpr char kRegistryTrancheName[] = "bioseq_cache registry";
inline constexpr char kReusableSharedName[] =
    "bioseq_cache reusable registry v1";
inline constexpr char kReusableWaitSlotsName[] =
    "bioseq_cache reusable wait slots v1";
inline constexpr char kReusableMembershipTrancheName[] =
    "bioseq_cache reusable membership";
inline constexpr char kReusableExactTrancheName[] =
    "bioseq_cache reusable exact partitions";
inline constexpr char kReusableCoverageTrancheName[] =
    "bioseq_cache reusable coverage partitions";
inline constexpr std::size_t kRegistryCapacity = 64;
inline constexpr std::size_t kMaximumKeyBytes = 64U * 1024U;
inline constexpr std::uint64_t kMaximumStoredResultBytes = 256ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kRelationEpochCapacity = 1024;
inline constexpr std::size_t kMaximumRelationDependencies = 256;

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

/** User-visible cache optimization modes. */
enum class QueryCacheMode : int {
    Disabled = 0,
    Same = 1,
    Reuse = 2,
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
    Count = 4,
};

struct CacheTestControl {
    pg_atomic_uint32 armed_mask;
    pg_atomic_uint32 paused_mask;
    pg_atomic_uint32 released_mask;
    pg_atomic_uint32 fail_next_write;
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
    Producing = 1,
    Complete = 2,
    Failed = 3,
    Retired = 4,
};

struct RegistryWaiterOwner {
    bool active;
    std::uint8_t reserved8[3];
    int owner_pid;
    std::uint32_t owner_proc_number;
    std::uint32_t reserved32;
    std::uint64_t registration_sequence;
    TimestampTz registered_at;
    storage::ManifestWaiterToken storage_token;
};

struct RegistryReaderOwner {
    bool active;
    std::uint8_t reserved8[3];
    int owner_pid;
    std::uint32_t owner_proc_number;
    std::uint32_t reserved32;
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
    Oid database_oid;
    Oid current_role_oid;
    Oid session_user_oid;
    int producer_pid;
    ProcNumber producer_proc_number;
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
    std::uint16_t reserved;
    Oid relation_oids[kMaximumRelationDependencies];
    storage::ManifestHandle manifest_handle;
    char display_sql[4096];
    ConditionVariable changed;
    unsigned char key[kMaximumKeyBytes];
};

/** Collision-checked bounded dependency epoch; protected by registry lock. */
struct RelationEpochSlot {
    Oid database_oid;
    Oid relation_oid;
    std::uint64_t committed_epoch;
    bool used;
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

/** PostgreSQL wake objects are a fixed sidecar keyed by ABA-safe slot index. */
struct ReusableWaitSlotV1 {
    ConditionVariable state_changed;
};

static_assert(std::is_standard_layout_v<CacheSharedState>);
static_assert(alignof(CacheSharedState) <= MAXIMUM_ALIGNOF);

inline CacheSharedState* g_shared = nullptr;
inline LWLock* g_registry_lock = nullptr;
inline ReusableRegistryAttachmentV1 g_reusable_registry{};
inline LWLock* g_reusable_membership_lock = nullptr;
inline LWLockPadded* g_reusable_exact_locks = nullptr;
inline LWLockPadded* g_reusable_coverage_locks = nullptr;
inline ReusableWaitSlotV1* g_reusable_wait_slots = nullptr;
inline std::uint32_t g_reusable_wait_slot_count = 0;
inline shmem_request_hook_type g_previous_shmem_request_hook = nullptr;
inline shmem_startup_hook_type g_previous_shmem_startup_hook = nullptr;
inline planner_hook_type g_previous_planner_hook = nullptr;
inline create_upper_paths_hook_type g_previous_create_upper_paths_hook = nullptr;
inline ExecutorStart_hook_type g_previous_executor_start_hook = nullptr;
inline ProcessUtility_hook_type g_previous_process_utility_hook = nullptr;
inline bool g_preload_requested = false;
inline bool g_gucs_registered = false;
inline bool g_hooks_installed = false;
inline bool g_callbacks_registered = false;
inline int g_query_cache_mode = static_cast<int>(QueryCacheMode::Disabled);
inline int g_query_cache_ttl_ms = 60 * 60 * 1000;
inline bool g_internal_planning = false;
#if defined(BIOSEQ_CACHE_TESTING)
inline bool g_forced_coverage_domain_digest_active = false;
inline CacheDigest256 g_forced_coverage_domain_digest{};
#endif

#if defined(BIOSEQ_CACHE_TESTING)
/** Backend-local verifier faults exercise unavailable third-party AM paths. */
enum class TestExactPlanFault : std::uint8_t {
    None,
    UnknownCustomScan,
    UnknownTableAccessMethod,
    UnknownIndexAccessMethod,
};

inline TestExactPlanFault g_test_exact_plan_fault = TestExactPlanFault::None;
#endif

/** Backend-local dirty state follows savepoint commit and rollback exactly. */
struct DirtyRelationObservationV1 {
    Oid relation_oid;
    bool physical_identity_valid;
    bool storage_parent;
    std::array<std::uint8_t, 2> reserved{};
    PhysicalIdentityV1 physical_identity{};
};

struct DirtyFrame {
    DirtyFrame* next;
    SubTransactionId subtransaction_id;
    bool dirty;
    bool coarse;
    std::uint16_t relation_count;
    DirtyRelationObservationV1 relations[kMaximumRelationDependencies];
};

inline DirtyFrame* g_dirty_frames = nullptr;
inline bool g_database_transition_published = false;
inline TransactionId g_database_transition_xid = InvalidTransactionId;
inline bool g_force_coarse_transition = false;

/** Per-planner invocation frame; PostgreSQL backends are single-threaded. */
struct PlannerFrame {
    PlannerFrame* previous = nullptr;
    Query* delegated_query = nullptr;
    char* canonical_query = nullptr;
    char* display_sql = nullptr;
    List* referenced_parameter_ids = NIL;
    List* referenced_relation_oids = NIL;
    std::array<const provider_registry::ProviderV1*,
        provider_registry::kMaximumRegisteredProvidersV1> providers{};
    std::array<reuse::PlannerOwnedIntentV1,
        provider_registry::kMaximumRegisteredProvidersV1> intents{};
    std::size_t provider_intent_count = 0;
    bool eligible = false;
};

inline PlannerFrame* g_planner_frame = nullptr;
inline bool g_producer_plan_capture_active = false;

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
    ConditionVariableInit(&shared.test_control.changed);
    pg_atomic_init_u64(&shared.next_injected_generation, 1);
#endif
    for (CacheEntry& entry : shared.entries) {
        entry.state = SharedEntryState::Empty;
        ConditionVariableInit(&entry.changed);
    }
}

/** Locate or create a savepoint-aware dirty frame in backend-lifetime memory. */
[[nodiscard]] DirtyFrame* dirty_frame(SubTransactionId subtransaction_id) {
    for (DirtyFrame* frame = g_dirty_frames; frame != nullptr;
         frame = frame->next) {
        if (frame->subtransaction_id == subtransaction_id) {
            return frame;
        }
    }
    MemoryContext old_context = MemoryContextSwitchTo(TopMemoryContext);
    auto* frame = static_cast<DirtyFrame*>(palloc0(sizeof(DirtyFrame)));
    MemoryContextSwitchTo(old_context);
    frame->subtransaction_id = subtransaction_id;
    frame->next = g_dirty_frames;
    g_dirty_frames = frame;
    return frame;
}

/** Remember a possible result-changing write until its transaction outcome. */
void mark_current_transaction_dirty() {
    if (IsTransactionState()) {
        DirtyFrame* frame = dirty_frame(GetCurrentSubTransactionId());
        frame->dirty = true;
        frame->coarse = true;
    }
}

enum class RelationPhysicalIdentityStatusV1 : std::uint8_t {
    Unknown = 0,
    StorageParent = 1,
    Precise = 2,
};

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
        || sizeof(locator) > destination->bytes.size()) {
        return RelationPhysicalIdentityStatusV1::Unknown;
    }
    *destination = PhysicalIdentityV1{
        .kind = PhysicalIdentityKind::PgRelFileLocator,
        .length = sizeof(locator),
    };
    std::memcpy(destination->bytes.data(), &locator, sizeof(locator));
    return RelationPhysicalIdentityStatusV1::Precise;
}

[[nodiscard]] bool copy_provider_physical_identity(
        const provider_registry::PhysicalIdentityV1* source,
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
        const provider_registry::PhysicalIdentityV1* provider_identity = nullptr) {
    if (!IsTransactionState() || !OidIsValid(relation_oid)) {
        return;
    }
    DirtyFrame* frame = dirty_frame(GetCurrentSubTransactionId());
    frame->dirty = true;
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
    for (std::uint16_t index = 0; index < frame->relation_count; ++index) {
        DirtyRelationObservationV1& existing = frame->relations[index];
        if (existing.relation_oid != relation_oid) {
            continue;
        }
        if (physical_identity_valid && !existing.physical_identity_valid) {
            existing.physical_identity_valid = true;
            existing.storage_parent = false;
            existing.physical_identity = physical_identity;
            return;
        }
        if (storage_parent && existing.storage_parent) {
            return;
        }
        if ((!physical_identity_valid && !storage_parent)
            || (existing.physical_identity_valid
                && existing.physical_identity == physical_identity)) {
            return;
        }
    }
    if (frame->relation_count >= kMaximumRelationDependencies) {
        frame->coarse = true;
        return;
    }
    frame->relations[frame->relation_count++] = DirtyRelationObservationV1{
        .relation_oid = relation_oid,
        .physical_identity_valid = physical_identity_valid,
        .storage_parent = storage_parent,
        .physical_identity = physical_identity,
    };
}

/** Provider write observations join the same savepoint-aware epoch ledger. */
void observe_provider_relation_write(
        std::uint32_t database_oid, std::uint32_t relation_oid,
        const provider_registry::PhysicalIdentityV1* physical_identity) noexcept {
    if (database_oid == static_cast<std::uint32_t>(MyDatabaseId)) {
        mark_current_transaction_relation_dirty(
            static_cast<Oid>(relation_oid), physical_identity);
    }
}

/** Implemented after the reusable-registry lock adapters are available. */
void consider_provider_reusable_paths(
    PlannerInfo*, RelOptInfo*, unsigned int, RangeTblEntry*,
    const provider_registry::ProviderV1*) noexcept;

/** Provider-owned path hook rendezvous; every optional failure is fail-open. */
void observe_provider_rel_paths(
        PlannerInfo* root, RelOptInfo* relation,
        unsigned int range_table_index, RangeTblEntry* range_entry,
        const provider_registry::ProviderV1* provider) noexcept {
    if (g_query_cache_mode != static_cast<int>(QueryCacheMode::Reuse)
        || !g_reusable_registry) {
        return;
    }
    consider_provider_reusable_paths(
        root, relation, range_table_index, range_entry, provider);
}

/** True when at least one non-aborted savepoint performed a write. */
[[nodiscard]] bool transaction_is_dirty() noexcept {
    for (const DirtyFrame* frame = g_dirty_frames; frame != nullptr;
         frame = frame->next) {
        if (frame->dirty) {
            return true;
        }
    }
    return false;
}

/** Drop all backend-local transaction frames without retaining stale pointers. */
void clear_dirty_frames() noexcept {
    while (g_dirty_frames != nullptr) {
        DirtyFrame* frame = g_dirty_frames;
        g_dirty_frames = frame->next;
        pfree(frame);
    }
}

/** Remove one subtransaction frame and optionally merge it into its parent. */
void finish_dirty_subtransaction(
        SubTransactionId child_id, SubTransactionId parent_id,
        bool committed) {
    DirtyFrame** link = &g_dirty_frames;
    while (*link != nullptr && (*link)->subtransaction_id != child_id) {
        link = &(*link)->next;
    }
    if (*link == nullptr) {
        return;
    }
    DirtyFrame* child = *link;
    *link = child->next;
    if (committed && child->dirty) {
        DirtyFrame* parent = dirty_frame(parent_id);
        parent->dirty = true;
        parent->coarse = parent->coarse || child->coarse;
        for (std::uint16_t index = 0;
             index < child->relation_count; ++index) {
            const DirtyRelationObservationV1& observation =
                child->relations[index];
            bool present = false;
            for (std::uint16_t parent_index = 0;
                 parent_index < parent->relation_count; ++parent_index) {
                DirtyRelationObservationV1& existing =
                    parent->relations[parent_index];
                if (existing.relation_oid != observation.relation_oid) {
                    continue;
                }
                if (observation.physical_identity_valid
                    && !existing.physical_identity_valid) {
                    existing = observation;
                    present = true;
                    break;
                }
                if (!observation.physical_identity_valid
                    || (existing.physical_identity_valid
                        && existing.physical_identity
                            == observation.physical_identity)) {
                    present = true;
                    break;
                }
            }
            if (present) {
                continue;
            }
            if (parent->relation_count >= kMaximumRelationDependencies) {
                parent->coarse = true;
                break;
            }
            parent->relations[parent->relation_count++] = observation;
        }
    }
    pfree(child);
}

[[nodiscard]] constexpr std::size_t relation_epoch_start(
        Oid database_oid, Oid relation_oid) noexcept {
    const std::uint64_t mixed =
        static_cast<std::uint64_t>(database_oid) * 0x9e3779b185ebca87ULL
        ^ static_cast<std::uint64_t>(relation_oid) * 0xc2b2ae3d27d4eb4fULL;
    return static_cast<std::size_t>(mixed % kRelationEpochCapacity);
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

/** Advance one precise dependency under the registry lock; false means full. */
[[nodiscard]] bool bump_relation_epoch_locked(Oid relation_oid) noexcept {
    const std::size_t start = relation_epoch_start(MyDatabaseId, relation_oid);
    for (std::size_t probe = 0; probe < kRelationEpochCapacity; ++probe) {
        RelationEpochSlot& slot =
            g_shared->relation_epochs[(start + probe) % kRelationEpochCapacity];
        if (!slot.used) {
            slot.used = true;
            slot.database_oid = MyDatabaseId;
            slot.relation_oid = relation_oid;
            slot.committed_epoch = 2;
            return true;
        }
        if (slot.database_oid == MyDatabaseId
            && slot.relation_oid == relation_oid) {
            ++slot.committed_epoch;
            if (slot.committed_epoch == 0) {
                slot.committed_epoch = 1;
            }
            return true;
        }
    }
    return false;
}

/** Publish savepoint-filtered precise dependencies, returning coarse fallback. */
[[nodiscard]] bool publish_dirty_dependency_epochs() noexcept {
    bool coarse = g_force_coarse_transition;
    Oid published[kMaximumRelationDependencies]{};
    std::size_t published_count = 0;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (const DirtyFrame* frame = g_dirty_frames;
         frame != nullptr; frame = frame->next) {
        coarse = coarse || frame->coarse;
        for (std::uint16_t index = 0;
             index < frame->relation_count; ++index) {
            const Oid relation_oid = frame->relations[index].relation_oid;
            bool duplicate = false;
            for (std::size_t prior = 0; prior < published_count; ++prior) {
                duplicate = duplicate || published[prior] == relation_oid;
            }
            if (duplicate) {
                continue;
            }
            if (published_count >= kMaximumRelationDependencies
                || !bump_relation_epoch_locked(relation_oid)) {
                coarse = true;
                continue;
            }
            published[published_count++] = relation_oid;
        }
    }
    LWLockRelease(g_registry_lock);
    return coarse;
}

/** Defined after reusable lock/CV adapters; called only during commit cleanup. */
void invalidate_dirty_reusable_generations() noexcept;

/** Announce a commit transition so concurrent captures cannot race visibility. */
void begin_database_transition() noexcept {
    if (g_database_transition_published || g_shared == nullptr) {
        return;
    }
    pg_atomic_fetch_add_u32(&g_shared->database_transition_count, 1);
    g_database_transition_xid = GetTopTransactionIdIfAny();
    g_database_transition_published = true;
    ConditionVariableBroadcast(&g_shared->database_changed);
}

/** Complete or abandon an announced transition in visibility-safe order. */
void finish_database_transition(bool committed) noexcept {
    if (!g_database_transition_published || g_shared == nullptr) {
        g_database_transition_published = false;
        g_database_transition_xid = InvalidTransactionId;
        g_force_coarse_transition = false;
        return;
    }
    if (committed) {
        if (publish_dirty_dependency_epochs()) {
            pg_atomic_fetch_add_u64(
                &g_shared->coarse_dependency_epoch, 1);
        }
        if (TransactionIdIsValid(g_database_transition_xid)) {
            pg_atomic_write_u32(
                &g_shared->last_committed_write_xid,
                g_database_transition_xid);
        }
        pg_atomic_fetch_add_u64(&g_shared->database_write_epoch, 1);
        pg_atomic_fetch_add_u64(&g_shared->stats.invalidations, 1);
        invalidate_dirty_reusable_generations();
    }
    pg_atomic_fetch_sub_u32(&g_shared->database_transition_count, 1);
    g_database_transition_published = false;
    g_database_transition_xid = InvalidTransactionId;
    g_force_coarse_transition = false;
    ConditionVariableBroadcast(&g_shared->database_changed);
}

/** Publish write epochs only for successful top-level outcomes. */
void cache_transaction_callback(XactEvent event, void*) {
    switch (event) {
        case XACT_EVENT_PRE_COMMIT:
        case XACT_EVENT_PRE_PREPARE:
            if (transaction_is_dirty()) {
                begin_database_transition();
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
    const provider_registry::ProviderV1*) noexcept;

void consider_provider_reusable_paths(
        PlannerInfo* root, RelOptInfo* relation,
        unsigned int range_table_index, RangeTblEntry* range_entry,
        const provider_registry::ProviderV1* provider) noexcept {
    consider_provider_reusable_paths_impl(
        root, relation, range_table_index, range_entry, provider);
}

[[nodiscard]] std::uint64_t reusable_monotonic_now_ns() noexcept {
    instr_time current{};
    INSTR_TIME_SET_CURRENT(current);
    return INSTR_TIME_GET_NANOSEC(current);
}

[[nodiscard]] ReusableProducerIdentityV1
current_reusable_producer_identity() noexcept {
    const std::uint64_t nonce =
        static_cast<std::uint64_t>(MyStartTimestamp)
        ^ (static_cast<std::uint64_t>(MyProcPid) << 32U)
        ^ static_cast<std::uint64_t>(MyProcNumber);
    return ReusableProducerIdentityV1{
        .proc_number = static_cast<std::uint32_t>(MyProcNumber),
        .pid = MyProcPid,
        .backend_nonce = nonce == 0 ? 1 : nonce,
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
    if (producer.pid <= 0
        || producer.proc_number
            >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    PGPROC* process = ProcNumberGetProc(
        static_cast<ProcNumber>(producer.proc_number));
    return process != nullptr && process->pid == producer.pid;
}

/** Request bounded shared memory and partitioned registry locks. */
void cache_shmem_request() {
    if (g_previous_shmem_request_hook != nullptr) {
        g_previous_shmem_request_hook();
    }
    RequestAddinShmemSpace(MAXALIGN(sizeof(CacheSharedState)));
    RequestNamedLWLockTranche(kRegistryTrancheName, 1);
    const auto reusable_request = reusable_registry_preload_request(
        reusable_registry_config());
    if (!reusable_request.has_value()) {
        ereport(ERROR,
            (errmsg("bioseq_cache reusable registry configuration is invalid")));
    }
    RequestAddinShmemSpace(MAXALIGN(reusable_request->layout.total_bytes));
    RequestNamedLWLockTranche(
        kReusableMembershipTrancheName,
        reusable_request->membership_lock_count);
    RequestNamedLWLockTranche(
        kReusableExactTrancheName,
        reusable_request->exact_partition_lock_count);
    RequestNamedLWLockTranche(
        kReusableCoverageTrancheName,
        reusable_request->coverage_partition_lock_count);
    RequestAddinShmemSpace(MAXALIGN(
        sizeof(ReusableWaitSlotV1)
            * reusable_request->config.max_generations));
    storage::request_storage_resources();
}

/** Attach or initialize the postmaster-lifetime cache header. */
void cache_shmem_startup() {
    if (g_previous_shmem_startup_hook != nullptr) {
        g_previous_shmem_startup_hook();
    }

    bool found = false;
    g_shared = static_cast<CacheSharedState*>(
        ShmemInitStruct(kSharedName, sizeof(CacheSharedState), &found));
    if (!found) {
        initialize_shared_state(*g_shared);
    }
    g_registry_lock = &GetNamedLWLockTranche(kRegistryTrancheName)[0].lock;
    const std::uint64_t boot_nonce = pg_atomic_read_u64(&g_shared->boot_nonce);
    const auto reusable_request = reusable_registry_preload_request(
        reusable_registry_config());
    if (!reusable_request.has_value()) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    bool reusable_found = false;
    void* reusable_region = ShmemInitStruct(
        kReusableSharedName,
        reusable_request->layout.total_bytes,
        &reusable_found);
    if (reusable_region == nullptr) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    auto reusable_started = startup_reusable_registry(
        std::span<std::byte>{
            static_cast<std::byte*>(reusable_region),
            reusable_request->layout.total_bytes},
        boot_nonce,
        reusable_request->config,
        reusable_found);
    if (!reusable_started.has_value()) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    g_reusable_registry = *reusable_started;
    g_reusable_membership_lock =
        &GetNamedLWLockTranche(kReusableMembershipTrancheName)[0].lock;
    g_reusable_exact_locks =
        GetNamedLWLockTranche(kReusableExactTrancheName);
    g_reusable_coverage_locks =
        GetNamedLWLockTranche(kReusableCoverageTrancheName);
    bool wait_slots_found = false;
    const std::size_t wait_slots_bytes = sizeof(ReusableWaitSlotV1)
        * reusable_request->config.max_generations;
    g_reusable_wait_slots = static_cast<ReusableWaitSlotV1*>(
        ShmemInitStruct(
            kReusableWaitSlotsName,
            wait_slots_bytes,
            &wait_slots_found));
    if (g_reusable_wait_slots == nullptr) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        return;
    }
    g_reusable_wait_slot_count = reusable_request->config.max_generations;
    if (!wait_slots_found) {
        for (std::uint32_t index = 0;
             index < g_reusable_wait_slot_count;
             ++index) {
            ConditionVariableInit(
                &g_reusable_wait_slots[index].state_changed);
        }
    }
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

/** True only when preload installed and the shared ABI is usable. */
[[nodiscard]] bool shared_cache_ready() noexcept {
    const bool header_ready = g_preload_requested
        && g_shared != nullptr
        && g_shared->magic == kSharedMagic
        && g_shared->shared_abi_version == kSharedAbiVersion
        && pg_atomic_read_u32(&g_shared->ready) != 0
        && pg_atomic_read_u32(&g_shared->bootstrap_failure) == 0;
    if (!header_ready) {
        return false;
    }
    if (!storage::storage_ready() && !storage::attach_storage()) {
        return false;
    }
    const CacheCorrectnessReadiness required_readiness =
        g_query_cache_mode == static_cast<int>(QueryCacheMode::Reuse)
        ? CacheCorrectnessReadiness::ReusableCoverageReady
        : CacheCorrectnessReadiness::GenericMvccReady;
    return storage::runtime_snapshot().correctness_readiness
            >= required_readiness
        && (required_readiness
                != CacheCorrectnessReadiness::ReusableCoverageReady
            || (g_reusable_registry
                && g_reusable_membership_lock != nullptr
                && g_reusable_exact_locks != nullptr
                && g_reusable_coverage_locks != nullptr
                && g_reusable_wait_slots != nullptr
                && g_reusable_wait_slot_count != 0));
}

[[nodiscard]] bool cache_interrupt_error(int sqlstate) noexcept {
    return sqlstate == ERRCODE_QUERY_CANCELED
        || sqlstate == ERRCODE_ADMIN_SHUTDOWN
        || sqlstate == ERRCODE_CRASH_SHUTDOWN
        || ProcDiePending;
}

inline constexpr std::uint32_t kCoverageDescriptorMagicV1 = 0x42434431U;
inline constexpr std::uint32_t kCoverageDescriptorFormatV1 = 1;

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
    std::uint32_t reserved = 0;
};

static_assert(std::is_standard_layout_v<CoverageDescriptorHeaderV1>);
static_assert(std::is_trivially_copyable_v<CoverageDescriptorHeaderV1>);

/** Planner-owned reconstruction of one pointer-free coverage descriptor. */
struct PlannerDecodedCoverageV1 {
    MemoryContext owner_context = nullptr;
    BioseqCachePathDomainV1 domain{};
    BioseqCacheReusableSourceV1 source{};
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

[[nodiscard]] bool coverage_descriptor_size(
        const CoverageDescriptorHeaderV1& header,
        std::uint64_t* total) noexcept {
    if (total == nullptr
        || header.magic != kCoverageDescriptorMagicV1
        || header.format_version != kCoverageDescriptorFormatV1
        || header.provider_id == 0 || header.flags != 0
        || header.compatibility_family_id == 0 || header.reserved != 0
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
            sizeof(BioseqCacheColumnSchemaV1))
        || !add_coverage_descriptor_size(
            bytes, header.lineage_count,
            sizeof(BioseqCacheColumnSchemaV1))
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
        const BioseqCachePathDomainV1& domain,
        MemoryContext planner_context) noexcept {
    if (planner_context == nullptr
        || !reuse::validate_path_domain(
                domain.provider_id, &domain).has_value()) {
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
        const auto append = [&](const void* source, std::size_t size) {
            if (size != 0) {
                std::memcpy(destination + offset, source, size);
                offset += size;
            }
        };
        append(&header, sizeof(header));
        append(
            domain.boundary_columns,
            domain.boundary_column_count
                * sizeof(BioseqCacheColumnSchemaV1));
        append(
            domain.lineage_columns,
            domain.lineage_column_count
                * sizeof(BioseqCacheColumnSchemaV1));
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
        PlannerDecodedCoverageV1* output) noexcept {
    if (planner_context == nullptr || output == nullptr
        || bytes.size() < sizeof(CoverageDescriptorHeaderV1)
        || bytes.size() > kMaxProviderRecipeBytes) {
        return false;
    }
    CoverageDescriptorHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    std::uint64_t expected_size = 0;
    if (!coverage_descriptor_size(header, &expected_size)
        || expected_size != bytes.size()) {
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
        auto copy_array = [&](std::size_t count, std::size_t width) -> void* {
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
        auto* boundary = static_cast<BioseqCacheColumnSchemaV1*>(copy_array(
            header.boundary_count, sizeof(BioseqCacheColumnSchemaV1)));
        auto* lineage = static_cast<BioseqCacheColumnSchemaV1*>(copy_array(
            header.lineage_count, sizeof(BioseqCacheColumnSchemaV1)));
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
        decoded->domain = BioseqCachePathDomainV1{
            .struct_size = sizeof(BioseqCachePathDomainV1),
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
        decoded->source = BioseqCacheReusableSourceV1{
            .struct_size = sizeof(BioseqCacheReusableSourceV1),
            .provider_id = header.provider_id,
            .compatibility_family_id = header.compatibility_family_id,
            .capability_mask = header.capability_mask,
            .boundary_columns = boundary,
            .boundary_column_count = header.boundary_count,
            .lineage_columns = lineage,
            .lineage_column_count = header.lineage_count,
            .provider_private_nodes = static_cast<List*>(provider_private),
        };
        if (!reuse::validate_path_domain(
                header.provider_id, &decoded->domain).has_value()) {
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

struct PlannerSemanticKeysV1 {
    bytea* canonical_domain_key = nullptr;
    bytea* canonical_generation_key = nullptr;
    CacheDigest256 domain_digest{};
    CacheDigest256 generation_digest{};
    ReusableSecurityNamespaceV1 security_namespace{};
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
        const provider_registry::ProviderV1* provider,
        const BioseqCachePathDomainV1& domain,
        MemoryContext planner_context,
        PlannerSemanticKeysV1* output) noexcept {
    if (provider == nullptr || planner_context == nullptr || output == nullptr
        || !reuse::validate_path_domain(
            domain.provider_id, &domain).has_value()) {
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
                    domain_payload, domain_payload_capacity},
                &domain_payload_size,
                planner_context,
                &diagnostic)
            || !reuse::invoke_semantic_digest_fail_open(
                provider,
                &domain,
                reuse::SemanticDigestKindV1::Generation,
                std::span<std::byte>{
                    generation_payload, generation_payload_capacity},
                &generation_payload_size,
                planner_context,
                &diagnostic)) {
            ereport(ERROR,
                (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                 errmsg("bioseq cache provider semantic key is unavailable")));
        }
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
        PostgreSqlColumnChecksumPolicy checksum_policy;
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
    const auto take = [&](std::uint32_t size) {
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
    std::uint32_t dependency_count = 0;
    WriteObservability observability = WriteObservability::None;
    bool valid = false;
};

/** Refuse a reusable snapshot that can observe this backend's own writes. */
[[nodiscard]] bool current_transaction_touches_dependency(
        Oid relation_oid,
        const PhysicalIdentityV1& physical_identity) noexcept {
    if (g_force_coarse_transition) {
        return true;
    }
    for (const DirtyFrame* frame = g_dirty_frames;
         frame != nullptr; frame = frame->next) {
        if (!frame->dirty) {
            continue;
        }
        if (frame->coarse) {
            return true;
        }
        for (std::uint16_t index = 0;
             index < frame->relation_count; ++index) {
            const DirtyRelationObservationV1& observation =
                frame->relations[index];
            if (!observation.physical_identity_valid
                && !observation.storage_parent) {
                return true;
            }
            if (observation.relation_oid == relation_oid
                || (observation.physical_identity_valid
                    && observation.physical_identity == physical_identity)) {
                return true;
            }
        }
    }
    return false;
}

/** Version one admits one precise provider leaf plus the coarse overflow epoch. */
[[nodiscard]] CoverageDependencyTicketV1 coverage_dependency_ticket(
        const BioseqCachePathDomainV1& contract,
        const provider_registry::ProviderV1* provider) noexcept {
    CoverageDependencyTicketV1 ticket{};
    if (g_shared == nullptr || provider == nullptr
        || (provider->provider_flags
                & BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION) == 0
        || contract.dependency_nodes == NIL
        || list_length(contract.dependency_nodes) <= 0
        || list_length(contract.dependency_nodes)
            > static_cast<int>(kMaximumRelationDependencies)
        || pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
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

    std::array<std::byte,
        128U + kMaximumRelationDependencies
            * (sizeof(Oid) + sizeof(std::uint64_t))> bytes{};
    std::size_t offset = 0;
    const auto append = [&](const void* source, std::size_t size) {
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
    const std::uint64_t coarse_epoch = pg_atomic_read_u64(
        &g_shared->coarse_dependency_epoch);
    if (!append(&version, sizeof(version))
        || !append(&database_oid, sizeof(database_oid))
        || !append(&coarse_epoch, sizeof(coarse_epoch))
        || !append(&dependency_count, sizeof(dependency_count))) {
        return ticket;
    }
    for (std::size_t index = 0; index < relation_count; ++index) {
        const std::uint64_t epoch = relation_dependency_epoch(
            relation_oids[index]);
        if (epoch == 0
            || !append(&relation_oids[index], sizeof(relation_oids[index]))
            || !append(&epoch, sizeof(epoch))) {
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
    if (pg_atomic_read_u32(&g_shared->database_transition_count) != 0
        || current_transaction_touches_dependency(
            relation_oids[0], ticket.physical_identity)) {
        return ticket;
    }
    ticket.fingerprint = *digest;
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
        ? &g_reusable_wait_slots[reference.generation_slot].state_changed
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
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr
        || MyDatabaseId == InvalidOid) {
        return;
    }
    std::array<PhysicalIdentityV1, kMaximumRelationDependencies> identities{};
    std::size_t identity_count = 0;
    bool coarse = g_force_coarse_transition;
    bool storage_parent_seen = false;
    for (const DirtyFrame* frame = g_dirty_frames;
         frame != nullptr; frame = frame->next) {
        coarse = coarse || frame->coarse;
        for (std::uint16_t index = 0;
             index < frame->relation_count; ++index) {
            const DirtyRelationObservationV1& observation =
                frame->relations[index];
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
    }
    coarse = coarse || (storage_parent_seen && identity_count == 0);
    if (!coarse && identity_count == 0) {
        return;
    }

    std::array<ReusableGenerationReferenceV1,
        kDefaultReusableRegistryGenerationsV1> affected{};
    std::size_t affected_count = 0;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        const auto collect = [&](const ReusableInvalidationResultV1& result) {
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
        const auto run_selector = [&](ReusableInvalidationSelectorV1 selector) {
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
        }
        if (coarse || !precise_ok) {
            static_cast<void>(run_selector(ReusableInvalidationSelectorV1{
                .database_oid = static_cast<std::uint32_t>(MyDatabaseId),
                .coarse_database_fallback = true,
            }));
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
    }
    PG_END_TRY();
    for (std::size_t index = 0; index < affected_count; ++index) {
        broadcast_reusable_generation(affected[index]);
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
        LWLockAcquire(g_reusable_membership_lock, LW_SHARED);
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
        const provider_registry::ProviderV1& provider,
        const BioseqCachePathDomainV1& domain,
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
    const auto append = [&](const void* source, std::size_t size) {
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

inline constexpr std::uint32_t kCoverageMappingMagicV1 = 0x42434d31U;

struct CoverageMappingHeaderV1 {
    std::uint32_t magic = kCoverageMappingMagicV1;
    std::uint32_t format_version = reuse::kReusablePlanFormatVersionV1;
    std::uint32_t boundary_count = 0;
    std::uint32_t lineage_count = 0;
};

struct CoverageMappingViewV1 {
    CoverageMappingHeaderV1 header{};
    std::span<const std::byte> boundary{};
    std::span<const std::byte> lineage{};
};

/** Decode the unaligned bytea payload without aliasing provider ABI structs. */
[[nodiscard]] bool decode_source_mapping(
        std::span<const std::byte> bytes,
        CoverageMappingViewV1* output) noexcept {
    if (output == nullptr || bytes.size() < sizeof(CoverageMappingHeaderV1)) {
        return false;
    }
    CoverageMappingHeaderV1 header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    if (header.magic != kCoverageMappingMagicV1
        || header.format_version != reuse::kReusablePlanFormatVersionV1
        || header.boundary_count > kMaxProviderBoundaryColumns
        || header.lineage_count > kMaxProviderBoundaryColumns) {
        return false;
    }
    std::uint64_t expected = sizeof(header);
    if (!add_coverage_descriptor_size(
            expected,
            header.boundary_count,
            sizeof(BioseqCacheColumnMapEntryV1))
        || !add_coverage_descriptor_size(
            expected,
            header.lineage_count,
            sizeof(BioseqCacheColumnMapEntryV1))
        || expected != bytes.size()) {
        return false;
    }
    const std::size_t boundary_bytes =
        static_cast<std::size_t>(header.boundary_count)
        * sizeof(BioseqCacheColumnMapEntryV1);
    const std::size_t lineage_bytes =
        static_cast<std::size_t>(header.lineage_count)
        * sizeof(BioseqCacheColumnMapEntryV1);
    output->header = header;
    output->boundary = bytes.subspan(sizeof(header), boundary_bytes);
    output->lineage = bytes.subspan(
        sizeof(header) + boundary_bytes, lineage_bytes);
    return true;
}

/** Persist validated source-column maps as POD; private Nodes stay separate. */
[[nodiscard]] Const* make_source_mapping_node(
        const BioseqCacheSourceMappingV1& mapping) {
    if (mapping.boundary_map_count > kMaxProviderBoundaryColumns
        || mapping.lineage_map_count > kMaxProviderBoundaryColumns
        || (mapping.boundary_map_count != 0
            && mapping.boundary_map == nullptr)
        || (mapping.lineage_map_count != 0
            && mapping.lineage_map == nullptr)) {
        return nullptr;
    }
    const CoverageMappingHeaderV1 header{
        .boundary_count = static_cast<std::uint32_t>(
            mapping.boundary_map_count),
        .lineage_count = static_cast<std::uint32_t>(
            mapping.lineage_map_count),
    };
    std::uint64_t total = sizeof(header);
    if (!add_coverage_descriptor_size(
            total, mapping.boundary_map_count,
            sizeof(BioseqCacheColumnMapEntryV1))
        || !add_coverage_descriptor_size(
            total, mapping.lineage_map_count,
            sizeof(BioseqCacheColumnMapEntryV1))) {
        return nullptr;
    }
    auto* packed = static_cast<std::byte*>(palloc(total));
    std::size_t offset = 0;
    std::memcpy(packed + offset, &header, sizeof(header));
    offset += sizeof(header);
    const std::size_t boundary_bytes = mapping.boundary_map_count
        * sizeof(BioseqCacheColumnMapEntryV1);
    const std::size_t lineage_bytes = mapping.lineage_map_count
        * sizeof(BioseqCacheColumnMapEntryV1);
    if (boundary_bytes != 0) {
        std::memcpy(packed + offset, mapping.boundary_map, boundary_bytes);
        offset += boundary_bytes;
    }
    if (lineage_bytes != 0) {
        std::memcpy(packed + offset, mapping.lineage_map, lineage_bytes);
        offset += lineage_bytes;
    }
    return make_binary_node(
        std::span<const std::byte>{packed, static_cast<std::size_t>(total)});
}

struct PlannerPinReleaseV1 {
    ReusableReferenceTokenV1 token{};
};

struct PlannerCandidateBytesV1 {
    bytea* canonical_domain_key = nullptr;
    bytea* canonical_generation_key = nullptr;
    bytea* coverage_descriptor = nullptr;
};

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
        const provider_registry::ProviderV1* provider,
        const BioseqCacheQueryIntentV1& intent,
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
    PlannerSemanticKeysV1 keys{};
    if (!capture_planner_semantic_keys_fail_open(
            provider,
            described.value,
            planner_context,
            &keys)) {
        return false;
    }
    bytea* descriptor = serialize_coverage_descriptor_fail_open(
        described.value, planner_context);
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
            root, described.value.coverage_predicate, &current_gate)) {
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
    List* candidate_private = NIL;
    for (std::size_t index = 0; index < candidates->candidate_count; ++index) {
        if (!selected_mask[index]) {
            reuse::release_candidate_reference_noerror(
                release_records[index]);
            continue;
        }
        const ReusableCandidateSnapshotV1& candidate =
            candidates->candidates[index];
        const auto descriptor_bytes = bytea_payload(
            candidate_bytes[index].coverage_descriptor);
        PlannerDecodedCoverageV1 decoded{};
        if (!decode_coverage_descriptor_fail_open(
                descriptor_bytes, planner_context, &decoded)
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
        reuse::PlannerGateNodesV1 gate{};
        if (reference_node == nullptr || generation_key_node == nullptr
            || descriptor_node == nullptr
            || !reuse::build_planner_gate_nodes(
                root, decoded.domain.coverage_predicate, &gate)) {
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
        Const* mapping_node = make_source_mapping_node(mapping.value);
        if (mapping_node == nullptr) {
            continue;
        }
        List* source_private = list_make5(
            reference_node,
            generation_key_node,
            descriptor_node,
            mapping_node,
            copyObject(mapping.value.provider_private_nodes));
        source_private = lappend(
            source_private,
            makeInteger(gate.param_id));
        source_private = lappend(
            source_private,
            copyObject(decoded.domain.coverage_predicate));
        candidate_private = lappend(candidate_private, source_private);
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
    List* private_nodes = list_make5(
        makeInteger(reuse::kReusablePlanFormatVersionV1),
        makeInteger(static_cast<std::int32_t>(provider->provider_id)),
        current_plan,
        candidate_private,
        copyObject(described.value.coverage_predicate));
    private_nodes = lappend(
        private_nodes, makeInteger(current_gate.param_id));
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
    return wrapper != nullptr
        && reuse::replace_provider_path(relation, provider_path, wrapper);
}

/** Discover, map, decorate, and replace paths owned by one provider bridge. */
void consider_provider_reusable_paths_impl(
        PlannerInfo* root,
        RelOptInfo* relation,
        unsigned int range_table_index,
        RangeTblEntry* range_entry,
        const provider_registry::ProviderV1* provider) noexcept {
    PlannerFrame* frame = g_planner_frame;
    if (frame == nullptr || !frame->eligible || root == nullptr
        || relation == nullptr || range_entry == nullptr
        || provider == nullptr || relation->pathlist == NIL
        || root->parse != frame->delegated_query
        || range_table_index == 0
        || range_table_index > static_cast<unsigned int>(
            list_length(root->parse->rtable))) {
        return;
    }
    const BioseqCacheQueryIntentV1* intent = nullptr;
    for (std::size_t index = 0; index < frame->provider_intent_count; ++index) {
        if (frame->providers[index] == provider) {
            intent = &frame->intents[index].value;
            break;
        }
    }
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
    TransactionId last_committed_write_xid = InvalidTransactionId;
    bool valid = false;
};

/** Bind the committed-write epoch to the statement's already-active snapshot. */
[[nodiscard]] VisibilityTicket mint_visibility_ticket() noexcept {
    VisibilityTicket ticket{};
    if (!shared_cache_ready()) {
        return ticket;
    }
    Snapshot snapshot = GetActiveSnapshot();
    if (snapshot == nullptr || !IsMVCCSnapshot(snapshot)) {
        return ticket;
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
            return ticket;
        }
        const std::uint64_t before = pg_atomic_read_u64(
            &g_shared->database_write_epoch);
        pg_read_barrier();
        const TransactionId last_write = static_cast<TransactionId>(
            pg_atomic_read_u32(&g_shared->last_committed_write_xid));
        pg_read_barrier();
        const std::uint64_t after = pg_atomic_read_u64(
            &g_shared->database_write_epoch);
        if (before != after
            || pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
            continue;
        }
        if (TransactionIdIsValid(last_write)
            && XidInMVCCSnapshot(last_write, snapshot)) {
            return ticket;
        }
        ticket.database_epoch = after;
        ticket.last_committed_write_xid = last_write;
        ticket.valid = true;
        return ticket;
    }
    return ticket;
}

/** Recheck the ticket after dependency-key construction closes commit races. */
[[nodiscard]] bool visibility_ticket_still_bound(
        const VisibilityTicket& ticket) noexcept {
    if (!ticket.valid || g_shared == nullptr
        || pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
        return false;
    }
    pg_read_barrier();
    return pg_atomic_read_u64(&g_shared->database_write_epoch)
        == ticket.database_epoch;
}

/** Sweep reusable owners under registry locks; wake and retire after unlock. */
void reusable_registry_janitor_sweep_v1() noexcept {
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr
        || g_reusable_exact_locks == nullptr
        || g_reusable_coverage_locks == nullptr
        || g_reusable_wait_slots == nullptr
        || g_reusable_wait_slot_count == 0
        || ProcGlobal == nullptr) {
        return;
    }
    LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
    const ReusableJanitorSweepResultV1 sweep = janitor_reusable_registry(
        g_reusable_registry,
        reusable_monotonic_now_ns(),
        kMaximumReusableJanitorBatchV1,
        ReusableJanitorCallbacksV1{
            .producer_alive = reusable_producer_alive,
            .reference_owner_alive = reusable_producer_alive,
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
    if (sweep.error != ReusableRegistryErrorV1::None && g_shared != nullptr) {
        pg_atomic_write_u32(&g_shared->bootstrap_failure, 1);
        pg_atomic_write_u32(&g_shared->ready, 0);
        for (std::uint32_t index = 0;
             index < g_reusable_wait_slot_count; ++index) {
            ConditionVariableBroadcast(
                &g_reusable_wait_slots[index].state_changed);
        }
    }
}

/** Reject non-disabled use when the library was not preloaded. */
bool check_query_cache_mode(int* new_value, void**, GucSource) {
    if (*new_value == static_cast<int>(QueryCacheMode::Disabled)) {
        return true;
    }
    if (process_shared_preload_libraries_in_progress || g_preload_requested) {
        return true;
    }
    GUC_check_errmsg("bioseq_cache shared memory is not registered");
    GUC_check_errhint(
        "Add bioseq_cache to shared_preload_libraries and restart PostgreSQL.");
    return false;
}

/** Invalidate generic prepared plans on every real mode transition. */
void assign_query_cache_mode(int new_value, void*) {
    static int previous_value = static_cast<int>(QueryCacheMode::Disabled);
    const bool changed = previous_value != new_value;
    previous_value = new_value;
    if (changed && IsTransactionState()) {
        ResetPlanCache();
    }
}

/** Register exactly the two production GUCs requested by DOC0371. */
void register_cache_gucs() {
    if (g_gucs_registered) {
        return;
    }
    static const config_enum_entry mode_options[] = {
        {"disabled", static_cast<int>(QueryCacheMode::Disabled), false},
        {"same", static_cast<int>(QueryCacheMode::Same), false},
        {"reuse", static_cast<int>(QueryCacheMode::Reuse), false},
        {nullptr, 0, false},
    };
    DefineCustomEnumVariable(
        "bioseq.query_cache_optimisation_mode",
        "Controls exact request locking and reusable query-result fragments.",
        nullptr,
        &g_query_cache_mode,
        static_cast<int>(QueryCacheMode::Disabled),
        mode_options,
        PGC_USERSET,
        0,
        check_query_cache_mode,
        assign_query_cache_mode,
        nullptr);
    DefineCustomIntVariable(
        "bioseq.query_cache_ttl",
        "Maximum age of a completed bioseq_cache result.",
        nullptr,
        &g_query_cache_ttl_ms,
        60 * 60 * 1000,
        0,
        INT_MAX,
        PGC_USERSET,
        GUC_UNIT_MS,
        nullptr,
        nullptr,
        nullptr);
    MarkGUCPrefixReserved("bioseq");
    g_gucs_registered = true;
}

/** Aggregates are immutable only when every executor support function is. */
[[nodiscard]] bool aggregate_has_mutable_support(const Aggref& aggregate) {
    HeapTuple tuple = SearchSysCache1(
        AGGFNOID, ObjectIdGetDatum(aggregate.aggfnoid));
    if (!HeapTupleIsValid(tuple)) {
        return true;
    }
    const auto* metadata = reinterpret_cast<const FormData_pg_aggregate*>(
        GETSTRUCT(tuple));
    const std::array<Oid, 8> functions{
        metadata->aggtransfn,
        metadata->aggfinalfn,
        metadata->aggcombinefn,
        metadata->aggserialfn,
        metadata->aggdeserialfn,
        metadata->aggmtransfn,
        metadata->aggminvtransfn,
        metadata->aggmfinalfn,
    };
    bool mutable_support = false;
    for (const Oid function_oid : functions) {
        mutable_support = mutable_support
            || (OidIsValid(function_oid)
                && func_volatile(function_oid) != PROVOLATILE_IMMUTABLE);
    }
    ReleaseSysCache(tuple);
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
            *reinterpret_cast<const Aggref*>(node))) {
        return true;
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

/** Collect only PARAM_EXTERN ids the analyzed query can actually evaluate. */
bool collect_external_parameter_walker(Node* node, void* context) {
    if (node == nullptr) {
        return false;
    }
    if (IsA(node, Param)) {
        const auto* parameter = reinterpret_cast<const Param*>(node);
        if (parameter->paramkind == PARAM_EXTERN && parameter->paramid > 0) {
            auto** parameters = static_cast<Bitmapset**>(context);
            *parameters = bms_add_member(
                *parameters, static_cast<int>(parameter->paramid));
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

[[nodiscard]] List* collect_external_parameter_ids(Query& query) {
    Bitmapset* parameters = nullptr;
    static_cast<void>(query_tree_walker(
        &query, collect_external_parameter_walker, &parameters,
        QTW_EXAMINE_RTES_BEFORE));
    List* result = NIL;
    int parameter_id = -1;
    while ((parameter_id = bms_next_member(parameters, parameter_id)) >= 0) {
        result = lappend(result, makeInteger(parameter_id));
    }
    bms_free(parameters);
    return result;
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

struct BioseqCacheState {
    CustomScanState css;
    bool child_initialized;
    bool capture_failed;
    bool producer_completed;
    bool manifest_pin_owned;
    bool manifest_waiter_owned;
    bool registry_reader_owned;
    RuntimeRole runtime_role;
    int registry_index;
    std::uint64_t generation;
    std::uint64_t row_count;
    std::uint64_t payload_bytes;
    std::uint64_t logical_bytes;
    RegistryReaderClaim registry_reader_claim;
    CacheEntryId entry_id;
    storage::ManifestHandle manifest_handle;
    storage::ManifestWaiterToken manifest_waiter_token;
    storage::ManifestPin manifest_pin;
    std::array<storage::StoredBlockToken,
        storage::kBlocksPerManifest> stored_tokens;
    std::uint32_t stored_token_count;
    ColumnStoreBuilder* column_builder;
    Oid* send_functions;
    Tuplestorestate* replay_store;
    TupleTableSlot* replay_slot;
    MemoryContextCallback cleanup_callback;
    bool cleanup_registered;
    bool cleanup_done;
};

static_assert(std::is_standard_layout_v<BioseqCacheState>);
static_assert(offsetof(BioseqCacheState, css) == 0);

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
    reinterpret_cast<Integer*>(marker)->ival = force_bypass ? 1 : 0;
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
    if (size > kMaximumKeyBytes
        || static_cast<std::size_t>(key->len) + sizeof(std::uint32_t) + size
            > kMaximumKeyBytes) {
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
    delete state.column_builder;
    state.column_builder = nullptr;
    if (state.send_functions != nullptr) {
        pfree(state.send_functions);
        state.send_functions = nullptr;
    }
}

/** Serialize one external parameter with type and NULL identity. */
[[nodiscard]] bool append_parameter_key(
        StringInfo key, const ParamExternData& parameter) {
    if (!append_key_scalar(key, parameter.ptype)
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
    const std::uint32_t key_format = 2;
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

    const std::uint64_t coarse_epoch = pg_atomic_read_u64(
        &g_shared->coarse_dependency_epoch);
    List* referenced_relations = scan.custom_private != NIL
            && list_length(scan.custom_private) > 4
        ? static_cast<List*>(list_nth(scan.custom_private, 4))
        : NIL;
    const int relation_count = list_length(referenced_relations);
    if (relation_count > static_cast<int>(kMaximumRelationDependencies)
        || !append_key_scalar(key, coarse_epoch)
        || !append_key_scalar(key, relation_count)) {
        pfree(key->data);
        pfree(key);
        return nullptr;
    }
    ListCell* relation_cell = nullptr;
    foreach(relation_cell, referenced_relations) {
        const Oid relation_oid = lfirst_oid(relation_cell);
        const std::uint64_t relation_epoch = relation_dependency_epoch(
            relation_oid);
        if (!OidIsValid(relation_oid)
            || !append_key_scalar(key, relation_oid)
            || !append_key_scalar(key, relation_epoch)) {
            pfree(key->data);
            pfree(key);
            return nullptr;
        }
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
        const int parameter_id = intVal(lfirst(parameter_cell));
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
                parameters, parameter_id, false, &workspace)
            : &parameters->params[parameter_id - 1];
        if (parameter == nullptr || !OidIsValid(parameter->ptype)
            || !append_parameter_key(key, *parameter)) {
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

#if defined(BIOSEQ_CACHE_TESTING)
[[nodiscard]] constexpr std::uint32_t pause_bit(
        TestPausePoint point) noexcept {
    return 1U << static_cast<std::uint32_t>(point);
}

[[nodiscard]] std::int64_t pause_lock_key(TestPausePoint point) noexcept {
    const std::uint64_t database = static_cast<std::uint64_t>(MyDatabaseId);
    const std::uint64_t ordinal = static_cast<std::uint32_t>(point) + 1U;
    return static_cast<std::int64_t>(
        0x4253514300000000ULL ^ (database << 8U) ^ ordinal);
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
    ereport(ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("unknown bioseq_cache pause point: %s", name)));
    pg_unreachable();
}

/** Deterministic test pause compiled out of production builds. */
void maybe_pause(TestPausePoint point) {
    if (g_shared == nullptr) {
        return;
    }
    CacheTestControl& control = g_shared->test_control;
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
};
inline void maybe_pause(TestPausePoint) noexcept {}
#endif

struct RegistryDecision {
    RuntimeRole role = RuntimeRole::Bypass;
    int index = -1;
    std::uint64_t generation = 0;
    RegistryReaderClaim reader_claim{};
    CacheEntryId entry_id{};
    storage::ManifestHandle manifest_handle{};
    storage::ManifestWaiterToken manifest_waiter_token{};
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

[[nodiscard]] std::uint64_t next_claim_sequence() noexcept {
    std::uint64_t sequence = pg_atomic_fetch_add_u64(
        &g_shared->next_claim_sequence, 1);
    if (sequence == 0) {
        sequence = pg_atomic_fetch_add_u64(
            &g_shared->next_claim_sequence, 1);
    }
    return sequence == 0 ? 1 : sequence;
}

[[nodiscard]] bool registry_owner_alive(
        int owner_pid,
        std::uint32_t owner_proc_number) noexcept {
    if (owner_pid <= 0
        || owner_proc_number
            >= static_cast<std::uint32_t>(ProcGlobal->allProcCount)) {
        return false;
    }
    PGPROC* owner = ProcNumberGetProc(
        static_cast<ProcNumber>(owner_proc_number));
    return owner != nullptr && owner->pid == owner_pid;
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

void reap_dead_registry_readers_locked(CacheEntry& entry) noexcept {
    for (RegistryReaderOwner& reader : entry.readers) {
        if (reader.active
            && !registry_owner_alive(
                reader.owner_pid, reader.owner_proc_number)) {
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
        reader = RegistryReaderOwner{
            .active = true,
            .owner_pid = MyProcPid,
            .owner_proc_number = static_cast<std::uint32_t>(MyProcNumber),
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
        bool storage_attached) noexcept {
    for (RegistryWaiterOwner& waiter : entry.waiters) {
        if (!waiter.active
            || registry_owner_alive(
                waiter.owner_pid, waiter.owner_proc_number)) {
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
        waiter = RegistryWaiterOwner{
            .active = true,
            .owner_pid = MyProcPid,
            .owner_proc_number = static_cast<std::uint32_t>(MyProcNumber),
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
        || waiter.owner_pid != MyProcPid
        || waiter.owner_proc_number
            != static_cast<std::uint32_t>(MyProcNumber)) {
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
    entry.database_oid = InvalidOid;
    entry.current_role_oid = InvalidOid;
    entry.session_user_oid = InvalidOid;
    entry.producer_pid = 0;
    entry.producer_proc_number = INVALID_PROC_NUMBER;
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
    entry.reserved = 0;
    std::memset(entry.relation_oids, 0, sizeof(entry.relation_oids));
    entry.manifest_handle = {};
    std::memset(entry.display_sql, 0, sizeof(entry.display_sql));
    std::memset(entry.key, 0, sizeof(entry.key));
    entry.generation = generation;
    pg_write_barrier();
    entry.state = SharedEntryState::Empty;
    ConditionVariableBroadcast(&entry.changed);
}

/** Wait for a matching producer; pre-existing waiters may consume TTL-zero. */
[[nodiscard]] RegistryDecision wait_for_producer(
        int index, std::uint64_t generation,
        const RegistryWaiterClaim& waiter_claim) {
    RegistryDecision decision{};
    CacheEntry& entry = g_shared->entries[index];
    storage::ManifestWaiterToken waiter_to_release{};
    ConditionVariablePrepareToSleep(&entry.changed);
    PG_TRY();
    {
        maybe_pause(TestPausePoint::AfterWaiterRegistered);
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
            if (entry.generation != generation
                || entry.state == SharedEntryState::Failed
                || entry.state == SharedEntryState::Retired
                || entry.state == SharedEntryState::Empty) {
                if (entry.generation == generation) {
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
                        decision.generation = generation;
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
            PGPROC* producer = entry.producer_proc_number == INVALID_PROC_NUMBER
                ? nullptr
                : ProcNumberGetProc(entry.producer_proc_number);
            if (entry.state == SharedEntryState::Producing
                && (producer == nullptr || producer->pid != entry.producer_pid)) {
                static_cast<void>(take_registry_waiter_locked(
                    entry, waiter_claim, waiter_to_release));
                fail_abandoned_producer(entry);
                LWLockRelease(g_registry_lock);
                break;
            }
            LWLockRelease(g_registry_lock);
            static_cast<void>(ConditionVariableTimedSleep(
                &entry.changed, 1000L, PG_WAIT_EXTENSION));
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        if (entry.generation == generation) {
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
[[nodiscard]] RegistryDecision acquire_registry_entry(
        const StringInfo key, std::uint64_t database_epoch,
        const char* display_sql, List* relation_oids) {
    RegistryDecision decision{};
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
        reap_dead_registry_readers_locked(entry);
        reap_dead_registry_waiters_locked(entry, true);
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
            if (entry.state == SharedEntryState::Producing
                && entry.database_epoch == database_epoch) {
                const RegistryWaiterClaim waiter_claim =
                    claim_registry_waiter_locked(entry);
                if (!waiter_claim) {
                    LWLockRelease(g_registry_lock);
                    return decision;
                }
                const std::uint64_t generation = entry.generation;
                LWLockRelease(g_registry_lock);
                if (g_shared != nullptr) {
                    pg_atomic_fetch_add_u64(
                        &g_shared->stats.inflight_waits, 1);
                }
                return wait_for_producer(
                    static_cast<int>(offset), generation, waiter_claim);
            }
        }

        const bool unreferenced = entry.reader_count == 0
            && entry.waiter_count == 0;
        if (!unreferenced || entry.state == SharedEntryState::Producing) {
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
        entry.state = SharedEntryState::Producing;
        entry.key_size = static_cast<std::uint32_t>(key->len);
        entry.generation = next_generation == 0 ? 1 : next_generation;
        entry.claim_sequence = next_claim_sequence();
        entry.key_hash = key_hash;
        entry.key_digest = *digest;
        entry.database_epoch = database_epoch;
        entry.database_oid = MyDatabaseId;
        entry.current_role_oid = GetUserId();
        entry.session_user_oid = GetSessionUserId();
        entry.producer_pid = MyProcPid;
        entry.producer_proc_number = MyProcNumber;
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
        entry.reserved = 0;
        std::memset(entry.relation_oids, 0, sizeof(entry.relation_oids));
        ListCell* relation_cell = nullptr;
        foreach(relation_cell, relation_oids) {
            if (entry.relation_count >= kMaximumRelationDependencies) {
                break;
            }
            entry.relation_oids[entry.relation_count++] =
                lfirst_oid(relation_cell);
        }
        entry.manifest_handle = {};
        strlcpy(
            entry.display_sql,
            display_sql == nullptr ? "<query text unavailable>" : display_sql,
            sizeof(entry.display_sql));
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

/** Release a short reader pin after full preflight/materialization. */
void release_registry_reader(
        int index, std::uint64_t generation,
        const RegistryReaderClaim& reader_claim,
        bool successful,
        bool invalidate_failure = true,
        bool count_outcome = true) noexcept {
    if (g_shared == nullptr || index < 0 || !reader_claim) {
        return;
    }
    storage::ManifestHandle retired_manifest{};
    bool released = false;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[index];
    if (entry.generation == generation) {
        RegistryReaderOwner& reader = entry.readers[reader_claim.slot];
        if (reader.active
            && reader.registration_sequence
                == reader_claim.registration_sequence
            && reader.owner_pid == MyProcPid
            && reader.owner_proc_number
                == static_cast<std::uint32_t>(MyProcNumber)) {
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

/** Fail a generation only when this backend still owns the producer token. */
void fail_registry_producer(BioseqCacheState& state) noexcept {
    storage::ManifestHandle retired_manifest{};
    if (g_shared != nullptr && state.registry_index >= 0) {
        LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
        CacheEntry& entry = g_shared->entries[state.registry_index];
        if (entry.generation == state.generation
            && entry.state == SharedEntryState::Producing
            && entry.producer_pid == MyProcPid) {
            retired_manifest = entry.manifest_handle;
            entry.manifest_handle = {};
            fail_abandoned_producer(entry);
        }
        LWLockRelease(g_registry_lock);
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
    if (state == nullptr || state->cleanup_done) {
        return;
    }
    state->cleanup_done = true;
    release_storage_reader_claims(*state);
    if (state->registry_reader_owned) {
        release_registry_reader(
            state->registry_index,
            state->generation,
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
}

/** Prepare one generation-private self-validating column-store capture. */
[[nodiscard]] bool begin_producer_capture(
        BioseqCacheState& state, TupleDesc descriptor) {
    try {
        state.send_functions = static_cast<Oid*>(
            palloc0(sizeof(Oid) * static_cast<std::size_t>(descriptor->natts)));
        std::vector<ColumnSchema> schema = build_column_schema(
            descriptor, state.send_functions);
        state.column_builder = new ColumnStoreBuilder(
            current_column_store_identity(), schema);
        return true;
    } catch (...) {
        destroy_column_builder(state);
        return false;
    }
}

/** Capture one projected row through PostgreSQL's lossless binary send ABI. */
[[nodiscard]] bool capture_projected_row(
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

/** Publish immutable storage blocks only if no commit raced their capture. */
[[nodiscard]] bool complete_registry_producer(
        BioseqCacheState& state, TupleDesc descriptor) {
    static_cast<void>(descriptor);
    if (state.column_builder == nullptr
        || state.column_builder->row_count() != state.row_count) {
        return false;
    }
    std::vector<std::byte> sealed;
    try {
        PostgreSqlColumnChecksumPolicy policy;
        auto result = state.column_builder->seal(policy);
        if (!result.has_value()) {
            return false;
        }
        sealed = std::move(*result);
    } catch (...) {
        return false;
    }
    if (sealed.size() > kMaximumStoredResultBytes) {
        return false;
    }
#if defined(BIOSEQ_CACHE_TESTING)
    if (g_shared != nullptr
        && pg_atomic_exchange_u32(
            &g_shared->test_control.fail_next_write, 0) != 0) {
        return false;
    }
#endif
    state.payload_bytes = sealed.size();
    if (sealed.empty()) {
        return false;
    }
    for (std::size_t offset = 0; offset < sealed.size();) {
        if (state.stored_token_count >= storage::kBlocksPerManifest) {
            discard_unpublished_tokens(state);
            return false;
        }
        const std::size_t remaining = sealed.size() - offset;
        const std::size_t block_bytes = remaining
                < static_cast<std::size_t>(storage::kDefaultMaximumBlockBytes)
            ? remaining
            : static_cast<std::size_t>(storage::kDefaultMaximumBlockBytes);
        auto stored = storage::store_block(std::span<const std::byte>{
            sealed.data() + offset, block_bytes});
        if (!stored) {
            discard_unpublished_tokens(state);
            return false;
        }
        state.stored_tokens[state.stored_token_count++] = stored.token;
        offset += block_bytes;
    }
    destroy_column_builder(state);
    maybe_pause(TestPausePoint::BeforeTerminalPublish);

    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    CacheEntry& entry = g_shared->entries[state.registry_index];
    const std::uint64_t current_epoch = pg_atomic_read_u64(
        &g_shared->database_write_epoch);
    const bool stable_visibility = pg_atomic_read_u32(
        &g_shared->database_transition_count) == 0;
    reap_dead_registry_waiters_locked(entry, true);
    if (entry.generation == state.generation
        && entry.state == SharedEntryState::Producing
        && entry.producer_pid == MyProcPid
        && entry.claim_sequence == state.entry_id.claim_sequence
        && entry.database_epoch == current_epoch
        && stable_visibility) {
        const TimestampTz completed_at = GetCurrentTimestamp();
        const std::int64_t ttl_microseconds =
            static_cast<std::int64_t>(g_query_cache_ttl_ms) * 1000;
        storage::ManifestDescriptor manifest{
            .id = state.entry_id,
            .kind = CacheEntryKind::ExactFinalResult,
            .logical_bytes = state.logical_bytes,
            .row_count = state.row_count,
            .created_at_us = entry.created_at,
            .expires_at_us = completed_at + ttl_microseconds,
        };
        auto publication = storage::publish_manifest(
            manifest,
            std::span<const storage::StoredBlockToken>{
                state.stored_tokens.data(), state.stored_token_count});
        if (!publication) {
            fail_abandoned_producer(entry);
            LWLockRelease(g_registry_lock);
            discard_unpublished_tokens(state);
            return false;
        }
        state.stored_token_count = 0;
        bool waiters_registered = true;
        for (RegistryWaiterOwner& waiter : entry.waiters) {
            if (!waiter.active) {
                continue;
            }
            auto registered = storage::register_manifest_waiter(
                publication.handle,
                waiter.owner_pid,
                waiter.owner_proc_number,
                waiter.registered_at);
            if (registered) {
                waiter.storage_token = registered.token;
                continue;
            }
            if (!registry_owner_alive(
                    waiter.owner_pid, waiter.owner_proc_number)) {
                clear_registry_waiter_locked(entry, waiter);
                continue;
            }
            waiters_registered = false;
            break;
        }
        if (!waiters_registered) {
            for (RegistryWaiterOwner& waiter : entry.waiters) {
                if (waiter.storage_token) {
                    static_cast<void>(storage::unregister_manifest_waiter(
                        waiter.storage_token));
                    waiter.storage_token = {};
                }
            }
            static_cast<void>(storage::retire_manifest(publication.handle));
            fail_abandoned_producer(entry);
            LWLockRelease(g_registry_lock);
            return false;
        }
        state.manifest_handle = publication.handle;
        entry.state = SharedEntryState::Complete;
        entry.producer_pid = 0;
        entry.producer_proc_number = INVALID_PROC_NUMBER;
        entry.completed_at = completed_at;
        entry.expires_at = completed_at + ttl_microseconds;
        entry.row_count = state.row_count;
        entry.logical_bytes = state.logical_bytes;
        entry.stored_bytes = state.payload_bytes;
        entry.manifest_handle = publication.handle;
        pg_atomic_fetch_add_u64(&g_shared->stats.produced, 1);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.logical_bytes, state.logical_bytes);
        pg_atomic_fetch_add_u64(
            &g_shared->stats.stored_bytes, entry.stored_bytes);
        state.producer_completed = true;
        ConditionVariableBroadcast(&entry.changed);
    } else if (entry.generation == state.generation
            && entry.state == SharedEntryState::Producing) {
        fail_abandoned_producer(entry);
    }
    const bool published = state.producer_completed;
    LWLockRelease(g_registry_lock);
    if (!published) {
        discard_unpublished_tokens(state);
    }
    return published;
}

/** Materialize and validate an entire shared result before exposing row one. */
[[nodiscard]] Tuplestorestate* load_complete_result(
        const storage::ManifestPin& pin, TupleDesc expected_descriptor,
        CacheEntryKind expected_kind = CacheEntryKind::ExactFinalResult) {
    std::vector<ColumnSchema> expected_schema;
    try {
        expected_schema = build_column_schema(expected_descriptor, nullptr);
    } catch (...) {
        return nullptr;
    }
    if (!pin || pin.kind != expected_kind
        || pin.block_count > storage::kBlocksPerManifest) {
        return nullptr;
    }
    std::uint64_t total_bytes = 0;
    for (std::uint32_t index = 0; index < pin.block_count; ++index) {
        const std::uint64_t length = pin.blocks[index].length;
        if (length == 0 || length > kMaximumStoredResultBytes
            || total_bytes > kMaximumStoredResultBytes - length) {
            return nullptr;
        }
        total_bytes += length;
    }
    if (total_bytes == 0 || total_bytes > kMaximumStoredResultBytes) {
        return nullptr;
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
            return nullptr;
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
            return nullptr;
        }
        view = new ColumnStoreView(std::move(*validated));
    } catch (...) {
        pfree(store);
        return nullptr;
    }
    if (view->column_count()
            != static_cast<std::size_t>(expected_descriptor->natts)
        || view->row_count() != pin.row_count) {
        delete view;
        pfree(store);
        return nullptr;
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
            delete view;
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
    delete view;
    pfree(store);
    if (!valid) {
        tuplestore_end(result);
        return nullptr;
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

/** Decide producer/consumer/bypass before touching the retained child. */
void cache_begin_custom_scan(CustomScanState* node, EState* estate, int eflags) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    auto* scan = reinterpret_cast<CustomScan*>(node->ss.ps.plan);
    state->runtime_role = RuntimeRole::Bypass;
    state->registry_index = -1;
    state->replay_store = nullptr;
    state->replay_slot = nullptr;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->capture_failed = false;
    state->producer_completed = false;
    state->manifest_pin_owned = false;
    state->manifest_waiter_owned = false;
    state->registry_reader_owned = false;
    state->registry_reader_claim = {};
    state->entry_id = {};
    state->manifest_handle = {};
    state->manifest_waiter_token = {};
    state->manifest_pin = {};
    state->stored_tokens = {};
    state->stored_token_count = 0;
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

    const bool explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
    const bool force_bypass = private_force_bypass(*scan);
    const VisibilityTicket visibility = explain_only || force_bypass
        ? VisibilityTicket{}
        : mint_visibility_ticket();
    const bool cache_allowed = !force_bypass && visibility.valid
        && g_query_cache_mode != static_cast<int>(QueryCacheMode::Disabled)
        && XactIsoLevel == XACT_READ_COMMITTED
        && !transaction_is_dirty()
        && node->ss.ps.ps_ResultTupleDesc != nullptr;
    if (cache_allowed) {
        StringInfo key = nullptr;
        MemoryContext caller_context = CurrentMemoryContext;
        PG_TRY();
        {
            validate_binary_schema(node->ss.ps.ps_ResultTupleDesc);
            key = build_runtime_key(
                *scan, estate, node->ss.ps.ps_ResultTupleDesc);
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
            RegistryDecision decision = acquire_registry_entry(
                key, visibility.database_epoch, private_string(*scan, 1),
                referenced_relations);
            state->runtime_role = decision.role;
            state->registry_index = decision.index;
            state->generation = decision.generation;
            state->registry_reader_claim = decision.reader_claim;
            state->entry_id = decision.entry_id;
            state->manifest_handle = decision.manifest_handle;
            state->manifest_waiter_token =
                decision.manifest_waiter_token;
            state->manifest_waiter_owned =
                static_cast<bool>(decision.manifest_waiter_token);
            state->registry_reader_owned =
                static_cast<bool>(decision.reader_claim);
        }
        if (key != nullptr) {
            pfree(key->data);
            pfree(key);
        }

        if (state->runtime_role == RuntimeRole::Consumer) {
            MemoryContext load_context = CurrentMemoryContext;
            PG_TRY();
            {
                storage::PinManifestResult pinned =
                    state->manifest_waiter_owned
                    ? storage::convert_waiter_to_pin(
                        state->manifest_waiter_token)
                    : storage::pin_manifest(state->entry_id);
                if (pinned) {
                    state->manifest_waiter_owned = false;
                    state->manifest_waiter_token = {};
                    state->manifest_pin = pinned.pin;
                    state->manifest_pin_owned = true;
                    maybe_pause(TestPausePoint::AfterReaderPinned);
                    state->replay_store = load_complete_result(
                        state->manifest_pin,
                        node->ss.ps.ps_ResultTupleDesc);
                }
                if (state->replay_store != nullptr) {
                    state->replay_slot = MakeSingleTupleTableSlot(
                        node->ss.ps.ps_ResultTupleDesc,
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
                    if (state->registry_reader_owned) {
                        release_registry_reader(
                            state->registry_index,
                            state->generation,
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
            release_registry_reader(
                state->registry_index,
                state->generation,
                state->registry_reader_claim,
                loaded);
            state->registry_reader_owned = false;
            state->registry_reader_claim = {};
            if (loaded) {
                return;
            }
            state->runtime_role = RuntimeRole::Bypass;
        }

        if (state->runtime_role == RuntimeRole::Producer
            ) {
            maybe_pause(TestPausePoint::AfterBuildingPublish);
            if (!begin_producer_capture(
                    *state, node->ss.ps.ps_ResultTupleDesc)) {
                fail_registry_producer(*state);
            }
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
        && g_shared != nullptr) {
        pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
    }
}

/** Execute the retained child and explicitly project its raw slot. */
TupleTableSlot* cache_exec_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<BioseqCacheState*>(node);
    if (state->runtime_role == RuntimeRole::Consumer) {
        TupleTableSlot* slot = state->replay_slot;
        ExecClearTuple(slot);
        static_cast<void>(tuplestore_gettupleslot(
            state->replay_store, true, false, slot));
        return slot;
    }
    PlanState* child = retained_child(node);
    if (child == nullptr) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    TupleTableSlot* raw_slot = ExecProcNode(child);
    if (TupIsNull(raw_slot)) {
        if (state->runtime_role == RuntimeRole::Producer) {
            MemoryContext caller_context = CurrentMemoryContext;
            bool completed = false;
            PG_TRY();
            {
                completed = complete_registry_producer(
                    *state, node->ss.ps.ps_ResultTupleDesc);
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
    ExprContext* expression_context = node->ss.ps.ps_ExprContext;
    expression_context->ecxt_scantuple = raw_slot;
    TupleTableSlot* result = node->ss.ps.ps_ProjInfo == nullptr
        ? raw_slot
        : ExecProject(node->ss.ps.ps_ProjInfo);
    if (state->runtime_role == RuntimeRole::Producer) {
        MemoryContext caller_context = CurrentMemoryContext;
        bool captured = false;
        PG_TRY();
        {
            captured = capture_projected_row(*state, result);
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
    return result;
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
    const QueryCacheMode mode = static_cast<QueryCacheMode>(g_query_cache_mode);
    const char* mode_name = mode == QueryCacheMode::Reuse
        ? "reuse"
        : mode == QueryCacheMode::Same ? "same" : "disabled";
    const bool force_bypass = private_force_bypass(*scan);
    const int parameter_count = private_list_length(*scan, 3, T_List);
    const int dependency_count = private_list_length(*scan, 4, T_OidList);
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
            "unsafe finalized source path",
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
            : psprintf("%d relation%s", dependency_count,
                dependency_count == 1 ? "" : "s");
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
    ExplainPropertyText(
        "Cache Janitor",
        worker_lifecycle_name(storage_snapshot.worker_lifecycle),
        explain_state);
}

const CustomExecMethods& cache_exec_methods() {
    static const CustomExecMethods methods = {
        .CustomName = "BioseqCacheScan",
        .BeginCustomScan = cache_begin_custom_scan,
        .ExecCustomScan = cache_exec_custom_scan,
        .EndCustomScan = cache_end_custom_scan,
        .ReScanCustomScan = cache_rescan_custom_scan,
        .ExplainCustomScan = cache_explain_custom_scan,
    };
    return methods;
}

/** Allocate the standard-layout executor state. */
Node* cache_create_custom_scan_state(CustomScan*) {
    auto* state = reinterpret_cast<BioseqCacheState*>(
        newNode(sizeof(BioseqCacheState), T_CustomScanState));
    state->css.methods = &cache_exec_methods();
    state->child_initialized = false;
    state->runtime_role = RuntimeRole::Bypass;
    state->registry_index = -1;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->replay_slot = nullptr;
    return reinterpret_cast<Node*>(&state->css);
}

const CustomScanMethods& cache_scan_methods() {
    static const CustomScanMethods methods = {
        .CustomName = "BioseqCacheScan",
        .CreateCustomScanState = cache_create_custom_scan_state,
    };
    return methods;
}

/** Convert one pre-setrefs outer CustomPath into a CustomScan. */
Plan* cache_plan_custom_path(
        PlannerInfo*, RelOptInfo* relation, CustomPath* best_path,
        List* target_list, List*, List* custom_plans) {
    if (custom_plans == NIL || list_length(custom_plans) != 1) {
        return nullptr;
    }
    auto* scan = makeNode(CustomScan);
    scan->methods = &cache_scan_methods();
    scan->flags = best_path->flags;
    scan->scan.scanrelid = 0;
    scan->scan.plan.targetlist = target_list;
    scan->scan.plan.qual = NIL;
    scan->custom_plans = custom_plans;
    scan->custom_private = static_cast<List*>(copyObject(best_path->custom_private));
    scan->custom_exprs = NIL;
    Plan* child = static_cast<Plan*>(linitial(custom_plans));
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
    PlannerDecodedCoverageV1 contract;
    TupleDesc stored_descriptor;
    CoverageStoredColumnV1* stored_columns;
    std::uint32_t stored_column_count;
    std::uint32_t boundary_map_count;
    std::uint32_t lineage_map_count;
    std::uint32_t reserved_count;
    BioseqCacheColumnMapEntryV1* boundary_map;
    BioseqCacheColumnMapEntryV1* lineage_map;
    List* provider_private_nodes;
    const void* generation_key_node;
    const void* descriptor_node;
    std::uint64_t row_count;
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
    bool older_preflight_ready;
    std::array<std::uint8_t, 1> reserved_flags;
    std::uint16_t source_count;
    std::uint16_t active_source_count;
    std::uint16_t replay_source_index;
    std::uint16_t reserved;
    CoverageRuntimeRoleV1 runtime_role;
    std::array<std::uint8_t, 7> reserved_role;
    const provider_registry::ProviderV1* provider;
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
    TimestampTz producer_started_at;
    ColumnStoreBuilder* column_builder;
    Oid* send_functions;
    TupleDesc stored_descriptor;
    CoverageStoredColumnV1* stored_columns;
    std::uint32_t stored_column_count;
    std::uint32_t reserved_stored;
    List* plan_domain_nodes;
    Tuplestorestate* current_replay_store;
    TupleTableSlot* current_replay_slot;
    ExprState* requested_predicate_state;
    std::array<CoverageRuntimeSourceV1, kMaxSourcesPerCoverageNode> sources;
    MemoryContextCallback cleanup_callback;
};

static_assert(std::is_standard_layout_v<ReusableCoverageState>);
static_assert(offsetof(ReusableCoverageState, css) == 0);

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
        const BioseqCachePathDomainV1& contract) noexcept;

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

[[nodiscard]] const BioseqCacheColumnSchemaV1* coverage_contract_column(
        const BioseqCachePathDomainV1& contract,
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
        const BioseqCachePathDomainV1& contract,
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
        const BioseqCachePathDomainV1& contract,
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
    const auto append = [&](const void* source, std::size_t size) {
        std::memcpy(bytes.data() + offset, source, size);
        offset += size;
    };
    append(&attno, sizeof(attno));
    const auto append_matching = [&](std::uint32_t role,
            const BioseqCacheColumnSchemaV1* columns,
            std::size_t count) {
        for (std::size_t index = 0; index < count; ++index) {
            if (columns[index].attno == attno) {
                const std::uint32_t normalized_struct_size =
                    sizeof(BioseqCacheColumnSchemaV1);
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
        const BioseqCachePathDomainV1& contract) noexcept {
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
        const auto append_column = [&](std::vector<std::byte>& output,
                std::uint32_t role, std::uint32_t ordinal,
                const BioseqCacheColumnSchemaV1& column) {
            const std::uint32_t normalized_struct_size =
                sizeof(BioseqCacheColumnSchemaV1);
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
        const auto append_attnos = [&](const std::uint32_t* values,
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
        const BioseqCachePathDomainV1& contract,
        CoverageStoredRecipeV1* output) {
    if (output == nullptr
        || !reuse::validate_path_domain(
            contract.provider_id, &contract).has_value()) {
        return false;
    }
    std::vector<std::uint32_t> attnos;
    attnos.reserve(
        contract.boundary_column_count + contract.lineage_column_count);
    const auto append_list = [&](const std::uint32_t* values,
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
    for (std::size_t index = 0;
         index < contract.lineage_column_count; ++index) {
        if (!append_unique_coverage_attno(
                attnos, contract, contract.lineage_columns[index].attno)) {
            return false;
        }
    }
    for (std::size_t index = 0;
         index < contract.boundary_column_count; ++index) {
        if (!append_unique_coverage_attno(
                attnos, contract, contract.boundary_columns[index].attno)) {
            return false;
        }
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
        const BioseqCacheColumnSchemaV1* schema =
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
    delete state.column_builder;
    state.column_builder = nullptr;
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
    if (token.active && g_reusable_registry) {
        static_cast<void>(release_reusable_reference(
            g_reusable_registry,
            token,
            reusable_partition_callbacks()));
    }
}

void release_coverage_source(CoverageRuntimeSourceV1& source) noexcept {
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
    source.stored_column_count = 0;
    source.boundary_map_count = 0;
    source.lineage_map_count = 0;
    source.provider_private_nodes = NIL;
    source.generation_key_node = nullptr;
    source.descriptor_node = nullptr;
    source.completed_snapshot = {};
    source.row_count = 0;
    source.waited = false;
    source.ttl_entitlement_consumed = false;
    source.valid = false;
}

void fail_coverage_producer(ReusableCoverageState& state) noexcept {
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
    state.runtime_role = CoverageRuntimeRoleV1::Bypass;
}

void coverage_state_cleanup(void* opaque) noexcept {
    auto* state = static_cast<ReusableCoverageState*>(opaque);
    if (state == nullptr || state->cleanup_done) {
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
    if (state->current_replay_store != nullptr) {
        tuplestore_end(state->current_replay_store);
        state->current_replay_store = nullptr;
    }
    if (state->current_replay_slot != nullptr) {
        ExecDropSingleTupleTableSlot(state->current_replay_slot);
        state->current_replay_slot = nullptr;
    }
    if (state->current_producer_owned && !state->producer_completed) {
        fail_coverage_producer(*state);
    }
    discard_coverage_blocks(*state);
    destroy_coverage_builder(*state);
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

/** Snapshot every in-flight generation produced by this backend. */
[[nodiscard]] std::size_t collect_local_reusable_claim_sequences(
        std::span<std::uint64_t> output) noexcept {
    if (output.empty() || !g_reusable_registry
        || g_reusable_membership_lock == nullptr) {
        return 0;
    }
    auto* rows = static_cast<ReusableGenerationDiagnosticsRowV1*>(palloc0(
        sizeof(ReusableGenerationDiagnosticsRowV1)
        * output.size()));
    std::size_t output_count = 0;
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
        if (diagnostics.error == ReusableRegistryErrorV1::None
            && !diagnostics.truncated) {
            const ReusableProducerIdentityV1 local =
                current_reusable_producer_identity();
            for (std::size_t index = 0;
                 index < diagnostics.count && output_count < output.size();
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
                output[output_count++] =
                    snapshot->reference.id.claim_sequence;
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
        output_count = 0;
    }
    PG_END_TRY();
    pfree(rows);
    std::sort(output.begin(), output.begin() + output_count);
    const auto unique_end = std::unique(
        output.begin(), output.begin() + output_count);
    return static_cast<std::size_t>(unique_end - output.begin());
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

/** Acquire one ABA-bound reader or waiter while membership cannot change. */
[[nodiscard]] bool acquire_older_reusable_reference(
        CoverageRuntimeSourceV1& source,
        std::span<const std::uint64_t> locally_owned_claim_sequences,
        const VisibilityTicket& visibility,
        std::uint64_t consumer_ttl_ms) noexcept {
    if (!source.reference || !g_reusable_registry
        || g_reusable_membership_lock == nullptr
        || !visibility_ticket_still_bound(visibility)) {
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
                                    != current_reusable_producer_identity()));
                    if (state_matches
                        && visibility_ticket_still_bound(visibility)) {
                        source.registry_reference = *pinned;
                        source.registry_reference_owned = true;
                        source.completed_snapshot = *rebound;
                        source.waited =
                            kind == ReusableReferenceKindV1::Waiter;
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
    return accepted;
}

/** Wait only after the ordered acquisition pass has pinned every source. */
[[nodiscard]] bool wait_for_older_reusable_generation(
        CoverageRuntimeSourceV1& source,
        std::span<const std::uint64_t> locally_owned_claim_sequences,
        const VisibilityTicket& visibility,
        std::uint64_t consumer_ttl_ms) {
    if (!source.registry_reference_owned
        || source.registry_reference.kind != ReusableReferenceKindV1::Waiter
        || source.reference.id.claim_sequence == 0
        || !visibility_ticket_still_bound(visibility)) {
        return false;
    }
    ConditionVariable* changed = reusable_generation_changed(source.reference);
    if (changed == nullptr) {
        return false;
    }
    if (!reuse::claim_order_allows_wait(
            source.reference.id.claim_sequence,
            locally_owned_claim_sequences)) {
        return false;
    }
    volatile bool accepted = false;
    ConditionVariablePrepareToSleep(changed);
    PG_TRY();
    {
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            if (!visibility_ticket_still_bound(visibility)) {
                break;
            }
            ReusableGenerationSnapshotV1 snapshot{};
            if (!snapshot_current_reusable_generation(
                    source.reference, &snapshot)
                || snapshot.reference != source.reference) {
                break;
            }
            if (snapshot.state == CacheEntryState::Complete) {
                if (snapshot.manifest
                    && older_reusable_ttl_decision(
                        snapshot,
                        consumer_ttl_ms,
                        true,
                        false).readable
                    && visibility_ticket_still_bound(visibility)) {
                    source.completed_snapshot = snapshot;
                    accepted = true;
                }
                break;
            }
            if (snapshot.state == CacheEntryState::Failed
                || snapshot.state == CacheEntryState::Invalidated
                || snapshot.state == CacheEntryState::Evicting
                || (snapshot.state != CacheEntryState::Claimed
                    && snapshot.state != CacheEntryState::Building)
                || snapshot.producer
                    == current_reusable_producer_identity()) {
                break;
            }
            if (!reusable_producer_alive(nullptr, snapshot.producer)) {
                fail_current_reusable_generation(
                    source.reference, CacheFailureCode::ProducerExited);
                continue;
            }
            ConditionVariableSleep(changed, PG_WAIT_EXTENSION);
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

/** Recheck the opaque provider payload under the same bounded node budget. */
[[nodiscard]] bool validate_provider_private_nodes_fail_open(
        const void* nodes) {
    MemoryContext caller_context = CurrentMemoryContext;
    MemoryContext disposable = AllocSetContextCreate(
        caller_context,
        "bioseq cache source private validation",
        ALLOCSET_SMALL_SIZES);
    volatile bool accepted = false;
    PG_TRY();
    {
        MemoryContextSwitchTo(disposable);
        if (nodes == nullptr) {
            accepted = true;
        } else if (IsA(nodes, List)
            && list_length(static_cast<const List*>(nodes)) >= 0
            && static_cast<std::uint32_t>(
                list_length(static_cast<const List*>(nodes)))
                <= kMaxProviderBoundaryColumns) {
            char* serialized = nodeToString(nodes);
            if (serialized != nullptr) {
                accepted = strnlen(
                    serialized,
                    static_cast<std::size_t>(kMaxProviderRecipeBytes) + 1U)
                    <= kMaxProviderRecipeBytes;
                pfree(serialized);
            }
        }
        MemoryContextSwitchTo(caller_context);
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (cache_interrupt_error(sqlstate)) {
            MemoryContextDelete(disposable);
            PG_RE_THROW();
        }
        FlushErrorState();
        accepted = false;
    }
    PG_END_TRY();
    MemoryContextDelete(disposable);
    return accepted;
}

[[nodiscard]] ProviderContractView coverage_provider_contract(
        const BioseqCachePathDomainV1& domain) noexcept {
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
        const BioseqCacheReusableSourceV1& source) noexcept {
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

/** Copy unaligned persisted entries, then rerun the generic map validator. */
[[nodiscard]] bool decode_and_validate_runtime_source_mapping(
        CoverageRuntimeSourceV1& source,
        const BioseqCachePathDomainV1& consumer,
        const void* mapping_node,
        const void* provider_private_nodes) {
    CoverageMappingViewV1 view{};
    if (!decode_source_mapping(binary_node_payload(mapping_node), &view)
        || !validate_provider_private_nodes_fail_open(
            provider_private_nodes)) {
        return false;
    }
    if (view.header.boundary_count != 0) {
        source.boundary_map = static_cast<BioseqCacheColumnMapEntryV1*>(
            palloc(sizeof(BioseqCacheColumnMapEntryV1)
                * view.header.boundary_count));
        std::memcpy(
            source.boundary_map,
            view.boundary.data(),
            view.boundary.size());
    }
    if (view.header.lineage_count != 0) {
        source.lineage_map = static_cast<BioseqCacheColumnMapEntryV1*>(
            palloc(sizeof(BioseqCacheColumnMapEntryV1)
                * view.header.lineage_count));
        std::memcpy(
            source.lineage_map,
            view.lineage.data(),
            view.lineage.size());
    }
    source.boundary_map_count = view.header.boundary_count;
    source.lineage_map_count = view.header.lineage_count;
    source.provider_private_nodes = const_cast<List*>(
        static_cast<const List*>(provider_private_nodes));
    const BioseqCacheSourceMappingV1 mapping{
        .struct_size = sizeof(BioseqCacheSourceMappingV1),
        .flags = 0,
        .boundary_map = source.boundary_map,
        .boundary_map_count = source.boundary_map_count,
        .lineage_map = source.lineage_map,
        .lineage_map_count = source.lineage_map_count,
        .provider_private_nodes = source.provider_private_nodes,
    };
    return validate_source_map(
        coverage_provider_contract(consumer),
        coverage_provider_contract(source.contract.source),
        mapping).has_value();
}

/** Reprove all shared metadata and collision bytes after executor reacquire. */
[[nodiscard]] bool older_reusable_snapshot_matches(
        const ReusableCoverageState& state,
        const CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        const ReusableGenerationSnapshotV1& snapshot) noexcept {
    if (!source.reference || snapshot.reference != source.reference
        || snapshot.state != CacheEntryState::Complete
        || !snapshot.manifest || state.provider == nullptr
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
    const CoverageDependencyTicketV1 dependency = coverage_dependency_ticket(
        source.contract.domain, state.provider);
    const ReusableGenerationMetadataV1& metadata = snapshot.metadata;
    return fingerprints.valid && dependency.valid
        && metadata.provider_id == state.provider->provider_id
        && metadata.provider_abi_version == state.provider->abi_version
        && metadata.provider_domain_abi_version
            == kCoverageDescriptorFormatV1
        && metadata.dependency_count == dependency.dependency_count
        && metadata.compatibility_family_id
            == source.contract.domain.compatibility_family_id
        && metadata.capability_mask
            == source.contract.domain.capability_mask
        && metadata.schema_fingerprint == fingerprints.schema
        && metadata.mapping_fingerprint == fingerprints.mapping
        && metadata.lineage_fingerprint == fingerprints.lineage
        && metadata.dependency_fingerprint == dependency.fingerprint
        && metadata.physical_identity == dependency.physical_identity
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
        && actual.dependency_fingerprint
            == expected_metadata.dependency_fingerprint
        && actual.physical_identity == expected_metadata.physical_identity
        && actual.observability == WriteObservability::ProviderComplete
        && reusable_bytes_equal(
            snapshot.canonical_domain_key, plan.canonical_domain_key)
        && reusable_bytes_equal(
            snapshot.canonical_generation_key,
            plan.canonical_generation_key)
        && reusable_bytes_equal(
            snapshot.coverage_descriptor, plan.coverage_descriptor);
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
        && pg_atomic_read_u32(&g_shared->database_transition_count) == 0;
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
    volatile bool accepted = false;
    ConditionVariablePrepareToSleep(changed);
    PG_TRY();
    {
        for (;;) {
            CHECK_FOR_INTERRUPTS();
            if (!visibility_ticket_still_bound(visibility)) {
                break;
            }
            ReusableGenerationSnapshotV1 snapshot{};
            if (!snapshot_current_reusable_generation(
                    state.current_reference, &snapshot)
                || snapshot.reference != state.current_reference) {
                break;
            }
            if (snapshot.state == CacheEntryState::Complete) {
                if (snapshot.manifest
                    && current_reusable_snapshot_matches(
                        snapshot, plan, expected_metadata)
                    && current_dependency_ticket_matches(state)
                    && visibility_ticket_still_bound(visibility)) {
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
            if (!reusable_producer_alive(nullptr, snapshot.producer)) {
                fail_current_reusable_generation(
                    state.current_reference,
                    CacheFailureCode::ProducerExited);
                continue;
            }
            ConditionVariableSleep(changed, PG_WAIT_EXTENSION);
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
        state.column_builder = new ColumnStoreBuilder(
            current_column_store_identity(), schema);
        return true;
    } catch (...) {
        destroy_coverage_builder(state);
        return false;
    }
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
    return state.current_reference_owned
        && convert_runtime_waiter_to_reader(state.current_reference_token);
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
    destroy_coverage_builder(state);
    state.runtime_role = CoverageRuntimeRoleV1::Bypass;
}

[[nodiscard]] bool coverage_stored_row_valid(
        const CoverageStoredColumnV1* columns,
        std::uint32_t column_count,
        std::uint64_t required_capability_mask,
        TupleTableSlot* stored,
        std::uint64_t expected_ordinal,
        BioseqCacheOccurrenceV1* occurrence = nullptr) {
    if (columns == nullptr || stored == nullptr
        || column_count < kCoverageOccurrenceColumnCountV1) {
        return false;
    }
    slot_getallattrs(stored);
    const auto scalar = [&](int attribute, Datum* value) {
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
        *occurrence = BioseqCacheOccurrenceV1{
            .struct_size = sizeof(BioseqCacheOccurrenceV1),
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
        const BioseqCacheColumnSchemaV1* semantic =
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
    for (int raw_index = 0; raw_index < raw_descriptor->natts; ++raw_index) {
        if (TupleDescAttr(raw_descriptor, raw_index)->attisdropped) {
            continue;
        }
        std::uint32_t mapping_count = 0;
        for (std::uint32_t stored_index = 0;
             stored_index < state.stored_column_count; ++stored_index) {
            const CoverageStoredColumnV1& stored =
                state.stored_columns[stored_index];
            if (stored.kind
                    == CoverageStoredColumnKindV1::ProviderAttribute
                && stored.provider_attno
                    == static_cast<std::uint32_t>(raw_index + 1)) {
                ++mapping_count;
            }
        }
        if (mapping_count != 1) {
            return false;
        }
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
    const auto validate_half = [&](
            const BioseqCacheColumnMapEntryV1* mappings,
            std::uint32_t count) {
        for (std::uint32_t index = 0; index < count; ++index) {
            const BioseqCacheColumnMapEntryV1& mapping = mappings[index];
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

/** Pin, load, checksum, map, and revalidate one completed older generation. */
[[nodiscard]] bool materialize_older_coverage_source(
        ReusableCoverageState& state,
        CoverageRuntimeSourceV1& source,
        const CoverageCurrentPlanViewV1& current_plan,
        TupleDesc raw_descriptor,
        const VisibilityTicket& visibility) {
    const ReusableGenerationSnapshotV1 completed =
        source.completed_snapshot;
    if (!visibility_ticket_still_bound(visibility)
        || !older_reusable_ttl_decision(
            completed,
            state.producer_ttl_ms,
            source.waited,
            source.ttl_entitlement_consumed).readable
        || !older_reusable_snapshot_matches(
            state, source, current_plan, completed)) {
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
    storage::PinManifestResult pinned = storage::pin_manifest(
        completed.reference.id);
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
    const bool materialized = source.replay_store != nullptr
        && source.replay_slot != nullptr
        && preflight_older_coverage_store(
            source,
            state.current_header.capability_mask,
            completed.row_count)
        && snapshot_current_reusable_generation(
            source.reference, &final_snapshot)
        && final_snapshot.reference == completed.reference
        && final_snapshot.manifest == completed.manifest
        && older_reusable_snapshot_matches(
            state, source, current_plan, final_snapshot)
        && older_reusable_ttl_decision(
            final_snapshot,
            state.producer_ttl_ms,
            source.waited,
            source.ttl_entitlement_consumed).readable
        && visibility_ticket_still_bound(visibility);
    if (!materialized) {
        return false;
    }
    if (source.registry_reference.kind == ReusableReferenceKindV1::Waiter
        && !convert_runtime_waiter_to_reader(source.registry_reference)) {
        return false;
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
    state.active_source_count = 0;
    state.older_preflight_ready = false;
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
        || !visibility_ticket_still_bound(visibility)) {
        return false;
    }

    std::array<std::size_t, kMaxSourcesPerCoverageNode> acquisition_order{};
    std::size_t acquisition_count = 0;
    std::size_t planner_index = 0;
    ListCell* cell = nullptr;
    foreach(cell, candidates) {
        CoverageRuntimeSourceV1& runtime = state.sources[planner_index];
        const auto* candidate = static_cast<const List*>(lfirst(cell));
        bool decoded = candidate != nullptr
            && IsA(candidate, List)
            && list_length(candidate) == 7
            && decode_reusable_reference_node(
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
                    CurrentMemoryContext,
                    &runtime.contract)
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
        for (std::size_t prior = 0; decoded && prior < planner_index; ++prior) {
            decoded = state.sources[prior].reference != runtime.reference;
        }
        if (decoded) {
            acquisition_order[acquisition_count++] = planner_index;
        } else {
            release_coverage_source(runtime);
        }
        ++planner_index;
    }
    if (acquisition_count == 0) {
        return false;
    }
    std::sort(
        acquisition_order.begin(),
        acquisition_order.begin() + acquisition_count,
        [&](std::size_t left, std::size_t right) {
            return reusable_reference_precedes(
                state.sources[left].reference,
                state.sources[right].reference);
        });

    if (g_reusable_wait_slot_count == 0
        || g_reusable_wait_slot_count
            > static_cast<std::size_t>(MaxAllocSize)
                / sizeof(std::uint64_t)) {
        release_all_older_coverage_sources(state);
        return false;
    }
    auto* locally_owned = static_cast<std::uint64_t*>(palloc0(
        sizeof(std::uint64_t) * g_reusable_wait_slot_count));
    const std::size_t locally_owned_count =
        collect_local_reusable_claim_sequences(
            std::span<std::uint64_t>{
                locally_owned, g_reusable_wait_slot_count});
    const auto* owned_begin = locally_owned;
    const auto* owned_end = locally_owned + locally_owned_count;
    if (locally_owned_count == 0
        || std::find(
            owned_begin,
            owned_end,
            state.current_reference.id.claim_sequence) == owned_end) {
        pfree(locally_owned);
        release_all_older_coverage_sources(state);
        return false;
    }
    const std::span<const std::uint64_t> owned_claims{
        locally_owned, locally_owned_count};
    for (std::size_t order = 0; order < acquisition_count; ++order) {
        CoverageRuntimeSourceV1& source =
            state.sources[acquisition_order[order]];
        if (!acquire_older_reusable_reference(
                source,
                owned_claims,
                visibility,
                state.producer_ttl_ms)) {
            release_coverage_source(source);
        }
    }
    for (std::size_t order = 0; order < acquisition_count; ++order) {
        CoverageRuntimeSourceV1& source =
            state.sources[acquisition_order[order]];
        if (source.registry_reference_owned
            && source.registry_reference.kind
                == ReusableReferenceKindV1::Waiter
            && !wait_for_older_reusable_generation(
                source,
                owned_claims,
                visibility,
                state.producer_ttl_ms)) {
            release_coverage_source(source);
        }
    }
    pfree(locally_owned);
    std::uint16_t active = 0;
    for (std::size_t index = 0; index < candidate_count; ++index) {
        CoverageRuntimeSourceV1& source = state.sources[index];
        if (source.registry_reference_owned
            && materialize_older_coverage_source(
                state,
                source,
                current_plan,
                raw_descriptor,
                visibility)) {
            ++active;
        } else {
            release_coverage_source(source);
        }
    }
    state.active_source_count = active;
    state.older_preflight_ready = active != 0
        && visibility_ticket_still_bound(visibility);
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
    if (completed.state != CacheEntryState::Complete
        || !completed.manifest
        || !current_reusable_snapshot_matches(
            completed, plan, expected_metadata)
        || !current_dependency_ticket_matches(state)) {
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
    storage::PinManifestResult pinned = storage::pin_manifest(
        completed.reference.id);
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
    const bool materialized = state.current_replay_store != nullptr
        && state.current_replay_slot != nullptr
        && preflight_current_coverage_store(
            state, raw_descriptor, completed.row_count)
        && snapshot_current_reusable_generation(
            state.current_reference, &final_snapshot)
        && final_snapshot.state == CacheEntryState::Complete
        && final_snapshot.reference == completed.reference
        && final_snapshot.manifest == completed.manifest
        && current_reusable_snapshot_matches(
            final_snapshot, plan, expected_metadata)
        && current_dependency_ticket_matches(state);
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

[[nodiscard]] bool capture_coverage_row(
        ReusableCoverageState& state, TupleTableSlot* slot) {
    if (state.column_builder == nullptr || state.send_functions == nullptr
        || state.stored_columns == nullptr
        || state.stored_descriptor == nullptr
        || state.stored_column_count == 0 || state.provider == nullptr
        || state.plan_domain_nodes == NIL || slot == nullptr
        || slot->tts_tupleDescriptor == nullptr) {
        return false;
    }
    BioseqCacheOccurrenceV1 occurrence{
        .struct_size = sizeof(BioseqCacheOccurrenceV1),
    };
    if (state.provider->extract_occurrence(
            state.plan_domain_nodes, slot, &occurrence)
            != BIOSEQ_CACHE_STATUS_OK
        || occurrence.struct_size < sizeof(BioseqCacheOccurrenceV1)
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
        || pg_atomic_read_u32(&g_shared->database_transition_count) != 0) {
        return false;
    }
    std::vector<std::byte> sealed;
    try {
        PostgreSqlColumnChecksumPolicy policy;
        auto result = state.column_builder->seal(policy);
        if (!result.has_value()) {
            return false;
        }
        sealed = std::move(*result);
    } catch (...) {
        return false;
    }
    if (sealed.empty() || sealed.size() > kMaximumStoredResultBytes) {
        return false;
    }
    state.payload_bytes = sealed.size();
    for (std::size_t offset = 0; offset < sealed.size();) {
        if (state.stored_token_count >= storage::kBlocksPerManifest) {
            discard_coverage_blocks(state);
            return false;
        }
        const std::size_t remaining = sealed.size() - offset;
        const std::size_t block_bytes = remaining
                < static_cast<std::size_t>(storage::kDefaultMaximumBlockBytes)
            ? remaining
            : static_cast<std::size_t>(storage::kDefaultMaximumBlockBytes);
        auto stored = storage::store_block(std::span<const std::byte>{
            sealed.data() + offset, block_bytes});
        if (!stored) {
            discard_coverage_blocks(state);
            return false;
        }
        state.stored_tokens[state.stored_token_count++] = stored.token;
        offset += block_bytes;
    }
    destroy_coverage_builder(state);
    const TimestampTz now = GetCurrentTimestamp();
    const std::int64_t ttl_us = static_cast<std::int64_t>(
        state.producer_ttl_ms) * 1000;
    storage::PublishManifestResult publication{};
    ReusableRegistryErrorV1 published =
        ReusableRegistryErrorV1::InvalidState;
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool membership_owned = false;
    volatile bool publication_error = false;
    PG_TRY();
    {
        LWLockAcquire(g_reusable_membership_lock, LW_EXCLUSIVE);
        membership_owned = true;
        const auto before_publish = wait_reusable_generation(
            g_reusable_registry,
            state.current_reference,
            reusable_partition_callbacks());
        const CoverageDependencyTicketV1 final_dependency =
            coverage_dependency_ticket(
                state.current_contract.domain, state.provider);
        if (before_publish.has_value()
            && before_publish->reference == state.current_reference
            && before_publish->state == CacheEntryState::Building
            && before_publish->producer
                == current_reusable_producer_identity()
            && final_dependency.valid
            && final_dependency.fingerprint == state.dependency_fingerprint
            && final_dependency.physical_identity
                == state.dependency_physical_identity
            && pg_atomic_read_u32(
                &g_shared->database_transition_count) == 0) {
            const bool preserve_waiter_entitlements =
                before_publish->waiter_count != 0;
            const storage::ManifestDescriptor descriptor{
                .id = state.current_reference.id,
                .kind = CacheEntryKind::ReusableCoverage,
                .logical_bytes = state.logical_bytes,
                .row_count = state.row_count,
                .created_at_us = state.producer_started_at,
                .expires_at_us = preserve_waiter_entitlements
                    ? 0
                    : now + ttl_us,
            };
            publication = storage::publish_manifest(
                descriptor,
                std::span<const storage::StoredBlockToken>{
                    state.stored_tokens.data(), state.stored_token_count});
            if (publication) {
                state.stored_token_count = 0;
                state.manifest_handle = publication.handle;
                published = publish_reusable_generation(
                    g_reusable_registry,
                    ReusablePublishRequestV1{
                        .reference = state.current_reference,
                        .manifest = reusable_manifest_handle(
                            publication.handle),
                        .completed_monotonic_ns = reusable_monotonic_now_ns(),
                        .row_count = state.row_count,
                        .logical_bytes = state.logical_bytes,
                        .stored_bytes = state.payload_bytes,
                    },
                    reusable_partition_callbacks());
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
        publication_error = true;
    }
    PG_END_TRY();
    if (publication_error || !publication
        || published != ReusableRegistryErrorV1::None) {
        if (publication) {
            static_cast<void>(storage::retire_manifest(publication.handle));
            state.manifest_handle = {};
        } else {
            discard_coverage_blocks(state);
        }
        return false;
    }
    broadcast_reusable_generation(state.current_reference);
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
};

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

/** Canonical child Plan text is diagnostics only and fails open. */
[[nodiscard]] bool capture_canonical_child_plan_fail_open(
        const Plan* child,
        BoundedDiagnosticTextV1* output) {
    if (child == nullptr || output == nullptr
        || g_producer_plan_capture_active) {
        return false;
    }
    *output = {};
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool capture_active = false;
    PG_TRY();
    {
        g_producer_plan_capture_active = true;
        capture_active = true;
        char* serialized = nodeToString(child);
        static_cast<void>(copy_bounded_diagnostic_text(
            serialized,
            kReusableCanonicalPlanCapacityV1,
            output));
        if (serialized != nullptr) {
            pfree(serialized);
        }
        g_producer_plan_capture_active = false;
        capture_active = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (capture_active) {
            g_producer_plan_capture_active = false;
            capture_active = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        *output = {};
    }
    PG_END_TRY();
    return output->available;
}

/** Render an initialized producer child without mutating the live QueryDesc. */
[[nodiscard]] bool capture_initialized_child_plan_fail_open(
        CustomScanState* node,
        EState* estate,
        const char* display_sql,
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
        || estate->es_plannedstmt == nullptr || output == nullptr
        || g_producer_plan_capture_active) {
        return false;
    }
    *output = {};
    MemoryContext caller_context = CurrentMemoryContext;
    volatile bool capture_active = false;
    PG_TRY();
    {
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

        ExplainState* explain = NewExplainState();
        explain->format = EXPLAIN_FORMAT_TEXT;
        explain->verbose = true;
        explain->costs = true;
        explain->analyze = false;
        explain->buffers = false;
        explain->wal = false;
        explain->timing = false;
        explain->summary = false;
        explain->settings = false;
        ExplainBeginOutput(explain);
        ExplainPrintPlan(explain, &view);
        ExplainEndOutput(explain);
        if (explain->str != nullptr && explain->str->data != nullptr) {
            static_cast<void>(copy_bounded_diagnostic_text(
                explain->str->data,
                kReusableProducerPlanCapacityV1,
                output));
        }
        g_producer_plan_capture_active = false;
        capture_active = false;
    }
    PG_CATCH();
    {
        const int sqlstate = geterrcode();
        MemoryContextSwitchTo(caller_context);
        if (capture_active) {
            g_producer_plan_capture_active = false;
            capture_active = false;
        }
        if (cache_interrupt_error(sqlstate)) {
            PG_RE_THROW();
        }
        FlushErrorState();
        *output = {};
    }
    PG_END_TRY();
    return output->available;
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
    state->older_preflight_ready = false;
    state->source_count = 0;
    state->active_source_count = 0;
    state->replay_source_index = 0;
    state->runtime_role = CoverageRuntimeRoleV1::Bypass;
    state->provider = nullptr;
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
    state->producer_started_at = 0;
    state->column_builder = nullptr;
    state->send_functions = nullptr;
    state->stored_descriptor = nullptr;
    state->stored_columns = nullptr;
    state->stored_column_count = 0;
    state->plan_domain_nodes = NIL;
    state->current_replay_store = nullptr;
    state->current_replay_slot = nullptr;
    state->requested_predicate_state = nullptr;
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

    const int provider_id = scan->custom_private != NIL
            && list_length(scan->custom_private) >= 2
            && IsA(list_nth(scan->custom_private, 1), Integer)
        ? intVal(list_nth(scan->custom_private, 1))
        : 0;
    const provider_registry::ProviderV1* provider = provider_id > 0
        ? provider_registry::find_provider_v1(
            static_cast<std::uint32_t>(provider_id))
        : nullptr;
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
            BioseqCachePlanDigestRecipeV1 recipe{
                .struct_size = sizeof(BioseqCachePlanDigestRecipeV1),
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
                    &recipe,
                    estate,
                    estate->es_query_cxt,
                &diagnostic);
        }
    }

    const bool diagnostic_private_valid = scan->custom_private != NIL
        && list_length(scan->custom_private) >= 10
        && IsA(list_nth(scan->custom_private, 7), String)
        && IsA(list_nth(scan->custom_private, 8), String)
        && IsA(list_nth(scan->custom_private, 9), Integer);
    const char* producer_display_sql = diagnostic_private_valid
        ? strVal(list_nth(scan->custom_private, 7))
        : "<query text unavailable>";
    const char* canonical_child_plan = diagnostic_private_valid
        ? strVal(list_nth(scan->custom_private, 8))
        : "";
    const int producer_diagnostic_flags = diagnostic_private_valid
        ? intVal(list_nth(scan->custom_private, 9))
        : 0;

    const bool explain_only = (eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0;
    const bool runtime_allowed = !explain_only
        && state->runtime_recipe_valid
        && state->plan_domain_nodes != NIL
        && provider != nullptr
        && (provider->provider_flags & BIOSEQ_CACHE_PROVIDER_REUSABLE_COVERAGE)
            != 0
        && (provider->provider_flags
                & BIOSEQ_CACHE_PROVIDER_COMPLETE_WRITE_OBSERVATION) != 0
        && provider->extract_occurrence != nullptr
        && g_query_cache_mode == static_cast<int>(QueryCacheMode::Reuse)
        && XactIsoLevel == XACT_READ_COMMITTED
        && shared_cache_ready()
        && scan->custom_private != NIL
        && diagnostic_private_valid;
    const VisibilityTicket visibility = runtime_allowed
        ? mint_visibility_ticket()
        : VisibilityTicket{};
    bool runtime_selected = false;
    if (runtime_allowed && visibility.valid) {
        CoverageCurrentPlanViewV1 current_plan{};
        const std::span<const std::byte> current_plan_bytes =
            binary_node_payload(list_nth(scan->custom_private, 2));
        if (decode_current_coverage_plan(
                current_plan_bytes, &current_plan)
            && current_plan.header.provider_id == provider->provider_id
            && current_plan.header.provider_abi_version
                == provider->abi_version
            && current_plan.header.compatibility_family_id != 0
            && current_plan.header.capability_mask != 0
            && decode_coverage_descriptor_fail_open(
                current_plan.coverage_descriptor,
                estate->es_query_cxt,
                &state->current_contract)
            && reuse::provider_security_digest(
                state->current_contract.domain)
                == current_plan.header.coverage_directory_key
                    .security_namespace.provider_security_digest
            && state->current_contract.domain.provider_id
                == current_plan.header.provider_id
            && state->current_contract.domain.compatibility_family_id
                == current_plan.header.compatibility_family_id
            && state->current_contract.domain.capability_mask
                == current_plan.header.capability_mask) {
            state->current_header = current_plan.header;
            const CoverageDependencyTicketV1 dependency =
                coverage_dependency_ticket(
                    state->current_contract.domain, provider);
            const CoverageSemanticFingerprintsV1 fingerprints =
                coverage_semantic_fingerprints(
                    state->current_contract.domain);
            if (dependency.valid && fingerprints.valid
                && pg_atomic_read_u32(
                    &g_shared->database_transition_count) == 0
                && visibility_ticket_still_bound(visibility)) {
                state->dependency_fingerprint = dependency.fingerprint;
                state->dependency_physical_identity =
                    dependency.physical_identity;
                state->schema_fingerprint = fingerprints.schema;
                state->mapping_fingerprint = fingerprints.mapping;
                state->lineage_fingerprint = fingerprints.lineage;
                const std::uint64_t ttl_ms = static_cast<std::uint64_t>(
                    g_query_cache_ttl_ms);
                state->producer_ttl_ms = ttl_ms;
                const ReusableProducerIdentityV1 owner =
                    current_reusable_producer_identity();
                const ReusableGenerationMetadataV1 metadata{
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
                const ReusableClaimResultV1 claimed =
                    visibility_ticket_still_bound(visibility)
                    ? claim_current_reusable_generation(request)
                    : ReusableClaimResultV1{};
                if (claimed) {
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
                        if (state->current_producer_owned
                            && begin_coverage_capture(*state)) {
                            PG_TRY();
                            {
                                static_cast<void>(
                                    prepare_older_coverage_sources_fail_open(
                                        *state,
                                        *scan,
                                        current_plan,
                                        node->ss.ss_ScanTupleSlot == nullptr
                                            ? nullptr
                                            : node->ss.ss_ScanTupleSlot
                                                ->tts_tupleDescriptor,
                                        visibility));
                                initialize_coverage_child(
                                    *state, node, estate, eflags);
                            }
                            PG_CATCH();
                            {
                                fail_coverage_producer(*state);
                                PG_RE_THROW();
                            }
                            PG_END_TRY();
                            BoundedDiagnosticTextV1 human_plan{};
                            static_cast<void>(
                                capture_initialized_child_plan_fail_open(
                                    node,
                                    estate,
                                    producer_display_sql,
                                    &human_plan));
                            constexpr int display_sql_truncated_flag = 1 << 0;
                            constexpr int canonical_plan_truncated_flag =
                                1 << 1;
                            const std::size_t display_sql_bytes = strnlen(
                                producer_display_sql,
                                kReusableDisplaySqlCapacityV1);
                            const std::size_t canonical_plan_bytes = strnlen(
                                canonical_child_plan,
                                kReusableCanonicalPlanCapacityV1);
                            const ReusableDiagnosticTextUpdateV1 diagnostics{
                                .reference = state->current_reference,
                                .producer = owner,
                                .display_sql = std::span<const char>{
                                    producer_display_sql,
                                    display_sql_bytes},
                                .canonical_plan = std::span<const char>{
                                    canonical_child_plan,
                                    canonical_plan_bytes},
                                .producer_plan = human_plan.available
                                    ? std::span<const char>{
                                        human_plan.data, human_plan.size}
                                    : std::span<const char>{},
                                .canonical_plan_status =
                                    canonical_plan_bytes == 0
                                    ? ReusablePlanSnapshotStatusV1::Unavailable
                                    : ReusablePlanSnapshotStatusV1::Planned,
                                .plan_status = human_plan.available
                                    ? ReusablePlanSnapshotStatusV1::Planned
                                    : ReusablePlanSnapshotStatusV1::Unavailable,
                                .display_sql_truncated =
                                    (producer_diagnostic_flags
                                        & display_sql_truncated_flag) != 0,
                                .canonical_plan_truncated =
                                    (producer_diagnostic_flags
                                        & canonical_plan_truncated_flag) != 0,
                                .producer_plan_truncated = human_plan.truncated,
                            };
                            if (begin_current_reusable_generation(diagnostics)
                                    == ReusableRegistryErrorV1::None) {
                                maybe_pause(
                                    TestPausePoint::AfterBuildingPublish);
                                runtime_selected = true;
                            } else {
                                fail_coverage_producer(*state);
                            }
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
                            complete = snapshot_current_reusable_generation(
                                    state->current_reference, &completed)
                                && completed.state
                                    == CacheEntryState::Complete
                                && current_reusable_snapshot_matches(
                                    completed, current_plan, metadata);
                        } else if (claimed.disposition
                                == ReusableClaimDispositionV1::ExistingInFlight) {
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
                                    node->ss.ss_ScanTupleSlot == nullptr
                                        ? nullptr
                                        : node->ss.ss_ScanTupleSlot
                                            ->tts_tupleDescriptor);
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
                                && visibility_ticket_still_bound(
                                    visibility)
                                && current_dependency_ticket_matches(*state)) {
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
    if (state->runtime_role == CoverageRuntimeRoleV1::CurrentConsumer) {
        TupleTableSlot* raw = next_current_coverage_row(*state, node);
        return raw == nullptr
            ? ExecClearTuple(node->ss.ps.ps_ResultTupleSlot)
            : project_coverage_tuple(node, raw);
    }
    PlanState* child = coverage_retained_child(node);
    if (child == nullptr) {
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    TupleTableSlot* raw = ExecProcNode(child);
    if (TupIsNull(raw)) {
        if (state->runtime_role == CoverageRuntimeRoleV1::Producer
            && !complete_coverage_producer_fail_open(*state)) {
            fail_coverage_producer(*state);
            if (g_shared != nullptr) {
                pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
            }
        }
        return ExecClearTuple(node->ss.ps.ps_ResultTupleSlot);
    }
    if (state->runtime_role == CoverageRuntimeRoleV1::Producer
        && !capture_coverage_row_fail_open(*state, raw)) {
        fail_coverage_producer(*state);
        if (g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
        }
    }
    return project_coverage_tuple(node, raw);
}

void coverage_end_custom_scan(CustomScanState* node) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(node);
    if (state->runtime_role == CoverageRuntimeRoleV1::Producer
        && !state->producer_completed) {
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
    if (state->runtime_role == CoverageRuntimeRoleV1::Producer) {
        fail_coverage_producer(*state);
        if (g_shared != nullptr) {
            pg_atomic_fetch_add_u64(&g_shared->stats.fallbacks, 1);
        }
    }
    if (coverage_retained_child(node) != nullptr) {
        ExecReScan(coverage_retained_child(node));
    }
}

void coverage_explain_custom_scan(
        CustomScanState* node, List*, ExplainState* explain) {
    const auto* state = reinterpret_cast<const ReusableCoverageState*>(node);
    ExplainPropertyText("Cache Entry Kind", "ReusableCoverage", explain);
    ExplainPropertyInteger(
        "Reusable Sources", nullptr, state->source_count, explain);
    ExplainPropertyBool(
        "Provider Runtime Recipe Valid",
        state->runtime_recipe_valid,
        explain);
}

const CustomExecMethods& coverage_exec_methods() {
    static const CustomExecMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .BeginCustomScan = coverage_begin_custom_scan,
        .ExecCustomScan = coverage_exec_custom_scan,
        .EndCustomScan = coverage_end_custom_scan,
        .ReScanCustomScan = coverage_rescan_custom_scan,
        .ExplainCustomScan = coverage_explain_custom_scan,
    };
    return methods;
}

Node* coverage_create_custom_scan_state(CustomScan*) {
    auto* state = reinterpret_cast<ReusableCoverageState*>(
        newNode(sizeof(ReusableCoverageState), T_CustomScanState));
    state->css.methods = &coverage_exec_methods();
    return reinterpret_cast<Node*>(&state->css);
}

const CustomScanMethods& coverage_scan_methods() {
    static const CustomScanMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .CreateCustomScanState = coverage_create_custom_scan_state,
    };
    return methods;
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
        || list_length(custom_plans) != 1
        || list_length(best_path->custom_private) != 6
        || !IsA(list_nth(best_path->custom_private, 0), Integer)
        || intVal(list_nth(best_path->custom_private, 0))
            != static_cast<int>(reuse::kReusablePlanFormatVersionV1)
        || !IsA(list_nth(best_path->custom_private, 1), Integer)) {
        return nullptr;
    }
    const int provider_id = intVal(list_nth(best_path->custom_private, 1));
    const provider_registry::ProviderV1* provider = provider_id > 0
        ? provider_registry::find_provider_v1(
            static_cast<std::uint32_t>(provider_id))
        : nullptr;
    Plan* child = static_cast<Plan*>(linitial(custom_plans));
    reuse::PlannerOwnedDigestRecipeV1 recipe{};
    reuse::ProviderCallDiagnosticV1 diagnostic{};
    const bool recipe_valid = provider != nullptr
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
    static_cast<void>(capture_canonical_child_plan_fail_open(
        child, &canonical_plan));
    constexpr int display_sql_truncated_flag = 1 << 0;
    constexpr int canonical_plan_truncated_flag = 1 << 1;
    int diagnostic_flags = 0;
    if (display_sql.truncated) {
        diagnostic_flags |= display_sql_truncated_flag;
    }
    if (canonical_plan.truncated) {
        diagnostic_flags |= canonical_plan_truncated_flag;
    }

    auto* scan = makeNode(CustomScan);
    scan->methods = &coverage_scan_methods();
    scan->flags = best_path->flags;
    scan->scan.scanrelid = 0;
    scan->scan.plan.targetlist = target_list;
    scan->scan.plan.qual = NIL;
    scan->custom_plans = custom_plans;
    scan->custom_private = static_cast<List*>(
        copyObject(best_path->custom_private));
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
    scan->custom_exprs = list_make1(
        copyObject(list_nth(best_path->custom_private, 4)));
    const auto* sources = static_cast<const List*>(
        list_nth(best_path->custom_private, 3));
    ListCell* cell = nullptr;
    foreach(cell, sources) {
        const auto* source = static_cast<const List*>(lfirst(cell));
        if (source == nullptr || list_length(source) != 7) {
            return nullptr;
        }
        scan->custom_exprs = lappend(
            scan->custom_exprs,
            copyObject(list_nth(source, 6)));
    }
    scan->custom_scan_tlist = static_cast<List*>(
        copyObject(child->targetlist));
    scan->custom_relids = bms_copy(relation->relids);
    return &scan->scan.plan;
}

const CustomPathMethods& coverage_path_methods() {
    static const CustomPathMethods methods{
        .CustomName = "BioseqReusableCoverageScan",
        .PlanCustomPath = coverage_plan_custom_path,
    };
    return methods;
}

[[nodiscard]] bool path_contains_reusable_coverage(const Path* path);

/** Inspect only selected child paths; relation pathlists contain alternatives. */
[[nodiscard]] bool path_list_contains_reusable_coverage(const List* paths) {
    const ListCell* cell = nullptr;
    foreach(cell, paths) {
        if (path_contains_reusable_coverage(
                static_cast<const Path*>(lfirst(cell)))) {
            return true;
        }
    }
    return false;
}

/** Recognize a cache-owned coverage node anywhere in one chosen Path tree. */
[[nodiscard]] bool path_contains_reusable_coverage(const Path* path) {
    if (path == nullptr) {
        return false;
    }
    switch (nodeTag(path)) {
        case T_BitmapHeapPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const BitmapHeapPath*>(path)->bitmapqual);
        case T_BitmapAndPath:
            return path_list_contains_reusable_coverage(
                reinterpret_cast<const BitmapAndPath*>(path)->bitmapquals);
        case T_BitmapOrPath:
            return path_list_contains_reusable_coverage(
                reinterpret_cast<const BitmapOrPath*>(path)->bitmapquals);
        case T_SubqueryScanPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const SubqueryScanPath*>(path)->subpath);
        case T_ForeignPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const ForeignPath*>(path)->fdw_outerpath);
        case T_CustomPath: {
            const auto* custom = reinterpret_cast<const CustomPath*>(path);
            return custom->methods == &coverage_path_methods()
                || path_list_contains_reusable_coverage(
                    custom->custom_paths);
        }
        case T_AppendPath:
            return path_list_contains_reusable_coverage(
                reinterpret_cast<const AppendPath*>(path)->subpaths);
        case T_MergeAppendPath:
            return path_list_contains_reusable_coverage(
                reinterpret_cast<const MergeAppendPath*>(path)->subpaths);
        case T_MaterialPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const MaterialPath*>(path)->subpath);
        case T_MemoizePath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const MemoizePath*>(path)->subpath);
        case T_UniquePath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const UniquePath*>(path)->subpath);
        case T_GatherPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const GatherPath*>(path)->subpath);
        case T_GatherMergePath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const GatherMergePath*>(path)->subpath);
        case T_NestPath:
        case T_MergePath:
        case T_HashPath: {
            const auto* join = reinterpret_cast<const JoinPath*>(path);
            return path_contains_reusable_coverage(join->outerjoinpath)
                || path_contains_reusable_coverage(join->innerjoinpath);
        }
        case T_ProjectionPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const ProjectionPath*>(path)->subpath);
        case T_ProjectSetPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const ProjectSetPath*>(path)->subpath);
        case T_SortPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const SortPath*>(path)->subpath);
        case T_IncrementalSortPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const IncrementalSortPath*>(path)
                    ->spath.subpath);
        case T_GroupPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const GroupPath*>(path)->subpath);
        case T_UpperUniquePath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const UpperUniquePath*>(path)->subpath);
        case T_AggPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const AggPath*>(path)->subpath);
        case T_GroupingSetsPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const GroupingSetsPath*>(path)->subpath);
        case T_MinMaxAggPath: {
            const auto* aggregate = reinterpret_cast<const MinMaxAggPath*>(
                path);
            const ListCell* cell = nullptr;
            foreach(cell, aggregate->mmaggregates) {
                const auto* info = static_cast<const MinMaxAggInfo*>(
                    lfirst(cell));
                if (info != nullptr
                    && path_contains_reusable_coverage(info->path)) {
                    return true;
                }
            }
            return false;
        }
        case T_WindowAggPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const WindowAggPath*>(path)->subpath);
        case T_SetOpPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const SetOpPath*>(path)->subpath);
        case T_RecursiveUnionPath: {
            const auto* recursive =
                reinterpret_cast<const RecursiveUnionPath*>(path);
            return path_contains_reusable_coverage(recursive->leftpath)
                || path_contains_reusable_coverage(recursive->rightpath);
        }
        case T_LockRowsPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const LockRowsPath*>(path)->subpath);
        case T_ModifyTablePath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const ModifyTablePath*>(path)->subpath);
        case T_LimitPath:
            return path_contains_reusable_coverage(
                reinterpret_cast<const LimitPath*>(path)->subpath);
        default:
            return false;
    }
}

/** Create an exact-result wrapper around one final path. */
[[nodiscard]] CustomPath* make_exact_cache_path(
        RelOptInfo& output_relation, Path& input_path,
        const PlannerFrame& frame) {
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
        makeInteger(0)); /* force_bypass is preallocated for later verification */
    path->custom_private = lappend(
        path->custom_private,
        copyObject(frame.referenced_parameter_ids));
    path->custom_private = lappend(
        path->custom_private,
        copyObject(frame.referenced_relation_oids));
    path->custom_restrictinfo = NIL;
    path->methods = &cache_path_methods();
    return path;
}

/** Predecessor-first top-level FINAL path decoration before setrefs. */
void cache_create_upper_paths(
        PlannerInfo* root, UpperRelationKind stage,
        RelOptInfo* input_relation, RelOptInfo* output_relation, void* extra) {
    if (g_previous_create_upper_paths_hook != nullptr) {
        g_previous_create_upper_paths_hook(
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
            replacements = lappend(
                replacements,
                make_exact_cache_path(
                    *output_relation, *input_path, *frame));
            exact_path_added = true;
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
        || !OidIsValid(range->relid) || verification.relation_free) {
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
            if (scan->methods != &coverage_scan_methods()
                || list_length(scan->custom_plans) != 1
                || contain_mutable_functions(
                    reinterpret_cast<Node*>(scan->custom_exprs))
                || !exact_plan_list_safe(
                    verification, scan->custom_plans)) {
                return false;
            }
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

[[nodiscard]] bool exact_dependency_metadata(
        const CustomScan& scan, const List** relation_oids) noexcept {
    if (relation_oids == nullptr || scan.custom_private == NIL
        || list_length(scan.custom_private) <= 4) {
        return false;
    }
    const auto* dependencies = static_cast<const List*>(
        list_nth(scan.custom_private, 4));
    if (dependencies != NIL && nodeTag(dependencies) != T_OidList) {
        return false;
    }
    *relation_oids = dependencies;
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
        ExactPlanVerification verification{
            .statement = &statement,
            .relation_oids = NIL,
            .relation_free = false,
            .saw_relation_free_source = false,
        };
        if (exact_dependency_metadata(*exact, &relation_oids)
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
                || verification.saw_relation_free_source);
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

/** Reentrant planner wrapper that owns canonical bytes for upper-path hooks. */
PlannedStmt* cache_planner(
        Query* query, const char* query_string,
        int cursor_options, ParamListInfo bound_parameters) {
    if (g_internal_planning
        || g_query_cache_mode == static_cast<int>(QueryCacheMode::Disabled)
        || !shared_cache_ready()) {
        return g_previous_planner_hook != nullptr
            ? g_previous_planner_hook(
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
        .referenced_parameter_ids = NIL,
        .referenced_relation_oids = NIL,
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
            frame.referenced_parameter_ids = collect_external_parameter_ids(*query);
            frame.referenced_relation_oids = collect_relation_oids(
                *query, relation_overflow);
            frame.eligible = !relation_overflow;
            if (frame.eligible
                && g_query_cache_mode
                    == static_cast<int>(QueryCacheMode::Reuse)) {
                const auto providers =
                    provider_registry::snapshot_providers_v1();
                for (std::uint32_t provider_index = 0;
                     provider_index < providers.provider_count
                     && frame.provider_intent_count
                         < frame.providers.size();
                     ++provider_index) {
                    const provider_registry::ProviderV1* provider =
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
        result = g_previous_planner_hook != nullptr
            ? g_previous_planner_hook(
                query, query_string, cursor_options, bound_parameters)
            : standard_planner(
                query, query_string, cursor_options, bound_parameters);
        if (result != nullptr) {
            verify_exact_cache_plan_fail_open(*result);
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
        precise = true;
    }
    if (!precise) {
        mark_current_transaction_dirty();
    }
}

void cache_executor_start(QueryDesc* query_description, int eflags) {
    if (query_description != nullptr
        && query_description->plannedstmt != nullptr
        && (query_description->plannedstmt->commandType != CMD_SELECT
            || query_description->plannedstmt->hasModifyingCTE)) {
        mark_planned_result_relations(*query_description->plannedstmt);
    }
    if (g_previous_executor_start_hook != nullptr) {
        g_previous_executor_start_hook(query_description, eflags);
    } else {
        standard_ExecutorStart(query_description, eflags);
    }
}

/** Exclude utility commands that cannot change a reusable SQL result. */
[[nodiscard]] bool utility_may_change_database(const Node* statement) noexcept {
    if (statement == nullptr) {
        return false;
    }
    switch (nodeTag(statement)) {
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

/** Conservatively publish one database-wide epoch for utility writes. */
void cache_process_utility(
        PlannedStmt* statement, const char* query_string, bool read_only_tree,
        ProcessUtilityContext context, ParamListInfo parameters,
        QueryEnvironment* environment, DestReceiver* destination,
        QueryCompletion* completion) {
    const bool dirty = statement != nullptr
        && utility_may_change_database(statement->utilityStmt);
    const bool outside_transaction = dirty && !IsTransactionState();
    if (dirty && !outside_transaction) {
        mark_current_transaction_dirty();
    }
    if (g_previous_process_utility_hook != nullptr) {
        g_previous_process_utility_hook(
            statement, query_string, read_only_tree, context, parameters,
            environment, destination, completion);
    } else {
        standard_ProcessUtility(
            statement, query_string, read_only_tree, context, parameters,
            environment, destination, completion);
    }
    if (outside_transaction) {
        g_force_coarse_transition = true;
        begin_database_transition();
        finish_database_transition(true);
    }
}

/** Install all permanently chained backend hooks. */
void install_cache_hooks() {
    if (g_hooks_installed) {
        return;
    }
    g_previous_planner_hook = planner_hook;
    planner_hook = cache_planner;
    g_previous_create_upper_paths_hook = create_upper_paths_hook;
    create_upper_paths_hook = cache_create_upper_paths;
    g_previous_executor_start_hook = ExecutorStart_hook;
    ExecutorStart_hook = cache_executor_start;
    g_previous_process_utility_hook = ProcessUtility_hook;
    ProcessUtility_hook = cache_process_utility;
    g_hooks_installed = true;
}

/** Read one shared counter without exposing shared-memory pointers. */
[[nodiscard]] std::uint64_t read_counter(pg_atomic_uint64& counter) noexcept {
    return g_shared == nullptr ? 0 : pg_atomic_read_u64(&counter);
}

/** Enforce the documented schema-USAGE plus function-EXECUTE boundary. */
void require_cache_diagnostics_reader(FunctionCallInfo function_call) {
    const Oid user_id = GetUserId();
    const Oid function_oid = function_call != nullptr
            && function_call->flinfo != nullptr
        ? function_call->flinfo->fn_oid
        : InvalidOid;
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
    std::uint64_t key_hash;
    std::uint64_t generation;
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
    std::uint16_t relation_count;
    Oid relation_oids[kMaximumRelationDependencies];
    char display_sql[4096];
};

[[nodiscard]] const char* entry_state_name(SharedEntryState state) noexcept {
    switch (state) {
        case SharedEntryState::Empty: return "EMPTY";
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
    storage::ManifestHandle manifests[kRegistryCapacity]{};
    std::size_t manifest_count = 0;
    std::uint64_t swept = 0;
    const TimestampTz now = GetCurrentTimestamp();
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (CacheEntry& entry : g_shared->entries) {
        reap_dead_registry_readers_locked(entry);
        reap_dead_registry_waiters_locked(entry, storage_attached);
        PGPROC* producer = entry.producer_proc_number == INVALID_PROC_NUMBER
            ? nullptr
            : ProcNumberGetProc(entry.producer_proc_number);
        const bool dead_producer = entry.state == SharedEntryState::Producing
            && (producer == nullptr || producer->pid != entry.producer_pid);
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
    return swept;
}

/** Copy bounded diagnostics under lock; never hold shared locks during SQL I/O. */
[[nodiscard]] int snapshot_entries(EntrySnapshot* snapshots) noexcept {
    if (g_shared == nullptr) {
        return 0;
    }
    int count = 0;
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const CacheEntry& entry : g_shared->entries) {
        if (entry.state == SharedEntryState::Empty) {
            continue;
        }
        EntrySnapshot& snapshot = snapshots[count++];
        snapshot.state = entry.state;
        snapshot.key_hash = entry.key_hash;
        snapshot.generation = entry.generation;
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
        snapshot.relation_count = entry.relation_count
                <= kMaximumRelationDependencies
            ? entry.relation_count
            : static_cast<std::uint16_t>(kMaximumRelationDependencies);
        std::memcpy(
            snapshot.relation_oids,
            entry.relation_oids,
            static_cast<std::size_t>(snapshot.relation_count) * sizeof(Oid));
        strlcpy(
            snapshot.display_sql, entry.display_sql,
            sizeof(snapshot.display_sql));
    }
    LWLockRelease(g_registry_lock);
    return count;
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
    std::array<char, kReusableDisplaySqlCapacityV1> display_sql{};
    std::array<char, kReusableCanonicalPlanCapacityV1> canonical_plan{};
    std::array<char, kReusableProducerPlanCapacityV1> producer_plan{};
    bool copied = false;
};

/** Copy row text only after the SQL caller passed row-level authorization. */
[[nodiscard]] bool copy_reusable_entry_text(
        const ReusableGenerationReferenceV1& reference,
        ReusableDiagnosticTextSnapshotV1* output) noexcept {
    if (!reference || output == nullptr || !g_reusable_registry
        || g_reusable_membership_lock == nullptr) {
        return false;
    }
    *output = {};
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
        *output = {};
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
    if (!g_reusable_registry || g_reusable_membership_lock == nullptr
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
        reap_dead_registry_readers_locked(entry);
        reap_dead_registry_waiters_locked(entry, storage_attached);
        if (entry.state == SharedEntryState::Producing) {
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
        reap_dead_registry_readers_locked(entry);
        reap_dead_registry_waiters_locked(entry, storage_attached);
        if (!entry_depends_on_relation(entry, relation_oid)) {
            continue;
        }
        if (entry.state == SharedEntryState::Producing) {
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

/** Require a materialized set-returning call and initialize its tuplestore. */
[[nodiscard]] TupleDesc initialize_srf(
        FunctionCallInfo function_call, Tuplestorestate** tuple_store) {
    auto* return_info = reinterpret_cast<ReturnSetInfo*>(
        function_call->resultinfo);
    if (return_info == nullptr
        || !IsA(return_info, ReturnSetInfo)
        || (return_info->allowedModes & SFRM_Materialize) == 0) {
        ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("bioseq_cache function requires materialize mode")));
    }
    TupleDesc tuple_descriptor = nullptr;
    if (get_call_result_type(
            function_call, nullptr, &tuple_descriptor) != TYPEFUNC_COMPOSITE) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq_cache function has an invalid result type")));
    }
    MemoryContext old_context = MemoryContextSwitchTo(
        return_info->econtext->ecxt_per_query_memory);
    *tuple_store = tuplestore_begin_heap(true, false, work_mem);
    return_info->returnMode = SFRM_Materialize;
    return_info->setResult = *tuple_store;
    return_info->setDesc = tuple_descriptor;
    MemoryContextSwitchTo(old_context);
    return tuple_descriptor;
}

}  // namespace bioseq::cache::pg

extern "C" {

PG_FUNCTION_INFO_V1(bioseq_cache_version);
Datum bioseq_cache_version(PG_FUNCTION_ARGS) {
    PG_RETURN_TEXT_P(cstring_to_text("1.0"));
}

PG_FUNCTION_INFO_V1(bioseq_cache_stats);
Datum bioseq_cache_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_diagnostics_reader(fcinfo);
    Tuplestorestate* tuple_store = nullptr;
    TupleDesc tuple_descriptor = initialize_srf(fcinfo, &tuple_store);
    Datum values[9]{};
    bool nulls[9]{};
    if (g_shared != nullptr) {
        values[0] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.produced)));
        values[1] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.completed_hits)));
        values[2] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.inflight_waits)));
        values[3] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.reuse_hits)));
        values[4] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.fallbacks)));
        values[5] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.invalidations)));
        values[6] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.evictions)));
        values[7] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.logical_bytes)));
        values[8] = Int64GetDatum(static_cast<int64>(
            read_counter(g_shared->stats.stored_bytes)));
    }
    tuplestore_putvalues(tuple_store, tuple_descriptor, values, nulls);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_entries);
Datum bioseq_cache_entries(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache;
    using namespace bioseq::cache::pg;
    require_cache_diagnostics_reader(fcinfo);
    Tuplestorestate* tuple_store = nullptr;
    TupleDesc tuple_descriptor = initialize_srf(fcinfo, &tuple_store);
    if (tuple_descriptor->natts != 20) {
        ereport(ERROR,
            (errcode(ERRCODE_DATATYPE_MISMATCH),
             errmsg("bioseq_cache.entries() has an invalid row type")));
    }
    auto* snapshots = static_cast<EntrySnapshot*>(
        palloc0(sizeof(EntrySnapshot) * kRegistryCapacity));
    const int count = snapshot_entries(snapshots);
    const TimestampTz now = GetCurrentTimestamp();
    for (int index = 0; index < count; ++index) {
        const EntrySnapshot& entry = snapshots[index];
        const bool full_disclosure = diagnostic_disclosure_for_role(
                entry.current_role_oid)
            == CacheDiagnosticDisclosure::FullText;
        Datum values[20]{};
        bool nulls[20]{};
        char* key = psprintf(
            "%016llx", static_cast<unsigned long long>(entry.key_hash));
        values[0] = CStringGetTextDatum(key);
        values[1] = Int64GetDatum(static_cast<int64>(entry.generation));
        values[2] = CStringGetTextDatum("ExactFinalResult");
        values[3] = CStringGetTextDatum(entry_state_name(entry.state));
        values[4] = ObjectIdGetDatum(entry.database_oid);
        if (full_disclosure) {
            values[5] = ObjectIdGetDatum(entry.current_role_oid);
            values[6] = Int32GetDatum(entry.producer_pid);
        } else {
            nulls[5] = true;
            nulls[6] = true;
        }
        values[7] = TimestampTzGetDatum(entry.created_at);
        if (entry.completed_at != 0) {
            values[8] = TimestampTzGetDatum(entry.completed_at);
        } else {
            nulls[8] = true;
        }
        auto* age = static_cast<Interval*>(palloc0(sizeof(Interval)));
        age->time = now - entry.created_at;
        values[9] = IntervalPGetDatum(age);
        auto* ttl = static_cast<Interval*>(palloc0(sizeof(Interval)));
        ttl->time = entry.completed_at == 0
            ? static_cast<std::int64_t>(g_query_cache_ttl_ms) * 1000
            : entry.expires_at - entry.completed_at;
        values[10] = IntervalPGetDatum(ttl);
        values[11] = Int64GetDatum(static_cast<int64>(entry.row_count));
        values[12] = Int64GetDatum(static_cast<int64>(entry.logical_bytes));
        values[13] = Int64GetDatum(static_cast<int64>(entry.stored_bytes));
        values[14] = Int32GetDatum(static_cast<int32>(entry.waiter_count));
        values[15] = Int32GetDatum(static_cast<int32>(entry.reader_count));
        values[16] = Int64GetDatum(static_cast<int64>(entry.hits));
        if (full_disclosure) {
            values[17] = CStringGetTextDatum(entry.display_sql);
        } else {
            nulls[17] = true;
        }
        nulls[18] = true; /* V1 does not persist a producer Plan snapshot. */
        StringInfoData dependencies{};
        initStringInfo(&dependencies);
        appendStringInfo(
            &dependencies,
            "{\"database_epoch\":%llu,\"relation_oids\":[",
            static_cast<unsigned long long>(entry.database_epoch));
        for (std::uint16_t relation_index = 0;
             relation_index < entry.relation_count;
             ++relation_index) {
            appendStringInfo(
                &dependencies,
                "%s%u",
                relation_index == 0 ? "" : ",",
                entry.relation_oids[relation_index]);
        }
        appendStringInfoChar(&dependencies, ']');
        appendStringInfoChar(&dependencies, '}');
        if (full_disclosure) {
            values[19] = DirectFunctionCall1(
                jsonb_in, CStringGetDatum(dependencies.data));
        } else {
            nulls[19] = true;
        }
        tuplestore_putvalues(tuple_store, tuple_descriptor, values, nulls);
        pfree(key);
        pfree(age);
        pfree(ttl);
        pfree(dependencies.data);
    }

    if (g_reusable_wait_slot_count != 0) {
        const std::size_t reusable_capacity = g_reusable_wait_slot_count;
        auto* reusable_rows =
            static_cast<ReusableGenerationDiagnosticsRowV1*>(palloc0(
                sizeof(ReusableGenerationDiagnosticsRowV1)
                    * reusable_capacity));
        const auto reusable_snapshot = snapshot_reusable_entries(
            std::span<ReusableGenerationDiagnosticsRowV1>{
                reusable_rows, reusable_capacity});
        if (reusable_snapshot.error != ReusableRegistryErrorV1::None
            || reusable_snapshot.truncated) {
            ereport(ERROR,
                (errcode(ERRCODE_DATA_CORRUPTED),
                 errmsg("bioseq_cache reusable diagnostics are inconsistent")));
        }
        const std::uint64_t now_monotonic_ns = reusable_monotonic_now_ns();
        for (std::size_t index = 0;
             index < reusable_snapshot.count;
             ++index) {
            const ReusableGenerationDiagnosticsRowV1& entry =
                reusable_rows[index];
            const bool full_disclosure = diagnostic_disclosure_for_role(
                    static_cast<Oid>(entry.current_role_oid))
                == CacheDiagnosticDisclosure::FullText;
            Datum values[20]{};
            bool nulls[20]{};
            const auto key = reusable_digest_hex(entry.reference.id.digest);
            const auto dependency = reusable_digest_hex(
                entry.dependency_fingerprint);
            values[0] = CStringGetTextDatum(key.data());
            values[1] = Int64GetDatum(static_cast<int64>(
                entry.reference.id.generation));
            values[2] = CStringGetTextDatum("ReusableCoverage");
            values[3] = CStringGetTextDatum(
                reusable_entry_state_name(entry.state));
            if (entry.database_oid != 0) {
                values[4] = ObjectIdGetDatum(entry.database_oid);
            } else {
                nulls[4] = true;
            }
            if (entry.current_role_oid != 0 && full_disclosure) {
                values[5] = ObjectIdGetDatum(entry.current_role_oid);
            } else {
                nulls[5] = true;
            }
            if (entry.producer_pid > 0 && full_disclosure) {
                values[6] = Int32GetDatum(entry.producer_pid);
            } else {
                nulls[6] = true;
            }
            const auto created_at = reusable_wall_timestamp(
                now, now_monotonic_ns, entry.started_monotonic_ns);
            if (created_at.has_value()) {
                values[7] = TimestampTzGetDatum(*created_at);
            } else {
                nulls[7] = true;
            }
            const auto completed_at = reusable_wall_timestamp(
                now, now_monotonic_ns, entry.completed_monotonic_ns);
            if (completed_at.has_value()) {
                values[8] = TimestampTzGetDatum(*completed_at);
            } else {
                nulls[8] = true;
            }
            auto* age = static_cast<Interval*>(palloc0(sizeof(Interval)));
            age->time = reusable_age_us(
                now_monotonic_ns, entry.started_monotonic_ns);
            values[9] = IntervalPGetDatum(age);
            auto* ttl = static_cast<Interval*>(palloc0(sizeof(Interval)));
            ttl->time = reusable_ttl_us(entry.producer_ttl_ms);
            values[10] = IntervalPGetDatum(ttl);
            values[11] = Int64GetDatum(static_cast<int64>(entry.row_count));
            values[12] = Int64GetDatum(static_cast<int64>(
                entry.logical_bytes));
            values[13] = Int64GetDatum(static_cast<int64>(
                entry.stored_bytes));
            values[14] = Int32GetDatum(static_cast<int32>(
                entry.waiter_count));
            values[15] = Int32GetDatum(static_cast<int32>(
                entry.reader_pin_count));
            values[16] = Int64GetDatum(0); /* Per-generation hits are absent. */
            ReusableDiagnosticTextSnapshotV1 diagnostic_text{};
            const bool diagnostic_text_copied = full_disclosure
                && copy_reusable_entry_text(
                    entry.reference, &diagnostic_text);
            if (diagnostic_text_copied
                && diagnostic_text.metadata.display_sql_bytes != 0) {
                values[17] = PointerGetDatum(cstring_to_text_with_len(
                    diagnostic_text.display_sql.data(),
                    diagnostic_text.metadata.display_sql_bytes));
            } else {
                nulls[17] = true;
            }
            if (diagnostic_text_copied
                && diagnostic_text.metadata.producer_plan_bytes != 0) {
                values[18] = PointerGetDatum(cstring_to_text_with_len(
                    diagnostic_text.producer_plan.data(),
                    diagnostic_text.metadata.producer_plan_bytes));
            } else {
                nulls[18] = true;
            }
            StringInfoData dependencies{};
            initStringInfo(&dependencies);
            appendStringInfo(
                &dependencies,
                "{\"action\":\"%s\",\"failure\":\"%s\","
                "\"provider_id\":%u,\"provider_abi_version\":%u,"
                "\"provider_domain_abi_version\":%u,"
                "\"compatibility_family_id\":%llu,"
                "\"capability_mask\":%llu,\"dependency_count\":%u,"
                "\"dependency_fingerprint\":\"%s\","
                "\"observability\":\"%s\",\"planner_pins\":%u,"
                "\"canonical_plan_status\":\"%s\","
                "\"producer_plan_status\":\"%s\","
                "\"display_sql_truncated\":%s,"
                "\"canonical_plan_truncated\":%s,"
                "\"producer_plan_truncated\":%s",
                reusable_entry_action_name(entry.state),
                reusable_failure_name(entry.failure),
                entry.provider_id,
                entry.provider_abi_version,
                entry.provider_domain_abi_version,
                static_cast<unsigned long long>(
                    entry.compatibility_family_id),
                static_cast<unsigned long long>(entry.capability_mask),
                entry.dependency_count,
                dependency.data(),
                reusable_observability_name(entry.observability),
                entry.planner_pin_count,
                reusable_plan_snapshot_status_name(
                    entry.canonical_plan_snapshot_status),
                reusable_plan_snapshot_status_name(
                    entry.plan_snapshot_status),
                entry.display_sql_truncated ? "true" : "false",
                entry.canonical_plan_truncated ? "true" : "false",
                entry.producer_plan_truncated ? "true" : "false");
            if (entry.manifest) {
                appendStringInfo(
                    &dependencies,
                    ",\"manifest\":{\"slot\":%u,"
                    "\"slot_sequence\":%llu,\"generation\":%llu}",
                    entry.manifest.slot,
                    static_cast<unsigned long long>(
                        entry.manifest.slot_sequence),
                    static_cast<unsigned long long>(
                        entry.manifest.id.generation));
            } else {
                appendStringInfoString(&dependencies, ",\"manifest\":null");
            }
            appendStringInfoChar(&dependencies, '}');
            if (full_disclosure) {
                values[19] = DirectFunctionCall1(
                    jsonb_in, CStringGetDatum(dependencies.data));
            } else {
                nulls[19] = true;
            }
            tuplestore_putvalues(
                tuple_store, tuple_descriptor, values, nulls);
            pfree(age);
            pfree(ttl);
            pfree(dependencies.data);
        }
        pfree(reusable_rows);
    }
    pfree(snapshots);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_invalidate_all);
Datum bioseq_cache_invalidate_all(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to invalidate bioseq_cache")));
    }
    const std::uint64_t invalidated = invalidate_registry()
        + invalidate_reusable_registry(std::nullopt);
    PG_RETURN_INT64(static_cast<int64>(invalidated));
}

PG_FUNCTION_INFO_V1(bioseq_cache_invalidate_relation);
Datum bioseq_cache_invalidate_relation(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to invalidate bioseq_cache")));
    }
    const Oid relation_oid = PG_GETARG_OID(0);
    const std::uint64_t invalidated =
        invalidate_relation_registry(relation_oid)
        + invalidate_reusable_relation_registry(relation_oid);
    PG_RETURN_INT64(static_cast<int64>(invalidated));
}

PG_FUNCTION_INFO_V1(bioseq_cache_reset_stats);
Datum bioseq_cache_reset_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to reset bioseq_cache statistics")));
    }
    if (g_shared != nullptr) {
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
        LWLockRelease(g_registry_lock);
    }
    PG_RETURN_VOID();
}

#if defined(BIOSEQ_CACHE_TESTING)

static void require_cache_test_superuser() {
    if (!superuser()) {
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to use bioseq_cache test controls")));
    }
    if (bioseq::cache::pg::g_shared == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq_cache test shared memory is unavailable")));
    }
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_exact_plan_fault);
Datum bioseq_cache_test_set_exact_plan_fault(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (std::strcmp(name, "none") == 0) {
        g_test_exact_plan_fault = TestExactPlanFault::None;
    } else if (std::strcmp(name, "unknown_custom_scan") == 0) {
        g_test_exact_plan_fault = TestExactPlanFault::UnknownCustomScan;
    } else if (std::strcmp(name, "unknown_table_am") == 0) {
        g_test_exact_plan_fault =
            TestExactPlanFault::UnknownTableAccessMethod;
    } else if (std::strcmp(name, "unknown_index_am") == 0) {
        g_test_exact_plan_fault =
            TestExactPlanFault::UnknownIndexAccessMethod;
    } else {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown exact-plan verifier fault: %s", name)));
    }
    pfree(name);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_arm_pause);
Datum bioseq_cache_test_arm_pause(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const TestPausePoint point = parse_pause_point(name);
    const std::uint32_t bit = pause_bit(point);
    CacheTestControl& control = g_shared->test_control;
    static_cast<void>(DirectFunctionCall1(
        pg_advisory_lock_int8,
        Int64GetDatum(pause_lock_key(point))));
    pg_atomic_fetch_and_u32(&control.paused_mask, ~bit);
    pg_atomic_fetch_and_u32(&control.released_mask, ~bit);
    pg_atomic_fetch_or_u32(&control.armed_mask, bit);
    ConditionVariableBroadcast(&control.changed);
    pfree(name);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_wait_until_paused);
Datum bioseq_cache_test_wait_until_paused(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const std::uint32_t bit = pause_bit(parse_pause_point(name));
    pfree(name);
    CacheTestControl& control = g_shared->test_control;
    const TimestampTz started = GetCurrentTimestamp();
    ConditionVariablePrepareToSleep(&control.changed);
    PG_TRY();
    {
        while ((pg_atomic_read_u32(&control.paused_mask) & bit) == 0) {
            CHECK_FOR_INTERRUPTS();
            if (TimestampDifferenceExceeds(
                    started, GetCurrentTimestamp(), 30000)) {
                ereport(ERROR,
                    (errcode(ERRCODE_LOCK_NOT_AVAILABLE),
                     errmsg("timed out waiting for bioseq_cache pause")));
            }
            static_cast<void>(ConditionVariableTimedSleep(
                &control.changed, 100L, PG_WAIT_EXTENSION));
        }
        ConditionVariableCancelSleep();
    }
    PG_CATCH();
    {
        ConditionVariableCancelSleep();
        PG_RE_THROW();
    }
    PG_END_TRY();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_release_pause);
Datum bioseq_cache_test_release_pause(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* name = text_to_cstring(PG_GETARG_TEXT_PP(0));
    const TestPausePoint point = parse_pause_point(name);
    const std::uint32_t bit = pause_bit(point);
    pfree(name);
    pg_atomic_fetch_or_u32(&g_shared->test_control.released_mask, bit);
    static_cast<void>(DirectFunctionCall1(
        pg_advisory_unlock_int8,
        Int64GetDatum(pause_lock_key(point))));
    ConditionVariableBroadcast(&g_shared->test_control.changed);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_fail_next_write);
Datum bioseq_cache_test_fail_next_write(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    char* failure = text_to_cstring(PG_GETARG_TEXT_PP(0));
    if (std::strcmp(failure, "encoder_error") != 0
        && std::strcmp(failure, "enospc") != 0) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("unknown bioseq_cache write failure: %s", failure)));
    }
    pfree(failure);
    pg_atomic_write_u32(&g_shared->test_control.fail_next_write, 1);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_force_file_tier);
Datum bioseq_cache_test_set_force_file_tier(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    if (!storage::set_test_force_file_tier(PG_GETARG_BOOL(0))) {
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("bioseq_cache storage is unavailable")));
    }
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_storage_tier_stats);
Datum bioseq_cache_test_storage_tier_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    const storage::StorageTierTestSnapshot snapshot =
        storage::test_storage_tier_snapshot();
    Datum values[13]{
        BoolGetDatum(snapshot.force_file_tier),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.dsa_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.file_blocks)),
        Int64GetDatum(
            static_cast<std::int64_t>(snapshot.reserved_hot_bytes)),
        Int64GetDatum(
            static_cast<std::int64_t>(snapshot.reserved_file_bytes)),
        BoolGetDatum(snapshot.coherent),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.occupied_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.reserved_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.stored_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.referenced_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.retiring_blocks)),
        Int64GetDatum(
            static_cast<std::int64_t>(snapshot.release_committed_blocks)),
        Int64GetDatum(static_cast<std::int64_t>(snapshot.reader_pins)),
    };
    bool nulls[13]{};
    tuplestore_putvalues(store, descriptor, values, nulls);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_entry_states);
Datum bioseq_cache_test_entry_states(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    auto* snapshots = static_cast<EntrySnapshot*>(
        palloc0(sizeof(EntrySnapshot) * kRegistryCapacity));
    const int count = snapshot_entries(snapshots);
    for (int index = 0; index < count; ++index) {
        const EntrySnapshot& entry = snapshots[index];
        char* key = psprintf(
            "%016llx", static_cast<unsigned long long>(entry.key_hash));
        Datum values[6]{
            CStringGetTextDatum(key),
            Int64GetDatum(static_cast<int64>(entry.generation)),
            CStringGetTextDatum(entry_state_name(entry.state)),
            Int32GetDatum(static_cast<int32>(entry.waiter_count)),
            Int32GetDatum(static_cast<int32>(entry.reader_count)),
            Int32GetDatum(entry.producer_pid),
        };
        bool nulls[6]{};
        tuplestore_putvalues(store, descriptor, values, nulls);
        pfree(key);
    }
    pfree(snapshots);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_planner_reference_counts);
Datum bioseq_cache_test_planner_reference_counts(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    auto* snapshots = static_cast<EntrySnapshot*>(
        palloc0(sizeof(EntrySnapshot) * kRegistryCapacity));
    const int count = snapshot_entries(snapshots);
    std::uint64_t readers = 0;
    std::uint64_t waiters = 0;
    for (int index = 0; index < count; ++index) {
        readers += snapshots[index].reader_count;
        waiters += snapshots[index].waiter_count;
    }
    Datum values[3]{
        Int64GetDatum(static_cast<int64>(readers)),
        Int64GetDatum(static_cast<int64>(waiters)),
        Int64GetDatum(0),
    };
    bool nulls[3]{};
    tuplestore_putvalues(store, descriptor, values, nulls);
    pfree(snapshots);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_provider_callback_stats);
Datum bioseq_cache_test_provider_callback_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    static_cast<void>(initialize_srf(fcinfo, &store));
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_plan_build_stats);
Datum bioseq_cache_test_plan_build_stats(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    const std::uint64_t generation = read_counter(
        g_shared->stats.planner_build_generation);
    const bool same = g_query_cache_mode == static_cast<int>(QueryCacheMode::Same);
    const bool reuse = g_query_cache_mode == static_cast<int>(QueryCacheMode::Reuse);
    Datum values[5]{
        Int64GetDatum(static_cast<int64>(generation)),
        Int64GetDatum(0),
        Int64GetDatum(static_cast<int64>(same ? generation : 0)),
        Int64GetDatum(static_cast<int64>(reuse ? generation : 0)),
        Int64GetDatum(0),
    };
    bool nulls[5]{};
    tuplestore_putvalues(store, descriptor, values, nulls);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_mapping_fault);
Datum bioseq_cache_test_set_mapping_fault(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    static_cast<void>(PG_GETARG_TEXT_PP(0));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_clear_mapping_fault);
Datum bioseq_cache_test_clear_mapping_fault(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_runtime_digest_fault);
Datum bioseq_cache_test_set_runtime_digest_fault(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    static_cast<void>(PG_GETARG_TEXT_PP(0));
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_clear_runtime_digest_fault);
Datum bioseq_cache_test_clear_runtime_digest_fault(PG_FUNCTION_ARGS) {
    require_cache_test_superuser();
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_set_forced_coverage_domain_digest);
Datum bioseq_cache_test_set_forced_coverage_domain_digest(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const bytea* value = PG_GETARG_BYTEA_PP(0);
    if (VARSIZE_ANY_EXHDR(value) != g_forced_coverage_domain_digest.bytes.size()) {
        ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("forced coverage domain digest must contain 32 bytes")));
    }
    std::memcpy(
        g_forced_coverage_domain_digest.bytes.data(),
        VARDATA_ANY(value),
        g_forced_coverage_domain_digest.bytes.size());
    g_forced_coverage_domain_digest_active = true;
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_clear_forced_coverage_domain_digest);
Datum bioseq_cache_test_clear_forced_coverage_domain_digest(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    g_forced_coverage_domain_digest = {};
    g_forced_coverage_domain_digest_active = false;
    PG_RETURN_VOID();
}

static void copy_injected_bytes(
        unsigned char* destination, std::uint16_t* destination_size,
        const bytea* source, const char* field_name) {
    const std::size_t size = VARSIZE_ANY_EXHDR(source);
    if (size > bioseq::cache::pg::kInjectedDigestBytes) {
        ereport(ERROR,
            (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
             errmsg("%s exceeds %zu bytes", field_name,
                    bioseq::cache::pg::kInjectedDigestBytes)));
    }
    *destination_size = static_cast<std::uint16_t>(size);
    if (size != 0) {
        std::memcpy(destination, VARDATA_ANY(source), size);
    }
}

static bytea* injected_bytes(const unsigned char* source, std::size_t size) {
    bytea* result = static_cast<bytea*>(palloc(VARHDRSZ + size));
    SET_VARSIZE(result, VARHDRSZ + size);
    if (size != 0) {
        std::memcpy(VARDATA(result), source, size);
    }
    return result;
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_inject_complete_coverage);
Datum bioseq_cache_test_inject_complete_coverage(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    bytea* domain = PG_GETARG_BYTEA_PP(0);
    bytea* predicate = PG_GETARG_BYTEA_PP(1);
    bytea* provider_digest = PG_GETARG_BYTEA_PP(2);
    bytea* forced_digest = PG_ARGISNULL(3) ? nullptr : PG_GETARG_BYTEA_PP(3);
    int selected = -1;
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (std::size_t index = 0; index < kInjectedCoverageCapacity; ++index) {
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
        ereport(ERROR,
            (errcode(ERRCODE_INSUFFICIENT_RESOURCES),
             errmsg("bioseq_cache injected coverage registry is full")));
    }
    InjectedCoverage& entry = g_shared->injected_coverage[selected];
    std::memset(&entry, 0, sizeof(entry));
    entry.used = true;
    entry.database_oid = MyDatabaseId;
    int security_context = 0;
    GetUserIdAndSecContext(&entry.current_role_oid, &security_context);
    entry.security_context_flags = static_cast<std::uint32_t>(security_context);
    entry.session_user_oid = GetSessionUserId();
    entry.generation = pg_atomic_fetch_add_u64(
        &g_shared->next_injected_generation, 1);
    copy_injected_bytes(entry.domain, &entry.domain_size, domain, "domain key");
    copy_injected_bytes(
        entry.predicate, &entry.predicate_size, predicate, "predicate key");
    copy_injected_bytes(
        entry.provider_digest, &entry.provider_digest_size,
        provider_digest, "provider security digest");
    if (forced_digest != nullptr) {
        copy_injected_bytes(
            entry.forced_digest, &entry.forced_digest_size,
            forced_digest, "forced domain digest");
    }
    const std::uint64_t generation = entry.generation;
    LWLockRelease(g_registry_lock);
    ConditionVariableBroadcast(&g_shared->test_control.changed);
    PG_RETURN_INT64(static_cast<int64>(generation));
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_lookup_injected_coverage);
Datum bioseq_cache_test_lookup_injected_coverage(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    bytea* domain = PG_GETARG_BYTEA_PP(0);
    const std::size_t domain_size = VARSIZE_ANY_EXHDR(domain);
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const InjectedCoverage& entry : g_shared->injected_coverage) {
        if (!entry.used || entry.database_oid != MyDatabaseId
            || entry.domain_size != domain_size
            || std::memcmp(entry.domain, VARDATA_ANY(domain), domain_size) != 0) {
            continue;
        }
        bytea* predicate = injected_bytes(entry.predicate, entry.predicate_size);
        Datum values[3]{
            Int64GetDatum(static_cast<int64>(entry.generation)),
            PointerGetDatum(predicate),
            CStringGetTextDatum("COMPLETE"),
        };
        bool nulls[3]{};
        tuplestore_putvalues(store, descriptor, values, nulls);
        pfree(predicate);
    }
    LWLockRelease(g_registry_lock);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_coverage_bucket_states);
Datum bioseq_cache_test_coverage_bucket_states(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    Tuplestorestate* store = nullptr;
    TupleDesc descriptor = initialize_srf(fcinfo, &store);
    LWLockAcquire(g_registry_lock, LW_SHARED);
    for (const InjectedCoverage& entry : g_shared->injected_coverage) {
        if (!entry.used || entry.database_oid != MyDatabaseId) {
            continue;
        }
        bytea* provider = injected_bytes(
            entry.provider_digest, entry.provider_digest_size);
        const unsigned char* digest = entry.forced_digest_size == 0
            ? entry.domain
            : entry.forced_digest;
        const std::size_t digest_size = entry.forced_digest_size == 0
            ? entry.domain_size
            : entry.forced_digest_size;
        bytea* coverage = injected_bytes(digest, digest_size);
        Datum generation_value = Int64GetDatum(
            static_cast<int64>(entry.generation));
        ArrayType* generations = construct_array(
            &generation_value, 1, INT8OID, 8, FLOAT8PASSBYVAL, TYPALIGN_DOUBLE);
        Datum values[9]{
            ObjectIdGetDatum(entry.database_oid),
            ObjectIdGetDatum(entry.current_role_oid),
            ObjectIdGetDatum(entry.session_user_oid),
            Int32GetDatum(static_cast<int32>(entry.security_context_flags)),
            Int32GetDatum(0),
            PointerGetDatum(provider),
            PointerGetDatum(coverage),
            Int32GetDatum(1),
            PointerGetDatum(generations),
        };
        bool nulls[9]{};
        tuplestore_putvalues(store, descriptor, values, nulls);
        pfree(provider);
        pfree(coverage);
        pfree(generations);
    }
    LWLockRelease(g_registry_lock);
    PG_RETURN_NULL();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_mutate_coverage_descriptor);
Datum bioseq_cache_test_mutate_coverage_descriptor(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const std::uint64_t generation = static_cast<std::uint64_t>(
        PG_GETARG_INT64(0));
    static_cast<void>(PG_GETARG_TEXT_PP(1));
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used && entry.generation == generation) {
            entry.predicate_size = 0;
            break;
        }
    }
    LWLockRelease(g_registry_lock);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_mark_generation_evictable);
Datum bioseq_cache_test_mark_generation_evictable(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const std::uint64_t generation = static_cast<std::uint64_t>(
        PG_GETARG_INT64(0));
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used && entry.generation == generation) {
            entry.evictable = true;
        }
    }
    LWLockRelease(g_registry_lock);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_request_janitor_sweep);
Datum bioseq_cache_test_request_janitor_sweep(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    storage::request_janitor_sweep(true);
    static_cast<void>(sweep_registry(true));
    LWLockAcquire(g_registry_lock, LW_EXCLUSIVE);
    for (InjectedCoverage& entry : g_shared->injected_coverage) {
        if (entry.used && entry.evictable) {
            entry.used = false;
        }
    }
    LWLockRelease(g_registry_lock);
    ConditionVariableBroadcast(&g_shared->test_control.changed);
    PG_RETURN_VOID();
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_wait_for_generation_absent);
Datum bioseq_cache_test_wait_for_generation_absent(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const std::uint64_t generation = static_cast<std::uint64_t>(
        PG_GETARG_INT64(0));
    const Interval* timeout = PG_GETARG_INTERVAL_P(1);
    const TimestampTz deadline = GetCurrentTimestamp()
        + timeout->time
        + static_cast<std::int64_t>(timeout->day) * USECS_PER_DAY;
    bool absent = false;
    for (;;) {
        absent = true;
        LWLockAcquire(g_registry_lock, LW_SHARED);
        for (const InjectedCoverage& entry : g_shared->injected_coverage) {
            if (entry.used && entry.generation == generation) {
                absent = false;
                break;
            }
        }
        LWLockRelease(g_registry_lock);
        if (absent || GetCurrentTimestamp() >= deadline) {
            break;
        }
        CHECK_FOR_INTERRUPTS();
        static_cast<void>(ConditionVariableTimedSleep(
            &g_shared->test_control.changed, 10L, PG_WAIT_EXTENSION));
        ConditionVariableCancelSleep();
    }
    PG_RETURN_BOOL(absent);
}

PG_FUNCTION_INFO_V1(bioseq_cache_test_direct_heap_insert);
Datum bioseq_cache_test_direct_heap_insert(PG_FUNCTION_ARGS) {
    using namespace bioseq::cache::pg;
    require_cache_test_superuser();
    const Oid relation_oid = PG_GETARG_OID(0);
    const int32 identifier = PG_GETARG_INT32(1);
    text* value = PG_GETARG_TEXT_PP(2);
    char* relation_name = get_rel_name(relation_oid);
    char* namespace_name = get_namespace_name(get_rel_namespace(relation_oid));
    if (relation_name == nullptr || namespace_name == nullptr) {
        ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_TABLE),
             errmsg("relation %u does not exist", relation_oid)));
    }
    char* qualified = quote_qualified_identifier(
        namespace_name, relation_name);
    char* command = psprintf("INSERT INTO %s VALUES ($1, $2)", qualified);
    Oid argument_types[2]{INT4OID, TEXTOID};
    Datum arguments[2]{Int32GetDatum(identifier), PointerGetDatum(value)};
    char nulls[2]{' ', ' '};
    if (SPI_connect() != SPI_OK_CONNECT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("could not connect to SPI")));
    }
    const int status = SPI_execute_with_args(
        command, 2, argument_types, arguments, nulls, false, 0);
    static_cast<void>(SPI_finish());
    pfree(command);
    pfree(qualified);
    pfree(namespace_name);
    if (status != SPI_OK_INSERT) {
        ereport(ERROR,
            (errcode(ERRCODE_INTERNAL_ERROR),
             errmsg("direct test insert failed with SPI status %d", status)));
    }
    mark_current_transaction_dirty();
    PG_RETURN_VOID();
}

#endif /* BIOSEQ_CACHE_TESTING */

void _PG_init() {
    using namespace bioseq::cache::pg;
    register_cache_gucs();
    if (!maintenance::register_reusable_janitor_callback_v1(
            reusable_registry_janitor_sweep_v1)) {
        ereport(ERROR,
            (errmsg("bioseq_cache could not register reusable maintenance")));
    }
    provider_registry::install_relation_write_observer_v1(
        observe_provider_relation_write);
    provider_registry::install_planner_bridge_observer_v1(
        observe_provider_rel_paths);
    if (!provider_registry::initialize_provider_registry_v1()) {
        ereport(WARNING,
            (errmsg("bioseq_cache provider rendezvous is owned by another library")));
    }
    if (process_shared_preload_libraries_in_progress) {
        g_preload_requested = true;
        if (!maintenance::register_static_janitor_worker()) {
            ereport(WARNING,
                (errmsg("bioseq_cache could not register its janitor worker")));
        }
        g_previous_shmem_request_hook = shmem_request_hook;
        shmem_request_hook = cache_shmem_request;
        g_previous_shmem_startup_hook = shmem_startup_hook;
        shmem_startup_hook = cache_shmem_startup;
    }
    install_cache_hooks();
    if (!g_callbacks_registered) {
        RegisterXactCallback(cache_transaction_callback, nullptr);
        RegisterSubXactCallback(cache_subtransaction_callback, nullptr);
        g_callbacks_registered = true;
    }
    RegisterCustomScanMethods(&cache_scan_methods());
    RegisterCustomScanMethods(&coverage_scan_methods());
}

void _PG_fini() {
    using namespace bioseq::cache::pg;
    maintenance::unregister_reusable_janitor_callback_v1(
        reusable_registry_janitor_sweep_v1);
    provider_registry::install_relation_write_observer_v1(nullptr);
    provider_registry::install_planner_bridge_observer_v1(nullptr);
    provider_registry::unpublish_provider_registry_v1();
}

}  // extern "C"
