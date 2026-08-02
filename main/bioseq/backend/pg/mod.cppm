/**-------------------------------------------------------------------------
 *
 * mod.cppm
 *
 * @defgroup bioseq_sql_public_api Bioseq SQL public API
 *
 * Extension lifecycle hooks and typed SQL routine owners. Generated fmgr
 * modules provide the C ABI for annotated SQL routines.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"
#include <hwy/highway.h>


extern "C" {
#include "access/genam.h"
#include "access/heapam.h"
#include "access/table.h"
#include "catalog/pg_collation_d.h"
#include "executor/executor.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
}

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG
extern "C" {
void bioseq_external_tableam_init(void);
void bioseq_external_scan_init(void);
}
#endif

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.mod;

import bioseq.common.mlogging;
import bioseq.codegen.sql.annotation_model;
import bioseq.column;
import bioseq.config;
import bioseq.mod.alignment.sql_rows;
import bioseq.mod.sql_rows;
import bioseq.pg_utils;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.pg_utils.row_codec_runtime_test_support;
#endif
import bioseq.pg_utils.runtime_test_counters;
import bioseq.pg_utils.sql_catalog_entities;
import bioseq.pg.cache_provider;
import bioseq.sql.generated.core.storage_indexing_pg_chunked_storage.entity_handles;
import bioseq.sql.generated.core.storage_manager_sequence_storage_manager.entity_handles;
import bioseq.sql.generated.core.storage_metadata_core.entity_handles;
#if defined(BIOSEQ_TEST_INDEXES)
import bioseq.sql.generated.test_indexes.guc_registry;
#else
import bioseq.sql.generated.production.guc_registry;
#endif
import bioseq.types.pg_compact_sequence;
import bioseq.vector.execution;
import bioseq.vector.types;
import bioseq.vector.types.aggregates;
import bioseq.sequences;
import bioseq.storage.manager;
import bioseq.storage.scanner_pg;
import bioseq.storage.lazy;
import bioseq.indexing.type;
import bioseq.indexing.minimizer_hashmap;
import bioseq.indexing.minimizer_sequence;
import bioseq.indexing.symbol_counts;
import bioseq.storage.indexing_pg.admin;
import bioseq.storage.indexing_pg.chunked_storage;
import bioseq.storage.indexing_pg.handles;
import bioseq.scheduler.supervisor_pg;
import bioseq.scheduler.dispatch_processor;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.mode;
import bioseq.common.sequan_compat;
import bioseq.column.tam;
import bioseq.pg_utils.nodes;
import bioseq.table.scan_state;
import bioseq.table;
import bioseq.table.sql_rows;
import bioseq.sequences.pipe;

using namespace bioseq::pg_utils;

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

inline constexpr int alignment_stats_v1_column_count = 150;
inline constexpr int alignment_stats_v2_column_count = 151;

[[nodiscard]] std::string fixed_hex_u64(std::uint64_t value)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string result(16, '0');
	for (std::size_t i = 0; i < result.size(); ++i) {
		result[result.size() - i - 1] = digits[value & 0x0fU];
		value >>= 4U;
	}
	return result;
}

template<std::size_t N>
[[nodiscard]] std::string fixed_hex_bytes(
		const std::array<std::uint8_t, N>& bytes)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string result(N * 2, '0');
	for (std::size_t i = 0; i < N; ++i) {
		result[i * 2] = digits[(bytes[i] >> 4U) & 0x0fU];
		result[i * 2 + 1] = digits[bytes[i] & 0x0fU];
	}
	return result;
}

template<std::size_t N>
[[nodiscard]] std::string fixed_hex_bytes(
		const std::array<std::byte, N>& bytes)
{
	constexpr char digits[] = "0123456789abcdef";
	std::string result(N * 2, '0');
	for (std::size_t i = 0; i < N; ++i) {
		const std::uint8_t value = std::to_integer<std::uint8_t>(bytes[i]);
		result[i * 2] = digits[(value >> 4U) & 0x0fU];
		result[i * 2 + 1] = digits[value & 0x0fU];
	}
	return result;
}

[[nodiscard]] std::string cold_index_preparation_json(
		const bioseq::pipeline_stats::PipelineOperationalStats& stats)
{
	/*
	 * Keep the cold-read diagnostic surface versioned and self-contained
	 * without permanently widening the already-large SRF by one column per
	 * counter.  Values are saturated before reaching this boundary, so every
	 * JSON number remains representable as a SQL bigint. Head/directory/range
	 * scan counts and TOAST/detoast counts remain in their pre-existing scalar
	 * SRF columns; duplicating those five values here would create two public
	 * names for the same measurement. stats_saturated is repeated deliberately
	 * so a consumer can validate this object without consulting another column.
	 * payload_ns is one inclusive wall interval from the first payload scan to
	 * final payload validation, including callbacks. Internal decode/transfer
	 * counters remain zero, while their public JSON values are null and their
	 * availability flags are false: measuring them at tuple/chunk granularity
	 * would add O(rows) clock overhead to the benchmarked path.
	 */
	std::string result =
		R"({"schema":"bioseq.cold_index_preparation","version":1,"timing_semantics":"payload_inclusive_wall_v1","decode_timing_available":false,"transfer_timing_available":false)";
	result.reserve(2048);
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};

	append("target_relation_reuses",
		stats.cold_index_target_relation_reuses);
	append("target_relation_extra_opens",
		stats.cold_index_target_relation_extra_opens);
	append("parent_random_reads", stats.cold_index_parent_random_reads);
	append("policy_heap_opens", stats.cold_index_policy_heap_opens);
	append("policy_index_opens", stats.cold_index_policy_index_opens);
	append("source_heap_opens", stats.cold_index_source_heap_opens);
	append("source_index_opens", stats.cold_index_source_index_opens);
	append("fragment_heap_opens", stats.cold_index_fragment_heap_opens);
	append("sidecar_heap_opens", stats.cold_index_sidecar_heap_opens);
	append("sidecar_index_opens", stats.cold_index_sidecar_index_opens);
	append("policy_schema_validations",
		stats.cold_index_policy_schema_validations);
	append("source_schema_validations",
		stats.cold_index_source_schema_validations);
	append("sidecar_schema_validations",
		stats.cold_index_sidecar_schema_validations);
	append("lock_acquires", stats.cold_index_lock_acquires);
	append("lock_releases", stats.cold_index_lock_releases);
	append("snapshot_registers", stats.cold_index_snapshot_registers);
	append("route_empty", stats.cold_index_route_empty);
	append("route_selective", stats.cold_index_route_selective);
	append("route_full", stats.cold_index_route_full);
	append("route_candidate_rows", stats.cold_index_route_candidate_rows);
	append("route_full_rows", stats.cold_index_route_full_rows);
	append("route_candidate_bytes", stats.cold_index_route_candidate_bytes);
	append("route_full_bytes", stats.cold_index_route_full_bytes);
	append("route_selective_cost", stats.cold_index_route_selective_cost);
	append("route_full_cost", stats.cold_index_route_full_cost);
	append("memory_reroutes", stats.cold_index_memory_reroutes);
	append("payload_bytes_requested",
		stats.cold_index_payload_bytes_requested);
	append("payload_bytes_delivered",
		stats.cold_index_payload_bytes_delivered);
	append("payload_rows_requested",
		stats.cold_index_payload_rows_requested);
	append("payload_rows_delivered",
		stats.cold_index_payload_rows_delivered);
	append("payload_rows_scanned", stats.cold_index_payload_rows_scanned);
	append("payload_bytes_scanned", stats.cold_index_payload_bytes_scanned);
	append("payload_rows_skipped", stats.cold_index_payload_rows_skipped);
	append("heap_tid_adjacent", stats.cold_index_heap_tid_adjacent);
	append("heap_tid_forward_gap", stats.cold_index_heap_tid_forward_gap);
	append("heap_tid_backward_jump", stats.cold_index_heap_tid_backward_jump);
	append("extent_crc_bytes", stats.cold_index_extent_crc_bytes);
	append("extent_crc_raw_bytes_hashed",
		stats.cold_index_extent_crc_raw_bytes_hashed);
	append("extent_crc_combined_bytes",
		stats.cold_index_extent_crc_combined_bytes);
	append("extent_crc_combine_rows",
		stats.cold_index_extent_crc_combine_rows);
	append("row_crc_bytes", stats.cold_index_row_crc_bytes);
	append("tuple_form_bytes", stats.cold_index_tuple_form_bytes);
	append("policy_ns", stats.cold_index_policy_ns);
	append("source_ns", stats.cold_index_source_ns);
	append("bind_ns", stats.cold_index_bind_ns);
	append("head_ns", stats.cold_index_head_ns);
	append("directory_ns", stats.cold_index_directory_ns);
	append("payload_ns", stats.cold_index_payload_ns);
	result += R"(,"decode_ns":null,"transfer_ns":null)";
	append("close_ns", stats.cold_index_close_ns);
	append("shared_blks_hit", stats.cold_index_shared_blks_hit);
	append("shared_blks_read", stats.cold_index_shared_blks_read);
	result += R"(,"stats_saturated":)";
	result += stats.stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

[[nodiscard]] std::string simd_execution_evidence_json(
		const bioseq::pipeline_stats::PipelineExecutionStats& stats)
{
	/*
	 * Version 2 adds one compact, exact-schema proof object without changing
	 * any v1 column or the frozen cold-index object. Every value comes from the
	 * single immutable AlignmentExecutionSnapshot copied by the SRF before it
	 * emits its twenty stage rows.
	 */
	const auto& operational = stats.operational;
	std::string result =
		R"({"schema":"bioseq.simd_execution_evidence","version":2)";
	result.reserve(1152);
	result += R"(,"requested_policy":")";
	result.append(bioseq::pipeline_stats::simd_chaining_policy_identity_name(
		stats.requested_simd_chaining_policy));
	result += R"(","observed_policy":")";
	result.append(bioseq::pipeline_stats::simd_chaining_policy_identity_name(
		stats.observed_simd_chaining_policy));
	result += '"';
	result += R"(,"requested_alignment_algorithm":")";
	result.append(
		bioseq::pipeline_stats::simd_alignment_algorithm_identity_name(
			stats.requested_simd_alignment_algorithm));
	result += R"(","observed_alignment_algorithm":")";
	result.append(
		bioseq::pipeline_stats::simd_alignment_algorithm_identity_name(
			stats.observed_simd_alignment_algorithm));
	result += '"';
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append("nthash_vector_batches", operational.nthash_vector_batches);
	append(
		"nthash_resident_vector_stream_kernel_calls",
		operational.nthash_resident_vector_stream_kernel_calls);
	append(
		"simd_chaining_exact_policy_calls",
		operational.simd_chaining_exact_policy_calls);
	append(
		"simd_chaining_bounded_policy_calls",
		operational.simd_chaining_bounded_policy_calls);
	append(
		"simd_chaining_bounded_score_batches",
		operational.simd_chaining_bounded_score_batches);
	append(
		"simd_chaining_bounded_cutoff_stops",
		operational.simd_chaining_bounded_cutoff_stops);
	append(
		"simd_chaining_bounded_candidates_skipped",
		operational.simd_chaining_bounded_candidates_skipped);
	append(
		"simd_chaining_policy_rejections",
		operational.simd_chaining_policy_rejections);
	result += R"(,"simd_chaining_policy_evidence_saturated":)";
	result += operational.simd_chaining_policy_evidence_saturated
		? "true" : "false";
	const auto& block = operational.block_aligner_execution_evidence;
	result += R"(,"block_aligner_execution_proof":{"present":)";
	result += block.present ? "true" : "false";
	result += R"(,"accepted":)";
	result += block.accepted ? "true" : "false";
	const auto append_block = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append_block("executed_calls", block.executed_calls);
	append_block("nonempty_calls", block.nonempty_calls);
	append_block(
		"nonempty_calls_with_simd", block.nonempty_calls_with_simd);
	append_block("simd_dp_vector_batches", block.simd_dp_vector_batches);
	append_block("simd_dp_cells", block.simd_dp_cells);
	append_block("forbidden_ksw2_calls", block.forbidden_ksw2_calls);
	append_block(
		"forbidden_scalar_biological_fallbacks",
		block.forbidden_scalar_biological_fallbacks);
	result += R"(,"saturated":)";
	result += block.saturated ? "true" : "false";
	result += '}';
	result += '}';
	return result;
}

