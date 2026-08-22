/**
 * @file instrumentation/runtime.cppm
 * @brief Process-wide instrumentation state and runtime gate.
 *
 * Each configuration owns persistent scan-wide state. A relaxed atomic gate
 * pauses profiling without changing the compiled feature set.
 */
module;

#include <atomic>

export module bioseq.instrumentation.runtime;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.profiler_state;

namespace bioseq::instrumentation::detail {

/// Process-wide gate checked by profiling-enabled scope constructors.
inline std::atomic<bool> runtime_enabled_flag{true};

}  // namespace bioseq::instrumentation::detail

export namespace bioseq::instrumentation {

/** @brief Return whether runtime instrumentation is currently enabled. */
[[nodiscard]]
[[gnu::always_inline]]
inline bool is_runtime_enabled() noexcept {
    return detail::runtime_enabled_flag.load(std::memory_order_relaxed);
}

/** @brief Enable or pause instrumentation without changing compiled features. */
inline void set_runtime_enabled(bool enabled) noexcept {
    detail::runtime_enabled_flag.store(enabled, std::memory_order_relaxed);
}

/** @brief Return persistent process-wide state for one configuration. */
template<profiling_configuration p_configuration_type>
[[nodiscard]] inline profiler_state<p_configuration_type>& persistent_profiler_state() noexcept {
    static profiler_state<p_configuration_type> state;
    return state;
}

}  // namespace bioseq::instrumentation
