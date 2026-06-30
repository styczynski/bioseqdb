/**
 * @file instrumentation/detail/summary_writer.cppm
 * @brief Cold-path one-shot summary dump.
 *
 * Declares cold-path non-templated entry points that route to the
 * process-wide profiler block. Bodies live in the same-module implementation
 * unit so GCC does not have to serialize the templated logging/reset bodies
 * into this exported BMI.
 */
export module bioseq.instr.detail.summary_writer;

export namespace bioseq::instr::access {

/// Non-templated entry: dump the persistent process-wide profiler block
/// for context_args_profiling_full to bioseq_log<DBG>. No-op if profiling never
/// ran in this process.
void write_summary_to_justlog() noexcept;

void reset_all() noexcept;

}  // namespace bioseq::instr::access
