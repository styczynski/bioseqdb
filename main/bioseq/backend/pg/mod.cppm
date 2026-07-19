/*-------------------------------------------------------------------------
 *
 * mod.cppm
 *
 * Extension entry points: PG_MODULE_MAGIC, _PG_init and the SQL-level C
 * functions exported to the bioseq extension script (mmsearch,
 * mmsearch_support). The C functions are defined with extern "C" linkage
 * so PostgreSQL's fmgr can call them directly.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"
#include <hwy/highway.h>


extern "C" {
#include "executor/executor.h"
#include "funcapi.h"
#include "utils/tuplestore.h"
#include "utils/timestamp.h"
#include "utils/builtins.h"
#include "utils/jsonb.h"
#include "utils/lsyscache.h"
#include "catalog/pg_type.h"
#include "lib/stringinfo.h"
}

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

export module bioseq.mod;

import bioseq.common.mlogging;
import bioseq.column;
import bioseq.pg_utils;
import bioseq.types.pg_compact_sequence;
import bioseq.sequences;
import bioseq.storage.manager;                   // is_locator_stub, decode_locator_from_stub
import bioseq.storage.scanner_pg;                // DOC0377 source-domain test diagnostics
import bioseq.storage.lazy;              // make_lazy_in_memory
import bioseq.indexing.type;             // DIRECTORY_SUB_KEY, index_sub_key_t
import bioseq.indexing.minimizer_hashmap;  // sql_bigint_can_represent
import bioseq.indexing.minimizer_sequence; // MPS generation execution evidence
import bioseq.indexing.symbol_counts; // DOC0279 §12.5 — tally_symbol_counts, SymbolCountsIndex
import bioseq.storage.indexing_pg.admin; // DOC0279 §11.4/§12 — update_index_impl, list helpers, letter_to_rank
import bioseq.storage.indexing_pg.chunked_storage; // DOC0279 §8.8 — g_last_index_merge_run_count, format_seqidx helpers
import bioseq.storage.indexing_pg.handles;
import bioseq.scheduler.supervisor_pg;
import bioseq.scheduler.dispatch_processor;
import bioseq.gpu.vulkan.runtime;
import bioseq.parallelism.mode;
import bioseq.common.sequan_compat;
import bioseq.column.tam; // DOC0322 §7.4 — tid_to_row_number, IsBioseqColumnTableAmTable (the leaf replaces the former extern "C++" reaches)
import bioseq.pg_utils.nodes; // DOC0333 — pg_node_try_as (support-request dispatch), expression walker adapter (find_first_var)
import bioseq.table.scan_state;
import bioseq.sequences.pipe;

/* The split compact-sequence module keeps the C entrypoint bodies in its
 * sql_entrypoints partition.  Keep fmgr V1 metadata centralized in the
 * extension module so every symbol named by bioseq--1.0.sql is exported. */
extern "C" {
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_typmod_in);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_typmod_out);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_in);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_out);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_recv);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_send);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cast);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_cmp);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_lt);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_le);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_eq);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ne);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_ge);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_gt);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_len);
CXX_PG_FUNCTION_INFO_V1(pg_compact_sequence_unit_len);
CXX_PG_FUNCTION_INFO_V1(seq_hash);
CXX_PG_FUNCTION_INFO_V1(seq_hash_extended);
CXX_PG_FUNCTION_INFO_V1(slice_trace_self_test);
CXX_PG_FUNCTION_INFO_V1(synthetic_lazy_sequence);
CXX_PG_FUNCTION_INFO_V1(sequence_piece_count);
CXX_PG_FUNCTION_INFO_V1(piece_tree_self_test);
CXX_PG_FUNCTION_INFO_V1(expanded_ownership_self_test);

CXX_PG_FUNCTION_INFO_V1(letter_typmod_in);
CXX_PG_FUNCTION_INFO_V1(letter_typmod_out);
CXX_PG_FUNCTION_INFO_V1(letter_in);
CXX_PG_FUNCTION_INFO_V1(letter_out);
CXX_PG_FUNCTION_INFO_V1(letter_recv);
CXX_PG_FUNCTION_INFO_V1(letter_send);
CXX_PG_FUNCTION_INFO_V1(letter_wire_self_test);
CXX_PG_FUNCTION_INFO_V1(letter_typmod_cast);
CXX_PG_FUNCTION_INFO_V1(letter_to_text);
CXX_PG_FUNCTION_INFO_V1(letter_cmp);
CXX_PG_FUNCTION_INFO_V1(letter_eq);
CXX_PG_FUNCTION_INFO_V1(letter_ne);
CXX_PG_FUNCTION_INFO_V1(letter_lt);
CXX_PG_FUNCTION_INFO_V1(letter_le);
CXX_PG_FUNCTION_INFO_V1(letter_gt);
CXX_PG_FUNCTION_INFO_V1(letter_ge);
CXX_PG_FUNCTION_INFO_V1(letter_hash);
CXX_PG_FUNCTION_INFO_V1(letter_hash_extended);

CXX_PG_FUNCTION_INFO_V1(seq_subscript_handler);
CXX_PG_FUNCTION_INFO_V1(seq_concat);
CXX_PG_FUNCTION_INFO_V1(seq_concat_text);
CXX_PG_FUNCTION_INFO_V1(text_concat_seq);
CXX_PG_FUNCTION_INFO_V1(seq_concat_varchar);
CXX_PG_FUNCTION_INFO_V1(varchar_concat_seq);
CXX_PG_FUNCTION_INFO_V1(seq_concat_text_array);
CXX_PG_FUNCTION_INFO_V1(text_array_concat_seq);
CXX_PG_FUNCTION_INFO_V1(seq_concat_letter_array);
CXX_PG_FUNCTION_INFO_V1(letter_array_concat_seq);
CXX_PG_FUNCTION_INFO_V1(seq_reverse);
CXX_PG_FUNCTION_INFO_V1(seq_reverse_complement);
CXX_PG_FUNCTION_INFO_V1(seq_array_length);
CXX_PG_FUNCTION_INFO_V1(seq_cardinality);
CXX_PG_FUNCTION_INFO_V1(seq_to_text);
CXX_PG_FUNCTION_INFO_V1(seq_to_varchar);
CXX_PG_FUNCTION_INFO_V1(seq_to_array);
CXX_PG_FUNCTION_INFO_V1(seq_from_letter_array_infer);
CXX_PG_FUNCTION_INFO_V1(seq_from_letter_array_named);
}

using namespace bioseq::pg_utils;   // DOC0333 — node view/walker helpers live in bioseq::pg_utils

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
	if (estate == nullptr || alignment_start_in_progress.empty()) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq alignment node began outside ExecutorStart")));
	}
	AlignmentExecutorFrame* frame = alignment_start_in_progress.back();
	if (frame == nullptr || frame->query_desc == nullptr
		|| frame->query_desc->estate != estate) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq alignment node could not bind to its ExecutorStart frame")));
	}
	frame->estate = estate;
	ensure_alignment_frame_cleanup(frame, estate);
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
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq alignment statistic exceeds SQL integer")));
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
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq alignment statistic exceeds SQL bigint")));
		}
		signed_bigint(static_cast<std::int64_t>(value));
	}

	void text(std::string_view value) noexcept
	{
		auto* datum = cstring_to_text_with_len(
			value.data(), static_cast<int>(value.size()));
		row.set_raw(
			next_attnum++,
			bioseq::pg_utils::pg_datum::from_pointer(datum));
	}

	void jsonb(std::string_view encoded)
	{
		const std::string terminated{encoded};
		const Datum datum = DirectFunctionCall1(
			jsonb_in, CStringGetDatum(terminated.c_str()));
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
			ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("bioseq alignment SRF column mapping is incomplete"),
				 errdetail("mapped=%d expected=%d",
					next_attnum - 1, ColumnCount)));
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
			ereport(ERROR,
				(errcode(ERRCODE_DATA_EXCEPTION),
				 errmsg("bioseq alignment snapshot has no valid environment backend")));
		}
		return environment_backend;
	}
	if (!bioseq::pipeline_stats::environment_is_valid_for_backend(
			stats.environment, environment_backend)) {
		ereport(ERROR,
			(errcode(ERRCODE_DATA_EXCEPTION),
			 errmsg("bioseq alignment snapshot contains an invalid execution environment"),
			 errdetail("requested_backend=%.*s actual_backend=%.*s workers=%u gpu_present=%s",
				static_cast<int>(bioseq::pipeline_stats::executed_backend_name(
					stats.requested_backend).size()),
				bioseq::pipeline_stats::executed_backend_name(
					stats.requested_backend).data(),
				static_cast<int>(bioseq::pipeline_stats::executed_backend_name(
					stats.actual_backend).size()),
				bioseq::pipeline_stats::executed_backend_name(
					stats.actual_backend).data(),
				stats.environment.effective_cpu_workers,
				stats.environment.gpu_environment_present ? "true" : "false")));
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
				ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("bioseq GPU driver identity is invalid")));
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
				ereport(ERROR,
					(errcode(ERRCODE_DATA_EXCEPTION),
					 errmsg("bioseq GPU pipeline-cache hit proof is invalid")));
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

template<int ColumnCount>
Datum materialize_alignment_execution_stats(
		FunctionCallInfo fcinfo,
		const char* function_name)
{
	static_assert(
		ColumnCount == alignment_stats_v1_column_count
		|| ColumnCount == alignment_stats_v2_column_count);
	ReturnSetInfo* rsinfo = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
	if (rsinfo == nullptr
		|| !pg_node_view{pg_as_node(rsinfo)}.is<ReturnSetInfo>()) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s must be called in a set-returning context", function_name)));
	}
	if ((rsinfo->allowedModes & SFRM_Materialize) == 0) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("materialize mode required for %s", function_name)));
	}
	TupleDesc tuple_desc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tuple_desc)
		!= TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("%s return type is not composite", function_name)));
	}
	if (tuple_desc == nullptr || tuple_desc->natts != ColumnCount) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("%s has an invalid row shape", function_name),
			 errdetail("columns=%d expected=%d",
				tuple_desc == nullptr ? -1 : tuple_desc->natts,
				ColumnCount)));
	}
	if (rsinfo->econtext == nullptr
		|| rsinfo->econtext->ecxt_per_query_memory == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq alignment stats SRF has no per-query memory context")));
	}

	MemoryContext old_context = MemoryContextSwitchTo(
		rsinfo->econtext->ecxt_per_query_memory);
	Tuplestorestate* tuple_store = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tuple_store;
	rsinfo->setDesc = tuple_desc;

	if (last_alignment_execution_snapshot.has_value()) {
		/* The backend-local snapshot is immutable after ExecutorFinish. The
		 * move-only evidence ledger is read repeatedly by const reference. */
		const alignment_exec::AlignmentExecutionSnapshot& snapshot =
			*last_alignment_execution_snapshot;
		(void)validate_alignment_snapshot_environment(snapshot);
		for (std::size_t stage_no = 0;
			 stage_no < bioseq::pipeline_stats::pipeline_stage_count;
			 ++stage_no) {
			auto row = make_alignment_stats_row<ColumnCount>(
				snapshot,
				static_cast<bioseq::pipeline_stats::PipelineStage>(stage_no));
			row.put_values(tuple_store, tuple_desc);
		}
	}
	MemoryContextSwitchTo(old_context);
	return static_cast<Datum>(0);
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
	static bool initialized = false;
	bioseq_register_postgres_logging_sink({
		.should_log_level = bioseq_pg_sink_should_log_level,
		.log_level = bioseq_pg_sink_log_level,
		.should_log_pg = bioseq_pg_should_log_elevel,
		.log_pg = bioseq_pg_sink_log_elevel,
		.report = bioseq_pg_sink_report,
	});
	if (initialized) {
		bioseq_log<DBG, PG>("[bioseq] pg: _PG_init() skipped duplicate call");
		return;
	}
	initialized = true;
	log_configure();
	bioseq_log<DBG, PG>("[bioseq] pg: _PG_init() entered");
	install_alignment_execution_hooks();
	bioseq_init();
}

/*
 * mmsearch is a planner/executor rendezvous. A supported plan removes the
 * exact owned FuncExpr before ExecInitQual; reaching this SQL function means
 * no executor proved ownership and must fail closed.
 */
CXX_PG_FUNCTION_INFO_V1(mmsearch);
Datum mmsearch(PG_FUNCTION_ARGS) {
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("mmsearch qualifier has no Bioseq executor owner"),
		 errdetail("Executing the SQL rendezvous function as a Boolean predicate is forbidden."),
		 errhint("Use exactly one top-level mmsearch predicate with supported operands, or split the query into separate levels.")));
	PG_RETURN_BOOL(false);
}

