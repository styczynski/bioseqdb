// Integration scenarios:
//   * `mmsearch(seq, query)` runs cleanly on a `USING bioseq` columnar table.
//     The exact match set depends on the alignment pipeline that is still
//     under development on this branch; we constrain only the output domain.
//   * The result set is invariant under the GUC matrix:
//        bioseq.enable_vectorization           on / off
//        bioseq.enable_column_cache            on / off
//        bioseq.enable_bioseq_index_scan       on / off
//        bioseq.aligner.enable_profiling       on / off
//   * `aligner_options()` with all-NULL fields produces the same rows as the
//     plain two-arg `mmsearch`; partial overrides (e.g. setting only
//     `kmer_size`) execute without erroring out.
//   * Per-row projection helpers (`bioseq.cigar`, `bioseq.target_start`, …)
//     produce well-shaped values: integer ranges in order, valid CIGAR text.
//   * SELECTs without an mmsearch qual on a `USING bioseq` table emit zero
//     rows. This is the documented "tuples only flow under mmsearch"
//     constraint from SPEC_HOW_TO_CONNECT_ALIGNER_3 §13.1.

#include "common_tests_integration.h"
import bioseq.integration_tests.common;

#include <bit>
#include <chrono>
#include <cstdint>
#include <libpq-fe.h>
#include <set>
#include <thread>

using namespace bioseq_int_tests;

namespace {

constexpr std::string_view kHoe = "hoe_int";
constexpr std::string_view kDuplicateTargets = "mmsearch_duplicate_targets_int";
constexpr std::string_view kMissingIndex = "mmsearch_missing_index_int";
constexpr std::string_view kBatchJoinTarget = "mmsearch_batch_join_target_int";
constexpr std::string_view kBatchJoinQueries = "mmsearch_batch_join_queries_int";
constexpr std::string_view kAsyncMpsWorkerLeaves =
	"mmsearch_async_mps_worker_leaves_int";

// Byte-for-byte parity with
// storage::indexing::index_advisory_xact_lock_key(). Integration tests are
// libpq clients and deliberately do not link the PostgreSQL backend module, so
// this small mirror is the only way to ask pg_advisory_lock(bigint) for the
// exact production logical-index identity. The observable writer wait below is
// the parity assertion: a drifted mirror cannot make that barrier pass.
[[nodiscard]] std::uint64_t doc0373_index_advisory_lock_key(
		const std::uint32_t database_oid,
		const std::uint32_t tablespace_oid,
		const std::uint32_t relfilenode,
		const std::int16_t column_attno,
		const std::string_view index_name) noexcept {
	std::uint64_t hash = 14695981039346656037ULL;
	hash ^= 0xB10D2790u;
	const auto mix_word = [&hash](std::uint64_t value) noexcept {
		for (std::uint32_t byte = 0; byte < 8; ++byte) {
			hash ^= value & 0xffu;
			hash *= 1099511628211ULL;
			value >>= 8u;
		}
	};
	mix_word(database_oid);
	mix_word(tablespace_oid);
	mix_word(relfilenode);
	mix_word(static_cast<std::uint16_t>(column_attno));
	for (const char byte : index_name) {
		hash ^= static_cast<std::uint8_t>(byte);
		hash *= 1099511628211ULL;
	}
	return hash;
}

struct Doc0373AsyncQueryOutcome {
	bool consume_ok = true;
	bool ready = false;
	bool got_result = false;
	bool expected_status = true;
};

// A Catch2 assertion or SQL helper may unwind while another backend is blocked
// at a lock barrier. This guard cancels and drains such a command before the
// owning connection can be destroyed, keeping every failure path bounded.
class Doc0373ScopedAsyncQuery {
private:
	PGconn* connection_ = nullptr;
	bool active_ = false;

	[[nodiscard]] bool cancel_and_drain_noexcept_() noexcept {
		if (!active_ || connection_ == nullptr) return true;
		if (PGcancel* cancel = PQgetCancel(connection_); cancel != nullptr) {
			char error[256]{};
			(void)PQcancel(cancel, error, sizeof(error));
			PQfreeCancel(cancel);
		}
		for (unsigned attempt = 0;
			 attempt < 1000 && PQisBusy(connection_) != 0; ++attempt) {
			if (PQconsumeInput(connection_) == 0) break;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		if (PQisBusy(connection_) == 0) {
			while (PGresult* result = PQgetResult(connection_)) {
				PQclear(result);
			}
			active_ = false;
		}
		return !active_;
	}

public:
	explicit Doc0373ScopedAsyncQuery(PGconn* connection) noexcept
		: connection_(connection) {}
	Doc0373ScopedAsyncQuery(const Doc0373ScopedAsyncQuery&) = delete;
	Doc0373ScopedAsyncQuery& operator=(
		const Doc0373ScopedAsyncQuery&) = delete;
	~Doc0373ScopedAsyncQuery() { (void)cancel_and_drain_noexcept_(); }

	[[nodiscard]] bool send(const std::string& sql) noexcept {
		if (connection_ == nullptr || active_) return false;
		active_ = PQsendQuery(connection_, sql.c_str()) == 1;
		return active_;
	}

	[[nodiscard]] bool cancel_and_drain() noexcept {
		return cancel_and_drain_noexcept_();
	}

	[[nodiscard]] Doc0373AsyncQueryOutcome drain(
			const int expected_status_value,
			const unsigned attempts = 1000) noexcept {
		Doc0373AsyncQueryOutcome outcome{};
		if (!active_ || connection_ == nullptr) {
			outcome.consume_ok = false;
			return outcome;
		}
		for (unsigned attempt = 0;
			 attempt < attempts && PQisBusy(connection_) != 0; ++attempt) {
			if (PQconsumeInput(connection_) == 0) {
				outcome.consume_ok = false;
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
		}
		outcome.ready = PQisBusy(connection_) == 0;
		if (!outcome.ready) return outcome;
		while (PGresult* result = PQgetResult(connection_)) {
			outcome.got_result = true;
			outcome.expected_status = outcome.expected_status
				&& static_cast<int>(PQresultStatus(result))
					== expected_status_value;
			PQclear(result);
		}
		active_ = false;
		return outcome;
	}
};

[[nodiscard]] rowset_t last_alignment_stats(
        const connection_test_context& ctx) {
    return query_rows(ctx, "SELECT * FROM bioseq.last_alignment_execution_stats();");
}

[[nodiscard]] long long stats_i64(
        const row_t& row,
        const std::size_t column) {
    REQUIRE(column < row.size());
    REQUIRE(row[column].has_value());
    return std::stoll(*row[column]);
}

[[nodiscard]] bool stats_bool(
        const row_t& row,
        const std::size_t column) {
    REQUIRE(column < row.size());
    REQUIRE(row[column].has_value());
    REQUIRE((*row[column] == "t" || *row[column] == "f"));
    return *row[column] == "t";
}

[[nodiscard]] bool selected_vulkan_runtime_untrusted(
    const connection_test_context& ctx) {
    // Intentionally make the status UDF the first Runtime consumer in this
    // backend.  The positive tiled-DP tests below are ordering regressions:
    // diagnostics must not construct an unprepared singleton that later lacks
    // the PostgreSQL shader provider or digest-derived MoltenVK archive env.
    return query_scalar(
        ctx,
        "SELECT (NOT available) OR software_device "
        "FROM bioseq.vulkan_runtime_status();") == "t";
}

[[nodiscard]] std::string index_requirement_sql(
        std::string_view table,
        const int ordinal) {
    return fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
        "aligner_options(index_read_requirement => {})) ORDER BY pk;",
        table,
        hoe_probe_literal(),
        ordinal);
}

void require_no_current_compatible_persisted_index(
        const connection_test_context& ctx,
        std::string_view sql) {
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "55000");
    REQUIRE(err.contains(
        "required compatible persisted minimizer index unavailable"));
    REQUIRE(err.contains("revision-3 READY"));
}

[[nodiscard]] std::string batch_join_target_literal() {
    return format_literal(
        hoe_seed_rows().front().sequences.front(),
        seq_typmod{.alphabet = "dna4", .width = 32});
}

void seed_batch_join_target(connection_test_context& ctx) {
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{kBatchJoinTarget},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{
            .pk = 1,
            .sequences = {hoe_seed_rows().front().sequences.front()},
        }});
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = off;");
	/* BatchJoin acceptance evidence is the requested all-SIMD pipeline, not
	 * the legacy scalar-sketch/scalar-chain/Block hybrid defaults. */
	exec_sql(ctx,
		"SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';");
	exec_sql(ctx,
		"SET bioseq.aligner.chaining_algorithm = 'simd_mm2';");
	exec_sql(ctx,
		"SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
}

void load_batch_join_queries(
        const connection_test_context& ctx,
        const std::size_t count,
        const bool only_last_can_match = false) {
    exec_sqlf(
        ctx,
        "CREATE TEMP TABLE {} "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 32));",
        kBatchJoinQueries);
    if (count == 0) {
        return;
    }
    const std::string query_expression = only_last_can_match
        ? fmt::format(
            "CASE WHEN g = {} THEN {} ELSE {} END",
            count - 1,
            batch_join_target_literal(),
            format_literal(
                "ACGTACGTAC",
                seq_typmod{.alphabet = "dna4", .width = 32}))
        : batch_join_target_literal();
    exec_sqlf(
        ctx,
        "INSERT INTO {} "
        "SELECT g::int, {} FROM generate_series(0, {}) AS g;",
        kBatchJoinQueries,
        query_expression,
        count - 1);
}

[[nodiscard]] std::string batch_join_sql(
        const int index_requirement,
        std::string_view select_list = "q.id, t.pk") {
    return fmt::format(
        "SELECT {} FROM {} t JOIN {} q "
        "ON mmsearch(t.seq0, q.seq, "
        "aligner_options(index_read_requirement => {}))",
        select_list,
        kBatchJoinTarget,
        kBatchJoinQueries,
        index_requirement);
}

void require_batch_join_aligned_query_rows(
        const connection_test_context& ctx,
        std::string_view sql,
        const std::size_t expected_queries,
        std::string_view expected_match_value = "1") {
    const auto rows = query_rows(ctx, std::string{sql} + " ORDER BY q.id;");
    REQUIRE(rows.size() == expected_queries);
    std::vector<bool> seen(expected_queries, false);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        const long long query_id = std::stoll(*row[0]);
        REQUIRE(query_id >= 0);
        REQUIRE(static_cast<std::size_t>(query_id) < expected_queries);
        REQUIRE(*row[1] == expected_match_value);
        REQUIRE_FALSE(seen[static_cast<std::size_t>(query_id)]);
        seen[static_cast<std::size_t>(query_id)] = true;
    }
    REQUIRE(std::all_of(seen.begin(), seen.end(), [](const bool value) {
        return value;
    }));
}

void require_batch_join_stats(
        const connection_test_context& ctx,
        const long long expected_targets,
        const long long expected_jobs) {
    const auto stats = last_alignment_stats(ctx);
    REQUIRE(stats.size() == 20);
	for (const auto& row : stats) {
			REQUIRE(row.size() == 150);
		REQUIRE(stats_i64(row, 11) == 1);
		REQUIRE(stats_i64(row, 32) == expected_targets);
		REQUIRE(stats_i64(row, 33) == expected_jobs);
		REQUIRE(stats_i64(row, 34) == expected_jobs);
		REQUIRE(stats_i64(row, 35) == 0);
		REQUIRE(stats_i64(row, 28) == 0);
		REQUIRE(stats_i64(row, 29) == 0);
	}
	/* require_absent is recompute-only.  The physical target and its index
	 * source are once-per-target outcomes, never once per query pair. */
	REQUIRE(stats_i64(stats.front(), 45) == 0);
	REQUIRE(stats_i64(stats.front(), 46) == 0);
	REQUIRE(stats_i64(stats.front(), 47) == expected_targets);
	REQUIRE_FALSE(stats_bool(stats.front(), 44));
    REQUIRE(stats_bool(stats.front(), 13));
    REQUIRE_FALSE(stats_bool(stats.front(), 14));
    REQUIRE(stats_bool(stats.front(), 15));
	REQUIRE_FALSE(stats_bool(stats.front(), 141));

	if (expected_jobs == 0) {
		/* An empty query child executes no biological backend.  It must remain
		 * the exact zero-work identity rather than laundering the selected GUCs
		 * into an execution claim. */
		REQUIRE(stats.front()[16].value_or("") == "none");
		REQUIRE(stats.front()[17].value_or("") == "none");
		REQUIRE(stats.front()[18].value_or("") == "none");
		REQUIRE(stats.front()[19].value_or("") == "none");
		for (const auto& row : stats) {
			for (std::size_t coverage_column = 24;
				 coverage_column <= 29; ++coverage_column) {
				REQUIRE(stats_i64(row, coverage_column) == 0);
			}
		}
		return;
	}

	/* Backend identity is derived from completed algorithm-local proofs, not
	 * from the requested option labels.  A fully attested simd_cpu job proves
	 * ExactSimd ntHash, SIMD score/backtrack and an accepted Block Aligner
	 * proof; that acceptance rejects every KSW2 or scalar-biological fallback. */
	REQUIRE(stats.front()[16].value_or("") == "simd_cpu");
	REQUIRE(stats.front()[17].value_or("") == "simd_cpu");
	REQUIRE(stats.front()[18].value_or("") == "recomputed");
	REQUIRE(stats.front()[19].value_or("") == "recomputed");
	REQUIRE(stats_i64(stats.front(), 94) > 0);
	REQUIRE(stats_i64(stats.front(), 95) == 0);
	REQUIRE(stats_i64(stats.front(), 96) == 0);
	REQUIRE(stats_i64(stats.front(), 98) > 0);
	REQUIRE(stats_i64(stats.front(), 99) > 0);
	REQUIRE(stats_i64(stats.front(), 100) == 0);
	REQUIRE(stats_i64(stats.front(), 101) == 0);
	for (std::size_t gpu_counter = 103; gpu_counter <= 138; ++gpu_counter) {
		REQUIRE(stats_i64(stats.front(), gpu_counter) == 0);
	}
	REQUIRE(stats_i64(stats.front(), 139) == 0);
	REQUIRE(stats_i64(stats.front(), 140) == 0);

	const auto require_all_simd_stage = [&](
			const std::size_t stage_no,
			const std::string_view stage_name) {
		const auto& row = stats[stage_no];
		REQUIRE(row[23].value_or("") == stage_name);
		REQUIRE(stats_i64(row, 24) == expected_jobs);
		REQUIRE(stats_i64(row, 25) == 0);
		REQUIRE(stats_i64(row, 26) == 0);
		REQUIRE(stats_i64(row, 27) == expected_jobs);
		REQUIRE(stats_i64(row, 28) == 0);
		REQUIRE(stats_i64(row, 29) == 0);
	};
	require_all_simd_stage(0, "query_sketch");
	require_all_simd_stage(1, "target_sketch");
	require_all_simd_stage(7, "chain_score");
	require_all_simd_stage(8, "chain_backtrack");

	/* Building the recomputed hash table is scalar orchestration by contract;
	 * it is neither a scalar minimizer backend nor a fallback. */
	const auto& index_build = stats[2];
	REQUIRE(index_build[23].value_or("") == "target_index_build");
	REQUIRE(stats_i64(index_build, 24) == expected_jobs);
	REQUIRE(stats_i64(index_build, 25) == 0);
	REQUIRE(stats_i64(index_build, 26) == expected_jobs);
	REQUIRE(stats_i64(index_build, 27) == 0);
	REQUIRE(stats_i64(index_build, 28) == 0);
	REQUIRE(stats_i64(index_build, 29) == 0);

	long long block_simd_executions = 0;
	const std::array<std::string_view, 3> block_stages{
		"left_extension", "gap_extension", "right_extension"};
	for (std::size_t offset = 0; offset < block_stages.size(); ++offset) {
		const auto& row = stats[17 + offset];
		REQUIRE(row[23].value_or("") == block_stages[offset]);
		REQUIRE(stats_i64(row, 25) == 0);
		REQUIRE(stats_i64(row, 26) == 0);
		REQUIRE(stats_i64(row, 27) == stats_i64(row, 24));
		REQUIRE(stats_i64(row, 28) == 0);
		REQUIRE(stats_i64(row, 29) == 0);
		block_simd_executions += stats_i64(row, 27);
	}
	/* Each fully attested job must contain at least one non-empty SIMD Block
	 * Aligner phase; a job may legitimately execute two or three phases. */
	REQUIRE(block_simd_executions >= expected_jobs);
}

}  // namespace

