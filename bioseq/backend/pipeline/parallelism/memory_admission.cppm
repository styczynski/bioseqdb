/**
 * @file memory_admission.cppm
 * @brief Account for memory reserved for pipeline tasks and index buffers.
 */
module;
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.parallelism.memory_admission;
import bioseq.common.checked_arithmetic;
import bioseq.common.memory_budget;

namespace bioseq::parallelism::detail {

/** Host-memory measurements and the resulting allocation limit. */
struct captured_memory_budget {
    bioseq::common::memory::memory_snapshot system{};
    std::uint64_t process_soft_cap_bytes = 0;
    std::uint64_t ceiling_bytes = (std::numeric_limits<std::uint64_t>::max)();
    bool is_under_pressure = false;
};

/**
 * Sample host memory and apply the common limits and optional index cap.
 *
 * @param index_cap_bytes Extra byte limit, or zero to use only the common limits.
 * @return Measurements and limits used by one reservation attempt.
 */
[[nodiscard]] inline captured_memory_budget
capture_memory_budget(std::uint64_t index_cap_bytes) noexcept {
    auto& policy = bioseq::common::memory::default_budget();
    const bioseq::common::memory::memory_snapshot system = policy.snapshot();
    const std::uint64_t process_soft_cap = system.system_total_bytes == 0U
        ? 0U
        : static_cast<std::uint64_t>(
              static_cast<double>(system.system_total_bytes) * policy.cap_fraction);
    std::uint64_t allocatable = (std::numeric_limits<std::uint64_t>::max)();
    bool is_under_pressure = false;
    if (system.system_total_bytes != 0U && system.system_available_bytes != 0U) {
        const std::uint64_t by_cap = system.process_footprint_bytes < process_soft_cap
            ? process_soft_cap - system.process_footprint_bytes
            : 0U;
        const std::uint64_t by_free = system.system_available_bytes > policy.min_free_bytes
            ? system.system_available_bytes - policy.min_free_bytes
            : 0U;
        allocatable = std::min(by_cap, by_free);
        is_under_pressure = system.process_footprint_bytes > process_soft_cap
            || system.system_available_bytes < policy.min_free_bytes;
    }
    const std::uint64_t ceiling = index_cap_bytes == 0U ? allocatable
                                  : allocatable == (std::numeric_limits<std::uint64_t>::max)()
                                      ? index_cap_bytes
                                      : std::min(allocatable, index_cap_bytes);
    return captured_memory_budget{
        .system = system,
        .process_soft_cap_bytes = process_soft_cap,
        .ceiling_bytes = ceiling,
        .is_under_pressure = is_under_pressure,
    };
}

} // namespace bioseq::parallelism::detail

export namespace bioseq::parallelism {

class memory_admission_controller;

/// Nonthrowing callback that reports the process memory footprint in bytes.
using memory_footprint_probe = std::uint64_t (*)() noexcept;

/// Reason a memory reservation failed. Callers can request this detail from
/// try_reserve() to report memory pressure separately from other execution errors.
enum class memory_admission_failure_reason : std::uint8_t {
    /// No reservation failure was recorded.
    none,
    /// The process or available system memory crossed a configured limit.
    system_pressure,
    /// Adding the request would overflow the reservation counter.
    reservation_counter_overflow,
    /// The request exceeds the remaining memory limit.
    budget_exhausted,
};

/// Memory measurements and the reason for one rejected reservation.
struct memory_admission_failure {
    // Host measurements come from the sample used to decide this request.
    // Reporting the failure does not sample the OS again.
    /// Check that rejected the reservation.
    memory_admission_failure_reason reason = memory_admission_failure_reason::none;
    /// Bytes requested by the caller.
    std::uint64_t requested_bytes = 0;
    /// Outstanding reservations when the request was rejected.
    std::uint64_t reserved_bytes = 0;
    /// Allocation limit after applying the common limits and index cap.
    std::uint64_t budget_ceiling_bytes = (std::numeric_limits<std::uint64_t>::max)();
    /// Bytes left under that limit after outstanding reservations.
    std::uint64_t allocatable_after_reservations_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    /// Process memory footprint measured for this request.
    std::uint64_t process_footprint_bytes = 0;
    /// Available system memory measured for this request.
    std::uint64_t system_available_bytes = 0;
    /// Total system memory measured for this request.
    std::uint64_t system_total_bytes = 0;
    /// Process memory limit calculated from the sampled system total.
    std::uint64_t process_soft_cap_bytes = 0;

