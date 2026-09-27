/**
 * @file bioseq_caching/core/dependency_directory.cppm
 * @brief Tracks dependency versions and the references that keep them in shared memory.
 *
 * Records use offsets within the shared region. The caller supplies
 * partition locks and increasing sequence numbers so separate processes
 * can update records and detect reused slots.
 */
module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.cache.core:dependency_directory;

import :wire;

export namespace bioseq::cache {

inline constexpr std::uint32_t k_dependency_directory_magic = 0x42444431U;
inline constexpr std::uint32_t k_dependency_directory_abi_revision = 3;
inline constexpr std::uint32_t k_minimum_dependency_directory_logical_capacity = 4'096;
inline constexpr std::uint32_t k_default_dependency_directory_logical_capacity = 4'096;
inline constexpr std::uint32_t k_maximum_dependency_directory_logical_capacity = 262'144;
inline constexpr std::uint32_t k_dependency_directory_entries_per_backend = 512;
inline constexpr std::uint32_t k_maximum_dependency_directory_physical_capacity = 524'288;
inline constexpr std::uint32_t k_default_dependency_directory_partitions = 16;
inline constexpr std::uint32_t k_default_dependency_reclaim_scan = 64;
inline constexpr std::uint32_t k_maximum_dependency_directory_partitions = 1'024;
inline constexpr std::uint32_t k_maximum_dependency_vector_size = 1'024;
inline constexpr std::uint32_t k_maximum_dependency_reference_vector = 1'024;
inline constexpr std::uint32_t k_default_dependency_backend_reference_limit = 1'024;
inline constexpr std::uint32_t k_default_dependency_generation_reference_limit = 65'536;
inline constexpr std::uint32_t k_maximum_dependency_reference_count = 1U << 20U;
inline constexpr std::uint32_t k_dependency_directory_pressure_percent = 70;
inline constexpr std::uint32_t k_invalid_dependency_partition =
    (std::numeric_limits<std::uint32_t>::max)();

/** Directory errors that the adapter handles by bypassing the cache or checking a broader dependency. */
enum class dependency_directory_error : std::uint32_t {
    none = 0,
    invalid_argument,
    invalid_configuration,
    region_too_small,
    region_size_mismatch,
    region_misaligned,
    not_initialized,
    abi_mismatch,
    corrupt_layout,
    corrupt_directory,
    directory_full,
    pressure_limit,
    not_found,
    stale_reference,
    invalid_reference_kind,
    observability_mismatch,
    physical_identity_mismatch,
    counter_overflow,
    counter_underflow,
    sequence_unavailable,
    wake_failed,
    invalid_state,
    emergency_fallback,
};

/** Reference categories counted separately to decide when a dependency record can be freed. */
enum class dependency_reference_kind : std::uint8_t {
    none = 0,
    owner = 1,
    ticket = 2,
    generation = 3,
};

enum class dependency_transition_outcome : std::uint8_t {
    aborted = 0,
    committed = 1,
    /** PostgreSQL has confirmed that the aborted write never became visible to other transactions. */
    known_aborted = 2,
};

enum class dependency_physical_identity_action : std::uint8_t {
    preserve = 1,
    replace = 2,
};

struct dependency_transition_resolution {
    dependency_transition_outcome outcome = dependency_transition_outcome::aborted;
    dependency_physical_identity_action physical_identity_action =
        dependency_physical_identity_action::preserve;
    std::array<std::uint8_t, 6> reserved{};
    bioseq::cache::physical_identity physical_identity{};
};

enum class dependency_terminal_stage : std::uint8_t {
    epoch_published = 0,
    transition_decremented = 1,
    change_generation_published = 2,
    waiters_broadcast = 3,
};

enum class dependency_directory_lock_mode : std::uint8_t {
    acquire_release = 1,
    already_locked = 2,
};

enum class dependency_admission_outcome : std::uint8_t {
    none = 0,
    existing = 1,
    inserted = 2,
    pressure_fallback = 3,
};

/**
 * Directory limits fixed at server startup. logical_capacity limits
 * active records, while the hash table has at least twice as many
 * slots to leave room for probing. Reference limits bound the number
 * of backend and cached-generation references to each record.
 */
struct dependency_directory_config {
    std::uint32_t logical_capacity = k_default_dependency_directory_logical_capacity;
    std::uint32_t partition_count = k_default_dependency_directory_partitions;
    std::uint32_t reclaim_scan_limit = k_default_dependency_reclaim_scan;
    std::uint32_t pressure_percent = k_dependency_directory_pressure_percent;
    std::uint32_t backend_reference_limit = k_default_dependency_backend_reference_limit;
    std::uint32_t generation_reference_limit = k_default_dependency_generation_reference_limit;
    std::uint32_t reserved = 0;

    [[nodiscard]] bool operator==
        (const dependency_directory_config&) const noexcept = default;
};

/** Byte offsets and sizes used to allocate and attach the shared region. */
struct dependency_directory_layout {
    std::uint32_t total_bytes = 0;
    std::uint32_t partitions_offset = 0;
    std::uint32_t records_offset = 0;
    std::uint32_t logical_capacity = 0;
    std::uint32_t physical_capacity = 0;
    std::uint32_t partition_capacity = 0;
    std::uint32_t reserved = 0;

    [[nodiscard]] bool operator==
        (const dependency_directory_layout&) const noexcept = default;
};

struct dependency_directory_preload_request {
    dependency_directory_config config{};
    dependency_directory_layout layout{};
    std::uint32_t partition_lock_count = 0;
    std::uint32_t reserved = 0;
};

struct dependency_directory_preload_result {
    dependency_directory_preload_request request{};
    dependency_directory_error error = dependency_directory_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == dependency_directory_error::none;
    }
};

/** One directory partition. record_offset is measured from the start of the shared region. */
struct dependency_directory_partition {
    std::uint32_t record_offset = 0;
    std::uint32_t physical_capacity = 0;
    std::uint32_t logical_capacity = 0;
    std::uint32_t occupied_count = 0;
    std::uint32_t tombstone_count = 0;
    std::uint32_t reclaim_cursor = 0;
    std::uint32_t emergency_latched = 0;
    std::uint32_t reserved = 0;
    std::uint64_t last_sequence = 0;
    std::uint64_t emergency_change_generation = 0;
    std::uint64_t pressure_event_count = 0;
    std::uint64_t reclaim_attempt_count = 0;
    std::uint64_t reclaimed_record_count = 0;
    std::uint64_t retiring_recheck_failure_count = 0;
    std::uint64_t pressure_fallback_count = 0;
    std::uint64_t directory_full_fallback_count = 0;
    std::uint64_t emergency_fallback_count = 0;
    std::uint64_t wake_failure_count = 0;
};

/** Saves one previous record version until its outstanding references have been released. */
struct retired_dependency_reference_cohort {
    bioseq::cache::physical_identity physical_identity{};
    std::uint64_t record_generation = 0;
    std::uint64_t committed_epoch = 0;
    std::uint32_t generation_ref_count = 0;
    std::uint32_t ticket_ref_count = 0;
    write_observability observability = write_observability::none;
    std::uint32_t reserved = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return record_generation != 0;
    }

    [[nodiscard]] bool
    operator==(const retired_dependency_reference_cohort&) const noexcept = default;
};

/** Shared dependency versions, pending writes and reference counts that prevent removal. */
struct shared_dependency_epoch {
    directory_slot_state slot_state = directory_slot_state::empty;
    std::uint32_t reserved = 0;
    dependency_key key{};
    std::uint32_t alignment_reserved = 0;
    bioseq::cache::physical_identity physical_identity{};
    std::uint64_t record_generation = 0;
    std::uint64_t committed_epoch = 0;
    std::uint64_t change_generation = 0;
    std::uint32_t transition_count = 0;
    std::uint32_t generation_ref_count = 0;
    std::uint32_t ticket_ref_count = 0;
    std::uint32_t owner_ref_count = 0;
    write_observability observability = write_observability::none;
    std::uint32_t reserved2 = 0;
    retired_dependency_reference_cohort retired_references{};
};

/** Header at the start of the shared region. Each process keeps its own pointer to that region. */
struct dependency_directory_header {
    std::uint32_t magic = 0;
    std::uint32_t abi_revision = 0;
    bioseq::cache::bootstrap_state bootstrap_state = bioseq::cache::bootstrap_state::uninitialized;
    std::uint32_t reserved = 0;
    dependency_directory_config config{};
    dependency_directory_layout layout{};
};

struct dependency_directory_attachment {
    std::span<std::byte> region{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return !region.empty();
    }
};

struct dependency_epoch_snapshot;

/**
 * Process-local callbacks for locking, sequence numbers and wakeups.
 * Omit lock callbacks only in already_locked mode, when the caller
 * holds the locks or an isolated test needs no concurrent access.
 */
struct dependency_directory_callbacks {
    void* context = nullptr;
    dependency_directory_lock_mode lock_mode = dependency_directory_lock_mode::acquire_release;
    std::array<std::uint8_t, 7> reserved{};
    void (*acquire_partition)(void*, std::uint32_t) noexcept = nullptr;
    void (*release_partition)(void*, std::uint32_t) noexcept = nullptr;
    std::uint64_t (*next_sequence)(void*) noexcept = nullptr;
    bool (*wake_partition)(void*, std::uint32_t) noexcept = nullptr;
    void (*observe_terminal_stage)(void*,
                                   dependency_terminal_stage,
                                   const dependency_epoch_snapshot&) noexcept = nullptr;

    [[nodiscard]] bool locks_valid() const noexcept {
        const bool reserved_zero =
            std::all_of(reserved.begin(), reserved.end(), [](std::uint8_t value) noexcept {
                return value == 0;
            });
        if (!reserved_zero) {
            return false;
        }
        if (lock_mode == dependency_directory_lock_mode::acquire_release) {
            return acquire_partition != nullptr && release_partition != nullptr;
        }
        if (lock_mode == dependency_directory_lock_mode::already_locked) {
            return acquire_partition == nullptr && release_partition == nullptr;
        }
        return false;
    }

    [[nodiscard]] bool mutation_valid() const noexcept {
        return locks_valid() && next_sequence != nullptr;
    }

    [[nodiscard]] bool terminal_mutation_valid() const noexcept {
        return mutation_valid() && wake_partition != nullptr;
    }
};