TEST_CASE("mmsearch on columnar table runs without errors and stays in domain",
          "[mmsearch][columnar]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string base_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());

    const auto pks = query_int_column(ctx, base_sql);
    INFO("mmsearch returned " << pks.size() << " pks");
    const std::set<long long> seeded{1, 2, 3, 4, 5};
    for (const auto pk : pks) {
        REQUIRE(seeded.contains(pk));
    }
}

TEST_CASE("alignment execution SRF is empty before the first published root",
          "[mmsearch][pipeline-stats][statement][srf]") {
    connection_test_context ctx;
    REQUIRE(last_alignment_stats(ctx).empty());
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    REQUIRE(last_alignment_stats(ctx).empty());
}

TEST_CASE("alignment execution SRF publishes one exact stage row set",
          "[mmsearch][pipeline-stats][statement][srf]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");

    const auto result = query_rows(ctx, index_requirement_sql(kHoe, 1));
    REQUIRE_FALSE(result.empty());
    const auto rows = last_alignment_stats(ctx);
    REQUIRE(rows.size() == 20);

    const std::array<std::string_view, 20> expected_stages{
        "query_sketch", "target_sketch", "target_index_build",
        "persisted_index_decode", "partition_selection",
        "device_index_upload", "seed_lookup", "chain_score",
        "chain_backtrack", "region_generation",
        "primary_secondary_reduction", "divergence_estimation", "mapq",
        "target_window_plan", "target_window_io", "target_window_upload",
        "cigar_job_generation", "left_extension", "gap_extension",
        "right_extension",
    };
    const long long execution_id = stats_i64(rows.front(), 0);
    REQUIRE(execution_id > 0);
    REQUIRE(stats_i64(rows.front(), 1)
            == query_scalar_int(ctx, "SELECT pg_backend_pid();"));
    REQUIRE(stats_i64(rows.front(), 2)
            == query_scalar_int(
                ctx,
                "SELECT oid::bigint FROM pg_database "
                "WHERE datname=current_database();"));
    for (std::size_t stage_no = 0; stage_no < rows.size(); ++stage_no) {
        const auto& row = rows[stage_no];
		REQUIRE(row.size() == 150);
        REQUIRE(stats_i64(row, 0) == execution_id);
        REQUIRE(stats_i64(row, 11) == 1);
        REQUIRE(stats_i64(row, 12) == 0);
        REQUIRE(stats_bool(row, 13));
        REQUIRE_FALSE(stats_bool(row, 14));
        REQUIRE(stats_bool(row, 15));
        REQUIRE(row[18].value_or("") == "recomputed");
        REQUIRE(row[19].value_or("") == "recomputed");
        REQUIRE(row[21].value_or("") == "0000000000000000");
        REQUIRE(row[23].value_or("") == expected_stages[stage_no]);
        REQUIRE(stats_i64(row, 32) == 5);
        REQUIRE(stats_i64(row, 33) == 5);
        REQUIRE(stats_i64(row, 34) == 0);
        REQUIRE(stats_i64(row, 35) == 5);
        REQUIRE_FALSE(stats_bool(row, 141));
        for (std::size_t gpu_column = 4; gpu_column <= 10; ++gpu_column) {
            REQUIRE_FALSE(row[gpu_column].has_value());
        }
    }
}

TEST_CASE("alignment execution id is fresh and errors do not publish",
          "[mmsearch][pipeline-stats][statement][freshness][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");

    (void)query_rows(ctx, index_requirement_sql(kHoe, 1));
    const long long first_id = stats_i64(last_alignment_stats(ctx).front(), 0);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0) == first_id);

    require_no_current_compatible_persisted_index(
        ctx, index_requirement_sql(kHoe, 2));
    REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0) == first_id);

    (void)query_rows(ctx, index_requirement_sql(kHoe, 1));
    REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0) == first_id + 1);
}

TEST_CASE("alignment LIMIT publishes a partial statement snapshot",
          "[mmsearch][pipeline-stats][statement][partial][limit]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    const std::string limited_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
        "aligner_options(index_read_requirement => 1)) LIMIT 1;",
        kHoe,
        hoe_probe_literal());
    REQUIRE_FALSE(query_rows(ctx, limited_sql).empty());
    const auto rows = last_alignment_stats(ctx);
    REQUIRE(rows.size() == 20);
    REQUIRE_FALSE(stats_bool(rows.front(), 13));
    REQUIRE(stats_bool(rows.front(), 14));
    REQUIRE_FALSE(stats_bool(rows.front(), 15));
    REQUIRE(rows.front()[16].value_or("") == "mixed_invalid");
    REQUIRE(rows.front()[17].value_or("") == "mixed_invalid");
    REQUIRE(stats_i64(rows.front(), 33) > 0);
    REQUIRE(stats_i64(rows.front(), 34) == 0);
    REQUIRE(stats_i64(rows.front(), 35) == stats_i64(rows.front(), 33));
}

TEST_CASE("one executor root aggregates two invoked alignment nodes",
          "[mmsearch][pipeline-stats][statement][multiple-nodes]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    const std::string sql = fmt::format(
        "SELECT "
        "(SELECT count(*) FROM {} a WHERE mmsearch(a.seq0, {}, "
        " aligner_options(index_read_requirement => 1))), "
        "(SELECT count(*) FROM {} b WHERE mmsearch(b.seq0, {}, "
        " aligner_options(index_read_requirement => 1)));",
        kHoe,
        hoe_probe_literal(),
        kHoe,
        hoe_probe_literal());
    REQUIRE(query_rows(ctx, sql).size() == 1);
    const auto rows = last_alignment_stats(ctx);
    REQUIRE(rows.size() == 20);
    REQUIRE(stats_i64(rows.front(), 11) == 2);
    REQUIRE(stats_i64(rows.front(), 12) == 0);
    REQUIRE(stats_bool(rows.front(), 13));
    REQUIRE(stats_i64(rows.front(), 32) == 10);
    REQUIRE(stats_i64(rows.front(), 33) == 10);
}

TEST_CASE("multiple broadcast queries keep target outcomes once per primary batch",
          "[mmsearch][columnar][pipeline-stats][multi-query]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    const std::string distinct_probe = format_literal(
        "CTAGTCACTGTAGCCTGATCGTAGCTAGC",
        seq_typmod{.alphabet = "dna4", .width = 32});

    const std::string sql = fmt::format(
        "SELECT pk FROM {} "
        "WHERE mmsearch(seq0, {}) AND mmsearch(seq0, {}) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal(),
        distinct_probe);
    const auto plan = explain_text<true>(ctx, sql);

    REQUIRE(plan_contains(plan, "actual_backend=mixed_invalid"));
    REQUIRE(plan_contains(
        plan, "attempted=10 fully_attested=0 unattested=10"));
    REQUIRE(plan_contains(plan, "Index outcomes [targets=5"));
    REQUIRE(plan_contains(plan, "recomputed=5]"));
    REQUIRE(plan_contains(plan, "First unattested [target_row="));
    REQUIRE(plan_contains(plan, "query_ordinal=0]"));
}

TEST_CASE("BioseqBatchJoin plans one physical target and rewrites compound helpers",
          "[mmsearch][batch-join][planner][projection]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);

    const std::string sql = batch_join_sql(
        1,
        "q.id, t.pk, "
        "bioseq.score(t.pk) + 1 AS adjusted_score, "
        "bioseq.query_end(t.pk) - bioseq.query_start(t.pk) AS query_span, "
        "bioseq.has_cigar(t.pk) AS has_cigar");
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Residual Mmsearch Qual: absent"));
	REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 2"));
	REQUIRE(plan_contains(plan, "Bioseq Match Placeholder Sites: 4"));
    REQUIRE_FALSE(plan_contains(plan, "Join Filter: mmsearch"));

    const auto rows = query_rows(ctx, sql + " ORDER BY q.id, t.pk;");
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
		REQUIRE(row.size() == 5);
        REQUIRE(row[0].has_value());
        REQUIRE(row[1].has_value());
        REQUIRE(row[2].has_value());
        REQUIRE(row[3].has_value());
		REQUIRE(row[4].has_value());
		/* Alignment scores are legitimately zero for this tiny exact fixture, so
		 * adjusted_score=1 is valid.  The direct-call score stub returns -1 and
		 * would instead produce the excluded sentinel value zero. */
		REQUIRE(std::stoll(*row[2]) >= 1);
		/* query_end() and query_start() have the same BIGINT result type but
		 * must occupy distinct executor-owned helper slots.  Aliasing helpers
		 * by result type (or leaving the SQL stubs to execute) collapses this
		 * positive aligned span to zero. */
		REQUIRE(std::stoll(*row[3]) > 0);
		/* has_cigar is both an unprunable Block Aligner consumer and a separate
		 * no-stub guard: the SQL rendezvous stub returns false. */
		REQUIRE(*row[4] == "t");
    }
    require_batch_join_stats(ctx, 1, 2);
}

TEST_CASE("BioseqBatchJoin base ProjectionPath guard is exact and fail closed",
          "[mmsearch][batch-join][planner][projection][negative]") {
    auto ctx = fresh_context();

    /* SQL normally moves an unrelated target expression to an upper
     * projection, so an EXPLAIN-only negative does not prove the dangerous
     * base ProjectionPath shape. This revoked seam constructs PostgreSQL's
     * real PathTarget/PlaceHolderVar nodes and calls the production finder. */
    const auto proof = query_one(
        ctx,
        "SELECT accepts_exact_helper::text, "
        "rejects_unrelated_phv::text, "
        "rejects_volatile_mixed_expression::text, "
        "preserves_wrapper_cost_owner::text, "
		"accepts_plain_target_var::text, "
		"rejects_plain_target_sequence_var::text, "
		"rejects_whole_row_var::text, "
		"rejects_system_var::text, "
		"rejects_nulling_var::text "
        "FROM bioseq_internal.batch_join_projection_guard_probe();");
    REQUIRE(proof.size() == 9);
    for (const auto& field : proof) {
        REQUIRE(field.has_value());
        REQUIRE(*field == "true");
    }
}

TEST_CASE("BioseqBatchJoin headline evidence is one target crossed with 16 queries",
          "[mmsearch][batch-join][e2e-shape]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 16);

	const std::string zero_placeholder_sql =
		batch_join_sql(1, "count(*)::bigint");
	const auto zero_placeholder_plan =
		explain_text<false>(ctx, zero_placeholder_sql);
	REQUIRE(plan_contains(
		zero_placeholder_plan, "Custom Scan (BioseqBatchJoin)"));
	REQUIRE(plan_contains(
		zero_placeholder_plan, "Bioseq Batch ABI Version: 2"));
	/* count(*) has no match-result helper. NIL is the canonical encoded empty
	 * placeholder list. This is a plan-ABI check, not an all-SIMD execution
	 * claim: executing it would intentionally elide base alignment. */
	REQUIRE(plan_contains(
		zero_placeholder_plan, "Bioseq Match Placeholder Sites: 0"));

	const std::string sql = batch_join_sql(
		1, "q.id, bioseq.has_cigar(t.pk) AS has_cigar");
	const auto plan = explain_text<false>(ctx, sql);
	REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
	REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 2"));
	REQUIRE(plan_contains(plan, "Bioseq Match Placeholder Sites: 1"));
	/* The ordinary parameterized BioseqScan carries a -100000 selection
	 * sentinel which Nested Loop multiplies by query rows. The eligible owner
	 * path must dominate that incumbent while retaining its honest cost. */
	REQUIRE(plan_contains(plan, "Bioseq Honest Startup Cost"));
	REQUIRE(plan_contains(plan, "Bioseq Honest Total Cost"));
	REQUIRE(plan_contains(plan, "Bioseq Incumbent Cost Floor"));
	const auto incumbent_floor = find_explain_property(
		plan, "Bioseq Incumbent Cost Floor");
	REQUIRE(incumbent_floor.has_value());
	REQUIRE(std::stod(*incumbent_floor) < 0.0);
	require_batch_join_aligned_query_rows(ctx, sql, 16, "t");
    require_batch_join_stats(ctx, 1, 16);
}

TEST_CASE("BioseqBatchJoin dispatches exactly one full 8192-query wave",
          "[mmsearch][batch-join][rollover][8192]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 8192);

	const std::string sql = batch_join_sql(
		1, "q.id, bioseq.has_cigar(t.pk) AS has_cigar");
	require_batch_join_aligned_query_rows(ctx, sql, 8192, "t");
    require_batch_join_stats(ctx, 1, 8192);
}

TEST_CASE("CpuPreparedTarget drops oversized capacity before same-object reprepare",
          "[mmsearch][batch-join][admission][reprepare]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    REQUIRE(query_scalar(
        ctx,
        "SELECT bioseq_internal."
        "cpu_prepared_target_same_object_reprepare_probe();") == "t");
}

