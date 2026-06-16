/**
 * @file instrumentation/detail/spinlock.cppm
 * @brief std::atomic_flag-based spinlock for cold-path mutexes.
 *
 * Used to guard rare cold-path state (descriptor allocation, the
 * thread registry list). Hot path never takes this lock.
 */
module;

#include <atomic>
#include <thread>

export module bioseq.instr.detail.spinlock;

export namespace bioseq::instr::detail {

class Spinlock {
public:
    [[gnu::always_inline]] void lock() noexcept {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }
    [[gnu::always_inline]] void unlock() noexcept {
        flag_.clear(std::memory_order_release);
    }

    struct Guard {
        Spinlock& lock;
        [[gnu::always_inline]] explicit Guard(Spinlock& l) noexcept : lock(l) { lock.lock(); }
        [[gnu::always_inline]] ~Guard() noexcept { lock.unlock(); }
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
    };

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

}  // namespace bioseq::instr::detail
