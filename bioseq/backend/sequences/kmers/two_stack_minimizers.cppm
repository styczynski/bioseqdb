/**
 * @file two_stack_minimizers.cppm
 * @brief Target-neutral facade for generated minimizer kernels.
 *
 * Canonical Highway bodies are owned exclusively by the annotated
 * `two_stack_nthash_kernel.cppm` and `bounded_nthash_kernel.cppm` generator
 * inputs. Production reaches them through the once-selected function table.
 */
module;

export module bioseq.sequences.kmers.two_stack_minimizers;

export import bioseq.sequences.kmers.highway_minimizer_contract;

export namespace bioseq::simd {

using ::bioseq::minimizers::k_max_two_stack_window;
using ::bioseq::minimizers::k_max_two_stack_kmer;
using ::bioseq::minimizers::k_max_exact_two_stack_kmer;
using ::bioseq::minimizers::two_stack_continuation_status;
using ::bioseq::minimizers::exact_simd_kernel;
using ::bioseq::minimizers::two_stack_continuation_status_name;
using ::bioseq::minimizers::two_stack_execution_stats;
using ::bioseq::minimizers::two_stack_finalize_result;
using ::bioseq::minimizers::bounded_nthash_status;
using ::bioseq::minimizers::k_bounded_nthash_scheme_id;
using ::bioseq::minimizers::k_bounded_nthash_max_window;
using ::bioseq::minimizers::bounded_nthash_execution_stats;
using ::bioseq::minimizers::bounded_nthash_result;
using ::bioseq::minimizers::exact_simd_runtime_error;
using ::bioseq::minimizers::exact_simd_minimizer_representation;

} // namespace bioseq::simd
