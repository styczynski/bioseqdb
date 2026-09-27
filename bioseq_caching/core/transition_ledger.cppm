/**
 * @file bioseq_caching/core/transition_ledger.cppm
 * @brief Pointer-free bounded owners for dependency transition handshakes.
 *
 * The shared region stores offsets and fixed-width values only.  PostgreSQL
 * ownership, liveness, locking, epoch mutation, and wakeup mechanics are
 * supplied by process-local callbacks.  In particular, a copied dependency
 * reference cannot resolve a transition: callbacks receive the authoritative
 * reference stored in the owner slot and mutate its linear transition flag.
 */
module;

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <type_traits>

export module bioseq.cache.core:transition_ledger;

import :wire;

export namespace bioseq::cache {

inline constexpr std::uint32_t k_transition_ledger_magic = 0x42544c31U;
inline constexpr std::uint32_t k_transition_ledger_abi_revision = 2;
inline constexpr std::uint32_t k_maximum_transition_owners = 1U << 20U;
inline constexpr std::uint32_t k_maximum_transition_partitions = 1U << 10U;
inline constexpr std::uint32_t k_maximum_transition_references =
    k_maximum_precise_dirty_dependencies_per_backend;
inline constexpr std::uint32_t k_invalid_transition_offset =
    (std::numeric_limits<std::uint32_t>::max)();

/** Every result is deterministic and safe to expose in diagnostics. */
enum class transition_ledger_error : std::uint32_t {
    none = 0,
    invalid_argument,
    invalid_configuration,
    region_too_small,
    region_misaligned,
    not_initialized,
    abi_mismatch,
    corrupt_layout,
    corrupt_ledger,
    sequence_unavailable,
    owner_busy,
    stale_owner,
    invalid_owner_state,
    invalid_dependency_reference,
    duplicate_dependency_reference,
    dependency_aba_mismatch,
    capacity_exceeded,
    subtransaction_rebuild_failed,
    partition_lock_failed,
    dependency_begin_failed,
    dependency_finish_failed,
    reference_release_failed,
    owner_alive,
    nothing_to_repair,
    bootstrap_failed,
};

enum class transition_resolution : std::uint8_t {
    aborted = 0,
    committed = 1,
    /** Resolution is proven not to have made the write visible. */
    known_aborted = 2,
};

enum class transition_reference_kind : std::uint8_t {
    none = 0,
    owner = 1,
};

enum class transition_initial_reference_role : std::uint8_t {
    emergency_only = 0,
    precise_and_fallback = 1,
};

enum class transition_repair_disposition : std::uint8_t {
    none = 0,
    cleared_dead_collecting = 1,
    owner_still_alive = 2,
    fatal_transition_owner = 3,
};

/** Full backend identity; ProcNumber alone never establishes ownership. */
struct transition_owner_identity {
    std::uint32_t proc_number = 0;
    std::int32_t pid = 0;
    std::uint64_t backend_nonce = 0;
    std::int64_t backend_started_at = 0;
    std::uint64_t full_xid = 0;

    [[nodiscard]] bool operator==(const transition_owner_identity&) const noexcept = default;
};

/** ABA-safe claim for one fixed ProcNumber slot and one top transaction. */
struct transition_owner_claim {
    transition_owner_identity owner{};
    std::uint64_t claim_sequence = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return pid_and_transaction_valid() && claim_sequence != 0;
    }

    [[nodiscard]] bool pid_and_transaction_valid() const noexcept {
        return owner.pid > 0 && owner.backend_nonce != 0 && owner.backend_started_at != 0 &&
               owner.full_xid != 0;
    }

    [[nodiscard]] bool operator==(const transition_owner_claim&) const noexcept = default;
};

/**
 * Exact dependency capability retained by an owner slot.
 *
 * `active` represents the directory owner reference. `transition_active` is
 * a linear capability: begin sets it only after incrementing the dependency,
 * finish clears it only after decrementing that exact contribution, and
 * release must refuse it while set.
 */
struct transition_dependency_reference {
    dependency_key key{};
    std::uint32_t record_offset = 0;
    std::uint32_t partition_index = 0;
    std::uint32_t reserved_alignment = 0;
    bioseq::cache::physical_identity physical_identity{};
    std::uint64_t record_generation = 0;
    transition_reference_kind kind = transition_reference_kind::none;
    bool active = false;
    bool transition_active = false;
    std::array<std::uint8_t, 5> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return active && !transition_active && record_offset != 0 && record_generation != 0 &&
               kind == transition_reference_kind::owner;
    }

    [[nodiscard]] bool
    exact_identity_equal(const transition_dependency_reference& other) const noexcept {
        return key == other.key && record_offset == other.record_offset &&
               partition_index == other.partition_index &&
               physical_identity == other.physical_identity &&
               record_generation == other.record_generation && kind == other.kind;
    }

    [[nodiscard]] bool operator==(const transition_dependency_reference&) const noexcept = default;
};

/** Fixed preload configuration; one additional binding stores the emergency. */
struct transition_ledger_config {
    std::uint32_t owner_capacity = 1;
    std::uint32_t precise_reference_capacity = k_maximum_transition_references;
    std::uint32_t dependency_partition_count = 1;
    std::uint32_t reserved = 0;
};

struct transition_ledger_layout {
    std::uint32_t total_bytes = 0;
    std::uint32_t owners_offset = 0;
    std::uint32_t references_offset = 0;
    std::uint32_t references_per_owner = 0;
};

struct transition_ledger_preload_result {
    transition_ledger_config config{};
    transition_ledger_layout layout{};
    std::uint32_t owner_lock_count = 0;
    transition_ledger_error error = transition_ledger_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == transition_ledger_error::none;
    }
};

/** One shared slot. references_offset is relative to the ledger region. */
struct shared_transition_owner_slot {
    alignas(4) std::uint32_t state = static_cast<std::uint32_t>(transition_owner_state::free);
    std::uint32_t references_offset = 0;
    std::uint32_t reference_capacity = 0;
    std::uint32_t precise_reference_count = 0;
    transition_owner_identity owner{};
    std::uint64_t claim_sequence = 0;
    std::uint64_t last_claim_sequence = 0;
    alignas(4) std::uint32_t incremented_count = 0;
    alignas(4) std::uint32_t outstanding_transition_count = 0;
    std::uint32_t transition_target_count = 0;
    std::uint32_t coarse = 0;
    std::uint32_t initial_reference_precise = 0;
    std::uint32_t diagnostic_failure_count = 0;
    transition_ledger_error last_error = transition_ledger_error::none;
    std::uint32_t repair_forbidden = 0;
    std::uint32_t collection_generation = 0;
};

/** Saturating shared counters; snapshots never expose process-local pointers. */
struct shared_transition_ledger_metrics {
    alignas(8) std::uint64_t successful_claim_count = 0;
    std::uint64_t coarse_fallback_count = 0;
    std::uint64_t bookkeeping_failure_count = 0;
    std::uint64_t begin_failure_count = 0;
    std::uint64_t finish_failure_count = 0;
    std::uint64_t diagnostic_failure_count = 0;
    std::uint64_t repaired_collecting_count = 0;
    std::uint64_t fatal_owner_count = 0;
    alignas(4) std::uint32_t current_owner_count = 0;
    std::uint32_t owner_high_water = 0;
    std::uint32_t current_transition_count = 0;
    std::uint32_t transition_high_water = 0;
};

struct transition_ledger_header {
    std::uint32_t magic = 0;
    std::uint32_t abi_revision = 0;
    alignas(4) std::uint32_t bootstrap_state_value =
        static_cast<std::uint32_t>(bootstrap_state::uninitialized);
    std::uint32_t reserved = 0;
    transition_ledger_config config{};
    transition_ledger_layout layout{};
    alignas(8) std::uint64_t next_claim_sequence = 1;
    shared_transition_ledger_metrics metrics{};
};

/** Process-local attachment; its pointer is never persisted in shared bytes. */
struct transition_ledger_attachment {
    std::span<std::byte> region{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return !region.empty();
    }
};

struct transition_ledger_attach_result {
    transition_ledger_attachment ledger{};
    transition_ledger_error error = transition_ledger_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == transition_ledger_error::none && static_cast<bool>(ledger);
    }
};

/**
 * Result of one dependency-directory callback.
 *
 * `applied` describes the correctness mutation separately from diagnostics.
 * A finish with `applied=true, diagnostic_success=false` has completed every
 * correctness mutation and must not be retried.  For finish callbacks,
 * `applied=true` specifically means that, under the dependency partition lock,
 * the callback published the conservative epoch, decremented this exact
 * owner's transition contribution, advanced change_generation, broadcast the
 * dependency condition variable, and cleared `transition_active`, in that
 * order.  Mandatory publication and wakeup are never merely diagnostics.
 */
struct transition_callback_result {
    bool applied = false;
    bool diagnostic_success = true;
    bool coarse_fallback_required = false;
    std::uint8_t reserved = 0;
    std::uint32_t error = 0;
};

/** No lock or callback address is stored in the shared region. */
struct transition_ledger_callbacks {
    void* context = nullptr;
    void (*lock_owner_slot)(void*, std::uint32_t) noexcept = nullptr;
    void (*unlock_owner_slot)(void*, std::uint32_t) noexcept = nullptr;
    bool (*lock_partitions)(void*, std::span<const std::uint32_t>) noexcept = nullptr;
    /** Receives ascending unique partitions and must release them in reverse. */
    void (*unlock_partitions_reverse)(void*, std::span<const std::uint32_t>) noexcept = nullptr;
    transition_callback_result (*begin_transition_locked)(
        void*, transition_dependency_reference*) noexcept = nullptr;
    transition_callback_result (*finish_transition_and_wake_locked)(
        void*, transition_dependency_reference*, transition_resolution) noexcept = nullptr;
    transition_callback_result (*release_owner_reference_locked)(
        void*, transition_dependency_reference*) noexcept = nullptr;
    bool (*owner_is_alive)(void*, const transition_owner_identity&) noexcept = nullptr;
    void (*mark_coarse_fallback)(void*, transition_ledger_error) noexcept = nullptr;
    void (*mark_bootstrap_failed)(void*, transition_ledger_error) noexcept = nullptr;

    [[nodiscard]] bool owner_locks_valid() const noexcept {
        return lock_owner_slot != nullptr && unlock_owner_slot != nullptr;
    }

    [[nodiscard]] bool partition_locks_valid() const noexcept {
        return lock_partitions != nullptr && unlock_partitions_reverse != nullptr;
    }

    [[nodiscard]] bool transition_mutations_valid() const noexcept {
        return owner_locks_valid() && partition_locks_valid() &&
               begin_transition_locked != nullptr && finish_transition_and_wake_locked != nullptr &&
               release_owner_reference_locked != nullptr;
    }
};

struct transition_ledger_operation_result {
    transition_owner_claim claim{};
    transition_ledger_error error = transition_ledger_error::none;
    transition_owner_state state = transition_owner_state::free;
    std::uint32_t precise_reference_count = 0;
    std::uint32_t incremented_count = 0;
    std::uint32_t outstanding_transition_count = 0;
    std::uint32_t diagnostic_failure_count = 0;
    bool coarse_fallback_required = false;
    bool bootstrap_failed = false;
    bool reference_consumed = false;
    std::uint8_t reserved = 0;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == transition_ledger_error::none;
    }
};

