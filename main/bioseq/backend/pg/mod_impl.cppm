/**-------------------------------------------------------------------------
 *
 * mod_impl.cppm
 *
 * @defgroup bioseq_sql_public_api Bioseq SQL public API
 *
 * Extension lifecycle and PostgreSQL logging ownership for bioseq.mod.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
extern "C" {
void bioseq_external_tableam_init(void);
void bioseq_external_scan_init(void);
}
#endif

extern "C" {
#include "executor/executor.h"
#include "utils/memutils.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.mod;

import bioseq.column;
import bioseq.common.mlogging;
import bioseq.config;
import bioseq.pipeline.stats;
import bioseq.pg.cache_provider;
import bioseq.pg_utils;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.sql.generated.test_indexes.guc_registry;
#else
import bioseq.sql.generated.production.guc_registry;
#endif
import bioseq.table.scan_state;
import bioseq.types.pg_compact_sequence;

using namespace bioseq::pg_utils;

namespace {

int bioseq_pg_elevel_for_level(LoggingLevel level) noexcept
{
	switch (level) {
		case LoggingLevel::DBG: return DEBUG1;
		case LoggingLevel::INF: return LOG;
		case LoggingLevel::WRN: return WARNING;
		case LoggingLevel::ERR: return ERROR;
		case LoggingLevel::OFF: return DEBUG5;
	}
	return ERROR;
}

bool bioseq_pg_should_log_elevel(int elevel) noexcept
{
	return elevel >= ERROR || message_level_is_interesting(elevel);
}

bool bioseq_pg_sink_should_log_level(LoggingLevel level) noexcept
{
	return bioseq_pg_should_log_elevel(bioseq_pg_elevel_for_level(level));
}

void bioseq_pg_sink_log_elevel(
	int elevel,
	BioseqSourceLocation location,
	std::string_view message)
{
	if (!errstart(elevel, TEXTDOMAIN)) {
		return;
	}
	errmsg_internal("%.*s",
					static_cast<int>(message.size()),
					message.data());
	errfinish(location.file_name,
			  static_cast<int>(location.line),
			  location.function_name);
	if (elevel >= ERROR) {
		__builtin_unreachable();
	}
}

void bioseq_pg_sink_log_level(
	LoggingLevel level,
	BioseqSourceLocation location,
	std::string_view message)
{
	bioseq_pg_sink_log_elevel(
		bioseq_pg_elevel_for_level(level),
		location,
		message);
}

void bioseq_pg_sink_report(const BioseqReportView& report)
{
	const int elevel = report.has_raw_pg_elevel
		? report.raw_pg_elevel
		: bioseq_pg_elevel_for_level(report.level);

	if (!errstart(elevel, TEXTDOMAIN)) {
		return;
	}
	if (report.has_sqlerrcode) {
		errcode(report.sqlerrcode);
	}
	if (!report.message.empty()) {
		errmsg_internal("%.*s",
						static_cast<int>(report.message.size()),
						report.message.data());
	}
	if (!report.detail.empty()) {
		errdetail_internal("%.*s",
						   static_cast<int>(report.detail.size()),
						   report.detail.data());
	}
	if (!report.hint.empty()) {
		errhint("%.*s",
				static_cast<int>(report.hint.size()),
				report.hint.data());
	}
	errfinish(report.location.file_name,
			  static_cast<int>(report.location.line),
			  report.location.function_name);
	if (elevel >= ERROR) {
		__builtin_unreachable();
	}
}

void initialize_extension_cpp()
{
	static bool initialized = false;
	if (initialized) {
		bioseq_log<DBG, PG>("[bioseq] pg: _PG_init() skipped duplicate call");
		return;
	}

	/*
	 * Resolve DSO ownership before registering GUCs, CustomScan methods, or
	 * hooks.  Publication is deferred until every ordinary initializer below
	 * has succeeded, so a failed _PG_init never exposes a half-initialized
	 * provider directory.
	 */
	bioseq::pg::cache_provider::bioseq_cache_provider_preflight();
	bioseq_register_postgres_logging_sink({
		.debug_logging_enabled = &bioseq::settings::logging,
		.should_log_level = bioseq_pg_sink_should_log_level,
		.log_level = bioseq_pg_sink_log_level,
		.should_log_pg = bioseq_pg_should_log_elevel,
		.log_pg = bioseq_pg_sink_log_elevel,
		.report = bioseq_pg_sink_report,
	});
