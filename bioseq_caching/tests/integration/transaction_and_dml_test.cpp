#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

struct dependency_limit_case {
    std::int64_t maximum_key;
    std::int64_t expected_leaf_count;
    std::int64_t expected_dependency_count;
    std::int64_t expected_row_count;
    std::int64_t expected_total;
    bool cacheable;
};

constexpr std::array<dependency_limit_case, 3> dependency_limit_cases{{
    {1022, 1022, 1023, 1022, 522753, true},
    {1023, 1023, 1024, 1023, 523776, true},
    {1024, 1024, 1025, 1024, 524800, false},
}};

[[nodiscard]] std::size_t relation_leaf_count(
    const bioseq::postgres_test::explain_plan_document& plan,
    std::string_view prefix) {
    std::size_t count = 0;
    for (const bioseq::postgres_test::explain_json_value* node :
         plan.plan_nodes()) {
        const auto relation =
            bioseq::postgres_test::read_plan_string_property(
                *node, "Relation Name");
        if (relation && relation->starts_with(prefix)) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("COPY, SPI, and dependency limits preserve exact generations",
          "[component:cache][layer:integration]"
          "[feature:transaction][resource:postgres]") {

    SECTION("caught SPI, routed COPY, pressure overflow, and dependency boundary") {
        bioseq::postgres_test::private_cluster_fixture fixture{
            bioseq::cache_test::cluster_options(
                false, bioseq::cache_test::preload_order::cache_only)};
        auto& connection = fixture.admin();
        bioseq::cache_test::execute(
            connection,
            "SET bioseq_cache.query_cache.mode = 'exact_query'");

        bioseq::cache_test::execute(
            connection,
            R"SQL(
                CREATE TABLE cache_caught_spi(
                    id integer PRIMARY KEY,
                    payload text NOT NULL);
                CREATE FUNCTION cache_caught_spi_write() RETURNS void
                LANGUAGE plpgsql VOLATILE AS $$
                BEGIN
                    BEGIN
                        INSERT INTO cache_caught_spi
                        VALUES (1, 'must roll back');
                        RAISE EXCEPTION 'caught write';
                    EXCEPTION WHEN OTHERS THEN
                        NULL;
                    END;
                END
                $$;
            )SQL");
        bioseq::cache_test::execute(
            connection,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        constexpr std::string_view caught_spi_query{
            "SELECT id, payload FROM cache_caught_spi ORDER BY id"};
        const auto spi_cold = connection.execute(caught_spi_query);
        const auto spi_warm = connection.execute(caught_spi_query);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(spi_cold)
              == bioseq::postgres_test::hash_ordered_result_rows(spi_warm));
        bioseq::cache_test::execute(
            connection, "SELECT cache_caught_spi_write()");
        const auto after_caught_spi = connection.execute(caught_spi_query);
        CHECK(after_caught_spi.row_count() == 0);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(after_caught_spi)
              == bioseq::postgres_test::hash_ordered_result_rows(spi_cold));
        const auto caught_spi_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(caught_spi_stats.produced == 1);
        CHECK(caught_spi_stats.completed_hits == 2);
        CHECK(caught_spi_stats.fallbacks == 0);
        CHECK(caught_spi_stats.invalidations == 0);

        bioseq::cache_test::execute(
            connection,
            R"SQL(
                CREATE TABLE cache_copy_routing(
                    id integer NOT NULL,
                    payload text NOT NULL
                ) PARTITION BY RANGE (id);
                CREATE TABLE cache_copy_routing_low
                    PARTITION OF cache_copy_routing
                    FOR VALUES FROM (0) TO (100);
                CREATE TABLE cache_copy_routing_high
                    PARTITION OF cache_copy_routing
                    FOR VALUES FROM (100) TO (200);
                SELECT bioseq_cache.invalidate_all();
                SELECT bioseq_cache.reset_stats();
            )SQL");
        const std::filesystem::path committed_copy_path =
            fixture.cluster().root_directory() / "cache-copy-commit.csv";
        const std::filesystem::path rolled_back_copy_path =
            fixture.cluster().root_directory() / "cache-copy-rollback.csv";
        {
            std::ofstream output(committed_copy_path, std::ios::binary);
            REQUIRE(output.is_open());
            output << "1,low\n101,high\n";
            output.flush();
            REQUIRE(output.good());
        }
        {
            std::ofstream output(rolled_back_copy_path, std::ios::binary);
            REQUIRE(output.is_open());
            output << "2,rolled back\n102,rolled back\n";
            output.flush();
            REQUIRE(output.good());
        }
        constexpr std::string_view copy_query{
            "SELECT id, payload FROM cache_copy_routing ORDER BY id"};
        static_cast<void>(connection.execute(copy_query));
        static_cast<void>(connection.execute(copy_query));
        bioseq::cache_test::execute(
            connection,
            "COPY cache_copy_routing FROM "
                + connection.escape_literal(committed_copy_path.string())
                + " WITH (FORMAT csv)",
            30s);
        const auto committed_copy = connection.execute(copy_query);
        REQUIRE(committed_copy.row_count() == 2);
        CHECK(committed_copy.text_value(0, 0) == "1");
        CHECK(committed_copy.text_value(0, 1) == "low");
        CHECK(committed_copy.text_value(1, 0) == "101");
        CHECK(committed_copy.text_value(1, 1) == "high");
        const auto committed_copy_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(committed_copy_stats.produced == 2);
        CHECK(committed_copy_stats.completed_hits == 1);
        CHECK(committed_copy_stats.fallbacks == 0);
        CHECK(committed_copy_stats.invalidations > 0);

        bioseq::cache_test::execute(
            connection,
            "SELECT bioseq_cache.invalidate_all(); "
            "SELECT bioseq_cache.reset_stats()");
        const auto before_rollback_copy = connection.execute(copy_query);
        static_cast<void>(connection.execute(copy_query));
        bioseq::cache_test::execute(
            connection, "BEGIN", 10s,
            bioseq::libpq::transaction_policy::allow_open);
        bioseq::cache_test::execute(
            connection,
            "COPY cache_copy_routing FROM "
                + connection.escape_literal(rolled_back_copy_path.string())
                + " WITH (FORMAT csv)",
            30s, bioseq::libpq::transaction_policy::allow_open);
        bioseq::cache_test::execute(
            connection, "ROLLBACK", 10s,
            bioseq::libpq::transaction_policy::require_idle);
        const auto after_rollback_copy = connection.execute(copy_query);
        CHECK(bioseq::postgres_test::hash_ordered_result_rows(after_rollback_copy)
              == bioseq::postgres_test::hash_ordered_result_rows(
                  before_rollback_copy));
        auto copy_oracle = fixture.connect();
        bioseq::cache_test::execute(
            copy_oracle,
            "SET bioseq_cache.query_cache.mode = 'disabled'");
        CHECK(bioseq::cache_test::integer(
                  copy_oracle,
                  "SELECT count(*) FROM cache_copy_routing "
                  "WHERE id IN (2, 102)")
              == 0);
        const auto rollback_copy_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(rollback_copy_stats.produced == 1);
        CHECK(rollback_copy_stats.completed_hits == 2);
        CHECK(rollback_copy_stats.fallbacks == 0);
        CHECK(rollback_copy_stats.invalidations == 0);

        bioseq::cache_test::execute(
            connection,
            R"SQL(
                DO $$
                BEGIN
                    FOR relation_index IN 1..258 LOOP
                        EXECUTE format(
                            'CREATE TABLE cache_pressure_%s('
                            'id integer PRIMARY KEY)',
                            relation_index);
                    END LOOP;
                END
                $$;
                CREATE FUNCTION cache_pressure_fill() RETURNS void
                LANGUAGE plpgsql VOLATILE AS $$
                BEGIN
                    FOR relation_index IN 1..258 LOOP
                        EXECUTE format(
                            'INSERT INTO cache_pressure_%s VALUES (1)',
                            relation_index);
                    END LOOP;
                END
                $$;
                SELECT bioseq_cache.invalidate_all();
                SELECT bioseq_cache.reset_stats();
            )SQL",
            90s);
        constexpr std::string_view pressure_query{
            "SELECT id FROM cache_pressure_258 ORDER BY id"};
        static_cast<void>(connection.execute(pressure_query));
        static_cast<void>(connection.execute(pressure_query));
        bioseq::cache_test::execute(
            connection, "SELECT cache_pressure_fill()", 90s);
        const auto after_pressure = connection.execute(pressure_query);
        REQUIRE(after_pressure.row_count() == 1);
        CHECK(after_pressure.text_value(0, 0) == "1");
        const auto pressure_stats =
            bioseq::cache_test::read_cache_stats(connection);
        CHECK(pressure_stats.produced == 2);
        CHECK(pressure_stats.completed_hits == 1);
        CHECK(pressure_stats.fallbacks == 0);
        CHECK(pressure_stats.invalidations > 0);

        bioseq::cache_test::execute(
            connection,
            R"SQL(
                CREATE TABLE cache_dependency_partitioned(
                    partition_key integer NOT NULL,
                    payload integer NOT NULL
                ) PARTITION BY RANGE (partition_key);
                DO $$
                BEGIN
                    FOR leaf_index IN 1..1024 LOOP
                        EXECUTE format(
                            'CREATE TABLE %I '
                            'PARTITION OF cache_dependency_partitioned '
                            'FOR VALUES FROM (%s) TO (%s)',
                            'cache_dependency_leaf_' || leaf_index,
                            leaf_index,
                            leaf_index + 1);
                    END LOOP;
                END
                $$;
                INSERT INTO cache_dependency_partitioned
                SELECT value, value * 10
                FROM generate_series(1, 1024) AS generated(value);
                SET max_parallel_workers_per_gather = 0;
            )SQL",
            180s);

        for (const dependency_limit_case& limit :
             dependency_limit_cases) {
            const std::string query =
                "SELECT count(*)::bigint AS row_count, "
                "sum(partition_key)::bigint AS total "
                "FROM cache_dependency_partitioned "
                "WHERE partition_key <= "
                + std::to_string(limit.maximum_key);

            bioseq::cache_test::execute(
                connection,
                "SET bioseq_cache.query_cache.mode = 'disabled'");
            const auto base_plan = bioseq::postgres_test::execute_explain(
                connection, query, false, 120s);
            CHECK(relation_leaf_count(
                      base_plan, "cache_dependency_leaf_")
                  == static_cast<std::size_t>(
                      limit.expected_leaf_count));

            bioseq::cache_test::execute(
                connection,
                "SET bioseq_cache.query_cache.mode = 'exact_query'");
            const auto cache_plan = bioseq::postgres_test::execute_explain(
                connection, query, false, 120s);
            const auto cache_nodes =
                cache_plan.find_plan_nodes(
                    "Custom Plan Provider", "BioseqCacheScan");
            if (limit.cacheable) {
                REQUIRE(cache_nodes.size() == 1);
                CHECK(
                    bioseq::postgres_test::read_plan_integer_property(
                        *cache_nodes.front(), "Cache Dependency Count")
                    == limit.expected_dependency_count);
            } else {
                CHECK(cache_nodes.empty());
            }

            bioseq::cache_test::execute(
                connection,
                "SELECT bioseq_cache.invalidate_all(); "
                "SELECT bioseq_cache.reset_stats()");
            const auto first = connection.execute(query, 120s);
            const auto second = connection.execute(query, 120s);
            REQUIRE(first.row_count() == 1);
            REQUIRE(first.column_count() == 2);
            REQUIRE(second.row_count() == 1);
            CHECK(std::string_view(first.text_value(0, 0))
                  == std::to_string(limit.expected_row_count));
            CHECK(std::string_view(first.text_value(0, 1))
                  == std::to_string(limit.expected_total));
            CHECK(bioseq::postgres_test::hash_ordered_result_rows(first)
                  == bioseq::postgres_test::hash_ordered_result_rows(second));

            const auto stats =
                bioseq::cache_test::read_cache_stats(connection);
            CHECK(stats.inflight_waits == 0);
            CHECK(stats.fallbacks == 0);
            if (limit.cacheable) {
                CHECK(stats.produced == 1);
                CHECK(stats.completed_hits == 1);
            } else {
                CHECK(stats.produced == 0);
                CHECK(stats.completed_hits == 0);
                CHECK(bioseq::cache_test::integer(
                          connection,
                          "SELECT count(*) "
                          "FROM bioseq_cache_test.entry_states() "
                          "WHERE state IN ('BUILDING', 'COMPLETE')")
                      == 0);
            }
        }

    }
}
