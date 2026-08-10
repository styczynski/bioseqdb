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

void write_summary_to_log() noexcept {
    using configuration_type = bioseq::instrumentation::production_profiling_configuration;
    auto& profiler = persistent_profiler_state<configuration_type>();
    write_summary_to_log<configuration_type>(profiler);
}

void reset_metrics() noexcept {
    using configuration_type = bioseq::instrumentation::production_profiling_configuration;
    auto& profiler = persistent_profiler_state<configuration_type>();
    reset_metrics<configuration_type>(profiler);
}

}  // namespace bioseq::instrumentation::summary
