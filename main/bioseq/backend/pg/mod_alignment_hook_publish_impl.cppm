/**
 * @file mod_alignment_hook_publish_impl.cppm
 * @brief Alignment execution snapshot publication and accessors.
 */
module;

#include "common.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

module bioseq.mod;

import bioseq.pipeline.stats;
import bioseq.table.scan_state;

namespace bioseq::pg::detail {

namespace {
namespace alignment_exec = bioseq::pg::alignment_execution;

std::optional<alignment_exec::AlignmentExecutionSnapshot>
	last_alignment_execution_snapshot{};
/* Evidence publication is stricter than the diagnostic v1/v2 SRFs: partial
 * roots remain visible there, but must not replace the last completed
 * operation ledger. Completed non-GPU roots deliberately publish an exact
 * empty ledger so stale GPU proof cannot survive a later successful query. */
std::optional<alignment_exec::AlignmentExecutionSnapshot>
	last_completed_alignment_evidence_snapshot{};
std::int64_t last_alignment_execution_id = 0;

} // namespace

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
