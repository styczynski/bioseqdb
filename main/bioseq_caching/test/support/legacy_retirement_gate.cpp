#include "legacy_retirement_report.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import bioseq.test.postgres;
import bioseq.common.durable_file_io;

namespace {

struct arguments {
    std::filesystem::path bundle;
};

void reject_symlinked_absolute_components(
    const std::filesystem::path& input,
    bool allow_missing_leaf = false) {
    const std::filesystem::path path =
        std::filesystem::absolute(input).lexically_normal();
    std::filesystem::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        std::error_code error;
        const auto status =
            std::filesystem::symlink_status(current, error);
        if (error) {
            throw std::runtime_error(
                "cannot inspect retirement evidence path component "
                + current.string() + ": " + error.message());
        }
        if (!std::filesystem::exists(status)) {
            if (allow_missing_leaf && current == path) {
                return;
            }
            throw std::runtime_error(
                "retirement evidence path component is missing: "
                + current.string());
        }
        if (std::filesystem::is_symlink(status)) {
            throw std::runtime_error(
                "retirement evidence path contains a symlink component: "
                + current.string());
        }
    }
}

[[nodiscard]] arguments parse_arguments(int argc, char* argv[]) {
    arguments result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option{argv[index]};
        if (option != "--bundle") {
            throw std::invalid_argument(
                "unknown retirement gate option: "
                + std::string(option));
        }
        if (++index == argc || !result.bundle.empty()) {
            throw std::invalid_argument(
                "--bundle requires exactly one JSON path");
        }
        result.bundle = argv[index];
    }
    if (result.bundle.empty()) {
        throw std::invalid_argument("--bundle is required");
    }
    return result;
}

struct retained_regular_file {
    std::filesystem::path path;
    std::vector<std::byte> bytes;
    bioseq::durable_file_io::regular_file_identity identity;
    std::string sha256;
};

struct active_file_identity {
    std::filesystem::path path;
    bioseq::durable_file_io::regular_file_identity identity;
    std::string sha256;
};

[[nodiscard]] std::string_view bytes_as_string_view(
    std::span<const std::byte> bytes) noexcept {
    if (bytes.empty()) {
        return {};
    }
    return {
        reinterpret_cast<const char*>(bytes.data()),
        bytes.size()};
}

[[nodiscard]] std::string sha256_bytes(
    std::span<const std::byte> bytes) {
    return bioseq::postgres_test::sha256(
        bytes_as_string_view(bytes)).hex();
}

[[nodiscard]] retained_regular_file read_retained_regular_file(
    const std::filesystem::path& input,
    std::uint64_t maximum_bytes = 64U * 1024U * 1024U) {
    const std::filesystem::path path =
        std::filesystem::absolute(input).lexically_normal();
    reject_symlinked_absolute_components(path);
    auto retained =
        bioseq::durable_file_io::read_stable_regular_file(
            path,
            bioseq::durable_file_io::stable_read_options{
                .minimum_bytes = 1U,
                .maximum_bytes = maximum_bytes});
    if (!retained.has_value()) {
        throw std::runtime_error(
            "cannot retain stable retirement evidence "
            + path.string() + "; system_error="
            + std::to_string(retained.error().system_error));
    }
    retained_regular_file result{
        .path = path,
        .bytes = std::move(retained->bytes),
        .identity = retained->identity};
    result.sha256 = sha256_bytes(result.bytes);
    return result;
}

[[nodiscard]] const retained_regular_file& retain_document(
    std::deque<retained_regular_file>& retained,
    const std::filesystem::path& path) {
    retained.push_back(read_retained_regular_file(path));
    return retained.back();
}

