/**
 * @file external/io/datasets_stream.cppm
 * @brief Downloads NCBI Datasets ZIP packages and emits verified FASTA chunks.
 */
module;



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

/// Selects the Datasets path for accessions beginning with GCF_ or GCA_.
[[nodiscard]] inline bool is_datasets_package_accession(std::string_view accession) {
    return accession.starts_with("GCF_") || accession.starts_with("GCA_");
}

/**
 * Downloads an assembly's NCBI Datasets package and emits its selected FASTA chunks.
 *
 * Buffers the ZIP before extraction. Retryable HTTP failures can restart the
 * download up to the attempt limit, while extraction failures return directly
 * because earlier members may already have emitted rows. Each member's MD5
 * is checked before its own rows are sent to sink.
 * @return Transfer and row statistics, or a download, extraction, or sink error.
 */
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
        options.maximum_wire_bytes = 64ull * 1024 * 1024; // Default compressed download limit.
    }

    std::string url = "https://api.ncbi.nlm.nih.gov/datasets/v2/genome/accession/";
    url.append(accession);
    url.append("/download?include_annotation_type=GENOME_FASTA");

    prov::candidate_transfer_request_view candidate{};
    candidate.method = prov::transfer_method::get;
    candidate.url = url;
    candidate.is_replayable = true;
    candidate.is_idempotent = true;

    // Retry HTTP 408, 429, and server errors before extraction starts.
    // Once rows can reach the sink, a failure ends this operation.
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

    // Limit decoded sequence data and its expansion relative to the ZIP.
    // The transport has already limited the compressed download.
    archive_read_limits limits;
    limits.maximum_total_decoded_bytes = 512ull * 1024 * 1024;
    // Use the queried assembly accession for emitted chunks so the SQL filter
    // matches. FASTA record accessions can differ, such as NC_045512.2 within
    // assembly GCF_009858895.2.
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
