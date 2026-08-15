// Unit coverage for identifier header definitions and name normalization.
#include <tests/support/common.hpp>

#include <array>
#include <string>
#include <string_view>

import bioseq.common.logging;
import bioseq.io.identifier_parser.formats;

using namespace bioseq::io::identifier_parser;

TEST_CASE("identifier_formats: SQL names parse case/separator-insensitively",
          "[component:bioseq][layer:unit][feature:io]") {
    REQUIRE(parse_header_format_name("uni_prot").value() == header_format::uniprotkb);
    REQUIRE(parse_header_format_name("Uni-Prot").value() == header_format::uniprotkb);
    REQUIRE(parse_header_format_name("GENBANK").value() == header_format::genbank);
    REQUIRE(parse_header_format_name("ncbi_gi").value() == header_format::ncbi_gi);
    REQUIRE(parse_header_format_name("auto").value() == header_format::automatic);
    REQUIRE(parse_header_format_name("imgt").value() == header_format::imgt);
    REQUIRE_FALSE(parse_header_format_name("unknown-format").has_value());
    REQUIRE(parse_header_format_name("unknown-format").error().find("valid values") !=
            std::string::npos);
}

namespace {

/// Resolve a field by canonical name, token, or normalised alias.
[[nodiscard]] const header_field_definition* find_field(const header_format_definition& def,
                                                        std::string_view name) {
    std::array<char, 64> scratch{};
    const auto n = normalise_name(name, scratch);
    for (const auto& fd : def.fields) {
        if (fd.normalised_key == n)
            return &fd;
        if (!fd.token.empty()) {
            std::array<char, 64> token_scratch{};
            if (normalise_name(fd.token, token_scratch) == n)
                return &fd;
        }
        for (const std::string_view alias : fd.aliases) {
            std::array<char, 64> alias_scratch{};
            if (normalise_name(alias, alias_scratch) == n)
                return &fd;
        }
    }
    return nullptr;
}

} // namespace

TEST_CASE("identifier_formats: UniProt aliases route to canonical keys",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto& def = header_format_definition_for(header_format::uniprotkb);
    REQUIRE(find_field(def, "sv")->normalised_key == std::string_view{"sequenceversion"});
    REQUIRE(find_field(def, "Sequence-Version")->normalised_key ==
            std::string_view{"sequenceversion"});
    REQUIRE(find_field(def, "name")->normalised_key == std::string_view{"proteinname"});
    REQUIRE(find_field(def, "protein_name")->normalised_key == std::string_view{"proteinname"});
    REQUIRE(find_field(def, "identifier")->normalised_key == std::string_view{"accession"});
    REQUIRE(find_field(def, "OS")->normalised_key == std::string_view{"organismname"});
    REQUIRE(find_field(def, "entry")->normalised_key == std::string_view{"entryname"});
}

TEST_CASE("identifier_formats: IMGT positional field table is complete",
          "[component:bioseq][layer:unit][feature:io]") {
    const auto& def = header_format_definition_for(header_format::imgt);
    REQUIRE(def.fields.size() == 15);
    REQUIRE(def.primary_delimiter == '|');
    REQUIRE(def.metadata == metadata_style::none);
    // `positions` is index 5 — the field the int4range column rewrites.
    const header_field_definition* positions = find_field(def, "positions");
    REQUIRE(positions != nullptr);
    REQUIRE(positions->position == 5);
}

TEST_CASE("identifier_formats: normalise_name lowercases and drops separators",
          "[component:bioseq][layer:unit][feature:io]") {
    std::array<char, 64> scratch{};
    REQUIRE(normalise_name("Sequence_Version", scratch) == std::string_view{"sequenceversion"});
    REQUIRE(normalise_name("Organism-Name", scratch) == std::string_view{"organismname"});
    REQUIRE(normalise_name("GAPS", scratch) == std::string_view{"gaps"});
}