struct transition_repair_result {
    transition_owner_claim observed_claim{};
    transition_ledger_error error = transition_ledger_error::none;
    transition_repair_disposition disposition = transition_repair_disposition::none;
    bool bootstrap_failed = false;
    std::array<std::uint8_t, 2> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == transition_ledger_error::none;
    }
};

/** Bounded, text-free public diagnostic row. */
struct transition_owner_diagnostic {
    std::uint32_t slot_index = 0;
    transition_owner_state state = transition_owner_state::free;
    transition_owner_identity owner{};
    std::uint64_t claim_sequence = 0;
    std::uint32_t precise_reference_count = 0;
    std::uint32_t incremented_count = 0;
    std::uint32_t outstanding_transition_count = 0;
    std::uint32_t retained_owner_reference_count = 0;
    std::uint32_t coarse = 0;
    std::uint32_t diagnostic_failure_count = 0;
    transition_ledger_error last_error = transition_ledger_error::none;
    std::uint32_t repair_forbidden = 0;
};

struct transition_owner_diagnostics_result {
    transition_ledger_error error = transition_ledger_error::none;
    std::uint32_t copied_count = 0;
    std::uint32_t total_non_free = 0;
    bool truncated = false;
    std::array<std::uint8_t, 3> reserved{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == transition_ledger_error::none;
    }
};

struct transition_ledger_metrics_result {
    shared_transition_ledger_metrics metrics{};
    transition_ledger_error error = transition_ledger_error::none;

    [[nodiscard]] explicit operator bool() const noexcept {
        return error == transition_ledger_error::none;
    }
};

static_assert(std::is_trivially_copyable_v<transition_owner_identity>);
static_assert(std::is_trivially_copyable_v<transition_owner_claim>);
static_assert(std::is_trivially_copyable_v<transition_dependency_reference>);
static_assert(std::is_trivially_copyable_v<shared_transition_owner_slot>);
static_assert(std::atomic_ref<std::uint32_t>::is_always_lock_free);
static_assert(std::atomic_ref<std::uint64_t>::is_always_lock_free);

