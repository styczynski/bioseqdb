/**
 * @file bioseq_caching/core/reusable_registry.cppm
 * @brief Shared registry for exact query results and reusable cached sources.
 *
 * The PostgreSQL adapter allocates the region at server startup and
 * provides its locks. Shared records use offsets instead of process
 * pointers. Tests can use an aligned byte buffer and callbacks that
 * leave locking to the test.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.cache.core:reusable_registry;

import :coverage;
import :provider_validation;
import :state_machine;
import :ttl;
import :wire;

export namespace bioseq::cache {

inline constexpr std::uint32_t k_reusable_registry_magic = 0x42524347U;
inline constexpr std::uint32_t k_reusable_registry_abi_revision = 8;
inline constexpr std::uint32_t k_default_reusable_registry_generations = 1'024;
inline constexpr std::uint32_t k_default_reusable_registry_partitions = 16;
inline constexpr std::uint32_t k_default_reusable_registry_arena_bytes = 16U << 20;
inline constexpr std::uint32_t k_maximum_reusable_registry_generations = 65'536;
inline constexpr std::uint32_t k_maximum_reusable_janitor_batch = 64;
inline constexpr std::uint32_t k_reusable_reference_owners_per_generation = 64;
inline constexpr std::size_t k_maximum_reusable_janitor_liveness_observations =
    static_cast<std::size_t>(k_maximum_reusable_janitor_batch) *
    (static_cast<std::size_t>(k_reusable_reference_owners_per_generation) + 1U);
inline constexpr std::uint32_t k_maximum_reusable_invalidation_scan = 64;
inline constexpr std::uint32_t k_reusable_display_sql_capacity = 1U << 20;
inline constexpr std::uint32_t k_reusable_canonical_plan_capacity = 4U << 20;
inline constexpr std::uint32_t k_reusable_producer_plan_capacity = 4U << 20;
inline constexpr std::uint32_t k_invalid_reusable_registry_slot =
    (std::numeric_limits<std::uint32_t>::max)();

/** Registry errors that let the adapter fall back to normal query execution. */
enum class reusable_registry_error : std::uint32_t {
    none = 0,
    invalid_argument,
    invalid_configuration,
    region_too_small,
    region_misaligned,
    not_initialized,
    abi_mismatch,
    corrupt_layout,
    corrupt_directory,
    corrupt_generation,
    directory_full,
    global_generation_limit,
    exact_bucket_full,
    coverage_bucket_full,
    byte_arena_full,
    key_too_large,
    descriptor_too_large,
    not_found,
    stale_reference,
    invalid_state,
    counter_overflow,
    counter_underflow,
    reference_slots_full,
    manifest_mismatch,
    no_reclaimable_generation,
};

/** Registry capacities fixed at server startup, before shared memory is allocated. */
struct reusable_registry_config {
    std::uint32_t max_generations = k_default_reusable_registry_generations;
    std::uint32_t exact_directory_capacity = 0;
    std::uint32_t coverage_directory_capacity = 0;
    std::uint32_t partition_count = k_default_reusable_registry_partitions;
    std::uint32_t byte_arena_bytes = k_default_reusable_registry_arena_bytes;
    std::uint32_t reserved = 0;
};

/** Byte offsets and total size used to allocate the registry's shared region. */
struct reusable_registry_layout {
    std::uint32_t total_bytes = 0;
    std::uint32_t exact_directory_offset = 0;
    std::uint32_t coverage_directory_offset = 0;
    std::uint32_t generations_offset = 0;
    std::uint32_t free_extents_offset = 0;
    std::uint32_t byte_arena_offset = 0;
    std::uint32_t free_extent_capacity = 0;
    std::uint32_t reserved = 0;
};

/** Shared-memory layout and lock counts requested from the PostgreSQL adapter. */
struct reusable_registry_preload_request {
    reusable_registry_config config{};
    reusable_registry_layout layout{};
    std::uint32_t membership_lock_count = 1;
    std::uint32_t exact_partition_lock_count = 0;
    std::uint32_t coverage_partition_lock_count = 0;
    std::uint32_t reserved = 0;
};

/** Separates cache entries by database, user and security settings before applying the per-domain entry limit. */
struct reusable_security_namespace {
    std::uint32_t database_oid = 0;
    std::uint32_t current_role_oid = 0;
    std::uint32_t session_user_oid = 0;
    std::uint32_t security_context_flags = 0;
    std::int32_t row_security_mode = 0;
    std::uint32_t reserved = 0;
    cache_digest_256 provider_security_digest{};

    [[nodiscard]] bool operator==(const reusable_security_namespace&) const noexcept = default;
};

/** Security digest used when no provider-specific security state contributes to the key. */
inline constexpr cache_digest_256 k_provider_independent_security_digest{};

/** Checks that database and role IDs are set, row security is Boolean, and the reserved field is zero. */
[[nodiscard]] constexpr bool
valid_reusable_security_namespace(const reusable_security_namespace& value) noexcept {
    return value.database_oid != 0 && value.current_role_oid != 0 && value.session_user_oid != 0 &&
           (value.row_security_mode == 0 || value.row_security_mode == 1) && value.reserved == 0;
}

/** Identifies a reusable domain by security settings and digest. Lookup also compares the full domain key. */
struct reusable_coverage_directory_key {
    reusable_security_namespace security_namespace{};
    cache_digest_256 coverage_domain_digest{};

    [[nodiscard]] bool operator==(const reusable_coverage_directory_key&) const noexcept = default;
};

/** Storage manifest identity copied into the registry without a storage-module dependency. */
struct reusable_manifest_handle {
    std::uint32_t slot = k_invalid_reusable_registry_slot;
    std::uint32_t reserved = 0;
    std::uint64_t slot_sequence = 0;
    cache_entry_id id{};

    [[nodiscard]] bool operator==(const reusable_manifest_handle&) const noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return slot != k_invalid_reusable_registry_slot && slot_sequence != 0 &&
               id.boot_nonce != 0 && id.generation != 0 && id.claim_sequence != 0;
    }
};

/** Backend identity used to check whether a cache producer is still running. */
struct reusable_producer_identity {
    std::uint32_t proc_number = k_invalid_reusable_registry_slot;
    std::int32_t pid = 0;
    std::uint64_t backend_nonce = 0;
    std::int64_t backend_started_at_us = 0;

    [[nodiscard]] bool valid() const noexcept {
        return proc_number != k_invalid_reusable_registry_slot && pid > 0 && backend_nonce != 0 &&
               backend_started_at_us != 0;
    }

    [[nodiscard]] bool operator==(const reusable_producer_identity&) const noexcept = default;
};

/** Indicates whether producer-plan text is missing, planned, or includes execution measurements. */
enum class reusable_plan_snapshot_status : std::uint8_t {
    unavailable = 0,
    planned = 1,
    analyzed = 2,
};

/** Backend identity shared by waiter, planner and reader references. */
using reusable_reference_owner = reusable_producer_identity;

/** Byte range measured from the start of the shared region. Zero offsets are invalid. */
struct reusable_shared_byte_range {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return offset != 0 && length != 0;
    }
};

/** Provider, schema and dependency information saved with a cached generation, plus its size and expiry policy. */
struct reusable_generation_metadata {
    std::uint32_t provider_id = 0;
    std::uint32_t provider_abi_revision = 0;
    std::uint32_t provider_domain_abi_revision = 0;
    std::uint32_t dependency_count = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t capability_mask = 0;
    cache_digest_256 schema_fingerprint{};
    cache_digest_256 mapping_fingerprint{};
    cache_digest_256 lineage_fingerprint{};
    cache_digest_256 dependency_fingerprint{};
    physical_identity dependency_physical_identity{};
    write_observability observability = write_observability::database_wide_fallback;
    std::uint32_t reserved = 0;
    std::uint64_t estimated_covered_rows = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t producer_ttl_ms = 0;
};

/**
 * Adds a database-wide dependency check alongside the plan's individual
 * dependencies. It is counted separately from the plan dependency list.
 * The completion counter detects writes during production. A completed
 * cache hit checks the saved dependency against the consumer's snapshot.
 */
struct reusable_conservative_guard {
    dependency_vector_entry dependency{};
    std::uint64_t snapshot_completion_count = 0;
    bool active = false;
    std::array<std::uint8_t, 7> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        const bool completion_valid = database_guard_requires_completion(dependency)
                                          ? snapshot_completion_count != 0
                                          : snapshot_completion_count == 0;
        return active && completion_valid && database_guard_entry_valid(dependency) &&
               std::all_of(reserved.begin(), reserved.end(), [](std::uint8_t value) noexcept {
                   return value == 0;
               });
    }

    [[nodiscard]] bool operator==(const reusable_conservative_guard&) const noexcept = default;
};

/** Identifies a cache generation by slot and sequence, letting lookups reject references to an earlier use of the slot. */
struct reusable_generation_reference {
    cache_entry_id id{};
    std::uint32_t generation_slot = k_invalid_reusable_registry_slot;
    std::uint32_t reserved = 0;
    std::uint64_t slot_sequence = 0;

    [[nodiscard]] bool operator==(const reusable_generation_reference&) const noexcept = default;

    [[nodiscard]] explicit operator bool() const noexcept {
        return generation_slot != k_invalid_reusable_registry_slot && slot_sequence != 0 &&
               id.boot_nonce != 0 && id.generation != 0 && id.claim_sequence != 0;
    }
};

/** SQL and plan text supplied by the producer for diagnostics. The text does not affect cache identity. */
struct reusable_diagnostic_text_update {
    reusable_generation_reference reference{};
    reusable_producer_identity producer{};
    std::span<const char> display_sql{};
    std::span<const char> canonical_plan{};
    std::span<const char> producer_plan{};
    reusable_plan_snapshot_status canonical_plan_status =
        reusable_plan_snapshot_status::unavailable;
    reusable_plan_snapshot_status plan_status = reusable_plan_snapshot_status::unavailable;
    bool display_sql_truncated = false;
    bool canonical_plan_truncated = false;
    bool producer_plan_truncated = false;
    std::array<std::uint8_t, 3> reserved{};
};

/** Text lengths, plan statuses and truncation flags returned when copying diagnostics. */
struct reusable_diagnostic_text_copy {
    reusable_plan_snapshot_status canonical_plan_status =
        reusable_plan_snapshot_status::unavailable;
    reusable_plan_snapshot_status plan_status = reusable_plan_snapshot_status::unavailable;
    bool display_sql_truncated = false;
    bool canonical_plan_truncated = false;
    bool producer_plan_truncated = false;
    std::array<std::uint8_t, 3> reserved{};
    std::uint32_t display_sql_bytes = 0;
    std::uint32_t canonical_plan_bytes = 0;
    std::uint32_t producer_plan_bytes = 0;
};

enum class reusable_reference_kind : std::uint8_t {
    none = 0,
    waiter = 1,
    planner_pin = 2,
    reader_pin = 3,
};

/** Records a reference owner in shared memory. Its sequence distinguishes successive uses of the same slot. */
struct reusable_shared_reference_owner {
    reusable_reference_owner owner{};
    std::uint64_t owner_sequence = 0;
    reusable_reference_kind kind = reusable_reference_kind::none;
    bool active = false;
    std::array<std::uint8_t, 6> reserved{};
};

/** Backend-local token identifying one owned waiter, planner or reader reference. */
struct reusable_reference_token {
    reusable_generation_reference reference{};
    reusable_reference_owner owner{};
    std::uint64_t owner_sequence = 0;
    std::uint32_t owner_slot = k_invalid_reusable_registry_slot;
    reusable_reference_kind kind = reusable_reference_kind::none;
    bool active = false;
    std::array<std::uint8_t, 2> reserved{};

    [[nodiscard]] bool operator==(const reusable_reference_token&) const noexcept = default;
};

/** Snapshot of the waiters attached to one cache generation. */
struct reusable_waiter_owner_batch {
    std::array<reusable_reference_token, k_reusable_reference_owners_per_generation> waiters{};
    reusable_registry_error error = reusable_registry_error::none;
    std::uint32_t count = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == reusable_registry_error::none && count <= waiters.size();
    }
};

/**
 * Saves a producer or reference owner for a liveness check after
 * registry locks are released. The generation and owner sequences let
 * cleanup detect a slot that has since been reused.
 */
struct reusable_janitor_owner_observation {
    reusable_generation_reference reference{};
    reusable_reference_owner owner{};
    reusable_reference_token reference_token{};
    cache_entry_kind entry_kind = cache_entry_kind::reusable_coverage;
    bool producer = false;
    std::array<std::uint8_t, 6> reserved{};

    /** Checks that the observation contains a complete producer identity or reference token of a recognized kind. */
    [[nodiscard]] bool valid() const noexcept {
        const bool known_kind = entry_kind == cache_entry_kind::exact_final_result ||
                                entry_kind == cache_entry_kind::reusable_coverage;
        const bool reserved_clear =
            std::all_of(reserved.begin(), reserved.end(), [](std::uint8_t value) noexcept {
                return value == 0;
            });
        const bool token_reserved_clear =
            std::all_of(reference_token.reserved.begin(),
                        reference_token.reserved.end(),
                        [](std::uint8_t value) noexcept { return value == 0; });
        if (!known_kind || !reference || reference.reserved != 0 || !owner.valid() ||
            !reserved_clear || !token_reserved_clear) {
            return false;
        }
        if (producer) {
            return !reference_token.active && !reference_token.reference &&
                   !reference_token.owner.valid() && reference_token.owner_sequence == 0 &&
                   reference_token.owner_slot == k_invalid_reusable_registry_slot &&
                   reference_token.kind == reusable_reference_kind::none;
        }
        return reference_token.active && reference_token.reference == reference &&
               reference_token.owner == owner && reference_token.owner_sequence != 0 &&
               reference_token.owner_slot < k_reusable_reference_owners_per_generation &&
               (reference_token.kind == reusable_reference_kind::waiter ||
                reference_token.kind == reusable_reference_kind::planner_pin ||
                reference_token.kind == reusable_reference_kind::reader_pin);
    }

    [[nodiscard]] bool
    operator==(const reusable_janitor_owner_observation&) const noexcept = default;
};

enum class reusable_record_membership : std::uint32_t {
    free = 0,
    indexed = 1,
    retired = 2,
};

/** Tracks whether a cache generation still owns dependency references or has begun releasing them. */
enum class reusable_dependency_release_state : std::uint8_t {
    none = 0,
    retained = 1,
    pending = 2,
    prepared = 3,
    released = 4,
};

/** Hash-table bucket for generations with the same digest. Full key bytes distinguish hash collisions. */
struct reusable_exact_bucket {
    directory_slot_state slot_state = directory_slot_state::empty;
    std::uint32_t reserved = 0;
    cache_digest_256 digest{};
    std::uint32_t first_generation = k_invalid_reusable_registry_slot;
    std::uint32_t generation_count = 0;
    std::uint32_t terminal_generation_count = 0;
    std::uint32_t reserved2 = 0;
};

/** Hash-table bucket for a reusable domain and its security identity. */
struct reusable_coverage_bucket {
    directory_slot_state slot_state = directory_slot_state::empty;
    std::uint32_t reserved = 0;
    reusable_coverage_directory_key key{};
    std::uint32_t first_generation = k_invalid_reusable_registry_slot;
    std::uint32_t generation_count = 0;
    std::uint32_t terminal_generation_count = 0;
    std::uint32_t reserved2 = 0;
};

/** Free byte range in the registry arena. The allocator keeps these ranges sorted and merges neighbors. */
struct reusable_free_extent {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

/** Shared cache-generation record. Variable-length data are stored as offsets and lengths within the region. */
struct reusable_shared_generation {
    reusable_record_membership membership = reusable_record_membership::free;
    cache_entry_kind entry_kind = cache_entry_kind::reusable_coverage;
    cache_entry_state state = cache_entry_state::absent;
    cache_failure_code failure = cache_failure_code::none;
    std::uint32_t next_in_exact_bucket = k_invalid_reusable_registry_slot;
    std::uint32_t next_in_coverage_bucket = k_invalid_reusable_registry_slot;
    std::uint32_t next_retired_or_free = k_invalid_reusable_registry_slot;
    std::uint32_t reserved = 0;
    std::uint64_t slot_sequence = 0;
    cache_entry_id id{};
    reusable_coverage_directory_key coverage_directory_key{};
    reusable_shared_byte_range canonical_generation_key{};
    reusable_shared_byte_range canonical_domain_key{};
    reusable_shared_byte_range coverage_descriptor{};
    reusable_shared_byte_range dependency_vector{};
    reusable_conservative_guard conservative_guard{};
    std::uint32_t allocation_offset = 0;
    std::uint32_t allocation_length = 0;
    std::uint32_t dependency_release_cursor = 0;
    reusable_dependency_release_state dependency_release_state =
        reusable_dependency_release_state::none;
    std::array<std::uint8_t, 3> reserved_dependency_release{};
    std::uint64_t dependency_release_sequence = 0;
    reusable_producer_identity producer{};
    std::uint32_t waiter_count = 0;
    std::uint32_t planner_pin_count = 0;
    std::uint32_t reader_pin_count = 0;
    std::uint32_t reserved_count = 0;
    std::array<reusable_shared_reference_owner, k_reusable_reference_owners_per_generation>
        reference_owners{};
    std::uint64_t started_monotonic_ns = 0;
    std::uint64_t completed_monotonic_ns = 0;
    std::uint64_t last_access_monotonic_ns = 0;
    std::uint64_t row_count = 0;
    reusable_generation_metadata metadata{};
    reusable_manifest_handle manifest{};
    reusable_plan_snapshot_status canonical_plan_snapshot_status =
        reusable_plan_snapshot_status::unavailable;
    reusable_plan_snapshot_status plan_snapshot_status = reusable_plan_snapshot_status::unavailable;
    bool display_sql_truncated = false;
    bool canonical_plan_truncated = false;
    bool producer_plan_truncated = false;
    std::array<std::uint8_t, 3> reserved_diagnostic{};
    reusable_shared_byte_range display_sql{};
    reusable_shared_byte_range canonical_plan{};
    reusable_shared_byte_range producer_plan{};
};

/** Header at the start of the region, with offsets to the other shared structures. */
struct reusable_registry_header {
    std::uint32_t magic = 0;
    std::uint32_t abi_revision = 0;
    bootstrap_state bootstrap_status = bootstrap_state::uninitialized;
    std::uint32_t reserved = 0;
    reusable_registry_config config{};
    reusable_registry_layout layout{};
    std::uint64_t boot_nonce = 0;
    std::uint64_t next_generation = 1;
    std::uint64_t next_claim_sequence = 1;
    std::uint64_t next_slot_sequence = 1;
    std::uint64_t next_reference_sequence = 1;
    std::uint64_t next_dependency_release_sequence = 1;
    std::uint32_t free_generation_head = k_invalid_reusable_registry_slot;
    std::uint32_t retired_generation_head = k_invalid_reusable_registry_slot;
    std::uint32_t free_extent_count = 0;
    std::uint32_t janitor_scan_cursor = 0;
    std::uint32_t indexed_generations = 0;
    std::uint32_t retired_generations = 0;
    std::uint32_t exact_occupied_buckets = 0;
    std::uint32_t coverage_occupied_buckets = 0;
    std::uint32_t evictions = 0;
    std::uint32_t invalidations = 0;
};

/** Process-local view of the shared registry bytes. */
struct reusable_registry_attachment {
    std::span<std::byte> region{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return !region.empty();
    }
};

/** Callbacks for exact-result and coverage partition locks. The caller acquires the membership lock first. */
struct reusable_registry_partition_callbacks {
    void* context = nullptr;
    void (*acquire_exact)(void*, std::uint32_t) noexcept = nullptr;
    void (*release_exact)(void*, std::uint32_t) noexcept = nullptr;
    void (*acquire_coverage)(void*, std::uint32_t) noexcept = nullptr;
    void (*release_coverage)(void*, std::uint32_t) noexcept = nullptr;

    [[nodiscard]] bool valid() const noexcept {
        const bool none = acquire_exact == nullptr && release_exact == nullptr &&
                          acquire_coverage == nullptr && release_coverage == nullptr;
        const bool complete = acquire_exact != nullptr && release_exact != nullptr &&
                              acquire_coverage != nullptr && release_coverage != nullptr;
        return none || complete;
    }
};

namespace reusable_registry_detail {

[[nodiscard]] constexpr bool power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] constexpr std::uint32_t next_power_of_two(std::uint32_t value) noexcept {
    if (value <= 1) {
        return 1;
    }
    --value;
    value |= value >> 1U;
    value |= value >> 2U;
    value |= value >> 4U;
    value |= value >> 8U;
    value |= value >> 16U;
    return value + 1U;
}

[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t value,
                                               std::uint64_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0 ||
        value > (std::numeric_limits<std::uint64_t>::max)() - (alignment - 1U)) {
        return 0;
    }
    return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] inline std::uint64_t mix_hash_byte(std::uint64_t hash, std::uint8_t value) noexcept {
    hash ^= value;
    return hash * 1'099'511'628'211ULL;
}

[[nodiscard]] inline std::uint64_t mix_hash_u32(std::uint64_t hash, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        hash = mix_hash_byte(hash, static_cast<std::uint8_t>(value >> (index * 8U)));
    }
    return hash;
}

[[nodiscard]] inline std::uint64_t digest_hash(const cache_digest_256& digest) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    for (const std::uint8_t value : digest.bytes) {
        hash = mix_hash_byte(hash, value);
    }
    return hash;
}

[[nodiscard]] inline std::uint64_t
coverage_key_hash(const reusable_coverage_directory_key& key) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    hash = mix_hash_u32(hash, key.security_namespace.database_oid);
    hash = mix_hash_u32(hash, key.security_namespace.current_role_oid);
    hash = mix_hash_u32(hash, key.security_namespace.session_user_oid);
    hash = mix_hash_u32(hash, key.security_namespace.security_context_flags);
    hash = mix_hash_u32(hash, static_cast<std::uint32_t>(key.security_namespace.row_security_mode));
    for (const std::uint8_t value : key.security_namespace.provider_security_digest.bytes) {
        hash = mix_hash_byte(hash, value);
    }
    for (const std::uint8_t value : key.coverage_domain_digest.bytes) {
        hash = mix_hash_byte(hash, value);
    }
    return hash;
}

template <class p_value_type>
[[nodiscard]] inline p_value_type* array_at(reusable_registry_attachment& attachment,
                                            std::uint32_t offset) noexcept {
    return reinterpret_cast<p_value_type*>(attachment.region.data() + offset);
}

template <class p_value_type>
[[nodiscard]] inline const p_value_type*
array_at(const reusable_registry_attachment& attachment, std::uint32_t offset) noexcept {
    return reinterpret_cast<const p_value_type*>(attachment.region.data() + offset);
}

[[nodiscard]] inline reusable_registry_header&
header(reusable_registry_attachment& attachment) noexcept {
    return *reinterpret_cast<reusable_registry_header*>(attachment.region.data());
}

[[nodiscard]] inline const reusable_registry_header&
header(const reusable_registry_attachment& attachment) noexcept {
    return *reinterpret_cast<const reusable_registry_header*>(attachment.region.data());
}

[[nodiscard]] inline reusable_exact_bucket*
exact_buckets(reusable_registry_attachment& attachment) noexcept {
    return array_at<reusable_exact_bucket>(attachment,
                                           header(attachment).layout.exact_directory_offset);
}

[[nodiscard]] inline const reusable_exact_bucket*
exact_buckets(const reusable_registry_attachment& attachment) noexcept {
    return array_at<reusable_exact_bucket>(attachment,
                                           header(attachment).layout.exact_directory_offset);
}

[[nodiscard]] inline reusable_coverage_bucket*
coverage_buckets(reusable_registry_attachment& attachment) noexcept {
    return array_at<reusable_coverage_bucket>(attachment,
                                              header(attachment).layout.coverage_directory_offset);
}

[[nodiscard]] inline const reusable_coverage_bucket*
coverage_buckets(const reusable_registry_attachment& attachment) noexcept {
    return array_at<reusable_coverage_bucket>(attachment,
                                              header(attachment).layout.coverage_directory_offset);
}

[[nodiscard]] inline reusable_shared_generation*
generations(reusable_registry_attachment& attachment) noexcept {
    return array_at<reusable_shared_generation>(attachment,
                                                header(attachment).layout.generations_offset);
}

[[nodiscard]] inline const reusable_shared_generation*
generations(const reusable_registry_attachment& attachment) noexcept {
    return array_at<reusable_shared_generation>(attachment,
                                                header(attachment).layout.generations_offset);
}

[[nodiscard]] inline reusable_free_extent*
free_extents(reusable_registry_attachment& attachment) noexcept {
    return array_at<reusable_free_extent>(attachment,
                                          header(attachment).layout.free_extents_offset);
}

[[nodiscard]] inline const reusable_free_extent*
free_extents(const reusable_registry_attachment& attachment) noexcept {
    return array_at<reusable_free_extent>(attachment,
                                          header(attachment).layout.free_extents_offset);
}

[[nodiscard]] inline bool range_valid(const reusable_registry_attachment& attachment,
                                      reusable_shared_byte_range range) noexcept {
    if (!range) {
        return false;
    }
    const reusable_registry_header& shared = header(attachment);
    const std::uint64_t start = range.offset;
    const std::uint64_t end = start + range.length;
    const std::uint64_t arena_start = shared.layout.byte_arena_offset;
    const std::uint64_t arena_end = arena_start + shared.config.byte_arena_bytes;
    return start >= arena_start && end >= start && end <= arena_end &&
           end <= shared.layout.total_bytes;
}

[[nodiscard]] inline bool range_equal(const reusable_registry_attachment& attachment,
                                      reusable_shared_byte_range stored,
                                      std::span<const std::byte> requested) noexcept {
    return range_valid(attachment, stored) && stored.length == requested.size() &&
           std::memcmp(
               attachment.region.data() + stored.offset, requested.data(), requested.size()) == 0;
}

[[nodiscard]] inline bool terminal_state(cache_entry_state state) noexcept {
    return state == cache_entry_state::complete || state == cache_entry_state::failed ||
           state == cache_entry_state::invalidated;
}

/** Recognizes exact-result and reusable-coverage entries. */
[[nodiscard]] constexpr bool known_entry_kind(cache_entry_kind kind) noexcept {
    return kind == cache_entry_kind::exact_final_result ||
           kind == cache_entry_kind::reusable_coverage;
}

/** Reports whether an entry belongs in the reusable-domain directory. */
[[nodiscard]] constexpr bool has_coverage_membership(cache_entry_kind kind) noexcept {
    return kind == cache_entry_kind::reusable_coverage;
}