/**
 * Identifies a process-local reference by key, offset and record
 * generation, so a reused slot cannot satisfy an old reference. Move
 * active references between owners instead of copying them.
 * transition_active detects repeated local writes, while the owner
 * ledger tracks writes shared by multiple owners.
 */
struct dependency_reference {
    dependency_key key{};
    std::uint32_t record_offset = 0;
    std::uint32_t partition_index = k_invalid_dependency_partition;
    std::uint32_t alignment_reserved = 0;
    bioseq::cache::physical_identity physical_identity{};
    std::uint64_t record_generation = 0;
    dependency_reference_kind kind = dependency_reference_kind::none;
    bool active = false;
    bool transition_active = false;
    std::array<std::uint8_t, 5> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return active && record_offset != 0 && partition_index != k_invalid_dependency_partition &&
               alignment_reserved == 0 && physical_identity_valid(physical_identity) &&
               record_generation != 0 && kind != dependency_reference_kind::none &&
               (!transition_active || kind == dependency_reference_kind::owner) &&
               std::all_of(reserved.begin(), reserved.end(), [](std::uint8_t value) noexcept {
                   return value == 0;
               });
    }

    [[nodiscard]] bool operator==(const dependency_reference&) const noexcept = default;
};

struct dependency_reference_request {
    dependency_key key{};
    bioseq::cache::physical_identity physical_identity{};
    write_observability observability = write_observability::none;
    dependency_reference_kind kind = dependency_reference_kind::none;
    std::array<std::uint8_t, 3> reserved{};
};

struct dependency_epoch_snapshot {
    dependency_key key{};
    std::uint32_t alignment_reserved = 0;
    bioseq::cache::physical_identity physical_identity{};
    std::uint64_t record_generation = 0;
    std::uint64_t committed_epoch = 0;
    std::uint64_t change_generation = 0;
    std::uint32_t transition_count = 0;
    std::uint32_t generation_ref_count = 0;
    std::uint32_t ticket_ref_count = 0;
    std::uint32_t owner_ref_count = 0;
    write_observability observability = write_observability::none;
    std::uint32_t reserved = 0;
};

struct dependency_reference_result {
    dependency_reference reference{};
    dependency_epoch_snapshot snapshot{};
    dependency_directory_error error = dependency_directory_error::none;
    std::uint32_t reclaimed_count = 0;
    bool pressure_observed = false;
    bool coarse_fallback_required = false;
    dependency_admission_outcome outcome = dependency_admission_outcome::none;
    std::uint8_t reserved = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == dependency_directory_error::none && static_cast<bool>(reference);
    }
};

struct dependency_snapshot_result {
    dependency_epoch_snapshot snapshot{};
    dependency_directory_error error = dependency_directory_error::none;
    bool coarse_fallback_required = false;
    std::array<std::uint8_t, 3> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == dependency_directory_error::none;
    }
};

struct dependency_mutation_result {
    dependency_epoch_snapshot snapshot{};
    dependency_directory_error error = dependency_directory_error::none;
    std::uint32_t reclaimed_count = 0;
    bool applied = false;
    bool diagnostic_success = false;
    bool coarse_fallback_required = false;
    std::uint8_t reserved = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == dependency_directory_error::none && applied && diagnostic_success;
    }
};

struct dependency_reclaim_result {
    dependency_directory_error error = dependency_directory_error::none;
    std::uint32_t reclaimed_count = 0;
    std::uint32_t scanned_count = 0;
    bool pressure_remains = false;
    bool coarse_fallback_required = false;
    std::array<std::uint8_t, 2> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == dependency_directory_error::none;
    }
};

struct dependency_directory_stats {
    std::uint32_t logical_capacity = 0;
    std::uint32_t physical_capacity = 0;
    std::uint32_t partition_count = 0;
    std::uint32_t occupied_count = 0;
    std::uint32_t tombstone_count = 0;
    std::uint32_t transition_count = 0;
    std::uint32_t generation_ref_count = 0;
    std::uint32_t ticket_ref_count = 0;
    std::uint32_t owner_ref_count = 0;
    std::uint64_t maximum_change_generation = 0;
    std::uint64_t emergency_change_generation = 0;
    std::uint64_t pressure_event_count = 0;
    std::uint64_t reclaim_attempt_count = 0;
    std::uint64_t reclaimed_record_count = 0;
    std::uint64_t retiring_recheck_failure_count = 0;
    std::uint64_t pressure_fallback_count = 0;
    std::uint64_t directory_full_fallback_count = 0;
    std::uint64_t emergency_fallback_count = 0;
    std::uint64_t wake_failure_count = 0;
};

struct dependency_directory_stats_result {
    dependency_directory_stats stats{};
    dependency_directory_error error = dependency_directory_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == dependency_directory_error::none;
    }
};

[[nodiscard]] inline dependency_directory_preload_result
make_dependency_directory_preload_request(dependency_directory_config config = {}) noexcept;

static_assert(std::is_trivially_copyable_v<dependency_directory_config>);
static_assert(std::is_trivially_copyable_v<dependency_directory_layout>);
static_assert(std::is_trivially_copyable_v<dependency_directory_partition>);
static_assert(std::is_trivially_copyable_v<retired_dependency_reference_cohort>);
static_assert(std::is_trivially_copyable_v<shared_dependency_epoch>);
static_assert(std::is_trivially_copyable_v<dependency_reference>);
static_assert(std::is_standard_layout_v<dependency_directory_config>);
static_assert(std::is_standard_layout_v<dependency_directory_layout>);
static_assert(std::is_standard_layout_v<dependency_directory_partition>);
static_assert(std::is_standard_layout_v<retired_dependency_reference_cohort>);
static_assert(std::is_standard_layout_v<shared_dependency_epoch>);
static_assert(std::is_standard_layout_v<dependency_directory_header>);
static_assert(alignof(dependency_directory_partition) >= alignof(std::uint64_t));

inline constexpr std::size_t k_dependency_directory_region_alignment =
    std::max({alignof(dependency_directory_header),
              alignof(dependency_directory_partition),
              alignof(shared_dependency_epoch)});
static_assert((k_dependency_directory_region_alignment &
               (k_dependency_directory_region_alignment - 1U)) == 0);

/** Chooses the dependency-record budget from MaxBackends, within the configured limits. */
[[nodiscard]] constexpr std::uint32_t
dependency_directory_logical_capacity_for_backends(std::uint32_t max_backends) noexcept {
    const std::uint64_t requested =
        static_cast<std::uint64_t>(max_backends) * k_dependency_directory_entries_per_backend;
    return static_cast<std::uint32_t>(
        std::clamp(requested,
                   static_cast<std::uint64_t>(k_minimum_dependency_directory_logical_capacity),
                   static_cast<std::uint64_t>(k_maximum_dependency_directory_logical_capacity)));
}

/** Returns a power-of-two table size with room for at least twice the record budget, or zero for an invalid budget. */
[[nodiscard]] constexpr std::uint32_t
dependency_directory_physical_capacity(std::uint32_t logical_capacity) noexcept {
    if (logical_capacity < k_minimum_dependency_directory_logical_capacity ||
        logical_capacity > k_maximum_dependency_directory_logical_capacity) {
        return 0;
    }
    const std::uint64_t minimum = static_cast<std::uint64_t>(logical_capacity) * 2U;
    std::uint64_t physical = 1;
    while (physical < minimum) {
        physical <<= 1U;
    }
    return physical <= k_maximum_dependency_directory_physical_capacity
               ? static_cast<std::uint32_t>(physical)
               : 0;
}

namespace dependency_directory_detail {

[[nodiscard]] constexpr bool power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] constexpr bool
valid_transition_outcome(dependency_transition_outcome outcome) noexcept {
    return outcome == dependency_transition_outcome::aborted ||
           outcome == dependency_transition_outcome::committed ||
           outcome == dependency_transition_outcome::known_aborted;
}

[[nodiscard]] constexpr bool
valid_transition_resolution(const dependency_transition_resolution& resolution) noexcept {
    const bool reserved_zero = std::all_of(resolution.reserved.begin(),
                                           resolution.reserved.end(),
                                           [](std::uint8_t value) noexcept { return value == 0; });
    const bool action_valid =
        resolution.physical_identity_action == dependency_physical_identity_action::preserve ||
        resolution.physical_identity_action == dependency_physical_identity_action::replace;
    return valid_transition_outcome(resolution.outcome) && action_valid && reserved_zero &&
           physical_identity_valid(resolution.physical_identity) &&
           (resolution.outcome == dependency_transition_outcome::committed ||
            resolution.physical_identity_action == dependency_physical_identity_action::preserve);
}

[[nodiscard]] constexpr std::uint32_t
logical_partition_capacity(std::uint32_t logical_capacity,
                           std::uint32_t partition_count,
                           std::uint32_t partition_index) noexcept {
    if (partition_count == 0 || partition_index >= partition_count) {
        return 0;
    }
    return logical_capacity / partition_count +
           (partition_index < logical_capacity % partition_count ? 1U : 0U);
}

inline void saturating_increment(std::uint64_t& value) noexcept {
    if (value != (std::numeric_limits<std::uint64_t>::max)()) {
        ++value;
    }
}

[[nodiscard]] inline bool checked_add_u64(std::uint64_t& destination,
                                          std::uint64_t value) noexcept {
    if (destination > (std::numeric_limits<std::uint64_t>::max)() - value) {
        return false;
    }
    destination += value;
    return true;
}

[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t value,
                                               std::uint64_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0 ||
        value > (std::numeric_limits<std::uint64_t>::max)() - (alignment - 1U)) {
        return 0;
    }
    return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] constexpr bool valid_observability(write_observability observability) noexcept {
    return observability == write_observability::provider_complete ||
           observability == write_observability::executor_utility_only ||
           observability == write_observability::database_wide_fallback;
}

[[nodiscard]] constexpr bool valid_reference_kind(dependency_reference_kind kind) noexcept {
    return kind == dependency_reference_kind::owner || kind == dependency_reference_kind::ticket ||
           kind == dependency_reference_kind::generation;
}

[[nodiscard]] constexpr bool valid_dependency_kind(dependency_kind kind) noexcept {
    return kind >= dependency_kind::relation_content &&
           kind <= dependency_kind::database_definition_epoch;
}

