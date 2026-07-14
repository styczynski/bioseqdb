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
#include <type_traits>
#include <utility>

export module bioseq.cache.core:admission;
import :checked_math;
import :wire;

export namespace bioseq::cache {

enum class AdmissionError : std::uint8_t {
    CounterOverflow,
    BudgetExhausted,
    SystemPressure,
    InvalidReservation,
};

template <class Counter>
concept cache_reservation_counter = requires(
    Counter counter,
    std::uint64_t expected,
    std::uint64_t desired,
    std::uint64_t bytes) {
    { counter.load() } -> std::same_as<std::uint64_t>;
    { counter.compare_exchange(expected, desired) } -> std::same_as<bool>;
    { counter.subtract(bytes) } -> std::same_as<void>;
};

template <class Budget>
concept cache_memory_budget = requires(const Budget budget) {
    { budget.allocatable_bytes() } -> std::same_as<std::uint64_t>;
    { budget.under_pressure() } -> std::same_as<bool>;
};

template <cache_reservation_counter Counter, cache_memory_budget Budget>
class BasicCacheAdmission;

/** Move-only ownership of bytes held against a shared reservation counter. */
template <cache_reservation_counter Counter, cache_memory_budget Budget>
class CachePermit {
public:
    CachePermit() = default;
    CachePermit(
        BasicCacheAdmission<Counter, Budget>* admission,
        std::uint64_t bytes) noexcept
        : admission_(admission), bytes_(bytes) {}

    CachePermit(CachePermit&& other) noexcept {
        move_from(other);
    }

    CachePermit& operator=(CachePermit&& other) noexcept {
        if (this != &other) {
            release();
            move_from(other);
        }
        return *this;
    }

    CachePermit(const CachePermit&) = delete;
    CachePermit& operator=(const CachePermit&) = delete;

    ~CachePermit() noexcept {
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

private:
    /** Transfer ownership and clear the source permit. */
    void move_from(CachePermit& other) noexcept {
        admission_ = std::exchange(other.admission_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0);
    }

    BasicCacheAdmission<Counter, Budget>* admission_ = nullptr;
    std::uint64_t bytes_ = 0;
};

/** Admission controller sharing one atomic-like counter across all producers. */
template <cache_reservation_counter Counter, cache_memory_budget Budget>
class BasicCacheAdmission {
public:
    using Permit = CachePermit<Counter, Budget>;

    BasicCacheAdmission(Counter& counter, const Budget& budget) noexcept
        : counter_(counter), budget_(budget) {}

    /** Reserve bytes without overcommitting the current shared ceiling. */
    [[nodiscard]] std::expected<Permit, AdmissionError> try_reserve(
        std::uint64_t bytes) noexcept {
        if (budget_.under_pressure()) {
            return std::unexpected(AdmissionError::SystemPressure);
        }
        if (bytes == 0) {
            return Permit{this, 0};
        }
        const std::uint64_t ceiling = budget_ceiling();
        std::uint64_t observed = counter_.load();
        for (;;) {
            const auto desired = checked_add(observed, bytes);
            if (!desired.has_value()) {
                return std::unexpected(AdmissionError::CounterOverflow);
            }
            if (ceiling != (std::numeric_limits<std::uint64_t>::max)()
                && *desired > ceiling) {
                return std::unexpected(AdmissionError::BudgetExhausted);
            }
            if (counter_.compare_exchange(observed, *desired)) {
                return Permit{this, bytes};
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
    friend class CachePermit<Counter, Budget>;

    /** Apply the cache's 25-percent physical-memory ceiling when available. */
    [[nodiscard]] std::uint64_t budget_ceiling() const noexcept {
        const std::uint64_t allocatable = budget_.allocatable_bytes();
        if constexpr (requires { budget_.total_bytes(); }) {
            const std::uint64_t total = budget_.total_bytes();
            if (total == 0) {
                return allocatable;
            }
            const std::uint64_t cache_share = total
                / kCacheMemoryFractionDenominator
                * kCacheMemoryFractionNumerator;
            return allocatable
                    == (std::numeric_limits<std::uint64_t>::max)()
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

    Counter& counter_;
    const Budget& budget_;
};

template <cache_reservation_counter Counter, cache_memory_budget Budget>
void CachePermit<Counter, Budget>::shrink_to(std::uint64_t bytes) noexcept {
    if (admission_ == nullptr || bytes >= bytes_) {
        return;
    }
    admission_->release_bytes(bytes_ - bytes);
    bytes_ = bytes;
}

template <cache_reservation_counter Counter, cache_memory_budget Budget>
bool CachePermit<Counter, Budget>::grow_to(std::uint64_t bytes) noexcept {
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

template <cache_reservation_counter Counter, cache_memory_budget Budget>
void CachePermit<Counter, Budget>::release() noexcept {
    if (admission_ == nullptr) {
        return;
    }
    admission_->release_bytes(bytes_);
    admission_ = nullptr;
    bytes_ = 0;
}

/** Internal v1 file-capacity/free-space policy. */
struct FileAdmissionPolicy {
    /** Return the smaller of 128 GiB and ten percent of filesystem capacity. */
    [[nodiscard]] static constexpr std::uint64_t ceiling_bytes(
        std::uint64_t filesystem_capacity) noexcept {
        return std::min(
            kMaxFileCacheBytes,
            filesystem_capacity / 10u);
    }

    /** Preserve the larger of 10 GiB and ten percent free space. */
    [[nodiscard]] static constexpr std::uint64_t free_floor_bytes(
        std::uint64_t filesystem_capacity) noexcept {
        return std::max(
            kMinFileFreeBytes,
            filesystem_capacity / 10u);
    }

    /** Validate one prospective file reservation with checked arithmetic. */
    [[nodiscard]] static constexpr bool can_reserve(
        std::uint64_t reserved_file_bytes,
        std::uint64_t requested_bytes,
        std::uint64_t filesystem_capacity,
        std::uint64_t filesystem_available) noexcept {
        const auto desired = checked_add(reserved_file_bytes, requested_bytes);
        return desired.has_value()
            && *desired <= ceiling_bytes(filesystem_capacity)
            && filesystem_available >= requested_bytes
            && filesystem_available - requested_bytes
                >= free_floor_bytes(filesystem_capacity);
    }
};

} // namespace bioseq::cache
