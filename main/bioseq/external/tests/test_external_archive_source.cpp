// DOC0372 — archive_source (NCBI hydrated-ZIP reader) offline test.
// Builds fixture ZIPs in-memory with libarchive's write API and exercises the
// spec test #16 matrix: ZIP path traversal / symlink rejection, entry & decoded
// caps, dataset_catalog.json member selection, decoded-member md5sum validation,
// corruption, and the verified happy path. No network, no .so — this compiles
// into the prebuilt bioseq_external_integration_tests_exec.
// Tag: [external][archive].

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include <catch2/catch_test_macros.hpp>

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
    std::vector<char> scratch(1U << 20);  // 1 MiB is ample for the fixtures
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
            archive_entry_set_size(
                e, static_cast<la_int64_t>(m.content.size()));
        }
        archive_write_header(a, e);
        if (m.filetype != AE_IFLNK && !m.content.empty()) {
            archive_write_data(a, m.content.data(), m.content.size());
        }
        archive_entry_free(e);
    }
    archive_write_close(a);
    archive_write_free(a);
    return std::vector<std::uint8_t>(scratch.begin(),
                                     scratch.begin() + static_cast<long>(used));
}

// A sink that just tallies the emitted sequence letters and chunk rows.
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
    std::vector<std::uint64_t> chunk_index_seq;   // every chunk_index, in order
    std::vector<std::uint64_t> base_offset_seq;   // every base_offset, in order
    static std::expected<void, prov::external_error> push(
        void* self, prov::owning_provider_message&& message) {
        auto* counter = static_cast<chunk_counter*>(self);
        if (auto* payload =
                std::get_if<prov::owning_sequence_payload>(&message)) {
            counter->letters += payload->bases.size();
            ++counter->chunks;
            if (payload->end_of_record) ++counter->record_ends;
            counter->chunk_index_seq.push_back(payload->chunk_index);
            counter->base_offset_seq.push_back(payload->base_offset);
            counter->last_accession =
                std::string(payload->key.accession.view());
            counter->last_original_identifier =
                std::string(payload->key.original_identifier.view());
        } else if (auto* end = std::get_if<prov::owning_sequence_asset_end>(
                       &message)) {
            ++counter->asset_ends;
            counter->last_md5 =
                std::string(end->integrity.actual_value.view());
            counter->last_record_count = end->record_count;
            counter->last_decoded_bytes = end->decoded_bytes;
        }
        return {};
    }
    prov::provider_message_sink_ref ref() {
        return prov::provider_message_sink_ref{this, &chunk_counter::push};
    }
};

constexpr std::string_view kFastaPath =
    "ncbi_dataset/data/GCF_000005845.2/cds.fna";

std::string fasta_body() {
    std::string body = ">lcl|test_cds\n";
    for (int i = 0; i < 64; ++i) {
        body += "ACGTACGTACGTACGTNNNNACGTACGTACGTACGTACGTACGT\n";
    }
    return body;
}

// Two FASTA records in one member (mirrors an NCBI Datasets multi-segment
// genomic.fna). Record ids are the first header token: "seg1", "seg2".
std::string multi_record_body() {
    std::string body = ">seg1 first segment description\n";
    for (int i = 0; i < 10; ++i) body += "ACGTACGTACGT\n";  // 120 bases
    body += ">seg2 second segment description\n";
    for (int i = 0; i < 5; ++i) body += "TTTTGGGGCCCC\n";  // 60 bases
    return body;
}

std::string catalog_selecting_fasta() {
    std::string c = "{\"assemblies\":[{\"files\":[{\"filePath\":\"";
    c += kFastaPath;
    c += "\",\"fileType\":\"CDS_NUCLEOTIDE_FASTA\"}]}]}";
    return c;
}

// A catalog that selects TWO FASTA members (both FASTA file types) so the archive
// reader emits both under one accession — exercises the cross-member chunk_index
// carry that keeps (accession, chunk_index) unique.
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

// Mirror the emitter's stripping: count non-whitespace bytes on lines that do
// not begin a FASTA header ('>').
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

// A well-formed hydrated package: catalog selects the FASTA, md5sum.txt carries
// its true digest.
std::vector<zip_member> good_members() {
    const std::string body = fasta_body();
    const std::string digest = exio::md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json",
                       .content = catalog_selecting_fasta()});
    members.push_back({.path = "ncbi_dataset/data/md5sum.txt",
                       .content = md5sum_line(digest, kFastaPath)});
    members.push_back({.path = std::string(kFastaPath), .content = body});
    return members;
}

