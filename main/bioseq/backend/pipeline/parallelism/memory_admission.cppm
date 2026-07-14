/**
 * @file memory_admission.cppm
 * @brief Backend-local memory reservations shared by dispatch, pipeline tasks,
 *        and index residency features.
 */
module;
// DOC0372 §413-424 — the controller lives in the single-owner
// bioseq_memory_admission_core leaf and imports only the sysinfo partition it
// uses (bioseq::memory::default_budget), not the bioseq.instr umbrella, so the
// leaf carries no edge back into the broad non-PG target. common.h was unused.
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
import bioseq.instr.detail.sysinfo;

export namespace bioseq::parallelism {

class MemoryAdmissionController;

using MemoryFootprintProbe = std::uint64_t (*)() noexcept;

/// Machine-readable reason returned when a non-blocking reservation is
/// refused.  Callers that do not need diagnostics may keep using
/// `try_reserve(bytes)`; strict accelerator paths pass an output record so a
/// fail-closed decline names the actual predicate instead of collapsing every
/// refusal into a generic "GPU path declined" error.
enum class MemoryAdmissionFailureReason : std::uint8_t {
    none,
    system_pressure,
    reservation_counter_overflow,
    budget_exhausted,
};

struct MemoryAdmissionFailure {
    // Every live-memory field below comes from one predicate snapshot captured
    // before the reservation CAS loop; diagnostics never resample the OS after
    // deciding which predicate rejected the request.
    MemoryAdmissionFailureReason reason = MemoryAdmissionFailureReason::none;
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

    [[nodiscard]] constexpr bool failed() const noexcept {
        return reason != MemoryAdmissionFailureReason::none;
    }
};

[[nodiscard]] inline constexpr std::string_view
memory_admission_failure_reason_name(
    MemoryAdmissionFailureReason reason) noexcept
{
    switch (reason) {
        case MemoryAdmissionFailureReason::none:
            return "none";
        case MemoryAdmissionFailureReason::system_pressure:
            return "system_pressure";
        case MemoryAdmissionFailureReason::reservation_counter_overflow:
            return "reservation_counter_overflow";
        case MemoryAdmissionFailureReason::budget_exhausted:
            return "budget_exhausted";
    }
    return "unknown";
}

inline constexpr std::uint64_t kMinimizerIndexPeakBytesPerBp = 6;

[[nodiscard]] inline std::uint64_t saturating_mul(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    if (lhs == 0 || rhs == 0) {
        return 0;
    }
    constexpr std::uint64_t max_value = (std::numeric_limits<std::uint64_t>::max)();
    if (lhs > max_value / rhs) {
        return max_value;
    }
    return lhs * rhs;
}

/// Saturating 64-bit add. Clamps at UINT64_MAX instead of wrapping so byte/size
/// estimators (e.g. `PhaseCMemoryEstimate::total`, DOC0310 §22) never overflow
/// into a small admitted value. The single shared definition lives here beside
/// `saturating_mul`; do not add a local copy.
[[nodiscard]] inline std::uint64_t saturating_add(std::uint64_t lhs, std::uint64_t rhs) noexcept {
    constexpr std::uint64_t max_value = (std::numeric_limits<std::uint64_t>::max)();
    if (lhs > max_value - rhs) {
        return max_value;
    }
    return lhs + rhs;
}

[[nodiscard]] inline std::uint64_t minimizer_index_peak_bytes_for_bp(std::uint64_t bp) noexcept {
    return saturating_mul(bp, kMinimizerIndexPeakBytesPerBp);
}

class MemoryPermit {
public:
    MemoryPermit() = default;
    MemoryPermit(MemoryAdmissionController* owner, std::uint64_t bytes) noexcept
        : owner_(owner), bytes_(bytes) {}
    MemoryPermit(MemoryPermit&& other) noexcept {
        move_from(other);
    }
    MemoryPermit& operator=(MemoryPermit&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }
    MemoryPermit(const MemoryPermit&) = delete;
    MemoryPermit& operator=(const MemoryPermit&) = delete;
    ~MemoryPermit() {
        release();
    }

    void release() noexcept;
    void shrink_to(std::uint64_t bytes) noexcept;
    [[nodiscard]] bool grow_to(
        std::uint64_t bytes,
        MemoryAdmissionFailure* failure = nullptr) noexcept;

    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return bytes_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return owner_ != nullptr;
    }

private:
    void move_from(MemoryPermit& other) noexcept {
        owner_ = other.owner_;
        bytes_ = other.bytes_;
        other.owner_ = nullptr;
        other.bytes_ = 0;
    }

