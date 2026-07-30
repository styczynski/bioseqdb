#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

import bioseq.test.postgres;

#if !defined(BIOSEQ_POSTGRES_TEST_PROCESS_CONTRACT_CHILD_PATH)
#error "BIOSEQ_POSTGRES_TEST_PROCESS_CONTRACT_CHILD_PATH must name the process contract child"
#endif

using namespace std::chrono_literals;
using namespace bioseq::postgres_test;

static_assert(requires(bioseq::libpq::connection& connection) {
    connection.exec(
        "BEGIN", 1s, bioseq::libpq::transaction_policy::allow_open);
    connection.current_transaction_state();
    connection.exec_single_row_observed("SELECT 1", 1s);
    connection.set_execution_observer(nullptr, "contract");
    connection.execution_observer_ptr();
});
static_assert(std::is_same_v<
    decltype(std::declval<bioseq::libpq::connection&>()
                 .exec_single_row_observed("SELECT 1", 1s)),
    bioseq::libpq::single_row_observed_result>);

namespace {

struct report_state_reset {
    ~report_state_reset() noexcept {
        clear_report_case_context();
        try {
            clear_process_sql_transcript();
        } catch (...) {
        }
        clear_cleanup_observations();
    }
};

class scoped_environment_override {
public:
    scoped_environment_override(const char* name, const char* value)
        : name_(name) {
        if (const char* current = std::getenv(name)) {
            original_ = current;
        }
        if (::setenv(name, value, 1) != 0) {
            throw std::runtime_error(
                "cannot configure process-environment contract");
        }
    }

    scoped_environment_override(const scoped_environment_override&) = delete;
    scoped_environment_override& operator=(
        const scoped_environment_override&) = delete;

    ~scoped_environment_override() noexcept {
        if (original_) {
            static_cast<void>(
                ::setenv(name_.c_str(), original_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name_.c_str()));
        }
    }

private:
    std::string name_;
    std::optional<std::string> original_;
};

class temporary_resource_journal_tree {
public:
    temporary_resource_journal_tree()
        : root_(
              std::filesystem::temp_directory_path()
              / ("bioseq-resource-journal-contract-"
                 + std::to_string(::getpid()) + "-"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch()
                         .count()))) {
        if (!std::filesystem::create_directories(data_directory())) {
            throw std::runtime_error(
                "cannot create resource-journal contract tree");
        }
    }

    temporary_resource_journal_tree(
        const temporary_resource_journal_tree&) = delete;
    temporary_resource_journal_tree& operator=(
        const temporary_resource_journal_tree&) = delete;

    ~temporary_resource_journal_tree() noexcept {
        set_supervision_journal_fault_for_testing(
            supervision_journal_fault::none);
        disable_supervision_state();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] std::filesystem::path owned_root() const {
        return root_ / "owned";
    }

    [[nodiscard]] std::filesystem::path data_directory() const {
        return owned_root() / "data";
    }

    [[nodiscard]] std::filesystem::path state_directory() const {
        return root_ / "state";
    }

private:
    std::filesystem::path root_;
};

}  // namespace

TEST_CASE("libpq first-row observation has explicit empty-result semantics",
          "[postgres-test-support][unit][libpq][first-row]") {
    const bioseq::libpq::single_row_observed_result observation;
    CHECK_FALSE(observation.time_to_first_row.has_value());
    CHECK(observation.elapsed == 0ns);
    CHECK(observation.rows.rows() == 0);
}