/*
 * bioseq_score_stub and its siblings back the SQL projection functions
 * bioseq.score / bioseq.dp_score / bioseq.cigar / bioseq.query_start / ...
 * None of them must ever execute in the hot path: the columnar custom
 * scan rewrites the target list to replace every FuncExpr whose funcid
 * matches one of these stubs with a Const we own. If any stub fires
 * here, the custom path was not chosen for the query and the operator
 * sees the sentinel value; we log loudly so the miss is obvious in
 * main/loglog.log.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_score_stub);
Datum bioseq_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_score_stub() UNEXPECTED direct call -- custom scan did not rewrite this call site");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_dp_score_stub);
Datum bioseq_dp_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_dp_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_len_stub);
Datum bioseq_query_len_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_query_len_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_start_stub);
Datum bioseq_query_start_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_query_start_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_query_end_stub);
Datum bioseq_query_end_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_query_end_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_len_stub);
Datum bioseq_target_len_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_target_len_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_start_stub);
Datum bioseq_target_start_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_target_start_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_target_end_stub);
Datum bioseq_target_end_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_target_end_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_num_matches_stub);
Datum bioseq_num_matches_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_num_matches_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_block_length_stub);
Datum bioseq_block_length_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_block_length_stub() UNEXPECTED direct call");
	PG_RETURN_INT64(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_edit_stub);
Datum bioseq_edit_stub(PG_FUNCTION_ARGS) {
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("bioseq.edit requires a bioseq alignment custom scan"),
		 errhint("Use bioseq.edit(...) in a query whose WHERE clause contains mmsearch(...).")));
	PG_RETURN_NULL();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_mapq_stub);
Datum bioseq_mapq_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_mapq_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_chain_score_stub);
Datum bioseq_chain_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_chain_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_dp_max_score_stub);
Datum bioseq_dp_max_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_dp_max_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_seed_count_stub);
Datum bioseq_seed_count_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_seed_count_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_secondary_chain_score_stub);
Datum bioseq_secondary_chain_score_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_secondary_chain_score_stub() UNEXPECTED direct call");
	PG_RETURN_INT32(-1);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_is_reverse_stub);
Datum bioseq_is_reverse_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_is_reverse_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_is_primary_stub);
Datum bioseq_is_primary_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_is_primary_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_has_cigar_stub);
Datum bioseq_has_cigar_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_has_cigar_stub() UNEXPECTED direct call");
	PG_RETURN_BOOL(false);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_divergence_stub);
Datum bioseq_divergence_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_divergence_stub() UNEXPECTED direct call");
	PG_RETURN_FLOAT4(-1.0f);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_cigar_stub);
Datum bioseq_cigar_stub(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_cigar_stub() UNEXPECTED direct call");
	PG_RETURN_TEXT_P(cstring_to_text("!UNEXPECTED!"));
}

CXX_PG_FUNCTION_INFO_V1(bioseq_gpu_warmup_shader_cache);
Datum bioseq_gpu_warmup_shader_cache(PG_FUNCTION_ARGS) {
	const std::string report =
		bioseq::parallelism::pg::warmup_backend_local_shader_cache_report();
	PG_RETURN_TEXT_P(cstring_to_text_with_len(
		report.data(),
		static_cast<int>(report.size())));
}

CXX_PG_FUNCTION_INFO_V1(bioseq_vulkan_runtime_status);
Datum bioseq_vulkan_runtime_status(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("bioseq.vulkan_runtime_status() must be called as a composite-returning function")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	bioseq::gpu::vulkan::Runtime* runtime_ptr = nullptr;
	ErrorData* runtime_pg_error = nullptr;
	bool runtime_cpp_error = false;
	bool runtime_unknown_cpp_error = false;
	char runtime_error_detail[512] = "std::exception without detail";
	try {
		runtime_ptr = std::addressof(
			bioseq::parallelism::pg::ensure_pg_vulkan_runtime_ready());
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		runtime_pg_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			runtime_error_detail,
			error.what() == nullptr ? "std::exception without detail" : error.what(),
			sizeof(runtime_error_detail));
		runtime_cpp_error = true;
	} catch (...) {
		runtime_unknown_cpp_error = true;
	}
	if (runtime_pg_error != nullptr) {
		ReThrowError(runtime_pg_error);
	}
	if (runtime_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("Vulkan runtime readiness raised a C++ exception"),
			 errdetail_internal("%s", runtime_error_detail)));
	}
	if (runtime_unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("Vulkan runtime readiness raised an unknown C++ exception")));
	}
	if (runtime_ptr == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("Vulkan runtime readiness returned no runtime")));
	}
	auto& runtime = *runtime_ptr;
	const auto& features = runtime.features();

	Datum values[7]{};
	bool nulls[7]{};
	values[0] = BoolGetDatum(features.available);
	values[1] = CStringGetTextDatum(features.device_name.data());
	values[2] = Int32GetDatum(static_cast<std::int32_t>(
		std::min<std::uint32_t>(
			features.max_workgroup_count_x,
			static_cast<std::uint32_t>(
				std::numeric_limits<std::int32_t>::max()))));
	values[3] = BoolGetDatum(features.scalar_block_layout);
	values[4] = CStringGetTextDatum(runtime.initialization_error().c_str());
	values[5] = Int64GetDatum(static_cast<std::int64_t>(
		std::min<std::uint64_t>(
			features.device_local_bytes,
			static_cast<std::uint64_t>(
				std::numeric_limits<std::int64_t>::max()))));
	values[6] = BoolGetDatum(features.software_device);

	HeapTuple tuple = heap_form_tuple(tupdesc, values, nulls);
	PG_RETURN_DATUM(HeapTupleGetDatum(tuple));
}

	/*
	 * bioseq.symbol_count(el bioseq.seq, sym text) -> bigint.
 *
 * The Postgres entry point computes the result directly from the passed-in
 * varlena (so it works on literals and outside a scan context). This path is
 * deliberately expensive (O(n) over the sequence): the planner-support
 * function bioseq_symbol_count_support flags it as such so any query that
 * routes through the bioseq columnar custom scan will prefer the custom
 * scan's cheap path, which resolves the value from bioseq_internal.meta_sidecar
 * without re-examining the varlena. See columnar_customscan.cppm —
 * VarInjectorMutator rewrites every `symbol_count(Var, 'L')` call site in
 * the target list into a placeholder Const that is populated per-row from
 * the sidecar; this function is never invoked inside the custom scan for a
 * column reference.
 *
 * DOC0279 §12.5: this scalar entry computes the REAL per-rank tally
 * (tally_symbol_counts, an int64[2^bits_per_char] histogram) over the
 * literal's inline bases — NOT the deleted `49`-stub. The custom-scan
 * fast path (columnar_customscan.cppm) routes the same tally through the
 * stored INLINE SymbolCountsIndex + the recompute fallback, so both paths
 * return identical values. A stored (locator-stub) value cannot be tallied
 * here because the scalar entry has no owning-relation context to stream
 * the sub-table fragments from; such values are handled by the custom scan.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_symbol_count);
Datum bioseq_symbol_count(PG_FUNCTION_ARGS) {
	/* Detoast the bioseq.seq varlena and reinterpret as pgseq_t. */
	pgseq_t* hdr = reinterpret_cast<pgseq_t*>(
		PG_DETOAST_DATUM(PG_GETARG_DATUM(0)));
	text* sym_text = PG_GETARG_TEXT_PP(1);
	const int32 sym_size = VARSIZE_ANY_EXHDR(sym_text);
	if (sym_size != 1) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq.symbol_count: symbol must be exactly one byte, got %d",
						sym_size)));
	}
	const char sym_char = *VARDATA_ANY(sym_text);

	if (bioseq::storage::is_locator_stub(hdr)) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("bioseq.symbol_count: cannot tally a stored (stub) bioseq.seq value outside a scan"),
				 errhint("query the column through the bioseq columnar scan, or run "
						 "bioseq.update_index(rel, 'symbol_counts') to materialise the counter.")));
	}

	int rank = bioseq::storage::indexing::letter_to_rank(hdr->alphabet_code, sym_char);
	if (rank < 0) {
		PG_FREE_IF_COPY(hdr, 0);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("bioseq.symbol_count: letter '%c' is not part of the alphabet (code=%u)",
						sym_char, static_cast<unsigned>(hdr->alphabet_code))));
	}

	int64_t computed_result = 0;
	pgseq_t::dispatch_instance(hdr->alphabet_code, hdr->storage_unit_byte_width,
		[&]<class S>() {
			compact_sequence_range<S> range{*hdr};
			auto lazy = bioseq::storage::make_lazy_in_memory(range);
			const auto lazy_view =
				bioseq::storage::as_lazy_sequence_view(lazy);
			auto counts = bioseq::indexing::tally_symbol_counts(lazy_view);  // std::array<int64,N=2^bits_per_char>
			if (static_cast<size_t>(rank) < counts.size()) {
				computed_result = static_cast<int64_t>(counts[rank]);
			}
		});

	PG_FREE_IF_COPY(hdr, 0);
	PG_RETURN_INT64(computed_result);
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

CXX_PG_FUNCTION_INFO_V1(bioseq_symbol_count_support);
Datum bioseq_symbol_count_support(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_symbol_count_support() entered");
	Node* request = reinterpret_cast<Node*>(PG_GETARG_POINTER(0));
	if (SupportRequestCost* cost_request = pg_node_try_as<SupportRequestCost>(request)) {
		bioseq_log<DBG>("[bioseq] pg: bioseq_symbol_count_support() received SupportRequestCost");
		cost_request->startup = 50000;
		cost_request->per_tuple = 0;
		PG_RETURN_POINTER(cost_request);
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
			PG_RETURN_POINTER(wrapped);
		}
	}
	PG_RETURN_POINTER(nullptr);
}

/*
 * mmsearch_support is the planner-support function attached to mmsearch().
 * PostgreSQL calls it with SupportRequest nodes so we can adjust cost
 * estimates before planning proceeds.
 */
CXX_PG_FUNCTION_INFO_V1(mmsearch_support);
Datum mmsearch_support(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: mmsearch_support() entered");
	Node* response = nullptr;
	Node* request = reinterpret_cast<Node*>(PG_GETARG_POINTER(0));
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
		PG_RETURN_POINTER(cost_request);
	}
	PG_RETURN_POINTER(response);
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
CXX_PG_FUNCTION_INFO_V1(bioseq_placeholder_support);
Datum bioseq_placeholder_support(PG_FUNCTION_ARGS) {
	bioseq_log<DBG>("[bioseq] pg: bioseq_placeholder_support() entered");
	Node* request = reinterpret_cast<Node*>(PG_GETARG_POINTER(0));
	SupportRequestSimplify* simplify = pg_node_try_as<SupportRequestSimplify>(request);
	if (simplify == nullptr) {
		PG_RETURN_POINTER(nullptr);
	}
	Node* wrapped = try_wrap_in_placeholder(simplify);
	PG_RETURN_POINTER(wrapped);
}

/*
 * bioseq.list_metadata() — DOC0279 §12.1/§12.2. Set-returning function
 * emitting one tuple per (user bioseq table, bioseq.seq column, registered
 * index) — the FULL cartesian product of {user bioseq relations} x {seq
 * columns} x pg_supported_indexes::list_types(), NOT gated on whether any
 * chunk exists. Columns: (table_name, column_attno, column_name, name,
 * index_type, update_policy, size, age). The index set is the compile-time
 * registry (no runtime MetadataKindRegistry). The heavy lifting is done by
 * one constructed SQL query that joins a VALUES source (built in C++ with
 * the per-column physical shard prefix) against index_policy + the derived
 * seqidx_* shards + metadata_age_log.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_list_metadata);
Datum bioseq_list_metadata(PG_FUNCTION_ARGS) {
	namespace bsi = bioseq::storage::indexing;
	namespace bidx = bioseq::indexing;

	ReturnSetInfo* rsinfo = reinterpret_cast<ReturnSetInfo*>(fcinfo->resultinfo);
	if (rsinfo == nullptr || !pg_node_view{pg_as_node(rsinfo)}.is<ReturnSetInfo>())
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("bioseq.list_metadata() must be called in a set-returning context")));
	if ((rsinfo->allowedModes & SFRM_Materialize) == 0)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("materialize mode required for bioseq.list_metadata()")));
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			errmsg("bioseq.list_metadata() return type is not composite")));

	MemoryContext per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	MemoryContext old_ctx = MemoryContextSwitchTo(per_query_ctx);
	Tuplestorestate* tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult  = tupstore;
	rsinfo->setDesc    = tupdesc;
	MemoryContextSwitchTo(old_ctx);

	const auto kinds = bsi::pg_supported_indexes::list_types();
	const auto allows = bsi::pg_supported_indexes::allows_on_insert_flags();

	if (SPI_connect() != SPI_OK_CONNECT)
		return static_cast<Datum>(0);

	/* 1. every user bioseq-AM relation (exclude internal namespaces / names). */
	std::vector<Oid> rels;
	if (SPI_execute(
			"SELECT c.oid FROM pg_class c JOIN pg_am am ON c.relam=am.oid "
			"JOIN pg_namespace n ON c.relnamespace=n.oid "
			"WHERE am.amname='bioseq' AND c.relkind='r' "
			"AND n.nspname NOT IN ('bioseq_internal','pg_catalog','information_schema') "
			"AND n.nspname NOT LIKE 'pg\\_temp%'", true, 0) == SPI_OK_SELECT) {
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
		if (SPI_execute(colsql.c_str(), true, 0) != SPI_OK_SELECT) continue;
		std::vector<std::pair<std::int16_t, std::string>> cols;
		for (std::uint64_t c = 0; c < SPI_processed; ++c) {
			auto att_cell = bioseq::pg_utils::pg_spi_cell::get(
				SPI_tuptable->vals[c], SPI_tuptable->tupdesc, 1);
			const std::int16_t a = att_cell.optional<std::int16_t>().value_or(0);
			char* nm = SPI_getvalue(SPI_tuptable->vals[c], SPI_tuptable->tupdesc, 2);
			cols.emplace_back(a, nm ? std::string(nm) : std::string());
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
					quote_literal_cstr(attname.c_str()),
					quote_literal_cstr(idx_name.c_str()),
					quote_literal_cstr(idx_type.c_str()),
					def_uoi ? "true" : "false",
					quote_literal_cstr(prefix.c_str()));
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
		if (SPI_execute(query.c_str(), true, 0) == SPI_OK_SELECT) {
			for (std::uint64_t r = 0; r < SPI_processed; ++r) {
				bioseq::pg_utils::pg_datum_list<8> row{};
				for (int c = 0; c < 8; ++c) {
					auto cell = bioseq::pg_utils::pg_spi_cell::get(
						SPI_tuptable->vals[r], SPI_tuptable->tupdesc, c + 1);
					if (cell.is_null) {
						row.set_null(c + 1);
					} else {
						row.set_raw(c + 1, cell.value);
					}
				}
				row.put_values(tupstore, tupdesc);
			}
		}
	}

	SPI_finish();
	return static_cast<Datum>(0);
}

/* DOC0371: same-backend, same-invocation alignment proof. */
CXX_PG_FUNCTION_INFO_V1(bioseq_last_alignment_execution_stats);
Datum bioseq_last_alignment_execution_stats(PG_FUNCTION_ARGS) {
	return materialize_alignment_execution_stats<
		alignment_stats_v1_column_count>(
			fcinfo, "bioseq.last_alignment_execution_stats()");
}

/* DOC0372: backward-compatible v2 appends one versioned SIMD proof object. */
CXX_PG_FUNCTION_INFO_V1(bioseq_last_alignment_execution_stats_v2);
Datum bioseq_last_alignment_execution_stats_v2(PG_FUNCTION_ARGS) {
	return materialize_alignment_execution_stats<
		alignment_stats_v2_column_count>(
			fcinfo, "bioseq.last_alignment_execution_stats_v2()");
}

/* DOC0373: exact Format-5 cold-reader allocation proof for the last snapshot. */
CXX_PG_FUNCTION_INFO_V1(
	bioseq_last_alignment_format5_reader_memory_evidence);
