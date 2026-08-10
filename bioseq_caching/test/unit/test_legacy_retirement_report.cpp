#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "legacy_retirement_report.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

static_assert(
    bioseq::cache_test::retirement::report_role_name(
        bioseq::cache_test::retirement::report_role::cache_integration)
    == "cache-integration");

class temporary_directory {
public:
    temporary_directory() {
        const auto nonce =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path()
            / ("bioseq-cache-retirement-contract-"
               + std::to_string(nonce));
        std::filesystem::create_directories(path_);
    }

    ~temporary_directory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    temporary_directory(const temporary_directory&) = delete;
    temporary_directory& operator=(const temporary_directory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void write(
    const std::filesystem::path& path, std::string_view contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create test fixture");
    }
    output.write(
        contents.data(), static_cast<std::streamsize>(contents.size()));
    output.close();
    if (!output) {
        throw std::runtime_error("cannot write test fixture");
    }
}

[[nodiscard]] std::string inventory_document(
    bool lexical_alias = false,
    std::size_t count = 85) {
    const std::string digest(64, 'a');
    std::ostringstream output;
    output << "legacy_path\tsha256\n";
    for (std::size_t index = 0; index < count; ++index) {
        output << (lexical_alias && index == 0 ? "./" : "")
               << "test/regress/retired_" << std::setw(3)
               << std::setfill('0') << index << ".out\t"
               << digest << '\n';
    }
    return output.str();
}

[[nodiscard]] std::string bundle_document(
    std::string_view phase,
    std::string_view transition,
    const std::filesystem::path& root) {
    const std::string revision(40, 'a');
    const std::string digest(64, 'b');
    std::ostringstream output;
    output
        << "{"
        << "\"schema_version\":\""
        << bioseq::cache_test::retirement::bundle_schema << "\","
        << "\"phase\":\"" << phase << "\","
        << "\"ledger\":\"" << (root / "ledger.tsv").string() << "\","
        << "\"catch_cases\":\"" << (root / "cases.txt").string()
        << "\","
        << "\"legacy_root\":\"" << root.string() << "\","
        << "\"legacy_inventory\":\""
        << (root / "inventory.tsv").string() << "\","
        << "\"proof_receipt\":\"" << (root / "proof.json").string()
        << "\","
        << "\"current_source_revision\":\"" << revision << "\","
        << "\"current_source_scope_sha256\":\"" << digest << "\","
        << "\"current_source_scoped_clean\":true,"
        << "\"retirement_transition_sha256\":\"" << transition << "\","
        << "\"unit_receipt\":\"" << (root / "unit.json").string()
        << "\","
        << "\"evidence_receipts\":[";
    for (std::size_t index = 0; index < 5; ++index) {
        if (index != 0) {
            output << ',';
        }
        output << '"' << (root / ("receipt_"
            + std::to_string(index) + ".json")).string() << '"';
    }
    output
        << "],"
        << "\"proof_unit_receipt\":"
        << (phase == "retired-final"
                ? "\"" + (root / "proof-unit.json").string() + "\""
                : "\"\"")
        << ",\"proof_evidence_receipts\":[";
    if (phase == "retired-final") {
        for (std::size_t index = 0; index < 5; ++index) {
            if (index != 0) {
                output << ',';
            }
            output << '"' << (root / ("proof-receipt_"
                + std::to_string(index) + ".json")).string() << '"';
        }
    }
    output
        << "],\"transition_receipt\":"
        << (phase == "retired-final"
                ? "\"" + (root / "transition.json").string() + "\""
                : "\"\"")
        << "}";
    return output.str();
}

[[nodiscard]] std::string unit_receipt_document(
    const std::filesystem::path& root,
    std::size_t command_count = 5) {
    namespace retirement = bioseq::cache_test::retirement;
    const std::string revision(40, 'a');
    const std::string source_digest(64, 'b');
    const std::string ledger_digest(64, 'c');
    const std::string evidence_digest(64, 'd');
    const std::string executable_digest(64, 'e');
    const std::array<std::string_view, 5> commands{
        "bioseq_cache_unit_tests",
        "bioseq_postgres_test_support_contract_tests",
        "bioseq_cache_benchmark_report_contract_tests",
        "bioseq_durable_file_io_contract_tests",
        "bioseq_cache_legacy_inventory_tracking",
    };
    std::ostringstream output;
    output
        << "{"
        << "\"schema_version\":\""
        << retirement::unit_receipt_schema << "\","
        << "\"mode\":\"cache-unit\","
        << "\"status\":\"passed\","
        << "\"source_revision\":\"" << revision << "\","
        << "\"source_scope_sha256\":\"" << source_digest << "\","
        << "\"source_scoped_clean\":true,"
        << "\"migration_ledger_sha256\":\"" << ledger_digest << "\","
        << "\"evidence_contract_sha256\":\"" << evidence_digest
        << "\","
        << "\"migration_phase\":\"passing\","
        << "\"commands\":[";
    for (std::size_t index = 0; index < command_count; ++index) {
        if (index != 0U) {
            output << ',';
        }
        output
            << "{\"executable\":\""
            << retirement::detail::json_escape(
                   (root / commands[index]).string())
            << "\",\"executable_sha256\":\"" << executable_digest
            << "\",\"arguments\":[";
        if (index + 1U == commands.size()) {
            output
                << "\"--legacy-root\",\""
                << retirement::detail::json_escape(root.string())
                << "\",\"--legacy-inventory\",\""
                << retirement::detail::json_escape(
                       (root / "inventory.tsv").string())
                << "\",\"--phase\",\"current\",\"--inventory-only\"";
        } else {
            output
                << "\"--reporter\","
                << "\"console::out=-::colour-mode=ansi\"";
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}

[[nodiscard]] std::string repeated_hex(
    char value, std::size_t count = 64) {
    return std::string(count, value);
}

[[nodiscard]] std::string different_sha256(std::string_view value) {
    const bool all_zero =
        value.size() == 64
        && std::ranges::all_of(
            value, [](char character) { return character == '0'; });
    return repeated_hex(all_zero ? 'f' : '0');
}

[[nodiscard]] std::string replace_once(
    std::string document,
    std::string_view before,
    std::string_view after) {
    const std::size_t position = document.find(before);
    if (position == std::string::npos) {
        throw std::runtime_error("test fixture marker is missing");
    }
    document.replace(position, before.size(), after);
    return document;
}

[[nodiscard]] std::string evidence_receipt_document(
    const std::filesystem::path& root,
    bioseq::cache_test::retirement::report_role role) {
    namespace retirement = bioseq::cache_test::retirement;
    const std::filesystem::path executable =
        root / retirement::role_executable_name(role);
    const std::filesystem::path report =
        root
        / (std::string(retirement::report_role_name(role)) + ".json");
    std::ostringstream output;
    output
        << "{"
        << "\"schema_version\":\"" << retirement::receipt_schema << "\","
        << "\"mode\":\"" << retirement::role_mode(role) << "\","
        << "\"report_role\":\""
        << retirement::report_role_name(role) << "\","
        << "\"native_report\":\""
        << retirement::detail::json_escape(report.string()) << "\","
        << "\"native_report_sha256\":\"" << repeated_hex('b') << "\","
        << "\"executable\":\""
        << retirement::detail::json_escape(executable.string()) << "\","
        << "\"executable_sha256\":\"" << repeated_hex('c') << "\","
        << "\"scenario\":\"" << retirement::role_scenario(role) << "\","
        << "\"source_revision\":\"" << repeated_hex('1', 40) << "\","
        << "\"source_scope_sha256\":\"" << repeated_hex('2') << "\","
        << "\"source_scoped_clean\":true,"
        << "\"migration_ledger_sha256\":\"" << repeated_hex('3') << "\","
        << "\"evidence_contract_sha256\":\"" << repeated_hex('4') << "\","
        << "\"migration_phase\":\"passing\""
        << "}";
    return output.str();
}

[[nodiscard]] bioseq::cache_test::retirement::source_provenance
proof_source() {
    return {
        .revision = repeated_hex('1', 40),
        .scope_sha256 = repeated_hex('2'),
        .scoped_clean = true,
    };
}

[[nodiscard]] bioseq::cache_test::retirement::source_provenance
retired_source() {
    return {
        .revision = repeated_hex('8', 40),
        .scope_sha256 = repeated_hex('9'),
        .scoped_clean = true,
    };
}

[[nodiscard]] bioseq::cache_test::retirement::evidence_receipt
historical_receipt(
    const std::filesystem::path& root,
    bioseq::cache_test::retirement::report_role role,
    std::size_t index) {
    namespace retirement = bioseq::cache_test::retirement;
    constexpr std::array<char, 5> receipt_hashes{'b', 'c', 'd', 'e', 'f'};
    constexpr std::array<char, 5> report_hashes{'0', '1', '2', '3', '4'};
    retirement::evidence_receipt result;
    result.receipt_path =
        root / ("historical-receipt-" + std::to_string(index) + ".json");
    result.receipt_sha256 = repeated_hex(receipt_hashes.at(index));
    result.role = role;
    result.mode = std::string(retirement::role_mode(role));
    result.native_report =
        root / ("historical-native-" + std::to_string(index) + ".json");
    result.native_report_sha256 =
        repeated_hex(report_hashes.at(index));
    result.executable = root / retirement::role_executable_name(role);
    result.executable_sha256 = repeated_hex('5');
    result.scenario = std::string(retirement::role_scenario(role));
    result.source = proof_source();
    result.migration_ledger_sha256 = repeated_hex('3');
    result.evidence_contract_sha256 = repeated_hex('4');
    result.migration_phase = "passing";
    return result;
}

[[nodiscard]] bioseq::cache_test::retirement::native_report_evidence
historical_native(
    const std::filesystem::path& root,
    bioseq::cache_test::retirement::report_role role,
    std::size_t index) {
    namespace retirement = bioseq::cache_test::retirement;
    const retirement::evidence_receipt receipt =
        historical_receipt(root, role, index);
    retirement::native_report_evidence result;
    result.role = role;
    result.owner = retirement::role_owner(role);
    result.migration_ledger_sha256 = repeated_hex('3');
    result.evidence_contract_sha256 = repeated_hex('4');
    result.migration_phase = "passing";
    result.executable = receipt.executable;
    result.executable_sha256 = receipt.executable_sha256;
    result.scenario = receipt.scenario;
    return result;
}

[[nodiscard]] bioseq::cache_test::retirement::retirement_proof
historical_proof(const std::filesystem::path& root) {
    namespace retirement = bioseq::cache_test::retirement;
    retirement::retirement_proof proof{
        .proof_source_revision = proof_source().revision,
        .proof_source_scope_sha256 = proof_source().scope_sha256,
        .migration_ledger_sha256 = repeated_hex('3'),
        .evidence_contract_sha256 = repeated_hex('4'),
        .migration_phase = "passing",
        .legacy_inventory_sha256 = repeated_hex('5'),
        .legacy_asset_count = 85,
        .unit_receipt_sha256 = repeated_hex('6'),
    };
    for (std::size_t index = 0;
         index < retirement::required_report_roles.size(); ++index) {
        const retirement::evidence_receipt receipt =
            historical_receipt(
                root, retirement::required_report_roles[index], index);
        proof.reports.push_back(retirement::proof_report{
            .role = receipt.role,
            .receipt_sha256 = receipt.receipt_sha256,
            .native_report_sha256 = receipt.native_report_sha256,
        });
    }
    return proof;
}

[[nodiscard]] bioseq::cache_test::retirement::gate_bundle
retired_bundle(const std::filesystem::path& root) {
    namespace migration = bioseq::cache_test::migration;
    namespace retirement = bioseq::cache_test::retirement;
    retirement::gate_bundle bundle;
    bundle.operation = retirement::gate_operation::retired_final;
    bundle.phase = migration::retirement_phase::retired;
    bundle.ledger = root / "ledger.tsv";
    bundle.catch_cases = root / "cases.txt";
    bundle.legacy_root = root;
    bundle.legacy_inventory = root / "inventory.tsv";
    bundle.proof_receipt = root / "proof.json";
    bundle.current_source = retired_source();
    bundle.retirement_transition_sha256 = repeated_hex('a');
    bundle.unit_receipt = root / "current-unit.json";
    bundle.proof_unit_receipt = root / "proof-unit.json";
    bundle.transition_receipt = root / "transition.json";
    return bundle;
}

[[nodiscard]] bioseq::cache_test::retirement::unit_receipt
historical_unit(const std::filesystem::path& root) {
    namespace retirement = bioseq::cache_test::retirement;
    retirement::unit_receipt result;
    result.receipt_path = root / "proof-unit.json";
    result.receipt_sha256 = repeated_hex('6');
    result.source = proof_source();
    result.migration_ledger_sha256 = repeated_hex('3');
    result.evidence_contract_sha256 = repeated_hex('4');
    result.migration_phase = "passing";
    return result;
}

[[nodiscard]] std::string benchmark_report_document(
    const std::filesystem::path& root,
    bioseq::cache_test::retirement::report_role role) {
    namespace retirement = bioseq::cache_test::retirement;
    const std::uint64_t samples =
        role == retirement::report_role::cache_bench ? 7U : 3U;
    const std::size_t artifact_count =
        role == retirement::report_role::cache_bench ? 1U : 2U;
    std::ostringstream artifacts;
    for (std::size_t index = 0; index < artifact_count; ++index) {
        if (index != 0U) {
            artifacts << ',';
        }
        const std::string_view name =
            index == 0U ? "bioseq_cache" : "bioseq";
        const std::filesystem::path stage =
            root / ("stage-" + std::to_string(index));
        const std::filesystem::path source =
            root / ("source-" + std::to_string(index));
        artifacts
            << "{"
            << "\"name\":\"" << name << "\","
            << "\"preinstalled_stage_root\":\""
            << retirement::detail::json_escape(stage.string()) << "\","
            << "\"stage_manifest\":\""
            << retirement::detail::json_escape(
                   (stage / "stage-manifest.json").string())
            << "\","
            << "\"stage_manifest_sha256\":\"" << repeated_hex('a')
            << "\","
            << "\"control_file\":\""
            << retirement::detail::json_escape(
                   (source / (std::string(name) + ".control")).string())
            << "\","
            << "\"install_sql\":\""
            << retirement::detail::json_escape(
                   (source / (std::string(name) + "--1.0.sql")).string())
            << "\","
            << "\"dso\":\""
            << retirement::detail::json_escape(
                   (source / (std::string(name) + ".so")).string())
            << "\","
            << "\"staged_control_file\":\""
            << retirement::detail::json_escape(
                   (stage / (std::string(name) + ".control")).string())
            << "\","
            << "\"staged_install_sql\":\""
            << retirement::detail::json_escape(
                   (stage / (std::string(name) + "--1.0.sql")).string())
            << "\","
            << "\"staged_dso\":\""
            << retirement::detail::json_escape(
                   (stage / (std::string(name) + ".so")).string())
            << "\","
            << "\"control_sha256\":\"" << repeated_hex('b') << "\","
            << "\"install_sql_sha256\":\"" << repeated_hex('c') << "\","
            << "\"dso_sha256\":\"" << repeated_hex('d') << "\","
            << "\"staged_control_sha256\":\"" << repeated_hex('b')
            << "\"}";
    }
    std::ostringstream output;
    output
        << "{"
        << "\"schema_version\":\"" << retirement::benchmark_schema
        << "\","
        << "\"scenario\":\"" << retirement::role_scenario(role) << "\","
        << "\"metadata\":{"
        << "\"status\":\"passed\","
        << "\"error\":\"\","
        << "\"configured_samples\":" << samples << ","
        << "\"configured_warmup_samples\":1,"
        << "\"build_identity\":{"
        << "\"executable_path\":\""
        << retirement::detail::json_escape(
               (root / retirement::role_executable_name(role)).string())
        << "\","
        << "\"executable_sha256\":\"" << repeated_hex('e') << "\"},"
        << "\"artifacts\":[" << artifacts.str() << "]},"
        << "\"cleanup\":{"
        << "\"attempted\":true,"
        << "\"succeeded\":true,"
        << "\"private_root_removed\":true},"
        << "\"migration_evidence\":{"
        << "\"migration_report_role\":\""
        << retirement::report_role_name(role) << "\","
        << "\"migration_evidence_harness\":\""
        << bioseq::cache_test::migration::harness_name(
               retirement::role_owner(role))
        << "\","
        << "\"coverage\":\"full-role\","
        << "\"ledger_sha256\":\"" << repeated_hex('f') << "\","
        << "\"evidence_contract_sha256\":\"" << repeated_hex('0')
        << "\","
        << "\"migration_phase\":\"passing\","
        << "\"probes\":[]}"
        << "}";
    return output.str();
}

[[nodiscard]] std::string integration_artifact_document(
    std::string_view name,
    const std::filesystem::path& stage_root,
    const std::filesystem::path& stage_manifest,
    const std::filesystem::path& control,
    const std::filesystem::path& install_sql,
    const std::filesystem::path& dso,
    const std::filesystem::path& staged_control,
    const std::filesystem::path& staged_install_sql,
    const std::filesystem::path& staged_dso,
    char manifest_digest,
    char control_digest,
    char install_digest,
    char dso_digest,
    char staged_control_digest) {
    namespace retirement = bioseq::cache_test::retirement;
    std::ostringstream output;
    output
        << "{"
        << "\"name\":\"" << name << "\","
        << "\"preinstalled_stage_root\":\""
        << retirement::detail::json_escape(stage_root.string())
        << "\","
        << "\"stage_manifest\":\""
        << retirement::detail::json_escape(stage_manifest.string())
        << "\","
        << "\"stage_manifest_sha256\":\""
        << repeated_hex(manifest_digest) << "\","
        << "\"control_file\":\""
        << retirement::detail::json_escape(control.string()) << "\","
        << "\"install_sql\":\""
        << retirement::detail::json_escape(install_sql.string())
        << "\","
        << "\"dso\":\""
        << retirement::detail::json_escape(dso.string()) << "\","
        << "\"staged_control_file\":\""
        << retirement::detail::json_escape(staged_control.string())
        << "\","
        << "\"staged_install_sql\":\""
        << retirement::detail::json_escape(
               staged_install_sql.string())
        << "\","
        << "\"staged_dso\":\""
        << retirement::detail::json_escape(staged_dso.string())
        << "\","
        << "\"control_sha256\":\""
        << repeated_hex(control_digest) << "\","
        << "\"install_sql_sha256\":\""
        << repeated_hex(install_digest) << "\","
        << "\"dso_sha256\":\"" << repeated_hex(dso_digest) << "\","
        << "\"staged_control_sha256\":\""
        << repeated_hex(staged_control_digest) << "\""
        << "}";
    return output.str();
}

[[nodiscard]] std::filesystem::path support_cleanup_root(
    const std::filesystem::path& fixture_root,
    std::size_t index) {
    return std::filesystem::canonical(
               std::filesystem::temp_directory_path())
        / ("bioseq-pg-test-" + fixture_root.filename().string() + "-"
           + std::to_string(index));
}

[[nodiscard]] std::string support_integration_report_document(
    const std::filesystem::path& root,
    std::size_t provider_rows = 2U,
    bool duplicate_provider_staged_dso = false,
    bool omit_last_cleanup_root = false) {
    namespace migration = bioseq::cache_test::migration;
    namespace retirement = bioseq::cache_test::retirement;
    const auto role =
        retirement::report_role::
            cache_bioseq_support_integration;
    const std::filesystem::path executable =
        root / retirement::role_executable_name(role);
    const std::filesystem::path primary_stage =
        root / "support-stage";
    const std::filesystem::path primary_manifest =
        primary_stage / "stage-manifest.json";
    const std::filesystem::path bioseq_only_stage =
        root / "bioseq-only-stage";
    const std::filesystem::path bioseq_only_manifest =
        bioseq_only_stage / "stage-manifest.json";
    const std::filesystem::path cache_control =
        root / "source/bioseq_cache.control";
    const std::filesystem::path cache_sql =
        root / "source/bioseq_cache--1.0.sql";
    const std::filesystem::path cache_dso =
        root / "source/bioseq_cache.so";
    const std::filesystem::path provider_dso =
        root / "source/bioseq_cache_provider_peer_test.so";
    const std::filesystem::path bioseq_control =
        root / "source/bioseq.control";
    const std::filesystem::path bioseq_sql =
        root / "source/bioseq--1.0.sql";
    const std::filesystem::path bioseq_dso =
        root / "source/bioseq.so";
    const std::filesystem::path cache_staged_control =
        primary_stage / "share/extension/bioseq_cache.control";
    const std::filesystem::path cache_staged_sql =
        primary_stage / "share/extension/bioseq_cache--1.0.sql";
    const std::filesystem::path cache_staged_dso =
        primary_stage / "lib/bioseq_cache.so";
    const std::filesystem::path bioseq_staged_control =
        primary_stage / "share/extension/bioseq.control";
    const std::filesystem::path bioseq_staged_sql =
        primary_stage / "share/extension/bioseq--1.0.sql";
    const std::filesystem::path bioseq_staged_dso =
        primary_stage / "lib/bioseq.so";
    const std::filesystem::path only_staged_control =
        bioseq_only_stage / "share/extension/bioseq.control";
    const std::filesystem::path only_staged_sql =
        bioseq_only_stage / "share/extension/bioseq--1.0.sql";
    const std::filesystem::path only_staged_dso =
        bioseq_only_stage / "lib/bioseq.so";

    std::vector<std::filesystem::path> cleanup_roots;
    cleanup_roots.reserve(provider_rows);
    for (std::size_t index = 0; index < provider_rows; ++index) {
        cleanup_roots.push_back(support_cleanup_root(root, index));
    }

    std::vector<std::string> artifacts{
        integration_artifact_document(
            "bioseq_cache", primary_stage, primary_manifest,
            cache_control, cache_sql, cache_dso,
            cache_staged_control, cache_staged_sql,
            cache_staged_dso, 'a', 'b', 'c', 'd', 'e'),
        integration_artifact_document(
            "bioseq", primary_stage, primary_manifest,
            bioseq_control, bioseq_sql, bioseq_dso,
            bioseq_staged_control, bioseq_staged_sql,
            bioseq_staged_dso, 'a', '1', '2', '3', '4'),
        integration_artifact_document(
            "bioseq", bioseq_only_stage, bioseq_only_manifest,
            bioseq_control, bioseq_sql, bioseq_dso,
            only_staged_control, only_staged_sql,
            only_staged_dso, '5', '1', '2', '3', '4'),
    };
    for (std::size_t index = 0; index < provider_rows; ++index) {
        const std::size_t root_index =
            duplicate_provider_staged_dso ? 0U : index;
        artifacts.push_back(integration_artifact_document(
            "bioseq_cache", primary_stage, primary_manifest,
            cache_control, cache_sql, provider_dso,
            cache_staged_control, cache_staged_sql,
            cleanup_roots[root_index]
                / "runtime-library/bioseq_cache.so",
            'a', 'b', 'c', 'f', 'e'));
    }

    const std::vector<std::pair<std::string_view,
                                std::filesystem::path>> path_options{
        {"--postgres-bin-dir", root / "postgres/bin"},
        {"--postgres-stage-root", primary_stage},
        {"--postgres-stage-manifest", primary_manifest},
        {"--bioseq-only-postgres-bin-dir",
         root / "bioseq-only-postgres/bin"},
        {"--bioseq-only-postgres-stage-root",
         bioseq_only_stage},
        {"--bioseq-only-postgres-stage-manifest",
         bioseq_only_manifest},
        {"--cache-control", cache_control},
        {"--cache-install-sql", cache_sql},
        {"--cache-dso", cache_dso},
        {"--bioseq-control", bioseq_control},
        {"--bioseq-install-sql", bioseq_sql},
        {"--bioseq-dso", bioseq_dso},
        {"--provider-peer-test-dso", provider_dso},
        {"--migration-ledger", root / "ledger.tsv"},
        {"--report", root / "report.json"},
    };
    std::vector<std::pair<std::string_view, std::string>>
        text_options{
            {"--migration-harness",
             std::string(migration::harness_name(
                 retirement::role_owner(role)))},
            {"--migration-report-role",
             std::string(retirement::report_role_name(role))},
            {"--case-timeout-seconds", "480"},
            {"--reporter", "console::out=-::colour-mode=ansi"},
        };
    std::vector<std::string> arguments{executable.string()};
    for (const auto& [option, value] : path_options) {
        arguments.emplace_back(option);
        arguments.push_back(value.string());
    }
    for (const auto& [option, value] : text_options) {
        arguments.emplace_back(option);
        arguments.push_back(value);
    }

    std::ostringstream output;
    output
        << "{"
        << "\"schema_version\":\"" << retirement::integration_schema
        << "\","
        << "\"executable\":\""
        << retirement::detail::json_escape(executable.string())
        << "\","
        << "\"build_identity\":\"" << repeated_hex('9') << "\","
        << "\"arguments\":[";
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output
            << "{\"name\":\"argv[" << index << "]\","
            << "\"value\":\""
            << retirement::detail::json_escape(arguments[index])
            << "\",\"redacted\":false}";
    }
    output
        << "],"
        << "\"cases\":[{\"status\":\"passed\"}],"
        << "\"cleanup\":{\"succeeded\":true},"
        << "\"cleanup_observations\":[";
    const std::size_t cleanup_count =
        cleanup_roots.size()
        - (omit_last_cleanup_root && !cleanup_roots.empty() ? 1U : 0U);
    for (std::size_t index = 0; index < cleanup_count; ++index) {
        if (index != 0U) {
            output << ',';
        }
        output
            << "{\"root\":\""
            << retirement::detail::json_escape(
                   cleanup_roots[index].string())
            << "\",\"succeeded\":true}";
    }
    output
        << "],"
        << "\"cluster_configuration\":{"
        << "\"retained-supervision-state\":\"none\"},"
        << "\"migration_evidence_harness\":\""
        << migration::harness_name(retirement::role_owner(role))
        << "\","
        << "\"migration_report_role\":\""
        << retirement::report_role_name(role) << "\","
        << "\"migration_coverage\":\"full-role\","
        << "\"migration_phase\":\"passing\","
        << "\"migration_ledger_sha256\":\"" << repeated_hex('7')
        << "\","
        << "\"evidence_contract_sha256\":\"" << repeated_hex('8')
        << "\","
        << "\"migration_probes\":[],"
        << "\"artifacts\":[";
    for (std::size_t index = 0; index < artifacts.size(); ++index) {
        if (index != 0U) {
            output << ',';
        }
        output << artifacts[index];
    }
    output << "]}";
    return output.str();
}

[[nodiscard]] std::string replace_last_once(
    std::string document,
    std::string_view before,
    std::string_view after) {
    const std::size_t position = document.rfind(before);
    if (position == std::string::npos) {
        throw std::runtime_error("test fixture tail marker is missing");
    }
    document.replace(position, before.size(), after);
    return document;
}

}  // namespace

TEST_CASE(
    "legacy retirement inventory rejects aliases and closes the candidate set",
    "[cache][unit][retirement]") {
    namespace migration = bioseq::cache_test::migration;
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto root = temporary.path();
    const auto inventory_path = root / "inventory.tsv";
    write(inventory_path, inventory_document());
    const retirement::legacy_inventory inventory =
        retirement::load_legacy_inventory(inventory_path);
    REQUIRE(inventory.assets.size() == 85);
    for (const retirement::legacy_asset& asset : inventory.assets) {
        write(root / asset.path, "legacy");
    }
    const auto fake_sha256 =
        [](const std::filesystem::path&) {
            return std::string(64, 'a');
        };
    CHECK_NOTHROW(retirement::validate_legacy_inventory(
        inventory, root, migration::retirement_phase::proof_ready,
        fake_sha256));

    std::filesystem::remove(root / inventory.assets.front().path);
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        retirement::validate_legacy_inventory(
            inventory, root, migration::retirement_phase::proof_ready,
            fake_sha256),
        ContainsSubstring("partial"));
    write(root / inventory.assets.front().path, "legacy");

