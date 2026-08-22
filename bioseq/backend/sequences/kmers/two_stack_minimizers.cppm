/**
 * @file two_stack_minimizers.cppm
 * @brief Target-neutral facade for generated minimizer kernels.
 *
 * @details Re-exports the target-neutral minimizer contract without wrappers. Canonical Highway
 * bodies remain in the annotated kernel inputs, and production reaches their exact emitted target
 * through the once-selected function table.
 */
module;

export module bioseq.sequences.kmers.two_stack_minimizers;

export import bioseq.sequences.kmers.highway_minimizer_contract;

/**
 * @brief Re-exports minimizer limits, status types, statistics, and runtime errors.
 *
 * Using-declarations preserve the original callable and type identities. This facade adds no
 * allocation, dispatch, or ownership layer.
 */
export namespace bioseq::simd {

/** Largest window size compiled into two-stack minimizer kernels. */
using ::bioseq::minimizers::k_max_two_stack_window;
/** Largest k-mer size accepted by two-stack minimizer kernels. */
using ::bioseq::minimizers::k_max_two_stack_kmer;
/** Largest k-mer size accepted by exact SIMD two-stack kernels. */
using ::bioseq::minimizers::k_max_exact_two_stack_kmer;
/** Incremental two-stack minimizer continuation outcome. */
using ::bioseq::minimizers::two_stack_continuation_status;
/** Operation-originated two_stack_nthash kernel identity. */
using ::bioseq::minimizers::exact_simd_kernel;
/** Returns the stable diagnostic name for a continuation status. */
using ::bioseq::minimizers::two_stack_continuation_status_name;
/** Operation-derived counters for two-stack minimizer algorithms. */
using ::bioseq::minimizers::two_stack_execution_stats;
/** Final status and counters from a two-stack continuation. */
using ::bioseq::minimizers::two_stack_finalize_result;
/** Outcome of one bounded packed ntHash execution. */
using ::bioseq::minimizers::bounded_nthash_status;
/** Persisted scheme identity for bounded ntHash minimizers. */
using ::bioseq::minimizers::k_bounded_nthash_scheme_id;
/** Maximum window size compiled into bounded ntHash kernels. */
using ::bioseq::minimizers::k_bounded_nthash_max_window;
/** Counters recorded for one bounded ntHash execution. */
using ::bioseq::minimizers::bounded_nthash_execution_stats;
/** Bounded ntHash status paired with its execution counters. */
using ::bioseq::minimizers::bounded_nthash_result;
/** Strict rejection exception for exact-SIMD continuation failures. */
using ::bioseq::minimizers::exact_simd_runtime_error;
/** Requires the native uint64 minimizer surface used by SIMD sinks. */
using ::bioseq::minimizers::exact_simd_minimizer_representation;

} // namespace bioseq::simd
