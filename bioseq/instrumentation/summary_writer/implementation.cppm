/**
 * @file instrumentation/summary_writer/implementation.cppm
 * @brief Cold-path one-shot summary dump implementation.
 */
module;

module bioseq.instrumentation.summary_writer;

import bioseq.instrumentation.configuration;
import bioseq.instrumentation.runtime;
import bioseq.instrumentation.summary;
import bioseq.instrumentation.profiler_state;

namespace bioseq::instrumentation::summary {

/** @brief Route the production profiler state through the templated summary writer. */
void write_summary_to_log() noexcept {
    using production_configuration_type =
        bioseq::instrumentation::production_profiling_configuration;
    auto& production_profiler = persistent_profiler_state<production_configuration_type>();
    write_summary_to_log<production_configuration_type>(production_profiler);
}

/** @brief Reset the persistent production profiler state. */
void reset_metrics() noexcept {
    using production_configuration_type =
        bioseq::instrumentation::production_profiling_configuration;
    auto& production_profiler = persistent_profiler_state<production_configuration_type>();
    reset_metrics<production_configuration_type>(production_profiler);
}

} // namespace bioseq::instrumentation::summary
