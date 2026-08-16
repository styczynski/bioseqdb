/**
 * @file mmsearch_test_support.cppm
 * @brief Provides shared fixtures for mmsearch integration scenarios.
 */

// Integration scenarios:
//   * `mmsearch(seq, query)` runs cleanly on a `USING bioseq` columnar table.
//     The result remains within the seeded primary-key domain.
//   * The result set is invariant under the GUC matrix:
//        bioseq.vectorization.enabled           on / off
//        bioseq.column_cache.enabled            on / off
//        bioseq.planner.index_scan.enabled       on / off
//        bioseq.aligner.profiling.enabled       on / off
//   * `aligner_options()` with all-NULL fields produces the same rows as the
//     plain two-arg `mmsearch`; partial overrides (e.g. setting only
//     `kmer_size`) execute without erroring out.
//   * Per-row projection helpers (`bioseq.cigar`, `bioseq.target_start`, …)
//     produce well-shaped values: integer ranges in order, valid CIGAR text.
//   * SELECTs without an mmsearch qual on a `USING bioseq` table preserve the
//     ordinary scan rows; mmsearch batching only engages for an owned qual.

module;

#include <tests/support/postgres.hpp>
#include <bioseq/pg_build_profile.h>

#include <array>
#include <bit>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <libpq-fe.h>
#include <set>
#include <span>
#include <string_view>
#include <thread>

export module bioseq.tests.integration.mmsearch_test_support;

import bioseq.tests.integration.support;

using namespace bioseq_int_tests;

export namespace bioseq_int_tests::mmsearch_test_support {

inline constexpr std::string_view k_hoe = "hoe_int";
inline constexpr std::string_view k_hoe_lazy_no_insert = "hoe_lazy_no_insert_int";
inline constexpr std::string_view k_lazy_queries =
    "lazy_queries_int";
inline constexpr std::string_view k_transform_target =
    "transform_target_int";
inline constexpr std::string_view k_oracle_target =
    "oracle_target_int";
inline constexpr std::string_view k_oracle_queries =
    "oracle_queries_int";
inline constexpr std::string_view k_duplicate_targets = "mmsearch_duplicate_targets_int";
inline constexpr std::string_view k_missing_index = "mmsearch_missing_index_int";
inline constexpr std::string_view k_batch_join_target = "mmsearch_batch_join_target_int";
inline constexpr std::string_view k_batch_join_queries = "mmsearch_batch_join_queries_int";
inline constexpr std::string_view k_sneaky_snake_edits = "mmsearch_sneaky_snake_edits_int";
inline constexpr std::string_view k_gatekeeper_edits = "gatekeeper_edits";
inline constexpr std::string_view k_async_minimizer_sequence_worker_leaves =
	"mmsearch_async_minimizer_sequence_worker_leaves_int";
#ifdef BIOSEQ_TEST_INDEXES
inline constexpr std::string_view k_output_restore =
    "mmsearch_output_restore_int";
inline constexpr std::string_view k_batch_recipe_queries_32 =
    "batch_recipe_queries32_int";
inline constexpr std::string_view k_batch_recipe_queries_64 =
    "batch_recipe_queries64_int";
inline constexpr std::string_view k_batch_recipe_oracle_32 =
    "batch_recipe_oracle32_int";
inline constexpr std::string_view k_batch_recipe_oracle_64 =
    "batch_recipe_oracle64_int";

/** Read one source-lifetime counter for an mmsearch scenario. */
[[nodiscard]] std::int64_t mmsearch_source_counter(
        const connection_test_context& ctx,
        const std::string_view name) {
    return query_scalar_int(ctx, fmt::format(
        "SELECT (bioseq_internal.sequence_source_counters()"
        "        ->>'{}')::bigint;", name));
}

/** Reset source-lifetime counters before an isolated mmsearch scenario. */
void reset_mmsearch_source_counters(
        const connection_test_context& ctx) {
    exec_sql(ctx,
        "SELECT bioseq_internal.reset_sequence_source_counters();");
}

/** Require every source-lifetime resource created by mmsearch to be released. */
void require_mmsearch_source_resources_balanced(
        const connection_test_context& ctx) {
    REQUIRE(mmsearch_source_counter(ctx, "registries_created")
            == mmsearch_source_counter(ctx, "registries_destroyed"));
    REQUIRE(mmsearch_source_counter(ctx, "providers_created")
            == mmsearch_source_counter(ctx, "providers_destroyed"));
    REQUIRE(mmsearch_source_counter(ctx, "domains_created")
            == mmsearch_source_counter(ctx, "domains_destroyed"));
    REQUIRE(mmsearch_source_counter(ctx, "cache_sets_created")
            == mmsearch_source_counter(ctx, "cache_sets_destroyed"));
    REQUIRE(mmsearch_source_counter(ctx, "snapshots_registered")
            == mmsearch_source_counter(ctx, "snapshots_unregistered"));
    REQUIRE(mmsearch_source_counter(ctx, "leases_acquired")
            == mmsearch_source_counter(ctx, "leases_released"));
    REQUIRE(mmsearch_source_counter(ctx, "wrong_thread_rejections") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "refcount_faults") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "lease_faults") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "live_registries") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "live_providers") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "live_domains") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "live_leases") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "live_cache_sets") == 0);
    REQUIRE(mmsearch_source_counter(ctx, "live_snapshots") == 0);
}
#endif

