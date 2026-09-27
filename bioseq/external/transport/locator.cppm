/**
 * @file external/transport/locator.cppm
 * @brief Check download URLs and resolve addresses for the transfer
 */
module;

#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <vector>

export module bioseq.external.transport.locator;

import bioseq.external.provider.base;     // provider_id
import bioseq.external.provider.errors;   // external_error, make_external_error
import bioseq.external.provider.messages; // candidate_transfer_request_view, locator_scheme
import bioseq.external.provider.locator;  // locator_policy_profile_view, scopes

/// Check download destinations and save resolved addresses for libcurl.
export namespace bioseq::external::transport {

namespace prov = bioseq::external::provider;

/// Resolved address saved for CURLOPT_RESOLVE.
struct resolved_address {
    int family{};
    std::string numeric;
};

/// Download URL and resolved addresses returned by validation.
class validated_transfer_request {
  public:
    /// Constructs an empty request with no approved destination.
    validated_transfer_request() = default;
    validated_transfer_request(const validated_transfer_request&) = delete;
    validated_transfer_request& operator=(const validated_transfer_request&) = delete;
    validated_transfer_request(validated_transfer_request&&) noexcept = default;
    validated_transfer_request& operator=(validated_transfer_request&&) noexcept = default;
    ~validated_transfer_request() = default;

    /// Return the requested transfer method.
    [[nodiscard]] prov::transfer_method method() const noexcept {
        return method_;
    }
    /// Return  the URL supplied to validation.
    [[nodiscard]] std::string_view url() const noexcept {
        return url_;
    }
    /// Return the host as written in the URL.
    [[nodiscard]] std::string_view host() const noexcept {
        return host_;
    }
    /// Return the parsed port or the scheme's default port.
    [[nodiscard]] std::uint16_t port() const noexcept {
        return port_;
    }
    /// Return the URL scheme, accepted by the policy.
    [[nodiscard]] prov::locator_scheme scheme() const noexcept {
        return scheme_;
    }
    /// Return the addresses accepted by the address filter.
    [[nodiscard]] const std::vector<resolved_address>& pinned_addresses() const noexcept {
        return pinned_addresses_;
    }
    /// Returns the CURLOPT_RESOLVE entry or an empty string when unpinned.
    [[nodiscard]] std::string curl_resolve_entry() const {
        if (pinned_addresses_.empty()) {
            return {};
        }
        std::string result;
        result.reserve(host_.size() + 8 + pinned_addresses_.size() * 40);
        result.append(host_);
        result.push_back(':');
        result.append(std::to_string(port_));
        result.push_back(':');
        for (std::size_t i = 0; i < pinned_addresses_.size(); ++i) {
            if (i != 0) {
                result.push_back(',');
            }
            result.append(pinned_addresses_[i].numeric);
        }
        return result;
    }

  private:
    friend std::expected<validated_transfer_request, prov::external_error>
    validate_candidate(const prov::candidate_transfer_request_view&,
                       const prov::locator_policy_profile_view&,
                       prov::provider_id);

