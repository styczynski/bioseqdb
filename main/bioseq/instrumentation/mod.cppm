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
export import bioseq_instr.histogram;
export import bioseq_instr.counter;
export import bioseq_instr.gauge;
export import bioseq_instr.arena;
export import bioseq_instr.trace;
export import bioseq_instr.sample;
export import bioseq_instr.pool;
export import bioseq_instr.pmu;
export import bioseq_instr.sampler;
export import bioseq_instr.detail.summary_writer;

// Detail modules — imported (not exported) so the symbols they declare
// are reachable inside bioseq_instr translation units without leaking
// into callers that just write `import bioseq_instr;`.
import bioseq_instr.detail.siglock;
import bioseq_instr.detail.spinlock;
import bioseq_instr.detail.arch;
import bioseq_instr.detail.sysinfo;
import bioseq_instr.detail.do_not_optimize;
import bioseq_instr.pmu.linux;
import bioseq_instr.pmu.macos;
import bioseq_instr.pmu.stub;
import bioseq_instr.sampler.posix;
import bioseq_instr.sampler.stub;