// Mirrors the REAL NCBI Datasets layout: the ZIP entry and md5sum.txt use the
// full "ncbi_dataset/data/<path>" while dataset_catalog.json filePath is relative
// to that data dir. Exercises the reader's three-way path canonicalization.
constexpr std::string_view kRealRelPath = "GCF_test.1/GCF_test.1_genomic.fna";
constexpr std::string_view kRealFullPath =
    "ncbi_dataset/data/GCF_test.1/GCF_test.1_genomic.fna";

std::vector<zip_member> real_layout_members() {
    const std::string body = fasta_body();
    const std::string digest = exio::md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    std::string catalog = "{\"assemblies\":[{\"files\":[{\"filePath\":\"";
    catalog += kRealRelPath;  // data-dir-relative, as NCBI emits it
    catalog += "\",\"fileType\":\"GENOMIC_NUCLEOTIDE_FASTA\"}]}]}";
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json",
                       .content = catalog});
    members.push_back({.path = "md5sum.txt",  // root, full-path key
                       .content = md5sum_line(digest, kRealFullPath)});
    members.push_back({.path = std::string(kRealFullPath), .content = body});
    return members;
}

exio::archive_read_limits default_limits() { return exio::archive_read_limits{}; }

}  // namespace

TEST_CASE("archive_source md5_hex matches a known vector", "[external][archive]") {
    // RFC 1321 test vector: MD5("abc") = 900150983cd24fb0d6963f7d28e17f72.
    const std::string_view abc = "abc";
    const std::string hex = exio::md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size()));
    REQUIRE(hex == "900150983cd24fb0d6963f7d28e17f72");
}

TEST_CASE("archive_source member-name safety rejects escapes",
          "[external][archive]") {
    REQUIRE(exio::archive_member_name_is_safe("ncbi_dataset/data/x.fna", 4096));
    REQUIRE_FALSE(exio::archive_member_name_is_safe("../escape.fna", 4096));
    REQUIRE_FALSE(exio::archive_member_name_is_safe("a/../../escape", 4096));
    REQUIRE_FALSE(exio::archive_member_name_is_safe("/etc/passwd", 4096));
    REQUIRE_FALSE(exio::archive_member_name_is_safe("a\\b", 4096));
    REQUIRE_FALSE(exio::archive_member_name_is_safe("", 4096));
    REQUIRE_FALSE(exio::archive_member_name_is_safe("toolong", 3));
}

TEST_CASE("archive_source extracts and verifies a well-formed package",
          "[external][archive]") {
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk,
        /*chunk_bases=*/1048572, counter.ref(), std::stop_token{});
    REQUIRE(result.has_value());
    REQUIRE(result->members_selected == 1);
    REQUIRE(result->members_verified == 1);
    REQUIRE(result->sequence_rows >= 1);
    // The emitter strips the '>' header and whitespace; letters equal the raw
    // base count and must have actually flowed to the sink.
    REQUIRE(counter.letters == expected_fasta_letters(fasta_body()));
    REQUIRE(counter.letters > 0);
    REQUIRE(counter.chunks >= 1);
    // total_decoded_bytes is the full member byte count (header + newlines
    // included), so it strictly exceeds the stripped letter count.
    REQUIRE(result->total_decoded_bytes == fasta_body().size());
    REQUIRE(counter.letters < result->total_decoded_bytes);
}

TEST_CASE("archive_source keys emitted chunks by the queried accession",
          "[external][archive]") {
    // Regression guard for the PG 0-rows bug: emitted chunks must be keyed by the
    // assembly accession the user queried (so the scan's `accession = '<assembly>'`
    // filter matches), NOT the member file path.
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{}, "GCF_009858895.2");
    REQUIRE(result.has_value());
    REQUIRE(counter.chunks >= 1);
    REQUIRE(counter.last_accession == "GCF_009858895.2");
}

TEST_CASE("archive_source emits one verified asset-end MD5 event per member",
          "[external][archive]") {
    // spec §3179-3213, test #29: a fully-decoded, MD5-verified member produces
    // exactly one terminal asset-end carrying the decoded-member digest.
    const std::string body = fasta_body();
    const std::string digest = exio::md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{}, "GCF_000005845.2");
    REQUIRE(result.has_value());
    REQUIRE(counter.asset_ends == 1);
    REQUIRE(counter.last_md5 == digest);
    REQUIRE(counter.last_record_count >= 1);
    REQUIRE(counter.last_decoded_bytes == body.size());
}