[[nodiscard]] std::string format5_reader_memory_evidence_json(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot)
{
	/*
	 * Keep this post-statement diagnostic separate from both frozen alignment
	 * stats SRFs and the version-1 cold-index object.  One immutable backend-
	 * local snapshot supplies the identity, counters and saturation flags.
	 */
	const auto& operational = snapshot.pipeline.operational;
	std::string result =
		R"({"schema":"bioseq.format5_reader_memory_evidence","version":1)";
	result.reserve(768);
	const auto append_signed = [&result](
			const std::string_view key,
			const std::int64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append_signed("execution_id", snapshot.execution_id);
	append_signed("backend_pid", snapshot.backend_pid);
	append("database_oid", snapshot.database_oid);
	append(
		"format5_reader_capacity_accounting_passes",
		operational.format5_reader_capacity_accounting_passes);
	append(
		"format5_reader_global_position_storage_preparations",
		operational.format5_reader_global_position_storage_preparations);
	append(
		"format5_reader_global_position_storage_allocations",
		operational.format5_reader_global_position_storage_allocations);
	append(
		"format5_reader_position_append_growth_reservations",
		operational.format5_reader_position_append_growth_reservations);
	append(
		"format5_reader_retained_position_count",
		operational.format5_reader_retained_position_count);
	append(
		"format5_reader_retained_position_capacity",
		operational.format5_reader_retained_position_capacity);
	result += R"(,"format5_reader_memory_evidence_saturated":)";
	result += operational.format5_reader_memory_evidence_saturated
		? "true" : "false";
	result += R"(,"stats_saturated":)";
	result += operational.stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

enum class Format5ReaderMemoryEvidenceVersion : std::uint32_t {
    exact_array_layout = 2u,
    fragment_native_query = 3u,
    provider_lifecycle = 4u,
    bucket_direct_query = 5u,
    compact_direct_one_decode = 6u,
};

[[nodiscard]] std::string format5_reader_memory_evidence_json_versioned(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot,
        const Format5ReaderMemoryEvidenceVersion version)
{
    bool include_fragment_native_query = false;
    bool include_provider_lifecycle = false;
    bool include_bucket_direct_query = false;
    bool include_compact_direct_one_decode = false;
    switch (version) {
        case Format5ReaderMemoryEvidenceVersion::exact_array_layout:
            break;
        case Format5ReaderMemoryEvidenceVersion::fragment_native_query:
            include_fragment_native_query = true;
            break;
        case Format5ReaderMemoryEvidenceVersion::provider_lifecycle:
            include_fragment_native_query = true;
            include_provider_lifecycle = true;
            break;
        case Format5ReaderMemoryEvidenceVersion::bucket_direct_query:
            include_fragment_native_query = true;
            include_provider_lifecycle = true;
            include_bucket_direct_query = true;
            break;
        case Format5ReaderMemoryEvidenceVersion::compact_direct_one_decode:
            include_fragment_native_query = true;
            include_provider_lifecycle = true;
            include_bucket_direct_query = true;
            include_compact_direct_one_decode = true;
            break;
    }
    const std::uint32_t version_number =
        static_cast<std::uint32_t>(version);
    const auto& operational = snapshot.pipeline.operational;
    std::string result =
        R"({"schema":"bioseq.format5_reader_memory_evidence","version":)"
        + std::to_string(version_number);
    result.reserve(include_provider_lifecycle
        ? 12'288u
        : include_fragment_native_query ? 6'144u : 2'048u);
	const auto append_signed = [&result](
			const std::string_view key,
			const std::int64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append_signed("execution_id", snapshot.execution_id);
	append_signed("backend_pid", snapshot.backend_pid);
	append("database_oid", snapshot.database_oid);
	append(
		"format5_reader_capacity_accounting_passes",
		operational.format5_reader_capacity_accounting_passes);
	append(
		"format5_reader_global_position_storage_preparations",
		operational.format5_reader_global_position_storage_preparations);
	append(
		"format5_reader_global_position_storage_allocations",
		operational.format5_reader_global_position_storage_allocations);
	append(
		"format5_reader_position_append_growth_reservations",
		operational.format5_reader_position_append_growth_reservations);
	append(
		"format5_reader_retained_position_count",
		operational.format5_reader_retained_position_count);
	append(
		"format5_reader_retained_position_capacity",
		operational.format5_reader_retained_position_capacity);
	append(
		"format5_reader_directory_capacity_bytes",
		operational.format5_reader_directory_capacity_bytes);
	append(
		"format5_reader_query_hash_capacity_bytes",
		operational.format5_reader_query_hash_capacity_bytes);
	append(
		"format5_reader_query_record_capacity_bytes",
		operational.format5_reader_query_record_capacity_bytes);
	append(
		"format5_reader_ordinal_route_capacity_bytes",
		operational.format5_reader_ordinal_route_capacity_bytes);
	append(
		"format5_reader_selected_extent_capacity_bytes",
		operational.format5_reader_selected_extent_capacity_bytes);
	append(
		"format5_reader_current_selected_control_capacity_bytes",
		operational.format5_reader_current_selected_control_capacity_bytes);
	append(
		"format5_reader_global_position_capacity_bytes",
		operational.format5_reader_global_position_capacity_bytes);
	append(
		"format5_reader_control_payload_capacity_bytes",
		operational.format5_reader_control_payload_capacity_bytes);
	append(
		"format5_reader_owned_capacity_bytes",
		operational.format5_reader_owned_capacity_bytes);
	append(
		"format5_reader_admitted_bytes",
		operational.format5_reader_admitted_bytes);
	append(
		"format5_reader_exact_position_image_checks",
		operational.format5_reader_exact_position_image_checks);
	append(
		"format5_reader_exact_position_image_failures",
		operational.format5_reader_exact_position_image_failures);
	append(
		"format5_reader_retained_layout_checks",
		operational.format5_reader_retained_layout_checks);
    append(
        "format5_reader_retained_layout_failures",
        operational.format5_reader_retained_layout_failures);
    if (include_fragment_native_query) {
        const auto& evidence = operational.fragment_native_query_evidence;
        append("fragment_native_waves", evidence.waves);
        append("fragment_native_queries", evidence.queries);
        append("fragment_native_occurrence_fill_ns",
            evidence.occurrence_fill_ns);
        append("fragment_native_occurrence_radix_ns",
            evidence.occurrence_radix_ns);
        append("fragment_native_unique_plan_build_ns",
            evidence.unique_plan_build_ns);
        append("fragment_native_query_plan_radix_fill_ns",
            evidence.query_plan_radix_fill_ns);
        append("fragment_native_control_count_pass_ns",
            evidence.control_count_pass_ns);
        append("fragment_native_rejected_control_validation_ns",
            evidence.rejected_control_validation_ns);
        append("fragment_native_orientation_layout_ns",
            evidence.orientation_layout_ns);
        append("fragment_native_direct_emission_ns",
            evidence.direct_emission_ns);
        append("fragment_native_local_sort_ns", evidence.local_sort_ns);
        append("fragment_native_tie_normalization_ns",
            evidence.tie_normalization_ns);
        append("fragment_native_control_count_bytes",
            evidence.control_count_bytes);
        append("fragment_native_rejected_control_validation_bytes",
            evidence.rejected_control_validation_bytes);
        append("fragment_native_pass2_position_bytes",
            evidence.pass2_position_bytes);
        append("fragment_native_pass3_position_bytes",
            evidence.pass3_position_bytes);
        append("fragment_native_anchors_constructed",
            evidence.anchors_constructed);
        append("fragment_native_local_sort_bytes",
            evidence.local_sort_bytes);
        append("fragment_native_tie_normalization_bytes",
            evidence.tie_normalization_bytes);
        append("fragment_native_exact_final_bytes",
            evidence.exact_final_bytes);
        append("fragment_native_peak_exact_final_bytes",
            evidence.peak_exact_final_bytes);
        append("fragment_native_global_sort_calls_invoked",
            evidence.global_sort_calls_invoked);
        append("fragment_native_global_sort_calls_skipped",
            evidence.global_sort_calls_skipped);
        append("fragment_native_candidate_anchor_bytes",
            evidence.candidate_anchor_bytes);
        append("fragment_native_plan_ordinal_bytes",
            evidence.plan_ordinal_bytes);
        append("fragment_native_compaction_bytes",
            evidence.compaction_bytes);
        append("fragment_native_final_copy_bytes",
            evidence.final_copy_bytes);
        append("fragment_native_peak_occurrence_capacity_bytes",
            evidence.peak_occurrence_capacity_bytes);
        append("fragment_native_peak_query_plan_capacity_bytes",
            evidence.peak_query_plan_capacity_bytes);
        append("fragment_native_peak_query_radix_capacity_bytes",
            evidence.peak_query_radix_capacity_bytes);
        append("fragment_native_peak_occurrence_count_capacity_bytes",
            evidence.peak_occurrence_count_capacity_bytes);
        append("fragment_native_peak_block_layout_capacity_bytes",
            evidence.peak_block_layout_capacity_bytes);
        append("fragment_native_peak_block_evidence_capacity_bytes",
            evidence.peak_block_evidence_capacity_bytes);
        append("fragment_native_occurrence_allocations",
            evidence.occurrence_allocations);
        append("fragment_native_query_plan_allocations",
            evidence.query_plan_allocations);
        append("fragment_native_query_radix_allocations",
            evidence.query_radix_allocations);
        append("fragment_native_occurrence_count_allocations",
            evidence.occurrence_count_allocations);
        append("fragment_native_block_layout_allocations",
            evidence.block_layout_allocations);
        append("fragment_native_block_evidence_allocations",
            evidence.block_evidence_allocations);
        append("fragment_native_query_occurrences",
            evidence.query_occurrences);
        append("fragment_native_unique_query_plans",
            evidence.unique_query_plans);
        append("fragment_native_packed_occurrence_waves",
            evidence.packed_occurrence_waves);
        append("fragment_native_wide_occurrence_waves",
            evidence.wide_occurrence_waves);
        result += R"(,"fragment_native_evidence_saturated":)";
        result += evidence.saturated ? "true" : "false";
    }
	result += R"(,"format5_reader_memory_evidence_saturated":)";
	result += operational.format5_reader_memory_evidence_saturated
		? "true" : "false";
	result += R"(,"stats_saturated":)";
	result += operational.stats_saturated ? "true" : "false";
	if (include_provider_lifecycle) {
		const auto& evidence = operational.format5_provider_evidence;
		append("provider_configured", evidence.provider_configured);
		append("provider_captured", evidence.provider_captured);
		append("provider_persisted", evidence.provider_persisted);
		append("provider_opened", evidence.provider_opened);
		append("provider_head_scans", evidence.provider_head_scans);
		append("provider_head_rows", evidence.provider_head_rows);
		append("provider_descriptor_scans",
			evidence.provider_descriptor_scans);
		append("provider_descriptor_rows", evidence.provider_descriptor_rows);
		append("payload_tuples_formed", evidence.payload_tuples_formed);
		append("payload_tuples_inserted", evidence.payload_tuples_inserted);
		append("payload_tuples_reread", evidence.payload_tuples_reread);
		append("mapped_sink_calls", evidence.mapped_sink_calls);
		append("mapped_sink_bytes", evidence.mapped_sink_bytes);
		append("mapped_sink_flushes", evidence.mapped_sink_flushes);
		append("mapped_sink_coalescer_capacity_bytes",
			evidence.mapped_sink_coalescer_capacity_bytes);
		append("mapped_sink_pwrite_calls", evidence.mapped_sink_pwrite_calls);
		append("mapped_sink_short_writes", evidence.mapped_sink_short_writes);
		append("mapped_sink_eintr_retries", evidence.mapped_sink_eintr_retries);
		append("mapped_sink_preallocation_bytes",
			evidence.mapped_sink_preallocation_bytes);
		append("mapped_sink_preallocation_result",
			evidence.mapped_sink_preallocation_result);
		append("header_crc_bytes", evidence.header_crc_bytes);
		append("directory_crc_bytes", evidence.directory_crc_bytes);
		append("manifest_crc_bytes", evidence.manifest_crc_bytes);
		append("seal_aggregate_payload_crc_bytes",
			evidence.seal_aggregate_payload_crc_bytes);
		append("open_aggregate_payload_crc_bytes",
			evidence.open_aggregate_payload_crc_bytes);
		append("lazy_extent_cas_winners", evidence.lazy_extent_cas_winners);
		append("lazy_extent_waiters", evidence.lazy_extent_waiters);
		append("lazy_extent_executions", evidence.lazy_extent_executions);
		append("lazy_extent_reuses", evidence.lazy_extent_reuses);
		append("lazy_extent_crc_bytes", evidence.lazy_extent_crc_bytes);
		append("lazy_extent_state_bytes", evidence.lazy_extent_state_bytes);
		append("lazy_extent_invalid_receipts",
			evidence.lazy_extent_invalid_receipts);
		append("lazy_state_layout", evidence.lazy_state_layout);
		append("mapping_bytes", evidence.mapping_bytes);
		append("mapping_count", evidence.mapping_count);
		append("mapping_resourceowner_releases",
			evidence.mapping_resourceowner_releases);
		append("file_action_high_water", evidence.file_action_high_water);
		append("file_action_identity_mismatches",
			evidence.file_action_identity_mismatches);
		append("reaper_descriptors_examined",
			evidence.reaper_descriptors_examined);
		append("reaper_relation_rows_deleted",
			evidence.reaper_relation_rows_deleted);
		append("reaper_payload_bytes_deleted",
			evidence.reaper_payload_bytes_deleted);
		append("reaper_files_queued", evidence.reaper_files_queued);
		append("reaper_remaining", evidence.reaper_remaining);
		append("orphan_entries_examined", evidence.orphan_entries_examined);
		append("orphan_files_acted", evidence.orphan_files_acted);
		append("orphan_bytes_acted", evidence.orphan_bytes_acted);
		append("orphan_remaining", evidence.orphan_remaining);
		append("orphan_cursor_wraps", evidence.orphan_cursor_wraps);
		append("madvise_choice", evidence.madvise_choice);
			append("madvise_result", evidence.madvise_result);
		}
    if (include_bucket_direct_query) {
        const auto& evidence = operational.fragment_native_query_evidence;
        append("fragment_native_peak_query_group_offset_capacity_bytes",
            evidence.peak_query_group_offset_capacity_bytes);
        append("fragment_native_peak_query_membership_filter_capacity_bytes",
            evidence.peak_query_membership_filter_capacity_bytes);
        append("fragment_native_query_group_offset_allocations",
            evidence.query_group_offset_allocations);
        append("fragment_native_query_membership_filter_allocations",
            evidence.query_membership_filter_allocations);
    }
	if (include_compact_direct_one_decode) {
		const auto& evidence = operational.fragment_native_query_evidence;
		append("fragment_native_compact_direct_lookup_ns",
			evidence.compact_direct_lookup_ns);
		append("fragment_native_compact_direct_selection_shards",
			evidence.compact_direct_selection_shards);
		append("fragment_native_compact_direct_shard_query_layouts",
			evidence.compact_direct_shard_query_layouts);
		append("fragment_native_compact_direct_layout_shards_completed",
			evidence.compact_direct_layout_shards_completed);
		append("fragment_native_compact_direct_emission_shards_completed",
			evidence.compact_direct_emission_shards_completed);
		append("fragment_native_compact_direct_posting_visit_passes",
			evidence.compact_direct_posting_visit_passes);
		append("fragment_native_compact_direct_selected_postings",
			evidence.compact_direct_selected_postings);
		append("fragment_native_compact_direct_layout_postings_visited",
			evidence.compact_direct_layout_postings_visited);
		append("fragment_native_compact_direct_emission_postings_visited",
			evidence.compact_direct_emission_postings_visited);
		append("fragment_native_compact_direct_target_control_scan_bytes",
			evidence.compact_direct_target_control_scan_bytes);
		append("fragment_native_compact_direct_target_control_scan_passes",
			evidence.compact_direct_target_control_scan_passes);
		append("fragment_native_compact_direct_fallbacks",
			evidence.compact_direct_fallbacks);
		append(
			"fragment_native_compact_direct_traversal_rank_checkpoints_examined",
			evidence.compact_direct_traversal_rank_checkpoints_examined);
		append(
			"fragment_native_compact_direct_traversal_membership_words_examined",
			evidence.compact_direct_traversal_membership_words_examined);
		append("fragment_native_compact_direct_rejected_posting_reads",
			evidence.compact_direct_rejected_posting_reads);
	}
	result += '}';
    return result;
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v2(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot, Format5ReaderMemoryEvidenceVersion::exact_array_layout);
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v3(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot, Format5ReaderMemoryEvidenceVersion::fragment_native_query);
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v4(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot, Format5ReaderMemoryEvidenceVersion::provider_lifecycle);
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v5(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot, Format5ReaderMemoryEvidenceVersion::bucket_direct_query);
}

[[nodiscard]] std::string format5_reader_memory_evidence_json_v6(
        const alignment_exec::AlignmentExecutionSnapshot& snapshot) {
    return format5_reader_memory_evidence_json_versioned(
        snapshot,
        Format5ReaderMemoryEvidenceVersion::compact_direct_one_decode);
}

[[nodiscard]] std::string format6_reader_evidence_json(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot)
{
	const auto& operational = snapshot.pipeline.operational;
	const auto& evidence = operational.format6_reader_evidence;
	std::string result =
		R"({"schema":"bioseq.format6_reader_evidence","version":1)";
	result.reserve(3'072);
	const auto append_signed = [&result](
			const std::string_view key,
			const std::int64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append_string = [&result](
			const std::string_view key,
			const std::string_view value) {
		result += ",\"";
		result.append(key);
		result += "\":\"";
		result.append(value);
		result += '"';
	};
	append_signed("execution_id", snapshot.execution_id);
	append_signed("backend_pid", snapshot.backend_pid);
	append("database_oid", snapshot.database_oid);
	append("format_version", evidence.format_version);
	append("sessions", evidence.sessions);
	append("generation_id", evidence.generation_id);
	append_string(
		"source_identity",
		fixed_hex_bytes(evidence.source_identity));
	append_string(
		"source_content_crc64",
		fixed_hex_u64(evidence.source_content_crc64));
	append_string(
		"compatibility_digest",
		fixed_hex_bytes(evidence.compatibility_digest));
	append("directory_rows", evidence.directory_rows);
	append("directory_bytes", evidence.directory_bytes);
	append("control_rows", evidence.control_rows);
	append("control_bytes", evidence.control_bytes);
	append("posting_rows", evidence.posting_rows);
	append("posting_bytes", evidence.posting_bytes);
	append(
		"directory_row_crc_receipts",
		evidence.directory_row_crc_receipts);
	append(
		"directory_stream_crc_receipts",
		evidence.directory_stream_crc_receipts);
	append(
		"control_row_crc_receipts",
		evidence.control_row_crc_receipts);
	append(
		"control_block_crc_receipts",
		evidence.control_block_crc_receipts);
	append(
		"control_semantic_receipts",
		evidence.control_semantic_receipts);
	append(
		"posting_row_crc_receipts",
		evidence.posting_row_crc_receipts);
	append(
		"posting_ranges_delivered",
		evidence.posting_ranges_delivered);
	append(
		"posting_range_crc_receipts",
		evidence.posting_range_crc_receipts);
	append(
		"posting_semantic_receipts",
		evidence.posting_semantic_receipts);
	append(
		"authorized_posting_bytes",
		evidence.authorized_posting_bytes);
	append("query_count", evidence.query_count);
	append("selected_block_count", evidence.selected_block_count);
	append("accepted_hash_count", evidence.accepted_hash_count);
	append(
		"retained_position_count",
		evidence.retained_position_count);
	append(
		"directory_phase_peak_bytes",
		evidence.directory_phase_peak_bytes);
	append(
		"control_phase_peak_bytes",
		evidence.control_phase_peak_bytes);
	append(
		"posting_phase_peak_bytes",
		evidence.posting_phase_peak_bytes);
	append("peak_admitted_bytes", evidence.peak_admitted_bytes);
	append("retained_owner_bytes", evidence.retained_owner_bytes);
	append("admitted_bytes", evidence.admitted_bytes);
	append(
		"legacy_assembly_calls",
		operational.persisted_hash_build_calls);
	append(
		"legacy_globalize_calls",
		operational.persisted_legacy_globalize_calls);
	append(
		"legacy_rebase_calls",
		operational.persisted_legacy_rebase_calls);
	append(
		"recompute_calls",
		operational.recomputed_target_index_rows);
	append(
		"generic_source_fallback_calls",
		evidence.generic_source_fallback_calls);
	append("backend_fallbacks", operational.backend_fallbacks);
	append("failures", evidence.failures);
	result += R"(,"saturated":)";
	result += evidence.saturated ? "true" : "false";
	result += R"(,"operational_identity_conflict":)";
	result += operational.operational_identity_conflict
		? "true" : "false";
	result += R"(,"stats_saturated":)";
	result += operational.stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

[[nodiscard]] std::string ready_minimizer_format6_inspection_json(
		const bioseq::storage::indexing::
			ReadyMinimizerFormat6Inspection& inspection)
{
	std::string result =
		R"({"schema":"bioseq.ready_minimizer_format6_generation","version":1,"format_version":6)";
	result.reserve(4'096);
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append_string = [&result](
			const std::string_view key,
			const std::string_view value) {
		result += ",\"";
		result.append(key);
		result += "\":\"";
		result.append(value);
		result += '"';
	};
	const auto bounded_name = []<std::size_t N>(
			const std::array<char, N>& value) {
		const auto end = std::find(value.begin(), value.end(), '\0');
		return std::string_view{
			value.data(),
			static_cast<std::size_t>(end - value.begin())};
	};
	const auto stream_kind_name = [](
			const bioseq::indexing::MinimizerFormat6StreamKind kind)
			-> std::string_view {
		switch (kind) {
			case bioseq::indexing::MinimizerFormat6StreamKind::directory:
				return "directory";
			case bioseq::indexing::MinimizerFormat6StreamKind::controls:
				return "controls";
			case bioseq::indexing::MinimizerFormat6StreamKind::postings:
				return "postings";
		}
		return "invalid";
	};

	append("database_oid", inspection.database_oid);
	append("main_relfilenode", inspection.main_relfilenode);
	append(
		"column_attno",
		static_cast<std::uint64_t>(inspection.column_attno));
	append("query_key", inspection.query_key);
	append_string("table_name", bounded_name(inspection.table_name));
	append_string("index_name", bounded_name(inspection.index_name));

	const auto& source = inspection.source;
	result += R"(,"source":{"sequence_rowno":)";
	result += std::to_string(source.sequence_rowno);
	result += R"(,"locator_version":)";
	result += std::to_string(source.locator.version);
	result += R"(,"bucket_idx":)";
	result += std::to_string(source.locator.bucket_idx);
	result += R"(,"shard_idx":)";
	result += std::to_string(source.locator.shard_idx);
	result += R"(,"num_fragments":)";
	result += std::to_string(source.locator.num_fragments);
	result += R"(,"total_bp_size":)";
	result += std::to_string(source.locator.total_bp_size);
	result += R"(,"first_sub_row_no":)";
	result += std::to_string(source.locator.first_sub_row_no);
	result += R"(,"source_epoch":)";
	result += std::to_string(source.locator.source_epoch);
	result += R"(,"padding_letters":)";
	result += std::to_string(source.padding_letters);
	result += R"(,"alphabet_code":)";
	result += std::to_string(source.alphabet_code);
	result += R"(,"storage_unit_byte_width":)";
	result += std::to_string(source.storage_unit_byte_width);
	result += R"(,"source_content_crc64":")";
	result += fixed_hex_u64(inspection.source_content_crc64);
	result += R"(","source_identity":")";
	result += fixed_hex_bytes(inspection.source_identity);
	result += R"("})";

	const auto& head = inspection.head;
	result += R"(,"head":{"generation_id":)";
	result += std::to_string(head.generation_id);
	result += R"(,"query_key":)";
	result += std::to_string(head.query_key);
	result += R"(,"source_epoch":)";
	result += std::to_string(head.source_epoch);
	result += R"(,"source_identity":")";
	result += fixed_hex_bytes(head.source_identity);
	result += R"(","source_content_crc64":")";
	result += fixed_hex_u64(head.source_content_crc64);
	result += R"(","compatibility_digest":")";
	result += fixed_hex_bytes(head.compatibility_digest);
	result += R"(","manifest_address_bound_row_crc64":")";
	result += fixed_hex_u64(head.manifest_address_bound_row_crc64);
	result += R"(","manifest_payload_crc64":")";
	result += fixed_hex_u64(head.manifest_payload_crc64);
	result += R"(","head_crc64":")";
	result += fixed_hex_u64(head.head_crc64);
	result += R"(","dense_seal_present":)";
	result += head.dense_seal.has_value() ? "true" : "false";
	result += '}';

	const auto& manifest = inspection.manifest;
	const auto& compatibility = manifest.compatibility;
	result += R"(,"manifest":{"generation_id":)";
	result += std::to_string(manifest.generation_id);
	result += R"(,"source_epoch":)";
	result += std::to_string(manifest.source_epoch);
	result += R"(,"source_identity":")";
	result += fixed_hex_bytes(manifest.source_identity);
	result += R"(","source_content_crc64":")";
	result += fixed_hex_u64(manifest.source_content_crc64);
	result += R"(","target_length":)";
	result += std::to_string(manifest.target_length);
	result += R"(,"unique_hash_count":)";
	result += std::to_string(manifest.unique_hash_count);
	result += R"(,"total_posting_count":)";
	result += std::to_string(manifest.total_posting_count);
	result += R"(,"compatibility_digest":")";
	result += fixed_hex_bytes(manifest.compatibility_digest);
	result += R"(","authenticated_directory_crc64":")";
	result += fixed_hex_u64(manifest.authenticated_directory_crc64);
	result += R"(","generation_crc64":")";
	result += fixed_hex_u64(manifest.generation_crc64);
	result += R"(","total_control_bytes":)";
	result += std::to_string(manifest.total_control_bytes);
	result += R"(,"total_posting_bytes":)";
	result += std::to_string(manifest.total_posting_bytes);
	result += R"(,"directory_entry_count":)";
	result += std::to_string(manifest.directory_entry_count);
	result += R"(,"compatibility":{"hash_scheme_id":)";
	result += std::to_string(compatibility.hash_scheme_id);
	result += R"(,"hash_scheme_version":)";
	result += std::to_string(compatibility.hash_scheme_version);
	result += R"(,"alphabet_code":)";
	result += std::to_string(compatibility.alphabet_code);
	result += R"(,"kmer_size":)";
	result += std::to_string(compatibility.kmer_size);
	result += R"(,"window_size":)";
	result += std::to_string(compatibility.window_size);
	result += R"(,"hash_width_bytes":)";
	result += std::to_string(compatibility.hash_width_bytes);
	result += R"(,"stored_posting_width_bytes":)";
	result += std::to_string(
		compatibility.stored_posting_width_bytes);
	result += R"(,"position_encoding_version":)";
	result += std::to_string(
		compatibility.position_encoding_version);
	result += R"(,"hash_semantics_version":)";
	result += std::to_string(compatibility.hash_semantics_version);
	result += R"(,"count_semantics_version":)";
	result += std::to_string(compatibility.count_semantics_version);
	result += R"(,"producer_backend_id":)";
	result += std::to_string(compatibility.producer_backend_id);
	result += R"(,"producer_backend_version":)";
	result += std::to_string(compatibility.producer_backend_version);
	result += R"(,"controls_per_block":)";
	result += std::to_string(compatibility.controls_per_block);
	result += R"(,"hash_seed":)";
	result += std::to_string(compatibility.hash_seed);
	result += R"(}})";

	result += R"(,"metadata":{"head_rows":)";
	result += std::to_string(inspection.head_rows);
	result += R"(,"head_bytes":)";
	result += std::to_string(inspection.head_bytes);
	result += R"(,"manifest_rows":)";
	result += std::to_string(inspection.manifest_rows);
	result += R"(,"manifest_bytes":)";
	result += std::to_string(inspection.manifest_bytes);
	result += '}';

	result += R"(,"streams":[)";
	for (std::size_t index = 0; index < inspection.streams.size(); ++index) {
		if (index != 0u) result += ',';
		const auto& stream = inspection.streams[index];
		result += R"({"kind":")";
		result.append(stream_kind_name(stream.kind));
		result += R"(","element_count":)";
		result += std::to_string(stream.element_count);
		result += R"(,"logical_bytes":)";
		result += std::to_string(stream.logical_bytes);
		result += R"(,"row_count":)";
		result += std::to_string(stream.row_count);
		result += R"(,"stream_crc64":")";
		result += fixed_hex_u64(stream.stream_crc64);
		result += R"(","exact_catalog_rows":)";
		result += std::to_string(stream.exact_catalog_rows);
		result += R"(,"exact_catalog_bytes":)";
		result += std::to_string(stream.exact_catalog_bytes);
		result += R"(,"exact_catalog_stream_crc64":")";
		result += fixed_hex_u64(
			stream.exact_catalog_stream_crc64);
		result += R"("})";
	}
	result += "]}";
	return result;
}

[[nodiscard]] std::string format6_parallel_scan_probe_json(
		const bioseq::storage::indexing::
			MinimizerFormat6ParallelScanProbePg& proof)
{
	std::string result =
		R"({"schema":"bioseq_internal.format6_parallel_scan_probe","version":1)";
	result.reserve(768);
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append("requested_workers", proof.requested_workers);
	append("launched_workers", proof.launched_workers);
	append("generation_id", proof.generation_id);
	append("query_key", proof.query_key);
	append("expected_rows", proof.expected_rows);
	append("scanned_rows", proof.scanned_rows);
	append("expected_bytes", proof.expected_bytes);
	append("scanned_bytes", proof.scanned_bytes);
	append("duplicate_rows", proof.duplicate_rows);
	append("missing_rows", proof.missing_rows);
	append("corrupt_rows", proof.corrupt_rows);
	append("elapsed_ns", proof.elapsed_ns);
	result += R"(,"checksum_digest":")";
	result += fixed_hex_u64(proof.checksum_digest);
	result += R"(","receipt_digest":")";
	result += fixed_hex_u64(proof.receipt_digest);
	result += R"(","leader_participated":)";
	result += proof.leader_participated ? "true" : "false";
	result += '}';
	return result;
}

[[nodiscard]] std::string persisted_gpu_operation_evidence_json(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot,
		const bool include_cache_qualification_v2)
{
	const auto& ledger = snapshot.persisted_gpu_operation_evidence;
	const auto& stats = snapshot.pipeline;
	const bool requires_evidence =
		stats.actual_backend
			== bioseq::pipeline_stats::ExecutedBackend::vulkan_gpu
		&& stats.actual_target_index_source
			== bioseq::pipeline_stats::TargetIndexSource::persisted;
	const bool ledger_complete = ledger.complete();
	const bool pipeline_matches = requires_evidence
		? ledger.matches_pipeline(stats)
		: ledger.operation_count() == 0u && ledger.job_count() == 0u;
	const bool complete = ledger_complete && pipeline_matches;
	const bool saturated = ledger.saturated()
		|| stats.operational.stats_saturated;
	const auto* first_chunk = ledger.head();
	const bioseq::pipeline_stats::PersistedGpuCacheQualificationEvidence*
		cache_qualification = first_chunk == nullptr
		? nullptr
		: &first_chunk->operation().cache_qualification;
	bool cache_qualification_complete = cache_qualification != nullptr
		&& cache_qualification->accepted();
	for (const auto* chunk = first_chunk; chunk != nullptr;
		 chunk = chunk->next()) {
		cache_qualification_complete = cache_qualification_complete
			&& chunk->operation().cache_qualification
				== *cache_qualification;
	}
	const bool accepted = requires_evidence && snapshot.completed
		&& !snapshot.partial && snapshot.fully_drained && complete
		&& !saturated && ledger.dropped_operation_count() == 0u
		&& (!include_cache_qualification_v2
			|| cache_qualification_complete);

	std::string result =
		R"({"schema":"bioseq.persisted_gpu_operation_evidence","version":)";
	result += include_cache_qualification_v2 ? "2" : "1";
	result.reserve(2048u + static_cast<std::size_t>(ledger.job_count()) * 1400u);
	const auto append_unsigned = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append_signed = [&result](
			const std::string_view key,
			const std::int64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	const auto append_bool = [&result](
			const std::string_view key,
			const bool value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += value ? "true" : "false";
	};
	const auto append_hex = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":\"";
		result += fixed_hex_u64(value);
		result += '"';
	};
	const auto append_text = [&result](
			const std::string_view key,
			const std::string_view value) {
		constexpr std::string_view hex = "0123456789abcdef";
		result += ",\"";
		result.append(key);
		result += "\":\"";
		for (const unsigned char byte : value) {
			if (byte == static_cast<unsigned char>('"')
				|| byte == static_cast<unsigned char>('\\')) {
				result += '\\';
				result += static_cast<char>(byte);
			} else if (byte < 0x20u || byte > 0x7eu) {
				result += "\\u00";
				result += hex[byte >> 4u];
				result += hex[byte & 0x0fu];
			} else {
				result += static_cast<char>(byte);
			}
		}
		result += '"';
	};
	append_signed("execution_id", snapshot.execution_id);
	append_signed("backend_pid", snapshot.backend_pid);
	append_unsigned("database_oid", snapshot.database_oid);
	append_bool("completed", snapshot.completed);
	append_bool("partial", snapshot.partial);
	append_bool("fully_drained", snapshot.fully_drained);
	append_bool("accepted", accepted);
	append_bool("complete", complete);
	append_bool("saturated", saturated);
	append_unsigned(
		"dropped_operation_count", ledger.dropped_operation_count());
	append_unsigned("operation_count", ledger.operation_count());
	append_unsigned(
		"accepted_operation_count",
		ledger.accepted() ? ledger.operation_count() : 0u);
	append_unsigned(
		"complete_operation_count",
		ledger_complete ? ledger.operation_count() : 0u);
	append_unsigned("job_count", ledger.job_count());
	append_unsigned(
		"accepted_job_count", ledger.accepted() ? ledger.job_count() : 0u);
	if (include_cache_qualification_v2) {
		result += R"(,"cache_qualification":)";
		if (cache_qualification == nullptr) {
			result += "null";
		} else {
			const auto& qualification = *cache_qualification;
			result += R"({"version":)";
			result += std::to_string(qualification.version);
			append_hex("identity", qualification.identity);
			append_bool("accepted", qualification.accepted());
			append_bool("saturated", qualification.saturated);
			append_unsigned("archive_count", qualification.archive_count);
			append_unsigned("archive_bytes", qualification.archive_bytes);
			append_hex("archive_checksum", qualification.archive_checksum);
			append_unsigned("receipt_count", qualification.receipt_count);
			append_unsigned("hash_passes", qualification.hash_passes);
			append_unsigned("hashed_bytes", qualification.hashed_bytes);
			append_unsigned(
				"private_files_created",
				qualification.private_files_created);
			append_unsigned(
				"private_files_unlinked",
				qualification.private_files_unlinked);
			append_unsigned(
				"live_private_paths", qualification.live_private_paths);
			append_unsigned(
				"pipeline_filesystem_hash_passes",
				qualification.pipeline_filesystem_hash_passes);
			append_unsigned(
				"pipeline_filesystem_hashed_bytes",
				qualification.pipeline_filesystem_hashed_bytes);
			append_unsigned(
				"helper_protocol_version",
				qualification.helper_protocol_version);
			append_text(
				"helper_build_identity",
				qualification.helper_build_identity_text());
			append_text(
				"helper_source", qualification.helper_source_text());
			append_unsigned(
				"helper_executable_size",
				qualification.helper_executable_size);
			result += R"(,"helper_executable_sha256":")";
			result += fixed_hex_bytes(
				qualification.helper_executable_sha256);
			result += '"';
			append_hex(
				"helper_metal_registry_id",
				qualification.helper_metal_registry_id);
			result += R"(,"device_uuid":")";
			result += fixed_hex_bytes(qualification.device_uuid);
			result += '"';
			append_unsigned("api_version", qualification.api_version);
			append_unsigned("driver_version", qualification.driver_version);
			append_unsigned("vendor_id", qualification.vendor_id);
			append_unsigned("device_id", qualification.device_id);
			result += R"(,"pipeline_cache_uuid":")";
			result += fixed_hex_bytes(qualification.pipeline_cache_uuid);
			result += '"';
			append_hex(
				"seed_receipt_identity",
				qualification.seed_receipt_identity);
			result += R"(,"seed_sha256":")";
			result += fixed_hex_bytes(qualification.seed_sha256);
			result += '"';
			append_unsigned("seed_size", qualification.seed_size);
			append_hex(
				"manifest_checksum", qualification.manifest_checksum);
			result += '}';
		}
	}
	result += R"(,"operations":[)";
	bool first_operation = true;
	for (const auto* chunk = ledger.head(); chunk != nullptr;
		 chunk = chunk->next()) {
		if (!first_operation) result += ',';
		first_operation = false;
		const auto& operation = chunk->operation();
		const auto& publication = operation.publication;
		const auto& environment = operation.environment;
		const auto& cache = operation.cache_metrics;
		result += '{';
		result += R"("operation_identity":")";
		result += fixed_hex_u64(operation.operation_identity);
		result += '"';
		append_unsigned("expected_jobs", publication.expected_jobs);
		append_unsigned("proof_jobs", publication.proof_jobs);
		append_unsigned("consumed_jobs", publication.consumed_jobs);
		append_unsigned("accepted_jobs", publication.accepted_jobs);
		append_unsigned("burned_jobs", publication.burned_jobs);
		append_unsigned("pending_jobs", publication.pending_jobs);
		append_unsigned("staged_jobs", publication.staged_jobs);
		append_unsigned("published_jobs", operation.published_jobs);
		append_unsigned("publication_commits", operation.publication_commits);
		append_unsigned(
			"query_pipeline_receipt_count",
			operation.query_pipeline_receipt_count);
		append_unsigned(
			"shape_pipeline_receipt_count",
			operation.shape_pipeline_receipt_count);
		append_unsigned("submission_count", operation.submission_count);
		append_hex(
			"query_pipeline_operation_identity",
			operation.query_pipeline_operation_identity);
		append_hex(
			"shape_pipeline_operation_identity",
			operation.shape_pipeline_operation_identity);
		result += R"(,"query_submission_ordinals":[)";
		result += std::to_string(operation.query_submission_ordinals[0]);
		result += ',';
		result += std::to_string(operation.query_submission_ordinals[1]);
		result += ']';
		append_hex("seed_receipt_identity", operation.seed_receipt_identity);
		append_hex("cache_token_identity", cache.token_identity);
		if (include_cache_qualification_v2) {
			append_hex(
				"cache_qualification_identity",
				operation.cache_qualification.identity);
		}
		append_unsigned("generation_id", operation.generation_id);
		append_hex("source_identity", operation.source_identity);
		append_hex("cold_load_identity", operation.cold_load_identity);
		result += R"(,"source":"persisted_shape_v5")";
		result += R"(,"environment":{"device_uuid":")";
		result += fixed_hex_bytes(environment.device_uuid);
		result += '"';
		append_unsigned("api_version", environment.api_version);
		append_unsigned("driver_version", environment.driver_version);
		append_unsigned("vendor_id", environment.vendor_id);
		append_unsigned("device_id", environment.device_id);
		result += R"(,"pipeline_cache_uuid":")";
		result += fixed_hex_bytes(environment.pipeline_cache_uuid);
		result += R"(","seed_sha256":")";
		result += fixed_hex_bytes(environment.seed_sha256);
		result += '"';
		append_unsigned("seed_size", environment.seed_size);
		append_hex("manifest_checksum", environment.manifest_checksum);
		result += '}';
		result += R"(,"cache_metrics":{"pipeline_cache_loads":)";
		result += std::to_string(cache.pipeline_cache_loads);
		append_unsigned(
			"pipeline_cache_seed_bytes", cache.pipeline_cache_seed_bytes);
		append_unsigned("pipeline_cache_load_ns", cache.pipeline_cache_load_ns);
		append_unsigned("pipeline_creates", cache.pipeline_creates);
		append_unsigned("pipeline_creation_ns", cache.pipeline_creation_ns);
		result += '}';
		result += R"(,"jobs":[)";
		bool first_job = true;
		for (const auto& job : chunk->jobs()) {
			if (!first_job) result += ',';
			first_job = false;
			result += '{';
			result += R"("logical_target_row":)";
			result += std::to_string(job.logical_target_row);
			append_unsigned(
				"logical_query_ordinal", job.logical_query_ordinal);
			append_unsigned("query_index", job.query_index);
			append_unsigned("query_length", job.query_length);
			append_unsigned(
				"query_minimizer_count", job.query_minimizer_count);
			append_unsigned("query_tile_count", job.query_tile_count);
			result += R"(,"query":{"count_dispatches":)";
			result += std::to_string(job.query_count_dispatches);
			append_unsigned("scan_dispatches", job.query_scan_dispatches);
			append_unsigned("emit_dispatches", job.query_emit_dispatches);
			append_unsigned(
				"completed_roundtrips", job.query_completed_roundtrips);
			result += '}';
			result += R"(,"direct":{"count_dispatches":)";
			result += std::to_string(job.direct_count_dispatches);
			append_unsigned("scan_dispatches", job.direct_scan_dispatches);
			append_unsigned("anchor_count", job.anchor_count);
			append_unsigned("emit_dispatches", job.direct_emit_dispatches);
			append_unsigned("sort_operations", job.sort_operations);
			append_unsigned(
				"range_build_dispatches", job.range_build_dispatches);
			append_unsigned("range_count", job.range_count);
			append_unsigned("chain_dispatches", job.chain_dispatches);
			append_unsigned(
				"chain_backtrack_dispatches",
				job.chain_backtrack_dispatches);
			append_unsigned(
				"coordinate_dispatches", job.coordinate_dispatches);
			result += '}';
			result += R"(,"submissions":[)";
			constexpr std::array<std::string_view, 4> stages{
				"count", "emit", "sort_range", "chain_coordinate"};
			for (std::size_t i = 0u; i < job.submissions.size(); ++i) {
				if (i != 0u) result += ',';
				const auto& submission = job.submissions[i];
				result += R"({"stage":")";
				result.append(stages[i]);
				result += R"(","operation_identity":")";
				result += fixed_hex_u64(submission.operation_identity);
				result += R"(","ordinal":)";
				result += std::to_string(submission.ordinal);
				result += R"(,"submit_wait_identity":")";
				result += fixed_hex_u64(submission.submit_wait_identity);
				result += R"("})";
			}
			result += ']';
			result += R"(,"status":{"count_checks":)";
			result += std::to_string(job.count_status_checks);
			append_unsigned("emit_checks", job.emit_status_checks);
			append_unsigned("status_or", job.status_or);
			append_unsigned("result_readbacks", job.result_readbacks);
			result += '}';
			result += R"(,"forbidden":{"coordinate_vote_dispatches":)";
			result += std::to_string(job.coordinate_vote_dispatches);
			append_unsigned(
				"cpu_or_simd_biological_calls",
				job.cpu_or_simd_biological_calls);
			append_unsigned("recompute_calls", job.recompute_calls);
			append_unsigned("fallback_calls", job.fallback_calls);
			result += '}';
			const auto& coordinate = job.coordinate;
			result += R"(,"coordinate":{"valid":)";
			result += coordinate.valid ? "true" : "false";
			append_unsigned("query_start", coordinate.query_start);
			append_unsigned("query_end", coordinate.query_end);
			append_unsigned("target_start", coordinate.target_start);
			append_unsigned("target_end", coordinate.target_end);
			append_unsigned("match_length", coordinate.match_length);
			append_unsigned("block_length", coordinate.block_length);
			append_signed("chain_score", coordinate.chain_score);
			append_signed("seed_count", coordinate.seed_count);
			append_bool("is_reverse", coordinate.is_reverse);
			result += '}';
			result += '}';
		}
		result += ']';
		result += '}';
	}
	result += ']';
	result += '}';
	return result;
}

