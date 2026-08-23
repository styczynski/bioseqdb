/**
 * @file scheduler_diagnostics.cppm
 * @brief Implements scheduler statistics and test-control SQL routines.
 */
module;

#include <bioseq/pg_compat.h>

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

module bioseq.postgres.extension;

import bioseq.common.logging;
import bioseq.postgres.extension.sql_rows;
import bioseq.postgres.support;
import bioseq.postgres.storage.indexing.minimizer_build_stats;
import bioseq.postgres.scheduler.supervisor;
import bioseq.parallelism.mode;

using namespace bioseq::postgres::support;

namespace bioseq::pg::sql_routines {

/** @return Statistics for the last minimizer-sequence build in this backend. */
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::last_minimizer_sequence_build_stats_row>
last_minimizer_sequence_build_stats() {
	const auto st = bioseq::storage::indexing::
		g_last_minimizer_sequence_build_stats;
    std::vector<
		bioseq::postgres::sql_rows::last_minimizer_sequence_build_stats_row> rows;
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
    return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::last_minimizer_sequence_build_stats_row>{
            std::move(rows)};
}

/** @return The scheduler's latest minimizer-sequence build statistics. */
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::scheduler_minimizer_sequence_build_stats_row>
scheduler_minimizer_sequence_build_stats() {
	const auto snap = bioseq::postgres::scheduler::snapshot_async_minimizer_sequence_build_stats();
	const auto st = snap.stats;
    std::vector<
		bioseq::postgres::sql_rows::scheduler_minimizer_sequence_build_stats_row>
		rows;
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
        static_cast<std::int64_t>(snap.shareable_lazy_workers),
    });
    return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::scheduler_minimizer_sequence_build_stats_row>{
            std::move(rows)};
}


/**
 * @brief Arms one named worker-boundary failure for the test build.
 * @return True when the failure point was armed.
 */
bool arm_async_minimizer_sequence_worker_failure(std::string failure_point) {
#ifdef BIOSEQ_TEST_INDEXES
	const std::string_view point_name{failure_point};
	using stage_failure = bioseq::postgres::scheduler::async_minimizer_sequence_stage_pg_failure;
	stage_failure stage_point = stage_failure::none;
	if (point_name == "stage_register_xact_callback")
		stage_point = stage_failure::register_xact_callback;
	else if (point_name == "stage_dsm_create")
		stage_point = stage_failure::dsm_create;
	else if (point_name == "stage_dsm_pin")
		stage_point = stage_failure::dsm_pin;
	else if (point_name == "stage_dsm_detach")
		stage_point = stage_failure::dsm_detach;
	if (stage_point != stage_failure::none) {
		return bioseq::postgres::scheduler::
			arm_async_minimizer_sequence_stage_pg_failure(stage_point);
	}
	using worker_failure = bioseq::postgres::scheduler::async_minimizer_sequence_worker_pg_failure;
	worker_failure point = worker_failure::none;
	if (point_name == "relation_open") point = worker_failure::relation_open;
	else if (point_name == "relation_identity") point = worker_failure::relation_identity;
	else if (point_name == "relation_close") point = worker_failure::relation_close;
	else if (point_name == "spi_connect") point = worker_failure::spi_connect;
	else if (point_name == "spi_execute") point = worker_failure::spi_execute;
	else if (point_name == "spi_read") point = worker_failure::spi_read;
	else if (point_name == "spi_finish") point = worker_failure::spi_finish;
	else if (point_name == "materialize_error_log")
		point = worker_failure::materialize_error_log;
	else if (point_name == "materialize_interrupt")
		point = worker_failure::materialize_interrupt;
	else if (point_name == "materialize_scanner_read")
		point = worker_failure::materialize_scanner_read;
	else {
		bioseq::postgres::support::raise_sql_error(
			ERRCODE_INVALID_PARAMETER_VALUE,
			{
				.message =
					"unknown asynchronous minimizer-sequence worker failure point",
				.detail = fmt::format(
					"failure_point={}", point_name),
			});
	}
	return bioseq::postgres::scheduler::
		arm_async_minimizer_sequence_worker_pg_failure(point);
#else
	(void)failure_point;
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{
			.message =
				"asynchronous minimizer-sequence worker failure injection is unavailable",
			.detail =
				"The extension was built without BIOSEQ_TEST_INDEXES.",
		});
#endif
}


/** @brief Disarms every worker and staging failure point in the test build. */
void disarm_async_minimizer_sequence_worker_failure() {
#ifdef BIOSEQ_TEST_INDEXES
	bioseq::postgres::scheduler::disarm_async_minimizer_sequence_worker_pg_failure();
	bioseq::postgres::scheduler::disarm_async_minimizer_sequence_stage_pg_failure();
#else
	bioseq::postgres::support::raise_sql_error(
		ERRCODE_FEATURE_NOT_SUPPORTED,
		{
			.message =
				"asynchronous minimizer-sequence worker failure injection is unavailable",
			.detail =
				"The extension was built without BIOSEQ_TEST_INDEXES.",
		});
#endif
}


/** @return Worker-boundary call and failure statistics. */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::async_minimizer_sequence_worker_boundary_stats_row>
async_minimizer_sequence_worker_boundary_stats() {
	const auto stats = bioseq::postgres::scheduler::
		snapshot_async_minimizer_sequence_worker_boundary_stats();
	const char* last_point = bioseq::postgres::scheduler::
		async_minimizer_sequence_worker_failure_point_name(stats.last_failure_point);
	const char* armed_point = bioseq::postgres::scheduler::
		async_minimizer_sequence_worker_failure_point_name(stats.armed_failure_point);
	std::vector<
		bioseq::postgres::sql_rows::
			async_minimizer_sequence_worker_boundary_stats_row> rows;
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
			? "injected asynchronous minimizer-sequence worker PostgreSQL leaf failure"
			: "",
	});
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::
			async_minimizer_sequence_worker_boundary_stats_row>{
			std::move(rows)};
}

/** @brief Requests a scheduler supervisor restart. */
void scheduler_restart() {
	bioseq::postgres::scheduler::restart_cluster();
}

/** @return A snapshot of scheduler queues, rates, and memory admission. */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::scheduler_stats_row>
scheduler_stats() {
	const auto snap = bioseq::postgres::scheduler::snapshot();
	const std::string mode_string{
		bioseq::parallelism::concurrency_mode_name(snap.mode)};
	std::vector<bioseq::postgres::sql_rows::scheduler_stats_row> rows;
	rows.push_back({
		snap.is_supervisor,
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
	return bioseq::postgres::support::table_rows<
		bioseq::postgres::sql_rows::scheduler_stats_row>{std::move(rows)};
}

} // namespace bioseq::pg::sql_routines
