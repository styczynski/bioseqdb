/**
 * @file instrumentation/mod.cppm
 * @brief Public entry point. Re-exports every user-facing primitive.
 *
 * Other modules in the bioseq tree write `import bioseq_instr;` and get
 * the full public API. Implementation-private modules (PMU backends, the
 * sampler) are imported but not exported.
 */
export module bioseq_instr;

export import bioseq_instr.config;
export import bioseq_instr.descriptor;
export import bioseq_instr.aggregate;
export import bioseq_instr.thread_state;
export import bioseq_instr.region;
export import bioseq_instr.loop;
export import bioseq_instr.access;
export import bioseq_instr.clock;
export import bioseq_instr.global_state;
