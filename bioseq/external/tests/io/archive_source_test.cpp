/**
 * @file external/tests/io/archive_source_test.cpp
 * @brief Check archive member names, size limits, and checksum validation.
 */

// Build ZIP fixtures in memory to test member selection and checksum checks.
// Also check unsafe paths and archive size limits.

#include <archive.h>
#include <archive_entry.h>
#include <catch2/catch_test_macros.hpp>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

import bioseq.external.provider.errors;
import bioseq.external.provider.query;
import bioseq.external.provider.model;
import bioseq.external.provider.messages;
import bioseq.external.io.archive_source;

namespace exio = bioseq::external::io;
namespace prov = bioseq::external::provider;

namespace {

struct zip_member {
    std::string path;
    unsigned int filetype = AE_IFREG;
    std::string content;
    std::string symlink_target;
};

// Build a ZIP archive in memory from the given members.
std::vector<std::uint8_t> build_zip(const std::vector<zip_member>& members) {
    struct archive* a = archive_write_new();
    archive_write_set_format_zip(a);
    std::vector<char> scratch(1U << 20); // 1 MiB is ample for the fixtures
    std::size_t used = 0;
    archive_write_open_memory(a, scratch.data(), scratch.size(), &used);
    for (const zip_member& m : members) {
        struct archive_entry* e = archive_entry_new();
        archive_entry_set_pathname(e, m.path.c_str());
        archive_entry_set_filetype(e, m.filetype);
        archive_entry_set_perm(e, 0644);
        if (m.filetype == AE_IFLNK) {
            archive_entry_set_symlink(e, m.symlink_target.c_str());
            archive_entry_set_size(e, 0);
        } else {
            archive_entry_set_size(e, static_cast<la_int64_t>(m.content.size()));
        }
        archive_write_header(a, e);
        if (m.filetype != AE_IFLNK && !m.content.empty()) {
            archive_write_data(a, m.content.data(), m.content.size());
        }
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return std::vector<std::uint8_t>(scratch.begin(), scratch.begin() + static_cast<long>(used));
}

// Collect sequence counts, chunk positions, and member-completion details.
struct chunk_counter {
    std::uint64_t letters = 0;
    std::uint64_t chunks = 0;
    std::uint64_t asset_ends = 0;
    std::uint64_t record_ends = 0;
    std::string last_accession;
    std::string last_original_identifier;
    std::string last_md5;
    std::uint64_t last_record_count = 0;
    std::uint64_t last_decoded_bytes = 0;
    std::vector<std::uint64_t> chunk_index_seq; // every chunk_index, in order
    std::vector<std::uint64_t> base_offset_seq; // every base_offset, in order
    static std::expected<void, prov::external_error> push(void* self,
                                                          prov::owning_provider_message&& message) {
        auto* counter = static_cast<chunk_counter*>(self);
        if (auto* payload = std::get_if<prov::owning_sequence_payload>(&message)) {
            counter->letters += payload->bases.size();
            ++counter->chunks;
            if (payload->end_of_record)
                ++counter->record_ends;
            counter->chunk_index_seq.push_back(payload->chunk_index);
            counter->base_offset_seq.push_back(payload->base_offset);
            counter->last_accession = std::string(payload->key.accession.view());
            counter->last_original_identifier =
                std::string(payload->key.original_identifier.view());
        } else if (auto* end = std::get_if<prov::owning_sequence_asset_end>(&message)) {
            ++counter->asset_ends;
            counter->last_md5 = std::string(end->integrity.actual_value.view());
            counter->last_record_count = end->record_count;
            counter->last_decoded_bytes = end->decoded_bytes;
        }
        return {};
    }
    prov::provider_message_sink_ref ref() {
        return prov::provider_message_sink_ref{this, &chunk_counter::push};
    }
};

constexpr std::string_view k_fasta_path = "ncbi_dataset/data/GCF_000005845.2/cds.fna";

std::string fasta_body() {
    std::string body = ">lcl|test_cds\n";
    for (int i = 0; i < 64; ++i) {
        body += "ACGTACGTACGTACGTNNNNACGTACGTACGTACGTACGTACGT\n";
    }
    return body;
}

// Put two FASTA records in one archive  member. Their first header tokens,
// seg1 and seg2, identify the records.
std::string multi_record_body() {
    std::string body = ">seg1 first segment description\n";
    for (int i = 0; i < 10; ++i)
        body += "ACGTACGTACGT\n"; // 120 bases across ten lines
    body += ">seg2 second segment description\n";
    for (int i = 0; i < 5; ++i)
        body += "TTTTGGGGCCCC\n"; // 60 bases across five lines
    return body;
}

std::string catalog_selecting_fasta() {
    std::string c = "{\"assemblies\":[{\"files\":[{\"filePath\":\"";
    c += k_fasta_path;
    c += "\",\"fileType\":\"CDS_NUCLEOTIDE_FASTA\"}]}]}";
    return c;
}

// Select two FASTA members to check that chunk indices continue across
// files sharing one assembly accession
std::string catalog_selecting_two_fasta(std::string_view p1, std::string_view p2) {
    std::string c = "{\"assemblies\":[{\"files\":[{\"filePath\":\"";
    c += p1;
    c += "\",\"fileType\":\"CDS_NUCLEOTIDE_FASTA\"},{\"filePath\":\"";
    c += p2;
    c += "\",\"fileType\":\"GENOMIC_NUCLEOTIDE_FASTA\"}]}]}";
    return c;
}

std::string md5sum_line(std::string_view digest, std::string_view path) {
    std::string s(digest);
    s += "  ";
    s += path;
    s += "\n";
    return s;
}

// Count fixture bases while skipping FASTA headers and whitespace.
std::uint64_t expected_fasta_letters(std::string_view body) {
    std::uint64_t letters = 0;
    bool in_header = false;
    for (const char c : body) {
        if (c == '\n') {
            in_header = false;
            continue;
        }
        if (c == '>') {
            in_header = true;
            continue;
        }
        if (in_header) {
            continue;
        }
        if (c != '\r' && c != ' ' && c != '\t') {
            ++letters;
        }
    }
    return letters;
}

// Build a package whose catalog selects the FASTA and whose checksum matches it.
std::vector<zip_member> good_members() {
    const std::string body = fasta_body();
    const std::string digest = exio::compute_md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    std::vector<zip_member> members;
    members.push_back(
        {.path = "ncbi_dataset/data/dataset_catalog.json", .content = catalog_selecting_fasta()});
    members.push_back(
        {.path = "ncbi_dataset/data/md5sum.txt", .content = md5sum_line(digest, k_fasta_path)});
    members.push_back({.path = std::string(k_fasta_path), .content = body});
    return members;
}

// Use a catalog path relative to ncbi_dataset/data, with full paths in
// the ZIP and checksum file. All three must identify the same member.
constexpr std::string_view k_real_relative_path = "GCF_test.1/GCF_test.1_genomic.fna";
constexpr std::string_view k_real_full_path = "ncbi_dataset/data/GCF_test.1/GCF_test.1_genomic.fna";

std::vector<zip_member> real_layout_members() {
    const std::string body = fasta_body();
    const std::string digest = exio::compute_md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    std::string catalog = "{\"assemblies\":[{\"files\":[{\"filePath\":\"";
    catalog += k_real_relative_path; // Path relative to the  data directory.
    catalog += "\",\"fileType\":\"GENOMIC_NUCLEOTIDE_FASTA\"}]}]}";
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json", .content = catalog});
    members.push_back({.path = "md5sum.txt", // Checksum file at ZIP root, containing full member paths.
                       .content = md5sum_line(digest, k_real_full_path)});
    members.push_back({.path = std::string(k_real_full_path), .content = body});
    return members;
}

exio::archive_read_limits default_limits() {
    return exio::archive_read_limits{};
}

} // namespace

TEST_CASE("archive_source compute_md5_hex matches a known vector",
          "[component:external_api][layer:integration][feature:external_source]") {
    // RFC 1321 test vector: MD5("abc") = 900150983cd24fb0d6963f7d28e17f72.
    const std::string_view abc = "abc";
    const std::string hex = exio::compute_md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()));
    REQUIRE(hex == "900150983cd24fb0d6963f7d28e17f72");
}

