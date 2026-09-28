/**
 * @file source.cppm
 * @brief Byte source result and protocol definitions.
 *
 * This module has no project imports. `bioseq.io.sources` re-exports it for
 * consumers that also need concrete source implementations.
 */
module;

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>

export module bioseq.io.source;

export namespace bioseq::io {

/// Result of one `read_into` call on a source.
struct source_read_result {
    std::size_t bytes_read = 0;
    bool is_eof = false;
    bool has_error = false;
};

/// Requires a byte source  with `read_into(dst, cap)` and `last_error_message()`.
template <typename p_source>
concept io_source = requires(p_source& source, std::uint8_t* dst, std::size_t cap) {
    { source.read_into(dst, cap) } -> std::same_as<source_read_result>;
    { source.last_error_message() } -> std::convertible_to<std::string>;
};

} // namespace bioseq::io
