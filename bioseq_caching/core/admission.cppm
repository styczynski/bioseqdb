/**
 * @file bioseq_caching/core/admission.cppm
 * @brief Shared-counter memory and file admission arithmetic with permits.
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
 * PostgreSQL 17 DSA obtains storage in 4-KiB pages and keeps allocator
 * metadata outside the caller's payload.  Admission deliberately rounds each
 * allocation to a page and reserves one additional metadata page.  This is a
 * conservative accounting contract, not an estimate of returned payload.
 */
inline constexpr std::uint64_t k_dsa_accounting_page_bytes = 4ULL << 10U;

/** Return a checked conservative reservation for one or more DSA allocations. */
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
 * Account a bootstrap arena and its fixed allocations exactly once.  The
 * allocator arena already contains the payloads, so the admission baseline is
 * the larger of the arena reservation and the sum of conservative allocation
 * reservations, never their sum.
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

/** Move-only ownership of bytes held against a shared reservation counter. */
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

    /** Return the bytes currently owned by this permit. */
    [[nodiscard]] std::uint64_t bytes() const noexcept {
        return bytes_;
    }

    /** Return whether this permit remains attached to an admission owner. */
    [[nodiscard]] explicit operator bool() const noexcept {
        return admission_ != nullptr;
    }

    /** Return excess bytes to the counter while retaining the smaller permit. */
    void shrink_to(std::uint64_t bytes) noexcept;

    /** Acquire the delta needed to grow this permit. */
    [[nodiscard]] bool grow_to(std::uint64_t bytes) noexcept;

    /** Release all currently owned bytes exactly once. */
    void release() noexcept;

    /** Transfer an admitted reservation into a separately tracked owner. */
    [[nodiscard]] std::uint64_t disown() noexcept {
        admission_ = nullptr;
        return std::exchange(bytes_, 0);
    }

  private:
    /** Transfer ownership and clear the source permit. */
    void move_from(cache_permit& other) noexcept {
        admission_ = std::exchange(other.admission_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }

    basic_cache_admission<p_counter_type, p_budget_type>* admission_ = nullptr;
    std::uint64_t bytes_ = 0;
};

/** Admission controller sharing one atomic-like counter across all producers. */
template <cache_reservation_counter p_counter_type, cache_memory_budget p_budget_type>
class basic_cache_admission {
  public:
    using permit_type = cache_permit<p_counter_type, p_budget_type>;

    basic_cache_admission(p_counter_type& counter, const p_budget_type& budget) noexcept
        : counter_(counter), budget_(budget) {}

    /** Reserve bytes without overcommitting the current shared ceiling. */
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

    /** Return the currently shared reserved-byte count. */
    [[nodiscard]] std::uint64_t reserved_bytes() const noexcept {
        return counter_.load();
    }

    /** Return pressure from the supplied live budget policy. */
    [[nodiscard]] bool under_pressure() const noexcept {
        return budget_.under_pressure();
    }

  private:
    friend class cache_permit<p_counter_type, p_budget_type>;

    /** Apply the cache's 25-percent physical-memory ceiling when available. */
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

    /** Return bytes after a permit shrinks/releases. */
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

/** File-capacity/free-space admission policy. */
struct file_admission_policy {
    /** Return the smaller of 128 GiB and ten percent of filesystem capacity. */
    [[nodiscard]] static constexpr std::uint64_t
    ceiling_bytes(std::uint64_t filesystem_capacity) noexcept {
        return std::min(k_maximum_file_cache_bytes, filesystem_capacity / 10u);
    }

    /** Preserve the larger of 10 GiB and ten percent free space. */
    [[nodiscard]] static constexpr std::uint64_t
    free_floor_bytes(std::uint64_t filesystem_capacity) noexcept {
        return std::max(k_minimum_file_free_bytes, filesystem_capacity / 10u);
    }

    /**
     * Validate one prospective file reservation against an explicit free
     * floor.  Callers may lower the floor only for isolated test policy; actual
     * availability and the ten-percent capacity ceiling always remain active.
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

    /** Validate one production file reservation with checked arithmetic. */
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
