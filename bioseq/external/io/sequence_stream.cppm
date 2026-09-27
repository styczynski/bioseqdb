/**
 * @file external/io/sequence_stream.cppm
 * @brief Reads NCBI FASTA responses and sends sequence rows to a sink.
 */
module;

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

export module bioseq.external.io.sequence_stream;

import bioseq.external.provider.base;
import bioseq.external.provider.errors;
import bioseq.external.provider.model;
import bioseq.external.provider.query;
import bioseq.external.provider.messages;
import bioseq.external.provider.capabilities;
import bioseq.external.provider.locator;
import bioseq.external.transport.locator;
import bioseq.external.transport.remote_source;
import bioseq.io.sources; // io_source, source_read_result

/// FASTA parsing and NCBI sequence  streaming.
export namespace bioseq::external::io {

namespace prov = bioseq::external::provider;
namespace tx = bioseq::external::transport;

/// Default number of bases emitted in a sequence chunk.
inline constexpr std::uint64_t k_default_chunk_bases = 1048572; // 21*49932 <= 1 Mi

/// Returns the HTTPS and host restrictions for NCBI E-utilities requests.
inline prov::locator_policy_profile_view ncbi_eutils_policy() {
    static constexpr std::array<prov::locator_scheme, 1> schemes{prov::locator_scheme::https};
    static constexpr std::array<prov::locator_host_suffix_view, 1> suffixes{
        prov::locator_host_suffix_view{"ncbi.nlm.nih.gov", false}};
    prov::locator_policy_profile_view policy{};
    policy.id = prov::locator_policy_id{"ncbi.eutils"};
    policy.schemes = schemes;
    policy.host_suffixes = suffixes;
    policy.maximum_url_bytes = prov::maximum_url_bytes;
    policy.maximum_header_bytes = 16 * 1024;
    policy.maximum_redirects = 0;
    policy.permit_https_to_http_downgrade = false;
    policy.address_scope = prov::locator_address_scope::public_only;
    policy.proxy = prov::locator_proxy_policy::disabled;
    return policy;
}

/// Returns the provider ID ncbi.
inline prov::provider_id ncbi_provider_id() {
    static constexpr std::string_view value = "ncbi";
    return prov::provider_id{value};
}

/// Reads FASTA a block at a time and sends sequence rows to a sink.
///
/// Skips headers and whitespace. The sequence_chunk target splits records
/// into chunks, while the sequence target keeps each record in one row.
class fasta_chunk_emitter {
  public:
    /// Sets the accession, row size, and sink. Archive mode can emit several records.
    fasta_chunk_emitter(prov::query_target target,
                        std::uint64_t chunk_bases,
                        std::string_view accession,
                        prov::provider_message_sink_ref sink,
                        bool multi_record = false,
                        std::uint64_t start_chunk_index = 0)
        : target_(target), chunk_bases_(chunk_bases == 0 ? k_default_chunk_bases : chunk_bases),
          accession_(accession), sink_(sink), multi_record_(multi_record),
          chunk_index_(start_chunk_index) {}