TEST_CASE("process report collectors preserve case SQL and cleanup evidence",
          "[postgres-test-support][unit][report]") {
    const report_state_reset reset;
    clear_process_sql_transcript();
    clear_cleanup_observations();
    set_report_case_context({
        .test_case = "report collector contract",
        .section = "observer"});

    bioseq::libpq::result command;
    process_sql_transcript().command_succeeded(
        "admin", "SELECT 1", command, 2ms);
    const bioseq::libpq::query_error expected{
        "expected failure", "22000"};
    process_sql_transcript().command_failed(
        "session", "SELECT fail()", expected, 3ms);
    bioseq::libpq::result oversized;
    oversized.columns_.push_back(
        {.name = "payload", .type_oid = 25});
    oversized.rows_.push_back({
        {.is_null = false,
         .text = std::string(64U * 1024U + 1U, 'x')}});
    process_sql_transcript().command_succeeded(
        "session", "SELECT oversized_payload", oversized, 4ms);

    const auto transcript = process_sql_transcript_entries();
    REQUIRE(transcript.size() == 3);
    CHECK(transcript[0].test_case == "report collector contract");
    CHECK(transcript[0].section == "observer");
    CHECK(transcript[0].status == sql_transcript_status::succeeded);
    CHECK(transcript[1].sqlstate == "22000");
    CHECK(transcript[1].status == sql_transcript_status::failed);
    CHECK(transcript[2].rows_truncated);
    CHECK(transcript[2].sampled_rows.empty());

    record_cleanup_observation(
        std::filesystem::path{"/tmp/bioseq-report-contract"},
        true, "removed",
        resource_report{
            .peak_rss_bytes = 1024,
            .observed_temp_bytes = 2048});
    const cleanup_report cleanup = aggregate_cleanup_report();
    CHECK(cleanup.succeeded);
    REQUIRE(cleanup_observations().size() == 1);
    CHECK(aggregate_resource_report().observed_temp_bytes >= 2048);

}

TEST_CASE("resource phase crash journal preserves exact boundary samples",
          "[postgres-test-support][unit][resource][journal]") {
    temporary_resource_journal_tree tree;
    {
        std::ofstream postmaster_pid(
            tree.data_directory() / "postmaster.pid",
            std::ios::trunc);
        postmaster_pid << ::getpid() << '\n';
        postmaster_pid.close();
        REQUIRE(static_cast<bool>(postmaster_pid));
    }
    configure_supervision_state(tree.state_directory());
    initialize_supervision_resource_journal();

    resource_sampling_report live;
    {
        resource_sampler sampler{
            tree.owned_root(), tree.data_directory(), 1h, 1h};
        sampler.observe_phase_boundary("contract-boundary");
        live = sampler.finish();
    }
    const resource_sampling_report recovered =
        load_supervision_resource_report(tree.state_directory());

    REQUIRE(live.phase_boundaries.size() == 1);
    REQUIRE(recovered.phase_boundaries.size() == 1);
    const resource_phase_boundary& expected = live.phase_boundaries.front();
    const resource_phase_boundary& observed =
        recovered.phase_boundaries.front();
    REQUIRE(expected.postmaster_process_tree_rss_bytes.has_value());
    REQUIRE(expected.owned_root_bytes.has_value());
    CHECK(observed.phase == expected.phase);
    CHECK(observed.elapsed == expected.elapsed);
    CHECK(
        observed.postmaster_process_tree_rss_bytes
        == expected.postmaster_process_tree_rss_bytes);
    CHECK(observed.owned_root_bytes == expected.owned_root_bytes);
}

TEST_CASE("watchdog decision has one atomic winner",
          "[postgres-test-support][unit][supervision][journal]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    REQUIRE(supervision_registries_empty(tree.state_directory()));
    register_supervised_cluster(supervised_cluster_record{
        .root = tree.owned_root(),
        .data_directory = tree.data_directory()});
    CHECK_FALSE(supervision_registries_empty(tree.state_directory()));
    REQUIRE(unregister_supervised_cluster(tree.owned_root()));
    REQUIRE(supervision_registries_empty(tree.state_directory()));

    REQUIRE(try_claim_supervision_decision(
        tree.state_directory(), "disarmed"));
    CHECK(supervision_decision_claimed(tree.state_directory()));
    CHECK_FALSE(try_claim_supervision_decision(
        tree.state_directory(), "timeout"));
}