Datum bioseq_last_alignment_format5_reader_memory_evidence(PG_FUNCTION_ARGS) {
	if (!last_alignment_execution_snapshot.has_value()) {
		PG_RETURN_NULL();
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	const std::string encoded =
		format5_reader_memory_evidence_json(snapshot);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

/* Additive exact-array/layout proof; v1 remains byte-for-byte compatible. */
CXX_PG_FUNCTION_INFO_V1(
	bioseq_last_alignment_format5_reader_memory_evidence_v2);
Datum bioseq_last_alignment_format5_reader_memory_evidence_v2(
		PG_FUNCTION_ARGS) {
	if (!last_alignment_execution_snapshot.has_value()) {
		PG_RETURN_NULL();
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	const std::string encoded =
		format5_reader_memory_evidence_json_v2(snapshot);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

/* DOC0383: v3 appends fragment-native query traffic/timing receipts. */
CXX_PG_FUNCTION_INFO_V1(
	bioseq_last_alignment_format5_reader_memory_evidence_v3);
Datum bioseq_last_alignment_format5_reader_memory_evidence_v3(
		PG_FUNCTION_ARGS) {
	if (!last_alignment_execution_snapshot.has_value()) {
		PG_RETURN_NULL();
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	const std::string encoded =
		format5_reader_memory_evidence_json_v3(snapshot);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

/* DOC0383: v4 appends exact provider/image/lifecycle evidence to frozen v3. */
CXX_PG_FUNCTION_INFO_V1(
	bioseq_last_alignment_format5_reader_memory_evidence_v4);
Datum bioseq_last_alignment_format5_reader_memory_evidence_v4(
		PG_FUNCTION_ARGS) {
	if (!last_alignment_execution_snapshot.has_value()) {
		PG_RETURN_NULL();
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	const std::string encoded =
		format5_reader_memory_evidence_json_v4(snapshot);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

/* P4: v5 appends bucket-direct/Bloom capacity evidence to frozen v4. */
CXX_PG_FUNCTION_INFO_V1(
    bioseq_last_alignment_format5_reader_memory_evidence_v5);
Datum bioseq_last_alignment_format5_reader_memory_evidence_v5(
        PG_FUNCTION_ARGS) {
    if (!last_alignment_execution_snapshot.has_value()) {
        PG_RETURN_NULL();
    }
    const alignment_exec::AlignmentExecutionSnapshot& snapshot =
        *last_alignment_execution_snapshot;
    (void)validate_alignment_snapshot_environment(snapshot);
    const std::string encoded =
        format5_reader_memory_evidence_json_v5(snapshot);
    PG_RETURN_DATUM(DirectFunctionCall1(
        jsonb_in, CStringGetDatum(encoded.c_str())));
}

/* P0: v6 appends compact-direct sharding/one-decode proof to frozen v5. */
CXX_PG_FUNCTION_INFO_V1(
    bioseq_last_alignment_format5_reader_memory_evidence_v6);
Datum bioseq_last_alignment_format5_reader_memory_evidence_v6(
        PG_FUNCTION_ARGS) {
    if (!last_alignment_execution_snapshot.has_value()) {
        PG_RETURN_NULL();
    }
    const alignment_exec::AlignmentExecutionSnapshot& snapshot =
        *last_alignment_execution_snapshot;
    (void)validate_alignment_snapshot_environment(snapshot);
    const std::string encoded =
        format5_reader_memory_evidence_json_v6(snapshot);
    PG_RETURN_DATUM(DirectFunctionCall1(
        jsonb_in, CStringGetDatum(encoded.c_str())));
}

/* Exact Format-6 timed-reader identity, physical-I/O and ownership receipt. */
CXX_PG_FUNCTION_INFO_V1(
	bioseq_last_alignment_format6_reader_evidence);
Datum bioseq_last_alignment_format6_reader_evidence(PG_FUNCTION_ARGS) {
	if (!last_alignment_execution_snapshot.has_value()) {
		PG_RETURN_NULL();
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_alignment_execution_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	const std::string encoded =
		format6_reader_evidence_json(snapshot);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

/* Exact accepted persisted Shape-V5 GPU operation/job causal ledger. */
CXX_PG_FUNCTION_INFO_V1(
	bioseq_last_alignment_persisted_gpu_operation_evidence_v1);
Datum bioseq_last_alignment_persisted_gpu_operation_evidence_v1(
		PG_FUNCTION_ARGS) {
	if (!last_completed_alignment_evidence_snapshot.has_value()) {
		PG_RETURN_NULL();
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_completed_alignment_evidence_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	const std::string encoded =
		persisted_gpu_operation_evidence_json(snapshot, false);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

/* Cache-qualified v2 persisted Shape-V5 GPU operation/job causal ledger. */
CXX_PG_FUNCTION_INFO_V1(
	bioseq_last_alignment_persisted_gpu_operation_evidence_v2);
Datum bioseq_last_alignment_persisted_gpu_operation_evidence_v2(
		PG_FUNCTION_ARGS) {
	if (!last_completed_alignment_evidence_snapshot.has_value()) {
		PG_RETURN_NULL();
	}
	const alignment_exec::AlignmentExecutionSnapshot& snapshot =
		*last_completed_alignment_evidence_snapshot;
	(void)validate_alignment_snapshot_environment(snapshot);
	const std::string encoded =
		persisted_gpu_operation_evidence_json(snapshot, true);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

/*
 * bioseq.update_index(rel regclass, kind text) — bulk recompute or
 * create the named metadata kind for the given user table. Returns the
 * count of entries written. Forwards into bioseq::metadata_admin.
 */
CXX_PG_FUNCTION_INFO_V1(bioseq_update_index);
Datum bioseq_update_index(PG_FUNCTION_ARGS) {
	// DOC0279 §11.4 — route to the chunked-storage reindex driver. The driver
	// validates the name, clears + rebuilds the named index for every SEQ
	// column of every row, and stamps metadata_age_log on success.
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t n =
		bioseq::storage::indexing::update_index_pg_checked(
			rel, VARDATA_ANY(nm),
			static_cast<std::size_t>(VARSIZE_ANY_EXHDR(nm)));
	PG_RETURN_INT64(n);
}

/* DOC0279 §12.2 — read the backend-local minimizers_per_sequence merge run
 * counter (test observability). */
CXX_PG_FUNCTION_INFO_V1(bioseq_last_index_merge_run_count);
Datum bioseq_last_index_merge_run_count(PG_FUNCTION_ARGS) {
	PG_RETURN_INT64((std::int64_t)bioseq::storage::indexing::g_last_index_merge_run_count);
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_reset_active_read_policy_debug);
Datum bioseq_internal_reset_active_read_policy_debug(PG_FUNCTION_ARGS) {
	bioseq::storage::indexing::reset_active_read_policy_debug_state();
	PG_RETURN_VOID();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_active_read_policy_debug);
Datum bioseq_internal_active_read_policy_debug(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc)
			!= TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("active-read policy debug requires a composite result")));
	}
	tupdesc = BlessTupleDesc(tupdesc);
	const auto resolver = bioseq::storage::indexing::
		g_active_read_policy_resolution_counters;
	const auto dispatch = bioseq::storage::indexing::
		g_active_read_policy_dispatch_proof_counters;
	const auto source = bioseq::storage::indexing::
		active_read_policy_resolution_source_name(
			bioseq::storage::indexing::
				g_last_active_read_policy_dispatch_source);
	bioseq::pg_utils::pg_datum_list<9> values{};
	values.set(1, static_cast<std::int64_t>(resolver.configured_rows));
	values.set(2, static_cast<std::int64_t>(
		resolver.authoritative_defaults));
	values.set(3, static_cast<std::int64_t>(resolver.non_relation));
	values.set(4, static_cast<std::int64_t>(dispatch.configured_batches));
	values.set(5, static_cast<std::int64_t>(
		dispatch.authoritative_default_batches));
	values.set(6, static_cast<std::int64_t>(dispatch.non_relation_batches));
	values.set_raw(7, bioseq::pg_utils::pg_datum::from_text_cstring(
		source.data()));
	values.set(8, static_cast<std::int64_t>(bioseq::storage::indexing::
		g_prohibited_external_file_producer_entry_counters
			.constructor_entries));
	values.set(9, static_cast<std::int64_t>(bioseq::storage::indexing::
		g_prohibited_external_file_producer_entry_counters.writer_entries));
	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(
		bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

CXX_PG_FUNCTION_INFO_V1(bioseq_last_mps_build_stats);
Datum bioseq_last_mps_build_stats(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR, (errmsg("bioseq.last_mps_build_stats: composite result type expected")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	const auto st = bioseq::storage::indexing::g_last_mps_build_stats;
	const std::array<std::uint64_t, 18> fields{
		st.backend_pid,
		st.db_oid,
		st.relfilenode,
		st.sequence_rowno,
		st.total_run_count,
		st.resident_runs,
		st.file_runs,
		st.resident_run_bytes,
		st.resident_peak_bytes,
		st.spilled_run_bytes,
		st.postgres_temp_file_bytes,
		st.postgres_temp_file_count,
		st.merge_reader_buffer_bytes,
		st.merge_pass_count,
		st.spill_count_from_pressure,
		st.resident_demotions,
		st.position_spill_bytes,
		st.position_spill_count,
	};
	bioseq::pg_utils::pg_datum_list<18> values{};
	for (std::size_t i = 0; i < fields.size(); ++i) {
		values.set(static_cast<int>(i + 1), static_cast<std::int64_t>(fields[i]));
	}

	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

CXX_PG_FUNCTION_INFO_V1(bioseq_scheduler_mps_build_stats);
Datum bioseq_scheduler_mps_build_stats(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR, (errmsg("bioseq.scheduler_mps_build_stats: composite result type expected")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	const auto snap = bioseq::parallelism::pg::async_mps_build_stats_snapshot();
	const auto st = snap.stats;
	const std::array<std::uint64_t, 24> fields{
		st.backend_pid,
		st.db_oid,
		st.relfilenode,
		st.sequence_rowno,
		st.total_run_count,
		st.resident_runs,
		st.file_runs,
		st.resident_run_bytes,
		st.resident_peak_bytes,
		st.spilled_run_bytes,
		st.postgres_temp_file_bytes,
		st.postgres_temp_file_count,
		st.merge_reader_buffer_bytes,
		st.merge_pass_count,
		st.spill_count_from_pressure,
		st.resident_demotions,
		st.position_spill_bytes,
		st.position_spill_count,
		snap.generation,
		snap.worker_build_ns,
		snap.pipeline_mode,
		snap.max_concurrency,
		snap.index_memory_cap_mb,
		snap.lazy_worker_shareable,
	};
	bioseq::pg_utils::pg_datum_list<24> values{};
	for (std::size_t i = 0; i < fields.size(); ++i) {
		values.set(static_cast<int>(i + 1), static_cast<std::int64_t>(fields[i]));
	}

	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_arm_async_mps_worker_failure);
Datum bioseq_internal_arm_async_mps_worker_failure(PG_FUNCTION_ARGS) {
	text* point_text = PG_GETARG_TEXT_PP(0);
	const std::string_view point_name{
		VARDATA_ANY(point_text),
		static_cast<std::size_t>(VARSIZE_ANY_EXHDR(point_text))};
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
		PG_RETURN_BOOL(
			bioseq::parallelism::pg::arm_async_mps_stage_pg_failure(
				stage_point));
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
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unknown asynchronous MPS worker failure point"),
			 errdetail("failure_point=%.*s",
				static_cast<int>(point_name.size()), point_name.data())));
	}
	PG_RETURN_BOOL(
		bioseq::parallelism::pg::arm_async_mps_worker_pg_failure(point));
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_disarm_async_mps_worker_failure);
Datum bioseq_internal_disarm_async_mps_worker_failure(PG_FUNCTION_ARGS) {
	bioseq::parallelism::pg::disarm_async_mps_worker_pg_failure();
	bioseq::parallelism::pg::disarm_async_mps_stage_pg_failure();
	PG_RETURN_VOID();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_async_mps_worker_boundary_stats);
Datum bioseq_internal_async_mps_worker_boundary_stats(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc)
			!= TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("asynchronous MPS worker boundary stats require a composite result")));
	}
	tupdesc = BlessTupleDesc(tupdesc);
	const auto stats = bioseq::parallelism::pg::
		async_mps_worker_boundary_stats_snapshot();
	const char* last_point = bioseq::parallelism::pg::
		async_mps_worker_failure_point_name(stats.last_failure_point);
	const char* armed_point = bioseq::parallelism::pg::
		async_mps_worker_failure_point_name(stats.armed_failure_point);
	bioseq::pg_utils::pg_datum_list<14> values{};
	values.set(1, static_cast<std::int64_t>(stats.relation_leaf_calls));
	values.set(2, static_cast<std::int64_t>(stats.spi_leaf_calls));
	values.set(3, static_cast<std::int64_t>(
		stats.materialize_interrupt_leaf_calls));
	values.set(4, static_cast<std::int64_t>(
		stats.materialize_scanner_read_leaf_calls));
	values.set(5, static_cast<std::int64_t>(stats.pg_leaf_failures));
	values.set(6, static_cast<std::int64_t>(
		stats.injected_pg_leaf_failures));
	values.set(7, static_cast<std::int64_t>(stats.materialize_admissions));
	values.set(8, static_cast<std::int64_t>(
		stats.materialize_failure_unwinds));
	values.set(9, static_cast<std::int64_t>(
		stats.materialize_admission_restored));
	values.set(10, static_cast<std::int64_t>(
		stats.materialize_admission_mismatches));
	values.set_raw(11, bioseq::pg_utils::pg_datum::from_text_cstring(
		last_point));
	values.set_raw(12, bioseq::pg_utils::pg_datum::from_text_cstring(
		armed_point));
	values.set_raw(13, bioseq::pg_utils::pg_datum::from_text_cstring(
		stats.last_failure_point
				== bioseq::parallelism::pg::AsyncMpsWorkerPgFailure::none
			? "" : "XX000"));
	values.set_raw(14, bioseq::pg_utils::pg_datum::from_text_cstring(
		stats.last_failure_point
				== bioseq::parallelism::pg::AsyncMpsWorkerPgFailure::none
			? ""
			: "injected asynchronous MPS worker PostgreSQL leaf failure"));
	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(
		bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

CXX_PG_FUNCTION_INFO_V1(bioseq_scheduler_restart);
Datum bioseq_scheduler_restart(PG_FUNCTION_ARGS) {
	bioseq::parallelism::pg::restart_cluster();
	PG_RETURN_VOID();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_scheduler_stats);
Datum bioseq_scheduler_stats(PG_FUNCTION_ARGS) {
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR, (errmsg("bioseq.scheduler_stats: composite result type expected")));
	}
	tupdesc = BlessTupleDesc(tupdesc);

	const auto snap = bioseq::parallelism::pg::snapshot();
	const std::string mode_string{bioseq::parallelism::to_string(snap.mode)};
	bioseq::pg_utils::pg_datum_list<16> values{};
	values.set(1, snap.supervisor);
	values.set(2, static_cast<std::int32_t>(snap.config_generation));
	values.set_raw(3, bioseq::pg_utils::pg_datum::from_text_cstring(mode_string.c_str()));
	values.set(4, static_cast<std::int32_t>(snap.min_threads));
	values.set(5, static_cast<std::int32_t>(snap.max_threads));
	values.set(6, static_cast<std::int32_t>(snap.current_threads));
	values.set(7, static_cast<std::int64_t>(snap.batches_scheduled));
	values.set(8, static_cast<std::int64_t>(snap.batches_awaiting));
	values.set(9, static_cast<std::int64_t>(snap.batches_processed));
	values.set(10, static_cast<std::int64_t>(snap.queries_waiting));
	values.set(11, static_cast<std::int64_t>(snap.queries_processed));
	values.set(12, snap.oldest_waiting_batch_age_secs);
	values.set(13, snap.queries_per_sec);
	values.set(14, snap.uptime_secs);
	values.set(15, static_cast<std::int64_t>(snap.async_persist_jobs_waiting));
	values.set(16, static_cast<std::int64_t>(snap.index_memory_reserved_bytes));

	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

CXX_PG_FUNCTION_INFO_V1(
	bioseq_internal_cpu_prepared_target_same_object_reprepare_probe);
Datum bioseq_internal_cpu_prepared_target_same_object_reprepare_probe(
		PG_FUNCTION_ARGS) {
	PG_RETURN_BOOL(
		bioseq::simd::HWY_NAMESPACE::BatchProcessor::
			cpu_prepared_target_same_object_reprepare_probe());
}

/* DOC0279 §12.2 — the remaining admin/test SQL helpers. */
CXX_PG_FUNCTION_INFO_V1(bioseq_index_table_row_count);
Datum bioseq_index_table_row_count(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	PG_RETURN_INT64(bioseq::storage::indexing::index_table_row_count_impl(rel, name));
}
CXX_PG_FUNCTION_INFO_V1(bioseq_inspect_cold_no_index_absence);
Datum bioseq_inspect_cold_no_index_absence(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const auto audit =
		bioseq::storage::indexing::cold_no_index_absence_audit_impl(rel, name);
	const auto sql_bigint = [](const std::uint64_t value) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq cold no-index audit value exceeds BIGINT")));
		}
		return static_cast<std::int64_t>(value);
	};
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("bioseq cold no-index audit requires a composite result type")));
	}
	tupdesc = BlessTupleDesc(tupdesc);
	bioseq::pg_utils::pg_datum_list<8> values{};
	values.set(1, sql_bigint(audit.target_rows));
	values.set(2, audit.target_cardinality_metadata_only);
	values.set(3, sql_bigint(audit.source_parent_rows_read));
	values.set(4, sql_bigint(audit.source_fragment_rows_read));
	values.set(5, sql_bigint(audit.source_decoded_bp));
	values.set(6, sql_bigint(audit.physical_index_relations));
	values.set(7, sql_bigint(audit.persisted_index_rows));
	values.set(8, sql_bigint(audit.generation_objects));
	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}
CXX_PG_FUNCTION_INFO_V1(bioseq_inspect_ready_minimizer_generation);
Datum bioseq_inspect_ready_minimizer_generation(PG_FUNCTION_ARGS) {
	constexpr std::size_t kColumnCount = 23;
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	if (signed_query_key < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq minimizer generation inspection query key is negative")));
	}
	using Inspection = bioseq::storage::indexing::
		ReadyMinimizerGenerationInspection;
	static_assert(std::is_trivially_destructible_v<Inspection>);
	Inspection inspection{};
	ErrorData* inspection_pg_error = nullptr;
	bool inspection_cpp_error = false;
	bool inspection_unknown_cpp_error = false;
	char inspection_error_detail[512] = "std::exception without detail";
	try {
		inspection = bioseq::storage::indexing::
			inspect_ready_minimizer_generation_impl(
				rel,
				std::string_view{
					VARDATA_ANY(nm),
					static_cast<std::size_t>(VARSIZE_ANY_EXHDR(nm))},
				static_cast<std::uint64_t>(signed_query_key));
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		inspection_pg_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			inspection_error_detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(inspection_error_detail));
		inspection_cpp_error = true;
	} catch (...) {
		inspection_unknown_cpp_error = true;
	}
	// The inspection bridge deliberately converts PostgreSQL ERROR into a C++
	// exception so its relation, snapshot, and memory owners unwind normally.
	// Re-enter PostgreSQL only here, after that C++ stack is gone and while this
	// frame contains no non-trivially-destructible result owner.
	if (inspection_pg_error != nullptr) {
		ReThrowError(inspection_pg_error);
	}
	if (inspection_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq minimizer generation inspection raised a C++ exception"),
			 errdetail_internal("%s", inspection_error_detail)));
	}
	if (inspection_unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq minimizer generation inspection raised an unknown C++ exception")));
	}

	const auto sql_bigint = [](std::uint64_t value, const char* field) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq minimizer generation inspection field exceeds BIGINT"),
				 errdetail("field=%s", field)));
		}
		return static_cast<std::int64_t>(value);
	};
	const auto sql_smallint = [](std::uint64_t value, const char* field) {
		if (value > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int16_t>::max)())) {
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq minimizer generation inspection field exceeds SMALLINT"),
				 errdetail("field=%s", field)));
		}
		return static_cast<std::int16_t>(value);
	};
	const auto sql_integer = [](std::uint64_t value, const char* field) {
		if (value > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int32_t>::max)())) {
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq minimizer generation inspection field exceeds INTEGER"),
				 errdetail("field=%s", field)));
		}
		return static_cast<std::int32_t>(value);
	};

	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("bioseq minimizer generation inspection requires a composite result type")));
	}
	tupdesc = BlessTupleDesc(tupdesc);
	bioseq::pg_utils::pg_datum_list<kColumnCount> values{};
	values.set(1, sql_bigint(
		inspection.identity.generation_id, "generation_id"));
	const std::string source_identity = fixed_hex_u64(
		inspection.identity.source_identity);
	values.set_raw(2, bioseq::pg_utils::pg_datum::from_text_cstring(
		source_identity.c_str()));
	const std::string compatibility = fixed_hex_u64(
		inspection.source.compatibility_checksum);
	values.set_raw(3, bioseq::pg_utils::pg_datum::from_text_cstring(
		compatibility.c_str()));
	const std::string content = fixed_hex_u64(
		inspection.source_content_checksum);
	values.set_raw(4, bioseq::pg_utils::pg_datum::from_text_cstring(
		content.c_str()));
	values.set_oid(5, inspection.database_oid);
	values.set_oid(6, inspection.main_relfilenode);
	values.set(7, inspection.column_attno);
	values.set(8, sql_bigint(
		inspection.source.sequence_rowno, "sequence_rowno"));
	values.set(9, sql_smallint(
		inspection.source.locator.version, "locator_version"));
	values.set(10, sql_smallint(
		inspection.source.locator.bucket_idx, "bucket_idx"));
	values.set(11, sql_integer(
		inspection.source.locator.shard_idx, "shard_idx"));
	values.set(12, sql_bigint(
		inspection.source.locator.num_fragments, "num_fragments"));
	values.set(13, sql_bigint(
		inspection.source.locator.total_bp_size, "total_bp_size"));
	values.set(14, sql_bigint(
		inspection.source.locator.first_sub_row_no, "first_sub_row_no"));
	values.set(15, sql_integer(
		inspection.source.padding_letters, "padding_letters"));
	values.set(16, sql_smallint(
		inspection.source.alphabet_code, "alphabet_code"));
	values.set(17, sql_smallint(
		inspection.source.storage_unit_byte_width,
		"storage_unit_byte_width"));
	values.set(18, sql_smallint(
		inspection.manifest.kmer_size, "kmer_size"));
	values.set(19, sql_smallint(
		inspection.manifest.window_size, "window_size"));
	values.set(20, sql_integer(
		inspection.manifest.gpu_abi_version, "gpu_abi_version"));
	values.set(21, sql_smallint(
		inspection.manifest.segment_geometry_version,
		"segment_geometry_version"));
	values.set(22, sql_bigint(
		inspection.manifest.position_segment_span_bp,
		"position_segment_span_bp"));
	values.set(23, sql_integer(
		inspection.manifest.position_segment_count,
		"position_segment_count"));

	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}