[[nodiscard]] std::string cold_index_selective_probe_json(
		const bioseq::storage::indexing::ColdIndexPreparationStatsPg& stats)
{
	std::string result =
		R"({"schema":"bioseq.cold_index_selective_probe","version":2,"timing_semantics":"payload_inclusive_wall_v1","decode_timing_available":false,"transfer_timing_available":false)";
	result.reserve(1024);
	const auto append = [&result](
			const std::string_view key,
			const std::uint64_t value) {
		result += ",\"";
		result.append(key);
		result += "\":";
		result += std::to_string(value);
	};
	append("route_empty", stats.route_empty);
	append("route_selective", stats.route_selective);
	append("route_full", stats.route_full);
	append("payload_rows_requested", stats.payload_rows_requested);
	append("payload_rows_delivered", stats.payload_rows_delivered);
	append("payload_rows_scanned", stats.payload_rows_scanned);
	append("payload_rows_skipped", stats.payload_rows_skipped);
	append("payload_bytes_requested", stats.payload_bytes_requested);
	append("payload_bytes_delivered", stats.payload_bytes_delivered);
	append("payload_bytes_scanned", stats.payload_bytes_scanned);
	append("heap_tid_adjacent", stats.heap_tid_adjacent);
	append("heap_tid_forward_gap", stats.heap_tid_forward_gap);
	append("heap_tid_backward_jump", stats.heap_tid_backward_jump);
	append("extent_crc_bytes", stats.extent_crc_bytes);
	append("extent_crc_raw_bytes_hashed", stats.extent_crc_raw_bytes_hashed);
	append("extent_crc_combined_bytes", stats.extent_crc_combined_bytes);
	append("extent_crc_combine_rows", stats.extent_crc_combine_rows);
	append("row_crc_bytes", stats.row_crc_bytes);
	append("payload_ns", stats.payload_ns);
	result += R"(,"decode_ns":null,"transfer_ns":null)";
	result += R"(,"stats_saturated":)";
	result += stats.stats_saturated ? "true" : "false";
	result += '}';
	return result;
}

template<int ColumnCount>
struct AlignmentStatsRowBuilder {
	bioseq::pg_utils::pg_datum_list<ColumnCount> row{};
	int next_attnum = 1;

	void null_value() noexcept
	{
		row.set_null(next_attnum++);
	}

	void boolean(bool value) noexcept
	{
		row.set(next_attnum++, value);
	}

	void oid(Oid value) noexcept
	{
		row.set_oid(next_attnum++, value);
	}

	void integer(std::int32_t value) noexcept
	{
		row.set(next_attnum++, value);
	}

	void unsigned_integer(std::uint32_t value)
	{
		if (value > bioseq::pipeline_stats::sql_integer_value_max) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{.message =
					"bioseq alignment statistic exceeds SQL integer"});
		}
		integer(static_cast<std::int32_t>(value));
	}

	void signed_bigint(std::int64_t value) noexcept
	{
		row.set(next_attnum++, value);
	}

	void bigint(std::uint64_t value)
	{
		if (value > bioseq::pipeline_stats::sql_bigint_value_max) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{.message =
					"bioseq alignment statistic exceeds SQL bigint"});
		}
		signed_bigint(static_cast<std::int64_t>(value));
	}

	void text(std::string_view value)
	{
		auto* datum = bioseq::pg_utils::invoke_pg_as_cpp(
			[value] {
				return cstring_to_text_with_len(
					value.data(), static_cast<int>(value.size()));
			});
		row.set_raw(
			next_attnum++,
			bioseq::pg_utils::pg_datum::from_pointer(datum));
	}

	void jsonb(std::string_view encoded)
	{
		const std::string terminated{encoded};
		const Datum datum = bioseq::pg_utils::invoke_pg_as_cpp(
			[&terminated] {
				return DirectFunctionCall1(
					jsonb_in, CStringGetDatum(terminated.c_str()));
			});
		row.set_raw(
			next_attnum++,
			bioseq::pg_utils::pg_datum::from_raw(datum));
	}

	void hex_u64(std::uint64_t value)
	{
		const std::string encoded = fixed_hex_u64(value);
		text(encoded);
	}

	void finish() const
	{
		if (next_attnum != ColumnCount + 1) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{
					.message =
						"bioseq alignment result mapping is incomplete",
					.detail = fmt::format(
						"mapped={} expected={}",
						next_attnum - 1, ColumnCount),
				});
		}
	}
};

[[nodiscard]] bioseq::pipeline_stats::ExecutedBackend
validate_alignment_snapshot_environment(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot)
{
	const auto& stats = snapshot.pipeline;
	bioseq::pipeline_stats::ExecutedBackend environment_backend =
		stats.actual_backend;
	if (!bioseq::pipeline_stats::is_concrete_backend(environment_backend)) {
		/*
		 * A statement whose alignment child is genuinely empty still publishes
		 * one row per pipeline stage.  That is diagnostic evidence, not an
		 * execution proof: both backend identities and the environment remain
		 * exactly unset and every job counter is zero.  Do not force a concrete
		 * backend onto this shape (which would launder an empty statement into
		 * backend acceptance), but do allow the SRF to materialise its twenty
		 * zero-work rows.
		 */
		const bool exact_zero_work_snapshot =
			environment_backend
				== bioseq::pipeline_stats::ExecutedBackend::none
			&& stats.requested_backend
				== bioseq::pipeline_stats::ExecutedBackend::none
			&& stats.attempted_alignment_jobs == 0
			&& stats.fully_attested_alignment_jobs == 0
			&& stats.unattested_alignment_jobs == 0
			&& stats.operational.target_rows == 0
			&& bioseq::pipeline_stats::ExecutionEnvironmentProof::is_unset(
				stats.environment);
		if (exact_zero_work_snapshot) {
			return bioseq::pipeline_stats::ExecutedBackend::none;
		}
		const bool honest_diagnostic =
			bioseq::pipeline_stats::PipelineExecutionStats::
				diagnostic_environment_is_valid(stats);
		if (!honest_diagnostic) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_DATA_EXCEPTION,
				{.message =
					"bioseq alignment snapshot has no valid environment backend"});
		}
		return environment_backend;
	}
	if (!bioseq::pipeline_stats::environment_is_valid_for_backend(
			stats.environment, environment_backend)) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_DATA_EXCEPTION,
			{
				.message =
					"bioseq alignment snapshot contains an invalid execution environment",
				.detail = fmt::format(
					"requested_backend={} actual_backend={} "
					"workers={} gpu_present={}",
					bioseq::pipeline_stats::executed_backend_name(
						stats.requested_backend),
					bioseq::pipeline_stats::executed_backend_name(
						stats.actual_backend),
					stats.environment.effective_cpu_workers,
					stats.environment.gpu_environment_present),
			});
	}
	return environment_backend;
}

template<int ColumnCount>
[[nodiscard]] bioseq::pg_utils::pg_datum_list<ColumnCount>
make_alignment_stats_row(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot,
		bioseq::pipeline_stats::PipelineStage stage)
{
	static_assert(
		ColumnCount == alignment_stats_v1_column_count
		|| ColumnCount == alignment_stats_v2_column_count);
	AlignmentStatsRowBuilder<ColumnCount> out{};
	const auto& stats = snapshot.pipeline;
	const auto& environment = stats.environment;
	const auto& operational = stats.operational;
	const auto stage_no = bioseq::pipeline_stats::stage_index(stage);
	const auto& coverage = stats.stages.coverage[stage_no];
	const auto& timing = operational.timing[stage_no];

	out.signed_bigint(snapshot.execution_id);                         // 1
	out.integer(snapshot.backend_pid);                               // 2
	out.oid(snapshot.database_oid);                                  // 3
	out.unsigned_integer(environment.effective_cpu_workers);          // 4
	if (environment.gpu_environment_present) {
		const std::string uuid = fixed_hex_bytes(
			environment.gpu_device_uuid);
		out.text(uuid);                                                // 5
		if (bioseq::pipeline_stats::canonical_nonempty_driver_identity(
				environment.gpu_driver_identity)) {
			std::size_t driver_size = 0;
			while (driver_size < environment.gpu_driver_identity.size()
				&& environment.gpu_driver_identity[driver_size] != '\0') {
				++driver_size;
			}
			out.text(std::string_view{
				environment.gpu_driver_identity.data(), driver_size});
			} else {
				bioseq::pg_utils::raise_sql_error(
					ERRCODE_DATA_EXCEPTION,
					{.message =
						"bioseq GPU driver identity is invalid"});
			}                                                           // 6
		out.text(bioseq::pipeline_stats::pipeline_cache_policy_name(
			environment.gpu_pipeline_cache_policy));                   // 7
		const std::string seed_sha = fixed_hex_bytes(
			environment.gpu_pipeline_cache_seed_sha256);
		out.text(seed_sha);                                           // 8
		out.bigint(environment.gpu_pipeline_cache_seed_size);          // 9
		out.hex_u64(environment.gpu_pipeline_cache_manifest_checksum); // 10
		switch (environment.gpu_pipeline_cache_hit) {
			case bioseq::pipeline_stats::OptionalProofBool::false_value:
				out.boolean(false);
				break;
			case bioseq::pipeline_stats::OptionalProofBool::true_value:
				out.boolean(true);
				break;
			default:
				bioseq::pg_utils::raise_sql_error(
					ERRCODE_DATA_EXCEPTION,
					{.message =
						"bioseq GPU pipeline-cache hit proof is invalid"});
		}                                                           // 11
	} else {
		for (int i = 0; i < 7; ++i) out.null_value();                // 5-11
	}
	out.integer(snapshot.scan_node_count);                           // 12
	out.integer(snapshot.nested_alignment_executor_count);           // 13
	out.boolean(snapshot.completed);                                 // 14
	out.boolean(snapshot.partial);                                   // 15
	out.boolean(snapshot.fully_drained);                             // 16
	out.text(bioseq::pipeline_stats::executed_backend_name(
		stats.requested_backend));                                    // 17
	out.text(bioseq::pipeline_stats::executed_backend_name(
		stats.actual_backend));                                       // 18
	out.text(bioseq::pipeline_stats::target_index_source_name(
		stats.requested_target_index_source));                         // 19
	out.text(bioseq::pipeline_stats::target_index_source_name(
		stats.actual_target_index_source));                            // 20
	out.bigint(stats.persisted_generation_id);                       // 21
	out.hex_u64(stats.persisted_source_identity);                    // 22
	out.boolean(stats.mixed_persisted_identity);                     // 23
	out.text(bioseq::pipeline_stats::pipeline_stage_name(stage));    // 24
	out.bigint(coverage.eligible_jobs);                              // 25
	out.bigint(coverage.executed_jobs[
		bioseq::pipeline_stats::engine_index(
			bioseq::pipeline_stats::ExecutionEngine::host_io)]);        // 26
	out.bigint(coverage.executed_jobs[
		bioseq::pipeline_stats::engine_index(
			bioseq::pipeline_stats::ExecutionEngine::scalar_cpu)]);     // 27
	out.bigint(coverage.executed_jobs[
		bioseq::pipeline_stats::engine_index(
			bioseq::pipeline_stats::ExecutionEngine::simd_cpu)]);       // 28
	out.bigint(coverage.executed_jobs[
		bioseq::pipeline_stats::engine_index(
			bioseq::pipeline_stats::ExecutionEngine::vulkan_gpu)]);     // 29
	out.bigint(coverage.fallback_jobs);                              // 30
	out.bigint(timing.host_wall_ns);                                 // 31
	out.bigint(timing.gpu_device_ns);                                // 32
	out.bigint(operational.target_rows);                             // 33
	out.bigint(stats.attempted_alignment_jobs);                      // 34
	out.bigint(stats.fully_attested_alignment_jobs);                 // 35
	out.bigint(stats.unattested_alignment_jobs);                     // 36
	if (stats.first_unattested_target_row
		== bioseq::pipeline_stats::unset_job_ordinal) out.null_value();
	else out.bigint(stats.first_unattested_target_row);              // 37
	if (stats.first_unattested_query_ordinal
		== bioseq::pipeline_stats::unset_job_ordinal) out.null_value();
	else out.bigint(stats.first_unattested_query_ordinal);           // 38
	out.bigint(operational.cpu_prepared_index_builds);               // 39
	out.bigint(operational.cpu_prepared_index_reuses);               // 40
	out.bigint(operational.cpu_prepared_unique_keys);                // 41
	out.bigint(operational.cpu_prepared_duplicate_build_attempts);   // 42
	if (operational.cpu_prepared_key_set_checksum_present)
		out.hex_u64(operational.cpu_prepared_key_set_checksum);
	else out.null_value();                                           // 43
	if (operational.cpu_prepared_build_set_checksum_present)
		out.hex_u64(operational.cpu_prepared_build_set_checksum);
	else out.null_value();                                           // 44
	out.boolean(operational.operational_identity_conflict);          // 45
	out.bigint(operational.persisted_index_rows);                    // 46
	out.bigint(operational.missing_persisted_index_rows);            // 47
	out.bigint(operational.recomputed_target_index_rows);            // 48
	out.bigint(operational.persisted_generation_read_sessions);      // 49
	out.bigint(operational.persisted_relation_opens);                // 50
	out.bigint(operational.persisted_index_scans);                   // 51
	out.bigint(operational.persisted_head_scans);                    // 52
	out.bigint(operational.persisted_head_rows_read);                // 53
	out.bigint(operational.persisted_head_bytes_read);               // 54
	out.bigint(operational.persisted_manifest_directory_scans);      // 55
	out.bigint(operational.persisted_manifest_rows_read);            // 56
	out.bigint(operational.persisted_manifest_bytes_read);           // 57
	out.bigint(operational.persisted_directory_chunks_read);         // 58
	out.bigint(operational.persisted_directory_bytes_read);          // 59
	out.bigint(operational.persisted_metadata_repeated_reads);       // 60
	out.bigint(operational.persisted_metadata_heap_rows_read);       // 61
	out.bigint(operational.persisted_metadata_bytes_read);           // 62
	out.bigint(operational.persisted_payload_heap_rows_read);        // 63
	out.bigint(operational.persisted_payload_bytes_read);            // 64
	out.bigint(operational.persisted_heap_rows_read);                // 65
	out.bigint(operational.persisted_toast_values_read);             // 66
	out.bigint(operational.persisted_detoast_bytes);                 // 67
	out.bigint(operational.persisted_explicit_copy_bytes);           // 68
	out.bigint(operational.persisted_wanted_subkeys);                // 69
	out.bigint(operational.persisted_read_subkeys);                  // 70
	out.bigint(operational.persisted_wanted_extents);                // 71
	out.bigint(operational.persisted_wanted_chunks);                 // 72
	out.bigint(operational.persisted_skipped_rows);                  // 73
	out.bigint(operational.persisted_repeated_chunk_reads);          // 74
	out.bigint(operational.persisted_extents_read);                  // 75
	out.bigint(operational.persisted_chunks_read);                   // 76
	out.bigint(operational.persisted_bytes_read);                    // 77
	out.bigint(operational.persisted_bulk_read_calls);               // 78
	out.bigint(operational.persisted_planned_request_groups);        // 79
	out.bigint(operational.persisted_executed_request_groups);       // 80
	out.bigint(operational.persisted_range_scan_calls);              // 81
	out.bigint(operational.persisted_skip_budget_rows);              // 82
	out.bigint(operational.persisted_hash_build_calls);              // 83
	out.bigint(operational.persisted_assembly_ns);                   // 84
	out.bigint(operational.pair_sort_calls);                         // 85
	out.bigint(operational.host_pair_materialization_calls);         // 86
	out.bigint(operational.host_biological_hash_filter_calls);       // 87
	out.bigint(operational.host_query_hash_readbacks);               // 88
	out.bigint(operational.host_query_symbol_packing_calls);         // 89
	out.bigint(operational.host_target_symbol_packing_calls);        // 90
	out.bigint(operational.host_alignment_job_classification_calls); // 91
	out.bigint(operational.host_anchor_concatenation_calls);         // 92
	out.bigint(operational.host_cigar_construction_calls);           // 93
	out.bigint(operational.full_segment_hydrations);                 // 94
	out.bigint(operational.nthash_vector_batches);                   // 95
	out.bigint(operational.nthash_scalar_backend_branches);          // 96
	out.bigint(operational.nthash_scalar_tail_symbols);              // 97
	out.bigint(operational.nthash_masked_tail_lanes);                // 98
	out.bigint(operational.simd_chain_vector_batches);               // 99
	out.bigint(operational.simd_chain_backtrack_vector_batches);     // 100
	out.bigint(operational.simd_chain_scalar_backend_branches);      // 101
	out.bigint(operational.simd_chain_backtrack_scalar_branches);    // 102
	out.bigint(operational.cpu_seed_positions_rejected_outside_collection); // 103
	out.bigint(operational.gpu_partition_select_dispatches);         // 104
	out.bigint(operational.gpu_partition_anchor_merge_dispatches);   // 105
	out.bigint(operational.gpu_index_build_candidates);              // 106
	out.bigint(operational.gpu_index_build_canonical_emitted);       // 107
	out.bigint(operational.gpu_index_build_overlap_duplicates_rejected); // 108
	out.bigint(operational.gpu_index_build_noncanonical_extras_rejected); // 109
	out.bigint(operational.gpu_index_build_surviving_extras);        // 110
	out.bigint(operational.gpu_persisted_resident_uploads);          // 111
	out.bigint(operational.gpu_persisted_resident_cache_hits);       // 112
	out.bigint(operational.gpu_overlap_anchor_duplicates);           // 113
	out.bigint(operational.gpu_seed_dispatches);                     // 114
	out.bigint(operational.gpu_chain_dispatches);                    // 115
	out.bigint(operational.gpu_chain_backtrack_dispatches);          // 116
	out.bigint(operational.gpu_extend_dispatches);                   // 117
	out.bigint(operational.gpu_target_window_request_dispatches);    // 118
	out.bigint(operational.gpu_target_window_logical_units_requested); // 119
	out.bigint(operational.host_target_window_requests_consumed);    // 120
	out.bigint(operational.host_target_window_physical_units_read);  // 121
	out.bigint(operational.host_target_window_remap_units_filled);   // 122
	out.bigint(operational.host_target_window_packed_bytes_read);    // 123
	out.bigint(operational.gpu_runtime_init_ns);                     // 124
	out.bigint(operational.gpu_pipeline_cache_loads);                // 125
	out.bigint(operational.gpu_pipeline_cache_seed_bytes);           // 126
	out.bigint(operational.gpu_pipeline_cache_load_ns);              // 127
	out.bigint(operational.gpu_pipeline_creation_ns);                // 128
	out.bigint(operational.gpu_submit_cpu_ns);                       // 129
	out.bigint(operational.gpu_wait_cpu_ns);                         // 130
	out.bigint(operational.gpu_ordinal_readback_cpu_ns);             // 131
	out.bigint(operational.gpu_result_readback_cpu_ns);              // 132
	out.bigint(operational.gpu_device_critical_path_ns);             // 133
	out.bigint(operational.gpu_timestamp_query_failures);            // 134
	out.bigint(operational.gpu_queue_submits);                       // 135
	out.bigint(operational.gpu_host_waits);                          // 136
	out.bigint(operational.gpu_buffer_creates);                      // 137
	out.bigint(operational.gpu_memory_allocations);                  // 138
	out.bigint(operational.gpu_pipeline_creates);                    // 139
	out.bigint(operational.backend_declines);                        // 140
	out.bigint(operational.backend_fallbacks);                       // 141
	out.boolean(operational.stats_saturated);                        // 142
	out.bigint(operational.persisted_source_parent_rows_read);       // 143
	out.bigint(operational.persisted_source_catalog_rows_read);      // 144
	out.bigint(operational.persisted_source_catalog_index_scans);    // 145
	out.bigint(operational.persisted_source_fragment_rows_read);     // 146
	out.bigint(operational.persisted_source_decoded_bp);             // 147
	out.bigint(operational.persisted_legacy_globalize_calls);        // 148
	out.bigint(operational.persisted_legacy_rebase_calls);           // 149
	out.jsonb(cold_index_preparation_json(operational));             // 150
	if constexpr (ColumnCount == alignment_stats_v2_column_count) {
		out.jsonb(simd_execution_evidence_json(stats));                // 151
	}
	out.finish();
	return out.row;
}

template<class T>
struct alignment_optional_traits {
	static constexpr bool is_optional = false;
};

template<class T>
struct alignment_optional_traits<std::optional<T>> {
	static constexpr bool is_optional = true;
	using value_type = T;
};

template<class T, std::size_t ColumnCount>
[[nodiscard]] T alignment_stats_field(
		const bioseq::pg_utils::pg_datum_list<ColumnCount>& raw,
		const int attnum) {
	if constexpr (alignment_optional_traits<T>::is_optional) {
		if (raw.is_null(attnum)) {
			return std::nullopt;
		}
		using Value = typename alignment_optional_traits<T>::value_type;
		return T{alignment_stats_field<Value>(raw, attnum)};
	} else {
		if (raw.is_null(attnum)) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_NULL_VALUE_NOT_ALLOWED,
				{
					.message =
						"alignment result contains a null required field",
					.detail = fmt::format("column={}", attnum),
				});
		}
		const Datum datum = raw.at(attnum).raw();
		if constexpr (std::is_same_v<T, bool>) {
			return DatumGetBool(datum);
		} else if constexpr (std::is_same_v<T, std::int32_t>) {
			return DatumGetInt32(datum);
		} else if constexpr (std::is_same_v<T, std::int64_t>) {
			return DatumGetInt64(datum);
		} else if constexpr (std::is_same_v<T, Oid>) {
			return DatumGetObjectId(datum);
		} else if constexpr (std::is_same_v<T, std::string>) {
			const auto* value =
				reinterpret_cast<const text*>(DatumGetPointer(datum));
			if (value == nullptr) {
				bioseq::pg_utils::raise_sql_error(
					ERRCODE_DATA_EXCEPTION,
					{.message =
						"alignment result contains invalid text"});
			}
			return std::string{
				VARDATA_ANY(value),
				static_cast<std::size_t>(VARSIZE_ANY_EXHDR(value))};
		} else if constexpr (
				std::is_same_v<T, bioseq::pg_utils::sql_jsonb>) {
			char* encoded = bioseq::pg_utils::invoke_pg_as_cpp(
				[datum] {
					return DatumGetCString(
						DirectFunctionCall1(jsonb_out, datum));
				});
			std::string owned{encoded};
			bioseq::pg_utils::invoke_pg_as_cpp(
				[encoded] { pfree(encoded); });
			return bioseq::pg_utils::sql_jsonb{std::move(owned)};
		} else {
			static_assert(
				std::is_same_v<T, void>,
				"unsupported alignment SQL result field type");
		}
	}
}

