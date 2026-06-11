/**
 * @file memory_admission.cppm
 * @brief Backend-local memory reservations shared by dispatch, pipeline tasks,
 *        and index residency features.
 */
module;
#include "common.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

export module bioseq_parallelism.memory_admission;
import bioseq_instr;

export namespace bioseq::parallelism {

class MemoryAdmissionController;

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

    [[nodiscard]] std::optional<MemoryPermit> try_reserve(std::uint64_t bytes) noexcept {
        if (bytes == 0) {
            return MemoryPermit{this, 0};
        }
        if (bioseq::memory::default_budget().under_pressure()) {
            return std::nullopt;
        }

        std::uint64_t current = reserved_.load(std::memory_order_relaxed);
        for (;;) {
            if (bytes > (std::numeric_limits<std::uint64_t>::max)() - current) {
                return std::nullopt;
            }
            const std::uint64_t budget = budget_ceiling();
            if (budget != (std::numeric_limits<std::uint64_t>::max)()) {
                if (current > budget || bytes > budget - current) {
                    return std::nullopt;
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

[[nodiscard]] inline BudgetedWave try_reserve_budgeted_wave(
        unsigned requested,
        std::uint64_t peak_bytes_per_task) noexcept {
    return MemoryAdmissionController::instance().try_reserve_wave(
        requested, peak_bytes_per_task);
}

} // namespace bioseq::parallelism