namespace transition_ledger_detail {

[[nodiscard]] constexpr bool power_of_two(std::uint32_t value) noexcept {
    return value != 0 && (value & (value - 1U)) == 0;
}

[[nodiscard]] constexpr std::uint64_t align_up(std::uint64_t value,
                                               std::uint64_t alignment) noexcept {
    if (alignment == 0 || (alignment & (alignment - 1U)) != 0 ||
        value > (std::numeric_limits<std::uint64_t>::max)() - (alignment - 1U)) {
        return 0;
    }
    return (value + alignment - 1U) & ~(alignment - 1U);
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

[[nodiscard]] inline transition_ledger_header*
header(transition_ledger_attachment ledger) noexcept {
    return object_at<transition_ledger_header>(ledger.region, 0);
}

[[nodiscard]] inline const transition_ledger_header*
header(std::span<const std::byte> region) noexcept {
    return object_at<transition_ledger_header>(region, 0);
}

[[nodiscard]] inline bootstrap_state
load_bootstrap_state(transition_ledger_header& header_value) noexcept {
    const std::uint32_t encoded_state =
        std::atomic_ref<std::uint32_t>(header_value.bootstrap_state_value)
            .load(std::memory_order_acquire);
    return static_cast<bootstrap_state>(encoded_state);
}

inline void store_bootstrap_state(transition_ledger_header& header_value,
                                  bootstrap_state state) noexcept {
    std::atomic_ref<std::uint32_t>(header_value.bootstrap_state_value)
        .store(static_cast<std::uint32_t>(state), std::memory_order_release);
}

[[nodiscard]] inline shared_transition_owner_slot* owner_slot(transition_ledger_attachment ledger,
                                                              std::uint32_t slot_index) noexcept {
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || slot_index >= header_value->config.owner_capacity) {
        return nullptr;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(header_value->layout.owners_offset) +
        static_cast<std::uint64_t>(slot_index) * sizeof(shared_transition_owner_slot);
    if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
        return nullptr;
    }
    return object_at<shared_transition_owner_slot>(ledger.region,
                                                   static_cast<std::uint32_t>(offset));
}

[[nodiscard]] inline transition_dependency_reference*
reference_at(transition_ledger_attachment ledger,
             const shared_transition_owner_slot& slot,
             std::uint32_t reference_index) noexcept {
    if (reference_index >= slot.reference_capacity) {
        return nullptr;
    }
    const std::uint64_t offset =
        static_cast<std::uint64_t>(slot.references_offset) +
        static_cast<std::uint64_t>(reference_index) * sizeof(transition_dependency_reference);
    if (offset > (std::numeric_limits<std::uint32_t>::max)()) {
        return nullptr;
    }
    return object_at<transition_dependency_reference>(ledger.region,
                                                      static_cast<std::uint32_t>(offset));
}

[[nodiscard]] inline transition_owner_state
load_state(shared_transition_owner_slot& slot) noexcept {
    return static_cast<transition_owner_state>(
        std::atomic_ref<std::uint32_t>(slot.state).load(std::memory_order_acquire));
}

inline void store_state(shared_transition_owner_slot& slot, transition_owner_state state) noexcept {
    std::atomic_ref<std::uint32_t>(slot.state)
        .store(static_cast<std::uint32_t>(state), std::memory_order_release);
}

[[nodiscard]] inline bool compare_exchange_state(shared_transition_owner_slot& slot,
                                                 transition_owner_state expected,
                                                 transition_owner_state desired) noexcept {
    std::uint32_t expected_value = static_cast<std::uint32_t>(expected);
    return std::atomic_ref<std::uint32_t>(slot.state)
        .compare_exchange_strong(expected_value,
                                 static_cast<std::uint32_t>(desired),
                                 std::memory_order_acq_rel,
                                 std::memory_order_acquire);
}

[[nodiscard]] inline std::uint32_t load_incremented(shared_transition_owner_slot& slot) noexcept {
    return std::atomic_ref<std::uint32_t>(slot.incremented_count).load(std::memory_order_acquire);
}

inline void store_incremented(shared_transition_owner_slot& slot, std::uint32_t value) noexcept {
    std::atomic_ref<std::uint32_t>(slot.incremented_count).store(value, std::memory_order_release);
}

[[nodiscard]] inline std::uint32_t load_outstanding(shared_transition_owner_slot& slot) noexcept {
    return std::atomic_ref<std::uint32_t>(slot.outstanding_transition_count)
        .load(std::memory_order_acquire);
}

inline void store_outstanding(shared_transition_owner_slot& slot, std::uint32_t value) noexcept {
    std::atomic_ref<std::uint32_t>(slot.outstanding_transition_count)
        .store(value, std::memory_order_release);
}

[[nodiscard]] constexpr bool valid_owner_state(transition_owner_state state) noexcept {
    return state == transition_owner_state::free || state == transition_owner_state::collecting ||
           state == transition_owner_state::transitioning ||
           state == transition_owner_state::active || state == transition_owner_state::repairing;
}

[[nodiscard]] inline bool valid_owner(const transition_owner_claim& claim,
                                      std::uint32_t owner_capacity) noexcept {
    return static_cast<bool>(claim) && claim.owner.proc_number < owner_capacity;
}

[[nodiscard]] inline bool valid_owner_identity(const transition_owner_identity& owner,
                                               std::uint32_t owner_capacity) noexcept {
    return owner.proc_number < owner_capacity && owner.pid > 0 && owner.backend_nonce != 0 &&
           owner.backend_started_at != 0 && owner.full_xid != 0;
}

[[nodiscard]] inline bool valid_reference_identity(const transition_dependency_reference& reference,
                                                   std::uint32_t partition_count) noexcept {
    if (!reference.active || reference.record_offset == 0 || reference.record_generation == 0 ||
        reference.kind != transition_reference_kind::owner ||
        reference.partition_index >= partition_count || reference.reserved_alignment != 0 ||
        !physical_identity_valid(reference.physical_identity) || reference.key.database_oid == 0 ||
        reference.key.kind < dependency_kind::relation_content ||
        reference.key.kind > dependency_kind::database_definition_epoch ||
        reference.key.reserved != 0) {
        return false;
    }
    for (const std::uint8_t value : reference.reserved) {
        if (value != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] inline bool valid_reference(const transition_dependency_reference& reference,
                                          std::uint32_t partition_count) noexcept {
    return valid_reference_identity(reference, partition_count) && !reference.transition_active;
}

[[nodiscard]] inline bool claim_matches(const shared_transition_owner_slot& slot,
                                        const transition_owner_claim& claim) noexcept {
    return slot.owner == claim.owner && slot.claim_sequence == claim.claim_sequence;
}

inline void saturating_increment(std::uint64_t& value) noexcept {
    std::atomic_ref<std::uint64_t> atomic_value(value);
    std::uint64_t observed = atomic_value.load(std::memory_order_relaxed);
    while (observed != (std::numeric_limits<std::uint64_t>::max)() &&
           !atomic_value.compare_exchange_weak(
               observed, observed + 1U, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

inline void saturating_add(std::uint64_t& value, std::uint32_t amount) noexcept {
    std::atomic_ref<std::uint64_t> atomic_value(value);
    std::uint64_t observed = atomic_value.load(std::memory_order_relaxed);
    for (;;) {
        const std::uint64_t maximum = (std::numeric_limits<std::uint64_t>::max)();
        const std::uint64_t desired = amount > maximum - observed ? maximum : observed + amount;
        if (desired == observed ||
            atomic_value.compare_exchange_weak(
                observed, desired, std::memory_order_relaxed, std::memory_order_relaxed)) {
            return;
        }
    }
}

[[nodiscard]] inline std::uint32_t increment_current(std::uint32_t& current,
                                                     std::uint32_t& high_water) noexcept {
    std::atomic_ref<std::uint32_t> atomic_current(current);
    std::uint32_t observed = atomic_current.load(std::memory_order_relaxed);
    while (observed != (std::numeric_limits<std::uint32_t>::max)() &&
           !atomic_current.compare_exchange_weak(
               observed, observed + 1U, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    const std::uint32_t value =
        observed == (std::numeric_limits<std::uint32_t>::max)() ? observed : observed + 1U;
    std::atomic_ref<std::uint32_t> atomic_high(high_water);
    std::uint32_t previous_high = atomic_high.load(std::memory_order_relaxed);
    while (previous_high < value &&
           !atomic_high.compare_exchange_weak(
               previous_high, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    return value;
}

inline void decrement_current(std::uint32_t& current) noexcept {
    std::atomic_ref<std::uint32_t> atomic_current(current);
    std::uint32_t observed = atomic_current.load(std::memory_order_relaxed);
    while (observed != 0 &&
           !atomic_current.compare_exchange_weak(
               observed, observed - 1U, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

[[nodiscard]] inline std::uint64_t
allocate_claim_sequence(transition_ledger_header& header_value) noexcept {
    std::atomic_ref<std::uint64_t> next(header_value.next_claim_sequence);
    std::uint64_t observed = next.load(std::memory_order_acquire);
    while (observed != 0 && observed != (std::numeric_limits<std::uint64_t>::max)()) {
        if (next.compare_exchange_weak(
                observed, observed + 1U, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return observed;
        }
    }
    return 0;
}

class owner_lock_guard {
  public:
    owner_lock_guard(const transition_ledger_callbacks& callbacks,
                     std::uint32_t slot_index) noexcept
        : callbacks_(callbacks), slot_index_(slot_index) {
        callbacks_.lock_owner_slot(callbacks_.context, slot_index_);
    }

    owner_lock_guard(const owner_lock_guard&) = delete;
    owner_lock_guard& operator=(const owner_lock_guard&) = delete;

    ~owner_lock_guard() {
        callbacks_.unlock_owner_slot(callbacks_.context, slot_index_);
    }

  private:
    const transition_ledger_callbacks& callbacks_;
    std::uint32_t slot_index_;
};

[[nodiscard]] inline std::uint32_t sort_and_deduplicate(std::span<std::uint32_t> values,
                                                        std::uint32_t count) noexcept {
    std::sort(values.begin(), values.begin() + count);
    std::uint32_t unique_count = 0;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (unique_count == 0 || values[index] != values[unique_count - 1U]) {
            values[unique_count++] = values[index];
        }
    }
    return unique_count;
}

[[nodiscard]] inline transition_ledger_operation_result
snapshot_result(shared_transition_owner_slot& slot,
                transition_ledger_error error = transition_ledger_error::none) noexcept {
    return transition_ledger_operation_result{
        .claim =
            transition_owner_claim{
                .owner = slot.owner,
                .claim_sequence = slot.claim_sequence,
            },
        .error = error,
        .state = load_state(slot),
        .precise_reference_count = slot.precise_reference_count + slot.initial_reference_precise,
        .incremented_count = load_incremented(slot),
        .outstanding_transition_count = load_outstanding(slot),
        .diagnostic_failure_count = slot.diagnostic_failure_count,
        .coarse_fallback_required = slot.coarse != 0,
        .bootstrap_failed = error == transition_ledger_error::bootstrap_failed,
    };
}

inline void note_diagnostic_failure(transition_ledger_header& header_value,
                                    shared_transition_owner_slot& slot,
                                    transition_ledger_error error) noexcept {
    if (slot.diagnostic_failure_count != (std::numeric_limits<std::uint32_t>::max)()) {
        ++slot.diagnostic_failure_count;
    }
    slot.last_error = error;
    saturating_increment(header_value.metrics.diagnostic_failure_count);
    saturating_increment(header_value.metrics.coarse_fallback_count);
    slot.coarse = 1;
}

inline void note_bookkeeping_failure(transition_ledger_header& header_value,
                                     shared_transition_owner_slot& slot,
                                     transition_ledger_error error) noexcept {
    saturating_increment(header_value.metrics.bookkeeping_failure_count);
    saturating_increment(header_value.metrics.coarse_fallback_count);
    slot.coarse = 1;
    slot.last_error = error;
}

inline void note_coarse_fallback(transition_ledger_header& header_value,
                                 shared_transition_owner_slot& slot,
                                 transition_ledger_error error) noexcept {
    saturating_increment(header_value.metrics.coarse_fallback_count);
    slot.coarse = 1;
    slot.last_error = error;
}

inline void add_diagnostic_failures(shared_transition_owner_slot& slot,
                                    std::uint32_t count) noexcept {
    const std::uint32_t maximum = (std::numeric_limits<std::uint32_t>::max)();
    slot.diagnostic_failure_count = count > maximum - slot.diagnostic_failure_count
                                        ? maximum
                                        : slot.diagnostic_failure_count + count;
}

inline void zero_reference(transition_dependency_reference& reference) noexcept {
    std::memset(&reference, 0, sizeof(reference));
}

[[nodiscard]] inline bool
reference_is_zero(const transition_dependency_reference& reference) noexcept {
    const auto* bytes = reinterpret_cast<const std::byte*>(&reference);
    return std::all_of(bytes, bytes + sizeof(reference), [](std::byte value) noexcept {
        return value == std::byte{};
    });
}

inline void clear_owner_slot(transition_ledger_attachment ledger,
                             transition_ledger_header& header_value,
                             shared_transition_owner_slot& slot) noexcept {
    const std::uint32_t references_offset = slot.references_offset;
    const std::uint32_t reference_capacity = slot.reference_capacity;
    const std::uint64_t last_claim_sequence = slot.last_claim_sequence;
    for (std::uint32_t index = 0; index < reference_capacity; ++index) {
        transition_dependency_reference* reference = reference_at(ledger, slot, index);
        if (reference != nullptr) {
            zero_reference(*reference);
        }
    }
    slot = shared_transition_owner_slot{
        .references_offset = references_offset,
        .reference_capacity = reference_capacity,
        .last_claim_sequence = last_claim_sequence,
    };
    decrement_current(header_value.metrics.current_owner_count);
    store_state(slot, transition_owner_state::free);
}

[[nodiscard]] inline std::uint32_t collect_partitions(transition_ledger_attachment ledger,
                                                      shared_transition_owner_slot& slot,
                                                      std::span<std::uint32_t> partitions,
                                                      bool transition_only) noexcept {
    std::uint32_t count = 0;
    for (std::uint32_t index = 0; index < slot.reference_capacity; ++index) {
        transition_dependency_reference* reference = reference_at(ledger, slot, index);
        if (reference == nullptr) {
            return k_invalid_transition_offset;
        }
        const bool selected = transition_only ? reference->transition_active
                                              : (reference->active || reference->transition_active);
        if (!selected) {
            continue;
        }
        if (count >= partitions.size()) {
            return k_invalid_transition_offset;
        }
        partitions[count++] = reference->partition_index;
    }
    return sort_and_deduplicate(partitions, count);
}

struct drain_result {
    transition_ledger_error error = transition_ledger_error::none;
    std::uint32_t outstanding_transitions = 0;
    std::uint32_t retained_owner_references = 0;
    std::uint32_t diagnostic_failures = 0;
    bool coarse_fallback_required = false;
    bool bootstrap_failed = false;
    bool contract_failure = false;
};

/** Drain every binding; diagnostic failure never short-circuits cleanup. */
[[nodiscard]] inline drain_result
drain_locked(transition_ledger_attachment ledger,
             transition_ledger_header& header_value,
             shared_transition_owner_slot& slot,
             transition_resolution resolution,
             bool release_owner_references,
             const transition_ledger_callbacks& callbacks) noexcept {
    drain_result result{};
    for (std::uint32_t index = 0; index < slot.reference_capacity; ++index) {
        transition_dependency_reference* reference = reference_at(ledger, slot, index);
        if (reference == nullptr) {
            result.error = transition_ledger_error::corrupt_layout;
            result.bootstrap_failed = true;
            return result;
        }
        if (!reference->transition_active) {
            continue;
        }
        const transition_callback_result mutation =
            callbacks.finish_transition_and_wake_locked(callbacks.context, reference, resolution);
        const bool contract_valid =
            mutation.applied ? !reference->transition_active : reference->transition_active;
        if (mutation.applied) {
            reference->transition_active = false;
            decrement_current(header_value.metrics.current_transition_count);
        }
        if (!contract_valid) {
            result.error = transition_ledger_error::dependency_finish_failed;
            result.bootstrap_failed = true;
            result.contract_failure = true;
        } else if (!mutation.applied) {
            result.error = transition_ledger_error::dependency_finish_failed;
            result.bootstrap_failed = true;
        }
        if (!mutation.diagnostic_success) {
            ++result.diagnostic_failures;
            result.coarse_fallback_required = true;
            if (result.error == transition_ledger_error::none) {
                result.error = transition_ledger_error::dependency_finish_failed;
            }
        }
        result.coarse_fallback_required =
            result.coarse_fallback_required || mutation.coarse_fallback_required;
    }

    if (release_owner_references) {
        for (std::uint32_t index = 0; index < slot.reference_capacity; ++index) {
            transition_dependency_reference* reference = reference_at(ledger, slot, index);
            if (reference == nullptr) {
                result.error = transition_ledger_error::corrupt_layout;
                result.bootstrap_failed = true;
                return result;
            }
            if (!reference->active || reference->transition_active) {
                continue;
            }
            const transition_callback_result mutation =
                callbacks.release_owner_reference_locked(callbacks.context, reference);
            const bool contract_valid = mutation.applied ? !reference->active : reference->active;
            if (mutation.applied) {
                reference->active = false;
            }
            if (!contract_valid || !mutation.applied) {
                result.error = transition_ledger_error::reference_release_failed;
                result.bootstrap_failed = true;
                result.contract_failure = result.contract_failure || !contract_valid;
            }
            if (!mutation.diagnostic_success) {
                ++result.diagnostic_failures;
                result.coarse_fallback_required = true;
                if (result.error == transition_ledger_error::none) {
                    result.error = transition_ledger_error::reference_release_failed;
                }
            }
            result.coarse_fallback_required =
                result.coarse_fallback_required || mutation.coarse_fallback_required;
        }
    }

    for (std::uint32_t index = 0; index < slot.reference_capacity; ++index) {
        transition_dependency_reference* reference = reference_at(ledger, slot, index);
        if (reference == nullptr) {
            result.error = transition_ledger_error::corrupt_layout;
            result.bootstrap_failed = true;
            return result;
        }
        result.outstanding_transitions += reference->transition_active ? 1U : 0U;
        result.retained_owner_references += reference->active ? 1U : 0U;
    }
    store_outstanding(slot, result.outstanding_transitions);
    return result;
}

[[nodiscard]] inline transition_ledger_error
structural_validate(std::span<std::byte> region,
                    transition_ledger_header** output_header) noexcept {
    if (output_header == nullptr || region.data() == nullptr) {
        return transition_ledger_error::invalid_argument;
    }
    if (reinterpret_cast<std::uintptr_t>(region.data()) % alignof(transition_ledger_header) != 0) {
        return transition_ledger_error::region_misaligned;
    }
    transition_ledger_header* header_value = object_at<transition_ledger_header>(region, 0);
    if (header_value == nullptr) {
        return transition_ledger_error::region_too_small;
    }
    if (header_value->magic != k_transition_ledger_magic ||
        load_bootstrap_state(*header_value) != bootstrap_state::ready) {
        return transition_ledger_error::not_initialized;
    }
    if (header_value->abi_revision != k_transition_ledger_abi_revision) {
        return transition_ledger_error::abi_mismatch;
    }
    const transition_ledger_config& config = header_value->config;
    const transition_ledger_layout& layout = header_value->layout;
    if (config.owner_capacity == 0 || config.owner_capacity > k_maximum_transition_owners ||
        config.precise_reference_capacity == 0 ||
        config.precise_reference_capacity > k_maximum_transition_references ||
        !power_of_two(config.dependency_partition_count) ||
        config.dependency_partition_count > k_maximum_transition_partitions ||
        config.reserved != 0 || layout.total_bytes > region.size() || layout.owners_offset == 0 ||
        layout.references_offset <= layout.owners_offset ||
        layout.references_per_owner != config.precise_reference_capacity + 1U ||
        std::atomic_ref<std::uint64_t>(header_value->next_claim_sequence)
                .load(std::memory_order_acquire) == 0 ||
        std::atomic_ref<std::uint32_t>(header_value->metrics.current_owner_count)
                .load(std::memory_order_acquire) > config.owner_capacity ||
        static_cast<std::uint64_t>(
            std::atomic_ref<std::uint32_t>(header_value->metrics.current_transition_count)
                .load(std::memory_order_acquire)) >
            static_cast<std::uint64_t>(config.owner_capacity) * layout.references_per_owner) {
        return transition_ledger_error::corrupt_layout;
    }
    *output_header = header_value;
    return transition_ledger_error::none;
}

} // namespace transition_ledger_detail

/** Compute exact fixed shared bytes; no runtime allocation follows preload. */
[[nodiscard]] inline transition_ledger_preload_result
transition_ledger_preload_request(transition_ledger_config config = {}) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_preload_result result{.config = config};
    if (config.owner_capacity == 0 || config.owner_capacity > k_maximum_transition_owners ||
        config.precise_reference_capacity == 0 ||
        config.precise_reference_capacity > k_maximum_transition_references ||
        !power_of_two(config.dependency_partition_count) ||
        config.dependency_partition_count > k_maximum_transition_partitions ||
        config.reserved != 0) {
        result.error = transition_ledger_error::invalid_configuration;
        return result;
    }
    const std::uint64_t owners_offset =
        align_up(sizeof(transition_ledger_header), alignof(shared_transition_owner_slot));
    const std::uint64_t owners_bytes =
        static_cast<std::uint64_t>(config.owner_capacity) * sizeof(shared_transition_owner_slot);
    const std::uint64_t references_offset =
        align_up(owners_offset + owners_bytes, alignof(transition_dependency_reference));
    const std::uint64_t references_per_owner =
        static_cast<std::uint64_t>(config.precise_reference_capacity) + 1U;
    const std::uint64_t references_bytes = static_cast<std::uint64_t>(config.owner_capacity) *
                                           references_per_owner *
                                           sizeof(transition_dependency_reference);
    const std::uint64_t total_bytes = references_offset + references_bytes;
    if (owners_offset == 0 || references_offset == 0 ||
        total_bytes > (std::numeric_limits<std::uint32_t>::max)()) {
        result.error = transition_ledger_error::invalid_configuration;
        return result;
    }
    result.layout = transition_ledger_layout{
        .total_bytes = static_cast<std::uint32_t>(total_bytes),
        .owners_offset = static_cast<std::uint32_t>(owners_offset),
        .references_offset = static_cast<std::uint32_t>(references_offset),
        .references_per_owner = static_cast<std::uint32_t>(references_per_owner),
    };
    result.owner_lock_count = config.owner_capacity;
    return result;
}

/** Initialize caller-owned bytes and publish Ready only after all offsets exist. */
[[nodiscard]] inline transition_ledger_error
initialize_transition_ledger(std::span<std::byte> region,
                             const transition_ledger_preload_result& request) noexcept {
    using namespace transition_ledger_detail;
    if (!request || region.data() == nullptr) {
        return transition_ledger_error::invalid_argument;
    }
    if (reinterpret_cast<std::uintptr_t>(region.data()) % alignof(transition_ledger_header) != 0) {
        return transition_ledger_error::region_misaligned;
    }
    if (region.size() < request.layout.total_bytes) {
        return transition_ledger_error::region_too_small;
    }
    std::memset(region.data(), 0, request.layout.total_bytes);
    transition_ledger_header* header_value = object_at<transition_ledger_header>(region, 0);
    if (header_value == nullptr) {
        return transition_ledger_error::corrupt_layout;
    }
    *header_value = transition_ledger_header{
        .magic = k_transition_ledger_magic,
        .abi_revision = k_transition_ledger_abi_revision,
        .bootstrap_state_value = static_cast<std::uint32_t>(bootstrap_state::initializing),
        .config = request.config,
        .layout = request.layout,
    };
    transition_ledger_attachment ledger{.region = region};
    for (std::uint32_t index = 0; index < request.config.owner_capacity; ++index) {
        shared_transition_owner_slot* slot = owner_slot(ledger, index);
        const std::uint64_t references_offset =
            static_cast<std::uint64_t>(request.layout.references_offset) +
            static_cast<std::uint64_t>(index) * request.layout.references_per_owner *
                sizeof(transition_dependency_reference);
        if (slot == nullptr || references_offset > (std::numeric_limits<std::uint32_t>::max)()) {
            store_bootstrap_state(*header_value, bootstrap_state::failed);
            return transition_ledger_error::corrupt_layout;
        }
        *slot = shared_transition_owner_slot{
            .references_offset = static_cast<std::uint32_t>(references_offset),
            .reference_capacity = request.layout.references_per_owner,
        };
        store_state(*slot, transition_owner_state::free);
    }
    store_bootstrap_state(*header_value, bootstrap_state::ready);
    return transition_ledger_error::none;
}

/** Attach only after validating every immutable owner/reference slice. */
[[nodiscard]] inline transition_ledger_attach_result
attach_transition_ledger(std::span<std::byte> region) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_attach_result result{};
    transition_ledger_header* header_value = nullptr;
    result.error = structural_validate(region, &header_value);
    if (result.error != transition_ledger_error::none) {
        return result;
    }
    transition_ledger_attachment ledger{.region = region};
    for (std::uint32_t index = 0; index < header_value->config.owner_capacity; ++index) {
        shared_transition_owner_slot* slot = owner_slot(ledger, index);
        const std::uint64_t expected_offset =
            static_cast<std::uint64_t>(header_value->layout.references_offset) +
            static_cast<std::uint64_t>(index) * header_value->layout.references_per_owner *
                sizeof(transition_dependency_reference);
        if (slot == nullptr || slot->references_offset != expected_offset ||
            slot->reference_capacity != header_value->layout.references_per_owner ||
            !valid_owner_state(load_state(*slot))) {
            result.error = transition_ledger_error::corrupt_layout;
            return result;
        }
    }
    result.ledger = ledger;
    return result;
}

/** Claim the ProcNumber-indexed slot and retain its emergency owner reference. */
[[nodiscard]] inline transition_ledger_operation_result
claim_transition_owner(transition_ledger_attachment ledger,
                       const transition_owner_identity& owner,
                       const transition_dependency_reference& emergency_reference,
                       const transition_ledger_callbacks& callbacks,
                       transition_initial_reference_role initial_role =
                           transition_initial_reference_role::emergency_only) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.owner_locks_valid() ||
        !valid_owner_identity(owner, header_value->config.owner_capacity) ||
        (initial_role != transition_initial_reference_role::emergency_only &&
         initial_role != transition_initial_reference_role::precise_and_fallback)) {
        return transition_ledger_operation_result{
            .claim = transition_owner_claim{.owner = owner},
            .error = transition_ledger_error::invalid_argument,
            .coarse_fallback_required = true,
        };
    }
    if (!valid_reference(emergency_reference, header_value->config.dependency_partition_count)) {
        saturating_increment(header_value->metrics.bookkeeping_failure_count);
        saturating_increment(header_value->metrics.coarse_fallback_count);
        if (callbacks.mark_coarse_fallback != nullptr) {
            callbacks.mark_coarse_fallback(callbacks.context,
                                           transition_ledger_error::invalid_dependency_reference);
        }
        return transition_ledger_operation_result{
            .claim = transition_owner_claim{.owner = owner},
            .error = transition_ledger_error::invalid_dependency_reference,
            .coarse_fallback_required = true,
        };
    }
    const std::uint32_t slot_index = owner.proc_number;
    owner_lock_guard lock(callbacks, slot_index);
    shared_transition_owner_slot* slot = owner_slot(ledger, slot_index);
    if (slot == nullptr) {
        return transition_ledger_operation_result{
            .claim = transition_owner_claim{.owner = owner},
            .error = transition_ledger_error::corrupt_layout,
            .bootstrap_failed = true,
        };
    }
    if (load_state(*slot) != transition_owner_state::free) {
        return snapshot_result(*slot, transition_ledger_error::owner_busy);
    }
    const std::uint64_t claim_sequence = allocate_claim_sequence(*header_value);
    if (claim_sequence == 0) {
        transition_ledger_operation_result result =
            snapshot_result(*slot, transition_ledger_error::sequence_unavailable);
        result.claim = transition_owner_claim{.owner = owner};
        result.coarse_fallback_required = true;
        return result;
    }
    transition_dependency_reference* emergency = reference_at(ledger, *slot, 0);
    if (emergency == nullptr) {
        return snapshot_result(*slot, transition_ledger_error::corrupt_layout);
    }
    for (std::uint32_t index = 0; index < slot->reference_capacity; ++index) {
        transition_dependency_reference* reference = reference_at(ledger, *slot, index);
        if (reference == nullptr) {
            return snapshot_result(*slot, transition_ledger_error::corrupt_layout);
        }
        zero_reference(*reference);
    }
    *emergency = emergency_reference;
    slot->owner = owner;
    slot->claim_sequence = claim_sequence;
    slot->last_claim_sequence = claim_sequence;
    slot->precise_reference_count = 0;
    slot->transition_target_count = 0;
    slot->coarse = 0;
    slot->initial_reference_precise =
        initial_role == transition_initial_reference_role::precise_and_fallback ? 1U : 0U;
    slot->diagnostic_failure_count = 0;
    slot->last_error = transition_ledger_error::none;
    slot->repair_forbidden = 0;
    slot->collection_generation = 1;
    store_incremented(*slot, 0);
    store_outstanding(*slot, 0);
    store_state(*slot, transition_owner_state::collecting);
    saturating_increment(header_value->metrics.successful_claim_count);
    static_cast<void>(increment_current(header_value->metrics.current_owner_count,
                                        header_value->metrics.owner_high_water));
    transition_ledger_operation_result result = snapshot_result(*slot);
    result.reference_consumed = true;
    return result;
}

/**
 * Add one precise owner reference while the slot is collecting.
 *
 * The coarse marker is set before validation and restored only after the full
 * exact binding is durable.  Capacity or ABA trouble therefore cannot leave a
 * partial precise promise.
 */
[[nodiscard]] inline transition_ledger_operation_result
record_transition_dependency(transition_ledger_attachment ledger,
                             const transition_owner_claim& claim,
                             const transition_dependency_reference& reference,
                             const transition_ledger_callbacks& callbacks) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.owner_locks_valid() ||
        !valid_owner(claim, header_value->config.owner_capacity)) {
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::invalid_argument,
            .coarse_fallback_required = true,
        };
    }
    const std::uint32_t slot_index = claim.owner.proc_number;
    owner_lock_guard lock(callbacks, slot_index);
    shared_transition_owner_slot* slot = owner_slot(ledger, slot_index);
    if (slot == nullptr) {
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::corrupt_layout,
            .bootstrap_failed = true,
        };
    }
    if (!claim_matches(*slot, claim)) {
        return snapshot_result(*slot, transition_ledger_error::stale_owner);
    }
    if (load_state(*slot) != transition_owner_state::collecting) {
        return snapshot_result(*slot, transition_ledger_error::invalid_owner_state);
    }
    if (slot->coarse != 0) {
        return snapshot_result(*slot, transition_ledger_error::capacity_exceeded);
    }
    const std::uint32_t previous_coarse = slot->coarse;
    slot->coarse = 1;
    if (!valid_reference(reference, header_value->config.dependency_partition_count)) {
        note_bookkeeping_failure(
            *header_value, *slot, transition_ledger_error::invalid_dependency_reference);
        return snapshot_result(*slot, transition_ledger_error::invalid_dependency_reference);
    }
    if (slot->initial_reference_precise != 0) {
        transition_dependency_reference* initial = reference_at(ledger, *slot, 0);
        if (initial == nullptr) {
            note_diagnostic_failure(*header_value, *slot, transition_ledger_error::corrupt_layout);
            saturating_increment(header_value->metrics.bookkeeping_failure_count);
            return snapshot_result(*slot, transition_ledger_error::corrupt_layout);
        }
        if (initial->exact_identity_equal(reference)) {
            note_bookkeeping_failure(
                *header_value, *slot, transition_ledger_error::duplicate_dependency_reference);
            return snapshot_result(*slot, transition_ledger_error::duplicate_dependency_reference);
        }
        if (initial->key == reference.key) {
            note_bookkeeping_failure(
                *header_value, *slot, transition_ledger_error::dependency_aba_mismatch);
            return snapshot_result(*slot, transition_ledger_error::dependency_aba_mismatch);
        }
    }
    for (std::uint32_t index = 1; index <= slot->precise_reference_count; ++index) {
        transition_dependency_reference* existing = reference_at(ledger, *slot, index);
        if (existing == nullptr) {
            note_diagnostic_failure(*header_value, *slot, transition_ledger_error::corrupt_layout);
            saturating_increment(header_value->metrics.bookkeeping_failure_count);
            return snapshot_result(*slot, transition_ledger_error::corrupt_layout);
        }
        if (existing->exact_identity_equal(reference)) {
            note_bookkeeping_failure(
                *header_value, *slot, transition_ledger_error::duplicate_dependency_reference);
            return snapshot_result(*slot, transition_ledger_error::duplicate_dependency_reference);
        }
        if (existing->key == reference.key) {
            note_bookkeeping_failure(
                *header_value, *slot, transition_ledger_error::dependency_aba_mismatch);
            return snapshot_result(*slot, transition_ledger_error::dependency_aba_mismatch);
        }
    }
    if (slot->precise_reference_count + slot->initial_reference_precise >=
        header_value->config.precise_reference_capacity) {
        note_bookkeeping_failure(*header_value, *slot, transition_ledger_error::capacity_exceeded);
        return snapshot_result(*slot, transition_ledger_error::capacity_exceeded);
    }
    if (slot->collection_generation == (std::numeric_limits<std::uint32_t>::max)()) {
        note_bookkeeping_failure(
            *header_value, *slot, transition_ledger_error::sequence_unavailable);
        return snapshot_result(*slot, transition_ledger_error::sequence_unavailable);
    }
    const std::uint32_t target_index = slot->precise_reference_count + 1U;
    transition_dependency_reference* target = reference_at(ledger, *slot, target_index);
    if (target == nullptr) {
        note_diagnostic_failure(*header_value, *slot, transition_ledger_error::corrupt_layout);
        saturating_increment(header_value->metrics.bookkeeping_failure_count);
        return snapshot_result(*slot, transition_ledger_error::corrupt_layout);
    }
    *target = reference;
    ++slot->precise_reference_count;
    ++slot->collection_generation;
    slot->coarse = previous_coarse;
    transition_ledger_operation_result result = snapshot_result(*slot);
    result.reference_consumed = true;
    return result;
}

/** Make the preallocated emergency dependency sticky for this transaction. */
[[nodiscard]] inline transition_ledger_operation_result
force_transition_owner_coarse(transition_ledger_attachment ledger,
                              const transition_owner_claim& claim,
                              transition_ledger_error reason,
                              const transition_ledger_callbacks& callbacks) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.owner_locks_valid() ||
        !valid_owner(claim, header_value->config.owner_capacity)) {
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::invalid_argument,
            .coarse_fallback_required = true,
        };
    }
    owner_lock_guard lock(callbacks, claim.owner.proc_number);
    shared_transition_owner_slot* slot = owner_slot(ledger, claim.owner.proc_number);
    if (slot == nullptr || !claim_matches(*slot, claim)) {
        return slot == nullptr
        ? transition_ledger_operation_result{
              .claim = claim,
              .error = transition_ledger_error::corrupt_layout,
              .bootstrap_failed = true,
          }
        : snapshot_result(*slot, transition_ledger_error::stale_owner);
    }
    if (load_state(*slot) != transition_owner_state::collecting) {
        return snapshot_result(*slot, transition_ledger_error::invalid_owner_state);
    }
    note_bookkeeping_failure(*header_value, *slot, reason);
    return snapshot_result(*slot);
}

/**
 * Replace the precise Collecting set with an exact retained subset.
 *
 * This is the bounded savepoint/subtransaction rollback primitive.  Every
 * retained capability must already belong to this owner slot.  Removed owner
 * references are released under ascending dependency partition locks, which
 * the callback releases in reverse.  Any invalid input, overflow, ABA, lock,
 * release, or diagnostic failure makes the emergency coarse binding sticky.
 */
[[nodiscard]] inline transition_ledger_operation_result
rebuild_transition_dependencies(transition_ledger_attachment ledger,
                                const transition_owner_claim& claim,
                                std::span<const transition_dependency_reference> retained,
                                const transition_ledger_callbacks& callbacks) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.owner_locks_valid() ||
        !callbacks.partition_locks_valid() || callbacks.release_owner_reference_locked == nullptr ||
        !valid_owner(claim, header_value->config.owner_capacity)) {
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::invalid_argument,
            .coarse_fallback_required = true,
        };
    }

    constexpr std::size_t k_binding_capacity =
        static_cast<std::size_t>(k_maximum_transition_references) + 1U;
    std::array<bool, k_binding_capacity> keep{};
    std::array<std::uint32_t, k_binding_capacity> partitions{};
    std::uint32_t partition_count = 0;
    std::uint32_t observed_generation = 0;
    std::uint32_t observed_precise_count = 0;
    std::uint32_t previous_coarse = 0;
    shared_transition_owner_slot* slot = nullptr;

    {
        owner_lock_guard owner_lock(callbacks, claim.owner.proc_number);
        slot = owner_slot(ledger, claim.owner.proc_number);
        if (slot == nullptr) {
            return transition_ledger_operation_result{
                .claim = claim,
                .error = transition_ledger_error::corrupt_layout,
                .bootstrap_failed = true,
            };
        }
        if (!claim_matches(*slot, claim)) {
            return snapshot_result(*slot, transition_ledger_error::stale_owner);
        }
        if (load_state(*slot) != transition_owner_state::collecting) {
            return snapshot_result(*slot, transition_ledger_error::invalid_owner_state);
        }
        previous_coarse = slot->coarse;
        slot->coarse = 1;
        observed_precise_count = slot->precise_reference_count;
        if (retained.size() > observed_precise_count ||
            retained.size() > header_value->config.precise_reference_capacity ||
            slot->collection_generation == (std::numeric_limits<std::uint32_t>::max)()) {
            note_bookkeeping_failure(
                *header_value, *slot, transition_ledger_error::subtransaction_rebuild_failed);
            return snapshot_result(*slot, transition_ledger_error::subtransaction_rebuild_failed);
        }

        for (const transition_dependency_reference& wanted : retained) {
            if (!valid_reference(wanted, header_value->config.dependency_partition_count)) {
                note_bookkeeping_failure(
                    *header_value, *slot, transition_ledger_error::invalid_dependency_reference);
                return snapshot_result(*slot,
                                       transition_ledger_error::invalid_dependency_reference);
            }
            std::uint32_t matched_index = 0;
            for (std::uint32_t index = 1; index <= observed_precise_count; ++index) {
                transition_dependency_reference* existing = reference_at(ledger, *slot, index);
                if (existing == nullptr) {
                    note_diagnostic_failure(
                        *header_value, *slot, transition_ledger_error::corrupt_layout);
                    saturating_increment(header_value->metrics.bookkeeping_failure_count);
                    return snapshot_result(*slot, transition_ledger_error::corrupt_layout);
                }
                if (existing->exact_identity_equal(wanted)) {
                    matched_index = index;
                    break;
                }
            }
            if (matched_index == 0 || keep[matched_index]) {
                note_bookkeeping_failure(
                    *header_value, *slot, transition_ledger_error::subtransaction_rebuild_failed);
                return snapshot_result(*slot,
                                       transition_ledger_error::subtransaction_rebuild_failed);
            }
            keep[matched_index] = true;
        }

        for (std::uint32_t index = 1; index <= observed_precise_count; ++index) {
            if (keep[index]) {
                continue;
            }
            transition_dependency_reference* removed = reference_at(ledger, *slot, index);
            if (removed == nullptr) {
                note_diagnostic_failure(
                    *header_value, *slot, transition_ledger_error::corrupt_layout);
                saturating_increment(header_value->metrics.bookkeeping_failure_count);
                return snapshot_result(*slot, transition_ledger_error::corrupt_layout);
            }
            if (removed->active) {
                partitions[partition_count++] = removed->partition_index;
            }
        }
        partition_count =
            sort_and_deduplicate(std::span<std::uint32_t>(partitions), partition_count);
        observed_generation = slot->collection_generation;
        if (partition_count == 0 && retained.size() == observed_precise_count) {
            slot->coarse = previous_coarse;
            return snapshot_result(*slot);
        }
    }

    const std::span<const std::uint32_t> locked_partitions(partitions.data(), partition_count);
    if (partition_count != 0 && !callbacks.lock_partitions(callbacks.context, locked_partitions)) {
        {
            owner_lock_guard owner_lock(callbacks, claim.owner.proc_number);
            slot = owner_slot(ledger, claim.owner.proc_number);
            if (slot != nullptr && claim_matches(*slot, claim) &&
                load_state(*slot) == transition_owner_state::collecting) {
                note_bookkeeping_failure(
                    *header_value, *slot, transition_ledger_error::partition_lock_failed);
            }
        }
        if (callbacks.mark_coarse_fallback != nullptr) {
            callbacks.mark_coarse_fallback(callbacks.context,
                                           transition_ledger_error::partition_lock_failed);
        }
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::partition_lock_failed,
            .state = transition_owner_state::collecting,
            .coarse_fallback_required = true,
        };
    }

    transition_ledger_operation_result result{};
    bool notify_coarse = false;
    callbacks.lock_owner_slot(callbacks.context, claim.owner.proc_number);
    slot = owner_slot(ledger, claim.owner.proc_number);
    if (slot == nullptr || !claim_matches(*slot, claim) ||
        load_state(*slot) != transition_owner_state::collecting ||
        slot->collection_generation != observed_generation ||
        slot->precise_reference_count != observed_precise_count) {
        if (slot != nullptr && claim_matches(*slot, claim)) {
            note_bookkeeping_failure(
                *header_value, *slot, transition_ledger_error::subtransaction_rebuild_failed);
            result = snapshot_result(*slot, transition_ledger_error::subtransaction_rebuild_failed);
        } else {
            result = transition_ledger_operation_result{
                .claim = claim,
                .error = transition_ledger_error::stale_owner,
                .coarse_fallback_required = true,
            };
        }
        notify_coarse = true;
    } else {
        bool release_failed = false;
        std::uint32_t diagnostic_failures = 0;
        for (std::uint32_t index = 1; index <= observed_precise_count; ++index) {
            if (keep[index]) {
                continue;
            }
            transition_dependency_reference* removed = reference_at(ledger, *slot, index);
            if (removed == nullptr) {
                release_failed = true;
                break;
            }
            if (!removed->active) {
                continue;
            }
            const transition_callback_result mutation =
                callbacks.release_owner_reference_locked(callbacks.context, removed);
            const bool contract_valid = mutation.applied ? !removed->active : removed->active;
            if (mutation.applied) {
                removed->active = false;
            }
            release_failed = release_failed || !mutation.applied || !contract_valid;
            if (!mutation.diagnostic_success) {
                ++diagnostic_failures;
            }
            notify_coarse =
                notify_coarse || mutation.coarse_fallback_required || !mutation.diagnostic_success;
        }

        bool all_removed = true;
        for (std::uint32_t index = 1; index <= observed_precise_count; ++index) {
            const transition_dependency_reference* reference = reference_at(ledger, *slot, index);
            if (!keep[index] && (reference == nullptr || reference->active)) {
                all_removed = false;
                break;
            }
        }
        release_failed = release_failed || !all_removed;
        if (!release_failed) {
            std::uint32_t target_index = 1;
            for (std::uint32_t index = 1; index <= observed_precise_count; ++index) {
                if (!keep[index]) {
                    continue;
                }
                transition_dependency_reference* source = reference_at(ledger, *slot, index);
                transition_dependency_reference* target = reference_at(ledger, *slot, target_index);
                if (source == nullptr || target == nullptr) {
                    release_failed = true;
                    break;
                }
                if (source != target) {
                    *target = *source;
                }
                ++target_index;
            }
            if (!release_failed) {
                for (std::uint32_t index = target_index; index < slot->reference_capacity;
                     ++index) {
                    transition_dependency_reference* unused = reference_at(ledger, *slot, index);
                    if (unused == nullptr) {
                        release_failed = true;
                        break;
                    }
                    zero_reference(*unused);
                }
            }
            if (!release_failed) {
                slot->precise_reference_count = static_cast<std::uint32_t>(retained.size());
                ++slot->collection_generation;
            }
        }

        add_diagnostic_failures(*slot, diagnostic_failures);
        if (diagnostic_failures != 0) {
            saturating_add(header_value->metrics.diagnostic_failure_count, diagnostic_failures);
        }
        if (release_failed || diagnostic_failures != 0 || notify_coarse) {
            note_bookkeeping_failure(
                *header_value, *slot, transition_ledger_error::subtransaction_rebuild_failed);
            result = snapshot_result(*slot,
                                     release_failed
                                         ? transition_ledger_error::reference_release_failed
                                         : transition_ledger_error::subtransaction_rebuild_failed);
            notify_coarse = true;
        } else {
            slot->coarse = previous_coarse;
            slot->last_error = transition_ledger_error::none;
            result = snapshot_result(*slot);
        }
    }
    callbacks.unlock_owner_slot(callbacks.context, claim.owner.proc_number);
    if (partition_count != 0) {
        callbacks.unlock_partitions_reverse(callbacks.context, locked_partitions);
    }
    if (notify_coarse && callbacks.mark_coarse_fallback != nullptr) {
        callbacks.mark_coarse_fallback(callbacks.context,
                                       transition_ledger_error::subtransaction_rebuild_failed);
    }
    return result;
}

