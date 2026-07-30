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

constexpr std::string_view rendezvous_case{
    "Bioseq provider rendezvous rejects only its owned directory collision"};

[[nodiscard]] bioseq::postgres_test::cluster_options
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
    options.startup_gucs.emplace_back(
        "bioseq_cache_peer_test.mode",
        std::move(mode));
    return options;
}

}  // namespace

TEST_CASE(
    "Bioseq provider rendezvous rejects only its owned directory collision",
    "[cache][integration][bioseq][provider][rendezvous][fault]") {
    bioseq::postgres_test::migration_probe_scope probes{
        std::string(rendezvous_case)};

    SECTION("incompatible non-null cache API warns and fails open") {
        bioseq::postgres_test::migration_probe_scope section_probes{
            std::string(rendezvous_case),
            "incompatible non-null cache API warns and fails open"};
        bioseq::postgres_test::postgres_fixture fixture{
            provider_peer_options("incompatible_cache_api"),
            false};
        auto& connection = fixture.admin();

        static_cast<void>(connection.take_notices());
        bioseq::cache_test::exec(connection, "LOAD 'bioseq'");
        const auto notices = connection.take_notices();
        const auto warning = std::ranges::find_if(
            notices,
            [](const bioseq::libpq::notice& notice) {
                return notice.message
                    == "Bioseq ignored an incompatible "
                       "cache API rendezvous";
            });
        REQUIRE(warning != notices.end());
        CHECK(warning->severity == "WARNING");

        bioseq::cache_test::exec(
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

        bioseq::cache_test::exec(
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
        bioseq::postgres_test::record_migration_probe("combined.provider_rendezvous.incompatible_peer_warns_fail_open");
        bioseq::postgres_test::record_migration_probe("combined.provider_rendezvous.incompatible_peer_no_token");
    }

    SECTION("foreign provider directory remains a fatal collision") {
        bioseq::postgres_test::migration_probe_scope section_probes{
            std::string(rendezvous_case),
            "foreign provider directory remains a fatal collision"};
        bioseq::postgres_test::postgres_fixture fixture{
            provider_peer_options("provider_directory_collision"),
            false};
        auto& connection = fixture.admin();

        const auto observed =
            bioseq::postgres_test::expect_sqlstate(
                connection,
                "LOAD 'bioseq'",
                "42710",
                30s);
        CHECK(
            observed.message.find(
                "Bioseq cache-provider rendezvous is already owned")
            != std::string::npos);
        CHECK(observed.severity == "ERROR");
        bioseq::postgres_test::record_migration_probe("combined.provider_rendezvous.owned_directory_collision_fatal");
    }
}
