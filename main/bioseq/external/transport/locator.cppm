module;

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

export module bioseq.external.transport.locator;

import bioseq.external.provider.base;      // provider_id, external_error key types
import bioseq.external.provider.errors;    // external_error, make_external_error
import bioseq.external.provider.messages;  // candidate_transfer_request_view, locator_scheme
import bioseq.external.provider.locator;   // locator_policy_profile_view, scopes

// DOC0372 — locator security (SSRF firewall) for the libcurl data plane.
//
// Provider core emits candidate transfer requests plus a locator policy; this
// module is the sole constructor of a `validated_transfer_request`. Every
// candidate is checked against the provider's declared policy BEFORE a socket is
// opened: scheme allowlist, exact-host / host-suffix allowlist, URL/host length
// caps, and DNS resolution whose every returned address must be a permitted
// (public, unless a test-only loopback policy is injected) address. The resolved
// addresses are pinned onto the validated request so libcurl connects only to
// the exact numeric peers approved here — closing DNS-rebinding and
// redirect-to-private gaps. The concrete transfer service resolves policy through
// this validator and contains no provider/hostname switch.
export namespace bioseq::external::transport {

namespace prov = bioseq::external::provider;

// One approved numeric peer for CURLOPT_RESOLVE pinning. `family` is AF_INET or
// AF_INET6; `numeric` is the textual form (e.g. "93.184.216.34").
struct resolved_address {
    int family{};
    std::string numeric;
};

// The only owning result of candidate validation. It contains the normalized URL
// components, the request method, and the pinned resolved address set. A remote
// byte source consumes this by move for its synchronous lifetime; there is no
// second-validation bypass and no raw-string streaming entry point.
class validated_transfer_request {
public:
    // A default-constructed request is empty (no URL/host/addresses) and cannot
    // connect anywhere; `validate_candidate` is the only producer of a populated,
    // security-checked request. Public so owning sources can move-hold it.
    validated_transfer_request() = default;
    validated_transfer_request(const validated_transfer_request&) = delete;
    validated_transfer_request& operator=(const validated_transfer_request&) =
        delete;
    validated_transfer_request(validated_transfer_request&&) noexcept = default;
    validated_transfer_request& operator=(
        validated_transfer_request&&) noexcept = default;
    ~validated_transfer_request() = default;

    [[nodiscard]] prov::transfer_method method() const noexcept { return method_; }
    [[nodiscard]] std::string_view url() const noexcept { return url_; }
    [[nodiscard]] std::string_view host() const noexcept { return host_; }
    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }
    [[nodiscard]] prov::locator_scheme scheme() const noexcept { return scheme_; }
    [[nodiscard]] const std::vector<resolved_address>& pinned_addresses()
        const noexcept {
        return pinned_addresses_;
    }
    // "host:port:addr,addr,..." for CURLOPT_RESOLVE (empty when nothing pinned).
    [[nodiscard]] std::string curl_resolve_entry() const {
        if (pinned_addresses_.empty()) {
            return {};
        }
        std::string out;
        out.reserve(host_.size() + 8 + pinned_addresses_.size() * 40);
        out.append(host_);
        out.push_back(':');
        out.append(std::to_string(port_));
        out.push_back(':');
        for (std::size_t i = 0; i < pinned_addresses_.size(); ++i) {
            if (i != 0) {
                out.push_back(',');
            }
            out.append(pinned_addresses_[i].numeric);
        }
        return out;
    }

private:
    friend std::expected<validated_transfer_request, prov::external_error>
    validate_candidate(
        const prov::candidate_transfer_request_view&,
        const prov::locator_policy_profile_view&,
        prov::provider_id);

    prov::transfer_method method_{};
    prov::locator_scheme scheme_{};
    std::string url_;
    std::string host_;
    std::uint16_t port_{};
    std::vector<resolved_address> pinned_addresses_;
};

// ---------------------------------------------------------------------------
// URL parsing (scheme://host[:port]/path) — no user-info accepted.
// ---------------------------------------------------------------------------
struct parsed_url {
    prov::locator_scheme scheme{};
    std::string host;
    std::uint16_t port{};
    bool has_userinfo{};
};