TEST_CASE("BioseqBatchJoin resumes after 8192 and merges the one-row tail",
          "[mmsearch][batch-join][rollover][8193]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 8193);
	const auto reserved_bytes = [&] {
		return query_scalar_int(ctx,
			"SELECT index_memory_reserved_bytes FROM bioseq.scheduler_stats();");
	};
	const long long admission_baseline = reserved_bytes();

	const std::string sql = batch_join_sql(
		1, "q.id, bioseq.has_cigar(t.pk) AS has_cigar");
	require_batch_join_aligned_query_rows(ctx, sql, 8193, "t");
    require_batch_join_stats(ctx, 1, 8193);
	REQUIRE(reserved_bytes() == admission_baseline);

    const auto analyzed = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(analyzed, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(analyzed, "Bioseq Query Batches: 2"));
    REQUIRE(plan_contains(
        analyzed, "attempted=8193 fully_attested=8193 unattested=0"));

	// A later much smaller preparation in the same backend must neither retain
	// nor inherit the 8193-query union's outer/inner vector capacities.
	exec_sqlf(ctx, "DROP TABLE {};", kBatchJoinQueries);
	load_batch_join_queries(ctx, 1);
	require_batch_join_aligned_query_rows(
		ctx,
		batch_join_sql(1, "q.id, bioseq.has_cigar(t.pk) AS has_cigar"),
		1,
		"t");
	REQUIRE(reserved_bytes() == admission_baseline);
}

TEST_CASE("BioseqBatchJoin emits a match after a zero-match full first wave",
          "[mmsearch][batch-join][rollover][zero-match]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 8193, /*only_last_can_match=*/true);

	const auto rows = query_rows(
		ctx,
		batch_join_sql(
			1,
			"DISTINCT q.id, bioseq.has_cigar(t.pk) AS has_cigar")
			+ " ORDER BY q.id;");
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
		REQUIRE(row.size() == 2);
        REQUIRE(row[0].has_value());
		REQUIRE(row[1].has_value());
        REQUIRE(std::stoll(*row[0]) == 8192);
		REQUIRE(*row[1] == "t");
    }

	/* The first wave deliberately uses 10-bp queries, shorter than k=15. They
	 * cannot produce the non-empty sketch/chain/Block proof required for a
	 * simd_cpu attestation, so the aggregate must stay diagnostic rather than
	 * laundering those 8192 jobs. The final eligible query still has to carry
	 * positive end-to-end SIMD evidence and zero fallback evidence. */
	const auto stats = last_alignment_stats(ctx);
	REQUIRE(stats.size() == 20);
	REQUIRE(stats.front()[16].value_or("") == "simd_cpu");
	REQUIRE(stats.front()[17].value_or("") == "mixed_invalid");
	REQUIRE(stats.front()[18].value_or("") == "recomputed");
	REQUIRE(stats.front()[19].value_or("") == "recomputed");
	REQUIRE(stats_i64(stats.front(), 32) == 1);
	REQUIRE(stats_i64(stats.front(), 33) == 8193);
	REQUIRE(stats_i64(stats.front(), 34) == 0);
	REQUIRE(stats_i64(stats.front(), 35) == 8193);
	REQUIRE(stats_i64(stats.front(), 37) == 0);
	REQUIRE(stats_i64(stats.front(), 45) == 0);
	REQUIRE(stats_i64(stats.front(), 46) == 0);
	REQUIRE(stats_i64(stats.front(), 47) == 1);
	REQUIRE(stats_i64(stats.front(), 94) > 0);
	REQUIRE(stats_i64(stats.front(), 95) == 0);
	REQUIRE(stats_i64(stats.front(), 96) == 0);
	REQUIRE(stats_i64(stats.front(), 98) > 0);
	REQUIRE(stats_i64(stats.front(), 99) > 0);
	REQUIRE(stats_i64(stats.front(), 100) == 0);
	REQUIRE(stats_i64(stats.front(), 101) == 0);
	for (std::size_t gpu_counter = 103; gpu_counter <= 138; ++gpu_counter) {
		REQUIRE(stats_i64(stats.front(), gpu_counter) == 0);
	}
	REQUIRE(stats_i64(stats.front(), 139) == 0);
	REQUIRE(stats_i64(stats.front(), 140) == 0);
	REQUIRE_FALSE(stats_bool(stats.front(), 141));
	for (const std::size_t stage_no : {std::size_t{0}, std::size_t{1},
			 std::size_t{7}, std::size_t{8}}) {
		REQUIRE(stats_i64(stats[stage_no], 24) == 1);
		REQUIRE(stats_i64(stats[stage_no], 25) == 0);
		REQUIRE(stats_i64(stats[stage_no], 26) == 0);
		REQUIRE(stats_i64(stats[stage_no], 27) == 1);
		REQUIRE(stats_i64(stats[stage_no], 28) == 0);
		REQUIRE(stats_i64(stats[stage_no], 29) == 0);
	}
	long long tail_block_simd_executions = 0;
	for (std::size_t stage_no = 17; stage_no <= 19; ++stage_no) {
		REQUIRE(stats_i64(stats[stage_no], 26) == 0);
		REQUIRE(stats_i64(stats[stage_no], 27)
			== stats_i64(stats[stage_no], 24));
		REQUIRE(stats_i64(stats[stage_no], 28) == 0);
		REQUIRE(stats_i64(stats[stage_no], 29) == 0);
		tail_block_simd_executions += stats_i64(stats[stage_no], 27);
	}
	REQUIRE(tail_block_simd_executions >= 1);
}

TEST_CASE("plain EXPLAIN exposes live CPU just-index BatchJoin before missing-generation failure",
          "[mmsearch][batch-join][just-index][explain][error]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);
    const std::string sql = batch_join_sql(2, "q.id, t.pk");

    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Bioseq Residual Mmsearch Qual: absent"));
    REQUIRE(plan_contains(
        plan,
        "Bioseq Index Read Requirement: require_compatible_persisted"));
    REQUIRE(plan_contains(plan, "Bioseq Requirement Executable: true"));
    require_no_current_compatible_persisted_index(ctx, sql + ";");
    require_no_current_compatible_persisted_index(
        ctx, "EXPLAIN (ANALYZE, COSTS OFF) " + sql + ";");
}