[[nodiscard]] inline bool reference_matches(const reusable_shared_generation& generation,
                                            const reusable_generation_reference& reference,
                                            bool allow_retired = false) noexcept {
    const bool membership_ok =
        generation.membership == reusable_record_membership::indexed ||
        (allow_retired && generation.membership == reusable_record_membership::retired);
    return membership_ok && generation.slot_sequence == reference.slot_sequence &&
           generation.id == reference.id;
}

inline void acquire_exact(const reusable_registry_partition_callbacks& callbacks,
                          std::uint32_t partition) noexcept {
    if (callbacks.acquire_exact != nullptr) {
        callbacks.acquire_exact(callbacks.context, partition);
    }
}

inline void release_exact(const reusable_registry_partition_callbacks& callbacks,
                          std::uint32_t partition) noexcept {
    if (callbacks.release_exact != nullptr) {
        callbacks.release_exact(callbacks.context, partition);
    }
}

inline void acquire_coverage(const reusable_registry_partition_callbacks& callbacks,
                             std::uint32_t partition) noexcept {
    if (callbacks.acquire_coverage != nullptr) {
        callbacks.acquire_coverage(callbacks.context, partition);
    }
}

inline void release_coverage(const reusable_registry_partition_callbacks& callbacks,
                             std::uint32_t partition) noexcept {
    if (callbacks.release_coverage != nullptr) {
        callbacks.release_coverage(callbacks.context, partition);
    }
}

} // namespace reusable_registry_detail

/** Calculates the shared-memory and lock requirements, filling in default directory capacities. */
[[nodiscard]] inline std::expected<reusable_registry_preload_request, reusable_registry_error>
make_reusable_registry_preload_request(reusable_registry_config requested = {}) noexcept {
    using namespace reusable_registry_detail;
    if (requested.reserved != 0 || requested.max_generations == 0 ||
        requested.max_generations > k_maximum_reusable_registry_generations ||
        requested.partition_count == 0 || !power_of_two(requested.partition_count) ||
        requested.partition_count > 256 || requested.byte_arena_bytes == 0) {
        return std::unexpected(reusable_registry_error::invalid_configuration);
    }

    const std::uint64_t minimum_capacity64 =
        static_cast<std::uint64_t>(requested.max_generations) * 2U;
    const std::uint64_t minimum_partitioned64 =
        static_cast<std::uint64_t>(requested.partition_count) * 8U;
    const std::uint64_t minimum64 = std::max(minimum_capacity64, minimum_partitioned64);
    if (minimum64 > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(reusable_registry_error::invalid_configuration);
    }
    const std::uint32_t minimum = static_cast<std::uint32_t>(minimum64);
    if (requested.exact_directory_capacity == 0) {
        requested.exact_directory_capacity = next_power_of_two(minimum);
    }
    if (requested.coverage_directory_capacity == 0) {
        requested.coverage_directory_capacity = next_power_of_two(minimum);
    }
    const bool directory_valid =
        power_of_two(requested.exact_directory_capacity) &&
        power_of_two(requested.coverage_directory_capacity) &&
        requested.exact_directory_capacity >= minimum &&
        requested.coverage_directory_capacity >= minimum &&
        requested.exact_directory_capacity % requested.partition_count == 0 &&
        requested.coverage_directory_capacity % requested.partition_count == 0 &&
        power_of_two(requested.exact_directory_capacity / requested.partition_count) &&
        power_of_two(requested.coverage_directory_capacity / requested.partition_count);
    if (!directory_valid) {
        return std::unexpected(reusable_registry_error::invalid_configuration);
    }

    reusable_registry_layout layout{};
    std::uint64_t cursor =
        align_up(sizeof(reusable_registry_header), alignof(reusable_exact_bucket));
    layout.exact_directory_offset = static_cast<std::uint32_t>(cursor);
    cursor = align_up(cursor + static_cast<std::uint64_t>(requested.exact_directory_capacity) *
                                   sizeof(reusable_exact_bucket),
                      alignof(reusable_coverage_bucket));
    layout.coverage_directory_offset = static_cast<std::uint32_t>(cursor);
    cursor = align_up(cursor + static_cast<std::uint64_t>(requested.coverage_directory_capacity) *
                                   sizeof(reusable_coverage_bucket),
                      alignof(reusable_shared_generation));
    layout.generations_offset = static_cast<std::uint32_t>(cursor);
    cursor = align_up(cursor + static_cast<std::uint64_t>(requested.max_generations) *
                                   sizeof(reusable_shared_generation),
                      alignof(reusable_free_extent));
    layout.free_extents_offset = static_cast<std::uint32_t>(cursor);
    const std::uint64_t free_extent_capacity64 =
        static_cast<std::uint64_t>(requested.max_generations) * 4U + 4U;
    if (free_extent_capacity64 > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(reusable_registry_error::invalid_configuration);
    }
    layout.free_extent_capacity = static_cast<std::uint32_t>(free_extent_capacity64);
    cursor = align_up(cursor + static_cast<std::uint64_t>(layout.free_extent_capacity) *
                                   sizeof(reusable_free_extent),
                      8U);
    layout.byte_arena_offset = static_cast<std::uint32_t>(cursor);
    cursor = align_up(cursor + requested.byte_arena_bytes, 8U);
    if (cursor == 0 || cursor > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(reusable_registry_error::invalid_configuration);
    }
    layout.total_bytes = static_cast<std::uint32_t>(cursor);

    return reusable_registry_preload_request{
        .config = requested,
        .layout = layout,
        .membership_lock_count = 1,
        .exact_partition_lock_count = requested.partition_count,
        .coverage_partition_lock_count = requested.partition_count,
    };
}

/** Checks the shared header and layout before accessing directory records. */
[[nodiscard]] inline reusable_registry_error
validate_reusable_registry_attachment(const reusable_registry_attachment& attachment) noexcept {
    if (attachment.region.size() < sizeof(reusable_registry_header)) {
        return reusable_registry_error::region_too_small;
    }
    if (reinterpret_cast<std::uintptr_t>(attachment.region.data()) %
            alignof(reusable_registry_header) !=
        0) {
        return reusable_registry_error::region_misaligned;
    }
    const reusable_registry_header& shared = reusable_registry_detail::header(attachment);
    if (shared.magic != k_reusable_registry_magic) {
        return reusable_registry_error::not_initialized;
    }
    if (shared.abi_revision != k_reusable_registry_abi_revision) {
        return reusable_registry_error::abi_mismatch;
    }
    if (shared.bootstrap_status != bootstrap_state::ready) {
        return reusable_registry_error::not_initialized;
    }
    const auto expected = make_reusable_registry_preload_request(shared.config);
    if (!expected.has_value() ||
        std::memcmp(&expected->layout, &shared.layout, sizeof(shared.layout)) != 0 ||
        shared.layout.total_bytes > attachment.region.size() || shared.boot_nonce == 0 ||
        shared.next_generation == 0 || shared.next_claim_sequence == 0 ||
        shared.next_slot_sequence == 0 || shared.next_reference_sequence == 0 ||
        shared.next_dependency_release_sequence == 0 ||
        shared.free_extent_count > shared.layout.free_extent_capacity ||
        shared.janitor_scan_cursor >= shared.config.max_generations ||
        shared.indexed_generations > shared.config.max_generations ||
        shared.retired_generations > shared.config.max_generations ||
        shared.indexed_generations + shared.retired_generations > shared.config.max_generations) {
        return reusable_registry_error::corrupt_layout;
    }
    return reusable_registry_error::none;
}

/** Validates an initialized region and returns this process's view of its registry bytes. */
[[nodiscard]] inline std::expected<reusable_registry_attachment, reusable_registry_error>
attach_reusable_registry(std::span<std::byte> region) noexcept {
    reusable_registry_attachment attachment{.region = region};
    const reusable_registry_error error = validate_reusable_registry_attachment(attachment);
    if (error != reusable_registry_error::none) {
        return std::unexpected(error);
    }
    attachment.region =
        attachment.region.first(reusable_registry_detail::header(attachment).layout.total_bytes);
    return attachment;
}

/** Initializes a new registry region or attaches to an existing one with matching startup settings. */
[[nodiscard]] inline std::expected<reusable_registry_attachment, reusable_registry_error>
startup_reusable_registry(std::span<std::byte> region,
                          std::uint64_t boot_nonce,
                          reusable_registry_config config = {},
                          bool found = false) noexcept {
    const auto request = make_reusable_registry_preload_request(config);
    if (!request.has_value()) {
        return std::unexpected(request.error());
    }
    if (boot_nonce == 0 || region.size() < request->layout.total_bytes) {
        return std::unexpected(boot_nonce == 0 ? reusable_registry_error::invalid_argument
                                               : reusable_registry_error::region_too_small);
    }
    if (reinterpret_cast<std::uintptr_t>(region.data()) % alignof(reusable_registry_header) != 0) {
        return std::unexpected(reusable_registry_error::region_misaligned);
    }
    if (found) {
        auto attached = attach_reusable_registry(region);
        if (!attached.has_value() ||
            reusable_registry_detail::header(*attached).boot_nonce != boot_nonce ||
            std::memcmp(&reusable_registry_detail::header(*attached).config,
                        &request->config,
                        sizeof(request->config)) != 0) {
            return std::unexpected(attached.has_value() ? reusable_registry_error::abi_mismatch
                                                        : attached.error());
        }
        return attached;
    }

    std::memset(region.data(), 0, request->layout.total_bytes);
    reusable_registry_attachment attachment{
        .region = region.first(request->layout.total_bytes),
    };
    reusable_registry_header& shared = reusable_registry_detail::header(attachment);
    shared.magic = k_reusable_registry_magic;
    shared.abi_revision = k_reusable_registry_abi_revision;
    shared.bootstrap_status = bootstrap_state::initializing;
    shared.config = request->config;
    shared.layout = request->layout;
    shared.boot_nonce = boot_nonce;
    shared.next_generation = 1;
    shared.next_claim_sequence = 1;
    shared.next_slot_sequence = 1;
    shared.next_reference_sequence = 1;
    shared.next_dependency_release_sequence = 1;
    shared.free_generation_head = 0;
    shared.retired_generation_head = k_invalid_reusable_registry_slot;
    shared.free_extent_count = 1;

    reusable_exact_bucket* exact = reusable_registry_detail::exact_buckets(attachment);
    for (std::uint32_t bucket = 0; bucket < shared.config.exact_directory_capacity; ++bucket) {
        exact[bucket] = reusable_exact_bucket{};
    }
    reusable_coverage_bucket* coverage = reusable_registry_detail::coverage_buckets(attachment);
    for (std::uint32_t bucket = 0; bucket < shared.config.coverage_directory_capacity; ++bucket) {
        coverage[bucket] = reusable_coverage_bucket{};
    }
    reusable_shared_generation* records = reusable_registry_detail::generations(attachment);
    for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
        records[slot] = reusable_shared_generation{};
        records[slot].next_retired_or_free = slot + 1U < shared.config.max_generations
                                                 ? slot + 1U
                                                 : k_invalid_reusable_registry_slot;
    }
    reusable_free_extent* extents = reusable_registry_detail::free_extents(attachment);
    extents[0] = reusable_free_extent{
        .offset = shared.layout.byte_arena_offset,
        .length = shared.config.byte_arena_bytes,
    };
    shared.bootstrap_status = bootstrap_state::ready;
    return attachment;
}

/** Returns the exact-result partition selected by the cache-key digest. */
[[nodiscard]] inline std::uint32_t
reusable_exact_partition(const reusable_registry_attachment& attachment,
                         const cache_digest_256& digest) noexcept {
    return static_cast<std::uint32_t>(
        reusable_registry_detail::digest_hash(digest) &
        (reusable_registry_detail::header(attachment).config.partition_count - 1U));
}

/** Returns the coverage partition selected by the domain digest and security identity. */
[[nodiscard]] inline std::uint32_t
reusable_coverage_partition(const reusable_registry_attachment& attachment,
                            const reusable_coverage_directory_key& key) noexcept {
    return static_cast<std::uint32_t>(
        reusable_registry_detail::coverage_key_hash(key) &
        (reusable_registry_detail::header(attachment).config.partition_count - 1U));
}

/** Returns a view of shared bytes. The caller must hold the corresponding reference while using them. */
[[nodiscard]] inline std::span<const std::byte>
reusable_shared_bytes(const reusable_registry_attachment& attachment,
                      reusable_shared_byte_range range) noexcept {
    if (!reusable_registry_detail::range_valid(attachment, range)) {
        return {};
    }
    return std::span<const std::byte>{
        attachment.region.data() + range.offset,
        range.length,
    };
}

/** Inputs for finding an existing result or claiming a new cache generation. */
struct reusable_claim_request {
    cache_entry_kind entry_kind = cache_entry_kind::reusable_coverage;
    reusable_coverage_directory_key coverage_directory_key{};
    cache_digest_256 generation_digest{};
    std::span<const std::byte> canonical_domain_key{};
    std::span<const std::byte> canonical_generation_key{};
    std::span<const std::byte> coverage_descriptor{};
    std::span<const std::byte> dependency_vector_envelope{};
    reusable_conservative_guard conservative_guard{};
    reusable_generation_metadata metadata{};
    reusable_producer_identity producer{};
    reusable_reference_owner reference_owner{};
    std::uint64_t started_monotonic_ns = 0;
    std::uint64_t consumer_ttl_ms = (std::numeric_limits<std::uint64_t>::max)();
    bool pin_complete = true;
    bool register_waiter = true;
};

enum class reusable_claim_disposition : std::uint8_t {
    rejected = 0,
    claimed_producer = 1,
    existing_in_flight = 2,
    existing_complete = 3,
};

struct reusable_claim_result {
    reusable_registry_error error = reusable_registry_error::none;
    reusable_claim_disposition disposition = reusable_claim_disposition::rejected;
    reusable_generation_reference reference{};
    reusable_reference_token token{};
    std::array<reusable_manifest_handle, 2> retired_manifests{};
    std::uint32_t retired_manifest_count = 0;
    std::uint32_t reserved = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == reusable_registry_error::none &&
               disposition != reusable_claim_disposition::rejected;
    }
};

/**
 * Identifies one attempt to release a retired generation's dependency
 * references after registry locks are dropped. The byte count and
 * checksum identify the copied dependency data. first_reference skips
 * references already released, with the database-wide dependency last.
 */
struct reusable_dependency_release_ticket {
    reusable_generation_reference reference{};
    reusable_manifest_handle manifest{};
    reusable_conservative_guard conservative_guard{};
    std::uint64_t release_sequence = 0;
    std::uint64_t payload_checksum = 0;
    std::uint32_t first_reference = 0;
    std::uint32_t reference_count = 0;
    std::uint32_t dependency_count = 0;
    std::uint32_t vector_bytes = 0;
    std::uint32_t reserved = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return reference && release_sequence != 0 && first_reference <= reference_count &&
               dependency_count <= k_maximum_dependencies_per_generation &&
               reference_count ==
                   dependency_count + static_cast<std::uint32_t>(conservative_guard.active) &&
               (conservative_guard.active ? static_cast<bool>(conservative_guard)
                                          : conservative_guard == reusable_conservative_guard{}) &&
               vector_bytes == dependency_vector_envelope_bytes(dependency_count) && reserved == 0;
    }

    [[nodiscard]] bool
    operator==(const reusable_dependency_release_ticket&) const noexcept = default;
};

/** Reports whether dependency-release work was claimed and how many bytes were copied or required. */
struct reusable_dependency_release_preparation {
    reusable_registry_error error = reusable_registry_error::none;
    reusable_dependency_release_ticket ticket{};
    std::uint32_t required_vector_bytes = 0;
    std::uint32_t copied_vector_bytes = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == reusable_registry_error::none && ticket &&
               copied_vector_bytes == required_vector_bytes;
    }
};

enum class reusable_lookup_mode : std::uint8_t {
    observe = 0,
    register_waiter = 1,
    pin_complete = 2,
};

/** Copy of a cache generation's state, using byte offsets for shared data. */
struct reusable_generation_snapshot {
    reusable_generation_reference reference{};
    cache_entry_kind entry_kind = cache_entry_kind::reusable_coverage;
    reusable_coverage_directory_key coverage_directory_key{};
    cache_entry_state state = cache_entry_state::absent;
    cache_failure_code failure = cache_failure_code::none;
    reusable_producer_identity producer{};
    reusable_generation_metadata metadata{};
    reusable_manifest_handle manifest{};
    reusable_shared_byte_range canonical_generation_key{};
    reusable_shared_byte_range canonical_domain_key{};
    reusable_shared_byte_range coverage_descriptor{};
    reusable_shared_byte_range dependency_vector{};
    reusable_conservative_guard conservative_guard{};
    std::uint32_t waiter_count = 0;
    std::uint32_t planner_pin_count = 0;
    std::uint32_t reader_pin_count = 0;
    std::uint32_t reserved = 0;
    std::uint64_t started_monotonic_ns = 0;
    std::uint64_t completed_monotonic_ns = 0;
    std::uint64_t last_access_monotonic_ns = 0;
    std::uint64_t row_count = 0;
};

struct reusable_lookup_result {
    reusable_registry_error error = reusable_registry_error::none;
    reusable_generation_snapshot snapshot{};
    reusable_reference_token token{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == reusable_registry_error::none && static_cast<bool>(snapshot.reference);
    }
};

/** Describes the cached domain and provider features needed by a query. Column mapping is checked separately by the provider. */
struct reusable_candidate_request {
    reusable_coverage_directory_key coverage_directory_key{};
    std::span<const std::byte> canonical_domain_key{};
    reusable_reference_owner reference_owner{};
    std::uint32_t provider_id = 0;
    std::uint32_t reserved = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t required_capability_mask = 0;
};

/** Candidate cache generation with a planner reference that must be released after use. */
struct reusable_candidate_snapshot {
    reusable_generation_snapshot generation{};
    reusable_reference_token planner_pin{};
    bool complete = false;
    bool in_flight = false;
    std::array<std::uint8_t, 6> reserved{};
};

struct reusable_candidate_batch {
    std::array<reusable_candidate_snapshot, k_maximum_coverage_generations_per_domain> candidates{};
    std::size_t candidate_count = 0;
    std::size_t visited_count = 0;
    std::size_t domain_collision_count = 0;
    std::size_t invalid_record_count = 0;
    bool bucket_truncated = false;
    bool selection_truncated = false;
};

/** Information used to publish a completed generation and its storage manifest. */
struct reusable_publish_request {
    reusable_generation_reference reference{};
    reusable_manifest_handle manifest{};
    std::uint64_t completed_monotonic_ns = 0;
    std::uint64_t row_count = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t stored_bytes = 0;
};

/**
 * Selects cache entries to invalidate within one database. Optional
 * fields narrow the match by provider, security settings or dependency
 * metadata. Database-wide invalidation requires all of those filters
 * to be disabled.
 */
struct reusable_invalidation_selector {
    std::uint32_t database_oid = 0;
    std::uint32_t provider_id = 0;
    reusable_security_namespace security_namespace{};
    physical_identity dependency_physical_identity{};
    cache_digest_256 dependency_fingerprint{};
    cache_digest_256 schema_fingerprint{};
    cache_digest_256 mapping_fingerprint{};
    cache_digest_256 lineage_fingerprint{};
    write_observability observability = write_observability::database_wide_fallback;
    std::uint32_t cursor = 0;
    std::uint32_t maximum_scan = k_maximum_reusable_invalidation_scan;
    bool match_security_namespace = false;
    bool match_provider_id = false;
    bool match_physical_identity = false;
    bool match_dependency_fingerprint = false;
    bool match_schema_fingerprint = false;
    bool match_mapping_fingerprint = false;
    bool match_lineage_fingerprint = false;
    bool match_observability = false;
    bool coarse_database_fallback = false;
    std::array<std::uint8_t, 7> reserved{};
};

/** Counts and entry identities from an invalidation scan. Storage cleanup is left to the caller. */
struct reusable_invalidation_result {
    reusable_registry_error error = reusable_registry_error::none;
    std::uint32_t next_cursor = 0;
    std::uint32_t scanned_generations = 0;
    std::uint32_t matched_generations = 0;
    std::uint32_t invalidated_complete = 0;
    std::uint32_t failed_inflight = 0;
    std::uint32_t already_invalidated = 0;
    std::uint32_t already_failed = 0;
    std::uint32_t referenced_generations = 0;
    std::uint32_t waiter_references = 0;
    std::uint32_t planner_pins = 0;
    std::uint32_t reader_pins = 0;
    std::array<reusable_generation_reference, k_maximum_reusable_invalidation_scan>
        affected_references{};
    std::array<reusable_manifest_handle, k_maximum_reusable_invalidation_scan> affected_manifests{};
    std::array<cache_entry_state, k_maximum_reusable_invalidation_scan> previous_states{};
    std::size_t affected_count = 0;
    bool done = false;
};

struct reusable_registry_snapshot {
    std::uint64_t boot_nonce = 0;
    std::uint32_t indexed_generations = 0;
    std::uint32_t retired_generations = 0;
    std::uint32_t free_generations = 0;
    std::uint32_t exact_occupied_buckets = 0;
    std::uint32_t coverage_occupied_buckets = 0;
    std::uint32_t exact_final_generations = 0;
    std::uint32_t reusable_coverage_generations = 0;
    std::uint32_t inflight_generations = 0;
    std::uint32_t complete_generations = 0;
    std::uint32_t failed_generations = 0;
    std::uint32_t invalidated_generations = 0;
    std::uint64_t waiter_references = 0;
    std::uint64_t planner_pins = 0;
    std::uint64_t reader_pins = 0;
    std::uint64_t arena_free_bytes = 0;
    std::uint32_t evictions = 0;
    std::uint32_t invalidations = 0;
};

/** Diagnostic values for an indexed generation, excluding SQL text, key bytes and shared pointers. */
struct reusable_generation_diagnostics_row {
    reusable_generation_reference reference{};
    reusable_manifest_handle manifest{};
    cache_digest_256 dependency_fingerprint{};
    cache_entry_kind entry_kind = cache_entry_kind::reusable_coverage;
    cache_entry_state state = cache_entry_state::absent;
    cache_failure_code failure = cache_failure_code::none;
    write_observability observability = write_observability::database_wide_fallback;
    std::uint32_t provider_id = 0;
    std::uint32_t provider_abi_revision = 0;
    std::uint32_t provider_domain_abi_revision = 0;
    std::uint32_t dependency_count = 0;
    std::uint32_t database_oid = 0;
    std::uint32_t current_role_oid = 0;
    std::int32_t producer_pid = 0;
    std::uint32_t waiter_count = 0;
    std::uint32_t planner_pin_count = 0;
    std::uint32_t reader_pin_count = 0;
    std::uint64_t compatibility_family_id = 0;
    std::uint64_t capability_mask = 0;
    std::uint64_t estimated_covered_rows = 0;
    std::uint64_t row_count = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t stored_bytes = 0;
    std::uint64_t producer_ttl_ms = 0;
    std::uint64_t conservative_guard_committed_epoch = 0;
    std::uint64_t conservative_guard_record_generation = 0;
    std::uint64_t snapshot_completion_count = 0;
    std::uint64_t started_monotonic_ns = 0;
    std::uint64_t completed_monotonic_ns = 0;
    std::uint64_t last_access_monotonic_ns = 0;
    reusable_plan_snapshot_status canonical_plan_snapshot_status =
        reusable_plan_snapshot_status::unavailable;
    reusable_plan_snapshot_status plan_snapshot_status = reusable_plan_snapshot_status::unavailable;
    bool display_sql_truncated = false;
    bool canonical_plan_truncated = false;
    bool producer_plan_truncated = false;
    bool has_conservative_guard = false;
    std::array<std::uint8_t, 2> reserved_diagnostic{};
};

/** Number of diagnostic rows copied, with a flag for output that did not fit. */
struct reusable_generation_diagnostics_result {
    reusable_registry_error error = reusable_registry_error::none;
    std::uint32_t indexed_generation_count = 0;
    std::size_t count = 0;
    bool truncated = false;
};

using reusable_janitor_owner_alive = bool (*)(void*,
                                              const reusable_janitor_owner_observation&) noexcept;

struct reusable_janitor_callbacks {
    void* context = nullptr;
    reusable_janitor_owner_alive owner_alive = nullptr;
};

/** Counts and scan position for a batch of backend liveness checks. */
struct reusable_janitor_liveness_snapshot {
    reusable_registry_error error = reusable_registry_error::none;
    std::size_t count = 0;
    std::uint32_t scan_start = 0;
    std::uint32_t scanned_generations = 0;
    bool truncated = false;
};

struct reusable_janitor_sweep_result {
    reusable_registry_error error = reusable_registry_error::none;
    std::uint32_t reclaimed_retired = 0;
    std::uint32_t retired_expired = 0;
    std::uint32_t retired_failed = 0;
    std::uint32_t retired_invalidated = 0;
    std::uint32_t failed_dead_producers = 0;
    std::uint32_t reaped_waiters = 0;
    std::uint32_t reaped_planner_pins = 0;
    std::uint32_t reaped_reader_pins = 0;
    std::array<reusable_generation_reference, k_maximum_reusable_janitor_batch>
        affected_references{};
    std::size_t affected_reference_count = 0;
    std::array<reusable_manifest_handle, k_maximum_reusable_janitor_batch> retired_manifests{};
    std::size_t retired_manifest_count = 0;
};