TEST_CASE("archive_source member-name safety rejects escapes",
          "[component:external_api][layer:integration][feature:external_source]") {
    REQUIRE(exio::is_safe_archive_member_name("ncbi_dataset/data/x.fna", 4096));
    REQUIRE_FALSE(exio::is_safe_archive_member_name("../escape.fna", 4096));
    REQUIRE_FALSE(exio::is_safe_archive_member_name("a/../../escape", 4096));
    REQUIRE_FALSE(exio::is_safe_archive_member_name("/etc/passwd", 4096));
    REQUIRE_FALSE(exio::is_safe_archive_member_name("a\\b", 4096));
    REQUIRE_FALSE(exio::is_safe_archive_member_name("", 4096));
    REQUIRE_FALSE(exio::is_safe_archive_member_name("toolong", 3));
}

TEST_CASE("archive_source extracts and verifies a well-formed package",
          "[component:external_api][layer:integration][feature:external_source]") {
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     /*chunk_bases=*/1048572,
                                                     counter.ref(),
                                                     std::stop_token{});
    REQUIRE(result.has_value());
    REQUIRE(result->members_selected == 1);
    REQUIRE(result->members_verified == 1);
    REQUIRE(result->sequence_rows >= 1);
    // Check that the sink received the sequence bases with headers removed.
    REQUIRE(counter.letters == expected_fasta_letters(fasta_body()));
    REQUIRE(counter.letters > 0);
    REQUIRE(counter.chunks >= 1);
    // Decoded bytes include headers and line endings. The base count excludes them.
    REQUIRE(result->total_decoded_bytes == fasta_body().size());
    REQUIRE(counter.letters < result->total_decoded_bytes);
}

