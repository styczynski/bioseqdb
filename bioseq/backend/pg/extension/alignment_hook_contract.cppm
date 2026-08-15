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

/**
 * @brief Keeps a query-context cleanup callback linked to its executor frame.
 *
 * PostgreSQL 17 cannot unregister reset callbacks, so frame retirement clears
 * the borrowed frame pointer before releasing the frame.
 */
struct alignment_frame_cleanup_token {
	MemoryContextCallback callback{};
	alignment_executor_frame* frame = nullptr;
};

/**
 * @brief Owns one ExecutorStart-to-ExecutorFinish alignment collection scope.
 *
 * The frame is backend-local. Its collector binding may be borrowed by scan
 * nodes until retirement clears or invalidates that binding.
 */
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

/** @brief Maps a PostgreSQL query descriptor to its active alignment frame. */
struct alignment_query_registry_entry {
	QueryDesc* key = nullptr;
	alignment_executor_frame* frame = nullptr;
};

/** @brief Maps executor state to its active alignment frame. */
struct alignment_estate_registry_entry {
	EState* key = nullptr;
	alignment_executor_frame* frame = nullptr;
};

/** @brief Previous ExecutorRun hook restored through the hook chain. */
extern ExecutorRun_hook_type previous_executor_run_hook;
/** @brief Backend-local owner list for active alignment frames. */
extern std::vector<alignment_executor_frame*> alignment_frames;
/** @brief Query-descriptor lookup entries for active frames. */
extern std::vector<alignment_query_registry_entry> alignment_query_registry;
/** @brief Executor-state lookup entries for active frames. */
extern std::vector<alignment_estate_registry_entry> alignment_estate_registry;
/** @brief Frames currently crossing the ExecutorStart callback boundary. */
extern std::vector<alignment_executor_frame*> alignment_start_in_progress;
/** @brief Frames currently crossing the ExecutorRun callback boundary. */
extern std::vector<alignment_executor_frame*> alignment_current_run;

/** @brief Removes every occurrence of a frame pointer from a stack. */
void erase_frame_pointer(
	std::vector<alignment_executor_frame*>& frames,
	alignment_executor_frame* frame) noexcept;

/** @brief Removes a frame from every backend-local registry and stack. */
void unlink_alignment_frame(alignment_executor_frame* frame) noexcept;
/** @brief Unlinks and destroys a frame while detaching its cleanup token. */
void destroy_alignment_frame(alignment_executor_frame* frame) noexcept;
/** @brief Registers query-context cleanup for a live executor frame. */
void ensure_alignment_frame_cleanup(
	alignment_executor_frame* frame,
	EState* estate);
/**
 * @brief Finds the active frame for a query descriptor.
 * @return The borrowed frame or null when no frame is registered.
 */
[[nodiscard]] alignment_executor_frame* find_alignment_frame(
	QueryDesc* query_desc) noexcept;
/**
 * @brief Finds the active frame for executor state.
 * @return The borrowed frame or null when no frame is registered.
 */
[[nodiscard]] alignment_executor_frame* find_alignment_frame(
	EState* estate) noexcept;
/** @brief Registers a complete frame under its query and executor identities. */
void register_alignment_frame(alignment_executor_frame* frame);
/** @brief Pops the expected top frame or raises a PostgreSQL lifecycle error. */
void pop_exact_alignment_stack(
	std::vector<alignment_executor_frame*>& stack,
	alignment_executor_frame* frame,
	const char* stack_name);
/** @brief Removes a frame from a stack during PostgreSQL error unwinding. */
void pop_alignment_stack_on_error(
	std::vector<alignment_executor_frame*>& stack,
	alignment_executor_frame* frame) noexcept;
/**
 * @brief Binds a scan node to the ExecutorStart frame owning its statement.
 * @return A borrowed registration handle tied to the frame lifetime.
 */
[[nodiscard]] bioseq::postgres::table::alignment_execution::alignment_node_registration_handle
bind_alignment_node_to_starting_frame(
	EState* estate,
	std::uint32_t scan_node_id);
/** @brief Invalidates nested bindings that escaped their root executor. */
void retire_escaped_nested_bindings(alignment_executor_frame* root) noexcept;
/** @brief Marks a frame finished and releases it when cleanup no longer owns it. */
void retire_alignment_frame(alignment_executor_frame* frame) noexcept;

/** @brief Publishes a completed root frame as the backend alignment snapshot. */
void publish_alignment_root(alignment_executor_frame* frame);

/** @brief Runs the chained PostgreSQL ExecutorRun hook with frame accounting. */
void bioseq_alignment_executor_run(
	QueryDesc* query_desc,
	ScanDirection direction,
	uint64 count,
	bool execute_once);

} // namespace bioseq::pg::detail
