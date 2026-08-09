/**
 * @file mod_alignment_hook_core_retirement_impl.cppm
 * @brief PostgreSQL executor-hook escaped-binding retirement helper.
 */
module;

#include "common.h"

extern "C" {
#include "executor/executor.h"
}

#include <vector>

module bioseq.mod:alignment_hook_core_retirement_impl;

import :alignment_hook_contract;

namespace bioseq::pg::detail {

void retire_escaped_nested_bindings(AlignmentExecutorFrame* root) noexcept
{
	if (root == nullptr || root->collector.root != &root->collector) {
		return;
	}
	for (AlignmentExecutorFrame* candidate : alignment_frames) {
		if (candidate == nullptr || candidate == root) {
			continue;
		}
		if (candidate->collector_binding.root == &root->collector) {
			candidate->collector_binding.root = nullptr;
			candidate->collector_binding.retired = true;
		}
	}
}

} // namespace bioseq::pg::detail