[[nodiscard]] inline std::expected<parsed_url, prov::external_error> parse_url(
    std::string_view url, prov::provider_id provider) {
    parsed_url out;
    const std::size_t scheme_end = url.find("://");
    if (scheme_end == std::string_view::npos) {
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::invalid_query, provider, "locator",
            "transfer URL has no scheme"));
    }
    const std::string_view scheme = url.substr(0, scheme_end);
    std::uint16_t default_port = 0;
    if (scheme == "https") {
        out.scheme = prov::locator_scheme::https;
        default_port = 443;
    } else if (scheme == "http") {
        out.scheme = prov::locator_scheme::http;
        default_port = 80;
    } else if (scheme == "ftps") {
        out.scheme = prov::locator_scheme::ftps;
        default_port = 990;
    } else if (scheme == "ftp") {
        out.scheme = prov::locator_scheme::ftp;
        default_port = 21;
    } else {
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::invalid_query, provider, "locator",
            "transfer URL scheme is not permitted"));
    }
    std::string_view authority = url.substr(scheme_end + 3);
    const std::size_t path_start = authority.find_first_of("/?#");
    if (path_start != std::string_view::npos) {
        authority = authority.substr(0, path_start);
    }
    if (authority.find('@') != std::string_view::npos) {
        out.has_userinfo = true;
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::invalid_query, provider, "locator",
            "transfer URL must not contain user-info"));
    }
    // host[:port]; IPv6 literals are rejected (providers use DNS names).
    std::uint16_t port = default_port;
    std::string_view host = authority;
    const std::size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos &&
        authority.find(']') == std::string_view::npos) {
        host = authority.substr(0, colon);
        const std::string_view port_text = authority.substr(colon + 1);
        std::uint32_t value = 0;
        for (const char digit : port_text) {
            if (digit < '0' || digit > '9') {
                return std::unexpected(prov::make_external_error(
                    prov::external_error_kind::invalid_query, provider,
                    "locator", "transfer URL has a malformed port"));
            }
            value = value * 10 + static_cast<std::uint32_t>(digit - '0');
            if (value > 65535) {
                return std::unexpected(prov::make_external_error(
                    prov::external_error_kind::invalid_query, provider,
                    "locator", "transfer URL port out of range"));
            }
        }
        port = static_cast<std::uint16_t>(value);
    }
    if (host.empty()) {
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::invalid_query, provider, "locator",
            "transfer URL has an empty host"));
    }
    out.host.assign(host);
    out.port = port;
    return out;
}

