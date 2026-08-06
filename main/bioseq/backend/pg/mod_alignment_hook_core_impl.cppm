/**
 * @file mod_alignment_hook_core_impl.cppm
 * @brief PostgreSQL executor-hook lifecycle and alignment-frame ownership.
 */
module;

#include "common.h"

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

module bioseq.mod;

import bioseq.table.scan_state;

namespace bioseq::pg::detail {

namespace {
namespace alignment_exec = bioseq::pg::alignment_execution;

/* count==0 is PostgreSQL's unbounded ExecutorRun contract.  A bounded run is
 * known to have reached EOF only when it produced fewer tuples than requested;
 * producing exactly count rows may have stopped at an upper portal boundary. */
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

ExecutorRun_hook_type PreviousExecutorRunHook = nullptr;

namespace {

std::vector<AlignmentExecutorFrame*> alignment_frames{};
struct AlignmentQueryRegistryEntry {
	QueryDesc* key = nullptr;
	AlignmentExecutorFrame* frame = nullptr;
};
struct AlignmentEstateRegistryEntry {
	EState* key = nullptr;
	AlignmentExecutorFrame* frame = nullptr;
};
std::vector<AlignmentQueryRegistryEntry> alignment_query_registry{};
std::vector<AlignmentEstateRegistryEntry> alignment_estate_registry{};

} // namespace

std::vector<AlignmentExecutorFrame*> alignment_start_in_progress{};
std::vector<AlignmentExecutorFrame*> alignment_current_run{};

namespace {

template<class T>
void erase_frame_pointer(std::vector<T*>& frames, T* frame) noexcept
{
	frames.erase(
		std::remove(frames.begin(), frames.end(), frame),
		frames.end());
}

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

} // namespace

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

[[nodiscard]] AlignmentExecutorFrame* find_alignment_frame(
		QueryDesc* query_desc) noexcept
{
	for (const AlignmentQueryRegistryEntry& entry : alignment_query_registry) {
		if (entry.key == query_desc) {
			return entry.frame;
		}
	}
	return nullptr;
}

[[nodiscard]] AlignmentExecutorFrame* find_alignment_frame(
		EState* estate) noexcept
{
	for (const AlignmentEstateRegistryEntry& entry : alignment_estate_registry) {
		if (entry.key == estate) {
			return entry.frame;
		}
	}
	return nullptr;
}

void register_alignment_frame(AlignmentExecutorFrame* frame)
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
		AlignmentQueryRegistryEntry{frame->query_desc, frame});
	alignment_estate_registry.push_back(
		AlignmentEstateRegistryEntry{frame->estate, frame});
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

namespace {

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

} // namespace

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

namespace {

void call_previous_or_standard_executor_run(
		QueryDesc* query_desc,
		ScanDirection direction,
		uint64 count,
		bool execute_once)
{
	if (PreviousExecutorRunHook != nullptr) {
		PreviousExecutorRunHook(query_desc, direction, count, execute_once);
	} else {
		standard_ExecutorRun(query_desc, direction, count, execute_once);
	}
}

} // namespace

void bioseq_alignment_executor_run(
		QueryDesc* query_desc,
		ScanDirection direction,
		uint64 count,
		bool execute_once)
{
	AlignmentExecutorFrame* frame = find_alignment_frame(query_desc);
	if (frame == nullptr) {
		call_previous_or_standard_executor_run(
			query_desc, direction, count, execute_once);
		return;
	}
	alignment_current_run.push_back(frame);
	MemoryContext saved_context = CurrentMemoryContext;
	PG_TRY();
	{
		call_previous_or_standard_executor_run(
			query_desc, direction, count, execute_once);
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