CXX_PG_FUNCTION_INFO_V1(
	bioseq_inspect_ready_minimizer_format6_generation);
Datum bioseq_inspect_ready_minimizer_format6_generation(
		PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	if (signed_query_key < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq Format-6 inspection query key is negative")));
	}
	using Inspection = bioseq::storage::indexing::
		ReadyMinimizerFormat6Inspection;
	static_assert(std::is_trivially_destructible_v<Inspection>);
	Inspection inspection{};
	ErrorData* inspection_pg_error = nullptr;
	bool inspection_cpp_error = false;
	bool inspection_unknown_cpp_error = false;
	char inspection_error_detail[512] = "std::exception without detail";
	try {
		inspection = bioseq::storage::indexing::
			inspect_ready_minimizer_format6_generation_impl(
				rel,
				std::string_view{
					VARDATA_ANY(nm),
					static_cast<std::size_t>(
						VARSIZE_ANY_EXHDR(nm))},
				static_cast<std::uint64_t>(signed_query_key));
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		inspection_pg_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			inspection_error_detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(inspection_error_detail));
		inspection_cpp_error = true;
	} catch (...) {
		inspection_unknown_cpp_error = true;
	}
	if (inspection_pg_error != nullptr) {
		ReThrowError(inspection_pg_error);
	}
	if (inspection_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq Format-6 inspection raised a C++ exception"),
			 errdetail_internal("%s", inspection_error_detail)));
	}
	if (inspection_unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq Format-6 inspection raised an unknown C++ exception")));
	}
	const std::string encoded =
		ready_minimizer_format6_inspection_json(inspection);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

CXX_PG_FUNCTION_INFO_V1(
	bioseq_internal_format6_parallel_scan_probe);
Datum bioseq_internal_format6_parallel_scan_probe(
		PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	const std::int32_t signed_workers = PG_GETARG_INT32(3);
	if (signed_query_key < 0 || signed_workers < 0
		|| signed_workers > 64) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq Format-6 parallel scan probe parameters are invalid")));
	}
	using Proof = bioseq::storage::indexing::
		MinimizerFormat6ParallelScanProbePg;
	static_assert(std::is_trivially_destructible_v<Proof>);
	Proof proof{};
	ErrorData* pg_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char error_detail[512] = "std::exception without detail";
	try {
		proof = bioseq::storage::indexing::
			format6_parallel_scan_probe_impl(
				rel,
				std::string_view{
					VARDATA_ANY(nm),
					static_cast<std::size_t>(
						VARSIZE_ANY_EXHDR(nm))},
				static_cast<std::uint64_t>(
					signed_query_key),
				static_cast<std::uint32_t>(
					signed_workers));
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		pg_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			error_detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(error_detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	if (pg_error != nullptr) {
		ReThrowError(pg_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq Format-6 parallel scan probe raised a C++ exception"),
			 errdetail_internal("%s", error_detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq Format-6 parallel scan probe raised an unknown C++ exception")));
	}
	const std::string encoded =
		format6_parallel_scan_probe_json(proof);
	PG_RETURN_DATUM(DirectFunctionCall1(
		jsonb_in, CStringGetDatum(encoded.c_str())));
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_build_fixture);
Datum bioseq_internal_rev3_test_build_fixture(PG_FUNCTION_ARGS) {
	constexpr std::size_t kColumnCount = 20;
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	const std::int16_t signed_k = PG_GETARG_INT16(3);
	const std::int16_t signed_w = PG_GETARG_INT16(4);
	const std::int16_t signed_wire_format = PG_GETARG_INT16(5);
	text* backend_text = PG_GETARG_TEXT_PP(6);
	if (signed_query_key < 0 || signed_k <= 0 || signed_w <= 0
		|| (signed_wire_format != 3 && signed_wire_format != 4)) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq rev3 test fixture parameters are invalid")));
	}
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const auto wire_format = static_cast<
		bioseq::indexing::MinimizerGenerationWireFormat>(
			static_cast<std::uint16_t>(signed_wire_format));
	const std::string requested_backend(
		VARDATA_ANY(backend_text), VARSIZE_ANY_EXHDR(backend_text));
	std::string_view canonical_backend = requested_backend;
	if (canonical_backend == "exact") canonical_backend = "exact_simd";
	if (canonical_backend == "paper") canonical_backend = "paper_nthash_simd";
	if (canonical_backend == "vulkan") canonical_backend = "vulkan_segment";
	const auto backend = bioseq::storage::indexing::
		ready_minimizer_fixture_backend_from_name(canonical_backend);
	const auto fixture =
		bioseq::storage::indexing::build_rev3_pg_integration_fixture_impl(
			rel, name, static_cast<std::uint64_t>(signed_query_key),
			static_cast<std::uint16_t>(signed_k),
			static_cast<std::uint16_t>(signed_w), wire_format, backend);

	const auto sql_bigint = [](std::uint64_t value, const char* field) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq rev3 test fixture field exceeds BIGINT"),
				 errdetail("field=%s", field)));
		}
		return static_cast<std::int64_t>(value);
	};
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("bioseq rev3 test fixture requires a composite result type")));
	}
	tupdesc = BlessTupleDesc(tupdesc);
	bioseq::pg_utils::pg_datum_list<kColumnCount> values{};
	values.set_raw(1, bioseq::pg_utils::pg_datum::from_text_cstring(
		fixture.table_name.c_str()));
	values.set(2, static_cast<std::int16_t>(fixture.wire_format));
	values.set(3, sql_bigint(fixture.generation_id, "generation_id"));
	const std::string source_identity = fixed_hex_u64(fixture.source_identity);
	values.set_raw(4, bioseq::pg_utils::pg_datum::from_text_cstring(
		source_identity.c_str()));
	const std::string source_checksum = fixed_hex_u64(
		fixture.source_content_checksum);
	values.set_raw(5, bioseq::pg_utils::pg_datum::from_text_cstring(
		source_checksum.c_str()));
	const std::string compatibility = fixed_hex_u64(
		fixture.compatibility_checksum);
	values.set_raw(6, bioseq::pg_utils::pg_datum::from_text_cstring(
		compatibility.c_str()));
	values.set(7, sql_bigint(
		fixture.source_num_fragments, "source_num_fragments"));
	values.set(8, sql_bigint(fixture.source_total_bp, "source_total_bp"));
	values.set(9, sql_bigint(
		fixture.source_padding_letters, "source_padding_letters"));
	values.set(10, sql_bigint(fixture.writer_rows, "writer_rows"));
	values.set(11, sql_bigint(fixture.writer_batches, "writer_batches"));
	values.set(12, sql_bigint(
		fixture.published_head_bytes, "published_head_bytes"));
	values.set(13, sql_bigint(fixture.control_count, "control_count"));
	values.set(14, sql_bigint(fixture.position_count, "position_count"));
	values.set(15, sql_bigint(
		fixture.directory_entries, "directory_entries"));
	values.set(16, sql_bigint(
		fixture.position_segment_count, "position_segment_count"));
	values.set(17, sql_bigint(
		fixture.control_encoded_bytes, "control_encoded_bytes"));
	values.set(18, sql_bigint(
		fixture.position_encoded_bytes, "position_encoded_bytes"));
	values.set(19, sql_bigint(fixture.control_chunks, "control_chunks"));
	values.set(20, sql_bigint(fixture.position_chunks, "position_chunks"));

	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_read_fixture);
