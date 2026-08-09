/**
 * @file mod_alignment_hook_core_stack_impl.cppm
 * @brief PostgreSQL executor-hook alignment-frame stack helpers.
 */
module;

#include "common.h"

extern "C" {
#include "executor/executor.h"
}

#include <algorithm>
#include <cstddef>
#include <vector>

module bioseq.mod:alignment_hook_core_stack_impl;

namespace bioseq::pg::detail {

struct AlignmentExecutorFrame;

void erase_frame_pointer(
		std::vector<AlignmentExecutorFrame*>& frames,
		AlignmentExecutorFrame* frame) noexcept
{
	frames.erase(
		std::remove(frames.begin(), frames.end(), frame),
		frames.end());
}

void pop_exact_alignment_stack(
		std::vector<AlignmentExecutorFrame*>& stack,
		AlignmentExecutorFrame* frame,
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

void pop_alignment_stack_on_error(
		std::vector<AlignmentExecutorFrame*>& stack,
		AlignmentExecutorFrame* frame) noexcept
{
	if (!stack.empty() && stack.back() == frame) {
		stack.pop_back();
		return;
	}
	erase_frame_pointer(stack, frame);
}

} // namespace bioseq::pg::detail