[[nodiscard]] std::string retain_active_file(
    std::vector<active_file_identity>& active_files,
    const std::filesystem::path& input) {
    const std::filesystem::path path =
        std::filesystem::absolute(input).lexically_normal();
    const auto existing = std::ranges::find(
        active_files, path, &active_file_identity::path);
    if (existing != active_files.end()) {
        return existing->sha256;
    }
    retained_regular_file retained =
        read_retained_regular_file(path, 2ULL * 1024ULL * 1024ULL * 1024ULL);
    const std::string result = retained.sha256;
    active_files.push_back(active_file_identity{
        .path = std::move(retained.path),
        .identity = retained.identity,
        .sha256 = std::move(retained.sha256)});
    return result;
}

void verify_retained_file(const retained_regular_file& expected) {
    retained_regular_file current = read_retained_regular_file(
        expected.path,
        static_cast<std::uint64_t>(expected.bytes.size()));
    if (current.identity != expected.identity
        || current.bytes != expected.bytes
        || current.sha256 != expected.sha256) {
        throw std::runtime_error(
            "retirement evidence changed after validation: "
            + expected.path.string());
    }
}

void verify_active_file(const active_file_identity& expected) {
    retained_regular_file current = read_retained_regular_file(
        expected.path, expected.identity.size);
    if (current.identity != expected.identity
        || current.sha256 != expected.sha256) {
        throw std::runtime_error(
            "active retirement file changed after validation: "
            + expected.path.string());
    }
}

void verify_all_retained_inputs(
    const std::deque<retained_regular_file>& retained,
    const std::vector<active_file_identity>& active_files) {
    for (const retained_regular_file& input : retained) {
        verify_retained_file(input);
    }
    for (const active_file_identity& executable : active_files) {
        verify_active_file(executable);
    }
}

void verify_absent_path_without_symlink_components(
    const std::filesystem::path& input) {
    const std::filesystem::path path =
        std::filesystem::absolute(input).lexically_normal();
    std::filesystem::path current = path.root_path();
    for (const auto& component : path.relative_path()) {
        current /= component;
        std::error_code error;
        const auto status =
            std::filesystem::symlink_status(current, error);
        if (error == std::errc::no_such_file_or_directory
            || error == std::errc::not_a_directory) {
            return;
        }
        if (error) {
            throw std::runtime_error(
                "cannot inspect absent retirement path component "
                + current.string() + ": " + error.message());
        }
        if (!std::filesystem::exists(status)) {
            return;
        }
        if (std::filesystem::is_symlink(status)) {
            throw std::runtime_error(
                "absent retirement path contains a symlink component: "
                + current.string());
        }
    }
    throw std::runtime_error(
        "retirement evidence retained a supposedly cleaned path: "
        + path.string());
}

void verify_all_active_absences(
    const std::vector<std::filesystem::path>& absent_paths) {
    for (const std::filesystem::path& path : absent_paths) {
        verify_absent_path_without_symlink_components(path);
    }
}

void verify_published_receipt(
    const std::filesystem::path& path, std::string_view expected) {
    const retained_regular_file published =
        read_retained_regular_file(path, expected.size());
    if (bytes_as_string_view(published.bytes) != expected) {
        throw std::runtime_error(
            "durably published retirement receipt changed before "
            "success: " + path.string());
    }
}

void publish_proof_once(
    const std::filesystem::path& path, std::string_view proof) {
    const auto bytes = std::as_bytes(std::span{
        proof.data(), proof.size()});
    const auto publication =
        bioseq::durable_file_io::durable_create_file(path, bytes);
    if (!publication.has_value()
        || publication->publication
            != bioseq::durable_file_io::publication_state::durable) {
        const int system_error = publication.has_value()
            ? 0 : publication.error().system_error;
        throw std::runtime_error(
            "cannot durably publish append-only retirement receipt "
            + path.string() + "; system_error="
            + std::to_string(system_error));
    }
}