[[nodiscard]] constexpr bool valid_key(const dependency_key& key) noexcept {
    return key.database_oid != 0 && key.reserved == 0 && valid_dependency_kind(key.kind);
}

[[nodiscard]] inline std::uint64_t hash_byte(std::uint64_t hash, std::uint8_t value) noexcept {
    return (hash ^ value) * 1'099'511'628'211ULL;
}

[[nodiscard]] inline std::uint64_t hash_u32(std::uint64_t hash, std::uint32_t value) noexcept {
    for (std::size_t index = 0; index < sizeof(value); ++index) {
        hash = hash_byte(hash, static_cast<std::uint8_t>(value >> (index * 8U)));
    }
    return hash;
}

[[nodiscard]] inline std::uint64_t dependency_key_hash(const dependency_key& key) noexcept {
    std::uint64_t hash = 14'695'981'039'346'656'037ULL;
    hash = hash_u32(hash, key.database_oid);
    hash = hash_u32(hash, key.class_oid);
    hash = hash_u32(hash, key.object_oid);
    hash = hash_u32(hash, static_cast<std::uint32_t>(key.object_sub_id));
    hash = hash_u32(hash, static_cast<std::uint32_t>(key.kind));
    return hash_u32(hash, key.reserved);
}

template <class p_value>
[[nodiscard]] p_value* object_at(std::span<std::byte> region, std::uint32_t offset) noexcept {
    if (offset > region.size() || sizeof(p_value) > region.size() - offset) {
        return nullptr;
    }
    std::byte* address = region.data() + offset;
    if (reinterpret_cast<std::uintptr_t>(address) % alignof(p_value) != 0) {
        return nullptr;
    }
    return reinterpret_cast<p_value*>(address);
}

template <class p_value>
[[nodiscard]] const p_value* object_at(std::span<const std::byte> region,
                                       std::uint32_t offset) noexcept {
    if (offset > region.size() || sizeof(p_value) > region.size() - offset) {
        return nullptr;
    }
    const std::byte* address = region.data() + offset;
    if (reinterpret_cast<std::uintptr_t>(address) % alignof(p_value) != 0) {
        return nullptr;
    }
    return reinterpret_cast<const p_value*>(address);
}

[[nodiscard]] inline dependency_directory_header*
header(dependency_directory_attachment directory) noexcept {
    return object_at<dependency_directory_header>(directory.region, 0);
}

[[nodiscard]] inline dependency_directory_partition*
partition(dependency_directory_attachment directory, std::uint32_t partition_index) noexcept {
    dependency_directory_header* value = header(directory);
    if (value == nullptr || partition_index >= value->config.partition_count) {
        return nullptr;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(value->layout.partitions_offset) +
        static_cast<std::uint64_t>(partition_index) * sizeof(dependency_directory_partition);
    if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
        return nullptr;
    }
    return object_at<dependency_directory_partition>(directory.region,
                                                     static_cast<std::uint32_t>(offset));
}

[[nodiscard]] inline shared_dependency_epoch*
record_at_offset(dependency_directory_attachment directory, std::uint32_t offset) noexcept {
    return object_at<shared_dependency_epoch>(directory.region, offset);
}

[[nodiscard]] inline shared_dependency_epoch*
record_at_index(dependency_directory_attachment directory,
                const dependency_directory_partition& partition_value,
                std::uint32_t index) noexcept {
    if (index >= partition_value.physical_capacity) {
        return nullptr;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(partition_value.record_offset) +
        static_cast<std::uint64_t>(index) * sizeof(shared_dependency_epoch);
    if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
        return nullptr;
    }
    return record_at_offset(directory, static_cast<std::uint32_t>(offset));
}

[[nodiscard]] inline std::uint32_t
record_offset(const dependency_directory_partition& partition_value, std::uint32_t index) noexcept {
    const std::uint64_t offset =
        static_cast<std::uint64_t>(partition_value.record_offset) +
        static_cast<std::uint64_t>(index) * sizeof(shared_dependency_epoch);
    return offset <= (std::numeric_limits<std::uint32_t>::max)()
               ? static_cast<std::uint32_t>(offset)
               : 0;
}

class partition_lock_guard {
  public:
    partition_lock_guard(const dependency_directory_callbacks& callbacks,
                         std::uint32_t partition_index) noexcept
        : callbacks_(callbacks), partition_index_(partition_index) {
        if (callbacks_.lock_mode == dependency_directory_lock_mode::acquire_release) {
            callbacks_.acquire_partition(callbacks_.context, partition_index_);
        }
    }

    partition_lock_guard(const partition_lock_guard&) = delete;
    partition_lock_guard& operator=(const partition_lock_guard&) = delete;

    ~partition_lock_guard() {
        if (callbacks_.lock_mode == dependency_directory_lock_mode::acquire_release) {
            callbacks_.release_partition(callbacks_.context, partition_index_);
        }
    }

  private:
    const dependency_directory_callbacks& callbacks_;
    std::uint32_t partition_index_;
};

[[nodiscard]] inline dependency_directory_error
take_sequence(dependency_directory_partition& partition_value,
              const dependency_directory_callbacks& callbacks,
              std::uint64_t* sequence) noexcept {
    if (sequence == nullptr || callbacks.next_sequence == nullptr) {
        return dependency_directory_error::sequence_unavailable;
    }
    const std::uint64_t next = callbacks.next_sequence(callbacks.context);
    if (next == 0 || next <= partition_value.last_sequence) {
        return dependency_directory_error::sequence_unavailable;
    }
    partition_value.last_sequence = next;
    *sequence = next;
    return dependency_directory_error::none;
}

inline void note_emergency_fallback(dependency_directory_partition& partition_value) noexcept {
    saturating_increment(partition_value.emergency_fallback_count);
}

inline void latch_emergency(dependency_directory_partition& partition_value,
                            shared_dependency_epoch* record = nullptr) noexcept {
    partition_value.emergency_latched = 1;
    partition_value.emergency_change_generation = (std::numeric_limits<std::uint64_t>::max)();
    note_emergency_fallback(partition_value);
    if (record != nullptr) {
        record->change_generation = (std::numeric_limits<std::uint64_t>::max)();
    }
}

[[nodiscard]] inline bool wake_partition(dependency_directory_partition& partition_value,
                                         const dependency_directory_callbacks& callbacks,
                                         std::uint32_t partition_index) noexcept {
    if (callbacks.wake_partition != nullptr &&
        callbacks.wake_partition(callbacks.context, partition_index)) {
        return true;
    }
    saturating_increment(partition_value.wake_failure_count);
    return false;
}

/**
 * Checks that the projected table use stays below 70 percent. Valid
 * layouts also have a smaller record budget, which limits each
 * partition before this fallback check is reached.
 */
[[nodiscard]] constexpr bool
projected_below_physical_pressure(const dependency_directory_header& header_value,
                                  const dependency_directory_partition& partition_value,
                                  std::uint32_t projected_occupied) noexcept {
    return static_cast<std::uint64_t>(projected_occupied) * 100U <
           static_cast<std::uint64_t>(partition_value.physical_capacity) *
               header_value.config.pressure_percent;
}

