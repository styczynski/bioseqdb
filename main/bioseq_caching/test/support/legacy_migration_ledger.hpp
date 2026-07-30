#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <istream>
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
    "source_kind\tscenario_or_permutation\t"
    "expected_sqlstate_or_result\texpected_state_invariant\t"
    "catch2_case\tcatch2_section\tcatch2_tags\tmigration_status"};

enum class status {
    mapped,
    passing,
    retired_with_proof,
};

struct ledger_row {
    std::string legacy_path;
    std::string legacy_line_or_assertion;
    std::string stable_id;
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
    bool legacy_deletion_started{};
    std::size_t mapped_rows{};
    std::size_t passing_rows{};
    std::size_t retired_with_proof_rows{};
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
        if (fields.size() != 11) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " does not have 11 fields");
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
            detail::parse_status(fields[10], line_number);
        if (fields[7].empty()) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " has an empty catch2_case mapping");
        }
        if (migration_status == status::retired_with_proof
            && (fields[4].empty() || fields[6].empty())) {
            throw std::runtime_error(
                "migration ledger line " + std::to_string(line_number)
                + " retires without a replacement scenario and rationale");
        }

        result.rows.push_back(ledger_row{
            .legacy_path = std::move(fields[0]),
            .legacy_line_or_assertion = std::move(fields[1]),
            .stable_id = std::move(fields[2]),
            .source_kind = std::move(fields[3]),
            .scenario_or_permutation = std::move(fields[4]),
            .expected_sqlstate_or_result = std::move(fields[5]),
            .expected_state_invariant = std::move(fields[6]),
            .catch2_case = std::move(fields[7]),
            .catch2_section = std::move(fields[8]),
            .catch2_tags = std::move(fields[9]),
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

[[nodiscard]] inline ledger load_ledger(
    const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(
            "cannot read migration ledger: " + path.string());
    }
    try {
        return parse_ledger(input);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "invalid migration ledger " + path.string() + ": "
            + error.what());
    }
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
        || end == count_text.data()) {
        throw std::runtime_error(
            "migration ledger line " + std::to_string(row.line_number)
            + " has invalid expected_count");
    }
    return count;
}

[[nodiscard]] inline std::unordered_set<std::string>
load_catch_case_manifest(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error(
            "cannot read Catch case manifest: " + path.string());
    }
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
            "failed while reading Catch case manifest: " + path.string());
    }
    if (result.empty()) {
        throw std::runtime_error("Catch case manifest has no test cases");
    }
    return result;
}

[[nodiscard]] inline global_validation validate_global_ledger(
    const ledger& value,
    const std::unordered_set<std::string>& catch_cases,
    const std::filesystem::path& legacy_root,
    const std::vector<std::filesystem::path>& legacy_assets = {}) {
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
        if (detail::is_tracked_legacy_path(row.legacy_path)
            && !std::filesystem::exists(legacy_root / row.legacy_path)) {
            result.legacy_deletion_started = true;
        }
    }
    for (const auto& asset : legacy_assets) {
        if (!std::filesystem::exists(legacy_root / asset)) {
            result.legacy_deletion_started = true;
        }
    }
    result.deletion_ready = result.mapped_rows == 0;
    if (result.legacy_deletion_started && !result.deletion_ready) {
        throw std::runtime_error(
            "legacy test asset deletion started while "
            + std::to_string(result.mapped_rows)
            + " migration-ledger rows remain mapped; every row must be "
              "passing or retired_with_proof before deleting legacy assets");
    }
    return result;
}

}  // namespace bioseq::cache_test::migration
