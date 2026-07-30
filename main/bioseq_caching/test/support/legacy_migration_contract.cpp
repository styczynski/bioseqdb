#include "legacy_migration_ledger.hpp"

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

import bioseq.test.postgres;

namespace {

struct arguments {
    std::filesystem::path ledger;
    std::filesystem::path catch_cases;
    std::filesystem::path legacy_root;
    std::filesystem::path legacy_inventory;
    bioseq::cache_test::migration::retirement_phase phase{
        bioseq::cache_test::migration::retirement_phase::tracking};
    bool phase_seen{};
    bool current_phase{};
    bool inventory_only{};
};

[[nodiscard]] arguments parse_arguments(int argc, char* argv[]) {
    arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--inventory-only") {
            if (result.inventory_only) {
                throw std::invalid_argument(
                    "--inventory-only may be supplied only once");
            }
            result.inventory_only = true;
            continue;
        }
        if (option == "--phase") {
            if (result.phase_seen) {
                throw std::invalid_argument(
                    "--phase may be supplied only once");
            }
            if (++index == argc) {
                throw std::invalid_argument(
                    "--phase requires tracking or current");
            }
            const std::string_view value{argv[index]};
            if (value == "tracking") {
                result.phase =
                    bioseq::cache_test::migration::retirement_phase::
                        tracking;
            } else if (value == "current") {
                result.phase =
                    bioseq::cache_test::migration::retirement_phase::
                        tracking;
                result.current_phase = true;
            } else {
                throw std::invalid_argument(
                    "--phase must be tracking or current");
            }
            result.phase_seen = true;
            continue;
        }
        if (option != "--ledger" && option != "--catch-cases"
            && option != "--legacy-root"
            && option != "--legacy-inventory") {
            throw std::invalid_argument(
                "unknown migration contract option: "
                + std::string(option));
        }
        if (++index == argc) {
            throw std::invalid_argument(
                std::string(option) + " requires a path");
        }
        if (option == "--ledger") {
            result.ledger = argv[index];
        } else if (option == "--catch-cases") {
            result.catch_cases = argv[index];
        } else if (option == "--legacy-root") {
            result.legacy_root = argv[index];
        } else {
            result.legacy_inventory = argv[index];
        }
    }
    if (result.legacy_root.empty() || !result.phase_seen
        || result.legacy_inventory.empty()
        || (!result.inventory_only
            && (result.ledger.empty() || result.catch_cases.empty()))
        || (result.inventory_only
            && (!result.ledger.empty() || !result.catch_cases.empty()
                || (result.phase
                        != bioseq::cache_test::migration::
                            retirement_phase::tracking
                    && !result.current_phase)))) {
        throw std::invalid_argument(
            "inventory-only requires --legacy-root, "
            "--legacy-inventory, and --phase tracking or current; "
            "full validation also requires --ledger and --catch-cases "
            "and may use current");
    }
    return result;
}

[[nodiscard]] bool is_legacy_candidate(
    const std::filesystem::path& relative) {
    const std::string path = relative.generic_string();
    if (path == "bench/sql_cases.json"
        || path.starts_with("test/regress/")
        || path.starts_with("test/isolation/")
        || path.starts_with("test/tap/")) {
        return true;
    }
    const std::string filename = relative.filename().string();
    return filename.ends_with(".py")
        || filename.ends_with(".pyc")
        || filename.ends_with(".pl")
        || filename.ends_with(".pm")
        || filename.ends_with(".sql")
        || filename.ends_with(".sql.in");
}

