/**
 * @file mod_alignment_hook_contract.cppm
 * @brief Private executor-hook frame contract shared by implementation partitions.
 */
module;

#include "common.h"

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <cstdint>
#include <vector>

module bioseq.mod:alignment_hook_contract;

import :alignment_hook_core_stack_impl;
import bioseq.table.alignment_execution;

namespace bioseq::pg::detail {

struct AlignmentExecutorFrame;

/* PostgreSQL 17 has no reset-callback unregister API. */
struct AlignmentFrameCleanupToken {
	MemoryContextCallback callback{};
	AlignmentExecutorFrame* frame = nullptr;
};

struct AlignmentExecutorFrame {
	QueryDesc* query_desc = nullptr;
	EState* estate = nullptr;
	bioseq::pg::alignment_execution::AlignmentExecutionCollector collector{};
	bioseq::pg::alignment_execution::AlignmentCollectorBinding collector_binding{};
	AlignmentFrameCleanupToken* cleanup_token = nullptr;
	bool nested = false;
	bool nested_alignment_invoked = false;
	bool query_output_fully_drained = false;
	bool finished = false;
};

struct AlignmentQueryRegistryEntry {
	QueryDesc* key = nullptr;
	AlignmentExecutorFrame* frame = nullptr;
};

struct AlignmentEstateRegistryEntry {
	EState* key = nullptr;
	AlignmentExecutorFrame* frame = nullptr;
};

extern ExecutorRun_hook_type PreviousExecutorRunHook;
extern std::vector<AlignmentExecutorFrame*> alignment_frames;
extern std::vector<AlignmentQueryRegistryEntry> alignment_query_registry;
extern std::vector<AlignmentEstateRegistryEntry> alignment_estate_registry;
extern std::vector<AlignmentExecutorFrame*> alignment_start_in_progress;
extern std::vector<AlignmentExecutorFrame*> alignment_current_run;

void erase_frame_pointer(
	std::vector<AlignmentExecutorFrame*>& frames,
	AlignmentExecutorFrame* frame) noexcept;

void unlink_alignment_frame(AlignmentExecutorFrame* frame) noexcept;
void destroy_alignment_frame(AlignmentExecutorFrame* frame) noexcept;
void ensure_alignment_frame_cleanup(
	AlignmentExecutorFrame* frame,
	EState* estate);
[[nodiscard]] AlignmentExecutorFrame* find_alignment_frame(
	QueryDesc* query_desc) noexcept;
[[nodiscard]] AlignmentExecutorFrame* find_alignment_frame(
	EState* estate) noexcept;
void register_alignment_frame(AlignmentExecutorFrame* frame);
void pop_exact_alignment_stack(
	std::vector<AlignmentExecutorFrame*>& stack,
	AlignmentExecutorFrame* frame,
	const char* stack_name);
void pop_alignment_stack_on_error(
	std::vector<AlignmentExecutorFrame*>& stack,
	AlignmentExecutorFrame* frame) noexcept;
[[nodiscard]] bioseq::pg::alignment_execution::AlignmentNodeRegistrationHandle
bind_alignment_node_to_starting_frame(
	EState* estate,
	std::uint32_t scan_node_id);
void retire_escaped_nested_bindings(AlignmentExecutorFrame* root) noexcept;
void retire_alignment_frame(AlignmentExecutorFrame* frame) noexcept;

void publish_alignment_root(AlignmentExecutorFrame* frame);

void bioseq_alignment_executor_run(
	QueryDesc* query_desc,
	ScanDirection direction,
	uint64 count,
	bool execute_once);

} // namespace bioseq::pg::detail