TEST_CASE("watchdog decision requires durable complete publication",
          "[postgres-test-support][unit][supervision][decision][fault]") {
    const auto exercise =
        [](supervision_journal_fault fault) {
        temporary_resource_journal_tree tree;
        configure_supervision_state(tree.state_directory());
        set_supervision_journal_fault_for_testing(fault);

        CHECK_FALSE(try_claim_supervision_decision(
            tree.state_directory(), "disarmed"));
        set_supervision_journal_fault_for_testing(
            supervision_journal_fault::none);
        CHECK_FALSE(supervision_decision_claimed(
            tree.state_directory()));
        CHECK_FALSE(std::filesystem::exists(
            tree.state_directory() / "decision"));

        REQUIRE(try_claim_supervision_decision(
            tree.state_directory(), "disarmed"));
        CHECK(supervision_decision_claimed(
            tree.state_directory()));
    };

    SECTION("open") {
        exercise(supervision_journal_fault::open);
    }
    SECTION("write") {
        exercise(supervision_journal_fault::write);
    }
    SECTION("file fsync") {
        exercise(supervision_journal_fault::fsync);
    }
    SECTION("close") {
        exercise(supervision_journal_fault::close);
    }
    SECTION("atomic publish") {
        exercise(supervision_journal_fault::rename);
    }
    SECTION("directory sync") {
        exercise(supervision_journal_fault::directory_sync);
    }
}

TEST_CASE("failed watchdog decision publication is never observable",
          "[postgres-test-support][unit][supervision][decision][concurrent]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::directory_sync);

    const pid_t publisher = ::fork();
    REQUIRE(publisher >= 0);
    if (publisher == 0) {
        const bool published = try_claim_supervision_decision(
            tree.state_directory(), "disarmed");
        ::_exit(published ? 0 : 1);
    }

    const auto deadline =
        std::chrono::steady_clock::now() + 2s;
    bool observed_candidate = false;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code error;
        if (std::filesystem::exists(
                tree.state_directory() / "decision", error)
            && !error) {
            observed_candidate = true;
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(observed_candidate);

    CHECK_FALSE(supervision_decision_claimed(
        tree.state_directory()));
    int status{};
    pid_t waited{};
    do {
        waited = ::waitpid(publisher, &status, 0);
    } while (waited < 0 && errno == EINTR);
    REQUIRE(waited == publisher);
    REQUIRE(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 1);

    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::none);
    CHECK_FALSE(std::filesystem::exists(
        tree.state_directory() / "decision"));
    CHECK_FALSE(supervision_decision_claimed(
        tree.state_directory()));
    REQUIRE(try_claim_supervision_decision(
        tree.state_directory(), "timeout"));
    CHECK(supervision_decision_claimed(
        tree.state_directory()));
}

TEST_CASE("live-resource publication and removal remain retryable",
          "[postgres-test-support][unit][supervision][journal][retry]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());
    const supervised_cluster_record cluster{
        .root = tree.owned_root(),
        .data_directory = tree.data_directory()};

    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::open);
    CHECK_THROWS(register_supervised_cluster(cluster));
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::none);
    REQUIRE_NOTHROW(register_supervised_cluster(cluster));
    const auto registered = load_supervision_registries(
        tree.state_directory());
    REQUIRE(registered.has_value());
    REQUIRE(registered->clusters.size() == 1);
    CHECK_FALSE(remove_supervision_state(
        tree.state_directory()));

    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::write);
    CHECK_FALSE(unregister_supervised_cluster(
        tree.owned_root()));
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::none);
    CHECK_FALSE(load_supervision_registries(
        tree.state_directory()));
    REQUIRE(unregister_supervised_cluster(
        tree.owned_root()));
    REQUIRE(supervision_registries_empty(
        tree.state_directory()));
    REQUIRE(remove_supervision_state(
        tree.state_directory()));
    CHECK_FALSE(std::filesystem::exists(
        tree.state_directory()));
}

TEST_CASE("live-resource registry publication failures stay dirty",
          "[postgres-test-support][unit][supervision][journal][fault]") {
    const auto exercise =
        [](supervision_journal_fault fault) {
        temporary_resource_journal_tree tree;
        configure_supervision_state(tree.state_directory());
        set_supervision_journal_fault_for_testing(fault);

        CHECK_THROWS(register_supervised_cluster(
            supervised_cluster_record{
                .root = tree.owned_root(),
                .data_directory = tree.data_directory()}));
        set_supervision_journal_fault_for_testing(
            supervision_journal_fault::none);

        CHECK(std::filesystem::is_regular_file(
            tree.state_directory() / "registries.dirty"));
        CHECK_FALSE(load_supervision_registries(
            tree.state_directory()));
        CHECK_FALSE(supervision_registries_empty(
            tree.state_directory()));
        CHECK_FALSE(remove_supervision_state(
            tree.state_directory()));
    };

    SECTION("open") {
        exercise(supervision_journal_fault::open);
    }
    SECTION("write") {
        exercise(supervision_journal_fault::write);
    }
    SECTION("rename") {
        exercise(supervision_journal_fault::rename);
    }
}