    /// Return whether this record describes a rejected reservation.
    [[nodiscard]] constexpr bool has_failure() const noexcept {
        return reason != memory_admission_failure_reason::none;
    }
};

/// Current memory measurements for reporting an allocation failure.
/// For a failed try_reserve(), use its memory_admission_failure instead so
/// the report contains the measurements that caused the rejection.
struct memory_admission_snapshot {
    /// Bytes currently reserved by this controller.
    std::uint64_t reserved_bytes = 0;
    /// Allocation limit after applying the common limits and index cap.
    std::uint64_t budget_ceiling_bytes = (std::numeric_limits<std::uint64_t>::max)();
    /// Bytes left under that limit after outstanding reservations.
    std::uint64_t allocatable_after_reservations_bytes =
        (std::numeric_limits<std::uint64_t>::max)();
    /// Process memory footprint from this sample.
    std::uint64_t process_footprint_bytes = 0;
    /// Available system memory from this sample.
    std::uint64_t system_available_bytes = 0;
    /// Total system memory from this sample.
    std::uint64_t system_total_bytes = 0;
    /// Process memory limit calculated from this sample.
    std::uint64_t process_soft_cap_bytes = 0;
};

/// Return the failure name used in diagnostics, or "unknown".
[[nodiscard]] inline constexpr std::string_view
memory_admission_failure_reason_name(memory_admission_failure_reason reason) noexcept {
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

/// Estimated peak bytes per target base when building a minimizer index.
inline constexpr std::uint64_t k_minimizer_index_peak_bytes_per_bp = 6;

/// Estimate index-building memory for a target, saturating on overflow.
[[nodiscard]] inline std::uint64_t
minimizer_index_peak_bytes_for_bp(std::uint64_t base_pair_count) noexcept {
    return bioseq::common::saturating_mul(base_pair_count, k_minimizer_index_peak_bytes_per_bp);
}

/// Own a memory reservation and return it when this permit is destroyed.
/// The caller owns the allocation covered by the reservation.
class memory_permit {
  public:
    /// Create an empty permit.
    memory_permit() = default;
    /// Adopt an existing controller reservation.
    memory_permit(memory_admission_controller* owner, std::uint64_t bytes) noexcept
        : owner_(owner), bytes_(bytes) {}
    /// Transfer reservation ownership from another permit.
    memory_permit(memory_permit&& other) noexcept {
        move_from(other);
    }
    /// Release this reservation, then take the other permit's reservation.
    memory_permit& operator=(memory_permit&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }
    memory_permit(const memory_permit&) = delete;
    memory_permit& operator=(const memory_permit&) = delete;
    /// Return the owned reservation to its controller.
    ~memory_permit() {
        release();
    }

    /// Return the entire reservation and make this permit empty.
    void release() noexcept;
    /// Reduce the reservation to this byte count. Larger requests do nothing.
    void shrink_to(std::uint64_t bytes) noexcept;
    /// Give part of this reservation its own permit without changing the total.
    /// Return an empty permit if this permit is empty or too small.
    [[nodiscard]] memory_permit split_off(std::uint64_t bytes) noexcept {
        if (owner_ == nullptr || bytes > bytes_) {
            return {};
        }
        bytes_ -= bytes;
        return memory_permit{owner_, bytes};
    }
    /// Try to enlarge this reservation, keeping it unchanged on failure.
    [[nodiscard]] bool grow_to(std::uint64_t bytes,
                               memory_admission_failure* failure = nullptr) noexcept;
    /// Take another permit's reservation when both use the same controller.
    /// This keeps the total reserved bytes unchanged. The other permit stays
    /// valid if the controllers differ or the combined size would overflow.
    [[nodiscard]] bool absorb(memory_permit&& other) noexcept;

    /// Return the number of bytes reserved by this permit.
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return bytes_;
    }

    /// Borrow the controller whose reservation this permit owns, or null if empty.
    [[nodiscard]] memory_admission_controller* controller() const noexcept {
        return owner_;
    }

    /// Return whether this permit owns a controller reservation.
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

/**
 * @brief Keep memory reservations until an operation's context is destroyed.
 *
 * Use this for backing blocks whose memory is not yet counted in the process
 * footprint. Buffers that can be replaced earlier need their own permits.
 * Allocations inside a reserved block share that block's reservation.
 */
class operation_memory_retention {
  public:
    /// Keep a block's existing permit, combining it with earlier reservations.
    [[nodiscard]] bool retain_context_block(memory_permit&& permit) noexcept {
        if (!permit) {
            return true;
        }
        if (!context_blocks_) {
            context_blocks_ = std::move(permit);
            return true;
        }
        // If combining permits fails, the caller still owns the incoming permit.
        return context_blocks_.absorb(std::move(permit));
    }

    /// Bytes reserved for the context's backing blocks.
    [[nodiscard]] std::uint64_t retained_bytes() const noexcept {
        return context_blocks_.bytes();
    }

  private:
    memory_permit context_blocks_;
};

/// Reserve extra room before growing a vector, since its capacity may exceed
/// the requested size. After allocation, check the actual capacity and reduce
/// the permit to the bytes that remain allocated.
inline constexpr std::uint64_t k_vector_preallocation_headroom_factor = 2u;

/**
 * Free a vector's allocation before releasing its memory reservation.
 * The temporary vector owns the old buffer after swap and is destroyed at the
 * end of the inner scope, before another worker can reserve those bytes.
 */
template <class p_value_type>
void release_admitted_vector_capacity_before_permit(std::vector<p_value_type>& values,
                                                    memory_permit& permit) noexcept {
    {
        std::vector<p_value_type> empty;
        values.swap(empty);
    }
    permit.release();
}

/** Free both vector allocations before releasing their shared reservation. */
template <class p_first_value_type, class p_second_value_type>
void release_admitted_vector_capacities_before_permit(std::vector<p_first_value_type>& first,
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

/// Allocation details returned by the buffer-creation callback.
/// Sizes include any space beyond the requested range.
struct memory_materialization_snapshot {
    /// Bytes owned by the completed allocation.
    std::uint64_t actual_allocation_bytes = 0;
    /// Allocation bytes touched while creating the buffer.
    std::uint64_t fully_touched_bytes = 0;
    /// Whether the allocation is visible to the host.
    bool is_host_visible = false;
    /// Whether host writes are coherent without an explicit flush.
    bool is_host_coherent = false;
    /// Whether the process-footprint probe accounts for this allocation.
    bool is_process_footprint_reflected = false;
};

/// Measurements taken around an allocation and its remaining reservation.
/// Allocations absent from the process footprint keep their permit.
struct memory_materialization_result {
    /// Process footprint before the allocation callback.
    std::uint64_t footprint_before_bytes = 0;
    /// Process footprint after the allocation callback.
    std::uint64_t footprint_after_bytes = 0;
    /// Non-negative difference between the two footprint samples.
    std::uint64_t reflected_delta_bytes = 0;
    /// Bytes held by the permit when allocation completed.
    std::uint64_t admitted_bytes = 0;
    /// Allocation details returned by the callback.
    memory_materialization_snapshot allocation_snapshot{};
    /// Whether the footprint increase allowed the reservation to be released.
    bool is_permit_released = false;
};

/// Check whether valid footprint samples account for the reserved bytes.
[[nodiscard]] inline bool footprint_delta_covers_admission(std::uint64_t before,
                                                           std::uint64_t after,
                                                           std::uint64_t admitted) noexcept {
    if (before == 0u || after == 0u || after < before || admitted == 0u) {
        return false;
    }
    return after - before >= admitted;
}

/// Read the process footprint through the shared memory policy.
[[nodiscard]] inline std::uint64_t live_process_footprint_probe() noexcept {
    return bioseq::common::memory::default_budget().footprint_bytes();
}

/// A group of work items admitted together and its memory reservation.
struct budgeted_wave {
    /// Number of work items to run together when admitted.
    unsigned fanout = 1;
    /// Reservation covering the wave's peak estimate.
    memory_permit permit;
    /// Whether the group may run, including work needing no reservation.
    bool is_admitted = false;
};

/// Account for outstanding memory reservations against sampled host limits.
class memory_admission_controller {
  public:
    /// Create a controller with its own reservation counter.
    memory_admission_controller() = default;

    /// Return the process-wide pipeline admission controller.
    static memory_admission_controller& instance() noexcept {
        static memory_admission_controller controller;
        return controller;
    }

    /// Return bytes reserved by live permits.
    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept {
        return reserved_.load(std::memory_order_relaxed);
    }

    /// Run an allocation callback while holding the controller's allocation lock.
    /// Measure the process footprint before and after it. Release the permit only
    /// when the allocation size and host-memory checks show that the reservation
    /// is already covered by the process footprint. The lock keeps two allocations
    /// through this controller from claiming the same measured increase.
    template <class p_materialize_callable>
    [[nodiscard]] memory_materialization_result
    materialize(memory_permit& permit,
                p_materialize_callable&& materialize_callable,
                memory_footprint_probe probe = live_process_footprint_probe) {
        if (permit.controller() != this) {
            throw std::invalid_argument("materialization requires this controller's live permit");
        }
        std::lock_guard<std::mutex> guard(materialization_mutex_);
        memory_materialization_result result{
            .footprint_before_bytes = probe != nullptr ? probe() : 0u,
        };
        result.allocation_snapshot = std::forward<p_materialize_callable>(materialize_callable)();
        result.admitted_bytes = permit.bytes();
        result.footprint_after_bytes = probe != nullptr ? probe() : 0u;
        if (result.footprint_after_bytes >= result.footprint_before_bytes) {
            result.reflected_delta_bytes =
                result.footprint_after_bytes - result.footprint_before_bytes;
        }
        const bool has_exact_owned_allocation =
            result.allocation_snapshot.actual_allocation_bytes == result.admitted_bytes &&
            result.allocation_snapshot.fully_touched_bytes >=
                result.allocation_snapshot.actual_allocation_bytes;
        if (has_exact_owned_allocation && result.allocation_snapshot.is_host_visible &&
            result.allocation_snapshot.is_host_coherent &&
            result.allocation_snapshot.is_process_footprint_reflected &&
            footprint_delta_covers_admission(result.footprint_before_bytes,
                                             result.footprint_after_bytes,
                                             result.admitted_bytes)) {
            permit.release();
            result.is_permit_released = true;
        }
        return result;
    }

    /// Set the additional index-memory limit. Zero disables this extra limit.
    void set_index_memory_cap_bytes(std::uint64_t bytes) noexcept {
        index_memory_cap_bytes_.store(bytes, std::memory_order_relaxed);
    }

    /// Return the configured index-memory limit, or zero when disabled.
    [[nodiscard]] std::uint64_t index_memory_cap_bytes() const noexcept {
        return index_memory_cap_bytes_.load(std::memory_order_relaxed);
    }

    /// Return the current allocation limit after applying the index cap.
    [[nodiscard]] std::uint64_t budget_ceiling() const noexcept {
        auto& policy = bioseq::common::memory::default_budget();
        const bioseq::common::memory::memory_snapshot system = policy.snapshot();
        const std::uint64_t allocatable = policy.allocatable_bytes(system);
        const std::uint64_t cap = index_memory_cap_bytes();
        if (cap == 0U) {
            return allocatable;
        }
        if (allocatable == (std::numeric_limits<std::uint64_t>::max)()) {
            return cap;
        }
        return std::min(allocatable, cap);
    }

    /// Return how many more bytes fit after outstanding reservations.
    [[nodiscard]] std::uint64_t allocatable_after_reservations() const noexcept {
        const std::uint64_t budget = budget_ceiling();
        if (budget == (std::numeric_limits<std::uint64_t>::max)()) {
            return budget;
        }
        const std::uint64_t reserved = reserved_bytes();
        return reserved < budget ? budget - reserved : 0;
    }

    /// Sample memory for a diagnostic without changing reservations.
    /// Use the failure record from try_reserve() when reporting a rejected
    /// request, since this method takes a fresh sample.
    [[nodiscard]] memory_admission_snapshot diagnostic_snapshot() const noexcept {
        const detail::captured_memory_budget captured_budget =
            detail::capture_memory_budget(index_memory_cap_bytes());
        memory_admission_snapshot snapshot{
            .reserved_bytes = reserved_bytes(),
            .budget_ceiling_bytes = captured_budget.ceiling_bytes,
            .process_footprint_bytes = captured_budget.system.process_footprint_bytes,
            .system_available_bytes = captured_budget.system.system_available_bytes,
            .system_total_bytes = captured_budget.system.system_total_bytes,
            .process_soft_cap_bytes = captured_budget.process_soft_cap_bytes,
        };
        snapshot.allocatable_after_reservations_bytes =
            snapshot.budget_ceiling_bytes == (std::numeric_limits<std::uint64_t>::max)()
                ? snapshot.budget_ceiling_bytes
            : snapshot.reserved_bytes < snapshot.budget_ceiling_bytes
                ? snapshot.budget_ceiling_bytes - snapshot.reserved_bytes
                : 0u;
        return snapshot;
    }

    /** Try to reserve bytes without waiting for memory or allocating storage. */
    [[nodiscard]] std::optional<memory_permit>
    try_reserve(std::uint64_t bytes, memory_admission_failure* failure = nullptr) noexcept {
        if (failure != nullptr) {
            *failure = memory_admission_failure{
                .requested_bytes = bytes,
            };
        }
        if (bytes == 0) {
            return memory_permit{this, 0};
        }

        const detail::captured_memory_budget captured_budget =
            detail::capture_memory_budget(index_memory_cap_bytes());
        const auto refuse = [failure, bytes, &captured_budget](
                                memory_admission_failure_reason reason,
                                std::uint64_t reserved) noexcept -> std::optional<memory_permit> {
            if (failure != nullptr) {
                const std::uint64_t ceiling = captured_budget.ceiling_bytes;
                const std::uint64_t after_reservations =
                    ceiling == (std::numeric_limits<std::uint64_t>::max)() ? ceiling
                    : reserved < ceiling                                   ? ceiling - reserved
                                                                           : 0u;
                *failure = memory_admission_failure{
                    .reason = reason,
                    .requested_bytes = bytes,
                    .reserved_bytes = reserved,
                    .budget_ceiling_bytes = ceiling,
                    .allocatable_after_reservations_bytes = after_reservations,
                    .process_footprint_bytes = captured_budget.system.process_footprint_bytes,
                    .system_available_bytes = captured_budget.system.system_available_bytes,
                    .system_total_bytes = captured_budget.system.system_total_bytes,
                    .process_soft_cap_bytes = captured_budget.process_soft_cap_bytes,
                };
            }
            return std::nullopt;
        };

        if (captured_budget.is_under_pressure) {
            const std::uint64_t current = reserved_.load(std::memory_order_relaxed);
            return refuse(memory_admission_failure_reason::system_pressure, current);
        }

        std::uint64_t current = reserved_.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > (std::numeric_limits<std::uint64_t>::max)() - current) {
                return refuse(memory_admission_failure_reason::reservation_counter_overflow,
                              current);
            }
            const std::uint64_t budget = captured_budget.ceiling_bytes;
            if (budget != (std::numeric_limits<std::uint64_t>::max)()) {
                if (current > budget || bytes > budget - current) {
                    return refuse(memory_admission_failure_reason::budget_exhausted, current);
                }
            }
            if (reserved_.compare_exchange_weak(current,
                                                current + bytes,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed)) {
                return memory_permit{this, bytes};
            }
        }
    }

    /** Try smaller groups until their estimated memory fits, down to one item. */
    [[nodiscard]] budgeted_wave try_reserve_wave(unsigned requested,
                                                 std::uint64_t bytes_per_task) noexcept {
        requested = std::max(1u, requested);
        if (bytes_per_task == 0) {
            return budgeted_wave{requested, {}, true};
        }

        for (unsigned fanout = requested; fanout > 1; --fanout) {
            const std::uint64_t bytes =
                bioseq::common::saturating_mul(static_cast<std::uint64_t>(fanout), bytes_per_task);
            if (auto permit = try_reserve(bytes)) {
                return budgeted_wave{fanout, std::move(*permit), true};
            }
        }
        if (auto permit = try_reserve(bytes_per_task)) {
            return budgeted_wave{1, std::move(*permit), true};
        }
        return budgeted_wave{1, {}, false};
    }

    /// Subtract released bytes from this controller's reservation total.
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

inline bool memory_permit::grow_to(std::uint64_t bytes,
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
    if (owner_ == nullptr || owner_ != other.owner_ ||
        bytes_ > (std::numeric_limits<std::uint64_t>::max)() - other.bytes_) {
        return false;
    }
    bytes_ += other.bytes_;
    other.owner_ = nullptr;
    other.bytes_ = 0u;
    return true;
}

/// Try to reserve a work group through the shared process controller.
[[nodiscard]] inline budgeted_wave
try_reserve_budgeted_wave(unsigned requested, std::uint64_t peak_bytes_per_task) noexcept {
    return memory_admission_controller::instance().try_reserve_wave(requested, peak_bytes_per_task);
}

} // namespace bioseq::parallelism