    /// Reads FASTA bytes and emits ready rows when chunked output is requested.
    [[nodiscard]] std::expected<void, prov::external_error> feed(const std::uint8_t* data,
                                                                 std::size_t length) {
        for (std::size_t i = 0; i < length && !stopped_; ++i) {
            const char symbol = static_cast<char>(data[i]);
            if (symbol == '>') {
                if (record_count_ >= 1) {
                    // Efetch uses only the first record. Archive mode finishes the current
                    // record before reading the next one.
                    if (!multi_record_) {
                        stopped_ = true;
                        break;
                    }
                    // Send full chunks, then mark the remaining chunk as the record end.
                    if (target_ == prov::query_target::sequence_chunk) {
                        auto full = flush_full_chunks();
                        if (!full.has_value()) {
                            return std::unexpected(full.error());
                        }
                    }
                    if (!rechunk_.empty()) {
                        auto flushed = emit(rechunk_, /*end_of_record=*/true);
                        if (!flushed.has_value()) {
                            return std::unexpected(flushed.error());
                        }
                        rechunk_.clear();
                    }
                    base_offset_ = 0; // Per-record while chunk_index_ stays global.
                }
                ++record_count_;
                in_header_ = true;
                current_record_id_.clear();
                capturing_id_ = true;
                continue;
            }
            if (symbol == '\n') {
                in_header_ = false;
                capturing_id_ = false;
                continue;
            }
            if (in_header_) {
                if (capturing_id_) {
                    if (symbol == ' ' || symbol == '\t' || symbol == '\r') {
                        capturing_id_ = false; // first token = the record id
                    } else {
                        current_record_id_.push_back(symbol);
                    }
                }
                continue;
            }
            if (symbol == '\r' || symbol == ' ' || symbol == '\t') {
                continue;
            }
            rechunk_.push_back(symbol);
            ++total_letters_;
        }
        if (rechunk_.size() > peak_rechunk_bytes_) {
            peak_rechunk_bytes_ = rechunk_.size();
        }
        if (target_ == prov::query_target::sequence_chunk) {
            return flush_full_chunks();
        }
        return {};
    }

    /// Returns the largest rechunk buffer size observed after  a feed call.
    [[nodiscard]] std::uint64_t peak_rechunk_bytes() const noexcept {
        return peak_rechunk_bytes_;
    }

    /// Emits the last record tail and returns the total number of rows sent.
    [[nodiscard]] std::expected<std::uint64_t, prov::external_error> finish() {
        if (total_letters_ == 0) {
            // Name the failing operation, according to the FASTA source.
            return std::unexpected(prov::make_external_error(
                prov::external_error_kind::parse,
                ncbi_provider_id(),
                multi_record_ ? "datasets_zip_fasta" : "efetch_nuccore_fasta",
                multi_record_ ? "NCBI Datasets archive member contained no sequence bases"
                              : "NCBI efetch returned no sequence bases"));
        }
        // Send the nonempty tail. rows_ already includes earlier records.
        if (!rechunk_.empty()) {
            auto pushed = emit(rechunk_, /*end_of_record=*/true);
            if (!pushed.has_value()) {
                return std::unexpected(pushed.error());
            }
            rechunk_.clear();
        }
        return rows_;
    }

    // Retry a download only before sending the first row, since replaying
    // a partly emitted retponse would duplicate rows.
    /// Returns the number of emitted sequence rows.
    [[nodiscard]] std::uint64_t emitted_rows() const noexcept {
        return rows_;
    }

    // Count records when their final chunk reaches the sink. Empty FASTA
    // records therefore do not increase the asset-end record count
    /// Returns the number of completed FASTA records.
    [[nodiscard]] std::uint64_t records_seen() const noexcept {
        return records_emitted_;
    }

    // Pass  this index to the next archive member so chunk IDs stay unique
    // across all members retrieved for the accession.
    /// Returns the index to use for the next sequence chunk.
    [[nodiscard]] std::uint64_t next_chunk_index() const noexcept {
        return chunk_index_;
    }

  private:
    /// Sends full chunks while keeping one chunk to mark the end of the record.
    [[nodiscard]] std::expected<void, prov::external_error> flush_full_chunks() {
        while (rechunk_.size() > chunk_bases_) {
            const std::string_view chunk(rechunk_.data(), static_cast<std::size_t>(chunk_bases_));
            auto pushed = emit(chunk, /*end_of_record=*/false);
            if (!pushed.has_value()) {
                return std::unexpected(pushed.error());
            }
            rechunk_.erase(0, static_cast<std::size_t>(chunk_bases_));
        }
        return {};
    }

