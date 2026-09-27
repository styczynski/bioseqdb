/**
 * @file identifier_parser_test.cpp
 * @brief Checks sequence header parsing, column mapping, and file sampling.
 */
#include <unistd.h>

#include <fmt/format.h>
#include <catch2/catch_test_macros.hpp>

import std;
import bioseq.common.logging;
import bioseq.io.options;
import bioseq.io.reader;
import bioseq.io.identifier_parser.formats;
import bioseq.io.identifier_parser;

using namespace bioseq::io::identifier_parser;

namespace {

/// Save the value sent to each destination column for test assertions.
struct capture_identifier_writer {
    std::vector<std::optional<std::string>> values;

    explicit capture_identifier_writer(std::size_t n) : values(n) {}
    void begin_tuple() {
        for (auto& v : values)
            v.reset();
    }
    void set_field(destination_slot slot, const char* data, std::int32_t len) {
        values.at(static_cast<std::size_t>(slot)) =
            std::string{data, static_cast<std::size_t>(len)};
    }
    void set_null(destination_slot slot) {
        values.at(static_cast<std::size_t>(slot)).reset();
    }
    void end_tuple() {}
    void finish() {}
    [[nodiscard]] std::size_t destination_count() const noexcept {
        return values.size();
    }
};
static_assert(identifier_destination_writer<capture_identifier_writer>);

/// Locate the destination slot assigned to a table column.
[[nodiscard]] std::size_t slot_for_column(const identifier_mapping_result& map,
                                          std::uint16_t col_index) {
    for (std::size_t slot = 0; slot < map.slot_to_column.size(); ++slot)
        if (map.slot_to_column[slot] == col_index)
            return slot;
    FAIL("column was not routed");
    return 0;
}

/// Return the captured value for a table column.
[[nodiscard]] const std::optional<std::string>&
value_for_column(const identifier_mapping_result& map,
                 const capture_identifier_writer& writer,
                 std::uint16_t col_index) {
    return writer.values.at(slot_for_column(map, col_index));
}

/// Describe a nullable text column for mapping tests.
[[nodiscard]] column_descriptor text_col(std::string_view name) {
    return {.name = name,
            .is_text = true,
            .is_primary_key = false,
            .not_null = false,
            .has_default = false,
            .is_identity_or_generated = false};
}
/// Describe a nullable column whose SQL type is not text.
[[nodiscard]] column_descriptor scalar_col(std::string_view name) {
    return {.name = name,
            .is_text = false,
            .is_primary_key = false,
            .not_null = false,
            .has_default = false,
            .is_identity_or_generated = false};
}

/// The column mapping and values produced from one header.
struct parse_run {
    identifier_mapping_result map;
    capture_identifier_writer writer;
};

/// Map header fields to columns and return the parsed values.
[[nodiscard]] parse_run run_parse(header_format requested,
                                  header_format effective,
                                  std::span<const column_descriptor> columns,
                                  std::string_view header,
                                  std::span<const user_column_mapping> maps = {}) {
    auto map = resolve_identifier_mapping(requested, effective, columns, maps);
    REQUIRE(map.has_value());
    identifier_parse_state state;
    capture_identifier_writer writer{map->routing.destination_count};
    writer.begin_tuple();
    auto parsed = parse_and_route_header(
        effective, header, map->routing, state, make_identifier_writer_reference(writer));
    REQUIRE(parsed.has_value());
    writer.end_tuple();
    return {std::move(*map), std::move(writer)};
}

} // namespace

TEST_CASE("identifier_parser: automatic GenBank parsing claims text columns before identifier",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{
        text_col("id"), text_col("organism"), scalar_col("chromosome"), text_col("moltype")};
    auto r = run_parse(header_format::automatic,
                       header_format::genbank,
                       columns,
                       "dna_new [organism=Homo sapiens] [chromosome=17] [moltype=mRNA]");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "dna_new");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "Homo sapiens");
    REQUIRE(value_for_column(r.map, r.writer, 2).value() == "17");
    REQUIRE(value_for_column(r.map, r.writer, 3).value() == "mRNA");
}

