#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
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
constexpr std::uint64_t target_bases =
    total_input_bases - query_bases;
static_assert(query_bases > 0);
static_assert(query_bases + target_bases == total_input_bases);
constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t minimum_total_ram = 16ULL * gibibyte;
constexpr std::uint64_t minimum_available_ram = 8ULL * gibibyte;
constexpr std::uint64_t minimum_data_space =
    32ULL * gibibyte > 12ULL * total_input_bases
    ? 32ULL * gibibyte
    : 12ULL * total_input_bases;
constexpr std::uint64_t io_probe_bytes = gibibyte;
constexpr double minimum_io_mib_per_second = 150.0;
constexpr std::int64_t coordinate_tolerance = 50'000;
constexpr std::array<std::string_view, 8> required_resource_phases{
    "cluster-started", "generation", "index", "oracle",
    "cold", "warm", "explain", "column-cache"};

constexpr std::string_view alignment_query{
    "SELECT id, "
    "bioseq.target_start(id), "
    "bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), "
    "bioseq.query_end(id), "
    "bioseq.query_len(id), "
    "bioseq.is_reverse(id) "
    "FROM cache_bioseq_2gbp "
    "WHERE mmsearch(target, query) "
    "ORDER BY id, "
    "bioseq.target_start(id), "
    "bioseq.target_end(id), "
    "bioseq.target_len(id), "
    "bioseq.query_start(id), "
    "bioseq.query_end(id), "
    "bioseq.query_len(id), "
    "bioseq.is_reverse(id)"};

[[nodiscard]] std::int64_t parse_i64(std::string_view text) {
    std::int64_t value{};
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        throw std::runtime_error("alignment coordinate is not int64");
    }
    return value;
}

[[nodiscard]] std::int64_t absolute_difference(std::int64_t lhs,
                                               std::int64_t rhs) {
    return lhs >= rhs ? lhs - rhs : rhs - lhs;
}

/// @brief Reads one required unsigned metric from Cache Storage diagnostics.
[[nodiscard]] std::uint64_t diagnostic_metric(
    std::string_view diagnostic, std::string_view label) {
    const std::size_t label_offset = diagnostic.find(label);
    if (label_offset == std::string_view::npos) {
        throw std::runtime_error(
            "Cache Storage diagnostic is missing "
            + std::string(label));
    }
    const std::size_t begin = label_offset + label.size();
    std::size_t end = begin;
    while (end < diagnostic.size()
           && diagnostic[end] >= '0' && diagnostic[end] <= '9') {
        ++end;
    }
    std::uint64_t value{};
    const auto [parsed_end, error] = std::from_chars(
        diagnostic.data() + begin, diagnostic.data() + end, value);
    if (error != std::errc{}
        || parsed_end != diagnostic.data() + end || begin == end) {
        throw std::runtime_error(
            "Cache Storage diagnostic has invalid "
            + std::string(label));
    }
    return value;
}

/// @brief Compares ordered libpq text-row streams byte for byte.
[[nodiscard]] bool byte_identical(const bioseq::libpq::result& lhs,
                                  const bioseq::libpq::result& rhs) {
    if (lhs.rows() != rhs.rows()
        || lhs.columns() != rhs.columns()) {
        return false;
    }
    for (int row = 0; row < lhs.rows(); ++row) {
        for (int column = 0; column < lhs.columns(); ++column) {
            if (lhs.is_null(row, column)
                != rhs.is_null(row, column)) {
                return false;
            }
            if (!lhs.is_null(row, column)
                && lhs.value(row, column)
                    != rhs.value(row, column)) {
                return false;
            }
        }
    }
    return true;
}

void collect_plan_nodes_with_property(
    const bioseq::postgres_test::json_value& value,
    std::string_view property,
    std::string_view expected,
    std::vector<const bioseq::postgres_test::json_value*>& matches) {
    if (const auto* object = value.as_object()) {
        const auto* candidate = value.find(property);
        if (candidate != nullptr
            && candidate->as_string() != nullptr
            && *candidate->as_string() == expected) {
            matches.push_back(&value);
        }
        for (const auto& [name, child] : *object) {
            static_cast<void>(name);
            collect_plan_nodes_with_property(
                child, property, expected, matches);
        }
        return;
    }
    if (const auto* array = value.as_array()) {
        for (const auto& child : *array) {
            collect_plan_nodes_with_property(
                child, property, expected, matches);
        }
    }
}

