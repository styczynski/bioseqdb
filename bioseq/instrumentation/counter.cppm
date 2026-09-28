/**
 * @file instrumentation/counter.cppm
 * @brief Named counters with a separate total for each thread.
 *
 * A counter name selects one shared counter Reads can return the calling
 * thread's value  or the sum across threads. Disabled configurations skip updates.
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

/** @brief Named counter with one accumulated value per thread. */
struct named_counter {
    /** @brief Registered identity for this counter. */
    const region_descriptor* descriptor = nullptr;
    /** @brief Lock for the hread map and its counter values. */
    mutable std::mutex mutex;
    /** @brief Values indexed by the thread that updates them. */
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

/** @brief Shared counters indexed by name. */
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

/** @brief Find the shared counter by name, creating it on first use. */
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

/** @brief Add `delta` to the calling thread's slot under the counter lock. */
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

/**
 * @brief Return the calling thread's slot, or zero when it has no entry.
 *
 * Updates made by nested workers remain in those workers' slots.
 */
[[nodiscard]]
inline std::int64_t current_thread_counter_value(const named_counter& counter) noexcept {
    std::lock_guard<std::mutex> lock(counter.mutex);
    const auto entry = counter.values_by_thread.find(std::this_thread::get_id());
    return entry == counter.values_by_thread.end() ? 0 : entry->second;
}

/** @brief Return the sum of all thread slots. */
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
