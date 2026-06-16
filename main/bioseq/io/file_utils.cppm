// bioseq/io/file_utils.cppm
module;
#include "common.h"            // DEBUG_ASSERT, fmt
#include <array>
#include <cctype>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
export module bioseq.io.file_utils;

import bioseq.common.mlogging;                // fmt (DOC0145: common.h + mlogging for fmt::format)
import bioseq.io.options;       // sequence_io_file_type

static_assert(__cplusplus >= 202302L, "C++23-or-later");

export namespace bioseq::io {

/// Result of inspecting a path's *name* (not its contents). `format` is the
/// FASTA/FASTQ enum the reader must be instantiated with; `is_gzip_suffix` is
/// true when the (case-insensitive) path ends in a recognised gzip suffix and
/// is informational only — the reader independently magic-byte-sniffs gzip.
struct sequence_file_kind {
    sequence_io_file_type format;
    bool                  is_gzip_suffix;
};

namespace detail {
    /// Single source of truth for ASCII case folding (DOC0295 §4.2): the one
    /// per-byte rule shared by `ascii_lower` (path sniffing) and the identifier
    /// parser's `normalise_name` (column/format-name matching). `constexpr` so
    /// the parser can drive its `consteval` `norm_key` drift guards through the
    /// exact same lowering, with no locale dependence (column names are ASCII).
    [[nodiscard]] inline constexpr char to_ascii_lower(char c) noexcept {
        return (c >= 'A' && c <= 'Z')
            ? static_cast<char>(c - 'A' + 'a')
            : c;
    }

    /// Lower-case ASCII copy (paths are short; no SIMD needed).
    [[nodiscard]] inline std::string ascii_lower(std::string_view s) {
        std::string out(s);
        for (char& c : out) c = to_ascii_lower(c);
        return out;
    }

    /// Recognised gzip suffixes, longest-first so `.fastq.gz` strips before
    /// `.gz`. constexpr table — adding a suffix is a one-line edit.
    inline constexpr std::array<std::string_view, 3> kGzipSuffixes{
        ".gz", ".bgz", ".bgzf"};

    /// Extension → format table. The first entry whose suffix matches (after a
    /// gzip suffix is stripped) wins. constexpr so it lowers to a jump table.
    struct ext_entry { std::string_view suffix; sequence_io_file_type format; };
    inline constexpr std::array<ext_entry, 6> kFormatSuffixes{{
        {".fasta", sequence_io_file_type::FASTA},
        {".fa",    sequence_io_file_type::FASTA},
        {".fna",   sequence_io_file_type::FASTA},
        {".fastq", sequence_io_file_type::FASTQ},
        {".fq",    sequence_io_file_type::FASTQ},
        {".fnq",   sequence_io_file_type::FASTQ},
    }};
}  // namespace detail

/// Map a file path to its sequence file kind by extension. Returns an error
/// string (never throws) when the extension is unrecognised so the postgres
/// caller can `ereport` a precise diagnostic. Pure, NO_POSTGRES.
[[nodiscard]] inline std::expected<sequence_file_kind, std::string>
infer_sequence_file_kind(std::string_view path) {
    const std::string lowered = detail::ascii_lower(path);
    std::string_view stem{lowered};

    bool is_gzip = false;
    for (const std::string_view gz : detail::kGzipSuffixes) {
        if (stem.size() > gz.size() && stem.ends_with(gz)) {
            stem.remove_suffix(gz.size());
            is_gzip = true;
            break;
        }
    }
    for (const auto& [suffix, format] : detail::kFormatSuffixes) {
        if (stem.size() > suffix.size() && stem.ends_with(suffix)) {
            return sequence_file_kind{.format = format, .is_gzip_suffix = is_gzip};
        }
    }
    return std::unexpected(fmt::format(
        "unrecognised sequence file extension for path '{}': expected one of "
        ".fasta/.fa/.fna/.fastq/.fq/.fnq (optionally .gz/.bgz/.bgzf)", path));
}

}  // namespace bioseq::io
