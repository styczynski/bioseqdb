/**
 * @file instrumentation/detail/summary_writer.cppm
 * @brief Cold-path one-shot summary dump.
 *
 * Re-exports the typed write_summary_to_justlog<Args>(prof) symbol from
 * bioseq_instr.access and provides a non-templated entry point that
 * routes to the process-wide profiler block held in
 * bioseq_instr.global_state. ColumnarScan_EndCustomScan calls the
 * non-templated form once per scan; the dispatch fork in
 * BatchProcessor::process_sequences does not call it (multi-batch scans
 * would otherwise emit one summary per batch — see Part 48.5).
 */
module;

export module bioseq.instr.detail.summary_writer;

import bioseq.instr.config;
import bioseq.instr.global_state;
import bioseq.instr.access;
import bioseq.instr.thread_state;
import bioseq.instr.counter;
import bioseq.instr.gauge;

export namespace bioseq::instr::access {

/// Non-templated entry: dump the persistent process-wide profiler block
/// for context_args_profiling_full to bioseq_log<DBG>. No-op if profiling never
/// ran in this process.
inline void write_summary_to_justlog() noexcept {
    using Args = bioseq::instr::context_args_profiling_full;
    auto& block = persistent_profiler_block<Args>();
    write_summary_to_justlog<Args>(block);
}

inline void reset_all() noexcept {
    using Args = bioseq::instr::context_args_profiling_full;
    auto& block = persistent_profiler_block<Args>();
    reset_all<Args>(block);
}

}  // namespace bioseq::instr::access
