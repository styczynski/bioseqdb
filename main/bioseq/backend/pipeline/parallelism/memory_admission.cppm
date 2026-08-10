/**
 * @file memory_admission.cppm
 * @brief Backend-local memory reservations shared by dispatch, pipeline tasks,
 *        and index residency features.
 */
module;
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.parallelism.memory_admission;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;

export namespace bioseq::parallelism {

class memory_admission_controller;

using memory_footprint_probe = std::uint64_t (*)() noexcept;

/// Machine-readable reason returned when a non-blocking reservation is
/// refused.  Callers that do not need diagnostics may keep using
/// `try_reserve(bytes)`; strict accelerator paths pass an output record so a
/// fail-closed decline names the actual predicate instead of collapsing every
/// refusal into a generic "GPU path declined" error.
enum class memory_admission_failure_reason : std::uint8_t {
    none,
    system_pressure,
    reservation_counter_overflow,
    budget_exhausted,
};

/// Immutable snapshot explaining one rejected non-blocking reservation.
struct memory_admission_failure {
    // Every live-memory field below comes from one predicate snapshot captured
    // before the reservation CAS loop; diagnostics never resample the OS after
    // deciding which predicate rejected the request.
    memory_admission_failure_reason reason = memory_admission_failure_reason::none;
    std::uint64_t requested_bytes = 0;
    std::uint64_t reserved_bytes = 0;
    std::uint64_t budget_ceiling_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t allocatable_after_reservations_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t process_footprint_bytes = 0;
    std::uint64_t system_available_bytes = 0;
    std::uint64_t system_total_bytes = 0;
    std::uint64_t process_soft_cap_bytes = 0;

    /// Return whether this record describes a rejected reservation.
    [[nodiscard]] constexpr bool has_failure() const noexcept {
        return reason != memory_admission_failure_reason::none;
    }
};

/// Read-only controller and host-memory snapshot used by fail-closed callers
/// that need to explain an allocator or post-allocation reconciliation failure
/// after the admission predicate itself succeeded.  A rejected `try_reserve`
/// still returns the stronger single-predicate `memory_admission_failure` above.
struct memory_admission_snapshot {
    std::uint64_t reserved_bytes = 0;
    std::uint64_t budget_ceiling_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t allocatable_after_reservations_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    std::uint64_t process_footprint_bytes = 0;
    std::uint64_t system_available_bytes = 0;
    std::uint64_t system_total_bytes = 0;
    std::uint64_t process_soft_cap_bytes = 0;
};

[[nodiscard]] inline constexpr std::string_view
memory_admission_failure_reason_name(
    memory_admission_failure_reason reason) noexcept
{
    switch (reason) {
        case memory_admission_failure_reason::none:
            return "none";
        case memory_admission_failure_reason::system_pressure:
            return "system_pressure";
        case memory_admission_failure_reason::reservation_counter_overflow:
            return "reservation_counter_overflow";
        case memory_admission_failure_reason::budget_exhausted:
            return "budget_exhausted";
    }
    return "unknown";
}

inline constexpr std::uint64_t k_minimizer_index_peak_bytes_per_bp = 6;

[[nodiscard]] inline std::uint64_t minimizer_index_peak_bytes_for_bp(
        std::uint64_t base_pair_count) noexcept {
    return bioseq::common::saturating_mul(
        base_pair_count, k_minimizer_index_peak_bytes_per_bp);
}

/// Move-only ownership of bytes admitted by the process memory controller.
class memory_permit {
public:
    memory_permit() = default;
    memory_permit(memory_admission_controller* owner, std::uint64_t bytes) noexcept
        : owner_(owner), bytes_(bytes) {}
    memory_permit(memory_permit&& other) noexcept {
        move_from(other);
    }
    memory_permit& operator=(memory_permit&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }
    memory_permit(const memory_permit&) = delete;
    memory_permit& operator=(const memory_permit&) = delete;
    ~memory_permit() {
        release();
    }

