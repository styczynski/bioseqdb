/**
 * @file external/tests/transport/transfer_service_test.cpp
 * @brief Check transfer-service callbacks and passkey access.
 */

// Use a fake service to check callback forwarding without network requests.

#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <expected>
#include <type_traits>

import bioseq.external.transport.transfer_service;
import bioseq.external.transport.locator;   // validated_transfer_request
import bioseq.external.provider.base;        // provider_id, external_execution_key
import bioseq.external.provider.errors;      // external_error
import bioseq.external.provider.messages;    // candidate_transfer_request_view, transfer_method
import bioseq.external.provider.locator;     // locator_policy_profile_view

namespace {
namespace tx = bioseq::external::transport;
namespace prov = bioseq::external::provider;

// A fake transfer service that records dispatch without opening any socket.
struct spy_service {
    bool validated = false;
    bool stopped = false;

    std::expected<tx::validated_transfer_request, prov::external_error> validate(
        const tx::transfer_service_passkey& /*borrowed*/, prov::provider_id,
        const prov::locator_policy_profile_view&,
        const prov::candidate_transfer_request_view&) {
        validated = true;
        return tx::validated_transfer_request{};
    }
    prov::external_execution_key execution_key() const noexcept {
        prov::external_execution_key k{};
        k[0] = std::byte{0x7f};
        return k;
    }
    void request_stop() noexcept { stopped = true; }
};

// Check the service interface and restricted passkey construction.
static_assert(tx::external_transfer_service_compatible<spy_service>);
static_assert(tx::external_transfer_service_compatible<tx::curl_transfer_service>);
static_assert(!std::is_default_constructible_v<tx::transfer_service_passkey>,
              "only the erasure may mint a passkey");
static_assert(!std::is_copy_constructible_v<tx::transfer_service_passkey>,
              "a service must not retain a borrowed passkey");
}  // namespace

TEST_CASE("transfer_service_ref dispatches only through the erasure",
          "[component:external_api][layer:contract][feature:external_source]") {
    spy_service spy;
    tx::transfer_service_ref ref = tx::make_transfer_service_ref(spy);
    REQUIRE(ref.object == &spy);
    REQUIRE(static_cast<bool>(ref.validate_fn));
    REQUIRE(static_cast<bool>(ref.execution_key_fn));
    REQUIRE(static_cast<bool>(ref.request_stop_fn));

    prov::candidate_transfer_request_view candidate{};
    candidate.method = prov::transfer_method::get;
    candidate.url = "https://api.ncbi.nlm.nih.gov/probe";
    prov::provider_id provider{};
    prov::locator_policy_profile_view policy{};

    auto result = ref.validate_fn(ref.object, provider, policy, candidate);
    REQUIRE(spy.validated);
    REQUIRE(result.has_value());  // spy returns an empty validated request

    const auto key = ref.execution_key_fn(ref.object);
    REQUIRE(key[0] == std::byte{0x7f});

    REQUIRE_FALSE(spy.stopped);
    ref.request_stop_fn(ref.object);
    REQUIRE(spy.stopped);
}

TEST_CASE("a hand-rolled transfer_service_ref has null thunks",
          "[component:external_api][layer:contract][feature:external_source]") {
    tx::transfer_service_ref ref{};
    REQUIRE(ref.object == nullptr);
    REQUIRE_FALSE(static_cast<bool>(ref.validate_fn));
    REQUIRE_FALSE(static_cast<bool>(ref.execution_key_fn));
    REQUIRE_FALSE(static_cast<bool>(ref.request_stop_fn));
}
