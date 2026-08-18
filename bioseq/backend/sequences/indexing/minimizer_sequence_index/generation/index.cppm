/**
 * @file minimizer_sequence_index/generation/index.cppm
 * @brief Standalone direct-minimizer spill generation and replay facade.
 */
export module bioseq.indexing.minimizer_sequence.generation;

export import :replay_contract;
export import :operations;

static_assert(__cplusplus >= 202302L, "C++23-or-later");
