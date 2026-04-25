/**
 * @file instrumentation/counter.cppm
 * @brief Process-wide atomic counters (Part 19.9 / 20.2 of the design doc).
 *
 * Counters are referenced by name; the runtime keeps a single canonical
 * Counter per name in a process-wide registry. Hot-path use is
 * counter_add(c, delta) which does a relaxed fetch_add — ~3-10 cycles
 * uncontended on modern hardware. The compile-time gate sits in
 * counter_add: when Args::enable_counters is false the body collapses
 * away.
 */
module;

#include <atomic>
#include <cstdint>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>

export module bioseq_instr.counter;

import bioseq_instr.config;
import bioseq_instr.descriptor;

export namespace bioseq::instr {

struct Counter {
    const RegionDescriptor* desc = nullptr;
    std::atomic<std::int64_t> value{0};

    Counter() = default;
    explicit Counter(const RegionDescriptor* d) noexcept : desc(d) {}
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

template<is_context_args Args>
[[gnu::always_inline]]
inline void counter_add(Counter& c, std::int64_t delta) noexcept {
    if constexpr (Args::enable_counters) {
        c.value.fetch_add(delta, std::memory_order_relaxed);
    } else {
        (void)c;
        (void)delta;
    }
}

[[nodiscard, gnu::always_inline]]
inline std::int64_t counter_value(const Counter& c) noexcept {
    return c.value.load(std::memory_order_relaxed);
}

}  // namespace bioseq::instr
