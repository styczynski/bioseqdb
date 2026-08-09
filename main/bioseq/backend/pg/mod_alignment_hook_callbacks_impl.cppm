/**
 * @file mod_alignment_hook_callbacks_impl.cppm
 * @brief PostgreSQL executor Start/Finish callbacks and hook installation.
 */
module;

#include "common.h"

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <new>
#include <vector>

module bioseq.mod:alignment_hook_callbacks_impl;

import :alignment_hook_contract;
import bioseq.table.alignment_execution;

namespace bioseq::pg::detail {

namespace {
namespace alignment_exec = bioseq::pg::alignment_execution;

ExecutorStart_hook_type PreviousExecutorStartHook = nullptr;
ExecutorFinish_hook_type PreviousExecutorFinishHook = nullptr;

void call_previous_or_standard_executor_start(QueryDesc* query_desc, int eflags)
{
	if (PreviousExecutorStartHook != nullptr) {
		PreviousExecutorStartHook(query_desc, eflags);
	} else {
		standard_ExecutorStart(query_desc, eflags);
	}
}

void call_previous_or_standard_executor_finish(QueryDesc* query_desc)
{
	if (PreviousExecutorFinishHook != nullptr) {
		PreviousExecutorFinishHook(query_desc);
	} else {
		standard_ExecutorFinish(query_desc);
	}
}

void bioseq_alignment_executor_start(QueryDesc* query_desc, int eflags)
{
	auto* frame = new AlignmentExecutorFrame{};
	frame->query_desc = query_desc;
	if (!alignment_current_run.empty()) {
		AlignmentExecutorFrame* active = alignment_current_run.back();
		frame->nested = true;
		frame->collector.root = active != nullptr
			? active->collector.root
			: nullptr;
	} else {
		frame->collector.root = &frame->collector;
	}
	frame->collector_binding.root = frame->collector.root;
	frame->collector_binding.nested_alignment_invoked = frame->nested
		? &frame->nested_alignment_invoked
		: nullptr;
	alignment_start_in_progress.push_back(frame);
	MemoryContext saved_context = CurrentMemoryContext;
	PG_TRY();
	{
		call_previous_or_standard_executor_start(query_desc, eflags);
		pop_exact_alignment_stack(
			alignment_start_in_progress, frame, "ExecutorStart");
		frame->estate = query_desc != nullptr ? query_desc->estate : nullptr;
		if (frame->estate == nullptr) {
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bioseq ExecutorStart completed without an EState")));
		}
		ensure_alignment_frame_cleanup(frame, frame->estate);
		register_alignment_frame(frame);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		destroy_alignment_frame(frame);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

void bioseq_alignment_executor_finish(QueryDesc* query_desc)
{
	AlignmentExecutorFrame* frame = find_alignment_frame(query_desc);
	MemoryContext saved_context = CurrentMemoryContext;
	PG_TRY();
	{
		call_previous_or_standard_executor_finish(query_desc);
		if (frame != nullptr && !frame->nested) {
			publish_alignment_root(frame);
			retire_escaped_nested_bindings(frame);
		}
		retire_alignment_frame(frame);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		if (frame != nullptr && !frame->nested) {
			retire_escaped_nested_bindings(frame);
		}
		retire_alignment_frame(frame);
		PG_RE_THROW();
	}
	PG_END_TRY();
}
} // namespace

void install_alignment_execution_hooks()
{
	PreviousExecutorStartHook = ExecutorStart_hook;
	PreviousExecutorRunHook = ExecutorRun_hook;
	PreviousExecutorFinishHook = ExecutorFinish_hook;
	ExecutorStart_hook = bioseq_alignment_executor_start;
	ExecutorRun_hook = bioseq_alignment_executor_run;
	ExecutorFinish_hook = bioseq_alignment_executor_finish;
	alignment_exec::install_alignment_node_bind_callback(
		bind_alignment_node_to_starting_frame);
}

} // namespace bioseq::pg::detail
