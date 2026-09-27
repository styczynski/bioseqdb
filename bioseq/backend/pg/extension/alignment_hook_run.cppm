/**
 * @file alignment_hook_run.cppm
 * @brief Track running alignment queries and whether their output was consumed.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <cstdint>
#include <vector>

module bioseq.postgres.extension:alignment_hook_run;

import :alignment_hook_contract;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

namespace {

/**
 * @brief Check whether, a forward run reached the end of its output.
 *
 * A zero count requests all output. With a row limit, fewer returned tuples
 * than requested establishes completion. Reaching the limit leaves completion
 * unknown. Backward scans do not establish forwad  completion.
 */
[[nodiscard]] constexpr bool executor_run_output_fully_drained(
		const ScanDirection direction,
		const std::uint64_t count,
		const std::uint64_t processed) noexcept {
	return direction == ForwardScanDirection
		&& (count == 0 || processed < count);
}
static_assert(executor_run_output_fully_drained(
	ForwardScanDirection, 0, 4));
static_assert(executor_run_output_fully_drained(
	ForwardScanDirection, 4, 3));
static_assert(!executor_run_output_fully_drained(
	ForwardScanDirection, 1, 1));
static_assert(!executor_run_output_fully_drained(
	BackwardScanDirection, 0, 4));

} // namespace

ExecutorRun_hook_type previous_executor_run_hook = nullptr;
std::vector<alignment_executor_frame*> alignment_current_run{};

namespace {

/** @brief Invokes the previous ExecutorRun hook or PostgreSQL's standard implementation. */
void call_previous_or_standard_executor_run(
		QueryDesc* query_desc,
		ScanDirection direction,
		uint64 count
#if PG_VERSION_NUM < PG_VERSION_18
		, bool execute_once
#endif
)
{
	if (previous_executor_run_hook != nullptr) {
		previous_executor_run_hook(query_desc, direction, count
#if PG_VERSION_NUM < PG_VERSION_18
			, execute_once
#endif
		);
	} else {
		standard_ExecutorRun(query_desc, direction, count
#if PG_VERSION_NUM < PG_VERSION_18
			, execute_once
#endif
		);
	}
}

} // namespace

/**
 * @brief Run PostgreSQL execution while tracking the active alignment frame.
 * Record whether this call consumed all forward output. Remove the frame
 * from the active-call stack on success or error.
 * @param[in,out] query_desc Query being executed.
 * @param[in] direction Requested scan direction.
 * @param[in] count Maximum output tuples, or zero for all tuples.
 * @param[in] execute_once Hint passed through on PostgreSQL 17.
 * PostgreSQL 18 has no execute_once argument
 */
void bioseq_alignment_executor_run(
		QueryDesc* query_desc,
		ScanDirection direction,
		uint64 count
#if PG_VERSION_NUM < PG_VERSION_18
		, bool execute_once
#endif
)
{
	alignment_executor_frame* frame = find_alignment_frame(query_desc);
	if (frame == nullptr) {
		call_previous_or_standard_executor_run(
			query_desc, direction, count
#if PG_VERSION_NUM < PG_VERSION_18
			, execute_once
#endif
		);
		return;
	}
	alignment_current_run.push_back(frame);
	MemoryContext saved_context = CurrentMemoryContext;
	PG_TRY();
	{
		call_previous_or_standard_executor_run(
			query_desc, direction, count
#if PG_VERSION_NUM < PG_VERSION_18
			, execute_once
#endif
		);
		frame->query_output_fully_drained =
			query_desc != nullptr && query_desc->estate != nullptr
			&& executor_run_output_fully_drained(
				direction,
				static_cast<std::uint64_t>(count),
				static_cast<std::uint64_t>(
					query_desc->estate->es_processed));
		pop_exact_alignment_stack(
			alignment_current_run, frame, "ExecutorRun");
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(saved_context);
		pop_alignment_stack_on_error(alignment_current_run, frame);
		PG_RE_THROW();
	}
	PG_END_TRY();
}

} // namespace bioseq::pg::detail
