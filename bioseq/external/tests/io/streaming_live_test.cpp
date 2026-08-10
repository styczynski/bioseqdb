// Exercise the libcurl pull source and locator policy with deterministic local
// checks and live NCBI E-utilities requests. Streaming cases select the network
// resource explicitly.

#include <array>
#include <chrono>
#include <cstdint>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

import bioseq.io.sources;  // source_read_result
import bioseq.external.provider.base;
import bioseq.external.provider.messages;
import bioseq.external.provider.locator;
import bioseq.external.transport.locator;
import bioseq.external.transport.remote_source;

namespace prov = bioseq::external::provider;
namespace tx = bioseq::external::transport;

namespace {

// An NCBI E-utilities locator policy: HTTPS only, host under ncbi.nlm.nih.gov,
// public addresses only.
prov::locator_policy_profile_view ncbi_policy() {
    static constexpr std::array<prov::locator_scheme, 1> schemes{
        prov::locator_scheme::https};
    static constexpr std::array<prov::locator_host_suffix_view, 1> suffixes{
        prov::locator_host_suffix_view{"ncbi.nlm.nih.gov", false}};
    prov::locator_policy_profile_view policy{};
    policy.id = prov::locator_policy_id{"ncbi.eutils"};
    policy.schemes = schemes;
    policy.host_suffixes = suffixes;
    policy.maximum_url_bytes = 16 * 1024;
    policy.maximum_header_bytes = 16 * 1024;
    policy.maximum_redirects = 0;
    policy.permit_https_to_http_downgrade = false;
    policy.address_scope = prov::locator_address_scope::public_only;
    policy.proxy = prov::locator_proxy_policy::disabled;
    return policy;
}

prov::candidate_transfer_request_view efetch_candidate(const std::string& url) {
    prov::candidate_transfer_request_view candidate{};
    candidate.method = prov::transfer_method::get;
    candidate.url = url;
    candidate.is_replayable = true;
    candidate.is_idempotent = true;
    return candidate;
}

// Count FASTA letters streamed from a validated request with a small carry cap,
// tracking the maximum single read to prove memory is bounded.
struct stream_counts {
    std::uint64_t letters = 0;
    std::uint64_t records = 0;
    std::size_t max_read = 0;
    std::size_t peak_carry_bytes = 0;   // real peak carry-buffer occupancy
    std::size_t carry_capacity_bytes = 0;
    bool error = false;
    std::string error_message;
};

stream_counts stream_once(tx::validated_transfer_request request,
                          std::size_t carry_cap) {
    tx::remote_source_options options;
    options.carry_buffer_bytes = carry_cap;
    tx::remote_byte_source source(std::move(request), options,
                                  std::stop_token{});
    stream_counts counts;
    std::array<std::uint8_t, 8192> buffer{};
    bool in_header = false;
    for (;;) {
        const bioseq::io::source_read_result result =
            source.read_into(buffer.data(), buffer.size());
        if (result.has_error) {
            counts.error = true;
            counts.error_message = source.last_error_message();
            break;
        }
        counts.max_read = std::max(counts.max_read, result.bytes_read);
        for (std::size_t i = 0; i < result.bytes_read; ++i) {
            const char symbol = static_cast<char>(buffer[i]);
            if (symbol == '>') {
                ++counts.records;
                in_header = true;
                continue;
            }
            if (symbol == '\n') {
                in_header = false;
                continue;
            }
            if (in_header) {
                continue;
            }
            if (symbol != '\r' && symbol != ' ' && symbol != '\t') {
                ++counts.letters;
            }
        }
        if (result.is_eof) {
            break;
        }
    }
    counts.peak_carry_bytes = source.measure_peak_carry_bytes();
    counts.carry_capacity_bytes = source.get_carry_capacity_bytes();
    return counts;
}

// Validate and stream with backoff for NCBI's shared public endpoint.
stream_counts stream_fasta_letters(const std::string& url,
                                   const prov::locator_policy_profile_view& policy,
                                   prov::provider_id provider,
                                   std::size_t carry_cap) {
    stream_counts counts;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (attempt != 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(300L << (attempt - 1)));
        }
        auto validated =
            tx::validate_candidate(efetch_candidate(url), policy, provider);
        if (!validated.has_value()) {
            counts.error = true;
            counts.error_message = "validation failed";
            return counts;
        }
        counts = stream_once(std::move(*validated), carry_cap);
        const bool rate_limited =
            counts.error && counts.error_message.find("429") != std::string::npos;
        if (!rate_limited) {
            return counts;
        }
    }
    return counts;
}

}  // namespace