#define BIOSEQ_ALIGNMENT_COMMON_FIELDS(raw) \
	.execution_id = alignment_stats_field<std::int64_t>(raw, 1), \
	.backend_pid = alignment_stats_field<std::int32_t>(raw, 2), \
	.database_oid = alignment_stats_field<Oid>(raw, 3), \
	.effective_cpu_workers = alignment_stats_field<std::int32_t>(raw, 4), \
	.gpu_device_uuid = alignment_stats_field<std::optional<std::string>>(raw, 5), \
	.gpu_driver = alignment_stats_field<std::optional<std::string>>(raw, 6), \
	.gpu_pipeline_cache_policy = alignment_stats_field<std::optional<std::string>>(raw, 7), \
	.gpu_pipeline_cache_seed_sha256 = alignment_stats_field<std::optional<std::string>>(raw, 8), \
	.gpu_pipeline_cache_seed_size = alignment_stats_field<std::optional<std::int64_t>>(raw, 9), \
	.gpu_pipeline_cache_manifest_checksum = alignment_stats_field<std::optional<std::string>>(raw, 10), \
	.gpu_pipeline_cache_hit = alignment_stats_field<std::optional<bool>>(raw, 11), \
	.scan_node_count = alignment_stats_field<std::int32_t>(raw, 12), \
	.nested_alignment_executor_count = alignment_stats_field<std::int32_t>(raw, 13), \
	.completed = alignment_stats_field<bool>(raw, 14), \
	.partial = alignment_stats_field<bool>(raw, 15), \
	.fully_drained = alignment_stats_field<bool>(raw, 16), \
	.requested_backend = alignment_stats_field<std::string>(raw, 17), \
	.actual_backend = alignment_stats_field<std::string>(raw, 18), \
	.requested_target_index_source = alignment_stats_field<std::string>(raw, 19), \
	.actual_target_index_source = alignment_stats_field<std::string>(raw, 20), \
	.persisted_generation_id = alignment_stats_field<std::int64_t>(raw, 21), \
	.persisted_source_identity = alignment_stats_field<std::string>(raw, 22), \
	.mixed_persisted_identity = alignment_stats_field<bool>(raw, 23), \
	.stage = alignment_stats_field<std::string>(raw, 24), \
	.eligible_jobs = alignment_stats_field<std::int64_t>(raw, 25), \
	.executed_host_io_jobs = alignment_stats_field<std::int64_t>(raw, 26), \
	.executed_scalar_cpu_jobs = alignment_stats_field<std::int64_t>(raw, 27), \
	.executed_simd_cpu_jobs = alignment_stats_field<std::int64_t>(raw, 28), \
	.executed_vulkan_gpu_jobs = alignment_stats_field<std::int64_t>(raw, 29), \
	.fallback_jobs = alignment_stats_field<std::int64_t>(raw, 30), \
	.stage_host_wall_ns = alignment_stats_field<std::int64_t>(raw, 31), \
	.stage_gpu_device_ns = alignment_stats_field<std::int64_t>(raw, 32), \
	.target_rows = alignment_stats_field<std::int64_t>(raw, 33), \
	.attempted_alignment_jobs = alignment_stats_field<std::int64_t>(raw, 34), \
	.fully_attested_alignment_jobs = alignment_stats_field<std::int64_t>(raw, 35), \
	.unattested_alignment_jobs = alignment_stats_field<std::int64_t>(raw, 36), \
	.first_unattested_target_row = alignment_stats_field<std::optional<std::int64_t>>(raw, 37), \
	.first_unattested_query_ordinal = alignment_stats_field<std::optional<std::int64_t>>(raw, 38), \
	.cpu_prepared_index_builds = alignment_stats_field<std::int64_t>(raw, 39), \
	.cpu_prepared_index_reuses = alignment_stats_field<std::int64_t>(raw, 40), \
	.cpu_prepared_unique_keys = alignment_stats_field<std::int64_t>(raw, 41), \
	.cpu_prepared_duplicate_build_attempts = alignment_stats_field<std::int64_t>(raw, 42), \
	.cpu_prepared_key_set_checksum = alignment_stats_field<std::optional<std::string>>(raw, 43), \
	.cpu_prepared_build_set_checksum = alignment_stats_field<std::optional<std::string>>(raw, 44), \
	.operational_identity_conflict = alignment_stats_field<bool>(raw, 45), \
	.persisted_index_rows = alignment_stats_field<std::int64_t>(raw, 46), \
	.missing_persisted_index_rows = alignment_stats_field<std::int64_t>(raw, 47), \
	.recomputed_target_index_rows = alignment_stats_field<std::int64_t>(raw, 48), \
	.persisted_generation_read_sessions = alignment_stats_field<std::int64_t>(raw, 49), \
	.persisted_relation_opens = alignment_stats_field<std::int64_t>(raw, 50), \
	.persisted_index_scans = alignment_stats_field<std::int64_t>(raw, 51), \
	.persisted_head_scans = alignment_stats_field<std::int64_t>(raw, 52), \
	.persisted_head_rows_read = alignment_stats_field<std::int64_t>(raw, 53), \
	.persisted_head_bytes_read = alignment_stats_field<std::int64_t>(raw, 54), \
	.persisted_manifest_directory_scans = alignment_stats_field<std::int64_t>(raw, 55), \
	.persisted_manifest_rows_read = alignment_stats_field<std::int64_t>(raw, 56), \
	.persisted_manifest_bytes_read = alignment_stats_field<std::int64_t>(raw, 57), \
	.persisted_directory_chunks_read = alignment_stats_field<std::int64_t>(raw, 58), \
	.persisted_directory_bytes_read = alignment_stats_field<std::int64_t>(raw, 59), \
	.persisted_metadata_repeated_reads = alignment_stats_field<std::int64_t>(raw, 60), \
	.persisted_metadata_heap_rows_read = alignment_stats_field<std::int64_t>(raw, 61), \
	.persisted_metadata_bytes_read = alignment_stats_field<std::int64_t>(raw, 62), \
	.persisted_payload_heap_rows_read = alignment_stats_field<std::int64_t>(raw, 63), \
	.persisted_payload_bytes_read = alignment_stats_field<std::int64_t>(raw, 64), \
	.persisted_heap_rows_read = alignment_stats_field<std::int64_t>(raw, 65), \
	.persisted_toast_values_read = alignment_stats_field<std::int64_t>(raw, 66), \
	.persisted_detoast_bytes = alignment_stats_field<std::int64_t>(raw, 67), \
	.persisted_explicit_copy_bytes = alignment_stats_field<std::int64_t>(raw, 68), \
	.persisted_wanted_subkeys = alignment_stats_field<std::int64_t>(raw, 69), \
	.persisted_read_subkeys = alignment_stats_field<std::int64_t>(raw, 70), \
	.persisted_wanted_extents = alignment_stats_field<std::int64_t>(raw, 71), \
	.persisted_wanted_chunks = alignment_stats_field<std::int64_t>(raw, 72), \
	.persisted_skipped_rows = alignment_stats_field<std::int64_t>(raw, 73), \
	.persisted_repeated_chunk_reads = alignment_stats_field<std::int64_t>(raw, 74), \
	.persisted_extents_read = alignment_stats_field<std::int64_t>(raw, 75), \
	.persisted_chunks_read = alignment_stats_field<std::int64_t>(raw, 76), \
	.persisted_bytes_read = alignment_stats_field<std::int64_t>(raw, 77), \
	.persisted_bulk_read_calls = alignment_stats_field<std::int64_t>(raw, 78), \
	.persisted_planned_request_groups = alignment_stats_field<std::int64_t>(raw, 79), \
	.persisted_executed_request_groups = alignment_stats_field<std::int64_t>(raw, 80), \
	.persisted_range_scan_calls = alignment_stats_field<std::int64_t>(raw, 81), \
	.persisted_skip_budget_rows = alignment_stats_field<std::int64_t>(raw, 82), \
	.persisted_hash_build_calls = alignment_stats_field<std::int64_t>(raw, 83), \
	.persisted_assembly_ns = alignment_stats_field<std::int64_t>(raw, 84), \
	.pair_sort_calls = alignment_stats_field<std::int64_t>(raw, 85), \
	.host_pair_materialization_calls = alignment_stats_field<std::int64_t>(raw, 86), \
	.host_biological_hash_filter_calls = alignment_stats_field<std::int64_t>(raw, 87), \
	.host_query_hash_readbacks = alignment_stats_field<std::int64_t>(raw, 88), \
	.host_query_symbol_packing_calls = alignment_stats_field<std::int64_t>(raw, 89), \
	.host_target_symbol_packing_calls = alignment_stats_field<std::int64_t>(raw, 90), \
	.host_alignment_job_classification_calls = alignment_stats_field<std::int64_t>(raw, 91), \
	.host_anchor_concatenation_calls = alignment_stats_field<std::int64_t>(raw, 92), \
	.host_cigar_construction_calls = alignment_stats_field<std::int64_t>(raw, 93), \
	.full_segment_hydrations = alignment_stats_field<std::int64_t>(raw, 94), \
	.nthash_vector_batches = alignment_stats_field<std::int64_t>(raw, 95), \
	.nthash_scalar_backend_branches = alignment_stats_field<std::int64_t>(raw, 96), \
	.nthash_scalar_tail_symbols = alignment_stats_field<std::int64_t>(raw, 97), \
	.nthash_masked_tail_lanes = alignment_stats_field<std::int64_t>(raw, 98), \
	.simd_chain_vector_batches = alignment_stats_field<std::int64_t>(raw, 99), \
	.simd_chain_backtrack_vector_batches = alignment_stats_field<std::int64_t>(raw, 100), \
	.simd_chain_scalar_backend_branches = alignment_stats_field<std::int64_t>(raw, 101), \
	.simd_chain_backtrack_scalar_branches = alignment_stats_field<std::int64_t>(raw, 102), \
	.cpu_seed_positions_rejected_outside_collection = alignment_stats_field<std::int64_t>(raw, 103), \
	.gpu_partition_select_dispatches = alignment_stats_field<std::int64_t>(raw, 104), \
	.gpu_partition_anchor_merge_dispatches = alignment_stats_field<std::int64_t>(raw, 105), \
	.gpu_index_build_candidates = alignment_stats_field<std::int64_t>(raw, 106), \
	.gpu_index_build_canonical_emitted = alignment_stats_field<std::int64_t>(raw, 107), \
	.gpu_index_build_overlap_duplicates_rejected = alignment_stats_field<std::int64_t>(raw, 108), \
	.gpu_index_build_noncanonical_extras_rejected = alignment_stats_field<std::int64_t>(raw, 109), \
	.gpu_index_build_surviving_extras = alignment_stats_field<std::int64_t>(raw, 110), \
	.gpu_persisted_resident_uploads = alignment_stats_field<std::int64_t>(raw, 111), \
	.gpu_persisted_resident_cache_hits = alignment_stats_field<std::int64_t>(raw, 112), \
	.gpu_overlap_anchor_duplicates = alignment_stats_field<std::int64_t>(raw, 113), \
	.gpu_seed_dispatches = alignment_stats_field<std::int64_t>(raw, 114), \
	.gpu_chain_dispatches = alignment_stats_field<std::int64_t>(raw, 115), \
	.gpu_chain_backtrack_dispatches = alignment_stats_field<std::int64_t>(raw, 116), \
	.gpu_extend_dispatches = alignment_stats_field<std::int64_t>(raw, 117), \
	.gpu_target_window_request_dispatches = alignment_stats_field<std::int64_t>(raw, 118), \
	.gpu_target_window_logical_units_requested = alignment_stats_field<std::int64_t>(raw, 119), \
	.host_target_window_requests_consumed = alignment_stats_field<std::int64_t>(raw, 120), \
	.host_target_window_physical_units_read = alignment_stats_field<std::int64_t>(raw, 121), \
	.host_target_window_remap_units_filled = alignment_stats_field<std::int64_t>(raw, 122), \
	.host_target_window_packed_bytes_read = alignment_stats_field<std::int64_t>(raw, 123), \
	.gpu_runtime_init_ns = alignment_stats_field<std::int64_t>(raw, 124), \
	.gpu_pipeline_cache_loads = alignment_stats_field<std::int64_t>(raw, 125), \
	.gpu_pipeline_cache_seed_bytes = alignment_stats_field<std::int64_t>(raw, 126), \
	.gpu_pipeline_cache_load_ns = alignment_stats_field<std::int64_t>(raw, 127), \
	.gpu_pipeline_creation_ns = alignment_stats_field<std::int64_t>(raw, 128), \
	.gpu_submit_cpu_ns = alignment_stats_field<std::int64_t>(raw, 129), \
	.gpu_wait_cpu_ns = alignment_stats_field<std::int64_t>(raw, 130), \
	.gpu_ordinal_readback_cpu_ns = alignment_stats_field<std::int64_t>(raw, 131), \
	.gpu_result_readback_cpu_ns = alignment_stats_field<std::int64_t>(raw, 132), \
	.gpu_device_critical_path_ns = alignment_stats_field<std::int64_t>(raw, 133), \
	.gpu_timestamp_query_failures = alignment_stats_field<std::int64_t>(raw, 134), \
	.gpu_queue_submits = alignment_stats_field<std::int64_t>(raw, 135), \
	.gpu_host_waits = alignment_stats_field<std::int64_t>(raw, 136), \
	.gpu_buffer_creates = alignment_stats_field<std::int64_t>(raw, 137), \
	.gpu_memory_allocations = alignment_stats_field<std::int64_t>(raw, 138), \
	.gpu_pipeline_creates = alignment_stats_field<std::int64_t>(raw, 139), \
	.backend_declines = alignment_stats_field<std::int64_t>(raw, 140), \
	.backend_fallbacks = alignment_stats_field<std::int64_t>(raw, 141), \
	.stats_saturated = alignment_stats_field<bool>(raw, 142), \
	.persisted_source_parent_rows_read = alignment_stats_field<std::int64_t>(raw, 143), \
	.persisted_source_catalog_rows_read = alignment_stats_field<std::int64_t>(raw, 144), \
	.persisted_source_catalog_index_scans = alignment_stats_field<std::int64_t>(raw, 145), \
	.persisted_source_fragment_rows_read = alignment_stats_field<std::int64_t>(raw, 146), \
	.persisted_source_decoded_bp = alignment_stats_field<std::int64_t>(raw, 147), \
	.persisted_legacy_globalize_calls = alignment_stats_field<std::int64_t>(raw, 148), \
	.persisted_legacy_rebase_calls = alignment_stats_field<std::int64_t>(raw, 149), \
	.cold_index_preparation = alignment_stats_field<bioseq::pg_utils::sql_jsonb>(raw, 150),

template<int ColumnCount>
[[nodiscard]] auto make_owned_alignment_stats_row(
		const alignment_exec::AlignmentExecutionSnapshot& snapshot,
		const bioseq::pipeline_stats::PipelineStage stage) {
	auto raw = make_alignment_stats_row<ColumnCount>(snapshot, stage);
	if constexpr (ColumnCount == alignment_stats_v1_column_count) {
		return bioseq::pg::sql_rows::alignment_execution_stats_row{
			BIOSEQ_ALIGNMENT_COMMON_FIELDS(raw)
		};
	} else {
		static_assert(ColumnCount == alignment_stats_v2_column_count);
		return bioseq::pg::sql_rows::alignment_execution_stats_v2_row{
			BIOSEQ_ALIGNMENT_COMMON_FIELDS(raw)
			.simd_execution_evidence =
				alignment_stats_field<bioseq::pg_utils::sql_jsonb>(
					raw, 151),
		};
	}
}

#undef BIOSEQ_ALIGNMENT_COMMON_FIELDS
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
	install_alignment_execution_hooks();
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

export namespace bioseq::pg::sql_routines {

[[bioseq_sql::function({
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "mmsearch_support",
})]]
/**
 * @brief Requests similarity search with default aligner options.
 * @ingroup bioseq_sql_public_api
 */
bool mmsearch(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        Datum el,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        Datum query) {
    static_cast<void>(el);
    static_cast<void>(query);
    bioseq::pg_utils::raise_sql_error(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        {
            .message =
                "mmsearch qualifier has no Bioseq executor owner",
            .detail =
                "Executing the SQL rendezvous function as a Boolean "
                "predicate is forbidden.",
            .hint =
                "Use exactly one top-level mmsearch predicate with "
                "supported operands, or split the query into separate "
                "levels.",
        });
}

[[bioseq_sql::function({
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "mmsearch_support",
})]]
/**
 * @brief Similarity-search predicate with explicit per-call aligner options.
 * @ingroup bioseq_sql_public_api
 */
std::optional<bool> mmsearch(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        std::optional<Datum> el,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        std::optional<Datum> query,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::type_of<
                bioseq::table::composite_rows::bioseq_aligner_options>,
        })]]
        std::optional<Datum> opts) {
    static_cast<void>(opts);
    if (!el.has_value() || !query.has_value()) {
        return std::nullopt;
    }
    bioseq::pg_utils::raise_sql_error(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        {
            .message =
                "mmsearch qualifier has no Bioseq executor owner",
            .detail =
                "Executing the SQL rendezvous function as a Boolean "
                "predicate is forbidden.",
            .hint =
                "Use exactly one top-level mmsearch predicate with "
                "supported operands, or split the query into separate "
                "levels.",
        });
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int32_t score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>(
        "[bioseq] pg: score() UNEXPECTED direct call -- custom scan did "
        "not rewrite this call site");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int32_t dp_score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: dp_score() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int64_t query_len(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: query_len() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief Start offset of the current mmsearch alignment on the query sequence.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t query_start(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: query_start() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief End offset of the current mmsearch alignment on the query sequence.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t query_end(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: query_end() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int64_t target_len(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: target_len() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief Start offset of the current mmsearch alignment on the stored target sequence.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t target_start(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: target_start() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief End offset of the current mmsearch alignment on the stored target sequence.
 * @ingroup bioseq_sql_public_api
 */
std::int64_t target_end(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: target_end() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int64_t num_matches(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: num_matches() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int64_t block_length(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: block_length() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .cost = 100000.0,
    .has_cost = true,
    .support = "bioseq_placeholder_support",
})]]
std::int64_t edit(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq::pg_utils::raise_sql_error(
        ERRCODE_FEATURE_NOT_SUPPORTED,
        {
            .message =
                "bioseq.edit requires a bioseq alignment custom scan",
            .hint =
                "Use bioseq.edit(...) in a query whose WHERE clause "
                "contains mmsearch(...).",
        });
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int32_t mapq(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: mapq() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int32_t chain_score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: chain_score() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int32_t dp_max_score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: dp_max_score() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int32_t seed_count(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: seed_count() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
std::int32_t secondary_chain_score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>(
        "[bioseq] pg: secondary_chain_score() UNEXPECTED direct call");
    return -1;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
bool is_reverse(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: is_reverse() UNEXPECTED direct call");
    return false;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
bool is_primary(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: is_primary() UNEXPECTED direct call");
    return false;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
bool has_cigar(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: has_cigar() UNEXPECTED direct call");
    return false;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
float divergence(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: divergence() UNEXPECTED direct call");
    return -1.0F;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief CIGAR string describing the current mmsearch alignment.
 * @ingroup bioseq_sql_public_api
 */
std::string cigar(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el) {
    static_cast<void>(el);
    bioseq_log<DBG>("[bioseq] pg: cigar() UNEXPECTED direct call");
    return "!UNEXPECTED!";
}

} // namespace bioseq::pg::sql_routines

export namespace bioseq::pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
std::string gpu_warmup_shader_cache() {
    return bioseq::parallelism::pg::
        warmup_backend_local_shader_cache_report();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
bioseq::pg_utils::named_out<
    bioseq::pg::sql_rows::vulkan_runtime_status_row>
vulkan_runtime_status() {
    auto& runtime =
        bioseq::parallelism::pg::ensure_pg_vulkan_runtime_ready();
    const auto& features = runtime.features();
    return bioseq::pg_utils::named_out{
        bioseq::pg::sql_rows::vulkan_runtime_status_row{
            .available = features.available,
            .device_name = std::string{features.device_name.data()},
            .max_workgroup_count_x = static_cast<std::int32_t>(
                std::min<std::uint32_t>(
                    features.max_workgroup_count_x,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<std::int32_t>::max()))),
            .scalar_block_layout = features.scalar_block_layout,
            .initialization_error = runtime.initialization_error(),
            .device_local_bytes = static_cast<std::int64_t>(
                std::min<std::uint64_t>(
                    features.device_local_bytes,
                    static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max()))),
            .software_device = features.software_device,
        }};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_symbol_count_support",
})]]
/**
 * @brief Number of occurrences of a given symbol within a bioseq.seq value.
 *
 * Stored locator values are resolved by the Bioseq scan path, which can
 * access their owning relation and persisted symbol-count metadata.
 *
 * @ingroup bioseq_sql_public_api
 */
std::int64_t symbol_count(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        Datum el,
        std::string sym) {
	/* Detoast the bioseq.seq varlena and reinterpret as bioseq::pg_types::seq. */
	bioseq::pg_types::seq* hdr = bioseq::pg_utils::invoke_pg_as_cpp(
		[el] {
			return reinterpret_cast<bioseq::pg_types::seq*>(
				PG_DETOAST_DATUM(el));
		});
	if (sym.size() != 1) {
        bioseq::pg_utils::pfree_if_distinct_from(
            bioseq::pg_utils::pg_datum::from_raw(el), hdr);
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.symbol_count: symbol must be exactly one byte",
            });
	}
	const char sym_char = sym.front();

	if (bioseq::storage::is_locator_stub(hdr)) {
        bioseq::pg_utils::pfree_if_distinct_from(
            bioseq::pg_utils::pg_datum::from_raw(el), hdr);
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_FEATURE_NOT_SUPPORTED,
            {
                .message =
                    "bioseq.symbol_count cannot tally a stored sequence "
                    "outside a scan",
                .hint =
                    "Query the column through the Bioseq scan or materialize "
                    "the symbol-count index with bioseq.update_index.",
            });
	}

	int rank = bioseq::storage::indexing::letter_to_rank(hdr->alphabet_code, sym_char);
	if (rank < 0) {
        bioseq::pg_utils::pfree_if_distinct_from(
            bioseq::pg_utils::pg_datum::from_raw(el), hdr);
        bioseq::pg_utils::raise_sql_error(
            ERRCODE_INVALID_PARAMETER_VALUE,
            {
                .message =
                    "bioseq.symbol_count: symbol is not part of the "
                    "sequence alphabet",
            });
	}

	int64_t computed_result = 0;
	bioseq::pg_types::seq::dispatch_instance(hdr->alphabet_code, hdr->storage_unit_byte_width,
		[hdr, rank, &computed_result]<class S>() {
			compact_sequence_range<S> range{*hdr};
			auto lazy = bioseq::storage::make_lazy_in_memory(range);
			const auto lazy_view =
				bioseq::storage::as_lazy_sequence_view(lazy);
			auto counts = bioseq::indexing::tally_symbol_counts(lazy_view);  // std::array<int64,N=2^bits_per_char>
			if (static_cast<size_t>(rank) < counts.size()) {
				computed_result = static_cast<int64_t>(counts[rank]);
			}
		});

    bioseq::pg_utils::pfree_if_distinct_from(
        bioseq::pg_utils::pg_datum::from_raw(el), hdr);
	return computed_result;
}

/*
 * bioseq_symbol_count_support is the planner-support function attached to
 * bioseq.symbol_count(). We return a high per-call cost so any query with a
 * cheaper alternative (the bioseq columnar custom scan) is preferred by the
 * optimiser. The cost is lower than mmsearch_support's (100000) because the
 * scalar symbol_count implementation is O(n) where n is the sequence
 * length, while alignment is substantially more expensive. Custom-scan
 * ownership is selected structurally; these support costs describe only the
 * ordinary scalar alternative.
 */
/*
 * Hand-rolled walker that returns the first Var in a node tree without
 * raising on Aggref/WindowFunc/SubLink/etc. Used by the simplify helper
 * below. We deliberately avoid Postgres' pull_var_clause() because that
 * call is documented to raise from late planning paths and we are
 * invoked from eval_const_expressions() which is reached very early in
 * subquery_planner.
 */
} // namespace bioseq::pg::sql_routines

namespace {

struct FirstVarFinder {
	Var* result = nullptr;
};
struct first_var_visitor {
	[[nodiscard]] pg_walk_step
	operator()(pg_expression_walk_cursor, pg_node_view node, FirstVarFinder& finder) const noexcept {
		if (finder.result != nullptr) {
			return pg_walk_step::stop;  /* abort the walk */
		}
		if (Var* var = node.try_as<Var>()) {
			finder.result = var;
			return pg_walk_step::stop;
		}
		return pg_walk_step::descend;
	}
};

/*
 * Shared simplify-time PHV wrapping. Used by both bioseq_placeholder_support
 * (attached to the ANYELEMENT projections) and bioseq_symbol_count_support
 * (which also folds bioseq.symbol_count()). Returning nullptr here means
 * "no rewrite" — the caller surfaces that to PostgreSQL as "leave the
 * FuncExpr alone."
 */
static Node* try_wrap_in_placeholder(SupportRequestSimplify* simplify) {
	if (simplify == nullptr || simplify->root == nullptr || simplify->fcall == nullptr) {
		return nullptr;
	}
	PlannerInfo* root = simplify->root;
	const bool has_joins = root->hasJoinRTEs;
	const bool has_aggs = root->parse != nullptr && root->parse->hasAggs;
	if (!has_joins && !has_aggs) {
		return nullptr;
	}
	FirstVarFinder finder;
	first_var_visitor visitor;
	pg_walk_expression_including_root(pg_as_node(simplify->fcall), finder, visitor);
	if (finder.result == nullptr) {
		bioseq_log<DBG>("[bioseq] pg: try_wrap_in_placeholder() no Var found in fcall — skipping");
		return nullptr;
	}
	Var* arg_var = finder.result;
	Relids phrels = bms_make_singleton(static_cast<int>(arg_var->varno));
	Expr* expr_copy = reinterpret_cast<Expr*>(copyObjectImpl(simplify->fcall));
	PlaceHolderVar* phv = make_placeholder_expr(root, expr_copy, phrels);
	phv->phlevelsup    = arg_var->varlevelsup;
	phv->phnullingrels = arg_var->varnullingrels;
	bioseq_log<DBG>("[bioseq] pg: try_wrap_in_placeholder() wrapped funcid={} phid={} phrels={}",
			simplify->fcall->funcid, phv->phid,
			static_cast<int>(arg_var->varno));
	return reinterpret_cast<Node*>(phv);
}

} // namespace

export namespace bioseq::pg::sql_routines {

[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]]
Datum bioseq_symbol_count_support(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::internal,
        })]]
        Datum request_datum) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_symbol_count_support() entered");
	Node* request = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
	if (SupportRequestCost* cost_request = pg_node_try_as<SupportRequestCost>(request)) {
		bioseq_log<DBG>("[bioseq] pg: bioseq_symbol_count_support() received SupportRequestCost");
		cost_request->startup = 50000;
		cost_request->per_tuple = 0;
		return PointerGetDatum(cost_request);
	}
	if (SupportRequestSimplify* simplify = pg_node_try_as<SupportRequestSimplify>(request)) {
		/*
		 * symbol_count's projection placeholder is rewritten by the
		 * VarInjectorMutator just like the bioseq.cigar/score/...
		 * projections, so the same join-pushdown bug bites it. The
		 * simplify branch here mirrors bioseq_placeholder_support so a
		 * LEFT JOIN with WHERE mmsearch + bioseq.symbol_count(...) on
		 * the projection no longer crashes.
		 */
		Node* wrapped = try_wrap_in_placeholder(simplify);
		if (wrapped != nullptr) {
			return PointerGetDatum(wrapped);
		}
	}
	return PointerGetDatum(nullptr);
}