/**
 * Publish a bounded transition prefix without holding an owner lock while
 * dependency partitions are acquired.  Any begin failure rolls back every
 * applied prefix member before returning to sticky coarse collection.
 */
[[nodiscard]] inline transition_ledger_operation_result
begin_owner_transitions(transition_ledger_attachment ledger,
                        const transition_owner_claim& claim,
                        const transition_ledger_callbacks& callbacks) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.transition_mutations_valid() ||
        !valid_owner(claim, header_value->config.owner_capacity)) {
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::invalid_argument,
            .coarse_fallback_required = true,
        };
    }
    constexpr std::size_t k_binding_capacity =
        static_cast<std::size_t>(k_maximum_transition_references) + 1U;
    std::array<std::uint32_t, k_binding_capacity> binding_indices{};
    std::array<std::uint32_t, k_binding_capacity> partitions{};
    std::uint32_t binding_count = 0;
    std::uint32_t partition_count = 0;
    shared_transition_owner_slot* slot = nullptr;
    {
        owner_lock_guard owner_lock(callbacks, claim.owner.proc_number);
        slot = owner_slot(ledger, claim.owner.proc_number);
        if (slot == nullptr) {
            return transition_ledger_operation_result{
                .claim = claim,
                .error = transition_ledger_error::corrupt_layout,
                .bootstrap_failed = true,
            };
        }
        if (!claim_matches(*slot, claim)) {
            return snapshot_result(*slot, transition_ledger_error::stale_owner);
        }
        if (load_state(*slot) != transition_owner_state::collecting) {
            return snapshot_result(*slot, transition_ledger_error::invalid_owner_state);
        }
        if (slot->coarse != 0) {
            binding_indices[0] = 0;
            binding_count = 1;
        } else {
            binding_count = slot->precise_reference_count + slot->initial_reference_precise;
            std::uint32_t target = 0;
            if (slot->initial_reference_precise != 0) {
                binding_indices[target++] = 0;
            }
            for (std::uint32_t index = 0; index < slot->precise_reference_count; ++index) {
                binding_indices[target++] = index + 1U;
            }
        }
        if (binding_count == 0) {
            note_bookkeeping_failure(
                *header_value, *slot, transition_ledger_error::invalid_owner_state);
            return snapshot_result(*slot, transition_ledger_error::invalid_owner_state);
        }
        for (std::uint32_t index = 0; index < binding_count; ++index) {
            transition_dependency_reference* reference =
                reference_at(ledger, *slot, binding_indices[index]);
            if (reference == nullptr ||
                !valid_reference(*reference, header_value->config.dependency_partition_count)) {
                note_bookkeeping_failure(
                    *header_value, *slot, transition_ledger_error::invalid_dependency_reference);
                return snapshot_result(*slot,
                                       transition_ledger_error::invalid_dependency_reference);
            }
            partitions[index] = reference->partition_index;
        }
        partition_count = sort_and_deduplicate(std::span<std::uint32_t>(partitions), binding_count);
        slot->transition_target_count = binding_count;
        store_incremented(*slot, 0);
        store_outstanding(*slot, 0);
        store_state(*slot, transition_owner_state::transitioning);
    }

    const std::span<const std::uint32_t> locked_partitions(partitions.data(), partition_count);
    if (!callbacks.lock_partitions(callbacks.context, locked_partitions)) {
        {
            owner_lock_guard owner_lock(callbacks, claim.owner.proc_number);
            slot = owner_slot(ledger, claim.owner.proc_number);
            if (slot != nullptr && claim_matches(*slot, claim) &&
                load_state(*slot) == transition_owner_state::transitioning &&
                load_incremented(*slot) == 0) {
                note_coarse_fallback(
                    *header_value, *slot, transition_ledger_error::partition_lock_failed);
                slot->transition_target_count = 0;
                store_state(*slot, transition_owner_state::collecting);
            }
        }
        saturating_increment(header_value->metrics.begin_failure_count);
        if (callbacks.mark_coarse_fallback != nullptr) {
            callbacks.mark_coarse_fallback(callbacks.context,
                                           transition_ledger_error::partition_lock_failed);
        }
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::partition_lock_failed,
            .state = transition_owner_state::collecting,
            .coarse_fallback_required = true,
        };
    }

    // Global mutation order is dependency partitions, then owner slot.  The
    // owner lock was not held while acquiring partitions and is released first.
    callbacks.lock_owner_slot(callbacks.context, claim.owner.proc_number);
    slot = owner_slot(ledger, claim.owner.proc_number);
    const bool cancelled_by_finish =
        slot != nullptr &&
        ((load_state(*slot) == transition_owner_state::free &&
          slot->last_claim_sequence == claim.claim_sequence) ||
         (claim_matches(*slot, claim) && load_state(*slot) == transition_owner_state::repairing));
    const transition_owner_state cancelled_state =
        slot == nullptr ? transition_owner_state::free : load_state(*slot);
    if (slot == nullptr || !claim_matches(*slot, claim) ||
        load_state(*slot) != transition_owner_state::transitioning ||
        load_incremented(*slot) != 0 || load_outstanding(*slot) != 0 ||
        slot->transition_target_count != binding_count) {
        callbacks.unlock_owner_slot(callbacks.context, claim.owner.proc_number);
        callbacks.unlock_partitions_reverse(callbacks.context, locked_partitions);
        if (cancelled_by_finish) {
            return transition_ledger_operation_result{
                .claim = claim,
                .error = transition_ledger_error::stale_owner,
                .state = cancelled_state,
            };
        }
        if (callbacks.mark_bootstrap_failed != nullptr) {
            callbacks.mark_bootstrap_failed(callbacks.context,
                                            transition_ledger_error::bootstrap_failed);
        }
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::bootstrap_failed,
            .state = transition_owner_state::repairing,
            .coarse_fallback_required = true,
            .bootstrap_failed = true,
        };
    }

    bool begin_failed = false;
    bool begin_contract_failed = false;
    bool coarse_required = false;
    std::uint32_t diagnostic_failures = 0;
    std::uint32_t applied_count = 0;
    for (std::uint32_t index = 0; index < binding_count; ++index) {
        transition_dependency_reference* reference =
            reference_at(ledger, *slot, binding_indices[index]);
        if (reference == nullptr) {
            begin_failed = true;
            begin_contract_failed = true;
            break;
        }
        const transition_callback_result mutation =
            callbacks.begin_transition_locked(callbacks.context, reference);
        const bool contract_valid =
            mutation.applied ? reference->transition_active : !reference->transition_active;
        if (mutation.applied) {
            reference->transition_active = true;
        }
        if (!contract_valid) {
            begin_failed = true;
            begin_contract_failed = true;
            break;
        }
        if (mutation.applied) {
            ++applied_count;
            store_incremented(*slot, applied_count);
            store_outstanding(*slot, applied_count);
            static_cast<void>(increment_current(header_value->metrics.current_transition_count,
                                                header_value->metrics.transition_high_water));
        }
        if (!mutation.diagnostic_success) {
            ++diagnostic_failures;
        }
        coarse_required =
            coarse_required || mutation.coarse_fallback_required || !mutation.diagnostic_success;
        if (!mutation.applied || !mutation.diagnostic_success ||
            mutation.coarse_fallback_required) {
            begin_failed = true;
            break;
        }
    }

    drain_result rollback{};
    if (begin_failed) {
        rollback = drain_locked(
            ledger, *header_value, *slot, transition_resolution::aborted, false, callbacks);
        diagnostic_failures += rollback.diagnostic_failures;
        coarse_required = true;
    }
    bool bootstrap_failed = rollback.bootstrap_failed || begin_contract_failed;
    transition_ledger_error error = transition_ledger_error::none;
    if (begin_failed) {
        saturating_increment(header_value->metrics.begin_failure_count);
        add_diagnostic_failures(*slot, diagnostic_failures);
        if (diagnostic_failures != 0) {
            saturating_add(header_value->metrics.diagnostic_failure_count, diagnostic_failures);
        }
        note_coarse_fallback(
            *header_value, *slot, transition_ledger_error::dependency_begin_failed);
        if (rollback.outstanding_transitions == 0 && !bootstrap_failed) {
            store_incremented(*slot, 0);
            store_outstanding(*slot, 0);
            slot->transition_target_count = 0;
            slot->last_error = transition_ledger_error::dependency_begin_failed;
            store_state(*slot, transition_owner_state::collecting);
            error = transition_ledger_error::dependency_begin_failed;
        } else {
            slot->last_error = transition_ledger_error::bootstrap_failed;
            slot->repair_forbidden = begin_contract_failed || rollback.contract_failure ? 1U : 0U;
            store_state(*slot, transition_owner_state::repairing);
            bootstrap_failed = true;
            error = transition_ledger_error::bootstrap_failed;
        }
    } else {
        add_diagnostic_failures(*slot, diagnostic_failures);
        if (diagnostic_failures != 0) {
            saturating_add(header_value->metrics.diagnostic_failure_count, diagnostic_failures);
        }
        store_state(*slot, transition_owner_state::active);
        error = transition_ledger_error::none;
    }
    const transition_ledger_operation_result result{
        .claim = claim,
        .error = error,
        .state = bootstrap_failed ? transition_owner_state::repairing
                                  : (begin_failed ? transition_owner_state::collecting
                                                  : transition_owner_state::active),
        .precise_reference_count = slot->precise_reference_count + slot->initial_reference_precise,
        .incremented_count = load_incremented(*slot),
        .outstanding_transition_count = load_outstanding(*slot),
        .diagnostic_failure_count = slot->diagnostic_failure_count,
        .coarse_fallback_required = coarse_required || begin_failed,
        .bootstrap_failed = bootstrap_failed,
    };
    callbacks.unlock_owner_slot(callbacks.context, claim.owner.proc_number);
    callbacks.unlock_partitions_reverse(callbacks.context, locked_partitions);
    if (coarse_required && callbacks.mark_coarse_fallback != nullptr) {
        callbacks.mark_coarse_fallback(callbacks.context,
                                       error == transition_ledger_error::none
                                           ? transition_ledger_error::dependency_begin_failed
                                           : error);
    }
    if (bootstrap_failed && callbacks.mark_bootstrap_failed != nullptr) {
        callbacks.mark_bootstrap_failed(callbacks.context,
                                        transition_ledger_error::bootstrap_failed);
    }
    return result;
}