    void release() noexcept;
    void shrink_to(std::uint64_t bytes) noexcept;
    [[nodiscard]] bool grow_to(
        std::uint64_t bytes,
        memory_admission_failure* failure = nullptr) noexcept;
    /// Consolidate two reservations owned by the same controller without
    /// changing the controller's global reserved-byte counter.  This is used
    /// after parallel workers hand independently admitted allocations to one
    /// aggregate lifetime owner.
    [[nodiscard]] bool absorb(memory_permit&& other) noexcept;

    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

private:
    void move_from(memory_permit& other) noexcept {
        owner_ = other.owner_;
        bytes_ = other.bytes_;
        other.owner_ = nullptr;
        other.bytes_ = 0;
    }

    memory_admission_controller* owner_ = nullptr;
    std::uint64_t bytes_ = 0;
};

/// Pre-allocation reservation multiplier for std::vector capacity ownership.
/// Callers reserve this headroom before `reserve()`, then verify actual summed
/// capacities fit and shrink the permit to exact retained capacity bytes.
inline constexpr std::uint64_t k_vector_preallocation_headroom_factor = 2u;

/**
 * Destroy/deallocate admitted vector capacity before returning its reservation.
 * The inner scope is intentional: `empty` owns the former allocation after
 * swap and is fully destroyed before `permit.release()` can make those bytes
 * available to another worker.
 */
template <class p_value_type>
void release_admitted_vector_capacity_before_permit(
        std::vector<p_value_type>& values,
        memory_permit& permit) noexcept {
    {
        std::vector<p_value_type> empty;
        values.swap(empty);
    }
    permit.release();
}

/** Same ordering contract for two buffers covered by one reservation. */
template <class p_first_value_type, class p_second_value_type>
void release_admitted_vector_capacities_before_permit(
        std::vector<p_first_value_type>& first,
        std::vector<p_second_value_type>& second,
        memory_permit& permit) noexcept {
    {
        std::vector<p_first_value_type> empty_first;
        std::vector<p_second_value_type> empty_second;
        first.swap(empty_first);
        second.swap(empty_second);
    }
    permit.release();
}

/// Allocation snapshot returned by the materialization callable. `actual_*` is the
/// Vulkan/allocator allocation size, not merely the logical buffer range.
struct memory_materialization_snapshot {
    std::uint64_t actual_allocation_bytes = 0;
    std::uint64_t fully_touched_bytes = 0;
    bool is_host_visible = false;
    bool is_host_coherent = false;
    bool is_process_footprint_reflected = false;
};

/// Result produced when a serialized allocation is committed. Device-local
/// allocations whose footprint is not reflected retain their permit for life.
struct memory_materialization_result {
    std::uint64_t footprint_before_bytes = 0;
    std::uint64_t footprint_after_bytes = 0;
    std::uint64_t reflected_delta_bytes = 0;
    std::uint64_t admitted_bytes = 0;
    memory_materialization_snapshot allocation_snapshot{};
    bool is_permit_released = false;
};

[[nodiscard]] inline bool footprint_delta_covers_admission(
        std::uint64_t before,
        std::uint64_t after,
        std::uint64_t admitted) noexcept {
    if (before == 0u || after == 0u || after < before || admitted == 0u) {
        return false;
    }
    return after - before >= admitted;
}

[[nodiscard]] inline std::uint64_t live_process_footprint_probe() noexcept {
    return bioseq::common::memory::default_budget().footprint_bytes();
}

/// One memory-admitted fan-out wave and its lifetime reservation.
struct budgeted_wave {
    unsigned fanout = 1;
    memory_permit permit;
    bool is_admitted = false;
};

/// Process-wide non-blocking memory reservation controller.
class memory_admission_controller {
public:
    static memory_admission_controller& instance() noexcept {
        static memory_admission_controller controller;
        return controller;
    }

    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept {
        return reserved_.load(std::memory_order_relaxed);
    }

