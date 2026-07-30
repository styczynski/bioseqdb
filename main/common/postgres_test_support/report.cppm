module;

#include <sys/resource.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.test.postgres:report;

import bioseq.libpq.client;
import :artifact;
import :migration_probe;

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
    std::string sql;
    sql_transcript_status status{sql_transcript_status::succeeded};
    std::string sqlstate;
    std::string message;
    std::string command_tag;
    std::int64_t row_count{};
    std::int64_t column_count{};
    std::vector<std::vector<std::optional<std::string>>> sampled_rows;
    bool rows_truncated{};
    std::chrono::milliseconds duration{};
};

/// @brief Thread-safe executor/collector for literal SQL transcripts.
class sql_transcript {
public:
    /// @brief Executes SQL, records bounded rows/errors, and preserves exceptions.
    [[nodiscard]] bioseq::libpq::result execute(
        bioseq::libpq::connection& connection,
        std::string_view session,
        std::string_view sql,
        std::chrono::milliseconds deadline,
        bioseq::libpq::transaction_policy policy =
            bioseq::libpq::transaction_policy::require_idle,
        std::size_t max_sample_cells = 256) {
        const auto started = std::chrono::steady_clock::now();
        try {
            bioseq::libpq::result result =
                connection.exec(sql, deadline, policy);
            sql_transcript_entry entry{
                .session = std::string(session),
                .sql = std::string(sql),
                .status = sql_transcript_status::succeeded,
                .command_tag = result.command_tag(),
                .row_count = result.rows(),
                .column_count = result.columns(),
                .duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)};
            std::size_t sampled_cells = 0;
            for (int row = 0; row < result.rows(); ++row) {
                if (sampled_cells
                    + static_cast<std::size_t>(result.columns())
                    > max_sample_cells) {
                    entry.rows_truncated = true;
                    break;
                }
                std::vector<std::optional<std::string>> sampled_row;
                sampled_row.reserve(
                    static_cast<std::size_t>(result.columns()));
                for (int column = 0; column < result.columns(); ++column) {
                    if (result.is_null(row, column)) {
                        sampled_row.push_back(std::nullopt);
                    } else {
                        sampled_row.emplace_back(result.value(row, column));
                    }
                    ++sampled_cells;
                }
                entry.sampled_rows.push_back(std::move(sampled_row));
            }
            append(std::move(entry));
            return result;
        } catch (const bioseq::libpq::query_error& error) {
            append(sql_transcript_entry{
                .session = std::string(session),
                .sql = std::string(sql),
                .status = sql_transcript_status::failed,
                .sqlstate = std::string(error.sqlstate()),
                .message = error.what(),
                .duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)});
            throw;
        } catch (const std::exception& error) {
            append(sql_transcript_entry{
                .session = std::string(session),
                .sql = std::string(sql),
                .status = sql_transcript_status::failed,
                .message = error.what(),
                .duration =
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started)});
            throw;
        }
    }

    /// @brief Appends a manually constructed transcript entry.
    void append(sql_transcript_entry entry) {
        std::scoped_lock lock(mutex_);
        entry.ordinal = next_ordinal_++;
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
    mutable std::mutex mutex_;
    std::vector<sql_transcript_entry> entries_;
    std::uint64_t next_ordinal_{};
};

/// @brief One command-line argument after runner-owned sensitivity classification.
struct report_argument {
    std::string name;
    std::string value;
    bool sensitive{};
};

/// @brief Source-neutral Catch case/section outcome supplied by runner glue.
struct test_case_report {
    std::string test_case;
    std::string section;
    std::string status;
    std::string message;
    std::chrono::milliseconds duration{};
};