    prov::transfer_method method_{};
    prov::locator_scheme scheme_{};
    std::string url_;
    std::string host_;
    std::uint16_t port_{};
    std::vector<resolved_address> pinned_addresses_;
};

// Parse scheme://host[:port]/path and reject embedded user information.
/// Parsed transfer URL fields used during validation.
struct parsed_url {
    prov::locator_scheme scheme{};
    std::string host;
    std::uint16_t port{};
    bool has_userinfo{};
};

/// Parses a supported URL without opening a connection.
[[nodiscard]] inline std::expected<parsed_url, prov::external_error>
parse_url(std::string_view url, prov::provider_id provider) {
    parsed_url result;
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected(prov::make_external_error(prov::external_error_kind::invalid_query,
                                                         provider,
                                                         "locator",
                                                         "transfer URL has no scheme"));
    }
    const std::string_view scheme = url.substr(0, scheme_end);
    std::uint16_t default_port = 0;
    if (scheme == "https") {
        result.scheme = prov::locator_scheme::https;
        default_port = 443;
    } else if (scheme == "http") {
        result.scheme = prov::locator_scheme::http;
        default_port = 80;
    } else if (scheme == "ftps") {
        result.scheme = prov::locator_scheme::ftps;
        default_port = 990;
    } else if (scheme == "ftp") {
        result.scheme = prov::locator_scheme::ftp;
        default_port = 21;
    } else {
        return std::unexpected(prov::make_external_error(prov::external_error_kind::invalid_query,
                                                         provider,
                                                         "locator",
                                                         "transfer URL scheme is not permitted"));
    }
    std::string_view authority = url.substr(scheme_end + 3);
    const std::size_t path_start = authority.find_first_of("/?#");
    if (path_start != std::string_view::npos) {
        authority = authority.substr(0, path_start);
    }
    if (authority.find('@') != std::string_view::npos) {
        result.has_userinfo = true;
        return std::unexpected(
            prov::make_external_error(prov::external_error_kind::invalid_query,
                                      provider,
                                      "locator",
                                      "transfer URL must not contain user-info"));
    }
    // Read an optional port from a hostname authority.
    std::uint16_t port = default_port;
    std::string_view host = authority;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos && authority.find(']') == std::string_view::npos) {
        host = authority.substr(0, colon);
        const std::string_view port_text = authority.substr(colon + 1);
        std::uint32_t value = 0;
        for (const char digit : port_text) {
            if (digit < '0' || digit > '9') {
                return std::unexpected(
                    prov::make_external_error(prov::external_error_kind::invalid_query,
                                              provider,
                                              "locator",
                                              "transfer URL has a malformed port"));
            }
            value = value * 10 + static_cast<std::uint32_t>(digit - '0');
            if (value > 65535) {
                return std::unexpected(
                    prov::make_external_error(prov::external_error_kind::invalid_query,
                                              provider,
                                              "locator",
                                              "transfer URL port result of range"));
            }
        }
        port = static_cast<std::uint16_t>(value);
    }
    if (host.empty()) {
        return std::unexpected(prov::make_external_error(prov::external_error_kind::invalid_query,
                                                         provider,
                                                         "locator",
                                                         "transfer URL has an empty host"));
    }
    result.host.assign(host);
    result.port = port;
    return result;
}

// Match the host against exact names and allowed DNS suffixes.
/// Check an exact host or DNS suffix match, preserving letter case.
[[nodiscard]] inline bool is_host_allowed(std::string_view host,
                                          const prov::locator_policy_profile_view& policy) {
    for (const std::string_view exact : policy.exact_hosts) {
        if (host == exact) {
            return true;
        }
    }
    for (const prov::locator_host_suffix_view& suffix : policy.host_suffixes) {
        if (host.size() > suffix.suffix.size() && host.ends_with(suffix.suffix) &&
            host[host.size() - suffix.suffix.size() - 1] == '.') {
            return true;
        }
        if (suffix.include_apex && host == suffix.suffix) {
            return true;
        }
    }
    return false;
}

/// Returns whether a scheme appears in the locator policy.
[[nodiscard]] inline bool is_scheme_allowed(prov::locator_scheme scheme,
                                            const prov::locator_policy_profile_view& policy) {
    for (const prov::locator_scheme candidate : policy.schemes) {
        if (candidate == scheme) {
            return true;
        }
    }
    return false;
}

// Address filters used for download destinations.
/// Check an IPv4 addres against the  blocked ranges below.
[[nodiscard]] inline bool is_public_ipv4(std::uint32_t host_order) noexcept {
    const std::uint8_t a = static_cast<std::uint8_t>(host_order >> 24);
    const std::uint8_t b = static_cast<std::uint8_t>(host_order >> 16);
    if (a == 0)
        return false; // 0.0.0.0/8
    if (a == 10)
        return false; // 10.0.0.0/8 private
    if (a == 127)
        return false; // 127.0.0.0/8 loopback
    if (a == 169 && b == 254)
        return false; // 169.254.0.0/16 link-local
    if (a == 172 && (b >= 16 && b <= 31))
        return false; // 172.16.0.0/12
    if (a == 192 && b == 168)
        return false; // 192.168.0.0/16 private
    if (a == 100 && (b >= 64 && b <= 127))
        return false; // 100.64.0.0/10 shared address space
    if (a >= 224)
        return false; // Multicast and higher reserved ranges, 224.0.0.0/3.
    return true;
}

