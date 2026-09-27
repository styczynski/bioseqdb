/**
 * @file alignment_hook_publish.cppm
 * @brief Save alignment statistics when the root query finishes.
 */
module;

#include <bioseq/pg_compat.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

module bioseq.postgres.extension:alignment_hook_publish;

import :alignment_hook_contract;
import bioseq.pipeline.stats;
import bioseq.postgres.table.alignment_execution;

namespace bioseq::pg::detail {

namespace {
namespace alignment_exec = bioseq::postgres::table::alignment_execution;

std::optional<alignment_exec::alignment_execution_snapshot>
	last_alignment_execution_snapshot{};
/* Diagnostics include partial query results. The operation ledger keeps
 * the last fully consumed alignment query. Completing a CPU alignment query
 * replaces that ledger with an empty one, so its results cannot include
 * GPU operations from the previous query. */
std::optional<alignment_exec::alignment_execution_snapshot>
	last_completed_alignment_snapshot{};
std::int64_t last_alignment_execution_id = 0;

} // namespace

/** @brief Combine scan-node statistics into a snapshot for the root query.
 * Mark it partial if any scan pass or query output was left unfinished.
 * Update the completed-operation snapshot only when all output was consumed.
 */
void publish_alignment_root(alignment_executor_frame* frame)
{
	if (frame == nullptr || frame->nested
		|| frame->collector.root_collector != &frame->collector
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

	bioseq::pipeline_stats::pipeline_execution_stats statement_stats{};
	bool fully_drained = frame->query_output_fully_drained;
	for (const alignment_exec::alignment_node_stats& node :
		 frame->collector.alignment_nodes) {
		statement_stats.merge(node.pipeline);
		fully_drained = fully_drained
			&& node.is_current_pass_fully_drained
			&& !node.has_incomplete_pass;
	}
	if (frame->collector.is_lifecycle_counter_saturated) {
		statement_stats.operational.are_stats_saturated = true;
	}
	// All tracked work has joined by ExecutorFinish. Compute the total time
	// covered by its intervals once for the statement, so overlapping work
	// is counted only once.
	auto downstream_active_wall =
		frame->collector.downstream_active_wall.finalize();
	statement_stats.operational.record_downstream_active_wall(
		downstream_active_wall.elapsed_ns,
		downstream_active_wall.available);
	#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
		statement_stats.operational.record_benchmark_e2e_interval_union(
			downstream_active_wall.benchmark_e2e_intervals.interval_union);
		auto benchmark_e2e_interval_origins =
			downstream_active_wall.benchmark_e2e_intervals.origins;
#endif
	frame->collector.persisted_gpu_operations.freeze();
	const bool are_persisted_gpu_stats_required =
		statement_stats.actual_backend
			== bioseq::pipeline_stats::executed_backend::vulkan_gpu
		&& statement_stats.actual_target_index_source
			== bioseq::pipeline_stats::target_index_source::persisted
		&& statement_stats.fully_attested_alignment_jobs != 0;
	if (are_persisted_gpu_stats_required
		&& !frame->collector.persisted_gpu_operations.
			is_pipeline_match(statement_stats)) {
		statement_stats.operational.are_stats_saturated = true;
		statement_stats.reclassify_all_attempted_jobs();
	}

	const std::int64_t execution_id = last_alignment_execution_id + 1;
	if (fully_drained) {
		last_completed_alignment_snapshot =
			alignment_exec::alignment_execution_snapshot{
				.execution_id = execution_id,
				.backend_pid = static_cast<std::int32_t>(MyProcPid),
				.database_oid = MyDatabaseId,
				.scan_node_count = static_cast<std::int32_t>(
					frame->collector.alignment_nodes.size()),
				.nested_alignment_executor_count = static_cast<std::int32_t>(
					frame->collector.nested_alignment_executor_count),
				.is_completed = true,
				.is_partial = false,
				.is_fully_drained = true,
				.pipeline = statement_stats,
				.persisted_gpu_operations = std::move(
					frame->collector.persisted_gpu_operations),
			};
	}
	last_alignment_execution_snapshot =
		alignment_exec::alignment_execution_snapshot{
			.execution_id = execution_id,
			.backend_pid = static_cast<std::int32_t>(MyProcPid),
			.database_oid = MyDatabaseId,
			.scan_node_count = static_cast<std::int32_t>(
				frame->collector.alignment_nodes.size()),
			.nested_alignment_executor_count = static_cast<std::int32_t>(
				frame->collector.nested_alignment_executor_count),
			.is_completed = fully_drained,
			.is_partial = !fully_drained,
			.is_fully_drained = fully_drained,
			.pipeline = statement_stats,
	#if defined(BIOSEQ_BENCHMARK_E2E_INTERVALS)
				.benchmark_e2e_interval_origins =
					benchmark_e2e_interval_origins,
	#endif
		};
	last_alignment_execution_id = execution_id;
}

/**
 * @brief Borrow the latest alignment snapshot, including partial queries.
 * @return Backend-local optional, empty before any snapshot is published.
 */
[[nodiscard]] const std::optional<
    bioseq::postgres::table::alignment_execution::alignment_execution_snapshot>&
last_alignment_execution_snapshot_ref() noexcept
{
    return last_alignment_execution_snapshot;
}

/**
 * @brief Borrow the latest alignment snapshot whose output was fully consumed.
 * @return Backend-local optional, empty before a query completes this way.
 */
[[nodiscard]] const std::optional<
    bioseq::postgres::table::alignment_execution::alignment_execution_snapshot>&
last_completed_alignment_snapshot_ref() noexcept
{
    return last_completed_alignment_snapshot;
}

} // namespace bioseq::pg::detail
