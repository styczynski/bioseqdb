#include "common_tests.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] std::filesystem::path guc_ledger_path() {
    return std::filesystem::path{__FILE__}
        .parent_path()
        .parent_path()
        .parent_path()
        / "testdata"
        / "doc0388_guc_parity_ledger.json";
}

[[nodiscard]] constexpr std::uint64_t ledger_hash(
    std::string_view document) noexcept {
    std::uint64_t hash = 14695981039346656037ULL;
    for (char const value : document) {
        hash ^= static_cast<unsigned char>(value);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// This capture-only fingerprint guards the checked fixture. Generator
// contract tests independently compare both generated profiles and their
// manifest proof with the parsed ledger.
inline constexpr std::uint64_t captured_guc_ledger_hash =
    0x9e63e1217aeb2d98ULL;

}  // namespace

TEST_CASE(
    "DOC0388 GUC parity ledger has the exact legacy inventory",
    "[doc0388][guc_ledger]") {
    auto const fixture = guc_ledger_path();
    std::ifstream input{fixture};
    CAPTURE(fixture);
    REQUIRE(input.good());

    std::string const document{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};

    REQUIRE(ledger_hash(document) == captured_guc_ledger_hash);
    REQUIRE(document.contains(
        R"("description_authority":"cleaned_doxygen_only")"));
    REQUIRE(document.contains(
        R"("baseline_legacy_description_hash_is_non_authoritative":true)"));
    REQUIRE(document.contains(
        R"("profiles":{"production":{"row_count":76},"test_indexes":{"row_count":77}})"));

    // Legacy prose is evidence only. Generated descriptions must come from
    // cleaned Doxygen and cannot select a target text field from this ledger.
    REQUIRE_FALSE(document.contains(R"("short_description")"));
    REQUIRE_FALSE(document.contains(R"("long_description")"));

    std::regex const row_pattern{
        R"guc(^\s*\[([0-9]+),"([^"]+)","([^"]+)","([^"]+)","([^"]+)","([^"]+)".*$)guc"};
    std::istringstream lines{document};
    std::string line;
    std::size_t expected_ordinal = 0;
    std::size_t production_rows = 0;
    std::size_t test_rows = 0;
    std::map<std::string, std::size_t> owner_counts;
    std::map<std::string, std::size_t> kind_counts;
    std::map<std::string, std::size_t> context_counts;
    std::set<std::string> names;

    while (std::getline(lines, line)) {
        std::smatch match;
        if (!std::regex_match(line, match, row_pattern)) {
            continue;
        }

        auto const ordinal =
            static_cast<std::size_t>(std::stoul(match[1].str()));
        auto const owner = match[2].str();
        auto const name = match[3].str();
        auto const group = match[4].str();
        auto const member = match[5].str();
        auto const kind = match[6].str();

        REQUIRE(ordinal == expected_ordinal);
        ++expected_ordinal;
        REQUIRE(name == group + "." + member);
        REQUIRE(names.insert(name).second);
        ++owner_counts[owner];
        ++kind_counts[kind];

        REQUIRE(line.contains("[null,null,null]"));
        if (line.contains(R"("superuser_set")")) {
            ++context_counts["superuser_set"];
        } else if (line.contains(R"("sighup")")) {
            ++context_counts["sighup"];
        } else {
            REQUIRE(line.contains(R"("user_set")"));
            ++context_counts["user_set"];
        }

        if (line.contains(R"(["production","test_indexes"])")) {
            ++production_rows;
            ++test_rows;
        } else {
            REQUIRE(ordinal == 24);
            REQUIRE(name == "bioseq.test_force_paper_index_failure");
            REQUIRE(line.contains(
                R"guc("defined(BIOSEQ_TEST_INDEXES)",["test_indexes"])guc"));
            ++test_rows;
        }
    }

    REQUIRE(expected_ordinal == 77);
    REQUIRE(names.size() == 77);
    REQUIRE(production_rows == 76);
    REQUIRE(test_rows == 77);
    std::map<std::string, std::size_t> const expected_owner_counts{
        {"batch_sort_scan", 1},
        {"column", 71},
        {"custom_scan", 5}};
    std::map<std::string, std::size_t> const expected_kind_counts{
        {"bool", 25},
        {"enum", 10},
        {"integer", 37},
        {"real", 5}};
    std::map<std::string, std::size_t> const expected_context_counts{
        {"sighup", 1},
        {"superuser_set", 6},
        {"user_set", 70}};
    REQUIRE(owner_counts == expected_owner_counts);
    REQUIRE(kind_counts == expected_kind_counts);
    REQUIRE(context_counts == expected_context_counts);

    REQUIRE(document.contains(
        R"(["debug","DEBUG2",1,"true","debug2"])"));
    REQUIRE(document.contains(
        R"([["invalid","-1"],["count","4"]])"));
    REQUIRE(document.contains(
        R"([["ksw2_batched","2"]])"));
}