TEST_CASE("identifier_parser: one source value routes to every matching destination column",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{scalar_col("SequenceVersion"),
                                                 scalar_col("sequence_version"),
                                                 scalar_col("sv"),
                                                 scalar_col("protein_existence")};
    auto r =
        run_parse(header_format::uniprotkb,
                  header_format::uniprotkb,
                  columns,
                  "tr|R9RJF1|R9RJF1_PSEAI Mandelate racemase OS=Pseudomonas aeruginosa PE=4 SV=1");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "1");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "1");
    REQUIRE(value_for_column(r.map, r.writer, 2).value() == "1");
    REQUIRE(value_for_column(r.map, r.writer, 3).value() == "4");
}

TEST_CASE("identifier_parser: fixed UniProt parsing routes positional and tagged fields",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{
        text_col("accession"), text_col("organism_name"), scalar_col("sv")};
    auto r =
        run_parse(header_format::uniprotkb,
                  header_format::uniprotkb,
                  columns,
                  "tr|R9RJF1|R9RJF1_PSEAI Mandelate racemase OS=Pseudomonas aeruginosa PE=4 SV=1");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "R9RJF1");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "Pseudomonas aeruginosa");
    REQUIRE(value_for_column(r.map, r.writer, 2).value() == "1");
}

TEST_CASE("identifier_parser: UniProt isoform leaves absent protein-existence field null",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{
        text_col("gene_name"), scalar_col("organism_identifier"), scalar_col("pe")};
    auto r = run_parse(header_format::uniprotkb,
                       header_format::uniprotkb,
                       columns,
                       "sp|Q4R572-2|1433B_MACFA Isoform Short of 14-3-3 protein beta/alpha "
                       "OS=Macaca fascicularis OX=9541 GN=YWHAB");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "YWHAB");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "9541");
    REQUIRE_FALSE(value_for_column(r.map, r.writer, 2).has_value()); // The header has no PE field.
}

TEST_CASE("identifier_parser: UniRef parsing routes identifier, cluster, and tags",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{
        text_col("id"), text_col("cluster"), scalar_col("n"), text_col("tax"), text_col("rep_id")};
    auto r = run_parse(
        header_format::uniref,
        header_format::uniref,
        columns,
        "UniRef100_A5DI11 Elongation factor 2 n=1 Tax=Pichia guilliermondii RepID=EF2_PICGU");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "UniRef100_A5DI11");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "Elongation factor 2");
    REQUIRE(value_for_column(r.map, r.writer, 2).value() == "1");
    REQUIRE(value_for_column(r.map, r.writer, 3).value() == "Pichia guilliermondii");
    REQUIRE(value_for_column(r.map, r.writer, 4).value() == "EF2_PICGU");
}

TEST_CASE("identifier_parser: UniParc parsing routes identifier and status without description",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{text_col("id"), text_col("status")};
    auto r = run_parse(
        header_format::uniparc, header_format::uniparc, columns, "UPI0000000005 status=active");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "UPI0000000005");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "active");
}

TEST_CASE("identifier_parser: SPD parsing splits positional gene and taxon fields",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{
        text_col("id"), text_col("gene_id"), text_col("taxon"), text_col("description")};
    auto r = run_parse(header_format::spd,
                       header_format::spd,
                       columns,
                       "P31947|1433S_HUMAN| 14-3-3 protein sigma");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "P31947");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "1433S");
    REQUIRE(value_for_column(r.map, r.writer, 2).value() == "HUMAN");
    REQUIRE(value_for_column(r.map, r.writer, 3).value() == "14-3-3 protein sigma");
}

TEST_CASE("identifier_parser: NCBI parsing routes identifier, description, and trailing organism",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{
        text_col("id"), text_col("description"), text_col("taxon")};
    auto r =
        run_parse(header_format::ncbi,
                  header_format::ncbi,
                  columns,
                  "NP_001351877.1 acylglycerol kinase, mitochondrial isoform 2 [Homo sapiens]");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "NP_001351877.1");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() ==
            "acylglycerol kinase, mitochondrial isoform 2");
    REQUIRE(value_for_column(r.map, r.writer, 2).value() == "Homo sapiens");
}

