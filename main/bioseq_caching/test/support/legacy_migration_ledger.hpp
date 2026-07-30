#pragma once

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <istream>
#include <iterator>
#include <map>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bioseq::cache_test::migration {

inline constexpr std::string_view ledger_header{
    "legacy_path\tlegacy_line_or_assertion\tstable_id\t"
    "evidence_harness\tevidence_role\t"
    "source_kind\tscenario_or_permutation\t"
    "expected_sqlstate_or_result\texpected_state_invariant\t"
    "catch2_case\tcatch2_section\tcatch2_tags\tmigration_status"};
inline constexpr std::string_view legacy_inventory_header{
    "legacy_path\tsha256"};
inline constexpr std::size_t legacy_inventory_size{85};

enum class status {
    mapped,
    passing,
    retired_with_proof,
};

enum class evidence_harness {
    cache_unit,
    cache_integration,
    cache_bioseq_integration,
    cache_bench,
    cache_bioseq_bench,
};

enum class evidence_role {
    cache_integration,
    cache_bioseq_support_integration,
    cache_bioseq_production_integration,
    cache_bench,
    cache_bioseq_bench,
};

enum class retirement_phase {
    tracking,
    proof_ready,
    retired,
};

[[nodiscard]] inline constexpr std::string_view harness_name(
    evidence_harness value) noexcept {
    switch (value) {
    case evidence_harness::cache_unit:
        return "cache-unit";
    case evidence_harness::cache_integration:
        return "cache-integration";
    case evidence_harness::cache_bioseq_integration:
        return "cache-bioseq-integration";
    case evidence_harness::cache_bench:
        return "cache-bench";
    case evidence_harness::cache_bioseq_bench:
        return "cache-bioseq-bench";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr std::string_view role_name(
    evidence_role value) noexcept {
    switch (value) {
    case evidence_role::cache_integration:
        return "cache-integration";
    case evidence_role::cache_bioseq_support_integration:
        return "cache-bioseq-support-integration";
    case evidence_role::cache_bioseq_production_integration:
        return "cache-bioseq-production-integration";
    case evidence_role::cache_bench:
        return "cache-bench";
    case evidence_role::cache_bioseq_bench:
        return "cache-bioseq-bench";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr evidence_harness role_harness(
    evidence_role value) noexcept {
    switch (value) {
    case evidence_role::cache_integration:
        return evidence_harness::cache_integration;
    case evidence_role::cache_bioseq_support_integration:
    case evidence_role::cache_bioseq_production_integration:
        return evidence_harness::cache_bioseq_integration;
    case evidence_role::cache_bench:
        return evidence_harness::cache_bench;
    case evidence_role::cache_bioseq_bench:
        return evidence_harness::cache_bioseq_bench;
    }
    return evidence_harness::cache_unit;
}

struct ledger_row {
    std::string legacy_path;
    std::string legacy_line_or_assertion;
    std::string stable_id;
    evidence_harness owner{};
    evidence_role role{};
    std::string source_kind;
    std::string scenario_or_permutation;
    std::string expected_sqlstate_or_result;
    std::string expected_state_invariant;
    std::string catch2_case;
    std::string catch2_section;
    std::string catch2_tags;
    status migration_status{};
    std::size_t line_number{};
};

struct ledger {
    std::vector<ledger_row> rows;
};

struct global_validation {
    bool deletion_ready{};
    bool proof_ready{};
    bool retirement_complete{};
    bool legacy_deletion_started{};
    std::size_t mapped_rows{};
    std::size_t passing_rows{};
    std::size_t retired_with_proof_rows{};
};

struct legacy_asset_record {
    std::filesystem::path path;
    std::string sha256;
};

/// @brief One report-owned runtime proof used by the final union gate.
struct probe_observation {
    evidence_harness owner{};
    evidence_role role{};
    std::string stable_id;
    std::string test_case;
    std::string section;
    std::string permutation;
    std::string origin_kind;
    std::string origin;
    bool passed{true};
};

namespace detail {

[[nodiscard]] inline std::vector<std::string> split_tsv(
    const std::string& line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', begin);
        fields.push_back(line.substr(
            begin, tab == std::string::npos
                ? std::string::npos : tab - begin));
        if (tab == std::string::npos) {
            return fields;
        }
        begin = tab + 1;
    }
}

[[nodiscard]] inline status parse_status(
    std::string_view text,
    std::size_t line_number) {
    if (text == "mapped") {
        return status::mapped;
    }
    if (text == "passing") {
        return status::passing;
    }
    if (text == "retired_with_proof") {
        return status::retired_with_proof;
    }
    throw std::runtime_error(
        "migration ledger line " + std::to_string(line_number)
        + " has invalid migration_status '" + std::string(text)
        + "'; expected mapped, passing, or retired_with_proof");
}

[[nodiscard]] inline evidence_harness parse_harness(
    std::string_view text,
    std::size_t line_number = 0) {
    if (text == "cache-unit") {
        return evidence_harness::cache_unit;
    }
    if (text == "cache-integration") {
        return evidence_harness::cache_integration;
    }
    if (text == "cache-bioseq-integration") {
        return evidence_harness::cache_bioseq_integration;
    }
    if (text == "cache-bench") {
        return evidence_harness::cache_bench;
    }
    if (text == "cache-bioseq-bench") {
        return evidence_harness::cache_bioseq_bench;
    }
    throw std::runtime_error(
        (line_number == 0
             ? std::string{"invalid migration evidence_harness '"}
             : "migration ledger line " + std::to_string(line_number)
                   + " has invalid evidence_harness '")
        + std::string(text)
        + "'; expected cache-unit, cache-integration, "
          "cache-bioseq-integration, cache-bench, or "
          "cache-bioseq-bench");
}

[[nodiscard]] inline evidence_role parse_role(
    std::string_view text,
    std::size_t line_number = 0) {
    if (text == "cache-integration") {
        return evidence_role::cache_integration;
    }
    if (text == "cache-bioseq-support-integration") {
        return evidence_role::cache_bioseq_support_integration;
    }
    if (text == "cache-bioseq-production-integration") {
        return evidence_role::cache_bioseq_production_integration;
    }
    if (text == "cache-bench") {
        return evidence_role::cache_bench;
    }
    if (text == "cache-bioseq-bench") {
        return evidence_role::cache_bioseq_bench;
    }
    throw std::runtime_error(
        (line_number == 0
             ? std::string{"invalid migration evidence_role '"}
             : "migration ledger line " + std::to_string(line_number)
                   + " has invalid evidence_role '")
        + std::string(text)
        + "'; expected one of the five native report roles");
}

[[nodiscard]] inline bool is_tracked_legacy_path(
    std::string_view path) {
    return path.starts_with("test/") || path.starts_with("bench/")
        || path.starts_with("sql/");
}

}  // namespace detail

[[nodiscard]] inline ledger parse_ledger(std::istream& input) {
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("migration ledger is empty");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != ledger_header) {
        throw std::runtime_error(
            "migration ledger has an unexpected header");
    }

    ledger result;
    std::unordered_set<std::string> stable_ids;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " is empty");
        }
        std::vector<std::string> fields = detail::split_tsv(line);
        if (fields.size() != 13) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " does not have 13 fields");
        }
        if (fields[2].empty()) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " has an empty stable_id");
        }
        if (!stable_ids.insert(fields[2]).second) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " duplicates stable_id '" + fields[2] + "'");
        }

        const status migration_status =
            detail::parse_status(fields[12], line_number);
        const evidence_harness owner =
            detail::parse_harness(fields[3], line_number);
        const evidence_role role =
            detail::parse_role(fields[4], line_number);
        if (role_harness(role) != owner) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " assigns evidence_role '" + fields[4]
                + "' to incompatible evidence_harness '" + fields[3]
                + "'");
        }
        if (fields[9].empty()) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " has an empty catch2_case mapping");
        }
        if (migration_status == status::retired_with_proof
            && (fields[6].empty() || fields[8].empty())) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " retires without a replacement scenario and rationale");
        }

        result.rows.push_back(ledger_row{
            .legacy_path = std::move(fields[0]),
            .legacy_line_or_assertion = std::move(fields[1]),
            .stable_id = std::move(fields[2]),
            .owner = owner,
            .role = role,
            .source_kind = std::move(fields[5]),
            .scenario_or_permutation = std::move(fields[6]),
            .expected_sqlstate_or_result = std::move(fields[7]),
            .expected_state_invariant = std::move(fields[8]),
            .catch2_case = std::move(fields[9]),
            .catch2_section = std::move(fields[10]),
            .catch2_tags = std::move(fields[11]),
            .migration_status = migration_status,
            .line_number = line_number});
    }
    if (!input.eof()) {
        throw std::runtime_error("failed while reading migration ledger");
    }
    if (result.rows.empty()) {
        throw std::runtime_error("migration ledger has no rows");
    }
    return result;
}