TEST_CASE("live-resource registry loading rejects invalid evidence",
          "[postgres-test-support][unit][supervision][journal][strict]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    SECTION("missing journal") {
        REQUIRE(std::filesystem::remove(
            tree.state_directory() / "processes.tsv"));
    }
    SECTION("malformed journal") {
        std::ofstream output(
            tree.state_directory() / "clusters.tsv",
            std::ios::trunc);
        output << "malformed\n";
        output.close();
        REQUIRE(static_cast<bool>(output));
    }
    SECTION("generation mismatch") {
        std::ofstream output(
            tree.state_directory() / "registries.clean",
            std::ios::trunc);
        output << "bioseq-supervision-registry-v1\t999999\n";
        output.close();
        REQUIRE(static_cast<bool>(output));
    }
    SECTION("incomplete publication") {
        std::ofstream output(
            tree.state_directory() / "clusters.tsv.partial",
            std::ios::trunc);
        output << "incomplete";
        output.close();
        REQUIRE(static_cast<bool>(output));
    }

    CHECK_FALSE(load_supervision_registries(
        tree.state_directory()));
    CHECK_FALSE(supervision_registries_empty(
        tree.state_directory()));
    CHECK_FALSE(remove_supervision_state(
        tree.state_directory()));
}

TEST_CASE("supervised child launch is dirty before posix_spawn",
          "[postgres-test-support][unit][supervision][journal][launch]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    const auto intent = begin_supervised_process_launch();
    REQUIRE(intent.has_value());
    REQUIRE(intent->supervision_required);
    CHECK(std::filesystem::is_regular_file(
        tree.state_directory() / "registries.dirty"));
    CHECK_FALSE(load_supervision_registries(
        tree.state_directory()));

    REQUIRE(cancel_supervised_process_launch(*intent));
    CHECK_FALSE(std::filesystem::exists(
        tree.state_directory() / "registries.dirty"));
    REQUIRE(supervision_registries_empty(
        tree.state_directory()));
}

TEST_CASE("unjournaled supervised child spawn fails synchronously",
          "[postgres-test-support][unit][supervision][journal][process]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());
    const std::filesystem::path sleeper{"/bin/sleep"};
    REQUIRE(std::filesystem::is_regular_file(sleeper));
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::open);

    CHECK_THROWS_AS(
        spawned_process::spawn(process_options{
            .executable = sleeper,
            .arguments = {"30"},
            .supervise = true}),
        process_error);
    set_supervision_journal_fault_for_testing(
        supervision_journal_fault::none);

    CHECK(std::filesystem::is_regular_file(
        tree.state_directory() / "registries.dirty"));
    CHECK_FALSE(load_supervision_registries(
        tree.state_directory()));
}

TEST_CASE("supervised spawn preserves exact argv zero and PostgreSQL PATH",
          "[postgres-test-support][unit][process][postgres-binary-identity]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    const std::filesystem::path executable =
        std::filesystem::canonical(
            std::filesystem::path{
                BIOSEQ_POSTGRES_TEST_PROCESS_CONTRACT_CHILD_PATH});
    const std::filesystem::path preferred_directory =
        executable.parent_path();
    const std::filesystem::path poisoned_directory =
        tree.state_directory() / "host-postgres-bin";
    REQUIRE(std::filesystem::create_directories(poisoned_directory));
    const std::filesystem::path output =
        tree.state_directory() / "process-contract-child.log";
    const scoped_environment_override host_pg{
        "PGHOST", "host-postgres.invalid"};
    const scoped_environment_override host_pgdata{
        "PGDATA", poisoned_directory.c_str()};

    spawned_process child = spawned_process::spawn(process_options{
        .executable = executable,
        .environment = {{"PATH", poisoned_directory.string()}},
        .preferred_path_directory = preferred_directory,
        .output_file = output,
        .supervise = true,
        .supervision_data_directory = tree.data_directory()});
    const process_exit exit = child.wait_for(5s);
    REQUIRE(exit.succeeded());

    const std::string captured = child.captured_output();
    CHECK(captured.find("argv0=" + executable.string() + "\n")
          != std::string::npos);
    CHECK(captured.find(
              "PATH=" + preferred_directory.string() + ":"
              + poisoned_directory.string() + "\n")
          != std::string::npos);
    CHECK(captured.find("PGHOST=<unset>\n") != std::string::npos);
    CHECK(captured.find("PGDATA=<unset>\n") != std::string::npos);
    CHECK(supervision_registries_empty(tree.state_directory()));
}

