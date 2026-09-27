/**
 * @file minimizer_sequence_index/generation/index.cppm
 * @brief Exports the code that spills minimizer runs and replays them to build an indjx.
 */
export module bioseq.indexing.minimizer_sequence.generation;

export import :replay_contract;
export import :operations;

static_assert(__cplusplus >= 202302L, "C++23-or-later");
