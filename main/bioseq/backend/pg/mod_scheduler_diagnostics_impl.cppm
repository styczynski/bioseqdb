/*-------------------------------------------------------------------------
 *
 * mod_scheduler_diagnostics_impl.cppm
 *
 * Ordinary implementation unit for scheduler diagnostics and test controls
 * declared by bioseq.mod.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

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

module bioseq.mod;

import bioseq.common.mlogging;
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
import bioseq.parallelism.mode;
import bioseq.common.sequan_compat;
import bioseq.column.tam;
import bioseq.pg_utils.nodes;
import bioseq.table.scan_state;
import bioseq.table;
import bioseq.table.sql_rows;

using namespace bioseq::pg_utils;

namespace bioseq::pg::sql_routines {

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

void scheduler_restart() {
	bioseq::parallelism::pg::restart_cluster();
}

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

} // namespace bioseq::pg::sql_routines
