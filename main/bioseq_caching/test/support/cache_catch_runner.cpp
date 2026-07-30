#include <catch2/catch_clara.hpp>
#include <catch2/catch_session.hpp>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

import bioseq.cache.test_context;
import bioseq.test.postgres;

namespace {

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

/// @brief Writes the native runner verdict and stable migration probes.
void write_report(const std::filesystem::path& report,
                  int catch_exit_code,
                  std::string_view executable,
                  const bioseq::cache_test::runner_configuration&
                      configuration) {
    if (report.empty()) {
        return;
    }
    bioseq::postgres_test::integration_test_report native_report{
        .schema_version = "bioseq-cache-test-v1",
        .executable = std::string(executable),
        .build_identity = "native-catch2",
        .arguments = {
            {"postgres-bin-dir", configuration.postgres_bin_dir.string(),
             false},
            {"cache-control", configuration.cache_control.string(), false},
            {"cache-install-sql",
             configuration.cache_install_sql.string(), false},
            {"cache-dso", configuration.cache_dso.string(), false},
            {"report", configuration.report.string(), false},
            {"case-timeout-seconds",
             std::to_string(configuration.case_timeout.count()), false}},
        .cluster_configuration = {
            {"ownership", "one isolated cluster per Catch case"},
            {"transport", "Unix socket only"},
            {"authentication", "local trust in private PGDATA"}},
        .cases = {
            bioseq::postgres_test::test_case_report{
                .test_case = "Catch2 session",
                .status = catch_exit_code == 0 ? "passed" : "failed"}},
        .migration_probes =
            bioseq::postgres_test::migration_probe_records(),
        .resources = bioseq::postgres_test::capture_resource_report(),
        .cleanup = bioseq::postgres_test::cleanup_report{
            .succeeded = catch_exit_code == 0,
            .detail = catch_exit_code == 0
                ? "all case-owned fixture destructors completed"
                : "one or more Catch cases failed"}};
    native_report.artifacts.push_back(make_artifact_report(
        "bioseq_cache", configuration.cache_control,
        configuration.cache_install_sql, configuration.cache_dso));
    if (configuration.bioseq_control
        && configuration.bioseq_install_sql
        && configuration.bioseq_dso) {
        native_report.arguments.push_back(
            {"bioseq-control",
             configuration.bioseq_control->string(), false});
        native_report.arguments.push_back(
            {"bioseq-install-sql",
             configuration.bioseq_install_sql->string(), false});
        native_report.arguments.push_back(
            {"bioseq-dso", configuration.bioseq_dso->string(), false});
        native_report.artifacts.push_back(make_artifact_report(
            "bioseq", *configuration.bioseq_control,
            *configuration.bioseq_install_sql,
            *configuration.bioseq_dso));
    }
    bioseq::postgres_test::write_integration_test_report(
        report, native_report);
}

}  // namespace

int main(int argc, char* argv[]) {
    Catch::Session session;

    std::string postgres_bin_dir;
    std::string cache_control;
    std::string cache_install_sql;
    std::string cache_dso;
    std::string bioseq_control;
    std::string bioseq_install_sql;
    std::string bioseq_dso;
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
        | Catch::Clara::Opt(report, "PATH")
              ["--report"]("native JSON report path")
        | Catch::Clara::Opt(case_timeout_seconds, "SECONDS")
              ["--case-timeout-seconds"]("per-case watchdog");
    session.cli(cli);

    const int cli_result = session.applyCommandLine(argc, argv);
    if (cli_result != 0) {
        return cli_result;
    }
    if (postgres_bin_dir.empty() || cache_control.empty()
        || cache_install_sql.empty() || cache_dso.empty()
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
    bioseq::cache_test::set_runner_configuration(std::move(configuration));
    bioseq::postgres_test::clear_migration_probes();

    const int catch_result = session.run();
    try {
        write_report(
            report, catch_result, argv[0],
            bioseq::cache_test::configuration());
    } catch (...) {
        return catch_result == 0 ? 3 : catch_result;
    }
    return catch_result;
}
