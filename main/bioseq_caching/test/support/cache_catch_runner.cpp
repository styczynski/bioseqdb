#include <catch2/catch_clara.hpp>
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "legacy_migration_ledger.hpp"

#include <signal.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
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
    std::string current_case;
    std::vector<std::string> section_stack;
    std::vector<std::size_t> section_failure_offsets;
    std::string failure_message;
    std::chrono::steady_clock::time_point case_started{};
    std::optional<bioseq::postgres_test::spawned_process> watchdog;
    std::filesystem::path watchdog_state;
};

[[nodiscard]] catch_report_evidence& catch_evidence() {
    static catch_report_evidence evidence;
    return evidence;
}

struct watchdog_launch_configuration {
    std::filesystem::path executable;
    std::filesystem::path report;
};

[[nodiscard]] std::optional<watchdog_launch_configuration>&
watchdog_launch() {
    static std::optional<watchdog_launch_configuration> configuration;
    return configuration;
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

void stop_watchdog(catch_report_evidence& evidence) noexcept {
    evidence.watchdog.reset();
    bioseq::postgres_test::disable_supervision_state();
    if (!evidence.watchdog_state.empty()) {
        static_cast<void>(
            bioseq::postgres_test::remove_supervision_state(
                evidence.watchdog_state));
        evidence.watchdog_state.clear();
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
        stop_watchdog(catch_evidence());
        catch_evidence() = {};
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
        const std::chrono::seconds timeout =
            bioseq::cache_test::configuration().case_timeout;
        if (!watchdog_launch()) {
            throw std::runtime_error(
                "external watchdog launch configuration is missing");
        }
        evidence.watchdog_state = make_watchdog_state();
        try {
            bioseq::postgres_test::configure_supervision_state(
                evidence.watchdog_state);
            bioseq::postgres_test::
                initialize_supervision_report_journals();
            bioseq::postgres_test::
                initialize_supervision_resource_journal();
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
                            std::to_string(timeout.count())},
                        .output_file =
                            evidence.watchdog_state / "watchdog.log"}));
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
        stop_watchdog(evidence);
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

[[nodiscard]] std::vector<bioseq::postgres_test::expected_migration_probe>
expected_migration_probes(
    const std::filesystem::path& ledger,
    const std::vector<bioseq::postgres_test::test_case_report>& cases) {
    std::unordered_set<std::string> executed_cases;
    for (const auto& test_case : cases) {
        if (test_case.section.empty()
            && test_case.status != "skipped") {
            executed_cases.insert(test_case.test_case);
        }
    }

    const bioseq::cache_test::migration::ledger parsed =
        bioseq::cache_test::migration::load_ledger(ledger);
    std::vector<bioseq::postgres_test::expected_migration_probe> expected;
    for (const auto& row : parsed.rows) {
        if (row.migration_status
                == bioseq::cache_test::migration::status::retired_with_proof
            || !executed_cases.contains(row.catch2_case)) {
            continue;
        }
        const bool case_has_explicit_sections = std::any_of(
            cases.begin(), cases.end(),
            [&row](const auto& test_case) {
                return test_case.test_case == row.catch2_case
                    && !test_case.section.empty();
            });
        const bool ledger_section_executed = std::any_of(
            cases.begin(), cases.end(),
            [&row](const auto& test_case) {
                return test_case.test_case == row.catch2_case
                    && test_case.section == row.catch2_section;
            });
        if (case_has_explicit_sections && !row.catch2_section.empty()
            && !ledger_section_executed) {
            continue;
        }
        expected.push_back(
            bioseq::postgres_test::expected_migration_probe{
                .stable_id = row.stable_id,
                .expected_count =
                    bioseq::cache_test::migration::expected_probe_count(row)});
    }
    return expected;
}

