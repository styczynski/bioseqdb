/**
 * @file instrumentation/index.cppm
 * @brief Import the public profiling APIs from one module.
 *
 * Re-exports timing scopes, counters, allocation tracking, and summary APIs.
 * @note Platform-specific counter selection remains inside the PMU module.
 */
export module bioseq.instrumentation;

export import bioseq.instrumentation.configuration;
export import bioseq.instrumentation.descriptor;
export import bioseq.instrumentation.aggregation;
export import bioseq.instrumentation.profiler_state;
export import bioseq.instrumentation.region;
export import bioseq.instrumentation.loop;
export import bioseq.instrumentation.summary;
export import bioseq.instrumentation.clock;
export import bioseq.instrumentation.runtime;
export import bioseq.instrumentation.counter;
export import bioseq.instrumentation.gauge;
export import bioseq.instrumentation.arena;
export import bioseq.instrumentation.tracing;
export import bioseq.instrumentation.samples;
export import bioseq.instrumentation.pmu;
export import bioseq.instrumentation.sampler;
export import bioseq.instrumentation.summary_writer;