[[nodiscard]] inline ledger load_ledger_document(
    std::string_view raw_document) {
    std::istringstream input{std::string(raw_document)};
    return parse_ledger(input);
}

[[nodiscard]] inline ledger load_ledger(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot read migration ledger: " + path.string());
    }
    const std::string raw_document{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        throw std::runtime_error(
            "cannot read migration ledger: " + path.string());
    }
    try {
        return load_ledger_document(raw_document);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "invalid migration ledger " + path.string() + ": "
            + error.what());
    }
}

[[nodiscard]] inline std::vector<legacy_asset_record>
load_legacy_asset_inventory(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(
            "cannot read legacy asset inventory: " + path.string());
    }
    std::string line;
    if (!std::getline(input, line)) {
        throw std::runtime_error("legacy asset inventory is empty");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    if (line != legacy_inventory_header) {
        throw std::runtime_error(
            "legacy asset inventory has an unexpected header");
    }

    std::vector<legacy_asset_record> result;
    std::unordered_set<std::string> unique_paths;
    std::string previous_path;
    std::size_t line_number = 1;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const std::vector<std::string> fields =
            detail::split_tsv(line);
        if (fields.size() != 2 || fields[0].empty()) {
            throw std::runtime_error(
                "legacy asset inventory line "
                + std::to_string(line_number)
                + " must contain one path and SHA-256");
        }
        const std::filesystem::path candidate{fields[0]};
        if (candidate.is_absolute()
            || candidate.has_root_name()
            || candidate.has_root_directory()
            || candidate.lexically_normal().generic_string()
                != fields[0]) {
            throw std::runtime_error(
                "legacy asset inventory line "
                + std::to_string(line_number)
                + " is not a normalized module-relative path: "
                + fields[0]);
        }
        for (const auto& component : candidate) {
            if (component.empty() || component == "."
                || component == "..") {
                throw std::runtime_error(
                    "legacy asset inventory line "
                    + std::to_string(line_number)
                    + " contains an unsafe path component");
            }
        }
        const auto first = candidate.begin();
        if (first == candidate.end()
            || (*first != "bench" && *first != "sql"
                && *first != "test")) {
            throw std::runtime_error(
                "legacy asset inventory line "
                + std::to_string(line_number)
                + " is outside the cache legacy roots");
        }
        if (fields[1].size() != 64
            || !std::ranges::all_of(
                fields[1], [](char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'a' && character <= 'f');
                })) {
            throw std::runtime_error(
                "legacy asset inventory line "
                + std::to_string(line_number)
                + " has invalid lowercase SHA-256");
        }
        if (!unique_paths.insert(fields[0]).second) {
            throw std::runtime_error(
                "legacy asset inventory duplicates " + fields[0]);
        }
        if (!previous_path.empty() && fields[0] <= previous_path) {
            throw std::runtime_error(
                "legacy asset inventory paths must be strictly sorted: "
                + fields[0]);
        }
        previous_path = fields[0];
        result.push_back(legacy_asset_record{
            .path = candidate,
            .sha256 = fields[1]});
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "failed while reading legacy asset inventory");
    }
    if (result.size() != legacy_inventory_size) {
        throw std::runtime_error(
            "legacy asset inventory has "
            + std::to_string(result.size()) + " entries; expected "
            + std::to_string(legacy_inventory_size));
    }
    return result;
}

