/**
 * @file bioseq_caching/core/admission.cppm
 * @brief Reserves cache space within memory and filesystem limits.
 */
module;

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

export module bioseq.cache.core:admission;
import :checked_math;
import :wire;

export namespace bioseq::cache {

enum class admission_error : std::uint8_t {
    counter_overflow,
    budget_exhausted,
    system_pressure,
    invalid_reservation,
};

/**
 * The DSA budget uses 4-KiB accounting pages. It rounds the total payload
 * up to a page and adds one page per allocation for allocator overhead
 */
inline constexpr std::uint64_t k_dsa_accounting_page_bytes = 4ULL << 10U;

/** Calculates a DSA reservation with room for page rounding and allocator overhead. */
[[nodiscard]] inline std::expected<std::uint64_t, admission_error>
conservative_dsa_reservation_bytes(std::uint64_t payload_bytes,
                                   std::uint64_t allocation_count = 1) noexcept {
    if (payload_bytes == 0 || allocation_count == 0) {
        return std::unexpected(admission_error::invalid_reservation);
    }
    const auto payload_with_rounding = checked_add(payload_bytes, k_dsa_accounting_page_bytes - 1U);
    const auto metadata_bytes = checked_mul(allocation_count, k_dsa_accounting_page_bytes);
    if (!payload_with_rounding.has_value() || !metadata_bytes.has_value()) {
        return std::unexpected(admission_error::counter_overflow);
    }
    const std::uint64_t rounded_payload =
        (*payload_with_rounding / k_dsa_accounting_page_bytes) * k_dsa_accounting_page_bytes;
    const auto reservation = checked_add(rounded_payload, *metadata_bytes);
    if (!reservation.has_value()) {
        return std::unexpected(admission_error::counter_overflow);
    }
    return *reservation;
}

/**
 * Accounts for  the initial DSA arena and its fixed allocations. The
 * arena already contains these allocations, so, use the larger of the
 * arena size and their estimated space to avoid counting them twice.
 */
[[nodiscard]] inline std::expected<std::uint64_t, admission_error>
conservative_fixed_dsa_reservation_bytes(
    std::uint64_t allocator_arena_bytes,
    std::span<const std::uint64_t> allocation_payload_bytes) noexcept {
    if (allocator_arena_bytes == 0 || allocation_payload_bytes.empty()) {
        return std::unexpected(admission_error::invalid_reservation);
    }
    std::uint64_t allocation_bytes = 0;
    for (const std::uint64_t payload_bytes : allocation_payload_bytes) {
        const auto reservation = conservative_dsa_reservation_bytes(payload_bytes);
        if (!reservation.has_value()) {
            return std::unexpected(reservation.error());
        }
        const auto next = checked_add(allocation_bytes, *reservation);
        if (!next.has_value()) {
            return std::unexpected(admission_error::counter_overflow);
        }
        allocation_bytes = *next;
    }
    return std::max(allocator_arena_bytes, allocation_bytes);
}

template <class p_counter_type>
concept cache_reservation_counter = requires(
    p_counter_type counter, std::uint64_t expected, std::uint64_t desired, std::uint64_t bytes) {
    { counter.load() } -> std::same_as<std::uint64_t>;
    { counter.compare_exchange(expected, desired) } -> std::same_as<bool>;
    { counter.subtract(bytes) } -> std::same_as<void>;
};

template <class p_budget_type>
concept cache_memory_budget = requires(const p_budget_type budget) {
    { budget.allocatable_bytes() } -> std::same_as<std::uint64_t>;
    { budget.under_pressure() } -> std::same_as<bool>;
};

template <cache_reservation_counter p_counter_type, cache_memory_budget p_budget_type>
class basic_cache_admission;

/** Owns a cache-space reservation and returns it when destroyed. Ownership can be moved. */
template <cache_reservation_counter p_counter_type, cache_memory_budget p_budget_type>
class cache_permit {
  public:
    cache_permit() = default;
    cache_permit(basic_cache_admission<p_counter_type, p_budget_type>* admission,
                 std::uint64_t bytes) noexcept
        : admission_(admission), bytes_(bytes) {}

