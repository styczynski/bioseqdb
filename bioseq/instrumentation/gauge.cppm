/**
 * @file instrumentation/gauge.cppm
 * @brief Named gauges that store a shared current value.
 *
 * Atomic reads and writes allow concurrent updates. They  do not snchronize
 * other program data.
 */
module;

#include <atomic>
#include <cstdint>
#include <mutex>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>

export module bioseq.instrumentation.gauge;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.descriptor;

export namespace bioseq::instrumentation {

/** @brief Shared named gauge with its current value. */
struct named_gauge {
    const region_descriptor* descriptor = nullptr;
    std::atomic<std::int64_t> value{0};

    /** @brief Construct an unregistered zero-valued gauge. */
    named_gauge() = default;

    /** @brief Construct a zero-valued gauge for a registered descriptor. */
    explicit named_gauge(const region_descriptor* metric_descriptor) noexcept
        : descriptor(metric_descriptor) {}
};

}  // namespace bioseq::instrumentation

namespace bioseq::instrumentation::detail {

/** @brief Shared gauges indexed by name. */
struct gauge_registry {
    std::mutex mutex;
    std::unordered_map<std::string, named_gauge*> by_name;
};

/** @brief Return the process-wide gauge registry. */
[[nodiscard]] inline gauge_registry& get_gauge_registry() noexcept {
    static gauge_registry registry;
    return registry;
}

}  // namespace detail

export namespace bioseq::instrumentation {

/** @brief Find the shared gauge by name, creating it on first use. */
[[nodiscard, gnu::cold]]
inline named_gauge& get_named_gauge(
    std::string_view name,
    std::source_location loc = std::source_location::current()) noexcept
{
    auto& registry = detail::get_gauge_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    std::string key{name};
    auto entry = registry.by_name.find(key);
    if (entry != registry.by_name.end()) return *entry->second;
    auto* descriptor = register_static_gauge(name, loc);
    auto* created_gauge = new named_gauge{descriptor};
    registry.by_name.emplace(std::move(key), created_gauge);
    return *created_gauge;
}

/** @brief Store the gauge's current value when profiling is compiled in. */
template<profiling_configuration p_configuration_type>
[[gnu::always_inline]]
inline void set_gauge_value(named_gauge& gauge, std::int64_t value) noexcept {
    if constexpr (p_configuration_type::enable_profiling) {
        gauge.value.store(value, std::memory_order_relaxed);
    } else {
        (void)gauge;
        (void)value;
    }
}

/** @brief Read the gauge's current value. */
[[nodiscard, gnu::always_inline]]
inline std::int64_t current_gauge_value(const named_gauge& gauge) noexcept {
    return gauge.value.load(std::memory_order_relaxed);
}

}  // namespace bioseq::instrumentation