/**
 * Resolve every durable transition and release every retained owner reference.
 * The loop never stops on diagnostic/sequence trouble after an applied
 * decrement.  A true unapplied decrement leaves REPAIRING and fails bootstrap.
 */
[[nodiscard]] inline transition_ledger_operation_result
finish_owner_transitions(transition_ledger_attachment ledger,
                         const transition_owner_claim& claim,
                         transition_resolution resolution,
                         const transition_ledger_callbacks& callbacks) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.transition_mutations_valid() ||
        !valid_owner(claim, header_value->config.owner_capacity) ||
        (resolution != transition_resolution::aborted &&
         resolution != transition_resolution::committed &&
         resolution != transition_resolution::known_aborted)) {
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::invalid_argument,
            .coarse_fallback_required = true,
        };
    }
    constexpr std::size_t k_binding_capacity =
        static_cast<std::size_t>(k_maximum_transition_references) + 1U;
    std::array<std::uint32_t, k_binding_capacity> partitions{};
    std::uint32_t partition_count = 0;
    std::uint32_t expected_incremented = 0;
    std::uint32_t expected_outstanding = 0;
    shared_transition_owner_slot* slot = nullptr;
    {
        owner_lock_guard owner_lock(callbacks, claim.owner.proc_number);
        slot = owner_slot(ledger, claim.owner.proc_number);
        if (slot == nullptr) {
            return transition_ledger_operation_result{
                .claim = claim,
                .error = transition_ledger_error::corrupt_layout,
                .bootstrap_failed = true,
            };
        }
        if (!claim_matches(*slot, claim)) {
            return snapshot_result(*slot, transition_ledger_error::stale_owner);
        }
        const transition_owner_state state = load_state(*slot);
        if (state == transition_owner_state::collecting &&
            resolution == transition_resolution::committed) {
            return snapshot_result(*slot, transition_ledger_error::invalid_owner_state);
        }
        if (state == transition_owner_state::repairing && slot->repair_forbidden != 0) {
            return snapshot_result(*slot, transition_ledger_error::bootstrap_failed);
        }
        if (state != transition_owner_state::collecting &&
            state != transition_owner_state::transitioning &&
            state != transition_owner_state::active && state != transition_owner_state::repairing) {
            return snapshot_result(*slot, transition_ledger_error::invalid_owner_state);
        }
        if (state != transition_owner_state::repairing &&
            !compare_exchange_state(*slot, state, transition_owner_state::repairing)) {
            return snapshot_result(*slot, transition_ledger_error::invalid_owner_state);
        }
        partition_count =
            collect_partitions(ledger, *slot, std::span<std::uint32_t>(partitions), false);
        if (partition_count == k_invalid_transition_offset) {
            slot->last_error = transition_ledger_error::corrupt_layout;
            return snapshot_result(*slot, transition_ledger_error::corrupt_layout);
        }
        expected_incremented = load_incremented(*slot);
        expected_outstanding = load_outstanding(*slot);
    }

    const std::span<const std::uint32_t> locked_partitions(partitions.data(), partition_count);
    if (partition_count != 0 && !callbacks.lock_partitions(callbacks.context, locked_partitions)) {
        {
            owner_lock_guard owner_lock(callbacks, claim.owner.proc_number);
            slot = owner_slot(ledger, claim.owner.proc_number);
            if (slot != nullptr && claim_matches(*slot, claim)) {
                note_coarse_fallback(
                    *header_value, *slot, transition_ledger_error::partition_lock_failed);
            }
        }
        saturating_increment(header_value->metrics.finish_failure_count);
        if (callbacks.mark_bootstrap_failed != nullptr) {
            callbacks.mark_bootstrap_failed(callbacks.context,
                                            transition_ledger_error::partition_lock_failed);
        }
        return transition_ledger_operation_result{
            .claim = claim,
            .error = transition_ledger_error::partition_lock_failed,
            .state = transition_owner_state::repairing,
            .incremented_count = expected_incremented,
            .outstanding_transition_count = expected_outstanding,
            .coarse_fallback_required = true,
            .bootstrap_failed = true,
        };
    }
    drain_result drain{};
    bool bootstrap_failed = false;
    transition_ledger_error error = transition_ledger_error::none;
    std::uint32_t precise_count = 0;
    std::uint32_t diagnostics = 0;
    std::uint32_t final_incremented = expected_incremented;
    callbacks.lock_owner_slot(callbacks.context, claim.owner.proc_number);
    slot = owner_slot(ledger, claim.owner.proc_number);
    if (slot == nullptr) {
        bootstrap_failed = true;
        error = transition_ledger_error::bootstrap_failed;
    } else if (!claim_matches(*slot, claim)) {
        error = transition_ledger_error::stale_owner;
        final_incremented = 0;
    } else if (load_state(*slot) != transition_owner_state::repairing ||
               load_incremented(*slot) != expected_incremented ||
               load_outstanding(*slot) != expected_outstanding) {
        bootstrap_failed = true;
        error = transition_ledger_error::bootstrap_failed;
    } else {
        drain = drain_locked(ledger, *header_value, *slot, resolution, true, callbacks);
        const bool fully_drained =
            drain.outstanding_transitions == 0 && drain.retained_owner_references == 0;
        bootstrap_failed = drain.bootstrap_failed || !fully_drained;
        error = drain.error;
        precise_count = slot->precise_reference_count + slot->initial_reference_precise;
        add_diagnostic_failures(*slot, drain.diagnostic_failures);
        if (drain.diagnostic_failures != 0) {
            saturating_add(header_value->metrics.diagnostic_failure_count,
                           drain.diagnostic_failures);
        }
        if (drain.coarse_fallback_required || drain.diagnostic_failures != 0) {
            saturating_increment(header_value->metrics.coarse_fallback_count);
        }
        diagnostics = slot->diagnostic_failure_count;
        if (fully_drained && !bootstrap_failed) {
            store_incremented(*slot, 0);
            store_outstanding(*slot, 0);
            clear_owner_slot(ledger, *header_value, *slot);
            final_incremented = 0;
        } else {
            note_coarse_fallback(*header_value, *slot, transition_ledger_error::bootstrap_failed);
            slot->repair_forbidden = drain.contract_failure ? 1U : 0U;
            store_state(*slot, transition_owner_state::repairing);
            final_incremented = load_incremented(*slot);
            error = transition_ledger_error::bootstrap_failed;
        }
    }
    callbacks.unlock_owner_slot(callbacks.context, claim.owner.proc_number);
    if (partition_count != 0) {
        callbacks.unlock_partitions_reverse(callbacks.context, locked_partitions);
    }
    if (error != transition_ledger_error::none || bootstrap_failed) {
        saturating_increment(header_value->metrics.finish_failure_count);
    }
    if ((drain.coarse_fallback_required || drain.diagnostic_failures != 0) &&
        callbacks.mark_coarse_fallback != nullptr) {
        callbacks.mark_coarse_fallback(callbacks.context,
                                       error == transition_ledger_error::none
                                           ? transition_ledger_error::dependency_finish_failed
                                           : error);
    }
    if (bootstrap_failed && callbacks.mark_bootstrap_failed != nullptr) {
        callbacks.mark_bootstrap_failed(callbacks.context,
                                        transition_ledger_error::bootstrap_failed);
    }
    return transition_ledger_operation_result{
        .claim = claim,
        .error = error,
        .state =
            bootstrap_failed ? transition_owner_state::repairing : transition_owner_state::free,
        .precise_reference_count = precise_count,
        .incremented_count = final_incremented,
        .outstanding_transition_count = drain.outstanding_transitions,
        .diagnostic_failure_count = diagnostics,
        .coarse_fallback_required =
            drain.coarse_fallback_required || drain.diagnostic_failures != 0,
        .bootstrap_failed = bootstrap_failed,
    };
}

