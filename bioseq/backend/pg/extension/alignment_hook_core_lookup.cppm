/**
 * @file alignment_hook_core_lookup.cppm
 * @brief Find a query's alignment frame by QueryDesc or EState.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "executor/executor.h"
}

#include <vector>

module bioseq.postgres.extension:alignment_hook_core_lookup;

import :alignment_hook_contract;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

/**
 * @brief Finds the frame registered for a query descriptor.
 * @return The borrowed frame, or null when the descriptor is not registered.
 */
[[nodiscard]] alignment_executor_frame* find_alignment_frame(
		QueryDesc* query_desc) noexcept
{
	for (const alignment_query_registry_entry& entry : alignment_query_registry) {
		if (entry.key == query_desc) {
			return entry.frame;
		}
	}
	return nullptr;
}

/**
 * @brief Finds the frame registered for executor state.
 * @return The borrowed frame, or null when the executor state is not registered.
 */
[[nodiscard]] alignment_executor_frame* find_alignment_frame(
		EState* estate) noexcept
{
	for (const alignment_estate_registry_entry& entry : alignment_estate_registry) {
		if (entry.key == estate) {
			return entry.frame;
		}
	}
	return nullptr;
}

/**
 * @brief Register a frame under its QueryDesc and EState pointers.
 * Reject missing pointers and duplicate registrations. Cleanup remains the
 * caller's responsibility if registration raises an error.
 */
void register_alignment_frame(alignment_executor_frame* frame)
{
	if (frame == nullptr || frame->query_desc == nullptr || frame->estate == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq cannot register an incomplete executor frame")));
	}
	if (find_alignment_frame(frame->query_desc) != nullptr
		|| find_alignment_frame(frame->estate) != nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq executor frame registry contains a duplicate key")));
	}
	alignment_frames.push_back(frame);
	alignment_query_registry.push_back(
		alignment_query_registry_entry{frame->query_desc, frame});
	alignment_estate_registry.push_back(
		alignment_estate_registry_entry{frame->estate, frame});
}

} // namespace bioseq::pg::detail