    /// Non-escapable serialized materialization transaction.  The lock is held
    /// before the first footprint sample and through allocation, complete touch,
    /// and the second sample, so two controlled commits cannot claim one delta.
    template <class p_materialize_callable>
    [[nodiscard]] memory_materialization_result materialize(
            memory_permit& permit,
            p_materialize_callable&& materialize_callable,
            memory_footprint_probe probe = live_process_footprint_probe) {
        std::lock_guard<std::mutex> guard(materialization_mutex_);
        memory_materialization_result result{
            .footprint_before_bytes = probe != nullptr ? probe() : 0u,
        };
        result.allocation_snapshot =
            std::forward<p_materialize_callable>(materialize_callable)();
        result.admitted_bytes = permit.bytes();
        result.footprint_after_bytes = probe != nullptr ? probe() : 0u;
        if (result.footprint_after_bytes >= result.footprint_before_bytes) {
            result.reflected_delta_bytes =
                result.footprint_after_bytes - result.footprint_before_bytes;
        }
        const bool has_exact_owned_allocation =
            result.allocation_snapshot.actual_allocation_bytes
                == result.admitted_bytes
            && result.allocation_snapshot.fully_touched_bytes
                >= result.allocation_snapshot.actual_allocation_bytes;
        if (has_exact_owned_allocation
            && result.allocation_snapshot.is_host_visible
            && result.allocation_snapshot.is_host_coherent
            && result.allocation_snapshot.is_process_footprint_reflected
            && footprint_delta_covers_admission(
                result.footprint_before_bytes,
                result.footprint_after_bytes,
                result.admitted_bytes)) {
            permit.release();
            result.is_permit_released = true;
        }
        return result;
    }

