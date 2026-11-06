/**
 * @file file_kind.cppm
 * @brief Classifies FASTA, FASTQ, SAM, BAM and compression path suffixes.
 */
module;
#include <array>
#include <cctype>
#include <cstddef>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
export module bioseq.io.file_kind;

import bioseq.common.logging;
import bioseq.io.options; // sequence_file_format

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::io {

/// Sequence format and compression framing inferred from the filename.
/// Detection uses none when there is no compression suffix; output resolution
/// additionally applies explicit options and the mandatory BAM BGZF framing.
/// The reader checks the file's gzip magic bytes separately when opening it.
struct sequence_file_kind {
    sequence_file_format format;
    sequence_compression compression{sequence_compression::none};
};

namespace detail {
/// Converts ASCII uppercase letters to lowercase without using the locale
/// Filename and identifier matcing share this rule, including compile-time checks.
[[nodiscard]] inline constexpr char lowercase_ascii_character(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

/// Returns a copy  with ASCII uppercase letters changed to lowercase.
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
inline constexpr std::array<file_format_suffix, 9> format_suffixes{{
    {".sam", sequence_file_format::sam},
    {".bam", sequence_file_format::bam},
    {".fasta", sequence_file_format::fasta},
    {".fa", sequence_file_format::fasta},
    {".fna", sequence_file_format::fasta},
    {".faa", sequence_file_format::fasta},
    {".fastq", sequence_file_format::fastq},
    {".fq", sequence_file_format::fastq},
    {".fnq", sequence_file_format::fastq},
}};
} // namespace detail

/// Detects a supported sequence/alignment format from a case-insensitive suffix.
/// An explicit format overrides the content suffix, including unknown extensions.
/// Compression suffixes are still detected. File contents are not read.
[[nodiscard]] inline std::expected<sequence_file_kind, std::string>
detect_sequence_file_kind(std::string_view path,
        std::optional<sequence_file_format> explicit_format = std::nullopt) {
    const std::string lowered = detail::lowercase_ascii_copy(path);
    std::string_view stem{lowered};

    auto compression = sequence_compression::none;
    for (const std::string_view gz : detail::gzip_suffixes) {
        if (stem.size() > gz.size() && stem.ends_with(gz)) {
            stem.remove_suffix(gz.size());
            compression = gz == ".gz" ? sequence_compression::gzip : sequence_compression::bgzf;
            break;
        }
    }
    if (explicit_format) return sequence_file_kind{*explicit_format, compression};
    for (const auto& [suffix, format] : detail::format_suffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            return sequence_file_kind{format, compression};
        }
    }
    return std::unexpected(
        fmt::format("unrecognised sequence file extension for path '{}': expected one of "
                    ".fasta/.fa/.fna/.faa/.fastq/.fq/.fnq/.sam/.bam (optionally .gz/.bgz/.bgzf)",
                    path));
}

/** Resolve output format once through the shared suffix tables before executing a source.
 * Explicit compression must agree with a recognized compression suffix. BAM
 * defaults to BGZF when unsuffixed and rejects a gzip suffix/request; .bam.gz is
 * deliberately rejected instead of silently replacing its requested framing.
 */
[[nodiscard]] inline std::expected<sequence_file_kind, std::string> resolve_output_format(
        std::string_view path, const writer_options& options) {
    const auto detected = detect_sequence_file_kind(path, options.format);
    if (!detected) return std::unexpected(detected.error());
    auto result = *detected;
    if (options.compression) {
        if (result.compression != sequence_compression::none
            && result.compression != *options.compression)
            return std::unexpected(std::string{"explicit compression disagrees with the output filename suffix"});
        result.compression = *options.compression;
    } else if (result.format == sequence_file_format::bam
            && result.compression == sequence_compression::none) {
        result.compression = sequence_compression::bgzf;
    }
    if (result.format == sequence_file_format::bam && result.compression != sequence_compression::bgzf)
        return std::unexpected(std::string{"BAM output requires BGZF framing; use .bam or .bam.bgz/.bam.bgzf, not .bam.gz"});
    if (options.compression_level < 0 || options.compression_level > 9)
        return std::unexpected(std::string{"compression_level must be between 0 and 9"});
    if (options.chunk_capacity_bytes == 0u)
        return std::unexpected(std::string{"writer chunk capacity must be positive"});
    return result;
}

} // namespace bioseq::io