[[nodiscard]] constexpr bool
can_admit_new_record(const dependency_directory_header& header_value,
                     const dependency_directory_partition& partition_value) noexcept {
    if (partition_value.occupied_count >= partition_value.logical_capacity ||
        partition_value.occupied_count == (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    return projected_below_physical_pressure(
        header_value, partition_value, partition_value.occupied_count + 1U);
}

[[nodiscard]] constexpr bool
under_pressure(const dependency_directory_header& header_value,
               const dependency_directory_partition& partition_value) noexcept {
    return !can_admit_new_record(header_value, partition_value);
}

[[nodiscard]] constexpr bool
zero_current_references(const shared_dependency_epoch& record) noexcept {
    return record.transition_count == 0 && record.generation_ref_count == 0 &&
           record.ticket_ref_count == 0 && record.owner_ref_count == 0;
}

[[nodiscard]] constexpr bool zero_references(const shared_dependency_epoch& record) noexcept {
    return zero_current_references(record) && !static_cast<bool>(record.retired_references);
}

inline void clear_retired_references_if_drained(shared_dependency_epoch& record) noexcept {
    retired_dependency_reference_cohort& retired = record.retired_references;
    if (retired && retired.generation_ref_count == 0 && retired.ticket_ref_count == 0) {
        retired = {};
    }
}

[[nodiscard]] inline std::uint32_t* reference_counter(shared_dependency_epoch& record,
                                                      dependency_reference_kind kind) noexcept {
    switch (kind) {
    case dependency_reference_kind::owner:
        return &record.owner_ref_count;
    case dependency_reference_kind::ticket:
        return &record.ticket_ref_count;
    case dependency_reference_kind::generation:
        return &record.generation_ref_count;
    case dependency_reference_kind::none:
        return nullptr;
    }
    return nullptr;
}

[[nodiscard]] inline std::uint32_t reference_limit(const dependency_directory_header& header_value,
                                                   dependency_reference_kind kind) noexcept {
    switch (kind) {
    case dependency_reference_kind::owner:
    case dependency_reference_kind::ticket:
        return header_value.config.backend_reference_limit;
    case dependency_reference_kind::generation:
        return header_value.config.generation_reference_limit;
    case dependency_reference_kind::none:
        return 0;
    }
    return 0;
}

[[nodiscard]] inline dependency_epoch_snapshot
snapshot_record(const shared_dependency_epoch& record) noexcept {
    return dependency_epoch_snapshot{
        .key = record.key,
        .physical_identity = record.physical_identity,
        .record_generation = record.record_generation,
        .committed_epoch = record.committed_epoch,
        .change_generation = record.change_generation,
        .transition_count = record.transition_count,
        .generation_ref_count = record.generation_ref_count,
        .ticket_ref_count = record.ticket_ref_count,
        .owner_ref_count = record.owner_ref_count,
        .observability = record.observability,
    };
}

struct probe_result {
    std::uint32_t matching_index = k_invalid_dependency_partition;
    std::uint32_t insertion_index = k_invalid_dependency_partition;
    bool corrupt = false;
};

[[nodiscard]] inline probe_result
probe_partition(dependency_directory_attachment directory,
                const dependency_directory_partition& partition_value,
                const dependency_key& key,
                std::uint64_t hash) noexcept {
    probe_result result{};
    const std::uint32_t mask = partition_value.physical_capacity - 1U;
    const std::uint32_t start =
        static_cast<std::uint32_t>((hash >> 32U) & static_cast<std::uint64_t>(mask));
    for (std::uint32_t probe = 0; probe < partition_value.physical_capacity; ++probe) {
        const std::uint32_t index = (start + probe) & mask;
        const shared_dependency_epoch* record = record_at_index(directory, partition_value, index);
        if (record == nullptr) {
            result.corrupt = true;
            return result;
        }
        if (record->slot_state == directory_slot_state::occupied) {
            if (record->key == key) {
                result.matching_index = index;
                return result;
            }
            continue;
        }
        if (record->slot_state == directory_slot_state::tombstone) {
            if (result.insertion_index == k_invalid_dependency_partition) {
                result.insertion_index = index;
            }
            continue;
        }
        if (record->slot_state == directory_slot_state::empty) {
            if (result.insertion_index == k_invalid_dependency_partition) {
                result.insertion_index = index;
            }
            return result;
        }
        result.corrupt = true;
        return result;
    }
    return result;
}

[[nodiscard]] inline dependency_reclaim_result
reclaim_locked(dependency_directory_attachment directory,
               dependency_directory_partition& partition_value,
               std::uint32_t budget,
               const dependency_directory_callbacks& callbacks) noexcept {
    dependency_reclaim_result result{};
    dependency_directory_header* header_value = header(directory);
    if (header_value == nullptr || partition_value.physical_capacity == 0) {
        result.error = dependency_directory_error::corrupt_layout;
        return result;
    }
    if (partition_value.emergency_latched != 0) {
        note_emergency_fallback(partition_value);
        result.error = dependency_directory_error::emergency_fallback;
        result.coarse_fallback_required = true;
        result.pressure_remains = under_pressure(*header_value, partition_value);
        return result;
    }
    const std::uint32_t bounded_budget = std::min(
        std::min(budget, partition_value.physical_capacity), k_maximum_dependency_vector_size);
    for (std::uint32_t scanned = 0; scanned < bounded_budget; ++scanned) {
        const std::uint32_t index =
            partition_value.reclaim_cursor & (partition_value.physical_capacity - 1U);
        partition_value.reclaim_cursor = (index + 1U) & (partition_value.physical_capacity - 1U);
        ++result.scanned_count;
        shared_dependency_epoch* record = record_at_index(directory, partition_value, index);
        if (record == nullptr) {
            result.error = dependency_directory_error::corrupt_layout;
            return result;
        }
        if (record->slot_state != directory_slot_state::occupied || !zero_references(*record)) {
            continue;
        }
        if (partition_value.occupied_count == 0 ||
            partition_value.tombstone_count >= partition_value.physical_capacity) {
            latch_emergency(partition_value, record);
            result.error = partition_value.occupied_count == 0
                               ? dependency_directory_error::counter_underflow
                               : dependency_directory_error::counter_overflow;
            result.coarse_fallback_required = true;
            return result;
        }
        saturating_increment(partition_value.reclaim_attempt_count);
        const shared_dependency_epoch before = *record;
        record->slot_state = directory_slot_state::retiring;
        std::uint64_t change_sequence = 0;
        const dependency_directory_error sequence_error =
            take_sequence(partition_value, callbacks, &change_sequence);
        if (sequence_error != dependency_directory_error::none) {
            *record = before;
            saturating_increment(partition_value.retiring_recheck_failure_count);
            note_emergency_fallback(partition_value);
            result.error = sequence_error;
            result.coarse_fallback_required = true;
            return result;
        }
        if (record->slot_state != directory_slot_state::retiring ||
            record->record_generation != before.record_generation || record->key != before.key ||
            !zero_references(*record)) {
            *record = before;
            saturating_increment(partition_value.retiring_recheck_failure_count);
            note_emergency_fallback(partition_value);
            result.error = dependency_directory_error::corrupt_directory;
            result.coarse_fallback_required = true;
            return result;
        }
        const std::uint64_t previous_record_generation = record->record_generation;
        std::memset(record, 0, sizeof(*record));
        record->slot_state = directory_slot_state::tombstone;
        record->record_generation = previous_record_generation;
        record->change_generation = change_sequence;
        --partition_value.occupied_count;
        ++partition_value.tombstone_count;
        ++result.reclaimed_count;
        saturating_increment(partition_value.reclaimed_record_count);
    }
    result.pressure_remains = under_pressure(*header_value, partition_value);
    return result;
}

[[nodiscard]] inline dependency_directory_error
validate_reference_locked(dependency_directory_attachment directory,
                          const dependency_directory_partition& partition_value,
                          const dependency_reference& reference,
                          shared_dependency_epoch** record,
                          bool require_physical_identity = true) noexcept {
    if (record == nullptr || !reference ||
        reference.partition_index == k_invalid_dependency_partition || !valid_key(reference.key) ||
        reference.alignment_reserved != 0 ||
        !physical_identity_valid(reference.physical_identity) ||
        !valid_reference_kind(reference.kind)) {
        return dependency_directory_error::invalid_argument;
    }
    const std::uint64_t partition_begin = partition_value.record_offset;
    const std::uint64_t partition_bytes =
        static_cast<std::uint64_t>(partition_value.physical_capacity) *
        sizeof(shared_dependency_epoch);
    const std::uint64_t reference_offset = reference.record_offset;
    if (reference_offset < partition_begin ||
        reference_offset >= partition_begin + partition_bytes ||
        (reference_offset - partition_begin) % sizeof(shared_dependency_epoch) != 0) {
        return dependency_directory_error::stale_reference;
    }
    shared_dependency_epoch* value = record_at_offset(directory, reference.record_offset);
    if (value == nullptr || value->slot_state != directory_slot_state::occupied ||
        value->record_generation != reference.record_generation || value->key != reference.key) {
        return dependency_directory_error::stale_reference;
    }
    if (require_physical_identity && value->physical_identity != reference.physical_identity) {
        return dependency_directory_error::physical_identity_mismatch;
    }
    *record = value;
    return dependency_directory_error::none;
}

[[nodiscard]] constexpr bool zero_dependency_key_storage(const dependency_key& key) noexcept {
    return key.database_oid == 0 && key.class_oid == 0 && key.object_oid == 0 &&
           key.object_sub_id == 0 && static_cast<std::uint16_t>(key.kind) == 0 && key.reserved == 0;
}

[[nodiscard]] inline bool empty_record_is_zero(const shared_dependency_epoch& record) noexcept {
    return record.reserved == 0 && zero_dependency_key_storage(record.key) &&
           record.alignment_reserved == 0 && record.physical_identity == physical_identity{} &&
           record.record_generation == 0 && record.committed_epoch == 0 &&
           record.change_generation == 0 && zero_references(record) &&
           record.observability == write_observability::none && record.reserved2 == 0 &&
           record.retired_references == retired_dependency_reference_cohort{};
}

[[nodiscard]] inline dependency_directory_error
validate_partition_locked(dependency_directory_attachment directory,
                          const dependency_directory_header& header_value,
                          std::uint32_t partition_index) noexcept {
    const dependency_directory_partition* partition_value = partition(directory, partition_index);
    const std::uint64_t expected_offset =
        static_cast<std::uint64_t>(header_value.layout.records_offset) +
        static_cast<std::uint64_t>(partition_index) * header_value.layout.partition_capacity *
            sizeof(shared_dependency_epoch);
    const std::uint32_t expected_logical_capacity = logical_partition_capacity(
        header_value.config.logical_capacity, header_value.config.partition_count, partition_index);
    if (partition_value == nullptr ||
        expected_offset > (std::numeric_limits<std::uint32_t>::max)() ||
        partition_value->record_offset != expected_offset ||
        partition_value->physical_capacity != header_value.layout.partition_capacity ||
        partition_value->logical_capacity != expected_logical_capacity ||
        partition_value->logical_capacity == 0 || partition_value->reserved != 0 ||
        partition_value->reclaim_cursor >= partition_value->physical_capacity ||
        partition_value->occupied_count > partition_value->logical_capacity ||
        partition_value->tombstone_count > partition_value->physical_capacity ||
        static_cast<std::uint64_t>(partition_value->occupied_count) +
                partition_value->tombstone_count >
            partition_value->physical_capacity ||
        partition_value->emergency_latched > 1 ||
        (partition_value->emergency_latched == 0 &&
         partition_value->emergency_change_generation != 0) ||
        (partition_value->emergency_latched == 1 &&
         partition_value->emergency_change_generation !=
             (std::numeric_limits<std::uint64_t>::max)()) ||
        !projected_below_physical_pressure(
            header_value, *partition_value, partition_value->logical_capacity)) {
        return dependency_directory_error::corrupt_layout;
    }

    std::uint32_t occupied = 0;
    std::uint32_t tombstones = 0;
    for (std::uint32_t index = 0; index < partition_value->physical_capacity; ++index) {
        const shared_dependency_epoch* record = record_at_index(directory, *partition_value, index);
        if (record == nullptr) {
            return dependency_directory_error::corrupt_layout;
        }
        if (record->slot_state == directory_slot_state::empty) {
            if (!empty_record_is_zero(*record)) {
                return dependency_directory_error::corrupt_directory;
            }
            continue;
        }
        if (record->slot_state == directory_slot_state::tombstone) {
            ++tombstones;
            if (record->reserved != 0 || !zero_dependency_key_storage(record->key) ||
                record->alignment_reserved != 0 ||
                record->physical_identity != physical_identity{} ||
                record->record_generation == 0 || record->committed_epoch != 0 ||
                record->change_generation == 0 || !zero_references(*record) ||
                record->observability != write_observability::none || record->reserved2 != 0 ||
                record->retired_references != retired_dependency_reference_cohort{}) {
                return dependency_directory_error::corrupt_directory;
            }
            continue;
        }
        if (record->slot_state != directory_slot_state::occupied || record->reserved != 0 ||
            !valid_key(record->key) || record->alignment_reserved != 0 ||
            !physical_identity_valid(record->physical_identity) || record->record_generation == 0 ||
            record->committed_epoch == 0 || record->change_generation == 0 ||
            !valid_observability(record->observability) || record->reserved2 != 0 ||
            record->transition_count > header_value.config.backend_reference_limit ||
            record->generation_ref_count > header_value.config.generation_reference_limit ||
            record->ticket_ref_count > header_value.config.backend_reference_limit ||
            record->owner_ref_count > header_value.config.backend_reference_limit ||
            record->retired_references.generation_ref_count >
                header_value.config.generation_reference_limit ||
            record->retired_references.ticket_ref_count >
                header_value.config.backend_reference_limit ||
            record->transition_count > record->owner_ref_count ||
            (dependency_key_hash(record->key) & (header_value.config.partition_count - 1U)) !=
                partition_index) {
            return dependency_directory_error::corrupt_directory;
        }
        const retired_dependency_reference_cohort& retired = record->retired_references;
        const bool retired_valid =
            !retired ? retired == retired_dependency_reference_cohort{}
                     : physical_identity_valid(retired.physical_identity) &&
                           retired.record_generation != record->record_generation &&
                           retired.committed_epoch != 0 &&
                           (retired.generation_ref_count != 0 || retired.ticket_ref_count != 0) &&
                           valid_observability(retired.observability) &&
                           retired.observability == record->observability && retired.reserved == 0;
        if (!retired_valid) {
            return dependency_directory_error::corrupt_directory;
        }
        ++occupied;
    }
    if (occupied != partition_value->occupied_count ||
        tombstones != partition_value->tombstone_count) {
        return dependency_directory_error::corrupt_directory;
    }
    return dependency_directory_error::none;
}

[[nodiscard]] inline dependency_directory_error
structural_validate(std::span<std::byte> region,
                    dependency_directory_header** output_header) noexcept {
    if (output_header == nullptr) {
        return dependency_directory_error::invalid_argument;
    }
    if (region.data() == nullptr ||
        reinterpret_cast<std::uintptr_t>(region.data()) % k_dependency_directory_region_alignment !=
            0) {
        return dependency_directory_error::region_misaligned;
    }
    dependency_directory_header* header_value = object_at<dependency_directory_header>(region, 0);
    if (header_value == nullptr) {
        return dependency_directory_error::region_too_small;
    }
    if (header_value->magic != k_dependency_directory_magic ||
        header_value->bootstrap_state != bootstrap_state::ready) {
        return dependency_directory_error::not_initialized;
    }
    if (header_value->abi_revision != k_dependency_directory_abi_revision) {
        return dependency_directory_error::abi_mismatch;
    }
    if (header_value->reserved != 0) {
        return dependency_directory_error::corrupt_layout;
    }
    const dependency_directory_preload_result expected =
        make_dependency_directory_preload_request(header_value->config);
    const dependency_directory_layout& layout = header_value->layout;
    if (!expected || layout != expected.request.layout) {
        return dependency_directory_error::corrupt_layout;
    }
    if (region.size() != layout.total_bytes) {
        return dependency_directory_error::region_size_mismatch;
    }
    *output_header = header_value;
    return dependency_directory_error::none;
}

} // namespace dependency_directory_detail

/** Calculates the shared-memory layout and lock count, rejecting unsupported capacities or partition sizes. */
[[nodiscard]] inline dependency_directory_preload_result
make_dependency_directory_preload_request(dependency_directory_config config) noexcept {
    using namespace dependency_directory_detail;
    dependency_directory_preload_result result{};
    const std::uint32_t physical_capacity =
        dependency_directory_physical_capacity(config.logical_capacity);
    if (physical_capacity == 0 || !power_of_two(config.partition_count) ||
        config.partition_count > config.logical_capacity ||
        config.partition_count > k_maximum_dependency_directory_partitions ||
        config.reclaim_scan_limit == 0 ||
        config.reclaim_scan_limit > k_maximum_dependency_vector_size ||
        config.pressure_percent != k_dependency_directory_pressure_percent ||
        config.backend_reference_limit == 0 ||
        config.backend_reference_limit > k_maximum_dependency_reference_count ||
        config.generation_reference_limit == 0 ||
        config.generation_reference_limit > k_maximum_dependency_reference_count ||
        config.reserved != 0) {
        result.error = dependency_directory_error::invalid_configuration;
        return result;
    }
    const std::uint32_t partition_capacity = physical_capacity / config.partition_count;
    if (!power_of_two(partition_capacity)) {
        result.error = dependency_directory_error::invalid_configuration;
        return result;
    }
    std::uint64_t cursor = sizeof(dependency_directory_header);
    cursor = align_up(cursor, alignof(dependency_directory_partition));
    const std::uint64_t partitions_offset = cursor;
    cursor +=
        static_cast<std::uint64_t>(config.partition_count) * sizeof(dependency_directory_partition);
    cursor = align_up(cursor, alignof(shared_dependency_epoch));
    const std::uint64_t records_offset = cursor;
    cursor += static_cast<std::uint64_t>(physical_capacity) * sizeof(shared_dependency_epoch);
    cursor = align_up(cursor, k_dependency_directory_region_alignment);
    if (partitions_offset == 0 || records_offset == 0 || cursor == 0 ||
        cursor > (std::numeric_limits<std::uint32_t>::max)()) {
        result.error = dependency_directory_error::invalid_configuration;
        return result;
    }
    result.request = dependency_directory_preload_request{
        .config = config,
        .layout =
            dependency_directory_layout{
                .total_bytes = static_cast<std::uint32_t>(cursor),
                .partitions_offset = static_cast<std::uint32_t>(partitions_offset),
                .records_offset = static_cast<std::uint32_t>(records_offset),
                .logical_capacity = config.logical_capacity,
                .physical_capacity = physical_capacity,
                .partition_capacity = partition_capacity,
            },
        .partition_lock_count = config.partition_count,
    };
    return result;
}

/** Clears and initializes a new shared region before any backend attaches to it. */
[[nodiscard]] inline dependency_directory_error
initialize_dependency_directory(std::span<std::byte> region,
                                const dependency_directory_preload_request& request) noexcept {
    using namespace dependency_directory_detail;
    const dependency_directory_preload_result expected =
        make_dependency_directory_preload_request(request.config);
    if (!expected || request.reserved != 0 ||
        request.partition_lock_count != request.config.partition_count ||
        request.layout != expected.request.layout) {
        return dependency_directory_error::invalid_configuration;
    }
    if (region.size() < request.layout.total_bytes) {
        return dependency_directory_error::region_too_small;
    }
    if (region.size() != request.layout.total_bytes) {
        return dependency_directory_error::region_size_mismatch;
    }
    if (region.data() == nullptr ||
        reinterpret_cast<std::uintptr_t>(region.data()) % k_dependency_directory_region_alignment !=
            0) {
        return dependency_directory_error::region_misaligned;
    }
    std::memset(region.data(), 0, request.layout.total_bytes);
    dependency_directory_header* header_value = object_at<dependency_directory_header>(region, 0);
    if (header_value == nullptr) {
        return dependency_directory_error::region_misaligned;
    }
    header_value->magic = k_dependency_directory_magic;
    header_value->abi_revision = k_dependency_directory_abi_revision;
    header_value->bootstrap_state = bootstrap_state::initializing;
    header_value->config = request.config;
    header_value->layout = request.layout;
    dependency_directory_attachment directory{.region = region};
    std::uint64_t logical_capacity_sum = 0;
    for (std::uint32_t index = 0; index < request.config.partition_count; ++index) {
        dependency_directory_partition* partition_value =
            dependency_directory_detail::partition(directory, index);
        if (partition_value == nullptr) {
            header_value->bootstrap_state = bootstrap_state::failed;
            return dependency_directory_error::corrupt_layout;
        }
        const std::uint64_t record_offset_value =
            static_cast<std::uint64_t>(request.layout.records_offset) +
            static_cast<std::uint64_t>(index) * request.layout.partition_capacity *
                sizeof(shared_dependency_epoch);
        if (record_offset_value > (std::numeric_limits<std::uint32_t>::max)()) {
            header_value->bootstrap_state = bootstrap_state::failed;
            return dependency_directory_error::corrupt_layout;
        }
        const std::uint32_t partition_logical_capacity = logical_partition_capacity(
            request.config.logical_capacity, request.config.partition_count, index);
        if (partition_logical_capacity == 0 ||
            !projected_below_physical_pressure(
                *header_value,
                dependency_directory_partition{
                    .physical_capacity = request.layout.partition_capacity,
                    .logical_capacity = partition_logical_capacity,
                },
                partition_logical_capacity)) {
            header_value->bootstrap_state = bootstrap_state::failed;
            return dependency_directory_error::invalid_configuration;
        }
        *partition_value = dependency_directory_partition{
            .record_offset = static_cast<std::uint32_t>(record_offset_value),
            .physical_capacity = request.layout.partition_capacity,
            .logical_capacity = partition_logical_capacity,
        };
        logical_capacity_sum += partition_logical_capacity;
    }
    if (logical_capacity_sum != request.config.logical_capacity) {
        header_value->bootstrap_state = bootstrap_state::failed;
        return dependency_directory_error::corrupt_layout;
    }
    header_value->bootstrap_state = bootstrap_state::ready;
    return dependency_directory_error::none;
}

struct dependency_directory_attach_result {
    dependency_directory_attachment directory{};
    dependency_directory_error error = dependency_directory_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == dependency_directory_error::none && static_cast<bool>(directory);
    }
};

