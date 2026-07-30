#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "legacy_migration_ledger.hpp"

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <libproc.h>
#include <mach-o/dyld.h>
#include <sys/proc.h>
#endif

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

namespace {

struct catch_report_evidence {
    std::vector<bioseq::postgres_test::test_case_report> cases;
    std::vector<bioseq::postgres_test::artifact_report>
        attested_artifacts;
    std::string current_case;
    std::vector<std::string> section_stack;
    std::vector<std::size_t> section_failure_offsets;
    std::string failure_message;
    std::chrono::steady_clock::time_point case_started{};
    std::optional<bioseq::postgres_test::spawned_process> watchdog;
    std::filesystem::path watchdog_state;
    std::vector<std::filesystem::path> retained_watchdog_states;
    bool watchdog_protocol_failed{};
    bool watchdog_retention_recording_failed{};
};

[[nodiscard]] catch_report_evidence& catch_evidence() {
    static catch_report_evidence evidence;
    return evidence;
}

[[nodiscard]] bool same_artifact_identity(
    const bioseq::postgres_test::artifact_report& left,
    const bioseq::postgres_test::artifact_report& right) {
    return left.name == right.name
        && left.preinstalled_stage_root
            == right.preinstalled_stage_root
        && left.stage_manifest == right.stage_manifest
        && left.stage_manifest_sha256
            == right.stage_manifest_sha256
        && left.control_file == right.control_file
        && left.install_sql == right.install_sql
        && left.dso == right.dso
        && left.staged_control_file == right.staged_control_file
        && left.staged_install_sql == right.staged_install_sql
        && left.staged_dso == right.staged_dso
        && left.control_sha256 == right.control_sha256
        && left.install_sql_sha256 == right.install_sql_sha256
        && left.dso_sha256 == right.dso_sha256
        && left.staged_control_sha256
            == right.staged_control_sha256;
}

void retain_attested_artifacts(catch_report_evidence& evidence) {
    for (const auto& artifact :
         bioseq::postgres_test::artifact_stage_reports()) {
        const bool already_retained = std::ranges::any_of(
            evidence.attested_artifacts,
            [&artifact](
                const bioseq::postgres_test::artifact_report& retained) {
                return same_artifact_identity(retained, artifact);
            });
        if (!already_retained) {
            evidence.attested_artifacts.push_back(artifact);
        }
    }
}

void begin_unattested_artifact_scope(
    catch_report_evidence& evidence,
    bool publish_empty_snapshot) {
    retain_attested_artifacts(evidence);
    bioseq::postgres_test::clear_artifact_stage_reports();
    if (publish_empty_snapshot) {
        const std::vector<bioseq::postgres_test::artifact_report> empty;
        if (!bioseq::postgres_test::
                publish_supervision_artifact_reports(empty)) {
            throw std::runtime_error(
                "cannot clear crash-resilient artifact evidence");
        }
    }
}

struct watchdog_launch_configuration {
    std::filesystem::path executable;
    std::string executable_sha256;
    std::filesystem::path report;
    std::filesystem::path migration_ledger;
    std::string migration_evidence_harness;
    std::string migration_report_role;
    std::string migration_phase;
    std::string migration_ledger_sha256;
    std::string evidence_contract_sha256;
};

constexpr std::string_view watchdog_armed_magic =
    "bioseq-watchdog-armed-v1";
constexpr auto watchdog_arming_budget = std::chrono::seconds(5);

[[nodiscard]] std::optional<watchdog_launch_configuration>&
watchdog_launch() {
    static std::optional<watchdog_launch_configuration> configuration;
    return configuration;
}

[[nodiscard]] std::filesystem::path watchdog_armed_path(
    const std::filesystem::path& state_directory) {
    return state_directory / "armed";
}

[[nodiscard]] std::string watchdog_armed_record(
    std::uint64_t parent_pid,
    std::uint64_t watchdog_pid) {
    return std::string(watchdog_armed_magic) + "\t"
        + std::to_string(parent_pid) + "\t"
        + std::to_string(watchdog_pid) + "\n";
}

[[nodiscard]] bool publish_watchdog_armed(
    const std::filesystem::path& state_directory,
    std::uint64_t parent_pid) noexcept {
    try {
        const std::filesystem::path armed =
            watchdog_armed_path(state_directory);
        const std::filesystem::path partial =
            state_directory / "armed.partial";
        const std::string record = watchdog_armed_record(
            parent_pid, static_cast<std::uint64_t>(::getpid()));
        const int descriptor = ::open(
            partial.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0) {
            return false;
        }
        bool written_all = true;
        std::size_t offset = 0;
        while (offset < record.size()) {
            const ssize_t written = ::write(
                descriptor, record.data() + offset,
                record.size() - offset);
            if (written < 0 && errno == EINTR) {
                continue;
            }
            if (written <= 0) {
                written_all = false;
                break;
            }
            offset += static_cast<std::size_t>(written);
        }
        if (written_all && ::fsync(descriptor) != 0) {
            written_all = false;
        }
        if (::close(descriptor) != 0) {
            written_all = false;
        }
        std::error_code error;
        if (written_all) {
            std::filesystem::rename(partial, armed, error);
            written_all = !error;
        }
        if (!written_all) {
            error.clear();
            static_cast<void>(
                std::filesystem::remove(partial, error));
        }
        return written_all;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::string watchdog_exit_detail(
    const bioseq::postgres_test::process_exit& exit) {
    if (exit.exit_code) {
        return "exit code " + std::to_string(*exit.exit_code);
    }
    if (exit.signal) {
        return "signal " + std::to_string(*exit.signal);
    }
    return "unknown process status";
}

void await_watchdog_armed(
    bioseq::postgres_test::spawned_process& watchdog,
    const std::filesystem::path& state_directory,
    std::uint64_t parent_pid) {
    const std::int64_t child_pid = watchdog.pid();
    if (child_pid <= 1) {
        throw std::runtime_error(
            "external watchdog has no live child identity");
    }
    const std::string expected = watchdog_armed_record(
        parent_pid, static_cast<std::uint64_t>(child_pid));
    const std::filesystem::path armed =
        watchdog_armed_path(state_directory);
    const auto deadline =
        std::chrono::steady_clock::now() + watchdog_arming_budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto exit = watchdog.try_wait()) {
            throw std::runtime_error(
                "external watchdog exited before arming: "
                + watchdog_exit_detail(*exit)
                + "; captured output:\n"
                + watchdog.captured_output());
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(armed, error);
        if (error) {
            throw std::runtime_error(
                "cannot inspect external-watchdog armed evidence: "
                + error.message());
        }
        if (exists) {
            if (!std::filesystem::is_regular_file(armed, error)
                || error) {
                throw std::runtime_error(
                    "external-watchdog armed evidence is not a "
                    "regular file");
            }
            std::ifstream input(armed, std::ios::binary);
            const std::string actual{
                std::istreambuf_iterator<char>(input),
                std::istreambuf_iterator<char>()};
            if (!input || actual != expected) {
                throw std::runtime_error(
                    "external-watchdog armed evidence is malformed");
            }
            if (const auto exit = watchdog.try_wait()) {
                throw std::runtime_error(
                    "external watchdog exited while arming: "
                    + watchdog_exit_detail(*exit)
                    + "; captured output:\n"
                    + watchdog.captured_output());
            }
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (const auto exit = watchdog.try_wait()) {
        throw std::runtime_error(
            "external watchdog exited before arming: "
            + watchdog_exit_detail(*exit)
            + "; captured output:\n"
            + watchdog.captured_output());
    }
    throw std::runtime_error(
        "external watchdog did not arm within "
        + std::to_string(watchdog_arming_budget.count())
        + " seconds");
}

[[nodiscard]] std::filesystem::path make_watchdog_state() {
    std::string pattern =
        (std::filesystem::temp_directory_path()
         / "bioseq-watchdog-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) {
        throw std::runtime_error(
            "cannot create external-watchdog state directory");
    }
    return std::filesystem::path(created);
}

[[nodiscard]] std::filesystem::path current_executable_path(
    [[maybe_unused]] std::string_view argv_zero) {
#if defined(__linux__)
    return std::filesystem::canonical("/proc/self/exe");
#elif defined(__APPLE__)
    std::uint32_t size{};
    static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
    std::vector<char> path(size);
    if (size == 0
        || ::_NSGetExecutablePath(path.data(), &size) != 0) {
        throw std::runtime_error(
            "cannot identify native test executable");
    }
    return std::filesystem::canonical(path.data());
#else
    return std::filesystem::canonical(
        std::filesystem::absolute(argv_zero));
#endif
}

void seed_supervision_evidence(
    const catch_report_evidence& evidence) {
    const bool published =
        bioseq::postgres_test::publish_supervision_test_case_reports(
            evidence.cases)
        && bioseq::postgres_test::
            publish_supervision_phase_duration_reports(
                bioseq::postgres_test::phase_duration_reports())
        && bioseq::postgres_test::
            publish_supervision_host_qualification_reports(
                bioseq::postgres_test::host_qualification_reports())
        && bioseq::postgres_test::
            publish_supervision_migration_probe_records(
                bioseq::postgres_test::migration_probe_records())
        && bioseq::postgres_test::publish_supervision_artifact_reports(
            bioseq::postgres_test::artifact_stage_reports());
    if (!published) {
        throw std::runtime_error(
            "cannot seed crash-resilient watchdog evidence");
    }
}

void retain_watchdog_state(catch_report_evidence& evidence) noexcept {
    if (evidence.watchdog_state.empty()) {
        return;
    }
    try {
        evidence.retained_watchdog_states.push_back(
            evidence.watchdog_state);
        evidence.watchdog_state.clear();
    } catch (...) {
        // Keep watchdog_state itself intact so the unrecorded path remains
        // recoverable and can be reported without allocating here.
        evidence.watchdog_retention_recording_failed = true;
    }
}

void stop_watchdog(catch_report_evidence& evidence) noexcept {
    try {
        bool registries_empty = true;
        if (!evidence.watchdog_state.empty()) {
            registries_empty =
                bioseq::postgres_test::supervision_registries_empty(
                    evidence.watchdog_state);
        }
        bool decision_owned = false;
        bool watchdog_exit_succeeded = false;
        if (evidence.watchdog
            && !evidence.watchdog_state.empty()) {
            decision_owned =
                bioseq::postgres_test::try_claim_supervision_decision(
                    evidence.watchdog_state,
                    registries_empty ? "disarmed" : "retained");
            try {
                const bioseq::postgres_test::process_exit exit =
                    evidence.watchdog->wait_for(
                        decision_owned
                            ? std::chrono::seconds(2)
                            : std::chrono::seconds(30));
                watchdog_exit_succeeded = exit.succeeded();
            } catch (...) {
                evidence.watchdog_protocol_failed = true;
            }
            if (!decision_owned || !watchdog_exit_succeeded) {
                evidence.watchdog_protocol_failed = true;
            }
        }
        evidence.watchdog.reset();
        bioseq::postgres_test::disable_supervision_state();
        if (!evidence.watchdog_state.empty()) {
            const bool registries_still_empty =
                registries_empty
                && bioseq::postgres_test::supervision_registries_empty(
                    evidence.watchdog_state);
            const bool clean_disarm =
                decision_owned && watchdog_exit_succeeded
                && registries_still_empty;
            const bool removed = clean_disarm
                && bioseq::postgres_test::remove_supervision_state(
                    evidence.watchdog_state);
            if (removed) {
                evidence.watchdog_state.clear();
            } else {
                retain_watchdog_state(evidence);
            }
        }
    } catch (...) {
        evidence.watchdog_protocol_failed = true;
        evidence.watchdog.reset();
        bioseq::postgres_test::disable_supervision_state();
        retain_watchdog_state(evidence);
    }
}

[[nodiscard]] std::string active_section(
    const catch_report_evidence& evidence) {
    std::string result;
    for (std::size_t index = 1;
         index < evidence.section_stack.size(); ++index) {
        if (!result.empty()) {
            result += " / ";
        }
        result += evidence.section_stack[index];
    }
    return result;
}

[[nodiscard]] std::string status(
    const Catch::Counts& counts,
    bool aborting = false) {
    if (aborting || counts.failed != 0) {
        return "failed";
    }
    if (counts.skipped != 0 && counts.passed == 0) {
        return "skipped";
    }
    return "passed";
}

[[nodiscard]] std::string test_case_status(
    const Catch::Totals& totals,
    bool aborting) {
    if (aborting || totals.assertions.failed != 0
        || totals.testCases.failed != 0) {
        return "failed";
    }
    if (totals.testCases.skipped != 0
        || (totals.assertions.skipped != 0
            && totals.assertions.passed == 0)) {
        return "skipped";
    }
    return "passed";
}

class native_report_listener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override {
        auto& evidence = catch_evidence();
        stop_watchdog(evidence);
        if (!evidence.watchdog_state.empty()) {
            throw std::runtime_error(
                "prior external-watchdog state could not be retained");
        }
        evidence = {};
        bioseq::postgres_test::clear_report_case_context();
        bioseq::postgres_test::clear_migration_probe_context();
    }

    void testCaseStarting(
        const Catch::TestCaseInfo& test_info) override {
        auto& evidence = catch_evidence();
        evidence.current_case = test_info.name;
        evidence.section_stack.clear();
        evidence.section_failure_offsets.clear();
        evidence.failure_message.clear();
        evidence.case_started = std::chrono::steady_clock::now();
        const bool large_case = std::ranges::any_of(
            test_info.tags,
            [](const Catch::Tag& tag) {
                return static_cast<std::string>(tag.original) == "2gbp";
            });
        const auto& runner_configuration =
            bioseq::cache_test::configuration();
        const std::chrono::seconds timeout = large_case
            ? runner_configuration.large_case_timeout
            : runner_configuration.case_timeout;
        if (!watchdog_launch()) {
            throw std::runtime_error(
                "external watchdog launch configuration is missing");
        }
        if (!evidence.watchdog_state.empty()
            || !evidence.retained_watchdog_states.empty()
            || evidence.watchdog_protocol_failed
            || evidence.watchdog_retention_recording_failed) {
            throw std::runtime_error(
                "prior external-watchdog supervision failure is still "
                "active");
        }
        // A case that fails before stage attestation must not inherit
        // manifest provenance from a previous fixture.
        begin_unattested_artifact_scope(evidence, false);
        evidence.watchdog_state = make_watchdog_state();
        try {
            bioseq::postgres_test::configure_supervision_state(
                evidence.watchdog_state);
            bioseq::postgres_test::
                initialize_supervision_report_journals();
            bioseq::postgres_test::
                initialize_supervision_resource_journal();
            seed_supervision_evidence(evidence);
            evidence.watchdog.emplace(
                bioseq::postgres_test::spawned_process::spawn(
                    bioseq::postgres_test::process_options{
                        .executable = watchdog_launch()->executable,
                        .arguments = {
                            "--bioseq-internal-watchdog",
                            evidence.watchdog_state.string(),
                            watchdog_launch()->report.string(),
                            evidence.current_case,
                            std::to_string(::getpid()),
                            std::to_string(timeout.count()),
                            watchdog_launch()->
                                migration_evidence_harness,
                            watchdog_launch()->migration_report_role,
                            watchdog_launch()->migration_phase,
                            watchdog_launch()->
                                migration_ledger_sha256,
                            watchdog_launch()->
                                evidence_contract_sha256,
                            watchdog_launch()->
                                migration_ledger.string(),
                            watchdog_launch()->
                                executable_sha256},
                        .output_file =
                            evidence.watchdog_state / "watchdog.log"}));
            await_watchdog_armed(
                *evidence.watchdog, evidence.watchdog_state,
                static_cast<std::uint64_t>(::getpid()));
        } catch (...) {
            stop_watchdog(evidence);
            throw;
        }
        bioseq::postgres_test::set_report_case_context(
            {.test_case = evidence.current_case});
        bioseq::postgres_test::update_migration_probe_case_section(
            evidence.current_case, {});
    }

    void sectionStarting(
        const Catch::SectionInfo& section_info) override {
        auto& evidence = catch_evidence();
        if (evidence.section_stack.empty()) {
            // Each outer section may own a different immutable stage.
            // Until that section attests one, the watchdog must see no
            // provenance inherited from an earlier section.
            begin_unattested_artifact_scope(evidence, true);
        }
        evidence.section_stack.push_back(section_info.name);
        evidence.section_failure_offsets.push_back(
            evidence.failure_message.size());
        bioseq::postgres_test::set_report_case_context({
            .test_case = evidence.current_case,
            .section = active_section(evidence)});
        bioseq::postgres_test::update_migration_probe_case_section(
            evidence.current_case, active_section(evidence));
    }

    void assertionEnded(
        const Catch::AssertionStats& assertion) override {
        if (assertion.assertionResult.isOk()) {
            return;
        }
        auto& message = catch_evidence().failure_message;
        if (!message.empty()) {
            message += '\n';
        }
        const Catch::StringRef reported =
            assertion.assertionResult.getMessage();
        if (!reported.empty()) {
            message += static_cast<std::string>(reported);
        } else if (assertion.assertionResult.hasExpandedExpression()) {
            message +=
                assertion.assertionResult.getExpandedExpression();
        } else {
            message += assertion.assertionResult.getExpression();
        }
    }

    void sectionEnded(
        const Catch::SectionStats& section_stats) override {
        auto& evidence = catch_evidence();
        if (evidence.section_stack.size() > 1) {
            std::string section_message;
            if (section_stats.assertions.failed != 0
                && !evidence.section_failure_offsets.empty()) {
                section_message = evidence.failure_message.substr(
                    evidence.section_failure_offsets.back());
                if (section_message.starts_with('\n')) {
                    section_message.erase(0, 1);
                }
            }
            evidence.cases.push_back(
                bioseq::postgres_test::test_case_report{
                    .test_case = evidence.current_case,
                    .section = active_section(evidence),
                    .status = status(section_stats.assertions),
                    .message = std::move(section_message),
                    .duration = std::chrono::duration_cast<
                        std::chrono::milliseconds>(
                            std::chrono::duration<double>(
                                section_stats.durationInSeconds))});
            static_cast<void>(
                bioseq::postgres_test::
                    publish_supervision_test_case_reports(
                        evidence.cases));
        }
        if (!evidence.section_stack.empty()) {
            evidence.section_stack.pop_back();
        }
        if (!evidence.section_failure_offsets.empty()) {
            evidence.section_failure_offsets.pop_back();
        }
        bioseq::postgres_test::set_report_case_context({
            .test_case = evidence.current_case,
            .section = active_section(evidence)});
        bioseq::postgres_test::update_migration_probe_case_section(
            evidence.current_case, active_section(evidence));
    }

    void testCaseEnded(
        const Catch::TestCaseStats& test_stats) override {
        auto& evidence = catch_evidence();
        std::string outcome =
            test_case_status(test_stats.totals, test_stats.aborting);
        if (bioseq::postgres_test::infrastructure_inconclusive_for_case(
                test_stats.testInfo->name)) {
            outcome = "infrastructure-inconclusive";
        }
        evidence.cases.push_back(
            bioseq::postgres_test::test_case_report{
                .test_case = test_stats.testInfo->name,
                .status = outcome,
                .message = evidence.failure_message,
                .duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now()
                        - evidence.case_started)});
        bioseq::postgres_test::reconcile_migration_probe_case(
            test_stats.testInfo->name, outcome == "passed");
        static_cast<void>(
            bioseq::postgres_test::
                publish_supervision_test_case_reports(evidence.cases));
        retain_attested_artifacts(evidence);
        const bool watchdog_protocol_failed_before =
            evidence.watchdog_protocol_failed;
        stop_watchdog(evidence);
        if (!watchdog_protocol_failed_before
            && evidence.watchdog_protocol_failed) {
            evidence.cases.back().status =
                "infrastructure-inconclusive";
            if (!evidence.cases.back().message.empty()) {
                evidence.cases.back().message += '\n';
            }
            evidence.cases.back().message +=
                "external watchdog failed validated disarm";
            bioseq::postgres_test::reconcile_migration_probe_case(
                test_stats.testInfo->name, false);
            static_cast<void>(
                bioseq::postgres_test::
                    publish_supervision_test_case_reports(
                        evidence.cases));
        }
        bioseq::postgres_test::clear_report_case_context();
        bioseq::postgres_test::clear_migration_probe_context();
        evidence.current_case.clear();
        evidence.section_stack.clear();
        evidence.section_failure_offsets.clear();
    }

    void skipTest(const Catch::TestCaseInfo& test_info) override {
        catch_evidence().cases.push_back(
            bioseq::postgres_test::test_case_report{
                .test_case = test_info.name,
                .status = "skipped"});
    }

    void testRunEnded(const Catch::TestRunStats&) override {
        stop_watchdog(catch_evidence());
    }
};

CATCH_REGISTER_LISTENER(native_report_listener);

/// @brief Builds source-artifact evidence before per-case private staging.
[[nodiscard]] bioseq::postgres_test::artifact_report make_artifact_report(
    std::string name,
    const std::filesystem::path& control,
    const std::filesystem::path& install_sql,
    const std::filesystem::path& dso) {
    return bioseq::postgres_test::artifact_report{
        .name = std::move(name),
        .control_file = control,
        .install_sql = install_sql,
        .dso = dso,
        .control_sha256 =
            bioseq::postgres_test::sha256_file(control).hex(),
        .install_sql_sha256 =
            bioseq::postgres_test::sha256_file(install_sql).hex(),
        .dso_sha256 =
            bioseq::postgres_test::sha256_file(dso).hex()};
}

void clear_stale_report_output(const std::filesystem::path& report) {
    const std::filesystem::path absolute =
        std::filesystem::absolute(report);
    std::filesystem::create_directories(absolute.parent_path());
    for (const std::filesystem::path& candidate :
         {absolute,
          absolute.parent_path()
              / (absolute.filename().string() + ".partial")}) {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(candidate, error));
        if (error) {
            throw std::runtime_error(
                "cannot clear stale native report output "
                + candidate.string() + ": " + error.message());
        }
    }
}

void reconcile_migration_probes(
    std::vector<bioseq::postgres_test::migration_probe_record>& probes,
    const std::vector<bioseq::postgres_test::test_case_report>& cases) {
    for (auto& probe : probes) {
        auto reconcile_section =
            [&cases, &probe](std::string_view section)
                -> std::optional<bool> {
            bool matched = false;
            bool passed = true;
            for (const auto& test_case : cases) {
                if (test_case.test_case != probe.test_case
                    || test_case.section != section) {
                    continue;
                }
                matched = true;
                passed = passed && test_case.status == "passed";
            }
            return matched ? std::optional<bool>{passed} : std::nullopt;
        };
        std::optional<bool> outcome =
            reconcile_section(probe.section);
        if (!outcome && !probe.section.empty()) {
            outcome = reconcile_section({});
        }
        probe.passed = probe.passed
            && outcome.value_or(false);
    }
}

struct migration_ledger_snapshot {
    std::filesystem::path path;
    bioseq::cache_test::migration::ledger parsed;
    std::string raw_sha256;
    std::string evidence_contract_sha256;
    std::string phase;
};

[[nodiscard]] migration_ledger_snapshot load_migration_ledger_snapshot(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "cannot read migration ledger: " + path.string());
    }
    const std::string raw{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        throw std::runtime_error(
            "failed while reading migration ledger: " + path.string());
    }
    auto parsed =
        bioseq::cache_test::migration::load_ledger_document(raw);
    const std::string evidence_contract_sha256 =
        bioseq::postgres_test::sha256(
            bioseq::cache_test::migration::
                canonical_evidence_contract_material(parsed)).hex();
    const std::string phase{
        bioseq::cache_test::migration::ledger_phase_name(parsed)};
    return migration_ledger_snapshot{
        .path = path,
        .parsed = std::move(parsed),
        .raw_sha256 = bioseq::postgres_test::sha256(raw).hex(),
        .evidence_contract_sha256 = evidence_contract_sha256,
        .phase = phase};
}

