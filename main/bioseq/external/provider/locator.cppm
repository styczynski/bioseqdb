module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

export module bioseq.external.provider.locator;

import bioseq.external.provider.base;      // provider_id, locator_policy_id
import bioseq.external.provider.errors;    // external_error
import bioseq.external.provider.messages;  // locator_scheme

// DOC0372 — locator security policy profiles. Provider core emits candidate
// transfer requests plus a static locator_policy_id; the configured pack builds
// one immutable registry. The concrete transfer service resolves profiles
// through the registry and contains no provider/hostname switch.
export namespace bioseq::external::provider {

struct locator_host_suffix_view {
    std::string_view suffix;
    bool include_apex;
};

struct locator_port_view {
    locator_scheme scheme;
    std::uint16_t port;
};

enum class locator_address_scope : std::uint8_t {
    public_only,
    test_loopback
};

enum class locator_proxy_policy : std::uint8_t {
    disabled,
    explicit_pinned_connect
};

struct locator_policy_profile_view {
    locator_policy_id id;
    std::span<const locator_scheme> schemes;
    std::span<const std::string_view> exact_hosts;
    std::span<const locator_host_suffix_view> host_suffixes;
    std::span<const locator_port_view> ports;
    std::size_t maximum_url_bytes;
    std::size_t maximum_header_bytes;
    std::uint32_t maximum_redirects;
    bool permit_https_to_http_downgrade;
    locator_address_scope address_scope;
    locator_proxy_policy proxy;
};

struct locator_policy_registry_ref {
    void* object{};
    std::expected<locator_policy_profile_view, external_error> (*resolve_fn)(
        void*, provider_id, locator_policy_id){};

    [[nodiscard]] std::expected<locator_policy_profile_view, external_error>
    resolve(provider_id provider, locator_policy_id policy) const {
        return resolve_fn(object, provider, policy);
    }
};

}  // namespace bioseq::external::provider
