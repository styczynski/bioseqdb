/**
 * @file instrumentation/gauge.cppm
 * @brief Per-thread "last set value" gauges (Part 19.10 of the design doc).
 *
 * Gauges are single-writer per-thread on the producer side; readers
 * see eventual consistency. On 64-bit platforms (our only target)
 * aligned 64-bit reads/writes are tear-free at the hardware level so
 * std::atomic is unnecessary.
 */
module;

#include <atomic>
#include <cstdint>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>

export module bioseq.instr.gauge;

import bioseq.instr.config;
import bioseq.instr.descriptor;

export namespace bioseq::instr {

struct Gauge {
    const RegionDescriptor* desc = nullptr;
    std::atomic<std::int64_t> value{0};

    Gauge() = default;
    explicit Gauge(const RegionDescriptor* d) noexcept : desc(d) {}
};

namespace detail {

struct GaugeRegistry {
    std::mutex mu;
    std::unordered_map<std::string, Gauge*> by_name;
};

[[nodiscard]] inline GaugeRegistry& gauge_registry() noexcept {
    static GaugeRegistry r;
    return r;
}

}  // namespace detail

[[nodiscard, gnu::cold]]
inline Gauge& gauge(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept
{
    auto& reg = detail::gauge_registry();
    std::lock_guard<std::mutex> g(reg.mu);
    std::string key{name};
    auto it = reg.by_name.find(key);
    if (it != reg.by_name.end()) return *it->second;
    auto* d = register_region_static(name, loc);
    auto* gp = new Gauge{d};
    reg.by_name.emplace(std::move(key), gp);
    return *gp;
}

template<is_context_args Args>
[[gnu::always_inline]]
inline void gauge_set(Gauge& g, std::int64_t v) noexcept {
    if constexpr (Args::enable_profiling) {
        g.value.store(v, std::memory_order_relaxed);
    } else {
        (void)g;
        (void)v;
    }
}

[[nodiscard, gnu::always_inline]]
inline std::int64_t gauge_value(const Gauge& g) noexcept {
    return g.value.load(std::memory_order_relaxed);
}

}  // namespace bioseq::instr
