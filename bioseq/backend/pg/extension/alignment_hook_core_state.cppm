/**
 * @file alignment_hook_core_state.cppm
 * @brief Backend-local lists of active alignment query frames.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "executor/executor.h"
}

#include <vector>

module bioseq.postgres.extension:alignment_hook_core_state;

import :alignment_hook_contract;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

std::vector<alignment_executor_frame*> alignment_frames{};
std::vector<alignment_query_registry_entry> alignment_query_registry{};
std::vector<alignment_estate_registry_entry> alignment_estate_registry{};
std::vector<alignment_executor_frame*> alignment_start_in_progress{};

} // namespace bioseq::pg::detail
