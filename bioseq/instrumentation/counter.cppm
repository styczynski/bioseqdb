/**
 * @file instrumentation/counter.cppm
 * @brief Process-wide named counters with per-thread storage.
 *
 * Counters are referenced by name; the runtime keeps a single canonical
 * `named_counter` per name in a process-wide registry.
 *
 * **Per-thread accumulation.** Each `add_to_counter` deposits into a slot
 * keyed by `std::this_thread::get_id()` — that is, the calling thread's
 * own slot. `current_thread_counter_value` reads only the current thread's slot.
 * Iterations on the same TBB worker are inherently serial, so the
 * before/after delta the bench framework computes around `on_run` is
 * exact for that iteration and never picks up other concurrent
 * iterations' contributions. This is what allows the framework to run
 * counter-bearing benchmarks in parallel without cross-iteration aliasing.
 *
 * **Compile-time gate.** When `p_configuration_type::enable_counters` is false, the
 * body of `add_to_counter` collapses away — no TLS lookup, no mutex.
 *
 * **Caveats.**
 *   - Nested parallelism inside `on_run` (a user-driven
 *     `tbb::parallel_for`) deposits into the *nested* worker's slot, not
 *     the outer iteration's slot. The framework's snapshot will not
 *     observe those increments. Avoid `add_to_counter` from nested workers
 *     or accept that those counts are lost from the outer-iteration
 *     delta.
 *   - `current_thread_counter_value` returns only the calling thread's slot. Use
 *     `total_counter_value(counter)` when you need the global sum across all
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

export module bioseq.instrumentation.counter;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;

export namespace bioseq::instrumentation {

/** @brief Canonical named counter with one accumulator per writer thread. */
struct named_counter {
    const region_descriptor* descriptor = nullptr;
    /// Per-thread accumulator. Keyed by `std::this_thread::get_id()`;
    /// only ever read/written under `mu`. The mutex covers both the
    /// map structure (`unordered_map`) and the per-slot integers, which
    /// is sufficient because every access happens through the
    /// `counter_*` API in this header.
    mutable std::mutex mutex;
    std::unordered_map<std::thread::id, std::int64_t> values_by_thread{};

    /** @brief Construct an unregistered zero-valued counter. */
    named_counter() = default;

    /** @brief Construct a zero-valued counter for a registered descriptor. */
    explicit named_counter(const region_descriptor* metric_descriptor) noexcept
        : descriptor(metric_descriptor) {}

    named_counter(const named_counter&) = delete;
    named_counter& operator=(const named_counter&) = delete;
    named_counter(named_counter&&) = delete;
    named_counter& operator=(named_counter&&) = delete;
};

}  // namespace bioseq::instrumentation

namespace bioseq::instrumentation::detail {

/** @brief Owns canonical counters indexed by metric name. */
struct counter_registry {
    std::mutex mutex;
    std::unordered_map<std::string, named_counter*> by_name;
};

/** @brief Return the process-wide counter registry. */
[[nodiscard]] inline counter_registry& get_counter_registry() noexcept {
    static counter_registry registry;
    return registry;
}

}  // namespace detail

export namespace bioseq::instrumentation {

/// Returns the canonical counter for `name`. Created on first call;
/// subsequent calls return the same reference.
[[nodiscard, gnu::cold]]
inline named_counter& get_named_counter(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept
{
    auto& registry = detail::get_counter_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::string key{name};
    auto entry = registry.by_name.find(key);
    if (entry != registry.by_name.end()) return *entry->second;
    auto* descriptor = register_static_counter(name, loc);
    auto* created_counter = new named_counter{descriptor};
    registry.by_name.emplace(std::move(key), created_counter);
    return *created_counter;
}

/// Add `delta` to the calling thread's slot of `c`. Safe to call from
/// any thread concurrently — different threads write to different
/// slots, all serialised through `c.mu`.
template<profiling_configuration p_configuration_type>
inline void add_to_counter(named_counter& counter, std::int64_t delta) noexcept {
    if constexpr (p_configuration_type::enable_counters) {
        std::lock_guard<std::mutex> lock(counter.mutex);
        counter.values_by_thread[std::this_thread::get_id()] += delta;
    } else {
        (void)counter;
        (void)delta;
    }
}

/// Returns the calling thread's slot of `c`, or 0 if this thread has
/// never deposited into the counter. Read-only; takes the mutex.
[[nodiscard]]
inline std::int64_t current_thread_counter_value(const named_counter& counter) noexcept {
    std::lock_guard<std::mutex> lock(counter.mutex);
    const auto entry = counter.values_by_thread.find(std::this_thread::get_id());
    return entry == counter.values_by_thread.end() ? 0 : entry->second;
}

/// Returns the sum of every thread's slot of `c`. Useful for ad-hoc
/// debugging and end-of-run roll-ups; the bench framework's per-
/// iteration snapshot path uses `current_thread_counter_value` instead so it sees
/// only the worker thread's own contribution.
[[nodiscard]]
inline std::int64_t total_counter_value(const named_counter& counter) noexcept {
    std::lock_guard<std::mutex> lock(counter.mutex);
    std::int64_t total = 0;
    for (const auto& [thread_id, slot_value] : counter.values_by_thread) {
        (void)thread_id;
        total += slot_value;
    }
    return total;
}

}  // namespace bioseq::instrumentation