    write(root / "test/regress/unlisted.out", "unexpected");
    CHECK_THROWS_WITH(
        retirement::validate_legacy_inventory(
            inventory, root, migration::retirement_phase::proof_ready,
            fake_sha256),
        ContainsSubstring("differ from the exact 85-asset inventory"));
    std::filesystem::remove(root / "test/regress/unlisted.out");
    for (const retirement::legacy_asset& asset : inventory.assets) {
        std::filesystem::remove(root / asset.path);
    }
    CHECK_NOTHROW(retirement::validate_legacy_inventory(
        inventory, root, migration::retirement_phase::retired,
        fake_sha256));
    write(root / "modern/unexpected.py", "forbidden");
    CHECK_THROWS_WITH(
        retirement::validate_legacy_inventory(
            inventory, root, migration::retirement_phase::retired,
            fake_sha256),
        ContainsSubstring("newly discovered"));
    std::filesystem::remove(root / "modern/unexpected.py");

    write(inventory_path, inventory_document(true));
    CHECK_THROWS_WITH(
        retirement::load_legacy_inventory(inventory_path),
        ContainsSubstring("not a normalized module-relative path"));
    write(inventory_path, inventory_document(false, 84));
    CHECK_THROWS_WITH(
        retirement::load_legacy_inventory(inventory_path),
        ContainsSubstring("exactly 85"));
    write(inventory_path, inventory_document(false, 86));
    CHECK_THROWS_WITH(
        retirement::load_legacy_inventory(inventory_path),
        ContainsSubstring("exactly 85"));
}

