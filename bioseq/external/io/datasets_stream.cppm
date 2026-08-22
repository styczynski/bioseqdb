/**
 * @file external/io/datasets_stream.cppm
 * @brief NCBI Datasets archive stream selection and decoding.
 */
module;

/// Streams bounded NCBI Datasets packages through archive validation.

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

export module bioseq.external.io.datasets_stream;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.query;
import bioseq.external.provider.messages;
import bioseq.external.provider.capabilities;
import bioseq.external.transport.locator;
import bioseq.external.transport.remote_source;
import bioseq.io.sources;
import bioseq.external.io.sequence_stream; // ncbi_eutils_policy, ncbi_provider_id
import bioseq.external.io.archive_source;

export namespace bioseq::external::io {

namespace prov = bioseq::external::provider;
namespace tx = bioseq::external::transport;

/// Returns whether an accession selects the NCBI Datasets archive path.
[[nodiscard]] inline bool is_datasets_package_accession(std::string_view accession) {
    return accession.starts_with("GCF_") || accession.starts_with("GCA_");
}

/// Downloads, validates, and streams one NCBI Datasets archive.
[[nodiscard]] inline std::expected<prov::provider_statistics, prov::external_error>
run_ncbi_datasets_stream(std::string_view accession,
                         prov::query_target target,
                         std::uint64_t chunk_bases,
                         prov::provider_message_sink_ref sink,
                         std::stop_token stop,
                         tx::remote_source_options options = {},
                         prov::external_execution_key execution_key = {}) {
    prov::provider_statistics stats;
    if (accession.empty()) {
        return std::unexpected(
            prov::make_external_error(prov::external_error_kind::unsupported_capability,
                                      ncbi_provider_id(),
                                      "datasets_download",
                                      "NCBI package retrieval requires an accession predicate"));
    }
    if (options.maximum_wire_bytes == 0) {
        options.maximum_wire_bytes = 64ull * 1024 * 1024; // bound the download
    }

    std::string url = "https://api.ncbi.nlm.nih.gov/datasets/v2/genome/accession/";
    url.append(accession);
    url.append("/download?include_annotation_type=GENOME_FASTA");

    prov::candidate_transfer_request_view candidate{};
    candidate.method = prov::transfer_method::get;
    candidate.url = url;
    candidate.is_replayable = true;
    candidate.is_idempotent = true;

    // Download the ZIP into a bounded buffer, retrying transient failures with
    // backoff. Retry only the download before any row is emitted. Extraction
    // emits rows and is therefore terminal on failure.
    std::vector<std::uint8_t> zip;
    constexpr int k_max_attempts = 5;
    for (int attempt = 0; attempt < k_max_attempts; ++attempt) {
        if (stop.stop_requested()) {
            return std::unexpected(prov::make_external_error(prov::external_error_kind::cancelled,
                                                             ncbi_provider_id(),
                                                             "datasets_download",
                                                             "package download cancelled"));
        }
        auto validated =
            tx::validate_candidate(candidate, ncbi_eutils_policy(), ncbi_provider_id());
        if (!validated.has_value()) {
            return std::unexpected(validated.error());
        }
        tx::remote_byte_source source(std::move(*validated), options, stop);
        zip.clear();
        std::array<std::uint8_t, 65536> buffer{};
        bool transport_failed = false;
        for (;;) {
            if (stop.stop_requested()) {
                return std::unexpected(
                    prov::make_external_error(prov::external_error_kind::cancelled,
                                              ncbi_provider_id(),
                                              "datasets_download",
                                              "package download cancelled"));
            }
            const bioseq::io::source_read_result result =
                source.read_into(buffer.data(), buffer.size());
            if (result.has_error) {
                transport_failed = true;
                break;
            }
            zip.insert(zip.end(), buffer.data(), buffer.data() + result.bytes_read);
            if (result.is_eof) {
                break;
            }
        }
        ++stats.requests;
        if (!transport_failed) {
            break; // download complete
        }
        const long status = source.get_status_code();
        const bool can_retry =
            attempt + 1 < k_max_attempts && (status == 429 || status == 408 || status >= 500);
        if (!can_retry) {
            return std::unexpected(prov::make_external_error(prov::external_error_kind::transport,
                                                             ncbi_provider_id(),
                                                             "datasets_download",
                                                             source.last_error_message()));
        }
        ++stats.retries;
        const long base_ms = 200L << attempt; // 200,400,800,1600
        std::this_thread::sleep_for(std::chrono::milliseconds(base_ms));
    }

    stats.wire_bytes = zip.size();

    // Extract + verify + emit. The reader's own limits cap per-member/total
    // decoded bytes and the zip-bomb expansion ratio. The wire cap already
    // bounded the compressed download.
    archive_read_limits limits;
    limits.maximum_total_decoded_bytes = 512ull * 1024 * 1024;
    // Key every emitted chunk by the queried assembly accession so the PG scan's
    // `accession = '<assembly>'` filter matches (the FASTA record's own accession
    // differs from the assembly accession, e.g. NC_045512.2 vs GCF_009858895.2).
    auto extracted = extract_fasta_members_from_zip_archive(
        std::span<const std::uint8_t>(zip.data(), zip.size()),
        limits,
        target,
        chunk_bases,
        sink,
        stop,
        accession,
        execution_key);
    if (!extracted.has_value()) {
        return std::unexpected(extracted.error());
    }
    stats.decoded_bytes = extracted->total_decoded_bytes;
    stats.rows = extracted->sequence_rows;
    stats.is_complete = true;
    return stats;
}

} // namespace bioseq::external::io