Datum bioseq_internal_rev3_test_read_fixture(PG_FUNCTION_ARGS) {
	constexpr std::size_t kColumnCount = 36;
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	const std::int16_t signed_k = PG_GETARG_INT16(3);
	const std::int16_t signed_w = PG_GETARG_INT16(4);
	const std::int16_t signed_wire_format = PG_GETARG_INT16(5);
	text* backend_text = PG_GETARG_TEXT_PP(6);
	if (signed_query_key < 0 || signed_k <= 0 || signed_w <= 0
		|| (signed_wire_format != 3 && signed_wire_format != 4)) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq rev3 test fixture reader parameters are invalid")));
	}
	const auto wire_format = static_cast<
		bioseq::indexing::MinimizerGenerationWireFormat>(
			static_cast<std::uint16_t>(signed_wire_format));
	struct ReadFixtureFmgrState {
		std::optional<bioseq::storage::indexing::
			Rev3PgIntegrationFixtureResult> result;
	};
	ReadFixtureFmgrState* state = nullptr;
	ErrorData* postgres_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char error_detail[512] = "std::exception without detail";
	try {
		state = new ReadFixtureFmgrState{};
		const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
		const std::string requested_backend(
			VARDATA_ANY(backend_text), VARSIZE_ANY_EXHDR(backend_text));
		std::string_view canonical_backend = requested_backend;
		if (canonical_backend == "exact") canonical_backend = "exact_simd";
		if (canonical_backend == "paper") {
			canonical_backend = "paper_nthash_simd";
		}
		if (canonical_backend == "vulkan") {
			canonical_backend = "vulkan_segment";
		}
		const auto backend = bioseq::storage::indexing::
			ready_minimizer_fixture_backend_from_name(canonical_backend);
		state->result.emplace(
			bioseq::storage::indexing::read_rev3_pg_integration_fixture_impl(
				rel, name, static_cast<std::uint64_t>(signed_query_key),
				static_cast<std::uint16_t>(signed_k),
				static_cast<std::uint16_t>(signed_w), wire_format, backend));
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		postgres_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(error_detail,
			error.what() == nullptr
				? "std::exception without detail" : error.what(),
			sizeof(error_detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	if (postgres_error != nullptr) {
		if (state != nullptr) state->result.reset();
		delete state;
		ReThrowError(postgres_error);
	}
	if (cpp_error || unknown_cpp_error || state == nullptr
		|| !state->result.has_value()) {
		const bool unknown = unknown_cpp_error;
		if (state != nullptr) state->result.reset();
		delete state;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq rev3 read fixture raised %s",
				unknown ? "an unknown C++ exception" : "a C++ exception"),
			 errdetail_internal("%s", error_detail)));
	}
	const auto& fixture = *state->result;

	const auto sql_bigint = [](std::uint64_t value, const char* field) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq rev3 test fixture reader field exceeds BIGINT"),
				 errdetail("field=%s", field)));
		}
		return static_cast<std::int64_t>(value);
	};
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("bioseq rev3 test fixture reader requires a composite result type")));
	}
	tupdesc = BlessTupleDesc(tupdesc);
	bioseq::pg_utils::pg_datum_list<kColumnCount> values{};
	values.set_raw(1, bioseq::pg_utils::pg_datum::from_text_cstring(
		fixture.table_name.c_str()));
	values.set(2, static_cast<std::int16_t>(fixture.wire_format));
	values.set(3, sql_bigint(fixture.generation_id, "generation_id"));
	const std::string source_identity = fixed_hex_u64(fixture.source_identity);
	values.set_raw(4, bioseq::pg_utils::pg_datum::from_text_cstring(
		source_identity.c_str()));
	const std::string source_checksum = fixed_hex_u64(
		fixture.source_content_checksum);
	values.set_raw(5, bioseq::pg_utils::pg_datum::from_text_cstring(
		source_checksum.c_str()));
	const std::string compatibility = fixed_hex_u64(
		fixture.compatibility_checksum);
	values.set_raw(6, bioseq::pg_utils::pg_datum::from_text_cstring(
		compatibility.c_str()));
	values.set(7, sql_bigint(
		fixture.source_num_fragments, "source_num_fragments"));
	values.set(8, sql_bigint(fixture.source_total_bp, "source_total_bp"));
	values.set(9, sql_bigint(
		fixture.source_padding_letters, "source_padding_letters"));
	values.set(10, sql_bigint(
		fixture.validated_control_bytes, "validated_control_bytes"));
	values.set(11, sql_bigint(
		fixture.validated_position_bytes, "validated_position_bytes"));
	const std::string control_checksum = fixed_hex_u64(
		fixture.validated_control_checksum);
	values.set_raw(12, bioseq::pg_utils::pg_datum::from_text_cstring(
		control_checksum.c_str()));
	const std::string position_checksum = fixed_hex_u64(
		fixture.validated_position_checksum);
	values.set_raw(13, bioseq::pg_utils::pg_datum::from_text_cstring(
		position_checksum.c_str()));
	values.set(14, sql_bigint(fixture.lookup_count, "lookup_count"));
	values.set(15, sql_bigint(fixture.lookup_bp, "lookup_bp"));
	values.set(16, fixture.lookup_reverse);
	values.set(17, fixture.positions_complete);
	values.set(18, sql_bigint(
		fixture.read_stats.relation_opens, "relation_opens"));
	values.set(19, sql_bigint(
		fixture.read_stats.index_scans, "index_scans"));
	values.set(20, sql_bigint(
		fixture.read_stats.head_scans, "head_scans"));
	values.set(21, sql_bigint(
		fixture.read_stats.manifest_directory_scans,
		"manifest_directory_scans"));
	values.set(22, sql_bigint(
		fixture.read_stats.planned_request_groups,
		"planned_request_groups"));
	values.set(23, sql_bigint(
		fixture.read_stats.executed_request_groups,
		"executed_request_groups"));
	values.set(24, sql_bigint(
		fixture.read_stats.bulk_read_calls, "bulk_read_calls"));
	values.set(25, sql_bigint(
		fixture.read_stats.range_scan_calls, "range_scan_calls"));
	values.set(26, sql_bigint(
		fixture.read_stats.payload.wanted_extents, "payload_wanted_extents"));
	values.set(27, sql_bigint(
		fixture.read_stats.payload.read_extents, "payload_read_extents"));
	values.set(28, sql_bigint(
		fixture.read_stats.payload.wanted_chunks, "payload_wanted_chunks"));
	values.set(29, sql_bigint(
		fixture.read_stats.payload.delivered_chunks,
		"payload_delivered_chunks"));
	values.set(30, sql_bigint(
		fixture.read_stats.payload.delivered_bytes,
		"payload_delivered_bytes"));
	values.set(31, sql_bigint(
		fixture.read_stats.payload_heap_rows_read,
		"payload_heap_rows_read"));
	values.set(32, sql_bigint(
		fixture.read_stats.payload.skipped_rows, "payload_skipped_rows"));
	values.set(33, sql_bigint(
		fixture.read_stats.toast_values_read, "toast_values_read"));
	values.set(34, sql_bigint(
		fixture.read_stats.detoast_bytes, "detoast_bytes"));
	values.set(35, sql_bigint(
		fixture.read_stats.cache_lookups, "cache_lookups"));
	const std::string selective_cold = cold_index_selective_probe_json(
		fixture.selective_probe_stats.cold);
	const Datum selective_cold_datum = DirectFunctionCall1(
		jsonb_in, CStringGetDatum(selective_cold.c_str()));
	values.set_raw(36,
		bioseq::pg_utils::pg_datum::from_raw(selective_cold_datum));

	HeapTuple tuple = values.form_tuple(tupdesc);
	state->result.reset();
	delete state;
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}
CXX_PG_FUNCTION_INFO_V1(
	bioseq_internal_rev3_test_read_full_generation_fixture);
Datum bioseq_internal_rev3_test_read_full_generation_fixture(
		PG_FUNCTION_ARGS) {
	constexpr std::size_t kColumnCount = 16;
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	const std::int16_t signed_k = PG_GETARG_INT16(3);
	const std::int16_t signed_w = PG_GETARG_INT16(4);
	const std::int16_t signed_wire_format = PG_GETARG_INT16(5);
	text* backend_text = PG_GETARG_TEXT_PP(6);
	if (signed_query_key < 0 || signed_k <= 0 || signed_w <= 0
		|| (signed_wire_format != 3 && signed_wire_format != 4
			&& signed_wire_format != 5)) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq rev3 full-generation fixture parameters are invalid")));
	}
	struct FullGenerationFmgrState {
		std::optional<bioseq::storage::indexing::
			Rev3FullGenerationFixtureResult> result;
	};
	FullGenerationFmgrState* state = nullptr;
	ErrorData* postgres_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char error_detail[512] = "std::exception without detail";
	try {
		state = new FullGenerationFmgrState{};
		const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
		const std::string requested_backend(
			VARDATA_ANY(backend_text), VARSIZE_ANY_EXHDR(backend_text));
		std::string_view canonical_backend = requested_backend;
		if (canonical_backend == "exact") canonical_backend = "exact_simd";
		if (canonical_backend == "paper") {
			canonical_backend = "paper_nthash_simd";
		}
		if (canonical_backend == "vulkan") {
			canonical_backend = "vulkan_segment";
		}
		const auto backend = bioseq::storage::indexing::
			ready_minimizer_fixture_backend_from_name(canonical_backend);
		state->result.emplace(bioseq::storage::indexing::
			read_rev3_pg_full_generation_fixture_impl(
				rel, name, static_cast<std::uint64_t>(signed_query_key),
				static_cast<std::uint16_t>(signed_k),
				static_cast<std::uint16_t>(signed_w),
				static_cast<bioseq::indexing::MinimizerGenerationWireFormat>(
					static_cast<std::uint16_t>(signed_wire_format)),
				backend));
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		postgres_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(error_detail,
			error.what() == nullptr
				? "std::exception without detail" : error.what(),
			sizeof(error_detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	if (postgres_error != nullptr) {
		if (state != nullptr) state->result.reset();
		delete state;
		ReThrowError(postgres_error);
	}
	if (cpp_error || unknown_cpp_error || state == nullptr
		|| !state->result.has_value()) {
		const bool unknown = unknown_cpp_error;
		if (state != nullptr) state->result.reset();
		delete state;
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq rev3 full-generation fixture raised %s",
				unknown ? "an unknown C++ exception" : "a C++ exception"),
			 errdetail_internal("%s", error_detail)));
	}
	struct FullGenerationFmgrOutput {
		char table_name[NAMEDATALEN]{};
		char selected_backend_name[32]{};
		std::uint64_t generation_id = 0;
		std::uint64_t control_bytes = 0;
		std::uint64_t position_bytes = 0;
		std::uint64_t control_checksum = 0;
		std::uint64_t position_checksum = 0;
		bioseq::storage::indexing::MinimizerGenerationReadStats read_stats{};
	};
	static_assert(std::is_trivially_copyable_v<FullGenerationFmgrOutput>);
	FullGenerationFmgrOutput output{};
	strlcpy(output.table_name, state->result->table_name.c_str(),
		sizeof(output.table_name));
	strlcpy(output.selected_backend_name,
		state->result->selected_backend_name.c_str(),
		sizeof(output.selected_backend_name));
	output.generation_id = state->result->generation_id;
	output.control_bytes = state->result->control_bytes;
	output.position_bytes = state->result->position_bytes;
	output.control_checksum = state->result->control_checksum;
	output.position_checksum = state->result->position_checksum;
	output.read_stats = state->result->read_stats;
	state->result.reset();
	delete state;
	const auto sql_bigint = [](std::uint64_t value, const char* field) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			ereport(ERROR,
				(errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
				 errmsg("bioseq rev3 full-generation fixture field exceeds BIGINT"),
				 errdetail("field=%s", field)));
		}
		return static_cast<std::int64_t>(value);
	};
	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("bioseq rev3 full-generation fixture requires a composite result type")));
	}
	tupdesc = BlessTupleDesc(tupdesc);
	bioseq::pg_utils::pg_datum_list<kColumnCount> values{};
	values.set_raw(1, bioseq::pg_utils::pg_datum::from_text_cstring(
		output.table_name));
	values.set_raw(2, bioseq::pg_utils::pg_datum::from_text_cstring(
		output.selected_backend_name));
	values.set(3, sql_bigint(output.generation_id, "generation_id"));
	values.set(4, sql_bigint(output.control_bytes, "control_bytes"));
	values.set(5, sql_bigint(output.position_bytes, "position_bytes"));
	const auto fixed_hex = [](std::uint64_t value, char (&text)[17]) noexcept {
		constexpr char digits[] = "0123456789abcdef";
		text[16] = '\0';
		for (std::size_t index = 16; index > 0; --index) {
			text[index - 1] = digits[value & 0x0fu];
			value >>= 4u;
		}
	};
	char control_checksum[17]{};
	char position_checksum[17]{};
	fixed_hex(output.control_checksum, control_checksum);
	fixed_hex(output.position_checksum, position_checksum);
	values.set_raw(6, bioseq::pg_utils::pg_datum::from_text_cstring(
		control_checksum));
	values.set_raw(7, bioseq::pg_utils::pg_datum::from_text_cstring(
		position_checksum));
	values.set(8, sql_bigint(
		output.read_stats.relation_opens, "relation_opens"));
	values.set(9, sql_bigint(output.read_stats.index_scans, "index_scans"));
	values.set(10, sql_bigint(
		output.read_stats.range_scan_calls, "range_scan_calls"));
	values.set(11, sql_bigint(
		output.read_stats.payload_heap_rows_read, "payload_heap_rows"));
	values.set(12, sql_bigint(
		output.read_stats.payload.delivered_chunks, "delivered_chunks"));
	values.set(13, sql_bigint(
		output.read_stats.payload.delivered_bytes, "delivered_bytes"));
	values.set(14, sql_bigint(
		output.read_stats.full_generation.planned_extents,
		"planned_extents"));
	values.set(15, sql_bigint(
		output.read_stats.full_generation.completed_extents,
		"completed_extents"));
	values.set(16, sql_bigint(
		output.read_stats.full_generation.scan_calls, "scan_calls"));
	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_remove_source_fragment);
