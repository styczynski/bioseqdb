/**
 * @file index.cppm
 * @brief Checks HTTP header text, DNS hosts and request-target bytes.
 */
module;

#include <algorithm>
#include <cstddef>
#include <string_view>

export module bioseq.common.http.validation;

export namespace bioseq::common::http::validation {

/**
 * @brief Rejects control bytes, spaces, non-ASCII bytes and backslashes.
 * @param[in] character Request-target byte to check.
 * @return True, if the byte is forbidden.
 */
[[nodiscard]] inline bool
is_invalid_request_target_byte(char character) noexcept {
  const auto value = static_cast<unsigned char>(character);
  return value <= 0x20 || value >= 0x7f || character == '\\';
}

/**
 * @brief Checks that a header name is a nonempty HTTP tokn.
 *
 * Accepts ASCII letters, digits and the punctuation allowed in field names.
 */
[[nodiscard]] inline bool is_valid_header_name(std::string_view name) noexcept {
  if (name.empty())
    return false;
  return std::ranges::all_of(name, [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') ||
           std::string_view{"!#$%&'*+-.^_`|~"}.contains(character);
  });
}

/**
 * @brief Rejects control bytes in a header value, except  horizontal tabs.
 *
 * Spaces and bytes above ASCII are accepted. An empty value is valid.
 */
[[nodiscard]] inline bool
is_valid_header_value(std::string_view value) noexcept {
  return std::ranges::all_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= 0x20 && byte != 0x7f) || character == '\t';
  });
}

/**
 * @brief Checks the ASCII hostname form accepted by the TLS client.
 *
 * Checks label lengths and characters, allowing a trailing root dot.
 * Rejects dotted names made entirely of digits to exclude IPv4-style input.
 */
[[nodiscard]] inline bool is_valid_dns_host(std::string_view host) noexcept {
  if (host.empty())
    return false;
  if (host.back() == '.')
    host.remove_suffix(1);
  if (host.empty() || host.size() > 253)
    return false;
  const auto is_alphanumeric = [](char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9');
  };
  bool looks_like_ipv4 = host.contains('.');
  std::size_t begin = 0;
  while (begin < host.size()) {
    const auto end = host.find('.', begin);
    const std::string_view label = host.substr(begin, end - begin);
    if (label.empty() || label.size() > 63 || !is_alphanumeric(label.front()) ||
        !is_alphanumeric(label.back()) ||
        std::ranges::any_of(label, [is_alphanumeric](char character) {
          return !is_alphanumeric(character) && character != '-';
        })) {
      return false;
    }
    looks_like_ipv4 =
        looks_like_ipv4 && std::ranges::all_of(label, [](char character) {
          return character >= '0' && character <= '9';
        });
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return !looks_like_ipv4;
}

/**
 * @brief Checks a request target for forbidden bytes and a raw fragment marker
 *
 * Percent-encoded bytes are left unchanged. This check does not require
 * a leading slash or a nonempty target.
 */
[[nodiscard]] inline bool
is_valid_request_target(std::string_view target) noexcept {
  return !target.contains('#') &&
         std::ranges::none_of(target, is_invalid_request_target_byte);
}

} // namespace bioseq::common::http::validation
