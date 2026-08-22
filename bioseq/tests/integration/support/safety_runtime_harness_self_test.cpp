/**
 * @file safety_runtime_harness_self_test.cpp
 * @brief Exercise local safety-protocol parsers and invariant checks.
 */

module;

#include <fmt/format.h>
#include <fmt/ostream.h>
#include <fmt/ranges.h>
#include <libpq-fe.h>
#include <nlohmann/json.hpp>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <spawn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

module bioseq.tests.integration.safety_runtime_harness_support;

import :scenario_support;

namespace bioseq::tests::integration::safety_runtime_harness {

/// Require an operation to fail with a diagnostic containing the marker.
template <typename p_operation>
void expect_failure(
        p_operation&& operation,
        std::string_view diagnostic) {
    try {
        std::forward<p_operation>(operation)();
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()}.find(diagnostic)
            == std::string_view::npos) {
            fail(
                "self-test",
                fmt::format(
                    "expected diagnostic '{}', observed '{}'",
                    diagnostic,
                    error.what()));
        }
        return;
    }
    fail(
        "self-test",
        fmt::format(
            "operation unexpectedly succeeded; expected '{}'",
            diagnostic));
}

/// Exercise local protocol parsers and invariant checks without activation.
[[nodiscard]] int self_test(
        const command_line& command) {
    require_exact_options(command, {});
    const case_plan plan = load_case_plan();
    if (plan.cases.empty()) {
        fail("self-test", "compiled case plan is empty");
    }
    const sql_manifest_contract manifest =
        load_sql_manifest();
    if (manifest.symbol_by_identity.empty()
        || !fs::is_regular_file(fs::path{k_control_dso})
        || fs::is_symlink(fs::path{k_control_dso})
        || !fs::is_regular_file(fs::path{k_control_file})
        || fs::is_symlink(fs::path{k_control_file})) {
        fail(
            "self-test",
            "compiled SQL/control/helper artifacts are incomplete");
    }
    const guc_profile_counts expected_gucs =
        expected_guc_profile(k_profile);
    if ((k_profile == "production"
            && (expected_gucs.total != 73
                || expected_gucs.test_only != 0))
        || (k_profile == "test_indexes"
            && (expected_gucs.total != 76
                || expected_gucs.test_only != 3))) {
        fail("self-test", "compiled GUC profile mapping differs");
    }
    expect_failure(
        [] {
            static_cast<void>(
                expected_guc_profile("unsupported"));
        },
        "unsupported profile");

    const std::string site_id =
        "probe:" + std::string(64, 'a');
    const std::string other_site_id =
        "probe:" + std::string(64, 'b');
    constexpr std::string_view fault_key =
        "generated_boundary_tail.std_exception";
    constexpr std::string_view generic_fault_key =
        "invoke_pg_as_cpp.cleanup_restore";
    if (!valid_fault_key(generic_fault_key)
        || !valid_fault_key(k_backend_scoped_vacuum_fault_key)
        || !valid_fault_key(std::string(128, 'a'))
        || valid_fault_key(std::string_view{})
        || valid_fault_key(std::string(129, 'a'))
        || valid_fault_key("invalid-key")
        || valid_fault_key("Invalid")) {
        fail("self-test", "runtime fault-key grammar differs");
    }
    json generic_catalog =
        plan.cases.begin()->second.catalog;
    generic_catalog["fault_key"] =
        std::string{generic_fault_key};
    const json normalized_generic_catalog =
        normalized_catalog_entry(
            generic_catalog, "self-test generic catalog");
    if (require_string_field(
            normalized_generic_catalog,
            "fault_key",
            "self-test generic catalog")
        != generic_fault_key) {
        fail(
            "self-test",
            "catalog parser rejected a valid generic fault key");
    }
    json backend_catalog =
        plan.cases.begin()->second.catalog;
    backend_catalog["fault_key"] =
        std::string{k_backend_scoped_vacuum_fault_key};
    backend_catalog["expected"]["postgres_cleanup_events"] =
        json::array();
    const json normalized_backend_catalog =
        normalized_catalog_entry(
            backend_catalog,
            "self-test backend-scoped VACUUM catalog");
    if (require_string_field(
            normalized_backend_catalog,
            "fault_key",
            "self-test backend-scoped VACUUM catalog")
        != k_backend_scoped_vacuum_fault_key) {
        fail(
            "self-test",
            "catalog parser rejected the backend-scoped VACUUM key");
    }
    json invalid_backend_catalog = backend_catalog;
    invalid_backend_catalog["expected"]
        ["postgres_cleanup_events"] =
            json::array({"fmgr_abort_observed"});
    expect_failure(
        [&invalid_backend_catalog] {
            static_cast<void>(normalized_catalog_entry(
                invalid_backend_catalog,
                "self-test invalid backend-scoped VACUUM catalog"));
        },
        "must expect no FMGR cleanup");
    json invalid_key_catalog =
        plan.cases.begin()->second.catalog;
    invalid_key_catalog["fault_key"] = "invalid-key";
    expect_failure(
        [&invalid_key_catalog] {
            static_cast<void>(normalized_catalog_entry(
                invalid_key_catalog,
                "self-test invalid-key catalog"));
        },
        "fault_key must be 1-128 bytes");

    const auto snapshot =
        [&command, &site_id](
            std::string_view relation,
            std::uint64_t visit_count,
            std::uint64_t trigger_count,
            std::uint64_t visit_sequence,
            std::uint64_t trigger_sequence,
            std::string_view snapshot_fault_key) {
            return fmt::format(
                "v2\t{}\t1\t0\t1\t{}\t{}\t0\t0\tfalse\t"
                "{}\t{}\t{}\t{}\t{}\t{}\n"
                "event\tdestructor\t"
                "generated_boundary_callable\t1\n"
                "event\tcleanup\tmemory_context.restore\t1\n"
                "event\tpostgres_cleanup\t"
                "fmgr_abort_observed\t1\n",
                k_profile,
                visit_count,
                trigger_count,
                fs::canonical(command.executable).string(),
                site_id,
                snapshot_fault_key,
                relation,
                visit_sequence,
                trigger_sequence);
        };
    const auto verify =
        [&command, &site_id, &fault_key](
            const probe_observation& observation,
            std::string_view relation) {
            verify_probe_observation(
                observation,
                k_profile,
                command.executable,
                site_id,
                fault_key,
                relation,
                probe_contract_for_relation(
                    relation, "self-test probe contract"),
                {"generated_boundary_callable"},
                {"memory_context.restore"},
                {"fmgr_abort_observed"});
        };

    const std::string natural_snapshot =
        snapshot(
            "natural_at_site", 1, 0, 1, 0, fault_key);
    probe_observation observation =
        parse_probe_observation(natural_snapshot);
    verify(observation, "natural_at_site");

    const std::string routes_snapshot =
        snapshot(
            "injected_routes_to_site", 1, 1, 2, 1, fault_key);
    const probe_observation routes =
        parse_probe_observation(routes_snapshot);
    verify(routes, "injected_routes_to_site");

    const std::string after_snapshot =
        snapshot(
            "injected_after_site_success",
            1,
            1,
            1,
            2,
            fault_key);
    const probe_observation after =
        parse_probe_observation(after_snapshot);
    verify(after, "injected_after_site_success");

    const probe_observation generic_observation =
        parse_probe_observation(
            snapshot(
                "natural_at_site",
                1,
                0,
                1,
                0,
                generic_fault_key));
    if (generic_observation.fault_key != generic_fault_key) {
        fail(
            "self-test",
            "observation parser rejected a valid generic fault key");
    }
    const std::string backend_snapshot = fmt::format(
        "v2\t{}\t0\t0\t0\t1\t1\t0\t0\tfalse\t"
        "{}\t{}\t{}\tinjected_routes_to_site\t2\t1\n"
        "event\tdestructor\t"
        "generated_boundary_callable\t1\n"
        "event\tcleanup\tmemory_context.restore\t1\n",
        k_profile,
        fs::canonical(command.executable).string(),
        site_id,
        k_backend_scoped_vacuum_fault_key);
    const probe_observation backend_observation =
        parse_probe_observation(backend_snapshot);
    verify_probe_observation(
        backend_observation,
        k_profile,
        command.executable,
        site_id,
        k_backend_scoped_vacuum_fault_key,
        "injected_routes_to_site",
        probe_contract_for_relation(
            "injected_routes_to_site",
            "self-test backend-scoped VACUUM probe contract"),
        {"generated_boundary_callable"},
        {"memory_context.restore"},
        {});
    probe_observation fabricated_backend_fmgr =
        backend_observation;
    fabricated_backend_fmgr.start_count = 1;
    fabricated_backend_fmgr.abort_count = 1;
    expect_failure(
        [&fabricated_backend_fmgr,
         &command,
         &site_id] {
            verify_probe_observation(
                fabricated_backend_fmgr,
                k_profile,
                command.executable,
                site_id,
                k_backend_scoped_vacuum_fault_key,
                "injected_routes_to_site",
                probe_contract_for_relation(
                    "injected_routes_to_site",
                    "self-test backend-scoped VACUUM probe contract"),
                {"generated_boundary_callable"},
                {"memory_context.restore"},
                {});
        },
        "FMGR/backend lifecycle");
    for (const std::string& invalid_fault_key :
         std::array{
             std::string{"invalid-key"},
             std::string{"Invalid"},
             std::string(129, 'a')}) {
        expect_failure(
            [&snapshot, &invalid_fault_key] {
                static_cast<void>(
                    parse_probe_observation(
                        snapshot(
                            "natural_at_site",
                            1,
                            0,
                            1,
                            0,
                            invalid_fault_key)));
            },
            "fault key must be 1-128 bytes");
    }

    probe_observation invalid = observation;
    invalid.invalid_count = 1;
    expect_failure(
        [&invalid, &verify] {
            verify(invalid, "natural_at_site");
        },
        "integrity");
    probe_observation overflow = observation;
    overflow.overflow_count = 1;
    expect_failure(
        [&overflow, &verify] {
            verify(overflow, "natural_at_site");
        },
        "integrity");
    probe_observation missing_abort = observation;
    missing_abort.abort_count = 0;
    expect_failure(
        [&missing_abort, &verify] {
            verify(missing_abort, "natural_at_site");
        },
        "FMGR lifecycle");
    probe_observation wrong_cleanup = observation;
    wrong_cleanup.postgres_cleanup_events.clear();
    expect_failure(
        [&wrong_cleanup, &verify] {
            verify(wrong_cleanup, "natural_at_site");
        },
        "event set differs");

    probe_observation reversed = routes;
    reversed.site_visit_sequence = 1;
    reversed.fault_trigger_sequence = 2;
    expect_failure(
        [&reversed, &verify] {
            verify(reversed, "injected_routes_to_site");
        },
        "causal");
    probe_observation missing_visit = observation;
    missing_visit.site_visit_count = 0;
    missing_visit.site_visit_sequence = 0;
    expect_failure(
        [&missing_visit, &verify] {
            verify(missing_visit, "natural_at_site");
        },
        "causal");
    probe_observation missing_trigger = routes;
    missing_trigger.fault_trigger_count = 0;
    missing_trigger.fault_trigger_sequence = 0;
    expect_failure(
        [&missing_trigger, &verify] {
            verify(
                missing_trigger,
                "injected_routes_to_site");
        },
        "causal");
    probe_observation unexpected_trigger = observation;
    unexpected_trigger.fault_trigger_count = 1;
    unexpected_trigger.fault_trigger_sequence = 2;
    expect_failure(
        [&unexpected_trigger, &verify] {
            verify(unexpected_trigger, "natural_at_site");
        },
        "causal");
    probe_observation wrong_echo = observation;
    wrong_echo.site_id = other_site_id;
    expect_failure(
        [&wrong_echo, &verify] {
            verify(wrong_echo, "natural_at_site");
        },
        "echo");

    expect_failure(
        [&routes_snapshot] {
            static_cast<void>(
                parse_probe_observation(
                    routes_snapshot
                    + "event\tpostgres_cleanup\t"
                      "fmgr_abort_observed\t1\n"));
        },
        "duplicate event");
    expect_failure(
        [&command, &site_id, &fault_key] {
            static_cast<void>(parse_probe_observation(
                fmt::format(
                    "v2\t{}\t1\t0\t1\t1\t0\t0\t0\tmaybe\t"
                    "{}\t{}\t{}\tnatural_at_site\t1\t0\n",
                    k_profile,
                    command.executable.string(),
                    site_id,
                    fault_key)));
        },
        "true or false");
    expect_failure(
        [&command, &site_id, &fault_key] {
            static_cast<void>(parse_probe_observation(
                fmt::format(
                    "v1\t{}\t1\t0\t1\t1\t0\t0\t0\tfalse\t"
                    "{}\t{}\t{}\tnatural_at_site\t1\t0\n",
                    k_profile,
                    command.executable.string(),
                    site_id,
                    fault_key)));
        },
        "header shape");
    json wrong_contract =
        probe_contract_for_relation(
            "natural_at_site", "self-test probe contract");
    wrong_contract["abi_revision"] = 1;
    expect_failure(
        [&wrong_contract] {
            require_probe_contract(
                wrong_contract,
                "natural_at_site",
                "self-test probe contract");
        },
        "differs");
    expect_failure(
        [] {
            static_cast<void>(parse_unsigned(
                "18446744073709551616",
                "self-test overflow"));
        },
        "overflows");
    expect_failure(
        [] {
            static_cast<void>(
                parse_json(
                    R"json({"value":1,"value":2})json",
                    "self-test duplicate JSON"));
        },
        "duplicate JSON key");

    const std::vector<
        std::vector<std::optional<std::string>>>
        result_rows{{
            std::optional<std::string>{"value"},
            std::nullopt}};
    if (result_rows_json(result_rows)
        != json::array(
            {json::array({"value", nullptr})})) {
        fail(
            "self-test",
            "exact recovery-row serialization differs");
    }
    return 0;
}

}  // namespace bioseq::tests::integration::safety_runtime_harness
