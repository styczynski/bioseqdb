module;

// DOC0372 — NCBI Datasets hydrated-ZIP retrieval (strategy 3, spec §3294-3320).
// A bounded, safety-first libarchive reader: it opens a seekable ZIP (a spooled
// temporary file in production; an in-memory buffer in tests), rejects every
// unsafe member (path escape, absolute path, symlink/hardlink/device/fifo),
// enforces entry-count / member-name / decoded-byte / compression-ratio caps,
// selects sequence members by `dataset_catalog.json` content type (not filename
// guessing), validates each selected member's decoded bytes against the root
// `md5sum.txt` digest BEFORE any of its rows are accepted, and feeds the
// verified FASTA bytes to the shared `fasta_chunk_emitter`. Two passes over the
// seekable archive: pass 1 captures the small control members (catalog +
// md5sum) so no sequence data is ever emitted before its integrity is known;
// pass 2 reads each selected member, hashes it while enforcing the byte/ratio
// caps, and emits its rows only once the digest matches (verify-before-emit).
//
// This slice buffers one member at a time to guarantee verify-before-emit and
// emits the terminal `owning_sequence_asset_end` MD5 event per member (spec
// §3187-3191, test #29). It is wired into the PG scan: `start_streaming` routes
// GCF_/GCA_ package accessions here (scan.cppm), and multi-record members stream
// every record. A spooled seekable temp file for *very large* packages (instead
// of the current bounded in-memory buffer) remains a later refinement. The module
// is proven both offline (in-memory fixtures) and end-to-end in PostgreSQL.

#include <archive.h>
#include <archive_entry.h>

#include "archive_md5.h"  // detail::md5_hex_native — OpenSSL isolated in a classic TU

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

export module bioseq.external.io.archive_source;

import bioseq.common.checked_arithmetic;
import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.query;
import bioseq.external.provider.messages;
import bioseq.external.provider.model;  // integrity_scope, owning_integrity_result
import bioseq.external.io.sequence_stream;