    MemoryAdmissionController* owner_ = nullptr;
    std::uint64_t bytes_ = 0;
};

/// Pre-allocation reservation multiplier for std::vector capacity ownership.
/// Callers reserve this headroom before `reserve()`, then verify actual summed
/// capacities fit and shrink the permit to exact retained capacity bytes.
inline constexpr std::uint64_t kVectorPreallocationHeadroomFactor = 2u;

/**
 * Destroy/deallocate admitted vector capacity before returning its reservation.
 * The inner scope is intentional: `empty` owns the former allocation after
 * swap and is fully destroyed before `permit.release()` can make those bytes
 * available to another worker.
 */
template <class T>
void release_admitted_vector_capacity_before_permit(
        std::vector<T>& values,
        MemoryPermit& permit) noexcept {
    {
        std::vector<T> empty;
        values.swap(empty);
    }
    permit.release();
}

/** Same ordering contract for two buffers covered by one reservation. */
template <class First, class Second>
void release_admitted_vector_capacities_before_permit(
        std::vector<First>& first,
        std::vector<Second>& second,
        MemoryPermit& permit) noexcept {
    {
        std::vector<First> empty_first;
        std::vector<Second> empty_second;
        first.swap(empty_first);
        second.swap(empty_second);
    }
    permit.release();
}

/// Typed evidence returned by the materialization callable.  `actual_*` is the
/// Vulkan/allocator allocation size, not merely the logical buffer range.
struct MemoryMaterializationEvidence {
    std::uint64_t actual_allocation_bytes = 0;
    std::uint64_t fully_touched_bytes = 0;
    bool host_visible = false;
    bool host_coherent = false;
    bool process_footprint_reflected = false;
};

/// Evidence produced when a serialized allocation is committed.  Device-local
/// or otherwise unproven allocations retain their permit for their lifetime.
struct MemoryMaterializationCommit {
    std::uint64_t footprint_before_bytes = 0;
    std::uint64_t footprint_after_bytes = 0;
    std::uint64_t reflected_delta_bytes = 0;
    std::uint64_t admitted_bytes = 0;
    MemoryMaterializationEvidence evidence{};
    bool permit_released = false;
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
    return bioseq::memory::default_budget().footprint_bytes();
}

struct BudgetedWave {
    unsigned fanout = 1;
    MemoryPermit permit;
    bool admitted = false;
};

class MemoryAdmissionController {
public:
    static MemoryAdmissionController& instance() noexcept {
        static MemoryAdmissionController controller;
        return controller;
    }

    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept {
        return reserved_.load(std::memory_order_relaxed);
    }