    cache_permit(cache_permit&& other) noexcept {
        move_from(other);
    }

    cache_permit& operator=(cache_permit&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }

    cache_permit(const cache_permit&) = delete;
    cache_permit& operator=(const cache_permit&) = delete;

    ~cache_permit() noexcept {
        release();
    }

    /** Returns the number of bytes held by this permit. */
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return bytes_;
    }

    /** Reports whether this permit is attached to a reservation counter, including a zero-byte permit. */
    [[nodiscard]] explicit operator bool() const noexcept {
        return admission_ != nullptr;
    }

    /** Releases excess bytes when the requested size is smaller. */
    void shrink_to(std::uint64_t bytes) noexcept;

    /** Resizes the reservation, checking the budget before adding bytes. */
    [[nodiscard]] bool grow_to(std::uint64_t bytes) noexcept;

    /** Returns the reserved bytes and detaches the  permit. Repeated calls do nothing. */
    void release() noexcept;

    /** Detaches the permit and returns its byte count. The caller must arrange to release those bytes. */
    [[nodiscard]] std::uint64_t disown() noexcept {
        admission_ = nullptr;
        return std::exchange(bytes_, 0);
    }

  private:
    /** Takes the other permit's reservation and clears that permit. */
    void move_from(cache_permit& other) noexcept {
        admission_ = std::exchange(other.admission_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }

    basic_cache_admission<p_counter_type, p_budget_type>* admission_ = nullptr;
    std::uint64_t bytes_ = 0;
};

/** Limits cache reservations using a shared byte counter and a memory budget. */
template <cache_reservation_counter p_counter_type, cache_memory_budget p_budget_type>
class basic_cache_admission {
  public:
    using permit_type = cache_permit<p_counter_type, p_budget_type>;

    basic_cache_admission(p_counter_type& counter, const p_budget_type& budget) noexcept
        : counter_(counter), budget_(budget) {}

    /** Reserves bytes if the memoqy budget allows them and the system is not under pressure. */
    [[nodiscard]] std::expected<permit_type, admission_error>
    try_reserve(std::uint64_t bytes) noexcept {
        if (budget_.under_pressure()) {
            return std::unexpected(admission_error::system_pressure);
        }
        if (bytes == 0) {
            return permit_type{this, 0};
        }
        const std::uint64_t ceiling = budget_ceiling();
        std::uint64_t observed = counter_.load();
        for (;;) {
            const auto desired = checked_add(observed, bytes);
            if (!desired.has_value()) {
                return std::unexpected(admission_error::counter_overflow);
            }
            if (ceiling != (std::numeric_limits<std::uint64_t>::max)() && *desired > ceiling) {
                return std::unexpected(admission_error::budget_exhausted);
            }
            if (counter_.compare_exchange(observed, *desired)) {
                return permit_type{this, bytes};
            }
        }
    }

    /** Returns the total bytes held in the shared reservation counter. */
    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept {
        return counter_.load();
    }

    /** Reports memory pressure from the supplied budget. */
    [[nodiscard]] bool under_pressure() const noexcept {
        return budget_.under_pressure();
    }

  private:
    friend class cache_permit<p_counter_type, p_budget_type>;

    /** Caps the cache at a quarter of total memory when that size is known, and at  the available budget */
    [[nodiscard]] std::uint64_t budget_ceiling() const noexcept {
        const std::uint64_t allocatable = budget_.allocatable_bytes();
        if constexpr (requires { budget_.total_bytes(); }) {
            const std::uint64_t total = budget_.total_bytes();
            if (total == 0) {
                return allocatable;
            }
            const std::uint64_t cache_share =
                total / k_cache_memory_fraction_denominator * k_cache_memory_fraction_numerator;
            return allocatable == (std::numeric_limits<std::uint64_t>::max)()
                       ? cache_share
                       : std::min(allocatable, cache_share);
        }
        return allocatable;
    }