/**
 * Checks the layout and every partition before attaching this process
 * to the directory. In already_locked mode the caller must hold all
 * partition locks for this call. Otherwise each partition is locked
 * while its records are checked.
 */
[[nodiscard]] inline dependency_directory_attach_result
attach_dependency_directory(std::span<std::byte> region,
                            const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_directory_attach_result result{};
    if (!callbacks.locks_valid()) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    dependency_directory_header* header_value = nullptr;
    result.error = structural_validate(region, &header_value);
    if (result.error != dependency_directory_error::none) {
        return result;
    }
    dependency_directory_attachment directory{.region = region};
    std::uint64_t logical_capacity_sum = 0;
    for (std::uint32_t index = 0; index < header_value->config.partition_count; ++index) {
        partition_lock_guard lock(callbacks, index);
        const dependency_directory_partition* partition_value =
            dependency_directory_detail::partition(directory, index);
        result.error = validate_partition_locked(directory, *header_value, index);
        if (result.error != dependency_directory_error::none) {
            return result;
        }
        logical_capacity_sum += partition_value->logical_capacity;
    }
    if (logical_capacity_sum != header_value->config.logical_capacity) {
        result.error = dependency_directory_error::corrupt_layout;
        return result;
    }
    result.directory = directory;
    return result;
}

