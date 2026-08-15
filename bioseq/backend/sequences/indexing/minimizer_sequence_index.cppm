/**
 * @file minimizer_sequence_index.cppm
 * @brief Public facade for the direct minimizer sequence index.
 */
export module bioseq.indexing.minimizer_sequence;

export import :bucket_codec;
export import :admitted_runs;
export import :replay_build;
export import :read_assembly;

static_assert(__cplusplus >= 202302L, "C++23-or-later");