TEST_CASE("ordinal-2 BatchJoin directly consumes READY grouped32 MPF and MPS",
			  "[mmsearch][batch-join][just-index][rev3][grouped32][simd]"
			  "[doc0373][p1][one-open]") {
	for (const std::string_view index_name : {
			std::string_view{"minimizers_per_fragment"},
			std::string_view{"minimizers_per_sequence"}}) {
		DYNAMIC_SECTION(index_name) {
			auto ctx = fresh_context({kBatchJoinTarget});
			seed_batch_join_target(ctx);
			load_batch_join_queries(ctx, 2);
			exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
			exec_sql(ctx, "SET bioseq.index_async_persist = off;");
			exec_sql(ctx, "SET bioseq.recompute_async_persist = off;");
			exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
			exec_sql(ctx,
				"SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';");
			exec_sql(ctx,
				"SET bioseq.aligner.chaining_algorithm = 'simd_mm2';");
			exec_sql(ctx,
				"SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
			/* The runtime capability gate alone is not a GPU component request.
			 * CPU query-union and grouped32 preparation must remain legal. */
			exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = on;");
			const auto rowno = static_cast<std::uint64_t>(query_scalar_int(
				ctx,
				fmt::format(
					"SELECT bioseq.column_row_number('{}'::regclass, ctid) "
					"FROM {} ORDER BY pk LIMIT 1;",
					kBatchJoinTarget, kBatchJoinTarget)));
			if (index_name == "minimizers_per_sequence") {
				exec_sqlf(ctx,
					"SELECT bioseq.set_active_read_index('{}'::regclass, '{}');",
					kBatchJoinTarget, index_name);
			}
			const row_t publication = query_one(ctx, fmt::format(
				"SELECT * FROM bioseq_internal.rev3_test_build_ready_{}_generation("
				"'{}'::regclass, '{}', {}, 'exact_simd');",
				index_name == "minimizers_per_fragment" ? "mpf" : "mps",
				kBatchJoinTarget, index_name, rowno));
			REQUIRE(!publication.empty());
			REQUIRE(publication[8].value_or("") == "t");
			REQUIRE(publication[9].value_or("") == "t");

			require_batch_join_aligned_query_rows(
				ctx,
				batch_join_sql(
					2, "q.id, bioseq.has_cigar(t.pk) AS has_cigar"),
				2,
				"t");
			const row_t proof = query_one(ctx,
				"SELECT requested_backend, actual_backend, "
				"requested_target_index_source, actual_target_index_source, "
				"persisted_generation_id::text, persisted_source_identity, "
				"persisted_relation_opens::text, persisted_index_scans::text, "
				"recomputed_target_index_rows::text, "
				"persisted_hash_build_calls::text, pair_sort_calls::text, "
				"host_pair_materialization_calls::text, "
				"backend_fallbacks::text, nthash_vector_batches::text, "
				"simd_chain_vector_batches::text, "
				"persisted_source_parent_rows_read::text, "
				"persisted_source_catalog_rows_read::text, "
				"persisted_source_catalog_index_scans::text, "
				"persisted_source_fragment_rows_read::text, "
				"persisted_source_decoded_bp::text, "
				"persisted_legacy_globalize_calls::text, "
				"persisted_legacy_rebase_calls::text "
				"FROM bioseq.last_alignment_execution_stats() "
				"WHERE stage='query_sketch';");
			REQUIRE(proof.size() == 22);
			REQUIRE(proof[0].value_or("") == "simd_cpu");
			REQUIRE(proof[1].value_or("") == "simd_cpu");
			REQUIRE(proof[2].value_or("") == "persisted");
			REQUIRE(proof[3].value_or("") == "persisted");
			REQUIRE(std::stoll(proof[4].value()) > 0);
			REQUIRE(!proof[5].value_or("").empty());
			REQUIRE(proof[6].value_or("") == "1");
			REQUIRE(proof[7].value_or("") == "3");
			for (std::size_t zero : {8u, 9u, 10u, 11u, 12u}) {
				REQUIRE(proof[zero].value_or("") == "0");
			}
			REQUIRE(std::stoll(proof[13].value()) > 0);
			REQUIRE(std::stoll(proof[14].value()) > 0);
			REQUIRE(proof[15].value_or("") == "1");
			REQUIRE(proof[16].value_or("") == "1");
			REQUIRE(proof[17].value_or("") == "1");
			for (std::size_t zero : {18u, 19u, 20u, 21u}) {
				REQUIRE(proof[zero].value_or("") == "0");
			}

			const row_t cold = query_one(ctx,
				"WITH stats AS ("
				" SELECT cold_index_preparation AS cold,"
				" persisted_head_scans, persisted_manifest_directory_scans,"
				" persisted_range_scan_calls, persisted_toast_values_read,"
				" persisted_detoast_bytes, stats_saturated AS scalar_saturated"
				" FROM bioseq.last_alignment_execution_stats()"
				" WHERE stage='query_sketch'"
				") SELECT "
				"jsonb_typeof(cold) = 'object', "
				"cold->>'schema' = 'bioseq.cold_index_preparation', "
				"jsonb_typeof(cold->'schema') = 'string', "
				"(cold->>'version')::bigint = 1, "
				"jsonb_typeof(cold->'version') = 'number', "
				"jsonb_typeof(cold->'stats_saturated') = 'boolean' "
				"AND cold->>'timing_semantics' = "
				"'payload_inclusive_wall_v1' "
				"AND jsonb_typeof(cold->'timing_semantics') = 'string', "
				"NOT (cold->>'stats_saturated')::boolean "
				"AND jsonb_typeof(cold->'decode_timing_available') = "
				"'boolean' AND NOT "
				"(cold->>'decode_timing_available')::boolean "
				"AND jsonb_typeof(cold->'transfer_timing_available') = "
				"'boolean' AND NOT "
				"(cold->>'transfer_timing_available')::boolean, "
				"cold ?& ARRAY["
				" 'schema', 'version', 'timing_semantics',"
				" 'decode_timing_available',"
				" 'transfer_timing_available', 'stats_saturated',"
				" 'target_relation_reuses', 'target_relation_extra_opens',"
				" 'parent_random_reads',"
				" 'policy_heap_opens', 'policy_index_opens',"
				" 'source_heap_opens', 'source_index_opens',"
				" 'fragment_heap_opens', 'sidecar_heap_opens',"
				" 'sidecar_index_opens', 'policy_schema_validations',"
				" 'source_schema_validations', 'sidecar_schema_validations',"
				" 'lock_acquires', 'lock_releases', 'snapshot_registers',"
				" 'route_empty', 'route_selective', 'route_full',"
				" 'route_candidate_rows', 'route_full_rows',"
				" 'route_candidate_bytes', 'route_full_bytes',"
				" 'route_selective_cost', 'route_full_cost',"
				" 'memory_reroutes', 'payload_bytes_requested',"
				" 'payload_bytes_delivered', 'payload_rows_requested',"
				" 'payload_rows_delivered', 'payload_rows_scanned',"
				" 'payload_bytes_scanned', 'payload_rows_skipped',"
				" 'heap_tid_adjacent',"
				" 'heap_tid_forward_gap', 'heap_tid_backward_jump',"
				" 'extent_crc_bytes', 'row_crc_bytes', 'tuple_form_bytes',"
				" 'policy_ns', 'source_ns', 'bind_ns', 'head_ns',"
				" 'directory_ns', 'payload_ns', 'decode_ns', 'transfer_ns',"
				" 'close_ns', 'shared_blks_hit', 'shared_blks_read'"
				"]::text[], "
				"(SELECT count(*) FROM jsonb_object_keys(cold)) = 56, "
				"NOT EXISTS ("
				" SELECT 1 FROM jsonb_each(cold) AS entry"
				" WHERE entry.key NOT IN ("
				" 'schema', 'timing_semantics',"
				" 'decode_timing_available',"
				" 'transfer_timing_available', 'decode_ns',"
				" 'transfer_ns', 'stats_saturated')"
				" AND jsonb_typeof(entry.value) <> 'number'"
				") AND jsonb_typeof(cold->'decode_ns') = 'null' "
				"AND jsonb_typeof(cold->'transfer_ns') = 'null', "
				"(cold->>'target_relation_reuses')::bigint = 1, "
				"(cold->>'target_relation_extra_opens')::bigint = 0, "
				"(cold->>'parent_random_reads')::bigint = 1, "
				"(cold->>'policy_heap_opens')::bigint = 1, "
				"(cold->>'policy_index_opens')::bigint = 1, "
				"(cold->>'source_heap_opens')::bigint = 1, "
				"(cold->>'source_index_opens')::bigint = 1, "
				"(cold->>'sidecar_heap_opens')::bigint = 1, "
				"(cold->>'sidecar_index_opens')::bigint = 1, "
				"(cold->>'policy_schema_validations')::bigint = 1 "
				" AND (cold->>'source_schema_validations')::bigint = 1 "
				" AND (cold->>'sidecar_schema_validations')::bigint = 1, "
				"(cold->>'lock_acquires')::bigint "
				" = (cold->>'lock_releases')::bigint, "
				"(cold->>'snapshot_registers')::bigint = 0, "
				"(cold->>'route_empty')::bigint "
				" + (cold->>'route_selective')::bigint "
				" + (cold->>'route_full')::bigint = 1, "
				"(cold->>'route_empty')::bigint = 0 "
				" AND (cold->>'route_selective')::bigint = 0 "
				" AND (cold->>'route_full')::bigint = 1, "
				"(cold->>'route_candidate_rows')::bigint = 0 "
				" AND (cold->>'route_candidate_bytes')::bigint = 0, "
				"(cold->>'route_full_rows')::bigint > 0 "
				" AND (cold->>'route_full_bytes')::bigint > 0 "
				" AND (cold->>'route_full_cost')::bigint "
				" > (cold->>'route_full_rows')::bigint, "
				"(cold->>'memory_reroutes')::bigint = 0, "
				"(cold->>'payload_bytes_requested')::bigint "
				" = (cold->>'payload_bytes_delivered')::bigint "
				" AND (cold->>'payload_bytes_delivered')::bigint "
				" = (cold->>'payload_bytes_scanned')::bigint "
				" AND (cold->>'payload_bytes_delivered')::bigint > 0, "
				"(cold->>'payload_rows_requested')::bigint "
				" = (cold->>'payload_rows_delivered')::bigint "
				" AND (cold->>'payload_rows_delivered')::bigint "
				" = (cold->>'payload_rows_scanned')::bigint "
				" AND (cold->>'payload_rows_skipped')::bigint = 0 "
				" AND (cold->>'payload_rows_delivered')::bigint > 0, "
				"(cold->>'heap_tid_adjacent')::bigint "
				" + (cold->>'heap_tid_forward_gap')::bigint "
				" + (cold->>'heap_tid_backward_jump')::bigint + 1 "
				" = (cold->>'payload_rows_scanned')::bigint, "
				"(cold->>'extent_crc_bytes')::bigint "
				" = (cold->>'payload_bytes_delivered')::bigint "
				" AND (cold->>'row_crc_bytes')::bigint "
				" = (cold->>'payload_bytes_scanned')::bigint, "
				"(cold->>'tuple_form_bytes')::bigint = 0, "
				"(cold->>'payload_ns')::bigint >= 0, "
				"(cold->>'policy_ns')::bigint >= 0 "
				" AND (cold->>'source_ns')::bigint >= 0 "
				" AND (cold->>'bind_ns')::bigint >= 0 "
				" AND (cold->>'head_ns')::bigint >= 0 "
				" AND (cold->>'directory_ns')::bigint >= 0 "
				" AND (cold->>'payload_ns')::bigint >= 0 "
				" AND (cold->>'close_ns')::bigint >= 0, "
				"NOT (cold ?| ARRAY["
				" 'head_scans', 'directory_scans', 'payload_range_scans',"
				" 'toast_values', 'detoast_bytes']::text[]) "
				"AND persisted_head_scans = 1 "
				"AND persisted_manifest_directory_scans = 1 "
				"AND persisted_range_scan_calls = 1 "
				"AND persisted_toast_values_read = 0 "
				"AND persisted_detoast_bytes = 0 "
				"AND (cold->>'stats_saturated')::boolean = scalar_saturated "
				"FROM stats;");
			REQUIRE(cold.size() == 35);
			for (const auto& assertion : cold) {
				REQUIRE(assertion.value_or("") == "t");
			}

			/* Extra secondary indexes must not increase the cold reader's open
			 * count: it binds and validates only the exact named primary index. */
			exec_sql(ctx,
				"CREATE INDEX sequence_source_version_user_relid_probe_idx "
				"ON bioseq_internal.sequence_source_version(user_relid);");
			require_batch_join_aligned_query_rows(
				ctx,
				batch_join_sql(
					2, "q.id, bioseq.has_cigar(t.pk) AS has_cigar"),
				2,
				"t");
			const row_t dynamic_source_indexes = query_one(ctx,
				"SELECT requested_backend='simd_cpu', "
				"actual_backend='simd_cpu', backend_fallbacks=0, "
				"(cold_index_preparation->>'source_heap_opens')::bigint=1, "
				"(cold_index_preparation->>'source_index_opens')::bigint=1, "
				"(cold_index_preparation->>'source_schema_validations')::bigint=1 "
				"FROM bioseq.last_alignment_execution_stats() "
				"WHERE stage='query_sketch';");
			REQUIRE(dynamic_source_indexes.size() == 6);
			for (const auto& assertion : dynamic_source_indexes) {
				REQUIRE(assertion.value_or("") == "t");
			}
		}
	}
}

TEST_CASE("DOC0373 P1 releases cold sidecar/catalog locks before later work",
			  "[mmsearch][batch-join][just-index][doc0373][p1]"
			  "[one-open][lock-order][concurrency]") {
	auto reader = fresh_context({kBatchJoinTarget});
	seed_batch_join_target(reader);
	load_batch_join_queries(reader, 2);
	exec_sql(reader, "SET bioseq.update_index_on_insert = on;");
	exec_sql(reader, "SET bioseq.index_async_persist = off;");
	exec_sql(reader, "SET bioseq.recompute_async_persist = off;");
	exec_sql(reader, "SET bioseq.read_sequence_index = on;");
	const auto rowno = static_cast<std::uint64_t>(query_scalar_int(
		reader,
		fmt::format(
			"SELECT bioseq.column_row_number('{}'::regclass, ctid) "
			"FROM {} ORDER BY pk LIMIT 1;",
			kBatchJoinTarget, kBatchJoinTarget)));
	const row_t publication = query_one(reader, fmt::format(
		"SELECT * FROM bioseq_internal.rev3_test_build_ready_mpf_generation("
		"'{}'::regclass, 'minimizers_per_fragment', {}, 'exact_simd');",
		kBatchJoinTarget, rowno));
	REQUIRE(publication.size() >= 10u);
	REQUIRE(publication[0].has_value());
	const std::string sidecar = publication[0].value();
	const row_t relation_oids = query_one(reader, fmt::format(
		"SELECT '{}'::regclass::oid::text, "
		"'bioseq_internal.{}'::regclass::oid::text, "
		"(SELECT indexrelid::text FROM pg_index WHERE indrelid="
		"'bioseq_internal.{}'::regclass AND indisprimary), "
		"'bioseq_internal.index_policy'::regclass::oid::text, "
		"'bioseq_internal.index_policy_pkey'::regclass::oid::text, "
		"'bioseq_internal.sequence_source_version'::regclass::oid::text, "
		"'bioseq_internal.sequence_source_version_pkey'::regclass::oid::text;",
		kBatchJoinTarget, sidecar, sidecar));
	REQUIRE(relation_oids.size() == 7u);
	for (const auto& oid : relation_oids) REQUIRE(oid.has_value());
	const row_t lock_identity = query_one(reader, fmt::format(
		"SELECT d.oid::bigint::text, "
		"COALESCE(NULLIF(c.reltablespace, 0), d.dattablespace)::bigint::text, "
		"COALESCE(pg_relation_filenode(c.oid), c.oid)::bigint::text, "
		"a.attnum::bigint::text "
		"FROM pg_class c JOIN pg_database d ON d.datname=current_database() "
		"JOIN pg_attribute a ON a.attrelid=c.oid AND a.attname='seq0' "
		"AND a.attnum>0 AND NOT a.attisdropped "
		"WHERE c.oid='{}'::regclass;",
		kBatchJoinTarget));
	REQUIRE(lock_identity.size() == 4u);
	for (const auto& identity : lock_identity) REQUIRE(identity.has_value());
	const auto database_oid = static_cast<std::uint32_t>(
		std::stoull(lock_identity[0].value()));
	const auto tablespace_oid = static_cast<std::uint32_t>(
		std::stoull(lock_identity[1].value()));
	const auto relfilenode = static_cast<std::uint32_t>(
		std::stoull(lock_identity[2].value()));
	const auto sequence_attno = static_cast<std::int16_t>(
		std::stoll(lock_identity[3].value()));
	const std::int64_t advisory_key = std::bit_cast<std::int64_t>(
		doc0373_index_advisory_lock_key(
			database_oid, tablespace_oid, relfilenode, sequence_attno,
			"minimizers_per_fragment"));

	connection_test_context gate;
	connection_test_context writer;
	connection_test_context dropper;
	connection_test_context observer;
	const long long reader_pid = query_scalar_int(
		reader, "SELECT pg_backend_pid();");
	const long long gate_pid = query_scalar_int(
		gate, "SELECT pg_backend_pid();");
	const long long writer_pid = query_scalar_int(
		writer, "SELECT pg_backend_pid();");
	const long long drop_pid = query_scalar_int(
		dropper, "SELECT pg_backend_pid();");
	exec_sql(reader, "BEGIN ISOLATION LEVEL REPEATABLE READ;");
	require_batch_join_aligned_query_rows(
		reader,
		batch_join_sql(2, "q.id, bioseq.has_cigar(t.pk) AS has_cigar"),
		2, "t");
	const row_t retained_locks = query_one(observer, fmt::format(
		"SELECT count(*) FILTER (WHERE relation={}::oid "
		"AND mode='AccessShareLock' AND granted)::text, "
		"count(*) FILTER (WHERE relation=ANY(ARRAY[{}::oid,{}::oid,"
		"{}::oid,{}::oid,{}::oid,{}::oid]) AND granted)::text "
		"FROM pg_locks WHERE pid={};",
		relation_oids[0].value(),
		relation_oids[1].value(), relation_oids[2].value(),
		relation_oids[3].value(), relation_oids[4].value(),
		relation_oids[5].value(), relation_oids[6].value(), reader_pid));
	REQUIRE(retained_locks.size() == 2u);
	REQUIRE(std::stoll(retained_locks[0].value()) >= 1);
	REQUIRE(std::stoll(retained_locks[1].value()) == 0);

	/* Freeze the exact writer boundary: the gate owns the production logical-
	 * index advisory identity and the exact sidecar. Releasing the session lock
	 * separately from the transaction lock lets the observer distinguish
	 * advisory wait from sidecar wait without timing guesses. */
	exec_sql(gate, "BEGIN;");
	exec_sql(gate, fmt::format(
		"SELECT pg_advisory_lock({}::bigint);", advisory_key));
	exec_sql(gate, fmt::format(
		"LOCK TABLE bioseq_internal.{} IN ACCESS EXCLUSIVE MODE;", sidecar));
	const row_t gate_advisory = query_one(observer, fmt::format(
		"SELECT database::text, classid::text, objid::text, objsubid::text "
		"FROM pg_locks WHERE pid={} AND locktype='advisory' "
		"AND mode='ExclusiveLock' AND granted;",
		gate_pid));
	REQUIRE(gate_advisory.size() == 4u);
	for (const auto& identity : gate_advisory) REQUIRE(identity.has_value());
	REQUIRE(query_scalar_int(observer, fmt::format(
		"SELECT count(*) FROM pg_locks WHERE pid={} AND relation={}::oid "
		"AND mode='AccessExclusiveLock' AND granted;",
		gate_pid, relation_oids[1].value())) == 1);
	const std::string exact_advisory_predicate = fmt::format(
		"locktype='advisory' AND database={}::oid AND classid={}::oid "
		"AND objid={}::oid AND objsubid={}",
		gate_advisory[0].value(), gate_advisory[1].value(),
		gate_advisory[2].value(), gate_advisory[3].value());

	/* Invoke the real production writer without a harness-supplied target lock.
	 * The AccessShareLock retained by IsBioseqColumnTableAmTable must remain
	 * observable while update_index waits at the next lock level. */
	exec_sql(writer, "SET statement_timeout = '30s';");
	exec_sql(writer, "SET bioseq.update_index_on_insert = off;");
	exec_sql(writer, "SET bioseq.index_async_persist = off;");
	exec_sql(writer, "SET bioseq.recompute_async_persist = off;");
	exec_sql(writer,
		"SET bioseq.aligner.grouped32_persisted_generation_enabled = on;");
	exec_sql(writer, "BEGIN;");
	Doc0373ScopedAsyncQuery writer_query{writer.raw()};
	const std::string writer_sql = fmt::format(
		"SELECT bioseq.update_index('{}'::regclass, "
		"'minimizers_per_fragment');",
		kBatchJoinTarget);
	REQUIRE(writer_query.send(writer_sql));

	const auto count_cell = [](const row_t& row, const std::size_t column) {
		if (column >= row.size() || !row[column].has_value()) return -1LL;
		try {
			return std::stoll(row[column].value());
		} catch (...) {
			return -1LL;
		}
	};
	bool writer_waited_exact_advisory = false;
	bool writer_held_target_before_advisory = false;
	bool writer_had_no_sidecar_before_advisory = false;
	for (unsigned attempt = 0; attempt < 1000; ++attempt) {
		const row_t locks = query_one(observer, fmt::format(
			"SELECT count(*) FILTER (WHERE relation={}::oid "
			"AND mode='AccessShareLock' AND granted)::text, "
			"count(*) FILTER (WHERE {} AND NOT granted)::text, "
			"count(*) FILTER (WHERE relation={}::oid)::text "
			"FROM pg_locks WHERE pid={};",
			relation_oids[0].value(), exact_advisory_predicate,
			relation_oids[1].value(), writer_pid));
		if (count_cell(locks, 1) > 0) {
			writer_waited_exact_advisory = true;
			writer_held_target_before_advisory = count_cell(locks, 0) > 0;
			writer_had_no_sidecar_before_advisory = count_cell(locks, 2) == 0;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	Doc0373ScopedAsyncQuery drop_query{dropper.raw()};
	const std::string drop_sql = fmt::format(
		"DROP TABLE {};", kBatchJoinTarget);
	const bool drop_sent = drop_query.send(drop_sql);
	bool target_wait_observed = false;
	bool drop_had_no_sidecar_lock = false;
	for (unsigned attempt = 0; drop_sent && attempt < 1000; ++attempt) {
		const row_t locks = query_one(observer, fmt::format(
			"SELECT count(*) FILTER (WHERE relation={}::oid "
			"AND mode='AccessExclusiveLock' AND NOT granted)::text, "
			"count(*) FILTER (WHERE relation={}::oid)::text "
			"FROM pg_locks WHERE pid={};",
			relation_oids[0].value(), relation_oids[1].value(), drop_pid));
		if (count_cell(locks, 0) > 0) {
			target_wait_observed = true;
			drop_had_no_sidecar_lock = count_cell(locks, 1) == 0;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	/* Release only the session advisory lock. The gate transaction intentionally
	 * retains sidecar AccessExclusiveLock, so the writer must advance from one
	 * exact observable wait to the next while keeping its target lock. */
	const bool gate_advisory_released = query_scalar(gate, fmt::format(
		"SELECT pg_advisory_unlock({}::bigint);", advisory_key)) == "t";
	bool writer_waited_sidecar = false;
	bool writer_held_advisory_while_waiting_sidecar = false;
	bool writer_held_target_while_waiting_sidecar = false;
	bool writer_had_no_granted_sidecar = false;
	bool gate_retained_sidecar = false;
	for (unsigned attempt = 0; attempt < 1000; ++attempt) {
		const row_t locks = query_one(observer, fmt::format(
			"SELECT count(*) FILTER (WHERE relation={}::oid "
			"AND mode='AccessShareLock' AND granted)::text, "
			"count(*) FILTER (WHERE {} AND granted)::text, "
			"count(*) FILTER (WHERE relation={}::oid AND NOT granted)::text, "
			"count(*) FILTER (WHERE relation={}::oid AND granted)::text, "
			"(SELECT count(*) FROM pg_locks WHERE pid={} "
			"AND relation={}::oid AND mode='AccessExclusiveLock' "
			"AND granted)::text "
			"FROM pg_locks WHERE pid={};",
			relation_oids[0].value(), exact_advisory_predicate,
			relation_oids[1].value(), relation_oids[1].value(), gate_pid,
			relation_oids[1].value(), writer_pid));
		if (count_cell(locks, 2) > 0) {
			writer_waited_sidecar = true;
			writer_held_target_while_waiting_sidecar =
				count_cell(locks, 0) > 0;
			writer_held_advisory_while_waiting_sidecar =
				count_cell(locks, 1) > 0;
			writer_had_no_granted_sidecar = count_cell(locks, 3) == 0;
			gate_retained_sidecar = count_cell(locks, 4) == 1;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}

	/* No assertions may unwind while either asynchronous command is live. */
	exec_sql(gate, "COMMIT;");
	const Doc0373AsyncQueryOutcome writer_outcome =
		writer_query.drain(static_cast<int>(PGRES_TUPLES_OK), 3000);
	const bool writer_connection_idle = writer_outcome.ready
		|| writer_query.cancel_and_drain();
	exec_sql(reader, "COMMIT;");
	if (writer_connection_idle) {
		exec_sql(writer,
			writer_outcome.ready && writer_outcome.got_result
				&& writer_outcome.expected_status
			? "COMMIT;" : "ROLLBACK;");
	}
	const Doc0373AsyncQueryOutcome drop_outcome =
		drop_sent && writer_connection_idle
		? drop_query.drain(static_cast<int>(PGRES_COMMAND_OK), 3000)
		: Doc0373AsyncQueryOutcome{
			.consume_ok = false,
			.ready = false,
			.got_result = false,
			.expected_status = false,
		};

	REQUIRE(writer_waited_exact_advisory);
	REQUIRE(writer_held_target_before_advisory);
	REQUIRE(writer_had_no_sidecar_before_advisory);
	REQUIRE(drop_sent);
	REQUIRE(target_wait_observed);
	REQUIRE(drop_had_no_sidecar_lock);
	REQUIRE(gate_advisory_released);
	REQUIRE(writer_waited_sidecar);
	REQUIRE(writer_held_target_while_waiting_sidecar);
	REQUIRE(writer_held_advisory_while_waiting_sidecar);
	REQUIRE(writer_had_no_granted_sidecar);
	REQUIRE(gate_retained_sidecar);
	REQUIRE(writer_outcome.consume_ok);
	REQUIRE(writer_outcome.ready);
	REQUIRE(writer_outcome.got_result);
	REQUIRE(writer_outcome.expected_status);
	REQUIRE(writer_connection_idle);
	REQUIRE(drop_outcome.consume_ok);
	REQUIRE(drop_outcome.ready);
	REQUIRE(drop_outcome.got_result);
	REQUIRE(drop_outcome.expected_status);
}

TEST_CASE("ordinal-2 BatchJoin rejects a malformed revision-3 relation without fallback",
			  "[mmsearch][batch-join][just-index][rev3][schema][malformed]"
			  "[doc0373][p1][cleanup]") {
	auto ctx = fresh_context({kBatchJoinTarget});
	seed_batch_join_target(ctx);
	load_batch_join_queries(ctx, 2);
	exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
	exec_sql(ctx, "SET bioseq.index_async_persist = off;");
	exec_sql(ctx, "SET bioseq.recompute_async_persist = off;");
	exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
	const auto rowno = static_cast<std::uint64_t>(query_scalar_int(
		ctx,
		fmt::format(
			"SELECT bioseq.column_row_number('{}'::regclass, ctid) "
			"FROM {} ORDER BY pk LIMIT 1;",
			kBatchJoinTarget, kBatchJoinTarget)));
	const row_t publication = query_one(ctx, fmt::format(
		"SELECT * FROM bioseq_internal.rev3_test_build_ready_mpf_generation("
		"'{}'::regclass, 'minimizers_per_fragment', {}, 'exact_simd');",
		kBatchJoinTarget, rowno));
	REQUIRE_FALSE(publication.empty());
	REQUIRE(publication[0].has_value());
	exec_sqlf(ctx,
		"ALTER TABLE bioseq_internal.{} "
		"ALTER COLUMN blob SET STORAGE EXTERNAL;",
		publication[0].value());

	const auto err = sql_error(ctx, batch_join_sql(2) + ";");
	REQUIRE(err.sqlstate == "XX001");
	REQUIRE(err.contains(
		"required compatible persisted minimizer index is invalid"));
	REQUIRE(err.contains("blob storage is not PLAIN"));
	REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("ordinal-2 BatchJoin LIMIT cleans up an undrained 8193-query reader",
		  "[mmsearch][batch-join][just-index][rev3][grouped32][limit][partial]") {
	auto ctx = fresh_context({kBatchJoinTarget});
	seed_batch_join_target(ctx);
	load_batch_join_queries(ctx, 8193);
	exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
	exec_sql(ctx, "SET bioseq.index_async_persist = off;");
	exec_sql(ctx, "SET bioseq.recompute_async_persist = off;");
	exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
	exec_sql(ctx,
		"SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';");
	exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'simd_mm2';");
	exec_sql(ctx,
		"SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
	const auto rowno = static_cast<std::uint64_t>(query_scalar_int(
		ctx,
		fmt::format(
			"SELECT bioseq.column_row_number('{}'::regclass, ctid) "
			"FROM {} ORDER BY pk LIMIT 1;",
			kBatchJoinTarget, kBatchJoinTarget)));
	const row_t publication = query_one(ctx, fmt::format(
		"SELECT * FROM bioseq_internal.rev3_test_build_ready_mpf_generation("
		"'{}'::regclass, 'minimizers_per_fragment', {}, 'exact_simd');",
		kBatchJoinTarget, rowno));
	REQUIRE(publication[8].value_or("") == "t");
	REQUIRE(publication[9].value_or("") == "t");
	const auto rows = query_rows(ctx, batch_join_sql(2) + " LIMIT 1;");
	REQUIRE(rows.size() == 1u);
	const auto stats = last_alignment_stats(ctx);
	REQUIRE(stats.size() == 20u);
	REQUIRE_FALSE(stats_bool(stats.front(), 13));
	REQUIRE(stats_bool(stats.front(), 14));
	REQUIRE_FALSE(stats_bool(stats.front(), 15));
	REQUIRE(stats_i64(stats.front(), 49) == 1);
	REQUIRE(stats_i64(stats.front(), 50) == 3);
}

TEST_CASE("BatchJoin LIMIT is partial and empty query child is fully drained",
          "[mmsearch][batch-join][limit][empty]") {
    SECTION("LIMIT") {
        auto ctx = fresh_context({kBatchJoinTarget});
        seed_batch_join_target(ctx);
        load_batch_join_queries(ctx, 4);
		const std::string sql = batch_join_sql(1) + " LIMIT 1";
		const auto plan = explain_text<false>(ctx, sql);
		REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
		REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
		REQUIRE(plan_contains(plan, "Bioseq Query Planner Rows"));
		/* The heap query provider is projection-free and may be represented as a
		 * SeqScan or IndexScan across PostgreSQL planning choices. Successful
		 * execution—not a brittle node-name substring—pins validation of the
		 * actual returned scan slot before query materialization. */
		REQUIRE_FALSE(query_rows(ctx, sql + ";").empty());
        const auto stats = last_alignment_stats(ctx);
        REQUIRE(stats.size() == 20);
        REQUIRE_FALSE(stats_bool(stats.front(), 13));
        REQUIRE(stats_bool(stats.front(), 14));
        REQUIRE_FALSE(stats_bool(stats.front(), 15));
    }

    SECTION("empty query child") {
        auto ctx = fresh_context({kBatchJoinTarget});
        seed_batch_join_target(ctx);
        load_batch_join_queries(ctx, 0);
        REQUIRE(query_rows(ctx, batch_join_sql(1) + ";").empty());
        require_batch_join_stats(ctx, 0, 0);
    }
}

TEST_CASE("BatchJoin validates the slot returned by a pass-through query child",
          "[mmsearch][batch-join][pass-through][slot-abi]") {
	/* OFFSET 0 prevents subquery pull-up and leaves a Limit/SubqueryScan
	 * provider whose executor may return its child's slot.  This pins the
	 * distinction between the post-setrefs Plan targetlist ABI and the actual
	 * slot returned by ExecProcNode. */
	auto ctx = fresh_context({kBatchJoinTarget});
	seed_batch_join_target(ctx);
	load_batch_join_queries(ctx, 4);
	const std::string sql = fmt::format(
		"SELECT q.id, t.pk, bioseq.has_cigar(t.pk) AS has_cigar "
		"FROM {} t JOIN "
		"(SELECT id, seq FROM {} OFFSET 0) q "
		"ON mmsearch(t.seq0, q.seq, "
		"aligner_options(index_read_requirement => 1))",
		kBatchJoinTarget,
		kBatchJoinQueries);
	const auto plan = explain_text<false>(ctx, sql);
	REQUIRE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
	REQUIRE(plan_contains(plan, "Bioseq Batch ABI Version: 2"));
	REQUIRE(plan_contains(plan, "Bioseq Query Planner Rows"));
	/* OFFSET 0 retains a pass-through provider contract, but PostgreSQL may
	 * omit or rename its physical node in text EXPLAIN. The successful rows
	 * below prove BatchJoin validated the slot actually returned by that child. */
	const auto rows = query_rows(ctx, sql + " ORDER BY q.id, t.pk;");
	REQUIRE(rows.size() == 4);
	for (const auto& row : rows) {
		REQUIRE(row.size() == 3);
		REQUIRE(row[2].has_value());
		REQUIRE(*row[2] == "t");
	}
	require_batch_join_stats(ctx, 1, 4);
}

TEST_CASE("unsupported BatchJoin shapes stay on ordinary planner paths",
          "[mmsearch][batch-join][planner][rejection]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);

    const auto target_projection = explain_text<false>(
        ctx, batch_join_sql(1, "t.seq0, q.id"));
    REQUIRE_FALSE(plan_contains(
        target_projection, "Custom Scan (BioseqBatchJoin)"));

    exec_sql(
        ctx,
        "CREATE TEMP TABLE mmsearch_batch_join_dna5 "
        "(id int PRIMARY KEY, seq bioseq.seq('dna5', 32));");
    exec_sqlf(
        ctx,
        "INSERT INTO mmsearch_batch_join_dna5 VALUES (1, {});",
        format_literal(
            "ACGTACGTACGTACGTACGTACGTACGTACGT",
            seq_typmod{.alphabet = "dna5", .width = 32}));
    const auto typmod_mismatch = explain_text<false>(
        ctx,
        fmt::format(
            "SELECT q.id, t.pk FROM {} t "
            "JOIN mmsearch_batch_join_dna5 q "
            "ON mmsearch(t.seq0, q.seq, "
            "aligner_options(index_read_requirement => 1));",
            kBatchJoinTarget));
    REQUIRE_FALSE(plan_contains(
        typmod_mismatch, "Custom Scan (BioseqBatchJoin)"));
}

TEST_CASE("target base mmsearch keeps ordinary parameterized scan ownership",
          "[mmsearch][batch-join][planner][target-qual][rejection]") {
    /* A locator-passthrough BatchJoin child cannot also own a target-side
     * base mmsearch.  The ordinary parameterized BioseqScan can safely bind
     * the join Param and stack the Const base qual, so prove both negative
     * planning and executable AND semantics.  Query 0 is intentionally a
     * no-match: if the join mmsearch leaked to its SQL rendezvous stub it
     * would incorrectly survive the target's literal-only base filter. */
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2, /*only_last_can_match=*/true);

    const std::string sql = batch_join_sql(1, "q.id, t.pk")
        + fmt::format(
            " WHERE mmsearch(t.seq0, {})",
            batch_join_target_literal());
    const auto plan = explain_text<false>(ctx, sql);
    REQUIRE_FALSE(plan_contains(plan, "Custom Scan (BioseqBatchJoin)"));
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));

    const auto rows = query_rows(ctx, sql + " ORDER BY q.id, t.pk;");
    REQUIRE(rows.size() == 1);
    REQUIRE(rows.front().size() == 2);
    REQUIRE(rows.front()[0].has_value());
    REQUIRE(rows.front()[1].has_value());
    REQUIRE(std::stoll(*rows.front()[0]) == 1);
    REQUIRE(std::stoll(*rows.front()[1]) == 1);
}

TEST_CASE("BatchJoin cursor publication waits for portal finish and stays partial",
          "[mmsearch][batch-join][portal][lifecycle]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 4);

    (void)query_rows(ctx, index_requirement_sql(kBatchJoinTarget, 1));
    const long long prior_id = stats_i64(last_alignment_stats(ctx).front(), 0);

    exec_sql(ctx, "BEGIN;");
    exec_sql(
        ctx,
        "DECLARE batch_join_cursor NO SCROLL CURSOR FOR "
        + batch_join_sql(1, "q.id, t.pk") + " ORDER BY q.id, t.pk;");
    REQUIRE(query_rows(ctx, "FETCH 1 FROM batch_join_cursor;").size() == 1);
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
    REQUIRE(stats_i64(last_alignment_stats(ctx).front(), 0) == prior_id);
    exec_sql(ctx, "CLOSE batch_join_cursor;");
    exec_sql(ctx, "COMMIT;");

    const auto stats = last_alignment_stats(ctx);
    REQUIRE(stats.size() == 20);
    REQUIRE(stats_i64(stats.front(), 0) == prior_id + 1);
    REQUIRE_FALSE(stats_bool(stats.front(), 13));
    REQUIRE(stats_bool(stats.front(), 14));
    REQUIRE_FALSE(stats_bool(stats.front(), 15));
}

TEST_CASE("nested SPI BatchJoin attaches once to its executor root",
          "[mmsearch][batch-join][nested][spi][lifecycle]") {
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);
    exec_sql(
        ctx,
        "CREATE OR REPLACE FUNCTION pg_temp.run_batch_join_spi() "
        "RETURNS bigint LANGUAGE plpgsql AS $$ "
        "DECLARE n bigint; BEGIN "
        "SELECT count(*)::bigint INTO n FROM mmsearch_batch_join_target_int t "
        "JOIN mmsearch_batch_join_queries_int q "
        "ON mmsearch(t.seq0, q.seq, "
        "aligner_options(index_read_requirement => 1)); "
        "RETURN n; END $$;");
    REQUIRE(query_scalar_int(
        ctx, "SELECT pg_temp.run_batch_join_spi();") > 0);

    const auto stats = last_alignment_stats(ctx);
    REQUIRE(stats.size() == 20);
    REQUIRE(stats_i64(stats.front(), 11) == 1);
    REQUIRE(stats_i64(stats.front(), 12) == 1);
    REQUIRE(stats_i64(stats.front(), 32) == 1);
    REQUIRE(stats_i64(stats.front(), 33) == 2);
	REQUIRE(stats_i64(stats.front(), 45) == 0);
	REQUIRE(stats_i64(stats.front(), 46) == 0);
	REQUIRE(stats_i64(stats.front(), 47) == 1);
    REQUIRE(stats_bool(stats.front(), 13));
}

TEST_CASE("GPU target outcomes count logical rows with identical sequence content",
          "[mmsearch][columnar][pipeline-stats][vulkan][logical-row]") {
    auto ctx = fresh_context({kDuplicateTargets});
    const std::string identical_target =
        hoe_seed_rows().front().sequences.front();
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{kDuplicateTargets},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {identical_target}},
            seq_row{.pk = 2, .sequences = {identical_target}},
        });
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'vulkan_tiled_dp';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_seed_hits_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_index_cache_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_fused_frontend_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_gpu_index_build_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_extend_enabled = on;");
    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kDuplicateTargets,
        hoe_probe_literal());
    if (selected_vulkan_runtime_untrusted(ctx)) {
        const auto err = sql_error(ctx, sql);
        REQUIRE(err.sqlstate == "XX000");
        REQUIRE(err.contains("Vulkan aligner execution failed"));
        return;
    }

    const auto plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(
        plan, "attempted=2 fully_attested=0 unattested=2"));
    REQUIRE(plan_contains(plan, "Index outcomes [targets=2"));
    REQUIRE(plan_contains(plan, "recomputed=2]"));
    REQUIRE(plan_contains(plan, "GPU dispatch [seed="));
    REQUIRE_FALSE(plan_contains(plan, "GPU dispatch [seed=0 "));
    REQUIRE_FALSE(plan_contains(plan, " chain=0 chain_backtrack="));
    REQUIRE_FALSE(plan_contains(plan, " chain_backtrack=0 "));
    const auto tiled_chain_dispatches =
        find_explain_property(plan, "GPU Chain Dispatches");
    const auto tiled_backtrack_dispatches =
        find_explain_property(plan, "GPU Chain Backtrack Dispatches");
    const auto coordinate_vote_dispatches =
        find_explain_property(plan, "GPU Coordinate Vote Dispatches");
    REQUIRE(tiled_chain_dispatches.has_value());
    REQUIRE(std::stoull(*tiled_chain_dispatches) > 0u);
    REQUIRE(tiled_backtrack_dispatches.has_value());
    REQUIRE(std::stoull(*tiled_backtrack_dispatches) > 0u);
    REQUIRE(coordinate_vote_dispatches.has_value());
    REQUIRE(std::stoull(*coordinate_vote_dispatches) == 0u);
    REQUIRE(plan_contains(
        plan, "backend_declines=0 backend_fallbacks=0"));
}