// ---------------------------------------------------------------------------
// Host-allowlist matching against the policy's exact hosts and suffixes.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool host_is_allowed(
    std::string_view host, const prov::locator_policy_profile_view& policy) {
    for (const std::string_view exact : policy.exact_hosts) {
        if (host == exact) {
            return true;
        }
    }
    for (const prov::locator_host_suffix_view& suffix : policy.host_suffixes) {
        if (host.size() > suffix.suffix.size() &&
            host.ends_with(suffix.suffix) &&
            host[host.size() - suffix.suffix.size() - 1] == '.') {
            return true;
        }
        if (suffix.include_apex && host == suffix.suffix) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] inline bool scheme_is_allowed(
    prov::locator_scheme scheme,
    const prov::locator_policy_profile_view& policy) {
    for (const prov::locator_scheme candidate : policy.schemes) {
        if (candidate == scheme) {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Address-scope classification. A public address is anything that is not
// loopback, private, link-local, multicast, unspecified, or otherwise reserved.
// ---------------------------------------------------------------------------
[[nodiscard]] inline bool ipv4_is_public(std::uint32_t host_order) noexcept {
    const std::uint8_t a = static_cast<std::uint8_t>(host_order >> 24);
    const std::uint8_t b = static_cast<std::uint8_t>(host_order >> 16);
    if (a == 0) return false;                       // 0.0.0.0/8 unspecified
    if (a == 10) return false;                      // 10.0.0.0/8 private
    if (a == 127) return false;                     // 127.0.0.0/8 loopback
    if (a == 169 && b == 254) return false;         // 169.254.0.0/16 link-local
    if (a == 172 && (b >= 16 && b <= 31)) return false;  // 172.16.0.0/12
    if (a == 192 && b == 168) return false;         // 192.168.0.0/16 private
    if (a == 100 && (b >= 64 && b <= 127)) return false;  // 100.64.0.0/10 CGNAT
    if (a >= 224) return false;                     // 224.0.0.0/4 multicast +
    return true;
}

[[nodiscard]] inline bool ipv6_is_public(const std::uint8_t bytes[16]) noexcept {
    // Unspecified ::.
    bool all_zero = true;
    for (int i = 0; i < 16; ++i) {
        if (bytes[i] != 0) {
            all_zero = false;
            break;
        }
    }
    if (all_zero) return false;
    // Loopback ::1.
    bool loopback = bytes[15] == 1;
    for (int i = 0; i < 15 && loopback; ++i) {
        if (bytes[i] != 0) loopback = false;
    }
    if (loopback) return false;
    if (bytes[0] == 0xFE && (bytes[1] & 0xC0) == 0x80) return false;  // fe80::/10
    if ((bytes[0] & 0xFE) == 0xFC) return false;                     // fc00::/7 ULA
    if (bytes[0] == 0xFF) return false;                              // ff00::/8 mcast
    return true;
}

// ---------------------------------------------------------------------------
// Full candidate validation. Resolves the host and requires every returned
// address to satisfy the policy address scope; pins them for connect.
// ---------------------------------------------------------------------------
[[nodiscard]] inline std::expected<validated_transfer_request, prov::external_error>
validate_candidate(const prov::candidate_transfer_request_view& candidate,
                   const prov::locator_policy_profile_view& policy,
                   prov::provider_id provider) {
    if (candidate.url.size() > policy.maximum_url_bytes) {
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::invalid_query, provider, "locator",
            "transfer URL exceeds the policy length cap"));
    }
    auto parsed = parse_url(candidate.url, provider);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (!scheme_is_allowed(parsed->scheme, policy)) {
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::invalid_query, provider, "locator",
            "transfer scheme not allowed by policy"));
    }
    if (!host_is_allowed(parsed->host, policy)) {
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::invalid_query, provider, "locator",
            "transfer host not on the provider allowlist"));
    }

    const bool allow_private =
        policy.address_scope == prov::locator_address_scope::test_loopback;

    ::addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo* results = nullptr;
    const std::string port_text = std::to_string(parsed->port);
    const int rc =
        ::getaddrinfo(parsed->host.c_str(), port_text.c_str(), &hints, &results);
    if (rc != 0 || results == nullptr) {
        if (results != nullptr) {
            ::freeaddrinfo(results);
        }
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::transport, provider, "locator",
            "transfer host DNS resolution failed"));
    }

    validated_transfer_request out;
    out.method_ = candidate.method;
    out.scheme_ = parsed->scheme;
    out.url_.assign(candidate.url);
    out.host_ = parsed->host;
    out.port_ = parsed->port;

    for (::addrinfo* it = results; it != nullptr; it = it->ai_next) {
        char numeric[INET6_ADDRSTRLEN] = {};
        if (it->ai_family == AF_INET) {
            auto* sin = reinterpret_cast<::sockaddr_in*>(it->ai_addr);
            const std::uint32_t host_order = ntohl(sin->sin_addr.s_addr);
            if (!allow_private && !ipv4_is_public(host_order)) {
                ::freeaddrinfo(results);
                return std::unexpected(prov::make_external_error(
                    prov::external_error_kind::invalid_query, provider,
                    "locator",
                    "transfer host resolved to a non-public address"));
            }
            ::inet_ntop(AF_INET, &sin->sin_addr, numeric, sizeof(numeric));
            out.pinned_addresses_.push_back(
                resolved_address{AF_INET, std::string(numeric)});
        } else if (it->ai_family == AF_INET6) {
            auto* sin6 = reinterpret_cast<::sockaddr_in6*>(it->ai_addr);
            if (!allow_private &&
                !ipv6_is_public(sin6->sin6_addr.s6_addr)) {
                ::freeaddrinfo(results);
                return std::unexpected(prov::make_external_error(
                    prov::external_error_kind::invalid_query, provider,
                    "locator",
                    "transfer host resolved to a non-public address"));
            }
            ::inet_ntop(AF_INET6, &sin6->sin6_addr, numeric, sizeof(numeric));
            out.pinned_addresses_.push_back(
                resolved_address{AF_INET6, std::string(numeric)});
        }
    }
    ::freeaddrinfo(results);

    if (out.pinned_addresses_.empty()) {
        return std::unexpected(prov::make_external_error(
            prov::external_error_kind::transport, provider, "locator",
            "transfer host produced no usable addresses"));
    }
    return out;
}

}  // namespace bioseq::external::transport
