#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string_view>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

constexpr std::string_view transaction_query{
    "SELECT id, value FROM cache_transaction_rows ORDER BY id"};

}  // namespace

TEST_CASE("transaction visibility and relation invalidation remain coherent",
          "[component:cache][layer:integration]"
          "[feature:transaction][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    auto data = fixture.connect();

    bioseq::cache_test::execute(
        admin,
        "SET bioseq_cache.query_cache.mode = 'disabled'; "
        "CREATE TABLE cache_transaction_rows("
        "id integer PRIMARY KEY, value text NOT NULL); "
        "INSERT INTO cache_transaction_rows VALUES "
        "(1, 'one'), (2, 'two')");
    bioseq::cache_test::execute(
        data, "SET bioseq_cache.query_cache.mode = 'exact_query'");
    const auto first = data.execute(transaction_query);
    const auto hit = data.execute(transaction_query);
    REQUIRE(first.row_count() == 2);
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(first)
          == bioseq::postgres_test::hash_ordered_result_rows(hit));

    bioseq::cache_test::execute(
        data, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        data,
        "UPDATE cache_transaction_rows "
        "SET value = 'rolled-back' WHERE id = 1",
        10s, bioseq::libpq::transaction_policy::allow_open);
    const auto inside = data.execute(
        transaction_query, 10s,
        bioseq::libpq::transaction_policy::allow_open);
    REQUIRE(inside.row_count() == 2);
    CHECK(inside.text_value(0, 1) == "rolled-back");
    bioseq::cache_test::execute(
        data, "ROLLBACK", 10s,
        bioseq::libpq::transaction_policy::require_idle);

    const auto after_rollback = data.execute(transaction_query);
    CHECK(after_rollback.text_value(0, 1) == "one");

    bioseq::cache_test::execute(
        data, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        data,
        "UPDATE cache_transaction_rows "
        "SET value = 'committed' WHERE id = 1",
        10s, bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        data, "COMMIT", 10s,
        bioseq::libpq::transaction_policy::require_idle);
    const auto after_commit = data.execute(transaction_query);
    CHECK(after_commit.text_value(0, 1) == "committed");
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(after_commit)
          != bioseq::postgres_test::hash_ordered_result_rows(first));

    bioseq::cache_test::execute(
        data, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        data, "SAVEPOINT cache_before_write", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        data,
        "UPDATE cache_transaction_rows "
        "SET value = 'savepoint-write' WHERE id = 2",
        10s, bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        data, "ROLLBACK TO SAVEPOINT cache_before_write", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::execute(
        data, "COMMIT", 10s,
        bioseq::libpq::transaction_policy::require_idle);
    const auto after_savepoint = data.execute(transaction_query);
    CHECK(after_savepoint.text_value(1, 1) == "two");

    bioseq::cache_test::execute(admin, "TRUNCATE cache_transaction_rows");
    const auto after_truncate = data.execute(transaction_query);
    CHECK(after_truncate.row_count() == 0);
}

TEST_CASE("partition and trigger writes invalidate exact generations",
          "[component:cache][layer:integration]"
          "[feature:transaction][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();

    bioseq::cache_test::execute(
        connection,
        R"SQL(
            SET bioseq_cache.query_cache.mode = 'exact_query';
            CREATE TABLE cache_partition_rows(
                id integer, value integer
            ) PARTITION BY RANGE (id);
            CREATE TABLE cache_partition_rows_low
                PARTITION OF cache_partition_rows FOR VALUES FROM (0) TO (100);
            CREATE TABLE cache_partition_rows_high
                PARTITION OF cache_partition_rows
                FOR VALUES FROM (100) TO (200);
            CREATE TABLE cache_trigger_audit(
                id integer, old_value integer, new_value integer);
            CREATE FUNCTION cache_trigger_capture()
            RETURNS trigger
            LANGUAGE plpgsql
            AS $$
            BEGIN
                INSERT INTO cache_trigger_audit
                VALUES (NEW.id, OLD.value, NEW.value);
                RETURN NEW;
            END
            $$;
            CREATE TRIGGER cache_partition_update
            AFTER UPDATE ON cache_partition_rows
            FOR EACH ROW EXECUTE FUNCTION cache_trigger_capture();
            INSERT INTO cache_partition_rows VALUES (1, 10), (101, 20);
        )SQL");
    constexpr std::string_view query{
        "SELECT id, value FROM cache_partition_rows ORDER BY id"};
    const auto cold = connection.execute(query);
    const auto warm = connection.execute(query);
    CHECK(bioseq::postgres_test::hash_ordered_result_rows(cold)
          == bioseq::postgres_test::hash_ordered_result_rows(warm));

    bioseq::cache_test::execute(
        connection,
        "UPDATE cache_partition_rows SET value = value + 1 WHERE id = 101");
    const auto refreshed = connection.execute(query);
    CHECK(refreshed.text_value(1, 1) == "21");
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT count(*) FROM cache_trigger_audit "
              "WHERE id = 101 AND old_value = 20 AND new_value = 21")
          == 1);
}