/// @brief Artifact digest/path evidence embedded in the native report.
struct artifact_report {
    std::string name;
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
struct resource_report {
    std::uint64_t peak_rss_bytes{};
    std::uint64_t observed_temp_bytes{};
};

/// @brief No-throw teardown verdict supplied after cluster cleanup.
struct cleanup_report {
    bool succeeded{};
    std::string detail;
};

/// @brief Complete source-neutral JSON model for a native integration run.
struct integration_test_report {
    std::string schema_version{"bioseq-postgres-test-v1"};
    std::string executable;
    std::string build_identity;
    std::vector<report_argument> arguments;
    std::vector<std::pair<std::string, std::string>> cluster_configuration;
    std::vector<test_case_report> cases;
    std::vector<sql_transcript_entry> sql_transcripts;
    std::vector<migration_probe_record> migration_probes;
    std::vector<artifact_report> artifacts;
    resource_report resources;
    cleanup_report cleanup;
};

/// @brief Converts a staged extension inventory into report evidence.
[[nodiscard]] inline std::vector<artifact_report> artifact_reports(
    const extension_stage& stage) {
    std::vector<artifact_report> reports;
    reports.reserve(stage.extensions.size());
    for (const staged_extension_artifact& artifact : stage.extensions) {
        reports.push_back(artifact_report{
            .name = artifact.name,
            .control_file = artifact.source_control_file,
            .install_sql = artifact.source_install_sql,
            .dso = artifact.source_dso,
            .staged_control_file = artifact.staged_control_file,
            .staged_install_sql = artifact.staged_install_sql,
            .staged_dso = artifact.staged_dso,
            .control_sha256 = artifact.control_digest.hex(),
            .install_sql_sha256 = artifact.install_sql_digest.hex(),
            .dso_sha256 = artifact.dso_digest.hex(),
            .staged_control_sha256 =
                artifact.staged_control_digest.hex()});
    }
    return reports;
}

/// @brief Samples runner peak RSS and exact bytes below selected temp roots.
[[nodiscard]] inline resource_report capture_resource_report(
    std::span<const std::filesystem::path> temp_roots = {}) {
    resource_report report;
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        report.peak_rss_bytes =
            static_cast<std::uint64_t>(usage.ru_maxrss);
#else
        report.peak_rss_bytes =
            static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
    }
    for (const std::filesystem::path& root : temp_roots) {
        std::error_code error;
        if (!std::filesystem::exists(root, error) || error) {
            continue;
        }
        std::filesystem::recursive_directory_iterator iterator(
            root, std::filesystem::directory_options::skip_permission_denied,
            error);
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

}  // namespace bioseq::postgres_test

namespace bioseq::postgres_test::report_detail {

/// @brief Writes one correctly escaped JSON string.
inline void write_string(std::ostream& output, std::string_view value) {
    constexpr std::string_view hex{"0123456789abcdef"};
    output.put('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20U) {
                output << "\\u00"
                       << hex[(character >> 4U) & 0x0fU]
                       << hex[character & 0x0fU];
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

inline void write_transcript(
    std::ostream& output, const sql_transcript_entry& entry) {
    output.put('{');
    bool first = true;
    const auto string_field =
        [&output, &first](std::string_view name, std::string_view value) {
            comma(output, first);
            key(output, name);
            write_string(output, value);
        };
    const auto integer_field =
        [&output, &first](std::string_view name, std::int64_t value) {
            comma(output, first);
            key(output, name);
            output << value;
        };
    integer_field("ordinal", static_cast<std::int64_t>(entry.ordinal));
    string_field("session", entry.session);
    string_field("sql", entry.sql);
    string_field("status",
                 entry.status == sql_transcript_status::succeeded
                     ? "passed" : "failed");
    string_field("sqlstate", entry.sqlstate);
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

}  // namespace bioseq::postgres_test::report_detail

export namespace bioseq::postgres_test {

/// @brief Atomically publishes a complete native integration JSON report.
inline void write_integration_test_report(
    const std::filesystem::path& path,
    const integration_test_report& report) {
    const std::filesystem::path absolute_path =
        std::filesystem::absolute(path);
    std::filesystem::create_directories(absolute_path.parent_path());
    const std::filesystem::path temporary =
        absolute_path.parent_path()
        / (absolute_path.filename().string() + ".partial");
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create integration report: "
                                 + temporary.string());
    }
    output.put('{');
    bool first = true;
    const auto string_field =
        [&output, &first](std::string_view name, std::string_view value) {
            report_detail::comma(output, first);
            report_detail::key(output, name);
            report_detail::write_string(output, value);
        };
    string_field("schema_version", report.schema_version);
    string_field("executable", report.executable);
    string_field("build_identity", report.build_identity);

    report_detail::comma(output, first);
    report_detail::key(output, "arguments");
    output.put('[');
    bool first_argument = true;
    for (const report_argument& argument : report.arguments) {
        report_detail::comma(output, first_argument);
        output.put('{');
        report_detail::key(output, "name");
        report_detail::write_string(output, argument.name);
        output.put(',');
        report_detail::key(output, "value");
        report_detail::write_string(
            output, argument.sensitive ? "<redacted>" : argument.value);
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
    for (const test_case_report& test_case : report.cases) {
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
    report_detail::key(output, "sql_transcripts");
    output.put('[');
    bool first_transcript = true;
    for (const sql_transcript_entry& entry : report.sql_transcripts) {
        report_detail::comma(output, first_transcript);
        report_detail::write_transcript(output, entry);
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "migration_probes");
    output.put('[');
    bool first_probe = true;
    for (const migration_probe_record& probe : report.migration_probes) {
        report_detail::comma(output, first_probe);
        output.put('{');
        report_detail::key(output, "stable_id");
        report_detail::write_string(output, probe.stable_id);
        output.put(',');
        report_detail::key(output, "test_case");
        report_detail::write_string(output, probe.test_case);
        output.put(',');
        report_detail::key(output, "section");
        report_detail::write_string(output, probe.section);
        output.put(',');
        report_detail::key(output, "permutation");
        report_detail::write_string(output, probe.permutation);
        output.put(',');
        report_detail::key(output, "passed");
        output << (probe.passed ? "true" : "false");
        output.put(',');
        report_detail::key(output, "ordinal");
        output << probe.ordinal;
        output.put('}');
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "artifacts");
    output.put('[');
    bool first_artifact = true;
    for (const artifact_report& artifact : report.artifacts) {
        report_detail::comma(output, first_artifact);
        output.put('{');
        bool first_artifact_field = true;
        const auto artifact_field =
            [&output, &first_artifact_field](
                std::string_view name, std::string_view value) {
                report_detail::comma(output, first_artifact_field);
                report_detail::key(output, name);
                report_detail::write_string(output, value);
            };
        artifact_field("name", artifact.name);
        artifact_field("control_file", artifact.control_file.string());
        artifact_field("install_sql", artifact.install_sql.string());
        artifact_field("dso", artifact.dso.string());
        artifact_field("staged_control_file",
                       artifact.staged_control_file.string());
        artifact_field("staged_install_sql",
                       artifact.staged_install_sql.string());
        artifact_field("staged_dso", artifact.staged_dso.string());
        artifact_field("control_sha256", artifact.control_sha256);
        artifact_field("install_sql_sha256", artifact.install_sql_sha256);
        artifact_field("dso_sha256", artifact.dso_sha256);
        artifact_field("staged_control_sha256",
                       artifact.staged_control_sha256);
        output.put('}');
    }
    output.put(']');

    report_detail::comma(output, first);
    report_detail::key(output, "resources");
    output << "{\"peak_rss_bytes\":" << report.resources.peak_rss_bytes
           << ",\"observed_temp_bytes\":"
           << report.resources.observed_temp_bytes << '}';
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
    output.close();
    if (!output) {
        throw std::runtime_error("failed writing integration report: "
                                 + temporary.string());
    }
    std::error_code error;
    std::filesystem::rename(temporary, absolute_path, error);
    if (error) {
        std::filesystem::remove(absolute_path, error);
        error.clear();
        std::filesystem::rename(temporary, absolute_path, error);
    }
    if (error) {
        throw std::runtime_error("cannot publish integration report: "
                                 + error.message());
    }
}

}  // namespace bioseq::postgres_test
