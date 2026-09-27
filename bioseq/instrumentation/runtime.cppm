/**
 * @file instrumentation/runtime.cppm
 * @brief Shared profiler state and a switch for region and loop timing.
 *
 * The runtime switch controls new measurement scopes. Existing scopes can
 * still finish and record their results.
 */
module;

#include <atomic>

export module bioseq.instrumentation.runtime;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.profiler_state;

namespace bioseq::instrumentation::detail {

/// Process-wide switch checked when region and loop scopes begin
inline std::atomic<bool> runtime_enabled_flag{true};

}  // namespace bioseq::instrumentation::detail

export namespace bioseq::instrumentation {

/** @brief Return whether new region and loop measurements are enabled. */
[[nodiscard]]
[[gnu::always_inline]]
inline bool is_runtime_enabled() noexcept {
    return detail::runtime_enabled_flag.load(std::memory_order_relaxed);
}

/** @brief Enable or pause new region and loop measurements. */
inline void set_runtime_enabled(bool enabled) noexcept {
    detail::runtime_enabled_flag.store(enabled, std::memory_order_relaxed);
}

/** @brief Return shared persistent state for one configuration. */
template<profiling_configuration p_configuration_type>
[[nodiscard]] inline profiler_state<p_configuration_type>& persistent_profiler_state() noexcept {
    static profiler_state<p_configuration_type> state;
    return state;
}

}  // namespace bioseq::instrumentation