/**
 * Acquires a reference to the matching dependency record, creating it
 * if needed. Under pressure, a bounded scan first tries to free records
 * with no references. Identity or tracking-mode mismatches require a
 * broader dependency check.
 */
[[nodiscard]] inline dependency_reference_result
acquire_dependency_reference(dependency_directory_attachment directory,
                             const dependency_reference_request& request,
                             const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_reference_result result{};
    dependency_directory_header* header_value = header(directory);
    if (header_value == nullptr || header_value->bootstrap_state != bootstrap_state::ready ||
        !callbacks.mutation_valid() || !valid_key(request.key) ||
        !physical_identity_valid(request.physical_identity) ||
        !valid_observability(request.observability) || !valid_reference_kind(request.kind) ||
        std::any_of(request.reserved.begin(),
                    request.reserved.end(),
                    [](std::uint8_t value) noexcept { return value != 0; })) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    const std::uint64_t hash = dependency_key_hash(request.key);
    const std::uint32_t partition_index =
        static_cast<std::uint32_t>(hash & (header_value->config.partition_count - 1U));
    partition_lock_guard lock(callbacks, partition_index);
    dependency_directory_partition* partition_value =
        dependency_directory_detail::partition(directory, partition_index);
    if (partition_value == nullptr) {
        result.error = dependency_directory_error::corrupt_layout;
        return result;
    }
    if (partition_value->emergency_latched != 0) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::emergency_fallback;
        result.coarse_fallback_required = true;
        result.outcome = dependency_admission_outcome::pressure_fallback;
        return result;
    }
    probe_result probe = probe_partition(directory, *partition_value, request.key, hash);
    if (probe.corrupt) {
        latch_emergency(*partition_value);
        result.error = dependency_directory_error::corrupt_directory;
        result.coarse_fallback_required = true;
        return result;
    }
    if (probe.matching_index == k_invalid_dependency_partition &&
        !can_admit_new_record(*header_value, *partition_value)) {
        result.pressure_observed = true;
        saturating_increment(partition_value->pressure_event_count);
        const dependency_reclaim_result reclaimed = reclaim_locked(
            directory, *partition_value, header_value->config.reclaim_scan_limit, callbacks);
        result.reclaimed_count = reclaimed.reclaimed_count;
        if (reclaimed.error != dependency_directory_error::none) {
            result.error = reclaimed.error;
            result.coarse_fallback_required = reclaimed.coarse_fallback_required;
            return result;
        }
        probe = probe_partition(directory, *partition_value, request.key, hash);
        if (probe.corrupt) {
            latch_emergency(*partition_value);
            result.error = dependency_directory_error::corrupt_directory;
            result.coarse_fallback_required = true;
            return result;
        }
        if (probe.matching_index == k_invalid_dependency_partition &&
            !can_admit_new_record(*header_value, *partition_value)) {
            saturating_increment(partition_value->pressure_fallback_count);
            note_emergency_fallback(*partition_value);
            result.error = dependency_directory_error::pressure_limit;
            result.coarse_fallback_required = true;
            result.outcome = dependency_admission_outcome::pressure_fallback;
            return result;
        }
    }
    shared_dependency_epoch* record = nullptr;
    std::uint32_t index = probe.matching_index;
    if (index != k_invalid_dependency_partition) {
        record = record_at_index(directory, *partition_value, index);
        if (record == nullptr) {
            result.error = dependency_directory_error::corrupt_layout;
            return result;
        }
        if (record->observability != request.observability) {
            note_emergency_fallback(*partition_value);
            result.error = dependency_directory_error::observability_mismatch;
            result.coarse_fallback_required = true;
            return result;
        }
        if (record->physical_identity != request.physical_identity) {
            note_emergency_fallback(*partition_value);
            result.error = dependency_directory_error::physical_identity_mismatch;
            result.coarse_fallback_required = true;
            return result;
        }
        result.outcome = dependency_admission_outcome::existing;
    } else {
        index = probe.insertion_index;
        if (index == k_invalid_dependency_partition) {
            saturating_increment(partition_value->directory_full_fallback_count);
            note_emergency_fallback(*partition_value);
            result.error = dependency_directory_error::directory_full;
            result.coarse_fallback_required = true;
            result.outcome = dependency_admission_outcome::pressure_fallback;
            return result;
        }
        if (!can_admit_new_record(*header_value, *partition_value)) {
            saturating_increment(partition_value->pressure_fallback_count);
            note_emergency_fallback(*partition_value);
            result.error = dependency_directory_error::pressure_limit;
            result.coarse_fallback_required = true;
            result.outcome = dependency_admission_outcome::pressure_fallback;
            return result;
        }
        record = record_at_index(directory, *partition_value, index);
        if (record == nullptr || (record->slot_state != directory_slot_state::empty &&
                                  record->slot_state != directory_slot_state::tombstone)) {
            result.error = dependency_directory_error::corrupt_directory;
            return result;
        }
        std::uint64_t record_sequence = 0;
        std::uint64_t change_sequence = 0;
        result.error = take_sequence(*partition_value, callbacks, &record_sequence);
        if (result.error == dependency_directory_error::none) {
            result.error = take_sequence(*partition_value, callbacks, &change_sequence);
        }
        if (result.error != dependency_directory_error::none) {
            note_emergency_fallback(*partition_value);
            result.coarse_fallback_required = true;
            return result;
        }
        const bool reused_tombstone = record->slot_state == directory_slot_state::tombstone;
        if (reused_tombstone && partition_value->tombstone_count == 0) {
            latch_emergency(*partition_value);
            result.error = dependency_directory_error::counter_underflow;
            result.coarse_fallback_required = true;
            return result;
        }
        *record = shared_dependency_epoch{
            .slot_state = directory_slot_state::occupied,
            .key = request.key,
            .physical_identity = request.physical_identity,
            .record_generation = record_sequence,
            .committed_epoch = 1,
            .change_generation = change_sequence,
            .observability = request.observability,
        };
        ++partition_value->occupied_count;
        if (reused_tombstone) {
            --partition_value->tombstone_count;
        }
        result.outcome = dependency_admission_outcome::inserted;
    }
    std::uint32_t* counter = reference_counter(*record, request.kind);
    if (counter == nullptr) {
        result.error = dependency_directory_error::invalid_reference_kind;
        return result;
    }
    const std::uint32_t limit = reference_limit(*header_value, request.kind);
    if (limit == 0 || *counter >= limit) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::counter_overflow;
        result.coarse_fallback_required = true;
        return result;
    }
    ++(*counter);
    result.reference = dependency_reference{
        .key = request.key,
        .record_offset = record_offset(*partition_value, index),
        .partition_index = partition_index,
        .physical_identity = record->physical_identity,
        .record_generation = record->record_generation,
        .kind = request.kind,
        .active = true,
    };
    result.snapshot = snapshot_record(*record);
    return result;
}

/** Copies a dependency's state after checking the reference still names the same record version. */
[[nodiscard]] inline dependency_snapshot_result
snapshot_dependency(dependency_directory_attachment directory,
                    const dependency_reference& reference,
                    const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_snapshot_result result{};
    dependency_directory_header* header_value = header(directory);
    if (header_value == nullptr || header_value->bootstrap_state != bootstrap_state::ready ||
        !callbacks.locks_valid() || !reference ||
        reference.partition_index >= header_value->config.partition_count) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    partition_lock_guard lock(callbacks, reference.partition_index);
    const dependency_directory_partition* partition_value =
        dependency_directory_detail::partition(directory, reference.partition_index);
    if (partition_value != nullptr && partition_value->emergency_latched != 0) {
        result.error = dependency_directory_error::emergency_fallback;
        result.coarse_fallback_required = true;
        return result;
    }
    shared_dependency_epoch* record = nullptr;
    result.error = partition_value == nullptr
                       ? dependency_directory_error::corrupt_layout
                       : validate_reference_locked(directory, *partition_value, reference, &record);
    if (result.error == dependency_directory_error::none) {
        result.snapshot = snapshot_record(*record);
    }
    return result;
}