namespace reusable_registry_detail {

/** Saves each affected generation once so waiters can be notified after registry locks are released. */
[[nodiscard]] inline bool
remember_janitor_affected_reference(reusable_janitor_sweep_result& result,
                                    const reusable_generation_reference& reference) noexcept {
    for (std::size_t index = 0; index < result.affected_reference_count; ++index) {
        if (result.affected_references[index] == reference) {
            return true;
        }
    }
    if (result.affected_reference_count >= result.affected_references.size()) {
        return false;
    }
    result.affected_references[result.affected_reference_count++] = reference;
    return true;
}

struct directory_probe {
    std::uint32_t slot = k_invalid_reusable_registry_slot;
    bool found = false;
};

struct arena_allocation {
    std::uint32_t offset = 0;
    std::uint32_t length = 0;
};

struct generation_byte_bundle {
    arena_allocation allocation{};
    reusable_shared_byte_range domain_key{};
    reusable_shared_byte_range generation_key{};
    reusable_shared_byte_range descriptor{};
    reusable_shared_byte_range dependency_vector{};
};

/** Separate shared-memory ranges for SQL text, the canonical plan and the producer plan. */
struct diagnostic_byte_bundle {
    reusable_shared_byte_range display_sql{};
    reusable_shared_byte_range canonical_plan{};
    reusable_shared_byte_range producer_plan{};
};

[[nodiscard]] inline std::uint32_t partition_slice_start(std::uint64_t hash,
                                                         std::uint32_t capacity,
                                                         std::uint32_t partitions) noexcept {
    const std::uint32_t partition = static_cast<std::uint32_t>(hash & (partitions - 1U));
    return partition * (capacity / partitions);
}

[[nodiscard]] inline std::uint32_t partition_probe_start(std::uint64_t hash,
                                                         std::uint32_t capacity,
                                                         std::uint32_t partitions) noexcept {
    const std::uint32_t slice = capacity / partitions;
    return static_cast<std::uint32_t>(hash / partitions) & (slice - 1U);
}

[[nodiscard]] inline directory_probe
probe_exact_directory(const reusable_registry_attachment& attachment,
                      const cache_digest_256& digest) noexcept {
    const reusable_registry_header& shared = header(attachment);
    const std::uint64_t hash = digest_hash(digest);
    const std::uint32_t slice =
        shared.config.exact_directory_capacity / shared.config.partition_count;
    const std::uint32_t base = partition_slice_start(
        hash, shared.config.exact_directory_capacity, shared.config.partition_count);
    const std::uint32_t start = partition_probe_start(
        hash, shared.config.exact_directory_capacity, shared.config.partition_count);
    const reusable_exact_bucket* buckets = exact_buckets(attachment);
    std::uint32_t first_tombstone = k_invalid_reusable_registry_slot;
    for (std::uint32_t step = 0; step < slice; ++step) {
        const std::uint32_t slot = base + ((start + step) & (slice - 1U));
        const reusable_exact_bucket& bucket = buckets[slot];
        if (bucket.slot_state == directory_slot_state::occupied && bucket.digest == digest) {
            return directory_probe{.slot = slot, .found = true};
        }
        if (bucket.slot_state == directory_slot_state::tombstone &&
            first_tombstone == k_invalid_reusable_registry_slot) {
            first_tombstone = slot;
        }
        if (bucket.slot_state == directory_slot_state::empty) {
            return directory_probe{
                .slot =
                    first_tombstone != k_invalid_reusable_registry_slot ? first_tombstone : slot,
            };
        }
    }
    return directory_probe{.slot = first_tombstone};
}

[[nodiscard]] inline directory_probe
probe_coverage_directory(const reusable_registry_attachment& attachment,
                         const reusable_coverage_directory_key& key) noexcept {
    const reusable_registry_header& shared = header(attachment);
    const std::uint64_t hash = coverage_key_hash(key);
    const std::uint32_t slice =
        shared.config.coverage_directory_capacity / shared.config.partition_count;
    const std::uint32_t base = partition_slice_start(
        hash, shared.config.coverage_directory_capacity, shared.config.partition_count);
    const std::uint32_t start = partition_probe_start(
        hash, shared.config.coverage_directory_capacity, shared.config.partition_count);
    const reusable_coverage_bucket* buckets = coverage_buckets(attachment);
    std::uint32_t first_tombstone = k_invalid_reusable_registry_slot;
    for (std::uint32_t step = 0; step < slice; ++step) {
        const std::uint32_t slot = base + ((start + step) & (slice - 1U));
        const reusable_coverage_bucket& bucket = buckets[slot];
        if (bucket.slot_state == directory_slot_state::occupied && bucket.key == key) {
            return directory_probe{.slot = slot, .found = true};
        }
        if (bucket.slot_state == directory_slot_state::tombstone &&
            first_tombstone == k_invalid_reusable_registry_slot) {
            first_tombstone = slot;
        }
        if (bucket.slot_state == directory_slot_state::empty) {
            return directory_probe{
                .slot =
                    first_tombstone != k_invalid_reusable_registry_slot ? first_tombstone : slot,
            };
        }
    }
    return directory_probe{.slot = first_tombstone};
}

[[nodiscard]] inline std::uint64_t take_sequence(std::uint64_t& next) noexcept {
    if (next == 0 || next == (std::numeric_limits<std::uint64_t>::max)()) {
        return 0;
    }
    const std::uint64_t result = next;
    ++next;
    return result;
}

[[nodiscard]] inline std::expected<arena_allocation, reusable_registry_error>
allocate_arena_bytes(reusable_registry_attachment& attachment,
                     std::uint32_t requested_bytes) noexcept {
    reusable_registry_header& shared = header(attachment);
    const std::uint64_t aligned64 = align_up(requested_bytes, 8U);
    if (requested_bytes == 0 || aligned64 == 0 ||
        aligned64 > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    const std::uint32_t aligned = static_cast<std::uint32_t>(aligned64);
    reusable_free_extent* extents = free_extents(attachment);
    for (std::uint32_t index = 0; index < shared.free_extent_count; ++index) {
        reusable_free_extent& extent = extents[index];
        if (extent.length < aligned) {
            continue;
        }
        const arena_allocation allocation{
            .offset = extent.offset,
            .length = aligned,
        };
        extent.offset += aligned;
        extent.length -= aligned;
        if (extent.length == 0) {
            for (std::uint32_t move = index + 1U; move < shared.free_extent_count; ++move) {
                extents[move - 1U] = extents[move];
            }
            --shared.free_extent_count;
        }
        return allocation;
    }
    return std::unexpected(reusable_registry_error::byte_arena_full);
}

[[nodiscard]] inline bool free_arena_bytes(reusable_registry_attachment& attachment,
                                           arena_allocation allocation) noexcept {
    reusable_registry_header& shared = header(attachment);
    if (allocation.offset < shared.layout.byte_arena_offset || allocation.length == 0 ||
        static_cast<std::uint64_t>(allocation.offset) + allocation.length >
            static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
                shared.config.byte_arena_bytes ||
        shared.free_extent_count >= shared.layout.free_extent_capacity) {
        return false;
    }
    reusable_free_extent* extents = free_extents(attachment);
    std::uint32_t position = 0;
    while (position < shared.free_extent_count && extents[position].offset < allocation.offset) {
        ++position;
    }
    if ((position > 0 &&
         static_cast<std::uint64_t>(extents[position - 1U].offset) + extents[position - 1U].length >
             allocation.offset) ||
        (position<shared.free_extent_count&& static_cast<std::uint64_t>(allocation.offset) +
                  allocation.length> extents[position]
             .offset)) {
        return false;
    }
    for (std::uint32_t move = shared.free_extent_count; move > position; --move) {
        extents[move] = extents[move - 1U];
    }
    extents[position] = reusable_free_extent{
        .offset = allocation.offset,
        .length = allocation.length,
    };
    ++shared.free_extent_count;

    if (position > 0) {
        reusable_free_extent& previous = extents[position - 1U];
        reusable_free_extent& current = extents[position];
        if (previous.offset + previous.length == current.offset) {
            previous.length += current.length;
            for (std::uint32_t move = position + 1U; move < shared.free_extent_count; ++move) {
                extents[move - 1U] = extents[move];
            }
            --shared.free_extent_count;
            --position;
        }
    }
    if (position + 1U < shared.free_extent_count) {
        reusable_free_extent& current = extents[position];
        reusable_free_extent& next = extents[position + 1U];
        if (current.offset + current.length == next.offset) {
            current.length += next.length;
            for (std::uint32_t move = position + 2U; move < shared.free_extent_count; ++move) {
                extents[move - 1U] = extents[move];
            }
            --shared.free_extent_count;
        }
    }
    return true;
}

[[nodiscard]] inline std::expected<generation_byte_bundle, reusable_registry_error>
copy_generation_bytes(reusable_registry_attachment& attachment,
                      std::span<const std::byte> domain_key,
                      std::span<const std::byte> generation_key,
                      std::span<const std::byte> descriptor,
                      std::span<const std::byte> dependency_vector_envelope) noexcept {
    if (generation_key.empty()) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    if (domain_key.size() > k_maximum_coverage_domain_key_bytes ||
        generation_key.size() > k_maximum_canonical_key_bytes) {
        return std::unexpected(reusable_registry_error::key_too_large);
    }
    if (descriptor.size() > k_maximum_provider_recipe_bytes) {
        return std::unexpected(reusable_registry_error::descriptor_too_large);
    }
    const dependency_vector_envelope_result dependency_vector_validation =
        validate_dependency_vector_envelope(dependency_vector_envelope);
    if (!dependency_vector_validation) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    const std::uint64_t total64 = domain_key.size() + generation_key.size() + descriptor.size() +
                                  dependency_vector_envelope.size();
    if (total64 == 0 || total64 > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(reusable_registry_error::byte_arena_full);
    }
    auto allocation = allocate_arena_bytes(attachment, static_cast<std::uint32_t>(total64));
    if (!allocation.has_value()) {
        return std::unexpected(allocation.error());
    }

    generation_byte_bundle bundle{
        .allocation = *allocation,
        .generation_key =
            {
                .offset = allocation->offset + static_cast<std::uint32_t>(domain_key.size()),
                .length = static_cast<std::uint32_t>(generation_key.size()),
            },
    };
    if (!domain_key.empty()) {
        bundle.domain_key = reusable_shared_byte_range{
            .offset = allocation->offset,
            .length = static_cast<std::uint32_t>(domain_key.size()),
        };
    }
    if (!descriptor.empty()) {
        bundle.descriptor = reusable_shared_byte_range{
            .offset = bundle.generation_key.offset + bundle.generation_key.length,
            .length = static_cast<std::uint32_t>(descriptor.size()),
        };
    }
    bundle.dependency_vector = reusable_shared_byte_range{
        .offset = bundle.generation_key.offset + bundle.generation_key.length +
                  static_cast<std::uint32_t>(descriptor.size()),
        .length = static_cast<std::uint32_t>(dependency_vector_envelope.size()),
    };
    std::memset(attachment.region.data() + allocation->offset, 0, allocation->length);
    if (!domain_key.empty()) {
        std::memcpy(attachment.region.data() + bundle.domain_key.offset,
                    domain_key.data(),
                    domain_key.size());
    }
    std::memcpy(attachment.region.data() + bundle.generation_key.offset,
                generation_key.data(),
                generation_key.size());
    if (!descriptor.empty()) {
        std::memcpy(attachment.region.data() + bundle.descriptor.offset,
                    descriptor.data(),
                    descriptor.size());
    }
    std::memcpy(attachment.region.data() + bundle.dependency_vector.offset,
                dependency_vector_envelope.data(),
                dependency_vector_envelope.size());
    return bundle;
}

[[nodiscard]] inline std::expected<std::uint32_t, reusable_registry_error>
allocate_generation_record(reusable_registry_attachment& attachment) noexcept {
    reusable_registry_header& shared = header(attachment);
    if (shared.indexed_generations + shared.retired_generations >= shared.config.max_generations ||
        shared.free_generation_head == k_invalid_reusable_registry_slot) {
        return std::unexpected(reusable_registry_error::global_generation_limit);
    }
    const std::uint32_t slot = shared.free_generation_head;
    reusable_shared_generation* records = generations(attachment);
    if (slot >= shared.config.max_generations ||
        records[slot].membership != reusable_record_membership::free) {
        return std::unexpected(reusable_registry_error::corrupt_generation);
    }
    const std::uint32_t next = records[slot].next_retired_or_free;
    const std::uint64_t slot_sequence = take_sequence(shared.next_slot_sequence);
    if (slot_sequence == 0) {
        return std::unexpected(reusable_registry_error::counter_overflow);
    }
    shared.free_generation_head = next;
    records[slot] = reusable_shared_generation{};
    records[slot].slot_sequence = slot_sequence;
    return slot;
}

inline void return_generation_record(reusable_registry_attachment& attachment,
                                     std::uint32_t slot) noexcept {
    reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    const std::uint64_t old_sequence = records[slot].slot_sequence;
    records[slot] = reusable_shared_generation{};
    records[slot].slot_sequence = old_sequence;
    records[slot].next_retired_or_free = shared.free_generation_head;
    shared.free_generation_head = slot;
}

[[nodiscard]] inline reusable_generation_reference
make_reference(const reusable_shared_generation& generation, std::uint32_t slot) noexcept {
    return reusable_generation_reference{
        .id = generation.id,
        .generation_slot = slot,
        .slot_sequence = generation.slot_sequence,
    };
}

/** Copies the producer and generation identities needed for a later liveness check. */
[[nodiscard]] inline reusable_janitor_owner_observation
make_producer_observation(const reusable_shared_generation& generation,
                          std::uint32_t slot) noexcept {
    return reusable_janitor_owner_observation{
        .reference = make_reference(generation, slot),
        .owner = generation.producer,
        .entry_kind = generation.entry_kind,
        .producer = true,
    };
}

/** Copies a reference owner and its full release token for a later liveness check. */
[[nodiscard]] inline reusable_janitor_owner_observation
make_reference_owner_observation(const reusable_shared_generation& generation,
                                 std::uint32_t slot,
                                 const reusable_shared_reference_owner& shared_owner,
                                 std::uint32_t owner_slot) noexcept {
    const reusable_generation_reference reference = make_reference(generation, slot);
    const reusable_reference_token token{
        .reference = reference,
        .owner = shared_owner.owner,
        .owner_sequence = shared_owner.owner_sequence,
        .owner_slot = owner_slot,
        .kind = shared_owner.kind,
        .active = shared_owner.active,
    };
    return reusable_janitor_owner_observation{
        .reference = reference,
        .owner = shared_owner.owner,
        .reference_token = token,
        .entry_kind = generation.entry_kind,
    };
}

[[nodiscard]] inline reusable_generation_snapshot
make_snapshot(const reusable_shared_generation& generation, std::uint32_t slot) noexcept {
    return reusable_generation_snapshot{
        .reference = make_reference(generation, slot),
        .entry_kind = generation.entry_kind,
        .coverage_directory_key = generation.coverage_directory_key,
        .state = generation.state,
        .failure = generation.failure,
        .producer = generation.producer,
        .metadata = generation.metadata,
        .manifest = generation.manifest,
        .canonical_generation_key = generation.canonical_generation_key,
        .canonical_domain_key = generation.canonical_domain_key,
        .coverage_descriptor = generation.coverage_descriptor,
        .dependency_vector = generation.dependency_vector,
        .conservative_guard = generation.conservative_guard,
        .waiter_count = generation.waiter_count,
        .planner_pin_count = generation.planner_pin_count,
        .reader_pin_count = generation.reader_pin_count,
        .started_monotonic_ns = generation.started_monotonic_ns,
        .completed_monotonic_ns = generation.completed_monotonic_ns,
        .last_access_monotonic_ns = generation.last_access_monotonic_ns,
        .row_count = generation.row_count,
    };
}

/** Copies diagnostic values while leaving SQL text and shared byte ranges out of the result. */
[[nodiscard]] inline reusable_generation_diagnostics_row
make_diagnostics_row(const reusable_shared_generation& generation, std::uint32_t slot) noexcept {
    return reusable_generation_diagnostics_row{
        .reference = make_reference(generation, slot),
        .manifest = generation.manifest,
        .dependency_fingerprint = generation.metadata.dependency_fingerprint,
        .entry_kind = generation.entry_kind,
        .state = generation.state,
        .failure = generation.failure,
        .observability = generation.metadata.observability,
        .provider_id = generation.metadata.provider_id,
        .provider_abi_revision = generation.metadata.provider_abi_revision,
        .provider_domain_abi_revision = generation.metadata.provider_domain_abi_revision,
        .dependency_count = generation.metadata.dependency_count,
        .database_oid = generation.coverage_directory_key.security_namespace.database_oid,
        .current_role_oid = generation.coverage_directory_key.security_namespace.current_role_oid,
        .producer_pid = generation.producer.pid,
        .waiter_count = generation.waiter_count,
        .planner_pin_count = generation.planner_pin_count,
        .reader_pin_count = generation.reader_pin_count,
        .compatibility_family_id = generation.metadata.compatibility_family_id,
        .capability_mask = generation.metadata.capability_mask,
        .estimated_covered_rows = generation.metadata.estimated_covered_rows,
        .row_count = generation.row_count,
        .logical_bytes = generation.metadata.logical_bytes,
        .stored_bytes = generation.metadata.stored_bytes,
        .producer_ttl_ms = generation.metadata.producer_ttl_ms,
        .conservative_guard_committed_epoch =
            generation.conservative_guard.active
                ? generation.conservative_guard.dependency.version.committed_epoch
                : 0,
        .conservative_guard_record_generation =
            generation.conservative_guard.active
                ? generation.conservative_guard.dependency.record_generation
                : 0,
        .snapshot_completion_count = generation.conservative_guard.snapshot_completion_count,
        .started_monotonic_ns = generation.started_monotonic_ns,
        .completed_monotonic_ns = generation.completed_monotonic_ns,
        .last_access_monotonic_ns = generation.last_access_monotonic_ns,
        .canonical_plan_snapshot_status = generation.canonical_plan_snapshot_status,
        .plan_snapshot_status = generation.plan_snapshot_status,
        .display_sql_truncated = generation.display_sql_truncated,
        .canonical_plan_truncated = generation.canonical_plan_truncated,
        .producer_plan_truncated = generation.producer_plan_truncated,
        .has_conservative_guard = generation.conservative_guard.active,
    };
}

[[nodiscard]] inline reusable_registry_error
link_exact_generation(reusable_registry_attachment& attachment,
                      std::uint32_t generation_slot) noexcept {
    reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    reusable_shared_generation& generation = records[generation_slot];
    if (!known_entry_kind(generation.entry_kind)) {
        return reusable_registry_error::corrupt_generation;
    }
    const directory_probe probe = probe_exact_directory(attachment, generation.id.digest);
    if (probe.slot == k_invalid_reusable_registry_slot) {
        return reusable_registry_error::directory_full;
    }
    reusable_exact_bucket* buckets = exact_buckets(attachment);
    reusable_exact_bucket& bucket = buckets[probe.slot];
    if (probe.found && bucket.generation_count >= k_maximum_generations_per_exact_digest) {
        return reusable_registry_error::exact_bucket_full;
    }
    if (!probe.found) {
        bucket = reusable_exact_bucket{
            .slot_state = directory_slot_state::occupied,
            .digest = generation.id.digest,
        };
        ++shared.exact_occupied_buckets;
    }
    generation.next_in_exact_bucket = bucket.first_generation;
    bucket.first_generation = generation_slot;
    ++bucket.generation_count;
    if (terminal_state(generation.state)) {
        ++bucket.terminal_generation_count;
    }
    return reusable_registry_error::none;
}

[[nodiscard]] inline reusable_registry_error
link_coverage_generation(reusable_registry_attachment& attachment,
                         std::uint32_t generation_slot) noexcept {
    reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    reusable_shared_generation& generation = records[generation_slot];
    if (generation.entry_kind != cache_entry_kind::reusable_coverage) {
        return reusable_registry_error::corrupt_generation;
    }
    const directory_probe probe =
        probe_coverage_directory(attachment, generation.coverage_directory_key);
    if (probe.slot == k_invalid_reusable_registry_slot) {
        return reusable_registry_error::directory_full;
    }
    reusable_coverage_bucket* buckets = coverage_buckets(attachment);
    reusable_coverage_bucket& bucket = buckets[probe.slot];
    if (probe.found && bucket.generation_count >= k_maximum_coverage_generations_per_domain) {
        return reusable_registry_error::coverage_bucket_full;
    }
    if (!probe.found) {
        bucket = reusable_coverage_bucket{
            .slot_state = directory_slot_state::occupied,
            .key = generation.coverage_directory_key,
        };
        ++shared.coverage_occupied_buckets;
    }
    generation.next_in_coverage_bucket = bucket.first_generation;
    bucket.first_generation = generation_slot;
    ++bucket.generation_count;
    if (terminal_state(generation.state)) {
        ++bucket.terminal_generation_count;
    }
    return reusable_registry_error::none;
}

[[nodiscard]] inline reusable_registry_error
unlink_exact_generation(reusable_registry_attachment& attachment,
                        std::uint32_t generation_slot) noexcept {
    reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    reusable_shared_generation& generation = records[generation_slot];
    if (!known_entry_kind(generation.entry_kind)) {
        return reusable_registry_error::corrupt_generation;
    }
    const directory_probe probe = probe_exact_directory(attachment, generation.id.digest);
    if (!probe.found) {
        return reusable_registry_error::corrupt_directory;
    }
    reusable_exact_bucket& bucket = exact_buckets(attachment)[probe.slot];
    std::uint32_t current = bucket.first_generation;
    std::uint32_t previous = k_invalid_reusable_registry_slot;
    for (std::uint32_t visited = 0; visited < k_maximum_generations_per_exact_digest &&
                                    current != k_invalid_reusable_registry_slot;
         ++visited) {
        if (current >= shared.config.max_generations) {
            return reusable_registry_error::corrupt_directory;
        }
        if (current == generation_slot) {
            if (previous == k_invalid_reusable_registry_slot) {
                bucket.first_generation = generation.next_in_exact_bucket;
            } else {
                records[previous].next_in_exact_bucket = generation.next_in_exact_bucket;
            }
            if (bucket.generation_count == 0) {
                return reusable_registry_error::corrupt_directory;
            }
            --bucket.generation_count;
            if (terminal_state(generation.state)) {
                if (bucket.terminal_generation_count == 0) {
                    return reusable_registry_error::corrupt_directory;
                }
                --bucket.terminal_generation_count;
            }
            generation.next_in_exact_bucket = k_invalid_reusable_registry_slot;
            if (bucket.generation_count == 0) {
                bucket = reusable_exact_bucket{
                    .slot_state = directory_slot_state::tombstone,
                };
                --shared.exact_occupied_buckets;
            }
            return reusable_registry_error::none;
        }
        previous = current;
        current = records[current].next_in_exact_bucket;
    }
    return reusable_registry_error::corrupt_directory;
}

[[nodiscard]] inline reusable_registry_error
unlink_coverage_generation(reusable_registry_attachment& attachment,
                           std::uint32_t generation_slot) noexcept {
    reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    reusable_shared_generation& generation = records[generation_slot];
    if (generation.entry_kind != cache_entry_kind::reusable_coverage) {
        return reusable_registry_error::corrupt_generation;
    }
    const directory_probe probe =
        probe_coverage_directory(attachment, generation.coverage_directory_key);
    if (!probe.found) {
        return reusable_registry_error::corrupt_directory;
    }
    reusable_coverage_bucket& bucket = coverage_buckets(attachment)[probe.slot];
    std::uint32_t current = bucket.first_generation;
    std::uint32_t previous = k_invalid_reusable_registry_slot;
    for (std::uint32_t visited = 0; visited < k_maximum_coverage_generations_per_domain &&
                                    current != k_invalid_reusable_registry_slot;
         ++visited) {
        if (current >= shared.config.max_generations) {
            return reusable_registry_error::corrupt_directory;
        }
        if (current == generation_slot) {
            if (previous == k_invalid_reusable_registry_slot) {
                bucket.first_generation = generation.next_in_coverage_bucket;
            } else {
                records[previous].next_in_coverage_bucket = generation.next_in_coverage_bucket;
            }
            if (bucket.generation_count == 0) {
                return reusable_registry_error::corrupt_directory;
            }
            --bucket.generation_count;
            if (terminal_state(generation.state)) {
                if (bucket.terminal_generation_count == 0) {
                    return reusable_registry_error::corrupt_directory;
                }
                --bucket.terminal_generation_count;
            }
            generation.next_in_coverage_bucket = k_invalid_reusable_registry_slot;
            if (bucket.generation_count == 0) {
                bucket = reusable_coverage_bucket{
                    .slot_state = directory_slot_state::tombstone,
                };
                --shared.coverage_occupied_buckets;
            }
            return reusable_registry_error::none;
        }
        previous = current;
        current = records[current].next_in_coverage_bucket;
    }
    return reusable_registry_error::corrupt_directory;
}

[[nodiscard]] inline reusable_registry_error
adjust_terminal_counts(reusable_registry_attachment& attachment,
                       const reusable_shared_generation& generation,
                       bool increment,
                       const reusable_registry_partition_callbacks& callbacks) noexcept {
    const std::uint32_t exact_partition =
        reusable_exact_partition(attachment, generation.id.digest);
    acquire_exact(callbacks, exact_partition);
    const directory_probe exact_probe = probe_exact_directory(attachment, generation.id.digest);
    if (!exact_probe.found) {
        release_exact(callbacks, exact_partition);
        return reusable_registry_error::corrupt_directory;
    }
    reusable_exact_bucket& exact = exact_buckets(attachment)[exact_probe.slot];
    if (!increment && exact.terminal_generation_count == 0) {
        release_exact(callbacks, exact_partition);
        return reusable_registry_error::corrupt_directory;
    }
    if (increment) {
        ++exact.terminal_generation_count;
    } else {
        --exact.terminal_generation_count;
    }
    release_exact(callbacks, exact_partition);

    if (generation.entry_kind == cache_entry_kind::exact_final_result) {
        return reusable_registry_error::none;
    }
    if (generation.entry_kind != cache_entry_kind::reusable_coverage) {
        acquire_exact(callbacks, exact_partition);
        reusable_exact_bucket& rollback = exact_buckets(attachment)[exact_probe.slot];
        if (increment) {
            --rollback.terminal_generation_count;
        } else {
            ++rollback.terminal_generation_count;
        }
        release_exact(callbacks, exact_partition);
        return reusable_registry_error::corrupt_generation;
    }

    const std::uint32_t coverage_partition =
        reusable_coverage_partition(attachment, generation.coverage_directory_key);
    acquire_coverage(callbacks, coverage_partition);
    const directory_probe coverage_probe =
        probe_coverage_directory(attachment, generation.coverage_directory_key);
    if (!coverage_probe.found) {
        release_coverage(callbacks, coverage_partition);
        acquire_exact(callbacks, exact_partition);
        reusable_exact_bucket& rollback = exact_buckets(attachment)[exact_probe.slot];
        if (increment) {
            --rollback.terminal_generation_count;
        } else {
            ++rollback.terminal_generation_count;
        }
        release_exact(callbacks, exact_partition);
        return reusable_registry_error::corrupt_directory;
    }
    reusable_coverage_bucket& coverage = coverage_buckets(attachment)[coverage_probe.slot];
    if (!increment && coverage.terminal_generation_count == 0) {
        release_coverage(callbacks, coverage_partition);
        acquire_exact(callbacks, exact_partition);
        reusable_exact_bucket& rollback = exact_buckets(attachment)[exact_probe.slot];
        ++rollback.terminal_generation_count;
        release_exact(callbacks, exact_partition);
        return reusable_registry_error::corrupt_directory;
    }
    if (increment) {
        ++coverage.terminal_generation_count;
    } else {
        --coverage.terminal_generation_count;
    }
    release_coverage(callbacks, coverage_partition);
    return reusable_registry_error::none;
}

[[nodiscard]] inline coverage_source_order_key
candidate_order_key(const reusable_candidate_snapshot& candidate) noexcept {
    return coverage_source_order_key{
        .complete = candidate.complete,
        .estimated_covered_rows = candidate.generation.metadata.estimated_covered_rows,
        .stored_bytes = candidate.generation.metadata.stored_bytes,
        .claim_sequence = candidate.generation.reference.id.claim_sequence,
        .digest = candidate.generation.reference.id.digest,
    };
}

[[nodiscard]] inline bool candidate_precedes(const reusable_candidate_snapshot& left,
                                             const reusable_candidate_snapshot& right) noexcept {
    return coverage_source_precedes(candidate_order_key(left), candidate_order_key(right));
}

} // namespace reusable_registry_detail

/** Copies an indexed generation after checking its slot sequence and identity. The caller holds the membership lock. */
[[nodiscard]] inline std::expected<reusable_generation_snapshot, reusable_registry_error>
snapshot_reusable_generation(const reusable_registry_attachment& attachment,
                             const reusable_generation_reference& reference) noexcept {
    if (validate_reusable_registry_attachment(attachment) != reusable_registry_error::none ||
        !reference ||
        reference.generation_slot >=
            reusable_registry_detail::header(attachment).config.max_generations) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    const reusable_shared_generation& generation =
        reusable_registry_detail::generations(attachment)[reference.generation_slot];
    if (!reusable_registry_detail::reference_matches(generation, reference)) {
        return std::unexpected(reusable_registry_error::stale_reference);
    }
    return reusable_registry_detail::make_snapshot(generation, reference.generation_slot);
}

namespace reusable_registry_detail {

struct matching_slot {
    reusable_registry_error error = reusable_registry_error::none;
    std::uint32_t slot = k_invalid_reusable_registry_slot;
};

[[nodiscard]] inline bool claim_dependencies_match(const reusable_registry_attachment& attachment,
                                                   const reusable_shared_generation& generation,
                                                   const reusable_claim_request& request) noexcept {
    if (!range_equal(
            attachment, generation.dependency_vector, request.dependency_vector_envelope) ||
        generation.conservative_guard.active != request.conservative_guard.active) {
        return false;
    }
    if (!generation.conservative_guard.active) {
        return generation.conservative_guard == reusable_conservative_guard{} &&
               request.conservative_guard == reusable_conservative_guard{};
    }
    if (generation.conservative_guard.dependency != request.conservative_guard.dependency) {
        return false;
    }
    const bool in_flight = generation.state == cache_entry_state::claimed ||
                           generation.state == cache_entry_state::building;
    return !in_flight || generation.conservative_guard.snapshot_completion_count ==
                             request.conservative_guard.snapshot_completion_count;
}

[[nodiscard]] inline matching_slot
find_exact_generation(const reusable_registry_attachment& attachment,
                      cache_entry_kind entry_kind,
                      const reusable_coverage_directory_key& coverage_key,
                      std::span<const std::byte> domain_key,
                      const cache_digest_256& generation_digest,
                      std::span<const std::byte> generation_key,
                      const reusable_claim_request* claim_request = nullptr) noexcept {
    const reusable_registry_header& shared = header(attachment);
    const directory_probe probe = probe_exact_directory(attachment, generation_digest);
    if (!probe.found) {
        return matching_slot{
            .error = reusable_registry_error::not_found,
        };
    }
    const reusable_exact_bucket& bucket = exact_buckets(attachment)[probe.slot];
    if (bucket.generation_count > k_maximum_generations_per_exact_digest) {
        return matching_slot{
            .error = reusable_registry_error::corrupt_directory,
        };
    }
    const reusable_shared_generation* records = generations(attachment);
    std::uint32_t current = bucket.first_generation;
    for (std::uint32_t visited = 0; visited < bucket.generation_count; ++visited) {
        if (current == k_invalid_reusable_registry_slot ||
            current >= shared.config.max_generations) {
            return matching_slot{
                .error = reusable_registry_error::corrupt_directory,
            };
        }
        const reusable_shared_generation& generation = records[current];
        if (generation.membership != reusable_record_membership::indexed ||
            !known_entry_kind(generation.entry_kind) || generation.id.digest != generation_digest) {
            return matching_slot{
                .error = reusable_registry_error::corrupt_generation,
            };
        }
        const bool kind_matches = generation.entry_kind == entry_kind;
        const bool coverage_identity_matches =
            entry_kind == cache_entry_kind::exact_final_result
                ? generation.coverage_directory_key.security_namespace ==
                          coverage_key.security_namespace &&
                      generation.coverage_directory_key.coverage_domain_digest == cache_digest_256{}
                : generation.coverage_directory_key == coverage_key &&
                      range_equal(attachment, generation.canonical_domain_key, domain_key);
        if (kind_matches && coverage_identity_matches &&
            range_equal(attachment, generation.canonical_generation_key, generation_key) &&
            (claim_request == nullptr ||
             claim_dependencies_match(attachment, generation, *claim_request))) {
            return matching_slot{.slot = current};
        }
        current = generation.next_in_exact_bucket;
    }
    if (current != k_invalid_reusable_registry_slot) {
        return matching_slot{
            .error = reusable_registry_error::corrupt_directory,
        };
    }
    return matching_slot{
        .error = reusable_registry_error::not_found,
    };
}

[[nodiscard]] inline std::expected<reusable_reference_token, reusable_registry_error>
increment_reference(reusable_registry_attachment& attachment,
                    reusable_shared_generation& generation,
                    std::uint32_t slot,
                    reusable_reference_kind kind,
                    const reusable_reference_owner& owner) noexcept {
    if (!owner.valid()) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    std::uint32_t* counter = nullptr;
    switch (kind) {
    case reusable_reference_kind::waiter:
        if (generation.state != cache_entry_state::claimed &&
            generation.state != cache_entry_state::building) {
            return std::unexpected(reusable_registry_error::invalid_state);
        }
        counter = &generation.waiter_count;
        break;
    case reusable_reference_kind::planner_pin:
        if (generation.state != cache_entry_state::claimed &&
            generation.state != cache_entry_state::building &&
            generation.state != cache_entry_state::complete) {
            return std::unexpected(reusable_registry_error::invalid_state);
        }
        counter = &generation.planner_pin_count;
        break;
    case reusable_reference_kind::reader_pin:
        if (generation.state != cache_entry_state::complete) {
            return std::unexpected(reusable_registry_error::invalid_state);
        }
        counter = &generation.reader_pin_count;
        break;
    case reusable_reference_kind::none:
        return std::unexpected(reusable_registry_error::invalid_argument);
    default:
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    if (*counter == (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(reusable_registry_error::counter_overflow);
    }
    std::uint32_t owner_slot = k_invalid_reusable_registry_slot;
    for (std::uint32_t index = 0; index < k_reusable_reference_owners_per_generation; ++index) {
        if (!generation.reference_owners[index].active) {
            owner_slot = index;
            break;
        }
    }
    if (owner_slot == k_invalid_reusable_registry_slot) {
        return std::unexpected(reusable_registry_error::reference_slots_full);
    }
    const std::uint64_t owner_sequence = take_sequence(header(attachment).next_reference_sequence);
    if (owner_sequence == 0) {
        return std::unexpected(reusable_registry_error::counter_overflow);
    }
    generation.reference_owners[owner_slot] = reusable_shared_reference_owner{
        .owner = owner,
        .owner_sequence = owner_sequence,
        .kind = kind,
        .active = true,
    };
    ++*counter;
    return reusable_reference_token{
        .reference = make_reference(generation, slot),
        .owner = owner,
        .owner_sequence = owner_sequence,
        .owner_slot = owner_slot,
        .kind = kind,
        .active = true,
    };
}

[[nodiscard]] inline std::uint32_t* reference_counter(reusable_shared_generation& generation,
                                                      reusable_reference_kind kind) noexcept {
    if (kind == reusable_reference_kind::waiter) {
        return &generation.waiter_count;
    }
    if (kind == reusable_reference_kind::planner_pin) {
        return &generation.planner_pin_count;
    }
    if (kind == reusable_reference_kind::reader_pin) {
        return &generation.reader_pin_count;
    }
    return nullptr;
}

[[nodiscard]] inline bool
reference_owner_matches(const reusable_shared_reference_owner& shared_owner,
                        const reusable_reference_token& token) noexcept {
    return token.owner_slot < k_reusable_reference_owners_per_generation && shared_owner.active &&
           shared_owner.kind == token.kind && shared_owner.owner_sequence == token.owner_sequence &&
           shared_owner.owner == token.owner;
}

[[nodiscard]] inline reusable_registry_error
deactivate_reference_owner(reusable_shared_generation& generation,
                           reusable_shared_reference_owner& shared_owner) noexcept {
    std::uint32_t* counter = reference_counter(generation, shared_owner.kind);
    if (!shared_owner.active || counter == nullptr) {
        return reusable_registry_error::invalid_state;
    }
    if (*counter == 0) {
        return reusable_registry_error::counter_underflow;
    }
    --*counter;
    shared_owner = reusable_shared_reference_owner{};
    return reusable_registry_error::none;
}

/** Checks that the write-tracking mode is a recognized enum value. */
[[nodiscard]] inline bool write_observability_valid(write_observability observability) noexcept {
    return observability == write_observability::none ||
           observability == write_observability::provider_complete ||
           observability == write_observability::executor_utility_only ||
           observability == write_observability::database_wide_fallback;
}

[[nodiscard]] inline bool
conservative_guard_valid(const reusable_conservative_guard& guard,
                         const reusable_generation_metadata& metadata,
                         const reusable_security_namespace& security_namespace) noexcept {
    if (!guard.active) {
        return guard == reusable_conservative_guard{} &&
               metadata.observability != write_observability::database_wide_fallback;
    }
    const dependency_vector_entry& entry = guard.dependency;
    const bool conservative_data =
        metadata.observability == write_observability::database_wide_fallback;
    const dependency_kind required_kind = conservative_data
                                              ? dependency_kind::database_write_epoch
                                              : dependency_kind::database_definition_epoch;
    return static_cast<bool>(guard) && database_guard_entry_valid(entry) &&
           entry.version.key.kind == required_kind &&
           entry.version.key.database_oid == security_namespace.database_oid &&
           entry.version.key.object_oid == security_namespace.database_oid;
}

[[nodiscard]] inline bool
shared_dependency_vector_valid(const reusable_registry_attachment& attachment,
                               const reusable_shared_generation& generation) noexcept {
    const std::size_t maximum_bytes =
        dependency_vector_envelope_bytes(k_maximum_dependencies_per_generation);
    if (!range_valid(attachment, generation.dependency_vector) ||
        generation.dependency_vector.length > maximum_bytes) {
        return false;
    }
    const std::span<const std::byte> envelope{
        attachment.region.data() + generation.dependency_vector.offset,
        generation.dependency_vector.length,
    };
    const dependency_vector_envelope_result validation =
        validate_dependency_vector_envelope(envelope);
    return validation && validation.header.entry_count == generation.metadata.dependency_count;
}

[[nodiscard]] inline std::uint32_t
generation_reference_count(const reusable_shared_generation& generation) noexcept {
    return generation.metadata.dependency_count +
           static_cast<std::uint32_t>(generation.conservative_guard.active);
}

[[nodiscard]] inline bool claim_request_valid(const reusable_claim_request& request) noexcept {
    const reusable_security_namespace& security_namespace =
        request.coverage_directory_key.security_namespace;
    const dependency_vector_envelope_result dependency_vector_validation =
        validate_dependency_vector_envelope(request.dependency_vector_envelope);
    const bool common_valid =
        known_entry_kind(request.entry_kind) &&
        valid_reusable_security_namespace(security_namespace) &&
        !request.canonical_generation_key.empty() &&
        request.metadata.dependency_count <= k_maximum_dependencies_per_generation &&
        dependency_vector_validation &&
        dependency_vector_validation.header.entry_count == request.metadata.dependency_count &&
        request.metadata.reserved == 0 &&
        request.metadata.dependency_physical_identity.length <=
            request.metadata.dependency_physical_identity.bytes.size() &&
        write_observability_valid(request.metadata.observability) &&
        conservative_guard_valid(
            request.conservative_guard, request.metadata, security_namespace) &&
        request.producer.valid() &&
        ((!request.pin_complete && !request.register_waiter) || request.reference_owner.valid());
    if (!common_valid) {
        return false;
    }
    if (request.entry_kind == cache_entry_kind::reusable_coverage) {
        return !request.canonical_domain_key.empty() && request.metadata.provider_id != 0 &&
               request.metadata.provider_abi_revision != 0 &&
               request.metadata.provider_domain_abi_revision != 0 &&
               request.metadata.compatibility_family_id != 0 &&
               request.metadata.observability == write_observability::provider_complete;
    }
    const bool provider_metadata_consistent =
        request.metadata.provider_id == 0
            ? request.metadata.provider_abi_revision == 0 &&
                  request.metadata.provider_domain_abi_revision == 0 &&
                  request.metadata.observability != write_observability::provider_complete
            : request.metadata.provider_abi_revision != 0 &&
                  request.metadata.provider_domain_abi_revision != 0 &&
                  request.metadata.observability == write_observability::provider_complete;
    return request.canonical_domain_key.empty() && request.coverage_descriptor.empty() &&
           request.coverage_directory_key.coverage_domain_digest == cache_digest_256{} &&
           provider_metadata_consistent;
}

[[nodiscard]] inline bool diagnostic_status_valid(reusable_plan_snapshot_status status) noexcept {
    return status == reusable_plan_snapshot_status::unavailable ||
           status == reusable_plan_snapshot_status::planned ||
           status == reusable_plan_snapshot_status::analyzed;
}

[[nodiscard]] inline bool diagnostic_text_valid(std::span<const char> text,
                                                std::size_t capacity) noexcept {
    return text.size() <= capacity &&
           (text.empty() || std::memchr(text.data(), 0, text.size()) == nullptr);
}

[[nodiscard]] inline bool diagnostic_capture_valid(std::span<const char> text,
                                                   std::size_t capacity,
                                                   reusable_plan_snapshot_status status,
                                                   bool truncated,
                                                   bool analyzed_allowed) noexcept {
    return diagnostic_status_valid(status) &&
           (analyzed_allowed || status != reusable_plan_snapshot_status::analyzed) &&
           diagnostic_text_valid(text, capacity) &&
           (!text.empty() || (status == reusable_plan_snapshot_status::unavailable && !truncated));
}

[[nodiscard]] inline bool
diagnostic_update_valid(const reusable_diagnostic_text_update& update) noexcept {
    return update.reference && update.producer.valid() &&
           diagnostic_text_valid(update.display_sql, k_reusable_display_sql_capacity) &&
           !update.display_sql.empty() &&
           diagnostic_capture_valid(update.canonical_plan,
                                    k_reusable_canonical_plan_capacity,
                                    update.canonical_plan_status,
                                    update.canonical_plan_truncated,
                                    false) &&
           diagnostic_capture_valid(update.producer_plan,
                                    k_reusable_producer_plan_capacity,
                                    update.plan_status,
                                    update.producer_plan_truncated,
                                    true) &&
           std::all_of(update.reserved.begin(),
                       update.reserved.end(),
                       [](std::uint8_t value) noexcept { return value == 0; });
}

[[nodiscard]] inline bool empty_range_shape(reusable_shared_byte_range range) noexcept {
    return range.offset == 0 && range.length == 0;
}

[[nodiscard]] inline std::uint32_t
diagnostic_allocation_length(reusable_shared_byte_range range) noexcept {
    const std::uint64_t aligned = align_up(range.length, 8U);
    return aligned == 0 || aligned > (std::numeric_limits<std::uint32_t>::max)()
               ? 0
               : static_cast<std::uint32_t>(aligned);
}

[[nodiscard]] inline bool
diagnostic_range_layout_valid(const reusable_registry_attachment& attachment,
                              reusable_shared_byte_range range,
                              std::uint32_t maximum) noexcept {
    if (range.length == 0) {
        return range.offset == 0;
    }
    const std::uint32_t allocation_length = diagnostic_allocation_length(range);
    if (range.offset == 0 || range.offset % 8U != 0 || range.length > maximum ||
        allocation_length == 0 || !range_valid(attachment, range)) {
        return false;
    }
    const reusable_registry_header& shared = header(attachment);
    const std::uint64_t allocation_end =
        static_cast<std::uint64_t>(range.offset) + allocation_length;
    return allocation_end <= static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
                                 shared.config.byte_arena_bytes;
}

[[nodiscard]] inline bool diagnostic_range_valid(const reusable_registry_attachment& attachment,
                                                 reusable_shared_byte_range range,
                                                 std::uint32_t maximum) noexcept {
    return diagnostic_range_layout_valid(attachment, range, maximum) &&
           (range.length == 0 ||
            std::memchr(attachment.region.data() + range.offset, 0, range.length) == nullptr);
}

[[nodiscard]] inline std::span<const char>
diagnostic_text_span(const reusable_registry_attachment& attachment,
                     reusable_shared_byte_range range) noexcept {
    if (range.length == 0) {
        return {};
    }
    return {
        reinterpret_cast<const char*>(attachment.region.data() + range.offset),
        range.length,
    };
}

[[nodiscard]] inline bool arena_allocations_overlap(arena_allocation left,
                                                    arena_allocation right) noexcept {
    if (left.length == 0 || right.length == 0) {
        return false;
    }
    const std::uint64_t left_end = static_cast<std::uint64_t>(left.offset) + left.length;
    const std::uint64_t right_end = static_cast<std::uint64_t>(right.offset) + right.length;
    return left.offset < right_end && right.offset < left_end;
}

[[nodiscard]] inline arena_allocation
diagnostic_allocation(reusable_shared_byte_range range) noexcept {
    return arena_allocation{
        .offset = range.offset,
        .length = range.length == 0 ? 0U : diagnostic_allocation_length(range),
    };
}

[[nodiscard]] inline diagnostic_byte_bundle
diagnostic_bundle(const reusable_shared_generation& generation) noexcept {
    return diagnostic_byte_bundle{
        .display_sql = generation.display_sql,
        .canonical_plan = generation.canonical_plan,
        .producer_plan = generation.producer_plan,
    };
}

[[nodiscard]] inline bool free_diagnostic_range(reusable_registry_attachment& attachment,
                                                reusable_shared_byte_range range) noexcept {
    if (range.length == 0) {
        return range.offset == 0;
    }
    if (!diagnostic_range_layout_valid(
            attachment, range, (std::numeric_limits<std::uint32_t>::max)())) {
        return false;
    }
    return free_arena_bytes(attachment, diagnostic_allocation(range));
}

[[nodiscard]] inline bool free_diagnostic_bundle(reusable_registry_attachment& attachment,
                                                 const diagnostic_byte_bundle& bundle) noexcept {
    bool valid = true;
    if (!free_diagnostic_range(attachment, bundle.display_sql)) {
        valid = false;
    }
    if (!free_diagnostic_range(attachment, bundle.canonical_plan)) {
        valid = false;
    }
    if (!free_diagnostic_range(attachment, bundle.producer_plan)) {
        valid = false;
    }
    return valid;
}

[[nodiscard]] inline std::expected<reusable_shared_byte_range, reusable_registry_error>
copy_diagnostic_range(reusable_registry_attachment& attachment,
                      std::span<const char> text) noexcept {
    if (text.empty()) {
        return reusable_shared_byte_range{};
    }
    if (text.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    auto allocation = allocate_arena_bytes(attachment, static_cast<std::uint32_t>(text.size()));
    if (!allocation.has_value()) {
        return std::unexpected(allocation.error());
    }
    std::memset(attachment.region.data() + allocation->offset, 0, allocation->length);
    std::memcpy(attachment.region.data() + allocation->offset, text.data(), text.size());
    return reusable_shared_byte_range{
        .offset = allocation->offset,
        .length = static_cast<std::uint32_t>(text.size()),
    };
}

[[nodiscard]] inline std::expected<diagnostic_byte_bundle, reusable_registry_error>
copy_diagnostic_bytes(reusable_registry_attachment& attachment,
                      const reusable_diagnostic_text_update& update) noexcept {
    diagnostic_byte_bundle bundle{};
    auto display = copy_diagnostic_range(attachment, update.display_sql);
    if (!display.has_value()) {
        return std::unexpected(display.error());
    }
    bundle.display_sql = *display;

    auto canonical = copy_diagnostic_range(attachment, update.canonical_plan);
    if (!canonical.has_value()) {
        const reusable_registry_error error = canonical.error();
        return free_diagnostic_bundle(attachment, bundle)
                   ? std::unexpected(error)
                   : std::unexpected(reusable_registry_error::corrupt_layout);
    }
    bundle.canonical_plan = *canonical;

    auto producer = copy_diagnostic_range(attachment, update.producer_plan);
    if (!producer.has_value()) {
        const reusable_registry_error error = producer.error();
        return free_diagnostic_bundle(attachment, bundle)
                   ? std::unexpected(error)
                   : std::unexpected(reusable_registry_error::corrupt_layout);
    }
    bundle.producer_plan = *producer;
    return bundle;
}

[[nodiscard]] inline bool
shared_diagnostic_layout_valid(const reusable_registry_attachment& attachment,
                               const reusable_shared_generation& generation) noexcept {
    if (!std::all_of(generation.reserved_diagnostic.begin(),
                     generation.reserved_diagnostic.end(),
                     [](std::uint8_t value) noexcept { return value == 0; }) ||
        !diagnostic_range_layout_valid(
            attachment, generation.display_sql, k_reusable_display_sql_capacity) ||
        !diagnostic_range_layout_valid(
            attachment, generation.canonical_plan, k_reusable_canonical_plan_capacity) ||
        !diagnostic_range_layout_valid(
            attachment, generation.producer_plan, k_reusable_producer_plan_capacity)) {
        return false;
    }
    const arena_allocation identity{
        .offset = generation.allocation_offset,
        .length = generation.allocation_length,
    };
    const arena_allocation display = diagnostic_allocation(generation.display_sql);
    const arena_allocation canonical = diagnostic_allocation(generation.canonical_plan);
    const arena_allocation producer = diagnostic_allocation(generation.producer_plan);
    return !arena_allocations_overlap(identity, display) &&
           !arena_allocations_overlap(identity, canonical) &&
           !arena_allocations_overlap(identity, producer) &&
           !arena_allocations_overlap(display, canonical) &&
           !arena_allocations_overlap(display, producer) &&
           !arena_allocations_overlap(canonical, producer);
}

[[nodiscard]] inline bool
shared_diagnostic_text_valid(const reusable_registry_attachment& attachment,
                             const reusable_shared_generation& generation) noexcept {
    if (!shared_diagnostic_layout_valid(attachment, generation) ||
        !diagnostic_range_valid(
            attachment, generation.display_sql, k_reusable_display_sql_capacity) ||
        !diagnostic_range_valid(
            attachment, generation.canonical_plan, k_reusable_canonical_plan_capacity) ||
        !diagnostic_range_valid(
            attachment, generation.producer_plan, k_reusable_producer_plan_capacity)) {
        return false;
    }
    const std::span<const char> display_sql =
        diagnostic_text_span(attachment, generation.display_sql);
    const std::span<const char> canonical_plan =
        diagnostic_text_span(attachment, generation.canonical_plan);
    const std::span<const char> producer_plan =
        diagnostic_text_span(attachment, generation.producer_plan);
    const bool has_sql = !display_sql.empty();
    const bool analyzed_state_valid =
        generation.plan_snapshot_status != reusable_plan_snapshot_status::analyzed ||
        generation.state == cache_entry_state::building ||
        generation.state == cache_entry_state::complete ||
        generation.state == cache_entry_state::failed ||
        generation.state == cache_entry_state::invalidated ||
        generation.state == cache_entry_state::evicting;
    if ((!has_sql && generation.display_sql_truncated) ||
        !diagnostic_text_valid(display_sql, k_reusable_display_sql_capacity) ||
        !diagnostic_capture_valid(canonical_plan,
                                  k_reusable_canonical_plan_capacity,
                                  generation.canonical_plan_snapshot_status,
                                  generation.canonical_plan_truncated,
                                  false) ||
        !diagnostic_capture_valid(producer_plan,
                                  k_reusable_producer_plan_capacity,
                                  generation.plan_snapshot_status,
                                  generation.producer_plan_truncated,
                                  true) ||
        (generation.plan_snapshot_status == reusable_plan_snapshot_status::analyzed &&
         !analyzed_state_valid) ||
        (generation.state == cache_entry_state::absent &&
         (has_sql || !canonical_plan.empty() || !producer_plan.empty())) ||
        (generation.state == cache_entry_state::claimed &&
         (has_sql || !canonical_plan.empty() || !producer_plan.empty())) ||
        ((generation.state == cache_entry_state::building ||
          generation.state == cache_entry_state::complete) &&
         !has_sql)) {
        return false;
    }
    return true;
}

/** Assigns previously copied diagnostic ranges after the caller validates generation identity and ownership. */
inline void
assign_diagnostic_text_unchecked(reusable_shared_generation& generation,
                                 const diagnostic_byte_bundle& bundle,
                                 const reusable_diagnostic_text_update& update) noexcept {
    generation.display_sql = bundle.display_sql;
    generation.canonical_plan = bundle.canonical_plan;
    generation.producer_plan = bundle.producer_plan;
    generation.display_sql_truncated = update.display_sql_truncated;
    generation.canonical_plan_truncated = update.canonical_plan_truncated;
    generation.producer_plan_truncated = update.producer_plan_truncated;
    generation.canonical_plan_snapshot_status = update.canonical_plan_status;
    generation.plan_snapshot_status = update.plan_status;
}

[[nodiscard]] inline bool
invalidation_selector_valid(const reusable_invalidation_selector& selector) noexcept {
    const bool reserved_clear = std::all_of(selector.reserved.begin(),
                                            selector.reserved.end(),
                                            [](std::uint8_t value) noexcept { return value == 0; });
    if (selector.database_oid == 0 || selector.maximum_scan == 0 || !reserved_clear) {
        return false;
    }
    if (selector.match_security_namespace &&
        (selector.security_namespace.database_oid != selector.database_oid ||
         !valid_reusable_security_namespace(selector.security_namespace))) {
        return false;
    }
    if (selector.match_provider_id && selector.provider_id == 0) {
        return false;
    }
    const bool physical_kind_valid =
        selector.dependency_physical_identity.kind ==
            physical_identity_kind::pg_rel_file_locator ||
        selector.dependency_physical_identity.kind == physical_identity_kind::provider_opaque;
    if (selector.match_physical_identity &&
        (!physical_kind_valid || selector.dependency_physical_identity.length == 0 ||
         selector.dependency_physical_identity.length >
             selector.dependency_physical_identity.bytes.size())) {
        return false;
    }
    if (selector.match_observability && !write_observability_valid(selector.observability)) {
        return false;
    }
    const bool precise_metadata_match =
        selector.match_provider_id || selector.match_physical_identity ||
        selector.match_dependency_fingerprint || selector.match_schema_fingerprint ||
        selector.match_mapping_fingerprint || selector.match_lineage_fingerprint ||
        selector.match_observability;
    if (selector.coarse_database_fallback) {
        return !selector.match_security_namespace && !precise_metadata_match;
    }
    return selector.match_security_namespace || precise_metadata_match;
}

[[nodiscard]] inline bool
generation_matches_invalidation(const reusable_shared_generation& generation,
                                const reusable_invalidation_selector& selector) noexcept {
    const reusable_security_namespace& security_namespace =
        generation.coverage_directory_key.security_namespace;
    if (security_namespace.database_oid != selector.database_oid ||
        (selector.match_security_namespace && security_namespace != selector.security_namespace)) {
        return false;
    }
    if (selector.coarse_database_fallback) {
        return true;
    }
    const reusable_generation_metadata& metadata = generation.metadata;
    return (!selector.match_provider_id || metadata.provider_id == selector.provider_id) &&
           (!selector.match_physical_identity ||
            physical_identity_equal(metadata.dependency_physical_identity,
                                    selector.dependency_physical_identity)) &&
           (!selector.match_dependency_fingerprint ||
            metadata.dependency_fingerprint == selector.dependency_fingerprint) &&
           (!selector.match_schema_fingerprint ||
            metadata.schema_fingerprint == selector.schema_fingerprint) &&
           (!selector.match_mapping_fingerprint ||
            metadata.mapping_fingerprint == selector.mapping_fingerprint) &&
           (!selector.match_lineage_fingerprint ||
            metadata.lineage_fingerprint == selector.lineage_fingerprint) &&
           (!selector.match_observability || metadata.observability == selector.observability);
}

[[nodiscard]] inline bool completed_generation_expired(const reusable_shared_generation& generation,
                                                       std::uint64_t consumer_ttl_ms,
                                                       std::uint64_t now_ns) noexcept {
    const cache_timestamps timestamps{
        .started_ns = generation.started_monotonic_ns,
        .completed_ns = generation.completed_monotonic_ns,
    };
    return expired(timestamps, generation.metadata.producer_ttl_ms, consumer_ttl_ms, now_ns);
}

struct admission_victim_selection {
    reusable_registry_error error = reusable_registry_error::none;
    reusable_generation_reference reference{};
    bool bucket_full = false;
};

[[nodiscard]] inline std::uint8_t
admission_victim_priority(const reusable_shared_generation& generation,
                          std::uint64_t now_monotonic_ns) noexcept {
    if (generation.state == cache_entry_state::complete &&
        completed_generation_expired(
            generation, (std::numeric_limits<std::uint64_t>::max)(), now_monotonic_ns)) {
        return 0;
    }
    if (generation.state == cache_entry_state::invalidated) {
        return 1;
    }
    if (generation.state == cache_entry_state::failed) {
        return 2;
    }
    return generation.state == cache_entry_state::complete ? 3 : 4;
}

[[nodiscard]] inline bool admission_victim_precedes(const reusable_shared_generation& candidate,
                                                    std::uint32_t candidate_slot,
                                                    const reusable_shared_generation& current,
                                                    std::uint32_t current_slot,
                                                    std::uint64_t now_monotonic_ns) noexcept {
    const std::uint8_t candidate_priority = admission_victim_priority(candidate, now_monotonic_ns);
    const std::uint8_t current_priority = admission_victim_priority(current, now_monotonic_ns);
    if (candidate_priority != current_priority) {
        return candidate_priority < current_priority;
    }
    if (candidate.last_access_monotonic_ns != current.last_access_monotonic_ns) {
        return candidate.last_access_monotonic_ns < current.last_access_monotonic_ns;
    }
    if (candidate.id.claim_sequence != current.id.claim_sequence) {
        return candidate.id.claim_sequence < current.id.claim_sequence;
    }
    return candidate_slot < current_slot;
}

[[nodiscard]] inline bool
admission_victim_eligible(const reusable_shared_generation& generation) noexcept {
    return generation.membership == reusable_record_membership::indexed &&
           terminal_state(generation.state) && generation.waiter_count == 0 &&
           generation.planner_pin_count == 0 && generation.reader_pin_count == 0;
}

/**
 * Chooses an unreferenced terminal entry to remove from a full exact
 * bucket. Expired, invalidated and failed entries come first, followed
 * by the least recently used completed entry. The caller holds the
 * exact partition lock.
 */
[[nodiscard]] inline admission_victim_selection
select_exact_admission_victim(reusable_registry_attachment& attachment,
                              const cache_digest_256& digest,
                              std::uint64_t now_monotonic_ns) noexcept {
    admission_victim_selection result{};
    const directory_probe probe = probe_exact_directory(attachment, digest);
    if (!probe.found) {
        return result;
    }
    const reusable_exact_bucket& bucket = exact_buckets(attachment)[probe.slot];
    if (bucket.generation_count > k_maximum_generations_per_exact_digest) {
        result.error = reusable_registry_error::corrupt_directory;
        return result;
    }
    if (bucket.generation_count < k_maximum_generations_per_exact_digest) {
        return result;
    }
    result.bucket_full = true;
    if (bucket.terminal_generation_count == 0) {
        return result;
    }

    const reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    std::uint32_t current = bucket.first_generation;
    std::uint32_t selected = k_invalid_reusable_registry_slot;
    for (std::uint32_t visited = 0; visited < bucket.generation_count; ++visited) {
        if (current == k_invalid_reusable_registry_slot ||
            current >= shared.config.max_generations) {
            result.error = reusable_registry_error::corrupt_directory;
            return result;
        }
        const reusable_shared_generation& generation = records[current];
        if (generation.membership != reusable_record_membership::indexed ||
            !known_entry_kind(generation.entry_kind) || generation.id.digest != digest) {
            result.error = reusable_registry_error::corrupt_directory;
            return result;
        }
        if (admission_victim_eligible(generation) &&
            (selected == k_invalid_reusable_registry_slot ||
             admission_victim_precedes(
                 generation, current, records[selected], selected, now_monotonic_ns))) {
            selected = current;
        }
        current = generation.next_in_exact_bucket;
    }
    if (current != k_invalid_reusable_registry_slot) {
        result.error = reusable_registry_error::corrupt_directory;
        return result;
    }
    if (selected != k_invalid_reusable_registry_slot) {
        result.reference = make_reference(records[selected], selected);
    }
    return result;
}

/**
 * Chooses an unreferenced terminal entry to remove from a full coverage
 * bucket, using the same expiry and last-use order as exact buckets.
 * The caller holds the coverage partition lock.
 */
[[nodiscard]] inline admission_victim_selection
select_coverage_admission_victim(reusable_registry_attachment& attachment,
                                 const reusable_coverage_directory_key& key,
                                 std::uint64_t now_monotonic_ns) noexcept {
    admission_victim_selection result{};
    const directory_probe probe = probe_coverage_directory(attachment, key);
    if (!probe.found) {
        return result;
    }
    const reusable_coverage_bucket& bucket = coverage_buckets(attachment)[probe.slot];
    if (bucket.generation_count > k_maximum_coverage_generations_per_domain) {
        result.error = reusable_registry_error::corrupt_directory;
        return result;
    }
    if (bucket.generation_count < k_maximum_coverage_generations_per_domain) {
        return result;
    }
    result.bucket_full = true;
    if (bucket.terminal_generation_count == 0) {
        return result;
    }

    const reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    std::uint32_t current = bucket.first_generation;
    std::uint32_t selected = k_invalid_reusable_registry_slot;
    for (std::uint32_t visited = 0; visited < bucket.generation_count; ++visited) {
        if (current == k_invalid_reusable_registry_slot ||
            current >= shared.config.max_generations) {
            result.error = reusable_registry_error::corrupt_directory;
            return result;
        }
        const reusable_shared_generation& generation = records[current];
        if (generation.membership != reusable_record_membership::indexed ||
            generation.entry_kind != cache_entry_kind::reusable_coverage ||
            generation.coverage_directory_key != key) {
            result.error = reusable_registry_error::corrupt_directory;
            return result;
        }
        if (admission_victim_eligible(generation) &&
            (selected == k_invalid_reusable_registry_slot ||
             admission_victim_precedes(
                 generation, current, records[selected], selected, now_monotonic_ns))) {
            selected = current;
        }
        current = generation.next_in_coverage_bucket;
    }
    if (current != k_invalid_reusable_registry_slot) {
        result.error = reusable_registry_error::corrupt_directory;
        return result;
    }
    if (selected != k_invalid_reusable_registry_slot) {
        result.reference = make_reference(records[selected], selected);
    }
    return result;
}

[[nodiscard]] inline bool
admission_victim_reduces_coverage_bucket(const reusable_registry_attachment& attachment,
                                         const reusable_generation_reference& reference,
                                         const reusable_coverage_directory_key& key) noexcept {
    if (!reference || reference.generation_slot >= header(attachment).config.max_generations) {
        return false;
    }
    const reusable_shared_generation& generation =
        generations(attachment)[reference.generation_slot];
    return reference_matches(generation, reference) &&
           generation.entry_kind == cache_entry_kind::reusable_coverage &&
           generation.coverage_directory_key == key;
}

[[nodiscard]] inline bool
remember_claim_retired_manifest(reusable_claim_result& result,
                                const reusable_manifest_handle& manifest) noexcept {
    if (!manifest) {
        return true;
    }
    if (result.retired_manifest_count >= result.retired_manifests.size()) {
        return false;
    }
    result.retired_manifests[result.retired_manifest_count++] = manifest;
    return true;
}

} // namespace reusable_registry_detail

[[nodiscard]] inline std::expected<reusable_manifest_handle, reusable_registry_error>
retire_reusable_generation(reusable_registry_attachment& attachment,
                           const reusable_generation_reference& reference,
                           const reusable_registry_partition_callbacks& callbacks = {}) noexcept;

/**
 * Finds a matching generation and optionally registers a waiter or reader.
 * Full key bytes distinguish hash collisions. The caller holds the shared
 * membership lock while this function locks the exact partition.
 */
[[nodiscard]] inline reusable_lookup_result
lookup_cache_generation(reusable_registry_attachment& attachment,
                        cache_entry_kind entry_kind,
                        const reusable_coverage_directory_key& coverage_key,
                        std::span<const std::byte> domain_key,
                        const cache_digest_256& generation_digest,
                        std::span<const std::byte> generation_key,
                        reusable_lookup_mode mode,
                        const reusable_reference_owner& reference_owner,
                        const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    reusable_lookup_result result{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none || !callbacks.valid() ||
        !known_entry_kind(entry_kind) || generation_key.empty() ||
        (mode != reusable_lookup_mode::observe && !reference_owner.valid())) {
        result.error = attachment_error != reusable_registry_error::none
                           ? attachment_error
                           : reusable_registry_error::invalid_argument;
        return result;
    }
    const reusable_security_namespace& security_namespace = coverage_key.security_namespace;
    const bool kind_identity_valid =
        entry_kind == cache_entry_kind::reusable_coverage
            ? !domain_key.empty()
            : domain_key.empty() && coverage_key.coverage_domain_digest == cache_digest_256{};
    if (!valid_reusable_security_namespace(security_namespace) || !kind_identity_valid ||
        domain_key.size() > k_maximum_coverage_domain_key_bytes ||
        generation_key.size() > k_maximum_canonical_key_bytes) {
        result.error = reusable_registry_error::invalid_argument;
        return result;
    }
    const std::uint32_t partition = reusable_exact_partition(attachment, generation_digest);
    acquire_exact(callbacks, partition);
    const matching_slot match = find_exact_generation(
        attachment, entry_kind, coverage_key, domain_key, generation_digest, generation_key);
    if (match.error != reusable_registry_error::none) {
        release_exact(callbacks, partition);
        result.error = match.error;
        return result;
    }
    reusable_shared_generation& generation = generations(attachment)[match.slot];
    result.snapshot = make_snapshot(generation, match.slot);
    reusable_reference_kind kind = reusable_reference_kind::none;
    if (mode == reusable_lookup_mode::register_waiter) {
        kind = reusable_reference_kind::waiter;
    } else if (mode == reusable_lookup_mode::pin_complete) {
        kind = reusable_reference_kind::reader_pin;
    }
    if (kind != reusable_reference_kind::none) {
        auto token = increment_reference(attachment, generation, match.slot, kind, reference_owner);
        if (!token.has_value()) {
            release_exact(callbacks, partition);
            result.error = token.error();
            return result;
        }
        result.token = *token;
        result.snapshot = make_snapshot(generation, match.slot);
    }
    release_exact(callbacks, partition);
    return result;
}

/** Looks up a reusable-coverage generation and optionally registers a waiter or reader. */
[[nodiscard]] inline reusable_lookup_result
lookup_reusable_generation(reusable_registry_attachment& attachment,
                           const reusable_coverage_directory_key& coverage_key,
                           std::span<const std::byte> domain_key,
                           const cache_digest_256& generation_digest,
                           std::span<const std::byte> generation_key,
                           reusable_lookup_mode mode,
                           const reusable_reference_owner& reference_owner,
                           const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    return lookup_cache_generation(attachment,
                                   cache_entry_kind::reusable_coverage,
                                   coverage_key,
                                   domain_key,
                                   generation_digest,
                                   generation_key,
                                   mode,
                                   reference_owner,
                                   callbacks);
}

/**
 * Shares matching work already in progress or a completed result, or
 * claims a new generation. The caller holds membership exclusively.
 * Exact and coverage partitions are locked separately.
 */
[[nodiscard]] inline reusable_claim_result
claim_reusable_generation(reusable_registry_attachment& attachment,
                          const reusable_claim_request& request,
                          const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    reusable_claim_result result{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none || !callbacks.valid() ||
        !claim_request_valid(request)) {
        result.error = attachment_error != reusable_registry_error::none
                           ? attachment_error
                           : reusable_registry_error::invalid_argument;
        return result;
    }
    if (request.canonical_domain_key.size() > k_maximum_coverage_domain_key_bytes ||
        request.canonical_generation_key.size() > k_maximum_canonical_key_bytes) {
        result.error = reusable_registry_error::key_too_large;
        return result;
    }
    if (request.coverage_descriptor.size() > k_maximum_provider_recipe_bytes) {
        result.error = reusable_registry_error::descriptor_too_large;
        return result;
    }

    reusable_registry_header& shared = header(attachment);
    const std::uint32_t exact_partition =
        reusable_exact_partition(attachment, request.generation_digest);
    acquire_exact(callbacks, exact_partition);
    const matching_slot existing = find_exact_generation(attachment,
                                                         request.entry_kind,
                                                         request.coverage_directory_key,
                                                         request.canonical_domain_key,
                                                         request.generation_digest,
                                                         request.canonical_generation_key,
                                                         &request);
    if (existing.error == reusable_registry_error::none) {
        reusable_shared_generation& generation = generations(attachment)[existing.slot];
        result.reference = make_reference(generation, existing.slot);
        if (generation.state == cache_entry_state::claimed ||
            generation.state == cache_entry_state::building) {
            result.disposition = reusable_claim_disposition::existing_in_flight;
            if (request.register_waiter) {
                auto token = increment_reference(attachment,
                                                 generation,
                                                 existing.slot,
                                                 reusable_reference_kind::waiter,
                                                 request.reference_owner);
                if (!token.has_value()) {
                    release_exact(callbacks, exact_partition);
                    result.error = token.error();
                    return result;
                }
                result.token = *token;
            }
            release_exact(callbacks, exact_partition);
            return result;
        }
        if (generation.state == cache_entry_state::complete &&
            !completed_generation_expired(
                generation, request.consumer_ttl_ms, request.started_monotonic_ns)) {
            result.disposition = reusable_claim_disposition::existing_complete;
            if (request.pin_complete) {
                auto token = increment_reference(attachment,
                                                 generation,
                                                 existing.slot,
                                                 reusable_reference_kind::reader_pin,
                                                 request.reference_owner);
                if (!token.has_value()) {
                    release_exact(callbacks, exact_partition);
                    result.error = token.error();
                    return result;
                }
                result.token = *token;
            }
            release_exact(callbacks, exact_partition);
            return result;
        }
    } else if (existing.error != reusable_registry_error::not_found) {
        release_exact(callbacks, exact_partition);
        result.error = existing.error;
        return result;
    }
    release_exact(callbacks, exact_partition);

    const bool coverage_membership = has_coverage_membership(request.entry_kind);
    const std::uint32_t coverage_partition =
        coverage_membership
            ? reusable_coverage_partition(attachment, request.coverage_directory_key)
            : 0;

    auto record_slot = allocate_generation_record(attachment);
    if (!record_slot.has_value()) {
        result.error = record_slot.error();
        return result;
    }
    auto bundle = copy_generation_bytes(attachment,
                                        request.canonical_domain_key,
                                        request.canonical_generation_key,
                                        request.coverage_descriptor,
                                        request.dependency_vector_envelope);
    if (!bundle.has_value()) {
        return_generation_record(attachment, *record_slot);
        result.error = bundle.error();
        return result;
    }
    const std::uint64_t generation_number = take_sequence(shared.next_generation);
    const std::uint64_t claim_sequence = take_sequence(shared.next_claim_sequence);
    if (generation_number == 0 || claim_sequence == 0) {
        static_cast<void>(free_arena_bytes(attachment, bundle->allocation));
        return_generation_record(attachment, *record_slot);
        result.error = reusable_registry_error::counter_overflow;
        return result;
    }

    reusable_shared_generation& generation = generations(attachment)[*record_slot];
    const std::uint64_t slot_sequence = generation.slot_sequence;
    generation = reusable_shared_generation{
        .membership = reusable_record_membership::indexed,
        .entry_kind = request.entry_kind,
        .state = cache_entry_state::claimed,
        .slot_sequence = slot_sequence,
        .id =
            {
                .digest = request.generation_digest,
                .boot_nonce = shared.boot_nonce,
                .generation = generation_number,
                .claim_sequence = claim_sequence,
            },
        .coverage_directory_key = request.coverage_directory_key,
        .canonical_generation_key = bundle->generation_key,
        .canonical_domain_key = bundle->domain_key,
        .coverage_descriptor = bundle->descriptor,
        .dependency_vector = bundle->dependency_vector,
        .conservative_guard = request.conservative_guard,
        .allocation_offset = bundle->allocation.offset,
        .allocation_length = bundle->allocation.length,
        .dependency_release_state = reusable_dependency_release_state::retained,
        .producer = request.producer,
        .started_monotonic_ns = request.started_monotonic_ns,
        .last_access_monotonic_ns = request.started_monotonic_ns,
        .metadata = request.metadata,
    };

    const auto discard_provisional_generation = [&attachment, &bundle, &record_slot]() noexcept {
        static_cast<void>(free_arena_bytes(attachment, bundle->allocation));
        return_generation_record(attachment, *record_slot);
    };

    acquire_exact(callbacks, exact_partition);
    const admission_victim_selection exact_victim = select_exact_admission_victim(
        attachment, request.generation_digest, request.started_monotonic_ns);
    release_exact(callbacks, exact_partition);
    if (exact_victim.error != reusable_registry_error::none) {
        discard_provisional_generation();
        result.error = exact_victim.error;
        return result;
    }

    admission_victim_selection coverage_victim{};
    if (coverage_membership) {
        acquire_coverage(callbacks, coverage_partition);
        coverage_victim = select_coverage_admission_victim(
            attachment, request.coverage_directory_key, request.started_monotonic_ns);
        release_coverage(callbacks, coverage_partition);
        if (coverage_victim.error != reusable_registry_error::none) {
            discard_provisional_generation();
            result.error = coverage_victim.error;
            return result;
        }
    }

    if (exact_victim.bucket_full && !exact_victim.reference) {
        discard_provisional_generation();
        result.error = reusable_registry_error::exact_bucket_full;
        return result;
    }
    const bool exact_victim_reduces_coverage =
        coverage_membership && exact_victim.reference &&
        admission_victim_reduces_coverage_bucket(
            attachment, exact_victim.reference, request.coverage_directory_key);
    if (coverage_victim.bucket_full && !exact_victim_reduces_coverage &&
        !coverage_victim.reference) {
        discard_provisional_generation();
        result.error = reusable_registry_error::coverage_bucket_full;
        return result;
    }

    std::array<reusable_generation_reference, 2> victims{};
    std::size_t victim_count = 0;
    if (exact_victim.reference) {
        victims[victim_count++] = exact_victim.reference;
    }
    if (coverage_victim.bucket_full && !exact_victim_reduces_coverage &&
        coverage_victim.reference &&
        (victim_count == 0 || victims[0] != coverage_victim.reference)) {
        victims[victim_count++] = coverage_victim.reference;
    }
    for (std::size_t index = 0; index < victim_count; ++index) {
        auto retired = retire_reusable_generation(attachment, victims[index], callbacks);
        if (!retired.has_value()) {
            discard_provisional_generation();
            result.error = retired.error();
            return result;
        }
        if (!remember_claim_retired_manifest(result, *retired)) {
            discard_provisional_generation();
            result.error = reusable_registry_error::corrupt_layout;
            return result;
        }
    }

    acquire_exact(callbacks, exact_partition);
    reusable_registry_error link_error = link_exact_generation(attachment, *record_slot);
    release_exact(callbacks, exact_partition);
    if (link_error == reusable_registry_error::none && coverage_membership) {
        acquire_coverage(callbacks, coverage_partition);
        link_error = link_coverage_generation(attachment, *record_slot);
        release_coverage(callbacks, coverage_partition);
    }
    if (link_error != reusable_registry_error::none) {
        acquire_exact(callbacks, exact_partition);
        const directory_probe linked_exact =
            probe_exact_directory(attachment, request.generation_digest);
        if (linked_exact.found) {
            static_cast<void>(unlink_exact_generation(attachment, *record_slot));
        }
        release_exact(callbacks, exact_partition);
        static_cast<void>(free_arena_bytes(attachment, bundle->allocation));
        return_generation_record(attachment, *record_slot);
        result.error = link_error;
        return result;
    }
    ++shared.indexed_generations;
    result.disposition = reusable_claim_disposition::claimed_producer;
    result.reference = make_reference(generation, *record_slot);
    return result;
}

/** Acquires a waiter, planner or reader reference after checking the generation identity. */
[[nodiscard]] inline std::expected<reusable_reference_token, reusable_registry_error>
pin_reusable_generation(reusable_registry_attachment& attachment,
                        const reusable_generation_reference& reference,
                        reusable_reference_kind kind,
                        const reusable_reference_owner& reference_owner,
                        const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return std::unexpected(attachment_error);
    }
    if (!callbacks.valid() || !reference || !reference_owner.valid() ||
        reference.generation_slot >= header(attachment).config.max_generations) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    const std::uint32_t partition = reusable_exact_partition(attachment, reference.id.digest);
    acquire_exact(callbacks, partition);
    reusable_shared_generation& generation = generations(attachment)[reference.generation_slot];
    if (!reference_matches(generation, reference)) {
        release_exact(callbacks, partition);
        return std::unexpected(reusable_registry_error::stale_reference);
    }
    auto token = increment_reference(
        attachment, generation, reference.generation_slot, kind, reference_owner);
    release_exact(callbacks, partition);
    return token;
}

/**
 * Clears the local token and releases its matching shared reference.
 * Repeated calls with the cleared token do nothing.
 */
[[nodiscard]] inline reusable_registry_error
release_reusable_reference(reusable_registry_attachment& attachment,
                           reusable_reference_token& token,
                           const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    if (!token.active) {
        return reusable_registry_error::none;
    }
    const reusable_reference_token owned = token;
    token = reusable_reference_token{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return attachment_error;
    }
    if (!callbacks.valid() || !owned.reference || !owned.owner.valid() ||
        owned.owner_sequence == 0 ||
        owned.owner_slot >= k_reusable_reference_owners_per_generation ||
        owned.kind == reusable_reference_kind::none ||
        owned.reference.generation_slot >= header(attachment).config.max_generations) {
        return reusable_registry_error::invalid_argument;
    }
    const std::uint32_t partition = reusable_exact_partition(attachment, owned.reference.id.digest);
    acquire_exact(callbacks, partition);
    reusable_shared_generation& generation =
        generations(attachment)[owned.reference.generation_slot];
    if (!reference_matches(generation, owned.reference)) {
        release_exact(callbacks, partition);
        return reusable_registry_error::stale_reference;
    }
    reusable_shared_reference_owner& shared_owner = generation.reference_owners[owned.owner_slot];
    if (!reference_owner_matches(shared_owner, owned)) {
        release_exact(callbacks, partition);
        return reusable_registry_error::stale_reference;
    }
    const reusable_registry_error release_error =
        deactivate_reference_owner(generation, shared_owner);
    release_exact(callbacks, partition);
    return release_error;
}

/** Converts a registered waiter into a reader once its generation is complete, under the exact partition lock. */
[[nodiscard]] inline reusable_registry_error convert_reusable_waiter_to_reader(
    reusable_registry_attachment& attachment,
    reusable_reference_token& token,
    const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return attachment_error;
    }
    if (!callbacks.valid() || !token.active || token.kind != reusable_reference_kind::waiter ||
        !token.reference || !token.owner.valid() || token.owner_sequence == 0 ||
        token.owner_slot >= k_reusable_reference_owners_per_generation ||
        token.reference.generation_slot >= header(attachment).config.max_generations) {
        return reusable_registry_error::invalid_argument;
    }
    const std::uint32_t partition = reusable_exact_partition(attachment, token.reference.id.digest);
    acquire_exact(callbacks, partition);
    reusable_shared_generation& generation =
        generations(attachment)[token.reference.generation_slot];
    if (!reference_matches(generation, token.reference)) {
        release_exact(callbacks, partition);
        return reusable_registry_error::stale_reference;
    }
    reusable_shared_reference_owner& shared_owner = generation.reference_owners[token.owner_slot];
    if (!reference_owner_matches(shared_owner, token)) {
        release_exact(callbacks, partition);
        return reusable_registry_error::stale_reference;
    }
    if (generation.state != cache_entry_state::complete) {
        release_exact(callbacks, partition);
        return reusable_registry_error::invalid_state;
    }
    if (generation.waiter_count == 0) {
        release_exact(callbacks, partition);
        return reusable_registry_error::counter_underflow;
    }
    if (generation.reader_pin_count == (std::numeric_limits<std::uint32_t>::max)()) {
        release_exact(callbacks, partition);
        return reusable_registry_error::counter_overflow;
    }
    --generation.waiter_count;
    ++generation.reader_pin_count;
    shared_owner.kind = reusable_reference_kind::reader_pin;
    token.kind = reusable_reference_kind::reader_pin;
    release_exact(callbacks, partition);
    return reusable_registry_error::none;
}

/** Reads the generation state so a waiter can decide whether to wait again. */
[[nodiscard]] inline std::expected<reusable_generation_snapshot, reusable_registry_error>
wait_reusable_generation(reusable_registry_attachment& attachment,
                         const reusable_generation_reference& reference,
                         const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return std::unexpected(attachment_error);
    }
    if (!callbacks.valid() || !reference ||
        reference.generation_slot >= header(attachment).config.max_generations) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    const std::uint32_t partition = reusable_exact_partition(attachment, reference.id.digest);
    acquire_exact(callbacks, partition);
    const reusable_shared_generation& generation =
        generations(attachment)[reference.generation_slot];
    if (!reference_matches(generation, reference)) {
        release_exact(callbacks, partition);
        return std::unexpected(reusable_registry_error::stale_reference);
    }
    const reusable_generation_snapshot result =
        make_snapshot(generation, reference.generation_slot);
    release_exact(callbacks, partition);
    return result;
}

/**
 * Copies the active waiter tokens for later wakeups. To exclude new
 * waiters, the caller holds membership exclusively from this snapshot
 * until it publishes the completed or failed state.
 */
[[nodiscard]] inline reusable_waiter_owner_batch snapshot_reusable_waiter_owners(
    reusable_registry_attachment& attachment,
    const reusable_generation_reference& reference,
    const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    reusable_waiter_owner_batch result{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none || !callbacks.valid() || !reference ||
        reference.generation_slot >= header(attachment).config.max_generations) {
        result.error = attachment_error != reusable_registry_error::none
                           ? attachment_error
                           : reusable_registry_error::invalid_argument;
        return result;
    }
    const std::uint32_t partition = reusable_exact_partition(attachment, reference.id.digest);
    acquire_exact(callbacks, partition);
    const reusable_shared_generation& generation =
        generations(attachment)[reference.generation_slot];
    if (!reference_matches(generation, reference)) {
        release_exact(callbacks, partition);
        result.error = reusable_registry_error::stale_reference;
        return result;
    }
    for (std::uint32_t owner_slot = 0; owner_slot < k_reusable_reference_owners_per_generation;
         ++owner_slot) {
        const reusable_shared_reference_owner& shared_owner =
            generation.reference_owners[owner_slot];
        if (!shared_owner.active || shared_owner.kind != reusable_reference_kind::waiter) {
            continue;
        }
        const bool reserved_clear =
            std::all_of(shared_owner.reserved.begin(),
                        shared_owner.reserved.end(),
                        [](std::uint8_t value) noexcept { return value == 0; });
        if (!shared_owner.owner.valid() || shared_owner.owner_sequence == 0 || !reserved_clear ||
            result.count >= result.waiters.size()) {
            release_exact(callbacks, partition);
            result.error = reusable_registry_error::corrupt_generation;
            result.count = 0;
            return result;
        }
        result.waiters[result.count++] = reusable_reference_token{
            .reference = reference,
            .owner = shared_owner.owner,
            .owner_sequence = shared_owner.owner_sequence,
            .owner_slot = owner_slot,
            .kind = reusable_reference_kind::waiter,
            .active = true,
        };
    }
    if (result.count != generation.waiter_count) {
        result.error = reusable_registry_error::corrupt_generation;
        result.count = 0;
    }
    release_exact(callbacks, partition);
    return result;
}

/**
 * Collects and orders up to 64 cached sources with matching domain keys
 * and provider features. Each returned candidate owns a planner
 * reference that the caller must release.
 */
[[nodiscard]] inline std::expected<reusable_candidate_batch, reusable_registry_error>
snapshot_reusable_candidates(reusable_registry_attachment& attachment,
                             const reusable_candidate_request& request,
                             const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    reusable_candidate_batch result{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none || !callbacks.valid() ||
        request.canonical_domain_key.empty() ||
        request.canonical_domain_key.size() > k_maximum_coverage_domain_key_bytes ||
        !request.reference_owner.valid() || request.provider_id == 0 ||
        request.compatibility_family_id == 0) {
        return std::unexpected(attachment_error != reusable_registry_error::none
                                   ? attachment_error
                                   : reusable_registry_error::invalid_argument);
    }
    const reusable_security_namespace& security_namespace =
        request.coverage_directory_key.security_namespace;
    if (!valid_reusable_security_namespace(security_namespace)) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }

