module;

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <limits>
#include <locale>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <utility>
#include <vector>

export module bioseq.test.postgres:report;

import bioseq.common.durable_file_io;
import bioseq.libpq.client;
import :artifact;
import :qualification;
import :resource_sampler;
import :supervision;

export namespace bioseq::postgres_test {

/// @brief Outcome of one SQL transcript entry.
enum class sql_transcript_status {
    succeeded,
    failed,
};

/// @brief Literal SQL plus bounded result/error evidence captured by a session.
struct sql_transcript_entry {
    std::uint64_t ordinal{};
    std::string session;
    std::string test_case;
    std::string section;
    std::string sql;
    sql_transcript_status status{sql_transcript_status::succeeded};
    std::string sql_state;
    std::string message;
    std::string command_tag;
    std::uint64_t row_count{};
    std::uint64_t column_count{};
    std::vector<std::vector<std::optional<std::string>>> sampled_rows;
    bool rows_truncated{};
    std::chrono::milliseconds duration{};
};

/// @brief Catch case/section associated with process-wide report observations.
struct execution_case_context {
    std::string test_case;
    std::string section;
};

} // namespace bioseq::postgres_test

namespace bioseq::postgres_test::report_state_detail {

struct context_state {
    std::mutex mutex;
    execution_case_context current;
};

[[nodiscard]] inline context_state& process_context() {
    static context_state state;
    return state;
}

[[nodiscard]] inline std::atomic_bool& access_automatic_collection_state() {
    static std::atomic_bool enabled{false};
    return enabled;
}

inline void append_crash_transcript(const sql_transcript_entry& entry) noexcept {
    try {
        const auto directory = supervision_state_directory();
        if (!directory) {
            return;
        }
        std::ofstream output(*directory / "sql-transcript.tsv", std::ios::app);
        if (!output) {
            return;
        }
        output << entry.ordinal << '\t' << supervision_hex_encode(entry.session) << '\t'
               << supervision_hex_encode(entry.test_case) << '\t'
               << supervision_hex_encode(entry.section) << '\t' << supervision_hex_encode(entry.sql)
               << '\t' << (entry.status == sql_transcript_status::succeeded ? "passed" : "failed")
               << '\t' << supervision_hex_encode(entry.sql_state) << '\t'
               << supervision_hex_encode(entry.message) << '\t'
               << supervision_hex_encode(entry.command_tag) << '\t' << entry.row_count << '\t'
               << entry.column_count << '\t' << entry.duration.count() << '\t'
               << (entry.rows_truncated ? 1 : 0) << '\n';
        output.flush();
    } catch (...) {
    }
}

} // namespace bioseq::postgres_test::report_state_detail

export namespace bioseq::postgres_test {

/// @brief Enables fixture-owned process evidence for an integration runner.
inline void set_automatic_observation_collection_enabled(bool enabled) noexcept {
    report_state_detail::access_automatic_collection_state().store(enabled,
                                                                   std::memory_order_relaxed);
}

/// @brief Returns whether fixtures should attach the process report collectors.
[[nodiscard]] inline bool is_automatic_observation_collection_enabled() noexcept {
    return report_state_detail::access_automatic_collection_state().load(std::memory_order_relaxed);
}

/// @brief Updates the active Catch context inherited by SQL/cleanup observers.
inline void set_execution_case_context(execution_case_context context) {
    auto& state = report_state_detail::process_context();
    std::scoped_lock lock(state.mutex);
    state.current = std::move(context);
}

/// @brief Clears the active Catch context outside test execution.
inline void clear_execution_case_context() noexcept {
    try {
        set_execution_case_context({});
    } catch (...) {
    }
}

/// @brief Returns a stable copy of the active process report context.
[[nodiscard]] inline execution_case_context current_execution_case_context() {
    auto& state = report_state_detail::process_context();
    std::scoped_lock lock(state.mutex);
    return state.current;
}

/// @brief Thread-safe executor/observer for literal SQL transcripts.
class sql_transcript : public bioseq::libpq::command_observer {
  public:
    /// @brief Executes SQL, records bounded rows/errors, and preserves exceptions.
    [[nodiscard]] bioseq::libpq::command_result execute(
        bioseq::libpq::client_connection& connection,
        std::string_view session,
        std::string_view sql,
        std::chrono::milliseconds deadline,
        bioseq::libpq::transaction_policy policy = bioseq::libpq::transaction_policy::require_idle,
        std::size_t max_sample_cells = 256,
        std::size_t max_sample_bytes = 64U * 1024U) {
        if (connection.observer() == this) {
            return connection.execute(sql, deadline, policy);
        }
        const auto started = std::chrono::steady_clock::now();
        try {
            bioseq::libpq::command_result result = connection.execute(sql, deadline, policy);
            append(success_entry(session,
                                 sql,
                                 result,
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - started),
                                 max_sample_cells,
                                 max_sample_bytes));
            return result;
        } catch (const bioseq::libpq::command_error& error) {
            append(failure_entry(session,
                                 sql,
                                 error,
                                 std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now() - started)));
            throw;
        } catch (const std::exception& error) {
            const execution_case_context context = current_execution_case_context();
            append(sql_transcript_entry{.session = std::string(session),
                                        .test_case = context.test_case,
                                        .section = context.section,
                                        .sql = std::string(sql),
                                        .status = sql_transcript_status::failed,
                                        .message = error.what(),
                                        .duration =
                                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                                std::chrono::steady_clock::now() - started)});
            throw;
        }
    }

    void command_succeeded(std::string_view context,
                           std::string_view sql,
                           const bioseq::libpq::command_result& outcome,
                           std::chrono::nanoseconds elapsed) noexcept override {
        try {
            append(success_entry(context,
                                 sql,
                                 outcome,
                                 elapsed,
                                 k_max_observed_sample_cells,
                                 k_max_observed_sample_bytes));
        } catch (...) {
        }
    }

    void command_failed(std::string_view context,
                        std::string_view sql,
                        const bioseq::libpq::command_error& error,
                        std::chrono::nanoseconds elapsed) noexcept override {
        try {
            append(failure_entry(context, sql, error, elapsed));
        } catch (...) {
        }
    }

    /// @brief Appends a manually constructed transcript entry.
    void append(sql_transcript_entry entry) {
        std::scoped_lock lock(mutex_);
        entry.ordinal = next_ordinal_++;
        report_state_detail::append_crash_transcript(entry);
        entries_.push_back(std::move(entry));
    }

    /// @brief Returns an ordinal-ordered snapshot.
    [[nodiscard]] std::vector<sql_transcript_entry> entries() const {
        std::scoped_lock lock(mutex_);
        return entries_;
    }

    /// @brief Clears transcript state before a new run.
    void clear() {
        std::scoped_lock lock(mutex_);
        entries_.clear();
        next_ordinal_ = 0;
    }

  private:
    static constexpr std::size_t k_max_observed_sample_cells = 256;
    static constexpr std::size_t k_max_observed_sample_bytes = 64U * 1024U;

    [[nodiscard]] static sql_transcript_entry
    success_entry(std::string_view session,
                  std::string_view sql,
                  const bioseq::libpq::command_result& result,
                  std::chrono::nanoseconds elapsed,
                  std::size_t max_sample_cells,
                  std::size_t max_sample_bytes) {
        const execution_case_context context = current_execution_case_context();
        sql_transcript_entry entry{
            .session = std::string(session),
            .test_case = context.test_case,
            .section = context.section,
            .sql = std::string(sql),
            .status = sql_transcript_status::succeeded,
            .command_tag = result.command_tag(),
            .row_count = static_cast<std::uint64_t>(result.row_count()),
            .column_count = static_cast<std::uint64_t>(result.column_count()),
            .duration = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)};
        std::size_t sampled_cells = 0;
        std::size_t sampled_bytes = 0;
        for (std::size_t row = 0; row < result.row_count(); ++row) {
            const std::size_t column_count = result.column_count();
            if (sampled_cells > max_sample_cells ||
                column_count > max_sample_cells - sampled_cells) {
                entry.rows_truncated = true;
                break;
            }
            std::size_t row_bytes = 0;
            bool row_exceeds_byte_limit = false;
            for (std::size_t column = 0; column < result.column_count(); ++column) {
                if (result.cell_is_null(row, column)) {
                    continue;
                }
                const std::size_t cell_bytes = result.text_value(row, column).size();
                if (row_bytes > max_sample_bytes || cell_bytes > max_sample_bytes - row_bytes) {
                    row_exceeds_byte_limit = true;
                    break;
                }
                row_bytes += cell_bytes;
            }
            if (row_exceeds_byte_limit || sampled_bytes > max_sample_bytes - row_bytes) {
                entry.rows_truncated = true;
                break;
            }
            std::vector<std::optional<std::string>> sampled_row;
            sampled_row.reserve(result.column_count());
            for (std::size_t column = 0; column < result.column_count(); ++column) {
                if (result.cell_is_null(row, column)) {
                    sampled_row.push_back(std::nullopt);
                } else {
                    sampled_row.emplace_back(result.text_value(row, column));
                }
                ++sampled_cells;
            }
            sampled_bytes += row_bytes;
            entry.sampled_rows.push_back(std::move(sampled_row));
        }
        return entry;
    }

    [[nodiscard]] static sql_transcript_entry
    failure_entry(std::string_view session,
                  std::string_view sql,
                  const bioseq::libpq::command_error& error,
                  std::chrono::nanoseconds elapsed) {
        const execution_case_context context = current_execution_case_context();
        return sql_transcript_entry{
            .session = std::string(session),
            .test_case = context.test_case,
            .section = context.section,
            .sql = std::string(sql),
            .status = sql_transcript_status::failed,
            .sql_state = std::string(error.sql_state()),
            .message = error.what(),
            .duration = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)};
    }

    mutable std::mutex mutex_;
    std::vector<sql_transcript_entry> entries_;
    std::uint64_t next_ordinal_{};
};

