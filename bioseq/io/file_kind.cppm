/**
 * @file file_kind.cppm
 * @brief Classifies FASTA, FASTQ, and gzip path suffixes.
 */
module;
#include <array>
#include <cctype>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
export module bioseq.io.file_kind;

import bioseq.common.logging;
import bioseq.io.options; // sequence_file_format

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::io {

/// Sequence format inferred from the filename, plus a gzip suffix flag.
/// The reader checks the file's gzip magic bytes separately when opening it.
struct sequence_file_kind {
    sequence_file_format format;
    bool is_gzip_suffix;
};

namespace detail {
/// Converts ASCII uppercase letters to lowercase without using the locale.
/// Filename and identifier matching share this rule, including compile-time checks.
[[nodiscard]] inline constexpr char lowercase_ascii_character(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Returns a copy with ASCII uppercase letters changed to lowercase.
[[nodiscard]] inline std::string lowercase_ascii_copy(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = lowercase_ascii_character(c);
    return out;
}

/// Compression suffixes removed before matching the sequence-file extension.
inline constexpr std::array<std::string_view, 3> gzip_suffixes{".gz", ".bgz", ".bgzf"};

/// One recognized extension and its sequence format.
struct file_format_suffix {
    std::string_view suffix;
    sequence_file_format format;
};
/// Sequence-file suffixes checked after removing an optional compression suffix.
inline constexpr std::array<file_format_suffix, 6> format_suffixes{{
    {".fasta", sequence_file_format::fasta},
    {".fa", sequence_file_format::fasta},
    {".fna", sequence_file_format::fasta},
    {".fastq", sequence_file_format::fastq},
    {".fq", sequence_file_format::fastq},
    {".fnq", sequence_file_format::fastq},
}};
} // namespace detail

/// Detects FASTA or FASTQ from a case-insensitive filename extension.
/// Returns a diagnostic for an unknown extension. File contents are not read.
[[nodiscard]] inline std::expected<sequence_file_kind, std::string>
detect_sequence_file_kind(std::string_view path) {
    const std::string lowered = detail::lowercase_ascii_copy(path);
    std::string_view stem{lowered};

    bool is_gzip = false;
    for (const std::string_view gz : detail::gzip_suffixes) {
        if (stem.size() > gz.size() && stem.ends_with(gz)) {
            stem.remove_suffix(gz.size());
            is_gzip = true;
            break;
        }
    }
    for (const auto& [suffix, format] : detail::format_suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            return sequence_file_kind{.format = format, .is_gzip_suffix = is_gzip};
        }
    }
    return std::unexpected(
        fmt::format("unrecognised sequence file extension for path '{}': expected one of "
                    ".fasta/.fa/.fna/.fastq/.fq/.fnq (optionally .gz/.bgz/.bgzf)",
                    path));
}

} // namespace bioseq::io
