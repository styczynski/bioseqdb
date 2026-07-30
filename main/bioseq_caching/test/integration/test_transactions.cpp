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
          "[cache][integration][dml][transactions][savepoint]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "transaction visibility and relation invalidation remain coherent"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& admin = fixture.admin();
    auto data = fixture.connect();

    bioseq::cache_test::exec(
        admin,
        "SET bioseq.query_cache_optimisation_mode = 'disabled'; "
        "CREATE TABLE cache_transaction_rows("
        "id integer PRIMARY KEY, value text NOT NULL); "
        "INSERT INTO cache_transaction_rows VALUES "
        "(1, 'one'), (2, 'two')");
    bioseq::cache_test::exec(
        data, "SET bioseq.query_cache_optimisation_mode = 'same'");
    const auto first = data.exec(transaction_query);
    const auto hit = data.exec(transaction_query);
    REQUIRE(first.rows() == 2);
    CHECK(bioseq::postgres_test::exact_row_hash(first)
          == bioseq::postgres_test::exact_row_hash(hit));

    bioseq::cache_test::exec(
        data, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        data,
        "UPDATE cache_transaction_rows "
        "SET value = 'rolled-back' WHERE id = 1",
        10s, bioseq::libpq::transaction_policy::allow_open);
    const auto inside = data.exec(
        transaction_query, 10s,
        bioseq::libpq::transaction_policy::allow_open);
    REQUIRE(inside.rows() == 2);
    CHECK(inside.value(0, 1) == "rolled-back");
    bioseq::cache_test::exec(
        data, "ROLLBACK", 10s,
        bioseq::libpq::transaction_policy::require_idle);

    const auto after_rollback = data.exec(transaction_query);
    CHECK(after_rollback.value(0, 1) == "one");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_dml_transactions.rollback_visibility");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_mvcc_rollback.no_uncommitted_publish");

    bioseq::cache_test::exec(
        data, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        data,
        "UPDATE cache_transaction_rows "
        "SET value = 'committed' WHERE id = 1",
        10s, bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        data, "COMMIT", 10s,
        bioseq::libpq::transaction_policy::require_idle);
    const auto after_commit = data.exec(transaction_query);
    CHECK(after_commit.value(0, 1) == "committed");
    CHECK(bioseq::postgres_test::exact_row_hash(after_commit)
          != bioseq::postgres_test::exact_row_hash(first));
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_dml_transactions.commit_visibility");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_mvcc_publish.committed_generation_visible");

    bioseq::cache_test::exec(
        data, "BEGIN", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        data, "SAVEPOINT cache_before_write", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        data,
        "UPDATE cache_transaction_rows "
        "SET value = 'savepoint-write' WHERE id = 2",
        10s, bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        data, "ROLLBACK TO SAVEPOINT cache_before_write", 10s,
        bioseq::libpq::transaction_policy::allow_open);
    bioseq::cache_test::exec(
        data, "COMMIT", 10s,
        bioseq::libpq::transaction_policy::require_idle);
    const auto after_savepoint = data.exec(transaction_query);
    CHECK(after_savepoint.value(1, 1) == "two");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_dml_transactions.savepoint_rollback");

    bioseq::cache_test::exec(admin, "TRUNCATE cache_transaction_rows");
    const auto after_truncate = data.exec(transaction_query);
    CHECK(after_truncate.rows() == 0);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_dml_transactions.truncate_invalidates");
}

TEST_CASE("partition and trigger writes invalidate exact generations",
          "[cache][integration][dml][partition][trigger]") {
    bioseq::postgres_test::migration_probe_scope probes{
        "partition and trigger writes invalidate exact generations"};
    bioseq::postgres_test::postgres_fixture fixture{
        bioseq::cache_test::cluster_options(
            false, bioseq::cache_test::preload_order::cache_only)};
    auto& connection = fixture.admin();

    bioseq::cache_test::exec(
        connection,
        R"SQL(
            SET bioseq.query_cache_optimisation_mode = 'same';
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
    const auto cold = connection.exec(query);
    const auto warm = connection.exec(query);
    CHECK(bioseq::postgres_test::exact_row_hash(cold)
          == bioseq::postgres_test::exact_row_hash(warm));

    bioseq::cache_test::exec(
        connection,
        "UPDATE cache_partition_rows SET value = value + 1 WHERE id = 101");
    const auto refreshed = connection.exec(query);
    CHECK(refreshed.value(1, 1) == "21");
    CHECK(bioseq::cache_test::integer(
              connection,
              "SELECT count(*) FROM cache_trigger_audit "
              "WHERE id = 101 AND old_value = 20 AND new_value = 21")
          == 1);
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_dml_transactions.partition_write_invalidates");
    bioseq::postgres_test::record_migration_probe(
        "regress.cache_dml_transactions.trigger_write_observed");
    bioseq::postgres_test::record_migration_probe(
        "isolation.cache_reuse_provider_partition.partition_identity");
}