[[nodiscard]] std::vector<
    const bioseq::postgres_test::json_value*>
find_plan_nodes_with_property(
    const bioseq::postgres_test::json_value& value,
    std::string_view property,
    std::string_view expected) {
    std::vector<const bioseq::postgres_test::json_value*> matches;
    collect_plan_nodes_with_property(
        value, property, expected, matches);
    return matches;
}

void configure_canonical_cpu_path(
    bioseq::libpq::connection& connection) {
    bioseq::cache_test::exec(
        connection,
        "SET bioseq.aligner.vulkan_enabled = off; "
        "SET bioseq.aligner.vulkan_seed_hits_enabled = off; "
        "SET bioseq.aligner.vulkan_index_cache_enabled = off; "
        "SET bioseq.aligner.vulkan_fused_frontend_enabled = off; "
        "SET bioseq.aligner.vulkan_gpu_index_build_enabled = off; "
        "SET bioseq.aligner.vulkan_extend_enabled = off; "
        "SET bioseq.enable_custom_scan = on; "
        "SET bioseq.enable_column_cache = on; "
        "SET bioseq.read_sequence_index = on");
}

/// @brief Detects cache staging files and propagates every filesystem error.
///
/// Production cache writers use the `.partial` suffix for both block files and
/// manifest staging files.  The throwing filesystem overloads are intentional:
/// an unreadable or disappearing subtree cannot be accepted as proof that no
/// temporary cache artifact remains.
[[nodiscard]] bool has_cache_temporary_file(
    const std::filesystem::path& root) {
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator{root}) {
        static_cast<void>(entry.symlink_status());
        if (entry.path().extension() == ".partial") {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool wait_for_production_cache_readiness(
    bioseq::libpq::connection& admin,
    std::chrono::steady_clock::time_point deadline) {
    constexpr std::string_view query{
        "SELECT 1::bigint AS cache_bootstrap_ready"};
    return bioseq::postgres_test::wait_until(
        [&admin, deadline, query] {
            bioseq::postgres_test::scoped_guc cache_mode{
                admin,
                "bioseq.query_cache_optimisation_mode",
                "same",
                30s};
            bioseq::cache_test::exec(
                admin,
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache.reset_stats()",
                30s);
            const auto before =
                bioseq::cache_test::read_cache_stats(admin);
            const auto cold = admin.exec(query, 30s);
            const auto after_cold =
                bioseq::cache_test::read_cache_stats(admin);
            const auto warm = admin.exec(query, 30s);
            const auto after_warm =
                bioseq::cache_test::read_cache_stats(admin);
            const auto cold_delta = after_cold - before;
            const auto warm_delta = after_warm - after_cold;
            const bool ready =
                cold.rows() == 1
                && cold.columns() == 1
                && !cold.is_null(0, 0)
                && cold.value(0, 0) == "1"
                && bioseq::postgres_test::exact_row_hash(warm)
                    == bioseq::postgres_test::exact_row_hash(cold)
                && cold_delta.produced == 1
                && cold_delta.completed_hits == 0
                && cold_delta.fallbacks == 0
                && warm_delta.produced == 0
                && warm_delta.completed_hits == 1
                && warm_delta.fallbacks == 0;
            bioseq::cache_test::exec(
                admin,
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache.reset_stats()",
                30s);
            if (!ready
                || std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            const bool drained =
                bioseq::postgres_test::wait_until(
                    [&admin] {
                        return bioseq::cache_test::integer(
                            admin,
                            "SELECT count(*) "
                            "FROM bioseq_cache.entries()")
                            == 0;
                    },
                    deadline, 50ms);
            bioseq::cache_test::exec(
                admin,
                "SELECT bioseq_cache.reset_stats()",
                30s);
            return drained;
        },
        deadline, 50ms);
}

}  // namespace

TEST_CASE("exactly 2 Gbp combined Bioseq input produces once and replays exactly",
          "[cache][integration][bioseq][large][2gbp]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "exactly 2 Gbp combined Bioseq input produces once and replays exactly"};
    const auto test_started = std::chrono::steady_clock::now();
    auto options = bioseq::cache_test::cluster_options(
        true, bioseq::cache_test::preload_order::cache_then_bioseq, {},
        false,
        bioseq::cache_test::bootstrap_readiness::caller_managed);
    REQUIRE_FALSE(options.post_extension_readiness.has_value());
    options.pre_initdb =
        [](const bioseq::postgres_test::pre_initdb_context& context) {
            bioseq::postgres_test::scoped_report_phase phase{
                "host-qualification"};
            static_cast<void>(
                bioseq::postgres_test::qualify_host_before_initdb(
                    context.data_directory,
                    bioseq::postgres_test::
                        host_qualification_requirements{
                            .test_case =
                                bioseq::postgres_test::
                                    current_report_case_context()
                                        .test_case,
                            .minimum_total_ram_bytes =
                                minimum_total_ram,
                            .minimum_available_ram_bytes =
                                minimum_available_ram,
                            .minimum_data_available_bytes =
                                minimum_data_space,
                            .minimum_logical_cores = 8,
                            .io_probe_bytes = io_probe_bytes,
                            .minimum_io_mib_per_second =
                                minimum_io_mib_per_second}));
            phase.complete();
        };
    bioseq::postgres_test::postgres_fixture fixture{std::move(options)};
    auto& admin = fixture.admin();
    auto data = fixture.connect(30s);

    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.host_qualified");

    REQUIRE(bioseq::cache_test::integer(
                admin,
                "SELECT count(*) FROM pg_extension "
                "WHERE extname IN ('bioseq', 'bioseq_cache')")
            == 2);
    CHECK(bioseq::cache_test::scalar(
              admin, "SHOW shared_preload_libraries")
          == "bioseq_cache,bioseq");
    CHECK(bioseq::cache_test::boolean(
        admin,
        "SELECT to_regnamespace('bioseq_cache_test') IS NULL"));
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.production_profile");

    REQUIRE(wait_for_production_cache_readiness(
        admin, std::chrono::steady_clock::now() + 90s));

    configure_canonical_cpu_path(admin);
    configure_canonical_cpu_path(data);
    {
        bioseq::postgres_test::scoped_report_phase phase{"generation"};
        bioseq::cache_test::exec(
            admin,
            "SET bioseq.update_index_on_insert = off; "
            "SELECT bioseq.create_query_target_sample_table("
            "'cache_bioseq_2gbp', 1, 5000000, 0.0, 1995000000, "
            "0.001, 0.0, 0.0, 1.0, 1.0, 0.5, 3219876)",
            20min);
        phase.complete();
        fixture.observe_resource_phase("generation");
    }
    const bioseq::libpq::result measured_input = admin.exec(
        "SELECT count(*)::bigint, "
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
    REQUIRE(measured_input.rows() == 1);
    REQUIRE(measured_input.columns() == 4);
    for (int column = 0; column < measured_input.columns(); ++column) {
        REQUIRE_FALSE(measured_input.is_null(0, column));
    }
    CHECK(parse_i64(measured_input.value(0, 0)) == 1);
    const std::int64_t measured_query_bases =
        parse_i64(measured_input.value(0, 1));
    const std::int64_t measured_target_bases =
        parse_i64(measured_input.value(0, 2));
    const std::int64_t measured_total_input_bases =
        parse_i64(measured_input.value(0, 3));
    REQUIRE(measured_query_bases
            == static_cast<std::int64_t>(query_bases));
    REQUIRE(measured_query_bases > 0);
    REQUIRE(measured_target_bases
            == static_cast<std::int64_t>(target_bases));
    REQUIRE(measured_total_input_bases
            == static_cast<std::int64_t>(total_input_bases));
    REQUIRE(measured_query_bases + measured_target_bases
            == measured_total_input_bases);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.aggregate_input_bases_exact");
    {
        bioseq::postgres_test::scoped_report_phase phase{"index"};
        bioseq::cache_test::exec(
            admin,
            "SELECT bioseq.update_index("
            "'cache_bioseq_2gbp'::regclass, "
            "'minimizers_per_fragment'); "
            "SELECT bioseq.set_active_read_index("
            "'cache_bioseq_2gbp'::regclass, "
            "'minimizers_per_fragment')",
            20min);
        phase.complete();
        fixture.observe_resource_phase("index");
    }
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.exact_fixture_created");

    bioseq::cache_test::exec(
        admin,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    const bioseq::libpq::result oracle = [&data, &fixture] {
        bioseq::postgres_test::scoped_report_phase phase{"oracle"};
        bioseq::libpq::result result =
            data.exec(alignment_query, 20min);
        phase.complete();
        fixture.observe_resource_phase("oracle");
        return result;
    }();
    REQUIRE(oracle.rows() > 0);
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.disabled_oracle_nonempty");

    bioseq::cache_test::exec(
        admin,
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'same'");
    const auto planned_explain =
        bioseq::postgres_test::run_explain(
            data, alignment_query, false, 5min);
    const auto planned_cache_nodes =
        planned_explain.find_plan_nodes(
            "Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(planned_cache_nodes.size() == 1);
    const bioseq::postgres_test::json_value* planned_children =
        planned_cache_nodes.front()->find("Plans");
    REQUIRE(planned_children != nullptr);
    const auto planned_bioseq_nodes = find_plan_nodes_with_property(
        *planned_children, "Custom Plan Provider", "BioseqScan");
    REQUIRE(planned_bioseq_nodes.size() == 1);
    const auto persisted_index =
        bioseq::postgres_test::explain_property_string(
            *planned_bioseq_nodes.front(),
            "Bioseq Fetched From Sequence Index");
    REQUIRE(persisted_index.has_value());
    REQUIRE(persisted_index->contains("index presence: 100%"));
    REQUIRE(
        planned_bioseq_nodes.front()->find("Bioseq Recomputed")
        == nullptr);
    REQUIRE(find_plan_nodes_with_property(
                *planned_children, "Node Type", "Seq Scan")
                .empty());
    const auto before_cold =
        bioseq::cache_test::read_cache_stats(admin);
    const bioseq::libpq::result cold = [&data, &fixture] {
        bioseq::postgres_test::scoped_report_phase phase{"cold"};
        bioseq::libpq::result result =
            data.exec(alignment_query, 20min);
        phase.complete();
        fixture.observe_resource_phase("cold");
        return result;
    }();
    const auto after_cold =
        bioseq::cache_test::read_cache_stats(admin);
    REQUIRE(cold.rows() > 0);
    REQUIRE(byte_identical(cold, oracle));
    CHECK(bioseq::postgres_test::exact_row_hash(cold)
          == oracle_digest);
    const auto cold_delta = after_cold - before_cold;
    CHECK(cold_delta.produced == 1);
    CHECK(cold_delta.completed_hits == 0);
    CHECK(cold_delta.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.cold_identity");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.cold_counter_contract");

    const bioseq::libpq::result truth = admin.exec(
        "SELECT id, truth_target_start, truth_target_end, "
        "is_reverse, original_query_length "
        "FROM cache_bioseq_2gbp ORDER BY id",
        5min);
    REQUIRE(truth.rows() > 0);
    for (int row = 0; row < cold.rows(); ++row) {
        const std::int64_t id = parse_i64(cold.value(row, 0));
        bool found = false;
        for (int truth_row = 0; truth_row < truth.rows(); ++truth_row) {
            if (parse_i64(truth.value(truth_row, 0)) != id) {
                continue;
            }
            const std::int64_t target_start =
                parse_i64(cold.value(row, 1));
            const std::int64_t target_end =
                parse_i64(cold.value(row, 2));
            const std::int64_t target_len =
                parse_i64(cold.value(row, 3));
            const std::int64_t query_start =
                parse_i64(cold.value(row, 4));
            const std::int64_t query_end =
                parse_i64(cold.value(row, 5));
            const std::int64_t query_len =
                parse_i64(cold.value(row, 6));
            const std::string_view is_reverse =
                cold.value(row, 7);
            const std::int64_t truth_start =
                parse_i64(truth.value(truth_row, 1));
            const std::int64_t truth_end =
                parse_i64(truth.value(truth_row, 2));
            const std::string_view truth_is_reverse =
                truth.value(truth_row, 3);
            const std::int64_t original_query_len =
                parse_i64(truth.value(truth_row, 4));
            CHECK(target_start >= 0);
            CHECK(target_start < target_end);
            CHECK(target_end <=
                  static_cast<std::int64_t>(target_bases));
            CHECK(target_len ==
                  static_cast<std::int64_t>(target_bases));
            CHECK(query_start >= 0);
            CHECK(query_start < query_end);
            CHECK(query_end <= query_len);
            CHECK(query_len ==
                  static_cast<std::int64_t>(query_bases));
            CHECK(original_query_len ==
                  static_cast<std::int64_t>(query_bases));
            CHECK(absolute_difference(query_len, original_query_len)
                  <= coordinate_tolerance);
            CHECK((is_reverse == "t" || is_reverse == "f"));
            CHECK(is_reverse == truth_is_reverse);
            CHECK(absolute_difference(target_start, truth_start)
                  <= coordinate_tolerance);
            CHECK(absolute_difference(target_end, truth_end)
                  <= coordinate_tolerance);
            found = true;
            break;
        }
        CHECK(found);
    }
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.coordinates_match_truth");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.target_len_exact");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.query_bounds_and_strand");

    const bioseq::libpq::result warm = [&data, &fixture] {
        bioseq::postgres_test::scoped_report_phase phase{"warm"};
        bioseq::libpq::result result =
            data.exec(alignment_query, 5min);
        phase.complete();
        fixture.observe_resource_phase("warm");
        return result;
    }();
    const auto after_warm =
        bioseq::cache_test::read_cache_stats(admin);
    REQUIRE(warm.rows() > 0);
    REQUIRE(byte_identical(warm, cold));
    CHECK(bioseq::postgres_test::exact_row_hash(warm) == oracle_digest);
    const auto warm_delta = after_warm - after_cold;
    CHECK(warm_delta.produced == 0);
    CHECK(warm_delta.completed_hits == 1);
    CHECK(warm_delta.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.warm_identity");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.warm_counter_contract");

    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT count(*) FROM bioseq_cache.entries() "
              "WHERE kind = 'ExactFinalResult' "
              "AND state = 'COMPLETE' "
              "AND rows > 0 "
              "AND logical_bytes > 0 "
              "AND stored_bytes > 0")
          == 1);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.complete_entry");

    const auto explain = [&data, &fixture] {
        bioseq::postgres_test::scoped_report_phase phase{"explain"};
        auto result = bioseq::postgres_test::run_explain(
            data, alignment_query, true, 5min);
        phase.complete();
        fixture.observe_resource_phase("explain");
        return result;
    }();
    const auto cache_nodes =
        explain.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::explain_property_string(
              *cache_nodes.front(), "Actual Cache Action")
          == "[Hit] exact final result");
    const bioseq::postgres_test::json_value* children =
        cache_nodes.front()->find("Plans");
    if (children != nullptr) {
        REQUIRE(children->as_array() != nullptr);
        REQUIRE_FALSE(children->as_array()->empty());
        CHECK(bioseq::postgres_test::explain_property_number(
                  children->as_array()->front(), "Actual Loops")
              == 0.0);
        const auto bioseq_nodes = find_plan_nodes_with_property(
            *children, "Custom Plan Provider", "BioseqScan");
        REQUIRE(bioseq_nodes.size() == 1);
        REQUIRE(bioseq::postgres_test::explain_property_string(
                    *bioseq_nodes.front(), "Node Type")
                == "Custom Scan");
    }
    const auto after_explain =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK((after_explain - after_warm).completed_hits == 1);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.persisted_index_active");
    bioseq::postgres_test::record_migration_probe(
        "python.benchmark.provider_placeholder_removed");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.explain_hit");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.child_not_executed");

    bioseq::cache_test::exec(
        data, "SET bioseq.enable_column_cache = off");
    const auto column_cache_off = [&data, &fixture] {
        bioseq::postgres_test::scoped_report_phase phase{"column-cache"};
        auto result = data.exec(alignment_query, 5min);
        phase.complete();
        fixture.observe_resource_phase("column-cache");
        return result;
    }();
    const auto after_column_cache_off =
        bioseq::cache_test::read_cache_stats(admin);
    REQUIRE(column_cache_off.rows() > 0);
    REQUIRE(byte_identical(column_cache_off, cold));
    CHECK(bioseq::postgres_test::exact_row_hash(column_cache_off)
          == oracle_digest);
    const auto column_cache_off_delta =
        after_column_cache_off - after_explain;
    CHECK(column_cache_off_delta.produced == 0);
    CHECK(column_cache_off_delta.completed_hits == 1);
    CHECK(column_cache_off_delta.fallbacks == 0);

    const auto terminal_explain = bioseq::postgres_test::run_explain(
        data, alignment_query, true, 5min);
    const auto terminal_cache_nodes =
        terminal_explain.find_plan_nodes(
            "Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(terminal_cache_nodes.size() == 1);
    CHECK(bioseq::postgres_test::explain_property_string(
              *terminal_cache_nodes.front(), "Actual Cache Action")
          == "[Hit] exact final result");
    const bioseq::postgres_test::json_value* terminal_children =
        terminal_cache_nodes.front()->find("Plans");
    if (terminal_children != nullptr) {
        REQUIRE(terminal_children->as_array() != nullptr);
        REQUIRE_FALSE(terminal_children->as_array()->empty());
        CHECK(bioseq::postgres_test::explain_property_number(
                  terminal_children->as_array()->front(), "Actual Loops")
              == 0.0);
        const auto terminal_bioseq_nodes = find_plan_nodes_with_property(
            *terminal_children, "Custom Plan Provider", "BioseqScan");
        REQUIRE(terminal_bioseq_nodes.size() == 1);
    }
    const auto after_terminal_explain =
        bioseq::cache_test::read_cache_stats(admin);
    const auto terminal_explain_delta =
        after_terminal_explain - after_column_cache_off;
    CHECK(terminal_explain_delta.produced == 0);
    CHECK(terminal_explain_delta.completed_hits == 1);
    CHECK(terminal_explain_delta.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.column_cache_off_hit");
    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT count(*) FROM bioseq_cache.entries() "
              "WHERE state = 'BUILDING'")
          == 0);
    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT coalesce(sum(waiters), 0) "
              "FROM bioseq_cache.entries()")
          == 0);
    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT coalesce(sum(readers), 0) "
              "FROM bioseq_cache.entries()")
          == 0);
    const auto storage_diagnostic =
        bioseq::postgres_test::explain_property_string(
            *terminal_cache_nodes.front(), "Cache Storage");
    REQUIRE(storage_diagnostic.has_value());
    CHECK(diagnostic_metric(*storage_diagnostic, "waiters=") == 0);
    const auto ownership_diagnostic =
        bioseq::postgres_test::explain_property_string(
            *terminal_cache_nodes.front(), "Cache Storage Ownership");
    REQUIRE(ownership_diagnostic.has_value());
    CHECK(diagnostic_metric(*ownership_diagnostic, "readers=") == 0);
    CHECK(diagnostic_metric(*ownership_diagnostic, "pending_hot=") == 0);
    CHECK(diagnostic_metric(*ownership_diagnostic, "pending_file=") == 0);
    CHECK(diagnostic_metric(
              *ownership_diagnostic, "registry_waiters=")
          == 0);
    CHECK(diagnostic_metric(
              *ownership_diagnostic, "registry_readers=")
          == 0);
    CHECK(diagnostic_metric(*ownership_diagnostic, "planner_pins=") == 0);
    CHECK_FALSE(has_cache_temporary_file(
        fixture.cluster().data_directory()));
    const auto sampling = fixture.finish_resource_sampling();
    const auto repeated_sampling = fixture.finish_resource_sampling();
    REQUIRE(repeated_sampling.rss_samples == sampling.rss_samples);
    REQUIRE(
        repeated_sampling.owned_root_samples
        == sampling.owned_root_samples);
    REQUIRE(
        repeated_sampling.peak_postmaster_process_tree_rss_bytes
        == sampling.peak_postmaster_process_tree_rss_bytes);
    REQUIRE(
        repeated_sampling.peak_owned_root_bytes
        == sampling.peak_owned_root_bytes);
    REQUIRE(
        repeated_sampling.rss_sampling_failures
        == sampling.rss_sampling_failures);
    REQUIRE(
        repeated_sampling.owned_root_sampling_failures
        == sampling.owned_root_sampling_failures);
    REQUIRE(
        repeated_sampling.phase_boundaries.size()
        == sampling.phase_boundaries.size());
    REQUIRE(sampling.rss_samples >= 2);
    REQUIRE(sampling.owned_root_samples >= 2);
    REQUIRE(sampling.peak_postmaster_process_tree_rss_bytes > 0);
    REQUIRE(sampling.peak_owned_root_bytes > 0);
    REQUIRE(sampling.rss_sampling_failures == 0);
    REQUIRE(sampling.owned_root_sampling_failures == 0);
    for (const std::string_view phase : required_resource_phases) {
        const auto boundary = std::ranges::find_if(
            sampling.phase_boundaries,
            [phase](const bioseq::postgres_test::
                        resource_phase_boundary& candidate) {
                return candidate.phase == phase;
            });
        REQUIRE(boundary != sampling.phase_boundaries.end());
        REQUIRE(
            boundary->postmaster_process_tree_rss_bytes.has_value());
        REQUIRE(boundary->owned_root_bytes.has_value());
    }
    CHECK(std::chrono::steady_clock::now() - test_started < 105min);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.cleanup_invariants");
}