[[nodiscard]] bool migration_ledger_unchanged(
    const migration_ledger_snapshot& snapshot) noexcept {
    try {
        return bioseq::postgres_test::sha256_file(snapshot.path).hex()
            == snapshot.raw_sha256;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool file_sha256_unchanged(
    const std::filesystem::path& path,
    std::string_view expected_sha256) noexcept {
    try {
        return bioseq::postgres_test::sha256_file(path).hex()
            == expected_sha256;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool executable_allows_migration_role(
    const std::filesystem::path& executable,
    bioseq::cache_test::migration::evidence_harness owner,
    bioseq::cache_test::migration::evidence_role role) noexcept {
    using bioseq::cache_test::migration::evidence_harness;
    using bioseq::cache_test::migration::evidence_role;
    const std::string name = executable.filename().string();
    if (name == "bioseq_cache_integration_tests") {
        return owner == evidence_harness::cache_integration
            && role == evidence_role::cache_integration;
    }
    if (name
        == "bioseq_cache_bioseq_support_integration_tests") {
        return owner == evidence_harness::cache_bioseq_integration
            && role
                == evidence_role::cache_bioseq_support_integration;
    }
    if (name
        == "bioseq_cache_bioseq_production_integration_tests") {
        return owner == evidence_harness::cache_bioseq_integration
            && role
                == evidence_role::cache_bioseq_production_integration;
    }
    return false;
}

[[nodiscard]] bool migration_row_was_executed(
    const bioseq::cache_test::migration::ledger_row& row,
    const std::vector<
        bioseq::postgres_test::test_case_report>& cases) {
    std::unordered_set<std::string> executed_cases;
    for (const auto& test_case : cases) {
        if (test_case.section.empty()
            && test_case.status != "skipped") {
            executed_cases.insert(test_case.test_case);
        }
    }
    if (!executed_cases.contains(row.catch2_case)) {
        return false;
    }
    const bool case_has_explicit_sections = std::any_of(
        cases.begin(), cases.end(),
        [&row](const auto& test_case) {
            return test_case.test_case == row.catch2_case
                && !test_case.section.empty();
        });
    if (!case_has_explicit_sections || row.catch2_section.empty()) {
        return true;
    }
    return std::any_of(
        cases.begin(), cases.end(),
        [&row](const auto& test_case) {
            return test_case.test_case == row.catch2_case
                && test_case.section == row.catch2_section;
        });
}

[[nodiscard]] std::vector<bioseq::postgres_test::expected_migration_probe>
expected_migration_probes(
    const bioseq::cache_test::migration::ledger& ledger,
    bioseq::cache_test::migration::evidence_harness owner,
    bioseq::cache_test::migration::evidence_role role,
    const std::vector<bioseq::postgres_test::test_case_report>& cases,
    bool executed_only) {
    std::vector<bioseq::postgres_test::expected_migration_probe> expected;
    for (const auto& row : ledger.rows) {
        if (!bioseq::cache_test::migration::requires_probe(
                row, owner, role)
            || (executed_only
                && !migration_row_was_executed(row, cases))) {
            continue;
        }
        expected.push_back(
            bioseq::postgres_test::expected_migration_probe{
                .stable_id = row.stable_id,
                .expected_count =
                    bioseq::cache_test::migration::expected_probe_count(row),
                .test_case = row.catch2_case,
                .section = row.catch2_section});
    }
    return expected;
}

/// @brief Writes the native runner verdict and stable migration probes.
void write_report(
    const std::filesystem::path& report,
    const std::filesystem::path& executable,
    std::string_view executable_sha256,
    std::vector<bioseq::postgres_test::report_argument> arguments,
    const bioseq::cache_test::runner_configuration& configuration,
    bioseq::cache_test::migration::evidence_harness migration_owner,
    bioseq::cache_test::migration::evidence_role migration_role,
    std::string_view migration_coverage,
    const migration_ledger_snapshot& migration_ledger) {
    retain_attested_artifacts(catch_evidence());
    bioseq::postgres_test::cleanup_report cleanup =
        bioseq::postgres_test::aggregate_cleanup_report();
    std::string retained_supervision_states;
    const auto append_retained_state =
        [&retained_supervision_states](
            const std::filesystem::path& state) {
            if (!retained_supervision_states.empty()) {
                retained_supervision_states += "; ";
            }
            retained_supervision_states += state.string();
        };
    for (const auto& state :
         catch_evidence().retained_watchdog_states) {
        append_retained_state(state);
    }
    if (!catch_evidence().watchdog_state.empty()) {
        append_retained_state(catch_evidence().watchdog_state);
    }
    if (!retained_supervision_states.empty()) {
        cleanup.succeeded = false;
        cleanup.detail += (cleanup.detail.empty() ? "" : "; ");
        cleanup.detail +=
            "live-resource supervision state retained at "
            + retained_supervision_states;
    }
    if (catch_evidence().watchdog_protocol_failed) {
        cleanup.succeeded = false;
        cleanup.detail += (cleanup.detail.empty() ? "" : "; ");
        cleanup.detail +=
            "external watchdog did not complete validated disarm";
    }
    if (catch_evidence().watchdog_retention_recording_failed) {
        cleanup.succeeded = false;
        cleanup.detail += (cleanup.detail.empty() ? "" : "; ");
        cleanup.detail +=
            "watchdog-state retention bookkeeping failed; the active "
            "state path was preserved";
    }
    bioseq::postgres_test::integration_test_report native_report{
        .schema_version = "bioseq-cache-test-v2",
        .executable = executable.string(),
        .build_identity = std::string(executable_sha256),
        .arguments = std::move(arguments),
        .cluster_configuration = {
            {"ownership",
             "fixture-owned isolated clusters; cases may run matrices"},
            {"transport", "Unix socket only"},
            {"authentication", "local trust in private PGDATA"},
            {"retained-supervision-state",
             retained_supervision_states.empty()
                 ? "none" : retained_supervision_states}},
        .cases = catch_evidence().cases,
        .phase_durations =
            bioseq::postgres_test::phase_duration_reports(),
        .host_qualifications =
            bioseq::postgres_test::host_qualification_reports(),
        .sql_transcripts =
            bioseq::postgres_test::process_sql_transcript_entries(),
        .migration_evidence_harness = std::string(
            bioseq::cache_test::migration::harness_name(
                migration_owner)),
        .migration_report_role = std::string(
            bioseq::cache_test::migration::role_name(
                migration_role)),
        .migration_coverage = std::string(migration_coverage),
        .migration_phase = migration_ledger.phase,
        .migration_ledger_sha256 = migration_ledger.raw_sha256,
        .evidence_contract_sha256 =
            migration_ledger.evidence_contract_sha256,
        .migration_probes =
            bioseq::postgres_test::migration_probe_records(),
        .artifacts = catch_evidence().attested_artifacts,
        .cleanup_observations =
            bioseq::postgres_test::cleanup_observations(),
        .resources =
            bioseq::postgres_test::aggregate_resource_report(),
        .cleanup = cleanup};
    reconcile_migration_probes(
        native_report.migration_probes, native_report.cases);
    const auto has_artifact =
        [&native_report](std::string_view name) {
            return std::any_of(
                native_report.artifacts.begin(),
                native_report.artifacts.end(),
                [name](const bioseq::postgres_test::artifact_report& artifact) {
                    return artifact.name == name;
                });
        };
    if (!has_artifact("bioseq_cache")) {
        native_report.artifacts.push_back(make_artifact_report(
            "bioseq_cache", configuration.cache_control,
            configuration.cache_install_sql, configuration.cache_dso));
    }
    if (configuration.bioseq_control
        && configuration.bioseq_install_sql
        && configuration.bioseq_dso) {
        if (!has_artifact("bioseq")) {
            native_report.artifacts.push_back(make_artifact_report(
                "bioseq", *configuration.bioseq_control,
                *configuration.bioseq_install_sql,
                *configuration.bioseq_dso));
        }
    }
    if (!migration_ledger_unchanged(migration_ledger)) {
        throw std::runtime_error(
            "migration ledger changed before report publication");
    }
    if (!file_sha256_unchanged(
            executable, executable_sha256)) {
        throw std::runtime_error(
            "integration executable changed before report publication");
    }
    bioseq::postgres_test::write_integration_test_report(
        report, native_report);
    if (!migration_ledger_unchanged(migration_ledger)) {
        throw std::runtime_error(
            "migration ledger changed during report publication");
    }
    if (!file_sha256_unchanged(
            executable, executable_sha256)) {
        throw std::runtime_error(
            "integration executable changed during report publication");
    }
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(
    std::string_view text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool process_absent(std::uint64_t pid) noexcept {
    if (pid <= 1
        || pid > static_cast<std::uint64_t>(
            std::numeric_limits<pid_t>::max())) {
        return true;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return false;
    }
    return errno == ESRCH;
}

class stopped_parent_fail_safe {
public:
    explicit stopped_parent_fail_safe(
        const bioseq::postgres_test::supervised_process_record*
            parent) noexcept
        : parent_(parent) {}

    stopped_parent_fail_safe(const stopped_parent_fail_safe&) = delete;
    stopped_parent_fail_safe& operator=(
        const stopped_parent_fail_safe&) = delete;

    ~stopped_parent_fail_safe() noexcept {
        resolve();
    }

    void arm() noexcept {
        armed_ = parent_ != nullptr;
    }

    void resolve() noexcept {
        if (!armed_) {
            return;
        }
        armed_ = false;
        if (parent_ == nullptr
            || !bioseq::postgres_test::supervised_process_matches(
                *parent_)) {
            return;
        }
        if (::kill(static_cast<pid_t>(parent_->pid), SIGKILL) != 0
            && errno != ESRCH
            && bioseq::postgres_test::supervised_process_matches(
                *parent_)) {
            // Never strand a still-matching parent in SIGSTOP if SIGKILL
            // itself is unavailable.
            static_cast<void>(
                ::kill(static_cast<pid_t>(parent_->pid), SIGCONT));
        }
    }

private:
    const bioseq::postgres_test::supervised_process_record* parent_{};
    bool armed_{};
};

[[nodiscard]] bool marker_owned_cluster(
    const bioseq::postgres_test::supervised_cluster_record& cluster) {
    if (cluster.root.empty()
        || !cluster.root.filename().string().starts_with(
            "bioseq-pg-test-")
        || !std::filesystem::is_regular_file(
            cluster.root / ".bioseq-private-cluster")) {
        return false;
    }
    if (std::filesystem::canonical(cluster.root.parent_path())
        != std::filesystem::canonical(
            std::filesystem::temp_directory_path())) {
        return false;
    }
    return cluster.data_directory.lexically_normal()
        == (cluster.root / "data").lexically_normal();
}

[[nodiscard]] bool marker_owned_data_directory(
    const std::filesystem::path& data_directory) {
    if (data_directory.empty()) {
        return true;
    }
    return marker_owned_cluster(
        bioseq::postgres_test::supervised_cluster_record{
            .root = data_directory.parent_path(),
            .data_directory = data_directory});
}

[[nodiscard]] bool supervised_members_absent(
    const std::vector<
        bioseq::postgres_test::supervised_process_record>& members,
    const std::vector<std::int64_t>& process_groups) noexcept {
    return std::ranges::none_of(
               members,
               bioseq::postgres_test::supervised_process_matches)
        && std::ranges::all_of(
            process_groups,
            bioseq::postgres_test::supervised_process_group_absent);
}

[[nodiscard]] bool wait_for_supervised_members_absence(
    const std::vector<
        bioseq::postgres_test::supervised_process_record>& members,
    const std::vector<std::int64_t>& process_groups,
    std::chrono::steady_clock::time_point deadline) noexcept {
    while (std::chrono::steady_clock::now() < deadline) {
        if (supervised_members_absent(members, process_groups)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return supervised_members_absent(members, process_groups);
}

[[nodiscard]] bool supervised_group_anchored(
    const std::vector<
        bioseq::postgres_test::supervised_process_record>& members,
    std::int64_t process_group) noexcept {
    return process_group > 1
        && process_group != static_cast<std::int64_t>(::getpgrp())
        && std::ranges::any_of(
            members,
            [process_group](
                const bioseq::postgres_test::supervised_process_record&
                    member) {
                return member.process_group == process_group
                    && bioseq::postgres_test::
                        supervised_process_group_matches(member);
            });
}

void signal_supervised_group(
    const std::vector<
        bioseq::postgres_test::supervised_process_record>& members,
    std::int64_t process_group,
    int signal) noexcept {
    if (supervised_group_anchored(members, process_group)) {
        static_cast<void>(::kill(
            -static_cast<pid_t>(process_group), signal));
    }
}

[[nodiscard]] std::vector<std::int64_t> supervised_process_groups(
    const std::vector<
        bioseq::postgres_test::supervised_process_record>& members) {
    std::vector<std::int64_t> groups;
    for (const auto& member : members) {
        if (member.process_group > 1
            && std::ranges::find(groups, member.process_group)
                == groups.end()) {
            groups.push_back(member.process_group);
        }
    }
    return groups;
}

void signal_supervised_groups(
    const std::vector<
        bioseq::postgres_test::supervised_process_record>& members,
    const std::vector<std::int64_t>& process_groups,
    int signal) noexcept {
    for (const std::int64_t process_group : process_groups) {
        signal_supervised_group(members, process_group, signal);
    }
}

[[nodiscard]] bool stop_supervised_groups(
    const std::vector<
        bioseq::postgres_test::supervised_process_record>& members,
    const std::vector<std::int64_t>& process_groups) noexcept {
    for (const std::int64_t process_group : process_groups) {
        if (!supervised_group_anchored(members, process_group)
            || ::kill(
                   -static_cast<pid_t>(process_group), SIGSTOP)
                != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<
    bioseq::postgres_test::supervised_process_record>>
capture_stable_supervised_tree(
    const bioseq::postgres_test::supervised_process_record&
        postmaster) noexcept {
    try {
        for (int attempt = 0; attempt != 5; ++attempt) {
            if (attempt != 0) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10));
            }
            auto first =
                bioseq::postgres_test::
                    sample_postmaster_process_tree_pids(
                        postmaster.pid);
            if (!first) {
                continue;
            }
            std::ranges::sort(*first);
            first->erase(
                std::unique(first->begin(), first->end()),
                first->end());
            if (!std::ranges::binary_search(
                    *first, postmaster.pid)) {
                continue;
            }
            std::vector<
                bioseq::postgres_test::supervised_process_record>
                members;
            members.reserve(first->size());
            bool complete = true;
            for (const std::uint64_t pid : *first) {
                const auto member =
                    bioseq::postgres_test::capture_supervised_process(
                        pid,
                        pid == postmaster.pid
                            ? postmaster.executable
                            : std::filesystem::path{},
                        postmaster.data_directory);
                if (!member
                    || (pid == postmaster.pid
                        && member->start_token
                            != postmaster.start_token)) {
                    complete = false;
                    break;
                }
                members.push_back(*member);
            }
            if (!complete) {
                continue;
            }
            auto second =
                bioseq::postgres_test::
                    sample_postmaster_process_tree_pids(
                        postmaster.pid);
            if (!second) {
                continue;
            }
            std::ranges::sort(*second);
            second->erase(
                std::unique(second->begin(), second->end()),
                second->end());
            if (*first == *second) {
                return members;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

[[nodiscard]] bool supervised_process_stopped(
    const bioseq::postgres_test::supervised_process_record&
        process) noexcept {
    try {
        if (!bioseq::postgres_test::supervised_process_matches(process)) {
            return false;
        }
#if defined(__linux__)
        std::ifstream input(
            std::filesystem::path("/proc")
            / std::to_string(process.pid) / "stat");
        std::string line;
        if (!input || !std::getline(input, line)) {
            return false;
        }
        const std::size_t command_end = line.rfind(')');
        if (command_end == std::string::npos
            || command_end + 2U >= line.size()) {
            return false;
        }
        const char state = line[command_end + 2U];
        return state == 'T' || state == 't';
#elif defined(__APPLE__)
        proc_bsdinfo information{};
        return ::proc_pidinfo(
                   static_cast<int>(process.pid),
                   PROC_PIDTBSDINFO, 0, &information,
                   sizeof(information))
                == static_cast<int>(sizeof(information))
            && information.pbi_status == SSTOP;
#else
        return false;
#endif
    } catch (...) {
        return false;
    }
}

void resume_supervised_processes(
    const std::vector<
        bioseq::postgres_test::supervised_process_record>&
        processes) noexcept {
    for (const auto& process : processes) {
        if (bioseq::postgres_test::supervised_process_matches(process)) {
            static_cast<void>(
                ::kill(static_cast<pid_t>(process.pid), SIGCONT));
        }
    }
}

[[nodiscard]] std::optional<std::vector<
    bioseq::postgres_test::supervised_process_record>>
freeze_stable_supervised_tree(
    const bioseq::postgres_test::supervised_process_record&
        postmaster) noexcept {
    std::vector<bioseq::postgres_test::supervised_process_record>
        stopped;
    std::vector<std::int64_t> stopped_groups;
    try {
        if (!bioseq::postgres_test::supervised_process_group_matches(
                postmaster)) {
            return std::nullopt;
        }
        stopped.push_back(postmaster);
        stopped_groups.push_back(postmaster.process_group);
        if (!stop_supervised_groups(stopped, stopped_groups)) {
            signal_supervised_groups(
                stopped, stopped_groups, SIGCONT);
            resume_supervised_processes(stopped);
            return std::nullopt;
        }
        for (int attempt = 0; attempt != 50; ++attempt) {
            const auto tree =
                capture_stable_supervised_tree(postmaster);
            if (!tree) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(20));
                continue;
            }
            bool signaled = true;
            for (const auto& member : *tree) {
                if (std::ranges::none_of(
                        stopped,
                        [&member](const auto& known) {
                            return known.pid == member.pid
                                && known.start_token
                                    == member.start_token;
                        })) {
                    stopped.push_back(member);
                }
                if (std::ranges::find(
                        stopped_groups, member.process_group)
                    == stopped_groups.end()) {
                    stopped_groups.push_back(member.process_group);
                }
            }
            if (!stop_supervised_groups(stopped, stopped_groups)) {
                signal_supervised_groups(
                    stopped, stopped_groups, SIGCONT);
                resume_supervised_processes(stopped);
                return std::nullopt;
            }
            for (const auto& member : *tree) {
                if (!bioseq::postgres_test::
                        supervised_process_matches(member)
                    || ::kill(
                           static_cast<pid_t>(member.pid),
                           SIGSTOP)
                        != 0) {
                    signaled = false;
                    break;
                }
            }
            if (!signaled) {
                signal_supervised_groups(
                    stopped, stopped_groups, SIGCONT);
                resume_supervised_processes(stopped);
                return std::nullopt;
            }
            std::this_thread::sleep_for(
                std::chrono::milliseconds(20));
            const auto confirmation =
                capture_stable_supervised_tree(postmaster);
            if (!confirmation
                || confirmation->size() != tree->size()) {
                continue;
            }
            bool fixed_point = true;
            for (std::size_t index = 0;
                 index < tree->size(); ++index) {
                if ((*tree)[index].pid
                        != (*confirmation)[index].pid
                    || (*tree)[index].start_token
                        != (*confirmation)[index].start_token
                    || !supervised_process_stopped(
                        (*confirmation)[index])) {
                    fixed_point = false;
                    break;
                }
            }
            if (fixed_point) {
                fixed_point = std::ranges::all_of(
                    stopped,
                    [](const auto& member) {
                        return !bioseq::postgres_test::
                                   supervised_process_matches(member)
                            || supervised_process_stopped(member);
                    });
            }
            if (fixed_point) {
                return stopped;
            }
        }
    } catch (...) {
    }
    signal_supervised_groups(
        stopped, stopped_groups, SIGCONT);
    resume_supervised_processes(stopped);
    return std::nullopt;
}

[[nodiscard]] bioseq::postgres_test::cleanup_observation
watchdog_cleanup_cluster(
    const bioseq::postgres_test::supervised_cluster_record& cluster,
    std::uint64_t ordinal,
    bool allow_root_removal) {
    bioseq::postgres_test::cleanup_observation observation{
        .ordinal = ordinal,
        .root = cluster.root};
    try {
        if (!marker_owned_cluster(cluster)) {
            observation.detail =
                "watchdog refused an unproven private-cluster root";
            return observation;
        }
        bioseq::postgres_test::supervised_cluster_record active = cluster;
        if (active.discover_postmaster_pid
            && active.postmaster_pid <= 1) {
            std::ifstream pid_input(
                active.data_directory / "postmaster.pid");
            std::uint64_t pid_from_data{};
            pid_input >> pid_from_data;
            if (pid_input && pid_from_data > 1) {
                active.postmaster_pid = pid_from_data;
            }
        }
        if (active.postmaster_pid == 1) {
            active.postmaster_pid = 0;
        }
        if (active.discover_postmaster_pid
            && active.postmaster_pid <= 1) {
            observation.detail =
                "watchdog retained PGDATA because launch-pending "
                "postmaster identity recovery was inconclusive";
            return observation;
        }
        if (active.postmaster_pid > 1
            && process_absent(active.postmaster_pid)) {
            if (active.postmaster_process_group > 1
                && !bioseq::postgres_test::
                        supervised_process_group_absent(
                            active.postmaster_process_group)) {
                observation.detail =
                    "watchdog refused to signal a surviving postmaster "
                    "group after its recorded identity disappeared";
            } else {
                observation.detail =
                    "watchdog retained PGDATA because the postmaster "
                    "identity disappeared before complete-tree proof";
            }
            return observation;
        }
        std::vector<bioseq::postgres_test::supervised_process_record>
            process_tree;
        if (active.postmaster_pid > 1
            && !process_absent(active.postmaster_pid)) {
            std::optional<bioseq::postgres_test::supervised_process_record>
                postmaster;
            if (!active.postmaster_start_token.empty()
                && !active.postmaster_executable.empty()
                && active.postmaster_process_group > 1) {
                bioseq::postgres_test::supervised_process_record recorded{
                    .pid = active.postmaster_pid,
                    .start_token = active.postmaster_start_token,
                    .executable = active.postmaster_executable,
                    .process_group =
                        active.postmaster_process_group,
                    .data_directory = active.data_directory};
                if (bioseq::postgres_test::supervised_process_matches(
                        recorded)) {
                    postmaster = std::move(recorded);
                }
            } else if (active.discover_postmaster_pid) {
                const auto discovered =
                    bioseq::postgres_test::capture_supervised_process(
                        active.postmaster_pid,
                        active.postmaster_executable,
                        active.data_directory);
                if (discovered
                    && bioseq::postgres_test::
                        supervised_postmaster_matches_data_directory(
                            *discovered)) {
                    postmaster = *discovered;
                }
            }
            if (!postmaster) {
                observation.detail =
                    "watchdog refused a postmaster without a matching "
                    "journaled identity or exact live PGDATA binding";
                return observation;
            }
            auto stable_tree =
                freeze_stable_supervised_tree(*postmaster);
            if (!stable_tree) {
                observation.detail =
                    "watchdog could not freeze a stable complete "
                    "postmaster descendant tree; all retained identities "
                    "were resumed";
                return observation;
            }
            process_tree = std::move(*stable_tree);
            const auto current_postmaster = std::ranges::find_if(
                process_tree,
                [pid = active.postmaster_pid](const auto& member) {
                    return member.pid == pid;
                });
            if (current_postmaster == process_tree.end()) {
                observation.detail =
                    "watchdog stable tree omitted the postmaster";
                return observation;
            }
            const std::vector<std::int64_t> process_groups =
                supervised_process_groups(process_tree);
            signal_supervised_groups(
                process_tree, process_groups, SIGKILL);
            for (auto iterator = process_tree.rbegin();
                 iterator != process_tree.rend(); ++iterator) {
                if (bioseq::postgres_test::
                        supervised_process_matches(*iterator)) {
                    static_cast<void>(::kill(
                        static_cast<pid_t>(iterator->pid), SIGKILL));
                }
            }
            const auto force_deadline =
                std::chrono::steady_clock::now()
                + std::chrono::seconds(5);
            if (!wait_for_supervised_members_absence(
                    process_tree, process_groups,
                    force_deadline)) {
                resume_supervised_processes(process_tree);
                observation.detail =
                    "watchdog could not stop every validated postmaster "
                    "process-tree member; surviving identities were resumed";
                return observation;
            }
        }
        if (!allow_root_removal) {
            observation.detail =
                "watchdog retained the private-cluster root because "
                "registered lifecycle-process cleanup was incomplete";
            return observation;
        }
        std::error_code error;
        std::filesystem::remove_all(cluster.root, error);
        if (error) {
            observation.detail =
                "watchdog could not remove private root: "
                + error.message();
            return observation;
        }
        observation.succeeded = true;
        observation.detail =
            "external watchdog stopped the process tree and removed "
            "the marker-owned root";
    } catch (const std::exception& error) {
        observation.detail = error.what();
    } catch (...) {
        observation.detail =
            "unknown external-watchdog cleanup failure";
    }
    return observation;
}

[[nodiscard]] bool watchdog_cleanup_process(
    const bioseq::postgres_test::supervised_process_record&
        process) {
    if (!marker_owned_data_directory(process.data_directory)) {
        return false;
    }
    if (!bioseq::postgres_test::supervised_process_matches(process)) {
        return process_absent(process.pid)
            && bioseq::postgres_test::supervised_process_group_absent(
                process.process_group);
    }
    auto frozen = freeze_stable_supervised_tree(process);
    if (!frozen) {
        return false;
    }
    const auto& members = *frozen;
    const std::vector<std::int64_t> process_groups =
        supervised_process_groups(members);
    signal_supervised_groups(
        members, process_groups, SIGKILL);
    for (auto iterator = members.rbegin();
         iterator != members.rend(); ++iterator) {
        if (bioseq::postgres_test::supervised_process_matches(
                *iterator)) {
            static_cast<void>(::kill(
                static_cast<pid_t>(iterator->pid), SIGKILL));
        }
    }
    const auto force_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const bool absent = wait_for_supervised_members_absence(
        members, process_groups, force_deadline);
    if (!absent) {
        resume_supervised_processes(members);
    }
    return absent;
}

[[nodiscard]] int run_internal_watchdog(
    const std::filesystem::path& state_directory,
    const std::filesystem::path& report_path,
    std::string test_case,
    std::uint64_t parent_pid,
    std::chrono::seconds timeout,
    const std::filesystem::path& executable,
    std::string migration_evidence_harness,
    std::string migration_report_role,
    std::string migration_phase,
    std::string migration_ledger_sha256,
    std::string evidence_contract_sha256,
    const std::filesystem::path& migration_ledger,
    std::string executable_sha256) {
    const bool parent_was_already_gone =
        ::getppid() != static_cast<pid_t>(parent_pid);
    const auto parent_identity = parent_was_already_gone
        ? std::optional<
              bioseq::postgres_test::supervised_process_record>{}
        : bioseq::postgres_test::capture_supervised_process(
              parent_pid, executable);
    if (!parent_identity && !parent_was_already_gone) {
        return 125;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + timeout;
    if (!bioseq::postgres_test::supervision_registries_empty(
            state_directory)
        || !publish_watchdog_armed(state_directory, parent_pid)) {
        return 125;
    }
    bool parent_disappeared = parent_was_already_gone;
    while (std::chrono::steady_clock::now() < deadline) {
        if (bioseq::postgres_test::supervision_decision_claimed(
                state_directory)) {
            return 0;
        }
        if (!parent_identity
            || !bioseq::postgres_test::supervised_process_matches(
                *parent_identity)
            || ::getppid() != static_cast<pid_t>(parent_pid)) {
            parent_disappeared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!bioseq::postgres_test::try_claim_supervision_decision(
            state_directory, "timeout")) {
        return 0;
    }
    if (!parent_disappeared && parent_identity
        && !bioseq::postgres_test::supervised_process_matches(
            *parent_identity)) {
        parent_disappeared = true;
    }
    stopped_parent_fail_safe parent_fail_safe{
        parent_identity ? &*parent_identity : nullptr};
    if (!parent_disappeared
        && ::kill(static_cast<pid_t>(parent_pid), SIGSTOP) != 0) {
        if (errno == ESRCH) {
            parent_disappeared = true;
        } else {
            return 125;
        }
    } else if (!parent_disappeared) {
        parent_fail_safe.arm();
    }

    const auto live_registries =
        bioseq::postgres_test::load_supervision_registries(
            state_directory);
    const std::vector<
        bioseq::postgres_test::supervised_cluster_record> clusters =
        live_registries
        ? live_registries->clusters
        : std::vector<
              bioseq::postgres_test::supervised_cluster_record>{};
    bioseq::postgres_test::resource_report resources;
    resources.postgres =
        bioseq::postgres_test::load_supervision_resource_report(
            state_directory);
    for (const auto& cluster : clusters) {
        std::uint64_t postmaster_pid = cluster.postmaster_pid;
        if (cluster.discover_postmaster_pid && postmaster_pid <= 1) {
            std::ifstream input(
                cluster.data_directory / "postmaster.pid");
            input >> postmaster_pid;
        }
        if (postmaster_pid > 1) {
            if (const auto rss =
                    bioseq::postgres_test::
                        sample_postmaster_process_tree_rss(
                            postmaster_pid)) {
                ++resources.postgres.rss_samples;
                resources.postgres
                    .peak_postmaster_process_tree_rss_bytes =
                    std::max(
                        resources.postgres
                            .peak_postmaster_process_tree_rss_bytes,
                        *rss);
            } else {
                ++resources.postgres.rss_sampling_failures;
            }
        }
        if (const auto root_bytes =
                bioseq::postgres_test::sample_owned_root_bytes(
                    cluster.root)) {
            ++resources.postgres.owned_root_samples;
            resources.postgres.peak_owned_root_bytes =
                std::max(
                    resources.postgres.peak_owned_root_bytes,
                    *root_bytes);
        } else {
            ++resources.postgres.owned_root_sampling_failures;
        }
    }
    resources.postgres.phase_boundaries.push_back(
        bioseq::postgres_test::resource_phase_boundary{
            .ordinal =
                resources.postgres.phase_boundaries.size(),
            .phase = parent_disappeared
                ? "runner-terminated" : "watchdog-timeout",
            .elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started),
            .postmaster_process_tree_rss_bytes =
                resources.postgres
                        .peak_postmaster_process_tree_rss_bytes == 0
                ? std::optional<std::uint64_t>{}
                : std::optional<std::uint64_t>{
                    resources.postgres
                        .peak_postmaster_process_tree_rss_bytes},
            .owned_root_bytes =
                resources.postgres.peak_owned_root_bytes == 0
                ? std::optional<std::uint64_t>{}
                : std::optional<std::uint64_t>{
                    resources.postgres.peak_owned_root_bytes}});
    resources.peak_rss_bytes =
        resources.postgres.peak_postmaster_process_tree_rss_bytes;
    resources.observed_temp_bytes =
        resources.postgres.peak_owned_root_bytes;

    std::string evidence_recovery_detail;
    const auto note_recovery_failure =
        [&evidence_recovery_detail](std::string_view kind) {
            if (!evidence_recovery_detail.empty()) {
                evidence_recovery_detail += ", ";
            }
            evidence_recovery_detail += kind;
        };
    if (!live_registries) {
        note_recovery_failure("live-resource registries");
    }
    auto recovered_cases =
        bioseq::postgres_test::load_supervision_test_case_reports(
            state_directory);
    if (!recovered_cases) {
        note_recovery_failure("cases");
    }
    auto recovered_phases =
        bioseq::postgres_test::
            load_supervision_phase_duration_reports(state_directory);
    if (!recovered_phases) {
        note_recovery_failure("phases");
    }
    auto recovered_qualifications =
        bioseq::postgres_test::
            load_supervision_host_qualification_reports(state_directory);
    if (!recovered_qualifications) {
        note_recovery_failure("qualifications");
    }
    auto recovered_probes =
        bioseq::postgres_test::
            load_supervision_migration_probe_records(state_directory);
    if (!recovered_probes) {
        note_recovery_failure("migration probes");
    }
    auto recovered_artifacts =
        bioseq::postgres_test::load_supervision_artifact_reports(
            state_directory);
    if (!recovered_artifacts) {
        note_recovery_failure("artifacts");
    }
    try {
        if (bioseq::postgres_test::sha256_file(
                migration_ledger).hex()
            != migration_ledger_sha256) {
            note_recovery_failure(
                "migration ledger changed after runner startup");
        }
    } catch (...) {
        note_recovery_failure(
            "migration ledger cannot be verified at watchdog exit");
    }
    if (!file_sha256_unchanged(
            executable, executable_sha256)) {
        note_recovery_failure(
            "integration executable path changed after runner startup");
    }

    std::vector<bioseq::postgres_test::test_case_report> cases =
        recovered_cases.value_or(
            std::vector<bioseq::postgres_test::test_case_report>{});
    std::erase_if(
        cases,
        [&test_case](const bioseq::postgres_test::test_case_report& report) {
            return report.test_case == test_case
                && report.section.empty();
        });
    std::vector<bioseq::postgres_test::migration_probe_record> probes =
        recovered_probes.value_or(
            std::vector<
                bioseq::postgres_test::migration_probe_record>{});
    for (auto& probe : probes) {
        if (probe.test_case == test_case) {
            probe.passed = false;
        }
    }
    std::string interruption_message = parent_disappeared
        ? "external watchdog observed an unexpected runner termination"
        : "external watchdog exceeded the per-case timeout of "
            + std::to_string(timeout.count()) + " seconds";
    if (!evidence_recovery_detail.empty()) {
        interruption_message +=
            "; missing or invalid evidence snapshot(s): "
            + evidence_recovery_detail;
    }
    cases.push_back(bioseq::postgres_test::test_case_report{
        .test_case = test_case,
        .status = "infrastructure-inconclusive",
        .message = std::move(interruption_message),
        .duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)});

    const std::vector<
        bioseq::postgres_test::supervised_process_record> processes =
        live_registries
        ? live_registries->processes
        : std::vector<
              bioseq::postgres_test::supervised_process_record>{};
    bool lifecycle_cleanup_succeeded = live_registries.has_value();
    for (const auto& process : processes) {
        lifecycle_cleanup_succeeded =
            watchdog_cleanup_process(process)
            && lifecycle_cleanup_succeeded;
    }

    std::vector<bioseq::postgres_test::cleanup_observation>
        cleanup_observations;
    cleanup_observations.reserve(clusters.size());
    bool cleanup_succeeded = lifecycle_cleanup_succeeded;
    for (std::size_t index = 0; index < clusters.size(); ++index) {
        auto observation = watchdog_cleanup_cluster(
            clusters[index], index, lifecycle_cleanup_succeeded);
        observation.test_case = test_case;
        if (index == 0) {
            observation.resources = resources;
        }
        cleanup_succeeded =
            cleanup_succeeded && observation.succeeded;
        cleanup_observations.push_back(std::move(observation));
    }

    bioseq::postgres_test::integration_test_report emergency{
        .schema_version = "bioseq-cache-test-v2",
        .executable = executable.string(),
        .build_identity = executable_sha256,
        .cluster_configuration = {
            {"supervision", "out-of-process per-case watchdog"},
            {"termination", "bounded process-tree cleanup"},
            {"evidence-recovery",
             evidence_recovery_detail.empty()
                 ? "all typed snapshots recovered"
                 : "missing or invalid: " + evidence_recovery_detail},
            {"retained-supervision-state",
             cleanup_succeeded
                 ? "none"
                 : state_directory.string()}},
        .cases = std::move(cases),
        .phase_durations =
            recovered_phases.value_or(
                std::vector<
                    bioseq::postgres_test::phase_duration_report>{}),
        .host_qualifications =
            recovered_qualifications.value_or(
                std::vector<
                    bioseq::postgres_test::host_qualification_report>{}),
        .sql_transcripts =
            bioseq::postgres_test::load_supervision_sql_transcript(
                state_directory),
        .migration_evidence_harness =
            migration_evidence_harness,
        .migration_report_role = migration_report_role,
        .migration_coverage = "executed-only",
        .migration_phase = migration_phase,
        .migration_ledger_sha256 =
            migration_ledger_sha256,
        .evidence_contract_sha256 =
            evidence_contract_sha256,
        .migration_probes = std::move(probes),
        .artifacts =
            recovered_artifacts.value_or(
                std::vector<
                    bioseq::postgres_test::artifact_report>{}),
        .cleanup_observations = std::move(cleanup_observations),
        .resources = std::move(resources),
        .cleanup = bioseq::postgres_test::cleanup_report{
            .succeeded = cleanup_succeeded,
            .detail = cleanup_succeeded
                ? (clusters.empty()
                    ? "watchdog observed no registered private clusters"
                    : "watchdog cleaned every registered private cluster")
                : "watchdog cleanup was incomplete"}};
    try {
        bioseq::postgres_test::write_integration_test_report(
            report_path, emergency);
        if (bioseq::postgres_test::sha256_file(
                migration_ledger).hex()
            != migration_ledger_sha256) {
            parent_fail_safe.resolve();
            return 126;
        }
        if (!file_sha256_unchanged(
                executable, executable_sha256)) {
            parent_fail_safe.resolve();
            return 126;
        }
    } catch (...) {
        parent_fail_safe.resolve();
        return 126;
    }
    if (cleanup_succeeded) {
        static_cast<void>(
            bioseq::postgres_test::remove_supervision_state(
                state_directory));
    }
    parent_fail_safe.resolve();
    return 124;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 14
        && std::string_view(argv[1])
            == "--bioseq-internal-watchdog") {
        const auto parent_pid = parse_unsigned(argv[5]);
        const auto timeout = parse_unsigned(argv[6]);
        if (!parent_pid || *parent_pid <= 1 || !timeout
            || *timeout == 0
            || *parent_pid
                > static_cast<std::uint64_t>(
                    std::numeric_limits<pid_t>::max())
            || *timeout
                > static_cast<std::uint64_t>(
                    std::chrono::seconds::max().count())) {
            return 2;
        }
        try {
            return run_internal_watchdog(
                argv[2], argv[3], argv[4], *parent_pid,
                std::chrono::seconds(
                    static_cast<std::chrono::seconds::rep>(*timeout)),
                current_executable_path(argv[0]),
                argv[7], argv[8], argv[9], argv[10], argv[11],
                argv[12], argv[13]);
        } catch (...) {
            return 126;
        }
    }
    Catch::Session session;

    std::string postgres_bin_dir;
    std::string postgres_stage_root;
    std::string postgres_stage_manifest;
    std::string bioseq_only_postgres_bin_dir;
    std::string bioseq_only_postgres_stage_root;
    std::string bioseq_only_postgres_stage_manifest;
    std::string cache_control;
    std::string cache_install_sql;
    std::string cache_dso;
    std::string bioseq_control;
    std::string bioseq_install_sql;
    std::string bioseq_dso;
    std::string provider_peer_test_dso;
    std::string migration_ledger;
    std::string migration_harness;
    std::string migration_report_role;
    std::string report;
    int case_timeout_seconds = 480;
    int large_case_timeout_seconds = 6300;

    auto cli = session.cli()
        | Catch::Clara::Opt(postgres_bin_dir, "PATH")
              ["--postgres-bin-dir"]("PostgreSQL executable directory")
        | Catch::Clara::Opt(postgres_stage_root, "PATH")
              ["--postgres-stage-root"](
                  "preinstalled PostgreSQL stage root")
        | Catch::Clara::Opt(postgres_stage_manifest, "PATH")
              ["--postgres-stage-manifest"](
                  "preinstalled PostgreSQL stage manifest")
        | Catch::Clara::Opt(bioseq_only_postgres_bin_dir, "PATH")
              ["--bioseq-only-postgres-bin-dir"](
                  "Bioseq-only PostgreSQL executable directory")
        | Catch::Clara::Opt(bioseq_only_postgres_stage_root, "PATH")
              ["--bioseq-only-postgres-stage-root"](
                  "cache-free Bioseq-only PostgreSQL stage root")
        | Catch::Clara::Opt(
              bioseq_only_postgres_stage_manifest, "PATH")
              ["--bioseq-only-postgres-stage-manifest"](
                  "cache-free Bioseq-only PostgreSQL stage manifest")
        | Catch::Clara::Opt(cache_control, "PATH")
              ["--cache-control"]("bioseq_cache control file")
        | Catch::Clara::Opt(cache_install_sql, "PATH")
              ["--cache-install-sql"]("bioseq_cache generated install SQL")
        | Catch::Clara::Opt(cache_dso, "PATH")
              ["--cache-dso"]("bioseq_cache DSO for the selected profile")
        | Catch::Clara::Opt(bioseq_control, "PATH")
              ["--bioseq-control"]("optional Bioseq control file")
        | Catch::Clara::Opt(bioseq_install_sql, "PATH")
              ["--bioseq-install-sql"]("optional Bioseq generated install SQL")
        | Catch::Clara::Opt(bioseq_dso, "PATH")
              ["--bioseq-dso"]("optional Bioseq DSO")
        | Catch::Clara::Opt(provider_peer_test_dso, "PATH")
              ["--provider-peer-test-dso"](
                  "optional incompatible provider-peer test DSO")
        | Catch::Clara::Opt(migration_ledger, "PATH")
              ["--migration-ledger"]("legacy migration ledger TSV")
        | Catch::Clara::Opt(migration_harness, "NAME")
              ["--migration-harness"](
                  "migration evidence owner for this executable")
        | Catch::Clara::Opt(migration_report_role, "NAME")
              ["--migration-report-role"](
                  "unique role of this migration evidence report")
        | Catch::Clara::Opt(report, "PATH")
              ["--report"]("native JSON report path")
        | Catch::Clara::Opt(case_timeout_seconds, "SECONDS")
              ["--case-timeout-seconds"]("ordinary per-case watchdog")
        | Catch::Clara::Opt(large_case_timeout_seconds, "SECONDS")
              ["--large-case-timeout-seconds"](
                  "watchdog for literal [2gbp] cases");
    session.cli(cli);

    const int cli_result = session.applyCommandLine(argc, argv);
    if (cli_result != 0) {
        return cli_result;
    }
    const Catch::ConfigData& catch_config = session.configData();
    const bool nonexecuting_action =
        catch_config.listTests || catch_config.listTags
        || catch_config.listReporters || catch_config.listListeners
        || catch_config.showHelp || catch_config.libIdentify;
    if (nonexecuting_action) {
        return session.run();
    }
    if (!report.empty()) {
        try {
            clear_stale_report_output(report);
        } catch (...) {
            return 3;
        }
    }
    if (postgres_bin_dir.empty() || postgres_stage_root.empty()
        || postgres_stage_manifest.empty() || cache_control.empty()
        || cache_install_sql.empty() || cache_dso.empty()
        || migration_ledger.empty()
        || migration_harness.empty()
        || migration_report_role.empty()
        || report.empty()
        || case_timeout_seconds <= 0
        || large_case_timeout_seconds <= 0) {
        return 2;
    }
    const bool any_bioseq =
        !bioseq_control.empty() || !bioseq_install_sql.empty()
        || !bioseq_dso.empty();
    const bool all_bioseq =
        !bioseq_control.empty() && !bioseq_install_sql.empty()
        && !bioseq_dso.empty();
    if (any_bioseq != all_bioseq) {
        return 2;
    }
    const bool any_bioseq_only_stage =
        !bioseq_only_postgres_bin_dir.empty()
        || !bioseq_only_postgres_stage_root.empty()
        || !bioseq_only_postgres_stage_manifest.empty();
    const bool all_bioseq_only_stage =
        !bioseq_only_postgres_bin_dir.empty()
        && !bioseq_only_postgres_stage_root.empty()
        && !bioseq_only_postgres_stage_manifest.empty();
    if (any_bioseq_only_stage != all_bioseq_only_stage
        || (all_bioseq_only_stage && !all_bioseq)) {
        return 2;
    }

    std::optional<
        bioseq::cache_test::migration::evidence_harness>
        migration_owner;
    std::optional<
        bioseq::cache_test::migration::evidence_role>
        migration_role;
    std::optional<migration_ledger_snapshot> migration_snapshot;
    std::filesystem::path executed_executable;
    std::string executed_executable_sha256;
    try {
        executed_executable = current_executable_path(argv[0]);
        executed_executable_sha256 =
            bioseq::postgres_test::sha256_file(
                executed_executable).hex();
        migration_owner =
            bioseq::cache_test::migration::parse_evidence_harness(
                migration_harness);
        migration_role =
            bioseq::cache_test::migration::parse_evidence_role(
                migration_report_role);
        if (bioseq::cache_test::migration::role_harness(
                *migration_role)
                != *migration_owner
            || !executable_allows_migration_role(
                executed_executable,
                *migration_owner, *migration_role)) {
            return 2;
        }
        migration_snapshot =
            load_migration_ledger_snapshot(migration_ledger);
    } catch (...) {
        return 3;
    }

    bioseq::cache_test::runner_configuration configuration{
        .postgres_bin_dir = postgres_bin_dir,
        .postgres_stage_root = postgres_stage_root,
        .postgres_stage_manifest = postgres_stage_manifest,
        .cache_control = cache_control,
        .cache_install_sql = cache_install_sql,
        .cache_dso = cache_dso,
        .report = report,
        .case_timeout = std::chrono::seconds(case_timeout_seconds),
        .large_case_timeout =
            std::chrono::seconds(large_case_timeout_seconds)};
    if (all_bioseq) {
        configuration.bioseq_control = bioseq_control;
        configuration.bioseq_install_sql = bioseq_install_sql;
        configuration.bioseq_dso = bioseq_dso;
    }
    if (all_bioseq_only_stage) {
        configuration.bioseq_only_postgres_bin_dir =
            bioseq_only_postgres_bin_dir;
        configuration.bioseq_only_postgres_stage_root =
            bioseq_only_postgres_stage_root;
        configuration.bioseq_only_postgres_stage_manifest =
            bioseq_only_postgres_stage_manifest;
    }
    if (!provider_peer_test_dso.empty()) {
        if (!all_bioseq) {
            return 2;
        }
        configuration.provider_peer_test_dso =
            provider_peer_test_dso;
    }
    bioseq::cache_test::set_runner_configuration(std::move(configuration));
    watchdog_launch() = watchdog_launch_configuration{
        .executable = executed_executable,
        .executable_sha256 = executed_executable_sha256,
        .report = std::filesystem::absolute(report),
        .migration_ledger =
            std::filesystem::absolute(migration_ledger),
        .migration_evidence_harness = std::string(
            bioseq::cache_test::migration::harness_name(
                *migration_owner)),
        .migration_report_role = std::string(
            bioseq::cache_test::migration::role_name(
                *migration_role)),
        .migration_phase = migration_snapshot->phase,
        .migration_ledger_sha256 =
            migration_snapshot->raw_sha256,
        .evidence_contract_sha256 =
            migration_snapshot->evidence_contract_sha256};
    bioseq::postgres_test::clear_migration_probes();
    bioseq::postgres_test::clear_process_sql_transcript();
    bioseq::postgres_test::clear_cleanup_observations();
    bioseq::postgres_test::clear_artifact_stage_reports();
    bioseq::postgres_test::clear_phase_duration_reports();
    bioseq::postgres_test::clear_host_qualification_reports();
    bioseq::postgres_test::clear_report_case_context();
    bioseq::postgres_test::set_automatic_report_collection_enabled(true);

    std::vector<bioseq::postgres_test::report_argument> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(bioseq::postgres_test::report_argument{
            .name = "argv[" + std::to_string(index) + "]",
            .value = argv[index],
            .sensitive = false});
    }

    const int catch_result = session.run();
    bioseq::postgres_test::set_automatic_report_collection_enabled(false);
    bool migration_valid = true;
    std::string migration_validation_message;
    std::string migration_coverage{"executed-only"};
    try {
        const auto expected_full_role = expected_migration_probes(
            migration_snapshot->parsed, *migration_owner,
            *migration_role, catch_evidence().cases, false);
        const auto expected_executed = expected_migration_probes(
            migration_snapshot->parsed, *migration_owner,
            *migration_role, catch_evidence().cases, true);
        const bool full_role =
            expected_executed.size() == expected_full_role.size();
        migration_coverage =
            full_role ? "full-role" : "executed-only";
        const auto& expected =
            full_role ? expected_full_role : expected_executed;
        bioseq::postgres_test::validate_migration_probe_records(
            bioseq::postgres_test::migration_probe_records(), expected);
        if (!migration_ledger_unchanged(*migration_snapshot)) {
            throw std::runtime_error(
                "migration ledger changed after runner startup");
        }
        migration_validation_message =
            std::to_string(expected.size())
            + " migration-ledger rows owned by report role "
            + std::string(
                bioseq::cache_test::migration::role_name(
                    *migration_role))
            + " validated with " + migration_coverage + " coverage";
    } catch (const std::exception& error) {
        migration_valid = false;
        migration_validation_message = error.what();
    } catch (...) {
        migration_valid = false;
        migration_validation_message =
            "unknown migration-ledger validation failure";
    }
    catch_evidence().cases.push_back(
        bioseq::postgres_test::test_case_report{
            .test_case = "migration ledger validation",
            .status = migration_valid ? "passed" : "failed",
            .message = migration_validation_message});
    const bool cleanup_succeeded =
        bioseq::postgres_test::aggregate_cleanup_report().succeeded
        && catch_evidence().retained_watchdog_states.empty()
        && catch_evidence().watchdog_state.empty()
        && !catch_evidence().watchdog_protocol_failed
        && !catch_evidence().watchdog_retention_recording_failed;
    try {
        write_report(
            report, executed_executable,
            executed_executable_sha256, std::move(arguments),
            bioseq::cache_test::configuration(), *migration_owner,
            *migration_role, migration_coverage,
            *migration_snapshot);
    } catch (...) {
        return catch_result == 0 ? 3 : catch_result;
    }
    if (catch_result != 0) {
        return catch_result;
    }
    if (!migration_valid) {
        return 5;
    }
    return cleanup_succeeded ? 0 : 4;
}
