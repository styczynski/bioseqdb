/**
 * @file alignment_hook_callbacks.cppm
 * @brief Start and finish collection of alignment statistics for a query.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <new>
#include <vector>

module bioseq.postgres.extension:alignment_hook_callbacks;

import :alignment_hook_contract;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

namespace {
namespace alignment_exec = bioseq::postgres::table::alignment_execution;

ExecutorStart_hook_type previous_executor_start_hook = nullptr;
ExecutorFinish_hook_type previous_executor_finish_hook = nullptr;

/** @brief Invokes the previous ExecutorStart hook or PostgreSQL's standard implementation. */
void call_previous_or_standard_executor_start(QueryDesc* query_desc, int eflags)
{
	if (previous_executor_start_hook != nullptr) {
		previous_executor_start_hook(query_desc, eflags);
	} else {
		standard_ExecutorStart(query_desc, eflags);
	}
}

/** @brief Invokes the previous ExecutorFinish hook or PostgreSQL's standard implementation. */
void call_previous_or_standard_executor_finish(QueryDesc* query_desc)
{
	if (previous_executor_finish_hook != nullptr) {
		previous_executor_finish_hook(query_desc);
	} else {
		standard_ExecutorFinish(query_desc);
	}
}

/** @brief Create a frame before starting PostgreSQL execution.
 * Nested queries share the running statement's collector. Register the frame
 * after ExecutorStart supplies its EState, and destroy it if startup fails.
 */
void bioseq_alignment_executor_start(QueryDesc* query_desc, int eflags)
{
	auto* frame = new alignment_executor_frame{};
	frame->query_desc = query_desc;
	if (!alignment_current_run.empty()) {
		alignment_executor_frame* active = alignment_current_run.back();
		frame->nested = true;
		frame->collector.root_collector = active != nullptr
			? active->collector.root_collector
			: nullptr;
	} else {
		frame->collector.root_collector = &frame->collector;
	}
	frame->collector_binding.root_collector = frame->collector.root_collector;
	frame->collector_binding.nested_executor_invoked = frame->nested
		? &frame->nested_executor_invoked
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

/** @brief Finish PostgreSQL execution and publish the root query's statistics.
 * Remove the frame on success or error. A failed finish does not publish results.
 */
void bioseq_alignment_executor_finish(QueryDesc* query_desc)
{
	alignment_executor_frame* frame = find_alignment_frame(query_desc);
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

/** @brief Install Start, Run and Finish hooks, preserving the previous hooks.
 * Also register the callback that connects alignment scans to their frames.
 */
void install_alignment_execution_hooks()
{
	previous_executor_start_hook = ExecutorStart_hook;
	previous_executor_run_hook = ExecutorRun_hook;
	previous_executor_finish_hook = ExecutorFinish_hook;
	ExecutorStart_hook = bioseq_alignment_executor_start;
	ExecutorRun_hook = bioseq_alignment_executor_run;
	ExecutorFinish_hook = bioseq_alignment_executor_finish;
	alignment_exec::install_alignment_node_binder(
		bind_alignment_node_to_starting_frame);
}

} // namespace bioseq::pg::detail