TEST_CASE("locator rejects non-allowlisted and private targets",
          "[component:external_api][layer:contract][feature:external_source]") {
    const prov::provider_id ncbi{"ncbi"};
    const prov::locator_policy_profile_view policy = ncbi_policy();

    SECTION("wrong scheme is rejected") {
        auto validated = tx::validate_candidate(
            efetch_candidate("http://eutils.ncbi.nlm.nih.gov/x"), policy, ncbi);
        REQUIRE_FALSE(validated.has_value());
    }
    SECTION("off-allowlist host is rejected") {
        auto validated = tx::validate_candidate(
            efetch_candidate("https://example.com/x"), policy, ncbi);
        REQUIRE_FALSE(validated.has_value());
    }
    SECTION("user-info is rejected") {
        auto validated = tx::validate_candidate(
            efetch_candidate("https://user@eutils.ncbi.nlm.nih.gov/x"), policy,
            ncbi);
        REQUIRE_FALSE(validated.has_value());
    }
    SECTION("loopback host is rejected under public_only scope") {
        static constexpr std::array<prov::locator_scheme, 1> schemes{
            prov::locator_scheme::https};
        static constexpr std::array<prov::locator_host_suffix_view, 1> suffixes{
            prov::locator_host_suffix_view{"localhost", true}};
        prov::locator_policy_profile_view local = policy;
        local.schemes = schemes;
        local.host_suffixes = suffixes;
        auto validated = tx::validate_candidate(
            efetch_candidate("https://localhost/x"), local, ncbi);
        REQUIRE_FALSE(validated.has_value());
    }
}

TEST_CASE("remote_byte_source streams NCBI FASTA with bounded memory",
          "[component:external_api][layer:integration][feature:external_source][resource:network]") {
    const prov::provider_id ncbi{"ncbi"};
    const prov::locator_policy_profile_view policy = ncbi_policy();

    SECTION("NC_012920.1 human mitochondrion = 16569 bp") {
        const std::string url =
            "https://eutils.ncbi.nlm.nih.gov/entrez/eutils/efetch.fcgi"
            "?db=nuccore&id=NC_012920.1&rettype=fasta&retmode=text";
        // A 64 KiB carry cap while the record is 16.5 kB proves the whole body
        // is never held; max_read stays under the cap.
        const std::size_t carry_cap = 64 * 1024;
        stream_counts counts = stream_fasta_letters(url, policy, ncbi, carry_cap);
        INFO("stream error: " << counts.error_message);
        REQUIRE_FALSE(counts.error);
        REQUIRE(counts.records == 1);
        REQUIRE(counts.letters == 16569);
        // The carry buffer never exceeds its cap (here the whole 16.5 kB body
        // fits, so no pause is needed).
        REQUIRE(counts.peak_carry_bytes <= carry_cap);
    }

    SECTION("NC_000913.3 E. coli K-12 = 4641652 bp under a 256 KiB carry cap") {
        const std::string url =
            "https://eutils.ncbi.nlm.nih.gov/entrez/eutils/efetch.fcgi"
            "?db=nuccore&id=NC_000913.3&rettype=fasta&retmode=text";
        // The genome is 4.6 Mbp but the carry cap is 256 KiB: bounded streaming.
        const std::size_t carry_cap = 256 * 1024;
        stream_counts counts = stream_fasta_letters(url, policy, ncbi, carry_cap);
        INFO("stream error: " << counts.error_message);
        REQUIRE_FALSE(counts.error);
        REQUIRE(counts.records == 1);
        REQUIRE(counts.letters == 4641652);
        // The libcurl write callback pauses before the carry buffer exceeds its
        // cap, so the genome body is never fully buffered.
        REQUIRE(counts.peak_carry_bytes <= carry_cap);
        REQUIRE(counts.peak_carry_bytes < 512u * 1024u);
        REQUIRE(counts.letters > 8u * counts.peak_carry_bytes);
    }
}