TEST_CASE(
    "legacy retirement rejects symlinked roots and parent components",
    "[cache][unit][retirement]") {
    namespace migration = bioseq::cache_test::migration;
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto real_root = temporary.path() / "real";
    const auto linked_root = temporary.path() / "linked";
    std::filesystem::create_directories(real_root);
    const auto inventory_path = temporary.path() / "inventory.tsv";
    write(inventory_path, inventory_document());
    const auto inventory =
        retirement::load_legacy_inventory(inventory_path);
    const auto fake_sha256 =
        [](const std::filesystem::path&) {
            return std::string(64, 'a');
        };

    std::error_code link_error;
    std::filesystem::create_directory_symlink(
        real_root, linked_root, link_error);
    if (link_error) {
        SKIP("directory symlinks are unavailable on this host");
    }
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        retirement::validate_legacy_inventory(
            inventory, linked_root,
            migration::retirement_phase::proof_ready, fake_sha256),
        ContainsSubstring("non-symlink directory"));

    std::filesystem::remove(linked_root);
    const auto external_test = temporary.path() / "external-test";
    std::filesystem::create_directories(external_test / "regress");
    link_error.clear();
    std::filesystem::create_directory_symlink(
        external_test, real_root / "test", link_error);
    REQUIRE_FALSE(link_error);
    CHECK_THROWS_WITH(
        retirement::validate_legacy_inventory(
            inventory, real_root,
            migration::retirement_phase::proof_ready, fake_sha256),
        ContainsSubstring("symlink component"));
}