    /** Returns released bytes to the shared counter. */
    void release_bytes(std::uint64_t bytes) noexcept {
        if (bytes != 0) {
            counter_.subtract(bytes);
        }
    }

    p_counter_type& counter_;
    const p_budget_type& budget_;
};

template <cache_reservation_counter p_counter_type, cache_memory_budget p_budget_type>
void cache_permit<p_counter_type, p_budget_type>::shrink_to(std::uint64_t bytes) noexcept {
    if (admission_ == nullptr || bytes >= bytes_) {
        return;
    }
    admission_->release_bytes(bytes_ - bytes);
    bytes_ = bytes;
}

template <cache_reservation_counter p_counter_type, cache_memory_budget p_budget_type>
bool cache_permit<p_counter_type, p_budget_type>::grow_to(std::uint64_t bytes) noexcept {
    if (admission_ == nullptr) {
        return false;
    }
    if (bytes <= bytes_) {
        shrink_to(bytes);
        return true;
    }
    auto extra = admission_->try_reserve(bytes - bytes_);
    if (!extra.has_value()) {
        return false;
    }
    bytes_ = bytes;
    extra->admission_ = nullptr;
    extra->bytes_ = 0;
    return true;
}

template <cache_reservation_counter p_counter_type, cache_memory_budget p_budget_type>
void cache_permit<p_counter_type, p_budget_type>::release() noexcept {
    if (admission_ == nullptr) {
        return;
    }
    admission_->release_bytes(bytes_);
    admission_ = nullptr;
    bytes_ = 0;
}

/** Checks cache-file limits and the free space that must remain on disk. */
struct file_admission_policy {
    /** Return the smaller of 128 GiB and ten percent of filesystem capacity. */
    [[nodiscard]] static constexpr std::uint64_t
    ceiling_bytes(std::uint64_t filesystem_capacity) noexcept {
        return std::min(k_maximum_file_cache_bytes, filesystem_capacity / 10u);
    }

    /** Returns the free-space minimum: the larger of 10 GiB and ten percent of filesystem capacity. */
    [[nodiscard]] static constexpr std::uint64_t
    free_floor_bytes(std::uint64_t filesystem_capacity) noexcept {
        return std::max(k_minimum_file_free_bytes, filesystem_capacity / 10u);
    }

    /**
     * Checks whether a reservation fits the cache limit and leaves the
     * requested amount of disk space free. Tests can supply a smaller
     * free-space minimum while keeping the capacity and availability checks.
     */
    [[nodiscard]] static constexpr bool
    can_reserve_with_free_floor(std::uint64_t reserved_file_bytes,
                                std::uint64_t requested_bytes,
                                std::uint64_t filesystem_capacity,
                                std::uint64_t filesystem_available,
                                std::uint64_t free_floor) noexcept {
        const auto desired = checked_add(reserved_file_bytes, requested_bytes);
        return desired.has_value() && *desired <= ceiling_bytes(filesystem_capacity) &&
               filesystem_available >= requested_bytes &&
               filesystem_available - requested_bytes >= free_floor;
    }

    /** Checks a file reservation using the,  normal cache limit and free-space minimum. */
    [[nodiscard]] static constexpr bool can_reserve(std::uint64_t reserved_file_bytes,
                                                    std::uint64_t requested_bytes,
                                                    std::uint64_t filesystem_capacity,
                                                    std::uint64_t filesystem_available) noexcept {
        return can_reserve_with_free_floor(reserved_file_bytes,
                                           requested_bytes,
                                           filesystem_capacity,
                                           filesystem_available,
                                           free_floor_bytes(filesystem_capacity));
    }
};

} // namespace bioseq::cache