Datum bioseq_internal_rev3_test_remove_source_fragment(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* index_text = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	const std::int32_t signed_fragment_ordinal = PG_GETARG_INT32(3);
	if (signed_query_key < 0 || signed_fragment_ordinal <= 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq rev3 source-fragment adversary parameters are invalid")));
	}
	const std::string index_name(
		VARDATA_ANY(index_text), VARSIZE_ANY_EXHDR(index_text));
	bioseq::storage::indexing::rev3_test_remove_source_fragment_impl(
		rel, index_name, static_cast<std::uint64_t>(signed_query_key),
		static_cast<std::uint32_t>(signed_fragment_ordinal));
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_copy_primary_index_shape);
Datum bioseq_internal_rev3_test_copy_primary_index_shape(PG_FUNCTION_ARGS) {
	const Oid table_oid = PG_GETARG_OID(0);
	const Oid donor_index_oid = PG_GETARG_OID(1);
	text* mode_text = PG_GETARG_TEXT_PP(2);
	const std::string_view mode(
		VARDATA_ANY(mode_text), VARSIZE_ANY_EXHDR(mode_text));
	if (mode != "partial" && mode != "expression") {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq rev3 index-shape mode must be partial or expression")));
	}
	ErrorData* postgres_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char error_detail[512] = "std::exception without detail";
	try {
		bioseq::storage::indexing::rev3_test_copy_primary_index_shape_pg(
			table_oid, donor_index_oid, mode == "partial");
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		postgres_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			error_detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(error_detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	// The corruption seam can raise PostgreSQL ERROR through the C++ cleanup
	// bridge.  Translate it back only after that C++ stack has unwound; every
	// local still alive here is trivially destructible.
	if (postgres_error != nullptr) {
		ReThrowError(postgres_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq rev3 index-shape fixture raised a C++ exception"),
			 errdetail_internal("%s", error_detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq rev3 index-shape fixture raised an unknown C++ exception")));
	}
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_rewrite_source_checksum);
Datum bioseq_internal_rev3_test_rewrite_source_checksum(PG_FUNCTION_ARGS) {
	const Oid table_oid = PG_GETARG_OID(0);
	const std::int64_t signed_query_key = PG_GETARG_INT64(1);
	const std::int64_t signed_mask = PG_GETARG_INT64(2);
	if (signed_query_key < 0 || signed_mask == 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq rev3 checksum corruption parameters are invalid")));
	}
	bioseq::storage::indexing::rev3_test_rewrite_source_checksum_pg(
		table_oid, static_cast<std::uint64_t>(signed_query_key),
		std::bit_cast<std::uint64_t>(signed_mask));
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_session_failure);
Datum bioseq_internal_rev3_test_session_failure(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* index_text = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	text* mode_text = PG_GETARG_TEXT_PP(3);
	if (signed_query_key < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq rev3 session fixture query key is negative")));
	}
	ErrorData* postgres_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char error_detail[512] = "std::exception without detail";
	try {
		bioseq::storage::indexing::exercise_rev3_pg_session_failure_impl(
			rel,
			std::string_view{
				VARDATA_ANY(index_text),
				static_cast<std::size_t>(VARSIZE_ANY_EXHDR(index_text))},
			static_cast<std::uint64_t>(signed_query_key),
			std::string_view{
				VARDATA_ANY(mode_text),
				static_cast<std::size_t>(VARSIZE_ANY_EXHDR(mode_text))});
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		postgres_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			error_detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(error_detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	// The fixture deliberately converts PostgreSQL ERROR into C++ so its
	// snapshot, relation, permit, and heap owners unwind. Re-enter PostgreSQL
	// only after that C++ stack is gone; every remaining local is trivial.
	if (postgres_error != nullptr) {
		ReThrowError(postgres_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq rev3 session fixture raised a C++ exception"),
			 errdetail_internal("%s", error_detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq rev3 session fixture raised an unknown C++ exception")));
	}
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_generation_storage_state);
Datum bioseq_internal_rev3_test_generation_storage_state(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* index_text = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	text* operation_text = PG_GETARG_TEXT_PP(3);
	if (signed_query_key < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("bioseq generation-storage fixture query key is negative")));
	}
	const std::string_view index_name{
		VARDATA_ANY(index_text),
		static_cast<std::size_t>(VARSIZE_ANY_EXHDR(index_text))};
	const std::string_view operation{
		VARDATA_ANY(operation_text),
		static_cast<std::size_t>(VARSIZE_ANY_EXHDR(operation_text))};
	static_assert(std::is_trivially_destructible_v<
		bioseq::storage::indexing::Rev3GenerationStorageStateResult>);
	bioseq::storage::indexing::Rev3GenerationStorageStateResult state{};
	ErrorData* postgres_error = nullptr;
	bool cpp_error = false;
	bool unknown_cpp_error = false;
	char error_detail[512] = "std::exception without detail";
	try {
		state = bioseq::storage::indexing::
			exercise_rev3_generation_storage_state_impl(
				rel, index_name,
				static_cast<std::uint64_t>(signed_query_key), operation);
	} catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
		postgres_error = error.release_error_data();
	} catch (const std::exception& error) {
		strlcpy(
			error_detail,
			error.what() == nullptr
				? "std::exception without detail"
				: error.what(),
			sizeof(error_detail));
		cpp_error = true;
	} catch (...) {
		unknown_cpp_error = true;
	}
	// The fixture translates PostgreSQL ERROR into C++ while its storage and
	// snapshot owners unwind. Re-enter PostgreSQL only after that stack is gone;
	// every local still alive here is trivially destructible.
	if (postgres_error != nullptr) {
		ReThrowError(postgres_error);
	}
	if (cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq generation-storage fixture raised a C++ exception"),
			 errdetail_internal("%s", error_detail)));
	}
	if (unknown_cpp_error) {
		ereport(ERROR,
			(errcode(ERRCODE_INTERNAL_ERROR),
			 errmsg("bioseq generation-storage fixture raised an unknown C++ exception")));
	}

	TupleDesc tupdesc = nullptr;
	if (get_call_result_type(fcinfo, nullptr, &tupdesc) != TYPEFUNC_COMPOSITE) {
		ereport(ERROR,
			(errcode(ERRCODE_DATATYPE_MISMATCH),
			 errmsg("bioseq generation-storage fixture requires a composite result type")));
	}
	tupdesc = BlessTupleDesc(tupdesc);
	bioseq::pg_utils::pg_datum_list<7> values{};
	const auto set_oid_or_null = [&values](int attnum, Oid value) {
		if (OidIsValid(value)) values.set_oid(attnum, value);
		else values.set_null(attnum);
	};
	const auto set_bool_or_null = [&values](
			int attnum, const std::optional<bool>& value) {
		if (value.has_value()) values.set(attnum, *value);
		else values.set_null(attnum);
	};
	set_oid_or_null(1, state.before_oid);
	set_oid_or_null(2, state.operation_oid);
	set_oid_or_null(3, state.after_oid);
	set_bool_or_null(4, state.same_handle_rebound);
	set_bool_or_null(5, state.generation_schema_after);
	set_bool_or_null(6, state.chunk_cache_hit);
	set_bool_or_null(7, state.assembled_cache_hit);
	HeapTuple tuple = values.form_tuple(tupdesc);
	PG_RETURN_DATUM(
		bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw());
}
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_build_ready_mpf_generation);

} // extern "C"

template <std::size_t ColumnCount>
struct ReadyMinimizerFixtureFmgrState {
    std::string index_name;
    std::string backend_name;
    std::optional<bioseq::storage::indexing::
        ReadyMinimizerGenerationCoordinatorFixtureResult> fixture;
    std::array<std::string, 5> rendered_text;
    bioseq::pg_utils::pg_datum_list<ColumnCount> values{};
    std::exception_ptr cpp_error;
    Datum result = 0;
};

template <class State>
[[noreturn]] void rethrow_ready_minimizer_fixture_cpp_error(
        State*& state,
        const char* fixture_kind) {
    char detail[256] = "unknown C++ exception";
    ErrorData* postgres_error = nullptr;
    try {
        std::rethrow_exception(state->cpp_error);
    } catch (bioseq::pg_utils::PostgresErrorAsCpp& error) {
        postgres_error = error.release_error_data();
    } catch (const std::exception& error) {
        strlcpy(detail, error.what(), sizeof(detail));
    } catch (...) {
    }
    delete state;
    state = nullptr;
    if (postgres_error != nullptr) {
        ReThrowError(postgres_error);
    }
    ereport(ERROR,
        (errcode(ERRCODE_INTERNAL_ERROR),
         errmsg("READY %s fixture raised a C++ exception", fixture_kind),
         errdetail_internal("%s", detail)));
    pg_unreachable();
}

[[noreturn]] void ready_minimizer_fixture_error_as_cpp(
		int sqlstate,
		std::string_view message,
		std::string_view detail = {}) {
	bioseq::pg_utils::invoke_pg_as_cpp([sqlstate, message, detail] {
		if (detail.empty()) {
			ereport(ERROR,
				(errcode(sqlstate),
				 errmsg("%.*s", static_cast<int>(message.size()),
					message.data())));
		}
		ereport(ERROR,
			(errcode(sqlstate),
			 errmsg("%.*s", static_cast<int>(message.size()),
				message.data()),
			 errdetail("%.*s", static_cast<int>(detail.size()),
				detail.data())));
	});
	pg_unreachable();
}

extern "C" {

Datum bioseq_internal_rev3_test_build_ready_mpf_generation(PG_FUNCTION_ARGS) {
	constexpr std::size_t kColumnCount = 45;
	const Oid rel = PG_GETARG_OID(0);
	text* index_text = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	text* backend_text = PG_GETARG_TEXT_PP(3);
	if (signed_query_key < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("READY MPF fixture row number is negative")));
	}
	using State = ReadyMinimizerFixtureFmgrState<kColumnCount>;
	State* volatile_state = new (std::nothrow) State{};
	if (volatile_state == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("READY MPF fixture could not allocate cleanup state")));
	}
	State* state = volatile_state;
	try {
			state->index_name.assign(
				VARDATA_ANY(index_text), VARSIZE_ANY_EXHDR(index_text));
			state->backend_name.assign(
				VARDATA_ANY(backend_text), VARSIZE_ANY_EXHDR(backend_text));
			if (state->index_name != bioseq::indexing::
					MINIMIZERS_PER_FRAGMENT_INDEX_NAME) {
				ready_minimizer_fixture_error_as_cpp(
					ERRCODE_INVALID_PARAMETER_VALUE,
					"READY MPF fixture requires minimizers_per_fragment");
			}
			state->fixture.emplace(bioseq::storage::indexing::
				build_ready_minimizer_generation_coordinator_fixture_impl(
					rel, state->index_name,
					static_cast<std::uint64_t>(signed_query_key),
					state->backend_name));
			const auto& fixture = *state->fixture;
	const auto& publication = fixture.sealed_publication();
	const auto* execution_ptr = std::get_if<
		bioseq::indexing::MpfGenerationExecutionEvidence>(
			&publication.execution());
	if (execution_ptr == nullptr) {
		ready_minimizer_fixture_error_as_cpp(
			ERRCODE_DATA_CORRUPTED,
			"READY MPF fixture returned non-MPF evidence");
	}
	const auto& execution = *execution_ptr;

	const auto sql_bigint = [](std::uint64_t value, const char* field) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			ready_minimizer_fixture_error_as_cpp(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				"READY MPF fixture field exceeds BIGINT",
				std::string("field=") + field);
		}
		return static_cast<std::int64_t>(value);
	};
	const auto sql_integer = [](std::uint64_t value, const char* field) {
		if (value > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int32_t>::max)())) {
			ready_minimizer_fixture_error_as_cpp(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				"READY MPF fixture field exceeds INTEGER",
				std::string("field=") + field);
		}
		return static_cast<std::int32_t>(value);
	};
	TupleDesc tupdesc = nullptr;
	if (bioseq::pg_utils::invoke_pg_as_cpp(
			[fcinfo, &tupdesc] {
				return get_call_result_type(fcinfo, nullptr, &tupdesc);
			})
			!= TYPEFUNC_COMPOSITE) {
		ready_minimizer_fixture_error_as_cpp(
			ERRCODE_DATATYPE_MISMATCH,
			"READY MPF fixture requires a composite result type");
			}
			tupdesc = bioseq::pg_utils::invoke_pg_as_cpp(
				[tupdesc] { return BlessTupleDesc(tupdesc); });
	auto& values = state->values;
	const auto set_text = [&values](std::size_t attno, const char* value) {
		bioseq::pg_utils::invoke_pg_as_cpp([&values, attno, value] {
			values.set_raw(
				attno, bioseq::pg_utils::pg_datum::from_text_cstring(value));
		});
	};
	set_text(1, fixture.table_name.c_str());
	values.set(2, fixture.column_attno);
	values.set(3, sql_bigint(fixture.sequence_rowno, "sequence_rowno"));
	state->rendered_text[0] = fixed_hex_u64(fixture.locator.source_epoch);
	set_text(4, state->rendered_text[0].c_str());
	values.set(5, sql_bigint(
		fixture.locator.num_fragments, "source_num_fragments"));
	values.set(6, sql_bigint(
		fixture.locator.total_bp_size, "source_total_bp"));
	set_text(7, fixture.selected_backend_name.c_str());
	values.set(8, static_cast<std::int16_t>(fixture.manifest.wire_format));
	values.set(9, publication.published());
	values.set(10, fixture.head_manifest_verified);
	values.set(11, sql_bigint(publication.generation_id(), "generation_id"));
	state->rendered_text[1] = fixed_hex_u64(publication.source_identity());
	set_text(12, state->rendered_text[1].c_str());
	state->rendered_text[2] = fixed_hex_u64(
		publication.source_content_checksum());
	set_text(13, state->rendered_text[2].c_str());
	state->rendered_text[3] = fixed_hex_u64(
		publication.compatibility_checksum());
	set_text(14, state->rendered_text[3].c_str());
	values.set(15, sql_bigint(
		fixture.manifest.target_length, "manifest_target_length"));
	values.set(16, static_cast<std::int16_t>(fixture.manifest.kmer_size));
	values.set(17, static_cast<std::int16_t>(fixture.manifest.window_size));
	values.set(18, sql_integer(
		fixture.manifest.minimizer_scheme_id, "minimizer_scheme_id"));
	values.set(19, sql_integer(
		fixture.manifest.minimizer_scheme_version,
		"minimizer_scheme_version"));
	values.set(20, sql_integer(
		fixture.manifest.gpu_abi_version, "gpu_abi_version"));
	values.set(21, sql_bigint(
		fixture.manifest.position_segment_span_bp,
		"position_segment_span_bp"));
	values.set(22, sql_integer(
		fixture.manifest.position_segment_count,
		"position_segment_count"));
	values.set(23, sql_bigint(publication.writer_rows(), "writer_rows"));
	values.set(24, sql_bigint(
		publication.writer_batches(), "writer_batches"));
	values.set(25, sql_bigint(publication.control_count(), "control_count"));
	values.set(26, sql_bigint(
		publication.position_count(), "position_count"));
	values.set(27, sql_bigint(
		execution.producer_scan_count, "producer_scan_count"));
	values.set(28, sql_bigint(
		execution.geometry_retry_count, "geometry_retry_count"));
	values.set(29, sql_bigint(execution.short_core_runs, "short_core_runs"));
	values.set(30, sql_bigint(execution.scalar_core_runs, "scalar_core_runs"));
	values.set(31, sql_bigint(
		execution.exact_simd_core_runs, "exact_simd_core_runs"));
	values.set(32, sql_bigint(
		execution.paper_nthash_core_runs, "paper_nthash_core_runs"));
	values.set(33, sql_bigint(execution.vulkan_core_runs, "vulkan_core_runs"));
	values.set(34, execution.accepted());
	values.set(35, execution.exact_simd_proof_present);
	values.set(36, execution.exact_simd_proof_present
		&& execution.exact_simd_proof.accepted(/*require_work=*/true));
	values.set(37, sql_bigint(
		execution.exact_simd_proof.vector_batches,
		"exact_vector_batches"));
	values.set(38, sql_bigint(
		execution.exact_simd_proof.scalar_backend_branches,
		"exact_scalar_backend_branches"));
	values.set(39, execution.packed_paper_proof_present);
	state->rendered_text[4] =
		bioseq::indexing::packed_paper_nthash_index_status_name(
			execution.packed_paper_status);
	set_text(40, state->rendered_text[4].c_str());
	values.set(41, execution.packed_paper_proof_present
		&& execution.packed_paper_status
			== bioseq::indexing::PackedPaperNthashIndexStatus::ok
		&& execution.packed_paper_proof.accepted(
			/*require_nonempty_vector_work=*/true));
	values.set(42, sql_bigint(
		execution.packed_paper_proof.packed_decode_batches,
		"paper_packed_decode_batches"));
	values.set(43, sql_bigint(
		execution.packed_paper_proof.fallback_calls,
		"paper_fallback_calls"));
	values.set(44, sql_bigint(
		execution.vulkan_proof.completed_roundtrips,
		"vulkan_completed_roundtrips"));
	values.set(45, sql_bigint(
		execution.vulkan_proof.downloaded_minimizers,
		"vulkan_downloaded_minimizers"));

	HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
		[&values, tupdesc] { return values.form_tuple(tupdesc); });
	state->result = bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw();
	} catch (...) {
		state->cpp_error = std::current_exception();
	}
	if (state->cpp_error != nullptr) {
		rethrow_ready_minimizer_fixture_cpp_error(state, "MPF");
	}
	const Datum result = state->result;
	delete state;
	PG_RETURN_DATUM(result);
}
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_rev3_test_build_ready_mps_generation);
Datum bioseq_internal_rev3_test_build_ready_mps_generation(PG_FUNCTION_ARGS) {
	constexpr std::size_t kColumnCount = 72;
	const Oid rel = PG_GETARG_OID(0);
	text* index_text = PG_GETARG_TEXT_PP(1);
	const std::int64_t signed_query_key = PG_GETARG_INT64(2);
	text* backend_text = PG_GETARG_TEXT_PP(3);
	if (signed_query_key < 0) {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("READY MPS fixture row number is negative")));
	}
	using State = ReadyMinimizerFixtureFmgrState<kColumnCount>;
	State* volatile_state = new (std::nothrow) State{};
	if (volatile_state == nullptr) {
		ereport(ERROR,
			(errcode(ERRCODE_OUT_OF_MEMORY),
			 errmsg("READY MPS fixture could not allocate cleanup state")));
	}
	State* state = volatile_state;
	try {
			state->index_name.assign(
				VARDATA_ANY(index_text), VARSIZE_ANY_EXHDR(index_text));
			state->backend_name.assign(
				VARDATA_ANY(backend_text), VARSIZE_ANY_EXHDR(backend_text));
			if (state->index_name != bioseq::indexing::
					MINIMIZERS_PER_SEQUENCE_INDEX_NAME) {
				ready_minimizer_fixture_error_as_cpp(
					ERRCODE_INVALID_PARAMETER_VALUE,
					"READY MPS fixture requires minimizers_per_sequence");
			}
			state->fixture.emplace(bioseq::storage::indexing::
				build_ready_minimizer_generation_coordinator_fixture_impl(
					rel, state->index_name,
					static_cast<std::uint64_t>(signed_query_key),
					state->backend_name));
			const auto& fixture = *state->fixture;
	const auto& publication = fixture.sealed_publication();
	const auto* execution_ptr = std::get_if<
		bioseq::indexing::MpsGenerationExecutionEvidence>(
			&publication.execution());
	if (execution_ptr == nullptr) {
		ready_minimizer_fixture_error_as_cpp(
			ERRCODE_DATA_CORRUPTED,
			"READY MPS fixture returned non-MPS evidence");
	}
	const auto& execution = *execution_ptr;

	const auto sql_bigint = [](std::uint64_t value, const char* field) {
		if (!bioseq::indexing::sql_bigint_can_represent(value)) {
			ready_minimizer_fixture_error_as_cpp(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				"READY MPS fixture field exceeds BIGINT",
				std::string("field=") + field);
		}
		return static_cast<std::int64_t>(value);
	};
	const auto sql_integer = [](std::uint64_t value, const char* field) {
		if (value > static_cast<std::uint64_t>(
				(std::numeric_limits<std::int32_t>::max)())) {
			ready_minimizer_fixture_error_as_cpp(
				ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE,
				"READY MPS fixture field exceeds INTEGER",
				std::string("field=") + field);
		}
		return static_cast<std::int32_t>(value);
	};
	TupleDesc tupdesc = nullptr;
	if (bioseq::pg_utils::invoke_pg_as_cpp(
			[fcinfo, &tupdesc] {
				return get_call_result_type(fcinfo, nullptr, &tupdesc);
			})
			!= TYPEFUNC_COMPOSITE) {
		ready_minimizer_fixture_error_as_cpp(
			ERRCODE_DATATYPE_MISMATCH,
			"READY MPS fixture requires a composite result type");
	}
	tupdesc = bioseq::pg_utils::invoke_pg_as_cpp(
		[tupdesc] { return BlessTupleDesc(tupdesc); });
	auto& values = state->values;
	const auto set_text = [&values](std::size_t attno, const char* value) {
		bioseq::pg_utils::invoke_pg_as_cpp([&values, attno, value] {
			values.set_raw(
				attno, bioseq::pg_utils::pg_datum::from_text_cstring(value));
		});
	};
	set_text(1, fixture.table_name.c_str());
	values.set(2, fixture.column_attno);
	values.set(3, sql_bigint(fixture.sequence_rowno, "sequence_rowno"));
	state->rendered_text[0] = fixed_hex_u64(fixture.locator.source_epoch);
	set_text(4, state->rendered_text[0].c_str());
	values.set(5, sql_bigint(
		fixture.locator.num_fragments, "source_num_fragments"));
	values.set(6, sql_bigint(
		fixture.locator.total_bp_size, "source_total_bp"));
	set_text(7, fixture.selected_backend_name.c_str());
	values.set(8, static_cast<std::int16_t>(fixture.manifest.wire_format));
	values.set(9, publication.published());
	values.set(10, fixture.head_manifest_verified);
	values.set(11, sql_bigint(publication.generation_id(), "generation_id"));
	state->rendered_text[1] = fixed_hex_u64(publication.source_identity());
	set_text(12, state->rendered_text[1].c_str());
	state->rendered_text[2] = fixed_hex_u64(
		publication.source_content_checksum());
	set_text(13, state->rendered_text[2].c_str());
	state->rendered_text[3] = fixed_hex_u64(
		publication.compatibility_checksum());
	set_text(14, state->rendered_text[3].c_str());
	values.set(15, sql_bigint(
		fixture.manifest.target_length, "manifest_target_length"));
	values.set(16, static_cast<std::int16_t>(fixture.manifest.kmer_size));
	values.set(17, static_cast<std::int16_t>(fixture.manifest.window_size));
	values.set(18, sql_integer(
		fixture.manifest.minimizer_scheme_id, "minimizer_scheme_id"));
	values.set(19, sql_integer(
		fixture.manifest.minimizer_scheme_version,
		"minimizer_scheme_version"));
	values.set(20, sql_integer(
		fixture.manifest.gpu_abi_version, "gpu_abi_version"));
	values.set(21, sql_bigint(
		fixture.manifest.position_segment_span_bp,
		"position_segment_span_bp"));
	values.set(22, sql_integer(
		fixture.manifest.position_segment_count,
		"position_segment_count"));
	values.set(23, sql_bigint(publication.writer_rows(), "writer_rows"));
	values.set(24, sql_bigint(
		publication.writer_batches(), "writer_batches"));
	values.set(25, sql_bigint(publication.control_count(), "control_count"));
	values.set(26, sql_bigint(publication.position_count(), "position_count"));
	values.set(27, sql_bigint(execution.spill_run_count, "spill_run_count"));
	values.set(28, sql_bigint(
		execution.spilled_event_count, "spilled_event_count"));
	values.set(29, sql_bigint(
		execution.spill_operation_epoch, "spill_operation_epoch"));
	values.set(30, sql_bigint(
		execution.spill_storage_binding.generation_token,
		"spill_generation_token"));
	values.set(31, sql_bigint(
		execution.spill_storage_binding.mutation_version,
		"spill_mutation_version"));
	values.set(32, sql_bigint(
		execution.spill_qualification_scan_count,
		"spill_qualification_scan_count"));
	values.set(33, sql_bigint(
		execution.spill_qualification_merge_count,
		"spill_qualification_merge_count"));
	values.set(34, sql_bigint(
		execution.configured_core_span_bp, "configured_core_span_bp"));
	values.set(35, sql_bigint(
		execution.peak_core_span_bp, "peak_core_span_bp"));
	values.set(36, sql_bigint(execution.peak_core_events, "peak_core_events"));
	values.set(37, sql_bigint(execution.short_core_runs, "short_core_runs"));
	values.set(38, sql_bigint(execution.scalar_core_runs, "scalar_core_runs"));
	values.set(39, sql_bigint(
		execution.exact_simd_core_runs, "exact_simd_core_runs"));
	values.set(40, sql_bigint(
		execution.paper_nthash_core_runs, "paper_nthash_core_runs"));
	values.set(41, sql_bigint(execution.vulkan_core_runs, "vulkan_core_runs"));
	values.set(42, execution.accepted());
	values.set(43, execution.exact_simd_proof_present);
	values.set(44, execution.exact_simd_proof_present
		&& execution.exact_simd_proof.accepted(/*require_work=*/true));
	values.set(45, sql_bigint(
		execution.exact_simd_proof.vector_batches, "exact_vector_batches"));
	values.set(46, sql_bigint(
		execution.exact_simd_proof.scalar_backend_branches,
		"exact_scalar_backend_branches"));
	values.set(47, execution.packed_paper_proof_present);
	state->rendered_text[4] =
		bioseq::indexing::packed_paper_nthash_index_status_name(
			execution.packed_paper_status);
	set_text(48, state->rendered_text[4].c_str());
	values.set(49, execution.packed_paper_proof_present
		&& execution.packed_paper_status
			== bioseq::indexing::PackedPaperNthashIndexStatus::ok
		&& execution.packed_paper_proof.accepted(
			/*require_nonempty_vector_work=*/true));
	values.set(50, sql_bigint(
		execution.packed_paper_proof.packed_decode_batches,
		"paper_packed_decode_batches"));
	values.set(51, sql_bigint(
		execution.packed_paper_proof.fallback_calls,
		"paper_fallback_calls"));
	values.set(52, sql_bigint(
		execution.vulkan_proof.completed_roundtrips,
		"vulkan_completed_roundtrips"));
	values.set(53, sql_bigint(
		execution.vulkan_proof.downloaded_minimizers,
		"vulkan_downloaded_minimizers"));
	values.set(54, sql_bigint(execution.replay_scan_count,
		"replay_scan_count"));
	values.set(55, sql_bigint(execution.replay_merge_pass_count,
		"replay_merge_pass_count"));
	values.set(56, sql_bigint(execution.replay_reroute_pass_count,
		"replay_reroute_pass_count"));
	values.set(57, sql_bigint(execution.replay_completed_scan_count,
		"replay_completed_scan_count"));
	values.set(58, sql_bigint(execution.replay_failed_scan_count,
		"replay_failed_scan_count"));
	values.set(59, sql_bigint(
		execution.replay_binding_checked_failed_scan_count,
		"replay_binding_checked_failed_scan_count"));
	values.set(60, sql_bigint(
		execution.replay_illegal_span_transition_count,
		"replay_illegal_span_transition_count"));
	values.set(61, sql_bigint(
		execution.replay_first_completed_event_count,
		"replay_first_completed_event_count"));
	values.set(62, sql_bigint(
		execution.replay_last_completed_event_count,
		"replay_last_completed_event_count"));
	values.set(63, sql_bigint(
		execution.replay_final_span_bp, "replay_final_span_bp"));
	values.set(64, sql_bigint(
		execution.replay_final_storage_binding.generation_token,
		"replay_final_generation_token"));
	values.set(65, sql_bigint(
		execution.replay_final_storage_binding.mutation_version,
		"replay_final_mutation_version"));
	values.set(66, sql_bigint(
		execution.replay_visited_event_count,
		"replay_visited_event_count"));
	values.set(67, sql_bigint(
		execution.manifest_position_count,
		"evidence_manifest_position_count"));
	values.set(68, sql_bigint(
		execution.manifest_position_segment_span_bp,
		"evidence_manifest_position_segment_span_bp"));
	values.set(69, sql_bigint(
		publication.mps_spill_generation_token(),
		"release_generation_token"));
	values.set(70, sql_bigint(
		publication.mps_spill_mutation_before_release(),
		"release_mutation_before"));
	values.set(71, sql_bigint(
		publication.mps_spill_mutation_after_release(),
		"release_mutation_after"));
	values.set(72, publication.mps_spill_resources_released());

	HeapTuple tuple = bioseq::pg_utils::invoke_pg_as_cpp(
		[&values, tupdesc] { return values.form_tuple(tupdesc); });
	state->result = bioseq::pg_utils::pg_datum::from_heap_tuple(tuple).raw();
	} catch (...) {
		state->cpp_error = std::current_exception();
	}
	if (state->cpp_error != nullptr) {
		rethrow_ready_minimizer_fixture_cpp_error(state, "MPS");
	}
	const Datum result = state->result;
	delete state;
	PG_RETURN_DATUM(result);
}
CXX_PG_FUNCTION_INFO_V1(bioseq_index_distinct_sub_keys);
Datum bioseq_index_distinct_sub_keys(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::int64_t qk = PG_GETARG_INT64(2);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	PG_RETURN_INT64(bioseq::storage::indexing::index_distinct_sub_keys_impl(rel, name, qk));
}
CXX_PG_FUNCTION_INFO_V1(bioseq_drop_all_index_tables);
Datum bioseq_drop_all_index_tables(PG_FUNCTION_ARGS) {
	bioseq::storage::indexing::drop_all_index_tables_impl();
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_set_index_policy);
Datum bioseq_set_index_policy(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	text* pol = PG_GETARG_TEXT_PP(2);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const std::string policy(VARDATA_ANY(pol), VARSIZE_ANY_EXHDR(pol));
	const bool uoi = (policy == "UPDATE_ON_INSERT");
	const int st = bioseq::storage::indexing::set_index_policy_impl(rel, name, uoi);
	if (st == 1) ereport(ERROR, (errmsg("bioseq.set_index_policy: unknown index '%s'", name.c_str())));
	if (st == 2) ereport(ERROR, (errmsg("bioseq.set_index_policy: index '%s' cannot be UPDATE_ON_INSERT", name.c_str())));
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_set_active_read_index);
Datum bioseq_set_active_read_index(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const int st = bioseq::storage::indexing::set_active_read_index_impl(rel, name);
	if (st == 1) ereport(ERROR, (errmsg("bioseq.set_active_read_index: unknown index '%s'", name.c_str())));
	if (st == 2) ereport(ERROR, (errmsg("bioseq.set_active_read_index: a CHUNKED_PER_TABLE index cannot be the active read index")));
	PG_RETURN_VOID();
}
CXX_PG_FUNCTION_INFO_V1(bioseq_column_row_number);
Datum bioseq_column_row_number(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	if (!IsBioseqColumnTableAmTable(rel))
		ereport(ERROR, (errmsg("bioseq.column_row_number: relation is not a USING bioseq table")));
	/* `ctid tid` is a by-reference type; PG_GETARG_DATUM holds a pointer to the
	 * 6-byte ItemPointerData. Copy by value before handing it to the extern
	 * tid_to_row_number (pure ctid arithmetic, access_method.cppm). */
	ItemPointer ctid = reinterpret_cast<ItemPointer>(PG_GETARG_DATUM(1));
	if (ctid == nullptr)
		ereport(ERROR, (errmsg("bioseq.column_row_number: NULL ctid")));
	ItemPointerData tid = *ctid;
	PG_RETURN_INT64(static_cast<std::int64_t>(tid_to_row_number(tid)));
}