/**
 * @brief Derive the production logical-index advisory-lock identity.
 *
 * Integration tests are libpq clients and do not link the PostgreSQL backend
 * module that owns the equivalent lock-key implementation.
 */
[[nodiscard]] std::uint64_t minimizer_index_advisory_lock_key(
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

/** Report the terminal state of one bounded asynchronous libpq query. */
struct async_query_outcome {
	bool consume_ok = true;
	bool ready = false;
	bool got_result = false;
	bool expected_status = true;
};

/**
 * @brief Cancel and drain a lock-blocked libpq command on every exit path.
 *
 * A Catch2 failure or SQL helper may unwind while another backend is blocked
 * at a lock barrier; the guard keeps that failure path bounded.
 */
class scoped_async_query {
private:
	PGconn* connection_ = nullptr;
	bool active_ = false;

	[[nodiscard]] bool cancel_and_drain_noexcept_() noexcept {
		if (!active_ || connection_ == nullptr) return true;
		if (PGcancel* cancel = PQgetCancel(connection_); cancel != nullptr) {
			std::array<char, 256> error{};
			(void)PQcancel(cancel, error.data(), static_cast<int>(error.size()));
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
	explicit scoped_async_query(PGconn* connection) noexcept
		: connection_(connection) {}
	scoped_async_query(const scoped_async_query&) = delete;
	scoped_async_query& operator=(
		const scoped_async_query&) = delete;
	~scoped_async_query() { (void)cancel_and_drain_noexcept_(); }

	[[nodiscard]] bool send(const std::string& sql) noexcept {
		if (connection_ == nullptr || active_) return false;
		active_ = PQsendQuery(connection_, sql.c_str()) == 1;
		return active_;
	}

	[[nodiscard]] bool cancel_and_drain() noexcept {
		return cancel_and_drain_noexcept_();
	}

	[[nodiscard]] async_query_outcome drain(
			const int expected_status_value,
			const unsigned attempts = 1000) noexcept {
		async_query_outcome outcome{};
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

/** Return the rows from the latest alignment-execution statistics snapshot. */
[[nodiscard]] rowset_t last_alignment_stats(
        const connection_test_context& ctx) {
    return query_rows(
        ctx, "SELECT * FROM bioseq.last_alignment_execution_stats();");
}

/** Parse one required signed-integer statistics cell. */
[[nodiscard]] long long stats_i64(
        const row_t& row,
        const std::size_t column) {
    REQUIRE(column < row.size());
    REQUIRE(row[column].has_value());
    return std::stoll(*row[column]);
}

/** Parse one required Boolean statistics cell. */
[[nodiscard]] bool stats_bool(
        const row_t& row,
        const std::size_t column) {
    REQUIRE(column < row.size());
    REQUIRE(row[column].has_value());
    REQUIRE((*row[column] == "t" || *row[column] == "f"));
    return *row[column] == "t";
}

/** Return whether the selected Vulkan runtime cannot execute trusted work. */
[[nodiscard]] bool selected_vulkan_runtime_untrusted(
    const connection_test_context& ctx) {
    // Make the status UDF the first Runtime consumer in this backend.
    // Diagnostics must leave the singleton prepared with the PostgreSQL shader
    // provider and digest-derived MoltenVK archive environment.
    return query_scalar(
        ctx,
        "SELECT (NOT is_available) OR is_software_device "
        "FROM bioseq.vulkan_runtime_status();") == "t";
}

/** Require the exact primary keys, edit counts, coordinates, and CIGAR rows. */
void require_gatekeeper_edit_rows(const rowset_t& rows) {
    constexpr std::array<std::string_view, 2u> expected_pks{"1", "2"};
    constexpr std::array<std::string_view, 2u> expected_edits{"0", "4"};
    REQUIRE(rows.size() == expected_pks.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        REQUIRE(row.size() == 4u);
        REQUIRE(row[0].has_value());
        REQUIRE(*row[0] == expected_pks[index]);
        REQUIRE(row[1].has_value());
        REQUIRE(*row[1] == expected_edits[index]);
        REQUIRE(row[2].has_value());
        const long long query_start = std::stoll(*row[2]);
        REQUIRE(query_start >= 0);
        REQUIRE(query_start < 64);
        REQUIRE(row[3].has_value());
        REQUIRE_FALSE(row[3]->empty());
        REQUIRE(is_valid_cigar(*row[3]));
    }
}

/** Require one textual EXPLAIN property to equal its expected value. */
void require_explain_property_value(
        std::span<const std::string> plan,
        const std::string_view key,
        const std::string_view expected) {
    const auto value = find_explain_property(plan, key);
    REQUIRE(value.has_value());
    REQUIRE(*value == expected);
}

/** Require one textual EXPLAIN property to equal an unsigned value. */
void require_explain_property_uint(
        std::span<const std::string> plan,
        const std::string_view key,
        const std::uint64_t expected) {
    const auto value = find_explain_property(plan, key);
    REQUIRE(value.has_value());
    REQUIRE(std::stoull(*value) == expected);
}

/** Require the complete GateKeeper execution-attestation properties. */
void require_gatekeeper_explain_attestation(
        std::span<const std::string> plan) {
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "prefilter_stage"));
    REQUIRE(plan_contains(plan, "Rank edit-distance prefilter"));
    REQUIRE(plan_contains(plan, "exact_edit_filter_stage"));
    REQUIRE(plan_contains(plan, "exclusive_upper_bound: 5"));
    require_explain_property_value(
        plan, "Prefilter Stats Available", "true");
    require_explain_property_value(
        plan, "Prefilter Resolved Algorithm", "gatekeeper");
    require_explain_property_value(
        plan, "Prefilter Resolved Backend", "vulkan");
    require_explain_property_uint(plan, "Prefilter Dispatches", 3u);
    require_explain_property_uint(
        plan, "Prefilter SneakySnake Dispatches", 0u);
    require_explain_property_uint(
        plan, "Prefilter GateKeeper Dispatches", 3u);
    require_explain_property_uint(
        plan, "Prefilter Scalar Dispatches", 0u);
    require_explain_property_uint(
        plan, "Prefilter SIMD Dispatches", 0u);
    require_explain_property_uint(
        plan, "Prefilter Vulkan Dispatches", 3u);
    require_explain_property_uint(plan, "Prefilter Jobs", 3u);
    require_explain_property_uint(plan, "Prefilter Evaluated Jobs", 3u);
    require_explain_property_uint(plan, "Prefilter Length 64 Jobs", 3u);
    require_explain_property_uint(
        plan, "Prefilter Unequal-Length Jobs", 0u);
    require_explain_property_uint(plan, "Prefilter Unsupported Jobs", 0u);
    require_explain_property_uint(plan, "Prefilter Rejected Jobs", 1u);
    require_explain_property_uint(plan, "Prefilter Rejected Regions", 1u);
    require_explain_property_uint(plan, "Prefilter Surviving Regions", 2u);
    require_explain_property_uint(
        plan, "Prefilter Mask Rejection Candidates", 1u);
    require_explain_property_uint(
        plan, "Prefilter Exact Guard Activations", 1u);
    require_explain_property_uint(
        plan, "Prefilter Guard Confirmed Rejections", 1u);
    require_explain_property_uint(
        plan, "Prefilter Hamming Exact Rejections", 0u);
    require_explain_property_value(
        plan, "Prefilter Stats Saturated", "false");
    require_explain_property_value(
        plan, "Prefilter Stats Consistent", "true");
}

/** Require the planned Vulkan GateKeeper selection properties. */
void require_gatekeeper_explain_selection(
        std::span<const std::string> plan) {
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "prefilter_stage"));
    REQUIRE(plan_contains(plan, "Rank edit-distance prefilter"));
    REQUIRE(plan_contains(plan, "backend: vulkan"));
    REQUIRE(plan_contains(
        plan, "algorithm: batch_resolved_gatekeeper_or_sneaky_snake"));
    REQUIRE(plan_contains(plan, "max_edits: 4"));
    REQUIRE(plan_contains(
        plan, "gatekeeper_auto_domain: length in {32,64}, E in {2,4}"));
    REQUIRE(plan_contains(plan, "exact_edit_filter_stage"));
    REQUIRE(plan_contains(plan, "exclusive_upper_bound: 5"));
}

/** Require the planned CPU SIMD prefilter selection properties. */
void require_cpu_prefilter_explain_selection(
        std::span<const std::string> plan) {
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE(plan_contains(plan, "prefilter_stage"));
    REQUIRE(plan_contains(plan, "Rank edit-distance prefilter"));
    REQUIRE(plan_contains(plan, "backend: highway_simd"));
    REQUIRE(plan_contains(plan, "algorithm: sneaky_snake"));
    REQUIRE_FALSE(plan_contains(plan, "backend: vulkan"));
    REQUIRE(plan_contains(plan, "exact_edit_filter_stage"));
    REQUIRE(plan_contains(plan, "exclusive_upper_bound: 5"));
}

/** Require a plan with no selected or executed prefilter stage. */
void require_no_prefilter_explain_selection(
        std::span<const std::string> plan) {
    REQUIRE(plan_contains(plan, "Custom Scan (BioseqScan)"));
    REQUIRE_FALSE(plan_contains(plan, "prefilter_stage"));
    REQUIRE_FALSE(plan_contains(plan, "Rank edit-distance prefilter"));
    REQUIRE_FALSE(find_explain_property(
        plan, "Prefilter Stats Available").has_value());
    REQUIRE_FALSE(find_explain_property(
        plan, "Prefilter Dispatches").has_value());
}

/** Build the mmsearch statement for one persisted-index requirement. */
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

/** Require the exact missing-compatible-index SQLSTATE and diagnostic. */
void require_no_current_compatible_persisted_index(
        const connection_test_context& ctx,
        std::string_view sql) {
    const auto err = sql_error(ctx, sql);
    REQUIRE(err.sqlstate == "55000");
    REQUIRE(err.contains(
        "required compatible persisted minimizer index unavailable"));
}

/** Return the canonical batch-join target as a typed SQL literal. */
[[nodiscard]] std::string batch_join_target_literal() {
    return format_literal(
        hoe_seed_rows().front().sequences.front(),
        seq_typmod{.alphabet = "dna4", .width = 32});
}

/** Build the transformed-target mmsearch projection statement. */
[[nodiscard]] std::string transformed_mmsearch_sql(
        std::string_view table) {
    return fmt::format(
        "SELECT t.pk, bioseq.query_start(t.pk), bioseq.query_end(t.pk), "
        "bioseq.target_start(t.pk), bioseq.target_end(t.pk), "
        "bioseq.cigar(t.pk) "
        "FROM {} AS t INNER JOIN {} AS q ON mmsearch("
        "bioseq.reverse(bioseq.reverse(t.seq0[0:16] "
        "OPERATOR(bioseq.||) t.seq0[16:])), "
        "q.seq[0:]) ORDER BY t.pk;",
        table, k_lazy_queries);
}

/** Build the eager-target mmsearch coordinate statement. */
[[nodiscard]] std::string eager_mmsearch_coordinate_sql(
        std::string_view table) {
    return fmt::format(
        "SELECT t.pk, bioseq.query_start(t.pk), bioseq.query_end(t.pk), "
        "bioseq.target_start(t.pk), bioseq.target_end(t.pk), "
        "bioseq.cigar(t.pk) "
        "FROM {} AS t INNER JOIN {} AS q "
        "ON mmsearch(t.seq0, q.seq, "
        "aligner_options(index_read_requirement => 1)) ORDER BY t.pk;",
        table, k_lazy_queries);
}

/** Build one base-alignment projection with an optional residual predicate. */
[[nodiscard]] std::string base_alignment_sql(
        const std::string_view table,
        const std::string_view target_expression,
        const std::string_view query_expression,
        const std::string_view residual_expression = {}) {
    const std::string residual = residual_expression.empty()
        ? std::string{}
        : fmt::format("{} AND ", residual_expression);
    return fmt::format(
        "SELECT t.pk, bioseq.query_start({2}), bioseq.query_end({2}), "
        "bioseq.target_start({2}), bioseq.target_end({2}), "
        "bioseq.is_reverse({2}), bioseq.cigar({2}), "
        "bioseq.has_cigar({2}) "
        "FROM {0} AS t WHERE {1}mmsearch({2}, {3}, "
        "aligner_options(index_read_requirement => 1)) "
        "ORDER BY 2, 3, 4, 5, 6, 7;",
        table, residual, target_expression, query_expression);
}

/** Generate stable high-entropy DNA4 input of the requested length. */
[[nodiscard]] std::string high_entropy_sequence(
        const std::size_t length) {
    constexpr std::string_view bases = "ACGT";
    std::string result(length, 'A');
    std::uint64_t state = 0x243f6a8885a308d3ull;
    for (char& base : result) {
        state ^= state << 13u;
        state ^= state >> 7u;
        state ^= state << 17u;
        base = bases[static_cast<std::size_t>(state & 3u)];
    }
    return result;
}

/** Return the supplied sequence in reverse order. */
[[nodiscard]] std::string reverse_sequence(
        const std::string_view value) {
    return std::string{value.rbegin(), value.rend()};
}

/** Return the reverse complement of a valid DNA4 fixture sequence. */
[[nodiscard]] std::string reverse_complement_sequence(
        const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (auto iterator = value.rbegin(); iterator != value.rend(); ++iterator) {
        switch (*iterator) {
            case 'A': result.push_back('T'); break;
            case 'C': result.push_back('G'); break;
            case 'G': result.push_back('C'); break;
            case 'T': result.push_back('A'); break;
            default: FAIL("DNA fixture contains an invalid base");
        }
    }
    return result;
}

/** Describe one transformed-target coordinate scenario. */
struct transform_case {
    std::string_view name;
    std::string target_expression;
    std::string query_expression;
    std::string logical_target;
    std::string stored_query;
    std::string logical_query;
    std::size_t expected_target_start = 0u;
    bool expected_reverse = false;
};

/** Build every transformed-target coordinate scenario for one source. */
[[nodiscard]] std::vector<transform_case>
transform_cases(const std::string& source) {
    std::vector<transform_case> cases;

    std::string sliced_target = source.substr(17u, 200u);
    std::string sliced_query = sliced_target.substr(47u, 80u);
    cases.push_back({
        .name = "nonzero target and bounded query slices",
        .target_expression = "t.seq0[17:217]",
        .query_expression = "q.source_seq[5:85]",
        .logical_target = std::move(sliced_target),
        .stored_query = "GTCAG" + sliced_query + "CTAGGCA",
        .logical_query = std::move(sliced_query),
        .expected_target_start = 47u,
        .expected_reverse = false,
    });

    std::string concat_target = source.substr(10u, 80u)
        + source.substr(150u, 80u);
    std::string concat_query = concat_target.substr(65u, 60u);
    cases.push_back({
        .name = "hit crossing a concat join",
        .target_expression =
            "t.seq0[10:90] OPERATOR(bioseq.||) t.seq0[150:230]",
        .query_expression = "q.source_seq",
        .logical_target = std::move(concat_target),
        .stored_query = concat_query,
        .logical_query = std::move(concat_query),
        .expected_target_start = 65u,
        .expected_reverse = false,
    });

    std::string reversed_target = reverse_sequence(
        std::string_view{source}.substr(25u, 200u));
    std::string reversed_forward_query = reversed_target.substr(55u, 80u);
    std::string reversed_query = reverse_complement_sequence(
        reversed_forward_query);
    cases.push_back({
        .name = "single reverse with a reverse-strand hit",
        .target_expression = "bioseq.reverse(t.seq0[25:225])",
        .query_expression = "q.source_seq",
        .logical_target = std::move(reversed_target),
        .stored_query = reversed_query,
        .logical_query = std::move(reversed_query),
        .expected_target_start = 55u,
        .expected_reverse = true,
    });

    std::string reverse_complement_target = reverse_complement_sequence(
        std::string_view{source}.substr(30u, 200u));
    std::string reverse_complement_query =
        reverse_complement_target.substr(40u, 90u);
    cases.push_back({
        .name = "reverse complement with forward logical coordinates",
        .target_expression =
            "bioseq.reverse_complement(t.seq0[30:230])",
        .query_expression = "q.source_seq",
        .logical_target = std::move(reverse_complement_target),
        .stored_query = reverse_complement_query,
        .logical_query = std::move(reverse_complement_query),
        .expected_target_start = 40u,
        .expected_reverse = false,
    });
    return cases;
}

/** Build the alignment projection for one target alias and key. */
[[nodiscard]] std::string alignment_projection(
        const std::string_view target_alias,
        const std::string_view target_key) {
    return fmt::format(
        "q.id, bioseq.query_start({}.{}), bioseq.query_end({}.{}), "
        "bioseq.target_start({}.{}), bioseq.target_end({}.{}), "
        "bioseq.is_reverse({}.{}), bioseq.cigar({}.{}), "
        "bioseq.has_cigar({}.{})",
        target_alias, target_key, target_alias, target_key,
        target_alias, target_key, target_alias, target_key,
        target_alias, target_key, target_alias, target_key,
        target_alias, target_key);
}

/** Build a deterministic wide target spanning several index partitions. */
[[nodiscard]] std::string batch_join_wide_target_sequence() {
    /* A reproducible high-entropy target large enough to produce several
     * 256-KiB directory partitions. Repeating the small alignment fixture
     * would make the prefix minimizer recur in every partition and therefore
     * exercise the full route instead of selective loading. */
    constexpr std::size_t target_bp = 512u * 1024u;
    constexpr std::string_view bases = "ACGT";
    std::string sequence(target_bp, 'A');
    std::uint64_t state = 0x6a09e667f3bcc909ull;
    for (char& base : sequence) {
        state = state * 6364136223846793005ull
            + 1442695040888963407ull;
        base = bases[static_cast<std::size_t>(state >> 62u)];
    }
    return sequence;
}

/** Seed the single target row and select the deterministic SIMD pipeline. */
void seed_batch_join_target(
        connection_test_context& ctx,
        std::string_view target_sequence = {}) {
    const std::string target = target_sequence.empty()
        ? hoe_seed_rows().front().sequences.front()
        : std::string{target_sequence};
    create_seq_table(
        ctx,
        seq_table_spec{
            .name = std::string{k_batch_join_target},
            .typmod = {.alphabet = "dna4", .width = 32},
            .seq_columns = 1,
            .using_bioseq = true,
        },
        {seq_row{
            .pk = 1,
            .sequences = {target},
        }});
    exec_sql(ctx, "SET max_parallel_workers_per_gather = 0;");
    exec_sql(ctx, "SET bioseq.index.sequence_read.enabled = off;");
    exec_sql(ctx, "SET bioseq.aligner.vulkan.enabled = off;");
	/* BatchJoin uses the requested all-SIMD pipeline rather than the
	 * scalar-sketch/scalar-chain/Block hybrid defaults. */
	exec_sql(ctx,
		"SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash';");
	exec_sql(ctx,
		"SET bioseq.aligner.chaining.algorithm = 'simd_minimap2';");
	exec_sql(ctx,
		"SET bioseq.aligner.base_alignment.algorithm = 'block_aligner';");
}

/** Populate the batch-join query table with deterministic fixture rows. */
void load_batch_join_queries(
        const connection_test_context& ctx,
        const std::size_t count,
        const bool only_last_can_match = false) {
    exec_sqlf(
        ctx,
        "CREATE TEMP TABLE {} "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 32));",
        k_batch_join_queries);
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
        k_batch_join_queries,
        query_expression,
        count - 1);
}