    const reusable_registry_header& shared = header(attachment);
    const std::uint32_t partition =
        reusable_coverage_partition(attachment, request.coverage_directory_key);
    acquire_coverage(callbacks, partition);
    const directory_probe probe =
        probe_coverage_directory(attachment, request.coverage_directory_key);
    if (!probe.found) {
        release_coverage(callbacks, partition);
        return result;
    }
    const reusable_coverage_bucket& bucket = coverage_buckets(attachment)[probe.slot];
    if (bucket.generation_count > k_maximum_coverage_generations_per_domain) {
        release_coverage(callbacks, partition);
        return std::unexpected(reusable_registry_error::corrupt_directory);
    }
    const std::size_t visit_limit =
        std::min<std::size_t>(bucket.generation_count, k_maximum_coverage_generations_per_domain);
    result.bucket_truncated = bucket.generation_count > visit_limit;
    reusable_shared_generation* records = generations(attachment);
    std::uint32_t current = bucket.first_generation;
    for (std::size_t visited = 0; visited < visit_limit; ++visited) {
        ++result.visited_count;
        if (current == k_invalid_reusable_registry_slot ||
            current >= shared.config.max_generations) {
            release_coverage(callbacks, partition);
            return std::unexpected(reusable_registry_error::corrupt_directory);
        }
        reusable_shared_generation& generation = records[current];
        const std::uint32_t next = generation.next_in_coverage_bucket;
        if (generation.membership != reusable_record_membership::indexed ||
            generation.entry_kind != cache_entry_kind::reusable_coverage ||
            generation.coverage_directory_key != request.coverage_directory_key) {
            ++result.invalid_record_count;
            current = next;
            continue;
        }
        if (!range_equal(
                attachment, generation.canonical_domain_key, request.canonical_domain_key)) {
            ++result.domain_collision_count;
            current = next;
            continue;
        }
        const bool state_eligible = generation.state == cache_entry_state::complete ||
                                    generation.state == cache_entry_state::claimed ||
                                    generation.state == cache_entry_state::building;
        const bool metadata_eligible =
            generation.metadata.provider_id == request.provider_id &&
            generation.metadata.compatibility_family_id == request.compatibility_family_id &&
            capabilities_satisfy(generation.metadata.capability_mask,
                                 request.required_capability_mask);
        if (!state_eligible || !metadata_eligible ||
            generation.planner_pin_count == (std::numeric_limits<std::uint32_t>::max)()) {
            current = next;
            continue;
        }
        if (result.candidate_count >= result.candidates.size()) {
            result.bucket_truncated = true;
            current = next;
            continue;
        }
        reusable_candidate_snapshot candidate{
            .generation = make_snapshot(generation, current),
            .complete = generation.state == cache_entry_state::complete,
            .in_flight = generation.state == cache_entry_state::claimed ||
                         generation.state == cache_entry_state::building,
        };
        std::size_t position = result.candidate_count;
        while (position > 0 && candidate_precedes(candidate, result.candidates[position - 1U])) {
            result.candidates[position] = result.candidates[position - 1U];
            --position;
        }
        result.candidates[position] = candidate;
        ++result.candidate_count;
        current = next;
    }
    if (current != k_invalid_reusable_registry_slot) {
        release_coverage(callbacks, partition);
        return std::unexpected(reusable_registry_error::corrupt_directory);
    }
    result.selection_truncated = result.candidate_count > k_maximum_sources_per_coverage_node;
    release_coverage(callbacks, partition);

