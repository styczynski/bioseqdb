#include <algorithm>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t total_input_bases = 2'000'000'000ULL;
constexpr std::uint64_t query_bases = 5'000'000ULL;
constexpr std::uint64_t target_bases = total_input_bases - query_bases;
static_assert(query_bases > 0);
static_assert(query_bases + target_bases == total_input_bases);
constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t minimum_data_space =
    32ULL * gibibyte > 12ULL * total_input_bases ? 32ULL * gibibyte : 12ULL * total_input_bases;
constexpr std::uint64_t io_probe_bytes = gibibyte;
constexpr double minimum_io_mib_per_second = 150.0;
constexpr std::int64_t coordinate_tolerance = 50'000;
constexpr std::array<std::string_view, 8> required_resource_stages{
    "cluster-started", "generation", "index", "oracle", "cold", "warm", "explain", "column-cache"};

constexpr std::string_view alignment_query{"SELECT target_sample.id, "
                                           "bioseq.target_start(target_sample.id), "
                                           "bioseq.target_end(target_sample.id), "
                                           "bioseq.target_len(target_sample.id), "
                                           "bioseq.query_start(target_sample.id), "
                                           "bioseq.query_end(target_sample.id), "
                                           "bioseq.query_len(target_sample.id), "
                                           "bioseq.is_reverse(target_sample.id) "
                                           "FROM cache_bioseq_2gbp AS target_sample "
                                           "JOIN cache_bioseq_2gbp AS query_sample "
                                           "ON target_sample.id = query_sample.id "
                                           "AND mmsearch(target_sample.target, query_sample.query, "
                                           "aligner_options(index_read_requirement => 2)) "
                                           "ORDER BY target_sample.id, "
                                           "bioseq.target_start(target_sample.id), "
                                           "bioseq.target_end(target_sample.id), "
                                           "bioseq.target_len(target_sample.id), "
                                           "bioseq.query_start(target_sample.id), "
                                           "bioseq.query_end(target_sample.id), "
                                           "bioseq.query_len(target_sample.id), "
                                           "bioseq.is_reverse(target_sample.id)"};

[[nodiscard]] std::int64_t parse_i64(std::string_view text) {
    std::int64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("alignment coordinate is not int64");
    }
    return value;
}

