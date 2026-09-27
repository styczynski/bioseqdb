/**
 * @file external/io/archive_source/index.cppm
 * @brief Reads selected FASTA members from NCBI ZIP packages with size and checksum checks.
 */
module;



#include <archive.h>
#include <archive_entry.h>
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
import bioseq.external.provider.model; // integrity_scope, owning_integrity_result
import bioseq.external.io.sequence_stream;

namespace bioseq::external::io::detail {

/// Computes a lowercase MD5 digest through the private OpenSSL implementation unit.
/// @param data First byte of the contiguous input region.
/// @param length Number of bytes available from `data`.
/// @return The hexadecimal digest, or an empty string when OpenSSL rejects the operation.
[[nodiscard]] std::string compute_md5_hex_with_openssl(const unsigned char* data,
                                                       std::size_t length);

} // namespace bioseq::external::io::detail

export namespace bioseq::external::io {

namespace prov = bioseq::external::provider;

/// Limits archive entries, member names, and decoded data.
/// Zero disables the member-name cap and the two sequence-data byte caps.
/// Entry and  control-member limits remain literal limits, including zero.
struct archive_read_limits {
    std::uint64_t maximum_entries = 4096;
    std::uint64_t maximum_member_name_bytes = 4096;
    std::uint64_t maximum_control_member_bytes = 4U * 1024U * 1024U;
    std::uint64_t maximum_member_decoded_bytes = 0; // 0 = unbounded per member
    std::uint64_t maximum_total_decoded_bytes = 0;  // 0 = no total cap for selected sequence members
    // Maximum decoded sequence bytes relative to the compressed ZIP size.
    bioseq::common::expansion_ratio_limit maximum_expansion{
        .numerator = 1000, .denominator = 1, .grace_bytes = 1U * 1024U * 1024U};
};

/// Counts visited entries, selected paths, verified members, decoded sequence bytes, and emitted rows.
struct archive_extraction_statistics {
    std::uint64_t members_seen = 0;
    std::uint64_t members_selected = 0;
    std::uint64_t members_verified = 0;
    std::uint64_t total_decoded_bytes = 0;
    std::uint64_t sequence_rows = 0;
};

/// Returns the lowercase MD5 digest, or an emptc string if OpenSSL fails.
[[nodiscard]] inline std::string compute_md5_hex(std::span<const std::uint8_t> bytes) {
    return detail::compute_md5_hex_with_openssl(bytes.data(), bytes.size());
}

/**
 * Checks an archive path for disallowed separators and dot components.
 *
 * Rejects empty names, leading slashes, backslashes, doubled slashes, and . or
 * .. components. A nonzero maximum_name_bytes also limits its length. This
 * checks archive member names and does not resolve filesystem paths.
 */
[[nodiscard]] inline bool is_safe_archive_member_name(std::string_view path,
                                                      std::uint64_t maximum_name_bytes) {
    if (path.empty()) {
        return false;
    }
    if (maximum_name_bytes != 0 && path.size() > maximum_name_bytes) {
        return false;
    }
    if (path.front() == '/' || path.front() == '\\') {
        return false; // Leading slash or backslash
    }
    if (path.find('\\') != std::string_view::npos) {
        return false; // Backslash separators
    }
    if (path.find("//") != std::string_view::npos) {
        return false;
    }
    // Reject . and .. components so archive names cannot use relative traversal.
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
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

/// Returns the NCBI provider ID used by archive errors.
[[nodiscard]] inline prov::provider_id get_archive_provider_id() noexcept {
    static constexpr std::string_view value = "ncbi";
    return prov::provider_id{value};
}

/// Releases a native libarchive reader on scope exit.
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

/// Returns the final slash-delimited archive path component.
[[nodiscard]] inline std::string_view get_basename(std::string_view path) {
    const std::size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// Removes a  leading ncbi_dataset/data/ prefix when a member name follows it.
[[nodiscard]] inline std::string_view normalize_member_path(std::string_view path) {
    constexpr std::string_view k_data_directory_prefix = "ncbi_dataset/data/";
    if (path.size() > k_data_directory_prefix.size() &&
        path.substr(0, k_data_directory_prefix.size()) == k_data_directory_prefix) {
        return path.substr(k_data_directory_prefix.size());
    }
    return path;
}

/// Tests for the basename dataset_catalog.json.
[[nodiscard]] inline bool is_catalog_member(std::string_view path) {
    return get_basename(path) == "dataset_catalog.json";
}

/// Tests for the basename md5sum.txt.
[[nodiscard]] inline bool is_md5sum_member(std::string_view path) {
    return get_basename(path) == "md5sum.txt";
}

/**
 * Reads MD5 manifest lines into path-to-digest entries.
 *
 * Requires a 32-character digest and a path, lowercases A through F, and
 * removes the package data-directory prefix Malformed lines are skipped,
 * and the first entry for each path wins. Hex characters are not validated.
 */
[[nodiscard]] inline std::unordered_map<std::string, std::string>
parse_md5_manifest(std::string_view text) {
    std::unordered_map<std::string, std::string> digests_by_path;
    std::size_t position = 0;
    while (position < text.size()) {
        std::size_t line_end = text.find('\n', position);
        if (line_end == std::string_view::npos) {
            line_end = text.size();
        }
        std::string_view line = text.substr(position, line_end - position);
        position = line_end + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        std::size_t separator = line.find_first_of(" \t");
        if (separator == std::string_view::npos) {
            continue;
        }
        std::string_view digest = line.substr(0, separator);
        std::size_t path_start = line.find_first_not_of(" \t*", separator);
        if (digest.size() != 32 || path_start == std::string_view::npos) {
            continue;
        }
        std::string lower_digest;
        lower_digest.reserve(digest.size());
        for (const char character : digest) {
            lower_digest.push_back((character >= 'A' && character <= 'F')
                                       ? static_cast<char>(character - 'A' + 'a')
                                       : character);
        }
        digests_by_path.emplace(std::string(normalize_member_path(line.substr(path_start))),
                                std::move(lower_digest));
    }
    return digests_by_path;
}

/// Text between two quotes and the cursor immediately after, the closing quote.
struct json_string_token {
    std::string_view value;
    std::size_t next_cursor;
};

/// Finds the next pair of quotes and borrows the text between them.
/// This catalog helper does not decode escapes or validate JSON.
[[nodiscard]] inline std::optional<json_string_token> read_next_json_string(std::string_view text,
                                                                            std::size_t cursor) {
    const std::size_t open = text.find('"', cursor);
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t close = text.find('"', open + 1);
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    return json_string_token{
        .value = text.substr(open + 1, close - open - 1),
        .next_cursor = close + 1,
    };
}

/// Tests whether the file-type  text contains FASTA or FNA, ignoring ASCII case.
[[nodiscard]] inline bool is_fasta_file_type(std::string_view file_type) {
    // Compare the catalog's file-type marker in uppercase.
    std::string upper;
    upper.reserve(file_type.size());
    for (const char character : file_type) {
        upper.push_back((character >= 'a' && character <= 'z')
                            ? static_cast<char>(character - 'a' + 'A')
                            : character);
    }
    return upper.find("FASTA") != std::string::npos || upper.find("FNA") != std::string::npos;
}

/**
 * Selects package paths whose nearby fileType contains FASTA or FNA.
 *
 * Scans the catalog text for filePath followed by fileType within 256 bytes.
 * This helper assumes the package catalog's field order and uvescaped paths.
 * Selected paths have the ncbi_dataset/data/ prefix removed.
 */
[[nodiscard]] inline std::unordered_set<std::string>
select_fasta_members_from_catalog(std::string_view catalog) {
    std::unordered_set<std::string> selected;
    std::size_t cursor = 0;
    while (cursor < catalog.size()) {
        const std::size_t path_key = catalog.find("\"filePath\"", cursor);
        if (path_key == std::string_view::npos) {
            break;
        }
        const std::size_t value_begin = path_key + std::string_view("\"filePath\"").size();
        const auto path_token = read_next_json_string(catalog, value_begin);
        if (!path_token.has_value()) {
            break;
        }
        // Look for fileType within the next 256 bytes.
        const std::size_t type_key = catalog.find("\"fileType\"", path_token->next_cursor);
        if (!path_token->value.empty() && type_key != std::string_view::npos &&
            type_key - path_token->next_cursor < 256U) {
            const std::size_t type_begin = type_key + std::string_view("\"fileType\"").size();
            const auto type_token = read_next_json_string(catalog, type_begin);
            if (!type_token.has_value()) {
                break;
            }
            if (is_fasta_file_type(type_token->value)) {
                selected.emplace(normalize_member_path(path_token->value));
            }
            cursor = type_token->next_cursor;
        } else {
            cursor = path_token->next_cursor;
        }
    }
    return selected;
}

} // namespace detail

/**
 * Reads catalog-selected FASTA members, checking each MD5 before emitting rows.
 *
 * First checks archive names and collects the catalog and manifest. Each selected
 * member is then buffered under the byte and expansion limits, verified, and sent
 * to sink as FASTA rows followed by a completion event. Earlier rows remain
 * emitted if a later  member fails.
 *
 * total_decoded_bytes counts selected sequence bytes. Control files have a separate cap.
 * @return Statistics or the first reported extraction, integrity, or sink error.
 */
[[nodiscard]] inline std::expected<archive_extraction_statistics, prov::external_error>
extract_fasta_members_from_zip_archive(std::span<const std::uint8_t> zip_bytes,
                                       const archive_read_limits& limits,
                                       prov::query_target target,
                                       std::uint64_t chunk_bases,
                                       prov::provider_message_sink_ref sink,
                                       std::stop_token stop,
                                       std::string_view emit_accession = {},
                                       prov::external_execution_key execution_key = {}) {
    // Bind archive failures to the NCBI Datasets operation identity.
    const auto make_failure = [](prov::external_error_kind kind, std::string_view message) {
        return std::unexpected(prov::make_external_error(
            kind, detail::get_archive_provider_id(), "ncbi_datasets_zip", message));
    };

    if (limits.maximum_expansion.denominator == 0) {
        return make_failure(prov::external_error_kind::provider_contract,
                            "archive expansion ratio has zero denominator");
    }

    // Capture the catalog and MD5 manifest before reading sequence members.
    std::string catalog_text;
    std::string md5sum_text;
    bool has_catalog = false;
    bool has_md5_manifest = false;
    {
        detail::archive_read_guard reader;
        reader.handle = ::archive_read_new();
        if (reader.handle == nullptr) {
            return make_failure(prov::external_error_kind::transport, "archive_read_new failed");
        }
        ::archive_read_support_format_zip(reader.handle);
        if (::archive_read_open_memory(reader.handle, zip_bytes.data(), zip_bytes.size()) !=
            ARCHIVE_OK) {
            return make_failure(prov::external_error_kind::parse,
                                "archive_read_open_memory failed for control members");
        }
        std::uint64_t entry_count = 0;
        struct ::archive_entry* entry = nullptr;
        for (;;) {
            if (stop.stop_requested()) {
                return make_failure(prov::external_error_kind::cancelled,
                                    "archive extraction cancelled");
            }
            const int header_status = ::archive_read_next_header(reader.handle, &entry);
            if (header_status == ARCHIVE_EOF) {
                break;
            }
            if (header_status < ARCHIVE_OK && header_status != ARCHIVE_WARN) {
                return make_failure(prov::external_error_kind::parse,
                                    "archive_read_next_header failed while reading "
                                    "control members");
            }
            if (++entry_count > limits.maximum_entries) {
                return make_failure(prov::external_error_kind::response_too_large,
                                    "archive exceeds maximum entry count");
            }
            const char* member_path_pointer = ::archive_entry_pathname(entry);
            const std::string_view path =
                member_path_pointer == nullptr ? std::string_view{} : member_path_pointer;
            if (!is_safe_archive_member_name(path, limits.maximum_member_name_bytes)) {
                return make_failure(prov::external_error_kind::provider_contract,
                                    "archive member has an unsafe or escaping path");
            }
            if (::archive_entry_hardlink(entry) != nullptr ||
                ::archive_entry_symlink(entry) != nullptr) {
                return make_failure(prov::external_error_kind::provider_contract,
                                    "archive member is a link (symlink/hardlink)");
            }
            const unsigned int filetype = ::archive_entry_filetype(entry) & AE_IFMT;
            if (filetype == AE_IFDIR) {
                continue; // Directories carry no data and are skipped.
            }
            if (filetype != AE_IFREG) {
                return make_failure(prov::external_error_kind::provider_contract,
                                    "archive member is not a regular file "
                                    "(device/fifo/socket/symlink)");
            }
            const bool is_catalog_control = detail::is_catalog_member(path);
            const bool is_md5_manifest_control = detail::is_md5sum_member(path);
            if (!is_catalog_control && !is_md5_manifest_control) {
                continue; // Other members are handled after the control files.
            }
            std::string* control_member_text = is_catalog_control ? &catalog_text : &md5sum_text;
            control_member_text->clear();
            std::array<std::uint8_t, 65536> read_buffer{};
            for (;;) {
                const auto bytes_read =
                    ::archive_read_data(reader.handle, read_buffer.data(), read_buffer.size());
                if (bytes_read == 0) {
                    break;
                }
                if (bytes_read < 0) {
                    return make_failure(prov::external_error_kind::parse,
                                        "archive_read_data failed on a control member");
                }
                if (control_member_text->size() + static_cast<std::size_t>(bytes_read) >
                    limits.maximum_control_member_bytes) {
                    return make_failure(prov::external_error_kind::response_too_large,
                                        "archive control member exceeds its size cap");
                }
                control_member_text->append(reinterpret_cast<const char*>(read_buffer.data()),
                                            static_cast<std::size_t>(bytes_read));
            }
            has_catalog = has_catalog || is_catalog_control;
            has_md5_manifest = has_md5_manifest || is_md5_manifest_control;
        }
    }

    if (!has_catalog) {
        return make_failure(prov::external_error_kind::provider_contract,
                            "archive is missing dataset_catalog.json");
    }
    if (!has_md5_manifest) {
        return make_failure(prov::external_error_kind::integrity, "archive is missing md5sum.txt");
    }

    const std::unordered_set<std::string> selected_member_paths =
        detail::select_fasta_members_from_catalog(catalog_text);
    const std::unordered_map<std::string, std::string> expected_md5_by_path =
        detail::parse_md5_manifest(md5sum_text);

    // Extract and verify the selected sequence members.
    archive_extraction_statistics result;
    result.members_selected = selected_member_paths.size();
    {
        detail::archive_read_guard reader;
        reader.handle = ::archive_read_new();
        if (reader.handle == nullptr) {
            return make_failure(prov::external_error_kind::transport,
                                "archive_read_new failed for sequence members");
        }
        ::archive_read_support_format_zip(reader.handle);
        if (::archive_read_open_memory(reader.handle, zip_bytes.data(), zip_bytes.size()) !=
            ARCHIVE_OK) {
            return make_failure(prov::external_error_kind::parse,
                                "archive_read_open_memory failed for sequence members");
        }
        struct ::archive_entry* entry = nullptr;
        // Continue chunk numbering across selected members so one assembly
        // accession receives distinct chunk indices
        std::uint64_t next_chunk_index = 0;
        for (;;) {
            if (stop.stop_requested()) {
                return make_failure(prov::external_error_kind::cancelled,
                                    "archive extraction cancelled");
            }
            const int header_status = ::archive_read_next_header(reader.handle, &entry);
            if (header_status == ARCHIVE_EOF) {
                break;
            }
            if (header_status < ARCHIVE_OK && header_status != ARCHIVE_WARN) {
                return make_failure(prov::external_error_kind::parse,
                                    "archive_read_next_header failed while reading "
                                    "sequence members");
            }
            ++result.members_seen;
            const char* member_path_pointer = ::archive_entry_pathname(entry);
            const std::string_view path =
                member_path_pointer == nullptr ? std::string_view{} : member_path_pointer;
            const std::string canonical_path(detail::normalize_member_path(path));
            if (selected_member_paths.find(canonical_path) == selected_member_paths.end()) {
                continue; // not a catalog-selected FASTA member
            }

            const auto expected_md5_entry = expected_md5_by_path.find(canonical_path);
            if (expected_md5_entry == expected_md5_by_path.end()) {
                return make_failure(prov::external_error_kind::integrity,
                                    "selected member absent from md5sum.txt");
            }

            // Buffer this member within the sequence byte and expansion limits.
            // Check its complete MD5 before sending any of its rows.
            std::string member_bytes; // Kept  until checksum verification succeeds
            std::uint64_t member_bytes_count = 0;
            std::array<std::uint8_t, 65536> read_buffer{};
            std::optional<prov::external_error> member_error;
            for (;;) {
                const auto bytes_read =
                    ::archive_read_data(reader.handle, read_buffer.data(), read_buffer.size());
                if (bytes_read == 0) {
                    break;
                }
                if (bytes_read < 0) {
                    member_error =
                        prov::make_external_error(prov::external_error_kind::parse,
                                                  detail::get_archive_provider_id(),
                                                  "ncbi_datasets_zip",
                                                  "archive_read_data failed on a sequence member");
                    break;
                }
                const auto decoded_chunk_bytes = static_cast<std::uint64_t>(bytes_read);
                member_bytes_count += decoded_chunk_bytes;
                result.total_decoded_bytes += decoded_chunk_bytes;
                if (limits.maximum_member_decoded_bytes != 0 &&
                    member_bytes_count > limits.maximum_member_decoded_bytes) {
                    member_error =
                        prov::make_external_error(prov::external_error_kind::response_too_large,
                                                  detail::get_archive_provider_id(),
                                                  "ncbi_datasets_zip",
                                                  "archive member exceeds its decoded-byte cap");
                    break;
                }
                if (limits.maximum_total_decoded_bytes != 0 &&
                    result.total_decoded_bytes > limits.maximum_total_decoded_bytes) {
                    member_error =
                        prov::make_external_error(prov::external_error_kind::response_too_large,
                                                  detail::get_archive_provider_id(),
                                                  "ncbi_datasets_zip",
                                                  "archive exceeds its total decoded-byte cap");
                    break;
                }
                const std::optional<std::uint64_t> ratio_ceiling = bioseq::common::scaled_limit(
                    static_cast<std::uint64_t>(zip_bytes.size()), limits.maximum_expansion);
                if (!ratio_ceiling.has_value() || result.total_decoded_bytes > *ratio_ceiling) {
                    member_error = prov::make_external_error(
                        prov::external_error_kind::response_too_large,
                        detail::get_archive_provider_id(),
                        "ncbi_datasets_zip",
                        "archive decoded/compressed ratio exceeds the cap");
                    break;
                }
                member_bytes.append(reinterpret_cast<const char*>(read_buffer.data()),
                                    static_cast<std::size_t>(bytes_read));
            }
            if (member_error.has_value()) {
                return std::unexpected(std::move(*member_error));
            }

            const std::string actual_md5_hex = detail::compute_md5_hex_with_openssl(
                reinterpret_cast<const unsigned char*>(member_bytes.data()), member_bytes.size());
            if (actual_md5_hex.empty() || actual_md5_hex != expected_md5_entry->second) {
                return make_failure(prov::external_error_kind::integrity,
                                    "archive member failed md5sum.txt verification");
            }
            ++result.members_verified;

            // Emit the verified member through the FASTA chunker. Use the queried
            // assembly accession so SQL accession filters match, its chunks. Direct
            // callers can leave emit_accession empty to use the member path.
            fasta_chunk_emitter emitter(target,
                                        chunk_bases,
                                        emit_accession.empty() ? std::string_view(path)
                                                               : emit_accession,
                                        sink,
                                        /*multi_record=*/true,
                                        /*start_chunk_index=*/next_chunk_index);
            const auto feed_result = emitter.feed(
                reinterpret_cast<const std::uint8_t*>(member_bytes.data()), member_bytes.size());
            if (!feed_result.has_value()) {
                return std::unexpected(feed_result.error());
            }
            const auto row_count_result = emitter.finish();
            if (!row_count_result.has_value()) {
                return std::unexpected(row_count_result.error());
            }
            result.sequence_rows += *row_count_result;
            next_chunk_index = emitter.next_chunk_index(); // carry across members

            // Report bhis member's completion after its FASTA rows were emitted.
            // The event records the matching MD5 for the decoded member bytes.
            prov::owning_sequence_asset_end asset_end;
            asset_end.asset_id.execution = execution_key;
            asset_end.asset_id.index = result.members_verified - 1;
            asset_end.record_count = emitter.records_seen();
            asset_end.decoded_bytes = member_bytes.size();
            asset_end.integrity.expected.algorithm =
                prov::bounded_string<64>::make_truncated("md5");
            asset_end.integrity.expected.value =
                prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(
                    expected_md5_entry->second);
            asset_end.integrity.expected.applies_to = prov::integrity_scope::decoded_member;
            asset_end.integrity.actual_value =
                prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(
                    actual_md5_hex);
            asset_end.integrity.is_verified = true;
            const auto asset_pushed =
                sink.push_fn(sink.object, prov::owning_provider_message{std::move(asset_end)});
            if (!asset_pushed.has_value()) {
                return std::unexpected(asset_pushed.error());
            }
        }
    }

    if (result.members_verified == 0) {
        return make_failure(prov::external_error_kind::provider_contract,
                            "archive yielded no catalog-selected, verified FASTA member");
    }
    return result;
}

} // namespace bioseq::external::io