void validate_inventory_bytes_and_closure(
    const arguments& options,
    const std::vector<
        bioseq::cache_test::migration::legacy_asset_record>& inventory) {
    std::error_code root_error;
    const auto root_status = std::filesystem::symlink_status(
        options.legacy_root, root_error);
    if (root_error || !std::filesystem::is_directory(root_status)
        || std::filesystem::is_symlink(root_status)) {
        throw std::runtime_error(
            "legacy inventory root must be an existing non-symlink "
            "directory: " + options.legacy_root.string());
    }
    std::set<std::string, std::less<>> expected;
    for (const auto& record : inventory) {
        if (!is_legacy_candidate(record.path)) {
            throw std::runtime_error(
                "legacy inventory contains a non-candidate path: "
                + record.path.string());
        }
        expected.emplace(record.path.generic_string());
        std::filesystem::path component_path = options.legacy_root;
        for (const auto& component : record.path) {
            component_path /= component;
            std::error_code component_error;
            const auto component_status =
                std::filesystem::symlink_status(
                    component_path, component_error);
            if (component_error) {
                throw std::runtime_error(
                    "cannot inspect legacy inventory component "
                    + component_path.string() + ": "
                    + component_error.message());
            }
            if (!std::filesystem::exists(component_status)) {
                break;
            }
            if (std::filesystem::is_symlink(component_status)) {
                throw std::runtime_error(
                    "legacy inventory path contains a symlink: "
                    + component_path.string());
            }
        }
        if (options.phase
            != bioseq::cache_test::migration::retirement_phase::retired) {
            const std::string actual =
                bioseq::postgres_test::sha256_file(
                    options.legacy_root / record.path).hex();
            if (actual != record.sha256) {
                throw std::runtime_error(
                    "legacy asset SHA-256 differs from frozen inventory: "
                    + record.path.string());
            }
        }
    }

    std::set<std::string, std::less<>> actual;
    for (const std::filesystem::path root :
         {std::filesystem::path{"bench"},
          std::filesystem::path{"sql"},
          std::filesystem::path{"test"}}) {
        const std::filesystem::path absolute =
            options.legacy_root / root;
        std::error_code error;
        const auto root_status =
            std::filesystem::symlink_status(absolute, error);
        if (error) {
            throw std::runtime_error(
                "cannot inspect legacy candidate root "
                + absolute.string() + ": " + error.message());
        }
        if (!std::filesystem::exists(root_status)) {
            continue;
        }
        if (!std::filesystem::is_directory(root_status)
            || std::filesystem::is_symlink(root_status)) {
            throw std::runtime_error(
                "legacy candidate root is not a non-symlink directory: "
                + absolute.string());
        }
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(absolute)) {
            const auto metadata = entry.symlink_status();
            if (std::filesystem::is_symlink(metadata)) {
                throw std::runtime_error(
                    "legacy candidate tree contains a symlink: "
                    + entry.path().string());
            }
            if (!std::filesystem::is_regular_file(metadata)) {
                continue;
            }
            const std::filesystem::path relative =
                entry.path().lexically_relative(options.legacy_root);
            if (is_legacy_candidate(relative)) {
                actual.emplace(relative.generic_string());
            }
        }
    }

    if (options.phase
        == bioseq::cache_test::migration::retirement_phase::retired) {
        if (!actual.empty()) {
            throw std::runtime_error(
                "retired legacy candidate set is not empty");
        }
    } else if (actual != expected) {
        throw std::runtime_error(
            "legacy candidate set differs from the exact frozen "
            "85-file inventory");
    }
}

[[nodiscard]] bioseq::cache_test::migration::retirement_phase
resolve_current_inventory_phase(
    arguments options,
    const std::vector<
        bioseq::cache_test::migration::legacy_asset_record>& inventory) {
    options.current_phase = false;
    options.phase =
        bioseq::cache_test::migration::retirement_phase::tracking;
    try {
        validate_inventory_bytes_and_closure(options, inventory);
        return options.phase;
    } catch (const std::exception& tracking_error) {
        const std::string tracking_detail = tracking_error.what();
        options.phase =
            bioseq::cache_test::migration::retirement_phase::retired;
        try {
            validate_inventory_bytes_and_closure(options, inventory);
            return options.phase;
        } catch (const std::exception& retired_error) {
            throw std::runtime_error(
                "current legacy inventory is neither the exact tracked "
                "85-file state nor the exact retired empty state; "
                "tracking validation: "
                + tracking_detail
                + "; retired validation: " + retired_error.what());
        }
    }
}

