/**
 * @file alignment_hook_core_state.cppm
 * @brief PostgreSQL executor-hook alignment-frame registry state.
 */
module;

#include "common.h"

extern "C" {
#include "executor/executor.h"
}

#include <vector>

module bioseq.postgres.extension:alignment_hook_core_state;

import :alignment_hook_contract;

namespace bioseq::pg::detail {

std::vector<AlignmentExecutorFrame*> alignment_frames{};
std::vector<AlignmentQueryRegistryEntry> alignment_query_registry{};
std::vector<AlignmentEstateRegistryEntry> alignment_estate_registry{};
std::vector<AlignmentExecutorFrame*> alignment_start_in_progress{};

} // namespace bioseq::pg::detail
