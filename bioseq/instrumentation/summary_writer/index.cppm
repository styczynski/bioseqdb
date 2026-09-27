/**
 * @file instrumentation/summary_writer/index.cppm
 * @brief Print and reset the shared production profiler.
 *
 * Definitions live in the implementation unit so GCC can keep the logging
 * and reset templates out of this exported module interface.
 */
export module bioseq.instrumentation.summary_writer;

export namespace bioseq::instrumentation::summary {

/// Print measurements fom the shared production profiler to the debug log
/// A profiler with  no recorded visits still produces summary markers.
void write_summary_to_log() noexcept;

/** @brief Reset metrics in the persistent production profiler state. */
void reset_metrics() noexcept;

}  // namespace bioseq::instrumentation::summary