#if defined(BIOSEQ_TEST_INDEXES)
	bioseq::sql::generated::test_indexes::register_gucs();
#else
	bioseq::sql::generated::production::register_gucs();
#endif
	log_configure();
	bioseq_log<DBG, PG>("[bioseq] pg: _PG_init() entered");
	bioseq::pg::detail::install_alignment_execution_hooks();
	bioseq_init();
#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
	bioseq_external_tableam_init();
	bioseq_external_scan_init();
#endif
	install_letter_literal_validation_hook();
	bioseq::pg::cache_provider::bioseq_cache_provider_publish_directory();
	initialized = true;
}

} // namespace

extern "C" {
PG_MODULE_MAGIC;

void _PG_init();

/*
 * _PG_init is PostgreSQL's extension entry point. It runs when the shared
 * library is loaded into a backend, before any SQL function in the extension
 * is callable.
 */
void
_PG_init()
{
	ErrorData* exact_error = nullptr;
	bool standard_error = false;
	bool unknown_error = false;
	char detail[512] = "std::exception without detail";

	try {
		initialize_extension_cpp();
		return;
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		exact_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(detail));
		standard_error = true;
	} catch (...) {
		unknown_error = true;
	}

	if (exact_error != nullptr) {
		ReThrowError(exact_error);
	}

	ereport(
		ERROR,
		(errcode(ERRCODE_INTERNAL_ERROR),
		 errmsg("bioseq extension initialization raised a C++ exception"),
		 errdetail(
			 "%s",
			 unknown_error
				 ? "unknown non-standard C++ exception"
				 : (standard_error
						? detail
						: "empty C++ exception state"))));
	pg_unreachable();
}

} // extern "C"