    for (std::size_t index = 0; index < result.candidate_count; ++index) {
        reusable_candidate_snapshot& candidate = result.candidates[index];
        auto planner_pin = pin_reusable_generation(attachment,
                                                   candidate.generation.reference,
                                                   reusable_reference_kind::planner_pin,
                                                   request.reference_owner,
                                                   callbacks);
        if (!planner_pin.has_value()) {
            for (std::size_t release_index = 0; release_index < index; ++release_index) {
                static_cast<void>(release_reusable_reference(
                    attachment, result.candidates[release_index].planner_pin, callbacks));
            }
            return std::unexpected(planner_pin.error());
        }
        candidate.planner_pin = *planner_pin;
        auto pinned_snapshot =
            wait_reusable_generation(attachment, candidate.generation.reference, callbacks);
        if (!pinned_snapshot.has_value()) {
            static_cast<void>(
                release_reusable_reference(attachment, candidate.planner_pin, callbacks));
            for (std::size_t release_index = 0; release_index < index; ++release_index) {
                static_cast<void>(release_reusable_reference(
                    attachment, result.candidates[release_index].planner_pin, callbacks));
            }
            return std::unexpected(pinned_snapshot.error());
        }
        candidate.generation = *pinned_snapshot;
        candidate.complete = candidate.generation.state == cache_entry_state::complete;
        candidate.in_flight = candidate.generation.state == cache_entry_state::claimed ||
                              candidate.generation.state == cache_entry_state::building;
        if (!candidate.complete && !candidate.in_flight) {
            static_cast<void>(
                release_reusable_reference(attachment, candidate.planner_pin, callbacks));
            for (std::size_t release_index = 0; release_index < index; ++release_index) {
                static_cast<void>(release_reusable_reference(
                    attachment, result.candidates[release_index].planner_pin, callbacks));
            }
            return std::unexpected(reusable_registry_error::invalid_state);
        }
    }
    return result;
}

