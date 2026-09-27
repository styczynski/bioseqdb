/**
 * @file instrumentation/summary_writer/index.cppm
 * @brief Cold-path one-shot summary dump.
 *
 * Declares cold-path non-templated entry points that route to the
 * process-wide profiler block. Bodies live in the same-module implementation
 * unit so GCC does not have to serialize the templated logging/reset bodies
 * into this exported BMI.
 */
export module bioseq.instrumentation.summary_writer;

export namespace bioseq::instrumentation::summary {

/// Non-templated entry: dump the persistent process-wide profiler block
/// for production_profiling_configuration to the debug log. No-op if profiling never
/// ran in this process.
void write_summary_to_log() noexcept;

/** @brief Reset metrics in the persistent production profiler state. */
void reset_metrics() noexcept;

}  // namespace bioseq::instrumentation::summary
