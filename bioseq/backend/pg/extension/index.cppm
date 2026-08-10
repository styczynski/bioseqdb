/**
 * @file index.cppm
 * @brief Declares typed owners for annotated PostgreSQL SQL routines.
 * @defgroup bioseq_sql_public_api Bioseq SQL public API
 *
 * Generated FMGR modules provide the C ABI wrappers; implementation units
 * own routine bodies and extension lifecycle state.
 */
module;

#include <bioseq/pg_compat.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

export module bioseq.postgres.extension;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.extension.alignment_sql_rows;
import bioseq.postgres.extension.sql_rows;
import bioseq.pipeline.stats;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.table.sql_rows;
import bioseq.postgres.table.alignment_execution;

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
bioseq::postgres::support::named_output<
    bioseq::postgres::sql_rows::vulkan_runtime_status_row>
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
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::list_metadata_row>
list_metadata();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::alignment_execution_stats_row>
last_alignment_execution_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_minimizer_fragment_reader_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_hash_major_index_reader_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_persisted_gpu_operation_stats();

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
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::active_read_policy_debug_row>
active_read_policy_debug();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::last_minimizer_sequence_build_stats_row>
last_minimizer_sequence_build_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::scheduler_minimizer_sequence_build_stats_row>
scheduler_minimizer_sequence_build_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bool arm_async_minimizer_sequence_worker_failure(std::string failure_point);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
void disarm_async_minimizer_sequence_worker_failure();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::async_minimizer_sequence_worker_boundary_stats_row>
async_minimizer_sequence_worker_boundary_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
void scheduler_restart();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::scheduler_stats_row>
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
bioseq::postgres::support::named_output<
	bioseq::postgres::sql_rows::inspect_cold_no_index_absence_row>
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
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::inspect_ready_minimizer_generation_row>
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
 * @brief Validates a source-bound ready hash-major minimizer generation,
 * including its metadata, catalog rows, byte counts, and checksums.
 */
bioseq::postgres::support::jsonb_value
inspect_ready_hash_major_minimizer_generation(
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
bioseq::postgres::support::jsonb_value hash_major_minimizer_parallel_scan_probe(
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
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_minimizer_fixture_row>
build_minimizer_fixture(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
	[[bioseq_sql::param({
		.default_value = bioseq::sql::checked_sql{"'scalar'"},
	})]]
	std::string requested_backend,
	[[bioseq_sql::param({
		.default_value = bioseq::sql::checked_sql{"0"},
	})]]
	std::int64_t position_segment_span_bp);

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
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::read_minimizer_fixture_row>
read_minimizer_fixture(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
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
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::scan_grouped_soa_minimizer_generation_test_row>
scan_grouped_soa_minimizer_generation_test(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
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
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::generation_storage_state_row>
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
 * @brief Builds and publishes a ready PostgreSQL-backed minimizer-fragment
 * generation without changing active routing or metadata age.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_ready_minimizer_fragment_generation_row>
build_ready_minimizer_fragment_generation(
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
 * @brief Builds and publishes a ready PostgreSQL-backed minimizer-sequence
 * generation, releases its spill runs, and leaves active routing and metadata
 * age unchanged.
 */
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_ready_minimizer_sequence_generation_row>
build_ready_minimizer_sequence_generation(
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
	bioseq::postgres::support::tuple_identifier ctid);

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
	bioseq::postgres::support::bytea_value payload);

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
bioseq::postgres::support::jsonb_value sequence_source_counters();

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

void install_alignment_execution_hooks();

[[nodiscard]] const std::optional<
	bioseq::postgres::table::alignment_execution::alignment_execution_snapshot>&
last_alignment_execution_snapshot_ref() noexcept;

[[nodiscard]] const std::optional<
	bioseq::postgres::table::alignment_execution::alignment_execution_snapshot>&
last_completed_alignment_snapshot_ref() noexcept;

[[nodiscard]] bioseq::pipeline_stats::executed_backend
validate_alignment_snapshot_environment(
	const bioseq::postgres::table::alignment_execution::alignment_execution_snapshot& snapshot);

[[nodiscard]] std::string fixed_hex_u64(std::uint64_t value);

[[nodiscard]] constexpr std::uint8_t hex_byte_value(
		const std::uint8_t byte) noexcept
{
	return byte;
}

[[nodiscard]] constexpr std::uint8_t hex_byte_value(
		const std::byte byte) noexcept
{
	return std::to_integer<std::uint8_t>(byte);
}

template<class p_byte, std::size_t p_extent>
[[nodiscard]] std::string fixed_hex_bytes(
		const std::array<p_byte, p_extent>& bytes)
{
	constexpr char hex_digits[] = "0123456789abcdef";
	std::string encoded(p_extent * 2, '0');
	for (std::size_t byte_index = 0; byte_index < p_extent; ++byte_index) {
		const std::uint8_t value = hex_byte_value(bytes[byte_index]);
		encoded[byte_index * 2] = hex_digits[(value >> 4U) & 0x0fU];
		encoded[byte_index * 2 + 1] = hex_digits[value & 0x0fU];
	}
	return encoded;
}

} // namespace bioseq::pg::detail