void validate_current_structural_contract(
    const bioseq::cache_test::migration::ledger& ledger,
    const std::unordered_set<std::string>& catch_cases,
    const std::vector<
        bioseq::cache_test::migration::legacy_asset_record>& inventory,
    bioseq::cache_test::migration::retirement_phase asset_phase) {
    using namespace bioseq::cache_test::migration;
    std::unordered_set<std::string> inventory_paths;
    inventory_paths.reserve(inventory.size());
    for (const auto& asset : inventory) {
        inventory_paths.insert(asset.path.generic_string());
    }

    bool has_retired_row = false;
    for (const ledger_row& row : ledger.rows) {
        if (!catch_cases.contains(row.catch2_case)) {
            throw std::runtime_error(
                "migration ledger line "
                + std::to_string(row.line_number)
                + " maps stable_id '" + row.stable_id
                + "' to unknown Catch2 case '" + row.catch2_case + "'");
        }
        const bool tracked_path =
            row.legacy_path.starts_with("test/")
            || row.legacy_path.starts_with("bench/")
            || row.legacy_path.starts_with("sql/");
        if (tracked_path
            && !inventory_paths.contains(row.legacy_path)) {
            throw std::runtime_error(
                "legacy asset inventory omits ledger path "
                + row.legacy_path);
        }
        has_retired_row = has_retired_row
            || row.migration_status == status::retired_with_proof;
    }

    if (asset_phase == retirement_phase::tracking) {
        if (has_retired_row) {
            throw std::runtime_error(
                "current structural contract cannot mark ledger rows "
                "retired while the exact legacy inventory remains");
        }
        return;
    }
    if (asset_phase != retirement_phase::retired
        || ledger_phase_name(ledger) != "retired_with_proof") {
        throw std::runtime_error(
            "current structural contract requires every ledger row to "
            "be retired_with_proof when the exact legacy inventory is "
            "empty");
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        arguments options = parse_arguments(argc, argv);
        const auto inventory =
            bioseq::cache_test::migration::
                load_legacy_asset_inventory(
                    options.legacy_inventory);
        if (options.current_phase) {
            options.phase =
                resolve_current_inventory_phase(options, inventory);
        }
        if (options.inventory_only) {
            validate_inventory_bytes_and_closure(
                options, inventory);
            std::cout
                << "bioseq_cache legacy inventory validation passed: "
                << (options.phase
                            == bioseq::cache_test::migration::
                                retirement_phase::tracking
                        ? "tracking exact 85-file inventory"
                        : "retired exact empty inventory")
                << '\n';
            return 0;
        }
        const auto ledger =
            bioseq::cache_test::migration::load_ledger(options.ledger);
        const std::unordered_set<std::string> catch_cases =
            bioseq::cache_test::migration::load_catch_case_manifest(
                options.catch_cases);
        if (options.current_phase) {
            validate_current_structural_contract(
                ledger, catch_cases, inventory, options.phase);
            if (options.phase
                == bioseq::cache_test::migration::
                    retirement_phase::retired) {
                validate_inventory_bytes_and_closure(
                    options, inventory);
                std::cout
                    << "migration ledger structural contract passed: "
                    << ledger.rows.size()
                    << " rows; phase=retired_with_proof; "
                       "inventory=exact-empty; "
                       "five-report-evidence-union=not-claimed\n";
                return 0;
            }
        }
        std::vector<std::filesystem::path> legacy_assets;
        legacy_assets.reserve(inventory.size());
        for (const auto& record : inventory) {
            legacy_assets.push_back(record.path);
        }
        const auto validation =
            bioseq::cache_test::migration::validate_global_ledger(
                ledger, catch_cases, options.legacy_root,
                legacy_assets, {}, options.phase,
                bioseq::cache_test::migration::
                    legacy_inventory_size);
        validate_inventory_bytes_and_closure(options, inventory);
        std::cout
            << "migration ledger contract passed: "
            << ledger.rows.size() << " rows; mapped="
            << validation.mapped_rows << "; passing="
            << validation.passing_rows << "; retired_with_proof="
            << validation.retired_with_proof_rows
            << "; deletion_ready="
            << (validation.deletion_ready ? "true" : "false")
            << "; proof_ready="
            << (validation.proof_ready ? "true" : "false")
            << "; retirement_complete="
            << (validation.retirement_complete ? "true" : "false")
            << '\n';
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "migration ledger contract usage error: "
                  << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "migration ledger contract failed: "
                  << error.what() << '\n';
        return 1;
    }
}
