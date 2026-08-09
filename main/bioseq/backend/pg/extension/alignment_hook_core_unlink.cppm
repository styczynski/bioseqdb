/**
 * @file alignment_hook_core_unlink.cppm
 * @brief PostgreSQL executor-hook alignment-frame unlink helpers.
 */
module;

#include "common.h"

extern "C" {
#include "executor/executor.h"
}

#include <algorithm>
#include <vector>

module bioseq.postgres.extension:alignment_hook_core_unlink;

import :alignment_hook_contract;

namespace bioseq::pg::detail {

void unlink_alignment_frame(AlignmentExecutorFrame* frame) noexcept
{
	if (frame == nullptr) {
		return;
	}
	erase_frame_pointer(alignment_frames, frame);
	erase_frame_pointer(alignment_start_in_progress, frame);
	erase_frame_pointer(alignment_current_run, frame);
	alignment_query_registry.erase(
		std::remove_if(
			alignment_query_registry.begin(), alignment_query_registry.end(),
			[frame](const AlignmentQueryRegistryEntry& entry) noexcept {
				return entry.frame == frame;
			}),
		alignment_query_registry.end());
	alignment_estate_registry.erase(
		std::remove_if(
			alignment_estate_registry.begin(), alignment_estate_registry.end(),
			[frame](const AlignmentEstateRegistryEntry& entry) noexcept {
				return entry.frame == frame;
			}),
		alignment_estate_registry.end());
}

} // namespace bioseq::pg::detail