    /// Copies sequence text into a row and advances offsets after the sink accepts it.
    [[nodiscard]] std::expected<void, prov::external_error> emit(std::string_view letters,
                                                                 bool end_of_record) {
        prov::owning_sequence_payload payload;
        payload.provider = ncbi_provider_id();
        payload.provider_name =
            prov::bounded_string<prov::maximum_provider_name_bytes>::make_truncated(
                "NCBI Datasets");
        payload.molecule = prov::molecule_kind::nucleotide;
        payload.key.provider = ncbi_provider_id();
        payload.key.provider_name = payload.provider_name;
        payload.key.domain =
            prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated("sequence");
        // For an archive record, keep its FASTA ID, such as NC_002023.1, as
        // original_identifier. Keep the queried accession in key.accession so
        // PostgreSQL can still match the scan's accession filter.
        payload.key.original_identifier =
            prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(
                (multi_record_ && !current_record_id_.empty()) ? current_record_id_ : accession_);
        payload.key.accession =
            prov::bounded_string<prov::maximum_identifier_bytes>::make_truncated(accession_);
        payload.strand = prov::genomic_interval::strand::forward;
        payload.bases.assign(letters.data(), letters.size());
        payload.chunk_index = chunk_index_;
        payload.base_offset = base_offset_;
        payload.end_of_record = end_of_record;
        payload.audit.completed_at = std::chrono::time_point_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now());
        const std::size_t bytes = payload.measure_deep_size_bytes();
        (void)bytes;
        const auto pushed =
            sink_.push_fn(sink_.object, prov::owning_provider_message{std::move(payload)});
        if (!pushed.has_value()) {
            return std::unexpected(pushed.error());
        }
        base_offset_ += letters.size();
        ++chunk_index_;
        ++rows_;
        if (end_of_record) {
            ++records_emitted_; // count a record after its final chunk  is accepted
        }
        return {};
    }

    prov::query_target target_;
    std::uint64_t chunk_bases_;
    std::string accession_;
    prov::provider_message_sink_ref sink_;
    std::string rechunk_;
    std::uint64_t chunk_index_ = 0;
    std::uint64_t base_offset_ = 0;
    std::uint64_t total_letters_ = 0;
    std::uint64_t rows_ = 0;
    std::uint64_t record_count_ = 0;
    std::uint64_t records_emitted_ = 0;
    std::size_t peak_rechunk_bytes_ = 0;
    bool in_header_ = false;
    bool stopped_ = false;
    bool multi_record_ = false;
    bool capturing_id_ = false;
    std::string current_record_id_;
};

// Use the first string value from an accession equality comparison.
/// Finds an accession equality value, or returns no value if none is present.
[[nodiscard]] inline std::optional<std::string>
accession_from_query(const prov::normalized_external_query& query) {
    for (const prov::predicate_node& node : query.predicates) {
        const auto* comparison = std::get_if<prov::comparison_predicate>(&node.leaf);
        if (comparison == nullptr) {
            continue;
        }
        if (comparison->field != prov::external_field::accession ||
            comparison->operation != prov::predicate_operator::equal ||
            comparison->values.empty()) {
            continue;
        }
        if (const auto* text = std::get_if<prov::bounded_string<prov::maximum_identifier_bytes>>(
                &comparison->values.front())) {
            return std::string(text->view());
        }
    }
    return std::nullopt;
}