TEST_CASE("archive_source keys emitted chunks by the queried accession",
          "[component:external_api][layer:integration][feature:external_source]") {
    // Keep the queried assembly accessjon on emitted chunks so the scan
    // can apply its accession filter.
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{},
                                                     "GCF_009858895.2");
    REQUIRE(result.has_value());
    REQUIRE(counter.chunks >= 1);
    REQUIRE(counter.last_accession == "GCF_009858895.2");
}

TEST_CASE("archive_source emits one verified asset-end MD5 event per member",
          "[component:external_api][layer:integration][feature:external_source]") {
    // A verified member produces one completion message containing its MD5.
    const std::string body = fasta_body();
    const std::string digest = exio::compute_md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{},
                                                     "GCF_000005845.2");
    REQUIRE(result.has_value());
    REQUIRE(counter.asset_ends == 1);
    REQUIRE(counter.last_md5 == digest);
    REQUIRE(counter.last_record_count >= 1);
    REQUIRE(counter.last_decoded_bytes == body.size());
}

TEST_CASE("archive_source streams every record of a multi-record member",
          "[component:external_api][layer:integration][feature:external_source]") {
    // Both records use the queried assembly accession and each ends with
    // its own end_of_record marker.
    const std::string body = multi_record_body();
    const std::string digest = exio::compute_md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    std::vector<zip_member> members;
    members.push_back(
        {.path = "ncbi_dataset/data/dataset_catalog.json", .content = catalog_selecting_fasta()});
    members.push_back(
        {.path = "ncbi_dataset/data/md5sum.txt", .content = md5sum_line(digest, k_fasta_path)});
    members.push_back({.path = std::string(k_fasta_path), .content = body});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{},
                                                     "GCF_multi.1");
    REQUIRE(result.has_value());
    REQUIRE(counter.letters == expected_fasta_letters(body)); // both records
    REQUIRE(counter.record_ends == 2);                        // one end_of_record per record
    REQUIRE(counter.last_accession == "GCF_multi.1");         // assembly keys all chunks
    REQUIRE(counter.last_original_identifier == "seg2");      // last record's own id
    REQUIRE(counter.asset_ends == 1);                         // One completion message for the  member.
    REQUIRE(counter.last_record_count == 2);                  // Both records counted.
    // Each short record fits in one chunk. Chunk indices continue across
    // records, while, each record starts at base offset zero.
    REQUIRE(counter.chunk_index_seq == std::vector<std::uint64_t>{0, 1});
    REQUIRE(counter.base_offset_seq == std::vector<std::uint64_t>{0, 0});
}