[[nodiscard]] inline evidence_harness parse_evidence_harness(
    std::string_view text) {
    return detail::parse_harness(text);
}

[[nodiscard]] inline evidence_role parse_evidence_role(
    std::string_view text) {
    return detail::parse_role(text);
}

/// @brief Canonical evidence contract excluding lifecycle-only status.
///
/// Runtime receipts bind to this payload so an audited mapped -> passing ->
/// retired transition does not invalidate already executed long-running
/// evidence. Every owner, mapping, expected result, invariant, and count stays
/// sealed.
[[nodiscard]] inline std::string canonical_evidence_contract_material(
    const ledger& value) {
    std::string result{
        "bioseq-cache-migration-evidence-contract/v1\n"};
    const auto append_field =
        [&result](std::string_view field) {
            result += std::to_string(field.size());
            result += ':';
            result.append(field.data(), field.size());
        };
    for (const ledger_row& row : value.rows) {
        append_field(row.legacy_path);
        append_field(row.legacy_line_or_assertion);
        append_field(row.stable_id);
        append_field(harness_name(row.owner));
        append_field(role_name(row.role));
        append_field(row.source_kind);
        append_field(row.scenario_or_permutation);
        append_field(row.expected_sqlstate_or_result);
        append_field(row.expected_state_invariant);
        append_field(row.catch2_case);
        append_field(row.catch2_section);
        append_field(row.catch2_tags);
        result += '\n';
    }
    return result;
}