void validate_fixed_paths(
    const bioseq::cache_test::retirement::gate_bundle& bundle) {
    reject_symlinked_absolute_components(bundle.legacy_root);
    std::error_code status_error;
    const auto root_status =
        std::filesystem::symlink_status(
            bundle.legacy_root, status_error);
    if (status_error || std::filesystem::is_symlink(root_status)
        || !std::filesystem::is_directory(root_status)) {
        throw std::runtime_error(
            "retirement gate legacy_root must be a regular "
            "non-symlink directory");
    }
    const std::filesystem::path root =
        std::filesystem::canonical(bundle.legacy_root);
    for (const auto& contract :
         {bundle.ledger, bundle.legacy_inventory}) {
        reject_symlinked_absolute_components(contract);
        const auto status =
            std::filesystem::symlink_status(contract, status_error);
        if (status_error || std::filesystem::is_symlink(status)
            || !std::filesystem::is_regular_file(status)) {
            throw std::runtime_error(
                "retirement gate contract path must be a regular "
                "non-symlink file: " + contract.string());
        }
    }
    const std::filesystem::path ledger =
        std::filesystem::canonical(bundle.ledger);
    const std::filesystem::path inventory =
        std::filesystem::canonical(bundle.legacy_inventory);
    if (ledger != root / "test/legacy_migration_ledger.tsv"
        || inventory != root / "test/legacy_asset_inventory.txt") {
        throw std::runtime_error(
            "retirement gate ledger/inventory are not the permanent "
            "cache-module contract files");
    }
    const std::filesystem::path expected_proof =
        root / "test/legacy_retirement_receipt.json";
    reject_symlinked_absolute_components(
        bundle.proof_receipt,
        bundle.operation
            == bioseq::cache_test::retirement::gate_operation::proof_ready);
    if (std::filesystem::absolute(bundle.proof_receipt).lexically_normal()
        != expected_proof.lexically_normal()) {
        throw std::runtime_error(
            "retirement proof output is not the permanent cache-module "
            "receipt path");
    }
    if (bundle.operation
        != bioseq::cache_test::retirement::gate_operation::proof_ready) {
        const std::filesystem::path expected_transition =
            root.parent_path() / "reporting"
            / "bioseq_cache_retirement"
            / ("transition_"
               + bundle.current_source.revision + ".json");
        const std::filesystem::path reporting_root =
            root.parent_path() / "reporting";
        std::error_code directory_error;
        std::filesystem::create_directory(
            reporting_root, directory_error);
        if (directory_error
            && directory_error
                != std::errc::file_exists) {
            throw std::runtime_error(
                "cannot create fixed retirement reporting root: "
                + directory_error.message());
        }
        reject_symlinked_absolute_components(reporting_root);
        directory_error.clear();
        std::filesystem::create_directory(
            expected_transition.parent_path(), directory_error);
        if (directory_error
            && directory_error
                != std::errc::file_exists) {
            throw std::runtime_error(
                "cannot create fixed retirement receipt directory: "
                + directory_error.message());
        }
        reject_symlinked_absolute_components(
            expected_transition.parent_path());
        reject_symlinked_absolute_components(
            bundle.transition_receipt,
            bundle.operation
                == bioseq::cache_test::retirement::
                    gate_operation::retired_transition);
        if (std::filesystem::absolute(bundle.transition_receipt)
                .lexically_normal()
            != expected_transition.lexically_normal()) {
            throw std::runtime_error(
                "retirement transition output is not the fixed "
                "revision-bound reporting receipt path");
        }
    }
}

}  // namespace

