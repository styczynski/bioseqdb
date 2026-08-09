/**
 * @file alignment_hook_binding.cppm
 * @brief PostgreSQL executor-hook alignment-frame binding and cleanup lifecycle.
 */
module;

#include "common.h"

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <cstdint>

module bioseq.postgres.extension:alignment_hook_binding;

import :alignment_hook_contract;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

namespace alignment_exec = bioseq::pg::alignment_execution;

void destroy_alignment_frame(AlignmentExecutorFrame* frame) noexcept
{
	if (frame == nullptr) {
		return;
	}
	retire_escaped_nested_bindings(frame);
	unlink_alignment_frame(frame);
	if (frame->cleanup_token != nullptr) {
		frame->cleanup_token->frame = nullptr;
		frame->cleanup_token = nullptr;
	}
	delete frame;
}

namespace {

void alignment_frame_context_cleanup(void* argument)
{
	auto* token = static_cast<AlignmentFrameCleanupToken*>(argument);
	if (token == nullptr || token->frame == nullptr) {
		return;
	}
	AlignmentExecutorFrame* frame = token->frame;
	token->frame = nullptr;
	frame->cleanup_token = nullptr;
	retire_escaped_nested_bindings(frame);
	unlink_alignment_frame(frame);
	delete frame;
}

} // namespace

void ensure_alignment_frame_cleanup(
		AlignmentExecutorFrame* frame,
		EState* estate)
{
	if (frame == nullptr || estate == nullptr || estate->es_query_cxt == nullptr
		|| frame->cleanup_token != nullptr) {
		return;
	}
	auto* token = static_cast<AlignmentFrameCleanupToken*>(
		MemoryContextAllocZero(
			estate->es_query_cxt, sizeof(AlignmentFrameCleanupToken)));
	token->frame = frame;
	token->callback.func = alignment_frame_context_cleanup;
	token->callback.arg = token;
	MemoryContextRegisterResetCallback(
		estate->es_query_cxt, &token->callback);
	frame->cleanup_token = token;
}

[[nodiscard]] alignment_exec::AlignmentNodeRegistrationHandle
bind_alignment_node_to_starting_frame(
		EState* estate,
		std::uint32_t scan_node_id)
{
	if (estate == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq alignment node began outside ExecutorStart")));
	}
	AlignmentExecutorFrame* frame = nullptr;
	if (!alignment_start_in_progress.empty()) {
		frame = alignment_start_in_progress.back();
		if (frame == nullptr || frame->query_desc == nullptr
			|| frame->query_desc->estate != estate) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bioseq alignment node could not bind to its ExecutorStart frame")));
		}
		frame->estate = estate;
	} else {
		frame = find_alignment_frame(estate);
		if (frame == nullptr && estate->es_epq_active != nullptr) {
			EState* const parent_estate =
				estate->es_epq_active->parentestate;
			if (parent_estate != nullptr && parent_estate != estate) {
				frame = find_alignment_frame(parent_estate);
			}
		}
		if (frame == nullptr || frame->finished
			|| alignment_current_run.empty()
			|| alignment_current_run.back() != frame
			|| frame->collector_binding.retired
			|| frame->collector_binding.root == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bioseq alignment node began outside ExecutorStart")));
		}
	}
	ensure_alignment_frame_cleanup(frame, frame->estate);
	return alignment_exec::AlignmentNodeRegistrationHandle{
		.binding = &frame->collector_binding,
		.scan_node_id = scan_node_id,
	};
}

void retire_alignment_frame(AlignmentExecutorFrame* frame) noexcept
{
	if (frame == nullptr) {
		return;
	}
	retire_escaped_nested_bindings(frame);
	unlink_alignment_frame(frame);
	frame->finished = true;
	if (frame->cleanup_token == nullptr) {
		delete frame;
	}
}

} // namespace bioseq::pg::detail
