/**
 * @file external/provider/locator.cppm
 * @brief Rules for allowed download URLs and policy lookup callbacks.
 */
module;

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>

export module bioseq.external.provider.locator;

import bioseq.external.provider.base;     // provider_id, locator_policy_id
import bioseq.external.provider.errors;   // external_error
import bioseq.external.provider.messages; // locator_scheme

/// Rules for the hosts, addresses, and protocols a provider may contact.
// Providers attach a policy ID to each download plan. The transfer code
// can use this registry interface to look up that provider's URL rules.
export namespace bioseq::external::provider {

/// Allowed DNS suffix and whether the suffix itself is also allowed.
struct locator_host_suffix_view {
    std::string_view suffix;
    bool include_apex;
};

/// Allowed port for one locator scheme.
struct locator_port_view {
    locator_scheme scheme;
    std::uint16_t port;
};

/// Address classes a locator policy permits after DNS resolution.
enum class locator_address_scope : std::uint8_t { public_only, test_loopback };

/// Whether proxy routing is forbidden or explicitly pinned.
enum class locator_proxy_policy : std::uint8_t { disabled, explicit_pinned_connect };

/// Download URL rules with borrowed host and protocol lists.
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

/// Callback for looking up configured URL rules.
struct locator_policy_registry_ref {
    void* object{};
    std::expected<locator_policy_profile_view, external_error> (*resolve_fn)(void*,
                                                                             provider_id,
                                                                             locator_policy_id){};

    /// Look up a policy for the given provider.
    [[nodiscard]] std::expected<locator_policy_profile_view, external_error>
    resolve(provider_id provider, locator_policy_id policy) const {
        return resolve_fn(object, provider, policy);
    }
};

} // namespace bioseq::external::provider