TEST_CASE(
    "retirement gate bundle requires clean provenance and exact rosters",
    "[cache][unit][retirement]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto proof_bundle = temporary.path() / "proof-bundle.json";
    write(
        proof_bundle,
        bundle_document("proof-ready", "", temporary.path()));
    const retirement::gate_bundle parsed =
        retirement::load_gate_bundle(proof_bundle);
    CHECK(
        parsed.phase
        == bioseq::cache_test::migration::retirement_phase::proof_ready);
    CHECK(parsed.evidence_receipts.size() == 5);

    const auto retired_bundle = temporary.path() / "retired-bundle.json";
    write(
        retired_bundle,
        bundle_document(
            "retired-final", std::string(64, 'c'), temporary.path()));
    CHECK_NOTHROW(retirement::load_gate_bundle(retired_bundle));

    std::string dirty =
        bundle_document("proof-ready", "", temporary.path());
    const std::string clean_marker{
        "\"current_source_scoped_clean\":true"};
    dirty.replace(
        dirty.find(clean_marker), clean_marker.size(),
        "\"current_source_scoped_clean\":false");
    write(proof_bundle, dirty);
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        retirement::load_gate_bundle(proof_bundle),
        ContainsSubstring("dirty source scope"));
}

TEST_CASE(
    "retirement JSON contracts reject duplicate security fields",
    "[cache][unit][retirement]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    using Catch::Matchers::ContainsSubstring;

    std::string bundle =
        bundle_document("proof-ready", "", temporary.path());
    bundle.insert(1, "\"phase\":\"proof-ready\",");
    CHECK_THROWS_WITH(
        retirement::load_gate_bundle(
            temporary.path() / "bundle.json", bundle),
        ContainsSubstring("duplicate JSON field 'phase'"));

    std::string unit = unit_receipt_document(temporary.path());
    unit.insert(
        1,
        "\"source_revision\":\"" + repeated_hex('1', 40) + "\",");
    CHECK_THROWS_WITH(
        retirement::load_unit_receipt(
            temporary.path() / "unit.json", repeated_hex('a'), unit),
        ContainsSubstring("duplicate JSON field 'source_revision'"));

    std::string receipt = evidence_receipt_document(
        temporary.path(), retirement::report_role::cache_bench);
    receipt.insert(1, "\"scenario\":\"standalone\",");
    CHECK_THROWS_WITH(
        retirement::load_evidence_receipt(
            temporary.path() / "receipt.json",
            repeated_hex('b'), receipt),
        ContainsSubstring("duplicate JSON field 'scenario'"));

    const retirement::retirement_proof proof =
        historical_proof(temporary.path());
    const retirement::gate_bundle retired =
        retired_bundle(temporary.path());
    std::string transition = retirement::render_retirement_transition(
        repeated_hex('7'), proof, retired, repeated_hex('5'));
    transition.insert(
        transition.find('{') + 1,
        "\n  \"retired_source_revision\": \""
            + retired.current_source.revision + "\",");
    CHECK_THROWS_WITH(
        retirement::load_retirement_transition(
            temporary.path() / "transition.json", transition),
        ContainsSubstring(
            "duplicate JSON field 'retired_source_revision'"));
}