/// @brief Returns the run-owned collector installed by PostgreSQL fixtures.
[[nodiscard]] inline sql_transcript& process_sql_transcript() {
    static sql_transcript transcript;
    return transcript;
}

/// @brief Attaches process transcript capture to one test connection.
inline void attach_process_sql_transcript(bioseq::libpq::client_connection& connection,
                                          std::string_view session) noexcept {
    try {
        if (!is_automatic_observation_collection_enabled()) {
            return;
        }
        connection.set_observer(&process_sql_transcript(), std::string(session));
    } catch (...) {
    }
}

/// @brief Clears SQL evidence before a new native Catch session.
inline void clear_process_sql_transcript() {
    process_sql_transcript().clear();
}

/// @brief Returns the run's ordinal-ordered SQL evidence.
[[nodiscard]] inline std::vector<sql_transcript_entry> process_sql_transcript_entries() {
    return process_sql_transcript().entries();
}

/// @brief Truncates crash-resilient transcript evidence for one case.
inline void initialize_supervision_report_journals() noexcept {
    try {
        const auto directory = supervision_state_directory();
        if (!directory) {
            return;
        }
        std::ofstream output(*directory / "sql-transcript.tsv", std::ios::trunc);
        output.close();
        if (!output) {
            return;
        }
        for (const sql_transcript_entry& entry : process_sql_transcript_entries()) {
            report_state_detail::append_crash_transcript(entry);
        }
    } catch (...) {
    }
}

/// @brief Loads all complete transcript journal rows after a runner crash.
[[nodiscard]] inline std::vector<sql_transcript_entry>
load_supervision_sql_transcript(const std::filesystem::path& directory) {
    std::ifstream input(directory / "sql-transcript.tsv");
    std::vector<sql_transcript_entry> entries;
    std::string line;
    while (std::getline(input, line)) {
        std::vector<std::string_view> fields;
        std::size_t begin = 0;
        while (begin <= line.size()) {
            const std::size_t end = line.find('\t', begin);
            fields.push_back(std::string_view(line).substr(
                begin, end == std::string::npos ? std::string::npos : end - begin));
            if (end == std::string::npos) {
                break;
            }
            begin = end + 1U;
        }
        if (fields.size() != 13U) {
            continue;
        }
        const auto parse_u64 = [](std::string_view text) -> std::optional<std::uint64_t> {
            std::uint64_t value{};
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size()) {
                return std::nullopt;
            }
            return value;
        };
        const auto parse_i64 = [](std::string_view text) -> std::optional<std::int64_t> {
            std::int64_t value{};
            const auto [end, error] =
                std::from_chars(text.data(), text.data() + text.size(), value);
            if (error != std::errc{} || end != text.data() + text.size()) {
                return std::nullopt;
            }
            return value;
        };
        const auto ordinal = parse_u64(fields[0]);
        const auto session = supervision_hex_decode(fields[1]);
        const auto test_case = supervision_hex_decode(fields[2]);
        const auto section = supervision_hex_decode(fields[3]);
        const auto sql = supervision_hex_decode(fields[4]);
        const auto sql_state = supervision_hex_decode(fields[6]);
        const auto message = supervision_hex_decode(fields[7]);
        const auto command_tag = supervision_hex_decode(fields[8]);
        const auto row_count = parse_u64(fields[9]);
        const auto column_count = parse_u64(fields[10]);
        const auto duration = parse_i64(fields[11]);
        if (!ordinal || !session || !test_case || !section || !sql || !sql_state || !message ||
            !command_tag || !row_count || !column_count || !duration ||
            (fields[5] != "passed" && fields[5] != "failed")) {
            continue;
        }
        entries.push_back(sql_transcript_entry{.ordinal = *ordinal,
                                               .session = std::move(*session),
                                               .test_case = std::move(*test_case),
                                               .section = std::move(*section),
                                               .sql = std::move(*sql),
                                               .status = fields[5] == "passed"
                                                             ? sql_transcript_status::succeeded
                                                             : sql_transcript_status::failed,
                                               .sql_state = std::move(*sql_state),
                                               .message = std::move(*message),
                                               .command_tag = std::move(*command_tag),
                                               .row_count = *row_count,
                                               .column_count = *column_count,
                                               .rows_truncated = fields[12] == "1",
                                               .duration = std::chrono::milliseconds(*duration)});
    }
    return entries;
}

