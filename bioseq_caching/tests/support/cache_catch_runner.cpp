#include <catch2/catch_session.hpp>
#include <catch2/catch_test_case_info.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
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
#include <utility>
#include <vector>

import bioseq.cache.test_context;
import bioseq.test.postgres;

namespace {

struct catch_report_evidence {
    std::vector<bioseq::postgres_test::test_case_result> cases;
    std::vector<bioseq::postgres_test::artifact_observation> attested_artifacts;
    std::string current_case;
    std::vector<std::string> section_stack;
    std::vector<std::size_t> section_failure_offsets;
    std::string failure_message;
    std::chrono::steady_clock::time_point case_started{};
    std::optional<bioseq::postgres_test::owned_process> watchdog;
    std::filesystem::path watchdog_state;
    std::vector<std::filesystem::path> retained_watchdog_states;
    bool watchdog_protocol_failed{};
    bool watchdog_retention_recording_failed{};
};

[[nodiscard]] catch_report_evidence& catch_evidence() {
    static catch_report_evidence evidence;
    return evidence;
}

[[nodiscard]] bool
same_artifact_identity(const bioseq::postgres_test::artifact_observation& left,
                       const bioseq::postgres_test::artifact_observation& right) {
    return left.name == right.name &&
           left.preinstalled_stage_root == right.preinstalled_stage_root &&
           left.stage_manifest == right.stage_manifest &&
           left.stage_manifest_sha256 == right.stage_manifest_sha256 &&
           left.control_file == right.control_file && left.install_sql == right.install_sql &&
           left.dso == right.dso && left.staged_control_file == right.staged_control_file &&
           left.staged_install_sql == right.staged_install_sql &&
           left.staged_dso == right.staged_dso && left.control_sha256 == right.control_sha256 &&
           left.install_sql_sha256 == right.install_sql_sha256 &&
           left.dso_sha256 == right.dso_sha256 &&
           left.staged_control_sha256 == right.staged_control_sha256;
}

void retain_attested_artifacts(catch_report_evidence& evidence) {
    for (const auto& artifact : bioseq::postgres_test::artifact_observations()) {
        const bool already_retained = std::ranges::any_of(
            evidence.attested_artifacts,
            [&artifact](const bioseq::postgres_test::artifact_observation& retained) {
                return same_artifact_identity(retained, artifact);
            });
        if (!already_retained) {
            evidence.attested_artifacts.push_back(artifact);
        }
    }
}

void begin_unattested_artifact_scope(catch_report_evidence& evidence, bool publish_empty_snapshot) {
    retain_attested_artifacts(evidence);
    bioseq::postgres_test::clear_artifact_observations();
    if (publish_empty_snapshot) {
        const std::vector<bioseq::postgres_test::artifact_observation> empty;
        if (!bioseq::postgres_test::publish_supervision_artifact_observations(empty)) {
            throw std::runtime_error("cannot clear crash-resilient artifact evidence");
        }
    }
}

struct watchdog_launch_configuration {
    std::filesystem::path executable;
    std::string executable_sha256;
    std::filesystem::path report;
};

constexpr std::string_view watchdog_armed_magic = "bioseq-watchdog-armed";
constexpr std::uint32_t watchdog_armed_schema_revision = 1;
constexpr auto watchdog_arming_budget = std::chrono::seconds(5);

[[nodiscard]] std::optional<watchdog_launch_configuration>& watchdog_launch() {
    static std::optional<watchdog_launch_configuration> configuration;
    return configuration;
}

[[nodiscard]] std::filesystem::path
watchdog_armed_path(const std::filesystem::path& state_directory) {
    return state_directory / "armed";
}

[[nodiscard]] std::string watchdog_armed_record(std::uint64_t parent_pid,
                                                std::uint64_t watchdog_pid) {
    return std::string(watchdog_armed_magic) + "\t" +
           std::to_string(watchdog_armed_schema_revision) + "\t" + std::to_string(parent_pid) +
           "\t" + std::to_string(watchdog_pid) + "\n";
}

[[nodiscard]] bool publish_watchdog_armed(const std::filesystem::path& state_directory,
                                          std::uint64_t parent_pid) noexcept {
    try {
        const std::filesystem::path armed = watchdog_armed_path(state_directory);
        const std::filesystem::path partial = state_directory / "armed.partial";
        const std::string record =
            watchdog_armed_record(parent_pid, static_cast<std::uint64_t>(::getpid()));
        const int descriptor = ::open(partial.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (descriptor < 0) {
            return false;
        }
        bool written_all = true;
        std::size_t offset = 0;
        while (offset < record.size()) {
            const ssize_t written =
                ::write(descriptor, record.data() + offset, record.size() - offset);
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
            static_cast<void>(std::filesystem::remove(partial, error));
        }
        return written_all;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] std::string
watchdog_exit_detail(const bioseq::postgres_test::process_exit_status& exit) {
    if (exit.exit_code) {
        return "exit code " + std::to_string(*exit.exit_code);
    }
    if (exit.signal) {
        return "signal " + std::to_string(*exit.signal);
    }
    return "unknown process status";
}

void await_watchdog_armed(bioseq::postgres_test::owned_process& watchdog,
                          const std::filesystem::path& state_directory,
                          std::uint64_t parent_pid) {
    const std::int64_t child_pid = watchdog.pid();
    if (child_pid <= 1) {
        throw std::runtime_error("external watchdog has no live child identity");
    }
    const std::string expected =
        watchdog_armed_record(parent_pid, static_cast<std::uint64_t>(child_pid));
    const std::filesystem::path armed = watchdog_armed_path(state_directory);
    const auto deadline = std::chrono::steady_clock::now() + watchdog_arming_budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (const auto exit = watchdog.try_wait()) {
            throw std::runtime_error(
                "external watchdog exited before arming: " + watchdog_exit_detail(*exit) +
                "; captured output:\n" + watchdog.captured_output());
        }
        std::error_code error;
        const bool exists = std::filesystem::exists(armed, error);
        if (error) {
            throw std::runtime_error("cannot inspect external-watchdog armed evidence: " +
                                     error.message());
        }
        if (exists) {
            if (!std::filesystem::is_regular_file(armed, error) || error) {
                throw std::runtime_error("external-watchdog armed evidence is not a "
                                         "regular file");
            }
            std::ifstream input(armed, std::ios::binary);
            const std::string actual{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
            if (!input || actual != expected) {
                throw std::runtime_error("external-watchdog armed evidence is malformed");
            }
            if (const auto exit = watchdog.try_wait()) {
                throw std::runtime_error(
                    "external watchdog exited while arming: " + watchdog_exit_detail(*exit) +
                    "; captured output:\n" + watchdog.captured_output());
            }
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (const auto exit = watchdog.try_wait()) {
        throw std::runtime_error(
            "external watchdog exited before arming: " + watchdog_exit_detail(*exit) +
            "; captured output:\n" + watchdog.captured_output());
    }
    throw std::runtime_error("external watchdog did not arm within " +
                             std::to_string(watchdog_arming_budget.count()) + " seconds");
}

[[nodiscard]] std::filesystem::path make_watchdog_state() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "bioseq-watchdog-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) {
        throw std::runtime_error("cannot create external-watchdog state directory");
    }
    return std::filesystem::path(created);
}

[[nodiscard]] std::filesystem::path
current_executable_path([[maybe_unused]] std::string_view argv_zero) {
#if defined(__linux__)
    return std::filesystem::canonical("/proc/self/exe");
#elif defined(__APPLE__)
    std::uint32_t size{};
    static_cast<void>(::_NSGetExecutablePath(nullptr, &size));
    std::vector<char> path(size);
    if (size == 0 || ::_NSGetExecutablePath(path.data(), &size) != 0) {
        throw std::runtime_error("cannot identify native test executable");
    }
    return std::filesystem::canonical(path.data());
#else
    return std::filesystem::canonical(std::filesystem::absolute(argv_zero));
#endif
}

void seed_supervision_evidence(const catch_report_evidence& evidence) {
    const bool published =
        bioseq::postgres_test::publish_supervision_test_case_results(evidence.cases) &&
        bioseq::postgres_test::publish_supervision_stage_timings(
            bioseq::postgres_test::stage_timings()) &&
        bioseq::postgres_test::publish_supervision_host_qualifications(
            bioseq::postgres_test::host_qualification_results()) &&
        bioseq::postgres_test::publish_supervision_artifact_observations(
            bioseq::postgres_test::artifact_observations());
    if (!published) {
        throw std::runtime_error("cannot seed crash-resilient watchdog evidence");
    }
}

void retain_watchdog_state(catch_report_evidence& evidence) noexcept {
    if (evidence.watchdog_state.empty()) {
        return;
    }
    try {
        evidence.retained_watchdog_states.push_back(evidence.watchdog_state);
        evidence.watchdog_state.clear();
    } catch (...) {
        // Preserve watchdog_state when recording its path fails. It can still
        // be reported later without allocating in this cleanup handler.
        evidence.watchdog_retention_recording_failed = true;
    }
}

void stop_watchdog(catch_report_evidence& evidence) noexcept {
    try {
        bool registries_empty = true;
        if (!evidence.watchdog_state.empty()) {
            registries_empty =
                bioseq::postgres_test::is_supervision_registry_empty(evidence.watchdog_state);
        }
        bool decision_owned = false;
        bool watchdog_exit_succeeded = false;
        if (evidence.watchdog && !evidence.watchdog_state.empty()) {
            decision_owned = bioseq::postgres_test::try_claim_supervision_decision(
                evidence.watchdog_state, registries_empty ? "disarmed" : "retained");
            try {
                const bioseq::postgres_test::process_exit_status exit = evidence.watchdog->wait_for(
                    decision_owned ? std::chrono::seconds(2) : std::chrono::seconds(30));
                watchdog_exit_succeeded = exit.is_successful();
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
                registries_empty &&
                bioseq::postgres_test::is_supervision_registry_empty(evidence.watchdog_state);
            const bool clean_disarm =
                decision_owned && watchdog_exit_succeeded && registries_still_empty;
            const bool removed = clean_disarm && bioseq::postgres_test::remove_supervision_state(
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

[[nodiscard]] std::string active_section(const catch_report_evidence& evidence) {
    std::string result;
    for (std::size_t index = 1; index < evidence.section_stack.size(); ++index) {
        if (!result.empty()) {
            result += " / ";
        }
        result += evidence.section_stack[index];
    }
    return result;
}

[[nodiscard]] std::string status(const Catch::Counts& counts, bool aborting = false) {
    if (aborting || counts.failed != 0) {
        return "failed";
    }
    if (counts.skipped != 0 && counts.passed == 0) {
        return "skipped";
    }
    return "passed";
}

[[nodiscard]] std::string test_case_status(const Catch::Totals& totals, bool aborting) {
    if (aborting || totals.assertions.failed != 0 || totals.testCases.failed != 0) {
        return "failed";
    }
    if (totals.testCases.skipped != 0 ||
        (totals.assertions.skipped != 0 && totals.assertions.passed == 0)) {
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
            throw std::runtime_error("prior external-watchdog state could not be retained");
        }
        evidence = {};
        bioseq::postgres_test::clear_execution_case_context();
    }

    void testCaseStarting(const Catch::TestCaseInfo& test_info) override {
        auto& evidence = catch_evidence();
        evidence.current_case = test_info.name;
        evidence.section_stack.clear();
        evidence.section_failure_offsets.clear();
        evidence.failure_message.clear();
        evidence.case_started = std::chrono::steady_clock::now();
        const auto& runner_configuration = bioseq::cache_test::configuration();
        const std::chrono::seconds timeout = runner_configuration.case_timeout;
        if (!watchdog_launch()) {
            throw std::runtime_error("external watchdog launch configuration is missing");
        }
        if (!evidence.watchdog_state.empty() || !evidence.retained_watchdog_states.empty() ||
            evidence.watchdog_protocol_failed || evidence.watchdog_retention_recording_failed) {
            throw std::runtime_error("prior external-watchdog supervision failure is still "
                                     "active");
        }
        // Clear the previous fixture's manifest details before starting this case.
        // A failure before stage validation must be reported without those details.
        begin_unattested_artifact_scope(evidence, false);
        evidence.watchdog_state = make_watchdog_state();
        try {
            bioseq::postgres_test::configure_supervision_state(evidence.watchdog_state);
            bioseq::postgres_test::initialize_supervision_report_journals();
            bioseq::postgres_test::initialize_supervision_resource_journal();
            seed_supervision_evidence(evidence);
            evidence.watchdog.emplace(bioseq::postgres_test::owned_process::spawn(
                bioseq::postgres_test::process_launch_options{
                    .executable = watchdog_launch()->executable,
                    .arguments = {"--bioseq-internal-watchdog",
                                  evidence.watchdog_state.string(),
                                  watchdog_launch()->report.string(),
                                  evidence.current_case,
                                  std::to_string(::getpid()),
                                  std::to_string(timeout.count()),
                                  watchdog_launch()->executable_sha256},
                    .output_file = evidence.watchdog_state / "watchdog.log"}));
            await_watchdog_armed(*evidence.watchdog,
                                 evidence.watchdog_state,
                                 static_cast<std::uint64_t>(::getpid()));
        } catch (...) {
            stop_watchdog(evidence);
            throw;
        }
        bioseq::postgres_test::set_execution_case_context({.test_case = evidence.current_case});
    }

    void sectionStarting(const Catch::SectionInfo& section_info) override {
        auto& evidence = catch_evidence();
        if (evidence.section_stack.empty()) {
            // Each outer section can use a different stage. Clear the previous
            // section's manifest details until this section validates its own files.
            begin_unattested_artifact_scope(evidence, true);
        }
        evidence.section_stack.push_back(section_info.name);
        evidence.section_failure_offsets.push_back(evidence.failure_message.size());
        bioseq::postgres_test::set_execution_case_context(
            {.test_case = evidence.current_case, .section = active_section(evidence)});
    }

    void assertionEnded(const Catch::AssertionStats& assertion) override {
        if (assertion.assertionResult.isOk()) {
            return;
        }
        auto& message = catch_evidence().failure_message;
        if (!message.empty()) {
            message += '\n';
        }
        const Catch::StringRef reported = assertion.assertionResult.getMessage();
        if (!reported.empty()) {
            message += static_cast<std::string>(reported);
        } else if (assertion.assertionResult.hasExpandedExpression()) {
            message += assertion.assertionResult.getExpandedExpression();
        } else {
            message += assertion.assertionResult.getExpression();
        }
    }

    void sectionEnded(const Catch::SectionStats& section_stats) override {
        auto& evidence = catch_evidence();
        if (evidence.section_stack.size() > 1) {
            std::string section_message;
            if (section_stats.assertions.failed != 0 && !evidence.section_failure_offsets.empty()) {
                section_message =
                    evidence.failure_message.substr(evidence.section_failure_offsets.back());
                if (section_message.starts_with('\n')) {
                    section_message.erase(0, 1);
                }
            }
            evidence.cases.push_back(bioseq::postgres_test::test_case_result{
                .test_case = evidence.current_case,
                .section = active_section(evidence),
                .status = status(section_stats.assertions),
                .message = std::move(section_message),
                .duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::duration<double>(section_stats.durationInSeconds))});
            static_cast<void>(
                bioseq::postgres_test::publish_supervision_test_case_results(evidence.cases));
        }
        if (!evidence.section_stack.empty()) {
            evidence.section_stack.pop_back();
        }
        if (!evidence.section_failure_offsets.empty()) {
            evidence.section_failure_offsets.pop_back();
        }
        bioseq::postgres_test::set_execution_case_context(
            {.test_case = evidence.current_case, .section = active_section(evidence)});
    }

    void testCaseEnded(const Catch::TestCaseStats& test_stats) override {
        auto& evidence = catch_evidence();
        std::string outcome = test_case_status(test_stats.totals, test_stats.aborting);
        if (bioseq::postgres_test::has_inconclusive_host_qualification(test_stats.testInfo->name)) {
            outcome = "infrastructure-inconclusive";
        }
        evidence.cases.push_back(bioseq::postgres_test::test_case_result{
            .test_case = test_stats.testInfo->name,
            .status = outcome,
            .message = evidence.failure_message,
            .duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - evidence.case_started)});
        static_cast<void>(
            bioseq::postgres_test::publish_supervision_test_case_results(evidence.cases));
        retain_attested_artifacts(evidence);
        const bool watchdog_protocol_failed_before = evidence.watchdog_protocol_failed;
        stop_watchdog(evidence);
        if (!watchdog_protocol_failed_before && evidence.watchdog_protocol_failed) {
            evidence.cases.back().status = "infrastructure-inconclusive";
            if (!evidence.cases.back().message.empty()) {
                evidence.cases.back().message += '\n';
            }
            evidence.cases.back().message += "external watchdog failed validated disarm";
            static_cast<void>(
                bioseq::postgres_test::publish_supervision_test_case_results(evidence.cases));
        }
        bioseq::postgres_test::clear_execution_case_context();
        evidence.current_case.clear();
        evidence.section_stack.clear();
        evidence.section_failure_offsets.clear();
    }

    void skipTest(const Catch::TestCaseInfo& test_info) override {
        catch_evidence().cases.push_back(bioseq::postgres_test::test_case_result{
            .test_case = test_info.name, .status = "skipped"});
    }

    void testRunEnded(const Catch::TestRunStats&) override {
        stop_watchdog(catch_evidence());
    }
};

CATCH_REGISTER_LISTENER(native_report_listener);

/// Record source paths and hashes for the extension files.
[[nodiscard]] bioseq::postgres_test::artifact_observation
make_artifact_observation(std::string name,
                          const std::filesystem::path& control,
                          const std::filesystem::path& install_sql,
                          const std::filesystem::path& dso) {
    return bioseq::postgres_test::artifact_observation{
        .name = std::move(name),
        .control_file = control,
        .install_sql = install_sql,
        .dso = dso,
        .control_sha256 = bioseq::postgres_test::hash_sha256_file(control).render_hex(),
        .install_sql_sha256 = bioseq::postgres_test::hash_sha256_file(install_sql).render_hex(),
        .dso_sha256 = bioseq::postgres_test::hash_sha256_file(dso).render_hex()};
}

void clear_stale_report_output(const std::filesystem::path& report) {
    const std::filesystem::path absolute = std::filesystem::absolute(report);
    std::filesystem::create_directories(absolute.parent_path());
    for (const std::filesystem::path& candidate :
         {absolute, absolute.parent_path() / (absolute.filename().string() + ".partial")}) {
        std::error_code error;
        static_cast<void>(std::filesystem::remove(candidate, error));
        if (error) {
            throw std::runtime_error("cannot clear stale native report output " +
                                     candidate.string() + ": " + error.message());
        }
    }
}

[[nodiscard]] bool file_sha256_unchanged(const std::filesystem::path& path,
                                         std::string_view expected_sha256) noexcept {
    try {
        return bioseq::postgres_test::hash_sha256_file(path).render_hex() == expected_sha256;
    } catch (...) {
        return false;
    }
}

/// Write test outcomes, file hashes, resource measurements and cleanup results.
void write_report(const std::filesystem::path& report,
                  const std::filesystem::path& executable,
                  std::string_view executable_sha256,
                  std::vector<bioseq::postgres_test::invocation_argument> arguments,
                  const bioseq::cache_test::runner_configuration& configuration) {
    retain_attested_artifacts(catch_evidence());
    bioseq::postgres_test::cleanup_summary cleanup = bioseq::postgres_test::summarize_cleanup();
    std::string retained_supervision_states;
    const auto append_retained_state =
        [&retained_supervision_states](const std::filesystem::path& state) {
            if (!retained_supervision_states.empty()) {
                retained_supervision_states += "; ";
            }
            retained_supervision_states += state.string();
        };
    for (const auto& state : catch_evidence().retained_watchdog_states) {
        append_retained_state(state);
    }
    if (!catch_evidence().watchdog_state.empty()) {
        append_retained_state(catch_evidence().watchdog_state);
    }
    if (!retained_supervision_states.empty()) {
        cleanup.succeeded = false;
        cleanup.detail += (cleanup.detail.empty() ? "" : "; ");
        cleanup.detail +=
            "live-resource supervision state retained at " + retained_supervision_states;
    }
    if (catch_evidence().watchdog_protocol_failed) {
        cleanup.succeeded = false;
        cleanup.detail += (cleanup.detail.empty() ? "" : "; ");
        cleanup.detail += "external watchdog did not complete validated disarm";
    }
    if (catch_evidence().watchdog_retention_recording_failed) {
        cleanup.succeeded = false;
        cleanup.detail += (cleanup.detail.empty() ? "" : "; ");
        cleanup.detail += "watchdog-state retention bookkeeping failed; the active "
                          "state path was preserved";
    }
    bioseq::postgres_test::integration_run_report native_report{
        .schema_revision = 2,
        .executable = executable.string(),
        .build_identity = std::string(executable_sha256),
        .arguments = std::move(arguments),
        .cluster_configuration =
            {{"ownership", "fixture-owned isolated clusters; cases may run matrices"},
             {"transport", "Unix socket only"},
             {"authentication", "local trust in private PGDATA"},
             {"retained-supervision-state",
              retained_supervision_states.empty() ? "none" : retained_supervision_states}},
        .cases = catch_evidence().cases,
        .stage_timings = bioseq::postgres_test::stage_timings(),
        .host_qualifications = bioseq::postgres_test::host_qualification_results(),
        .sql_transcripts = bioseq::postgres_test::process_sql_transcript_entries(),
        .artifacts = catch_evidence().attested_artifacts,
        .cleanup_observations = bioseq::postgres_test::cleanup_observations(),
        .resources = bioseq::postgres_test::summarize_resources(),
        .cleanup = cleanup};
    const auto has_artifact = [&native_report](std::string_view name) {
        return std::any_of(native_report.artifacts.begin(),
                           native_report.artifacts.end(),
                           [name](const bioseq::postgres_test::artifact_observation& artifact) {
                               return artifact.name == name;
                           });
    };
    if (!has_artifact("bioseq_cache")) {
        native_report.artifacts.push_back(make_artifact_observation("bioseq_cache",
                                                                    configuration.cache_control,
                                                                    configuration.cache_install_sql,
                                                                    configuration.cache_dso));
    }
    if (configuration.bioseq_control && configuration.bioseq_install_sql &&
        configuration.bioseq_dso) {
        if (!has_artifact("bioseq")) {
            native_report.artifacts.push_back(
                make_artifact_observation("bioseq",
                                          *configuration.bioseq_control,
                                          *configuration.bioseq_install_sql,
                                          *configuration.bioseq_dso));
        }
    }
    if (!file_sha256_unchanged(executable, executable_sha256)) {
        throw std::runtime_error("integration executable changed before report publication");
    }
    bioseq::postgres_test::write_integration_run_report(report, native_report);
    if (!file_sha256_unchanged(executable, executable_sha256)) {
        throw std::runtime_error("integration executable changed during report publication");
    }
}

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(std::string_view text) {
    std::uint64_t value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool process_absent(std::uint64_t pid) noexcept {
    if (pid <= 1 || pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) {
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
        const bioseq::postgres_test::supervised_process_identity* parent) noexcept
        : parent_(parent) {}

    stopped_parent_fail_safe(const stopped_parent_fail_safe&) = delete;
    stopped_parent_fail_safe& operator=(const stopped_parent_fail_safe&) = delete;

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
        if (parent_ == nullptr ||
            !bioseq::postgres_test::is_supervised_process_identity_current(*parent_)) {
            return;
        }
        if (::kill(static_cast<pid_t>(parent_->pid), SIGKILL) != 0 && errno != ESRCH &&
            bioseq::postgres_test::is_supervised_process_identity_current(*parent_)) {
            // Try to resume the same parent if killing it fails, so it is not left
            // stopped by SIGSTOP.
            static_cast<void>(::kill(static_cast<pid_t>(parent_->pid), SIGCONT));
        }
    }

  private:
    const bioseq::postgres_test::supervised_process_identity* parent_{};
    bool armed_{};
};

[[nodiscard]] bool
marker_owned_cluster(const bioseq::postgres_test::supervised_cluster_identity& cluster) {
    if (cluster.root.empty() || !cluster.root.filename().string().starts_with("bioseq-pg-test-") ||
        !std::filesystem::is_regular_file(cluster.root / ".bioseq-private-cluster")) {
        return false;
    }
    if (std::filesystem::canonical(cluster.root.parent_path()) !=
        std::filesystem::canonical(std::filesystem::temp_directory_path())) {
        return false;
    }
    return cluster.data_directory.lexically_normal() == (cluster.root / "data").lexically_normal();
}

[[nodiscard]] bool marker_owned_data_directory(const std::filesystem::path& data_directory) {
    if (data_directory.empty()) {
        return true;
    }
    return marker_owned_cluster(bioseq::postgres_test::supervised_cluster_identity{
        .root = data_directory.parent_path(), .data_directory = data_directory});
}

[[nodiscard]] bool supervised_members_absent(
    const std::vector<bioseq::postgres_test::supervised_process_identity>& members,
    const std::vector<std::int64_t>& process_groups) noexcept {
    return std::ranges::none_of(members,
                                bioseq::postgres_test::is_supervised_process_identity_current) &&
           std::ranges::all_of(process_groups,
                               bioseq::postgres_test::is_supervised_process_group_absent);
}

[[nodiscard]] bool wait_for_supervised_members_absence(
    const std::vector<bioseq::postgres_test::supervised_process_identity>& members,
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
    const std::vector<bioseq::postgres_test::supervised_process_identity>& members,
    std::int64_t process_group) noexcept {
    return process_group > 1 && process_group != static_cast<std::int64_t>(::getpgrp()) &&
           std::ranges::any_of(
               members,
               [process_group](const bioseq::postgres_test::supervised_process_identity& member) {
                   return member.process_group == process_group &&
                          bioseq::postgres_test::is_supervised_process_group_current(member);
               });
}

void signal_supervised_group(
    const std::vector<bioseq::postgres_test::supervised_process_identity>& members,
    std::int64_t process_group,
    int signal) noexcept {
    if (supervised_group_anchored(members, process_group)) {
        static_cast<void>(::kill(-static_cast<pid_t>(process_group), signal));
    }
}

[[nodiscard]] std::vector<std::int64_t> supervised_process_groups(
    const std::vector<bioseq::postgres_test::supervised_process_identity>& members) {
    std::vector<std::int64_t> groups;
    for (const auto& member : members) {
        if (member.process_group > 1 &&
            std::ranges::find(groups, member.process_group) == groups.end()) {
            groups.push_back(member.process_group);
        }
    }
    return groups;
}

void signal_supervised_groups(
    const std::vector<bioseq::postgres_test::supervised_process_identity>& members,
    const std::vector<std::int64_t>& process_groups,
    int signal) noexcept {
    for (const std::int64_t process_group : process_groups) {
        signal_supervised_group(members, process_group, signal);
    }
}

[[nodiscard]] bool stop_supervised_groups(
    const std::vector<bioseq::postgres_test::supervised_process_identity>& members,
    const std::vector<std::int64_t>& process_groups) noexcept {
    for (const std::int64_t process_group : process_groups) {
        if (!supervised_group_anchored(members, process_group) ||
            ::kill(-static_cast<pid_t>(process_group), SIGSTOP) != 0) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::optional<std::vector<bioseq::postgres_test::supervised_process_identity>>
capture_stable_supervised_tree(
    const bioseq::postgres_test::supervised_process_identity& postmaster) noexcept {
    try {
        for (int attempt = 0; attempt != 5; ++attempt) {
            if (attempt != 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            auto first = bioseq::postgres_test::capture_postgres_process_tree_pids(postmaster.pid);
            if (!first) {
                continue;
            }
            std::ranges::sort(*first);
            first->erase(std::unique(first->begin(), first->end()), first->end());
            if (!std::ranges::binary_search(*first, postmaster.pid)) {
                continue;
            }
            std::vector<bioseq::postgres_test::supervised_process_identity> members;
            members.reserve(first->size());
            bool complete = true;
            for (const std::uint64_t pid : *first) {
                const auto member = bioseq::postgres_test::capture_supervised_process_identity(
                    pid,
                    pid == postmaster.pid ? postmaster.executable : std::filesystem::path{},
                    postmaster.data_directory);
                if (!member ||
                    (pid == postmaster.pid && member->start_token != postmaster.start_token)) {
                    complete = false;
                    break;
                }
                members.push_back(*member);
            }
            if (!complete) {
                continue;
            }
            auto second = bioseq::postgres_test::capture_postgres_process_tree_pids(postmaster.pid);
            if (!second) {
                continue;
            }
            std::ranges::sort(*second);
            second->erase(std::unique(second->begin(), second->end()), second->end());
            if (*first == *second) {
                return members;
            }
        }
    } catch (...) {
    }
    return std::nullopt;
}

[[nodiscard]] bool locally_supervised_process_stopped(
    const bioseq::postgres_test::supervised_process_identity& process) noexcept {
    try {
        if (!bioseq::postgres_test::is_supervised_process_identity_current(process)) {
            return false;
        }
#if defined(__linux__)
        std::ifstream input(std::filesystem::path("/proc") / std::to_string(process.pid) / "stat");
        std::string line;
        if (!input || !std::getline(input, line)) {
            return false;
        }
        const std::size_t command_end = line.rfind(')');
        if (command_end == std::string::npos || command_end + 2U >= line.size()) {
            return false;
        }
        const char state = line[command_end + 2U];
        return state == 'T' || state == 't';
#elif defined(__APPLE__)
        proc_bsdinfo information{};
        return ::proc_pidinfo(static_cast<int>(process.pid),
                              PROC_PIDTBSDINFO,
                              0,
                              &information,
                              sizeof(information)) == static_cast<int>(sizeof(information)) &&
               information.pbi_status == SSTOP;
#else
        return false;
#endif
    } catch (...) {
        return false;
    }
}

void resume_supervised_processes(
    const std::vector<bioseq::postgres_test::supervised_process_identity>& processes) noexcept {
    for (const auto& process : processes) {
        if (bioseq::postgres_test::is_supervised_process_identity_current(process)) {
            static_cast<void>(::kill(static_cast<pid_t>(process.pid), SIGCONT));
        }
    }
}

[[nodiscard]] std::optional<std::vector<bioseq::postgres_test::supervised_process_identity>>
freeze_stable_supervised_tree(
    const bioseq::postgres_test::supervised_process_identity& postmaster) noexcept {
    std::vector<bioseq::postgres_test::supervised_process_identity> stopped;
    std::vector<std::int64_t> stopped_groups;
    try {
        if (!bioseq::postgres_test::is_supervised_process_group_current(postmaster)) {
            return std::nullopt;
        }
        stopped.push_back(postmaster);
        stopped_groups.push_back(postmaster.process_group);
        if (!stop_supervised_groups(stopped, stopped_groups)) {
            signal_supervised_groups(stopped, stopped_groups, SIGCONT);
            resume_supervised_processes(stopped);
            return std::nullopt;
        }
        for (int attempt = 0; attempt != 50; ++attempt) {
            const auto tree = capture_stable_supervised_tree(postmaster);
            if (!tree) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            bool signaled = true;
            for (const auto& member : *tree) {
                if (std::ranges::none_of(stopped, [&member](const auto& known) {
                        return known.pid == member.pid && known.start_token == member.start_token;
                    })) {
                    stopped.push_back(member);
                }
                if (std::ranges::find(stopped_groups, member.process_group) ==
                    stopped_groups.end()) {
                    stopped_groups.push_back(member.process_group);
                }
            }
            if (!stop_supervised_groups(stopped, stopped_groups)) {
                signal_supervised_groups(stopped, stopped_groups, SIGCONT);
                resume_supervised_processes(stopped);
                return std::nullopt;
            }
            for (const auto& member : *tree) {
                if (!bioseq::postgres_test::is_supervised_process_identity_current(member) ||
                    ::kill(static_cast<pid_t>(member.pid), SIGSTOP) != 0) {
                    signaled = false;
                    break;
                }
            }
            if (!signaled) {
                signal_supervised_groups(stopped, stopped_groups, SIGCONT);
                resume_supervised_processes(stopped);
                return std::nullopt;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            const auto confirmation = capture_stable_supervised_tree(postmaster);
            if (!confirmation || confirmation->size() != tree->size()) {
                continue;
            }
            bool fixed_point = true;
            for (std::size_t index = 0; index < tree->size(); ++index) {
                if ((*tree)[index].pid != (*confirmation)[index].pid ||
                    (*tree)[index].start_token != (*confirmation)[index].start_token ||
                    !locally_supervised_process_stopped((*confirmation)[index])) {
                    fixed_point = false;
                    break;
                }
            }
            if (fixed_point) {
                fixed_point = std::ranges::all_of(stopped, [](const auto& member) {
                    return !bioseq::postgres_test::is_supervised_process_identity_current(member) ||
                           locally_supervised_process_stopped(member);
                });
            }
            if (fixed_point) {
                return stopped;
            }
        }
    } catch (...) {
    }
    signal_supervised_groups(stopped, stopped_groups, SIGCONT);
    resume_supervised_processes(stopped);
    return std::nullopt;
}

[[nodiscard]] bioseq::postgres_test::cleanup_observation
watchdog_cleanup_cluster(const bioseq::postgres_test::supervised_cluster_identity& cluster,
                         std::uint64_t ordinal,
                         bool allow_root_removal) {
    bioseq::postgres_test::cleanup_observation observation{.ordinal = ordinal,
                                                           .root = cluster.root};
    try {
        if (!marker_owned_cluster(cluster)) {
            observation.detail = "watchdog refused an unproven private-cluster root";
            return observation;
        }
        bioseq::postgres_test::supervised_cluster_identity active = cluster;
        if (active.discover_postmaster_pid && active.postmaster_pid <= 1) {
            std::ifstream pid_input(active.data_directory / "postmaster.pid");
            std::uint64_t pid_from_data{};
            pid_input >> pid_from_data;
            if (pid_input && pid_from_data > 1) {
                active.postmaster_pid = pid_from_data;
            }
        }
        if (active.postmaster_pid == 1) {
            active.postmaster_pid = 0;
        }
        if (active.discover_postmaster_pid && active.postmaster_pid <= 1) {
            observation.detail = "watchdog retained PGDATA because launch-pending "
                                 "postmaster identity recovery was inconclusive";
            return observation;
        }
        if (active.postmaster_pid > 1 && process_absent(active.postmaster_pid)) {
            if (active.postmaster_process_group > 1 &&
                !bioseq::postgres_test::is_supervised_process_group_absent(
                    active.postmaster_process_group)) {
                observation.detail = "watchdog refused to signal a surviving postmaster "
                                     "group after its recorded identity disappeared";
            } else {
                observation.detail = "watchdog retained PGDATA because the postmaster "
                                     "identity disappeared before complete-tree verification";
            }
            return observation;
        }
        std::vector<bioseq::postgres_test::supervised_process_identity> process_tree;
        if (active.postmaster_pid > 1 && !process_absent(active.postmaster_pid)) {
            std::optional<bioseq::postgres_test::supervised_process_identity> postmaster;
            if (!active.postmaster_start_token.empty() && !active.postmaster_executable.empty() &&
                active.postmaster_process_group > 1) {
                bioseq::postgres_test::supervised_process_identity recorded{
                    .pid = active.postmaster_pid,
                    .start_token = active.postmaster_start_token,
                    .executable = active.postmaster_executable,
                    .process_group = active.postmaster_process_group,
                    .data_directory = active.data_directory};
                if (bioseq::postgres_test::is_supervised_process_identity_current(recorded)) {
                    postmaster = std::move(recorded);
                }
            } else if (active.discover_postmaster_pid) {
                const auto discovered = bioseq::postgres_test::capture_supervised_process_identity(
                    active.postmaster_pid, active.postmaster_executable, active.data_directory);
                if (discovered &&
                    bioseq::postgres_test::is_supervised_postmaster_bound_to_data_directory(
                        *discovered)) {
                    postmaster = *discovered;
                }
            }
            if (!postmaster) {
                observation.detail = "watchdog refused a postmaster without a matching "
                                     "journaled identity or exact live PGDATA binding";
                return observation;
            }
            auto stable_tree = freeze_stable_supervised_tree(*postmaster);
            if (!stable_tree) {
                observation.detail = "watchdog could not freeze a stable complete "
                                     "postmaster descendant tree; all retained identities "
                                     "were resumed";
                return observation;
            }
            process_tree = std::move(*stable_tree);
            const auto current_postmaster = std::ranges::find_if(
                process_tree,
                [pid = active.postmaster_pid](const auto& member) { return member.pid == pid; });
            if (current_postmaster == process_tree.end()) {
                observation.detail = "watchdog stable tree omitted the postmaster";
                return observation;
            }
            const std::vector<std::int64_t> process_groups =
                supervised_process_groups(process_tree);
            signal_supervised_groups(process_tree, process_groups, SIGKILL);
            for (auto iterator = process_tree.rbegin(); iterator != process_tree.rend();
                 ++iterator) {
                if (bioseq::postgres_test::is_supervised_process_identity_current(*iterator)) {
                    static_cast<void>(::kill(static_cast<pid_t>(iterator->pid), SIGKILL));
                }
            }
            const auto force_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (!wait_for_supervised_members_absence(
                    process_tree, process_groups, force_deadline)) {
                resume_supervised_processes(process_tree);
                observation.detail = "watchdog could not stop every validated postmaster "
                                     "process-tree member; surviving identities were resumed";
                return observation;
            }
        }
        if (!allow_root_removal) {
            observation.detail = "watchdog retained the private-cluster root because "
                                 "registered lifecycle-process cleanup was incomplete";
            return observation;
        }
        std::error_code error;
        std::filesystem::remove_all(cluster.root, error);
        if (error) {
            observation.detail = "watchdog could not remove private root: " + error.message();
            return observation;
        }
        observation.succeeded = true;
        observation.detail = "external watchdog stopped the process tree and removed "
                             "the marker-owned root";
    } catch (const std::exception& error) {
        observation.detail = error.what();
    } catch (...) {
        observation.detail = "unknown external-watchdog cleanup failure";
    }
    return observation;
}

[[nodiscard]] bool
watchdog_cleanup_process(const bioseq::postgres_test::supervised_process_identity& process) {
    if (!marker_owned_data_directory(process.data_directory)) {
        return false;
    }
    if (!bioseq::postgres_test::is_supervised_process_identity_current(process)) {
        return process_absent(process.pid) &&
               bioseq::postgres_test::is_supervised_process_group_absent(process.process_group);
    }
    auto frozen = freeze_stable_supervised_tree(process);
    if (!frozen) {
        return false;
    }
    const auto& members = *frozen;
    const std::vector<std::int64_t> process_groups = supervised_process_groups(members);
    signal_supervised_groups(members, process_groups, SIGKILL);
    for (auto iterator = members.rbegin(); iterator != members.rend(); ++iterator) {
        if (bioseq::postgres_test::is_supervised_process_identity_current(*iterator)) {
            static_cast<void>(::kill(static_cast<pid_t>(iterator->pid), SIGKILL));
        }
    }
    const auto force_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    const bool absent =
        wait_for_supervised_members_absence(members, process_groups, force_deadline);
    if (!absent) {
        resume_supervised_processes(members);
    }
    return absent;
}

[[nodiscard]] int run_internal_watchdog(const std::filesystem::path& state_directory,
                                        const std::filesystem::path& report_path,
                                        std::string test_case,
                                        std::uint64_t parent_pid,
                                        std::chrono::seconds timeout,
                                        const std::filesystem::path& executable,
                                        std::string executable_sha256) {
    const bool parent_was_already_gone = ::getppid() != static_cast<pid_t>(parent_pid);
    const auto parent_identity =
        parent_was_already_gone
            ? std::optional<bioseq::postgres_test::supervised_process_identity>{}
            : bioseq::postgres_test::capture_supervised_process_identity(parent_pid, executable);
    if (!parent_identity && !parent_was_already_gone) {
        return 125;
    }
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + timeout;
    if (!bioseq::postgres_test::is_supervision_registry_empty(state_directory) ||
        !publish_watchdog_armed(state_directory, parent_pid)) {
        return 125;
    }
    bool parent_disappeared = parent_was_already_gone;
    while (std::chrono::steady_clock::now() < deadline) {
        if (bioseq::postgres_test::is_supervision_decision_claimed(state_directory)) {
            return 0;
        }
        if (!parent_identity ||
            !bioseq::postgres_test::is_supervised_process_identity_current(*parent_identity) ||
            ::getppid() != static_cast<pid_t>(parent_pid)) {
            parent_disappeared = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!bioseq::postgres_test::try_claim_supervision_decision(state_directory, "timeout")) {
        return 0;
    }
    if (!parent_disappeared && parent_identity &&
        !bioseq::postgres_test::is_supervised_process_identity_current(*parent_identity)) {
        parent_disappeared = true;
    }
    stopped_parent_fail_safe parent_fail_safe{parent_identity ? &*parent_identity : nullptr};
    if (!parent_disappeared && ::kill(static_cast<pid_t>(parent_pid), SIGSTOP) != 0) {
        if (errno == ESRCH) {
            parent_disappeared = true;
        } else {
            return 125;
        }
    } else if (!parent_disappeared) {
        parent_fail_safe.arm();
    }

    const auto live_registries =
        bioseq::postgres_test::load_supervision_registry_snapshot(state_directory);
    const std::vector<bioseq::postgres_test::supervised_cluster_identity> clusters =
        live_registries ? live_registries->clusters
                        : std::vector<bioseq::postgres_test::supervised_cluster_identity>{};
    bioseq::postgres_test::integration_resource_summary resources;
    resources.postgres = bioseq::postgres_test::load_supervision_resource_samples(state_directory);
    for (const auto& cluster : clusters) {
        std::uint64_t postmaster_pid = cluster.postmaster_pid;
        if (cluster.discover_postmaster_pid && postmaster_pid <= 1) {
            std::ifstream input(cluster.data_directory / "postmaster.pid");
            input >> postmaster_pid;
        }
        if (postmaster_pid > 1) {
            if (const auto rss =
                    bioseq::postgres_test::sample_postgres_tree_rss_bytes(postmaster_pid)) {
                ++resources.postgres.rss_samples;
                resources.postgres.peak_postmaster_process_tree_rss_bytes =
                    std::max(resources.postgres.peak_postmaster_process_tree_rss_bytes, *rss);
            } else {
                ++resources.postgres.rss_sampling_failures;
            }
        }
        if (const auto root_bytes = bioseq::postgres_test::measure_owned_root_bytes(cluster.root)) {
            ++resources.postgres.owned_root_samples;
            resources.postgres.peak_owned_root_bytes =
                std::max(resources.postgres.peak_owned_root_bytes, *root_bytes);
        } else {
            ++resources.postgres.owned_root_sampling_failures;
        }
    }
    resources.postgres.stage_boundaries.push_back(bioseq::postgres_test::resource_sample_boundary{
        .ordinal = resources.postgres.stage_boundaries.size(),
        .stage = parent_disappeared ? "runner-terminated" : "watchdog-timeout",
        .elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started),
        .postmaster_process_tree_rss_bytes =
            resources.postgres.peak_postmaster_process_tree_rss_bytes == 0
                ? std::optional<std::uint64_t>{}
                : std::optional<std::uint64_t>{resources.postgres
                                                   .peak_postmaster_process_tree_rss_bytes},
        .owned_root_bytes =
            resources.postgres.peak_owned_root_bytes == 0
                ? std::optional<std::uint64_t>{}
                : std::optional<std::uint64_t>{resources.postgres.peak_owned_root_bytes}});
    resources.peak_rss_bytes = resources.postgres.peak_postmaster_process_tree_rss_bytes;
    resources.observed_temp_bytes = resources.postgres.peak_owned_root_bytes;

    std::string evidence_recovery_detail;
    const auto note_recovery_failure = [&evidence_recovery_detail](std::string_view kind) {
        if (!evidence_recovery_detail.empty()) {
            evidence_recovery_detail += ", ";
        }
        evidence_recovery_detail += kind;
    };
    if (!live_registries) {
        note_recovery_failure("live-resource registries");
    }
    auto recovered_cases =
        bioseq::postgres_test::load_supervision_test_case_results(state_directory);
    if (!recovered_cases) {
        note_recovery_failure("cases");
    }
    auto recovered_stages = bioseq::postgres_test::load_supervision_stage_timings(state_directory);
    if (!recovered_stages) {
        note_recovery_failure("stages");
    }
    auto recovered_qualifications =
        bioseq::postgres_test::load_supervision_host_qualifications(state_directory);
    if (!recovered_qualifications) {
        note_recovery_failure("qualifications");
    }
    auto recovered_artifacts =
        bioseq::postgres_test::load_supervision_artifact_observations(state_directory);
    if (!recovered_artifacts) {
        note_recovery_failure("artifacts");
    }
    if (!file_sha256_unchanged(executable, executable_sha256)) {
        note_recovery_failure("integration executable path changed after runner startup");
    }

    std::vector<bioseq::postgres_test::test_case_result> cases =
        recovered_cases.value_or(std::vector<bioseq::postgres_test::test_case_result>{});
    std::erase_if(cases, [&test_case](const bioseq::postgres_test::test_case_result& report) {
        return report.test_case == test_case && report.section.empty();
    });
    std::string interruption_message =
        parent_disappeared ? "external watchdog observed an unexpected runner termination"
                           : "external watchdog exceeded the per-case timeout of " +
                                 std::to_string(timeout.count()) + " seconds";
    if (!evidence_recovery_detail.empty()) {
        interruption_message +=
            "; missing or invalid evidence snapshot(s): " + evidence_recovery_detail;
    }
    cases.push_back(bioseq::postgres_test::test_case_result{
        .test_case = test_case,
        .status = "infrastructure-inconclusive",
        .message = std::move(interruption_message),
        .duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started)});

    const std::vector<bioseq::postgres_test::supervised_process_identity> processes =
        live_registries ? live_registries->processes
                        : std::vector<bioseq::postgres_test::supervised_process_identity>{};
    bool lifecycle_cleanup_succeeded = live_registries.has_value();
    for (const auto& process : processes) {
        lifecycle_cleanup_succeeded =
            watchdog_cleanup_process(process) && lifecycle_cleanup_succeeded;
    }

    std::vector<bioseq::postgres_test::cleanup_observation> cleanup_observations;
    cleanup_observations.reserve(clusters.size());
    bool cleanup_succeeded = lifecycle_cleanup_succeeded;
    for (std::size_t index = 0; index < clusters.size(); ++index) {
        auto observation =
            watchdog_cleanup_cluster(clusters[index], index, lifecycle_cleanup_succeeded);
        observation.test_case = test_case;
        if (index == 0) {
            observation.resources = resources;
        }
        cleanup_succeeded = cleanup_succeeded && observation.succeeded;
        cleanup_observations.push_back(std::move(observation));
    }

    bioseq::postgres_test::integration_run_report emergency{
        .schema_revision = 2,
        .executable = executable.string(),
        .build_identity = executable_sha256,
        .cluster_configuration = {{"supervision", "out-of-process per-case watchdog"},
                                  {"termination", "bounded process-tree cleanup"},
                                  {"evidence-recovery",
                                   evidence_recovery_detail.empty()
                                       ? "all typed snapshots recovered"
                                       : "missing or invalid: " + evidence_recovery_detail},
                                  {"retained-supervision-state",
                                   cleanup_succeeded ? "none" : state_directory.string()}},
        .cases = std::move(cases),
        .stage_timings =
            recovered_stages.value_or(std::vector<bioseq::postgres_test::stage_timing>{}),
        .host_qualifications = recovered_qualifications.value_or(
            std::vector<bioseq::postgres_test::host_qualification_result>{}),
        .sql_transcripts = bioseq::postgres_test::load_supervision_sql_transcript(state_directory),
        .artifacts = recovered_artifacts.value_or(
            std::vector<bioseq::postgres_test::artifact_observation>{}),
        .cleanup_observations = std::move(cleanup_observations),
        .resources = std::move(resources),
        .cleanup = bioseq::postgres_test::cleanup_summary{
            .succeeded = cleanup_succeeded,
            .detail = cleanup_succeeded
                          ? (clusters.empty() ? "watchdog observed no registered private clusters"
                                              : "watchdog cleaned every registered private cluster")
                          : "watchdog cleanup was incomplete"}};
    try {
        bioseq::postgres_test::write_integration_run_report(report_path, emergency);
        if (!file_sha256_unchanged(executable, executable_sha256)) {
            parent_fail_safe.resolve();
            return 126;
        }
    } catch (...) {
        parent_fail_safe.resolve();
        return 126;
    }
    if (cleanup_succeeded) {
        static_cast<void>(bioseq::postgres_test::remove_supervision_state(state_directory));
    }
    parent_fail_safe.resolve();
    return 124;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 8 && std::string_view(argv[1]) == "--bioseq-internal-watchdog") {
        const auto parent_pid = parse_unsigned(argv[5]);
        const auto timeout = parse_unsigned(argv[6]);
        if (!parent_pid || *parent_pid <= 1 || !timeout || *timeout == 0 ||
            *parent_pid > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) ||
            *timeout > static_cast<std::uint64_t>(std::chrono::seconds::max().count())) {
            return 2;
        }
        try {
            return run_internal_watchdog(
                argv[2],
                argv[3],
                argv[4],
                *parent_pid,
                std::chrono::seconds(static_cast<std::chrono::seconds::rep>(*timeout)),
                current_executable_path(argv[0]),
                argv[7]);
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
    std::string report;
    int case_timeout_seconds = 480;

    auto cli =
        session.cli() |
        Catch::Clara::Opt(postgres_bin_dir,
                          "PATH")["--postgres-bin-dir"]("PostgreSQL executable directory") |
        Catch::Clara::Opt(postgres_stage_root,
                          "PATH")["--postgres-stage-root"]("preinstalled PostgreSQL stage root") |
        Catch::Clara::Opt(postgres_stage_manifest, "PATH")["--postgres-stage-manifest"](
            "preinstalled PostgreSQL stage manifest") |
        Catch::Clara::Opt(bioseq_only_postgres_bin_dir, "PATH")["--bioseq-only-postgres-bin-dir"](
            "Bioseq-only PostgreSQL executable directory") |
        Catch::Clara::Opt(bioseq_only_postgres_stage_root,
                          "PATH")["--bioseq-only-postgres-stage-root"](
            "cache-free Bioseq-only PostgreSQL stage root") |
        Catch::Clara::Opt(bioseq_only_postgres_stage_manifest,
                          "PATH")["--bioseq-only-postgres-stage-manifest"](
            "cache-free Bioseq-only PostgreSQL stage manifest") |
        Catch::Clara::Opt(cache_control, "PATH")["--cache-control"]("bioseq_cache control file") |
        Catch::Clara::Opt(cache_install_sql,
                          "PATH")["--cache-install-sql"]("bioseq_cache generated install SQL") |
        Catch::Clara::Opt(cache_dso,
                          "PATH")["--cache-dso"]("bioseq_cache DSO for the selected profile") |
        Catch::Clara::Opt(bioseq_control,
                          "PATH")["--bioseq-control"]("optional Bioseq control file") |
        Catch::Clara::Opt(bioseq_install_sql,
                          "PATH")["--bioseq-install-sql"]("optional Bioseq generated install SQL") |
        Catch::Clara::Opt(bioseq_dso, "PATH")["--bioseq-dso"]("optional Bioseq DSO") |
        Catch::Clara::Opt(provider_peer_test_dso, "PATH")["--provider-peer-test-dso"](
            "optional incompatible provider-peer test DSO") |
        Catch::Clara::Opt(report, "PATH")["--report"]("native JSON report path") |
        Catch::Clara::Opt(case_timeout_seconds,
                          "SECONDS")["--case-timeout-seconds"]("per-case watchdog");
    session.cli(cli);

    const int cli_result = session.applyCommandLine(argc, argv);
    if (cli_result != 0) {
        return cli_result;
    }
    const Catch::ConfigData& catch_config = session.configData();
    const bool nonexecuting_action = catch_config.listTests || catch_config.listTags ||
                                     catch_config.listReporters || catch_config.listListeners ||
                                     catch_config.showHelp || catch_config.libIdentify;
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
    if (postgres_bin_dir.empty() || postgres_stage_root.empty() ||
        postgres_stage_manifest.empty() || cache_control.empty() || cache_install_sql.empty() ||
        cache_dso.empty() || report.empty() || case_timeout_seconds <= 0) {
        return 2;
    }
    const bool any_bioseq =
        !bioseq_control.empty() || !bioseq_install_sql.empty() || !bioseq_dso.empty();
    const bool all_bioseq =
        !bioseq_control.empty() && !bioseq_install_sql.empty() && !bioseq_dso.empty();
    if (any_bioseq != all_bioseq) {
        return 2;
    }
    const bool any_bioseq_only_stage = !bioseq_only_postgres_bin_dir.empty() ||
                                       !bioseq_only_postgres_stage_root.empty() ||
                                       !bioseq_only_postgres_stage_manifest.empty();
    const bool all_bioseq_only_stage = !bioseq_only_postgres_bin_dir.empty() &&
                                       !bioseq_only_postgres_stage_root.empty() &&
                                       !bioseq_only_postgres_stage_manifest.empty();
    if (any_bioseq_only_stage != all_bioseq_only_stage || (all_bioseq_only_stage && !all_bioseq)) {
        return 2;
    }

    std::filesystem::path executed_executable;
    std::string executed_executable_sha256;
    try {
        executed_executable = current_executable_path(argv[0]);
        executed_executable_sha256 =
            bioseq::postgres_test::hash_sha256_file(executed_executable).render_hex();
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
        .case_timeout = std::chrono::seconds(case_timeout_seconds)};
    if (all_bioseq) {
        configuration.bioseq_control = bioseq_control;
        configuration.bioseq_install_sql = bioseq_install_sql;
        configuration.bioseq_dso = bioseq_dso;
    }
    if (all_bioseq_only_stage) {
        configuration.bioseq_only_postgres_bin_dir = bioseq_only_postgres_bin_dir;
        configuration.bioseq_only_postgres_stage_root = bioseq_only_postgres_stage_root;
        configuration.bioseq_only_postgres_stage_manifest = bioseq_only_postgres_stage_manifest;
    }
    if (!provider_peer_test_dso.empty()) {
        if (!all_bioseq) {
            return 2;
        }
        configuration.provider_peer_test_dso = provider_peer_test_dso;
    }
    bioseq::cache_test::set_runner_configuration(std::move(configuration));
    watchdog_launch() =
        watchdog_launch_configuration{.executable = executed_executable,
                                      .executable_sha256 = executed_executable_sha256,
                                      .report = std::filesystem::absolute(report)};
    bioseq::postgres_test::clear_process_sql_transcript();
    bioseq::postgres_test::clear_cleanup_observations();
    bioseq::postgres_test::clear_artifact_observations();
    bioseq::postgres_test::clear_stage_timings();
    bioseq::postgres_test::clear_host_qualifications();
    bioseq::postgres_test::clear_execution_case_context();
    bioseq::postgres_test::set_automatic_observation_collection_enabled(true);

    std::vector<bioseq::postgres_test::invocation_argument> arguments;
    arguments.reserve(static_cast<std::size_t>(argc));
    for (int index = 0; index < argc; ++index) {
        arguments.push_back(bioseq::postgres_test::invocation_argument{
            .name = "argv[" + std::to_string(index) + "]",
            .value = argv[index],
            .sensitive = false});
    }

    const int catch_result = session.run();
    bioseq::postgres_test::set_automatic_observation_collection_enabled(false);
    const bool cleanup_succeeded = bioseq::postgres_test::summarize_cleanup().succeeded &&
                                   catch_evidence().retained_watchdog_states.empty() &&
                                   catch_evidence().watchdog_state.empty() &&
                                   !catch_evidence().watchdog_protocol_failed &&
                                   !catch_evidence().watchdog_retention_recording_failed;
    try {
        write_report(report,
                     executed_executable,
                     executed_executable_sha256,
                     std::move(arguments),
                     bioseq::cache_test::configuration());
    } catch (...) {
        return catch_result == 0 ? 3 : catch_result;
    }
    if (catch_result != 0) {
        return catch_result;
    }
    return cleanup_succeeded ? 0 : 4;
}