TEST_CASE("archive_source streams every record of a multi-record member",
          "[external][archive]") {
    // Reviewer HIGH 2a: a multi-record member (an NCBI assembly genomic.fna) must
    // stream ALL records, not just the first — keyed by the queried assembly
    // accession, each record ending in its own end_of_record chunk.
    const std::string body = multi_record_body();
    const std::string digest = exio::md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json",
                       .content = catalog_selecting_fasta()});
    members.push_back({.path = "ncbi_dataset/data/md5sum.txt",
                       .content = md5sum_line(digest, kFastaPath)});
    members.push_back({.path = std::string(kFastaPath), .content = body});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{}, "GCF_multi.1");
    REQUIRE(result.has_value());
    REQUIRE(counter.letters == expected_fasta_letters(body));  // both records
    REQUIRE(counter.record_ends == 2);          // one end_of_record per record
    REQUIRE(counter.last_accession == "GCF_multi.1");   // assembly keys all chunks
    REQUIRE(counter.last_original_identifier == "seg2");  // last record's own id
    REQUIRE(counter.asset_ends == 1);           // still one asset-end per member
    REQUIRE(counter.last_record_count == 2);    // true record count, uncapped
    // The two invariants the PG proof headlines, now asserted at unit level: each
    // record here is < chunk_bases so it is exactly one chunk. chunk_index is global
    // and contiguous (0,1) — unique across records so PG (accession,chunk_index)
    // never collides — and base_offset resets to 0 at each record boundary.
    REQUIRE(counter.chunk_index_seq == std::vector<std::uint64_t>{0, 1});
    REQUIRE(counter.base_offset_seq == std::vector<std::uint64_t>{0, 0});
}

TEST_CASE("archive_source multi-record whole-sequence target counts every row",
          "[external][archive]") {
    // Reviewer A HIGH: for the `sequence` (whole-record) target on a multi-record
    // member, finish() must return the TRUE row count (N), not a literal 1 — the
    // earlier records were already emitted at their `>` boundaries. Two records must
    // report sequence_rows == 2 and deliver two whole-record rows. (Before the fix
    // this undercounted to 1 while 2 rows were pushed to the sink.)
    const std::string body = multi_record_body();
    const std::string digest = exio::md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body.data()), body.size()));
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json",
                       .content = catalog_selecting_fasta()});
    members.push_back({.path = "ncbi_dataset/data/md5sum.txt",
                       .content = md5sum_line(digest, kFastaPath)});
    members.push_back({.path = std::string(kFastaPath), .content = body});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence, 1048572,
        counter.ref(), std::stop_token{}, "GCF_multi.1");
    REQUIRE(result.has_value());
    REQUIRE(result->sequence_rows == 2);   // the fix: true count, not literal 1
    REQUIRE(counter.chunks == 2);          // two whole-record rows actually delivered
    REQUIRE(counter.record_ends == 2);     // each whole record is one end_of_record row
    REQUIRE(counter.letters == expected_fasta_letters(body));
    REQUIRE(counter.chunk_index_seq == std::vector<std::uint64_t>{0, 1});
}

TEST_CASE("archive_source carries chunk_index across multiple selected members",
          "[external][archive]") {
    // Reviewer A MEDIUM: when more than one FASTA member is selected under one
    // accession, each member uses a fresh emitter — their chunk_index must NOT both
    // restart at 0, or PG's (accession, chunk_index) key would collide. The reader
    // threads a running index across members. Two members (2 records + 1 record)
    // must yield contiguous chunk_index 0,1,2.
    constexpr std::string_view kPath1 = "ncbi_dataset/data/GCF_x/cds.fna";
    constexpr std::string_view kPath2 = "ncbi_dataset/data/GCF_x/genomic.fna";
    const std::string body1 = multi_record_body();          // seg1(120) + seg2(60)
    std::string body2 = ">seg3 third segment\n";
    for (int i = 0; i < 9; ++i) body2 += "ACGTACGTAC\n";     // 90 bases, one record
    const std::string d1 = exio::md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body1.data()), body1.size()));
    const std::string d2 = exio::md5_hex(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(body2.data()), body2.size()));
    std::string md5s = md5sum_line(d1, kPath1);
    md5s += md5sum_line(d2, kPath2);
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json",
                       .content = catalog_selecting_two_fasta(kPath1, kPath2)});
    members.push_back({.path = "ncbi_dataset/data/md5sum.txt", .content = md5s});
    members.push_back({.path = std::string(kPath1), .content = body1});
    members.push_back({.path = std::string(kPath2), .content = body2});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{}, "GCF_x.1");
    REQUIRE(result.has_value());
    REQUIRE(result->members_verified == 2);
    REQUIRE(counter.asset_ends == 2);            // one asset-end per member
    REQUIRE(counter.record_ends == 3);           // seg1, seg2, seg3
    REQUIRE(counter.letters == 270);             // 120 + 60 + 90
    // The fix: chunk_index is contiguous 0..2 ACROSS members (no per-member reset),
    // so it stays unique under one accession. base_offset still resets per record.
    REQUIRE(counter.chunk_index_seq == std::vector<std::uint64_t>{0, 1, 2});
    REQUIRE(counter.base_offset_seq == std::vector<std::uint64_t>{0, 0, 0});
}