namespace bioseq::pg::detail {

namespace {
namespace alignment_exec = bioseq::pg::alignment_execution;

struct AlignmentExecutorFrame;

/* PostgreSQL 17 has no reset-callback unregister API. */
struct AlignmentFrameCleanupToken {
	MemoryContextCallback callback{};
	AlignmentExecutorFrame* frame = nullptr;
};

struct AlignmentExecutorFrame {
	QueryDesc* query_desc = nullptr;
	EState* estate = nullptr;
	alignment_exec::AlignmentExecutionCollector collector{};
	alignment_exec::AlignmentCollectorBinding collector_binding{};
	AlignmentFrameCleanupToken* cleanup_token = nullptr;
	bool nested = false;
	bool nested_alignment_invoked = false;
	bool query_output_fully_drained = false;
	bool finished = false;
};

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

ExecutorStart_hook_type PreviousExecutorStartHook = nullptr;
ExecutorRun_hook_type PreviousExecutorRunHook = nullptr;
ExecutorFinish_hook_type PreviousExecutorFinishHook = nullptr;

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
std::vector<AlignmentExecutorFrame*> alignment_start_in_progress{};
std::vector<AlignmentExecutorFrame*> alignment_current_run{};
std::optional<alignment_exec::AlignmentExecutionSnapshot>
	last_alignment_execution_snapshot{};
/* Evidence publication is stricter than the diagnostic v1/v2 SRFs: partial
 * roots remain visible there, but must not replace the last completed
 * operation ledger. Completed non-GPU roots deliberately publish an exact
 * empty ledger so stale GPU proof cannot survive a later successful query. */
std::optional<alignment_exec::AlignmentExecutionSnapshot>
	last_completed_alignment_evidence_snapshot{};
std::int64_t last_alignment_execution_id = 0;

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

void publish_alignment_root(AlignmentExecutorFrame* frame)
{
	if (frame == nullptr || frame->nested
		|| frame->collector.root != &frame->collector
		|| frame->collector.alignment_nodes.empty()) {
		return;
	}
	if (frame->collector.alignment_nodes.size()
		> static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)())) {
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("bioseq alignment scan-node count exceeds SQL integer")));
	}
	if (last_alignment_execution_id
		== (std::numeric_limits<std::int64_t>::max)()) {
		ereport(ERROR,
			(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
			 errmsg("bioseq alignment execution id exhausted")));
	}

	bioseq::pipeline_stats::PipelineExecutionStats statement_stats{};
	bool fully_drained = frame->query_output_fully_drained;
	for (const alignment_exec::AlignmentNodeStats& node :
		 frame->collector.alignment_nodes) {
		statement_stats.merge(node.pipeline);
		fully_drained = fully_drained
			&& node.current_pass_fully_drained
			&& !node.incomplete_pass_seen;
	}
	if (frame->collector.lifecycle_counter_saturated) {
		statement_stats.operational.stats_saturated = true;
	}
	frame->collector.persisted_gpu_operation_evidence.freeze();
	const bool persisted_gpu_evidence_required =
		statement_stats.actual_backend
			== bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu
		&& statement_stats.actual_target_index_source
			== bioseq::pipeline_stats::TargetIndexSource::persisted
		&& statement_stats.fully_attested_alignment_jobs != 0;
	if (persisted_gpu_evidence_required
		&& !frame->collector.persisted_gpu_operation_evidence.
			matches_pipeline(statement_stats)) {
		statement_stats.operational.stats_saturated = true;
		statement_stats.reclassify_all_attempted_jobs();
	}

	const std::int64_t execution_id = last_alignment_execution_id + 1;
	if (fully_drained) {
		last_completed_alignment_evidence_snapshot =
			alignment_exec::AlignmentExecutionSnapshot{
				.execution_id = execution_id,
				.backend_pid = static_cast<std::int32_t>(MyProcPid),
				.database_oid = MyDatabaseId,
				.scan_node_count = static_cast<std::int32_t>(
					frame->collector.alignment_nodes.size()),
				.nested_alignment_executor_count = static_cast<std::int32_t>(
					frame->collector.nested_alignment_executor_count),
				.completed = true,
				.partial = false,
				.fully_drained = true,
				.pipeline = statement_stats,
				.persisted_gpu_operation_evidence = std::move(
					frame->collector.persisted_gpu_operation_evidence),
			};
	}
	last_alignment_execution_snapshot =
		alignment_exec::AlignmentExecutionSnapshot{
			.execution_id = execution_id,
			.backend_pid = static_cast<std::int32_t>(MyProcPid),
			.database_oid = MyDatabaseId,
			.scan_node_count = static_cast<std::int32_t>(
				frame->collector.alignment_nodes.size()),
			.nested_alignment_executor_count = static_cast<std::int32_t>(
				frame->collector.nested_alignment_executor_count),
			.completed = fully_drained,
			.partial = !fully_drained,
			.fully_drained = fully_drained,
			.pipeline = statement_stats,
		};
	last_alignment_execution_id = execution_id;
}

void call_previous_or_standard_executor_start(QueryDesc* query_desc, int eflags)
{
	if (PreviousExecutorStartHook != nullptr) {
		PreviousExecutorStartHook(query_desc, eflags);
	} else {
		standard_ExecutorStart(query_desc, eflags);
	}
}

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

[[nodiscard]] const std::optional<
    bioseq::pg::alignment_execution::AlignmentExecutionSnapshot>&
last_alignment_execution_snapshot_ref() noexcept
{
    return last_alignment_execution_snapshot;
}

[[nodiscard]] const std::optional<
    bioseq::pg::alignment_execution::AlignmentExecutionSnapshot>&
last_completed_alignment_evidence_snapshot_ref() noexcept
{
    return last_completed_alignment_evidence_snapshot;
}

} // namespace bioseq::pg::detail
