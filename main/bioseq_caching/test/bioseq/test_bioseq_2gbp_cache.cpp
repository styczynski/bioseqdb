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

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::uint64_t reference_bases = 2'000'000'000ULL;
constexpr std::uint64_t gibibyte = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t minimum_total_ram = 16ULL * gibibyte;
constexpr std::uint64_t minimum_available_ram = 8ULL * gibibyte;
constexpr std::uint64_t minimum_data_space =
    32ULL * gibibyte > 12ULL * reference_bases
    ? 32ULL * gibibyte
    : 12ULL * reference_bases;
constexpr std::uint64_t io_probe_bytes = gibibyte;
constexpr double minimum_io_mib_per_second = 150.0;
constexpr std::int64_t coordinate_tolerance = 50'000;

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

[[nodiscard]] bool has_partial_file(
    const std::filesystem::path& root) {
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator{
             root, std::filesystem::directory_options::skip_permission_denied,
             error},
         end;
         !error && iterator != end;
         iterator.increment(error)) {
        if (iterator->is_regular_file(error)
            && iterator->path().extension() == ".partial") {
            return true;
        }
        error.clear();
    }
    return false;
}

}  // namespace

TEST_CASE("exactly 2 Gbp Bioseq result produces once and replays exactly",
          "[cache][integration][bioseq][large][2gbp]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "exactly 2 Gbp Bioseq result produces once and replays exactly"};
    const auto test_started = std::chrono::steady_clock::now();
    auto options = bioseq::cache_test::cluster_options(
        true, bioseq::cache_test::preload_order::cache_then_bioseq);
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

    configure_canonical_cpu_path(admin);
    configure_canonical_cpu_path(data);
    {
        bioseq::postgres_test::scoped_report_phase phase{"generation"};
        bioseq::cache_test::exec(
            admin,
            "SET bioseq.update_index_on_insert = off; "
            "SELECT bioseq.create_query_target_sample_table("
            "'cache_bioseq_2gbp', 1, 5000000, 100000.0, 2000000000, "
            "0.001, 0.0005, 0.0005, 1.0, 1.0, 0.5, 3219876)",
            20min);
        phase.complete();
        fixture.observe_resource_phase("generation");
    }
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
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.persisted_index_active");

    bioseq::cache_test::exec(
        admin,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    bioseq::cache_test::exec(
        data,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'");
    const bioseq::libpq::result oracle = [&] {
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
    const auto before_cold =
        bioseq::cache_test::read_cache_stats(admin);
    const bioseq::libpq::result cold = [&] {
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
    CHECK(bioseq::postgres_test::exact_row_hash(cold) == oracle_digest);
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
                  static_cast<std::int64_t>(reference_bases));
            CHECK(target_len ==
                  static_cast<std::int64_t>(reference_bases));
            CHECK(query_start >= 0);
            CHECK(query_start < query_end);
            CHECK(query_end <= query_len);
            CHECK(query_len > 0);
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

    const bioseq::libpq::result warm = [&] {
        bioseq::postgres_test::scoped_report_phase phase{"warm"};
        bioseq::libpq::result result =
            data.exec(alignment_query, 5min);
        phase.complete();
        fixture.observe_resource_phase("warm");
        return result;
    }();
    const auto after_warm =
        bioseq::cache_test::read_cache_stats(admin);
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

    const auto explain = [&] {
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
    REQUIRE(children != nullptr);
    REQUIRE(children->as_array() != nullptr);
    REQUIRE_FALSE(children->as_array()->empty());
    CHECK(bioseq::postgres_test::explain_property_number(
              children->as_array()->front(), "Actual Loops")
          == 0.0);
    const auto after_explain =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK((after_explain - after_warm).completed_hits == 1);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.explain_hit");
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.child_not_executed");

    bioseq::cache_test::exec(
        data, "SET bioseq.enable_column_cache = off");
    const auto column_cache_off = [&] {
        bioseq::postgres_test::scoped_report_phase phase{"column-cache"};
        auto result = data.exec(alignment_query, 5min);
        phase.complete();
        fixture.observe_resource_phase("column-cache");
        return result;
    }();
    const auto after_column_cache_off =
        bioseq::cache_test::read_cache_stats(admin);
    CHECK(bioseq::postgres_test::exact_row_hash(column_cache_off)
          == oracle_digest);
    CHECK((after_column_cache_off - after_explain).completed_hits == 1);
    CHECK((after_column_cache_off - after_explain).fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.column_cache_off_hit");

    CHECK(bioseq::cache_test::integer(
              admin,
              "SELECT count(*) FROM bioseq_cache.entries() "
              "WHERE state = 'BUILDING'")
          == 0);
    const auto references = admin.exec(
        "SELECT reader_pins, waiter_references, admission_reservations "
        "FROM bioseq_cache_test.planner_reference_counts()");
    REQUIRE(references.rows() == 1);
    CHECK(references.value(0, 0) == "0");
    CHECK(references.value(0, 1) == "0");
    CHECK(references.value(0, 2) == "0");
    CHECK_FALSE(has_partial_file(fixture.cluster().data_directory()));
    CHECK(std::chrono::steady_clock::now() - test_started < 105min);
    bioseq::postgres_test::record_migration_probe(
        "combined.bioseq_2gbp.cleanup_invariants");
}
