#include "legacy_migration_ledger.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

struct arguments {
    std::filesystem::path ledger;
    std::filesystem::path catch_cases;
    std::filesystem::path legacy_root;
    std::vector<std::filesystem::path> legacy_assets;
};

[[nodiscard]] arguments parse_arguments(int argc, char* argv[]) {
    arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option == "--legacy-asset") {
            if (++index == argc) {
                throw std::invalid_argument(
                    "--legacy-asset requires a path");
            }
            result.legacy_assets.emplace_back(argv[index]);
            continue;
        }
        if (option != "--ledger" && option != "--catch-cases"
            && option != "--legacy-root") {
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
        } else {
            result.legacy_root = argv[index];
        }
    }
    if (result.ledger.empty() || result.catch_cases.empty()
        || result.legacy_root.empty()) {
        throw std::invalid_argument(
            "--ledger, --catch-cases, and --legacy-root are required");
    }
    return result;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const arguments options = parse_arguments(argc, argv);
        const auto ledger =
            bioseq::cache_test::migration::load_ledger(options.ledger);
        const std::unordered_set<std::string> catch_cases =
            bioseq::cache_test::migration::load_catch_case_manifest(
                options.catch_cases);
        const auto validation =
            bioseq::cache_test::migration::validate_global_ledger(
                ledger, catch_cases, options.legacy_root,
                options.legacy_assets);
        std::cout
            << "migration ledger contract passed: "
            << ledger.rows.size() << " rows; mapped="
            << validation.mapped_rows << "; passing="
            << validation.passing_rows << "; retired_with_proof="
            << validation.retired_with_proof_rows
            << "; deletion_ready="
            << (validation.deletion_ready ? "true" : "false") << '\n';
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
