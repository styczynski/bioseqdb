/**
 * @file alignment_hook_core_stack.cppm
 * @brief PostgreSQL executor-hook alignment-frame stack helpers.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "executor/executor.h"
}

#include <algorithm>
#include <cstddef>
#include <vector>

module bioseq.postgres.extension:alignment_hook_core_stack;

namespace bioseq::pg::detail {

/** Executor frame stored by the backend-local lifecycle stacks. */
struct alignment_executor_frame;

/** @brief Removes every occurrence of a frame pointer from a lifecycle stack. */
void erase_frame_pointer(
		std::vector<alignment_executor_frame*>& frames,
		alignment_executor_frame* frame) noexcept
{
	frames.erase(
		std::remove(frames.begin(), frames.end(), frame),
		frames.end());
}

/** @brief Pops the expected top frame or reports a PostgreSQL lifecycle error. */
void pop_exact_alignment_stack(
		std::vector<alignment_executor_frame*>& stack,
		alignment_executor_frame* frame,
		const char* stack_name)
{
	if (stack.empty() || stack.back() != frame) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq executor lifecycle stack is not LIFO"),
			 errdetail("stack=%s depth=%zu", stack_name, stack.size())));
	}
	stack.pop_back();
}

/** @brief Removes a frame without raising while PostgreSQL unwinds an error. */
void pop_alignment_stack_on_error(
		std::vector<alignment_executor_frame*>& stack,
		alignment_executor_frame* frame) noexcept
{
	if (!stack.empty() && stack.back() == frame) {
		stack.pop_back();
		return;
	}
	erase_frame_pointer(stack, frame);
}

} // namespace bioseq::pg::detail
