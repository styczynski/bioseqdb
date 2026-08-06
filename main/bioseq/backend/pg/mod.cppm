/**-------------------------------------------------------------------------
 *
 * mod.cppm
 *
 * @defgroup bioseq_sql_public_api Bioseq SQL public API
 *
 * Exported declarations for typed SQL routine owners. Generated fmgr
 * modules provide the C ABI for annotated SQL routines; implementations and
 * extension lifecycle state live in the bioseq.mod implementation unit.
 *
 *-------------------------------------------------------------------------
 */
module;

#include "common.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

export module bioseq.mod;

import bioseq.codegen.sql.annotation_model;
import bioseq.mod.alignment.sql_rows;
import bioseq.mod.sql_rows;
import bioseq.pipeline.stats;
import bioseq.pg_utils.sql_boundary;
import bioseq.table.sql_rows;
import bioseq.table.scan_state;

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
        Datum query);

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
        std::optional<Datum> opts);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

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
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
std::string gpu_warmup_shader_cache();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
bioseq::pg_utils::named_out<
    bioseq::pg::sql_rows::vulkan_runtime_status_row>
vulkan_runtime_status();

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
        std::string sym);

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
        Datum request_datum);

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
        Datum request_datum);

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
        Datum request_datum);

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
list_metadata();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::alignment_execution_stats_row>
last_alignment_execution_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::alignment_execution_stats_v2_row>
last_alignment_execution_stats_v2();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v2();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v3();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v4();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v5();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format5_reader_memory_evidence_v6();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_format6_reader_evidence();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_persisted_gpu_operation_evidence_v1();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::pg_utils::sql_jsonb>
last_alignment_persisted_gpu_operation_evidence_v2();

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
	std::string kind);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
std::int64_t last_index_merge_run_count();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void reset_active_read_policy_debug();

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
active_read_policy_debug();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
    bioseq::pg::sql_rows::last_mps_build_stats_row>
last_mps_build_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
    bioseq::pg::sql_rows::scheduler_mps_build_stats_row>
scheduler_mps_build_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool arm_async_mps_worker_failure(std::string failure_point);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void disarm_async_mps_worker_failure();

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
async_mps_worker_boundary_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void scheduler_restart();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::pg_utils::returns_table<
	bioseq::pg::sql_rows::scheduler_stats_row>
scheduler_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
std::int64_t index_table_row_count(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name);

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
	std::string name);

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
	std::int64_t query_key);

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
	std::int64_t query_key);

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
	std::int32_t requested_workers);

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
	std::string requested_backend);

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
	std::string requested_backend);

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
	std::string requested_backend);

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
	std::int32_t fragment_ordinal);

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
	std::string shape);

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
	std::int64_t xor_mask);

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
	std::string failure_mode);

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
	std::string operation);

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
	std::string backend);

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
	std::string backend);

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
	std::int64_t query_key);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void drop_all_index_tables();

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
	std::string policy);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void set_active_read_index(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name);

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
	bioseq::pg_utils::sql_tid ctid);

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
void arm_inline_sequence_pg_failure(std::string failure_point);

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
	bioseq::pg_utils::sql_bytea payload);

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
	std::int64_t query_key);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void arm_sequence_source_failure(std::string failure_point);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void disarm_sequence_source_failure();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void reset_sequence_source_counters();

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
bioseq::pg_utils::sql_jsonb sequence_source_counters();

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
void arm_slice_dsm_fixture(std::string mode);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool sequence_source_wrong_thread_probe();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool sequence_source_fault_contract_probe();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool sequence_source_route_alias_probe();

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
	std::optional<Datum> sequence);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void sequence_source_pre_prepare_probe();

} // namespace bioseq::pg::sql_routines

namespace bioseq::pg::detail {

struct AlignmentExecutorFrame;

/* PostgreSQL 17 has no reset-callback unregister API. */
struct AlignmentFrameCleanupToken {
	MemoryContextCallback callback{};
	AlignmentExecutorFrame* frame = nullptr;
};

struct AlignmentExecutorFrame {
	QueryDesc* query_desc = nullptr;
	EState* estate = nullptr;
	bioseq::pg::alignment_execution::AlignmentExecutionCollector collector{};
	bioseq::pg::alignment_execution::AlignmentCollectorBinding collector_binding{};
	AlignmentFrameCleanupToken* cleanup_token = nullptr;
	bool nested = false;
	bool nested_alignment_invoked = false;
	bool query_output_fully_drained = false;
	bool finished = false;
};

void publish_alignment_root(AlignmentExecutorFrame* frame);

void install_alignment_execution_hooks();

[[nodiscard]] const std::optional<
	bioseq::pg::alignment_execution::AlignmentExecutionSnapshot>&
last_alignment_execution_snapshot_ref() noexcept;

[[nodiscard]] const std::optional<
	bioseq::pg::alignment_execution::AlignmentExecutionSnapshot>&
last_completed_alignment_evidence_snapshot_ref() noexcept;

[[nodiscard]] bioseq::pipeline_stats::ExecutedBackend
validate_alignment_snapshot_environment(
	const bioseq::pg::alignment_execution::AlignmentExecutionSnapshot& snapshot);

[[nodiscard]] std::string fixed_hex_u64(std::uint64_t value);

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

} // namespace bioseq::pg::detail