/*
 * mmsearch_support is the planner-support function attached to mmsearch().
 * PostgreSQL calls it with SupportRequest nodes so we can adjust cost
 * estimates before planning proceeds.
 */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]]
Datum mmsearch_support(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::internal,
        })]]
        Datum request_datum) {
	bioseq_log<DBG>("[bioseq] pg: mmsearch_support() entered");
	Node* response = nullptr;
	Node* request = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
	if (SupportRequestCost* cost_request = pg_node_try_as<SupportRequestCost>(request))
	{
		bioseq_log<DBG>("[bioseq] pg: mmsearch_support() received SupportRequestCost");
		PlannerInfo* planner_info = cost_request->root;
		if (planner_info == nullptr) {
			bioseq_log<DBG>("[bioseq] pg: mmsearch_support() planner_info is null");
		} else {
			if (planner_info->non_recursive_path == nullptr) {
				bioseq_log<DBG>("[bioseq] pg: mmsearch_support() non_recursive_path is null");
			} else {
				bioseq_log<DBG>("[bioseq] pg: mmsearch_support() non_recursive_path pathtype={}", static_cast<int>(planner_info->non_recursive_path->pathtype));
			}
		}
		cost_request->startup = 100000;
		cost_request->per_tuple = 0;
		return PointerGetDatum(cost_request);
	}
	return PointerGetDatum(response);
}

/*
 * bioseq_placeholder_support is the planner-support function attached to
 * every bioseq.* projection function (bioseq.cigar, bioseq.score,
 * bioseq.target_start, ...). Mirrors paradedb's `placeholder_support`
 * (see paradedb-experiments/pg_search/.../projections/mod.rs) — when a
 * query contains JOINs or aggregates, we wrap the FuncExpr in a
 * PlaceHolderVar so PostgreSQL evaluates it once at the originating scan
 * level and propagates the value upward through joins/aggregates without
 * re-evaluating it. Without this, the planner pushes copies of the
 * FuncExpr into every join child's targetlist, which crashes the LEFT
 * JOIN + bioseq.cigar + WHERE mmsearch path because the inner-side scan
 * has no MatchResult to back the rewritten placeholder Const.
 *
 * Returns NULL (no rewrite) when the query has no joins or aggregates;
 * the FuncExpr stays bare and the scan-time VarInjectorMutator rewrites
 * it to a placeholder Const directly. Returns NULL likewise when the
 * function call shape cannot be wrapped (zero or multiple Vars,
 * varlevelsup mismatch, …) — the executor still falls back to the stub
 * which logs and returns a sentinel.
 */
[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]]
Datum bioseq_placeholder_support(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::internal,
        })]]
        Datum request_datum) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_placeholder_support() entered");
	Node* request = reinterpret_cast<Node*>(
        DatumGetPointer(request_datum));
	SupportRequestSimplify* simplify = pg_node_try_as<SupportRequestSimplify>(request);
	if (simplify == nullptr) {
		return PointerGetDatum(nullptr);
	}
	Node* wrapped = try_wrap_in_placeholder(simplify);
	return PointerGetDatum(wrapped);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
/**
 * @brief Lists registered indexes, policies, sizes, and ages for Bioseq sequence columns.
 *
 * The result combines configured policy, persisted shard size, and last
 * update time without requiring an index generation to exist.
 */
bioseq::pg_utils::returns_table<
    bioseq::pg::sql_rows::list_metadata_row>