/// Check an IPv6 address against the blocked ranges below.
[[nodiscard]] inline bool is_public_ipv6(const std::uint8_t bytes[16]) noexcept {
    // Unspecified ::.
    bool all_zero = true;
    for (int i = 0; i < 16; ++i) {
        if (bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero)
        return false;
    // Loopback ::1.
    bool loopback = bytes[15] == 1;
    for (int i = 0; i < 15 && loopback; ++i) {
        if (bytes[i] != 0)
            loopback = false;
    }
    if (loopback)
        return false;
    if (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80)
        return false; // fe80::/10 link-local
    if ((bytes[0] & 0xFE) == 0xFC)
        return false; // fc00::/7 unique local addresses
    if (bytes[0] == 0xFF)
        return false; // ff00::/8 multicast
    return true;
}

// Resolve the host once and save the accepted addresses for connection.
// Reject the request if any resolved address fails the selected filter.
/// Check URL length, scheme, and host, then resolve and filter addresses.
/// The test_loopback policy skips address  filtering.
[[nodiscard]] inline std::expected<validated_transfer_request, prov::external_error>
validate_candidate(const prov::candidate_transfer_request_view& candidate,
                   const prov::locator_policy_profile_view& policy,
                   prov::provider_id provider) {
    if (candidate.url.size() > policy.maximum_url_bytes) {
        return std::unexpected(
            prov::make_external_error(prov::external_error_kind::invalid_query,
                                      provider,
                                      "locator",
                                      "transfer URL exceeds the policy length cap"));
    }
    auto parsed_url_result = parse_url(candidate.url, provider);
    if (!parsed_url_result.has_value()) {
        return std::unexpected(parsed_url_result.error());
    }
    if (!is_scheme_allowed(parsed_url_result->scheme, policy)) {
        return std::unexpected(prov::make_external_error(prov::external_error_kind::invalid_query,
                                                         provider,
                                                         "locator",
                                                         "transfer scheme not allowed by policy"));
    }
    if (!is_host_allowed(parsed_url_result->host, policy)) {
        return std::unexpected(
            prov::make_external_error(prov::external_error_kind::invalid_query,
                                      provider,
                                      "locator",
                                      "transfer host not on the provider allowlist"));
    }

    const bool allow_private = policy.address_scope == prov::locator_address_scope::test_loopback;

    ::addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo* results = nullptr;
    const std::string port_text = std::to_string(parsed_url_result->port);
    const int resolution_status =
        ::getaddrinfo(parsed_url_result->host.c_str(), port_text.c_str(), &hints, &results);
    if (resolution_status != 0 || results == nullptr) {
        if (results != nullptr) {
            ::freeaddrinfo(results);
        }
        return std::unexpected(prov::make_external_error(prov::external_error_kind::transport,
                                                         provider,
                                                         "locator",
                                                         "transfer host DNS resolution failed"));
    }

    validated_transfer_request validated_request;
    validated_request.method_ = candidate.method;
    validated_request.scheme_ = parsed_url_result->scheme;
    validated_request.url_.assign(candidate.url);
    validated_request.host_ = parsed_url_result->host;
    validated_request.port_ = parsed_url_result->port;

    for (::addrinfo* address_info = results; address_info != nullptr;
         address_info = address_info->ai_next) {
        std::array<char, INET6_ADDRSTRLEN> numeric_address{};
        if (address_info->ai_family == AF_INET) {
            auto* ipv4_address = reinterpret_cast<::sockaddr_in*>(address_info->ai_addr);
            const std::uint32_t host_order = ntohl(ipv4_address->sin_addr.s_addr);
            if (!allow_private && !is_public_ipv4(host_order)) {
                ::freeaddrinfo(results);
                return std::unexpected(
                    prov::make_external_error(prov::external_error_kind::invalid_query,
                                              provider,
                                              "locator",
                                              "transfer host resolved to a non-public address"));
            }
            ::inet_ntop(
                AF_INET, &ipv4_address->sin_addr, numeric_address.data(), numeric_address.size());
            validated_request.pinned_addresses_.push_back(
                resolved_address{AF_INET, std::string(numeric_address.data())});
        } else if (address_info->ai_family == AF_INET6) {
            auto* ipv6_address = reinterpret_cast<::sockaddr_in6*>(address_info->ai_addr);
            if (!allow_private && !is_public_ipv6(ipv6_address->sin6_addr.s6_addr)) {
                ::freeaddrinfo(results);
                return std::unexpected(
                    prov::make_external_error(prov::external_error_kind::invalid_query,
                                              provider,
                                              "locator",
                                              "transfer host resolved to a non-public address"));
            }
            ::inet_ntop(
                AF_INET6, &ipv6_address->sin6_addr, numeric_address.data(), numeric_address.size());
            validated_request.pinned_addresses_.push_back(
                resolved_address{AF_INET6, std::string(numeric_address.data())});
        }
    }
    ::freeaddrinfo(results);

    if (validated_request.pinned_addresses_.empty()) {
        return std::unexpected(
            prov::make_external_error(prov::external_error_kind::transport,
                                      provider,
                                      "locator",
                                      "transfer host produced no usable addresses"));
    }
    return validated_request;
}

} // namespace bioseq::external::transport