/** Releases every planner reference in a candidate batch and returns the first error, if any. */
[[nodiscard]] inline reusable_registry_error release_reusable_candidate_batch(
    reusable_registry_attachment& attachment,
    reusable_candidate_batch& batch,
    const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    if (batch.candidate_count > batch.candidates.size()) {
        return reusable_registry_error::invalid_argument;
    }
    reusable_registry_error first_error = reusable_registry_error::none;
    for (std::size_t index = 0; index < batch.candidate_count; ++index) {
        const reusable_registry_error error =
            release_reusable_reference(attachment, batch.candidates[index].planner_pin, callbacks);
        if (first_error == reusable_registry_error::none &&
            error != reusable_registry_error::none) {
            first_error = error;
        }
    }
    return first_error;
}

namespace reusable_registry_detail {

enum class diagnostic_mutation_mode : std::uint8_t {
    begin_building,
    replace,
};

[[nodiscard]] inline reusable_registry_error
diagnostic_mutation_error(const reusable_registry_attachment& attachment,
                          const reusable_shared_generation& generation,
                          const reusable_diagnostic_text_update& update,
                          diagnostic_mutation_mode mode) noexcept {
    if (!known_entry_kind(generation.entry_kind) ||
        !shared_diagnostic_layout_valid(attachment, generation)) {
        return reusable_registry_error::corrupt_generation;
    }
    if (generation.producer != update.producer) {
        return reusable_registry_error::invalid_state;
    }
    if (mode == diagnostic_mutation_mode::begin_building) {
        return legal_state_transition(generation.state, cache_entry_state::building)
                   ? reusable_registry_error::none
                   : reusable_registry_error::invalid_state;
    }

    const bool state_allows_update = generation.state == cache_entry_state::building ||
                                     generation.state == cache_entry_state::complete;
    const bool analyzed_state_valid =
        update.plan_status != reusable_plan_snapshot_status::analyzed || state_allows_update;
    const bool no_status_downgrade =
        generation.plan_snapshot_status != reusable_plan_snapshot_status::analyzed ||
        update.plan_status == reusable_plan_snapshot_status::analyzed;
    const bool no_canonical_status_downgrade =
        generation.canonical_plan_snapshot_status != reusable_plan_snapshot_status::planned ||
        update.canonical_plan_status == reusable_plan_snapshot_status::planned;
    return state_allows_update && analyzed_state_valid && no_status_downgrade &&
                   no_canonical_status_downgrade
               ? reusable_registry_error::none
               : reusable_registry_error::invalid_state;
}

[[nodiscard]] inline reusable_registry_error
rollback_diagnostic_bundle(reusable_registry_attachment& attachment,
                           const diagnostic_byte_bundle& bundle,
                           reusable_registry_error error) noexcept {
    return free_diagnostic_bundle(attachment, bundle) ? error
                                                      : reusable_registry_error::corrupt_layout;
}

/**
 * Allocates replacement diagnostic text, then checks that the same
 * producer still owns the generation before saving it. The caller
 * holds membership exclusively so the shared allocations stay valid
 * while the exact partition lock is released.
 */
[[nodiscard]] inline reusable_registry_error
mutate_diagnostic_text(reusable_registry_attachment& attachment,
                       const reusable_diagnostic_text_update& update,
                       diagnostic_mutation_mode mode,
                       const reusable_registry_partition_callbacks& callbacks) noexcept {
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return attachment_error;
    }
    if (!callbacks.valid() || !diagnostic_update_valid(update) ||
        (mode == diagnostic_mutation_mode::begin_building &&
         update.plan_status == reusable_plan_snapshot_status::analyzed) ||
        update.reference.generation_slot >= header(attachment).config.max_generations) {
        return reusable_registry_error::invalid_argument;
    }

    const std::uint32_t partition =
        reusable_exact_partition(attachment, update.reference.id.digest);
    acquire_exact(callbacks, partition);
    reusable_shared_generation& preflight =
        generations(attachment)[update.reference.generation_slot];
    if (!reference_matches(preflight, update.reference)) {
        release_exact(callbacks, partition);
        return reusable_registry_error::stale_reference;
    }
    const reusable_registry_error preflight_error =
        diagnostic_mutation_error(attachment, preflight, update, mode);
    const cache_entry_kind preflight_kind = preflight.entry_kind;
    release_exact(callbacks, partition);
    if (preflight_error != reusable_registry_error::none) {
        return preflight_error;
    }
    if (!shared_diagnostic_text_valid(attachment, preflight)) {
        return reusable_registry_error::corrupt_generation;
    }

    auto replacement = copy_diagnostic_bytes(attachment, update);
    if (!replacement.has_value()) {
        return replacement.error();
    }

    acquire_exact(callbacks, partition);
    reusable_shared_generation& generation =
        generations(attachment)[update.reference.generation_slot];
    reusable_registry_error publish_error = reusable_registry_error::none;
    if (!reference_matches(generation, update.reference)) {
        publish_error = reusable_registry_error::stale_reference;
    } else if (generation.entry_kind != preflight_kind ||
               !known_entry_kind(generation.entry_kind)) {
        publish_error = reusable_registry_error::corrupt_generation;
    } else {
        publish_error = diagnostic_mutation_error(attachment, generation, update, mode);
    }
    if (publish_error != reusable_registry_error::none) {
        release_exact(callbacks, partition);
        return rollback_diagnostic_bundle(attachment, *replacement, publish_error);
    }

    const diagnostic_byte_bundle previous = diagnostic_bundle(generation);
    assign_diagnostic_text_unchecked(generation, *replacement, update);
    if (mode == diagnostic_mutation_mode::begin_building) {
        generation.state = cache_entry_state::building;
    }
    release_exact(callbacks, partition);
    return free_diagnostic_bundle(attachment, previous) ? reusable_registry_error::none
                                                        : reusable_registry_error::corrupt_layout;
}

} // namespace reusable_registry_detail

/**
 * Marks a claimed generation as building with SQL and plan diagnostics
 * marked unavailable. Use begin_reusable_build_with_diagnostics when
 * the child plan is available. The caller holds membership exclusively.
 */
[[nodiscard]] inline reusable_registry_error
begin_reusable_build(reusable_registry_attachment& attachment,
                     const reusable_generation_reference& reference,
                     const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return attachment_error;
    }
    if (!callbacks.valid() || !reference ||
        reference.generation_slot >= header(attachment).config.max_generations) {
        return reusable_registry_error::invalid_argument;
    }
    const std::uint32_t partition = reusable_exact_partition(attachment, reference.id.digest);
    acquire_exact(callbacks, partition);
    const reusable_shared_generation& generation =
        generations(attachment)[reference.generation_slot];
    if (!reference_matches(generation, reference)) {
        release_exact(callbacks, partition);
        return reusable_registry_error::stale_reference;
    }
    const reusable_producer_identity producer = generation.producer;
    release_exact(callbacks, partition);
    constexpr char unavailable_sql[] = "SQL capture unavailable";
    const reusable_diagnostic_text_update unavailable{
        .reference = reference,
        .producer = producer,
        .display_sql = std::span<const char>{unavailable_sql, sizeof(unavailable_sql) - 1},
    };
    return mutate_diagnostic_text(
        attachment, unavailable, diagnostic_mutation_mode::begin_building, callbacks);
}

/**
 * Saves the child plan diagnostics and marks the generation as building
 * under the same exact partition lock. The caller holds membership
 * exclusively while diagnostic text is allocated.
 */
[[nodiscard]] inline reusable_registry_error begin_reusable_build_with_diagnostics(
    reusable_registry_attachment& attachment,
    const reusable_diagnostic_text_update& update,
    const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    return mutate_diagnostic_text(
        attachment, update, diagnostic_mutation_mode::begin_building, callbacks);
}

/**
 * Replaces a producer's diagnostic text after checking generation
 * identity and ownership. The caller holds membership exclusively.
 * The old text is freed after the replacement is saved.
 */
[[nodiscard]] inline reusable_registry_error store_reusable_diagnostic_text(
    reusable_registry_attachment& attachment,
    const reusable_diagnostic_text_update& update,
    const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    return mutate_diagnostic_text(attachment, update, diagnostic_mutation_mode::replace, callbacks);
}

/**
 * Copies SQL and plan diagnostics into caller buffers after the
 * PostgreSQL adapter authorizes access to the row. The caller holds
 * shared membership throughout the copy so the text stays allocated.
 */
[[nodiscard]] inline std::expected<reusable_diagnostic_text_copy, reusable_registry_error>
copy_reusable_diagnostic_text(
    reusable_registry_attachment& attachment,
    const reusable_generation_reference& reference,
    std::span<char> display_sql,
    std::span<char> canonical_plan,
    std::span<char> producer_plan,
    const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return std::unexpected(attachment_error);
    }
    if (!callbacks.valid() || !reference ||
        reference.generation_slot >= header(attachment).config.max_generations) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    const std::uint32_t partition = reusable_exact_partition(attachment, reference.id.digest);
    acquire_exact(callbacks, partition);
    const reusable_shared_generation& generation =
        generations(attachment)[reference.generation_slot];
    if (!reference_matches(generation, reference)) {
        release_exact(callbacks, partition);
        return std::unexpected(reusable_registry_error::stale_reference);
    }
    if (!shared_diagnostic_layout_valid(attachment, generation)) {
        release_exact(callbacks, partition);
        return std::unexpected(reusable_registry_error::corrupt_generation);
    }
    const diagnostic_byte_bundle ranges = diagnostic_bundle(generation);
    const reusable_diagnostic_text_copy result{
        .canonical_plan_status = generation.canonical_plan_snapshot_status,
        .plan_status = generation.plan_snapshot_status,
        .display_sql_truncated = generation.display_sql_truncated,
        .canonical_plan_truncated = generation.canonical_plan_truncated,
        .producer_plan_truncated = generation.producer_plan_truncated,
        .display_sql_bytes = ranges.display_sql.length,
        .canonical_plan_bytes = ranges.canonical_plan.length,
        .producer_plan_bytes = ranges.producer_plan.length,
    };
    release_exact(callbacks, partition);

    if (!shared_diagnostic_text_valid(attachment, generation)) {
        return std::unexpected(reusable_registry_error::corrupt_generation);
    }
    if (display_sql.size() < result.display_sql_bytes ||
        canonical_plan.size() < result.canonical_plan_bytes ||
        producer_plan.size() < result.producer_plan_bytes) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    if (result.display_sql_bytes != 0) {
        std::memcpy(display_sql.data(),
                    attachment.region.data() + ranges.display_sql.offset,
                    result.display_sql_bytes);
    }
    if (result.canonical_plan_bytes != 0) {
        std::memcpy(canonical_plan.data(),
                    attachment.region.data() + ranges.canonical_plan.offset,
                    result.canonical_plan_bytes);
    }
    if (result.producer_plan_bytes != 0) {
        std::memcpy(producer_plan.data(),
                    attachment.region.data() + ranges.producer_plan.offset,
                    result.producer_plan_bytes);
    }
    return result;
}

/**
 * Publishes the result of a completed build. The caller seals its
 * storage manifest first and holds membership exclusively. The
 * generation becomes COMPLETE after its manifest and counts are saved.
 */
[[nodiscard]] inline reusable_registry_error
publish_reusable_generation(reusable_registry_attachment& attachment,
                            const reusable_publish_request& request,
                            const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return attachment_error;
    }
    if (!callbacks.valid() || !request.reference || !request.manifest ||
        request.reference.generation_slot >= header(attachment).config.max_generations ||
        request.manifest.reserved != 0 || request.manifest.id != request.reference.id ||
        request.completed_monotonic_ns <
            generations(attachment)[request.reference.generation_slot].started_monotonic_ns) {
        return reusable_registry_error::invalid_argument;
    }
    reusable_shared_generation& generation =
        generations(attachment)[request.reference.generation_slot];
    if (!reference_matches(generation, request.reference)) {
        return reusable_registry_error::stale_reference;
    }
    if (!legal_state_transition(generation.state, cache_entry_state::complete)) {
        return reusable_registry_error::invalid_state;
    }
    const reusable_registry_error count_error =
        adjust_terminal_counts(attachment, generation, true, callbacks);
    if (count_error != reusable_registry_error::none) {
        return count_error;
    }
    generation.manifest = request.manifest;
    generation.completed_monotonic_ns = request.completed_monotonic_ns;
    generation.last_access_monotonic_ns = request.completed_monotonic_ns;
    generation.row_count = request.row_count;
    generation.metadata.logical_bytes = request.logical_bytes;
    generation.metadata.stored_bytes = request.stored_bytes;
    generation.failure = cache_failure_code::none;
    generation.state = cache_entry_state::complete;
    return reusable_registry_error::none;
}

/** Records a producer failure while keeping the generation available to its waiters. */
[[nodiscard]] inline reusable_registry_error
fail_reusable_generation(reusable_registry_attachment& attachment,
                         const reusable_generation_reference& reference,
                         cache_failure_code failure,
                         const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return attachment_error;
    }
    if (!callbacks.valid() || !reference || failure == cache_failure_code::none ||
        reference.generation_slot >= header(attachment).config.max_generations) {
        return reusable_registry_error::invalid_argument;
    }
    reusable_shared_generation& generation = generations(attachment)[reference.generation_slot];
    if (!reference_matches(generation, reference)) {
        return reusable_registry_error::stale_reference;
    }
    if (!legal_state_transition(generation.state, cache_entry_state::failed)) {
        return reusable_registry_error::invalid_state;
    }
    const reusable_registry_error count_error =
        adjust_terminal_counts(attachment, generation, true, callbacks);
    if (count_error != reusable_registry_error::none) {
        return count_error;
    }
    generation.failure = failure;
    generation.state = cache_entry_state::failed;
    return reusable_registry_error::none;
}

/** Marks a generation invalid so it cannot acquire new references. Existing references stay in place. */
[[nodiscard]] inline reusable_registry_error invalidate_reusable_generation(
    reusable_registry_attachment& attachment,
    const reusable_generation_reference& reference,
    cache_failure_code failure = cache_failure_code::dependency_changed,
    const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return attachment_error;
    }
    if (!callbacks.valid() || !reference || failure == cache_failure_code::none ||
        reference.generation_slot >= header(attachment).config.max_generations) {
        return reusable_registry_error::invalid_argument;
    }
    reusable_shared_generation& generation = generations(attachment)[reference.generation_slot];
    if (!reference_matches(generation, reference)) {
        return reusable_registry_error::stale_reference;
    }
    if (!legal_state_transition(generation.state, cache_entry_state::invalidated)) {
        return reusable_registry_error::invalid_state;
    }
    if (!terminal_state(generation.state)) {
        const reusable_registry_error count_error =
            adjust_terminal_counts(attachment, generation, true, callbacks);
        if (count_error != reusable_registry_error::none) {
            return count_error;
        }
    }
    generation.failure = failure;
    generation.state = cache_entry_state::invalidated;
    ++header(attachment).invalidations;
    return reusable_registry_error::none;
}

/**
 * Invalidates matching cache entries in a bounded scan. Existing
 * references and storage blocks stay in place for their current users.
 * The caller holds membership exclusively.
 */