/// @brief One command-line argument after runner-owned sensitivity classification.
struct invocation_argument {
    std::string name;
    std::string value;
    bool sensitive{};
};

/// @brief Source-neutral Catch case/section outcome supplied by runner glue.
struct test_case_result {
    std::string test_case;
    std::string section;
    std::string status;
    std::string message;
    std::chrono::milliseconds duration{};
};

/// @brief Timed acceptance stage associated with the active Catch context.
struct stage_timing {
    std::uint64_t ordinal{};
    std::string test_case;
    std::string section;
    std::string stage;
    std::string status;
    std::chrono::milliseconds duration{};
};

/// @brief Artifact digest/path evidence embedded in the native report.
struct artifact_observation {
    std::string name;
    std::filesystem::path preinstalled_stage_root;
    std::filesystem::path stage_manifest;
    std::string stage_manifest_sha256;
    std::filesystem::path control_file;
    std::filesystem::path install_sql;
    std::filesystem::path dso;
    std::filesystem::path staged_control_file;
    std::filesystem::path staged_install_sql;
    std::filesystem::path staged_dso;
    std::string control_sha256;
    std::string install_sql_sha256;
    std::string dso_sha256;
    std::string staged_control_sha256;
};

/// @brief Resource peaks/sizes sampled by the native runner.
struct integration_resource_summary {
    std::uint64_t peak_rss_bytes{};
    std::uint64_t observed_temp_bytes{};
    resource_usage_samples postgres;
};

/// @brief No-throw teardown verdict supplied after cluster cleanup.
struct cleanup_summary {
    bool succeeded{};
    std::string detail;
};

/// @brief One case-associated private-cluster teardown observation.
struct cleanup_observation {
    std::uint64_t ordinal{};
    std::string test_case;
    std::string section;
    std::filesystem::path root;
    bool succeeded{};
    std::string detail;
    integration_resource_summary resources;
};

/// @brief Complete source-neutral JSON model for a native integration run.
struct integration_run_report {
    std::uint32_t schema_revision{1};
    std::string executable;
    std::string build_identity;
    std::vector<invocation_argument> arguments;
    std::vector<std::pair<std::string, std::string>> cluster_configuration;
    std::vector<test_case_result> cases;
    std::vector<stage_timing> stage_timings;
    std::vector<host_qualification_result> host_qualifications;
    std::vector<sql_transcript_entry> sql_transcripts;
    std::vector<artifact_observation> artifacts;
    std::vector<cleanup_observation> cleanup_observations;
    integration_resource_summary resources;
    cleanup_summary cleanup;
};

/// @brief Converts a staged extension inventory into report evidence.
[[nodiscard]] inline std::vector<artifact_observation>
collect_artifact_observations(const extension_stage_layout& stage) {
    std::vector<artifact_observation> items;
    items.reserve(stage.extensions.size());
    for (const staged_extension_files& artifact : stage.extensions) {
        items.push_back(artifact_observation{
            .name = artifact.name,
            .preinstalled_stage_root =
                stage.manifest_file ? stage.stage_root : std::filesystem::path{},
            .stage_manifest = stage.manifest_file.value_or(std::filesystem::path{}),
            .stage_manifest_sha256 =
                stage.manifest_digest
                    ? stage.manifest_digest->render_hex()
                    : std::string{},
            .control_file = artifact.source_control_file,
            .install_sql = artifact.source_install_sql,
            .dso = artifact.source_dso,
            .staged_control_file = artifact.staged_control_file,
            .staged_install_sql = artifact.staged_install_sql,
            .staged_dso = artifact.staged_dso,
            .control_sha256 = artifact.control_digest.render_hex(),
            .install_sql_sha256 = artifact.install_sql_digest.render_hex(),
            .dso_sha256 = artifact.dso_digest.render_hex(),
            .staged_control_sha256 = artifact.staged_control_digest.render_hex()});
    }
    return items;
}

} // namespace bioseq::postgres_test