/**
 * Sample liveness outside every cache lock, then revalidate the full token.
 * Dead COLLECTING owners with no transition are aborted safely.  Any dead
 * TRANSITIONING/ACTIVE/REPAIRING owner fails bootstrap without guessed repair.
 */
[[nodiscard]] inline transition_repair_result
repair_dead_transition_owner(transition_ledger_attachment ledger,
                             std::uint32_t slot_index,
                             const transition_ledger_callbacks& callbacks) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.transition_mutations_valid() ||
        callbacks.owner_is_alive == nullptr || slot_index >= header_value->config.owner_capacity) {
        return transition_repair_result{
            .error = transition_ledger_error::invalid_argument,
        };
    }
    transition_owner_claim observed{};
    transition_owner_state observed_state = transition_owner_state::free;
    std::uint32_t observed_incremented = 0;
    bool fatal_dead_owner = false;
    {
        owner_lock_guard owner_lock(callbacks, slot_index);
        shared_transition_owner_slot* slot = owner_slot(ledger, slot_index);
        if (slot == nullptr) {
            return transition_repair_result{
                .error = transition_ledger_error::corrupt_layout,
                .bootstrap_failed = true,
            };
        }
        observed_state = load_state(*slot);
        if (observed_state == transition_owner_state::free) {
            return transition_repair_result{
                .error = transition_ledger_error::nothing_to_repair,
            };
        }
        observed = transition_owner_claim{
            .owner = slot->owner,
            .claim_sequence = slot->claim_sequence,
        };
        observed_incremented = load_incremented(*slot);
    }

    if (callbacks.owner_is_alive(callbacks.context, observed.owner)) {
        return transition_repair_result{
            .observed_claim = observed,
            .error = transition_ledger_error::owner_alive,
            .disposition = transition_repair_disposition::owner_still_alive,
        };
    }

    {
        owner_lock_guard owner_lock(callbacks, slot_index);
        shared_transition_owner_slot* slot = owner_slot(ledger, slot_index);
        if (slot == nullptr || !claim_matches(*slot, observed) ||
            load_state(*slot) != observed_state ||
            load_incremented(*slot) != observed_incremented) {
            return transition_repair_result{
                .observed_claim = observed,
                .error = transition_ledger_error::stale_owner,
            };
        }
        if (observed_state != transition_owner_state::collecting || observed_incremented != 0 ||
            load_outstanding(*slot) != 0) {
            note_coarse_fallback(*header_value, *slot, transition_ledger_error::bootstrap_failed);
            fatal_dead_owner = true;
        }
    }
    if (fatal_dead_owner) {
        saturating_increment(header_value->metrics.fatal_owner_count);
        if (callbacks.mark_bootstrap_failed != nullptr) {
            callbacks.mark_bootstrap_failed(callbacks.context,
                                            transition_ledger_error::bootstrap_failed);
        }
        return transition_repair_result{
            .observed_claim = observed,
            .error = transition_ledger_error::bootstrap_failed,
            .disposition = transition_repair_disposition::fatal_transition_owner,
            .bootstrap_failed = true,
        };
    }

    const transition_ledger_operation_result cleanup =
        finish_owner_transitions(ledger, observed, transition_resolution::aborted, callbacks);
    if (cleanup.bootstrap_failed) {
        saturating_increment(header_value->metrics.fatal_owner_count);
    } else {
        saturating_increment(header_value->metrics.repaired_collecting_count);
    }
    return transition_repair_result{
        .observed_claim = observed,
        .error = cleanup.error,
        .disposition = cleanup.bootstrap_failed
                           ? transition_repair_disposition::fatal_transition_owner
                           : transition_repair_disposition::cleared_dead_collecting,
        .bootstrap_failed = cleanup.bootstrap_failed,
    };
}