/** Populate the batch-join query table with one explicit sequence. */
void load_batch_join_query(
        const connection_test_context& ctx,
        std::string_view sequence) {
    exec_sqlf(
        ctx,
        "CREATE TEMP TABLE {} "
        "(id int PRIMARY KEY, seq bioseq.seq('dna4', 32));",
        k_batch_join_queries);
    exec_sqlf(
        ctx,
        "INSERT INTO {} VALUES (0, {});",
        k_batch_join_queries,
        format_literal(
            sequence, seq_typmod{.alphabet = "dna4", .width = 32}));
}

/** Build the batch-join statement for one index-read requirement. */
[[nodiscard]] std::string batch_join_sql(
        const int index_requirement,
        std::string_view select_list = "q.id, t.pk") {
    return fmt::format(
        "SELECT {} FROM {} t JOIN {} q "
        "ON mmsearch(t.seq0, q.seq, "
        "aligner_options(index_read_requirement => {}))",
        select_list,
        k_batch_join_target,
        k_batch_join_queries,
        index_requirement);
}

/** Require exactly one aligned result for every expected query identifier. */
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

/** Require the exact stage and backend statistics for one batch join. */
void require_batch_join_stats(
        const connection_test_context& ctx,
        const long long expected_targets,
        const long long expected_jobs) {
    const auto stats = last_alignment_stats(ctx);
    REQUIRE(stats.size() == 20);
	for (const auto& row : stats) {
			REQUIRE(row.size() == 151);
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

	/* Backend identity is derived from completed algorithm-local attestations,
	 * not requested option labels. A fully attested simd_cpu job records
	 * ExactSimd ntHash, SIMD score/backtrack, and accepted Block Aligner work;
	 * that acceptance rejects every KSW2 or scalar-biological fallback. */
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

	const auto require_all_simd_stage = [&expected_jobs, &stats](
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
	 * Aligner execution; a job may legitimately execute two or three passes. */
	REQUIRE(block_simd_executions >= expected_jobs);
}

}  // namespace bioseq_int_tests::mmsearch_test_support