TEST_CASE("USING bioseq table without mmsearch qual emits zero rows",
          "[mmsearch][columnar][no_qual]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    // Pinned per SPEC_HOW_TO_CONNECT_ALIGNER_3 §13.1: the custom scan only
    // buffers tuples when SeqMmsearchOpExpr is part of the scan qual list.
    REQUIRE(query_scalar_int(
                ctx, fmt::format("SELECT COUNT(*)::bigint FROM {};", kHoe)) ==
            0);
}

TEST_CASE(
    "mmsearch result set is byte-identical across the full GUC parity matrix",
    "[mmsearch][parity]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string sql = fmt::format(
        "SELECT pk, "
        "       bioseq.query_start(pk)  AS qs, "
        "       bioseq.query_end(pk)    AS qe, "
        "       bioseq.target_start(pk) AS ts, "
        "       bioseq.target_end(pk)   AS te, "
        "       bioseq.cigar(pk)        AS cg "
        "FROM {} "
        "WHERE mmsearch(seq0, {}) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal());

    const auto reference = expect_query_invariant_under_options(
        ctx, sql,
        {
            {"bioseq.enable_vectorization",     {"on", "off"}},
            {"bioseq.enable_column_cache",      {"on", "off"}},
            {"bioseq.enable_bioseq_index_scan", {"on", "off"}},
            {"bioseq.aligner.enable_profiling", {"on", "off"}},
        });

    // CIGAR strings on materialised rows must be either empty (no alignment)
    // or syntactically valid.
    for (const auto& row : reference) {
        REQUIRE(row.size() == 6);
        if (row[5].has_value() && !row[5].value().empty()) {
            REQUIRE(is_valid_cigar(row[5].value()));
        }
    }
}