list_metadata() {
	namespace bsi = bioseq::storage::indexing;
	namespace bidx = bioseq::indexing;
	std::vector<bioseq::pg::sql_rows::list_metadata_row> result;

	const auto kinds = bsi::pg_supported_indexes::list_types();
	const auto allows = bsi::pg_supported_indexes::allows_on_insert_flags();

    bioseq::pg_utils::scoped_spi_session spi;
	if (spi.connect() != SPI_OK_CONNECT) {
        throw std::runtime_error{
            "bioseq.list_metadata could not connect to SPI"};
    }
	const auto execute_spi = [](const char* sql, const bool read_only) {
		return bioseq::pg_utils::invoke_pg_as_cpp(
			[sql, read_only] {
				return SPI_execute(sql, read_only, 0);
			});
	};
	const auto quote_literal_owned = [](const std::string_view value) {
		/*
		 * Keep every C++ owner outside the PG_TRY bridge.  The leaf callback
		 * captures only a stable raw pointer and returns PostgreSQL's owned
		 * buffer; copying and freeing happen after the bridge has returned.
		 */
		const std::string input{value};
		char* const quoted = bioseq::pg_utils::invoke_pg_as_cpp(
			[input_data = input.c_str()] {
				return quote_literal_cstr(input_data);
			});
		try {
			std::string result{quoted};
			bioseq::pg_utils::invoke_pg_as_cpp(
				[quoted] { pfree(quoted); });
			return result;
		} catch (...) {
			const std::exception_ptr original =
				std::current_exception();
			try {
				bioseq::pg_utils::invoke_pg_as_cpp(
					[quoted] { pfree(quoted); });
			} catch (...) {
				std::terminate();
			}
			std::rethrow_exception(original);
		}
	};

	/* 1. every user bioseq-AM relation (exclude internal namespaces / names). */
	std::vector<Oid> rels;
	if (execute_spi(
			"SELECT c.oid FROM pg_class c JOIN pg_am am ON c.relam=am.oid "
			"JOIN pg_namespace n ON c.relnamespace=n.oid "
			"WHERE am.amname='bioseq' AND c.relkind='r' "
			"AND n.nspname NOT IN ('bioseq_internal','pg_catalog','information_schema') "
			"AND n.nspname NOT LIKE 'pg\\_temp%'", true) == SPI_OK_SELECT) {
		for (std::uint64_t r = 0; r < SPI_processed; ++r) {
			auto rel_cell = bioseq::pg_utils::pg_spi_cell::get(
				SPI_tuptable->vals[r], SPI_tuptable->tupdesc, 1);
			if (auto rel_oid = rel_cell.optional_oid()) rels.push_back(*rel_oid);
		}
	}

	/* 2. build the VALUES source: (rel, attno, attname, idx_name, idx_type, def_uoi, prefix). */
	std::string values;
	for (Oid rel : rels) {
		const auto rid = bsi::index_relation_identity(rel);
		std::string colsql = fmt::format(
			"SELECT a.attnum, a.attname FROM pg_attribute a JOIN pg_type t ON a.atttypid=t.oid "
			"JOIN pg_namespace tn ON t.typnamespace=tn.oid WHERE a.attrelid={}::oid AND t.typname='seq' "
			"AND tn.nspname IN ('bioseq','bioseq_internal') AND a.attnum>0 AND NOT a.attisdropped ORDER BY a.attnum",
			static_cast<unsigned>(rel));
		if (execute_spi(colsql.c_str(), true) != SPI_OK_SELECT) continue;
		std::vector<std::pair<std::int16_t, std::string>> cols;
		for (std::uint64_t c = 0; c < SPI_processed; ++c) {
			auto att_cell = bioseq::pg_utils::pg_spi_cell::get(
				SPI_tuptable->vals[c], SPI_tuptable->tupdesc, 1);
			const std::int16_t a = att_cell.optional<std::int16_t>().value_or(0);
			char* nm = bioseq::pg_utils::invoke_pg_as_cpp(
				[c] {
					return SPI_getvalue(
						SPI_tuptable->vals[c],
						SPI_tuptable->tupdesc, 2);
				});
			cols.emplace_back(
				a, nm == nullptr ? std::string{} : std::string{nm});
			if (nm != nullptr) {
				bioseq::pg_utils::invoke_pg_as_cpp(
					[nm] { pfree(nm); });
			}
		}
		for (auto& [attno, attname] : cols) {
			for (std::size_t i = 0; i < kinds.size(); ++i) {
				const std::string idx_name{kinds[i].first};
				const std::string idx_type{bidx::sequence_index_type_name(kinds[i].second)};
				const bool def_uoi = bidx::default_update_on_insert_contains(kinds[i].first) && allows[i];
				const std::string prefix = fmt::format("seqidx_{}_{:016x}_",
					bsi::short_index_for(kinds[i].first),
					bsi::identity_hash64(rid.db_oid, rid.tablespace_oid, rid.relfilenode, attno, kinds[i].first));
				if (!values.empty()) values += ",";
				values += fmt::format("({}::oid,{}::int2,{},{},{},{},{})",
					static_cast<unsigned>(rel), static_cast<int>(attno),
					quote_literal_owned(attname),
					quote_literal_owned(idx_name),
					quote_literal_owned(idx_type),
					def_uoi ? "true" : "false",
					quote_literal_owned(prefix));
			}
		}
	}

	if (!values.empty()) {
		std::string query = fmt::format(
			"WITH src(rel,attno,attname,idx_name,idx_type,def_uoi,prefix) AS (VALUES {}) "
			"SELECT src.rel::regclass, src.attno::int2, src.attname, src.idx_name, src.idx_type, "
			"  CASE WHEN p.update_on_insert IS NOT NULL "
			"       THEN (CASE WHEN p.update_on_insert THEN 'UPDATE_ON_INSERT' ELSE 'UPDATE_MANUALLY' END) "
			"       WHEN src.def_uoi THEN 'UPDATE_ON_INSERT' ELSE 'UPDATE_MANUALLY' END, "
			"  pg_size_pretty(COALESCE((SELECT SUM(pg_total_relation_size(ic.oid)) FROM pg_class ic "
			"     JOIN pg_namespace inx ON ic.relnamespace=inx.oid "
			"     WHERE inx.nspname='bioseq_internal' AND ic.relname LIKE src.prefix||'%'),0)::bigint), "
			"  (SELECT now()-m.last_updated FROM bioseq_internal.metadata_age_log m "
			"     WHERE m.user_relid=src.rel AND m.column_attno=src.attno AND m.index_name=src.idx_name LIMIT 1) "
			"FROM src LEFT JOIN bioseq_internal.index_policy p "
			"  ON p.user_relid=src.rel AND p.column_attno=src.attno AND p.index_name=src.idx_name "
			"ORDER BY src.rel, src.attno, src.idx_name", values);
		if (execute_spi(query.c_str(), true) == SPI_OK_SELECT) {
			for (std::uint64_t r = 0; r < SPI_processed; ++r) {
                HeapTuple tuple = SPI_tuptable->vals[r];
                TupleDesc descriptor = SPI_tuptable->tupdesc;
                const auto required_text =
                    [tuple, descriptor](int column) {
                        char* value =
							bioseq::pg_utils::invoke_pg_as_cpp(
								[tuple, descriptor, column] {
									return SPI_getvalue(
										tuple, descriptor, column);
								});
                        if (value == nullptr) {
                            throw std::runtime_error{
                                "bioseq.list_metadata produced a null "
                                "required text field"};
                        }
                        std::string owned{value};
						bioseq::pg_utils::invoke_pg_as_cpp(
							[value] { pfree(value); });
                        return owned;
                    };
                const auto table_name =
                    bioseq::pg_utils::pg_spi_cell::get(
                        tuple, descriptor, 1).optional_oid();
                const auto column_attno =
                    bioseq::pg_utils::pg_spi_cell::get(
                        tuple, descriptor, 2)
                        .optional<std::int16_t>();
                if (!table_name.has_value()
                    || !column_attno.has_value()) {
                    throw std::runtime_error{
                        "bioseq.list_metadata produced a null relation "
                        "identity"};
                }
                std::optional<bioseq::pg_utils::sql_interval> age;
                const auto age_cell =
                    bioseq::pg_utils::pg_spi_cell::get(
                        tuple, descriptor, 8);
                if (!age_cell.is_null) {
                    const auto* interval =
                        reinterpret_cast<const Interval*>(
                            DatumGetPointer(age_cell.value.raw()));
                    if (interval == nullptr) {
                        throw std::runtime_error{
                            "bioseq.list_metadata produced an invalid "
                            "interval"};
                    }
                    age = bioseq::pg_utils::sql_interval{
                        .time = interval->time,
                        .day = interval->day,
                        .month = interval->month,
                    };
                }
                result.push_back({
                    .table_name = *table_name,
                    .column_attno = *column_attno,
                    .column_name = required_text(3),
                    .name = required_text(4),
                    .index_type = required_text(5),
                    .update_policy = required_text(6),
                    .size = required_text(7),
                    .age = age,
                });
			}
		}
	}

	if (spi.finish() != SPI_OK_FINISH) {
        throw std::runtime_error{
            "bioseq.list_metadata could not finish SPI"};
    }
	return bioseq::pg_utils::returns_table<
        bioseq::pg::sql_rows::list_metadata_row>{
            std::move(result)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::alignment_execution_stats_row>
last_alignment_execution_stats() {
	std::vector<
		bioseq::pg::sql_rows::alignment_execution_stats_row> rows;
	if (last_alignment_execution_snapshot.has_value()) {
		const auto& snapshot = *last_alignment_execution_snapshot;
		(void)validate_alignment_snapshot_environment(snapshot);
		rows.reserve(bioseq::pipeline_stats::pipeline_stage_count);
		for (std::size_t stage_no = 0;
				stage_no < bioseq::pipeline_stats::pipeline_stage_count;
				++stage_no) {
			rows.push_back(
				make_owned_alignment_stats_row<
					alignment_stats_v1_column_count>(
						snapshot,
						static_cast<
							bioseq::pipeline_stats::PipelineStage>(
								stage_no)));
		}
	}
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::alignment_execution_stats_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::alignment_execution_stats_v2_row>
last_alignment_execution_stats_v2() {
	std::vector<
		bioseq::pg::sql_rows::alignment_execution_stats_v2_row> rows;
	if (last_alignment_execution_snapshot.has_value()) {
		const auto& snapshot = *last_alignment_execution_snapshot;
		(void)validate_alignment_snapshot_environment(snapshot);
		rows.reserve(bioseq::pipeline_stats::pipeline_stage_count);
		for (std::size_t stage_no = 0;
				stage_no < bioseq::pipeline_stats::pipeline_stage_count;
				++stage_no) {
			rows.push_back(
				make_owned_alignment_stats_row<
					alignment_stats_v2_column_count>(
						snapshot,
						static_cast<
							bioseq::pipeline_stats::PipelineStage>(
								stage_no)));
		}
	}
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::alignment_execution_stats_v2_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence() {
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format5_reader_memory_evidence_json(snapshot)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v2() {
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format5_reader_memory_evidence_json_v2(snapshot)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v3() {
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format5_reader_memory_evidence_json_v3(snapshot)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v4() {
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format5_reader_memory_evidence_json_v4(snapshot)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v5() {
    if (!last_alignment_execution_snapshot.has_value()) {
        return std::nullopt;
    }
    const alignment_exec::AlignmentExecutionSnapshot& snapshot =
        *last_alignment_execution_snapshot;
    (void)validate_alignment_snapshot_environment(snapshot);
    return bioseq::pg_utils::sql_jsonb{
        format5_reader_memory_evidence_json_v5(snapshot)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v6() {
    if (!last_alignment_execution_snapshot.has_value()) {
        return std::nullopt;
    }
    const alignment_exec::AlignmentExecutionSnapshot& snapshot =
        *last_alignment_execution_snapshot;
    (void)validate_alignment_snapshot_environment(snapshot);
    return bioseq::pg_utils::sql_jsonb{
        format5_reader_memory_evidence_json_v6(snapshot)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format6_reader_evidence() {
	if (!last_alignment_execution_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		format6_reader_evidence_json(snapshot)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_persisted_gpu_operation_evidence_v1() {
	if (!last_completed_alignment_evidence_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_completed_alignment_evidence_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		persisted_gpu_operation_evidence_json(snapshot, false)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_persisted_gpu_operation_evidence_v2() {
	if (!last_completed_alignment_evidence_snapshot.has_value()) {
		return std::nullopt;
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_completed_alignment_evidence_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	return bioseq::pg_utils::sql_jsonb{
		persisted_gpu_operation_evidence_json(snapshot, true)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * @brief Rebuilds a named sequence index and returns the number of indexed sequences.
 */
std::int64_t update_index(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string kind) {
	return bioseq::storage::indexing::update_index_pg_checked(
        rel, kind.data(), kind.size());
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
std::int64_t last_index_merge_run_count() {
	return static_cast<std::int64_t>(
        bioseq::storage::indexing::g_last_index_merge_run_count);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void reset_active_read_policy_debug() {
	bioseq::storage::indexing::reset_active_read_policy_debug_state();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bioseq::pg_utils::returns_table<
    bioseq::pg::sql_rows::active_read_policy_debug_row>
active_read_policy_debug() {
	const auto resolver = bioseq::storage::indexing::
		g_active_read_policy_resolution_counters;
	const auto dispatch = bioseq::storage::indexing::
		g_active_read_policy_dispatch_proof_counters;
	const auto source = bioseq::storage::indexing::
		active_read_policy_resolution_source_name(
			bioseq::storage::indexing::
				g_last_active_read_policy_dispatch_source);
    std::vector<bioseq::pg::sql_rows::active_read_policy_debug_row> rows;
    rows.push_back({
        .resolver_configured_rows =
            static_cast<std::int64_t>(resolver.configured_rows),
        .resolver_authoritative_defaults =
            static_cast<std::int64_t>(resolver.authoritative_defaults),
        .resolver_non_relation =
            static_cast<std::int64_t>(resolver.non_relation),
        .dispatch_configured_batches =
            static_cast<std::int64_t>(dispatch.configured_batches),
        .dispatch_authoritative_default_batches =
            static_cast<std::int64_t>(
                dispatch.authoritative_default_batches),
        .dispatch_non_relation_batches =
            static_cast<std::int64_t>(dispatch.non_relation_batches),
        .last_dispatch_source = std::string{source},
        .prohibited_file_constructor_entries =
            static_cast<std::int64_t>(bioseq::storage::indexing::
                g_prohibited_external_file_producer_entry_counters
                    .constructor_entries),
        .prohibited_file_writer_entries =
            static_cast<std::int64_t>(bioseq::storage::indexing::
                g_prohibited_external_file_producer_entry_counters
                    .writer_entries),
    });
    return bioseq::pg_utils::returns_table<
        bioseq::pg::sql_rows::active_read_policy_debug_row>{
            std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
    bioseq::pg::sql_rows::last_mps_build_stats_row>
last_mps_build_stats() {
	const auto st = bioseq::storage::indexing::g_last_mps_build_stats;
    std::vector<bioseq::pg::sql_rows::last_mps_build_stats_row> rows;
    rows.push_back({
        static_cast<std::int64_t>(st.backend_pid),
        static_cast<std::int64_t>(st.db_oid),
        static_cast<std::int64_t>(st.relfilenode),
        static_cast<std::int64_t>(st.sequence_rowno),
        static_cast<std::int64_t>(st.total_run_count),
        static_cast<std::int64_t>(st.resident_runs),
        static_cast<std::int64_t>(st.file_runs),
        static_cast<std::int64_t>(st.resident_run_bytes),
        static_cast<std::int64_t>(st.resident_peak_bytes),
        static_cast<std::int64_t>(st.spilled_run_bytes),
        static_cast<std::int64_t>(st.postgres_temp_file_bytes),
        static_cast<std::int64_t>(st.postgres_temp_file_count),
        static_cast<std::int64_t>(st.merge_reader_buffer_bytes),
        static_cast<std::int64_t>(st.merge_pass_count),
        static_cast<std::int64_t>(st.spill_count_from_pressure),
        static_cast<std::int64_t>(st.resident_demotions),
        static_cast<std::int64_t>(st.position_spill_bytes),
        static_cast<std::int64_t>(st.position_spill_count),
    });
    return bioseq::pg_utils::returns_table<
        bioseq::pg::sql_rows::last_mps_build_stats_row>{
            std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
    bioseq::pg::sql_rows::scheduler_mps_build_stats_row>
scheduler_mps_build_stats() {
	const auto snap = bioseq::parallelism::pg::async_mps_build_stats_snapshot();
	const auto st = snap.stats;
    std::vector<bioseq::pg::sql_rows::scheduler_mps_build_stats_row> rows;
    rows.push_back({
        static_cast<std::int64_t>(st.backend_pid),
        static_cast<std::int64_t>(st.db_oid),
        static_cast<std::int64_t>(st.relfilenode),
        static_cast<std::int64_t>(st.sequence_rowno),
        static_cast<std::int64_t>(st.total_run_count),
        static_cast<std::int64_t>(st.resident_runs),
        static_cast<std::int64_t>(st.file_runs),
        static_cast<std::int64_t>(st.resident_run_bytes),
        static_cast<std::int64_t>(st.resident_peak_bytes),
        static_cast<std::int64_t>(st.spilled_run_bytes),
        static_cast<std::int64_t>(st.postgres_temp_file_bytes),
        static_cast<std::int64_t>(st.postgres_temp_file_count),
        static_cast<std::int64_t>(st.merge_reader_buffer_bytes),
        static_cast<std::int64_t>(st.merge_pass_count),
        static_cast<std::int64_t>(st.spill_count_from_pressure),
        static_cast<std::int64_t>(st.resident_demotions),
        static_cast<std::int64_t>(st.position_spill_bytes),
        static_cast<std::int64_t>(st.position_spill_count),
        static_cast<std::int64_t>(snap.generation),
        static_cast<std::int64_t>(snap.worker_build_ns),
        static_cast<std::int64_t>(snap.pipeline_mode),
        static_cast<std::int64_t>(snap.max_concurrency),
        static_cast<std::int64_t>(snap.index_memory_cap_mb),
        static_cast<std::int64_t>(snap.lazy_worker_shareable),
    });
    return bioseq::pg_utils::returns_table<
        bioseq::pg::sql_rows::scheduler_mps_build_stats_row>{
            std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool arm_async_mps_worker_failure(std::string failure_point) {
#ifdef BIOSEQ_TEST_INDEXES
	const std::string_view point_name{failure_point};
	using StageFailure = bioseq::parallelism::pg::AsyncMpsStagePgFailure;
	StageFailure stage_point = StageFailure::none;
	if (point_name == "stage_register_xact_callback")
		stage_point = StageFailure::register_xact_callback;
	else if (point_name == "stage_dsm_create")
		stage_point = StageFailure::dsm_create;
	else if (point_name == "stage_dsm_pin")
		stage_point = StageFailure::dsm_pin;
	else if (point_name == "stage_dsm_detach")
		stage_point = StageFailure::dsm_detach;
	if (stage_point != StageFailure::none) {
		return bioseq::parallelism::pg::
			arm_async_mps_stage_pg_failure(stage_point);
	}
	using Failure = bioseq::parallelism::pg::AsyncMpsWorkerPgFailure;
	Failure point = Failure::none;
	if (point_name == "relation_open") point = Failure::relation_open;
	else if (point_name == "relation_identity") point = Failure::relation_identity;
	else if (point_name == "relation_close") point = Failure::relation_close;
	else if (point_name == "spi_connect") point = Failure::spi_connect;
	else if (point_name == "spi_execute") point = Failure::spi_execute;
	else if (point_name == "spi_read") point = Failure::spi_read;
	else if (point_name == "spi_finish") point = Failure::spi_finish;
	else if (point_name == "materialize_error_log")
		point = Failure::materialize_error_log;
	else if (point_name == "materialize_interrupt")
		point = Failure::materialize_interrupt;
	else if (point_name == "materialize_scanner_read")
		point = Failure::materialize_scanner_read;
	else {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{
				.message =
					"unknown asynchronous MPS worker failure point",
				.detail = fmt::format(
					"failure_point={}", point_name),
			});
	}
	return bioseq::parallelism::pg::
		arm_async_mps_worker_pg_failure(point);
#else
	(void)failure_point;
	bioseq::pg_utils::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{
			.message =
				"asynchronous MPS worker failure injection is unavailable",
			.detail =
				"The extension was built without BIOSEQ_TEST_INDEXES.",
		});
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void disarm_async_mps_worker_failure() {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::parallelism::pg::disarm_async_mps_worker_pg_failure();
	bioseq::parallelism::pg::disarm_async_mps_stage_pg_failure();
#else
	bioseq::pg_utils::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{
			.message =
				"asynchronous MPS worker failure injection is unavailable",
			.detail =
				"The extension was built without BIOSEQ_TEST_INDEXES.",
		});
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::async_mps_worker_boundary_stats_row>
async_mps_worker_boundary_stats() {
	const auto stats = bioseq::parallelism::pg::
		async_mps_worker_boundary_stats_snapshot();
	const char* last_point = bioseq::parallelism::pg::
		async_mps_worker_failure_point_name(stats.last_failure_point);
	const char* armed_point = bioseq::parallelism::pg::
		async_mps_worker_failure_point_name(stats.armed_failure_point);
	std::vector<
		bioseq::pg::sql_rows::async_mps_worker_boundary_stats_row> rows;
	rows.push_back({
		static_cast<std::int64_t>(stats.relation_leaf_calls),
		static_cast<std::int64_t>(stats.spi_leaf_calls),
		static_cast<std::int64_t>(
			stats.materialize_interrupt_leaf_calls),
		static_cast<std::int64_t>(
			stats.materialize_scanner_read_leaf_calls),
		static_cast<std::int64_t>(stats.pg_leaf_failures),
		static_cast<std::int64_t>(stats.injected_pg_leaf_failures),
		static_cast<std::int64_t>(stats.materialize_admissions),
		static_cast<std::int64_t>(stats.materialize_failure_unwinds),
		static_cast<std::int64_t>(stats.materialize_admission_restored),
		static_cast<std::int64_t>(stats.materialize_admission_mismatches),
		std::string{last_point},
		std::string{armed_point},
		stats.last_failure_injected ? "XX000" : "",
		stats.last_failure_injected
			? "injected asynchronous MPS worker PostgreSQL leaf failure"
			: "",
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::async_mps_worker_boundary_stats_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void scheduler_restart() {
	bioseq::parallelism::pg::restart_cluster();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::scheduler_stats_row>
scheduler_stats() {
	const auto snap = bioseq::parallelism::pg::snapshot();
	const std::string mode_string{bioseq::parallelism::to_string(snap.mode)};
	std::vector<bioseq::pg::sql_rows::scheduler_stats_row> rows;
	rows.push_back({
		snap.supervisor,
		static_cast<std::int32_t>(snap.config_generation),
		mode_string,
		static_cast<std::int32_t>(snap.min_threads),
		static_cast<std::int32_t>(snap.max_threads),
		static_cast<std::int32_t>(snap.current_threads),
		static_cast<std::int64_t>(snap.batches_scheduled),
		static_cast<std::int64_t>(snap.batches_awaiting),
		static_cast<std::int64_t>(snap.batches_processed),
		static_cast<std::int64_t>(snap.queries_waiting),
		static_cast<std::int64_t>(snap.queries_processed),
		snap.oldest_waiting_batch_age_secs,
		snap.queries_per_sec,
		snap.uptime_secs,
		static_cast<std::int64_t>(snap.async_persist_jobs_waiting),
		static_cast<std::int64_t>(snap.index_memory_reserved_bytes),
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::scheduler_stats_row>{std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool cpu_prepared_target_same_object_reprepare_probe() {
	return bioseq::simd::HWY_NAMESPACE::BatchProcessor::
		cpu_prepared_target_same_object_reprepare_probe();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
std::int64_t index_table_row_count(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name) {
	return bioseq::storage::indexing::
		index_table_row_count_impl(rel, name);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::named_out<
	bioseq::pg::sql_rows::inspect_cold_no_index_absence_row>
inspect_cold_no_index_absence(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name) {
	const auto audit =
		bioseq::storage::indexing::cold_no_index_absence_audit_impl(rel, name);
	const auto sql_bigint = [](const std::uint64_t value) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{.message =
					"bioseq cold no-index audit value exceeds BIGINT"});
		}
		return static_cast<std::int64_t>(value);
	};
	return bioseq::pg_utils::named_out<
		bioseq::pg::sql_rows::inspect_cold_no_index_absence_row>{
		bioseq::pg::sql_rows::inspect_cold_no_index_absence_row{
			sql_bigint(audit.target_rows),
			audit.target_cardinality_metadata_only,
			sql_bigint(audit.source_parent_rows_read),
			sql_bigint(audit.source_fragment_rows_read),
			sql_bigint(audit.source_decoded_bp),
			sql_bigint(audit.physical_index_relations),
			sql_bigint(audit.persisted_index_rows),
			sql_bigint(audit.generation_objects),
		},
	};
}
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Validates a source-bound ready minimizer generation without reading
 * payload extents or enabling live generation reads.
 */
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::inspect_ready_minimizer_generation_row>
inspect_ready_minimizer_generation(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string index_name,
	std::int64_t query_key) {
	if (query_key < 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"bioseq minimizer generation inspection query key is negative"});
	}
	const auto inspection = bioseq::storage::indexing::
		inspect_ready_minimizer_generation_impl(
			rel, index_name,
			static_cast<std::uint64_t>(query_key));
	const auto sql_bigint = [](std::uint64_t value, const char* field) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{
					.message =
						"bioseq minimizer generation inspection field exceeds BIGINT",
					.detail = fmt::format("field={}", field),
				});
		}
		return static_cast<std::int64_t>(value);
	};
	const auto sql_smallint = [](std::uint64_t value, const char* field) {
		if (value > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int16_t>::max)())) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{
					.message =
						"bioseq minimizer generation inspection field exceeds SMALLINT",
					.detail = fmt::format("field={}", field),
				});
		}
		return static_cast<std::int16_t>(value);
	};
	const auto sql_integer = [](std::uint64_t value, const char* field) {
		if (value > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int32_t>::max)())) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				{
					.message =
						"bioseq minimizer generation inspection field exceeds INTEGER",
					.detail = fmt::format("field={}", field),
				});
		}
		return static_cast<std::int32_t>(value);
	};
	std::vector<
		bioseq::pg::sql_rows::inspect_ready_minimizer_generation_row> rows;
	rows.push_back({
		sql_bigint(inspection.identity.generation_id, "generation_id"),
		fixed_hex_u64(inspection.identity.source_identity),
		fixed_hex_u64(inspection.source.compatibility_checksum),
		fixed_hex_u64(inspection.source_content_checksum),
		inspection.database_oid,
		inspection.main_relfilenode,
		inspection.column_attno,
		sql_bigint(inspection.source.sequence_rowno, "sequence_rowno"),
		sql_smallint(inspection.source.locator.version, "locator_version"),
		sql_smallint(inspection.source.locator.bucket_idx, "bucket_idx"),
		sql_integer(inspection.source.locator.shard_idx, "shard_idx"),
		sql_bigint(inspection.source.locator.num_fragments, "num_fragments"),
		sql_bigint(inspection.source.locator.total_bp_size, "total_bp_size"),
		sql_bigint(
			inspection.source.locator.first_sub_row_no,
			"first_sub_row_no"),
		sql_integer(inspection.source.padding_letters, "padding_letters"),
		sql_smallint(inspection.source.alphabet_code, "alphabet_code"),
		sql_smallint(
			inspection.source.storage_unit_byte_width,
			"storage_unit_byte_width"),
		sql_smallint(inspection.manifest.kmer_size, "kmer_size"),
		sql_smallint(inspection.manifest.window_size, "window_size"),
		sql_integer(inspection.manifest.gpu_abi_version, "gpu_abi_version"),
		sql_smallint(
			inspection.manifest.segment_geometry_version,
			"segment_geometry_version"),
		sql_bigint(
			inspection.manifest.position_segment_span_bp,
			"position_segment_span_bp"),
		sql_integer(
			inspection.manifest.position_segment_count,
			"position_segment_count"),
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::inspect_ready_minimizer_generation_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
/**
 * @brief Validates a source-bound ready Format-6 generation and verifies its
 * metadata, catalog rows, byte counts, and checksums.
 */
bioseq::pg_utils::sql_jsonb
inspect_ready_minimizer_format6_generation(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string index_name,
	std::int64_t query_key) {
	if (query_key < 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "bioseq Format-6 inspection query key is negative"});
	}
	const auto inspection = bioseq::storage::indexing::
		inspect_ready_minimizer_format6_generation_impl(
			rel, index_name,
			static_cast<std::uint64_t>(query_key));
	return bioseq::pg_utils::sql_jsonb{
		ready_minimizer_format6_inspection_json(inspection)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
/**
 * @brief Validates a ready generation row, plain payload, and address checksum
 * using a serial scan when requested_workers is zero or a shared parallel heap
 * scan when it is positive.
 */
bioseq::pg_utils::sql_jsonb format6_parallel_scan_probe(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int32_t requested_workers) {
	if (sequence_rowno < 0 || requested_workers < 0
		|| requested_workers > 64) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"bioseq Format-6 parallel scan probe parameters are invalid"});
	}
	const auto proof = bioseq::storage::indexing::
		format6_parallel_scan_probe_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno),
			static_cast<std::uint32_t>(requested_workers));
	return bioseq::pg_utils::sql_jsonb{
		format6_parallel_scan_probe_json(proof)};
}

} // namespace bioseq::pg::sql_routines

namespace {

[[nodiscard]] static std::int64_t fixture_sql_bigint(
		const std::uint64_t value,
		const std::string_view context,
		const std::string_view field) {
	if (!bioseq::indexing::sql_bigint_can_represent(value)) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
			{
				.message = fmt::format("{} field exceeds BIGINT", context),
				.detail = fmt::format("field={}", field),
			});
	}
	return static_cast<std::int64_t>(value);
}

[[nodiscard]] static std::int32_t fixture_sql_integer(
		const std::uint64_t value,
		const std::string_view context,
		const std::string_view field) {
	if (value > static_cast<std::uint64_t>(
			(std::numeric_limits<std::int32_t>::max)())) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
			{
				.message = fmt::format("{} field exceeds INTEGER", context),
				.detail = fmt::format("field={}", field),
			});
	}
	return static_cast<std::int32_t>(value);
}

[[nodiscard]] static auto minimizer_fixture_backend(
		const std::string_view requested_backend) {
	std::string_view canonical_backend = requested_backend;
	if (canonical_backend == "exact") {
		canonical_backend = "exact_simd";
	} else if (canonical_backend == "paper") {
		canonical_backend = "paper_nthash_simd";
	} else if (canonical_backend == "vulkan") {
		canonical_backend = "vulkan_segment";
	}
	return bioseq::storage::indexing::
		ready_minimizer_fixture_backend_from_name(canonical_backend);
}

static void validate_minimizer_fixture_parameters(
		const std::int64_t sequence_rowno,
		const std::int16_t kmer_size,
		const std::int16_t window_size,
		const std::int16_t wire_format,
		const bool allow_format5) {
	const bool valid_format =
		wire_format == 3 || wire_format == 4
		|| (allow_format5 && wire_format == 5);
	if (sequence_rowno < 0 || kmer_size <= 0 || window_size <= 0
		|| !valid_format) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "minimizer fixture parameters are invalid"});
	}
}

} // namespace

export namespace bioseq::pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Builds and publishes an authenticated minimizer generation for one
 * explicit sketch backend without opening a read session.
 */
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::build_minimizer_fixture_row>
build_minimizer_fixture(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_wire_format,
	[[bioseq_sql::param({
		.default_value = bioseq::sql::checked_sql{"'scalar'"},
	})]]
	std::string requested_backend) {
	validate_minimizer_fixture_parameters(
		sequence_rowno, kmer_size, window_size,
		selected_wire_format, false);
	const auto wire_format = static_cast<
		bioseq::indexing::MinimizerGenerationWireFormat>(
			static_cast<std::uint16_t>(selected_wire_format));
	const auto fixture =
		bioseq::storage::indexing::build_rev3_pg_integration_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno),
			static_cast<std::uint16_t>(kmer_size),
			static_cast<std::uint16_t>(window_size),
			wire_format,
			minimizer_fixture_backend(requested_backend));
	constexpr std::string_view context = "minimizer build fixture";
	std::vector<bioseq::pg::sql_rows::build_minimizer_fixture_row> rows;
	rows.push_back({
		fixture.table_name,
		static_cast<std::int16_t>(fixture.wire_format),
		fixture_sql_bigint(fixture.generation_id, context, "generation_id"),
		fixed_hex_u64(fixture.source_identity),
		fixed_hex_u64(fixture.source_content_checksum),
		fixed_hex_u64(fixture.compatibility_checksum),
		fixture_sql_bigint(
			fixture.source_num_fragments, context, "source_num_fragments"),
		fixture_sql_bigint(
			fixture.source_total_bp, context, "source_total_bp"),
		fixture_sql_bigint(
			fixture.source_padding_letters, context,
			"source_padding_letters"),
		fixture_sql_bigint(fixture.writer_rows, context, "writer_rows"),
		fixture_sql_bigint(fixture.writer_batches, context, "writer_batches"),
		fixture_sql_bigint(
			fixture.published_head_bytes, context, "published_head_bytes"),
		fixture_sql_bigint(fixture.control_count, context, "control_count"),
		fixture_sql_bigint(fixture.position_count, context, "position_count"),
		fixture_sql_bigint(
			fixture.directory_entries, context, "directory_entries"),
		fixture_sql_bigint(
			fixture.position_segment_count, context,
			"position_segment_count"),
		fixture_sql_bigint(
			fixture.control_encoded_bytes, context,
			"control_encoded_bytes"),
		fixture_sql_bigint(
			fixture.position_encoded_bytes, context,
			"position_encoded_bytes"),
		fixture_sql_bigint(fixture.control_chunks, context, "control_chunks"),
		fixture_sql_bigint(
			fixture.position_chunks, context, "position_chunks"),
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::build_minimizer_fixture_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Opens an authenticated minimizer-generation read session and returns
 * payload validation, lookup, scan, and cache counters.
 */
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::read_minimizer_fixture_row>
read_minimizer_fixture(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_wire_format,
	[[bioseq_sql::param({
		.default_value = bioseq::sql::checked_sql{"'scalar'"},
	})]]
	std::string requested_backend) {
	validate_minimizer_fixture_parameters(
		sequence_rowno, kmer_size, window_size,
		selected_wire_format, false);
	const auto wire_format = static_cast<
		bioseq::indexing::MinimizerGenerationWireFormat>(
			static_cast<std::uint16_t>(selected_wire_format));
	const auto fixture =
		bioseq::storage::indexing::read_rev3_pg_integration_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno),
			static_cast<std::uint16_t>(kmer_size),
			static_cast<std::uint16_t>(window_size),
			wire_format,
			minimizer_fixture_backend(requested_backend));
	constexpr std::string_view context = "minimizer read fixture";
	const auto& stats = fixture.read_stats;
	std::vector<bioseq::pg::sql_rows::read_minimizer_fixture_row> rows;
	rows.push_back({
		fixture.table_name,
		static_cast<std::int16_t>(fixture.wire_format),
		fixture_sql_bigint(fixture.generation_id, context, "generation_id"),
		fixed_hex_u64(fixture.source_identity),
		fixed_hex_u64(fixture.source_content_checksum),
		fixed_hex_u64(fixture.compatibility_checksum),
		fixture_sql_bigint(
			fixture.source_num_fragments, context, "source_num_fragments"),
		fixture_sql_bigint(
			fixture.source_total_bp, context, "source_total_bp"),
		fixture_sql_bigint(
			fixture.source_padding_letters, context,
			"source_padding_letters"),
		fixture_sql_bigint(
			fixture.validated_control_bytes, context,
			"validated_control_bytes"),
		fixture_sql_bigint(
			fixture.validated_position_bytes, context,
			"validated_position_bytes"),
		fixed_hex_u64(fixture.validated_control_checksum),
		fixed_hex_u64(fixture.validated_position_checksum),
		fixture_sql_bigint(fixture.lookup_count, context, "lookup_count"),
		fixture_sql_bigint(fixture.lookup_bp, context, "lookup_bp"),
		fixture.lookup_reverse,
		fixture.positions_complete,
		fixture_sql_bigint(stats.relation_opens, context, "relation_opens"),
		fixture_sql_bigint(stats.index_scans, context, "index_scans"),
		fixture_sql_bigint(stats.head_scans, context, "head_scans"),
		fixture_sql_bigint(
			stats.manifest_directory_scans, context,
			"manifest_directory_scans"),
		fixture_sql_bigint(
			stats.planned_request_groups, context,
			"planned_request_groups"),
		fixture_sql_bigint(
			stats.executed_request_groups, context,
			"executed_request_groups"),
		fixture_sql_bigint(
			stats.bulk_read_calls, context, "bulk_read_calls"),
		fixture_sql_bigint(
			stats.range_scan_calls, context, "range_scan_calls"),
		fixture_sql_bigint(
			stats.payload.wanted_extents, context,
			"payload_wanted_extents"),
		fixture_sql_bigint(
			stats.payload.read_extents, context, "payload_read_extents"),
		fixture_sql_bigint(
			stats.payload.wanted_chunks, context,
			"payload_wanted_chunks"),
		fixture_sql_bigint(
			stats.payload.delivered_chunks, context,
			"payload_delivered_chunks"),
		fixture_sql_bigint(
			stats.payload.delivered_bytes, context,
			"payload_delivered_bytes"),
		fixture_sql_bigint(
			stats.payload_heap_rows_read, context,
			"payload_heap_rows_read"),
		fixture_sql_bigint(
			stats.payload.skipped_rows, context, "payload_skipped_rows"),
		fixture_sql_bigint(
			stats.toast_values_read, context, "toast_values_read"),
		fixture_sql_bigint(
			stats.detoast_bytes, context, "detoast_bytes"),
		fixture_sql_bigint(stats.cache_lookups, context, "cache_lookups"),
		bioseq::pg_utils::sql_jsonb{
			cold_index_selective_probe_json(
				fixture.selective_probe_stats.cold)},
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::read_minimizer_fixture_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Scans one complete minimizer generation through the requested sketch
 * backend and returns payload and scan counters without decoding payload bytes.
 */
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::scan_minimizer_generation_fixture_row>
scan_minimizer_generation_fixture(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_wire_format,
	std::string requested_backend) {
	validate_minimizer_fixture_parameters(
		sequence_rowno, kmer_size, window_size,
		selected_wire_format, true);
	const auto fixture = bioseq::storage::indexing::
		read_rev3_pg_full_generation_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno),
			static_cast<std::uint16_t>(kmer_size),
			static_cast<std::uint16_t>(window_size),
			static_cast<
				bioseq::indexing::MinimizerGenerationWireFormat>(
					static_cast<std::uint16_t>(selected_wire_format)),
			minimizer_fixture_backend(requested_backend));
	constexpr std::string_view context =
		"minimizer generation scan fixture";
	const auto& stats = fixture.read_stats;
	std::vector<
		bioseq::pg::sql_rows::scan_minimizer_generation_fixture_row> rows;
	rows.push_back({
		fixture.table_name,
		fixture.selected_backend_name,
		fixture_sql_bigint(fixture.generation_id, context, "generation_id"),
		fixture_sql_bigint(fixture.control_bytes, context, "control_bytes"),
		fixture_sql_bigint(
			fixture.position_bytes, context, "position_bytes"),
		fixed_hex_u64(fixture.control_checksum),
		fixed_hex_u64(fixture.position_checksum),
		fixture_sql_bigint(stats.relation_opens, context, "relation_opens"),
		fixture_sql_bigint(stats.index_scans, context, "index_scans"),
		fixture_sql_bigint(
			stats.range_scan_calls, context, "range_scan_calls"),
		fixture_sql_bigint(
			stats.payload_heap_rows_read, context, "payload_heap_rows"),
		fixture_sql_bigint(
			stats.payload.delivered_chunks, context, "delivered_chunks"),
		fixture_sql_bigint(
			stats.payload.delivered_bytes, context, "delivered_bytes"),
		fixture_sql_bigint(
			stats.full_generation.planned_extents, context,
			"planned_extents"),
		fixture_sql_bigint(
			stats.full_generation.completed_extents, context,
			"completed_extents"),
		fixture_sql_bigint(
			stats.full_generation.scan_calls, context, "scan_calls"),
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::scan_minimizer_generation_fixture_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Removes the selected non-first sequence fragment referenced by a
 * source locator.
 */
void remove_sequence_fragment(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int32_t fragment_ordinal) {
	if (sequence_rowno < 0 || fragment_ordinal <= 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "source-fragment fixture parameters are invalid"});
	}
	bioseq::storage::indexing::remove_sequence_fragment_impl(
		source_table, index_name,
		static_cast<std::uint64_t>(sequence_rowno),
		static_cast<std::uint32_t>(fragment_ordinal));
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Copies selected primary-index predicate or expression metadata from a
 * donor index inside the current transaction.
 */
void copy_primary_index_shape(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid target_table,
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid donor_index,
	std::string shape) {
	if (shape != "partial" && shape != "expression") {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"index shape must be partial or expression"});
	}
	bioseq::storage::indexing::copy_primary_index_shape_pg(
		target_table, donor_index, shape == "partial");
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Applies the requested mask to matching source and generation checksums
 * while keeping their metadata consistent.
 */
void rewrite_source_checksum(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid target_table,
	std::int64_t query_key,
	std::int64_t xor_mask) {
	if (query_key < 0 || xor_mask == 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "checksum rewrite parameters are invalid"});
	}
	bioseq::storage::indexing::rewrite_source_checksum_pg(
		target_table,
		static_cast<std::uint64_t>(query_key),
		std::bit_cast<std::uint64_t>(xor_mask));
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Exercises the requested read-session failure mode and verifies permit
 * and resource cleanup.
 */
void exercise_generation_session_failure(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t query_key,
	std::string failure_mode) {
	if (query_key < 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"generation session fixture query key is negative"});
	}
	bioseq::storage::indexing::exercise_rev3_pg_session_failure_impl(
		source_table, index_name,
		static_cast<std::uint64_t>(query_key), failure_mode);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Runs the selected generation-storage transition and returns
 * relation-handle, rollback, and cache-invalidation state.
 */
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::generation_storage_state_row>
inspect_generation_storage_transition(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string operation) {
	if (sequence_rowno < 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"generation-storage fixture query key is negative"});
	}
	const auto state = bioseq::storage::indexing::
		exercise_rev3_generation_storage_state_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno), operation);
	const auto optional_oid = [](const Oid value) -> std::optional<Oid> {
		if (OidIsValid(value)) {
			return value;
		}
		return std::nullopt;
	};
	std::vector<bioseq::pg::sql_rows::generation_storage_state_row> rows;
	rows.push_back({
		optional_oid(state.before_oid),
		optional_oid(state.operation_oid),
		optional_oid(state.after_oid),
		state.same_handle_rebound,
		state.generation_schema_after,
		state.chunk_cache_hit,
		state.assembled_cache_hit,
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::generation_storage_state_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Builds and publishes a ready PostgreSQL-backed MPF generation without
 * changing active routing or metadata age.
 */
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::build_ready_mpf_generation_row>
build_ready_mpf_generation(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string backend) {
	if (sequence_rowno < 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "ready MPF fixture row number is negative"});
	}
	if (index_name
			!= bioseq::indexing::MINIMIZERS_PER_FRAGMENT_INDEX_NAME) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"ready MPF fixture requires minimizers_per_fragment"});
	}
	const auto fixture = bioseq::storage::indexing::
		build_ready_minimizer_generation_coordinator_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno), backend);
	const auto& publication = fixture.sealed_publication();
	const auto* execution = std::get_if<
		bioseq::indexing::MpfGenerationExecutionEvidence>(
			&publication.execution());
	if (execution == nullptr) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_DATA_CORRUPTED,
			{.message = "ready MPF fixture returned non-MPF evidence"});
	}
	constexpr std::string_view context = "ready MPF fixture";
	const auto paper_accepted =
		execution->packed_paper_proof_present
		&& execution->packed_paper_status
			== bioseq::indexing::PackedPaperNthashIndexStatus::ok
		&& execution->packed_paper_proof.accepted(
			/*require_nonempty_vector_work=*/true);
	std::vector<
		bioseq::pg::sql_rows::build_ready_mpf_generation_row> rows;
	rows.push_back({
		fixture.table_name,
		fixture.column_attno,
		fixture_sql_bigint(
			fixture.sequence_rowno, context, "sequence_rowno"),
		fixed_hex_u64(fixture.locator.source_epoch),
		fixture_sql_bigint(
			fixture.locator.num_fragments, context,
			"source_num_fragments"),
		fixture_sql_bigint(
			fixture.locator.total_bp_size, context, "source_total_bp"),
		fixture.selected_backend_name,
		static_cast<std::int16_t>(fixture.manifest.wire_format),
		publication.published(),
		fixture.head_manifest_verified,
		fixture_sql_bigint(
			publication.generation_id(), context, "generation_id"),
		fixed_hex_u64(publication.source_identity()),
		fixed_hex_u64(publication.source_content_checksum()),
		fixed_hex_u64(publication.compatibility_checksum()),
		fixture_sql_bigint(
			fixture.manifest.target_length, context,
			"manifest_target_length"),
		static_cast<std::int16_t>(fixture.manifest.kmer_size),
		static_cast<std::int16_t>(fixture.manifest.window_size),
		fixture_sql_integer(
			fixture.manifest.minimizer_scheme_id, context,
			"minimizer_scheme_id"),
		fixture_sql_integer(
			fixture.manifest.minimizer_scheme_version, context,
			"minimizer_scheme_version"),
		fixture_sql_integer(
			fixture.manifest.gpu_abi_version, context,
			"gpu_abi_version"),
		fixture_sql_bigint(
			fixture.manifest.position_segment_span_bp, context,
			"position_segment_span_bp"),
		fixture_sql_integer(
			fixture.manifest.position_segment_count, context,
			"position_segment_count"),
		fixture_sql_bigint(
			publication.writer_rows(), context, "writer_rows"),
		fixture_sql_bigint(
			publication.writer_batches(), context, "writer_batches"),
		fixture_sql_bigint(
			publication.control_count(), context, "control_count"),
		fixture_sql_bigint(
			publication.position_count(), context, "position_count"),
		fixture_sql_bigint(
			execution->producer_scan_count, context,
			"producer_scan_count"),
		fixture_sql_bigint(
			execution->geometry_retry_count, context,
			"geometry_retry_count"),
		fixture_sql_bigint(
			execution->short_core_runs, context, "short_core_runs"),
		fixture_sql_bigint(
			execution->scalar_core_runs, context, "scalar_core_runs"),
		fixture_sql_bigint(
			execution->exact_simd_core_runs, context,
			"exact_simd_core_runs"),
		fixture_sql_bigint(
			execution->paper_nthash_core_runs, context,
			"paper_nthash_core_runs"),
		fixture_sql_bigint(
			execution->vulkan_core_runs, context, "vulkan_core_runs"),
		execution->accepted(),
		execution->exact_simd_proof_present,
		execution->exact_simd_proof_present
			&& execution->exact_simd_proof.accepted(
				/*require_work=*/true),
		fixture_sql_bigint(
			execution->exact_simd_proof.vector_batches, context,
			"exact_vector_batches"),
		fixture_sql_bigint(
			execution->exact_simd_proof.scalar_backend_branches,
			context, "exact_scalar_backend_branches"),
		execution->packed_paper_proof_present,
		std::string{
			bioseq::indexing::packed_paper_nthash_index_status_name(
				execution->packed_paper_status)},
		paper_accepted,
		fixture_sql_bigint(
			execution->packed_paper_proof.packed_decode_batches,
			context, "paper_packed_decode_batches"),
		fixture_sql_bigint(
			execution->packed_paper_proof.fallback_calls, context,
			"paper_fallback_calls"),
		fixture_sql_bigint(
			execution->vulkan_proof.completed_roundtrips, context,
			"vulkan_completed_roundtrips"),
		fixture_sql_bigint(
			execution->vulkan_proof.downloaded_minimizers, context,
			"vulkan_downloaded_minimizers"),
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::build_ready_mpf_generation_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Builds and publishes a ready PostgreSQL-backed MPS generation,
 * releases its spill runs, and leaves active routing and metadata age unchanged.
 */
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::build_ready_mps_generation_row>
build_ready_mps_generation(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string backend) {
	if (sequence_rowno < 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "ready MPS fixture row number is negative"});
	}
	if (index_name
			!= bioseq::indexing::MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"ready MPS fixture requires minimizers_per_sequence"});
	}
	const auto fixture = bioseq::storage::indexing::
		build_ready_minimizer_generation_coordinator_fixture_impl(
			source_table, index_name,
			static_cast<std::uint64_t>(sequence_rowno), backend);
	const auto& publication = fixture.sealed_publication();
	const auto* execution = std::get_if<
		bioseq::indexing::MpsGenerationExecutionEvidence>(
			&publication.execution());
	if (execution == nullptr) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_DATA_CORRUPTED,
			{.message = "ready MPS fixture returned non-MPS evidence"});
	}
	constexpr std::string_view context = "ready MPS fixture";
	const auto paper_accepted =
		execution->packed_paper_proof_present
		&& execution->packed_paper_status
			== bioseq::indexing::PackedPaperNthashIndexStatus::ok
		&& execution->packed_paper_proof.accepted(
			/*require_nonempty_vector_work=*/true);
	std::vector<
		bioseq::pg::sql_rows::build_ready_mps_generation_row> rows;
	rows.push_back({
		fixture.table_name,
		fixture.column_attno,
		fixture_sql_bigint(
			fixture.sequence_rowno, context, "sequence_rowno"),
		fixed_hex_u64(fixture.locator.source_epoch),
		fixture_sql_bigint(
			fixture.locator.num_fragments, context,
			"source_num_fragments"),
		fixture_sql_bigint(
			fixture.locator.total_bp_size, context, "source_total_bp"),
		fixture.selected_backend_name,
		static_cast<std::int16_t>(fixture.manifest.wire_format),
		publication.published(),
		fixture.head_manifest_verified,
		fixture_sql_bigint(
			publication.generation_id(), context, "generation_id"),
		fixed_hex_u64(publication.source_identity()),
		fixed_hex_u64(publication.source_content_checksum()),
		fixed_hex_u64(publication.compatibility_checksum()),
		fixture_sql_bigint(
			fixture.manifest.target_length, context,
			"manifest_target_length"),
		static_cast<std::int16_t>(fixture.manifest.kmer_size),
		static_cast<std::int16_t>(fixture.manifest.window_size),
		fixture_sql_integer(
			fixture.manifest.minimizer_scheme_id, context,
			"minimizer_scheme_id"),
		fixture_sql_integer(
			fixture.manifest.minimizer_scheme_version, context,
			"minimizer_scheme_version"),
		fixture_sql_integer(
			fixture.manifest.gpu_abi_version, context,
			"gpu_abi_version"),
		fixture_sql_bigint(
			fixture.manifest.position_segment_span_bp, context,
			"position_segment_span_bp"),
		fixture_sql_integer(
			fixture.manifest.position_segment_count, context,
			"position_segment_count"),
		fixture_sql_bigint(
			publication.writer_rows(), context, "writer_rows"),
		fixture_sql_bigint(
			publication.writer_batches(), context, "writer_batches"),
		fixture_sql_bigint(
			publication.control_count(), context, "control_count"),
		fixture_sql_bigint(
			publication.position_count(), context, "position_count"),
		fixture_sql_bigint(
			execution->spill_run_count, context, "spill_run_count"),
		fixture_sql_bigint(
			execution->spilled_event_count, context,
			"spilled_event_count"),
		fixture_sql_bigint(
			execution->spill_operation_epoch, context,
			"spill_operation_epoch"),
		fixture_sql_bigint(
			execution->spill_storage_binding.generation_token, context,
			"spill_generation_token"),
		fixture_sql_bigint(
			execution->spill_storage_binding.mutation_version, context,
			"spill_mutation_version"),
		fixture_sql_bigint(
			execution->spill_qualification_scan_count, context,
			"spill_qualification_scan_count"),
		fixture_sql_bigint(
			execution->spill_qualification_merge_count, context,
			"spill_qualification_merge_count"),
		fixture_sql_bigint(
			execution->configured_core_span_bp, context,
			"configured_core_span_bp"),
		fixture_sql_bigint(
			execution->peak_core_span_bp, context, "peak_core_span_bp"),
		fixture_sql_bigint(
			execution->peak_core_events, context, "peak_core_events"),
		fixture_sql_bigint(
			execution->short_core_runs, context, "short_core_runs"),
		fixture_sql_bigint(
			execution->scalar_core_runs, context, "scalar_core_runs"),
		fixture_sql_bigint(
			execution->exact_simd_core_runs, context,
			"exact_simd_core_runs"),
		fixture_sql_bigint(
			execution->paper_nthash_core_runs, context,
			"paper_nthash_core_runs"),
		fixture_sql_bigint(
			execution->vulkan_core_runs, context, "vulkan_core_runs"),
		execution->accepted(),
		execution->exact_simd_proof_present,
		execution->exact_simd_proof_present
			&& execution->exact_simd_proof.accepted(
				/*require_work=*/true),
		fixture_sql_bigint(
			execution->exact_simd_proof.vector_batches, context,
			"exact_vector_batches"),
		fixture_sql_bigint(
			execution->exact_simd_proof.scalar_backend_branches,
			context, "exact_scalar_backend_branches"),
		execution->packed_paper_proof_present,
		std::string{
			bioseq::indexing::packed_paper_nthash_index_status_name(
				execution->packed_paper_status)},
		paper_accepted,
		fixture_sql_bigint(
			execution->packed_paper_proof.packed_decode_batches,
			context, "paper_packed_decode_batches"),
		fixture_sql_bigint(
			execution->packed_paper_proof.fallback_calls, context,
			"paper_fallback_calls"),
		fixture_sql_bigint(
			execution->vulkan_proof.completed_roundtrips, context,
			"vulkan_completed_roundtrips"),
		fixture_sql_bigint(
			execution->vulkan_proof.downloaded_minimizers, context,
			"vulkan_downloaded_minimizers"),
		fixture_sql_bigint(
			execution->replay_scan_count, context, "replay_scan_count"),
		fixture_sql_bigint(
			execution->replay_merge_pass_count, context,
			"replay_merge_pass_count"),
		fixture_sql_bigint(
			execution->replay_reroute_pass_count, context,
			"replay_reroute_pass_count"),
		fixture_sql_bigint(
			execution->replay_completed_scan_count, context,
			"replay_completed_scan_count"),
		fixture_sql_bigint(
			execution->replay_failed_scan_count, context,
			"replay_failed_scan_count"),
		fixture_sql_bigint(
			execution->replay_binding_checked_failed_scan_count,
			context, "replay_binding_checked_failed_scan_count"),
		fixture_sql_bigint(
			execution->replay_illegal_span_transition_count, context,
			"replay_illegal_span_transition_count"),
		fixture_sql_bigint(
			execution->replay_first_completed_event_count, context,
			"replay_first_completed_event_count"),
		fixture_sql_bigint(
			execution->replay_last_completed_event_count, context,
			"replay_last_completed_event_count"),
		fixture_sql_bigint(
			execution->replay_final_span_bp, context,
			"replay_final_span_bp"),
		fixture_sql_bigint(
			execution->replay_final_storage_binding.generation_token,
			context, "replay_final_generation_token"),
		fixture_sql_bigint(
			execution->replay_final_storage_binding.mutation_version,
			context, "replay_final_mutation_version"),
		fixture_sql_bigint(
			execution->replay_visited_event_count, context,
			"replay_visited_event_count"),
		fixture_sql_bigint(
			execution->manifest_position_count, context,
			"evidence_manifest_position_count"),
		fixture_sql_bigint(
			execution->manifest_position_segment_span_bp, context,
			"evidence_manifest_position_segment_span_bp"),
		fixture_sql_bigint(
			publication.mps_spill_generation_token(), context,
			"release_generation_token"),
		fixture_sql_bigint(
			publication.mps_spill_mutation_before_release(), context,
			"release_mutation_before"),
		fixture_sql_bigint(
			publication.mps_spill_mutation_after_release(), context,
			"release_mutation_after"),
		publication.mps_spill_resources_released(),
	});
	return bioseq::pg_utils::returns_table<
		bioseq::pg::sql_rows::build_ready_mps_generation_row>{
			std::move(rows)};
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
std::int64_t index_distinct_sub_keys(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name,
	std::int64_t query_key) {
	return bioseq::storage::indexing::
		index_distinct_sub_keys_impl(rel, name, query_key);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void drop_all_index_tables() {
	bioseq::storage::indexing::drop_all_index_tables_impl();
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void set_index_policy(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name,
	std::string policy) {
	const int status = bioseq::storage::indexing::
		set_index_policy_impl(rel, name, policy == "UPDATE_ON_INSERT");
	if (status == 1) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format("unknown index '{}'", name)});
	}
	if (status == 2) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format(
				"index '{}' cannot be UPDATE_ON_INSERT", name)});
	}
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void set_active_read_index(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name) {
	const int status = bioseq::storage::indexing::
		set_active_read_index_impl(rel, name);
	if (status == 1) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format("unknown index '{}'", name)});
	}
	if (status == 2) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"a CHUNKED_PER_TABLE index cannot be the active read index"});
	}
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
std::int64_t column_row_number(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::tid,
	})]]
	bioseq::pg_utils::sql_tid ctid) {
	if (!IsBioseqColumnTableAmTable(rel)) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_WRONG_OBJECT_TYPE,
			{.message = "relation is not a USING bioseq table"});
	}
	ItemPointerData tid{};
	ItemPointerSet(
		&tid,
		static_cast<BlockNumber>(ctid.block_number),
		static_cast<OffsetNumber>(ctid.offset_number));
	return static_cast<std::int64_t>(tid_to_row_number(tid));
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Arms one PostgreSQL error at the selected inline-sequence lookup, read,
 * detoast, or close operation.
 */
