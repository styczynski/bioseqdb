/**
 * @file alignment_hook_core_retirement.cppm
 * @brief PostgreSQL executor-hook escaped-binding retirement helper.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "executor/executor.h"
}

#include <vector>

module bioseq.postgres.extension:alignment_hook_core_retirement;

import :alignment_hook_contract;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

/**
 * @brief Invalidates nested collector bindings that outlive their root frame.
 * @param[in,out] root Root frame whose collector is being retired.
 */
void retire_escaped_nested_bindings(alignment_executor_frame* root) noexcept
{
	if (root == nullptr
		|| root->collector.root_collector != &root->collector) {
		return;
	}
	for (alignment_executor_frame* candidate : alignment_frames) {
		if (candidate == nullptr || candidate == root) {
			continue;
		}
		if (candidate->collector_binding.root_collector == &root->collector) {
			candidate->collector_binding.root_collector = nullptr;
			candidate->collector_binding.is_retired = true;
		}
	}
}

} // namespace bioseq::pg::detail