/// @brief Writes the native runner verdict and stable migration probes.
void write_report(
    const std::filesystem::path& report,
    std::string_view executable,
    std::vector<bioseq::postgres_test::report_argument> arguments,
    const bioseq::cache_test::runner_configuration& configuration) {
    const bioseq::postgres_test::cleanup_report cleanup =
        bioseq::postgres_test::aggregate_cleanup_report();
    bioseq::postgres_test::integration_test_report native_report{
        .schema_version = "bioseq-cache-test-v1",
        .executable = std::string(executable),
        .build_identity =
            bioseq::postgres_test::sha256_file(
                std::filesystem::absolute(
                    std::filesystem::path(executable))).hex(),
        .arguments = std::move(arguments),
        .cluster_configuration = {
            {"ownership",
             "fixture-owned isolated clusters; cases may run matrices"},
            {"transport", "Unix socket only"},
            {"authentication", "local trust in private PGDATA"}},
        .cases = catch_evidence().cases,
        .phase_durations =
            bioseq::postgres_test::phase_duration_reports(),
        .host_qualifications =
            bioseq::postgres_test::host_qualification_reports(),
        .sql_transcripts =
            bioseq::postgres_test::process_sql_transcript_entries(),
        .migration_probes =
            bioseq::postgres_test::migration_probe_records(),
        .artifacts =
            bioseq::postgres_test::artifact_stage_reports(),
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
    bioseq::postgres_test::write_integration_test_report(
        report, native_report);
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
    if (pid <= 1) {
        return true;
    }
    if (::kill(static_cast<pid_t>(pid), 0) == 0) {
        return false;
    }
    return errno == ESRCH;
}

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

[[nodiscard]] bioseq::postgres_test::cleanup_observation
watchdog_cleanup_cluster(
    const bioseq::postgres_test::supervised_cluster_record& cluster,
    std::uint64_t ordinal) noexcept {
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
        std::vector<std::uint64_t> process_tree;
        if (active.postmaster_pid > 1
            && !process_absent(active.postmaster_pid)) {
            if (auto sampled =
                    bioseq::postgres_test::
                        sample_postmaster_process_tree_pids(
                            active.postmaster_pid)) {
                process_tree = std::move(*sampled);
            } else {
                process_tree.push_back(active.postmaster_pid);
            }
            const pid_t native_pid =
                static_cast<pid_t>(active.postmaster_pid);
            const pid_t group = ::getpgid(native_pid);
            if (group > 1 && group != ::getpgrp()) {
                static_cast<void>(::kill(-group, SIGTERM));
            }
            static_cast<void>(::kill(native_pid, SIGTERM));
            const auto graceful_deadline =
                std::chrono::steady_clock::now()
                + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < graceful_deadline
                   && !process_absent(active.postmaster_pid)) {
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(20));
            }
            if (!process_absent(active.postmaster_pid)) {
                for (auto iterator = process_tree.rbegin();
                     iterator != process_tree.rend(); ++iterator) {
                    if (*iterator > 1) {
                        static_cast<void>(::kill(
                            static_cast<pid_t>(*iterator), SIGKILL));
                    }
                }
                if (group > 1 && group != ::getpgrp()) {
                    static_cast<void>(::kill(-group, SIGKILL));
                }
                const auto force_deadline =
                    std::chrono::steady_clock::now()
                    + std::chrono::seconds(2);
                while (std::chrono::steady_clock::now() < force_deadline
                       && !process_absent(active.postmaster_pid)) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(20));
                }
            }
        }
        if (active.postmaster_pid > 1
            && !process_absent(active.postmaster_pid)) {
            observation.detail =
                "watchdog could not stop the registered postmaster";
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
    std::uint64_t process_pid) noexcept {
    if (process_absent(process_pid)) {
        return true;
    }
    const pid_t native_pid = static_cast<pid_t>(process_pid);
    const pid_t group = ::getpgid(native_pid);
    if (group > 1 && group != ::getpgrp()) {
        static_cast<void>(::kill(-group, SIGTERM));
    }
    static_cast<void>(::kill(native_pid, SIGTERM));
    const auto graceful_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < graceful_deadline
           && !process_absent(process_pid)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (process_absent(process_pid)) {
        return true;
    }
    if (group > 1 && group != ::getpgrp()) {
        static_cast<void>(::kill(-group, SIGKILL));
    }
    static_cast<void>(::kill(native_pid, SIGKILL));
    const auto force_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < force_deadline
           && !process_absent(process_pid)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return process_absent(process_pid);
}