/** Releases the reference and marks its token inactive. Under pressure, it may also free unreferenced records. */
[[nodiscard]] inline dependency_mutation_result
release_dependency_reference(dependency_directory_attachment directory,
                             dependency_reference& reference,
                             const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_mutation_result result{};
    dependency_directory_header* header_value = header(directory);
    if (header_value == nullptr || header_value->bootstrap_state != bootstrap_state::ready ||
        !callbacks.mutation_valid() || !reference ||
        reference.partition_index >= header_value->config.partition_count) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    const std::uint32_t partition_index = reference.partition_index;
    partition_lock_guard lock(callbacks, partition_index);
    dependency_directory_partition* partition_value =
        dependency_directory_detail::partition(directory, partition_index);
    shared_dependency_epoch* record = nullptr;
    bool retired_reference = false;
    result.error =
        partition_value == nullptr
            ? dependency_directory_error::corrupt_layout
            : validate_reference_locked(directory, *partition_value, reference, &record, false);
    const std::uint64_t partition_begin =
        partition_value == nullptr ? 0 : partition_value->record_offset;
    const std::uint64_t partition_bytes =
        partition_value == nullptr
            ? 0
            : static_cast<std::uint64_t>(partition_value->physical_capacity) *
                  sizeof(shared_dependency_epoch);
    const std::uint64_t reference_offset = reference.record_offset;
    const bool offset_in_locked_partition =
        partition_value != nullptr && reference_offset >= partition_begin &&
        reference_offset < partition_begin + partition_bytes &&
        (reference_offset - partition_begin) % sizeof(shared_dependency_epoch) == 0;
    if (result.error == dependency_directory_error::stale_reference && offset_in_locked_partition) {
        record = record_at_offset(directory, reference.record_offset);
        const retired_dependency_reference_cohort* retired =
            record == nullptr ? nullptr : &record->retired_references;
        if (record != nullptr && record->slot_state == directory_slot_state::occupied &&
            record->key == reference.key && retired != nullptr && *retired &&
            retired->record_generation == reference.record_generation &&
            retired->physical_identity == reference.physical_identity &&
            (reference.kind == dependency_reference_kind::ticket ||
             reference.kind == dependency_reference_kind::generation) &&
            !reference.transition_active) {
            result.error = dependency_directory_error::none;
            retired_reference = true;
        }
    }
    if (result.error != dependency_directory_error::none) {
        return result;
    }
    if (!retired_reference && record->physical_identity != reference.physical_identity) {
        result.error = dependency_directory_error::physical_identity_mismatch;
        return result;
    }
    if (reference.kind == dependency_reference_kind::owner &&
        (reference.transition_active || record->transition_count >= record->owner_ref_count)) {
        result.error = dependency_directory_error::invalid_state;
        result.coarse_fallback_required = true;
        return result;
    }
    std::uint32_t* counter = nullptr;
    if (retired_reference) {
        counter = reference.kind == dependency_reference_kind::ticket
                      ? &record->retired_references.ticket_ref_count
                      : &record->retired_references.generation_ref_count;
    } else {
        counter = reference_counter(*record, reference.kind);
    }
    if (counter == nullptr || *counter == 0) {
        latch_emergency(*partition_value, record);
        result.error = dependency_directory_error::counter_underflow;
        result.coarse_fallback_required = true;
        return result;
    }
    --(*counter);
    reference.active = false;
    clear_retired_references_if_drained(*record);
    result.applied = true;
    result.diagnostic_success = true;
    result.snapshot = snapshot_record(*record);
    if (partition_value->emergency_latched != 0) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::emergency_fallback;
        result.diagnostic_success = false;
        result.coarse_fallback_required = true;
        return result;
    }
    if (under_pressure(*header_value, *partition_value) && zero_references(*record)) {
        const dependency_reclaim_result reclaimed = reclaim_locked(
            directory, *partition_value, header_value->config.reclaim_scan_limit, callbacks);
        result.reclaimed_count = reclaimed.reclaimed_count;
        if (reclaimed.error != dependency_directory_error::none) {
            result.error = reclaimed.error;
            result.diagnostic_success = false;
            result.coarse_fallback_required = true;
        }
    }
    return result;
}

/** Releases a cached generation's dependency reference using its saved key, identity and record version. */
[[nodiscard]] inline dependency_mutation_result
release_dependency_generation_entry(dependency_directory_attachment directory,
                                    const dependency_vector_entry& entry,
                                    const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_mutation_result result{};
    dependency_directory_header* header_value = header(directory);
    if (header_value == nullptr || header_value->bootstrap_state != bootstrap_state::ready ||
        !callbacks.mutation_valid() || !dependency_vector_entry_valid(entry)) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    const std::uint64_t hash = dependency_key_hash(entry.version.key);
    const std::uint32_t partition_index =
        static_cast<std::uint32_t>(hash & (header_value->config.partition_count - 1U));
    partition_lock_guard lock(callbacks, partition_index);
    dependency_directory_partition* partition_value =
        dependency_directory_detail::partition(directory, partition_index);
    if (partition_value == nullptr) {
        result.error = dependency_directory_error::corrupt_layout;
        return result;
    }
    const probe_result probe =
        probe_partition(directory, *partition_value, entry.version.key, hash);
    if (probe.corrupt) {
        latch_emergency(*partition_value);
        result.error = dependency_directory_error::corrupt_directory;
        result.coarse_fallback_required = true;
        return result;
    }
    if (probe.matching_index == k_invalid_dependency_partition) {
        latch_emergency(*partition_value);
        result.error = dependency_directory_error::stale_reference;
        result.coarse_fallback_required = true;
        return result;
    }
    shared_dependency_epoch* record =
        record_at_index(directory, *partition_value, probe.matching_index);
    const bool current_generation =
        record != nullptr && record->record_generation == entry.record_generation;
    const bool retired_generation =
        record != nullptr && record->retired_references &&
        record->retired_references.record_generation == entry.record_generation;
    const bool current_identity = current_generation &&
                                  record->observability == entry.version.observability &&
                                  record->physical_identity == entry.version.physical_identity;
    const bool retired_identity =
        retired_generation &&
        record->retired_references.observability == entry.version.observability &&
        record->retired_references.physical_identity == entry.version.physical_identity;
    if (record == nullptr || (!current_identity && !retired_identity)) {
        latch_emergency(*partition_value, record);
        result.error = (current_generation || retired_generation)
                           ? dependency_directory_error::physical_identity_mismatch
                           : dependency_directory_error::stale_reference;
        result.coarse_fallback_required = true;
        return result;
    }
    std::uint32_t& generation_ref_count = retired_identity
                                              ? record->retired_references.generation_ref_count
                                              : record->generation_ref_count;
    if (generation_ref_count == 0) {
        latch_emergency(*partition_value, record);
        result.error = dependency_directory_error::counter_underflow;
        result.coarse_fallback_required = true;
        return result;
    }
    --generation_ref_count;
    clear_retired_references_if_drained(*record);
    result.applied = true;
    result.diagnostic_success = true;
    result.snapshot = snapshot_record(*record);
    if (partition_value->emergency_latched != 0) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::emergency_fallback;
        result.diagnostic_success = false;
        result.coarse_fallback_required = true;
        return result;
    }
    if (under_pressure(*header_value, *partition_value) && zero_references(*record)) {
        const dependency_reclaim_result reclaimed = reclaim_locked(
            directory, *partition_value, header_value->config.reclaim_scan_limit, callbacks);
        result.reclaimed_count = reclaimed.reclaimed_count;
        if (reclaimed.error != dependency_directory_error::none) {
            result.error = reclaimed.error;
            result.diagnostic_success = false;
            result.coarse_fallback_required = true;
        }
    }
    return result;
}

/** Marks an owned dependency as being written before the write can commit. */
[[nodiscard]] inline dependency_mutation_result
begin_dependency_transition(dependency_directory_attachment directory,
                            dependency_reference& owner_reference,
                            const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_mutation_result result{};
    dependency_directory_header* header_value = header(directory);
    if (header_value == nullptr || header_value->bootstrap_state != bootstrap_state::ready ||
        !callbacks.mutation_valid() || !owner_reference ||
        owner_reference.kind != dependency_reference_kind::owner ||
        owner_reference.transition_active ||
        owner_reference.partition_index >= header_value->config.partition_count) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    partition_lock_guard lock(callbacks, owner_reference.partition_index);
    dependency_directory_partition* partition_value =
        dependency_directory_detail::partition(directory, owner_reference.partition_index);
    shared_dependency_epoch* record = nullptr;
    result.error =
        partition_value == nullptr
            ? dependency_directory_error::corrupt_layout
            : validate_reference_locked(directory, *partition_value, owner_reference, &record);
    if (result.error != dependency_directory_error::none) {
        return result;
    }
    if (partition_value->emergency_latched != 0) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::emergency_fallback;
        result.coarse_fallback_required = true;
        return result;
    }
    if (record->owner_ref_count == 0) {
        latch_emergency(*partition_value, record);
        result.error = dependency_directory_error::counter_underflow;
        result.coarse_fallback_required = true;
        return result;
    }
    if (record->transition_count >= record->owner_ref_count) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::invalid_state;
        result.coarse_fallback_required = true;
        return result;
    }
    if (record->transition_count >= header_value->config.backend_reference_limit) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::counter_overflow;
        result.coarse_fallback_required = true;
        return result;
    }
    std::uint64_t sequence = 0;
    result.error = take_sequence(*partition_value, callbacks, &sequence);
    if (result.error != dependency_directory_error::none) {
        note_emergency_fallback(*partition_value);
        result.coarse_fallback_required = true;
        return result;
    }
    ++record->transition_count;
    record->change_generation = sequence;
    owner_reference.transition_active = true;
    result.applied = true;
    result.diagnostic_success = true;
    result.snapshot = snapshot_record(*record);
    return result;
}

/**
 * Finishes a tracked write and updates the dependency version before
 * waking waiters. A confirmed invisible abort leaves the committed
 * version unchanged. Other outcomes advance it so cached results are
 * rechecked.
 */