TEST_CASE("supervised process identity rejects a changed start token",
          "[postgres-test-support][unit][supervision][identity]") {
    const auto process = capture_supervised_process(
        static_cast<std::uint64_t>(::getpid()));
    REQUIRE(process.has_value());
    REQUIRE(supervised_process_matches(*process));

    supervised_process_record stale = *process;
    stale.start_token += "-reused";
    CHECK_FALSE(supervised_process_matches(stale));

    supervised_process_record moved_group = *process;
    ++moved_group.process_group;
    CHECK(supervised_process_matches(moved_group));
    CHECK_FALSE(supervised_process_group_matches(moved_group));
}

TEST_CASE("startup discovery refuses pidfile identity without live PGDATA binding",
          "[postgres-test-support][unit][supervision][start-window]") {
    temporary_resource_journal_tree tree;
    {
        std::ofstream marker(
            tree.owned_root() / ".bioseq-private-cluster",
            std::ios::trunc);
        marker << "contract\n";
        marker.close();
        REQUIRE(static_cast<bool>(marker));
    }
    {
        std::ofstream postmaster_pid(
            tree.data_directory() / "postmaster.pid",
            std::ios::trunc);
        postmaster_pid << ::getpid() << '\n'
                       << std::filesystem::canonical(
                              tree.data_directory()).string()
                       << "\n1\n";
        postmaster_pid.close();
        REQUIRE(static_cast<bool>(postmaster_pid));
    }
    const auto same_pid_and_pidfile = capture_supervised_process(
        static_cast<std::uint64_t>(::getpid()), {},
        tree.data_directory());
    REQUIRE(same_pid_and_pidfile.has_value());
    CHECK_FALSE(supervised_postmaster_matches_data_directory(
        *same_pid_and_pidfile));
}