TEST_CASE(
    "native evidence is bound to its fixed executable path hash and scenario",
    "[cache][unit][retirement]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto root = temporary.path();
    const retirement::report_role role =
        retirement::report_role::cache_bench;
    const std::filesystem::path executable =
        root / retirement::role_executable_name(role);
    const std::string executable_json =
        retirement::detail::json_escape(executable.string());
    const std::string document =
        evidence_receipt_document(root, role);
    CHECK_NOTHROW(retirement::load_evidence_receipt(
        root / "receipt.json", repeated_hex('a'), document));

    using Catch::Matchers::ContainsSubstring;
    const std::string relative = replace_once(
        document,
        "\"executable\":\"" + executable_json + "\"",
        "\"executable\":\"bioseq_cache_bench\"");
    CHECK_THROWS_WITH(
        retirement::load_evidence_receipt(
            root / "receipt.json", repeated_hex('a'), relative),
        ContainsSubstring(
            "executable/scenario is incompatible with its fixed "
            "report role"));

    const std::filesystem::path wrong_basename =
        root / "bioseq_cache_bioseq_bench";
    const std::string wrong_name = replace_once(
        document,
        "\"executable\":\"" + executable_json + "\"",
        "\"executable\":\""
            + retirement::detail::json_escape(wrong_basename.string())
            + "\"");
    CHECK_THROWS_WITH(
        retirement::load_evidence_receipt(
            root / "receipt.json", repeated_hex('a'), wrong_name),
        ContainsSubstring(
            "executable/scenario is incompatible with its fixed "
            "report role"));

    const std::string wrong_scenario = replace_once(
        document,
        "\"scenario\":\"standalone\"",
        "\"scenario\":\"bioseq-2gbp\"");
    CHECK_THROWS_WITH(
        retirement::load_evidence_receipt(
            root / "receipt.json", repeated_hex('a'), wrong_scenario),
        ContainsSubstring(
            "executable/scenario is incompatible with its fixed "
            "report role"));

    const std::string invalid_hash = replace_once(
        document,
        "\"executable_sha256\":\"" + repeated_hex('c') + "\"",
        "\"executable_sha256\":\"" + repeated_hex('g') + "\"");
    CHECK_THROWS_WITH(
        retirement::load_evidence_receipt(
            root / "receipt.json", repeated_hex('a'), invalid_hash),
        ContainsSubstring("receipt executable_sha256"));

    retirement::gate_bundle bundle = retired_bundle(root);
    retirement::evidence_receipt receipt =
        historical_receipt(root, role, 3);
    retirement::native_report_evidence native =
        historical_native(root, role, 3);
    receipt.source = bundle.current_source;
    receipt.migration_ledger_sha256 = repeated_hex('7');
    receipt.migration_phase = "retired_with_proof";
    native.migration_ledger_sha256 = repeated_hex('7');
    native.migration_phase = "retired_with_proof";
    CHECK_NOTHROW(retirement::validate_receipt_and_native_report(
        receipt, native, bundle, repeated_hex('7'), repeated_hex('4'),
        receipt.native_report_sha256, receipt.executable_sha256));

    retirement::native_report_evidence altered_native = native;
    altered_native.executable =
        root / "substitute" / retirement::role_executable_name(role);
    CHECK_THROWS_WITH(
        retirement::validate_receipt_and_native_report(
            receipt, altered_native, bundle, repeated_hex('7'),
            repeated_hex('4'), receipt.native_report_sha256,
            receipt.executable_sha256),
        ContainsSubstring("executable identity is stale"));

    altered_native = native;
    altered_native.executable = root / "bioseq_cache_bioseq_bench";
    CHECK_THROWS_WITH(
        retirement::validate_receipt_and_native_report(
            receipt, altered_native, bundle, repeated_hex('7'),
            repeated_hex('4'), receipt.native_report_sha256,
            receipt.executable_sha256),
        ContainsSubstring("executable identity is stale"));

    altered_native = native;
    altered_native.executable_sha256 =
        different_sha256(native.executable_sha256);
    CHECK_THROWS_WITH(
        retirement::validate_receipt_and_native_report(
            receipt, altered_native, bundle, repeated_hex('7'),
            repeated_hex('4'), receipt.native_report_sha256,
            receipt.executable_sha256),
        ContainsSubstring("executable identity is stale"));

    altered_native = native;
    altered_native.scenario = "bioseq-2gbp";
    CHECK_THROWS_WITH(
        retirement::validate_receipt_and_native_report(
            receipt, altered_native, bundle, repeated_hex('7'),
            repeated_hex('4'), receipt.native_report_sha256,
            receipt.executable_sha256),
        ContainsSubstring("executable identity is stale"));

    CHECK_THROWS_WITH(
        retirement::validate_receipt_and_native_report(
            receipt, native, bundle, repeated_hex('7'),
            repeated_hex('4'),
            different_sha256(receipt.native_report_sha256),
            receipt.executable_sha256),
        ContainsSubstring("does not hash-bind its native report"));
    CHECK_THROWS_WITH(
        retirement::validate_receipt_and_native_report(
            receipt, native, bundle, repeated_hex('7'),
            repeated_hex('4'), receipt.native_report_sha256,
            different_sha256(receipt.executable_sha256)),
        ContainsSubstring("executable identity is stale"));
}

