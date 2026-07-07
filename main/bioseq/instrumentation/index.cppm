/**
 * @file instrumentation/mod.cppm
 * @brief Public entry point. Re-exports every user-facing primitive.
 *
 * Other modules in the bioseq tree write `import bioseq_instr;` and get
 * the full public API. Implementation-private modules (PMU backends, the
 * sampler) are imported but not exported.
 */
export module bioseq.instr;

export import bioseq.instr.config;
export import bioseq.instr.descriptor;
export import bioseq.instr.aggregate;
export import bioseq.instr.thread_state;
export import bioseq.instr.region;
export import bioseq.instr.loop;
export import bioseq.instr.access;
export import bioseq.instr.clock;
export import bioseq.instr.global_state;
export import bioseq.instr.histogram;
export import bioseq.instr.counter;
export import bioseq.instr.gauge;
export import bioseq.instr.arena;
export import bioseq.instr.trace;
export import bioseq.instr.sample;
export import bioseq.instr.pool;
export import bioseq.instr.pmu;
export import bioseq.instr.sampler;
export import bioseq.instr.detail.summary_writer;

// DOC0276 — the system-memory probe (process footprint / available / total)
// is part of the public surface: the production extension and the bench
// MemoryBudget both consult it, so it is exported rather than detail-private.
export import bioseq.instr.detail.sysinfo;

// Detail modules — imported (not exported) so the symbols they declare
// are reachable inside bioseq_instr translation units without leaking
// into callers that just write `import bioseq_instr;`.
import bioseq.instr.detail.siglock;
import bioseq.instr.detail.spinlock;
import bioseq.instr.detail.arch;
import bioseq.instr.detail.do_not_optimize;
import bioseq.instr.pmu.linux_backend_mod;
import bioseq.instr.pmu.macos;
import bioseq.instr.pmu.stub;
import bioseq.instr.sampler.posix;
import bioseq.instr.sampler.stub;