namespace {
void check_single_guc_parity(std::string_view guc) {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());
    expect_invariant_under_guc_toggle(ctx, sql, guc);
}
}  // namespace

TEST_CASE("mmsearch parity: bioseq.enable_vectorization on/off agree",
          "[mmsearch][parity][toggle]") {
    check_single_guc_parity("bioseq.enable_vectorization");
}

TEST_CASE("mmsearch parity: bioseq.enable_column_cache on/off agree",
          "[mmsearch][parity][toggle]") {
    check_single_guc_parity("bioseq.enable_column_cache");
}

TEST_CASE("mmsearch parity: bioseq.enable_bioseq_index_scan on/off agree",
          "[mmsearch][parity][toggle]") {
    check_single_guc_parity("bioseq.enable_bioseq_index_scan");
}

TEST_CASE("mmsearch parity: bioseq.aligner.enable_profiling on/off agree",
          "[mmsearch][parity][toggle]") {
    check_single_guc_parity("bioseq.aligner.enable_profiling");
}

TEST_CASE("mmsearch with all-NULL aligner_options matches the GUC defaults",
          "[mmsearch][aligner_options]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string base_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());

    const std::string overridden_sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, aligner_options()) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal());

    REQUIRE(query_rows(ctx, base_sql) == query_rows(ctx, overridden_sql));
}

TEST_CASE("aligner_options rejects an invalid index read requirement ordinal",
          "[mmsearch][aligner_options][index-read-requirement][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    for (const int invalid_ordinal : {-1, 3}) {
        INFO("invalid ordinal=" << invalid_ordinal);
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
            "aligner_options(index_read_requirement => {})) ORDER BY pk;",
            kHoe,
            hoe_probe_literal(),
            invalid_ordinal);
        const auto err = sql_error(ctx, sql);
        REQUIRE(err.sqlstate == "22023");
        REQUIRE(err.contains("index_read_requirement out of range"));
    }
}

TEST_CASE("aligner_options exposes the appended index requirement field",
          "[mmsearch][aligner_options][index-read-requirement][layout]") {
    connection_test_context ctx;
    REQUIRE(query_scalar(
        ctx,
        "SELECT ((aligner_options()).index_read_requirement IS NULL)::text;")
        == "true");
    REQUIRE(query_scalar_int(
        ctx,
        "SELECT (aligner_options(index_read_requirement => 0))."
        "index_read_requirement;") == 0);
    REQUIRE(query_scalar(
        ctx,
        "SELECT string_agg(attname || ':' || atttypid::text, ',' "
        "ORDER BY attnum) FROM pg_attribute "
        "WHERE attrelid='bioseq_aligner_options'::regclass AND attnum > 0;")
        == "kmer_size:23,window_size:23,max_gap:23,max_gap_ref:23,"
           "bandwidth:23,bandwidth_long:23,max_chain_skip:23,"
           "max_chain_iter:23,min_seed_count:23,min_chain_score:23,"
           "chain_gap_scale:701,chain_skip_scale:701,mask_level:701,"
           "mask_len:23,pri_ratio:701,best_n:23,match_score:23,"
           "mismatch_pen:23,gap_open1:23,gap_ext1:23,gap_open2:23,"
           "gap_ext2:23,sc_ambi:23,zdrop:23,end_bonus:23,max_occ:23,"
           "mid_occ:23,block_min_size:23,block_max_size:23,"
           "chaining_algorithm:23,base_alignment_algorithm:23,"
           "minimizer_algorithm:23,vulkan_enabled:16,"
           "vulkan_seed_hits_enabled:16,vulkan_index_cache_enabled:16,"
           "index_read_requirement:23");
}

TEST_CASE("mmsearch rejects a non-Const aligner_options third argument",
          "[mmsearch][aligner_options][index-read-requirement][shape][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET plan_cache_mode = force_generic_plan;");
    exec_sql(ctx, fmt::format(
        "PREPARE mmsearch_nonconst_options(bioseq_aligner_options) AS "
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}, $1) ORDER BY pk;",
        kHoe,
        hoe_probe_literal()));
    const auto err = sql_error(
        ctx,
        "EXECUTE mmsearch_nonconst_options("
        "aligner_options(index_read_requirement => 1));");
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("must be a plan-time constant"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("index read requirement allow ordinal preserves production policy",
          "[mmsearch][aligner_options][index-read-requirement][allow]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    const std::string sql = index_requirement_sql(kHoe, 0);

    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    const auto disabled_plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(
        disabled_plan,
        "requested_index_source=recomputed actual_index_source=recomputed"));
    REQUIRE(plan_contains(disabled_plan, "persisted=0"));

    exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
    const auto enabled_plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(
        enabled_plan,
        "requested_index_source=persisted actual_index_source=persisted"));
    REQUIRE(plan_contains(enabled_plan, "Index outcomes [targets=5 persisted=5"));
}

TEST_CASE("stacked mmsearch rejects conflicting index read requirements",
          "[mmsearch][aligner_options][index-read-requirement][stacked][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    const std::string second_probe = format_literal(
        "CTAGTCACTGTAGCCTGATCGTAGCTAGC",
        seq_typmod{.alphabet = "dna4", .width = 32});
    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE "
        "mmsearch(seq0, {}, aligner_options(index_read_requirement => 1)) "
        "AND mmsearch(seq0, {}, aligner_options(index_read_requirement => 2)) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal(),
        second_probe);
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "22023");
    REQUIRE(err.contains("index_read_requirement conflicts"));
}

TEST_CASE("stacked mmsearch resolves one explicit requirement independent of order",
          "[mmsearch][aligner_options][index-read-requirement][stacked]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
    const std::string second_probe = format_literal(
        "CTAGTCACTGTAGCCTGATCGTAGCTAGC",
        seq_typmod{.alphabet = "dna4", .width = 32});
    const std::array<std::string, 2> sqls{
        fmt::format(
            "SELECT pk FROM {} WHERE "
            "mmsearch(seq0, {}, aligner_options()) AND "
            "mmsearch(seq0, {}, aligner_options(index_read_requirement => 1)) "
            "ORDER BY pk;",
            kHoe,
            hoe_probe_literal(),
            second_probe),
        fmt::format(
            "SELECT pk FROM {} WHERE "
            "mmsearch(seq0, {}, aligner_options(index_read_requirement => 1)) "
            "AND mmsearch(seq0, {}, aligner_options()) ORDER BY pk;",
            kHoe,
            hoe_probe_literal(),
            second_probe),
    };
    for (const auto& sql : sqls) {
        const auto plan = explain_text<true>(ctx, sql);
        REQUIRE(plan_contains(
            plan,
            "requested_index_source=recomputed actual_index_source=recomputed"));
        REQUIRE(plan_contains(plan, "persisted=0"));
    }
}

TEST_CASE("require_absent bypasses a prewarmed persisted assembled cache",
          "[mmsearch][aligner_options][index-read-requirement][absent]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
    const auto warm_plan = explain_text<true>(
        ctx, index_requirement_sql(kHoe, 0));
    REQUIRE(plan_contains(warm_plan, "Index outcomes [targets=5 persisted=5"));

    const std::string sql = index_requirement_sql(kHoe, 1);
    const auto plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(
        plan,
        "requested_index_source=recomputed actual_index_source=recomputed"));
    REQUIRE(plan_contains(plan, "Index outcomes [targets=5 persisted=0"));
    REQUIRE(plan_contains(plan, "missing_persisted=0 recomputed=5]"));
}

TEST_CASE("require_compatible_persisted rejects current legacy MPF",
          "[mmsearch][aligner_options][index-read-requirement][persisted][legacy][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", kHoe) > 0);
    require_no_current_compatible_persisted_index(
        ctx, index_requirement_sql(kHoe, 2));
}