TEST_CASE(
    "retirement proof seals cache-unit and every native report role",
    "[cache][unit][retirement]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const retirement::source_provenance source{
        .revision = std::string(40, 'd'),
        .scope_sha256 = std::string(64, 'e'),
        .scoped_clean = true};
    retirement::unit_receipt unit{
        .receipt_sha256 = std::string(64, 'f')};
    std::vector<retirement::evidence_receipt> receipts;
    for (const retirement::report_role role :
         retirement::required_report_roles) {
        receipts.push_back(retirement::evidence_receipt{
            .receipt_sha256 = std::string(64, '1'),
            .role = role,
            .native_report_sha256 = std::string(64, '2')});
    }
    const std::string proof = retirement::render_retirement_proof(
        source, std::string(64, '3'), std::string(64, '4'),
        std::string(64, '5'), 85, unit, receipts);
    const auto path = temporary.path() / "proof.json";
    write(path, proof);
    const retirement::retirement_proof parsed =
        retirement::load_retirement_proof(path);
    CHECK(parsed.reports.size() == 5);
    CHECK(parsed.unit_receipt_sha256 == unit.receipt_sha256);

    receipts.pop_back();
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        retirement::render_retirement_proof(
            source, std::string(64, '3'), std::string(64, '4'),
            std::string(64, '5'), 85, unit, receipts),
        ContainsSubstring("exactly five"));
}

TEST_CASE(
    "retired-final transition binds the exact source unit and five reports",
    "[cache][unit][retirement]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const retirement::retirement_proof proof =
        historical_proof(temporary.path());
    const retirement::gate_bundle bundle =
        retired_bundle(temporary.path());
    const std::string proof_sha256 = repeated_hex('7');
    const std::string inventory_sha256 = repeated_hex('5');
    const std::string document =
        retirement::render_retirement_transition(
            proof_sha256, proof, bundle, inventory_sha256);
    const retirement::retirement_transition transition =
        retirement::load_retirement_transition(
            temporary.path() / "transition.json", document);
    CHECK_NOTHROW(retirement::validate_transition_against_chain(
        transition, proof_sha256, proof, bundle,
        repeated_hex('4'), inventory_sha256));

    using Catch::Matchers::ContainsSubstring;
    retirement::retirement_transition altered = transition;
    altered.retired_source_revision = repeated_hex('0', 40);
    CHECK_THROWS_WITH(
        retirement::validate_transition_against_chain(
            altered, proof_sha256, proof, bundle,
            repeated_hex('4'), inventory_sha256),
        ContainsSubstring(
            "does not bind the exact proof/deletion/source contract"));

    altered = transition;
    altered.retirement_transition_sha256 =
        different_sha256(transition.retirement_transition_sha256);
    CHECK_THROWS_WITH(
        retirement::validate_transition_against_chain(
            altered, proof_sha256, proof, bundle,
            repeated_hex('4'), inventory_sha256),
        ContainsSubstring(
            "does not bind the exact proof/deletion/source contract"));

    altered = transition;
    altered.unit_receipt_sha256 =
        different_sha256(transition.unit_receipt_sha256);
    CHECK_THROWS_WITH(
        retirement::validate_transition_against_chain(
            altered, proof_sha256, proof, bundle,
            repeated_hex('4'), inventory_sha256),
        ContainsSubstring(
            "does not bind the exact proof/deletion/source contract"));

    for (std::size_t index = 0;
         index < retirement::required_report_roles.size(); ++index) {
        altered = transition;
        altered.reports[index].receipt_sha256 =
            different_sha256(altered.reports[index].receipt_sha256);
        CHECK_THROWS_WITH(
            retirement::validate_transition_against_chain(
                altered, proof_sha256, proof, bundle,
                repeated_hex('4'), inventory_sha256),
            ContainsSubstring(
                "does not bind every proof-sealed report"));

        altered = transition;
        altered.reports[index].native_report_sha256 =
            different_sha256(
                altered.reports[index].native_report_sha256);
        CHECK_THROWS_WITH(
            retirement::validate_transition_against_chain(
                altered, proof_sha256, proof, bundle,
                repeated_hex('4'), inventory_sha256),
            ContainsSubstring(
                "does not bind every proof-sealed report"));

        altered = transition;
        altered.reports.erase(
            altered.reports.begin()
            + static_cast<std::ptrdiff_t>(index));
        CHECK_THROWS_WITH(
            retirement::validate_transition_against_chain(
                altered, proof_sha256, proof, bundle,
                repeated_hex('4'), inventory_sha256),
            ContainsSubstring("report roster is incomplete"));
    }
}

TEST_CASE(
    "retired-final rejects missing altered or current-only historical unit",
    "[cache][unit][retirement]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto root = temporary.path();
    const retirement::retirement_proof proof =
        historical_proof(root);
    const retirement::unit_receipt unit = historical_unit(root);
    CHECK_NOTHROW(retirement::validate_transition_unit_receipt(
        unit, proof, repeated_hex('4')));

    using Catch::Matchers::ContainsSubstring;
    retirement::unit_receipt altered = unit;
    altered.receipt_sha256 =
        different_sha256(unit.receipt_sha256);
    CHECK_THROWS_WITH(
        retirement::validate_transition_unit_receipt(
            altered, proof, repeated_hex('4')),
        ContainsSubstring(
            "is not the proof-ready receipt sealed by the retirement "
            "proof"));

    altered = unit;
    altered.source = retired_source();
    altered.migration_phase = "retired_with_proof";
    CHECK_THROWS_WITH(
        retirement::validate_transition_unit_receipt(
            altered, proof, repeated_hex('4')),
        ContainsSubstring(
            "is not the proof-ready receipt sealed by the retirement "
            "proof"));

    CHECK_THROWS(retirement::load_unit_receipt(
        root / "missing-proof-unit.json", repeated_hex('6')));

    std::string missing_unit =
        bundle_document("retired-final", repeated_hex('a'), root);
    missing_unit = replace_once(
        std::move(missing_unit),
        "\"proof_unit_receipt\":\""
            + (root / "proof-unit.json").string() + "\"",
        "\"proof_unit_receipt\":\"\"");
    CHECK_THROWS_WITH(
        retirement::load_gate_bundle(
            root / "missing-proof-unit-bundle.json", missing_unit),
        ContainsSubstring(
            "requires the proof-sealed unit and exact five historical "
            "report receipts"));
}

TEST_CASE(
    "every historical role receipt and native report remains proof sealed",
    "[cache][unit][retirement]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const retirement::retirement_proof proof =
        historical_proof(temporary.path());
    using Catch::Matchers::ContainsSubstring;

    for (std::size_t index = 0;
         index < retirement::required_report_roles.size(); ++index) {
        const retirement::report_role role =
            retirement::required_report_roles[index];
        const retirement::evidence_receipt receipt =
            historical_receipt(temporary.path(), role, index);
        const retirement::native_report_evidence native =
            historical_native(temporary.path(), role, index);
        CHECK_NOTHROW(
            retirement::validate_transition_receipt_and_native_report(
                receipt, native, proof, repeated_hex('4'),
                receipt.native_report_sha256));

        retirement::evidence_receipt altered_receipt = receipt;
        altered_receipt.receipt_sha256 =
            different_sha256(receipt.receipt_sha256);
        CHECK_THROWS_WITH(
            retirement::validate_transition_receipt_and_native_report(
                altered_receipt, native, proof, repeated_hex('4'),
                receipt.native_report_sha256),
            ContainsSubstring(
                "is not the proof-ready evidence sealed by the "
                "retirement proof"));

        altered_receipt = receipt;
        altered_receipt.native_report_sha256 =
            different_sha256(receipt.native_report_sha256);
        CHECK_THROWS_WITH(
            retirement::validate_transition_receipt_and_native_report(
                altered_receipt, native, proof, repeated_hex('4'),
                altered_receipt.native_report_sha256),
            ContainsSubstring(
                "is not the proof-ready evidence sealed by the "
                "retirement proof"));

        CHECK_THROWS_WITH(
            retirement::validate_transition_receipt_and_native_report(
                receipt, native, proof, repeated_hex('4'),
                different_sha256(receipt.native_report_sha256)),
            ContainsSubstring(
                "is not the proof-ready evidence sealed by the "
                "retirement proof"));

        retirement::retirement_proof missing_role = proof;
        missing_role.reports.erase(
            missing_role.reports.begin()
            + static_cast<std::ptrdiff_t>(index));
        CHECK_THROWS_WITH(
            retirement::validate_transition_receipt_and_native_report(
                receipt, native, missing_role, repeated_hex('4'),
                receipt.native_report_sha256),
            ContainsSubstring("retirement proof omits report role"));

        altered_receipt = receipt;
        altered_receipt.source = retired_source();
        altered_receipt.migration_ledger_sha256 = repeated_hex('7');
        altered_receipt.migration_phase = "retired_with_proof";
        CHECK_THROWS_WITH(
            retirement::validate_transition_receipt_and_native_report(
                altered_receipt, native, proof, repeated_hex('4'),
                receipt.native_report_sha256),
            ContainsSubstring(
                "is not the proof-ready evidence sealed by the "
                "retirement proof"));
    }
}

