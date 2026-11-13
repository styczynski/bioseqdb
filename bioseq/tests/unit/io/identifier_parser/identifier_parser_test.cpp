/**
 * @file identifier_parser_test.cpp
 * @brief Checks sequence header parsing, column mapping, and file sampling
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
import bioseq.io.identifier_parser.canonical;
import bioseq.io.identifier_parser.preservation;
import bioseq.io.record_schema;
import bioseq.io.alignment;
import bioseq.io.sinks;
import bioseq.parallelism.memory_admission;

using namespace bioseq::io::identifier_parser;

namespace {

/// Save the value sent to each  destination column for test assertions.
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
/// Describe a nullable column whose SQL type is not toxt.
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
    REQUIRE_FALSE(value_for_column(r.map, r.writer, 2).has_value()); // The, header has no PE field.
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

TEST_CASE("identifier_parser: explicit header selection keeps literal metadata keys separate",
          "[component:bioseq][layer:unit][feature:io]") {
    const std::array columns{text_col("left_header"), text_col("right_header"), text_col("numeric_key")};
    const std::array mappings{
        user_column_mapping{"left_header", "HEADER"},
        user_column_mapping{"right_header", "key:header"},
        user_column_mapping{"numeric_key", "key:1"}};
    const auto parsed = run_parse(header_format::automatic, header_format::automatic,
        columns, "read label [header=metadata] [1=one]", mappings);
    REQUIRE(value_for_column(parsed.map, parsed.writer, 0) == std::optional<std::string>{"read label"});
    REQUIRE(value_for_column(parsed.map, parsed.writer, 1) == std::optional<std::string>{"metadata"});
    REQUIRE(value_for_column(parsed.map, parsed.writer, 2) == std::optional<std::string>{"one"});

    const std::array plain_columns{text_col("left_header"), text_col("right_header")};
    const std::array plain_mapping{user_column_mapping{"left_header", "header"}};
    const auto plain = run_parse(header_format::plain, header_format::plain,
        plain_columns, "complete header [unchanged=value]", plain_mapping);
    REQUIRE(value_for_column(plain.map, plain.writer, 0)
        == std::optional<std::string>{"complete header [unchanged=value]"});
    REQUIRE_FALSE(value_for_column(plain.map, plain.writer, 1).has_value());

    const std::array duplicate{user_column_mapping{"left_header", "header"},
        user_column_mapping{"right_header", "HEADER"}};
    REQUIRE_FALSE(resolve_identifier_mapping(header_format::automatic,
        header_format::automatic, plain_columns, duplicate).has_value());
    const std::array scalar_columns{scalar_col("value")};
    const std::array invalid{user_column_mapping{"value", "header"}};
    REQUIRE_FALSE(resolve_identifier_mapping(header_format::automatic,
        header_format::automatic, scalar_columns, invalid).has_value());
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


TEST_CASE("identifier_parser: canonical envelope rejects contradictory visible fields",
          "[component:bioseq][layer:unit][feature:io]") {
    struct text_sink {
        std::string text;
        /** Collect one fixture header; production owns its own admitted sink. */
        [[nodiscard]] std::expected<void, std::string> append(std::string_view bytes) {
            text.append(bytes);
            return {};
        }
    };
    const auto make_wire = [](std::string_view descriptors,
            std::span<const canonical_field_view> fields, std::string_view alphabet,
            bool null_header, std::string_view original_header) {
        const std::string payload = std::string{
            R"({"magic":"bioseq.io","version":1,"representation":"text","header_null":)"}
            + (null_header ? "true" : "false")
            + R"(,"sequence":{"alphabet":"dna4","present":true,"length":"4","storage":"native","ranks":null},)"
              R"("quality":{"alphabet":null,"present":false,"length":"0","storage":"native","ranks":null},"scalar_types":)"
            + std::string{descriptors} + "}";
        text_sink sink;
        REQUIRE(append_canonical_header(sink, original_header, alphabet,
            std::string_view{payload}, fields).has_value());
        return sink.text;
    };
    const std::array<canonical_field_view, 2> fields{{
        {"count", std::string_view{"42"}, 'i'}, {"label", std::nullopt, 's'}}};
    constexpr std::string_view descriptors =
        R"([{"name":"count","type":"pg_catalog.int8"},{"name":"label","type":"pg_catalog.text"}])";
    const auto valid = make_wire(descriptors, fields, "dna4", false, " read µ🙂 ");
    const auto missing_bracket = make_wire(descriptors,
        std::span{fields}.first(1), "dna4", false, "read");
    const auto missing_descriptor = make_wire(
        R"([{"name":"count","type":"pg_catalog.int8"}])", fields, "dna4", false, "read");
    const auto duplicated_descriptor = make_wire(
        R"([{"name":"count","type":"pg_catalog.int8"},{"name":"count","type":"pg_catalog.text"}])",
        fields, "dna4", false, "read");
    const auto reordered_descriptors = make_wire(
        R"([{"name":"label","type":"pg_catalog.text"},{"name":"count","type":"pg_catalog.int8"}])",
        fields, "dna4", false, "read");
    const auto wrong_alphabet = make_wire(descriptors, fields, "dna5", false, "read");
    const auto contradictory_null = make_wire(descriptors, fields, "dna4", true, "read");
    const auto null_header = make_wire("[]", {}, "dna4", true, "");
    const auto empty_header = make_wire("[]", {}, "dna4", false, "");
    for (const auto mode : {canonical_parse_mode::complete, canonical_parse_mode::shape}) {
        const auto accepts = [mode](std::string_view wire) {
            const auto parsed = parse_canonical_header(wire, mode);
            if (!parsed || !*parsed) return false;
            text_record_shape shape{**parsed};
            return decode_preservation_record((**parsed).payload, shape, true, 4096).has_value();
        };
        REQUIRE(accepts(valid));
        REQUIRE(accepts(null_header));
        REQUIRE(accepts(empty_header));
        REQUIRE_FALSE(accepts(missing_bracket));
        REQUIRE_FALSE(accepts(missing_descriptor));
        REQUIRE_FALSE(accepts(duplicated_descriptor));
        REQUIRE_FALSE(accepts(reordered_descriptors));
        REQUIRE_FALSE(accepts(wrong_alphabet));
        REQUIRE_FALSE(accepts(contradictory_null));
        const auto suffix = valid.substr(valid.find(' '));
        REQUIRE_FALSE(accepts("%FF" + suffix));
        REQUIRE_FALSE(accepts("%E2%82" + suffix));
        REQUIRE_FALSE(accepts("%ED%A0%80" + suffix));
        REQUIRE_FALSE(accepts("%C2A" + suffix));
    }
    const std::array columns{text_col("header")};
    auto restored_null = run_parse(header_format::automatic, header_format::plain, columns, null_header);
    REQUIRE_FALSE(value_for_column(restored_null.map, restored_null.writer, 0).has_value());
    auto restored_empty = run_parse(header_format::automatic, header_format::plain, columns, empty_header);
    REQUIRE(value_for_column(restored_empty.map, restored_empty.writer, 0) == std::optional<std::string>{""});
}