int main(int argc, char* argv[]) {
    namespace migration = bioseq::cache_test::migration;
    namespace retirement = bioseq::cache_test::retirement;
    try {
        const arguments options = parse_arguments(argc, argv);
        std::deque<retained_regular_file> retained_inputs;
        std::vector<active_file_identity> active_files;
        std::vector<std::filesystem::path> active_absent_paths;
        const retained_regular_file& bundle_document =
            retain_document(retained_inputs, options.bundle);
        const retirement::gate_bundle bundle =
            retirement::load_gate_bundle(
                options.bundle,
                bytes_as_string_view(bundle_document.bytes));
        validate_fixed_paths(bundle);

        const retained_regular_file& ledger_document =
            retain_document(retained_inputs, bundle.ledger);
        const migration::ledger ledger =
            migration::load_ledger_document(
                bytes_as_string_view(ledger_document.bytes));
        const std::string& ledger_sha256 = ledger_document.sha256;
        const std::string evidence_contract_sha256 =
            bioseq::postgres_test::sha256(
                migration::canonical_evidence_contract_material(
                    ledger)).hex();
        const std::string_view expected_phase =
            bundle.phase == migration::retirement_phase::proof_ready
            ? "passing" : "retired_with_proof";
        if (migration::ledger_phase_name(ledger) != expected_phase) {
            throw std::runtime_error(
                "current migration ledger phase does not match the "
                "retirement union phase");
        }

        const retained_regular_file& inventory_document =
            retain_document(retained_inputs, bundle.legacy_inventory);
        const retirement::legacy_inventory inventory =
            retirement::load_legacy_inventory_document(
                bytes_as_string_view(inventory_document.bytes));
        const auto retain_inventory_asset =
            [&retained_inputs](const std::filesystem::path& path) {
                retained_inputs.push_back(
                    read_retained_regular_file(path));
                return retained_inputs.back().sha256;
            };
        retirement::validate_legacy_inventory(
            inventory, bundle.legacy_root, bundle.phase,
            retain_inventory_asset);
        const auto revalidate_inventory_namespace =
            [&inventory, &bundle]() {
                retirement::validate_legacy_inventory(
                    inventory, bundle.legacy_root, bundle.phase,
                    [](const std::filesystem::path& path) {
                        return read_retained_regular_file(path).sha256;
                    });
            };
        const std::string& inventory_sha256 =
            inventory_document.sha256;
        std::optional<retirement::retirement_proof> prior_proof;
        std::string prior_proof_sha256;
        if (bundle.operation
            != retirement::gate_operation::proof_ready) {
            const retained_regular_file& proof_document =
                retain_document(retained_inputs, bundle.proof_receipt);
            prior_proof_sha256 = proof_document.sha256;
            prior_proof =
                retirement::load_retirement_proof(
                    bundle.proof_receipt,
                    bytes_as_string_view(proof_document.bytes));
        }

        const retained_regular_file& unit_document =
            retain_document(retained_inputs, bundle.unit_receipt);
        const retirement::unit_receipt unit =
            retirement::load_unit_receipt(
                bundle.unit_receipt, unit_document.sha256,
                bytes_as_string_view(unit_document.bytes));
        if (bundle.operation
            == retirement::gate_operation::retired_transition) {
            retirement::validate_transition_unit_receipt(
                unit, *prior_proof, evidence_contract_sha256);
        } else {
            retirement::validate_unit_receipt(
                unit, bundle, ledger_sha256,
                evidence_contract_sha256,
                [&active_files](const std::filesystem::path& path) {
                    return retain_active_file(active_files, path);
                });
        }

        std::vector<retirement::evidence_receipt> receipts;
        std::vector<migration::probe_observation> combined_probes;
        receipts.reserve(bundle.evidence_receipts.size());
        for (const std::filesystem::path& receipt_path :
             bundle.evidence_receipts) {
            const retained_regular_file& receipt_document =
                retain_document(retained_inputs, receipt_path);
            retirement::evidence_receipt receipt =
                retirement::load_evidence_receipt(
                    receipt_path, receipt_document.sha256,
                    bytes_as_string_view(receipt_document.bytes));
            if (!receipt.native_report.is_absolute()) {
                throw std::runtime_error(
                    "MCP evidence receipt native report path is not "
                    "absolute");
            }
            const retained_regular_file& native_document =
                retain_document(
                    retained_inputs, receipt.native_report);
            const std::string& native_report_sha256 =
                native_document.sha256;
            retirement::native_report_evidence native =
                retirement::is_integration_role(receipt.role)
                ? retirement::load_integration_report(
                      receipt.native_report,
                      bytes_as_string_view(native_document.bytes))
                : retirement::load_benchmark_report(
                      receipt.native_report,
                      bytes_as_string_view(native_document.bytes));
            if (bundle.operation
                == retirement::gate_operation::retired_transition) {
                retirement::
                    validate_transition_receipt_and_native_report(
                        receipt, native, *prior_proof,
                        evidence_contract_sha256,
                        native_report_sha256);
            } else {
                const std::string executable_sha256 =
                    retain_active_file(active_files, native.executable);
                retirement::validate_current_native_artifacts(
                    native,
                    [&active_files](const std::filesystem::path& path) {
                        return retain_active_file(
                            active_files, path);
                    });
                const auto absent_paths =
                    retirement::current_native_absent_paths(native);
                active_absent_paths.insert(
                    active_absent_paths.end(),
                    absent_paths.begin(), absent_paths.end());
                retirement::validate_receipt_and_native_report(
                    receipt, native, bundle, ledger_sha256,
                    evidence_contract_sha256, native_report_sha256,
                    executable_sha256);
            }
            migration::validate_report_evidence(
                ledger, native.owner, native.role, native.probes);
            combined_probes.insert(
                combined_probes.end(),
                std::make_move_iterator(native.probes.begin()),
                std::make_move_iterator(native.probes.end()));
            receipts.push_back(std::move(receipt));
        }
        retirement::validate_exact_report_roster(receipts);
        migration::validate_combined_evidence(ledger, combined_probes);

        if (bundle.operation
            == retirement::gate_operation::retired_final) {
            const retained_regular_file& historical_unit_document =
                retain_document(
                    retained_inputs, bundle.proof_unit_receipt);
            const retirement::unit_receipt historical_unit =
                retirement::load_unit_receipt(
                    bundle.proof_unit_receipt,
                    historical_unit_document.sha256,
                    bytes_as_string_view(
                        historical_unit_document.bytes));
            retirement::validate_transition_unit_receipt(
                historical_unit, *prior_proof,
                evidence_contract_sha256);
            std::vector<retirement::evidence_receipt>
                historical_receipts;
            std::vector<migration::probe_observation>
                historical_probes;
            historical_receipts.reserve(
                bundle.proof_evidence_receipts.size());
            for (const std::filesystem::path& receipt_path :
                 bundle.proof_evidence_receipts) {
                const retained_regular_file& receipt_document =
                    retain_document(retained_inputs, receipt_path);
                retirement::evidence_receipt receipt =
                    retirement::load_evidence_receipt(
                        receipt_path, receipt_document.sha256,
                        bytes_as_string_view(receipt_document.bytes));
                if (!receipt.native_report.is_absolute()) {
                    throw std::runtime_error(
                        "historical MCP evidence report path is not "
                        "absolute");
                }
                const retained_regular_file& native_document =
                    retain_document(
                        retained_inputs, receipt.native_report);
                const std::string& native_report_sha256 =
                    native_document.sha256;
                retirement::native_report_evidence native =
                    retirement::is_integration_role(receipt.role)
                    ? retirement::load_integration_report(
                          receipt.native_report,
                          bytes_as_string_view(
                              native_document.bytes))
                    : retirement::load_benchmark_report(
                          receipt.native_report,
                          bytes_as_string_view(
                              native_document.bytes));
                retirement::
                    validate_transition_receipt_and_native_report(
                        receipt, native, *prior_proof,
                        evidence_contract_sha256,
                        native_report_sha256);
                migration::validate_report_evidence(
                    ledger, native.owner, native.role,
                    native.probes);
                historical_probes.insert(
                    historical_probes.end(),
                    std::make_move_iterator(native.probes.begin()),
                    std::make_move_iterator(native.probes.end()));
                historical_receipts.push_back(std::move(receipt));
            }
            retirement::validate_exact_report_roster(
                historical_receipts);
            migration::validate_combined_evidence(
                ledger, historical_probes);
        }

        const retained_regular_file& catch_case_document =
            retain_document(retained_inputs, bundle.catch_cases);
        const auto catch_cases =
            migration::load_catch_case_manifest_document(
                bytes_as_string_view(catch_case_document.bytes));
        const auto lifecycle = migration::validate_global_ledger(
            ledger, catch_cases, bundle.legacy_root,
            retirement::inventory_paths(inventory), combined_probes,
            bundle.phase, inventory.assets.size());
        if (bundle.operation
            == retirement::gate_operation::proof_ready) {
            if (!lifecycle.proof_ready
                || lifecycle.retirement_complete) {
                throw std::runtime_error(
                    "proof-ready lifecycle verdict is inconsistent");
            }
            const std::string proof =
                retirement::render_retirement_proof(
                    bundle.current_source, ledger_sha256,
                    evidence_contract_sha256, inventory_sha256,
                    inventory.assets.size(), unit, receipts);
            verify_all_retained_inputs(
                retained_inputs, active_files);
            verify_all_active_absences(active_absent_paths);
            revalidate_inventory_namespace();
            publish_proof_once(bundle.proof_receipt, proof);
            verify_published_receipt(bundle.proof_receipt, proof);
            verify_all_retained_inputs(
                retained_inputs, active_files);
            verify_all_active_absences(active_absent_paths);
            revalidate_inventory_namespace();
            std::cout
                << "bioseq_cache retirement proof published: "
                << bundle.proof_receipt << "; reports="
                << receipts.size() << "; ledger_rows="
                << ledger.rows.size() << '\n';
            return 0;
        }

        if (!lifecycle.retirement_complete) {
            throw std::runtime_error(
                "retired lifecycle verdict is incomplete");
        }
        retirement::validate_proof_against_current_contract(
            *prior_proof, receipts, evidence_contract_sha256,
            inventory_sha256, inventory.assets.size());
        if (prior_proof->proof_source_revision
                == bundle.current_source.revision
            || prior_proof->proof_source_scope_sha256
                == bundle.current_source.scope_sha256
            || prior_proof->migration_ledger_sha256 == ledger_sha256) {
            throw std::runtime_error(
                "retired gate did not cross a committed proof/deletion "
                "boundary");
        }
        if (bundle.operation
            == retirement::gate_operation::retired_transition) {
            const std::string transition =
                retirement::render_retirement_transition(
                    prior_proof_sha256, *prior_proof, bundle,
                    inventory_sha256);
            verify_all_retained_inputs(
                retained_inputs, active_files);
            verify_all_active_absences(active_absent_paths);
            revalidate_inventory_namespace();
            publish_proof_once(
                bundle.transition_receipt, transition);
            verify_published_receipt(
                bundle.transition_receipt, transition);
            verify_all_retained_inputs(
                retained_inputs, active_files);
            verify_all_active_absences(active_absent_paths);
            revalidate_inventory_namespace();
            std::cout
                << "bioseq_cache retired transition authorized by "
                << "proof-ready evidence: reports=" << receipts.size()
                << "; transition="
                << bundle.retirement_transition_sha256 << '\n';
            return 0;
        }
        const retained_regular_file& transition_document =
            retain_document(retained_inputs, bundle.transition_receipt);
        const retirement::retirement_transition transition =
            retirement::load_retirement_transition(
                bundle.transition_receipt,
                bytes_as_string_view(transition_document.bytes));
        retirement::validate_transition_against_chain(
            transition, prior_proof_sha256, *prior_proof, bundle,
            evidence_contract_sha256, inventory_sha256);
        verify_all_retained_inputs(retained_inputs, active_files);
        verify_all_active_absences(active_absent_paths);
        revalidate_inventory_namespace();
        std::cout
            << "bioseq_cache legacy retirement verified: reports="
            << receipts.size() << "; retired_rows="
            << lifecycle.retired_with_proof_rows
            << "; transition="
            << bundle.retirement_transition_sha256 << '\n';
        return 0;
    } catch (const std::invalid_argument& error) {
        std::cerr << "bioseq_cache retirement gate usage error: "
                  << error.what() << '\n';
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "bioseq_cache retirement gate failed: "
                  << error.what() << '\n';
        return 1;
    }
}