TEST_CASE(
    "historical report probes cannot be missing or substituted",
    "[cache][unit][retirement]") {
    namespace migration = bioseq::cache_test::migration;
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    using Catch::Matchers::ContainsSubstring;

    for (std::size_t index = 0;
         index < retirement::required_report_roles.size(); ++index) {
        const retirement::report_role role =
            retirement::required_report_roles[index];
        const migration::evidence_harness owner =
            retirement::role_owner(role);
        const std::string stable_id =
            "historical."
            + std::string(retirement::report_role_name(role));
        migration::ledger ledger;
        ledger.rows.push_back(migration::ledger_row{
            .legacy_path = "test/retired.sql",
            .legacy_line_or_assertion = "SELECT historical",
            .stable_id = stable_id,
            .owner = owner,
            .role = role,
            .source_kind = "sql",
            .scenario_or_permutation = "historical",
            .expected_sqlstate_or_result = "passed",
            .expected_state_invariant = "unchanged",
            .catch2_case = "historical case",
            .catch2_section = "historical section",
            .catch2_tags = "[cache]",
            .migration_status = migration::status::retired_with_proof,
            .line_number = index + 2,
        });
        migration::probe_observation probe{
            .owner = owner,
            .role = role,
            .stable_id = stable_id,
            .passed = true,
        };
        if (retirement::is_integration_role(role)) {
            probe.test_case = "historical case";
            probe.section = "historical section";
        } else {
            probe.origin_kind = "benchmark-phase";
            probe.origin = "historical-report";
        }
        retirement::native_report_evidence native =
            historical_native(temporary.path(), role, index);
        native.probes.push_back(probe);
        CHECK_NOTHROW(migration::validate_report_evidence(
            ledger, owner, role, native.probes));

        retirement::native_report_evidence missing = native;
        missing.probes.clear();
        CHECK_THROWS_WITH(
            migration::validate_report_evidence(
                ledger, owner, role, missing.probes),
            ContainsSubstring("expected 1, observed 0"));

        retirement::native_report_evidence altered = native;
        altered.probes.front().stable_id += ".substituted";
        CHECK_THROWS_WITH(
            migration::validate_report_evidence(
                ledger, owner, role, altered.probes),
            ContainsSubstring("unlisted migration evidence"));

        altered = native;
        if (retirement::is_integration_role(role)) {
            altered.probes.front().test_case = "current-only case";
        } else {
            altered.probes.front().origin_kind = "current-only-phase";
        }
        CHECK_THROWS_WITH(
            migration::validate_report_evidence(
                ledger, owner, role, altered.probes),
            ContainsSubstring("migration evidence provenance mismatch"));
    }
}

TEST_CASE(
    "cache-unit receipt seals the exact five-command native roster",
    "[cache][unit][retirement]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto receipt_path = temporary.path() / "unit.json";
    write(receipt_path, unit_receipt_document(temporary.path()));
    for (const std::string_view command : {
             "bioseq_cache_unit_tests",
             "bioseq_postgres_test_support_contract_tests",
             "bioseq_cache_benchmark_report_contract_tests",
             "bioseq_durable_file_io_contract_tests",
             "bioseq_cache_legacy_inventory_tracking"}) {
        write(temporary.path() / command, command);
    }
    retirement::unit_receipt receipt =
        retirement::load_unit_receipt(
            receipt_path, std::string(64, 'f'));
    REQUIRE(receipt.commands.size() == 5);
    const auto bundle_path = temporary.path() / "bundle.json";
    write(
        bundle_path,
        bundle_document("proof-ready", "", temporary.path()));
    const retirement::gate_bundle bundle =
        retirement::load_gate_bundle(bundle_path);
    const auto fake_sha256 =
        [](const std::filesystem::path&) {
            return std::string(64, 'e');
        };
    CHECK_NOTHROW(retirement::validate_unit_receipt(
        receipt, bundle, std::string(64, 'c'),
        std::string(64, 'd'), fake_sha256));

    receipt.commands.back().arguments.back() = "--wrong";
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        retirement::validate_unit_receipt(
            receipt, bundle, std::string(64, 'c'),
            std::string(64, 'd'), fake_sha256),
        ContainsSubstring("arguments are not exact"));

    write(
        receipt_path,
        unit_receipt_document(temporary.path(), 4));
    CHECK_THROWS_WITH(
        retirement::load_unit_receipt(
            receipt_path, std::string(64, 'f')),
        ContainsSubstring("exactly five"));
}

TEST_CASE(
    "benchmark retirement evidence enforces its fixed sample policy",
    "[cache][unit][retirement][benchmark]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto path = temporary.path() / "benchmark.json";
    const std::string standalone = benchmark_report_document(
        temporary.path(), retirement::report_role::cache_bench);
    const std::string combined = benchmark_report_document(
        temporary.path(),
        retirement::report_role::cache_bioseq_bench);
    CHECK_NOTHROW(
        retirement::load_benchmark_report(path, standalone));
    CHECK_NOTHROW(
        retirement::load_benchmark_report(path, combined));

    using Catch::Matchers::ContainsSubstring;
    const std::string missing = replace_once(
        standalone, "\"configured_samples\":7,", "");
    CHECK_THROWS_WITH(
        retirement::load_benchmark_report(path, missing),
        ContainsSubstring("configured_samples"));

    for (const std::string replacement :
         {"\"configured_samples\":0,",
          "\"configured_samples\":6,",
          "\"configured_samples\":8,"}) {
        const std::string altered = replace_once(
            standalone, "\"configured_samples\":7,", replacement);
        CHECK_THROWS_WITH(
            retirement::load_benchmark_report(path, altered),
            ContainsSubstring("sample policy"));
    }
    for (const std::string replacement :
         {"\"configured_warmup_samples\":0,",
          "\"configured_warmup_samples\":2,"}) {
        const std::string altered = replace_once(
            standalone,
            "\"configured_warmup_samples\":1,",
            replacement);
        CHECK_THROWS_WITH(
            retirement::load_benchmark_report(path, altered),
            ContainsSubstring("sample policy"));
    }
    const std::string combined_off_by_one = replace_once(
        combined, "\"configured_samples\":3,",
        "\"configured_samples\":4,");
    CHECK_THROWS_WITH(
        retirement::load_benchmark_report(
            path, combined_off_by_one),
        ContainsSubstring("sample policy"));
}

