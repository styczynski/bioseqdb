/**
 * @file instrumentation/detail/siglock.cppm
 * @brief Reentrancy counter for signal-safe paths.
 *
 * Despite the name, Siglock is NOT a mutual-exclusion lock. It is a
 * reentrancy counter (Caliper-style). A signal handler that re-enters
 * profiler code can detect mid-update state via is_held() and bail out.
 * Acquire/release pair around a critical section that must not be
 * re-entered from the same thread; signal handlers check is_held() before
 * touching shared state.
 */
module;

#include <csignal>

export module bioseq_instr.detail.siglock;

export namespace bioseq::instr::detail {

class Siglock {
public:
    [[gnu::always_inline]] void lock() noexcept   { ++depth_; }
    [[gnu::always_inline]] void unlock() noexcept { --depth_; }
    [[nodiscard, gnu::always_inline]]
    bool is_held() const noexcept { return depth_ > 0; }

    struct Guard {
        Siglock& lock;
        [[gnu::always_inline]] explicit Guard(Siglock& l) noexcept : lock(l) { lock.lock(); }
        [[gnu::always_inline]] ~Guard() noexcept { lock.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

private:
    volatile std::sig_atomic_t depth_ = 0;
};

}  // namespace bioseq::instr::detail