TEST_CASE("identifier_parser: IMGT parsing routes positional range-shaped fields",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{text_col("ligm_accession"),
                                                 text_col("imgt_allele_name"),
                                                 text_col("species"),
                                                 text_col("positions")};
    auto r = run_parse(
        header_format::imgt,
        header_format::imgt,
        columns,
        "M99641|IGHV1-18*01|Homo sapiens|F|V-REGION|188..483|296 nt|1| | | | |296+24=320| | ");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "M99641");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "IGHV1-18*01");
    REQUIRE(value_for_column(r.map, r.writer, 2).value() == "Homo sapiens");
    REQUIRE(value_for_column(r.map, r.writer, 3).value() == "188..483");
}

TEST_CASE("identifier_parser: automatic UniProt parsing keeps embedded equals in description",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{
        text_col("protein_name"), scalar_col("sv"), text_col("organism_name")};
    auto r = run_parse(header_format::uniprotkb,
                       header_format::uniprotkb,
                       columns,
                       "tr|Q9S8M8|Q9S8M8_WHEAT FRIII-2-VIII=GAMMA-gliadin (Fragment) "
                       "OS=Triticum aestivum OX=4565 PE=1 SV=1");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() ==
            "FRIII-2-VIII=GAMMA-gliadin (Fragment)");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "1");
    REQUIRE(value_for_column(r.map, r.writer, 2).value() == "Triticum aestivum");
}

TEST_CASE("identifier_parser: automatic parsing strips trailing organism bracket from identifier",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{text_col("id"), text_col("organism")};
    auto r = run_parse(header_format::automatic,
                       header_format::automatic,
                       columns,
                       "gi|347012980| 4-O-methyl-glucuronoyl methylesterase "
                       "[Myceliophthora thermophila ATCC 42464]");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() ==
            "gi|347012980| 4-O-methyl-glucuronoyl methylesterase");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() ==
            "Myceliophthora thermophila ATCC 42464");
}

TEST_CASE("identifier_parser: trailing value-only bracket populates organism and taxon",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{text_col("organism"), text_col("taxon")};
    auto r = run_parse(header_format::automatic,
                       header_format::ncbi_gi,
                       columns,
                       "gi|12345| some protein description [Homo sapiens]");
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "Homo sapiens");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "Homo sapiens");
}

TEST_CASE("identifier_parser: numeric and named data sources coexist",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{text_col("x"), text_col("organism_name")};
    const std::vector<user_column_mapping> maps{
        {.column_name = "x", .data_source = "1"},               // Field indexes start at zero.
        {.column_name = "organism_name", .data_source = "OS"}}; // Match the OS header field.
    auto r =
        run_parse(header_format::uniprotkb,
                  header_format::uniprotkb,
                  columns,
                  "tr|R9RJF1|R9RJF1_PSEAI Mandelate racemase OS=Pseudomonas aeruginosa PE=4 SV=1",
                  maps);
    REQUIRE(value_for_column(r.map, r.writer, 0).value() == "R9RJF1");
    REQUIRE(value_for_column(r.map, r.writer, 1).value() == "Pseudomonas aeruginosa");
}

TEST_CASE("identifier_parser: unknown mapping target column returns an error",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{text_col("x")};
    const std::vector<user_column_mapping> maps{{.column_name = "nope", .data_source = "0"}};
    auto map = resolve_identifier_mapping(
        header_format::automatic, header_format::automatic, columns, maps);
    REQUIRE_FALSE(map.has_value());
    REQUIRE(map.error().find("not found") != std::string::npos);
}