TEST_CASE("require_compatible_persisted classifies a damaged legacy lookalike as invalid",
          "[mmsearch][aligner_options][index-read-requirement][persisted][legacy][malformed]") {
    // The live ordinal-2 storage classifier is owned by CPU BioseqBatchJoin;
    // ordinary non-prepared scans intentionally fail before inspecting any
    // sidecar. Exercise the real prepared route instead of asserting an error
    // precedence that route cannot prove.
    auto ctx = fresh_context({kBatchJoinTarget});
    seed_batch_join_target(ctx);
    load_batch_join_queries(ctx, 2);
    exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
    const auto sidecars = query_rows(ctx,
        "SELECT c.relname FROM pg_class c "
        "JOIN pg_namespace n ON n.oid=c.relnamespace "
        "WHERE n.nspname='bioseq_internal' "
        "AND c.relkind='r' AND c.relname LIKE 'seqidx_mpf_%' "
        "ORDER BY c.relname;");
    REQUIRE_FALSE(sidecars.empty());
    for (const row_t& sidecar : sidecars) {
        REQUIRE(sidecar.size() == 1);
        REQUIRE(sidecar[0].has_value());
        exec_sqlf(ctx,
            "ALTER TABLE bioseq_internal.{} "
            "ALTER COLUMN blob SET STORAGE PLAIN;",
            sidecar[0].value());
    }

    const auto err = sql_error(ctx, batch_join_sql(2) + ";");
    REQUIRE(err.sqlstate == "XX001");
    REQUIRE(err.contains(
        "required compatible persisted minimizer index is invalid"));
    REQUIRE(err.contains("neither the canonical revision-3 schema"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("require_compatible_persisted rejects current proper MPS",
          "[mmsearch][aligner_options][index-read-requirement][persisted][mps][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    REQUIRE(query_scalar_int(ctx, fmt::format(
        "SELECT bioseq.update_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        kHoe)) > 0);
    exec_sql(ctx, fmt::format(
        "SELECT bioseq.set_active_read_index('{}'::regclass, "
        "'minimizers_per_sequence');",
        kHoe));
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_sequence", kHoe) > 0);
    require_no_current_compatible_persisted_index(
        ctx, index_requirement_sql(kHoe, 2));
}

TEST_CASE("active-read policy preserves configured versus authoritative no-row source",
		  "[mmsearch][index-policy][source-attestation]") {
	auto ctx = fresh_context({kHoe});
	seed_hoe_fixture(ctx, kHoe);
	exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
	const std::string search = index_requirement_sql(kHoe, 0);

	const auto policy_debug = [&] {
		const auto rows = query_rows(
			ctx, "SELECT * FROM bioseq_internal.active_read_policy_debug();");
		REQUIRE(rows.size() == 1);
		REQUIRE(rows.front().size() == 9);
		REQUIRE(rows.front()[7].value_or("") == "0");
		REQUIRE(rows.front()[8].value_or("") == "0");
		return rows.front();
	};
	const auto value = [](const row_t& row, const std::size_t column) {
		REQUIRE(column < row.size());
		REQUIRE(row[column].has_value());
		return std::stoll(*row[column]);
	};

	// An authoritative zero-row result selects the documented transient MPF
	// shape, but its provenance must not be relabelled as a configured row.
	exec_sqlf(ctx,
		"UPDATE bioseq_internal.index_policy SET is_active_read = false "
		"WHERE user_relid = '{}'::regclass;",
		kHoe);
	exec_sql(ctx,
		"SELECT bioseq_internal.reset_active_read_policy_debug();");
	(void)query_rows(ctx, search);
	const row_t authoritative = policy_debug();
	REQUIRE(value(authoritative, 0) == 0);
	REQUIRE(value(authoritative, 1) > 0);
	REQUIRE(value(authoritative, 2) == 0);
	REQUIRE(value(authoritative, 3) == 0);
	REQUIRE(value(authoritative, 4) > 0);
	REQUIRE(value(authoritative, 5) == 0);
	REQUIRE(authoritative[6].value_or("")
		== "authoritative_no_row_default");

	// Setting the same physical MPF choice creates an explicit configured-row
	// source. The algorithm name may match the default; the attestation source
	// and both resolver/dispatch counters still must differ.
	exec_sqlf(ctx,
		"SELECT bioseq.set_active_read_index('{}'::regclass, "
		"'minimizers_per_fragment');",
		kHoe);
	exec_sql(ctx,
		"SELECT bioseq_internal.reset_active_read_policy_debug();");
	(void)query_rows(ctx, search);
	const row_t configured = policy_debug();
	REQUIRE(value(configured, 0) > 0);
	REQUIRE(value(configured, 1) == 0);
	REQUIRE(value(configured, 2) == 0);
	REQUIRE(value(configured, 3) > 0);
	REQUIRE(value(configured, 4) == 0);
	REQUIRE(value(configured, 5) == 0);
	REQUIRE(configured[6].value_or("") == "configured_row");
}

TEST_CASE("require_compatible_persisted rejects a prewarmed legacy cache",
          "[mmsearch][aligner_options][index-read-requirement][persisted][cache][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
    const auto warm_plan = explain_text<true>(
        ctx, index_requirement_sql(kHoe, 0));
    REQUIRE(plan_contains(warm_plan, "Index outcomes [targets=5 persisted=5"));
    require_no_current_compatible_persisted_index(
        ctx, index_requirement_sql(kHoe, 2));
}

TEST_CASE("require_compatible_persisted errors when target chunks are missing",
          "[mmsearch][aligner_options][index-read-requirement][missing][error]") {
    auto ctx = fresh_context({kMissingIndex});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{kMissingIndex},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{
                .pk = 1,
                .sequences = {hoe_seed_rows().front().sequences.front()}},
        });
    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", kMissingIndex) == 0);
    require_no_current_compatible_persisted_index(
        ctx, index_requirement_sql(kMissingIndex, 2));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("require_compatible_persisted rejects before alternate scanner and GPU paths",
          "[mmsearch][aligner_options][index-read-requirement][strict-paths][error]") {
    auto ctx = fresh_context({kMissingIndex});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{kMissingIndex},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {
            seq_row{.pk = 1, .sequences = {"ACGTACGTACGTACGT"}},
            seq_row{.pk = 2, .sequences = {"TGCATGCATGCATGCA"}},
        });
    const std::string sql = index_requirement_sql(kMissingIndex, 2);

    INFO("small inline-eligible target path");
    require_no_current_compatible_persisted_index(ctx, sql);

    INFO("forced in-memory path");
    exec_sql(ctx, "SET bioseq.test_force_inmemory_scanner = on;");
    require_no_current_compatible_persisted_index(ctx, sql);
    exec_sql(ctx, "SET bioseq.test_force_inmemory_scanner = off;");

    INFO("forced chunked path");
    exec_sql(ctx, "SET bioseq.test_force_chunked_scanner = on;");
    require_no_current_compatible_persisted_index(ctx, sql);
    exec_sql(ctx, "SET bioseq.test_force_chunked_scanner = off;");

    INFO("batched selected-Vulkan path");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'vulkan_tiled_dp';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_seed_hits_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_index_cache_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_fused_frontend_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_gpu_index_build_enabled = on;");
    require_no_current_compatible_persisted_index(ctx, sql);
}

TEST_CASE("require_absent preserves on-insert policy and prohibits external async files",
		  "[mmsearch][aligner_options][index-read-requirement][absent][write-policy][no-external-file]") {
    auto ctx = fresh_context({kMissingIndex});
    exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{kMissingIndex},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        });
    exec_sql(ctx, fmt::format(
        "INSERT INTO {} VALUES "
        "(1, repeat('ACGT', 2000)::bioseq.seq('dna4', 32));",
        kMissingIndex));
    REQUIRE(index_table_row_count(
        ctx, "minimizers_per_fragment", kMissingIndex) == 0);

    exec_sql(ctx, "SET bioseq.read_sequence_index = on;");
    exec_sql(ctx, "SET bioseq.index_async_persist = on;");
    exec_sql(ctx, "SET bioseq.recompute_async_persist = on;");
    const auto async_runtime = query_one(
        ctx,
        "SELECT current_setting('shared_preload_libraries', true), "
        "supervisor::text FROM bioseq.scheduler_stats();");
    REQUIRE(async_runtime.size() == 2);
    REQUIRE(async_runtime[0].has_value());
    REQUIRE(async_runtime[1].has_value());
    INFO("async persistence requires bioseq in shared_preload_libraries and "
         "a live scheduler supervisor; observed shared_preload_libraries='"
         << *async_runtime[0] << "', supervisor=" << *async_runtime[1]);
    REQUIRE(*async_runtime[1] == "true");
    const auto absent_plan = explain_text<true>(
        ctx, index_requirement_sql(kMissingIndex, 1));
    REQUIRE(plan_contains(absent_plan, "persisted=0"));
    REQUIRE(plan_contains(absent_plan, "recomputed=1"));
    REQUIRE(query_scalar(ctx, "SHOW bioseq.index_async_persist;") == "on");
    REQUIRE(query_scalar(ctx, "SHOW bioseq.recompute_async_persist;") == "on");
    REQUIRE(query_scalar(ctx, "SHOW bioseq.update_index_on_insert;") == "off");

	// Legacy MPF recompute used to create a /tmp biological payload before the
	// queue rejected it. The producer is now absent at source, so no async MPF
	// rows appear and both constructor/writer entry proofs stay exactly zero.
	REQUIRE(index_table_row_count(
		ctx, "minimizers_per_fragment", kMissingIndex) == 0);
	const auto producer = query_one(
		ctx,
		"SELECT prohibited_file_constructor_entries, "
		"prohibited_file_writer_entries "
		"FROM bioseq_internal.active_read_policy_debug();");
	REQUIRE(producer.size() == 2);
	REQUIRE(producer[0].value_or("") == "0");
	REQUIRE(producer[1].value_or("") == "0");

    exec_sql(ctx, "SET bioseq.update_index_on_insert = on;");
    exec_sql(ctx, fmt::format(
        "INSERT INTO {} VALUES "
        "(2, repeat('TGCA', 2000)::bioseq.seq('dna4', 32));",
        kMissingIndex));
	REQUIRE(query_scalar(ctx, "SHOW bioseq.update_index_on_insert;") == "on");
	REQUIRE(index_table_row_count(
		ctx, "minimizers_per_fragment", kMissingIndex) > 0);
}

TEST_CASE("async MPS staging and worker PostgreSQL leaves recover exactly",
		  "[mmsearch][async-mps][pg-leaf][recovery][admission][dsm]") {
	auto ctx = fresh_context({kAsyncMpsWorkerLeaves});
	exec_sqlf(ctx,
		"CREATE TABLE {} (pk int, seq0 bioseq.seq('dna4', 32)) USING bioseq;",
		kAsyncMpsWorkerLeaves);
	exec_sqlf(ctx,
		"SELECT bioseq.set_index_policy('{}'::regclass, "
		"'minimizers_per_fragment', 'UPDATE_MANUALLY');",
		kAsyncMpsWorkerLeaves);
	exec_sqlf(ctx,
		"SELECT bioseq.set_index_policy('{}'::regclass, "
		"'minimizers_per_sequence', 'UPDATE_ON_INSERT');",
		kAsyncMpsWorkerLeaves);
	exec_sqlf(ctx,
		"SELECT bioseq.set_active_read_index('{}'::regclass, "
		"'minimizers_per_sequence');",
		kAsyncMpsWorkerLeaves);
	exec_sql(ctx, "SET bioseq.update_index_on_insert = off;");
	exec_sqlf(ctx,
		"INSERT INTO {} VALUES "
		"(1, repeat('ACGT', 10000)::bioseq.seq('dna4', 32));",
		kAsyncMpsWorkerLeaves);
	exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
	exec_sql(ctx, "SET bioseq.index_async_persist = on;");
	exec_sql(ctx, "SET bioseq.recompute_async_persist = on;");
	exec_sql(ctx,
		"SET bioseq.scheduler.pipeline_concurrency_mode = 'task_parallel';");
	exec_sql(ctx, "SET bioseq.index_memory_cap_mb = 256;");
	REQUIRE(query_scalar(
		ctx, "SELECT supervisor::text FROM bioseq.scheduler_stats();") == "true");
	exec_sql(ctx,
		"SELECT bioseq_internal.disarm_async_mps_worker_failure();");

	const std::string trigger_sql = fmt::format(
		"SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
		"aligner_options(index_read_requirement => 1));",
		kAsyncMpsWorkerLeaves, hoe_probe_literal());
	// Staging runs in this client backend. Each injected PG leaf must unwind
	// the attached/pinned DSM state, abort only that statement, and leave the
	// next fresh transaction able to stage the same identity again.
	constexpr std::array<std::string_view, 4> stage_failure_points{
		"stage_register_xact_callback", "stage_dsm_create",
		"stage_dsm_pin", "stage_dsm_detach"};
	for (const std::string_view point : stage_failure_points) {
		REQUIRE(query_scalar(ctx, fmt::format(
			"SELECT bioseq_internal.arm_async_mps_worker_failure('{}')::text;",
			point)) == "true");
		const auto error = sql_error(ctx, trigger_sql);
		REQUIRE(error.sqlstate == "XX000");
		REQUIRE(error.contains(
			"injected asynchronous MPS staging PostgreSQL leaf failure"));
		REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
	}
	const auto boundary = [&] {
		const auto row = query_one(ctx,
			"SELECT * FROM "
			"bioseq_internal.async_mps_worker_boundary_stats();");
		REQUIRE(row.size() == 14);
		return row;
	};
	const auto count_at = [](const row_t& row, const std::size_t column) {
		REQUIRE(column < row.size());
		REQUIRE(row[column].has_value());
		return std::stoll(*row[column]);
	};
	const auto async_jobs_waiting = [&] {
		return query_scalar_int(ctx,
			"SELECT async_persist_jobs_waiting "
			"FROM bioseq.scheduler_stats();");
	};
	const auto wait_for_settled = [&](
			const row_t& before,
			const bool materialize,
			const long long waiting_before) {
		row_t observed;
		for (int attempt = 0; attempt < 400; ++attempt) {
			observed = boundary();
			const bool failure_settled =
				count_at(observed, 4) == count_at(before, 4) + 1
				&& count_at(observed, 5) == count_at(before, 5) + 1
				&& observed[11].value_or("") == "none";
			const bool admission_settled = !materialize
				|| (count_at(observed, 6) > count_at(before, 6)
					&& count_at(observed, 7) == count_at(before, 7) + 1
					&& count_at(observed, 8) == count_at(before, 8) + 1
					&& count_at(observed, 9) == count_at(before, 9));
			if (failure_settled && admission_settled
				&& async_jobs_waiting() == waiting_before) {
				return observed;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{25});
		}
		FAIL("asynchronous MPS worker failure did not fully unwind");
		return observed;
	};

	constexpr std::array<std::string_view, 10> failure_points{
		"relation_open", "relation_identity", "relation_close",
		"spi_connect", "spi_execute", "spi_read", "spi_finish",
		"materialize_error_log", "materialize_interrupt",
		"materialize_scanner_read"};
	const long long initial_admission_mismatches = count_at(boundary(), 9);
	for (const std::string_view point : failure_points) {
		const row_t before = boundary();
		const long long waiting_before = async_jobs_waiting();
		REQUIRE(query_scalar(ctx, fmt::format(
			"SELECT bioseq_internal.arm_async_mps_worker_failure('{}')::text;",
			point)) == "true");
		(void)query_rows(ctx, trigger_sql);
		const bool materialize = point.starts_with("materialize_");
		const row_t after = wait_for_settled(
			before, materialize, waiting_before);
		REQUIRE(count_at(after, 4) > count_at(before, 4));
		REQUIRE(count_at(after, 5) == count_at(before, 5) + 1);
		REQUIRE(after[10].value_or("") == point);
		REQUIRE(after[11].value_or("") == "none");
		REQUIRE(after[12].value_or("") == "XX000");
		REQUIRE(after[13].value_or("")
			== "injected asynchronous MPS worker PostgreSQL leaf failure");

		if (materialize) {
			REQUIRE(count_at(after, 6) > count_at(before, 6));
			REQUIRE(count_at(after, 7) == count_at(before, 7) + 1);
			REQUIRE(count_at(after, 8) == count_at(before, 8) + 1);
			REQUIRE(count_at(after, 9) == count_at(before, 9));
		}
		// The client backend is unchanged, and the next iteration's job proving
		// another worker leaf is also a worker-process recovery check.
		REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
	}

	// Explicitly disarm, then prove the same worker accepts and completes a
	// clean MPS build after the chain of ten failed jobs.
	exec_sql(ctx,
		"SELECT bioseq_internal.disarm_async_mps_worker_failure();");
	const long long generation_before = query_scalar_int(
		ctx, "SELECT generation FROM bioseq.scheduler_mps_build_stats();");
	(void)query_rows(ctx, trigger_sql);
	long long generation_after = generation_before;
	for (int attempt = 0;
		 attempt < 400 && generation_after <= generation_before; ++attempt) {
		std::this_thread::sleep_for(std::chrono::milliseconds{25});
		generation_after = query_scalar_int(
			ctx, "SELECT generation FROM bioseq.scheduler_mps_build_stats();");
	}
	REQUIRE(generation_after > generation_before);
	const row_t recovered = boundary();
	REQUIRE(recovered[11].value_or("") == "none");
	REQUIRE(count_at(recovered, 9) == initial_admission_mismatches);
}