[[nodiscard]] inline reusable_invalidation_result invalidate_reusable_generations(
    reusable_registry_attachment& attachment,
    const reusable_invalidation_selector& selector,
    const reusable_registry_partition_callbacks& callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    reusable_invalidation_result result{
        .next_cursor = selector.cursor,
    };
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none || !callbacks.valid() ||
        !invalidation_selector_valid(selector)) {
        result.error = attachment_error != reusable_registry_error::none
                           ? attachment_error
                           : reusable_registry_error::invalid_argument;
        return result;
    }
    reusable_registry_header& shared = header(attachment);
    if (selector.cursor > shared.config.max_generations) {
        result.error = reusable_registry_error::invalid_argument;
        return result;
    }
    if (selector.cursor == shared.config.max_generations) {
        result.done = true;
        return result;
    }
    const std::uint32_t maximum_scan =
        std::min(selector.maximum_scan, k_maximum_reusable_invalidation_scan);
    const std::uint32_t scan_count =
        std::min(maximum_scan, shared.config.max_generations - selector.cursor);
    const std::uint32_t scan_end = selector.cursor + scan_count;
    reusable_shared_generation* records = generations(attachment);
    for (std::uint32_t slot = selector.cursor; slot < scan_end; ++slot) {
        result.next_cursor = slot + 1U;
        ++result.scanned_generations;
        reusable_shared_generation& generation = records[slot];
        if (generation.membership != reusable_record_membership::indexed ||
            !generation_matches_invalidation(generation, selector)) {
            continue;
        }
        ++result.matched_generations;
        result.waiter_references += generation.waiter_count;
        result.planner_pins += generation.planner_pin_count;
        result.reader_pins += generation.reader_pin_count;
        if (generation.waiter_count != 0 || generation.planner_pin_count != 0 ||
            generation.reader_pin_count != 0) {
            ++result.referenced_generations;
        }

        const reusable_generation_reference reference = make_reference(generation, slot);
        const cache_entry_state previous_state = generation.state;
        reusable_registry_error transition_error = reusable_registry_error::none;
        bool affected = false;
        if (previous_state == cache_entry_state::claimed ||
            previous_state == cache_entry_state::building) {
            transition_error = fail_reusable_generation(
                attachment, reference, cache_failure_code::dependency_changed, callbacks);
            if (transition_error == reusable_registry_error::none) {
                ++shared.invalidations;
                ++result.failed_inflight;
                affected = true;
            }
        } else if (previous_state == cache_entry_state::complete) {
            transition_error = invalidate_reusable_generation(
                attachment, reference, cache_failure_code::dependency_changed, callbacks);
            if (transition_error == reusable_registry_error::none) {
                ++result.invalidated_complete;
                affected = true;
            }
        } else if (previous_state == cache_entry_state::invalidated) {
            ++result.already_invalidated;
        } else if (previous_state == cache_entry_state::failed) {
            ++result.already_failed;
        } else {
            result.error = reusable_registry_error::corrupt_generation;
            return result;
        }
        if (transition_error != reusable_registry_error::none) {
            result.error = transition_error;
            return result;
        }
        if (!affected) {
            continue;
        }
        if (result.affected_count >= result.affected_references.size()) {
            result.error = reusable_registry_error::corrupt_layout;
            return result;
        }
        result.affected_references[result.affected_count] = reference;
        result.affected_manifests[result.affected_count] = generation.manifest;
        result.previous_states[result.affected_count] = previous_state;
        ++result.affected_count;
    }
    result.done = result.next_cursor == shared.config.max_generations;
    return result;
}

/**
 * Removes an unreferenced generation from the lookup directories and
 * queues it for cleanup. Its bytes stay allocated until dependency
 * references have been released.
 */
[[nodiscard]] inline std::expected<reusable_manifest_handle, reusable_registry_error>
retire_reusable_generation(reusable_registry_attachment& attachment,
                           const reusable_generation_reference& reference,
                           const reusable_registry_partition_callbacks& callbacks) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return std::unexpected(attachment_error);
    }
    if (!callbacks.valid() || !reference ||
        reference.generation_slot >= header(attachment).config.max_generations) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    reusable_registry_header& shared = header(attachment);
    reusable_shared_generation& generation = generations(attachment)[reference.generation_slot];
    if (!reference_matches(generation, reference)) {
        return std::unexpected(reusable_registry_error::stale_reference);
    }
    if (!terminal_state(generation.state) ||
        !legal_state_transition(generation.state, cache_entry_state::evicting) ||
        generation.waiter_count != 0 || generation.planner_pin_count != 0 ||
        generation.reader_pin_count != 0) {
        return std::unexpected(reusable_registry_error::invalid_state);
    }
    if (!shared_dependency_vector_valid(attachment, generation) ||
        !conservative_guard_valid(generation.conservative_guard,
                                  generation.metadata,
                                  generation.coverage_directory_key.security_namespace) ||
        !std::all_of(generation.reserved_dependency_release.begin(),
                     generation.reserved_dependency_release.end(),
                     [](std::uint8_t value) noexcept { return value == 0; })) {
        return std::unexpected(reusable_registry_error::corrupt_generation);
    }
    if (generation.dependency_release_state != reusable_dependency_release_state::retained ||
        generation.dependency_release_cursor != 0 || generation.dependency_release_sequence != 0) {
        return std::unexpected(reusable_registry_error::invalid_state);
    }
    if (shared.indexed_generations == 0) {
        return std::unexpected(reusable_registry_error::corrupt_generation);
    }

    const std::uint32_t exact_partition =
        reusable_exact_partition(attachment, generation.id.digest);
    acquire_exact(callbacks, exact_partition);
    const reusable_registry_error exact_error =
        unlink_exact_generation(attachment, reference.generation_slot);
    release_exact(callbacks, exact_partition);
    if (exact_error != reusable_registry_error::none) {
        return std::unexpected(exact_error);
    }

    if (generation.entry_kind == cache_entry_kind::reusable_coverage) {
        const std::uint32_t coverage_partition =
            reusable_coverage_partition(attachment, generation.coverage_directory_key);
        acquire_coverage(callbacks, coverage_partition);
        const reusable_registry_error coverage_error =
            unlink_coverage_generation(attachment, reference.generation_slot);
        release_coverage(callbacks, coverage_partition);
        if (coverage_error != reusable_registry_error::none) {
            acquire_exact(callbacks, exact_partition);
            const reusable_registry_error rollback_error =
                link_exact_generation(attachment, reference.generation_slot);
            release_exact(callbacks, exact_partition);
            return std::unexpected(
                rollback_error == reusable_registry_error::none ? coverage_error : rollback_error);
        }
    } else if (generation.entry_kind != cache_entry_kind::exact_final_result) {
        acquire_exact(callbacks, exact_partition);
        const reusable_registry_error rollback_error =
            link_exact_generation(attachment, reference.generation_slot);
        release_exact(callbacks, exact_partition);
        return std::unexpected(rollback_error == reusable_registry_error::none
                                   ? reusable_registry_error::corrupt_generation
                                   : rollback_error);
    }

    generation.state = cache_entry_state::evicting;
    generation.membership = reusable_record_membership::retired;
    generation.dependency_release_state = reusable_dependency_release_state::pending;
    generation.next_retired_or_free = shared.retired_generation_head;
    shared.retired_generation_head = reference.generation_slot;
    --shared.indexed_generations;
    ++shared.retired_generations;
    ++shared.evictions;
    return generation.manifest;
}

namespace reusable_registry_detail {

[[nodiscard]] inline bool
arena_allocation_reclaimable(const reusable_registry_attachment& attachment,
                             arena_allocation allocation) noexcept {
    if (allocation.length == 0) {
        return allocation.offset == 0;
    }
    const reusable_registry_header& shared = header(attachment);
    const std::uint64_t end = static_cast<std::uint64_t>(allocation.offset) + allocation.length;
    if (allocation.offset < shared.layout.byte_arena_offset || allocation.offset % 8U != 0 ||
        allocation.length % 8U != 0 ||
        end > static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
                  shared.config.byte_arena_bytes) {
        return false;
    }
    const reusable_free_extent* extents = free_extents(attachment);
    for (std::uint32_t index = 0; index < shared.free_extent_count; ++index) {
        if (arena_allocations_overlap(allocation,
                                      arena_allocation{.offset = extents[index].offset,
                                                       .length = extents[index].length})) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool
dependency_release_ticket_matches(const reusable_registry_attachment& attachment,
                                  const reusable_shared_generation& generation,
                                  const reusable_dependency_release_ticket& ticket) noexcept {
    if (!reference_matches(generation, ticket.reference, true) ||
        generation.membership != reusable_record_membership::retired ||
        generation.state != cache_entry_state::evicting ||
        generation.dependency_release_sequence != ticket.release_sequence ||
        generation.metadata.dependency_count != ticket.dependency_count ||
        generation_reference_count(generation) != ticket.reference_count ||
        generation.conservative_guard != ticket.conservative_guard ||
        generation.dependency_vector.length != ticket.vector_bytes ||
        generation.manifest != ticket.manifest ||
        !conservative_guard_valid(generation.conservative_guard,
                                  generation.metadata,
                                  generation.coverage_directory_key.security_namespace) ||
        !shared_dependency_vector_valid(attachment, generation)) {
        return false;
    }
    const std::span<const std::byte> envelope{
        attachment.region.data() + generation.dependency_vector.offset,
        generation.dependency_vector.length,
    };
    const dependency_vector_envelope_result validation =
        validate_dependency_vector_envelope(envelope);
    return validation && validation.header.payload_checksum == ticket.payload_checksum;
}

} // namespace reusable_registry_detail

/**
 * Copies a retired generation's dependency data and claims its release
 * work while the caller holds membership exclusively. The caller drops
 * all registry locks before decoding the copy and releasing those
 * dependencies. A short output buffer reports the required size.
 */
[[nodiscard]] inline reusable_dependency_release_preparation
prepare_reusable_dependency_release(reusable_registry_attachment& attachment,
                                    std::span<std::byte> copied_dependency_vector) noexcept {
    using namespace reusable_registry_detail;
    reusable_dependency_release_preparation result{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        result.error = attachment_error;
        return result;
    }

    reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    std::uint32_t current = shared.retired_generation_head;
    std::uint32_t visited = 0;
    while (current != k_invalid_reusable_registry_slot && visited < shared.config.max_generations) {
        if (current >= shared.config.max_generations) {
            result.error = reusable_registry_error::corrupt_generation;
            return result;
        }
        reusable_shared_generation& generation = records[current];
        if (generation.membership != reusable_record_membership::retired ||
            generation.state != cache_entry_state::evicting || generation.waiter_count != 0 ||
            generation.planner_pin_count != 0 || generation.reader_pin_count != 0 ||
            generation.dependency_release_cursor > generation_reference_count(generation) ||
            !std::all_of(generation.reserved_dependency_release.begin(),
                         generation.reserved_dependency_release.end(),
                         [](std::uint8_t value) noexcept { return value == 0; }) ||
            !conservative_guard_valid(generation.conservative_guard,
                                      generation.metadata,
                                      generation.coverage_directory_key.security_namespace) ||
            !shared_dependency_vector_valid(attachment, generation)) {
            result.error = reusable_registry_error::corrupt_generation;
            return result;
        }
        const reusable_dependency_release_state release_state = generation.dependency_release_state;
        if (release_state == reusable_dependency_release_state::none ||
            release_state == reusable_dependency_release_state::retained ||
            (release_state == reusable_dependency_release_state::released &&
             generation.dependency_release_cursor != generation_reference_count(generation)) ||
            (release_state == reusable_dependency_release_state::prepared &&
             generation.dependency_release_sequence == 0)) {
            result.error = reusable_registry_error::corrupt_generation;
            return result;
        }
        if (release_state == reusable_dependency_release_state::pending ||
            release_state == reusable_dependency_release_state::released) {
            const std::span<const std::byte> envelope{
                attachment.region.data() + generation.dependency_vector.offset,
                generation.dependency_vector.length,
            };
            const dependency_vector_envelope_result validation =
                validate_dependency_vector_envelope(envelope);
            if (!validation ||
                validation.header.entry_count != generation.metadata.dependency_count) {
                result.error = reusable_registry_error::corrupt_generation;
                return result;
            }
            result.required_vector_bytes = generation.dependency_vector.length;
            if (copied_dependency_vector.size() < result.required_vector_bytes) {
                result.error = reusable_registry_error::region_too_small;
                return result;
            }
            const std::uint64_t release_sequence =
                take_sequence(shared.next_dependency_release_sequence);
            if (release_sequence == 0) {
                result.error = reusable_registry_error::counter_overflow;
                return result;
            }
            std::memcpy(copied_dependency_vector.data(), envelope.data(), envelope.size());
            generation.dependency_release_sequence = release_sequence;
            generation.dependency_release_state = reusable_dependency_release_state::prepared;
            result.ticket = reusable_dependency_release_ticket{
                .reference = make_reference(generation, current),
                .manifest = generation.manifest,
                .conservative_guard = generation.conservative_guard,
                .release_sequence = release_sequence,
                .payload_checksum = validation.header.payload_checksum,
                .first_reference = generation.dependency_release_cursor,
                .reference_count = generation_reference_count(generation),
                .dependency_count = generation.metadata.dependency_count,
                .vector_bytes = generation.dependency_vector.length,
            };
            result.copied_vector_bytes = generation.dependency_vector.length;
            return result;
        }
        current = generation.next_retired_or_free;
        ++visited;
    }
    if (current != k_invalid_reusable_registry_slot) {
        result.error = reusable_registry_error::corrupt_generation;
        return result;
    }
    result.error = reusable_registry_error::no_reclaimable_generation;
    return result;
}

/**
 * Records the consecutive dependency releases that took effect,
 * including releases that also reported a diagnostic error. Count
 * each call with `applied == true`. A partial release leaves the next
 * attempt starting at the first unreleased dependency.
 */
[[nodiscard]] inline std::expected<bool, reusable_registry_error>
record_reusable_dependency_release_progress(reusable_registry_attachment& attachment,
                                            const reusable_dependency_release_ticket& ticket,
                                            std::uint32_t released_dependencies) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return std::unexpected(attachment_error);
    }
    if (!ticket || ticket.reference.generation_slot >= header(attachment).config.max_generations) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    reusable_shared_generation& generation =
        generations(attachment)[ticket.reference.generation_slot];
    if (!dependency_release_ticket_matches(attachment, generation, ticket)) {
        return std::unexpected(reusable_registry_error::stale_reference);
    }
    const std::uint32_t remaining = ticket.reference_count - ticket.first_reference;
    if (released_dependencies > remaining) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    if (generation.dependency_release_state == reusable_dependency_release_state::released &&
        generation.dependency_release_cursor == ticket.reference_count &&
        released_dependencies == remaining) {
        return true;
    }
    if (generation.dependency_release_state != reusable_dependency_release_state::prepared ||
        generation.dependency_release_cursor != ticket.first_reference) {
        return std::unexpected(reusable_registry_error::stale_reference);
    }
    generation.dependency_release_cursor = ticket.first_reference + released_dependencies;
    const bool complete =
        generation.dependency_release_cursor == generation_reference_count(generation);
    generation.dependency_release_state = complete ? reusable_dependency_release_state::released
                                                   : reusable_dependency_release_state::pending;
    if (!complete) {
        generation.dependency_release_sequence = 0;
    }
    return complete;
}

/**
 * Frees a retired generation's bytes and returns its slot for reuse
 * after all dependency references are released. The ticket must still
 * identify the same generation and release attempt. The caller holds
 * membership exclusively.
 */
[[nodiscard]] inline std::expected<reusable_manifest_handle, reusable_registry_error>
finalize_reusable_generation_reclaim(reusable_registry_attachment& attachment,
                                     const reusable_dependency_release_ticket& ticket) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return std::unexpected(attachment_error);
    }
    reusable_registry_header& shared = header(attachment);
    if (!ticket || ticket.reference.generation_slot >= shared.config.max_generations) {
        return std::unexpected(reusable_registry_error::invalid_argument);
    }
    reusable_shared_generation* records = generations(attachment);
    reusable_shared_generation& generation = records[ticket.reference.generation_slot];
    if (generation.dependency_release_state != reusable_dependency_release_state::released ||
        !dependency_release_ticket_matches(attachment, generation, ticket) ||
        generation.dependency_release_cursor != generation_reference_count(generation)) {
        return std::unexpected(reusable_registry_error::stale_reference);
    }
    if (!shared_diagnostic_text_valid(attachment, generation) || shared.retired_generations == 0) {
        return std::unexpected(reusable_registry_error::corrupt_generation);
    }

    std::uint32_t previous = k_invalid_reusable_registry_slot;
    std::uint32_t current = shared.retired_generation_head;
    std::uint32_t visited = 0;
    while (current != k_invalid_reusable_registry_slot &&
           current != ticket.reference.generation_slot && visited < shared.config.max_generations) {
        if (current >= shared.config.max_generations ||
            records[current].membership != reusable_record_membership::retired) {
            return std::unexpected(reusable_registry_error::corrupt_generation);
        }
        previous = current;
        current = records[current].next_retired_or_free;
        ++visited;
    }
    if (current != ticket.reference.generation_slot) {
        return std::unexpected(reusable_registry_error::corrupt_generation);
    }

    const arena_allocation identity{
        .offset = generation.allocation_offset,
        .length = generation.allocation_length,
    };
    const diagnostic_byte_bundle diagnostics = diagnostic_bundle(generation);
    const std::array<arena_allocation, 4> allocations{
        identity,
        diagnostic_allocation(diagnostics.display_sql),
        diagnostic_allocation(diagnostics.canonical_plan),
        diagnostic_allocation(diagnostics.producer_plan),
    };
    std::uint32_t allocation_count = 0;
    for (const arena_allocation allocation : allocations) {
        if (allocation.length != 0) {
            ++allocation_count;
        }
        if (!arena_allocation_reclaimable(attachment, allocation)) {
            return std::unexpected(reusable_registry_error::corrupt_layout);
        }
    }
    if (shared.free_extent_count + allocation_count > shared.layout.free_extent_capacity) {
        return std::unexpected(reusable_registry_error::corrupt_layout);
    }

    if (!free_diagnostic_bundle(attachment, diagnostics) ||
        !free_arena_bytes(attachment, identity)) {
        return std::unexpected(reusable_registry_error::corrupt_layout);
    }
    if (previous == k_invalid_reusable_registry_slot) {
        shared.retired_generation_head = generation.next_retired_or_free;
    } else {
        records[previous].next_retired_or_free = generation.next_retired_or_free;
    }
    const reusable_manifest_handle manifest = generation.manifest;
    --shared.retired_generations;
    return_generation_record(attachment, ticket.reference.generation_slot);
    return manifest;
}

/**
 * Copies producer and reference-owner identities for the next cleanup
 * scan. The caller holds shared membership and checks backend liveness
 * after releasing all registry locks. If the output is truncated,
 * skip reference cleanup for this sample.
 */
[[nodiscard]] inline reusable_janitor_liveness_snapshot snapshot_reusable_janitor_liveness(
    const reusable_registry_attachment& attachment,
    std::span<reusable_janitor_owner_observation> observations,
    std::uint32_t maximum = k_maximum_reusable_janitor_batch,
    const reusable_registry_partition_callbacks& partition_callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    reusable_janitor_liveness_snapshot result{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none || !partition_callbacks.valid()) {
        result.error = attachment_error != reusable_registry_error::none
                           ? attachment_error
                           : reusable_registry_error::invalid_argument;
        return result;
    }
    const reusable_registry_header& shared = header(attachment);
    const reusable_shared_generation* records = generations(attachment);
    maximum = std::min(maximum, k_maximum_reusable_janitor_batch);
    const std::uint32_t scan_limit = std::min(maximum, shared.config.max_generations);
    result.scan_start = shared.janitor_scan_cursor;
    for (std::uint32_t scanned = 0; scanned < scan_limit; ++scanned) {
        const std::uint32_t slot = (result.scan_start + scanned) % shared.config.max_generations;
        const reusable_shared_generation& generation = records[slot];
        ++result.scanned_generations;
        if (generation.membership != reusable_record_membership::indexed) {
            continue;
        }
        const reusable_generation_reference reference = make_reference(generation, slot);
        const std::uint32_t exact_partition =
            reusable_exact_partition(attachment, reference.id.digest);
        acquire_exact(partition_callbacks, exact_partition);
        if (!reference_matches(generation, reference)) {
            release_exact(partition_callbacks, exact_partition);
            continue;
        }
        if (generation.state == cache_entry_state::claimed ||
            generation.state == cache_entry_state::building) {
            const reusable_janitor_owner_observation observation =
                make_producer_observation(generation, slot);
            if (!observation.valid()) {
                release_exact(partition_callbacks, exact_partition);
                result.error = reusable_registry_error::corrupt_generation;
                return result;
            }
            if (result.count >= observations.size()) {
                result.truncated = true;
            } else {
                observations[result.count++] = observation;
            }
        }
        for (std::uint32_t owner_slot = 0; owner_slot < k_reusable_reference_owners_per_generation;
             ++owner_slot) {
            const reusable_shared_reference_owner& owner = generation.reference_owners[owner_slot];
            if (!owner.active) {
                continue;
            }
            const reusable_janitor_owner_observation observation =
                make_reference_owner_observation(generation, slot, owner, owner_slot);
            if (!observation.valid()) {
                release_exact(partition_callbacks, exact_partition);
                result.error = reusable_registry_error::corrupt_generation;
                return result;
            }
            if (result.count >= observations.size()) {
                result.truncated = true;
                break;
            }
            observations[result.count++] = observation;
        }
        release_exact(partition_callbacks, exact_partition);
        if (result.truncated) {
            break;
        }
    }
    return result;
}

/**
 * Cleans up a bounded group of cache entries by releasing references
 * left by dead owners and retiring unused expired, failed or invalid
 * results. The liveness callback must return without waiting because
 * it runs under an exact partition lock.
 */
[[nodiscard]] inline reusable_janitor_sweep_result janitor_reusable_registry(
    reusable_registry_attachment& attachment,
    std::uint64_t now_monotonic_ns,
    std::uint32_t maximum = k_maximum_reusable_janitor_batch,
    const reusable_janitor_callbacks& janitor_callbacks = {},
    const reusable_registry_partition_callbacks& partition_callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    reusable_janitor_sweep_result result{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none || !partition_callbacks.valid()) {
        result.error = attachment_error != reusable_registry_error::none
                           ? attachment_error
                           : reusable_registry_error::invalid_argument;
        return result;
    }
    maximum = std::min(maximum, k_maximum_reusable_janitor_batch);
    if (maximum == 0) {
        return result;
    }

    reusable_registry_header& shared = header(attachment);
    reusable_shared_generation* records = generations(attachment);
    const std::uint32_t remaining = maximum;
    const std::uint32_t scan_limit = std::min(remaining, shared.config.max_generations);
    const std::uint32_t scan_start = shared.janitor_scan_cursor;
    std::uint32_t scanned = 0;
    std::uint32_t retired_this_pass = 0;
    std::uint32_t reaped_this_pass = 0;
    for (; scanned < scan_limit && retired_this_pass + reaped_this_pass < remaining; ++scanned) {
        const std::uint32_t slot = (scan_start + scanned) % shared.config.max_generations;
        reusable_shared_generation& generation = records[slot];
        if (generation.membership != reusable_record_membership::indexed) {
            continue;
        }
        const reusable_generation_reference reference = make_reference(generation, slot);
        const std::uint32_t exact_partition =
            reusable_exact_partition(attachment, reference.id.digest);
        acquire_exact(partition_callbacks, exact_partition);
        if (!reference_matches(generation, reference)) {
            release_exact(partition_callbacks, exact_partition);
            continue;
        }
        if (janitor_callbacks.owner_alive != nullptr) {
            for (std::uint32_t owner_slot = 0;
                 owner_slot < k_reusable_reference_owners_per_generation;
                 ++owner_slot) {
                reusable_shared_reference_owner& shared_owner =
                    generation.reference_owners[owner_slot];
                if (retired_this_pass + reaped_this_pass >= remaining) {
                    break;
                }
                if (!shared_owner.active) {
                    continue;
                }
                const reusable_janitor_owner_observation observation =
                    make_reference_owner_observation(generation, slot, shared_owner, owner_slot);
                if (!observation.valid()) {
                    release_exact(partition_callbacks, exact_partition);
                    result.error = reusable_registry_error::corrupt_generation;
                    return result;
                }
                if (janitor_callbacks.owner_alive(janitor_callbacks.context, observation)) {
                    continue;
                }
                const reusable_reference_kind reaped_kind = shared_owner.kind;
                const reusable_registry_error reap_error =
                    deactivate_reference_owner(generation, shared_owner);
                if (reap_error != reusable_registry_error::none) {
                    release_exact(partition_callbacks, exact_partition);
                    result.error = reap_error;
                    return result;
                }
                if (reaped_kind == reusable_reference_kind::waiter) {
                    ++result.reaped_waiters;
                } else if (reaped_kind == reusable_reference_kind::planner_pin) {
                    ++result.reaped_planner_pins;
                } else if (reaped_kind == reusable_reference_kind::reader_pin) {
                    ++result.reaped_reader_pins;
                } else {
                    release_exact(partition_callbacks, exact_partition);
                    result.error = reusable_registry_error::corrupt_generation;
                    return result;
                }
                ++reaped_this_pass;
            }
        }
        bool producer_dead = false;
        if ((generation.state == cache_entry_state::claimed ||
             generation.state == cache_entry_state::building) &&
            janitor_callbacks.owner_alive != nullptr) {
            const reusable_janitor_owner_observation observation =
                make_producer_observation(generation, slot);
            if (!observation.valid()) {
                release_exact(partition_callbacks, exact_partition);
                result.error = reusable_registry_error::corrupt_generation;
                return result;
            }
            producer_dead = !janitor_callbacks.owner_alive(janitor_callbacks.context, observation);
        }
        release_exact(partition_callbacks, exact_partition);

        if (producer_dead) {
            const reusable_registry_error failure_error = fail_reusable_generation(
                attachment, reference, cache_failure_code::producer_exited, partition_callbacks);
            if (failure_error != reusable_registry_error::none) {
                result.error = failure_error;
                return result;
            }
            if (!remember_janitor_affected_reference(result, reference)) {
                result.error = reusable_registry_error::corrupt_layout;
                return result;
            }
            ++result.failed_dead_producers;
        }

        enum class retire_reason : std::uint8_t {
            none,
            expired,
            failed,
            invalidated,
        };
        retire_reason reason = retire_reason::none;
        acquire_exact(partition_callbacks, exact_partition);
        if (!reference_matches(generation, reference)) {
            release_exact(partition_callbacks, exact_partition);
            continue;
        }
        if (generation.state == cache_entry_state::complete &&
            completed_generation_expired(
                generation, generation.metadata.producer_ttl_ms, now_monotonic_ns)) {
            reason = retire_reason::expired;
        } else if (generation.state == cache_entry_state::failed) {
            reason = retire_reason::failed;
        } else if (generation.state == cache_entry_state::invalidated) {
            reason = retire_reason::invalidated;
        }
        const bool referenced = generation.waiter_count != 0 || generation.planner_pin_count != 0 ||
                                generation.reader_pin_count != 0;
        release_exact(partition_callbacks, exact_partition);
        if (reason == retire_reason::none || referenced ||
            retired_this_pass + reaped_this_pass >= remaining) {
            continue;
        }
        auto manifest = retire_reusable_generation(attachment, reference, partition_callbacks);
        if (!manifest.has_value()) {
            if (manifest.error() == reusable_registry_error::invalid_state ||
                manifest.error() == reusable_registry_error::stale_reference) {
                continue;
            }
            result.error = manifest.error();
            return result;
        }
        if (!remember_janitor_affected_reference(result, reference)) {
            result.error = reusable_registry_error::corrupt_layout;
            return result;
        }
        if (static_cast<bool>(*manifest) &&
            result.retired_manifest_count < result.retired_manifests.size()) {
            result.retired_manifests[result.retired_manifest_count++] = *manifest;
        }
        if (reason == retire_reason::expired) {
            ++result.retired_expired;
        } else if (reason == retire_reason::failed) {
            ++result.retired_failed;
        } else if (reason == retire_reason::invalidated) {
            ++result.retired_invalidated;
        }
        ++retired_this_pass;
    }
    shared.janitor_scan_cursor = (scan_start + scanned) % shared.config.max_generations;
    return result;
}