void arm_inline_sequence_pg_failure(std::string failure_point) {
	using Failure = bioseq::storage::indexing::InlineSequencePgLeafFailure;
	Failure failure = Failure::none;
	if (failure_point == "relation_lookup") {
		failure = Failure::relation_lookup;
	} else if (failure_point == "tuple_read") {
		failure = Failure::tuple_read;
	} else if (failure_point == "detoast") {
		failure = Failure::detoast;
	} else if (failure_point == "close") {
		failure = Failure::close;
	} else {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{
				.message =
					"unknown inline sequence PostgreSQL failure point",
				.detail = fmt::format(
					"failure_point={}", failure_point),
			});
	}
	bioseq::storage::indexing::
		arm_inline_sequence_pg_leaf_failure(failure);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void write_index_chunk(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name,
	std::int64_t query_key,
	std::int64_t sub_key,
	std::int32_t chunk_no,
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::bytea,
	})]]
	bioseq::pg_utils::sql_bytea payload) {
	const auto payload_datum = bioseq::pg_utils::invoke_pg_as_cpp(
		[&payload] {
			const auto size = payload.bytes().size();
			auto* value = reinterpret_cast<bytea*>(
				palloc(VARHDRSZ + size));
			SET_VARSIZE(value, VARHDRSZ + size);
			if (size != 0) {
				std::memcpy(
					VARDATA(value), payload.bytes().data(), size);
			}
			return PointerGetDatum(value);
		});
	bool wrote = false;
	for (const std::string& prefix :
			bioseq::storage::indexing::seqidx_prefixes_for(rel, name)) {
		auto shards =
			bioseq::storage::indexing::shard_relnames_with_prefix(prefix);
		if (shards.empty()) {
			continue;
		}
		std::string target = shards.front();
		for (const std::string& shard : shards) {
			if (bioseq::storage::indexing::count_in_shard(
					shard,
					fmt::format(
						"WHERE query_key={}",
						static_cast<long long>(query_key))) > 0) {
				target = shard;
				break;
			}
		}
		bioseq::pg_utils::scoped_spi_session spi;
		if (spi.connect() != SPI_OK_CONNECT) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_CONNECTION_FAILURE,
				{.message = "could not connect to SPI"});
		}
		const std::string quoted_target =
			bioseq::pg_utils::invoke_pg_as_cpp([&target] {
				return std::string{quote_identifier(target.c_str())};
			});
		const std::string sql = fmt::format(
			"INSERT INTO bioseq_internal.{} "
			"(query_key, sub_key, chunk_no, chunk_bytes, blob) "
			"VALUES ($1,$2,$3,octet_length($4),$4) "
			"ON CONFLICT (query_key, sub_key, chunk_no) DO UPDATE "
			"SET chunk_bytes=EXCLUDED.chunk_bytes, blob=EXCLUDED.blob",
			quoted_target);
		Oid argument_types[4] = {
			INT8OID, INT8OID, INT4OID, BYTEAOID};
		Datum arguments[4] = {
			Int64GetDatum(query_key),
			Int64GetDatum(sub_key),
			Int32GetDatum(chunk_no),
			payload_datum,
		};
		const int status = bioseq::pg_utils::invoke_pg_as_cpp(
			[&sql, &argument_types, &arguments] {
				return SPI_execute_with_args(
					sql.c_str(), 4, argument_types, arguments,
					nullptr, false, 0);
			});
		if (status < 0) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{.message = "could not write index chunk"});
		}
		if (spi.finish() != SPI_OK_FINISH) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_CONNECTION_FAILURE,
				{.message = "could not finish SPI"});
		}
		wrote = true;
		break;
	}
	if (!wrote) {
		const std::string relation_name =
			bioseq::pg_utils::invoke_pg_as_cpp([rel] {
				const char* value = get_rel_name(rel);
				return value == nullptr
					? std::string{"<missing>"}
					: std::string{value};
			});
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_UNDEFINED_TABLE,
			{.message = fmt::format(
				"no seqidx shard for ({}, {})",
				relation_name, name)});
	}
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void delete_index_manifest(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name,
	std::int64_t query_key) {
	const auto directory_sub_key =
		static_cast<std::int64_t>(bioseq::indexing::DIRECTORY_SUB_KEY);
	for (const std::string& prefix :
			bioseq::storage::indexing::seqidx_prefixes_for(rel, name)) {
		for (const std::string& shard :
				bioseq::storage::indexing::
					shard_relnames_with_prefix(prefix)) {
			bioseq::pg_utils::scoped_spi_session spi;
			if (spi.connect() != SPI_OK_CONNECT) {
				bioseq::pg_utils::raise_sql_error(
					ERRCODE_CONNECTION_FAILURE,
					{.message = "could not connect to SPI"});
			}
			const std::string quoted_shard =
				bioseq::pg_utils::invoke_pg_as_cpp([&shard] {
					return std::string{
						quote_identifier(shard.c_str())};
				});
			const std::string sql = fmt::format(
				"DELETE FROM bioseq_internal.{} "
				"WHERE query_key={} AND sub_key={}",
				quoted_shard,
				static_cast<long long>(query_key),
				static_cast<long long>(directory_sub_key));
			const int status = bioseq::pg_utils::invoke_pg_as_cpp(
				[&sql] {
					return SPI_execute(sql.c_str(), false, 0);
				});
			if (status < 0) {
				bioseq::pg_utils::raise_sql_error(
					ERRCODE_INTERNAL_ERROR,
					{.message = "could not delete index manifest"});
			}
			if (spi.finish() != SPI_OK_FINISH) {
				bioseq::pg_utils::raise_sql_error(
					ERRCODE_CONNECTION_FAILURE,
					{.message = "could not finish SPI"});
			}
		}
	}
}

} // namespace bioseq::pg::sql_routines

namespace {

[[noreturn]] static void sequence_source_diagnostics_unavailable(
		const std::string_view feature) {
	bioseq::pg_utils::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{.message = fmt::format("{} requires a test build", feature)});
}

#if defined(BIOSEQ_TEST_INDEXES)
namespace catalog_runtime_contract {

namespace metadata_handles =
	bioseq::sql::generated::core::
		storage_metadata_core::entity_handles;
namespace indexing_handles =
	bioseq::sql::generated::core::
		storage_indexing_pg_chunked_storage::entity_handles;
namespace manager_handles =
	bioseq::sql::generated::core::
		storage_manager_sequence_storage_manager::entity_handles;

[[nodiscard]] inline std::int64_t parse_integer(
		std::string_view text,
		std::string_view context) {
	std::int64_t value = 0;
	const auto parsed = std::from_chars(
		text.data(), text.data() + text.size(), value);
	if (parsed.ec != std::errc{}
		|| parsed.ptr != text.data() + text.size()) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format(
				"{} requires an integer, got '{}'",
				context,
				text)});
	}
	return value;
}

template<class Handle>
inline void require_oid(
		bioseq::pg_utils::catalog_missing_policy policy,
		Oid expected) {
	const Oid actual = Handle::resolve_oid(policy);
	if (actual != expected) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = fmt::format(
				"catalog handle '{}' resolved OID {}, expected {}",
				Handle::leaf_name(),
				actual,
				expected)});
	}
}

template<class Handle>
inline void require_identity(
		std::string_view leaf,
		std::string_view stable_schema,
		std::string_view installed_schema) {
	const auto installed =
		Handle::installed_schema_name(
			bioseq::pg_utils::catalog_missing_policy::error);
	if (Handle::leaf_name() != leaf
		|| Handle::stable_schema_name() != stable_schema
		|| !installed.has_value()
		|| *installed != installed_schema) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = fmt::format(
				"catalog handle identity mismatch for '{}'",
				leaf)});
	}
}

template<class Callback>
inline bool dispatch_handle(
		std::string_view handle,
		Callback&& callback) {
	if (handle == "row_mask_seq") {
		callback.template operator()<
			std::remove_cvref_t<
				decltype(metadata_handles::row_mask_seq)>>();
		return true;
	}
	if (handle == "storageid_seq") {
		callback.template operator()<
			std::remove_cvref_t<
				decltype(metadata_handles::storageid_seq)>>();
		return true;
	}
	if (handle == "minimizer_generation_id_seq") {
		callback.template operator()<
			std::remove_cvref_t<
				decltype(indexing_handles::
					minimizer_generation_id_seq)>>();
		return true;
	}
	if (handle == "sequence_source_epoch_seq") {
		callback.template operator()<
			std::remove_cvref_t<
				decltype(manager_handles::
					sequence_source_epoch_seq)>>();
		return true;
	}
	return false;
}

inline void resolve_expected(std::string_view payload) {
	const std::size_t first = payload.find(':');
	const std::size_t second = first == std::string_view::npos
		? std::string_view::npos
		: payload.find(':', first + 1);
	if (first == std::string_view::npos
		|| second == std::string_view::npos) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message =
				"catalog handle probe requires "
				"<handle>:<policy>:<expected_oid>"});
	}
	const std::string_view handle = payload.substr(0, first);
	const std::string_view policy_text =
		payload.substr(first + 1, second - first - 1);
	const Oid expected = static_cast<Oid>(parse_integer(
		payload.substr(second + 1),
		"catalog handle expected OID"));
	const auto policy = policy_text == "error"
		? bioseq::pg_utils::catalog_missing_policy::error
		: bioseq::pg_utils::catalog_missing_policy::return_invalid;
	if (policy_text != "error"
		&& policy_text != "return_invalid") {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = "unknown catalog handle missing policy"});
	}
	const bool found = dispatch_handle(
		handle,
		[policy, expected]<class Handle> {
			require_oid<Handle>(policy, expected);
		});
	if (!found) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{.message = fmt::format(
				"unknown catalog handle '{}'", handle)});
	}
}

inline void validate_identities() {
	require_identity<
		std::remove_cvref_t<
			decltype(metadata_handles::row_mask_seq)>>(
				"row_mask_seq",
				"bioseq_internal",
				"bioseq_internal");
	require_identity<
		std::remove_cvref_t<
			decltype(metadata_handles::storageid_seq)>>(
				"storageid_seq",
				"bioseq_internal",
				"bioseq_internal");
	require_identity<
		std::remove_cvref_t<
			decltype(indexing_handles::
				minimizer_generation_id_seq)>>(
				"minimizer_generation_id_seq",
				"bioseq_internal",
				"bioseq_internal");
	require_identity<
		std::remove_cvref_t<
			decltype(manager_handles::
				sequence_source_epoch_seq)>>(
				"sequence_source_epoch_seq",
				"bioseq_internal",
				"bioseq_internal");
}

inline void validate_bpchar_vector_resolver() {
	constexpr std::array<std::pair<Oid, std::string_view>, 6>
		expected{{
			{F_BPCHAREQ, "vbpchareq"},
			{F_BPCHARNE, "vbpcharne"},
			{F_BPCHARGT, "vbpchargt"},
			{F_BPCHARLT, "vbpcharlt"},
			{F_BPCHARLE, "vbpcharle"},
			{F_BPCHARGE, "vbpcharge"},
		}};
	for (const auto& [scalar_oid, expected_name] : expected) {
		const std::optional<Oid> vector_oid =
			GetVectorizedProcedureOid(scalar_oid);
		if (!vector_oid.has_value()) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_UNDEFINED_FUNCTION,
				{.message = fmt::format(
					"no vector BPCHAR owner for OID {}",
					scalar_oid)});
		}
		char* raw_name = bioseq::pg_utils::invoke_pg_as_cpp(
			[resolved_oid = *vector_oid] {
				return get_func_name(resolved_oid);
			});
		const std::string actual_name =
			raw_name == nullptr ? std::string{} : std::string{raw_name};
		if (raw_name != nullptr) {
			bioseq::pg_utils::invoke_pg_as_cpp(
				[raw_name] { pfree(raw_name); });
		}
		if (actual_name != expected_name) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_INTERNAL_ERROR,
				{.message = fmt::format(
					"BPCHAR scalar OID {} resolved '{}', expected '{}'",
					scalar_oid,
					actual_name,
					expected_name)});
		}
	}
}

[[noreturn]] inline void bpchar_physical_contract_failure(
		std::string detail) {
	throw std::runtime_error{
		"BPCHAR physical-lifetime contract failed: "
		+ std::move(detail)};
}

[[nodiscard]] inline
bioseq::pg_utils::runtime_test_counters::bpchar_source_form
classify_bpchar_source(Datum source) noexcept {
	using enum bioseq::pg_utils::runtime_test_counters::
		bpchar_source_form;
	const Pointer raw = DatumGetPointer(source);
	if (raw == nullptr) return unknown;
	if (VARATT_IS_EXTERNAL_ONDISK(raw)) return external;
	if (VARATT_IS_COMPRESSED(raw)) return compressed;
	if (VARATT_IS_4B_U(raw)) return four_byte_uncompressed;
	if (VARATT_IS_SHORT(raw)) return short_header;
	return unknown;
}

[[nodiscard]] inline bool apply_bpchar_relation(
		Oid scalar_oid,
		char left,
		char right) {
	switch (scalar_oid) {
		case F_BPCHAREQ:
			return left == right;
		case F_BPCHARNE:
			return left != right;
		case F_BPCHARGT:
			return left > right;
		case F_BPCHARLT:
			return left < right;
		case F_BPCHARLE:
			return left <= right;
		case F_BPCHARGE:
			return left >= right;
		default:
			bpchar_physical_contract_failure(
				fmt::format(
					"unexpected scalar comparator OID {}",
					scalar_oid));
	}
}

inline void validate_bpchar_vector_call(
		Oid scalar_oid,
		VectorColumn* input,
		Datum constant,
		char normalized_constant,
		std::span<const char> normalized_lanes,
		bool constant_is_left) {
	const std::optional<Oid> vector_oid =
		GetVectorizedProcedureOid(scalar_oid);
	if (!vector_oid.has_value()) {
		bpchar_physical_contract_failure(
			fmt::format(
				"no generated vector owner for scalar OID {}",
				scalar_oid));
	}

	VectorFnArgument column_argument{
		VECTOR_FN_ARG_VAR,
		PointerGetDatum(input)};
	VectorFnArgument constant_argument{
		VECTOR_FN_ARG_CONSTANT,
		constant};
	VectorFnArgument* const left =
		constant_is_left ? &constant_argument : &column_argument;
	VectorFnArgument* const right =
		constant_is_left ? &column_argument : &constant_argument;
	const Datum raw_result = bioseq::pg_utils::invoke_pg_as_cpp(
		[resolved_oid = *vector_oid, left, right] {
			return OidFunctionCall2Coll(
				resolved_oid,
				C_COLLATION_OID,
				PointerGetDatum(left),
				PointerGetDatum(right));
		});
	auto* const result = reinterpret_cast<VectorColumn*>(
		DatumGetPointer(raw_result));
	if (result == nullptr) {
		bpchar_physical_contract_failure(
			"generated vector owner returned a null result");
	}

	std::string failure;
	if (result->dimension != input->dimension
		|| result->dimension != normalized_lanes.size()
		|| result->columnTypeLen != sizeof(bool)
		|| !result->columnIsVal
		|| result->storageKind != VectorStorageKind::inline_fixed
		|| result->value == nullptr) {
		failure = fmt::format(
			"vector result metadata mismatch for scalar OID {}",
			scalar_oid);
	} else {
		const auto* const values =
			reinterpret_cast<const bool*>(result->value);
		for (std::size_t lane = 0;
			 lane < normalized_lanes.size();
			 ++lane) {
			if (result->isnull[lane] != input->isnull[lane]) {
				failure = fmt::format(
					"NULL propagation mismatch for scalar OID {} lane {}",
					scalar_oid,
					lane);
				break;
			}
			if (input->isnull[lane]) {
				if (values[lane]) {
					failure = fmt::format(
						"NULL result lane {} retained a true payload",
						lane);
					break;
				}
				continue;
			}
			const char left_value = constant_is_left
				? normalized_constant
				: normalized_lanes[lane];
			const char right_value = constant_is_left
				? normalized_lanes[lane]
				: normalized_constant;
			const bool expected = apply_bpchar_relation(
				scalar_oid, left_value, right_value);
			if (values[lane] != expected) {
				failure = fmt::format(
					"comparison mismatch for scalar OID {} lane {} "
					"(constant_is_left={})",
					scalar_oid,
					lane,
					constant_is_left);
				break;
			}
		}
	}
	if (result->value != nullptr) {
		pfree(result->value);
	}
	pfree(result);
	if (!failure.empty()) {
		bpchar_physical_contract_failure(std::move(failure));
	}
}

inline void populate_bpchar_vector(
		VectorColumn* column,
		const std::array<Datum, 4>& sources) {
	for (std::size_t lane = 0; lane < sources.size(); ++lane) {
		WriteDatumToVectorColumn(
			column,
			sources[lane],
			false,
			static_cast<std::int32_t>(lane));
	}
	WriteDatumToVectorColumn(
		column,
		static_cast<Datum>(0),
		true,
		static_cast<std::int32_t>(sources.size()));
}

inline void exercise_bpchar_comparison_matrix(
		VectorColumn* column,
		const std::array<Datum, 4>& sources) {
	constexpr std::array<char, 5> normalized_lanes{
		'a', 'b', 'c', 'b', '\0'};
	constexpr std::array<Oid, 6> scalar_oids{
		F_BPCHAREQ,
		F_BPCHARNE,
		F_BPCHARGT,
		F_BPCHARLT,
		F_BPCHARLE,
		F_BPCHARGE};

	for (const Oid scalar_oid : scalar_oids) {
		validate_bpchar_vector_call(
			scalar_oid,
			column,
			sources[3],
			'b',
			normalized_lanes,
			false);
		validate_bpchar_vector_call(
			scalar_oid,
			column,
			sources[3],
			'b',
			normalized_lanes,
			true);
	}

	constexpr std::array<char, 4> normalized_constants{
		'a', 'b', 'c', 'b'};
	for (std::size_t source_index = 0;
		 source_index < sources.size();
		 ++source_index) {
		validate_bpchar_vector_call(
			F_BPCHAREQ,
			column,
			sources[source_index],
			normalized_constants[source_index],
			normalized_lanes,
			false);
	}
}