TEST_CASE("identifier_parser: fixed header format rejects unknown data-source field",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{text_col("x")};
    const std::vector<user_column_mapping> maps{{.column_name = "x", .data_source = "nonsense"}};
    auto map = resolve_identifier_mapping(
        header_format::uniprotkb, header_format::uniprotkb, columns, maps);
    REQUIRE_FALSE(map.has_value());
    REQUIRE(map.error().find("not a field of format") != std::string::npos);
}

TEST_CASE("identifier_parser: ambiguous unmapped text columns return an error",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<column_descriptor> columns{text_col("h1"), text_col("h2")};
    auto map = resolve_identifier_mapping(header_format::automatic,
                                          header_format::plain,
                                          columns,
                                          std::span<const user_column_mapping>{});
    REQUIRE_FALSE(map.has_value());
    REQUIRE(map.error().find("disambiguate") != std::string::npos);
}

TEST_CASE("identifier_parser: detect_header_format majority vote",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::vector<std::string_view> uniprot{
        "tr|R9RJF1|R9RJF1_PSEAI Mandelate racemase OS=Pseudomonas aeruginosa PE=4 SV=1",
        "sp|P27748|ACOX_CUPNH Acetoin catabolism protein X OS=Cupriavidus necator OX=381666 "
        "GN=acoX PE=4 SV=2"};
    REQUIRE(detect_header_format(uniprot) == header_format::uniprotkb);

    const std::vector<std::string_view> plain{"chr1 human chromosome 1", "chr2"};
    REQUIRE(detect_header_format(plain) == header_format::plain);

    const std::vector<std::string_view> gi{"gi|12345| protein [Homo sapiens]"};
    REQUIRE(detect_header_format(gi) == header_format::ncbi_gi);
}

namespace {

/// Write test input to a temporary path named with the process ID and a counter.
/// @param content Bytes to write.
/// @return The temporary path.
std::string write_tmp_file(const std::string& content) {
    namespace fs = std::filesystem;
    static std::atomic<std::uint64_t> counter{0};
    fs::path tmp =
        fs::temp_directory_path() / fmt::format("bioseq_identifier_parser_test_{}_{}",
                                                ::getpid(),
                                                counter.fetch_add(1, std::memory_order_relaxed));
    std::ofstream out(tmp, std::ios::binary);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    out.close();
    return tmp.string();
}

} // namespace

TEST_CASE("identifier_parser: sample_sequence_headers supports multiline FASTQ",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    const std::string corpus = "@read1 OS=Homo sapiens SV=1\n"
                               "ACGT\nACGT\n"
                               "+\n"
                               "IIII\nJJJJ\n"
                               "@read2 OS=Mus musculus SV=2\n"
                               "TTTT\n"
                               "+\n"
                               "####\n";
    const std::string path = write_tmp_file(corpus);

    bioseq::io::reader_options opts{};
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap = false;
    auto headers = bioseq::io::sample_sequence_headers(
        path.c_str(), bioseq::io::sequence_file_format::fastq, opts, 4, 4096);

    REQUIRE(headers.has_value());
    REQUIRE(headers->size() == 2);
    REQUIRE((*headers)[0] == "read1 OS=Homo sapiens SV=1");
    REQUIRE((*headers)[1] == "read2 OS=Mus musculus SV=2");
    std::filesystem::remove(path);
}

TEST_CASE("identifier_parser: sample_sequence_headers FASTA prefix",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    const std::string corpus = ">chr1 human chromosome 1\nACGTACGT\n>chr2\nTTTT\n";
    const std::string path = write_tmp_file(corpus);

    bioseq::io::reader_options opts{};
    opts.io.gzip_auto_detect = false;
    opts.io.prefer_mmap = false;
    auto headers = bioseq::io::sample_sequence_headers(
        path.c_str(), bioseq::io::sequence_file_format::fasta, opts, 8, 4096);
    REQUIRE(headers.has_value());
    REQUIRE(headers->size() == 2);
    REQUIRE((*headers)[0] == "chr1 human chromosome 1");
    REQUIRE((*headers)[1] == "chr2");
    std::filesystem::remove(path);
}