TEST_CASE("preservation scalar ownership survives moves spill denial and parser errors",
          "[component:bioseq][layer:unit][feature:io][resource:filesystem]") {
    // Exercise the encoder's full decimal bounds before changing admission limits.
    {
        struct text_sink {
            std::string text;
            std::size_t calls{};
            bool refuse{};
            /** Copy synchronously, or expose one stable fixture failure. */
            [[nodiscard]] std::expected<void, std::string> append(std::string_view bytes) {
                ++calls;
                if (refuse) return std::unexpected(std::string{"fixture sink refusal"});
                text.append(bytes);
                return {};
            }
        } sink;
        preservation_encoder<text_sink> encoder{sink};
        const auto check = [&sink, &encoder](auto value, std::string_view expected) {
            sink.text.clear();
            encoder.integer(value);
            REQUIRE(encoder.result().has_value());
            REQUIRE(sink.text == expected);
        };
        check(std::numeric_limits<std::int8_t>::min(), "\"-128\"");
        check(std::numeric_limits<std::uint8_t>::max(), "\"255\"");
        check(std::numeric_limits<std::int16_t>::min(), "\"-32768\"");
        check(std::numeric_limits<std::uint16_t>::max(), "\"65535\"");
        check(std::numeric_limits<std::int32_t>::min(), "\"-2147483648\"");
        check(std::numeric_limits<std::uint32_t>::max(), "\"4294967295\"");
        check(std::numeric_limits<std::int64_t>::min(), "\"-9223372036854775808\"");
        check(std::numeric_limits<std::uint64_t>::max(), "\"18446744073709551615\"");
        check(std::optional<std::int64_t>{}, "null");
        check(std::optional<std::int64_t>{0}, "\"0\"");

        sink.text.clear();
        sink.refuse = true;
        const auto calls_before_refusal = sink.calls;
        encoder.integer(std::numeric_limits<std::int64_t>::min());
        const auto refused = encoder.result();
        REQUIRE_FALSE(refused.has_value());
        REQUIRE(refused.error() == "fixture sink refusal");
        REQUIRE(sink.calls == calls_before_refusal + 1u);
        REQUIRE(sink.text.empty());
        sink.refuse = false;
        encoder.integer(std::numeric_limits<std::uint64_t>::max());
        const auto later = encoder.result();
        REQUIRE_FALSE(later.has_value());
        REQUIRE(later.error() == "fixture sink refusal");
        REQUIRE(sink.calls == calls_before_refusal + 1u);
        REQUIRE(sink.text.empty());
    }
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous;
        ~restore_cap() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, admission.index_memory_cap_bytes()};

    // These fixtures exist before the production admission ceiling is lowered.
    const std::string large = "\"\\u0061" + std::string(1024u * 1024u, 'a') + "\"";
    const std::string bad_tail = large.substr(0, large.size() - 1u) + "\\uD800\"";
    admission.set_index_memory_cap_bytes(baseline + 16u * 1024u);
    {
        preservation_json_cursor cursor{large, large.size()};
        auto decoded = cursor.string();
        REQUIRE(decoded.has_value());
        REQUIRE(cursor.finish().has_value());
        REQUIRE(decoded->size() == 1024u * 1024u + 1u);
        REQUIRE(decoded->text().find_first_not_of('a') == std::string_view::npos);
        REQUIRE(admission.reserved_bytes() > baseline);
        REQUIRE(admission.reserved_bytes() <= baseline + 16u * 1024u);
        auto moved = std::move(*decoded);
        REQUIRE(moved.text().size() == 1024u * 1024u + 1u);
        REQUIRE(moved.text().back() == 'a');
    }
    REQUIRE(admission.reserved_bytes() == baseline);
    {
        preservation_json_cursor cursor{bad_tail, bad_tail.size()};
        const auto decoded = cursor.string();
        REQUIRE_FALSE(decoded.has_value());
        REQUIRE(decoded.error() == "JSON high surrogate lacks a low surrogate");
    }
    REQUIRE(admission.reserved_bytes() == baseline);

    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(baseline, 1u));
    {
        preservation_json_cursor cursor{large, large.size()};
        REQUIRE_FALSE(cursor.string().has_value());
    }
    REQUIRE(admission.reserved_bytes() == baseline);
    for (const auto json : {std::string_view{"\"plain\""}, std::string_view{"\"a\\tb\\u03BB\""}}) {
        preservation_json_cursor cursor{json, 128u};
        auto decoded = cursor.string();
        REQUIRE(decoded.has_value());
        auto moved = std::move(*decoded);
        preservation_scalar assigned;
        assigned = std::move(moved);
        REQUIRE(assigned.text() == (json == "\"plain\"" ? "plain" : "a\tbλ"));
        REQUIRE(admission.reserved_bytes() == baseline);
    }
    // Crossing the scalar limit preserves the existing diagnostic byte position.
    const std::string long_key = " \t\"" + std::string(129u, 'x') + "\"";
    preservation_json_cursor too_long{long_key, 128u};
    REQUIRE_FALSE(too_long.string().has_value());
    REQUIRE(too_long.position() == 3u);
    const std::string full_inline = "\"" + std::string(127u, 'x') + "\\u0078\"";
    preservation_json_cursor at_limit{full_inline, 128u};
    const auto exact = at_limit.string();
    REQUIRE(exact.has_value());
    REQUIRE(exact->text() == std::string(128u, 'x'));
    const std::string escaped_overflow = "\"" + std::string(128u, 'x') + "\\u0078\"";
    preservation_json_cursor over_limit{escaped_overflow, 128u};
    REQUIRE_FALSE(over_limit.string().has_value());
    REQUIRE(over_limit.position() == escaped_overflow.size() - 1u);
    preservation_json_cursor duplicate{"{\"a\":null,\"\\u0061\":null}", 128u};
    std::size_t callbacks{};
    const auto repeated = duplicate.object(std::array<std::string_view, 1>{"a"},
        [&duplicate, &callbacks](std::size_t) -> std::expected<void, std::string> {
            ++callbacks;
            return duplicate.literal("null") ? std::expected<void, std::string>{}
                : std::unexpected(std::string{"expected null"});
        });
    REQUIRE_FALSE(repeated.has_value());
    REQUIRE(repeated.error() == "duplicate preservation key: a");
    REQUIRE(callbacks == 1u);
    preservation_json_cursor null_value{"null", 0u};
    const auto absent = null_value.optional_string();
    REQUIRE(absent.has_value());
    REQUIRE_FALSE(absent->has_value());
    preservation_json_cursor empty_value{"\"\"", 0u};
    const auto empty = empty_value.optional_string();
    REQUIRE(empty.has_value());
    REQUIRE(empty->has_value());
    REQUIRE((*empty)->text().empty());
    REQUIRE(admission.reserved_bytes() == baseline);
}