TEST_CASE("archive_source multi-record whole-sequence target counts every row",
          "[component:external_api][layer:integration][feature:external_source]") {
    // Whole-record output reports one emitted row per FASTA record.
    const std::string body = multi_record_body();
    const std::string digest = exio::compute_md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    std::vector<zip_member> members;
    members.push_back(
        {.path = "ncbi_dataset/data/dataset_catalog.json", .content = catalog_selecting_fasta()});
    members.push_back(
        {.path = "ncbi_dataset/data/md5sum.txt", .content = md5sum_line(digest, k_fasta_path)});
    members.push_back({.path = std::string(k_fasta_path), .content = body});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip_archive(zip,
                                                                     default_limits(),
                                                                     prov::query_target::sequence,
                                                                     1048572,
                                                                     counter.ref(),
                                                                     std::stop_token{},
                                                                     "GCF_multi.1");
    REQUIRE(result.has_value());
    REQUIRE(result->sequence_rows == 2);
    REQUIRE(counter.chunks == 2);      // Two whole-record rows delivered.
    REQUIRE(counter.record_ends == 2); // each whole record is one end_of_record row
    REQUIRE(counter.letters == expected_fasta_letters(body));
    REQUIRE(counter.chunk_index_seq == std::vector<std::uint64_t>{0, 1});
}

TEST_CASE("archive_source carries chunk_index across multiple selected members",
          "[component:external_api][layer:integration][feature:external_source]") {
    // Multiple FASTA members under one accession share a contiguous chunk index
    // to keep the (accession, chunk_index) key unique.
    constexpr std::string_view k_first_path = "ncbi_dataset/data/GCF_x/cds.fna";
    constexpr std::string_view k_second_path = "ncbi_dataset/data/GCF_x/genomic.fna";
    const std::string body1 = multi_record_body(); // seg1(120) + seg2(60)
    std::string body2 = ">seg3 third segment\n";
    for (int i = 0; i < 9; ++i)
        body2 += "ACGTACGTAC\n"; // 90 bases across nine lines, one record
    const std::string d1 = exio::compute_md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body1.data()), body1.size()));
    const std::string d2 = exio::compute_md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body2.data()), body2.size()));
    std::string md5s = md5sum_line(d1, k_first_path);
    md5s += md5sum_line(d2, k_second_path);
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json",
                       .content = catalog_selecting_two_fasta(k_first_path, k_second_path)});
    members.push_back({.path = "ncbi_dataset/data/md5sum.txt", .content = md5s});
    members.push_back({.path = std::string(k_first_path), .content = body1});
    members.push_back({.path = std::string(k_second_path), .content = body2});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{},
                                                     "GCF_x.1");
    REQUIRE(result.has_value());
    REQUIRE(result->members_verified == 2);
    REQUIRE(counter.asset_ends == 2);  // one asset-end per member
    REQUIRE(counter.record_ends == 3); // seg1, seg2, seg3
    REQUIRE(counter.letters == 270);   // 120 + 60 + 90
    // chunk_index is contiguous across members and unique under one accession,
    // while base_offset resets for each record.
    REQUIRE(counter.chunk_index_seq == std::vector<std::uint64_t>{0, 1, 2});
    REQUIRE(counter.base_offset_seq == std::vector<std::uint64_t>{0, 0, 0});
}

