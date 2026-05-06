/**
 * @file instrumentation/counter.cppm
 * @brief Process-wide named counters with per-thread storage.
 *
 * Counters are referenced by name; the runtime keeps a single canonical
 * `Counter` per name in a process-wide registry.
 *
 * **Per-thread accumulation.** Each `counter_add` deposits into a slot
 * keyed by `std::this_thread::get_id()` — that is, the calling thread's
 * own slot. `counter_value` reads only the current thread's slot.
 * Iterations on the same TBB worker are inherently serial, so the
 * before/after delta the bench framework computes around `on_run` is
 * exact for that iteration and never picks up other concurrent
 * iterations' contributions. This is what allows the framework to run
 * counter-bearing benchmarks in parallel since V5; before that, every
 * counter-using benchmark had to opt out of `tbb::parallel_for_each`
 * to avoid cross-iteration aliasing.
 *
 * **Compile-time gate.** When `Args::enable_counters` is false, the
 * body of `counter_add` collapses away — no TLS lookup, no mutex.
 *
 * **Caveats.**
 *   - Nested parallelism inside `on_run` (a user-driven
 *     `tbb::parallel_for`) deposits into the *nested* worker's slot, not
 *     the outer iteration's slot. The framework's snapshot will not
 *     observe those increments. Avoid `counter_add` from nested workers
 *     or accept that those counts are lost from the outer-iteration
 *     delta.
 *   - Pre-V5 callers used to read a global total via `counter_value`.
 *     The new semantics return only the calling thread's slot. Use
 *     `counter_total(c)` when you need the global sum across all
 *     threads (e.g. ad-hoc debugging or end-of-run roll-ups).
 */
module;

#include <cstdint>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>

export module bioseq_instr.counter;

import bioseq_instr.config;
import bioseq_instr.descriptor;

export namespace bioseq::instr {

struct Counter {
    const RegionDescriptor* desc = nullptr;
    /// Per-thread accumulator. Keyed by `std::this_thread::get_id()`;
    /// only ever read/written under `mu`. The mutex covers both the
    /// map structure (`unordered_map`) and the per-slot integers, which
    /// is sufficient because every access happens through the
    /// `counter_*` API in this header.
    mutable std::mutex                                       mu;
    std::unordered_map<std::thread::id, std::int64_t>        per_thread_value{};

    Counter() = default;
    explicit Counter(const RegionDescriptor* d) noexcept : desc(d) {}

    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    Counter(Counter&&) = delete;
    Counter& operator=(Counter&&) = delete;
};

namespace detail {

struct CounterRegistry {
    std::mutex mu;
    std::unordered_map<std::string, Counter*> by_name;
};

[[nodiscard]] inline CounterRegistry& counter_registry() noexcept {
    static CounterRegistry r;
    return r;
}

}  // namespace detail

/// Returns the canonical counter for `name`. Created on first call;
/// subsequent calls return the same reference.
[[nodiscard, gnu::cold]]
inline Counter& counter(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept
{
    auto& reg = detail::counter_registry();
    std::lock_guard<std::mutex> g(reg.mu);
    std::string key{name};
    auto it = reg.by_name.find(key);
    if (it != reg.by_name.end()) return *it->second;
    auto* d = register_region_static(name, loc);
    auto* c = new Counter{d};
    reg.by_name.emplace(std::move(key), c);
    return *c;
}

/// Add `delta` to the calling thread's slot of `c`. Safe to call from
/// any thread concurrently — different threads write to different
/// slots, all serialised through `c.mu`.
template<is_context_args Args>
inline void counter_add(Counter& c, std::int64_t delta) noexcept {
    if constexpr (Args::enable_counters) {
        std::lock_guard<std::mutex> guard(c.mu);
        c.per_thread_value[std::this_thread::get_id()] += delta;
    } else {
        (void)c;
        (void)delta;
    }
}

/// Returns the calling thread's slot of `c`, or 0 if this thread has
/// never deposited into the counter. Read-only; takes the mutex.
[[nodiscard]]
inline std::int64_t counter_value(const Counter& c) noexcept {
    std::lock_guard<std::mutex> guard(c.mu);
    const auto it = c.per_thread_value.find(std::this_thread::get_id());
    return it == c.per_thread_value.end() ? 0 : it->second;
}

/// Returns the sum of every thread's slot of `c`. Useful for ad-hoc
/// debugging and end-of-run roll-ups; the bench framework's per-
/// iteration snapshot path uses `counter_value` instead so it sees
/// only the worker thread's own contribution.
[[nodiscard]]
inline std::int64_t counter_total(const Counter& c) noexcept {
    std::lock_guard<std::mutex> guard(c.mu);
    std::int64_t total = 0;
    for (const auto& [_thread_id, slot_value] : c.per_thread_value) {
        total += slot_value;
    }
    return total;
}

}  // namespace bioseq::instr