/**
 * Copies diagnostic rows without SQL text or cache key bytes. The
 * caller holds shared membership. Each row is copied under its exact
 * partition lock when lock callbacks are supplied.
 */
[[nodiscard]] inline reusable_generation_diagnostics_result
snapshot_reusable_generation_diagnostics(
    const reusable_registry_attachment& attachment,
    std::span<reusable_generation_diagnostics_row> rows,
    const reusable_registry_partition_callbacks& partition_callbacks = {}) noexcept {
    using namespace reusable_registry_detail;
    reusable_generation_diagnostics_result result{};
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none || !partition_callbacks.valid()) {
        result.error = attachment_error != reusable_registry_error::none
                           ? attachment_error
                           : reusable_registry_error::invalid_argument;
        return result;
    }

    const reusable_registry_header& shared = header(attachment);
    result.indexed_generation_count = shared.indexed_generations;
    if (rows.empty()) {
        result.truncated = shared.indexed_generations != 0;
        return result;
    }

    const reusable_shared_generation* records = generations(attachment);
    for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
        const reusable_shared_generation& generation = records[slot];
        if (generation.membership != reusable_record_membership::indexed) {
            continue;
        }
        if (result.count >= rows.size()) {
            result.truncated = true;
            break;
        }

        const reusable_generation_reference reference = make_reference(generation, slot);
        if (!reference) {
            result.error = reusable_registry_error::corrupt_generation;
            return result;
        }
        const std::uint32_t exact_partition =
            reusable_exact_partition(attachment, reference.id.digest);
        acquire_exact(partition_callbacks, exact_partition);
        const bool known_kind = generation.entry_kind == cache_entry_kind::exact_final_result ||
                                generation.entry_kind == cache_entry_kind::reusable_coverage;
        const bool known_state = generation.state == cache_entry_state::claimed ||
                                 generation.state == cache_entry_state::building ||
                                 generation.state == cache_entry_state::complete ||
                                 generation.state == cache_entry_state::failed ||
                                 generation.state == cache_entry_state::invalidated;
        if (!reference_matches(generation, reference) || !known_kind || !known_state ||
            !shared_diagnostic_text_valid(attachment, generation)) {
            release_exact(partition_callbacks, exact_partition);
            result.error = reusable_registry_error::corrupt_generation;
            return result;
        }
        rows[result.count++] = make_diagnostics_row(generation, slot);
        release_exact(partition_callbacks, exact_partition);
    }
    if (!result.truncated && result.count != shared.indexed_generations) {
        result.error = reusable_registry_error::corrupt_generation;
    }
    return result;
}

/** Counts generations, references and free space for registry diagnostics, excluding query text and cache keys. */
[[nodiscard]] inline reusable_registry_snapshot
snapshot_reusable_registry(const reusable_registry_attachment& attachment) noexcept {
    using namespace reusable_registry_detail;
    reusable_registry_snapshot result{};
    if (validate_reusable_registry_attachment(attachment) != reusable_registry_error::none) {
        return result;
    }
    const reusable_registry_header& shared = header(attachment);
    result.boot_nonce = shared.boot_nonce;
    result.indexed_generations = shared.indexed_generations;
    result.retired_generations = shared.retired_generations;
    result.exact_occupied_buckets = shared.exact_occupied_buckets;
    result.coverage_occupied_buckets = shared.coverage_occupied_buckets;
    result.evictions = shared.evictions;
    result.invalidations = shared.invalidations;
    const reusable_shared_generation* records = generations(attachment);
    for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
        const reusable_shared_generation& generation = records[slot];
        if (generation.membership == reusable_record_membership::free) {
            ++result.free_generations;
            continue;
        }
        result.waiter_references += generation.waiter_count;
        result.planner_pins += generation.planner_pin_count;
        result.reader_pins += generation.reader_pin_count;
        if (generation.membership != reusable_record_membership::indexed) {
            continue;
        }
        if (generation.entry_kind == cache_entry_kind::exact_final_result) {
            ++result.exact_final_generations;
        } else if (generation.entry_kind == cache_entry_kind::reusable_coverage) {
            ++result.reusable_coverage_generations;
        } else {
            return reusable_registry_snapshot{};
        }
        if (generation.state == cache_entry_state::claimed ||
            generation.state == cache_entry_state::building) {
            ++result.inflight_generations;
        } else if (generation.state == cache_entry_state::complete) {
            ++result.complete_generations;
        } else if (generation.state == cache_entry_state::failed) {
            ++result.failed_generations;
        } else if (generation.state == cache_entry_state::invalidated) {
            ++result.invalidated_generations;
        }
    }
    const reusable_free_extent* extents =
        array_at<reusable_free_extent>(attachment, shared.layout.free_extents_offset);
    for (std::uint32_t index = 0; index < shared.free_extent_count; ++index) {
        result.arena_free_bytes += extents[index].length;
    }
    return result;
}

/**
 * Checks generation records, lookup directories and shared-memory
 * allocations for inconsistencies. The caller holds membership
 * exclusively and prevents changes to both directories during the check.
 */
[[nodiscard]] inline reusable_registry_error
validate_reusable_registry(const reusable_registry_attachment& attachment) noexcept {
    using namespace reusable_registry_detail;
    const reusable_registry_error attachment_error =
        validate_reusable_registry_attachment(attachment);
    if (attachment_error != reusable_registry_error::none) {
        return attachment_error;
    }

    const reusable_registry_header& shared = header(attachment);
    const reusable_shared_generation* records = generations(attachment);
    std::uint32_t indexed_records = 0;
    std::uint32_t indexed_exact_records = 0;
    std::uint32_t indexed_coverage_records = 0;
    std::uint32_t retired_records = 0;
    std::uint32_t free_records = 0;
    std::uint64_t allocated_bytes = 0;
    for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
        const reusable_shared_generation& generation = records[slot];
        std::uint32_t waiter_owners = 0;
        std::uint32_t planner_owners = 0;
        std::uint32_t reader_owners = 0;
        for (const reusable_shared_reference_owner& shared_owner : generation.reference_owners) {
            const bool reserved_clear =
                std::all_of(shared_owner.reserved.begin(),
                            shared_owner.reserved.end(),
                            [](std::uint8_t value) noexcept { return value == 0; });
            if (!shared_owner.active) {
                if (shared_owner.kind != reusable_reference_kind::none ||
                    shared_owner.owner_sequence != 0 ||
                    shared_owner.owner != reusable_reference_owner{} || !reserved_clear) {
                    return reusable_registry_error::corrupt_generation;
                }
                continue;
            }
            if (!shared_owner.owner.valid() || shared_owner.owner_sequence == 0 ||
                !reserved_clear) {
                return reusable_registry_error::corrupt_generation;
            }
            if (shared_owner.kind == reusable_reference_kind::waiter) {
                ++waiter_owners;
            } else if (shared_owner.kind == reusable_reference_kind::planner_pin) {
                ++planner_owners;
            } else if (shared_owner.kind == reusable_reference_kind::reader_pin) {
                ++reader_owners;
            } else {
                return reusable_registry_error::corrupt_generation;
            }
        }
        if (waiter_owners != generation.waiter_count ||
            planner_owners != generation.planner_pin_count ||
            reader_owners != generation.reader_pin_count) {
            return reusable_registry_error::corrupt_generation;
        }
        if (generation.membership == reusable_record_membership::free) {
            ++free_records;
            if (generation.state != cache_entry_state::absent || generation.id.boot_nonce != 0 ||
                generation.id.generation != 0 || generation.id.claim_sequence != 0 ||
                generation.allocation_offset != 0 || generation.allocation_length != 0 ||
                generation.canonical_generation_key || generation.canonical_domain_key ||
                generation.coverage_descriptor || generation.dependency_vector ||
                generation.conservative_guard != reusable_conservative_guard{} ||
                generation.dependency_release_cursor != 0 ||
                generation.dependency_release_state != reusable_dependency_release_state::none ||
                generation.dependency_release_sequence != 0 ||
                !std::all_of(generation.reserved_dependency_release.begin(),
                             generation.reserved_dependency_release.end(),
                             [](std::uint8_t value) noexcept { return value == 0; }) ||
                !empty_range_shape(generation.display_sql) ||
                !empty_range_shape(generation.canonical_plan) ||
                !empty_range_shape(generation.producer_plan) || generation.waiter_count != 0 ||
                generation.planner_pin_count != 0 || generation.reader_pin_count != 0 ||
                !shared_diagnostic_text_valid(attachment, generation) ||
                generation.next_in_exact_bucket != k_invalid_reusable_registry_slot ||
                generation.next_in_coverage_bucket != k_invalid_reusable_registry_slot) {
                return reusable_registry_error::corrupt_generation;
            }
            continue;
        }
        if (generation.membership != reusable_record_membership::indexed &&
            generation.membership != reusable_record_membership::retired) {
            return reusable_registry_error::corrupt_generation;
        }
        const bool known_kind = known_entry_kind(generation.entry_kind);
        const bool is_coverage = generation.entry_kind == cache_entry_kind::reusable_coverage;
        const bool provider_metadata_consistent =
            generation.metadata.provider_id == 0
                ? generation.metadata.provider_abi_revision == 0 &&
                      generation.metadata.provider_domain_abi_revision == 0 &&
                      generation.metadata.observability != write_observability::provider_complete
                : generation.metadata.provider_abi_revision != 0 &&
                      generation.metadata.provider_domain_abi_revision != 0 &&
                      generation.metadata.observability == write_observability::provider_complete;
        const bool kind_metadata_valid =
            is_coverage
                ? generation.metadata.provider_id != 0 &&
                      generation.metadata.provider_abi_revision != 0 &&
                      generation.metadata.provider_domain_abi_revision != 0 &&
                      generation.metadata.compatibility_family_id != 0 &&
                      generation.metadata.observability == write_observability::provider_complete
                : provider_metadata_consistent;
        if (!known_kind || !kind_metadata_valid || generation.slot_sequence == 0 ||
            generation.id.boot_nonce != shared.boot_nonce || generation.id.generation == 0 ||
            generation.id.claim_sequence == 0 || !generation.producer.valid() ||
            !valid_reusable_security_namespace(
                generation.coverage_directory_key.security_namespace) ||
            generation.metadata.dependency_count > k_maximum_dependencies_per_generation ||
            generation.metadata.reserved != 0 || generation.manifest.reserved != 0 ||
            !write_observability_valid(generation.metadata.observability) ||
            !conservative_guard_valid(generation.conservative_guard,
                                      generation.metadata,
                                      generation.coverage_directory_key.security_namespace) ||
            generation.metadata.dependency_physical_identity.length >
                generation.metadata.dependency_physical_identity.bytes.size() ||
            !std::all_of(generation.reserved_dependency_release.begin(),
                         generation.reserved_dependency_release.end(),
                         [](std::uint8_t value) noexcept { return value == 0; }) ||
            !shared_dependency_vector_valid(attachment, generation) ||
            !shared_diagnostic_text_valid(attachment, generation) ||
            !range_valid(attachment, generation.canonical_generation_key) ||
            generation.canonical_generation_key.length > k_maximum_canonical_key_bytes ||
            generation.coverage_descriptor.length > k_maximum_provider_recipe_bytes) {
            return reusable_registry_error::corrupt_generation;
        }
        const bool domain_valid =
            is_coverage
                ? range_valid(attachment, generation.canonical_domain_key) &&
                      generation.canonical_domain_key.length <= k_maximum_coverage_domain_key_bytes
                : generation.canonical_domain_key.offset == 0 &&
                      generation.canonical_domain_key.length == 0 &&
                      generation.coverage_directory_key.coverage_domain_digest ==
                          cache_digest_256{};
        const bool descriptor_shape_valid =
            generation.coverage_descriptor.length == 0
                ? generation.coverage_descriptor.offset == 0
                : range_valid(attachment, generation.coverage_descriptor);
        const bool descriptor_valid = is_coverage ? descriptor_shape_valid
                                                  : generation.coverage_descriptor.offset == 0 &&
                                                        generation.coverage_descriptor.length == 0;
        const std::uint64_t unaligned_length =
            static_cast<std::uint64_t>(generation.canonical_domain_key.length) +
            generation.canonical_generation_key.length + generation.coverage_descriptor.length +
            generation.dependency_vector.length;
        const std::uint64_t aligned_length = align_up(unaligned_length, 8U);
        const bool identity_start_valid =
            is_coverage
                ? generation.allocation_offset == generation.canonical_domain_key.offset
                : generation.allocation_offset == generation.canonical_generation_key.offset;
        const bool domain_contiguous =
            !is_coverage || static_cast<std::uint64_t>(generation.canonical_domain_key.offset) +
                                    generation.canonical_domain_key.length ==
                                generation.canonical_generation_key.offset;
        const std::uint64_t generation_key_end =
            static_cast<std::uint64_t>(generation.canonical_generation_key.offset) +
            generation.canonical_generation_key.length;
        const bool descriptor_contiguous =
            generation.coverage_descriptor.length == 0
                ? generation.coverage_descriptor.offset == 0
                : generation_key_end == generation.coverage_descriptor.offset;
        const bool dependency_vector_contiguous =
            generation_key_end + generation.coverage_descriptor.length ==
            generation.dependency_vector.offset;
        const bool contiguous = identity_start_valid && domain_contiguous &&
                                descriptor_contiguous && dependency_vector_contiguous;
        const std::uint64_t allocation_end =
            static_cast<std::uint64_t>(generation.allocation_offset) + generation.allocation_length;
        if (!domain_valid || !descriptor_valid || !contiguous || aligned_length == 0 ||
            generation.allocation_length != aligned_length ||
            generation.allocation_offset % 8U != 0 || generation.allocation_length % 8U != 0 ||
            generation.allocation_offset < shared.layout.byte_arena_offset ||
            allocation_end > static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
                                 shared.config.byte_arena_bytes) {
            return reusable_registry_error::corrupt_generation;
        }
        const std::uint64_t diagnostic_bytes =
            diagnostic_allocation(generation.display_sql).length +
            static_cast<std::uint64_t>(diagnostic_allocation(generation.canonical_plan).length) +
            diagnostic_allocation(generation.producer_plan).length;
        allocated_bytes += generation.allocation_length + diagnostic_bytes;
        if (allocated_bytes > shared.config.byte_arena_bytes) {
            return reusable_registry_error::corrupt_layout;
        }

        if (generation.membership == reusable_record_membership::retired) {
            ++retired_records;
            const bool pending_release =
                generation.dependency_release_state == reusable_dependency_release_state::pending &&
                generation.dependency_release_cursor <= generation_reference_count(generation) &&
                generation.dependency_release_sequence == 0;
            const bool prepared_release =
                generation.dependency_release_state ==
                    reusable_dependency_release_state::prepared &&
                generation.dependency_release_cursor <= generation_reference_count(generation) &&
                generation.dependency_release_sequence != 0;
            const bool released =
                generation.dependency_release_state ==
                    reusable_dependency_release_state::released &&
                generation.dependency_release_cursor == generation_reference_count(generation) &&
                generation.dependency_release_sequence != 0;
            if (generation.state != cache_entry_state::evicting || generation.waiter_count != 0 ||
                generation.planner_pin_count != 0 || generation.reader_pin_count != 0 ||
                (!pending_release && !prepared_release && !released) ||
                generation.next_in_exact_bucket != k_invalid_reusable_registry_slot ||
                generation.next_in_coverage_bucket != k_invalid_reusable_registry_slot) {
                return reusable_registry_error::corrupt_generation;
            }
            continue;
        }
        ++indexed_records;
        if (generation.dependency_release_state != reusable_dependency_release_state::retained ||
            generation.dependency_release_cursor != 0 ||
            generation.dependency_release_sequence != 0) {
            return reusable_registry_error::corrupt_generation;
        }
        if (is_coverage) {
            ++indexed_coverage_records;
        } else {
            ++indexed_exact_records;
            if (generation.next_in_coverage_bucket != k_invalid_reusable_registry_slot) {
                return reusable_registry_error::corrupt_generation;
            }
        }
        const bool indexed_state = generation.state == cache_entry_state::claimed ||
                                   generation.state == cache_entry_state::building ||
                                   generation.state == cache_entry_state::complete ||
                                   generation.state == cache_entry_state::failed ||
                                   generation.state == cache_entry_state::invalidated;
        if (!indexed_state ||
            (generation.state == cache_entry_state::complete &&
             (!generation.manifest || generation.manifest.id != generation.id ||
              generation.failure != cache_failure_code::none ||
              generation.completed_monotonic_ns < generation.started_monotonic_ns)) ||
            (generation.state == cache_entry_state::failed &&
             generation.failure == cache_failure_code::none) ||
            (generation.state == cache_entry_state::invalidated &&
             generation.failure == cache_failure_code::none) ||
            ((generation.state == cache_entry_state::claimed ||
              generation.state == cache_entry_state::building) &&
             (generation.failure != cache_failure_code::none ||
              static_cast<bool>(generation.manifest)))) {
            return reusable_registry_error::corrupt_generation;
        }
    }
    if (indexed_records != shared.indexed_generations ||
        indexed_exact_records + indexed_coverage_records != indexed_records ||
        retired_records != shared.retired_generations ||
        indexed_records + retired_records + free_records != shared.config.max_generations) {
        return reusable_registry_error::corrupt_generation;
    }

    std::uint32_t free_list_count = 0;
    std::uint32_t current = shared.free_generation_head;
    while (current != k_invalid_reusable_registry_slot &&
           free_list_count <= shared.config.max_generations) {
        if (current >= shared.config.max_generations ||
            records[current].membership != reusable_record_membership::free) {
            return reusable_registry_error::corrupt_generation;
        }
        ++free_list_count;
        current = records[current].next_retired_or_free;
    }
    if (current != k_invalid_reusable_registry_slot || free_list_count != free_records) {
        return reusable_registry_error::corrupt_generation;
    }
    std::uint32_t retired_list_count = 0;
    current = shared.retired_generation_head;
    while (current != k_invalid_reusable_registry_slot &&
           retired_list_count <= shared.config.max_generations) {
        if (current >= shared.config.max_generations ||
            records[current].membership != reusable_record_membership::retired) {
            return reusable_registry_error::corrupt_generation;
        }
        ++retired_list_count;
        current = records[current].next_retired_or_free;
    }
    if (current != k_invalid_reusable_registry_slot || retired_list_count != retired_records) {
        return reusable_registry_error::corrupt_generation;
    }

    const reusable_exact_bucket* exact = exact_buckets(attachment);
    std::uint32_t exact_occupied = 0;
    std::uint64_t exact_members = 0;
    for (std::uint32_t bucket_index = 0; bucket_index < shared.config.exact_directory_capacity;
         ++bucket_index) {
        const reusable_exact_bucket& bucket = exact[bucket_index];
        if (bucket.slot_state != directory_slot_state::occupied) {
            if ((bucket.slot_state != directory_slot_state::empty &&
                 bucket.slot_state != directory_slot_state::tombstone) ||
                bucket.first_generation != k_invalid_reusable_registry_slot ||
                bucket.generation_count != 0 || bucket.terminal_generation_count != 0) {
                return reusable_registry_error::corrupt_directory;
            }
            continue;
        }
        ++exact_occupied;
        if (bucket.generation_count == 0 ||
            bucket.generation_count > k_maximum_generations_per_exact_digest ||
            bucket.terminal_generation_count > bucket.generation_count) {
            return reusable_registry_error::corrupt_directory;
        }
        std::uint32_t terminal_count = 0;
        current = bucket.first_generation;
        for (std::uint32_t visited = 0; visited < bucket.generation_count; ++visited) {
            if (current == k_invalid_reusable_registry_slot ||
                current >= shared.config.max_generations ||
                records[current].membership != reusable_record_membership::indexed ||
                !known_entry_kind(records[current].entry_kind) ||
                records[current].id.digest != bucket.digest) {
                return reusable_registry_error::corrupt_directory;
            }
            terminal_count += terminal_state(records[current].state) ? 1U : 0U;
            current = records[current].next_in_exact_bucket;
        }
        if (current != k_invalid_reusable_registry_slot ||
            terminal_count != bucket.terminal_generation_count) {
            return reusable_registry_error::corrupt_directory;
        }
        exact_members += bucket.generation_count;
    }
    if (exact_occupied != shared.exact_occupied_buckets || exact_members != indexed_records) {
        return reusable_registry_error::corrupt_directory;
    }

    const reusable_coverage_bucket* coverage = coverage_buckets(attachment);
    std::uint32_t coverage_occupied = 0;
    std::uint64_t coverage_members = 0;
    for (std::uint32_t bucket_index = 0; bucket_index < shared.config.coverage_directory_capacity;
         ++bucket_index) {
        const reusable_coverage_bucket& bucket = coverage[bucket_index];
        if (bucket.slot_state != directory_slot_state::occupied) {
            if ((bucket.slot_state != directory_slot_state::empty &&
                 bucket.slot_state != directory_slot_state::tombstone) ||
                bucket.first_generation != k_invalid_reusable_registry_slot ||
                bucket.generation_count != 0 || bucket.terminal_generation_count != 0) {
                return reusable_registry_error::corrupt_directory;
            }
            continue;
        }
        ++coverage_occupied;
        if (bucket.generation_count == 0 ||
            bucket.generation_count > k_maximum_coverage_generations_per_domain ||
            bucket.terminal_generation_count > bucket.generation_count) {
            return reusable_registry_error::corrupt_directory;
        }
        std::uint32_t terminal_count = 0;
        current = bucket.first_generation;
        for (std::uint32_t visited = 0; visited < bucket.generation_count; ++visited) {
            if (current == k_invalid_reusable_registry_slot ||
                current >= shared.config.max_generations ||
                records[current].membership != reusable_record_membership::indexed ||
                records[current].entry_kind != cache_entry_kind::reusable_coverage ||
                records[current].coverage_directory_key != bucket.key) {
                return reusable_registry_error::corrupt_directory;
            }
            terminal_count += terminal_state(records[current].state) ? 1U : 0U;
            current = records[current].next_in_coverage_bucket;
        }
        if (current != k_invalid_reusable_registry_slot ||
            terminal_count != bucket.terminal_generation_count) {
            return reusable_registry_error::corrupt_directory;
        }
        coverage_members += bucket.generation_count;
    }
    if (coverage_occupied != shared.coverage_occupied_buckets ||
        coverage_members != indexed_coverage_records) {
        return reusable_registry_error::corrupt_directory;
    }

    for (std::uint32_t slot = 0; slot < shared.config.max_generations; ++slot) {
        const reusable_shared_generation& generation = records[slot];
        if (generation.membership != reusable_record_membership::indexed) {
            continue;
        }
        const directory_probe exact_probe = probe_exact_directory(attachment, generation.id.digest);
        if (!exact_probe.found) {
            return reusable_registry_error::corrupt_directory;
        }
        std::uint32_t occurrences = 0;
        current = exact[exact_probe.slot].first_generation;
        for (std::uint32_t visited = 0; visited < exact[exact_probe.slot].generation_count;
             ++visited) {
            occurrences += current == slot ? 1U : 0U;
            current = records[current].next_in_exact_bucket;
        }
        if (occurrences != 1) {
            return reusable_registry_error::corrupt_directory;
        }
        if (generation.entry_kind == cache_entry_kind::reusable_coverage) {
            const directory_probe coverage_probe =
                probe_coverage_directory(attachment, generation.coverage_directory_key);
            if (!coverage_probe.found) {
                return reusable_registry_error::corrupt_directory;
            }
            occurrences = 0;
            current = coverage[coverage_probe.slot].first_generation;
            for (std::uint32_t visited = 0;
                 visited < coverage[coverage_probe.slot].generation_count;
                 ++visited) {
                occurrences += current == slot ? 1U : 0U;
                current = records[current].next_in_coverage_bucket;
            }
            if (occurrences != 1) {
                return reusable_registry_error::corrupt_directory;
            }
        } else if (generation.entry_kind != cache_entry_kind::exact_final_result) {
            return reusable_registry_error::corrupt_generation;
        }
    }

    const reusable_free_extent* extents =
        array_at<reusable_free_extent>(attachment, shared.layout.free_extents_offset);
    std::uint64_t free_bytes = 0;
    std::uint64_t previous_end = shared.layout.byte_arena_offset;
    for (std::uint32_t extent_index = 0; extent_index < shared.free_extent_count; ++extent_index) {
        const reusable_free_extent& extent = extents[extent_index];
        const std::uint64_t extent_end = static_cast<std::uint64_t>(extent.offset) + extent.length;
        if (extent.length == 0 || extent.offset % 8U != 0 || extent.length % 8U != 0 ||
            extent.offset < shared.layout.byte_arena_offset ||
            extent_end > static_cast<std::uint64_t>(shared.layout.byte_arena_offset) +
                             shared.config.byte_arena_bytes ||
            (extent_index != 0 && extent.offset <= previous_end)) {
            return reusable_registry_error::corrupt_layout;
        }
        previous_end = extent_end;
        free_bytes += extent.length;
    }
    if (free_bytes + allocated_bytes != shared.config.byte_arena_bytes) {
        return reusable_registry_error::corrupt_layout;
    }
    return reusable_registry_error::none;
}

static_assert(std::is_standard_layout_v<reusable_registry_header>);
static_assert(std::is_standard_layout_v<reusable_shared_generation>);
static_assert(std::is_trivially_copyable_v<reusable_shared_generation>);
static_assert(std::is_trivially_copyable_v<reusable_manifest_handle>);
static_assert(std::is_trivially_copyable_v<reusable_generation_reference>);
static_assert(std::is_trivially_copyable_v<reusable_shared_reference_owner>);
static_assert(std::is_trivially_copyable_v<reusable_reference_token>);
static_assert(std::is_trivially_copyable_v<reusable_janitor_owner_observation>);
static_assert(std::is_trivially_copyable_v<reusable_invalidation_selector>);
static_assert(std::is_trivially_copyable_v<reusable_invalidation_result>);
static_assert(std::is_standard_layout_v<reusable_generation_diagnostics_row>);
static_assert(std::is_trivially_copyable_v<reusable_generation_diagnostics_row>);
static_assert(std::is_trivially_copyable_v<reusable_generation_diagnostics_result>);

} // namespace bioseq::cache