/** Copy non-FREE slots in increasing ProcNumber order. */
[[nodiscard]] inline transition_owner_diagnostics_result
transition_owner_diagnostics(transition_ledger_attachment ledger,
                             std::span<transition_owner_diagnostic> output,
                             const transition_ledger_callbacks& callbacks) noexcept {
    using namespace transition_ledger_detail;
    transition_owner_diagnostics_result result{};
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.owner_locks_valid()) {
        result.error = transition_ledger_error::invalid_argument;
        return result;
    }
    for (std::uint32_t slot_index = 0; slot_index < header_value->config.owner_capacity;
         ++slot_index) {
        owner_lock_guard owner_lock(callbacks, slot_index);
        shared_transition_owner_slot* slot = owner_slot(ledger, slot_index);
        if (slot == nullptr) {
            result.error = transition_ledger_error::corrupt_layout;
            return result;
        }
        const transition_owner_state state = load_state(*slot);
        if (state == transition_owner_state::free) {
            continue;
        }
        ++result.total_non_free;
        if (result.copied_count >= output.size()) {
            result.truncated = true;
            continue;
        }
        std::uint32_t retained = 0;
        for (std::uint32_t index = 0; index < slot->reference_capacity; ++index) {
            const transition_dependency_reference* reference = reference_at(ledger, *slot, index);
            if (reference == nullptr) {
                result.error = transition_ledger_error::corrupt_layout;
                return result;
            }
            retained += reference->active ? 1U : 0U;
        }
        output[result.copied_count++] = transition_owner_diagnostic{
            .slot_index = slot_index,
            .state = state,
            .owner = slot->owner,
            .claim_sequence = slot->claim_sequence,
            .precise_reference_count =
                slot->precise_reference_count + slot->initial_reference_precise,
            .incremented_count = load_incremented(*slot),
            .outstanding_transition_count = load_outstanding(*slot),
            .retained_owner_reference_count = retained,
            .coarse = slot->coarse,
            .diagnostic_failure_count = slot->diagnostic_failure_count,
            .last_error = slot->last_error,
            .repair_forbidden = slot->repair_forbidden,
        };
    }
    return result;
}