TEST_CASE("typed watchdog snapshots preserve prior report evidence",
          "[postgres-test-support][unit][supervision][report]") {
    temporary_resource_journal_tree tree;
    configure_supervision_state(tree.state_directory());

    const std::array cases{
        test_case_report{
            .test_case = "case\tone",
            .section = "section\none",
            .status = "passed",
            .message = "message",
            .duration = 17ms}};
    const std::array phases{
        phase_duration_report{
            .ordinal = 3,
            .test_case = "case\tone",
            .section = "section\none",
            .phase = "load",
            .status = "completed",
            .duration = 19ms}};
    const std::array artifacts{
        artifact_report{
            .name = "bioseq_cache",
            .control_file = "/source/cache.control",
            .install_sql = "/source/cache.sql",
            .dso = "/source/cache.so",
            .staged_control_file = "/stage/cache.control",
            .staged_install_sql = "/stage/cache.sql",
            .staged_dso = "/stage/cache.so",
            .control_sha256 = "control-digest",
            .install_sql_sha256 = "sql-digest",
            .dso_sha256 = "dso-digest",
            .staged_control_sha256 = "staged-control-digest"}};
    const std::array probes{
        migration_probe_record{
            .stable_id = "cache.contract",
            .test_case = "case\tone",
            .section = "section\none",
            .permutation = "mode=cache",
            .passed = true,
            .ordinal = 5}};

    REQUIRE(publish_supervision_test_case_reports(cases));
    REQUIRE(publish_supervision_phase_duration_reports(phases));
    REQUIRE(publish_supervision_artifact_reports(artifacts));
    REQUIRE(publish_supervision_migration_probe_records(probes));

    const auto recovered_cases =
        load_supervision_test_case_reports(tree.state_directory());
    const auto recovered_phases =
        load_supervision_phase_duration_reports(tree.state_directory());
    const auto recovered_artifacts =
        load_supervision_artifact_reports(tree.state_directory());
    const auto recovered_probes =
        load_supervision_migration_probe_records(tree.state_directory());
    REQUIRE(recovered_cases.has_value());
    REQUIRE(recovered_phases.has_value());
    REQUIRE(recovered_artifacts.has_value());
    REQUIRE(recovered_probes.has_value());
    REQUIRE(recovered_cases->size() == 1);
    REQUIRE(recovered_phases->size() == 1);
    REQUIRE(recovered_artifacts->size() == 1);
    REQUIRE(recovered_probes->size() == 1);
    CHECK(recovered_cases->front().test_case == cases.front().test_case);
    CHECK(recovered_cases->front().section == cases.front().section);
    CHECK(recovered_cases->front().duration == cases.front().duration);
    CHECK(recovered_phases->front().ordinal == phases.front().ordinal);
    CHECK(recovered_phases->front().phase == phases.front().phase);
    CHECK(recovered_phases->front().duration == phases.front().duration);
    CHECK(
        recovered_artifacts->front().staged_dso
        == artifacts.front().staged_dso);
    CHECK(
        recovered_artifacts->front().install_sql_sha256
        == artifacts.front().install_sql_sha256);
    CHECK(recovered_probes->front().stable_id == probes.front().stable_id);
    CHECK(recovered_probes->front().passed);
    CHECK(recovered_probes->front().ordinal == probes.front().ordinal);
}

TEST_CASE("postgres test support SHA-256 matches the standard vector",
          "[postgres-test-support][unit]") {
    REQUIRE(
        sha256("abc").hex()
        == "ba7816bf8f01cfea414140de5dae2223"
           "b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("postgres test support parses typed recursive EXPLAIN JSON",
          "[postgres-test-support][unit]") {
    const explain_document document = explain_document::parse(
        R"([{"Plan":{"Node Type":"Custom Scan",)"
        R"("Custom Plan Provider":"BioseqCacheScan",)"
        R"("Actual Loops":0,"Plans":[{"Node Type":"Seq Scan"}]}}])");
    const auto nodes =
        document.find_plan_nodes("Custom Plan Provider", "BioseqCacheScan");
    REQUIRE(nodes.size() == 1);
    REQUIRE(explain_property_integer(*nodes.front(), "Actual Loops") == 0);
    REQUIRE(document.plan_nodes().size() == 2);
}

TEST_CASE("postgres test support enforces migration probe identity and counts",
          "[postgres-test-support][unit]") {
    clear_migration_probes();
    {
        migration_probe_scope scope(
            "probe contract", "single", "value=1");
        record_migration_probe("support.probe.contract");
        REQUIRE_THROWS_AS(
            record_migration_probe("support.probe.contract"),
            migration_probe_error);
    }
    const std::array expected{
        expected_migration_probe{
            .stable_id = "support.probe.contract",
            .expected_count = 1}};
    REQUIRE_NOTHROW(validate_migration_probes(expected));
    clear_migration_probes();
}

TEST_CASE("canonical row hash distinguishes NULL from empty text",
          "[postgres-test-support][unit]") {
    exact_row_hasher null_hash;
    const std::array null_row{
        nullable_text_cell{.is_null = true, .bytes = {}}};
    null_hash.append_row(null_row);

    exact_row_hasher empty_hash;
    const std::array empty_row{
        nullable_text_cell{.is_null = false, .bytes = {}}};
    empty_hash.append_row(empty_row);

    REQUIRE(null_hash.finalize() != empty_hash.finalize());

    exact_row_hasher normative;
    const std::array mixed_row{
        nullable_text_cell{.is_null = true, .bytes = {}},
        nullable_text_cell{.is_null = false, .bytes = {}}};
    normative.append_row(mixed_row);
    REQUIRE(
        normative.finalize().hex()
        == "48a1b931ccb6063e8f71c216c9f04d1c"
           "0bd426edf4ce8ee4c98601f6c603b206");
}
