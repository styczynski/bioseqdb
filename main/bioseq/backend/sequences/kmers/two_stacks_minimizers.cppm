/**
 * @file two_stacks_minimizers.cppm
 * @brief Target-neutral compatibility facade for generated minimizer kernels.
 *
 * Canonical Highway bodies are owned exclusively by the annotated
 * `two_stack_nthash_kernel.cppm` and `paper_nthash_kernel.cppm` generator
 * inputs. Production reaches them through the once-selected function table.
 */
module;

export module bioseq.sequences.kmers.two_stacks;

export import bioseq.sequences.kmers.highway_contract;

export namespace bioseq::simd {

using ::bioseq::minimizers::two_stack_max_window;
using ::bioseq::minimizers::two_stack_max_kmer;
using ::bioseq::minimizers::two_stack_exact_max_kmer;
using ::bioseq::minimizers::TwoStackContinuationStatus;
using ::bioseq::minimizers::ExactSimdKernelKind;
using ::bioseq::minimizers::two_stack_status_name;
using ::bioseq::minimizers::TwoStackExecutionProof;
using ::bioseq::minimizers::TwoStackFinalizeResult;
using ::bioseq::minimizers::PackedPaperNthashStatus;
using ::bioseq::minimizers::packed_paper_nthash_scheme_id;
using ::bioseq::minimizers::packed_paper_nthash_max_window;
using ::bioseq::minimizers::PackedPaperNthashExecutionProof;
using ::bioseq::minimizers::PackedPaperNthashResult;
using ::bioseq::minimizers::ExactSimdRuntimeError;
using ::bioseq::minimizers::exact_simd_minimizer_representation;

} // namespace bioseq::simd