/** Lock-free bounded aggregate snapshot; individual fields may advance. */
[[nodiscard]] inline transition_ledger_metrics_result
transition_ledger_metrics(transition_ledger_attachment ledger) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || header_value->magic != k_transition_ledger_magic ||
        header_value->abi_revision != k_transition_ledger_abi_revision ||
        load_bootstrap_state(*header_value) != bootstrap_state::ready) {
        return transition_ledger_metrics_result{
            .error = transition_ledger_error::not_initialized,
        };
    }
    shared_transition_ledger_metrics& shared = header_value->metrics;
    return transition_ledger_metrics_result{
        .metrics =
            shared_transition_ledger_metrics{
                .successful_claim_count =
                    std::atomic_ref<std::uint64_t>(shared.successful_claim_count)
                        .load(std::memory_order_acquire),
                .coarse_fallback_count =
                    std::atomic_ref<std::uint64_t>(shared.coarse_fallback_count)
                        .load(std::memory_order_acquire),
                .bookkeeping_failure_count =
                    std::atomic_ref<std::uint64_t>(shared.bookkeeping_failure_count)
                        .load(std::memory_order_acquire),
                .begin_failure_count = std::atomic_ref<std::uint64_t>(shared.begin_failure_count)
                                           .load(std::memory_order_acquire),
                .finish_failure_count = std::atomic_ref<std::uint64_t>(shared.finish_failure_count)
                                            .load(std::memory_order_acquire),
                .diagnostic_failure_count =
                    std::atomic_ref<std::uint64_t>(shared.diagnostic_failure_count)
                        .load(std::memory_order_acquire),
                .repaired_collecting_count =
                    std::atomic_ref<std::uint64_t>(shared.repaired_collecting_count)
                        .load(std::memory_order_acquire),
                .fatal_owner_count = std::atomic_ref<std::uint64_t>(shared.fatal_owner_count)
                                         .load(std::memory_order_acquire),
                .current_owner_count = std::atomic_ref<std::uint32_t>(shared.current_owner_count)
                                           .load(std::memory_order_acquire),
                .owner_high_water = std::atomic_ref<std::uint32_t>(shared.owner_high_water)
                                        .load(std::memory_order_acquire),
                .current_transition_count =
                    std::atomic_ref<std::uint32_t>(shared.current_transition_count)
                        .load(std::memory_order_acquire),
                .transition_high_water =
                    std::atomic_ref<std::uint32_t>(shared.transition_high_water)
                        .load(std::memory_order_acquire),
            },
    };
}

/** Bounded structural oracle; takes exactly one owner lock at a time. */
[[nodiscard]] inline transition_ledger_error
deep_validate_transition_ledger(transition_ledger_attachment ledger,
                                const transition_ledger_callbacks& callbacks) noexcept {
    using namespace transition_ledger_detail;
    transition_ledger_header* header_value = header(ledger);
    if (header_value == nullptr || !callbacks.owner_locks_valid()) {
        return transition_ledger_error::invalid_argument;
    }
    for (std::uint32_t slot_index = 0; slot_index < header_value->config.owner_capacity;
         ++slot_index) {
        owner_lock_guard owner_lock(callbacks, slot_index);
        shared_transition_owner_slot* slot = owner_slot(ledger, slot_index);
        if (slot == nullptr || !valid_owner_state(load_state(*slot)) ||
            slot->reference_capacity != header_value->layout.references_per_owner ||
            slot->initial_reference_precise > 1U ||
            slot->precise_reference_count + slot->initial_reference_precise >
                header_value->config.precise_reference_capacity ||
            load_outstanding(*slot) > load_incremented(*slot) ||
            load_incremented(*slot) > slot->transition_target_count ||
            slot->transition_target_count > header_value->config.precise_reference_capacity + 1U) {
            return transition_ledger_error::corrupt_ledger;
        }
        const transition_owner_state state = load_state(*slot);
        if (state == transition_owner_state::free) {
            if (slot->claim_sequence != 0 || slot->owner.pid != 0 ||
                slot->precise_reference_count != 0 || slot->initial_reference_precise != 0 ||
                load_incremented(*slot) != 0 || load_outstanding(*slot) != 0 ||
                slot->collection_generation != 0) {
                return transition_ledger_error::corrupt_ledger;
            }
            for (std::uint32_t index = 0; index < slot->reference_capacity; ++index) {
                const transition_dependency_reference* reference =
                    reference_at(ledger, *slot, index);
                if (reference == nullptr || !reference_is_zero(*reference)) {
                    return transition_ledger_error::corrupt_ledger;
                }
            }
            continue;
        }
        if (slot->repair_forbidden != 0 && state != transition_owner_state::repairing) {
            return transition_ledger_error::corrupt_ledger;
        }
        const transition_owner_claim claim{
            .owner = slot->owner,
            .claim_sequence = slot->claim_sequence,
        };
        if (!valid_owner(claim, header_value->config.owner_capacity) ||
            claim.owner.proc_number != slot_index) {
            return transition_ledger_error::corrupt_ledger;
        }
        std::uint32_t active_transitions = 0;
        for (std::uint32_t index = 0; index < slot->reference_capacity; ++index) {
            const transition_dependency_reference* reference = reference_at(ledger, *slot, index);
            if (reference == nullptr) {
                return transition_ledger_error::corrupt_layout;
            }
            if (reference->transition_active) {
                ++active_transitions;
                if (!reference->active) {
                    return transition_ledger_error::corrupt_ledger;
                }
            }
            if (reference->active &&
                !valid_reference_identity(*reference,
                                          header_value->config.dependency_partition_count)) {
                return transition_ledger_error::corrupt_ledger;
            }
        }
        if (active_transitions != load_outstanding(*slot)) {
            return transition_ledger_error::corrupt_ledger;
        }
        if ((state == transition_owner_state::collecting && active_transitions != 0) ||
            (state == transition_owner_state::active &&
             active_transitions != slot->transition_target_count)) {
            return transition_ledger_error::corrupt_ledger;
        }
    }
    return transition_ledger_error::none;
}

} // namespace bioseq::cache
