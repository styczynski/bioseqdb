/**
 * @file instrumentation/index.cppm
 * @brief Public entry point. Re-exports every user-facing primitive.
 *
 * Other modules import `bioseq.instrumentation` to receive the stable public
 * instrumentation surface. The facade preserves each leaf module's ownership.
 * It adds no state, initialization, or runtime dispatch of its own.
 *
 * @note Re-export order follows the public dependency-facing API order and
 * is intentionally independent of backend selection inside the leaf modules.
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