[[nodiscard]] std::int64_t absolute_difference(std::int64_t lhs, std::int64_t rhs) {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

/// Read the unsigned, number after a required Cache Storage label.
[[nodiscard]] std::uint64_t diagnostic_metric(std::string_view diagnostic, std::string_view label) {
    const std::size_t label_offset = diagnostic.find(label);
    if (label_offset == std::string_view::npos) {
        throw std::runtime_error("Cache Storage diagnostic is missing " + std::string(label));
    }
    const std::size_t begin = label_offset + label.size();
    std::size_t end = begin;
    while (end < diagnostic.size() && diagnostic[end] >= '0' && diagnostic[end] <= '9') {
        ++end;
    }
    std::uint64_t value{};
    const auto [parsed_end, error] =
        std::from_chars(diagnostic.data() + begin, diagnostic.data() + end, value);
    if (error != std::errc{} || parsed_end != diagnostic.data() + end || begin == end) {
        throw std::runtime_error("Cache Storage diagnostic has invalid " + std::string(label));
    }
    return value;
}

/// Compare resunt sizes, NULL flags and text bytes in row order.
[[nodiscard]] bool byte_identical(const bioseq::libpq::command_result& lhs,
                                  const bioseq::libpq::command_result& rhs) {
    if (lhs.row_count() != rhs.row_count() || lhs.column_count() != rhs.column_count()) {
        return false;
    }
    for (int row = 0; row < lhs.row_count(); ++row) {
        for (int column = 0; column < lhs.column_count(); ++column) {
            if (lhs.cell_is_null(row, column) != rhs.cell_is_null(row, column)) {
                return false;
            }
            if (!lhs.cell_is_null(row, column) &&
                lhs.text_value(row, column) != rhs.text_value(row, column)) {
                return false;
            }
        }
    }
    return true;
}

void collect_plan_nodes_with_property(
    const bioseq::postgres_test::explain_json_value& value,
    std::string_view property,
    std::string_view expected,
    std::vector<const bioseq::postgres_test::explain_json_value*>& matches) {
    if (const auto* object = value.as_object()) {
        const auto* candidate = value.find(property);
        if (candidate != nullptr && candidate->as_string() != nullptr &&
            *candidate->as_string() == expected) {
            matches.push_back(&value);
        }
        for (const auto& [name, child] : *object) {
            static_cast<void>(name);
            collect_plan_nodes_with_property(child, property, expected, matches);
        }
        return;
    }
    if (const auto* array = value.as_array()) {
        for (const auto& child : *array) {
            collect_plan_nodes_with_property(child, property, expected, matches);
        }
    }
}

[[nodiscard]] std::vector<const bioseq::postgres_test::explain_json_value*>
find_plan_nodes_with_property(const bioseq::postgres_test::explain_json_value& value,
                              std::string_view property,
                              std::string_view expected) {
    std::vector<const bioseq::postgres_test::explain_json_value*> matches;
    collect_plan_nodes_with_property(value, property, expected, matches);
    return matches;
}

void configure_canonical_cpu_path(bioseq::libpq::client_connection& connection) {
    bioseq::cache_test::execute(connection,
                                "SET bioseq.aligner.vulkan.enabled = off; "
                                "SET bioseq.aligner.vulkan.seed_lookup.enabled = off; "
                                "SET bioseq.aligner.vulkan.index_cache.enabled = off; "
                                "SET bioseq.aligner.vulkan.fused_frontend.enabled = off; "
                                "SET bioseq.aligner.vulkan.index_build.enabled = off; "
                                "SET bioseq.aligner.vulkan.alignment_extension.enabled = off; "
                                "SET bioseq.index.async_persistence.enabled = off; "
                                "SET bioseq.index.recompute_async_persistence.enabled = off; "
                                "SET bioseq.aligner.grouped_soa_generation.enabled = on; "
                                "SET bioseq.aligner.minimizer.algorithm = 'two_stack_nthash'; "
                                "SET bioseq.aligner.chaining.algorithm = 'simd_minimap2'; "
                                "SET bioseq.aligner.base_alignment.algorithm = 'block_aligner'; "
                                "SET bioseq.planner.custom_scan.enabled = on; "
                                "SET bioseq.column_cache.enabled = on; "
                                "SET bioseq.index.sequence_read.enabled = on");
}

/**
 * Check recursively for cache files with the .partial suffix.
 * Let filesystem errors fail the check, since an unreadable directory cannot
 * show that all temporary files have been removed.
 */
[[nodiscard]] bool has_cache_temporary_file(const std::filesystem::path& root) {
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator{root}) {
        static_cast<void>(entry.symlink_status());
        if (entry.path().extension() == ".partial") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool
wait_for_production_cache_readiness(bioseq::libpq::client_connection& admin,
                                    std::chrono::steady_clock::time_point deadline) {
    constexpr std::string_view query{"SELECT 1::bigint AS cache_bootstrap_ready"};
    return bioseq::postgres_test::wait_until(
        [&admin, deadline, query] {
            bioseq::postgres_test::scoped_session_setting cache_mode{
                admin, "bioseq_cache.query_cache.mode", "exact_query", 30s};
            bioseq::cache_test::execute(admin,
                                        "SELECT bioseq_cache.invalidate_all(); "
                                        "SELECT bioseq_cache.reset_stats()",
                                        30s);
            const auto before = bioseq::cache_test::read_cache_stats(admin);
            const auto cold = admin.execute(query, 30s);
            const auto after_cold = bioseq::cache_test::read_cache_stats(admin);
            const auto warm = admin.execute(query, 30s);
            const auto after_warm = bioseq::cache_test::read_cache_stats(admin);
            const auto cold_delta = after_cold - before;
            const auto warm_delta = after_warm - after_cold;
            const bool ready = cold.row_count() == 1 && cold.column_count() == 1 &&
                               !cold.cell_is_null(0, 0) && cold.text_value(0, 0) == "1" &&
                               bioseq::postgres_test::hash_ordered_result_rows(warm) ==
                                   bioseq::postgres_test::hash_ordered_result_rows(cold) &&
                               cold_delta.produced == 1 && cold_delta.completed_hits == 0 &&
                               cold_delta.fallbacks == 0 && warm_delta.produced == 0 &&
                               warm_delta.completed_hits == 1 && warm_delta.fallbacks == 0;
            bioseq::cache_test::execute(admin,
                                        "SELECT bioseq_cache.invalidate_all(); "
                                        "SELECT bioseq_cache.reset_stats()",
                                        30s);
            // Invalidation retires the probe generation. The janitor removes it  later,
            // so entries() may still show a retired row when the cache is ready.
            return ready;
        },
        deadline,
        50ms);
}

} // namespace

TEST_CASE("exactly 2 Gbp combined Bioseq input produces once and replays exactly",
          "[component:cache][layer:e2e]"
          "[feature:result_cache][resource:large_memory]") {
    const auto test_started = std::chrono::steady_clock::now();
    auto options = bioseq::cache_test::cluster_options(
        true,
        bioseq::cache_test::preload_order::cache_then_bioseq,
        {},
        false,
        bioseq::cache_test::bootstrap_readiness::caller_managed);
    REQUIRE_FALSE(options.extension_readiness.has_value());
    options.pre_initdb = [](const bioseq::postgres_test::pre_initdb_paths& context) {
        bioseq::postgres_test::scoped_stage_timing stage{"host-qualification"};
        static_cast<void>(bioseq::postgres_test::qualify_host_before_initdb(
            context.data_directory,
            bioseq::postgres_test::host_qualification_requirements{
                .test_case = bioseq::postgres_test::current_execution_case_context().test_case,
                // Record host RAM without imposing a fixed minimum for this test.
                // Bioseq applies its memory budgets while loading and processing sequences.
                .minimum_total_ram_bytes = 0,
                .minimum_available_ram_bytes = 0,
                .minimum_data_available_bytes = minimum_data_space,
                .minimum_logical_cores = 8,
                .io_probe_bytes = io_probe_bytes,
                .minimum_io_mib_per_second = minimum_io_mib_per_second}));
        stage.mark_completed();
    };
    bioseq::postgres_test::private_cluster_fixture fixture{std::move(options)};
    auto& admin = fixture.admin();
    auto data = fixture.connect(30s);

    REQUIRE(bioseq::cache_test::integer(admin,
                                        "SELECT count(*) FROM pg_extension "
                                        "WHERE extname IN ('bioseq', 'bioseq_cache')") == 2);
    CHECK(bioseq::cache_test::scalar(admin, "SHOW shared_preload_libraries") ==
          "bioseq_cache,bioseq");
    CHECK(
        bioseq::cache_test::boolean(admin, "SELECT to_regnamespace('bioseq_cache_test') IS NULL"));

    REQUIRE(wait_for_production_cache_readiness(admin, std::chrono::steady_clock::now() + 90s));

    configure_canonical_cpu_path(admin);
    configure_canonical_cpu_path(data);
    {
        bioseq::postgres_test::scoped_stage_timing stage{"generation"};
        bioseq::cache_test::execute(admin,
                                    "SET bioseq.index.insert_maintenance.enabled = off; "
                                    "SELECT bioseq.create_query_target_sample_table("
                                    "'cache_bioseq_2gbp', 1, 5000000, 0.0, 1995000000, "
                                    "0.001, 0.0, 0.0, 1.0, 1.0, 0.5, 3219876)",
                                    20min);
        stage.mark_completed();
        fixture.record_resource_stage("generation");
    }
    const bioseq::libpq::command_result measured_input =
        admin.execute("SELECT count(*)::bigint, "
                      "sum(query_locator.total_bp_size)::bigint, "
                      "sum(target_locator.total_bp_size)::bigint, "
                      "sum(query_locator.total_bp_size "
                      "    + target_locator.total_bp_size)::bigint "
                      "FROM cache_bioseq_2gbp AS sample "
                      "CROSS JOIN LATERAL "
                      "bioseq.inspect_locator(sample.query) AS query_locator "
                      "CROSS JOIN LATERAL "
                      "bioseq.inspect_locator(sample.target) AS target_locator",
                      5min);
    REQUIRE(measured_input.row_count() == 1);
    REQUIRE(measured_input.column_count() == 4);
    for (int column = 0; column < measured_input.column_count(); ++column) {
        REQUIRE_FALSE(measured_input.cell_is_null(0, column));
    }
    CHECK(parse_i64(measured_input.text_value(0, 0)) == 1);
    const std::int64_t measured_query_bases = parse_i64(measured_input.text_value(0, 1));
    const std::int64_t measured_target_bases = parse_i64(measured_input.text_value(0, 2));
    const std::int64_t measured_total_input_bases = parse_i64(measured_input.text_value(0, 3));
    REQUIRE(measured_query_bases == static_cast<std::int64_t>(query_bases));
    REQUIRE(measured_query_bases > 0);
    REQUIRE(measured_target_bases == static_cast<std::int64_t>(target_bases));
    REQUIRE(measured_total_input_bases == static_cast<std::int64_t>(total_input_bases));
    REQUIRE(measured_query_bases + measured_target_bases == measured_total_input_bases);
    {
        bioseq::postgres_test::scoped_stage_timing stage{"index"};
        bioseq::cache_test::execute(admin,
                                    "SELECT bioseq.update_index("
                                    "'cache_bioseq_2gbp'::regclass, "
                                    "'minimizers_per_sequence'); "
                                    "SELECT bioseq.set_active_read_index("
                                    "'cache_bioseq_2gbp'::regclass, "
                                    "'minimizers_per_sequence')",
                                    20min);
        stage.mark_completed();
        fixture.record_resource_stage("index");
    }

    bioseq::cache_test::execute(admin, "SET bioseq_cache.query_cache.mode = 'disabled'");
    bioseq::cache_test::execute(data, "SET bioseq_cache.query_cache.mode = 'disabled'");
    const bioseq::libpq::command_result oracle = [&data, &fixture] {
        bioseq::postgres_test::scoped_stage_timing stage{"oracle"};
        bioseq::libpq::command_result result = data.execute(alignment_query, 20min);
        stage.mark_completed();
        fixture.record_resource_stage("oracle");
        return result;
    }();
    REQUIRE(oracle.row_count() > 0);
    const auto oracle_digest = bioseq::postgres_test::hash_ordered_result_rows(oracle);

    bioseq::cache_test::execute(admin,
                                "SELECT bioseq_cache.invalidate_all(); "
                                "SELECT bioseq_cache.reset_stats()");
    bioseq::cache_test::execute(data, "SET bioseq_cache.query_cache.mode = 'exact_query'");
    const auto planned_explain =
        bioseq::postgres_test::execute_explain(data, alignment_query, false, 5min);
    const auto planned_cache_nodes =
        planned_explain.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(planned_cache_nodes.size() == 1);
    const bioseq::postgres_test::explain_json_value* planned_children =
        planned_cache_nodes.front()->find("Plans");
    REQUIRE(planned_children != nullptr);
    const auto planned_bioseq_nodes =
        find_plan_nodes_with_property(*planned_children, "Custom Plan Provider", "BioseqBatchJoin");
    REQUIRE(planned_bioseq_nodes.size() == 1);
    const auto index_requirement = bioseq::postgres_test::read_plan_string_property(
        *planned_bioseq_nodes.front(), "Bioseq Index Read Requirement");
    REQUIRE(index_requirement == "require_compatible_persisted");
    REQUIRE(find_plan_nodes_with_property(*planned_children, "Node Type", "Seq Scan").empty());
    const auto before_cold = bioseq::cache_test::read_cache_stats(admin);
    const bioseq::libpq::command_result cold = [&data, &fixture] {
        bioseq::postgres_test::scoped_stage_timing stage{"cold"};
        bioseq::libpq::command_result result = data.execute(alignment_query, 20min);
        stage.mark_completed();
        fixture.record_resource_stage("cold");
        return result;
    }();
    const auto after_cold = bioseq::cache_test::read_cache_stats(admin);
    REQUIRE(cold.row_count() > 0);
    REQUIRE(byte_identical(cold, oracle));
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(cold) == oracle_digest);
    const auto cold_delta = after_cold - before_cold;
    CHECK(cold_delta.produced == 1);
    CHECK(cold_delta.completed_hits == 0);
    CHECK(cold_delta.fallbacks == 0);
    {
        bioseq::postgres_test::scoped_session_setting cache_disabled{
            data, "bioseq_cache.query_cache.mode", "disabled", 30s};
        REQUIRE(bioseq::cache_test::boolean(data,
                                            "SELECT count(*) > 0 "
                                            "AND bool_and(completed AND fully_drained "
                                            "AND requested_target_index_source = 'persisted' "
                                            "AND actual_target_index_source = 'persisted' "
                                            "AND target_rows = 1 "
                                            "AND persisted_index_rows = 1 "
                                            "AND missing_persisted_index_rows = 0 "
                                            "AND recomputed_target_index_rows = 0) "
                                            "FROM bioseq.last_alignment_execution_stats()"));
    }

    const bioseq::libpq::command_result truth =
        admin.execute("SELECT id, truth_target_start, truth_target_end, "
                      "is_reverse, original_query_length "
                      "FROM cache_bioseq_2gbp ORDER BY id",
                      5min);
    REQUIRE(truth.row_count() > 0);
    for (int row = 0; row < cold.row_count(); ++row) {
        const std::int64_t id = parse_i64(cold.text_value(row, 0));
        bool found = false;
        for (int truth_row = 0; truth_row < truth.row_count(); ++truth_row) {
            if (parse_i64(truth.text_value(truth_row, 0)) != id) {
                continue;
            }
            const std::int64_t target_start = parse_i64(cold.text_value(row, 1));
            const std::int64_t target_end = parse_i64(cold.text_value(row, 2));
            const std::int64_t target_len = parse_i64(cold.text_value(row, 3));
            const std::int64_t query_start = parse_i64(cold.text_value(row, 4));
            const std::int64_t query_end = parse_i64(cold.text_value(row, 5));
            const std::int64_t query_len = parse_i64(cold.text_value(row, 6));
            const std::string_view is_reverse = cold.text_value(row, 7);
            const std::int64_t truth_start = parse_i64(truth.text_value(truth_row, 1));
            const std::int64_t truth_end = parse_i64(truth.text_value(truth_row, 2));
            const std::string_view truth_is_reverse = truth.text_value(truth_row, 3);
            const std::int64_t original_query_len = parse_i64(truth.text_value(truth_row, 4));
            CHECK(target_start >= 0);
            CHECK(target_start < target_end);
            CHECK(target_end <= static_cast<std::int64_t>(target_bases));
            CHECK(target_len == static_cast<std::int64_t>(target_bases));
            CHECK(query_start >= 0);
            CHECK(query_start < query_end);
            CHECK(query_end <= query_len);
            CHECK(query_len == static_cast<std::int64_t>(query_bases));
            CHECK(original_query_len == static_cast<std::int64_t>(query_bases));
            CHECK(absolute_difference(query_len, original_query_len) <= coordinate_tolerance);
            CHECK((is_reverse == "t" || is_reverse == "f"));
            CHECK(is_reverse == truth_is_reverse);
            CHECK(absolute_difference(target_start, truth_start) <= coordinate_tolerance);
            CHECK(absolute_difference(target_end, truth_end) <= coordinate_tolerance);
            found = true;
            break;
        }
        CHECK(found);
    }

    const bioseq::libpq::command_result warm = [&data, &fixture] {
        bioseq::postgres_test::scoped_stage_timing stage{"warm"};
        bioseq::libpq::command_result result = data.execute(alignment_query, 5min);
        stage.mark_completed();
        fixture.record_resource_stage("warm");
        return result;
    }();
    const auto after_warm = bioseq::cache_test::read_cache_stats(admin);
    REQUIRE(warm.row_count() > 0);
    REQUIRE(byte_identical(warm, cold));
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(warm) == oracle_digest);
    const auto warm_delta = after_warm - after_cold;
    CHECK(warm_delta.produced == 0);
    CHECK(warm_delta.completed_hits == 1);
    CHECK(warm_delta.fallbacks == 0);

    CHECK(bioseq::cache_test::integer(admin,
                                      "SELECT count(*) FROM bioseq_cache.entries() "
                                      "WHERE kind = 'ExactFinalResult' "
                                      "AND state = 'COMPLETE' "
                                      "AND rows > 0 "
                                      "AND logical_bytes > 0 "
                                      "AND stored_bytes > 0") == 1);

    const auto explain = [&data, &fixture] {
        bioseq::postgres_test::scoped_stage_timing stage{"explain"};
        auto result = bioseq::postgres_test::execute_explain(data, alignment_query, true, 5min);
        stage.mark_completed();
        fixture.record_resource_stage("explain");
        return result;
    }();
    const auto cache_nodes = explain.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::read_plan_string_property(
              *cache_nodes.front(), "Actual Cache Action") == "[Hit] exact final result");
    const bioseq::postgres_test::explain_json_value* children = cache_nodes.front()->find("Plans");
    if (children != nullptr) {
        REQUIRE(children->as_array() != nullptr);
        REQUIRE_FALSE(children->as_array()->empty());
        CHECK(bioseq::postgres_test::read_plan_number_property(children->as_array()->front(),
                                                               "Actual Loops") == 0.0);
        const auto bioseq_nodes =
            find_plan_nodes_with_property(*children, "Custom Plan Provider", "BioseqBatchJoin");
        REQUIRE(bioseq_nodes.size() == 1);
        REQUIRE(bioseq::postgres_test::read_plan_string_property(*bioseq_nodes.front(),
                                                                 "Node Type") == "Custom Scan");
    }
    const auto after_explain = bioseq::cache_test::read_cache_stats(admin);
    CHECK((after_explain - after_warm).completed_hits == 1);

    bioseq::cache_test::execute(data, "SET bioseq.column_cache.enabled = off");
    const auto column_cache_off = [&data, &fixture] {
        bioseq::postgres_test::scoped_stage_timing stage{"column-cache"};
        auto result = data.execute(alignment_query, 5min);
        stage.mark_completed();
        fixture.record_resource_stage("column-cache");
        return result;
    }();
    const auto after_column_cache_off = bioseq::cache_test::read_cache_stats(admin);
    REQUIRE(column_cache_off.row_count() > 0);
    REQUIRE(byte_identical(column_cache_off, cold));
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(column_cache_off) == oracle_digest);
    const auto column_cache_off_delta = after_column_cache_off - after_explain;
    CHECK(column_cache_off_delta.produced == 0);
    CHECK(column_cache_off_delta.completed_hits == 1);
    CHECK(column_cache_off_delta.fallbacks == 0);

    const auto terminal_explain =
        bioseq::postgres_test::execute_explain(data, alignment_query, true, 5min);
    const auto terminal_cache_nodes =
        terminal_explain.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(terminal_cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::read_plan_string_property(
              *terminal_cache_nodes.front(), "Actual Cache Action") == "[Hit] exact final result");
    const bioseq::postgres_test::explain_json_value* terminal_children =
        terminal_cache_nodes.front()->find("Plans");
    if (terminal_children != nullptr) {
        REQUIRE(terminal_children->as_array() != nullptr);
        REQUIRE_FALSE(terminal_children->as_array()->empty());
        CHECK(bioseq::postgres_test::read_plan_number_property(
                  terminal_children->as_array()->front(), "Actual Loops") == 0.0);
        const auto terminal_bioseq_nodes = find_plan_nodes_with_property(
            *terminal_children, "Custom Plan Provider", "BioseqBatchJoin");
        REQUIRE(terminal_bioseq_nodes.size() == 1);
    }
    const auto after_terminal_explain = bioseq::cache_test::read_cache_stats(admin);
    const auto terminal_explain_delta = after_terminal_explain - after_column_cache_off;
    CHECK(terminal_explain_delta.produced == 0);
    CHECK(terminal_explain_delta.completed_hits == 1);
    CHECK(terminal_explain_delta.fallbacks == 0);
    CHECK(bioseq::cache_test::integer(admin,
                                      "SELECT count(*) FROM bioseq_cache.entries() "
                                      "WHERE state = 'BUILDING'") == 0);
    CHECK(bioseq::cache_test::integer(admin,
                                      "SELECT coalesce(sum(waiters), 0) "
                                      "FROM bioseq_cache.entries()") == 0);
    CHECK(bioseq::cache_test::integer(admin,
                                      "SELECT coalesce(sum(readers), 0) "
                                      "FROM bioseq_cache.entries()") == 0);
    const auto storage_diagnostic = bioseq::postgres_test::read_plan_string_property(
        *terminal_cache_nodes.front(), "Cache Storage");
    REQUIRE(storage_diagnostic.has_value());
    CHECK(diagnostic_metric(*storage_diagnostic, "waiters=") == 0);
    const auto ownership_diagnostic = bioseq::postgres_test::read_plan_string_property(
        *terminal_cache_nodes.front(), "Cache Storage Ownership");
    REQUIRE(ownership_diagnostic.has_value());
    CHECK(diagnostic_metric(*ownership_diagnostic, "readers=") == 0);
    CHECK(diagnostic_metric(*ownership_diagnostic, "pending_hot=") == 0);
    CHECK(diagnostic_metric(*ownership_diagnostic, "pending_file=") == 0);
    CHECK(diagnostic_metric(*ownership_diagnostic, "registry_waiters=") == 0);
    CHECK(diagnostic_metric(*ownership_diagnostic, "registry_readers=") == 0);
    CHECK(diagnostic_metric(*ownership_diagnostic, "planner_pins=") == 0);
    CHECK_FALSE(has_cache_temporary_file(fixture.cluster().data_directory()));
    const auto sampling = fixture.finish_resource_sampling();
    const auto repeated_sampling = fixture.finish_resource_sampling();
    REQUIRE(repeated_sampling.rss_samples == sampling.rss_samples);
    REQUIRE(repeated_sampling.owned_root_samples == sampling.owned_root_samples);
    REQUIRE(repeated_sampling.peak_postmaster_process_tree_rss_bytes ==
            sampling.peak_postmaster_process_tree_rss_bytes);
    REQUIRE(repeated_sampling.peak_owned_root_bytes == sampling.peak_owned_root_bytes);
    REQUIRE(repeated_sampling.rss_sampling_failures == sampling.rss_sampling_failures);
    REQUIRE(repeated_sampling.owned_root_sampling_failures ==
            sampling.owned_root_sampling_failures);
    REQUIRE(repeated_sampling.stage_boundaries.size() == sampling.stage_boundaries.size());
    REQUIRE(sampling.rss_samples >= 2);
    REQUIRE(sampling.owned_root_samples >= 2);
    REQUIRE(sampling.peak_postmaster_process_tree_rss_bytes > 0);
    REQUIRE(sampling.peak_owned_root_bytes > 0);
    REQUIRE(sampling.rss_sampling_failures == 0);
    REQUIRE(sampling.owned_root_sampling_failures == 0);
    for (const std::string_view stage : required_resource_stages) {
        const auto boundary = std::ranges::find_if(
            sampling.stage_boundaries,
            [stage](const bioseq::postgres_test::resource_sample_boundary& candidate) {
                return candidate.stage == stage;
            });
        REQUIRE(boundary != sampling.stage_boundaries.end());
        REQUIRE(boundary->postmaster_process_tree_rss_bytes.has_value());
        REQUIRE(boundary->owned_root_bytes.has_value());
    }
    CHECK(std::chrono::steady_clock::now() - test_started < 105min);
}