// Each named override field is a separate TEST_CASE; the helper keeps the body
// of each call site to a single line.
namespace {
void check_aligner_options_partial_override(std::string_view named_arg_sql) {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const std::string overridden_sql = fmt::format(
        "SELECT pk FROM {} "
        "WHERE mmsearch(seq0, {}, aligner_options({})) "
        "ORDER BY pk;",
        kHoe, hoe_probe_literal(), named_arg_sql);

    // We assert the query executes and stays inside the seeded pk domain;
    // the override may shift the match set but must not raise.
    const auto pks = query_int_column(ctx, overridden_sql);
    const std::set<long long> seeded{1, 2, 3, 4, 5};
    for (const auto pk : pks) {
        REQUIRE(seeded.contains(pk));
    }
}
}  // namespace

TEST_CASE("aligner_options(kmer_size => 11) executes without errors",
          "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("kmer_size => 11");
}

// Strengthens item 3 of the round-2 review: a per-call override on
// `kmer_size` must produce the same row set as setting the corresponding
// `bioseq.aligner.kmer_size` GUC ahead of a plain two-arg mmsearch. If they
// diverge, the override is plumbed differently and the suite catches it.
TEST_CASE("aligner_options(kmer_size => K) matches SET bioseq.aligner.kmer_size = K",
          "[mmsearch][aligner_options][parity]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    constexpr int kKmerSize = 11;

    rowset_t via_options;
    rowset_t via_guc;
    {
        // Per-call override leaves session GUCs untouched.
        const std::string sql = fmt::format(
            "SELECT pk FROM {} "
            "WHERE mmsearch(seq0, {}, aligner_options(kmer_size => {})) "
            "ORDER BY pk;",
            kHoe, hoe_probe_literal(), kKmerSize);
        via_options = query_rows(ctx, sql);
    }
    {
        guc_scope guc(ctx, "bioseq.aligner.kmer_size",
                      std::to_string(kKmerSize));
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
            kHoe, hoe_probe_literal());
        via_guc = query_rows(ctx, sql);
    }

    REQUIRE(via_options == via_guc);
}

TEST_CASE("aligner_options(window_size => 5) executes without errors",
          "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("window_size => 5");
}

TEST_CASE("aligner_options(max_occ => 69) executes without errors",
          "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("max_occ => 69");
}

TEST_CASE("aligner_options(chaining_algorithm => 0) executes without errors",
          "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("chaining_algorithm => 0");
}

TEST_CASE("DOC0372 PostgreSQL finalizes SIMD chaining policy after the last override",
          "[mmsearch][doc0372][simd-chaining][policy][override][proof]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'two_stack_nthash';");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");

    SECTION("a per-call SIMD override selects bounded after a scalar GUC") {
        exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'scalar_mm2';");
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
            "aligner_options(chaining_algorithm => 1)) ORDER BY pk;",
            kHoe,
            hoe_probe_literal());
        const auto plan = explain_text<true>(ctx, sql);
        REQUIRE(plan_contains(plan, "| ChainStage: Minimap2 SIMD"));
        REQUIRE(plan_contains(
            plan,
            "simd_chaining_policy: bounded_batch_approximate"));
        REQUIRE(plan_contains(plan, "backend_fallbacks=0"));
    }

    SECTION("a per-call scalar override clears bounded after a SIMD GUC") {
        exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'simd_mm2';");
        const std::string sql = fmt::format(
            "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
            "aligner_options(chaining_algorithm => 0)) ORDER BY pk;",
            kHoe,
            hoe_probe_literal());
        const auto plan = explain_text<true>(ctx, sql);
        REQUIRE(plan_contains(plan, "| ChainStage: Minimap2 Scalar"));
        REQUIRE_FALSE(plan_contains(
            plan,
            "simd_chaining_policy: bounded_batch_approximate"));
        REQUIRE(plan_contains(plan, "backend_fallbacks=0"));
    }

    SECTION("RMQ and Vulkan overrides never carry the bounded SIMD policy") {
        exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'simd_mm2';");
        for (const int ordinal : {2, 3}) {
            CAPTURE(ordinal);
            const std::string sql = fmt::format(
                "SELECT pk FROM {} WHERE mmsearch(seq0, {}, "
                "aligner_options(chaining_algorithm => {})) ORDER BY pk;",
                kHoe,
                hoe_probe_literal(),
                ordinal);
            const auto plan = explain_text<false>(ctx, sql);
            REQUIRE_FALSE(plan_contains(
                plan,
                "simd_chaining_policy: bounded_batch_approximate"));
        }
    }
}

TEST_CASE("selected Vulkan minimizer reports a Postgres error when Vulkan is disabled",
          "[mmsearch][vulkan][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = off;");

    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "XX000");
    REQUIRE(err.contains("Vulkan aligner execution failed"));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("selected Vulkan batch rejects CIGAR projection instead of CPU fallback",
          "[mmsearch][vulkan][error]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    exec_sql(ctx, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'vulkan_tiled_dp';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_seed_hits_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_index_cache_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_fused_frontend_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_gpu_index_build_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_extend_enabled = off;");

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.cigar(pk) "
        "FROM {} "
        "WHERE mmsearch(seq0, {}) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal());
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "XX000");
    REQUIRE(err.contains("Vulkan aligner execution failed"));
    REQUIRE((err.contains("GPU CIGAR/extension is not implemented")
             || err.contains("bounded GPU CIGAR extension")
             || err.contains("complete supported all-GPU tuple")
             || err.contains("software/CPU Vulkan device")
             || err.contains("usable Vulkan runtime")));
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("selected Vulkan batch rejects forced chunked routing before generic biological work",
          "[mmsearch][vulkan][chunked][fail-closed]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);
    const bool untrusted_runtime = selected_vulkan_runtime_untrusted(ctx);

    exec_sql(ctx, "SET bioseq.read_sequence_index = off;");
    exec_sql(ctx, "SET bioseq.test_force_chunked_scanner = on;");
    exec_sql(ctx, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'vulkan_tiled_dp';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_seed_hits_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_index_cache_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_fused_frontend_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_gpu_index_build_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_extend_enabled = on;");

    const std::string sql = fmt::format(
        "SELECT pk FROM {} WHERE mmsearch(seq0, {}) ORDER BY pk;",
        kHoe,
        hoe_probe_literal());
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "XX000");
    REQUIRE(err.contains("Vulkan aligner execution failed"));
    if (!untrusted_runtime) {
        REQUIRE(err.contains("forced chunked scanner"));
        REQUIRE(err.contains("CPU/SIMD biological fallback is forbidden"));
    }
    REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
}

TEST_CASE("selected Vulkan batch emits bounded GPU CIGAR when extension is enabled",
          "[mmsearch][vulkan][cigar]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    exec_sql(ctx, "SET bioseq.aligner.base_alignment_algorithm = 'block_aligner';");
    exec_sql(ctx, "SET bioseq.aligner.chaining_algorithm = 'vulkan_tiled_dp';");
    exec_sql(ctx, "SET bioseq.aligner.minimizer_algorithm = 'vulkan_minimap_int';");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_seed_hits_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_index_cache_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_fused_frontend_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_gpu_index_build_enabled = on;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan_extend_enabled = on;");
    // The canonical probe is only 29 bp. Exact minimap2 tiled-DP chaining
    // scores its overlapping k=15 seeds by newly covered distance, so its
    // legitimate maximum is below the production default threshold of 40.
    exec_sql(ctx, "SET bioseq.aligner.min_chain_score = 20;");

    const std::string sql = fmt::format(
        "SELECT pk, bioseq.cigar(pk), bioseq.has_cigar(pk) "
        "FROM {} "
        "WHERE mmsearch(seq0, {}) "
        "ORDER BY pk;",
        kHoe,
        hoe_probe_literal());
    if (selected_vulkan_runtime_untrusted(ctx)) {
        const auto err = sql_error(ctx, sql);
        REQUIRE(err.sqlstate == "XX000");
        REQUIRE(err.contains("Vulkan aligner execution failed"));
        REQUIRE((err.contains("software/CPU Vulkan device")
                 || err.contains("usable Vulkan runtime")));
        REQUIRE(query_scalar_int(ctx, "SELECT 1;") == 1);
        return;
    }

    const auto rows = query_rows(ctx, sql);
    REQUIRE_FALSE(rows.empty());
    for (const auto& row : rows) {
        REQUIRE(row.size() == 3);
        REQUIRE(row[1].has_value());
        REQUIRE_FALSE(row[1].value().empty());
        REQUIRE(is_valid_cigar(row[1].value()));
        REQUIRE(row[2].has_value());
        REQUIRE(row[2].value() == "t");
    }

    const auto plan = explain_text<true>(ctx, sql);
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(
        plan,
        "Execution proof [requested_backend=vulkan_gpu "
        "actual_backend=mixed_invalid"));
    REQUIRE(plan_contains(plan, "fully_attested=0"));
    REQUIRE(plan_contains(plan, "GPU dispatch [seed="));
    REQUIRE_FALSE(plan_contains(plan, "GPU dispatch [seed=0 "));
    REQUIRE_FALSE(plan_contains(plan, " chain=0 chain_backtrack="));
    REQUIRE_FALSE(plan_contains(plan, " chain_backtrack=0 "));
    const auto tiled_chain_dispatches =
        find_explain_property(plan, "GPU Chain Dispatches");
    const auto tiled_backtrack_dispatches =
        find_explain_property(plan, "GPU Chain Backtrack Dispatches");
    const auto coordinate_vote_dispatches =
        find_explain_property(plan, "GPU Coordinate Vote Dispatches");
    REQUIRE(tiled_chain_dispatches.has_value());
    REQUIRE(std::stoull(*tiled_chain_dispatches) > 0u);
    REQUIRE(tiled_backtrack_dispatches.has_value());
    REQUIRE(std::stoull(*tiled_backtrack_dispatches) > 0u);
    REQUIRE(coordinate_vote_dispatches.has_value());
    REQUIRE(std::stoull(*coordinate_vote_dispatches) == 0u);
    REQUIRE(plan_contains(plan, "extend=1"));
    REQUIRE(plan_contains(
        plan, "backend_declines=0 backend_fallbacks=0"));
}

TEST_CASE(
    "aligner_options(base_alignment_algorithm => 0) executes without errors",
    "[mmsearch][aligner_options][partial]") {
    check_aligner_options_partial_override("base_alignment_algorithm => 0");
}

TEST_CASE("mmsearch projection helpers run cleanly for the produced row set",
          "[mmsearch][projections]") {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    const auto rows = query_rows(
        ctx, fmt::format(
                 "SELECT pk, "
                 "       bioseq.query_start(pk)  AS qs, "
                 "       bioseq.query_end(pk)    AS qe, "
                 "       bioseq.target_start(pk) AS ts, "
                 "       bioseq.target_end(pk)   AS te, "
                 "       bioseq.cigar(pk)        AS cigar, "
                 "       bioseq.has_cigar(pk)    AS has_cigar, "
                 "       bioseq.is_reverse(pk)   AS is_reverse, "
                 "       bioseq.is_primary(pk)   AS is_primary "
                 "FROM {} "
                 "WHERE mmsearch(seq0, {}) "
                 "ORDER BY pk;",
                 kHoe, hoe_probe_literal()));

    for (const auto& row : rows) {
        REQUIRE(row.size() == 9);
        if (row[1].has_value() && row[2].has_value()) {
            REQUIRE(std::stoll(row[1].value()) <= std::stoll(row[2].value()));
        }
        if (row[3].has_value() && row[4].has_value()) {
            REQUIRE(std::stoll(row[3].value()) <= std::stoll(row[4].value()));
        }
        if (row[5].has_value() && row[6].has_value()) {
            const bool has_text = !row[5].value().empty();
            REQUIRE(row[6].value() == (has_text ? "t" : "f"));
            if (has_text) {
                REQUIRE(is_valid_cigar(row[5].value()));
            }
        }
        if (row[7].has_value()) {
            REQUIRE((row[7].value() == "t" || row[7].value() == "f"));
        }
        if (row[8].has_value()) {
            REQUIRE((row[8].value() == "t" || row[8].value() == "f"));
        }
    }
}

// All remaining MatchResult helpers exercised in one parameterised pass.
// Each row of `kProjectionHelpers` is a (sql_function, expected_pg_type)
// pair; for any rows the alignment pipeline materialises we assert the
// helper either returns NULL or a value parseable by the named type. Adding
// a new helper takes one line.
namespace {
struct projection_probe {
    std::string_view sql_call;     // e.g. "bioseq.score(pk)"
    std::string_view pg_typename;  // "int4", "bool", "float4", "text"
};

constexpr std::array<projection_probe, 13> kProjectionHelpers = {{
    {"bioseq.score(pk)",                  "int4"},
    {"bioseq.dp_score(pk)",               "int4"},
    {"bioseq.dp_max_score(pk)",           "int4"},
    {"bioseq.chain_score(pk)",            "int4"},
    {"bioseq.secondary_chain_score(pk)",  "int4"},
    {"bioseq.seed_count(pk)",             "int4"},
    // DOC0274 §1.8 — these four are sequence_size_t (uint64) in MatchResult,
    // so the projections RETURN BIGINT (int8), not int4.
    {"bioseq.num_matches(pk)",            "int8"},
    {"bioseq.block_length(pk)",           "int8"},
    {"bioseq.mapq(pk)",                   "int4"},
    {"bioseq.target_len(pk)",             "int8"},
    {"bioseq.query_len(pk)",              "int8"},
    {"bioseq.divergence(pk)",             "float4"},
    {"bioseq.cigar(pk)",                  "text"},
}};

void check_projection_helper(const projection_probe& probe) {
    auto ctx = fresh_context({kHoe});
    seed_hoe_fixture(ctx, kHoe);

    // The cast forces Postgres to type-check the helper's return value at
    // query time — a misdeclared return type lights up here as a server
    // error rather than as a silent text-is-text comparison.
    const std::string sql = fmt::format(
        "SELECT pk, ({})::{} "
        "  FROM {} "
        " WHERE mmsearch(seq0, {}) "
        " ORDER BY pk;",
        probe.sql_call, probe.pg_typename, kHoe,
        hoe_probe_literal());

    const auto rows = query_rows(ctx, sql);
    for (const auto& row : rows) {
        REQUIRE(row.size() == 2);
        // The pk column must come back as an integer.
        REQUIRE(row[0].has_value());
        (void)std::stoll(row[0].value());
        // Cast itself succeeded: any value, even NULL, is acceptable here.
    }
}
}  // namespace

TEMPLATE_TEST_CASE_SIG(
    "Every MatchResult projection helper executes and casts to its declared type",
    "[mmsearch][projections][matrix]",
    ((size_t Idx), Idx),
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12) {
    static_assert(Idx < kProjectionHelpers.size());
    check_projection_helper(kProjectionHelpers[Idx]);
}
