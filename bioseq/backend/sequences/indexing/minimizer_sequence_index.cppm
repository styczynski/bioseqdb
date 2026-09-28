/**
 * @file minimizer_sequence_index.cppm
 * @brief Exports the direct minimizer sequence index and its build and read modules.
 */
export module bioseq.indexing.minimizer_sequence;

export import bioseq.indexing.minimizer_sequence.bucket_codec;
export import bioseq.indexing.minimizer_sequence.admitted_runs;
export import bioseq.indexing.minimizer_sequence.generation;
export import :read_assembly;

static_assert(__cplusplus >= 202302L, "C++23-or-later");