[[nodiscard]] inline std::string_view ledger_phase_name(
    const ledger& value) noexcept {
    if (value.rows.empty()) {
        return "empty";
    }
    const status first = value.rows.front().migration_status;
    if (!std::ranges::all_of(
            value.rows,
            [first](const ledger_row& row) {
                return row.migration_status == first;
            })) {
        return "mixed";
    }
    switch (first) {
    case status::mapped:
        return "mapped";
    case status::passing:
        return "passing";
    case status::retired_with_proof:
        return "retired_with_proof";
    }
    return "unknown";
}

[[nodiscard]] inline bool requires_probe(
    const ledger_row& row,
    evidence_harness owner,
    evidence_role role) noexcept {
    return row.owner == owner && row.role == role;
}

[[nodiscard]] inline std::size_t expected_probe_count(
    const ledger_row& row) {
    constexpr std::string_view marker{"expected_count="};
    const std::string_view scenario{row.scenario_or_permutation};
    const std::size_t marker_position = scenario.find(marker);
    if (marker_position == std::string_view::npos) {
        return 1;
    }
    const std::string_view count_text =
        scenario.substr(marker_position + marker.size());
    std::size_t count{};
    const auto [end, error] = std::from_chars(
        count_text.data(), count_text.data() + count_text.size(), count);
    if (error != std::errc{} || count == 0
        || end != count_text.data() + count_text.size()) {
        throw std::runtime_error(
            "migration ledger line " + std::to_string(row.line_number)
            + " has invalid expected_count");
    }
    return count;
}

/// @brief Validates one report without requiring evidence owned elsewhere.
inline void validate_harness_evidence(
    const ledger& value,
    evidence_harness owner,
    std::span<const probe_observation> observations) {
    std::map<std::string, std::size_t, std::less<>> expected;
    std::map<std::string, const ledger_row*, std::less<>> expected_rows;
    for (const ledger_row& row : value.rows) {
        if (row.owner == owner) {
            expected.emplace(row.stable_id, expected_probe_count(row));
            expected_rows.emplace(row.stable_id, &row);
        }
    }

    std::map<std::string, std::size_t, std::less<>> actual;
    for (const probe_observation& observation : observations) {
        if (observation.owner != owner) {
            throw std::runtime_error(
                "migration report for " + std::string(harness_name(owner))
                + " contains evidence attributed to "
                + std::string(harness_name(observation.owner)) + ": "
                + observation.stable_id);
        }
        if (!observation.passed) {
            throw std::runtime_error(
                "non-passing migration evidence: "
                + observation.stable_id);
        }
        if (!expected.contains(observation.stable_id)) {
            throw std::runtime_error(
                "unlisted migration evidence for "
                + std::string(harness_name(owner)) + ": "
                + observation.stable_id);
        }
        const ledger_row& row = *expected_rows.at(
            observation.stable_id);
        const bool integration_owner =
            observation.owner == evidence_harness::cache_integration
            || observation.owner
                == evidence_harness::cache_bioseq_integration;
        const bool provenance_matches =
            integration_owner
            ? observation.test_case == row.catch2_case
                && observation.section == row.catch2_section
            : observation.test_case.empty()
                && observation.section.empty()
                && observation.origin_kind == "benchmark-phase"
                && !observation.origin.empty();
        if (observation.role != row.role || !provenance_matches) {
            throw std::runtime_error(
                "migration evidence provenance mismatch for "
                + observation.stable_id + ": expected role '"
                + std::string(role_name(row.role)) + "', Catch case '"
                + row.catch2_case + "', section '" + row.catch2_section
                + "'; observed role '"
                + std::string(role_name(observation.role)) + "', case '"
                + observation.test_case + "', section '"
                + observation.section + "', origin kind '"
                + observation.origin_kind + "', origin '"
                + observation.origin + "'");
        }
        ++actual[observation.stable_id];
    }
    for (const auto& [stable_id, expected_count] : expected) {
        const auto found = actual.find(stable_id);
        const std::size_t observed =
            found == actual.end() ? 0 : found->second;
        if (observed != expected_count) {
            throw std::runtime_error(
                "migration evidence count mismatch for " + stable_id
                + " owned by " + std::string(harness_name(owner))
                + ": expected " + std::to_string(expected_count)
                + ", observed " + std::to_string(observed));
        }
    }
}

