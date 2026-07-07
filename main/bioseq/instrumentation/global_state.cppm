/**
 * @file instrumentation/global_state.cppm
 * @brief Process-wide profiler state.
 *
 * Owns the persistent (across-batch) ProfilerBlock<Args> for each
 * canonical Args type. The dispatch fork in
 * BatchProcessor::process_sequences installs this persistent block via
 * installed_profiler_scope so aggregates accumulate across multiple
 * batches in the same scan. The summary writer runs at scan end
 * (ColumnarScan_EndCustomScan) and reads from the persistent block,
 * then resets it via access::reset_all() so the next scan starts clean.
 *
 * The block is heap-constructed once per Args via Meyer's-singleton
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

export module bioseq.instr.global_state;

import bioseq.instr.config;
import bioseq.instr.thread_state;

export namespace bioseq::instr {

inline std::atomic<bool> g_runtime_enabled{true};

[[gnu::always_inline]]
inline bool runtime_enabled() noexcept {
    return g_runtime_enabled.load(std::memory_order_relaxed);
}

inline void set_runtime_enabled(bool on) noexcept {
    g_runtime_enabled.store(on, std::memory_order_relaxed);
}

/// Persistent process-wide ProfilerBlock for the given Args. Accumulates
/// across all batches in a scan. ColumnarScan_EndCustomScan reads from
/// it once at scan end, prints the summary, then calls reset_all to
/// clear before the next scan.
template<is_context_args Args>
[[nodiscard]] inline ProfilerBlock<Args>& persistent_profiler_block() noexcept {
    static ProfilerBlock<Args> block;
    return block;
}

}  // namespace bioseq::instr
