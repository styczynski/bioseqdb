/**
 * @file alignment_hook_contract.cppm
 * @brief Shared query state used by the alignment executor hooks.
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
 * @brief Connect a query-context reset callback to its heap-allocated frame.
 *
 * The callback stays registered after an early frame deletion. Clearing the
 * frame pointer makes that later callback harmless.
 */
struct alignment_frame_cleanup_token {
	MemoryContextCallback callback{};
	alignment_executor_frame* frame = nullptr;
};

/**
 * @brief Alignment statistics and cleanup state for one PostgreSQL executor.
 *
 * Scan nodes borrow its collector binding. Nested executors write to the
 * root statement's collector, and lose that access when the root finishes.
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

/** @brief ExecutorRun hook to call before updating Bioseq statistics. */
extern ExecutorRun_hook_type previous_executor_run_hook;
/** @brief Active frames used to find nested queries during cleanup. */
extern std::vector<alignment_executor_frame*> alignment_frames;
/** @brief Query-descriptor lookup entries for active frames. */
extern std::vector<alignment_query_registry_entry> alignment_query_registry;
/** @brief Executor-state lookup entries for active frames. */
extern std::vector<alignment_estate_registry_entry> alignment_estate_registry;
/** @brief Stack of ExecutorStart calls currently in progress. */
extern std::vector<alignment_executor_frame*> alignment_start_in_progress;
/** @brief Stack of ExecutorRun calls currently in progress. */
extern std::vector<alignment_executor_frame*> alignment_current_run;

/** @brief Removes every occurrence of a frame pointer from a stack. */
void erase_frame_pointer(
	std::vector<alignment_executor_frame*>& frames,
	alignment_executor_frame* frame) noexcept;

/** @brief Removes a frame from every backend-local registry and stack. */
void unlink_alignment_frame(alignment_executor_frame* frame) noexcept;
/** @brief Unlink and delete a frame, clearing its registered cleanup pointer. */
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
/** @brief Add a frame to the query-descriptor and executor-state registries. */
void register_alignment_frame(alignment_executor_frame* frame);
/** @brief Pop the expected top frame, raising an error if calls are out of order. */
void pop_exact_alignment_stack(
	std::vector<alignment_executor_frame*>& stack,
	alignment_executor_frame* frame,
	const char* stack_name);
/** @brief Removes a frame from a stack during PostgreSQL error unwinding. */
void pop_alignment_stack_on_error(
	std::vector<alignment_executor_frame*>& stack,
	alignment_executor_frame* frame) noexcept;
/**
 * @brief Connect a scan to the frame for its statement.
 * Accept scans created during ExecutorStart or by the currently running
 * executor, including EvalPlanQual scans using their parent frame.
 * @return Borrowed registration handle valid until frame retirement.
 */
[[nodiscard]] bioseq::postgres::table::alignment_execution::alignment_node_registration_handle
bind_alignment_node_to_starting_frame(
	EState* estate,
	std::uint32_t scan_node_id);
/** @brief Block nested queries from using a root collector after it finishes. */
void retire_escaped_nested_bindings(alignment_executor_frame* root) noexcept;
/** @brief Unlink a finished frame and delete it if no cleanup callback owns it. */
void retire_alignment_frame(alignment_executor_frame* frame) noexcept;

/** @brief Save a root query's alignment statistics, including partial results. */
void publish_alignment_root(alignment_executor_frame* frame);

/** @brief Run the previous hook or standard executor and track query completion. */
void bioseq_alignment_executor_run(
	QueryDesc* query_desc,
	ScanDirection direction,
	uint64 count
#if PG_VERSION_NUM < PG_VERSION_18
	, bool execute_once
#endif
);

} // namespace bioseq::pg::detail