[[nodiscard]] inline dependency_mutation_result
finish_dependency_transition(dependency_directory_attachment directory,
                             dependency_reference& owner_reference,
                             const dependency_transition_resolution& resolution,
                             const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_mutation_result result{};
    dependency_directory_header* header_value = header(directory);
    if (header_value == nullptr || header_value->bootstrap_state != bootstrap_state::ready ||
        !callbacks.terminal_mutation_valid() || !owner_reference ||
        owner_reference.kind != dependency_reference_kind::owner ||
        !owner_reference.transition_active || !valid_transition_resolution(resolution) ||
        owner_reference.partition_index >= header_value->config.partition_count) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    partition_lock_guard lock(callbacks, owner_reference.partition_index);
    dependency_directory_partition* partition_value =
        dependency_directory_detail::partition(directory, owner_reference.partition_index);
    shared_dependency_epoch* record = nullptr;
    result.error =
        partition_value == nullptr
            ? dependency_directory_error::corrupt_layout
            : validate_reference_locked(directory, *partition_value, owner_reference, &record);
    if (result.error != dependency_directory_error::none) {
        return result;
    }
    if (record->transition_count == 0 || record->owner_ref_count == 0) {
        latch_emergency(*partition_value, record);
        result.error = dependency_directory_error::counter_underflow;
        result.coarse_fallback_required = true;
        return result;
    }
    if (resolution.physical_identity_action == dependency_physical_identity_action::preserve &&
        resolution.physical_identity != record->physical_identity) {
        result.error = dependency_directory_error::physical_identity_mismatch;
        result.coarse_fallback_required = true;
        return result;
    }

    /* Reserve sequence numbers before publishing the write outcome.
     * An unconfirmed abort still advances the dependency version. Only
     * known_aborted can finish without changing that version. */
    const bool publish_epoch = resolution.outcome != dependency_transition_outcome::known_aborted;
    const bool epoch_overflow =
        publish_epoch && record->committed_epoch == (std::numeric_limits<std::uint64_t>::max)();
    const bool emergency_before = partition_value->emergency_latched != 0;
    const bool replacing =
        resolution.physical_identity_action == dependency_physical_identity_action::replace;
    if (replacing && record->retired_references) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::pressure_limit;
        result.coarse_fallback_required = true;
        return result;
    }
    if (replacing && (record->owner_ref_count != 1 || record->transition_count != 1)) {
        note_emergency_fallback(*partition_value);
        result.error = dependency_directory_error::invalid_state;
        result.coarse_fallback_required = true;
        return result;
    }
    std::uint64_t record_sequence = record->record_generation;
    std::uint64_t change_sequence = 0;
    dependency_directory_error sequence_error =
        !epoch_overflow && !emergency_before ? dependency_directory_error::none
                                             : dependency_directory_error::emergency_fallback;
    if (sequence_error == dependency_directory_error::none && replacing) {
        sequence_error = take_sequence(*partition_value, callbacks, &record_sequence);
    }
    if (sequence_error == dependency_directory_error::none) {
        sequence_error = take_sequence(*partition_value, callbacks, &change_sequence);
    }
    if (replacing && sequence_error != dependency_directory_error::none) {
        note_emergency_fallback(*partition_value);
        result.error = sequence_error;
        result.coarse_fallback_required = true;
        return result;
    }

    if (replacing) {
        if (record->ticket_ref_count != 0 || record->generation_ref_count != 0) {
            record->retired_references = retired_dependency_reference_cohort{
                .physical_identity = record->physical_identity,
                .record_generation = record->record_generation,
                .committed_epoch = record->committed_epoch,
                .generation_ref_count = record->generation_ref_count,
                .ticket_ref_count = record->ticket_ref_count,
                .observability = record->observability,
            };
        }
        record->generation_ref_count = 0;
        record->ticket_ref_count = 0;
        record->physical_identity = resolution.physical_identity;
        record->record_generation = record_sequence;
        owner_reference.physical_identity = resolution.physical_identity;
        owner_reference.record_generation = record_sequence;
    }
    if (publish_epoch && !epoch_overflow) {
        ++record->committed_epoch;
    }
    if (callbacks.observe_terminal_stage != nullptr) {
        const dependency_epoch_snapshot snapshot = snapshot_record(*record);
        callbacks.observe_terminal_stage(
            callbacks.context, dependency_terminal_stage::epoch_published, snapshot);
    }

    --record->transition_count;
    owner_reference.transition_active = false;
    result.applied = true;
    result.diagnostic_success = true;
    if (callbacks.observe_terminal_stage != nullptr) {
        const dependency_epoch_snapshot snapshot = snapshot_record(*record);
        callbacks.observe_terminal_stage(
            callbacks.context, dependency_terminal_stage::transition_decremented, snapshot);
    }

    if (epoch_overflow || emergency_before || sequence_error != dependency_directory_error::none) {
        latch_emergency(*partition_value, record);
        result.error =
            epoch_overflow ? dependency_directory_error::counter_overflow : sequence_error;
        result.diagnostic_success = false;
        result.coarse_fallback_required = true;
    } else {
        record->change_generation = change_sequence;
    }
    if (callbacks.observe_terminal_stage != nullptr) {
        const dependency_epoch_snapshot snapshot = snapshot_record(*record);
        callbacks.observe_terminal_stage(
            callbacks.context, dependency_terminal_stage::change_generation_published, snapshot);
    }

    if (!wake_partition(*partition_value, callbacks, owner_reference.partition_index)) {
        latch_emergency(*partition_value, record);
        if (result.error == dependency_directory_error::none) {
            result.error = dependency_directory_error::wake_failed;
        }
        result.diagnostic_success = false;
        result.coarse_fallback_required = true;
    } else if (callbacks.observe_terminal_stage != nullptr) {
        const dependency_epoch_snapshot snapshot = snapshot_record(*record);
        callbacks.observe_terminal_stage(
            callbacks.context, dependency_terminal_stage::waiters_broadcast, snapshot);
    }
    result.snapshot = snapshot_record(*record);
    return result;
}

/** Scans up to the requested budget in one partition and frees records with no references or pending writes. */
[[nodiscard]] inline dependency_reclaim_result
reclaim_dependency_partition(dependency_directory_attachment directory,
                             std::uint32_t partition_index,
                             std::uint32_t budget,
                             const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_reclaim_result result{};
    dependency_directory_header* header_value = header(directory);
    if (header_value == nullptr || header_value->bootstrap_state != bootstrap_state::ready ||
        !callbacks.mutation_valid() || budget == 0 || budget > k_maximum_dependency_vector_size ||
        partition_index >= header_value->config.partition_count) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    partition_lock_guard lock(callbacks, partition_index);
    dependency_directory_partition* partition_value =
        dependency_directory_detail::partition(directory, partition_index);
    if (partition_value == nullptr) {
        result.error = dependency_directory_error::corrupt_layout;
        return result;
    }
    return reclaim_locked(directory, *partition_value, budget, callbacks);
}

/** Checks the layout and every partition under its lock. In already_locked mode, the caller supplies the locks. */
[[nodiscard]] inline dependency_directory_error
deep_validate_dependency_directory(dependency_directory_attachment directory,
                                   const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    if (!callbacks.locks_valid()) {
        return dependency_directory_error::invalid_argument;
    }
    dependency_directory_header* header_value = nullptr;
    dependency_directory_error error = structural_validate(directory.region, &header_value);
    if (error != dependency_directory_error::none) {
        return error;
    }
    std::uint64_t logical_capacity_sum = 0;
    for (std::uint32_t partition_index = 0; partition_index < header_value->config.partition_count;
         ++partition_index) {
        partition_lock_guard lock(callbacks, partition_index);
        const dependency_directory_partition* partition_value =
            dependency_directory_detail::partition(directory, partition_index);
        error = validate_partition_locked(directory, *header_value, partition_index);
        if (error != dependency_directory_error::none) {
            return error;
        }
        logical_capacity_sum += partition_value->logical_capacity;
    }
    return logical_capacity_sum == header_value->config.logical_capacity
               ? dependency_directory_error::none
               : dependency_directory_error::corrupt_layout;
}

/** Adds up directory statistics under partition locks and returns plain values. */
[[nodiscard]] inline dependency_directory_stats_result
read_dependency_directory_stats(dependency_directory_attachment directory,
                                const dependency_directory_callbacks& callbacks) noexcept {
    using namespace dependency_directory_detail;
    dependency_directory_stats_result result{};
    if (!callbacks.locks_valid()) {
        result.error = dependency_directory_error::invalid_argument;
        return result;
    }
    dependency_directory_header* header_value = nullptr;
    result.error = structural_validate(directory.region, &header_value);
    if (result.error != dependency_directory_error::none) {
        return result;
    }
    result.stats.logical_capacity = header_value->config.logical_capacity;
    result.stats.physical_capacity = header_value->layout.physical_capacity;
    result.stats.partition_count = header_value->config.partition_count;
    for (std::uint32_t partition_index = 0; partition_index < header_value->config.partition_count;
         ++partition_index) {
        partition_lock_guard lock(callbacks, partition_index);
        const dependency_directory_partition* partition_value =
            dependency_directory_detail::partition(directory, partition_index);
        result.error = validate_partition_locked(directory, *header_value, partition_index);
        if (result.error != dependency_directory_error::none) {
            return result;
        }
        if (result.stats.occupied_count >
                (std::numeric_limits<std::uint32_t>::max)() - partition_value->occupied_count ||
            result.stats.tombstone_count >
                (std::numeric_limits<std::uint32_t>::max)() - partition_value->tombstone_count) {
            result.error = dependency_directory_error::counter_overflow;
            return result;
        }
        result.stats.occupied_count += partition_value->occupied_count;
        result.stats.tombstone_count += partition_value->tombstone_count;
        result.stats.emergency_change_generation = std::max(
            result.stats.emergency_change_generation, partition_value->emergency_change_generation);
        if (!checked_add_u64(result.stats.pressure_event_count,
                             partition_value->pressure_event_count) ||
            !checked_add_u64(result.stats.reclaim_attempt_count,
                             partition_value->reclaim_attempt_count) ||
            !checked_add_u64(result.stats.reclaimed_record_count,
                             partition_value->reclaimed_record_count) ||
            !checked_add_u64(result.stats.retiring_recheck_failure_count,
                             partition_value->retiring_recheck_failure_count) ||
            !checked_add_u64(result.stats.pressure_fallback_count,
                             partition_value->pressure_fallback_count) ||
            !checked_add_u64(result.stats.directory_full_fallback_count,
                             partition_value->directory_full_fallback_count) ||
            !checked_add_u64(result.stats.emergency_fallback_count,
                             partition_value->emergency_fallback_count) ||
            !checked_add_u64(result.stats.wake_failure_count,
                             partition_value->wake_failure_count)) {
            result.error = dependency_directory_error::counter_overflow;
            return result;
        }
        for (std::uint32_t index = 0; index < partition_value->physical_capacity; ++index) {
            const shared_dependency_epoch* record =
                record_at_index(directory, *partition_value, index);
            if (record == nullptr) {
                result.error = dependency_directory_error::corrupt_layout;
                return result;
            }
            if (record->slot_state != directory_slot_state::occupied) {
                continue;
            }
            const std::array<std::uint32_t, 6> additions{
                record->transition_count,
                record->generation_ref_count,
                record->retired_references.generation_ref_count,
                record->ticket_ref_count,
                record->retired_references.ticket_ref_count,
                record->owner_ref_count,
            };
            std::array<std::uint32_t*, 6> destinations{
                &result.stats.transition_count,
                &result.stats.generation_ref_count,
                &result.stats.generation_ref_count,
                &result.stats.ticket_ref_count,
                &result.stats.ticket_ref_count,
                &result.stats.owner_ref_count,
            };
            for (std::size_t count_index = 0; count_index < additions.size(); ++count_index) {
                if (*destinations[count_index] >
                    (std::numeric_limits<std::uint32_t>::max)() - additions[count_index]) {
                    result.error = dependency_directory_error::counter_overflow;
                    return result;
                }
                *destinations[count_index] += additions[count_index];
            }
            result.stats.maximum_change_generation =
                std::max(result.stats.maximum_change_generation, record->change_generation);
        }
    }
    return result;
}

} // namespace bioseq::cache