/* DOC0279 §12.2/§16.2 — TEST-ONLY controlled-corruption helpers. They resolve
 * the dynamic seqidx_* shard for (rel, name) internally and write / delete a
 * single chunk row, so the integration tests can pin reserved-sentinel
 * filtering, missing-manifest -> recompute, and present-manifest/missing-
 * fragment corruption detection. Harmless in production (explicit admin verbs;
 * never called by the engine). The TestTableCountIndex registry entry IS gated
 * by BIOSEQ_TEST_INDEXES (§14); these byte-level hooks are not, because they
 * change no production behaviour. */
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_arm_inline_sequence_pg_failure);
Datum bioseq_internal_arm_inline_sequence_pg_failure(PG_FUNCTION_ARGS) {
	text* mode_text = PG_GETARG_TEXT_PP(0);
	const std::string_view mode{
		VARDATA_ANY(mode_text),
		static_cast<std::size_t>(VARSIZE_ANY_EXHDR(mode_text))};
	using Failure = bioseq::storage::indexing::InlineSequencePgLeafFailure;
	Failure failure = Failure::none;
	if (mode == "relation_lookup") {
		failure = Failure::relation_lookup;
	} else if (mode == "tuple_read") {
		failure = Failure::tuple_read;
	} else if (mode == "detoast") {
		failure = Failure::detoast;
	} else if (mode == "close") {
		failure = Failure::close;
	} else {
		ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unknown inline sequence PostgreSQL failure point"),
			 errdetail("failure_point=%.*s",
				static_cast<int>(mode.size()), mode.data())));
	}
	bioseq::storage::indexing::arm_inline_sequence_pg_leaf_failure(failure);
	PG_RETURN_VOID();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_test_write_index_chunk);