export namespace bioseq::external::io {

namespace prov = bioseq::external::provider;

// Archive-safety policy (DOC0372 §3294-3320). A value of 0 means "unbounded"
// only where noted; the defaults are conservative and match the spec's field
// caps (maximum_archive_member_name_bytes = 4096).
struct archive_read_limits {
    std::uint64_t maximum_entries = 4096;
    std::uint64_t maximum_member_name_bytes = 4096;
    std::uint64_t maximum_control_member_bytes = 4U * 1024U * 1024U;
    std::uint64_t maximum_member_decoded_bytes = 0;   // 0 = unbounded per member
    std::uint64_t maximum_total_decoded_bytes = 0;    // 0 = unbounded overall
    // decoded_total <= grace + compressed * numerator / denominator (zip bomb).
    bioseq::common::expansion_ratio_limit maximum_expansion{
        .numerator = 1000, .denominator = 1, .grace_bytes = 1U * 1024U * 1024U};
};

struct archive_extract_result {
    std::uint64_t members_seen = 0;
    std::uint64_t members_selected = 0;
    std::uint64_t members_verified = 0;
    std::uint64_t total_decoded_bytes = 0;
    std::uint64_t sequence_rows = 0;
};

// Lower-case hex MD5 of `bytes` via OpenSSL EVP (the low-level MD5() one-shot is
// deprecated under OpenSSL 3.x, so use the EVP one-shot).
[[nodiscard]] inline std::string md5_hex(std::span<const std::uint8_t> bytes) {
    return detail::md5_hex_native(bytes.data(), bytes.size());
}

// A member path is safe iff it is relative, has no `.` / `..` component, no
// leading slash, no drive/backslash, and is within the name-length cap. Empty
// paths are unsafe. This is filename-agnostic path-escape defense; member TYPE
// (symlink/device) is rejected separately from the archive_entry.
[[nodiscard]] inline bool archive_member_name_is_safe(
    std::string_view path, std::uint64_t maximum_name_bytes) {
    if (path.empty()) {
        return false;
    }
    if (maximum_name_bytes != 0 && path.size() > maximum_name_bytes) {
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return false;  // absolute
    }
    if (path.find('\\') != std::string_view::npos) {
        return false;  // backslash / Windows drive separators
    }
    if (path.find("//") != std::string_view::npos) {
        return false;
    }
    // Reject any `.` or `..` path component (a component is a slash-delimited
    // segment). A leading/trailing/internal ".." would escape the root.
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end =
            slash == std::string_view::npos ? path.size() : slash;
        const std::string_view component = path.substr(start, end - start);
        if (component == "." || component == "..") {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

namespace detail {

[[nodiscard]] inline prov::provider_id archive_provider_id() noexcept {
    static constexpr std::string_view value = "ncbi";
    return prov::provider_id{value};
}

// RAII guards so the native handles are freed on every early return.
struct archive_read_guard {
    struct ::archive* handle = nullptr;
    archive_read_guard() = default;
    archive_read_guard(const archive_read_guard&) = delete;
    archive_read_guard& operator=(const archive_read_guard&) = delete;
    ~archive_read_guard() {
        if (handle != nullptr) {
            ::archive_read_free(handle);
        }
    }
};

// The pathname NCBI uses for the two control members; matched by basename so a
// nested `ncbi_dataset/data/...` layout still resolves them.
[[nodiscard]] inline std::string_view basename_of(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// NCBI Datasets ZIPs reference members with three conventions: ZIP entries and
// md5sum.txt use the full "ncbi_dataset/data/<path>", while dataset_catalog.json
// `filePath` is relative to that data directory. Canonicalize all three to the
// data-relative form by stripping a leading "ncbi_dataset/data/" so exact
// matching (selection + md5 lookup) reconciles across the three sources.
[[nodiscard]] inline std::string_view canonical_member_path(
    std::string_view path) {
    constexpr std::string_view kDataPrefix = "ncbi_dataset/data/";
    if (path.size() > kDataPrefix.size() &&
        path.substr(0, kDataPrefix.size()) == kDataPrefix) {
        return path.substr(kDataPrefix.size());
    }
    return path;
}

[[nodiscard]] inline bool is_catalog_member(std::string_view path) {
    return basename_of(path) == "dataset_catalog.json";
}

[[nodiscard]] inline bool is_md5sum_member(std::string_view path) {
    return basename_of(path) == "md5sum.txt";
}

// Parse `md5sum.txt`: each line is `<32-hex-digest><whitespace><relative-path>`.
// Returns path -> lower-case digest. Unparseable lines are skipped.
[[nodiscard]] inline std::unordered_map<std::string, std::string> parse_md5sum(
    std::string_view text) {
    std::unordered_map<std::string, std::string> out;
    std::size_t pos = 0;
    while (pos < text.size()) {
        std::size_t eol = text.find('\n', pos);
        if (eol == std::string_view::npos) {
            eol = text.size();
        }
        std::string_view line = text.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        std::size_t sep = line.find_first_of(" \t");
        if (sep == std::string_view::npos) {
            continue;
        }
        std::string_view digest = line.substr(0, sep);
        std::size_t path_start = line.find_first_not_of(" \t*", sep);
        if (digest.size() != 32 || path_start == std::string_view::npos) {
            continue;
        }
        std::string lower_digest;
        lower_digest.reserve(digest.size());
        for (const char c : digest) {
            lower_digest.push_back(
                (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c);
        }
        out.emplace(
            std::string(canonical_member_path(line.substr(path_start))),
            std::move(lower_digest));
    }
    return out;
}

// Minimal, adjacency-based scan of `dataset_catalog.json`: NCBI emits each file
// object as `{"filePath": "...", "fileType": "..."}`, so for every `"filePath"`
// value we look ahead a bounded window for the sibling `"fileType"` and select
// the path when the type names a FASTA content type. This is content-type
// selection (not filename guessing); a full JSON parse is a deferred refinement.
[[nodiscard]] inline std::string_view next_json_string(std::string_view text,
                                                       std::size_t& cursor) {
    const std::size_t open = text.find('"', cursor);
    if (open == std::string_view::npos) {
        cursor = text.size();
        return {};
    }
    const std::size_t close = text.find('"', open + 1);
    if (close == std::string_view::npos) {
        cursor = text.size();
        return {};
    }
    cursor = close + 1;
    return text.substr(open + 1, close - open - 1);
}

[[nodiscard]] inline bool is_fasta_file_type(std::string_view file_type) {
    // Upper-case compare against the FASTA markers NCBI Datasets emits
    // (…_FASTA, …_FNA); the check is substring "FASTA" or "FNA".
    std::string upper;
    upper.reserve(file_type.size());
    for (const char c : file_type) {
        upper.push_back(
            (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c);
    }
    return upper.find("FASTA") != std::string::npos ||
           upper.find("FNA") != std::string::npos;
}

[[nodiscard]] inline std::unordered_set<std::string>
select_fasta_members_from_catalog(std::string_view catalog) {
    std::unordered_set<std::string> selected;
    std::size_t cursor = 0;
    while (cursor < catalog.size()) {
        const std::size_t key = catalog.find("\"filePath\"", cursor);
        if (key == std::string_view::npos) {
            break;
        }
        std::size_t value_cursor = key + std::string_view("\"filePath\"").size();
        // skip the ':' and read the path value
        const std::string_view path = next_json_string(catalog, value_cursor);
        // look ahead for the sibling fileType within a bounded window
        const std::size_t type_key = catalog.find("\"fileType\"", value_cursor);
        if (!path.empty() && type_key != std::string_view::npos &&
            type_key - value_cursor < 256U) {
            std::size_t type_cursor =
                type_key + std::string_view("\"fileType\"").size();
            const std::string_view file_type =
                next_json_string(catalog, type_cursor);
            if (is_fasta_file_type(file_type)) {
                selected.emplace(canonical_member_path(path));
            }
            cursor = type_cursor;
        } else {
            cursor = value_cursor;
        }
    }
    return selected;
}

}  // namespace detail

// Extract and stream the catalog-selected, MD5-verified FASTA members of a
// hydrated NCBI Datasets ZIP held in `zip_bytes`. Every emitted row is verified;
// any policy violation, integrity failure, or corruption fails the whole call
// with no partial rows retained downstream (the sink sees rows only for a
// member whose MD5 already matched). `target`/`chunk_bases`/`sink`/`stop` are
// threaded straight into a per-member `fasta_chunk_emitter`. `emit_accession`
// is the accession every emitted chunk is keyed by so the PG scan's
// `accession = '<assembly>'` filter matches (e.g. "GCF_009858895.2"); when empty
// it falls back to the member path. `execution_key` identifies the enclosing
// execution for the terminal per-asset MD5 event.
[[nodiscard]] inline std::expected<archive_extract_result, prov::external_error>
extract_fasta_members_from_zip(std::span<const std::uint8_t> zip_bytes,
                               const archive_read_limits& limits,
                               prov::query_target target,
                               std::uint64_t chunk_bases,
                               prov::provider_message_sink_ref sink,
                               std::stop_token stop,
                               std::string_view emit_accession = {},
                               prov::external_execution_key execution_key = {}) {
    const auto fail = [](prov::external_error_kind kind,
                         std::string_view message) {
        return std::unexpected(prov::make_external_error(
            kind, detail::archive_provider_id(), "ncbi_datasets_zip", message));
    };

    if (limits.maximum_expansion.denominator == 0) {
        return fail(prov::external_error_kind::provider_contract,
                    "archive expansion ratio has zero denominator");
    }

    // ---- Pass 1: capture the control members (catalog + md5sum) ---------------
    std::string catalog_text;
    std::string md5sum_text;
    bool have_catalog = false;
    bool have_md5sum = false;
    {
        detail::archive_read_guard reader;
        reader.handle = ::archive_read_new();
        if (reader.handle == nullptr) {
            return fail(prov::external_error_kind::transport,
                        "archive_read_new failed");
        }
        ::archive_read_support_format_zip(reader.handle);
        if (::archive_read_open_memory(reader.handle, zip_bytes.data(),
                                       zip_bytes.size()) != ARCHIVE_OK) {
            return fail(prov::external_error_kind::parse,
                        "archive_read_open_memory failed (pass 1)");
        }
        std::uint64_t entries = 0;
        struct ::archive_entry* entry = nullptr;
        for (;;) {
            if (stop.stop_requested()) {
                return fail(prov::external_error_kind::cancelled,
                            "archive extraction cancelled");
            }
            const int rc = ::archive_read_next_header(reader.handle, &entry);
            if (rc == ARCHIVE_EOF) {
                break;
            }
            if (rc < ARCHIVE_OK && rc != ARCHIVE_WARN) {
                return fail(prov::external_error_kind::parse,
                            "archive_read_next_header failed (pass 1)");
            }
            if (++entries > limits.maximum_entries) {
                return fail(prov::external_error_kind::response_too_large,
                            "archive exceeds maximum entry count");
            }
            const char* raw_path = ::archive_entry_pathname(entry);
            const std::string_view path =
                raw_path == nullptr ? std::string_view{} : raw_path;
            if (!archive_member_name_is_safe(path,
                                             limits.maximum_member_name_bytes)) {
                return fail(prov::external_error_kind::provider_contract,
                            "archive member has an unsafe or escaping path");
            }
            if (::archive_entry_hardlink(entry) != nullptr ||
                ::archive_entry_symlink(entry) != nullptr) {
                return fail(prov::external_error_kind::provider_contract,
                            "archive member is a link (symlink/hardlink)");
            }
            const unsigned int filetype =
                ::archive_entry_filetype(entry) & AE_IFMT;
            if (filetype == AE_IFDIR) {
                continue;  // directories carry no data; skip
            }
            if (filetype != AE_IFREG) {
                return fail(prov::external_error_kind::provider_contract,
                            "archive member is not a regular file "
                            "(device/fifo/socket/symlink)");
            }
            const bool is_catalog = detail::is_catalog_member(path);
            const bool is_md5sum = detail::is_md5sum_member(path);
            if (!is_catalog && !is_md5sum) {
                continue;  // sequence members are read in pass 2
            }
            std::string* target_text = is_catalog ? &catalog_text : &md5sum_text;
            target_text->clear();
            std::array<std::uint8_t, 65536> buffer{};
            for (;;) {
                const auto n = ::archive_read_data(reader.handle, buffer.data(),
                                                   buffer.size());
                if (n == 0) {
                    break;
                }
                if (n < 0) {
                    return fail(prov::external_error_kind::parse,
                                "archive_read_data failed on a control member");
                }
                if (target_text->size() + static_cast<std::size_t>(n) >
                    limits.maximum_control_member_bytes) {
                    return fail(prov::external_error_kind::response_too_large,
                                "archive control member exceeds its size cap");
                }
                target_text->append(reinterpret_cast<const char*>(buffer.data()),
                                    static_cast<std::size_t>(n));
            }
            have_catalog = have_catalog || is_catalog;
            have_md5sum = have_md5sum || is_md5sum;
        }
    }

    if (!have_catalog) {
        return fail(prov::external_error_kind::provider_contract,
                    "archive is missing dataset_catalog.json");
    }
    if (!have_md5sum) {
        return fail(prov::external_error_kind::integrity,
                    "archive is missing md5sum.txt");
    }

    const std::unordered_set<std::string> selected =
        detail::select_fasta_members_from_catalog(catalog_text);
    const std::unordered_map<std::string, std::string> expected_md5 =
        detail::parse_md5sum(md5sum_text);

    // ---- Pass 2: stream + verify the selected sequence members ----------------
    archive_extract_result result;
    result.members_selected = selected.size();
    {
        detail::archive_read_guard reader;
        reader.handle = ::archive_read_new();
        if (reader.handle == nullptr) {
            return fail(prov::external_error_kind::transport,
                        "archive_read_new failed (pass 2)");
        }
        ::archive_read_support_format_zip(reader.handle);
        if (::archive_read_open_memory(reader.handle, zip_bytes.data(),
                                       zip_bytes.size()) != ARCHIVE_OK) {
            return fail(prov::external_error_kind::parse,
                        "archive_read_open_memory failed (pass 2)");
        }
        struct ::archive_entry* entry = nullptr;
        // Global chunk index carried across members so their chunks never collide on
        // (accession, chunk_index) when more than one FASTA member is selected under
        // one queried accession. Today the Datasets download requests a single
        // GENOME_FASTA member, but the selection predicate admits several file types.
        std::uint64_t next_chunk_index = 0;
        for (;;) {
            if (stop.stop_requested()) {
                return fail(prov::external_error_kind::cancelled,
                            "archive extraction cancelled");
            }
            const int rc = ::archive_read_next_header(reader.handle, &entry);
            if (rc == ARCHIVE_EOF) {
                break;
            }
            if (rc < ARCHIVE_OK && rc != ARCHIVE_WARN) {
                return fail(prov::external_error_kind::parse,
                            "archive_read_next_header failed (pass 2)");
            }
            ++result.members_seen;
            const char* raw_path = ::archive_entry_pathname(entry);
            const std::string_view path =
                raw_path == nullptr ? std::string_view{} : raw_path;
            const std::string canonical(detail::canonical_member_path(path));
            if (selected.find(canonical) == selected.end()) {
                continue;  // not a catalog-selected FASTA member
            }

            const auto md5_it = expected_md5.find(canonical);
            if (md5_it == expected_md5.end()) {
                return fail(prov::external_error_kind::integrity,
                            "selected member absent from md5sum.txt");
            }

            // Read the whole member under the per-member + total decoded caps and
            // the decoded/compressed expansion ratio, then hash it one-shot; its
            // rows are emitted only after the digest matches md5sum.txt.
            std::string member_bytes;  // buffered for verify-before-emit
            std::uint64_t member_bytes_count = 0;
            std::array<std::uint8_t, 65536> buffer{};
            std::optional<std::unexpected<prov::external_error>> member_error;
            for (;;) {
                const auto n = ::archive_read_data(reader.handle, buffer.data(),
                                                   buffer.size());
                if (n == 0) {
                    break;
                }
                if (n < 0) {
                    member_error = std::unexpected(prov::make_external_error(
                        prov::external_error_kind::parse,
                        detail::archive_provider_id(), "ncbi_datasets_zip",
                        "archive_read_data failed on a sequence member"));
                    break;
                }
                const auto chunk_len = static_cast<std::uint64_t>(n);
                member_bytes_count += chunk_len;
                result.total_decoded_bytes += chunk_len;
                if (limits.maximum_member_decoded_bytes != 0 &&
                    member_bytes_count > limits.maximum_member_decoded_bytes) {
                    member_error = std::unexpected(prov::make_external_error(
                        prov::external_error_kind::response_too_large,
                        detail::archive_provider_id(), "ncbi_datasets_zip",
                        "archive member exceeds its decoded-byte cap"));
                    break;
                }
                if (limits.maximum_total_decoded_bytes != 0 &&
                    result.total_decoded_bytes >
                        limits.maximum_total_decoded_bytes) {
                    member_error = std::unexpected(prov::make_external_error(
                        prov::external_error_kind::response_too_large,
                        detail::archive_provider_id(), "ncbi_datasets_zip",
                        "archive exceeds its total decoded-byte cap"));
                    break;
                }
                const std::optional<std::uint64_t> ratio_ceiling =
                    bioseq::common::scaled_limit(
                        static_cast<std::uint64_t>(zip_bytes.size()),
                        limits.maximum_expansion);
                if (!ratio_ceiling.has_value() ||
                    result.total_decoded_bytes > *ratio_ceiling) {
                    member_error = std::unexpected(prov::make_external_error(
                        prov::external_error_kind::response_too_large,
                        detail::archive_provider_id(), "ncbi_datasets_zip",
                        "archive decoded/compressed ratio exceeds the cap"));
                    break;
                }
                member_bytes.append(reinterpret_cast<const char*>(buffer.data()),
                                    static_cast<std::size_t>(n));
            }
            if (member_error.has_value()) {
                return *member_error;
            }

            const std::string actual_hex = detail::md5_hex_native(
                reinterpret_cast<const unsigned char*>(member_bytes.data()),
                member_bytes.size());
            if (actual_hex.empty() || actual_hex != md5_it->second) {
                return fail(prov::external_error_kind::integrity,
                            "archive member failed md5sum.txt verification");
            }
            ++result.members_verified;

            // Verified — now emit its rows through the shared FASTA chunker. Rows
            // are keyed by `emit_accession` (the queried assembly accession) so the
            // PG scan's `accession = '<assembly>'` filter matches; the member path
            // is only a fallback for direct callers that pass no accession.
            fasta_chunk_emitter emitter(
                target, chunk_bases,
                emit_accession.empty() ? std::string_view(path) : emit_accession,
                sink, /*multi_record=*/true, /*start_chunk_index=*/next_chunk_index);
            const auto fed = emitter.feed(
                reinterpret_cast<const std::uint8_t*>(member_bytes.data()),
                member_bytes.size());
            if (!fed.has_value()) {
                return std::unexpected(fed.error());
            }
            const auto rows = emitter.finish();
            if (!rows.has_value()) {
                return std::unexpected(rows.error());
            }
            result.sequence_rows += *rows;
            next_chunk_index = emitter.next_chunk_index();  // carry across members

            // Terminal per-asset event: this member is fully decoded and its
            // decoded-member MD5 already verified above, so emit exactly one
            // owning_sequence_asset_end carrying the digest as integrity metadata
            // (spec §3179-3213, §3316; test #29). A mismatch returned earlier, so
            // a successful asset-end is never emitted for a failed member.
            prov::owning_sequence_asset_end asset_end;
            asset_end.asset_id.execution = execution_key;
            asset_end.asset_id.index = result.members_verified - 1;
            asset_end.record_count = emitter.records_seen();
            asset_end.decoded_bytes = member_bytes.size();
            asset_end.integrity.expected.algorithm =
                prov::bounded_string<64>::make_truncated("md5");
            asset_end.integrity.expected.value =
                prov::bounded_string<prov::maximum_identifier_bytes>::
                    make_truncated(md5_it->second);
            asset_end.integrity.expected.applies_to =
                prov::integrity_scope::decoded_member;
            asset_end.integrity.actual_value =
                prov::bounded_string<prov::maximum_identifier_bytes>::
                    make_truncated(actual_hex);
            asset_end.integrity.verified = true;
            const auto asset_pushed = sink.push_fn(
                sink.object,
                prov::owning_provider_message{std::move(asset_end)});
            if (!asset_pushed.has_value()) {
                return std::unexpected(asset_pushed.error());
            }
        }
    }

    if (result.members_verified == 0) {
        return fail(prov::external_error_kind::provider_contract,
                    "archive yielded no catalog-selected, verified FASTA member");
    }
    return result;
}

}  // namespace bioseq::external::io