/// @brief Validates one exact report role without accepting sibling evidence.
inline void validate_report_evidence(
    const ledger& value,
    evidence_harness owner,
    evidence_role role,
    std::span<const probe_observation> observations) {
    if (role_harness(role) != owner) {
        throw std::runtime_error(
            "migration report role " + std::string(role_name(role))
            + " is incompatible with evidence harness "
            + std::string(harness_name(owner)));
    }
    ledger role_ledger;
    for (const ledger_row& row : value.rows) {
        if (requires_probe(row, owner, role)) {
            role_ledger.rows.push_back(row);
        }
    }
    for (const probe_observation& observation : observations) {
        if (observation.role != role) {
            throw std::runtime_error(
                "migration report for " + std::string(role_name(role))
                + " contains evidence attributed to "
                + std::string(role_name(observation.role)) + ": "
                + observation.stable_id);
        }
    }
    validate_harness_evidence(role_ledger, owner, observations);
}

/// @brief Requires the union of report evidence to close every ledger row.
///
/// Per-report validators deliberately select just one owner. This separate
/// final gate reconstructs the full `(owner, stable_id)` key space so moving a
/// row between harnesses or relabelling it retired cannot make absent evidence
/// disappear. `retired_with_proof` changes lifecycle classification; it never
/// substitutes a self-declared TSV status for executed machine evidence.
inline void validate_combined_evidence(
    const ledger& value,
    std::span<const probe_observation> observations) {
    using role_key = std::pair<evidence_harness, evidence_role>;
    using key = std::pair<role_key, std::string>;
    std::map<key, std::size_t> expected;
    std::map<std::string, role_key, std::less<>> owners;
    std::map<std::string, const ledger_row*, std::less<>> expected_rows;
    for (const ledger_row& row : value.rows) {
        owners.emplace(
            row.stable_id, role_key{row.owner, row.role});
        expected_rows.emplace(row.stable_id, &row);
        expected.emplace(
            key{{row.owner, row.role}, row.stable_id},
            expected_probe_count(row));
    }

    std::map<key, std::size_t> actual;
    for (const probe_observation& observation : observations) {
        if (!observation.passed) {
            throw std::runtime_error(
                "non-passing combined migration evidence: "
                + observation.stable_id);
        }
        const auto known = owners.find(observation.stable_id);
        if (known == owners.end()) {
            throw std::runtime_error(
                "unlisted combined migration evidence: "
                + observation.stable_id);
        }
        if (known->second
            != role_key{observation.owner, observation.role}) {
            throw std::runtime_error(
                "migration evidence " + observation.stable_id
                + " is attributed to " + std::string(
                    harness_name(observation.owner))
                + "/" + std::string(role_name(observation.role))
                + " but the ledger owner/role is "
                + std::string(harness_name(known->second.first))
                + "/" + std::string(role_name(known->second.second)));
        }
        const ledger_row& row = *expected_rows.at(
            observation.stable_id);
        const bool integration_owner =
            observation.owner == evidence_harness::cache_integration
            || observation.owner
                == evidence_harness::cache_bioseq_integration;
        const bool provenance_matches =
            integration_owner
            ? observation.test_case == row.catch2_case
                && observation.section == row.catch2_section
            : observation.test_case.empty()
                && observation.section.empty()
                && observation.origin_kind == "benchmark-phase"
                && !observation.origin.empty();
        if (observation.role != row.role || !provenance_matches) {
            throw std::runtime_error(
                "combined migration evidence provenance mismatch for "
                + observation.stable_id + ": expected Catch case '"
                + row.catch2_case + "', section '" + row.catch2_section
                + "'; observed case '" + observation.test_case
                + "', section '" + observation.section + "'");
        }
        ++actual[
            key{{observation.owner, observation.role},
                observation.stable_id}];
    }

    for (const auto& [expected_key, expected_count] : expected) {
        const auto found = actual.find(expected_key);
        const std::size_t observed =
            found == actual.end() ? 0 : found->second;
        if (observed != expected_count) {
            throw std::runtime_error(
                "combined migration evidence count mismatch for "
                + expected_key.second + " owned by "
                + std::string(harness_name(
                    expected_key.first.first))
                + "/" + std::string(role_name(
                    expected_key.first.second))
                + ": expected " + std::to_string(expected_count)
                + ", observed " + std::to_string(observed));
        }
    }
    for (const auto& [actual_key, count] : actual) {
        const auto found = expected.find(actual_key);
        if (found == expected.end()) {
            throw std::runtime_error(
                "combined migration evidence has no ledger row for "
                + actual_key.second + " owned by "
                + std::string(harness_name(
                    actual_key.first.first))
                + "/" + std::string(role_name(
                    actual_key.first.second)));
        }
        if (found->second != count) {
            throw std::runtime_error(
                "combined migration evidence count mismatch for "
                + actual_key.second);
        }
    }
}