namespace bioseq::postgres_test::report_snapshot_detail {

[[nodiscard]] std::vector<std::string_view> fields(std::string_view line) {
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    while (begin <= line.size()) {
        const std::size_t end = line.find('\t', begin);
        result.push_back(line.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return result;
}

template <class p_integer> [[nodiscard]] std::optional<p_integer> integer(std::string_view text) {
    p_integer value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] bool
parse_header(std::istringstream& input, std::string_view expected_kind, std::size_t& count) {
    std::string line;
    if (!std::getline(input, line)
        || line != "bioseq-watchdog"
        || !std::getline(input, line)
        || line != "revision=1"
        || !std::getline(input, line)) {
        return false;
    }
    const auto values = fields(line);
    const auto parsed_count = values.size() == 2U
        ? integer<std::size_t>(values[1]) : std::nullopt;
    if (values.size() != 2U || values[0] != expected_kind ||
        (values[1].size() > 1U && values[1].front() == '0') ||
        !parsed_count || *parsed_count > 1'000'000U) {
        return false;
    }
    count = *parsed_count;
    return true;
}

template <class p_range, class p_writer>
[[nodiscard]] bool publish(std::string_view file,
                           std::string_view kind,
                           const p_range& records,
                           p_writer&& writer) noexcept {
    try {
        std::ostringstream output;
        output << "bioseq-watchdog\nrevision=1\n"
               << kind << '\t' << records.size() << '\n';
        for (const auto& record : records) {
            std::invoke(writer, output, record);
            output << '\n';
        }
        return publish_supervision_snapshot(file, output.str());
    } catch (...) {
        return false;
    }
}

} // namespace bioseq::postgres_test::report_snapshot_detail

export namespace bioseq::postgres_test {

/// @brief Durably replaces the watchdog snapshot of native test-case results.
[[nodiscard]] inline bool
publish_supervision_test_case_results(std::span<const test_case_result> items) noexcept {
    return report_snapshot_detail::publish(
        "cases.snapshot.tsv",
        "cases",
        items,
        [](std::ostream& output, const test_case_result& report) {
            output << supervision_hex_encode(report.test_case) << '\t'
                   << supervision_hex_encode(report.section) << '\t'
                   << supervision_hex_encode(report.status) << '\t'
                   << supervision_hex_encode(report.message) << '\t' << report.duration.count();
        });
}

/// @brief Loads one complete, bounded test-case snapshot or returns nullopt.
[[nodiscard]] inline std::optional<std::vector<test_case_result>>
load_supervision_test_case_results(const std::filesystem::path& directory) noexcept {
    try {
        const auto payload = load_supervision_snapshot(directory, "cases.snapshot.tsv");
        if (!payload || payload->empty() || payload->back() != '\n') {
            return std::nullopt;
        }
        std::istringstream input(*payload);
        std::size_t count{};
        if (!report_snapshot_detail::parse_header(input, "cases", count)) {
            return std::nullopt;
        }
        std::vector<test_case_result> items;
        items.reserve(count);
        std::string line;
        for (std::size_t index = 0; index < count; ++index) {
            if (!std::getline(input, line)) {
                return std::nullopt;
            }
            const auto values = report_snapshot_detail::fields(line);
            if (values.size() != 5U) {
                return std::nullopt;
            }
            const auto test_case = supervision_hex_decode(values[0]);
            const auto section = supervision_hex_decode(values[1]);
            const auto status = supervision_hex_decode(values[2]);
            const auto message = supervision_hex_decode(values[3]);
            const auto duration = report_snapshot_detail::integer<std::int64_t>(values[4]);
            if (!test_case || !section || !status || !message || !duration || *duration < 0) {
                return std::nullopt;
            }
            items.push_back(test_case_result{.test_case = std::move(*test_case),
                                             .section = std::move(*section),
                                             .status = std::move(*status),
                                             .message = std::move(*message),
                                             .duration = std::chrono::milliseconds(*duration)});
        }
        if (std::getline(input, line)) {
            return std::nullopt;
        }
        return items;
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Durably replaces the watchdog snapshot of completed stage timings.
[[nodiscard]] inline bool
publish_supervision_stage_timings(std::span<const stage_timing> items) noexcept {
    return report_snapshot_detail::publish("stages.snapshot.tsv",
                                           "stages",
                                           items,
                                           [](std::ostream& output, const stage_timing& report) {
                                               output
                                                   << report.ordinal << '\t'
                                                   << supervision_hex_encode(report.test_case)
                                                   << '\t' << supervision_hex_encode(report.section)
                                                   << '\t' << supervision_hex_encode(report.stage)
                                                   << '\t' << supervision_hex_encode(report.status)
                                                   << '\t' << report.duration.count();
                                           });
}

/// @brief Loads one complete, bounded stage-timing snapshot or returns nullopt.
[[nodiscard]] inline std::optional<std::vector<stage_timing>>
load_supervision_stage_timings(const std::filesystem::path& directory) noexcept {
    try {
        const auto payload = load_supervision_snapshot(directory, "stages.snapshot.tsv");
        if (!payload || payload->empty() || payload->back() != '\n') {
            return std::nullopt;
        }
        std::istringstream input(*payload);
        std::size_t count{};
        if (!report_snapshot_detail::parse_header(input, "stages", count)) {
            return std::nullopt;
        }
        std::vector<stage_timing> items;
        items.reserve(count);
        std::string line;
        for (std::size_t index = 0; index < count; ++index) {
            if (!std::getline(input, line)) {
                return std::nullopt;
            }
            const auto values = report_snapshot_detail::fields(line);
            if (values.size() != 6U) {
                return std::nullopt;
            }
            const auto ordinal = report_snapshot_detail::integer<std::uint64_t>(values[0]);
            const auto test_case = supervision_hex_decode(values[1]);
            const auto section = supervision_hex_decode(values[2]);
            const auto stage = supervision_hex_decode(values[3]);
            const auto status = supervision_hex_decode(values[4]);
            const auto duration = report_snapshot_detail::integer<std::int64_t>(values[5]);
            if (!ordinal || !test_case || !section || !stage || !status || !duration ||
                *duration < 0) {
                return std::nullopt;
            }
            items.push_back(stage_timing{.ordinal = *ordinal,
                                         .test_case = std::move(*test_case),
                                         .section = std::move(*section),
                                         .stage = std::move(*stage),
                                         .status = std::move(*status),
                                         .duration = std::chrono::milliseconds(*duration)});
        }
        if (std::getline(input, line)) {
            return std::nullopt;
        }
        return items;
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Durably replaces the watchdog snapshot of staged artifacts.
[[nodiscard]] inline bool
publish_supervision_artifact_observations(std::span<const artifact_observation> items) noexcept {
    return report_snapshot_detail::publish(
        "artifacts.snapshot.tsv",
        "artifacts",
        items,
        [](std::ostream& output, const artifact_observation& report) {
            output << supervision_hex_encode(report.name) << '\t'
                   << supervision_hex_encode(report.preinstalled_stage_root.string()) << '\t'
                   << supervision_hex_encode(report.stage_manifest.string()) << '\t'
                   << supervision_hex_encode(report.stage_manifest_sha256) << '\t'
                   << supervision_hex_encode(report.control_file.string()) << '\t'
                   << supervision_hex_encode(report.install_sql.string()) << '\t'
                   << supervision_hex_encode(report.dso.string()) << '\t'
                   << supervision_hex_encode(report.staged_control_file.string()) << '\t'
                   << supervision_hex_encode(report.staged_install_sql.string()) << '\t'
                   << supervision_hex_encode(report.staged_dso.string()) << '\t'
                   << supervision_hex_encode(report.control_sha256) << '\t'
                   << supervision_hex_encode(report.install_sql_sha256) << '\t'
                   << supervision_hex_encode(report.dso_sha256) << '\t'
                   << supervision_hex_encode(report.staged_control_sha256);
        });
}

/// @brief Loads one complete, bounded artifact snapshot or returns nullopt.
[[nodiscard]] inline std::optional<std::vector<artifact_observation>>
load_supervision_artifact_observations(const std::filesystem::path& directory) noexcept {
    try {
        const auto payload = load_supervision_snapshot(directory, "artifacts.snapshot.tsv");
        if (!payload || payload->empty() || payload->back() != '\n') {
            return std::nullopt;
        }
        std::istringstream input(*payload);
        std::size_t count{};
        if (!report_snapshot_detail::parse_header(input, "artifacts", count)) {
            return std::nullopt;
        }
        std::vector<artifact_observation> items;
        items.reserve(count);
        std::string line;
        for (std::size_t index = 0; index < count; ++index) {
            if (!std::getline(input, line)) {
                return std::nullopt;
            }
            const auto values = report_snapshot_detail::fields(line);
            if (values.size() != 14U) {
                return std::nullopt;
            }
            std::vector<std::string> decoded;
            decoded.reserve(values.size());
            for (const std::string_view value : values) {
                auto field = supervision_hex_decode(value);
                if (!field) {
                    return std::nullopt;
                }
                decoded.push_back(std::move(*field));
            }
            items.push_back(artifact_observation{.name = std::move(decoded[0]),
                                                 .preinstalled_stage_root = std::move(decoded[1]),
                                                 .stage_manifest = std::move(decoded[2]),
                                                 .stage_manifest_sha256 = std::move(decoded[3]),
                                                 .control_file = std::move(decoded[4]),
                                                 .install_sql = std::move(decoded[5]),
                                                 .dso = std::move(decoded[6]),
                                                 .staged_control_file = std::move(decoded[7]),
                                                 .staged_install_sql = std::move(decoded[8]),
                                                 .staged_dso = std::move(decoded[9]),
                                                 .control_sha256 = std::move(decoded[10]),
                                                 .install_sql_sha256 = std::move(decoded[11]),
                                                 .dso_sha256 = std::move(decoded[12]),
                                                 .staged_control_sha256 = std::move(decoded[13])});
        }
        if (std::getline(input, line)) {
            return std::nullopt;
        }
        return items;
    } catch (...) {
        return std::nullopt;
    }
}

/// @brief Samples runner peak RSS and exact bytes below selected temp roots.
[[nodiscard]] inline integration_resource_summary
capture_integration_resources(std::span<const std::filesystem::path> temp_roots = {}) {
    integration_resource_summary report;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        report.peak_rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
        report.peak_rss_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
    }
    for (const std::filesystem::path& root : temp_roots) {
        std::error_code error;
        if (!std::filesystem::exists(root, error) || error) {
            continue;
        }
        std::filesystem::recursive_directory_iterator iterator(
            root, std::filesystem::directory_options::skip_permission_denied, error);
        const std::filesystem::recursive_directory_iterator end;
        while (!error && iterator != end) {
            if (iterator->is_regular_file(error) && !error) {
                const std::uintmax_t bytes = iterator->file_size(error);
                if (!error) {
                    report.observed_temp_bytes += bytes;
                }
            }
            if (error) {
                error.clear();
            }
            iterator.increment(error);
        }
    }
    return report;
}

} // namespace bioseq::postgres_test

namespace bioseq::postgres_test::report_state_detail {

struct cleanup_state {
    std::mutex mutex;
    std::vector<cleanup_observation> observations;
    std::uint64_t next_ordinal{};
};

struct artifact_state {
    std::mutex mutex;
    std::vector<artifact_observation> items;
};

struct stage_state {
    std::mutex mutex;
    std::vector<stage_timing> items;
    std::uint64_t next_ordinal{};
};

[[nodiscard]] inline cleanup_state& process_cleanup() {
    static cleanup_state state;
    return state;
}

[[nodiscard]] inline artifact_state& process_artifacts() {
    static artifact_state state;
    return state;
}

[[nodiscard]] inline stage_state& process_stages() {
    static stage_state state;
    return state;
}

} // namespace bioseq::postgres_test::report_state_detail

export namespace bioseq::postgres_test {

/// @brief Records one completed or interrupted timed stage.
inline void
record_stage_timing(std::string_view stage,
                    std::chrono::milliseconds duration,
                    bool completed,
                    execution_case_context context = current_execution_case_context()) noexcept {
    try {
        auto& state = report_state_detail::process_stages();
        std::scoped_lock lock(state.mutex);
        state.items.push_back(stage_timing{.ordinal = state.next_ordinal++,
                                           .test_case = std::move(context.test_case),
                                           .section = std::move(context.section),
                                           .stage = std::string(stage),
                                           .status = completed ? "completed" : "interrupted",
                                           .duration = duration});
        static_cast<void>(publish_supervision_stage_timings(state.items));
    } catch (...) {
    }
}

/// @brief Clears stage timing evidence before a native Catch session.
inline void clear_stage_timings() noexcept {
    try {
        auto& state = report_state_detail::process_stages();
        std::scoped_lock lock(state.mutex);
        state.items.clear();
        state.next_ordinal = 0;
    } catch (...) {
    }
}

/// @brief Returns stage timings in observation order.
[[nodiscard]] inline std::vector<stage_timing> stage_timings() {
    auto& state = report_state_detail::process_stages();
    std::scoped_lock lock(state.mutex);
    return state.items;
}

/// @brief RAII stage timer that records interruption during unwinding.
class scoped_stage_timing {
  public:
    explicit scoped_stage_timing(std::string stage)
        : stage_(std::move(stage)), context_(current_execution_case_context()),
          started_(std::chrono::steady_clock::now()) {}

    scoped_stage_timing(const scoped_stage_timing&) = delete;
    scoped_stage_timing& operator=(const scoped_stage_timing&) = delete;

    ~scoped_stage_timing() noexcept {
        record_stage_timing(stage_,
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - started_),
                            completed_,
                            std::move(context_));
    }

    /// @brief Marks the timed stage as normally completed.
    void mark_completed() noexcept {
        completed_ = true;
    }

  private:
    std::string stage_;
    execution_case_context context_;
    std::chrono::steady_clock::time_point started_;
    bool completed_{};
};

/// @brief Records exact source/staged artifact evidence for one fixture.
inline void record_artifact_observations(const extension_stage_layout& stage) noexcept {
    try {
        std::vector<artifact_observation> items = collect_artifact_observations(stage);
        auto& state = report_state_detail::process_artifacts();
        std::scoped_lock lock(state.mutex);
        state.items.insert(state.items.end(),
                           std::make_move_iterator(items.begin()),
                           std::make_move_iterator(items.end()));
        static_cast<void>(publish_supervision_artifact_observations(state.items));
    } catch (...) {
    }
}

/// @brief Clears staged-artifact evidence before a native Catch session.
inline void clear_artifact_observations() noexcept {
    try {
        auto& state = report_state_detail::process_artifacts();
        std::scoped_lock lock(state.mutex);
        state.items.clear();
    } catch (...) {
    }
}

/// @brief Returns source/staged artifact evidence in fixture order.
[[nodiscard]] inline std::vector<artifact_observation> artifact_observations() {
    auto& state = report_state_detail::process_artifacts();
    std::scoped_lock lock(state.mutex);
    return state.items;
}

/// @brief Records one no-throw private-cluster cleanup and resource sample.
inline void record_cleanup_observation(const std::filesystem::path& root,
                                       bool succeeded,
                                       std::string_view detail,
                                       integration_resource_summary resources = {}) noexcept {
    try {
        const execution_case_context context = current_execution_case_context();
        auto& state = report_state_detail::process_cleanup();
        std::scoped_lock lock(state.mutex);
        state.observations.push_back(cleanup_observation{.ordinal = state.next_ordinal++,
                                                         .test_case = context.test_case,
                                                         .section = context.section,
                                                         .root = root,
                                                         .succeeded = succeeded,
                                                         .detail = std::string(detail),
                                                         .resources = resources});
    } catch (...) {
    }
}

/// @brief Clears teardown evidence before a new native Catch session.
inline void clear_cleanup_observations() noexcept {
    try {
        auto& state = report_state_detail::process_cleanup();
        std::scoped_lock lock(state.mutex);
        state.observations.clear();
        state.next_ordinal = 0;
    } catch (...) {
    }
}

/// @brief Returns an ordinal-ordered cleanup snapshot.
[[nodiscard]] inline std::vector<cleanup_observation> cleanup_observations() {
    auto& state = report_state_detail::process_cleanup();
    std::scoped_lock lock(state.mutex);
    return state.observations;
}

/// @brief Aggregates all observed fixture teardowns into one run verdict.
[[nodiscard]] inline cleanup_summary summarize_cleanup() {
    const std::vector<cleanup_observation> observations = cleanup_observations();
    if (observations.empty()) {
        return cleanup_summary{.succeeded = false,
                               .detail = "no private-cluster cleanup observation was recorded"};
    }
    std::size_t failed = 0;
    std::string failures;
    for (const cleanup_observation& observation : observations) {
        if (observation.succeeded) {
            continue;
        }
        ++failed;
        if (!failures.empty()) {
            failures += "; ";
        }
        failures +=
            observation.test_case.empty() ? observation.root.string() : observation.test_case;
        if (!observation.detail.empty()) {
            failures += ": " + observation.detail;
        }
    }
    if (failed != 0) {
        return cleanup_summary{.succeeded = false,
                               .detail = std::to_string(failed) +
                                         " private-cluster cleanup(s) failed: " + failures};
    }
    return cleanup_summary{.succeeded = true,
                           .detail = std::to_string(observations.size()) +
                                     " private-cluster cleanup(s) completed"};
}

/// @brief Combines client peak RSS with the largest observed fixture tree.
[[nodiscard]] inline integration_resource_summary summarize_resources() {
    integration_resource_summary aggregate = capture_integration_resources();
    for (const cleanup_observation& observation : cleanup_observations()) {
        aggregate.peak_rss_bytes =
            std::max(aggregate.peak_rss_bytes, observation.resources.peak_rss_bytes);
        aggregate.observed_temp_bytes =
            std::max(aggregate.observed_temp_bytes, observation.resources.observed_temp_bytes);
        aggregate.postgres.peak_postmaster_process_tree_rss_bytes =
            std::max(aggregate.postgres.peak_postmaster_process_tree_rss_bytes,
                     observation.resources.postgres.peak_postmaster_process_tree_rss_bytes);
        aggregate.postgres.peak_owned_root_bytes =
            std::max(aggregate.postgres.peak_owned_root_bytes,
                     observation.resources.postgres.peak_owned_root_bytes);
        aggregate.postgres.rss_samples += observation.resources.postgres.rss_samples;
        aggregate.postgres.owned_root_samples += observation.resources.postgres.owned_root_samples;
        aggregate.postgres.rss_sampling_failures +=
            observation.resources.postgres.rss_sampling_failures;
        aggregate.postgres.owned_root_sampling_failures +=
            observation.resources.postgres.owned_root_sampling_failures;
        auto boundaries = observation.resources.postgres.stage_boundaries;
        for (resource_sample_boundary& boundary : boundaries) {
            boundary.ordinal = aggregate.postgres.stage_boundaries.size();
            aggregate.postgres.stage_boundaries.push_back(std::move(boundary));
        }
    }
    return aggregate;
}

} // namespace bioseq::postgres_test

namespace bioseq::postgres_test::report_detail {

/// @brief Writes one correctly escaped JSON string.
inline void write_string(std::ostream& output, std::string_view value) {
    constexpr std::string_view k_hex{"0123456789abcdef"};
    output.put('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20U) {
                output << "\\u00" << k_hex[(character >> 4U) & 0x0fU] << k_hex[character & 0x0fU];
            } else {
                output.put(static_cast<char>(character));
            }
        }
    }
    output.put('"');
}

inline void key(std::ostream& output, std::string_view name) {
    write_string(output, name);
    output.put(':');
}

inline void comma(std::ostream& output, bool& first) {
    if (!first) {
        output.put(',');
    }
    first = false;
}

inline void write_transcript(std::ostream& output, const sql_transcript_entry& entry) {
    output.put('{');
    bool first = true;
    const auto string_field = [&output, &first](std::string_view name, std::string_view value) {
        comma(output, first);
        key(output, name);
        write_string(output, value);
    };
    const auto integer_field = [&output, &first](std::string_view name, std::int64_t value) {
        comma(output, first);
        key(output, name);
        output << value;
    };
    integer_field("ordinal", static_cast<std::int64_t>(entry.ordinal));
    string_field("session", entry.session);
    string_field("test_case", entry.test_case);
    string_field("section", entry.section);
    string_field("sql", entry.sql);
    string_field("status", entry.status == sql_transcript_status::succeeded ? "passed" : "failed");
    string_field("sql_state", entry.sql_state);
    string_field("message", entry.message);
    string_field("command_tag", entry.command_tag);
    integer_field("row_count", entry.row_count);
    integer_field("column_count", entry.column_count);
    integer_field("duration_ms", entry.duration.count());
    comma(output, first);
    key(output, "rows_truncated");
    output << (entry.rows_truncated ? "true" : "false");
    comma(output, first);
    key(output, "sampled_rows");
    output.put('[');
    bool first_row = true;
    for (const auto& row : entry.sampled_rows) {
        comma(output, first_row);
        output.put('[');
        bool first_cell = true;
        for (const auto& cell : row) {
            comma(output, first_cell);
            if (cell) {
                write_string(output, *cell);
            } else {
                output << "null";
            }
        }
        output.put(']');
    }
    output.put(']');
    output.put('}');
}

inline void write_resource_sampling(std::ostream& output, const resource_usage_samples& report) {
    output.put('{');
    key(output, "peak_postmaster_process_tree_rss_bytes");
    output << report.peak_postmaster_process_tree_rss_bytes;
    output.put(',');
    key(output, "peak_owned_root_bytes");
    output << report.peak_owned_root_bytes;
    output.put(',');
    key(output, "rss_samples");
    output << report.rss_samples;
    output.put(',');
    key(output, "owned_root_samples");
    output << report.owned_root_samples;
    output.put(',');
    key(output, "rss_sampling_failures");
    output << report.rss_sampling_failures;
    output.put(',');
    key(output, "owned_root_sampling_failures");
    output << report.owned_root_sampling_failures;
    output.put(',');
    key(output, "stage_boundaries");
    output.put('[');
    bool first = true;
    for (const resource_sample_boundary& boundary : report.stage_boundaries) {
        comma(output, first);
        output.put('{');
        key(output, "ordinal");
        output << boundary.ordinal;
        output.put(',');
        key(output, "stage");
        write_string(output, boundary.stage);
        output.put(',');
        key(output, "elapsed_ms");
        output << boundary.elapsed.count();
        output.put(',');
        key(output, "postmaster_process_tree_rss_bytes");
        if (boundary.postmaster_process_tree_rss_bytes) {
            output << *boundary.postmaster_process_tree_rss_bytes;
        } else {
            output << "null";
        }
        output.put(',');
        key(output, "owned_root_bytes");
        if (boundary.owned_root_bytes) {
            output << *boundary.owned_root_bytes;
        } else {
            output << "null";
        }
        output.put('}');
    }
    output.put(']');
    output.put('}');
}

} // namespace bioseq::postgres_test::report_detail

export namespace bioseq::postgres_test {

/// @brief Atomically and durably publishes a native integration JSON report.
inline void write_integration_run_report(const std::filesystem::path& path,
                                         const integration_run_report& report) {
    const std::filesystem::path absolute_path = std::filesystem::absolute(path);
    const auto directories_created =
        bioseq::common::durable_file_io::durable_create_directories(absolute_path.parent_path());
    if (!directories_created.has_value()) {
        const bioseq::common::durable_file_io::file_error& error = directories_created.error();
        throw std::runtime_error("cannot durably create integration report directory: operation " +
                                 std::to_string(static_cast<unsigned>(error.operation)) +
                                 ", code " + std::to_string(static_cast<unsigned>(error.code)) +
                                 ", system error " + std::to_string(error.system_error) +
                                 ", publication " +
                                 std::to_string(static_cast<unsigned>(error.publication)));
    }
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10);
    output.put('{');
    bool first = true;
    const auto string_field = [&output, &first](std::string_view name, std::string_view value) {
        report_detail::comma(output, first);
        report_detail::key(output, name);
        report_detail::write_string(output, value);
    };
    report_detail::comma(output, first);
    report_detail::key(output, "schema_revision");
    output << report.schema_revision;
    string_field("executable", report.executable);
    string_field("build_identity", report.build_identity);

    report_detail::comma(output, first);
    report_detail::key(output, "arguments");
    output.put('[');
    bool first_argument = true;
    for (const invocation_argument& argument : report.arguments) {
        report_detail::comma(output, first_argument);
        output.put('{');
        report_detail::key(output, "name");
        report_detail::write_string(output, argument.name);
        output.put(',');
        report_detail::key(output, "value");
        report_detail::write_string(output, argument.sensitive ? "<redacted>" : argument.value);
        output.put(',');
        report_detail::key(output, "redacted");
        output << (argument.sensitive ? "true" : "false");
        output.put('}');
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "cluster_configuration");
    output.put('{');
    bool first_configuration = true;
    for (const auto& [name, value] : report.cluster_configuration) {
        report_detail::comma(output, first_configuration);
        report_detail::key(output, name);
        report_detail::write_string(output, value);
    }
    output.put('}');

    report_detail::comma(output, first);
    report_detail::key(output, "cases");
    output.put('[');
    bool first_case = true;
    for (const test_case_result& test_case : report.cases) {
        report_detail::comma(output, first_case);
        output.put('{');
        report_detail::key(output, "test_case");
        report_detail::write_string(output, test_case.test_case);
        output.put(',');
        report_detail::key(output, "section");
        report_detail::write_string(output, test_case.section);
        output.put(',');
        report_detail::key(output, "status");
        report_detail::write_string(output, test_case.status);
        output.put(',');
        report_detail::key(output, "message");
        report_detail::write_string(output, test_case.message);
        output.put(',');
        report_detail::key(output, "duration_ms");
        output << test_case.duration.count();
        output.put('}');
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "stage_timings");
    output.put('[');
    bool first_stage = true;
    for (const stage_timing& stage : report.stage_timings) {
        report_detail::comma(output, first_stage);
        output.put('{');
        report_detail::key(output, "ordinal");
        output << stage.ordinal;
        output.put(',');
        report_detail::key(output, "test_case");
        report_detail::write_string(output, stage.test_case);
        output.put(',');
        report_detail::key(output, "section");
        report_detail::write_string(output, stage.section);
        output.put(',');
        report_detail::key(output, "stage");
        report_detail::write_string(output, stage.stage);
        output.put(',');
        report_detail::key(output, "status");
        report_detail::write_string(output, stage.status);
        output.put(',');
        report_detail::key(output, "duration_ms");
        output << stage.duration.count();
        output.put('}');
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "host_qualifications");
    output.put('[');
    bool first_qualification = true;
    for (const host_qualification_result& qualification : report.host_qualifications) {
        report_detail::comma(output, first_qualification);
        output.put('{');
        report_detail::key(output, "ordinal");
        output << qualification.ordinal;
        output.put(',');
        report_detail::key(output, "test_case");
        report_detail::write_string(output, qualification.test_case);
        output.put(',');
        report_detail::key(output, "status");
        report_detail::write_string(
            output,
            qualification.status == host_qualification_status::passed ? "passed" : "inconclusive");
        output.put(',');
        report_detail::key(output, "detail");
        report_detail::write_string(output, qualification.detail);
        output.put(',');
        report_detail::key(output, "host_total_ram_bytes");
        output << qualification.host_total_ram_bytes;
        output.put(',');
        report_detail::key(output, "host_available_ram_bytes");
        output << qualification.host_available_ram_bytes;
        output.put(',');
        report_detail::key(output, "effective_total_ram_bytes");
        output << qualification.effective_total_ram_bytes;
        output.put(',');
        report_detail::key(output, "effective_available_ram_bytes");
        output << qualification.effective_available_ram_bytes;
        output.put(',');
        report_detail::key(output, "cgroup_memory_total_limit_bytes");
        output << qualification.cgroup_memory_total_limit_bytes;
        output.put(',');
        report_detail::key(output, "cgroup_memory_headroom_limit_bytes");
        output << qualification.cgroup_memory_headroom_limit_bytes;
        output.put(',');
        report_detail::key(output, "cgroup_memory_current_bytes");
        output << qualification.cgroup_memory_current_bytes;
        output.put(',');
        report_detail::key(output, "cgroup_memory_headroom_bytes");
        output << qualification.cgroup_memory_headroom_bytes;
        output.put(',');
        report_detail::key(output, "cgroup_memory_version");
        report_detail::write_string(output, qualification.cgroup_memory_version);
        output.put(',');
        report_detail::key(output, "cgroup_memory_total_source");
        report_detail::write_string(output, qualification.cgroup_memory_total_source);
        output.put(',');
        report_detail::key(output, "cgroup_memory_headroom_source");
        report_detail::write_string(output, qualification.cgroup_memory_headroom_source);
        output.put(',');
        report_detail::key(output, "host_logical_cores");
        output << qualification.host_logical_cores;
        output.put(',');
        report_detail::key(output, "affinity_logical_cores");
        output << qualification.affinity_logical_cores;
        output.put(',');
        report_detail::key(output, "cpuset_logical_cores");
        output << qualification.cpuset_logical_cores;
        output.put(',');
        report_detail::key(output, "cgroup_cpu_quota_microseconds");
        output << qualification.cgroup_cpu_quota_microseconds;
        output.put(',');
        report_detail::key(output, "cgroup_cpu_period_microseconds");
        output << qualification.cgroup_cpu_period_microseconds;
        output.put(',');
        report_detail::key(output, "cgroup_cpu_quota_cores");
        output << qualification.cgroup_cpu_quota_cores;
        output.put(',');
        report_detail::key(output, "effective_cpu_cores");
        output << qualification.effective_cpu_cores;
        output.put(',');
        report_detail::key(output, "cgroup_cpu_version");
        report_detail::write_string(output, qualification.cgroup_cpu_version);
        output.put(',');
        report_detail::key(output, "cgroup_cpu_source");
        report_detail::write_string(output, qualification.cgroup_cpu_source);
        output.put(',');
        report_detail::key(output, "required_total_ram_bytes");
        output << qualification.required_total_ram_bytes;
        output.put(',');
        report_detail::key(output, "required_available_ram_bytes");
        output << qualification.required_available_ram_bytes;
        output.put(',');
        report_detail::key(output, "required_logical_cores");
        output << qualification.required_logical_cores;
        output.put(',');
        report_detail::key(output, "required_io_mib_per_second");
        output << qualification.required_io_mib_per_second;
        output.put(',');
        report_detail::key(output, "duration_ms");
        output << qualification.duration.count();
        output.put(',');
        report_detail::key(output, "filesystems");
        output.put('[');
        bool first_filesystem = true;
        for (const filesystem_qualification_result& filesystem : qualification.filesystems) {
            report_detail::comma(output, first_filesystem);
            output.put('{');
            report_detail::key(output, "role");
            report_detail::write_string(output, filesystem.role);
            output.put(',');
            report_detail::key(output, "path");
            report_detail::write_string(output, filesystem.path.string());
            output.put(',');
            report_detail::key(output, "capacity_bytes");
            output << filesystem.capacity_bytes;
            output.put(',');
            report_detail::key(output, "available_bytes");
            output << filesystem.available_bytes;
            output.put(',');
            report_detail::key(output, "required_available_bytes");
            output << filesystem.required_available_bytes;
            output.put(',');
            report_detail::key(output, "device_id");
            output << filesystem.device_id;
            output.put(',');
            report_detail::key(output, "distinct_from_data");
            output << (filesystem.distinct_from_data ? "true" : "false");
            output.put(',');
            report_detail::key(output, "passed");
            output << (filesystem.passed ? "true" : "false");
            output.put('}');
        }
        output.put(']');
        output.put(',');
        report_detail::key(output, "io");
        output.put('{');
        report_detail::key(output, "path");
        report_detail::write_string(output, qualification.io.path.string());
        output.put(',');
        report_detail::key(output, "bytes");
        output << qualification.io.bytes;
        output.put(',');
        report_detail::key(output, "write_duration_ms");
        output << qualification.io.write_duration.count();
        output.put(',');
        report_detail::key(output, "read_duration_ms");
        output << qualification.io.read_duration.count();
        output.put(',');
        report_detail::key(output, "write_mib_per_second");
        output << qualification.io.write_mib_per_second;
        output.put(',');
        report_detail::key(output, "read_mib_per_second");
        output << qualification.io.read_mib_per_second;
        output.put(',');
        report_detail::key(output, "cache_eviction_method");
        report_detail::write_string(output, qualification.io.cache_eviction_method);
        output.put(',');
        report_detail::key(output, "cache_eviction_succeeded");
        output << (qualification.io.cache_eviction_succeeded ? "true" : "false");
        output.put(',');
        report_detail::key(output, "passed");
        output << (qualification.io.passed ? "true" : "false");
        output.put('}');
        output.put('}');
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "sql_transcripts");
    output.put('[');
    bool first_transcript = true;
    for (const sql_transcript_entry& entry : report.sql_transcripts) {
        report_detail::comma(output, first_transcript);
        report_detail::write_transcript(output, entry);
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "artifacts");
    output.put('[');
    bool first_artifact = true;
    for (const artifact_observation& artifact : report.artifacts) {
        report_detail::comma(output, first_artifact);
        output.put('{');
        bool first_artifact_field = true;
        const auto artifact_field = [&output, &first_artifact_field](std::string_view name,
                                                                     std::string_view value) {
            report_detail::comma(output, first_artifact_field);
            report_detail::key(output, name);
            report_detail::write_string(output, value);
        };
        artifact_field("name", artifact.name);
        artifact_field("preinstalled_stage_root", artifact.preinstalled_stage_root.string());
        artifact_field("stage_manifest", artifact.stage_manifest.string());
        artifact_field("stage_manifest_sha256", artifact.stage_manifest_sha256);
        artifact_field("control_file", artifact.control_file.string());
        artifact_field("install_sql", artifact.install_sql.string());
        artifact_field("dso", artifact.dso.string());
        artifact_field("staged_control_file", artifact.staged_control_file.string());
        artifact_field("staged_install_sql", artifact.staged_install_sql.string());
        artifact_field("staged_dso", artifact.staged_dso.string());
        artifact_field("control_sha256", artifact.control_sha256);
        artifact_field("install_sql_sha256", artifact.install_sql_sha256);
        artifact_field("dso_sha256", artifact.dso_sha256);
        artifact_field("staged_control_sha256", artifact.staged_control_sha256);
        output.put('}');
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "cleanup_observations");
    output.put('[');
    bool first_cleanup_observation = true;
    for (const cleanup_observation& observation : report.cleanup_observations) {
        report_detail::comma(output, first_cleanup_observation);
        output.put('{');
        report_detail::key(output, "ordinal");
        output << observation.ordinal;
        output.put(',');
        report_detail::key(output, "test_case");
        report_detail::write_string(output, observation.test_case);
        output.put(',');
        report_detail::key(output, "section");
        report_detail::write_string(output, observation.section);
        output.put(',');
        report_detail::key(output, "root");
        report_detail::write_string(output, observation.root.string());
        output.put(',');
        report_detail::key(output, "succeeded");
        output << (observation.succeeded ? "true" : "false");
        output.put(',');
        report_detail::key(output, "detail");
        report_detail::write_string(output, observation.detail);
        output.put(',');
        report_detail::key(output, "peak_rss_bytes");
        output << observation.resources.peak_rss_bytes;
        output.put(',');
        report_detail::key(output, "observed_temp_bytes");
        output << observation.resources.observed_temp_bytes;
        output.put(',');
        report_detail::key(output, "postgres");
        report_detail::write_resource_sampling(output, observation.resources.postgres);
        output.put('}');
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "resources");
    output << "{\"peak_rss_bytes\":" << report.resources.peak_rss_bytes
           << ",\"observed_temp_bytes\":" << report.resources.observed_temp_bytes
           << ",\"postgres\":";
    report_detail::write_resource_sampling(output, report.resources.postgres);
    output.put('}');
    report_detail::comma(output, first);
    report_detail::key(output, "cleanup");
    output.put('{');
    report_detail::key(output, "succeeded");
    output << (report.cleanup.succeeded ? "true" : "false");
    output.put(',');
    report_detail::key(output, "detail");
    report_detail::write_string(output, report.cleanup.detail);
    output.put('}');
    output.put('}');
    output.put('\n');
    if (!output) {
        throw std::runtime_error("failed writing integration report: " + absolute_path.string());
    }
    const std::string serialized = std::move(output).str();
    const auto published = bioseq::common::durable_file_io::durable_replace_file(
        absolute_path,
        std::as_bytes(std::span{serialized.data(), serialized.size()}),
        bioseq::common::durable_file_io::publication_options{
            .mode = 0666}); // Preserve ofstream mode before umask.
    if (!published.has_value()) {
        const bioseq::common::durable_file_io::file_error& error = published.error();
        throw std::runtime_error("cannot durably publish integration report: operation " +
                                 std::to_string(static_cast<unsigned>(error.operation)) +
                                 ", code " + std::to_string(static_cast<unsigned>(error.code)) +
                                 ", system error " + std::to_string(error.system_error) +
                                 ", publication " +
                                 std::to_string(static_cast<unsigned>(error.publication)));
    }
    if (published->publication != bioseq::common::durable_file_io::publication_state::durable) {
        throw std::runtime_error("integration report publication did not become durable");
    }
}

} // namespace bioseq::postgres_test