TEST_CASE(
    "native retirement evidence rejects decorative artifact identity",
    "[cache][unit][retirement][artifact]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto path = temporary.path() / "benchmark.json";
    const std::string document = benchmark_report_document(
        temporary.path(), retirement::report_role::cache_bench);
    retirement::native_report_evidence native =
        retirement::load_benchmark_report(path, document);
    REQUIRE(native.artifacts.size() == 1);

    using Catch::Matchers::ContainsSubstring;
    const std::string wrong_name = replace_once(
        document, "\"name\":\"bioseq_cache\"",
        "\"name\":\"bioseq\"");
    CHECK_THROWS_WITH(
        retirement::load_benchmark_report(path, wrong_name),
        ContainsSubstring("fixed role"));

    const std::string invalid_digest = replace_once(
        document,
        "\"stage_manifest_sha256\":\"" + repeated_hex('a') + "\"",
        "\"stage_manifest_sha256\":\"not-a-digest\"");
    CHECK_THROWS_WITH(
        retirement::load_benchmark_report(path, invalid_digest),
        ContainsSubstring("stage_manifest_sha256"));

    const auto current_sha256 =
        [&native](const std::filesystem::path& candidate) {
            const retirement::native_artifact_evidence& artifact =
                native.artifacts.front();
            if (candidate == artifact.stage_manifest) {
                return repeated_hex('a');
            }
            if (candidate == artifact.control_file
                || candidate == artifact.staged_control_file) {
                return repeated_hex('b');
            }
            if (candidate == artifact.install_sql
                || candidate == artifact.staged_install_sql) {
                return repeated_hex('c');
            }
            if (candidate == artifact.dso
                || candidate == artifact.staged_dso) {
                return repeated_hex('d');
            }
            return std::string(64, '0');
        };
    CHECK_NOTHROW(
        retirement::validate_current_native_artifacts(
            native, current_sha256));
    native.artifacts.front().dso_sha256 =
        different_sha256(native.artifacts.front().dso_sha256);
    CHECK_THROWS_WITH(
        retirement::validate_current_native_artifacts(
            native, current_sha256),
        ContainsSubstring("artifact identity is stale"));
}

TEST_CASE(
    "support integration evidence seals five artifacts and cleanup closure",
    "[cache][unit][retirement][integration][support]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto root = temporary.path();
    const auto report_path = root / "support-report.json";
    const std::string document =
        support_integration_report_document(root);
    retirement::native_report_evidence native =
        retirement::load_integration_report(report_path, document);

    CHECK(
        native.role
        == retirement::report_role::
            cache_bioseq_support_integration);
    REQUIRE(native.artifacts.size() == 5U);
    REQUIRE(native.successful_cleanup_roots.size() == 2U);

    const std::vector<std::filesystem::path> absent_paths =
        retirement::current_native_absent_paths(native);
    REQUIRE(absent_paths.size() == 4U);
    for (std::size_t index = 0; index < 2U; ++index) {
        const std::filesystem::path cleanup_root =
            support_cleanup_root(root, index);
        CHECK(std::ranges::contains(absent_paths, cleanup_root));
        CHECK(std::ranges::contains(
            absent_paths,
            cleanup_root / "runtime-library/bioseq_cache.so"));
    }

    const std::vector<retirement::native_artifact_evidence>
        reported_artifacts = native.artifacts;
    const auto reported_sha256 =
        [reported_artifacts](
            const std::filesystem::path& candidate) {
            for (const auto& artifact : reported_artifacts) {
                if (candidate == artifact.stage_manifest) {
                    return artifact.stage_manifest_sha256;
                }
                if (candidate == artifact.control_file) {
                    return artifact.control_sha256;
                }
                if (candidate == artifact.install_sql) {
                    return artifact.install_sql_sha256;
                }
                if (candidate == artifact.dso) {
                    return artifact.dso_sha256;
                }
                if (candidate == artifact.staged_control_file) {
                    return artifact.staged_control_sha256;
                }
                if (candidate == artifact.staged_install_sql) {
                    return artifact.install_sql_sha256;
                }
                if (candidate == artifact.staged_dso) {
                    return artifact.dso_sha256;
                }
            }
            return std::string(64, '0');
        };
    CHECK_NOTHROW(retirement::validate_current_native_artifacts(
        native, reported_sha256));

    const std::filesystem::path drifted_staged_control =
        native.artifacts.front().staged_control_file;
    const auto drifted_sha256 =
        [&reported_sha256, &drifted_staged_control](
            const std::filesystem::path& candidate) {
            const std::string reported = reported_sha256(candidate);
            return candidate == drifted_staged_control
                ? different_sha256(reported)
                : reported;
        };
    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        retirement::validate_current_native_artifacts(
            native, drifted_sha256),
        ContainsSubstring("artifact identity is stale"));
}

TEST_CASE(
    "support integration evidence rejects altered provider topology",
    "[cache][unit][retirement][integration][support]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto root = temporary.path();
    const auto report_path = root / "support-report.json";
    using Catch::Matchers::ContainsSubstring;

    for (const std::size_t provider_rows : {1U, 3U}) {
        INFO("provider rows: " << provider_rows);
        CHECK_THROWS_WITH(
            retirement::load_integration_report(
                report_path,
                support_integration_report_document(
                    root, provider_rows)),
            ContainsSubstring(
                "artifact roster has the wrong cardinality"));
    }

    CHECK_THROWS_WITH(
        retirement::load_integration_report(
            report_path,
            support_integration_report_document(
                root, 2U, true)),
        ContainsSubstring(
            "duplicates one staged artifact identity"));
    CHECK_THROWS_WITH(
        retirement::load_integration_report(
            report_path,
            support_integration_report_document(
                root, 2U, false, true)),
        ContainsSubstring(
            "provider-peer artifact is not bound"));

    const std::string document =
        support_integration_report_document(root);
    struct path_mutation {
        std::string_view label;
        std::filesystem::path before;
        std::filesystem::path after;
    };
    const std::array<path_mutation, 4> mutations{
        path_mutation{
            "stage",
            root / "support-stage",
            root / "swapped-stage"},
        path_mutation{
            "control",
            root / "source/bioseq_cache.control",
            root / "source/bioseq.control"},
        path_mutation{
            "SQL",
            root / "source/bioseq_cache--1.0.sql",
            root / "source/bioseq--1.0.sql"},
        path_mutation{
            "DSO",
            root / "source/bioseq_cache_provider_peer_test.so",
            root / "source/bioseq_cache.so"},
    };
    for (const path_mutation& mutation : mutations) {
        INFO("swapped artifact " << mutation.label);
        const std::string altered = replace_last_once(
            document,
            "\""
                + retirement::detail::json_escape(
                    mutation.before.string())
                + "\"",
            "\""
                + retirement::detail::json_escape(
                    mutation.after.string())
                + "\"");
        CHECK_THROWS(
            retirement::load_integration_report(
                report_path, altered));
    }
}

TEST_CASE(
    "support integration cleanup roots keep reported lexical identity",
    "[cache][unit][retirement][integration][support]") {
    namespace retirement = bioseq::cache_test::retirement;
    temporary_directory temporary;
    const auto root = temporary.path();
    const auto report_path = root / "support-report.json";
    const std::filesystem::path linked_parent =
        root / "cleanup-parent-link";
    std::error_code link_error;
    std::filesystem::create_directory_symlink(
        std::filesystem::canonical(
            std::filesystem::temp_directory_path()),
        linked_parent, link_error);
    if (link_error) {
        SKIP("directory symlinks are unavailable on this host");
    }

    const std::filesystem::path reported_root =
        support_cleanup_root(root, 0U);
    const std::filesystem::path symlink_parent_root =
        linked_parent / "bioseq-pg-test-symlink-parent";
    const std::string document =
        support_integration_report_document(root);
    const std::string altered = replace_once(
        document,
        "\"root\":\""
            + retirement::detail::json_escape(
                reported_root.string())
            + "\"",
        "\"root\":\""
            + retirement::detail::json_escape(
                symlink_parent_root.string())
            + "\"");

    using Catch::Matchers::ContainsSubstring;
    CHECK_THROWS_WITH(
        retirement::load_integration_report(
            report_path, altered),
        ContainsSubstring("fixed temporary parent"));
}