TEST_CASE("archive_source reconciles NCBI's three path conventions",
          "[component:external_api][layer:integration][feature:external_source]") {
    // Match the relative catalog path with the full ZIP and checksum paths
    // so the member can be selected and verified.
    const std::vector<std::uint8_t> zip = build_zip(real_layout_members());
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{});
    REQUIRE(result.has_value());
    REQUIRE(result->members_selected == 1);
    REQUIRE(result->members_verified == 1);
    REQUIRE(counter.letters == expected_fasta_letters(fasta_body()));
    REQUIRE(counter.letters > 0);
}

TEST_CASE("archive_source rejects a path-traversal member",
          "[component:external_api][layer:integration][feature:external_source]") {
    std::vector<zip_member> members = good_members();
    members.push_back({.path = "../evil.fna", .content = ">x\nACGT\n"});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::provider_contract);
}

TEST_CASE("archive_source rejects an absolute-path member",
          "[component:external_api][layer:integration][feature:external_source]") {
    std::vector<zip_member> members = good_members();
    members.push_back({.path = "/etc/passwd", .content = "root\n"});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::provider_contract);
}

TEST_CASE("archive_source rejects a symlink member",
          "[component:external_api][layer:integration][feature:external_source]") {
    std::vector<zip_member> members = good_members();
    members.push_back({.path = "ncbi_dataset/data/link.fna",
                       .filetype = AE_IFLNK,
                       .symlink_target = "/etc/passwd"});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::provider_contract);
}

TEST_CASE("archive_source rejects a wrong md5sum digest (corruption)",
          "[component:external_api][layer:integration][feature:external_source]") {
    std::vector<zip_member> members;
    members.push_back(
        {.path = "ncbi_dataset/data/dataset_catalog.json", .content = catalog_selecting_fasta()});
    members.push_back({.path = "ncbi_dataset/data/md5sum.txt",
                       .content = md5sum_line("00000000000000000000000000000000", k_fasta_path)});
    members.push_back({.path = std::string(k_fasta_path), .content = fasta_body()});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::integrity);
    // No rows may reach the sink for an unverified member.
    REQUIRE(counter.letters == 0);
    // A failed member does not produce a successful asset-end.
    REQUIRE(counter.asset_ends == 0);
}

TEST_CASE("archive_source rejects a missing md5sum.txt",
          "[component:external_api][layer:integration][feature:external_source]") {
    std::vector<zip_member> members;
    members.push_back(
        {.path = "ncbi_dataset/data/dataset_catalog.json", .content = catalog_selecting_fasta()});
    members.push_back({.path = std::string(k_fasta_path), .content = fasta_body()});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result =
        exio::extract_fasta_members_from_zip_archive(zip,
                                                     default_limits(),
                                                     prov::query_target::sequence_chunk,
                                                     1048572,
                                                     counter.ref(),
                                                     std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::integrity);
}

TEST_CASE("archive_source enforces the maximum entry count",
          "[component:external_api][layer:integration][feature:external_source]") {
    exio::archive_read_limits limits = default_limits();
    limits.maximum_entries = 2; // the well-formed package has 3 members
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip_archive(
        zip, limits, prov::query_target::sequence_chunk, 1048572, counter.ref(), std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::response_too_large);
}

TEST_CASE("archive_source enforces the decoded/compressed expansion ratio",
          "[component:external_api][layer:integration][feature:external_source]") {
    exio::archive_read_limits limits = default_limits();
    // Set the expansion limit low enough to reject the FASTA fixture
    limits.maximum_expansion = {.numerator = 1, .denominator = 1000000, .grace_bytes = 0};
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip_archive(
        zip, limits, prov::query_target::sequence_chunk, 1048572, counter.ref(), std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::response_too_large);
}