Datum bioseq_test_write_index_chunk(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const std::int64_t query_key = PG_GETARG_INT64(2);
	const std::int64_t sub_key   = PG_GETARG_INT64(3);
	const std::int32_t chunk_no  = PG_GETARG_INT32(4);
	Datum payload = bioseq::pg_utils::pg_datum::from_pointer(PG_GETARG_BYTEA_PP(5)).raw();   // detoast before SPI
	bool wrote = false;
	for (const std::string& pfx : bioseq::storage::indexing::seqidx_prefixes_for(rel, name)) {
		auto shards = bioseq::storage::indexing::shard_relnames_with_prefix(pfx);
		if (shards.empty()) continue;
		/* prefer the shard already holding query_key; else the first shard. */
		std::string target = shards.front();
		for (const std::string& sh : shards) {
			if (bioseq::storage::indexing::count_in_shard(sh,
					fmt::format("WHERE query_key={}", static_cast<long long>(query_key))) > 0) {
				target = sh;
				break;
			}
		}
		if (SPI_connect() != SPI_OK_CONNECT) break;
		std::string sql = fmt::format(
			"INSERT INTO bioseq_internal.{} (query_key, sub_key, chunk_no, chunk_bytes, blob) "
			"VALUES ($1,$2,$3,octet_length($4),$4) "
			"ON CONFLICT (query_key, sub_key, chunk_no) DO UPDATE SET chunk_bytes=EXCLUDED.chunk_bytes, blob=EXCLUDED.blob",
			quote_identifier(target.c_str()));
		Oid argt[4] = { INT8OID, INT8OID, INT4OID, BYTEAOID };
		Datum argv[4] = {
			bioseq::pg_utils::pg_datum::from(query_key).raw(),
			bioseq::pg_utils::pg_datum::from(sub_key).raw(),
			bioseq::pg_utils::pg_datum::from(chunk_no).raw(),
			payload
		};
		SPI_execute_with_args(sql.c_str(), 4, argt, argv, nullptr, false, 0);
		SPI_finish();
		wrote = true;
		break;
	}
	if (!wrote)
		ereport(ERROR, (errmsg("bioseq.test_write_index_chunk: no seqidx shard for (%s, %s)",
			get_rel_name(rel), name.c_str())));
	PG_RETURN_VOID();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_test_delete_index_manifest);
Datum bioseq_test_delete_index_manifest(PG_FUNCTION_ARGS) {
	const Oid rel = PG_GETARG_OID(0);
	text* nm = PG_GETARG_TEXT_PP(1);
	const std::string name(VARDATA_ANY(nm), VARSIZE_ANY_EXHDR(nm));
	const std::int64_t query_key = PG_GETARG_INT64(2);
	const std::int64_t dir = static_cast<std::int64_t>(bioseq::indexing::DIRECTORY_SUB_KEY);
	for (const std::string& pfx : bioseq::storage::indexing::seqidx_prefixes_for(rel, name)) {
		for (const std::string& sh : bioseq::storage::indexing::shard_relnames_with_prefix(pfx)) {
			if (SPI_connect() != SPI_OK_CONNECT) continue;
			std::string sql = fmt::format(
				"DELETE FROM bioseq_internal.{} WHERE query_key={} AND sub_key={}",
				quote_identifier(sh.c_str()), static_cast<long long>(query_key), static_cast<long long>(dir));
			SPI_execute(sql.c_str(), false, 0);
			SPI_finish();
		}
	}
	PG_RETURN_VOID();
}

/* DOC0377 §7/§17.4 — fixed, test-build-only source-domain fault and lifetime
 * diagnostics.  SQL declarations are always present so extension schemas do
 * not differ; production builds fail closed when a test hook is invoked. */
CXX_PG_FUNCTION_INFO_V1(bioseq_internal_arm_sequence_source_failure);
Datum bioseq_internal_arm_sequence_source_failure(PG_FUNCTION_ARGS) {
#ifdef BIOSEQ_TEST_INDEXES
  text *point_text = PG_GETARG_TEXT_PP(0);
  const std::string_view point{
      VARDATA_ANY(point_text),
      static_cast<std::size_t>(VARSIZE_ANY_EXHDR(point_text))};
  if (point == "facade_resident_probe") {
    arm_pg_sequence_facade_test_poison(
        PgSequenceFacadeTestPoison::resident_probe);
    PG_RETURN_VOID();
  }
  if (point == "facade_materialization") {
    arm_pg_sequence_facade_test_poison(
        PgSequenceFacadeTestPoison::materialization);
    PG_RETURN_VOID();
  }
  if (point == "facade_contiguous_access") {
    arm_pg_sequence_facade_test_poison(
        PgSequenceFacadeTestPoison::contiguous_access);
    PG_RETURN_VOID();
  }
  using Failure = bioseq::storage::PgSequenceSourceFailure;
  constexpr std::array<std::pair<std::string_view, Failure>, 13> points{{
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
      {"close_before_before_locks", Failure::close_before_before_locks},
      {"close_before_locks", Failure::close_before_locks},
      {"close_before_after_locks", Failure::close_before_after_locks},
      {"read_before_scanner", Failure::read_before_scanner},
  }};
  for (const auto &[name, failure] : points) {
    if (name == point) {
      bioseq::storage::arm_pg_sequence_source_failure(failure);
      PG_RETURN_VOID();
    }
  }
  ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                  errmsg("unknown bioseq sequence source-domain failure point"),
                  errdetail("failure_point=%.*s",
                            static_cast<int>(point.size()), point.data())));
#else
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("bioseq sequence source-domain fault injection requires a test build")));
#endif
	PG_RETURN_VOID();
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_disarm_sequence_source_failure);
Datum bioseq_internal_disarm_sequence_source_failure(PG_FUNCTION_ARGS) {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::disarm_pg_sequence_source_failure();
	PG_RETURN_VOID();
#else
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("bioseq sequence source-domain fault injection requires a test build")));
	PG_RETURN_VOID();
#endif
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_reset_sequence_source_counters);
Datum bioseq_internal_reset_sequence_source_counters(PG_FUNCTION_ARGS) {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::disarm_pg_sequence_source_failure();
	bioseq::storage::reset_pg_sequence_source_debug_counters();
	PG_RETURN_VOID();
#else
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("bioseq sequence source-domain diagnostics require a test build")));
	PG_RETURN_VOID();
#endif
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_sequence_source_counters);
Datum bioseq_internal_sequence_source_counters(PG_FUNCTION_ARGS) {
#ifdef BIOSEQ_TEST_INDEXES
  const auto c = bioseq::storage::pg_sequence_source_debug_counters();
  const std::string encoded = fmt::format(
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
      "\"facade_source_stream_callbacks\":{},"
      "\"facade_source_resident_probes\":{},"
      "\"facade_declared_materializations\":{},"
      "\"facade_completed_materializations\":{},"
      "\"facade_poison_hits\":{},"
      "\"live_registries\":{},\"live_providers\":{},"
      "\"live_domains\":{},\"live_leases\":{},"
      "\"live_cache_sets\":{},\"live_snapshots\":{}}}",
      c.registries_created, c.registries_destroyed, c.providers_created,
      c.providers_destroyed, c.scan_providers_created,
      c.standalone_providers_created, c.domains_created, c.domains_reused,
      c.domains_destroyed, c.cache_sets_created, c.cache_sets_destroyed,
      c.snapshots_registered, c.snapshots_unregistered,
      c.scanner_entries_created, c.scanner_entry_hits, c.leases_acquired,
      c.leases_released, c.provider_closed_with_upper_leases,
      c.last_upper_lease_after_provider_end, c.cleanup_commit, c.cleanup_abort,
      c.sweeps, c.swept_domains, c.subxact_promotions, c.subxact_aborts,
      c.pre_prepare_rejections, c.release_before_locks, c.release_locks,
      c.release_after_locks, c.wrong_thread_rejections, c.refcount_faults,
      c.lease_faults, c.fault_injections, c.read_fault_injections,
      c.facade_source_stream_callbacks, c.facade_source_resident_probes,
      c.facade_declared_materializations, c.facade_completed_materializations,
      c.facade_poison_hits, c.live_registries, c.live_providers, c.live_domains,
      c.live_leases, c.live_cache_sets, c.live_snapshots);
  PG_RETURN_DATUM(
      DirectFunctionCall1(jsonb_in, CStringGetDatum(encoded.c_str())));
#else
  ereport(
      ERROR,
      (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
       errmsg(
           "bioseq sequence source-domain diagnostics require a test build")));
  PG_RETURN_NULL();
#endif
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_sequence_source_wrong_thread_probe);
Datum bioseq_internal_sequence_source_wrong_thread_probe(PG_FUNCTION_ARGS) {
#ifdef BIOSEQ_TEST_INDEXES
	PG_RETURN_BOOL(bioseq::storage::pg_sequence_source_wrong_thread_probe());
#else
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("bioseq sequence source-domain diagnostics require a test build")));
	PG_RETURN_BOOL(false);
#endif
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_sequence_source_fault_contract_probe);
Datum bioseq_internal_sequence_source_fault_contract_probe(PG_FUNCTION_ARGS) {
#ifdef BIOSEQ_TEST_INDEXES
	PG_RETURN_BOOL(bioseq::storage::pg_sequence_source_fault_contract_probe());
#else
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("bioseq sequence source-domain diagnostics require a test build")));
	PG_RETURN_BOOL(false);
#endif
}

CXX_PG_FUNCTION_INFO_V1(bioseq_internal_sequence_source_pre_prepare_probe);
Datum bioseq_internal_sequence_source_pre_prepare_probe(PG_FUNCTION_ARGS) {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::storage::pg_sequence_source_pre_prepare_probe();
	PG_RETURN_VOID();
#else
	ereport(ERROR,
		(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
		 errmsg("bioseq sequence source-domain diagnostics require a test build")));
	PG_RETURN_VOID();
#endif
}
}