    void set_index_memory_cap_bytes(std::uint64_t bytes) noexcept {
        index_memory_cap_bytes_.store(bytes, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t index_memory_cap_bytes() const noexcept {
        return index_memory_cap_bytes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t budget_ceiling() const noexcept {
        std::uint64_t budget =
            bioseq::common::memory::default_budget().allocatable_bytes();
        const std::uint64_t cap = index_memory_cap_bytes();
        if (cap == 0) {
            return budget;
        }
        if (budget == (std::numeric_limits<std::uint64_t>::max)()) {
            return cap;
        }
        return std::min(budget, cap);
    }

    [[nodiscard]] std::uint64_t allocatable_after_reservations() const noexcept {
        const std::uint64_t budget = budget_ceiling();
        if (budget == (std::numeric_limits<std::uint64_t>::max)()) {
            return budget;
        }
        const std::uint64_t reserved = reserved_bytes();
        return reserved < budget ? budget - reserved : 0;
    }

    /// Capture the memory values available to a diagnostic without reserving
    /// or releasing anything.  The OS probes are sampled once each; callers
    /// must prefer `memory_admission_failure` when a reservation was rejected,
    /// because that record is tied to the exact has_failure predicate snapshot.
    [[nodiscard]] memory_admission_snapshot diagnostic_snapshot() const noexcept {
        auto& live_budget = bioseq::common::memory::default_budget();
        memory_admission_snapshot snapshot{
            .reserved_bytes = reserved_bytes(),
            .process_footprint_bytes = live_budget.footprint_bytes(),
            .system_available_bytes = live_budget.available_bytes(),
            .system_total_bytes = live_budget.total_bytes(),
        };
        std::uint64_t allocatable =
            (std::numeric_limits<std::uint64_t>::max)();
        if (snapshot.system_total_bytes != 0u) {
            snapshot.process_soft_cap_bytes = static_cast<std::uint64_t>(
                static_cast<double>(snapshot.system_total_bytes)
                * live_budget.cap_fraction);
        }
        if (snapshot.system_total_bytes != 0u
            && snapshot.system_available_bytes != 0u) {
            const std::uint64_t by_cap =
                snapshot.process_footprint_bytes
                        < snapshot.process_soft_cap_bytes
                    ? snapshot.process_soft_cap_bytes
                        - snapshot.process_footprint_bytes
                    : 0u;
            const std::uint64_t by_free =
                snapshot.system_available_bytes > live_budget.min_free_bytes
                    ? snapshot.system_available_bytes
                        - live_budget.min_free_bytes
                    : 0u;
            allocatable = std::min(by_cap, by_free);
        }
        const std::uint64_t cap = index_memory_cap_bytes();
        snapshot.budget_ceiling_bytes = cap == 0u
            ? allocatable
            : allocatable == (std::numeric_limits<std::uint64_t>::max)()
                ? cap : std::min(allocatable, cap);
        snapshot.allocatable_after_reservations_bytes =
            snapshot.budget_ceiling_bytes
                    == (std::numeric_limits<std::uint64_t>::max)()
                ? snapshot.budget_ceiling_bytes
                : snapshot.reserved_bytes < snapshot.budget_ceiling_bytes
                    ? snapshot.budget_ceiling_bytes - snapshot.reserved_bytes
                    : 0u;
        return snapshot;
    }

    [[nodiscard]] std::optional<memory_permit> try_reserve(
            std::uint64_t bytes,
            memory_admission_failure* failure = nullptr) noexcept {
        if (failure != nullptr) {
            *failure = memory_admission_failure{
                .requested_bytes = bytes,
            };
        }
        if (bytes == 0) {
            return memory_permit{this, 0};
        }

        auto& live_budget = bioseq::common::memory::default_budget();
        struct budget_snapshot {
            std::uint64_t process_footprint_bytes = 0;
            std::uint64_t system_available_bytes = 0;
            std::uint64_t system_total_bytes = 0;
            std::uint64_t process_soft_cap_bytes = 0;
            std::uint64_t ceiling_bytes =
                (std::numeric_limits<std::uint64_t>::max)();
            bool is_under_pressure = false;
        };
        const budget_snapshot budget_snapshot = [&]() noexcept {
            budget_snapshot snapshot{
                .process_footprint_bytes = live_budget.footprint_bytes(),
                .system_available_bytes = live_budget.available_bytes(),
                .system_total_bytes = live_budget.total_bytes(),
            };
            if (snapshot.system_total_bytes != 0u) {
                snapshot.process_soft_cap_bytes = static_cast<std::uint64_t>(
                    static_cast<double>(snapshot.system_total_bytes)
                    * live_budget.cap_fraction);
            }
            std::uint64_t allocatable =
                (std::numeric_limits<std::uint64_t>::max)();
            if (snapshot.system_total_bytes != 0u
                && snapshot.system_available_bytes != 0u) {
                const std::uint64_t by_cap =
                    snapshot.process_footprint_bytes
                            < snapshot.process_soft_cap_bytes
                        ? snapshot.process_soft_cap_bytes
                            - snapshot.process_footprint_bytes
                        : 0u;
                const std::uint64_t by_free =
                    snapshot.system_available_bytes > live_budget.min_free_bytes
                        ? snapshot.system_available_bytes
                            - live_budget.min_free_bytes
                        : 0u;
                allocatable = std::min(by_cap, by_free);
                snapshot.is_under_pressure =
                    snapshot.process_footprint_bytes
                            > snapshot.process_soft_cap_bytes
                    || snapshot.system_available_bytes
                            < live_budget.min_free_bytes;
            }
            const std::uint64_t cap = index_memory_cap_bytes();
            snapshot.ceiling_bytes = cap == 0u
                ? allocatable
                : allocatable
                        == (std::numeric_limits<std::uint64_t>::max)()
                    ? cap
                    : std::min(allocatable, cap);
            return snapshot;
        }();
        const auto refuse = [failure, bytes, &budget_snapshot](
                memory_admission_failure_reason reason,
                std::uint64_t reserved) noexcept
                -> std::optional<memory_permit> {
            if (failure != nullptr) {
                const std::uint64_t ceiling = budget_snapshot.ceiling_bytes;
                const std::uint64_t after_reservations =
                    ceiling == (std::numeric_limits<std::uint64_t>::max)()
                        ? ceiling
                        : reserved < ceiling ? ceiling - reserved : 0u;
                *failure = memory_admission_failure{
                    .reason = reason,
                    .requested_bytes = bytes,
                    .reserved_bytes = reserved,
                    .budget_ceiling_bytes = ceiling,
                    .allocatable_after_reservations_bytes = after_reservations,
                    .process_footprint_bytes =
                        budget_snapshot.process_footprint_bytes,
                    .system_available_bytes =
                        budget_snapshot.system_available_bytes,
                    .system_total_bytes = budget_snapshot.system_total_bytes,
                    .process_soft_cap_bytes =
                        budget_snapshot.process_soft_cap_bytes,
                };
            }
            return std::nullopt;
        };

        if (budget_snapshot.is_under_pressure) {
            const std::uint64_t current =
                reserved_.load(std::memory_order_relaxed);
            return refuse(
                memory_admission_failure_reason::system_pressure,
                current);
        }

        std::uint64_t current = reserved_.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > (std::numeric_limits<std::uint64_t>::max)() - current) {
                return refuse(
                    memory_admission_failure_reason::reservation_counter_overflow,
                    current);
            }
            const std::uint64_t budget = budget_snapshot.ceiling_bytes;
            if (budget != (std::numeric_limits<std::uint64_t>::max)()) {
                if (current > budget || bytes > budget - current) {
                    return refuse(
                        memory_admission_failure_reason::budget_exhausted,
                        current);
                }
            }
            if (reserved_.compare_exchange_weak(
                    current, current + bytes,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return memory_permit{this, bytes};
            }
        }
    }

    [[nodiscard]] budgeted_wave try_reserve_wave(
            unsigned requested,
            std::uint64_t bytes_per_task) noexcept {
        requested = std::max(1u, requested);
        if (bytes_per_task == 0) {
            return budgeted_wave{requested, {}, true};
        }

        for (unsigned fanout = requested; fanout > 1; --fanout) {
            const std::uint64_t bytes = bioseq::common::saturating_mul(
                static_cast<std::uint64_t>(fanout), bytes_per_task);
            if (auto permit = try_reserve(bytes)) {
                return budgeted_wave{fanout, std::move(*permit), true};
            }
        }
        if (auto permit = try_reserve(bytes_per_task)) {
            return budgeted_wave{1, std::move(*permit), true};
        }
        return budgeted_wave{1, {}, false};
    }

    void release(std::uint64_t bytes) noexcept {
        reserved_.fetch_sub(bytes, std::memory_order_acq_rel);
    }

private:
    std::mutex materialization_mutex_;
    std::atomic<std::uint64_t> reserved_{0};
    std::atomic<std::uint64_t> index_memory_cap_bytes_{0};
};

inline void memory_permit::release() noexcept {
    if (owner_ != nullptr) {
        owner_->release(bytes_);
        owner_ = nullptr;
        bytes_ = 0;
    }
}

inline void memory_permit::shrink_to(std::uint64_t bytes) noexcept {
    if (owner_ == nullptr || bytes_ <= bytes) {
        return;
    }
    const std::uint64_t delta = bytes_ - bytes;
    bytes_ = bytes;
    owner_->release(delta);
}

inline bool memory_permit::grow_to(
        std::uint64_t bytes,
        memory_admission_failure* failure) noexcept {
    if (owner_ == nullptr) {
        return bytes == 0u;
    }
    if (bytes <= bytes_) {
        return true;
    }
    const std::uint64_t delta = bytes - bytes_;
    auto extra = owner_->try_reserve(delta, failure);
    if (!extra) {
        return false;
    }
    // Transfer the extra reservation into this permit without releasing it.
    extra->owner_ = nullptr;
    extra->bytes_ = 0u;
    bytes_ = bytes;
    return true;
}

inline bool memory_permit::absorb(memory_permit&& other) noexcept {
    if (this == &other) {
        return true;
    }
    if (other.owner_ == nullptr) {
        return other.bytes_ == 0u;
    }
    if (owner_ == nullptr || owner_ != other.owner_
        || bytes_ > (std::numeric_limits<std::uint64_t>::max)()
                - other.bytes_) {
        return false;
    }
    bytes_ += other.bytes_;
    other.owner_ = nullptr;
    other.bytes_ = 0u;
    return true;
}

[[nodiscard]] inline budgeted_wave try_reserve_budgeted_wave(
        unsigned requested,
        std::uint64_t peak_bytes_per_task) noexcept {
    return memory_admission_controller::instance().try_reserve_wave(
        requested, peak_bytes_per_task);
}

} // namespace bioseq::parallelism
