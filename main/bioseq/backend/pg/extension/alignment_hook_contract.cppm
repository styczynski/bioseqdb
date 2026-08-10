/**
 * @file alignment_hook_contract.cppm
 * @brief Private executor-hook frame contract shared by implementation partitions.
 */
module;

#include <bioseq/pg_compat.h>

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <cstdint>
#include <vector>

module bioseq.postgres.extension:alignment_hook_contract;

import :alignment_hook_core_stack;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

struct alignment_executor_frame;

/* PostgreSQL 17 has no reset-callback unregister API. */
struct alignment_frame_cleanup_token {
	MemoryContextCallback callback{};
	alignment_executor_frame* frame = nullptr;
};

struct alignment_executor_frame {
	QueryDesc* query_desc = nullptr;
	EState* estate = nullptr;
	bioseq::postgres::table::alignment_execution::alignment_execution_collector collector{};
	bioseq::postgres::table::alignment_execution::alignment_execution_binding collector_binding{};
	alignment_frame_cleanup_token* cleanup_token = nullptr;
	bool nested = false;
	bool nested_executor_invoked = false;
	bool query_output_fully_drained = false;
	bool finished = false;
};

struct alignment_query_registry_entry {
	QueryDesc* key = nullptr;
	alignment_executor_frame* frame = nullptr;
};

struct alignment_estate_registry_entry {
	EState* key = nullptr;
	alignment_executor_frame* frame = nullptr;
};

extern ExecutorRun_hook_type PreviousExecutorRunHook;
extern std::vector<alignment_executor_frame*> alignment_frames;
extern std::vector<alignment_query_registry_entry> alignment_query_registry;
extern std::vector<alignment_estate_registry_entry> alignment_estate_registry;
extern std::vector<alignment_executor_frame*> alignment_start_in_progress;
extern std::vector<alignment_executor_frame*> alignment_current_run;

void erase_frame_pointer(
	std::vector<alignment_executor_frame*>& frames,
	alignment_executor_frame* frame) noexcept;

void unlink_alignment_frame(alignment_executor_frame* frame) noexcept;
void destroy_alignment_frame(alignment_executor_frame* frame) noexcept;
void ensure_alignment_frame_cleanup(
	alignment_executor_frame* frame,
	EState* estate);
[[nodiscard]] alignment_executor_frame* find_alignment_frame(
	QueryDesc* query_desc) noexcept;
[[nodiscard]] alignment_executor_frame* find_alignment_frame(
	EState* estate) noexcept;
void register_alignment_frame(alignment_executor_frame* frame);
void pop_exact_alignment_stack(
	std::vector<alignment_executor_frame*>& stack,
	alignment_executor_frame* frame,
	const char* stack_name);
void pop_alignment_stack_on_error(
	std::vector<alignment_executor_frame*>& stack,
	alignment_executor_frame* frame) noexcept;
[[nodiscard]] bioseq::postgres::table::alignment_execution::alignment_node_registration_handle
bind_alignment_node_to_starting_frame(
	EState* estate,
	std::uint32_t scan_node_id);
void retire_escaped_nested_bindings(alignment_executor_frame* root) noexcept;
void retire_alignment_frame(alignment_executor_frame* frame) noexcept;

void publish_alignment_root(alignment_executor_frame* frame);

void bioseq_alignment_executor_run(
	QueryDesc* query_desc,
	ScanDirection direction,
	uint64 count,
	bool execute_once);

} // namespace bioseq::pg::detail
