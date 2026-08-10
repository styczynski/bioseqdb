module;

// Lightweight source protocol. Holds only `source_read_result` and the
// `io_source` concept so a generated client or external
// transfer can depend on the byte-source protocol WITHOUT pulling the whole
// sequences/alphabets graph. Imports nothing project-side (spec §2774);
// `bioseq.io.sources` re-exports it so existing importers are unaffected.

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>

export module bioseq.io.source;

export namespace bioseq::io {

/// Result of one `read_into` call on a source.
struct source_read_result {
    std::size_t bytes_read = 0;
    bool        is_eof = false;
    bool        has_error = false;
};

/// Concept: an I/O source supplies bytes via `read_into(dst, cap)` and
/// surfaces errors via `last_error_message()`. Sources are monomorphic
/// per template instantiation; no virtuals.
template <typename p_source>
concept io_source = requires(p_source& source, std::uint8_t* dst, std::size_t cap) {
    { source.read_into(dst, cap) } -> std::same_as<source_read_result>;
    { source.last_error_message() } -> std::convertible_to<std::string>;
};

}  // namespace bioseq::io
