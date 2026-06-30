/**
 * @file instrumentation/detail/summary_writer.cpp
 * @brief Cold-path one-shot summary dump implementation.
 */
module;

module bioseq.instr.detail.summary_writer;

import bioseq.instr.config;
import bioseq.instr.global_state;
import bioseq.instr.access;
import bioseq.instr.thread_state;
import bioseq.instr.counter;
import bioseq.instr.gauge;

namespace bioseq::instr::access {

void write_summary_to_justlog() noexcept {
    using Args = bioseq::instr::context_args_profiling_full;
    auto& block = persistent_profiler_block<Args>();
    write_summary_to_justlog<Args>(block);
}

void reset_all() noexcept {
    using Args = bioseq::instr::context_args_profiling_full;
    auto& block = persistent_profiler_block<Args>();
    reset_all<Args>(block);
}

}  // namespace bioseq::instr::access
