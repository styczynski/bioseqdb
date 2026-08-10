/**
 * @file instrumentation/runtime.cppm
 * @brief Process-wide instrumentation state and runtime gate.
 *
 * Owns the persistent (across-batch) profiler_state<p_configuration_type> for each
 * canonical configuration type. Pipeline dispatch installs this state via
 * installed_profiler_scope so aggregates accumulate across multiple
 * batches in the same scan. The summary writer runs at scan end
 * the summary writer and resets it before the next scan.
 *
 * The block is heap-constructed once per p_configuration_type via Meyer's-singleton
 * pattern (the static-local lazy init); destruction at process exit is
 * fine because the destructor only frees aggregate blocks, no cross-
 * thread state.
 *
 * Also provides a process-wide runtime enable flag
 * (`set_runtime_enabled`) used to pause profiling without recompiling —
 * checked on every region begin in the profiling-enabled instantiation.
 */
module;

#include <atomic>

export module bioseq.instrumentation.runtime;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.profiler_state;

namespace bioseq::instrumentation::detail {

inline std::atomic<bool> runtime_enabled_flag{true};

}  // namespace bioseq::instrumentation::detail

export namespace bioseq::instrumentation {

[[gnu::always_inline]]
inline bool is_runtime_enabled() noexcept {
    return detail::runtime_enabled_flag.load(std::memory_order_relaxed);
}

inline void set_runtime_enabled(bool enabled) noexcept {
    detail::runtime_enabled_flag.store(enabled, std::memory_order_relaxed);
}

/// Persistent process-wide profiler_state for the given p_configuration_type. Accumulates
/// across all batches in a scan. ColumnarScan_EndCustomScan reads from
/// it once at scan end, prints the summary, then calls reset_metrics to
/// clear before the next scan.
template<profiling_configuration p_configuration_type>
[[nodiscard]] inline profiler_state<p_configuration_type>& persistent_profiler_state() noexcept {
    static profiler_state<p_configuration_type> state;
    return state;
}

}  // namespace bioseq::instrumentation