    /// Non-escapable serialized materialization transaction.  The lock is held
    /// before the first footprint sample and through allocation, complete touch,
    /// and the second sample, so two controlled commits cannot claim one delta.
    template <class MaterializeFn>
    [[nodiscard]] MemoryMaterializationCommit materialize(
            MemoryPermit& permit,
            MaterializeFn&& materialize_fn,
            MemoryFootprintProbe probe = live_process_footprint_probe) {
        std::lock_guard<std::mutex> guard(materialization_mutex_);
        MemoryMaterializationCommit result{
            .footprint_before_bytes = probe != nullptr ? probe() : 0u,
        };
        result.evidence = std::forward<MaterializeFn>(materialize_fn)();
        result.admitted_bytes = permit.bytes();
        result.footprint_after_bytes = probe != nullptr ? probe() : 0u;
        if (result.footprint_after_bytes >= result.footprint_before_bytes) {
            result.reflected_delta_bytes =
                result.footprint_after_bytes - result.footprint_before_bytes;
        }
        const bool exact_owned_allocation =
            result.evidence.actual_allocation_bytes == result.admitted_bytes
            && result.evidence.fully_touched_bytes
                >= result.evidence.actual_allocation_bytes;
        if (exact_owned_allocation
            && result.evidence.host_visible
            && result.evidence.host_coherent
            && result.evidence.process_footprint_reflected
            && footprint_delta_covers_admission(
                result.footprint_before_bytes,
                result.footprint_after_bytes,
                result.admitted_bytes)) {
            permit.release();
            result.permit_released = true;
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
        std::uint64_t budget = bioseq::memory::default_budget().allocatable_bytes();
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

    [[nodiscard]] std::optional<MemoryPermit> try_reserve(
            std::uint64_t bytes,
            MemoryAdmissionFailure* failure = nullptr) noexcept {
        if (failure != nullptr) {
            *failure = MemoryAdmissionFailure{
                .requested_bytes = bytes,
            };
        }
        if (bytes == 0) {
            return MemoryPermit{this, 0};
        }

        auto& live_budget = bioseq::memory::default_budget();
        struct BudgetSnapshot {
            std::uint64_t process_footprint_bytes = 0;
            std::uint64_t system_available_bytes = 0;
            std::uint64_t system_total_bytes = 0;
            std::uint64_t process_soft_cap_bytes = 0;
            std::uint64_t ceiling_bytes =
                (std::numeric_limits<std::uint64_t>::max)();
            bool under_pressure = false;
        };
        const BudgetSnapshot budget_snapshot = [&]() noexcept {
            BudgetSnapshot snapshot{
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
                snapshot.under_pressure =
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
                MemoryAdmissionFailureReason reason,
                std::uint64_t reserved) noexcept
                -> std::optional<MemoryPermit> {
            if (failure != nullptr) {
                const std::uint64_t ceiling = budget_snapshot.ceiling_bytes;
                const std::uint64_t after_reservations =
                    ceiling == (std::numeric_limits<std::uint64_t>::max)()
                        ? ceiling
                        : reserved < ceiling ? ceiling - reserved : 0u;
                *failure = MemoryAdmissionFailure{
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

        if (budget_snapshot.under_pressure) {
            const std::uint64_t current =
                reserved_.load(std::memory_order_relaxed);
            return refuse(
                MemoryAdmissionFailureReason::system_pressure,
                current);
        }

        std::uint64_t current = reserved_.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > (std::numeric_limits<std::uint64_t>::max)() - current) {
                return refuse(
                    MemoryAdmissionFailureReason::reservation_counter_overflow,
                    current);
            }
            const std::uint64_t budget = budget_snapshot.ceiling_bytes;
            if (budget != (std::numeric_limits<std::uint64_t>::max)()) {
                if (current > budget || bytes > budget - current) {
                    return refuse(
                        MemoryAdmissionFailureReason::budget_exhausted,
                        current);
                }
            }
            if (reserved_.compare_exchange_weak(
                    current, current + bytes,
                    std::memory_order_acq_rel,
                    std::memory_order_relaxed)) {
                return MemoryPermit{this, bytes};
            }
        }
    }

    [[nodiscard]] BudgetedWave try_reserve_wave(
            unsigned requested,
            std::uint64_t bytes_per_task) noexcept {
        requested = std::max(1u, requested);
        if (bytes_per_task == 0) {
            return BudgetedWave{requested, {}, true};
        }

        for (unsigned fanout = requested; fanout > 1; --fanout) {
            const std::uint64_t bytes = saturating_mul(
                static_cast<std::uint64_t>(fanout), bytes_per_task);
            if (auto permit = try_reserve(bytes)) {
                return BudgetedWave{fanout, std::move(*permit), true};
            }
        }
        if (auto permit = try_reserve(bytes_per_task)) {
            return BudgetedWave{1, std::move(*permit), true};
        }
        return BudgetedWave{1, {}, false};
    }

    void release(std::uint64_t bytes) noexcept {
        reserved_.fetch_sub(bytes, std::memory_order_acq_rel);
    }

private:
    std::mutex materialization_mutex_;
    std::atomic<std::uint64_t> reserved_{0};
    std::atomic<std::uint64_t> index_memory_cap_bytes_{0};
};

inline void MemoryPermit::release() noexcept {
    if (owner_ != nullptr) {
        owner_->release(bytes_);
        owner_ = nullptr;
        bytes_ = 0;
    }
}

inline void MemoryPermit::shrink_to(std::uint64_t bytes) noexcept {
    if (owner_ == nullptr || bytes_ <= bytes) {
        return;
    }
    const std::uint64_t delta = bytes_ - bytes;
    bytes_ = bytes;
    owner_->release(delta);
}

inline bool MemoryPermit::grow_to(
        std::uint64_t bytes,
        MemoryAdmissionFailure* failure) noexcept {
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

[[nodiscard]] inline BudgetedWave try_reserve_budgeted_wave(
        unsigned requested,
        std::uint64_t peak_bytes_per_task) noexcept {
    return MemoryAdmissionController::instance().try_reserve_wave(
        requested, peak_bytes_per_task);
}

} // namespace bioseq::parallelism