[[nodiscard]] inline std::unordered_set<std::string>
load_catch_case_manifest_document(std::string_view raw_document) {
    std::istringstream input{std::string(raw_document)};
    std::unordered_set<std::string> result;
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            throw std::runtime_error(
                "Catch case manifest line "
                + std::to_string(line_number) + " is empty");
        }
        if (!result.insert(line).second) {
            throw std::runtime_error(
                "Catch case manifest duplicates test case '" + line + "'");
        }
    }
    if (!input.eof()) {
        throw std::runtime_error(
            "failed while reading Catch case manifest");
    }
    if (result.empty()) {
        throw std::runtime_error("Catch case manifest has no test cases");
    }
    return result;
}

[[nodiscard]] inline std::unordered_set<std::string>
load_catch_case_manifest(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot read Catch case manifest: " + path.string());
    }
    const std::string raw_document{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        throw std::runtime_error(
            "cannot read Catch case manifest: " + path.string());
    }
    try {
        return load_catch_case_manifest_document(raw_document);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "invalid Catch case manifest " + path.string() + ": "
            + error.what());
    }
}

[[nodiscard]] inline global_validation validate_global_ledger(
    const ledger& value,
    const std::unordered_set<std::string>& catch_cases,
    const std::filesystem::path& legacy_root,
    const std::vector<std::filesystem::path>& legacy_assets = {},
    std::span<const probe_observation> combined_evidence = {},
    retirement_phase phase = retirement_phase::tracking,
    std::size_t expected_inventory_size = 0) {
    global_validation result;
    for (const ledger_row& row : value.rows) {
        if (!catch_cases.contains(row.catch2_case)) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(row.line_number)
                + " maps stable_id '" + row.stable_id
                + "' to unknown Catch2 case '" + row.catch2_case + "'");
        }
        switch (row.migration_status) {
        case status::mapped:
            ++result.mapped_rows;
            break;
        case status::passing:
            ++result.passing_rows;
            break;
        case status::retired_with_proof:
            ++result.retired_with_proof_rows;
            break;
        }
    }
    result.deletion_ready = result.mapped_rows == 0;
    if (expected_inventory_size != 0
        && legacy_assets.size() != expected_inventory_size) {
        throw std::runtime_error(
            "legacy asset inventory has "
            + std::to_string(legacy_assets.size()) + " entries; expected "
            + std::to_string(expected_inventory_size));
    }

    std::unordered_set<std::string> inventory_paths;
    std::size_t present_assets = 0;
    std::size_t absent_assets = 0;
    if (!legacy_assets.empty()) {
        std::error_code root_error;
        const auto root_status =
            std::filesystem::symlink_status(legacy_root, root_error);
        if (root_error || !std::filesystem::is_directory(root_status)
            || std::filesystem::is_symlink(root_status)) {
            throw std::runtime_error(
                "legacy asset root must be an existing non-symlink "
                "directory: " + legacy_root.string());
        }
    }
    for (const auto& asset : legacy_assets) {
        if (asset.empty() || asset.is_absolute()
            || asset.has_root_name() || asset.has_root_directory()
            || asset.lexically_normal() != asset) {
            throw std::runtime_error(
                "legacy asset inventory entries must be normalized, "
                "nonempty paths relative to the module root");
        }
        for (const auto& component : asset) {
            if (component.empty() || component == "."
                || component == "..") {
                throw std::runtime_error(
                    "legacy asset inventory cannot escape the module root: "
                    + asset.string());
            }
        }
        if (!inventory_paths.insert(asset.generic_string()).second) {
            throw std::runtime_error(
                "legacy asset inventory duplicates " + asset.string());
        }
        std::filesystem::path inspected = legacy_root;
        bool missing = false;
        std::size_t component_index = 0;
        const std::size_t component_count =
            static_cast<std::size_t>(
                std::distance(asset.begin(), asset.end()));
        for (const auto& component : asset) {
            ++component_index;
            inspected /= component;
            std::error_code error;
            const auto inspected_status =
                std::filesystem::symlink_status(inspected, error);
            if (error) {
                throw std::runtime_error(
                    "cannot inspect legacy asset component "
                    + inspected.string() + ": " + error.message());
            }
            if (!std::filesystem::exists(inspected_status)) {
                missing = true;
                break;
            }
            if (std::filesystem::is_symlink(inspected_status)) {
                throw std::runtime_error(
                    "legacy asset path contains a symlink: "
                    + inspected.string());
            }
            const bool final_component =
                component_index == component_count;
            if (!final_component
                && !std::filesystem::is_directory(inspected_status)) {
                throw std::runtime_error(
                    "legacy asset parent is not a directory: "
                    + inspected.string());
            }
            if (final_component
                && !std::filesystem::is_regular_file(
                    inspected_status)) {
                throw std::runtime_error(
                    "legacy asset inventory entry is not a regular file: "
                    + inspected.string());
            }
        }
        if (missing) {
            ++absent_assets;
            continue;
        }
        ++present_assets;
    }

    const bool has_tracked_rows = std::ranges::any_of(
        value.rows, [](const ledger_row& row) {
            return detail::is_tracked_legacy_path(row.legacy_path);
        });
    if (has_tracked_rows && inventory_paths.empty()) {
        throw std::runtime_error(
            "tracked legacy rows require a nonempty exact asset inventory");
    }
    for (const ledger_row& row : value.rows) {
        if (detail::is_tracked_legacy_path(row.legacy_path)
            && !inventory_paths.contains(row.legacy_path)) {
            throw std::runtime_error(
                "legacy asset inventory omits ledger path "
                + row.legacy_path);
        }
    }

    result.legacy_deletion_started = absent_assets != 0;
    if (present_assets != 0 && absent_assets != 0) {
        throw std::runtime_error(
            "legacy asset inventory is partially deleted: "
            + std::to_string(present_assets) + " present, "
            + std::to_string(absent_assets)
            + " absent; proof and retirement transitions must be atomic");
    }

    if (phase == retirement_phase::tracking) {
        if (absent_assets != 0) {
            throw std::runtime_error(
                "legacy asset deletion started during migration tracking");
        }
        return result;
    }

    if (!result.deletion_ready) {
        throw std::runtime_error(
            "migration ledger cannot enter proof-ready or retired phase "
            "while " + std::to_string(result.mapped_rows)
            + " rows remain mapped");
    }
    if (phase == retirement_phase::proof_ready
        && (result.passing_rows != value.rows.size()
            || result.retired_with_proof_rows != 0)) {
        throw std::runtime_error(
            "proof-ready phase requires every migration-ledger row to be "
            "passing and none retired_with_proof");
    }
    if (phase == retirement_phase::retired
        && (result.retired_with_proof_rows != value.rows.size()
            || result.passing_rows != 0)) {
        throw std::runtime_error(
            "retired phase requires every migration-ledger row to be "
            "retired_with_proof and none passing");
    }
    validate_combined_evidence(value, combined_evidence);
    if (phase == retirement_phase::proof_ready) {
        if (present_assets != legacy_assets.size()) {
            throw std::runtime_error(
                "proof-ready phase requires every exact legacy asset "
                "to remain present");
        }
        result.proof_ready = true;
        return result;
    }
    if (absent_assets != legacy_assets.size()) {
        throw std::runtime_error(
            "retired phase requires every exact legacy asset to be absent");
    }
    result.proof_ready = true;
    result.retirement_complete = true;
    return result;
}

}  // namespace bioseq::cache_test::migration