// The PostgreSQL scan runs this download in a proucer thread and receives
// rows through a channel. Chunked output lets it read sequences as they arrive.
/// Downloads NCBI nucleotide FASTA and sends sequence rows to the sink.
[[nodiscard]] inline std::expected<prov::provider_statistics, prov::external_error>
run_ncbi_sequence_stream(std::string_view accession,
                         prov::query_target target,
                         std::uint64_t chunk_bases,
                         prov::provider_message_sink_ref sink,
                         std::stop_token stop,
                         tx::remote_source_options options = {}) {
    prov::provider_statistics stats;
    if (accession.empty()) {
        return std::unexpected(
            prov::make_external_error(prov::external_error_kind::unsupported_capability,
                                      ncbi_provider_id(),
                                      "efetch_nuccore_fasta",
                                      "NCBI sequence retrieval requires an accession predicate"));
    }

    std::string url = "https://eutils.ncbi.nlm.nih.gov/entrez/eutils/"
                      "efetch.fcgi?db=nuccore&id=";
    url.append(accession);
    url.append("&rettype=fasta&retmode=text");

    prov::candidate_transfer_request_view candidate{};
    candidate.method = prov::transfer_method::get;
    candidate.url = url;
    candidate.is_replayable = true;
    candidate.is_idempotent = true;

    // Retry temporary HTTP or transport failures with increasing delays.
    // Stop retrying after any row reaches the sink, since restarting would
    // duplicate  sequence chunks already delivered to PostgreSQL.
    constexpr int k_max_attempts = 5;
    for (int attempt = 0; attempt < k_max_attempts; ++attempt) {
        if (attempt != 0) {
            const long base_ms = 200L << (attempt - 1); // retry delay in milliseconds
            const long jitter_ms =
                static_cast<long>((accession.size() * 37u + attempt * 53u) % 200u);
            std::this_thread::sleep_for(std::chrono::milliseconds(base_ms + jitter_ms));
        }
        if (stop.stop_requested()) {
            return std::unexpected(prov::make_external_error(prov::external_error_kind::cancelled,
                                                             ncbi_provider_id(),
                                                             "efetch_nuccore_fasta",
                                                             "sequence stream cancelled"));
        }

        auto validated =
            tx::validate_candidate(candidate, ncbi_eutils_policy(), ncbi_provider_id());
        if (!validated.has_value()) {
            return std::unexpected(validated.error());
        }

        tx::remote_byte_source source(std::move(*validated), options, stop);
        fasta_chunk_emitter emitter(target, chunk_bases, accession, sink);

        std::array<std::uint8_t, 65536> buffer{};
        bool transport_failed = false;
        for (;;) {
            if (stop.stop_requested()) {
                return std::unexpected(
                    prov::make_external_error(prov::external_error_kind::cancelled,
                                              ncbi_provider_id(),
                                              "efetch_nuccore_fasta",
                                              "sequence stream cancelled"));
            }
            const bioseq::io::source_read_result result =
                source.read_into(buffer.data(), buffer.size());
            if (result.has_error) {
                transport_failed = true;
                break;
            }
            auto fed = emitter.feed(buffer.data(), result.bytes_read);
            if (!fed.has_value()) {
                return std::unexpected(fed.error());
            }
            if (result.is_eof) {
                break;
            }
        }

        if (transport_failed) {
            const long status = source.get_status_code();
            const bool can_retry =
                emitter.emitted_rows() == 0 && attempt + 1 < k_max_attempts &&
                (status == 429 || status == 408 || status == 500 || status == 502 ||
                 status == 503 || status == 504 ||
                 status == 0 /* transport failure before an HTTP status was received */);
            if (can_retry) {
                continue;
            }
            const auto kind = (status == 429) ? prov::external_error_kind::rate_limited
                                              : prov::external_error_kind::transport;
            return std::unexpected(prov::make_external_error(
                kind, ncbi_provider_id(), "efetch_nuccore_fasta", source.last_error_message()));
        }

        auto rows = emitter.finish();
        if (!rows.has_value()) {
            return std::unexpected(rows.error());
        }
        stats.requests = static_cast<std::uint64_t>(attempt) + 1;
        stats.retries = static_cast<std::uint64_t>(attempt);
        stats.wire_bytes = source.count_wire_bytes();
        stats.decoded_bytes = source.count_decoded_bytes();
        stats.rows = *rows;
        stats.is_complete = true;
        return stats;
    }

    return std::unexpected(
        prov::make_external_error(prov::external_error_kind::rate_limited,
                                  ncbi_provider_id(),
                                  "efetch_nuccore_fasta",
                                  "NCBI efetch ret/rate-limit attempts exhausted"));
}

} // namespace bioseq::external::io
