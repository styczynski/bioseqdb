/**
 * @file two_stack_minimizers.cppm
 * @brief Exports types used to call generated minimizer kernels.
 * CPU-specific implementations are reached through the selected function table.
 */
module;

export module bioseq.sequences.kmers.two_stack_minimizers;

export import bioseq.sequences.kmers.highway_minimizer_contract;

/** @brief Makes minimizer limits, status types and counters available in bioseq::simd. */
export namespace bioseq::simd {

/** Largest window size compiled into two-stack minimizer kernels. */
using ::bioseq::minimizers::k_max_two_stack_window;
/** General two-stack k-mer limit. Exact SIMD kernels use the separate exact limit. */
using ::bioseq::minimizers::k_max_two_stack_kmer;
/** Largest k-mer size accepted by exact SIMD two-stack kernels. */
using ::bioseq::minimizers::k_max_exact_two_stack_kmer;
/** Status of an incremental two-stack minimizer call. */
using ::bioseq::minimizers::two_stack_continuation_status;
/** Two-stack implementation recorded by the executed kernel. */
using ::bioseq::minimizers::exact_simd_kernel;
/** Returns the stable diagnostic name for a continuation status. */
using ::bioseq::minimizers::two_stack_continuation_status_name;
/** Work counters for two-stack minimizer algorithms. */
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
/** Exception carrying a rejected two-stack SIMD operation's status. */
using ::bioseq::minimizers::exact_simd_runtime_error;
/** Requires a 64-bit minimizer record with packed-word access and field masks. */
using ::bioseq::minimizers::exact_simd_minimizer_representation;

} // namespace bioseq::simd
