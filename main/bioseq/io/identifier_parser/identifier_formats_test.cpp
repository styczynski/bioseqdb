// bioseq/io/identifier_parser/identifier_formats_test.cpp
//
// DOC0295 §11.1 — NO_POSTGRES unit tests for the format/field tables and the
// name-normalisation rule. Compiles into the normal unit-test executable.
#include "common_tests.h"
#include "common.h"

#include <array>
#include <string>
#include <string_view>

import mlogging;
import bioseq_io.identifier_formats;

using namespace bioseq::io::idparse;

TEST_CASE("identifier_formats: SQL names parse case/separator-insensitively",
          "[io-identifier-formats]") {
    REQUIRE(header_format_from_sql_name("uni_prot").value() == header_format::UNIPROTKB);
    REQUIRE(header_format_from_sql_name("Uni-Prot").value() == header_format::UNIPROTKB);
    REQUIRE(header_format_from_sql_name("GENBANK").value() == header_format::GENBANK);
    REQUIRE(header_format_from_sql_name("ncbi_gi").value() == header_format::NCBI_GI);
    REQUIRE(header_format_from_sql_name("auto").value() == header_format::AUTO);
    REQUIRE(header_format_from_sql_name("imgt").value() == header_format::IMGT);
    REQUIRE_FALSE(header_format_from_sql_name("unknown-format").has_value());
    REQUIRE(header_format_from_sql_name("unknown-format").error().find("valid values")
            != std::string::npos);
}

namespace {

// Mirror the field-resolution match (canonical / token / alias, normalised).
[[nodiscard]] const header_field_def* find_field(
        const header_format_def& def, std::string_view name) {
    std::array<char, 64> scratch{};
    const auto n = normalise_name(name, scratch);
    for (const auto& fd : def.fields) {
        if (fd.norm_key == n) return &fd;
        if (!fd.token.empty()) {
            std::array<char, 64> token_scratch{};
            if (normalise_name(fd.token, token_scratch) == n) return &fd;
        }
        for (const std::string_view alias : fd.aliases) {
            std::array<char, 64> alias_scratch{};
            if (normalise_name(alias, alias_scratch) == n) return &fd;
        }
    }
    return nullptr;
}

}  // namespace

TEST_CASE("identifier_formats: UniProt aliases route to canonical keys",
          "[io-identifier-formats]") {
    const auto& def = format_def(header_format::UNIPROTKB);
    REQUIRE(find_field(def, "sv")->norm_key == std::string_view{"sequenceversion"});
    REQUIRE(find_field(def, "Sequence-Version")->norm_key
            == std::string_view{"sequenceversion"});
    REQUIRE(find_field(def, "name")->norm_key == std::string_view{"proteinname"});
    REQUIRE(find_field(def, "protein_name")->norm_key == std::string_view{"proteinname"});
    REQUIRE(find_field(def, "identifier")->norm_key == std::string_view{"accession"});
    REQUIRE(find_field(def, "OS")->norm_key == std::string_view{"organismname"});
    REQUIRE(find_field(def, "entry")->norm_key == std::string_view{"entryname"});
}

TEST_CASE("identifier_formats: IMGT positional field table is complete (T11/F22)",
          "[io-identifier-formats]") {
    const auto& def = format_def(header_format::IMGT);
    REQUIRE(def.fields.size() == 15);
    REQUIRE(def.primary_delim == '|');
    REQUIRE(def.meta == metadata_style::NONE);
    // `positions` is index 5 — the field the int4range column rewrites.
    const header_field_def* positions = find_field(def, "positions");
    REQUIRE(positions != nullptr);
    REQUIRE(positions->position == 5);
}

TEST_CASE("identifier_formats: normalise_name lowercases and drops separators",
          "[io-identifier-formats]") {
    std::array<char, 64> scratch{};
    REQUIRE(normalise_name("Sequence_Version", scratch) == std::string_view{"sequenceversion"});
    REQUIRE(normalise_name("Organism-Name", scratch) == std::string_view{"organismname"});
    REQUIRE(normalise_name("GAPS", scratch) == std::string_view{"gaps"});
}
