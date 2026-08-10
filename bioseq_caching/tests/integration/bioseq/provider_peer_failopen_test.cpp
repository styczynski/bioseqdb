#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

import bioseq.cache.test_context;
import bioseq.test.postgres;

using namespace std::chrono_literals;

namespace {

[[nodiscard]] bioseq::postgres_test::private_cluster_options
provider_peer_options(std::string mode) {
    const auto& configuration =
        bioseq::cache_test::configuration();
    if (!configuration.provider_peer_test_dso.has_value()) {
        throw std::logic_error(
            "provider-peer test DSO was not supplied");
    }
    auto options = bioseq::cache_test::cluster_options(
        true,
        bioseq::cache_test::preload_order::cache_only);
    if (options.extensions.size() != 2
        || options.extensions.front().name != "bioseq_cache") {
        throw std::logic_error(
            "provider-peer test requires cache and Bioseq artifacts");
    }
    options.extensions.front().dso =
        *configuration.provider_peer_test_dso;
    options.allowed_runtime_dso_overrides.insert("bioseq_cache");
    options.startup_gucs.emplace_back(
        "bioseq_cache_peer_test.mode",
        std::move(mode));
    return options;
}

}  // namespace

TEST_CASE(
    "Bioseq provider rendezvous warns and fails open for an incompatible cache API",
    "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        provider_peer_options("incompatible_cache_api"),
        false};
    auto& connection = fixture.admin();

    static_cast<void>(connection.drain_notices());
    bioseq::cache_test::execute(connection, "LOAD 'bioseq'");
    const auto notices = connection.drain_notices();
    const auto warning = std::ranges::find_if(
        notices,
        [](const bioseq::libpq::server_notice& notice) {
            return notice.message
                == "Bioseq ignored an incompatible "
                   "cache API rendezvous";
        });
    REQUIRE(warning != notices.end());
    CHECK(warning->severity == "WARNING");

    bioseq::cache_test::execute(
        connection,
        "CREATE FUNCTION "
        "public.bioseq_cache_peer_registration_calls() "
        "RETURNS bigint "
        "AS 'bioseq_cache', "
        "'bioseq_cache_peer_test_registration_calls' "
        "LANGUAGE C");
    CHECK(
        bioseq::cache_test::integer(
            connection,
            "SELECT "
            "public.bioseq_cache_peer_registration_calls()")
        == 0);

    bioseq::cache_test::execute(
        connection,
        "CREATE EXTENSION bioseq");
    CHECK(
        bioseq::cache_test::integer(
            connection,
            "SELECT count(*) "
            "FROM pg_type AS t "
            "JOIN pg_namespace AS n "
            "  ON n.oid = t.typnamespace "
            "WHERE n.nspname = 'bioseq' "
            "  AND t.typname = 'seq'")
        == 1);
    CHECK(
        bioseq::cache_test::integer(
            connection,
            "SELECT bioseq.len("
            "'ACGT'::bioseq.seq('dna4', 8))")
        == 4);
    CHECK(
        bioseq::cache_test::integer(
            connection,
            "SELECT count(*) FROM pg_extension "
            "WHERE extname = 'bioseq_cache'")
        == 0);
    CHECK(
        bioseq::cache_test::integer(
            connection,
            "SELECT "
            "public.bioseq_cache_peer_registration_calls()")
        == 0);
}

TEST_CASE(
    "Bioseq provider rendezvous rejects a foreign provider directory collision",
    "[component:cache][layer:integration]"
          "[feature:provider][resource:postgres]") {
    bioseq::postgres_test::private_cluster_fixture fixture{
        provider_peer_options("provider_directory_collision"),
        false};
    auto& connection = fixture.admin();

    const auto observed =
        bioseq::postgres_test::execute_expecting_sql_state(
            connection,
            "LOAD 'bioseq'",
            "42710",
            30s);
    CHECK(
        observed.message.find(
            "Bioseq cache-provider rendezvous is already owned")
        != std::string::npos);
    CHECK(observed.severity == "ERROR");
}