[[nodiscard]] int run_internal_watchdog(
    const std::filesystem::path& state_directory,
    const std::filesystem::path& report_path,
    std::string test_case,
    std::uint64_t parent_pid,
    std::chrono::seconds timeout,
    const std::filesystem::path& executable) {
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool parent_disappeared = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (process_absent(parent_pid)
            || ::getppid() != static_cast<pid_t>(parent_pid)) {
            parent_disappeared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!parent_disappeared
        && ::kill(static_cast<pid_t>(parent_pid), SIGSTOP) != 0) {
        if (errno == ESRCH) {
            parent_disappeared = true;
        } else {
            return 125;
        }
    }

    const auto clusters =
        bioseq::postgres_test::load_supervised_clusters(state_directory);
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

    const auto processes =
        bioseq::postgres_test::load_supervised_processes(state_directory);
    bool lifecycle_cleanup_succeeded = true;
    for (const std::uint64_t process_pid : processes) {
        lifecycle_cleanup_succeeded =
            watchdog_cleanup_process(process_pid)
            && lifecycle_cleanup_succeeded;
    }

    std::vector<bioseq::postgres_test::cleanup_observation>
        cleanup_observations;
    cleanup_observations.reserve(clusters.size());
    bool cleanup_succeeded = lifecycle_cleanup_succeeded;
    for (std::size_t index = 0; index < clusters.size(); ++index) {
        auto observation = watchdog_cleanup_cluster(
            clusters[index], index);
        observation.test_case = test_case;
        if (index == 0) {
            observation.resources = resources;
        }
        cleanup_succeeded =
            cleanup_succeeded && observation.succeeded;
        cleanup_observations.push_back(std::move(observation));
    }

    std::string build_identity;
    try {
        build_identity =
            bioseq::postgres_test::sha256_file(executable).hex();
    } catch (...) {
    }
    bioseq::postgres_test::integration_test_report emergency{
        .schema_version = "bioseq-cache-test-v1",
        .executable = executable.string(),
        .build_identity = std::move(build_identity),
        .cluster_configuration = {
            {"supervision", "out-of-process per-case watchdog"},
            {"termination", "bounded process-tree cleanup"}},
        .cases = {
            bioseq::postgres_test::test_case_report{
                .test_case = std::move(test_case),
                .status = "infrastructure-inconclusive",
                .message = parent_disappeared
                    ? "external watchdog observed an unexpected runner "
                      "termination"
                    : "external watchdog exceeded the per-case timeout of "
                        + std::to_string(timeout.count()) + " seconds",
                .duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)}},
        .sql_transcripts =
            bioseq::postgres_test::load_supervision_sql_transcript(
                state_directory),
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
    } catch (...) {
        static_cast<void>(
            ::kill(static_cast<pid_t>(parent_pid), SIGKILL));
        return 126;
    }
    static_cast<void>(
        bioseq::postgres_test::remove_supervision_state(state_directory));
    if (!parent_disappeared) {
        static_cast<void>(
            ::kill(static_cast<pid_t>(parent_pid), SIGKILL));
    }
    return 124;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 7
        && std::string_view(argv[1])
            == "--bioseq-internal-watchdog") {
        const auto parent_pid = parse_unsigned(argv[5]);
        const auto timeout = parse_unsigned(argv[6]);
        if (!parent_pid || *parent_pid <= 1 || !timeout
            || *timeout == 0
            || *timeout
                > static_cast<std::uint64_t>(
                    std::chrono::seconds::max().count())) {
            return 2;
        }
        return run_internal_watchdog(
            argv[2], argv[3], argv[4], *parent_pid,
            std::chrono::seconds(
                static_cast<std::chrono::seconds::rep>(*timeout)),
            current_executable_path(argv[0]));
    }
    Catch::Session session;

    std::string postgres_bin_dir;
    std::string cache_control;
    std::string cache_install_sql;
    std::string cache_dso;
    std::string bioseq_control;
    std::string bioseq_install_sql;
    std::string bioseq_dso;
    std::string provider_peer_test_dso;
    std::string migration_ledger;
    std::string report;
    int case_timeout_seconds = 480;

    auto cli = session.cli()
        | Catch::Clara::Opt(postgres_bin_dir, "PATH")
              ["--postgres-bin-dir"]("PostgreSQL executable directory")
        | Catch::Clara::Opt(cache_control, "PATH")
              ["--cache-control"]("bioseq_cache control file")
        | Catch::Clara::Opt(cache_install_sql, "PATH")
              ["--cache-install-sql"]("bioseq_cache generated install SQL")
        | Catch::Clara::Opt(cache_dso, "PATH")
              ["--cache-dso"]("bioseq_cache test-support DSO")
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
        | Catch::Clara::Opt(report, "PATH")
              ["--report"]("native JSON report path")
        | Catch::Clara::Opt(case_timeout_seconds, "SECONDS")
              ["--case-timeout-seconds"]("per-case watchdog");
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
    if (postgres_bin_dir.empty() || cache_control.empty()
        || cache_install_sql.empty() || cache_dso.empty()
        || migration_ledger.empty()
        || report.empty()
        || case_timeout_seconds <= 0) {
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

    bioseq::cache_test::runner_configuration configuration{
        .postgres_bin_dir = postgres_bin_dir,
        .cache_control = cache_control,
        .cache_install_sql = cache_install_sql,
        .cache_dso = cache_dso,
        .report = report,
        .case_timeout = std::chrono::seconds(case_timeout_seconds)};
    if (all_bioseq) {
        configuration.bioseq_control = bioseq_control;
        configuration.bioseq_install_sql = bioseq_install_sql;
        configuration.bioseq_dso = bioseq_dso;
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
        .executable = current_executable_path(argv[0]),
        .report = std::filesystem::absolute(report)};
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
    try {
        const auto expected = expected_migration_probes(
            migration_ledger, catch_evidence().cases);
        bioseq::postgres_test::validate_migration_probes(expected);
        migration_validation_message =
            std::to_string(expected.size())
            + " selected migration-ledger rows validated";
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
        bioseq::postgres_test::aggregate_cleanup_report().succeeded;
    try {
        write_report(
            report, argv[0], std::move(arguments),
            bioseq::cache_test::configuration());
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
