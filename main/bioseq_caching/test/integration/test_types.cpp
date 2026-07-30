#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

TEST_CASE("exact replay preserves PostgreSQL type representations",
          "[cache][integration][exact][types][toast]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "exact replay preserves PostgreSQL type representations"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();

    bioseq::cache_test::exec(
        connection,
        R"SQL(
            SET TimeZone = 'UTC';
            SET bioseq.query_cache_optimisation_mode = 'disabled';
            CREATE TYPE cache_test_enum AS ENUM ('one', 'two');
            CREATE DOMAIN cache_test_positive AS integer CHECK (VALUE > 0);
            CREATE TYPE cache_test_pair AS (x integer, y text);
            CREATE TABLE cache_type_rows(
                id bigint,
                i2 smallint,
                i4 integer,
                i8 bigint,
                b boolean,
                f4 real,
                f8 double precision,
                n numeric,
                t text,
                bytes bytea,
                u uuid,
                d date,
                tm time,
                ts timestamp,
                tstz timestamptz,
                iv interval,
                a integer[],
                j jsonb,
                e cache_test_enum,
                dom cache_test_positive,
                pair cache_test_pair
            );
            INSERT INTO cache_type_rows VALUES
              (1, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
               NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
               NULL, NULL, NULL),
              (2, -32768, -2147483648, -9223372036854775808, false,
               '-Infinity', 'NaN', 1234567890.00100, '', '\x',
               '00000000-0000-0000-0000-000000000000',
               '2000-01-01', '00:00:00', '2000-01-01 00:00:00',
               '2000-01-01 00:00:00+00', '0 seconds', '{}',
               '{}'::jsonb, 'one', 1,
               ROW(NULL, '')::cache_test_pair),
              (3, 32767, 2147483647, 9223372036854775807, true,
               'Infinity', '-Infinity', -0.0000012300, 'zażółć €',
               decode('004100e282acff', 'hex'),
               'ffffffff-ffff-ffff-ffff-ffffffffffff',
               '2099-12-31', '23:59:59.999999',
               '2099-12-31 23:59:59.999999',
               '2099-12-31 23:59:59.999999+00',
               '2 years 3 mons 4 days 05:06:07',
               ARRAY[1, NULL, 3],
               '{"nested":[1,null,"x"]}'::jsonb, 'two', 42,
               ROW(7, repeat('toast', 3000))::cache_test_pair),
              (4, 7, 8, 9, true, 1.5, 2.5, 999.9900,
               repeat('large-text-', 5000),
               decode(repeat('0041', 6000), 'hex'),
               '01234567-89ab-cdef-0123-456789abcdef',
               '2026-07-30', '12:34:56.123456',
               '2026-07-30 12:34:56.123456',
               '2026-07-30 12:34:56.123456+00',
               interval '1 microsecond', ARRAY[]::integer[],
               'null'::jsonb, 'one', 2,
               ROW(8, NULL)::cache_test_pair);
        )SQL");

    constexpr std::string_view query{
        "SELECT * FROM cache_type_rows ORDER BY id"};
    const bioseq::libpq::result oracle = connection.exec(query, 30s);
    REQUIRE(oracle.rows() == 4);
    REQUIRE(oracle.columns() == 21);
    const auto oracle_digest =
        bioseq::postgres_test::exact_row_hash(oracle);

    bioseq::cache_test::exec(
        connection,
        "SET bioseq.query_cache_optimisation_mode = 'same'; "
        "SELECT bioseq_cache.invalidate_all(); "
        "SELECT bioseq_cache.reset_stats()");
    const bioseq::libpq::result cold = connection.exec(query, 30s);
    const bioseq::libpq::result warm = connection.exec(query, 30s);
    CHECK(bioseq::postgres_test::exact_row_hash(cold) == oracle_digest);
    CHECK(bioseq::postgres_test::exact_row_hash(warm) == oracle_digest);
    CHECK(warm.is_null(0, 1));
    CHECK(warm.value(2, 8) == "zażółć €");
    CHECK(warm.value(2, 9) == "\\x004100e282acff");
    CHECK(warm.value(2, 16) == "{1,NULL,3}");
    CHECK(warm.value(2, 17) == R"({"nested": [1, null, "x"]})");
    CHECK(warm.value(1, 5) == "-Infinity");
    CHECK(warm.value(1, 6) == "NaN");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.nulls");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.scalar_extremes");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.time_values");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.array_json");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.enum_domain_composite");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.bytea_uuid_unicode");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.toast");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.binary_identity");

    const auto stats = bioseq::cache_test::read_cache_stats(connection);
    CHECK(stats.produced >= 1);
    CHECK(stats.completed_hits >= 1);
    CHECK(stats.fallbacks == 0);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_types.producer_and_hit");
}