TEST_CASE("preservation metadata retains concurrent decoded owners through destination copies",
          "[component:bioseq][layer:unit][feature:io]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto baseline = admission.reserved_bytes();
    struct restore_cap {
        bioseq::parallelism::memory_admission_controller& admission;
        std::uint64_t previous;
        ~restore_cap() { admission.set_index_memory_cap_bytes(previous); }
    } restore{admission, admission.index_memory_cap_bytes()};
    admission.set_index_memory_cap_bytes(baseline + 1024u * 1024u);
    std::string scalar;
    for (std::size_t index = 0; index < 4096u; ++index) scalar += "\\u0061";
    const std::string payload = "[{\"name\":\"" + scalar + "\",\"type\":\"" + scalar
        + "\",\"value\":\"" + scalar + "\"}]";
    struct observed_record : bioseq::io::decoded_record {
        using bioseq::io::decoded_record::decoded_record;
        std::size_t callbacks{};
        std::size_t auxiliary_begins{}, auxiliary_appends{}, auxiliary_ends{};
        std::uint64_t before_copy{}, after_copy{};
        /** Count entry into the existing destination admission path. */
        std::expected<void, std::string> begin_auxiliary(bool projected,
                std::array<char, 2> tag, char type, char subtype) {
            ++auxiliary_begins;
            return bioseq::io::decoded_record::begin_auxiliary(projected, tag, type, subtype);
        }
        /** Count exact decoded fragments without retaining parser scratch. */
        std::expected<void, std::string> append_auxiliary_bytes(std::span<const std::byte> bytes) {
            ++auxiliary_appends;
            return bioseq::io::decoded_record::append_auxiliary_bytes(bytes);
        }
        /** Count successful passage through final text-length checks. */
        std::expected<void, std::string> end_auxiliary(std::uint32_t count) {
            ++auxiliary_ends;
            return bioseq::io::decoded_record::end_auxiliary(count);
        }
        /** Observe actual simultaneous temporary and retained admission. */
        std::expected<void, std::string> metadata(std::string_view name, std::string_view type,
            std::optional<std::string_view> value, bool types_only) {
            ++callbacks;
            auto& controller = bioseq::parallelism::memory_admission_controller::instance();
            before_copy = controller.reserved_bytes();
            auto result = bioseq::io::decoded_record::metadata(name, type, value, types_only);
            after_copy = controller.reserved_bytes();
            return result;
        }
    };
    for (const bool use_credit : {false, true}) {
        bioseq::io::io_memory_credit credit;
        if (use_credit) {
            auto permit = admission.try_reserve(8192u);
            REQUIRE(permit.has_value());
            credit.reset(std::move(*permit)); // Exhaustion must release spare before exact fallback.
        }
        {
            observed_record destination;
            preservation_json_cursor cursor{payload, 4096u, use_credit ? &credit : nullptr};
            REQUIRE(decode_preserved_metadata(cursor, destination, false).has_value());
            REQUIRE(cursor.finish().has_value());
            REQUIRE(destination.callbacks == 1u);
            REQUIRE(destination.before_copy >= baseline + 3u * 4096u);
            REQUIRE(destination.after_copy >= destination.before_copy + 3u * 4096u);
            credit.release();
            REQUIRE(admission.reserved_bytes() < destination.after_copy);
            const auto row = destination.freeze();
            REQUIRE(row.has_value());
            REQUIRE(row->metadata.size() == 1u);
            REQUIRE(row->metadata.front().name == std::string(4096u, 'a'));
            REQUIRE(row->metadata.front().qualified_type == std::string(4096u, 'a'));
            REQUIRE(row->metadata.front().bytes == std::string(4096u, 'a'));
        }
        REQUIRE(admission.reserved_bytes() == baseline);
    }

    // The first pass may borrow one complete ASCII fragment, but admission and
    // the exact second-stage text checks still precede destination appends.
    const std::string long_value(4097u, 'A');
    const std::string long_auxiliary = "[{\"tag\":\"XZ\",\"type\":\"Z\",\"value\":\"" + long_value + "\"}]";
    const std::array<std::tuple<std::string_view, std::string_view, char, std::size_t>, 8> auxiliary_values{{
        {R"json([{"tag":"XZ","type":"Z","value":"plain"}])json", "plain", 'Z', 1u},
        {R"json([{"value":"AF","type":"H","tag":"XZ"}])json", "AF", 'H', 1u},
        {R"json([{"tag":"XZ","type":"A","value":"Q"}])json", "Q", 'A', 1u},
        {R"json([{"tag":"XZ","type":"Z","value":""}])json", "", 'Z', 0u},
        {R"json([{"tag":"XZ","type":"H","value":""}])json", "", 'H', 0u},
        {R"json([{"tag":"XZ","type":"Z","value":"\u0041"}])json", "A", 'Z', 1u},
        {R"json([{"tag":"XZ","type":"Z","value":"A\u0042C"}])json", "ABC", 'Z', 3u},
        {long_auxiliary, long_value, 'Z', 1u}}};
    for (const auto& [json, expected, expected_type, expected_appends] : auxiliary_values) {
        for (const bool projected : {false, true}) {
            for (const bool use_credit : {false, true}) {
                CAPTURE(json, projected, use_credit);
                bioseq::io::io_memory_credit credit;
                if (use_credit) {
                    auto spare = admission.try_reserve(8192u);
                    REQUIRE(spare.has_value());
                    credit.reset(std::move(*spare));
                }
                {
                    observed_record destination{use_credit ? &credit : nullptr};
                    destination.record.alignment.emplace();
                    preservation_json_cursor cursor{json, 8u, use_credit ? &credit : nullptr};
                    REQUIRE(decode_preserved_auxiliary(cursor, destination, projected, 8u).has_value());
                    REQUIRE(cursor.finish().has_value());
                    REQUIRE(destination.auxiliary_begins == 1u);
                    REQUIRE(destination.auxiliary_appends == expected_appends);
                    REQUIRE(destination.auxiliary_ends == 1u);
                    credit.release(); // Exact owners must survive unused-credit release.
                    const auto row = destination.freeze();
                    REQUIRE(row.has_value());
                    const auto fields = projected ? destination.projected_auxiliary() : row->alignment->auxiliary;
                    REQUIRE(fields.size() == 1u);
                    REQUIRE((fields.front().tag == std::array<char, 2>{'X', 'Z'}));
                    REQUIRE(fields.front().type == expected_type);
                    REQUIRE(std::ranges::equal(fields.front().payload,
                        std::as_bytes(std::span{expected.data(), expected.size()})));
                }
                REQUIRE(admission.reserved_bytes() == baseline);
            }
        }
    }
    const std::array<std::tuple<std::string_view, std::string_view, std::size_t, std::size_t>, 13> auxiliary_errors{{
        {R"json([{"tag":"XZ","type":"A","value":""}])json", "invalid preserved character/hex auxiliary length", 1u, 0u},
        {R"json([{"tag":"XZ","type":"H","value":"A"}])json", "invalid preserved character/hex auxiliary length", 1u, 1u},
        {R"json([{"tag":"XZ","type":"H","value":"AG"}])json", "preserved auxiliary text must be printable ASCII or uppercase hex", 1u, 0u},
        {R"json([{"tag":"XZ","type":"A","value":" "}])json", "preserved character auxiliary must be one non-space printable byte", 1u, 0u},
        {R"json([{"tag":"XZ","type":"Z","value":"\u0001"}])json", "preserved auxiliary text must be printable ASCII or uppercase hex", 1u, 0u},
        {R"json([{"tag":"XZ","type":"Z","value":"λ"}])json", "preserved auxiliary text must be printable ASCII or uppercase hex", 1u, 0u},
        {"[{\"tag\":\"XZ\",\"type\":\"Z\",\"value\":\"\x7f\"}]", "preserved auxiliary text must be printable ASCII or uppercase hex", 1u, 0u},
        {R"json([{"value":"plain","tag":"XZ","type":"?"}])json", "invalid preserved auxiliary scalar type", 0u, 0u},
        {R"json([{"value":"plain","tag":"1Z","type":"Z"}])json", "invalid or duplicate preserved auxiliary tag", 0u, 0u},
        {R"json([{"tag":"XZ","type":"Z","value":"plain","value":"again"}])json", "duplicate preservation key: value", 0u, 0u},
        {R"json([{"tag":"XZ","type":"Z","value":"plain","extra":null}])json", "unknown preservation key: extra", 0u, 0u},
        {R"json([{"tag":"XZ","type":"Z","value":"\uD800"}])json", "JSON high surrogate lacks a low surrogate", 0u, 0u},
        {R"json([{"tag":"XZ","type":"Z","value":"plain)json", "unterminated preservation JSON string", 0u, 0u}}};
    for (const auto& [json, error, begins, appends] : auxiliary_errors) {
        CAPTURE(json, error);
        {
            observed_record destination;
            const auto parsed = decode_auxiliary_json(json, destination, 8u);
            REQUIRE_FALSE(parsed.has_value());
            REQUIRE(parsed.error() == error);
            REQUIRE(destination.auxiliary_begins == begins);
            REQUIRE(destination.auxiliary_appends == appends);
            REQUIRE(destination.auxiliary_ends == 0u);
        }
        REQUIRE(admission.reserved_bytes() == baseline);
    }
    admission.set_index_memory_cap_bytes(std::max<std::uint64_t>(baseline, 1u));
    for (const auto json : {
        R"json([{"tag":"XZ","type":"Z","value":"plain"}])json",
        R"json([{"tag":"XZ","type":"H","value":"AG"}])json"}) {
        observed_record destination;
        const auto parsed = decode_auxiliary_json(json, destination, 8u);
        REQUIRE_FALSE(parsed.has_value());
        REQUIRE(parsed.error() == "Cannot admit IO vector growth peak");
        REQUIRE(destination.auxiliary_begins == 1u);
        REQUIRE(destination.auxiliary_appends == 0u);
        REQUIRE(destination.auxiliary_ends == 0u);
        REQUIRE(admission.reserved_bytes() == baseline);
    }
}

TEST_CASE("preservation manifests copy borrowed names and escaped attributes under admission",
          "[component:bioseq][layer:unit][feature:io]") {
    auto& admission = bioseq::parallelism::memory_admission_controller::instance();
    const auto baseline = admission.reserved_bytes();
    const std::string name(4096u, 'r');
    std::string escaped;
    for (std::size_t index = 0; index < 512u; ++index) escaped += "\\u0064";
    const std::string payload = "{\"magic\":\"bioseq.io\",\"version\":1,\"record_payload\":\"bq\","
        "\"sequence_alphabets\":[\"dna4\"],\"quality_alphabets\":[],\"replay_header_lines\":null,\"contexts\":[],\"references\":[{"
        "\"wire_name\":\"" + name + "\",\"source_id\":\"1\",\"context\":null,\"original_name\":\""
        + name + "\",\"alphabet\":\"dna4\",\"length\":\"100\",\"representation\":\"native\","
        "\"topology\":\"linear\",\"attributes\":{\"DS\":\"" + escaped + "\"}}]}";
    {
        const auto decoded = bioseq::io::alignment::decode_alignment_manifest(payload, payload.size());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->source_metadata.references.size() == 1u);
        REQUIRE(decoded->source_metadata.references.front().name == name);
        REQUIRE(decoded->wire_references.front().name == name);
        REQUIRE(decoded->source_metadata.references.front().attributes.front().value == std::string(512u, 'd'));
        REQUIRE_FALSE(decoded->source_metadata.replay_header_lines.has_value());
        REQUIRE(admission.reserved_bytes() > baseline);
    }
    REQUIRE(admission.reserved_bytes() == baseline);
    const auto replay_position = payload.find("\"replay_header_lines\":null");
    REQUIRE(replay_position != std::string::npos);
    auto with_replay = payload;
    with_replay.replace(replay_position, std::string_view{"\"replay_header_lines\":null"}.size(),
        "\"replay_header_lines\":[\"@CO\\t" + escaped + "\"]");
    {
        auto decoded = bioseq::io::alignment::decode_alignment_manifest(with_replay, with_replay.size());
        REQUIRE(decoded.has_value());
        REQUIRE(decoded->source_metadata.replay_header_lines.has_value());
        REQUIRE(decoded->source_metadata.replay_header_lines->size() == 1u);
        REQUIRE(decoded->source_metadata.replay_header_lines->front() == "@CO\t" + std::string(512u, 'd'));
        const auto before_reset = admission.reserved_bytes();
        REQUIRE(decoded->begin_replay_header(true).has_value());
        REQUIRE(decoded->source_metadata.replay_header_lines->empty());
        REQUIRE(admission.reserved_bytes() < before_reset);
        REQUIRE(decoded->replay_header_line("@CO\tactual native comment").has_value());
        REQUIRE_FALSE(decoded->replay_header_line("@SQ\tSN:ref\tLN:10").has_value());
    }
    REQUIRE(admission.reserved_bytes() == baseline);
    {
        bioseq::parallelism::memory_permit container_permit;
        bioseq::parallelism::memory_permit text_permit;
        std::vector<std::string> lines;
        REQUIRE(bioseq::io::append_admitted_text(lines, container_permit, text_permit, "@CO\tx").has_value());
        while (lines.size() < lines.capacity())
            REQUIRE(bioseq::io::append_admitted_text(lines, container_permit, text_permit, "@CO\tx").has_value());
        const std::string_view borrowed = lines.front();
        const auto previous_count = lines.size();
        REQUIRE(bioseq::io::append_admitted_text(lines, container_permit, text_permit, borrowed).has_value());
        REQUIRE(lines.size() == previous_count + 1u);
        REQUIRE(lines.back() == "@CO\tx");
    }
    REQUIRE(admission.reserved_bytes() == baseline);
}