inline void validate_bpchar_physical_lifetime() {
	const auto value =
		bioseq::pg_utils::runtime_test_counters::
			bpchar_lifetime();
	const std::uint64_t payload_frees =
		value.vector_payload_overwrite_frees
		+ value.vector_payload_cleanup_frees;
	const auto& copies = value.vector_copy_sources;
	const auto& views = value.view_decode_sources;
	const bool complete_copy_forms =
		copies.short_datums != 0
		&& copies.four_byte_uncompressed_datums != 0
		&& copies.compressed_datums != 0
		&& copies.external_datums != 0
		&& copies.unknown_datums == 0;
	const bool complete_view_forms =
		views.short_datums != 0
		&& views.four_byte_uncompressed_datums != 0
		&& views.compressed_datums != 0
		&& views.external_datums != 0
		&& views.unknown_datums == 0;
	if (value.source_observation_active
		|| value.source_observation_sessions != 1
		|| value.source_observation_completions != 1
		|| !complete_copy_forms
		|| !complete_view_forms
		|| value.source_unknown_datums != 0
		|| value.view_constructions == 0
		|| value.view_constructions != value.view_destructions
		|| value.live_views != 0
		|| value.view_owned_copies != value.view_owned_frees
		|| value.view_owned_copies
			!= views.compressed_datums + views.external_datums
		|| value.detoast_temporary_frees
			!= copies.compressed_datums + copies.external_datums
		|| value.vector_payload_allocations != payload_frees
		|| value.vector_payload_overwrite_frees == 0
		|| value.vector_payload_explicit_cleanup_frees == 0
		|| value.vector_payload_clear_frees == 0
		|| value.vector_payload_release_frees == 0
		|| value.explicit_cleanup_calls < 2
		|| value.slot_clear_calls < 2
		|| value.slot_release_calls < 2
		|| value.context_resets != 1
		|| value.context_reuses != 1
		|| value.payload_frees_while_view_live != 0) {
		bpchar_physical_contract_failure(fmt::format(
			"sessions={}/{}, active={}, "
			"copy_forms={}/{}/{}/{}/{}, "
			"view_forms={}/{}/{}/{}/{}, "
			"views={}/{}, owned={}/{}, live={}, "
			"detoast={}, payloads={}/{}, "
			"overwrite={}, cleanup={}/{}/{}, calls={}/{}/{}, "
			"context={}/{}, live_free={}",
			value.source_observation_sessions,
			value.source_observation_completions,
			value.source_observation_active,
			copies.short_datums,
			copies.four_byte_uncompressed_datums,
			copies.compressed_datums,
			copies.external_datums,
			copies.unknown_datums,
			views.short_datums,
			views.four_byte_uncompressed_datums,
			views.compressed_datums,
			views.external_datums,
			views.unknown_datums,
			value.view_constructions,
			value.view_destructions,
			value.view_owned_copies,
			value.view_owned_frees,
			value.live_views,
			value.detoast_temporary_frees,
			value.vector_payload_allocations,
			payload_frees,
			value.vector_payload_overwrite_frees,
			value.vector_payload_explicit_cleanup_frees,
			value.vector_payload_clear_frees,
			value.vector_payload_release_frees,
			value.explicit_cleanup_calls,
			value.slot_clear_calls,
			value.slot_release_calls,
			value.context_resets,
			value.context_reuses,
			value.payload_frees_while_view_live));
	}
}

inline void exercise_bpchar_physical_lifetime(Oid relation_oid) {
	using bioseq::pg_utils::runtime_test_counters::
		bpchar_source_form;
	namespace counters =
		bioseq::pg_utils::runtime_test_counters;

	counters::reset_bpchar_lifetime();
	counters::begin_bpchar_source_observation();

	MemoryContext const parent_context = CurrentMemoryContext;
	MemoryContext probe_context = nullptr;
	Relation relation = nullptr;
	SysScanDesc scan = nullptr;
	VectorTupleTableSlot* vector_slot = nullptr;
	bool in_probe_context = false;

	const auto switch_to_parent = [
			parent_context,
			&in_probe_context] {
		if (in_probe_context) {
			MemoryContextSwitchTo(parent_context);
			in_probe_context = false;
		}
	};
	const auto drop_vector_slot = [&vector_slot] {
		if (vector_slot != nullptr) {
			TupleTableSlot* const slot =
				reinterpret_cast<TupleTableSlot*>(vector_slot);
			bioseq::pg_utils::invoke_pg_as_cpp(
				[slot] { ExecDropSingleTupleTableSlot(slot); });
			vector_slot = nullptr;
		}
	};
	const auto end_scan = [&scan] {
		if (scan != nullptr) {
			bioseq::pg_utils::invoke_pg_as_cpp(
				[active_scan = scan] {
					systable_endscan(active_scan);
				});
			scan = nullptr;
		}
	};
	const auto close_relation = [&relation] {
		if (relation != nullptr) {
			bioseq::pg_utils::invoke_pg_as_cpp(
				[open_relation = relation] {
					table_close(
						open_relation,
						AccessShareLock);
				});
			relation = nullptr;
		}
	};

	try {
		probe_context = bioseq::pg_utils::invoke_pg_as_cpp(
			[parent_context] {
				return AllocSetContextCreate(
					parent_context,
					"doc0388 BPCHAR physical lifetime",
					ALLOCSET_SMALL_SIZES);
			});
		MemoryContextSwitchTo(probe_context);
		in_probe_context = true;

		relation = bioseq::pg_utils::invoke_pg_as_cpp(
			[relation_oid] {
				return table_open(
					relation_oid,
					AccessShareLock);
			});
		TupleDesc const descriptor = RelationGetDescr(relation);
		if (relation->rd_rel->relkind != RELKIND_RELATION
			|| descriptor == nullptr
			|| descriptor->natts != 2
			|| TupleDescAttr(descriptor, 0)->atttypid != INT4OID
			|| TupleDescAttr(descriptor, 1)->atttypid != BPCHAROID) {
			bpchar_physical_contract_failure(
				"probe relation must be a two-column "
				"(integer, bpchar) heap table");
		}

		scan = bioseq::pg_utils::invoke_pg_as_cpp(
			[relation] {
				return systable_beginscan(
					relation,
					InvalidOid,
					false,
					SnapshotSelf,
					0,
					nullptr);
			});
		std::array<Datum, 4> sources{};
		std::array<bool, 4> source_seen{};
		bool null_seen = false;
		std::size_t row_count = 0;
		while (true) {
			HeapTuple const tuple =
				bioseq::pg_utils::invoke_pg_as_cpp(
					[active_scan = scan] {
						return systable_getnext(active_scan);
					});
			if (!HeapTupleIsValid(tuple)) break;
			++row_count;
			bool id_is_null = false;
			bool value_is_null = false;
			const Datum id_datum = heap_getattr(
				tuple, 1, descriptor, &id_is_null);
			const Datum value = heap_getattr(
				tuple, 2, descriptor, &value_is_null);
			if (id_is_null) {
				bpchar_physical_contract_failure(
					"probe relation contains a NULL row id");
			}
			const std::int32_t id = DatumGetInt32(id_datum);
			if (id == 5) {
				if (!value_is_null || null_seen) {
					bpchar_physical_contract_failure(
						"probe NULL row is missing or duplicated");
				}
				null_seen = true;
				continue;
			}
			if (id < 1 || id > 4 || value_is_null) {
				bpchar_physical_contract_failure(
					fmt::format(
						"unexpected probe row id {} or NULL value",
						id));
			}
			const std::size_t source_index =
				static_cast<std::size_t>(id - 1);
			if (source_seen[source_index]) {
				bpchar_physical_contract_failure(
					fmt::format(
						"duplicate probe row id {}",
						id));
			}
			constexpr std::array<
				bpchar_source_form, 4> expected_forms{
					bpchar_source_form::short_header,
					bpchar_source_form::four_byte_uncompressed,
					bpchar_source_form::compressed,
					bpchar_source_form::external};
			if (classify_bpchar_source(value)
				!= expected_forms[source_index]) {
				bpchar_physical_contract_failure(
					fmt::format(
						"row {} has the wrong physical varlena form",
						id));
			}
			sources[source_index] =
				bioseq::pg_utils::invoke_pg_as_cpp(
					[value] {
						return datumCopy(value, false, -1);
					});
			source_seen[source_index] = true;
		}
		end_scan();
		if (row_count != 5
			|| !null_seen
			|| std::ranges::find(source_seen, false)
				!= source_seen.end()) {
			bpchar_physical_contract_failure(
				fmt::format(
					"probe relation shape mismatch: rows={}, "
					"null_seen={}, forms={}/{}/{}/{}",
					row_count,
					null_seen,
					source_seen[0],
					source_seen[1],
					source_seen[2],
					source_seen[3]));
		}

		vector_slot = reinterpret_cast<VectorTupleTableSlot*>(
			bioseq::pg_utils::invoke_pg_as_cpp(
				[descriptor] {
					return CreateVectorTupleTableSlot(descriptor);
				}));
		auto* column = reinterpret_cast<VectorColumn*>(
			DatumGetPointer(
				vector_slot->tts.tts_values[1]));
		populate_bpchar_vector(column, sources);
		WriteDatumToVectorColumn(
			column, sources[0], false, 0);
		exercise_bpchar_comparison_matrix(column, sources);

		TupleTableSlot* const slot =
			reinterpret_cast<TupleTableSlot*>(vector_slot);
		bioseq::pg_utils::invoke_pg_as_cpp(
			[slot] { ExecClearTuple(slot); });
		bioseq::pg_utils::invoke_pg_as_cpp(
			[slot] { ExecClearTuple(slot); });
		if (column->dimension != 0) {
			bpchar_physical_contract_failure(
				"slot clear did not reset the BPCHAR vector");
		}
		populate_bpchar_vector(column, sources);
		constexpr std::array<char, 5> normalized_lanes{
			'a', 'b', 'c', 'b', '\0'};
		validate_bpchar_vector_call(
			F_BPCHAREQ,
			column,
			sources[1],
			'b',
			normalized_lanes,
			false);
		CleanupVectorSlot(vector_slot);
		CleanupVectorSlot(vector_slot);
		if (column->dimension != 0) {
			bpchar_physical_contract_failure(
				"explicit cleanup did not reset the BPCHAR vector");
		}
		populate_bpchar_vector(column, sources);
		drop_vector_slot();

		switch_to_parent();
		bioseq::pg_utils::invoke_pg_as_cpp(
			[probe_context] {
				MemoryContextReset(probe_context);
			});
		counters::note_bpchar_context_reset();
		MemoryContextSwitchTo(probe_context);
		in_probe_context = true;
		counters::note_bpchar_context_reuse();

		scan = bioseq::pg_utils::invoke_pg_as_cpp(
			[relation] {
				return systable_beginscan(
					relation,
					InvalidOid,
					false,
					SnapshotSelf,
					0,
					nullptr);
			});
		Datum reused_source = static_cast<Datum>(0);
		while (true) {
			HeapTuple const tuple =
				bioseq::pg_utils::invoke_pg_as_cpp(
					[active_scan = scan] {
						return systable_getnext(active_scan);
					});
			if (!HeapTupleIsValid(tuple)) break;
			bool id_is_null = false;
			bool value_is_null = false;
			const Datum id_datum = heap_getattr(
				tuple, 1, descriptor, &id_is_null);
			if (id_is_null || DatumGetInt32(id_datum) != 1) {
				continue;
			}
			reused_source = heap_getattr(
				tuple, 2, descriptor, &value_is_null);
			if (value_is_null
				|| classify_bpchar_source(reused_source)
					!= bpchar_source_form::short_header) {
				bpchar_physical_contract_failure(
					"context-reuse source is not short BPCHAR");
			}
			break;
		}
		if (DatumGetPointer(reused_source) == nullptr) {
			bpchar_physical_contract_failure(
				"context-reuse source row is missing");
		}
		vector_slot = reinterpret_cast<VectorTupleTableSlot*>(
			bioseq::pg_utils::invoke_pg_as_cpp(
				[descriptor] {
					return CreateVectorTupleTableSlot(descriptor);
				}));
		column = reinterpret_cast<VectorColumn*>(
			DatumGetPointer(
				vector_slot->tts.tts_values[1]));
		WriteDatumToVectorColumn(
			column, reused_source, false, 0);
		constexpr std::array<char, 1> reused_lane{'a'};
		validate_bpchar_vector_call(
			F_BPCHAREQ,
			column,
			reused_source,
			'a',
			reused_lane,
			false);
		end_scan();
		drop_vector_slot();

		switch_to_parent();
		close_relation();
		counters::end_bpchar_source_observation();
		bioseq::pg_utils::invoke_pg_as_cpp(
			[probe_context] {
				MemoryContextDelete(probe_context);
			});
		probe_context = nullptr;
		validate_bpchar_physical_lifetime();
	} catch (...) {
		switch_to_parent();
		drop_vector_slot();
		end_scan();
		close_relation();
		counters::end_bpchar_source_observation();
		if (probe_context != nullptr) {
			bioseq::pg_utils::invoke_pg_as_cpp(
				[probe_context] {
					MemoryContextDelete(probe_context);
				});
		}
		throw;
	}
}

inline void validate_bpchar_lifetime() {
	const auto value =
		bioseq::pg_utils::runtime_test_counters::
			bpchar_lifetime();
	const std::uint64_t payload_frees =
		value.vector_payload_overwrite_frees
		+ value.vector_payload_cleanup_frees;
	if (value.view_constructions == 0
		|| value.vector_payload_allocations == 0
		|| value.live_views != 0
		|| value.view_constructions != value.view_destructions
		|| value.view_owned_copies != value.view_owned_frees
		|| value.vector_payload_allocations != payload_frees
		|| value.payload_frees_while_view_live != 0) {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{.message = fmt::format(
				"BPCHAR lifetime contract failed: "
				"views={}/{}, live={}, owned={}/{}, "
				"payloads={}/{}, live_free={}",
				value.view_constructions,
				value.view_destructions,
				value.live_views,
				value.view_owned_copies,
				value.view_owned_frees,
				value.vector_payload_allocations,
				payload_frees,
				value.payload_frees_while_view_live)});
	}
}

}  // namespace catalog_runtime_contract
#endif

} // namespace

export namespace bioseq::pg::sql_routines {

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void arm_sequence_source_failure(std::string failure_point) {
#ifdef BIOSEQ_TEST_INDEXES
	constexpr std::string_view catalog_barrier_prefix{
		"catalog_handle_barrier:"};
	constexpr std::string_view reaper_barrier_prefix{
		"orphan_reaper_barrier:"};
	constexpr std::string_view catalog_resolve_prefix{
		"catalog_handle_resolve:"};
	constexpr std::string_view bpchar_physical_prefix{
		"bpchar_physical_probe:"};
	if (failure_point.starts_with(catalog_barrier_prefix)) {
		bioseq::pg_utils::catalog_entity_test_support::
			arm_lookup_barrier(
				catalog_runtime_contract::parse_integer(
					std::string_view{failure_point}.substr(
						catalog_barrier_prefix.size()),
					"catalog handle barrier"));
		return;
	}
	if (failure_point.starts_with(reaper_barrier_prefix)) {
		bioseq::storage::orphan_reaper_test_support::
			arm_drop_barrier(
				catalog_runtime_contract::parse_integer(
					std::string_view{failure_point}.substr(
						reaper_barrier_prefix.size()),
					"orphan reaper barrier"));
		return;
	}
	if (failure_point.starts_with(catalog_resolve_prefix)) {
		catalog_runtime_contract::resolve_expected(
			std::string_view{failure_point}.substr(
				catalog_resolve_prefix.size()));
		return;
	}
	if (failure_point.starts_with(bpchar_physical_prefix)) {
		const std::int64_t parsed_relation =
			catalog_runtime_contract::parse_integer(
				std::string_view{failure_point}.substr(
					bpchar_physical_prefix.size()),
				"BPCHAR physical probe relation");
		if (parsed_relation <= 0
			|| static_cast<std::uint64_t>(parsed_relation)
				> std::numeric_limits<Oid>::max()) {
			bioseq::pg_utils::raise_sql_error(
				ERRCODE_INVALID_PARAMETER_VALUE,
				{.message =
					"BPCHAR physical probe relation OID is out of range"});
		}
		catalog_runtime_contract::
			exercise_bpchar_physical_lifetime(
				static_cast<Oid>(parsed_relation));
		return;
	}
	if (failure_point == "catalog_handle_identities") {
		catalog_runtime_contract::validate_identities();
		return;
	}
	if (failure_point == "bpchar_vector_resolver") {
		catalog_runtime_contract::validate_bpchar_vector_resolver();
		return;
	}
	if (failure_point == "bpchar_lifetime_reset") {
		bioseq::pg_utils::runtime_test_counters::
			reset_bpchar_lifetime();
		return;
	}
	if (failure_point == "bpchar_lifetime_validate") {
		catalog_runtime_contract::validate_bpchar_lifetime();
		return;
	}
	if (failure_point == "aggregate_owner_visits_reset") {
		bioseq::vector::aggregate_sql_routines::
			reset_runtime_owner_visits();
		return;
	}
	if (failure_point == "aggregate_owner_visits_validate") {
		bioseq::vector::aggregate_sql_routines::
			validate_runtime_owner_visits();
		return;
	}
	if (failure_point == "aggregate_int8_serial_roundtrip") {
		bioseq::vector::aggregate_sql_routines::
			validate_int8_serial_roundtrip();
		return;
	}
	if (failure_point == "metadata_age_log_context_stress") {
		const std::string index_name(1024, 'm');
		for (std::uint32_t iteration = 0;
			 iteration != 512;
			 ++iteration) {
			static_cast<void>(
				bioseq::storage::indexing::
					upsert_metadata_age_log_pg(
						static_cast<Oid>(388079),
						static_cast<Oid>(388079),
						static_cast<std::int16_t>(1),
						index_name));
		}
		const std::uint64_t deleted =
			bioseq::storage::indexing::
				delete_metadata_age_log_pg(
					static_cast<Oid>(388079),
					static_cast<Oid>(388079),
					static_cast<std::int16_t>(1),
					index_name);
		if (deleted != 1) {
			throw std::runtime_error{
				"metadata_age_log stress row was not deleted"};
		}
		return;
	}
	constexpr std::string_view row_codec_all_prefix{
		"row_codec_all:"};
	if (failure_point.starts_with(row_codec_all_prefix)) {
		bioseq::pg_utils::row_codec_runtime_test_support::
			exercise_all(
				std::string_view{failure_point}.substr(
					row_codec_all_prefix.size()));
		return;
	}
	if (failure_point == "row_codec_reaper_text_bridge_error") {
		bioseq::pg_utils::row_codec_runtime_test_support::
			exercise_reaper_text_bridge_error();
		return;
	}
	if (failure_point == "diagnostic_std_exception") {
		bioseq::pg_utils::raise_sql_error(
			ERRCODE_INTERNAL_ERROR,
			{
				.message =
					"bioseq sequence callback raised a C++ exception",
				.detail =
					"injected DOC0377 diagnostic C++ exception",
			});
	}
	if (failure_point == "facade_resident_probe") {
		arm_pg_sequence_facade_test_poison(
			PgSequenceFacadeTestPoison::resident_probe);
		return;
	}
	if (failure_point == "facade_materialization") {
		arm_pg_sequence_facade_test_poison(
			PgSequenceFacadeTestPoison::materialization);
		return;
	}
	if (failure_point == "facade_contiguous_access") {
		arm_pg_sequence_facade_test_poison(
			PgSequenceFacadeTestPoison::contiguous_access);
		return;
	}
	using Failure = bioseq::storage::PgSequenceSourceFailure;
	constexpr std::array<std::pair<std::string_view, Failure>, 14>
		points{{
			{"provider_after_registry_control",
				Failure::provider_after_registry_control},
			{"provider_after_provider_control",
				Failure::provider_after_provider_control},
			{"domain_after_control", Failure::domain_after_control},
			{"domain_after_context", Failure::domain_after_context},
			{"domain_after_owner", Failure::domain_after_owner},
			{"domain_after_snapshot", Failure::domain_after_snapshot},
			{"domain_after_caches", Failure::domain_after_caches},
			{"close_before_caches", Failure::close_before_caches},
			{"close_before_snapshot", Failure::close_before_snapshot},
			{"close_before_before_locks",
				Failure::close_before_before_locks},
			{"close_before_locks", Failure::close_before_locks},
			{"close_before_after_locks",
				Failure::close_before_after_locks},
			{"read_before_scanner", Failure::read_before_scanner},
			{"mmsearch_output_after_promotion",
				Failure::mmsearch_output_after_promotion},
		}};
	for (const auto& [name, failure] : points) {
		if (name == failure_point) {
			bioseq::storage::arm_pg_sequence_source_failure(failure);
			return;
		}
	}
	bioseq::pg_utils::raise_sql_error(
		ERRCODE_INVALID_PARAMETER_VALUE,
		{
			.message =
				"unknown bioseq sequence source-domain failure point",
			.detail = fmt::format(
				"failure_point={}", failure_point),
		});
#else
	(void)failure_point;
	sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain fault injection");
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void disarm_sequence_source_failure() {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::disarm_pg_sequence_source_failure();
#else
	sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain fault injection");
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void reset_sequence_source_counters() {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::disarm_pg_sequence_source_failure();
	bioseq::storage::reset_pg_sequence_source_debug_counters();
#else
	sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
bioseq::pg_utils::sql_jsonb sequence_source_counters() {
#ifdef BIOSEQ_TEST_INDEXES
	const auto counters =
		bioseq::storage::pg_sequence_source_debug_counters();
	return bioseq::pg_utils::sql_jsonb{fmt::format(
		"{{\"registries_created\":{},\"registries_destroyed\":{},"
		"\"providers_created\":{},\"providers_destroyed\":{},"
		"\"scan_providers_created\":{},\"standalone_providers_created\":{},"
		"\"domains_created\":{},\"domains_reused\":{},"
		"\"domains_destroyed\":{},\"cache_sets_created\":{},"
		"\"cache_sets_destroyed\":{},\"snapshots_registered\":{},"
		"\"snapshots_unregistered\":{},\"scanner_entries_created\":{},"
		"\"scanner_entry_hits\":{},\"leases_acquired\":{},"
		"\"leases_released\":{},\"provider_closed_with_upper_leases\":{},"
		"\"last_upper_lease_after_provider_end\":{},\"cleanup_commit\":{},"
		"\"cleanup_abort\":{},\"sweeps\":{},\"swept_domains\":{},"
		"\"subxact_promotions\":{},\"subxact_aborts\":{},"
		"\"pre_prepare_rejections\":{},\"release_before_locks\":{},"
		"\"release_locks\":{},\"release_after_locks\":{},"
		"\"wrong_thread_rejections\":{},\"refcount_faults\":{},"
		"\"lease_faults\":{},\"fault_injections\":{},"
		"\"read_fault_injections\":{},"
		"\"mmsearch_output_context_resets\":{},"
		"\"mmsearch_output_locator_promotions\":{},"
		"\"mmsearch_output_pg_error_restores\":{},"
		"\"mmsearch_output_restore_mismatches\":{},"
		"\"facade_source_stream_callbacks\":{},"
		"\"facade_source_resident_probes\":{},"
		"\"facade_declared_materializations\":{},"
		"\"facade_completed_materializations\":{},"
		"\"facade_poison_hits\":{},"
		"\"live_registries\":{},\"live_providers\":{},"
		"\"live_domains\":{},\"live_leases\":{},"
		"\"live_cache_sets\":{},\"live_snapshots\":{}}}",
		counters.registries_created,
		counters.registries_destroyed,
		counters.providers_created,
		counters.providers_destroyed,
		counters.scan_providers_created,
		counters.standalone_providers_created,
		counters.domains_created,
		counters.domains_reused,
		counters.domains_destroyed,
		counters.cache_sets_created,
		counters.cache_sets_destroyed,
		counters.snapshots_registered,
		counters.snapshots_unregistered,
		counters.scanner_entries_created,
		counters.scanner_entry_hits,
		counters.leases_acquired,
		counters.leases_released,
		counters.provider_closed_with_upper_leases,
		counters.last_upper_lease_after_provider_end,
		counters.cleanup_commit,
		counters.cleanup_abort,
		counters.sweeps,
		counters.swept_domains,
		counters.subxact_promotions,
		counters.subxact_aborts,
		counters.pre_prepare_rejections,
		counters.release_before_locks,
		counters.release_locks,
		counters.release_after_locks,
		counters.wrong_thread_rejections,
		counters.refcount_faults,
		counters.lease_faults,
		counters.fault_injections,
		counters.read_fault_injections,
		counters.mmsearch_output_context_resets,
		counters.mmsearch_output_locator_promotions,
		counters.mmsearch_output_pg_error_restores,
		counters.mmsearch_output_restore_mismatches,
		counters.facade_source_stream_callbacks,
		counters.facade_source_resident_probes,
		counters.facade_declared_materializations,
		counters.facade_completed_materializations,
		counters.facade_poison_hits,
		counters.live_registries,
		counters.live_providers,
		counters.live_domains,
		counters.live_leases,
		counters.live_cache_sets,
		counters.live_snapshots)};
#else
	sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Arms one SliceProject DSM payload as incomplete, malformed, or absent for worker-state validation.
 */
void arm_slice_dsm_fixture(std::string mode) {
	bioseq_arm_slice_dsm_fixture_for_test(mode);
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool sequence_source_wrong_thread_probe() {
#ifdef BIOSEQ_TEST_INDEXES
	return bioseq::storage::pg_sequence_source_wrong_thread_probe();
#else
	sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool sequence_source_fault_contract_probe() {
#ifdef BIOSEQ_TEST_INDEXES
	return bioseq::storage::pg_sequence_source_fault_contract_probe();
#else
	sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool sequence_source_route_alias_probe() {
#ifdef BIOSEQ_TEST_INDEXES
	return bioseq::storage::pg_sequence_source_route_alias_probe();
#else
	sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
std::optional<bool> sequence_facade_orientation_probe(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::custom_type<"bioseq.seq">,
	})]]
	std::optional<Datum> sequence) {
#ifdef BIOSEQ_TEST_INDEXES
	if (!sequence.has_value()) {
		return std::nullopt;
	}
	return pg_sequence_facade_orientation_contract_probe(*sequence);
#else
	(void)sequence;
	sequence_source_diagnostics_unavailable(
		"bioseq sequence facade diagnostics");
#endif
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void sequence_source_pre_prepare_probe() {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::pg_sequence_source_pre_prepare_probe();
#else
	sequence_source_diagnostics_unavailable(
		"bioseq sequence source-domain diagnostics");
#endif
}

}  // namespace bioseq::pg::sql_routines