TEST_CASE("archive_source reconciles NCBI's three path conventions",
          "[external][archive]") {
    // catalog filePath is data-dir-relative; md5sum.txt + ZIP entry are full
    // paths. Canonicalization must make all three match → member selected+verified.
    const std::vector<std::uint8_t> zip = build_zip(real_layout_members());
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{});
    REQUIRE(result.has_value());
    REQUIRE(result->members_selected == 1);
    REQUIRE(result->members_verified == 1);
    REQUIRE(counter.letters == expected_fasta_letters(fasta_body()));
    REQUIRE(counter.letters > 0);
}

TEST_CASE("archive_source rejects a path-traversal member",
          "[external][archive]") {
    std::vector<zip_member> members = good_members();
    members.push_back({.path = "../evil.fna", .content = ">x\nACGT\n"});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::provider_contract);
}

TEST_CASE("archive_source rejects an absolute-path member",
          "[external][archive]") {
    std::vector<zip_member> members = good_members();
    members.push_back({.path = "/etc/passwd", .content = "root\n"});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::provider_contract);
}

TEST_CASE("archive_source rejects a symlink member", "[external][archive]") {
    std::vector<zip_member> members = good_members();
    members.push_back({.path = "ncbi_dataset/data/link.fna",
                       .filetype = AE_IFLNK,
                       .symlink_target = "/etc/passwd"});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::provider_contract);
}

TEST_CASE("archive_source rejects a wrong md5sum digest (corruption)",
          "[external][archive]") {
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json",
                       .content = catalog_selecting_fasta()});
    members.push_back(
        {.path = "ncbi_dataset/data/md5sum.txt",
         .content = md5sum_line("00000000000000000000000000000000", kFastaPath)});
    members.push_back({.path = std::string(kFastaPath), .content = fasta_body()});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::integrity);
    // No rows may reach the sink for an unverified member.
    REQUIRE(counter.letters == 0);
    // A failed member must never produce a successful asset-end (spec §3208-3211).
    REQUIRE(counter.asset_ends == 0);
}

TEST_CASE("archive_source rejects a missing md5sum.txt", "[external][archive]") {
    std::vector<zip_member> members;
    members.push_back({.path = "ncbi_dataset/data/dataset_catalog.json",
                       .content = catalog_selecting_fasta()});
    members.push_back({.path = std::string(kFastaPath), .content = fasta_body()});
    const std::vector<std::uint8_t> zip = build_zip(members);
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, default_limits(), prov::query_target::sequence_chunk, 1048572,
        counter.ref(), std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::integrity);
}

TEST_CASE("archive_source enforces the maximum entry count",
          "[external][archive]") {
    exio::archive_read_limits limits = default_limits();
    limits.maximum_entries = 2;  // the well-formed package has 3 members
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, limits, prov::query_target::sequence_chunk, 1048572, counter.ref(),
        std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::response_too_large);
}

TEST_CASE("archive_source enforces the decoded/compressed expansion ratio",
          "[external][archive]") {
    exio::archive_read_limits limits = default_limits();
    // A near-zero ratio ceiling: any decoded FASTA body exceeds it.
    limits.maximum_expansion = {.numerator = 1, .denominator = 1000000,
                                .grace_bytes = 0};
    const std::vector<std::uint8_t> zip = build_zip(good_members());
    chunk_counter counter;
    const auto result = exio::extract_fasta_members_from_zip(
        zip, limits, prov::query_target::sequence_chunk, 1048572, counter.ref(),
        std::stop_token{});
    REQUIRE_FALSE(result.has_value());
    REQUIRE(result.error().kind == prov::external_error_kind::response_too_large);
}
