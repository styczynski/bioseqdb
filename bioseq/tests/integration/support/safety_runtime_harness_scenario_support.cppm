/**
 * @file safety_runtime_harness_scenario_support.cppm
 * @brief Checks test-build records, probe results, and backend-exit logs.
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

export module bioseq.tests.integration.safety_runtime_harness_support:scenario_support;

import :core_support;
import :case_support;
import :manifest_support;

export namespace bioseq::tests::integration::safety_runtime_harness {

/// Read a JSON string array as a set, rejecting duplicates.
[[nodiscard]] std::set<std::string, std::less<>>
string_set(
        const json& value,
        std::string_view context) {
    if (!value.is_array()) {
        fail(context, "expected an array");
    }
    std::set<std::string, std::less<>> result;
    for (std::size_t index = 0; index < value.size(); ++index) {
        const std::string item = require_string(
            value[index],
            fmt::format("{}[{}]", context, index));
        if (!result.insert(item).second) {
            fail(context, "contains a duplicate string");
        }
    }
    return result;
}

/// Find the case's ledger entry by SQL identity and site hash, and require ERROR severity
[[nodiscard]] json resolve_ledger_site(
        const json& ledger,
        const json& catalog,
        std::string_view requested_site_sha256) {
    require_exact_fields(
        ledger,
        {
            "schema_revision",
            "profile",
            "postgresql_major",
            "postgresql_revision",
            "provenance_bundle_sha256",
            "source_list_sha256",
            "compile_database_sha256",
            "producer_executable_sha256",
            "compile_graph_sha256",
            "pcm_manifest_sha256",
            "sql_manifest_sha256",
            "compiler_facts_sha256",
            "clang_version",
            "analyzer_version",
            "generator_revision",
            "callback_root_count",
            "callback_closure_sha256",
            "parents",
            "symbol_groups",
        },
        "ledger");
    if (require_field(
            ledger, "schema_revision", "ledger") != 2
        || require_string_field(
               ledger, "profile", "ledger") != k_profile) {
        fail("ledger", "schema or profile differs");
    }
    const std::string sql_identity =
        require_string_field(
            catalog, "sql_identity", "catalog-entry");
    const json& parents =
        require_field(ledger, "parents", "ledger");
    if (!parents.is_array()) {
        fail("ledger.parents", "expected an array");
    }
    const json* matching_parent = nullptr;
    for (const json& parent : parents) {
        if (require_string_field(
                parent, "sql_identity", "ledger-parent")
            == sql_identity) {
            if (matching_parent != nullptr) {
                fail("ledger", "duplicate SQL identity");
            }
            matching_parent = &parent;
        }
    }
    if (matching_parent == nullptr) {
        fail("ledger", "catalog SQL identity is absent");
    }
    const json& sites = require_field(
        *matching_parent, "sites", "ledger-parent");
    if (!sites.is_array()) {
        fail("ledger-parent.sites", "expected an array");
    }
    const json* matching_site = nullptr;
    for (const json& site : sites) {
        if (sha256_bytes(canonical_json(site))
            == requested_site_sha256) {
            if (matching_site != nullptr) {
                fail(
                    "ledger",
                    "duplicate canonical ledger-site SHA-256");
            }
            matching_site = &site;
        }
    }
    if (matching_site == nullptr) {
        fail("ledger", "request ledger-site SHA-256 is absent");
    }
    if (require_string_field(
            catalog,
            "ledger_site_sha256",
            "catalog-entry")
        != requested_site_sha256) {
        fail(
            "ledger",
            "catalog ledger-site SHA-256 differs from request");
    }
    const json& severities = require_field(
        *matching_site, "possible_severities", "ledger-site");
    if (!severities.is_array()
        || severities.size() != 1
        || require_string(
               severities.front(),
               "ledger-site.possible_severities[0]")
            != "error") {
        fail(
            "ledger", "runtime site is not recoverable ERROR-only");
    }
    return *matching_site;
}

/// Checked activation record with the control library's path and SHA-256.
struct activation_receipt final {
    json value;
    std::string sha256;
    fs::path control_dso_path;
    std::string control_dso_sha256;
};

/// Check that an activation record matches the requested build, settings, and hmlper library.
[[nodiscard]] activation_receipt load_activation_receipt(
        const json& request,
        const fs::path& active_dso,
        std::string_view dso_sha256,
        std::string_view config_sha256,
        std::string_view executable_sha256,
        const json& config,
        const sql_manifest_contract& manifest) {
    const fs::path path{
        require_string_field(
            request,
            "activation_receipt_path",
            "request")};
    if (!path.is_absolute()
        || !fs::is_regular_file(path)
        || fs::is_symlink(path)) {
        fail(
            "activation-receipt",
            "path must be an existing absolute non-symlink file");
    }
    const std::string raw = read_file(path);
    const std::string actual_sha256 = sha256_bytes(raw);
    if (actual_sha256
        != require_string_field(
            request,
            "activation_receipt_sha256",
            "request")) {
        fail(
            "activation-receipt",
            "hash differs from request");
    }
    const json receipt =
        parse_json(raw, "activation-receipt");
    require_exact_fields(
        receipt,
        {
            "schema_revision",
            "profile",
            "case_plan_sha256",
            "test_executable_sha256",
            "requested_dso_sha256",
            "cluster_config_sha256",
            "active_dso_path",
            "active_dso_sha256",
            "cluster_system_identifier",
            "cluster_data_directory",
            "postmaster_start_time",
            "activation_nonce",
            "server_version_num",
            "extension_oid",
            "bioseq_guc_count",
            "bioseq_test_only_guc_count",
            "bioseq_guc_properties",
            "bioseq_guc_properties_sha256",
            "installed_control_sha256",
            "installed_sql_sha256",
            "sql_manifest_sha256",
            "sql_script_sha256",
            "control_dso_path",
            "control_dso_sha256",
        },
        "activation-receipt");
    const guc_profile_counts expected_gucs =
        expected_guc_profile(k_profile);
    const json& guc_properties = require_field(
        receipt,
        "bioseq_guc_properties",
        "activation-receipt");
    const std::string guc_properties_sha256 =
        require_string_field(
            receipt,
            "bioseq_guc_properties_sha256",
            "activation-receipt");
    validate_guc_property_receipt(
        guc_properties,
        guc_properties_sha256,
        manifest.gucs,
        "activation-receipt.bioseq_guc_properties");
    if (require_field(
            receipt, "schema_revision", "activation-receipt")
            != 1
        || require_string_field(
               receipt, "profile", "activation-receipt")
            != k_profile
        || fs::path{
               require_string_field(
                   receipt,
                   "active_dso_path",
                   "activation-receipt")}
               .lexically_normal()
            != active_dso.lexically_normal()
        || require_string_field(
               receipt,
               "active_dso_sha256",
               "activation-receipt")
            != dso_sha256
        || require_string_field(
               receipt,
               "requested_dso_sha256",
               "activation-receipt")
            != dso_sha256
        || require_string_field(
               receipt,
               "cluster_config_sha256",
               "activation-receipt")
            != config_sha256
        || require_string_field(
               receipt,
               "test_executable_sha256",
               "activation-receipt")
            != executable_sha256
        || require_string_field(
               receipt,
               "case_plan_sha256",
               "activation-receipt")
            != k_case_plan_sha256
        || require_string_field(
               receipt,
               "sql_manifest_sha256",
               "activation-receipt")
            != manifest.sha256
        || require_string_field(
               receipt,
               "sql_script_sha256",
               "activation-receipt")
            != manifest.script_sha256
        || require_string_field(
               receipt,
               "installed_sql_sha256",
               "activation-receipt")
            != manifest.script_sha256
        || require_string_field(
               receipt,
               "installed_control_sha256",
               "activation-receipt")
            != sha256_file(fs::path{k_control_file})
        || parse_unsigned(
               require_field(
                   receipt,
                   "bioseq_guc_count",
                   "activation-receipt")
                   .dump(),
               "activation-receipt.bioseq_guc_count")
            != expected_gucs.total
        || parse_unsigned(
               require_field(
                   receipt,
                   "bioseq_test_only_guc_count",
                   "activation-receipt")
                   .dump(),
               "activation-receipt.bioseq_test_only_guc_count")
            != expected_gucs.test_only) {
        fail(
            "activation-receipt",
            "profile, DSO, SQL, executable, GUC profile, or cluster "
            "binding differs");
    }
    const fs::path control_dso_path{
        require_string_field(
            receipt,
            "control_dso_path",
            "activation-receipt")};
    const std::string control_dso_sha256 =
        require_string_field(
            receipt,
            "control_dso_sha256",
            "activation-receipt");
    const fs::path configured_control_dso =
        require_absolute_path(
            config,
            "installed_control_dso_path",
            "cluster-config");
    if (!control_dso_path.is_absolute()
        || !fs::is_regular_file(control_dso_path)
        || fs::is_symlink(control_dso_path)
        || fs::canonical(control_dso_path)
            != fs::canonical(configured_control_dso)
        || !lowercase_sha256(control_dso_sha256)
        || sha256_file(control_dso_path)
            != control_dso_sha256
        || control_dso_sha256
            != sha256_file(fs::path{k_control_dso})
        || fs::canonical(control_dso_path)
            == fs::canonical(active_dso)
        || control_dso_sha256 == dso_sha256
        || parse_unsigned(
               require_field(
                   receipt,
                   "extension_oid",
                   "activation-receipt")
                   .dump(),
               "activation-receipt.extension_oid")
            == 0) {
        fail(
            "activation-receipt",
            "control helper, extension OID, or DSO separation differs");
    }
    return activation_receipt{
        .value = receipt,
        .sha256 = actual_sha256,
        .control_dso_path = fs::canonical(control_dso_path),
        .control_dso_sha256 = control_dso_sha256};
}

/// Function-call counters and cleanup events reported by the control, library.
struct probe_observation final {
    std::string profile;
    std::uint64_t start_count = 0;
    std::uint64_t end_count = 0;
    std::uint64_t abort_count = 0;
    std::uint64_t site_visit_count = 0;
    std::uint64_t fault_trigger_count = 0;
    std::uint64_t invalid_count = 0;
    std::uint64_t overflow_count = 0;
    bool inside_target = false;
    fs::path mapped_target_path;
    std::string site_id;
    std::string fault_key;
    std::string relation;
    std::uint64_t site_visit_sequence = 0;
    std::uint64_t fault_trigger_sequence = 0;
    std::map<std::string, std::uint64_t, std::less<>>
        destructor_events;
    std::map<std::string, std::uint64_t, std::less<>>
        cleanup_events;
    std::map<std::string, std::uint64_t, std::less<>>
        postgres_cleanup_events;
};

/// Split a protocol line without allocating its individual fields.
[[nodiscard]] std::vector<std::string_view> split(
        std::string_view value,
        char delimiter) {
    std::vector<std::string_view> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find(delimiter, begin);
        if (end == std::string_view::npos) {
            result.push_back(value.substr(begin));
            break;
        }
        result.push_back(value.substr(begin, end - begin));
        begin = end + 1;
    }
    return result;
}

/// Parse the version 2 probe report and reject malformed headers or  duplicate events.
[[nodiscard]] probe_observation parse_probe_observation(
        std::string_view value) {
    const std::vector<std::string_view> lines = split(value, '\n');
    if (lines.empty() || lines.front().empty()) {
        fail("probe observation", "header is absent");
    }
    const std::vector<std::string_view> header =
        split(lines.front(), '\t');
    if (header.size() != 16 || header[0] != "v2") {
        fail("probe observation", "header shape differs");
    }
    const auto parse_boolean =
        [](std::string_view raw, std::string_view context) {
            if (raw == "true") {
                return true;
            }
            if (raw == "false") {
                return false;
            }
            fail(context, "expected true or false");
        };
    const fs::path mapped_target_path{header[10]};
    if (!mapped_target_path.is_absolute()
        || header[10].find_first_of("\t\r\n")
            != std::string_view::npos) {
        fail(
            "probe mapped target path",
            "expected an unambiguous absolute path");
    }
    if (!probe_site_id(header[11])) {
        fail(
            "probe observation",
            "site ID is not a full ABI revision 2 probe ID");
    }
    if (!valid_fault_key(header[12])) {
        fail(
            "probe observation",
            "fault key must be 1-128 bytes from [a-z0-9_.]");
    }
    static_cast<void>(
        contract_for_relation(
            header[13], "probe observation"));
    probe_observation result{
        .profile = std::string{header[1]},
        .start_count =
            parse_unsigned(header[2], "probe START count"),
        .end_count =
            parse_unsigned(header[3], "probe END count"),
        .abort_count =
            parse_unsigned(header[4], "probe ABORT count"),
        .site_visit_count =
            parse_unsigned(
                header[5], "probe site-visit count"),
        .fault_trigger_count =
            parse_unsigned(
                header[6], "probe fault-trigger count"),
        .invalid_count =
            parse_unsigned(header[7], "probe invalid count"),
        .overflow_count =
            parse_unsigned(header[8], "probe overflow count"),
        .inside_target =
            parse_boolean(header[9], "probe inside-target state"),
        .mapped_target_path = mapped_target_path,
        .site_id = std::string{header[11]},
        .fault_key = std::string{header[12]},
        .relation = std::string{header[13]},
        .site_visit_sequence =
            parse_unsigned(
                header[14], "probe site-visit sequence"),
        .fault_trigger_sequence =
            parse_unsigned(
                header[15], "probe fault-trigger sequence")};
    for (std::size_t index = 1; index < lines.size(); ++index) {
        if (lines[index].empty()) {
            continue;
        }
        const std::vector<std::string_view> event =
            split(lines[index], '\t');
        if (event.size() != 4
            || event[0] != "event"
            || event[2].empty()) {
            fail("probe observation", "event shape differs");
        }
        std::map<std::string, std::uint64_t, std::less<>>*
            destination = nullptr;
        if (event[1] == "destructor") {
            destination = &result.destructor_events;
        } else if (event[1] == "cleanup") {
            destination = &result.cleanup_events;
        } else if (event[1] == "postgres_cleanup") {
            destination = &result.postgres_cleanup_events;
        } else {
            fail("probe observation", "event kind differs");
        }
        const std::uint64_t count =
            parse_unsigned(event[3], "probe event count");
        if (count == 0
            || !destination->emplace(
                    std::string{event[2]}, count)
                    .second) {
            fail("probe observation", "duplicate event");
        }
    }
    return result;
}

/// Require the request's file path to contain the bytes named by its SHA-256.
void verify_file_binding(
        const json& request,
        std::string_view path_field,
        std::string_view hash_field,
        std::string_view context) {
    const fs::path path{
        require_string_field(request, path_field, "request")};
    if (!path.is_absolute()
        || !fs::is_regular_file(path)
        || fs::is_symlink(path)) {
        fail(
            context,
            "bound path must be an existing absolute non-symlink file");
    }
    const std::string expected =
        require_string_field(request, hash_field, "request");
    if (!lowercase_sha256(expected)
        || sha256_file(path) != expected) {
        fail(context, "bound file SHA-256 differs");
    }
}

/// Find one case after checking the catalog profile and compiled-plan hash.
[[nodiscard]] const json& catalog_entry(
        const json& catalog,
        std::string_view case_id_value) {
    if (require_field(
            catalog, "schema_revision", "catalog") != 2
        || require_string_field(
               catalog, "profile", "catalog") != k_profile
        || require_string_field(
               catalog,
               "case_plan_sha256",
               "catalog")
            != k_case_plan_sha256) {
        fail(
            "catalog",
            "schema, profile, or case-plan binding differs");
    }
    const json& cases = require_field(
        catalog, "cases", "catalog");
    if (!cases.is_array()) {
        fail("catalog.cases", "expected an array");
    }
    const json* result = nullptr;
    for (const json& entry : cases) {
        if (require_string_field(
                entry, "case_id", "catalog-entry")
            == case_id_value) {
            if (result != nullptr) {
                fail("catalog", "duplicate case ID");
            }
            result = &entry;
        }
    }
    if (result == nullptr) {
        fail("catalog", "requested case is absent");
    }
    return *result;
}

/// Require a named JSON field to match between two protocol objects.
void require_equal_field(
        const json& left,
        const json& right,
        std::string_view field,
        std::string_view context) {
    if (require_field(left, field, context)
        != require_field(right, field, context)) {
        fail(
            context,
            fmt::format("field '{}' differs", field));
    }
}

/// Look up a SQL function signature and return its OID, failing if it is missing.
[[nodiscard]] std::uint64_t resolve_target_oid(
        const pg_connection& connection,
        std::string_view regprocedure) {
    const std::array<std::string, 1> parameters{
        std::string{regprocedure}};
    const pg_result result = connection.execute_parameters(
        "SELECT pg_catalog.to_regprocedure($1)::oid::text;",
        parameters);
    const auto row = one_row(result, "target regprocedure");
    return parse_unsigned(
        nonnull_cell(row, 0, "target regprocedure"),
        "target regprocedure OID");
}

/// Require the connection to be outside a transaction.
void require_idle(
        const pg_connection& connection,
        std::string_view context) {
    if (connection.transaction_status() != PQTRANS_IDLE) {
        fail(
            context,
            "PostgreSQL connection did not recover to PQTRANS_IDLE");
    }
}

/// Load the control library and create temporary functions to arm, inspect, and disarm probes.
void create_probe_bindings(
        const pg_connection& connection,
        const fs::path& control_dso) {
    if (!control_dso.is_absolute()
        || !fs::is_regular_file(control_dso)
        || fs::is_symlink(control_dso)) {
        fail(
            "safety runtime control DSO",
            "expected an absolute regular non-symlink helper");
    }
    const std::string library =
        sql_literal(fs::canonical(control_dso).string());
    require_success(
        connection.execute(
            "LOAD " + library + ";"),
        "load safety runtime control DSO");
    require_success(
        connection.execute(
            "CREATE FUNCTION "
            "pg_temp.bioseq_safety_runtime_probe_arm("
            "oid, text, text, text, text, text, text, text) "
            "RETURNS void "
            "AS " + library
            + ", 'bioseq_safety_runtime_probe_arm' "
              "LANGUAGE C STRICT VOLATILE;"),
        "create probe arm binding");
    require_success(
        connection.execute(
            "CREATE FUNCTION "
            "pg_temp.bioseq_safety_runtime_probe_snapshot(text) "
            "RETURNS text "
            "AS " + library
            + ", 'bioseq_safety_runtime_probe_snapshot' "
              "LANGUAGE C STRICT VOLATILE;"),
        "create probe snapshot binding");
    require_success(
        connection.execute(
            "CREATE FUNCTION "
            "pg_temp.bioseq_safety_runtime_probe_disarm(text) "
            "RETURNS void "
            "AS " + library
            + ", 'bioseq_safety_runtime_probe_disarm' "
              "LANGUAGE C STRICT VOLATILE;"),
        "create probe disarm binding");
}

/// Require exactly the expected events, with each event recorded once.
void verify_event_set(
        const std::map<
            std::string,
            std::uint64_t,
            std::less<>>& observed,
        const std::set<std::string, std::less<>>& expected,
        std::string_view context) {
    std::set<std::string, std::less<>> names;
    for (const auto& [name, count] : observed) {
        if (count != 1) {
            fail(
                context,
                fmt::format(
                    "event '{}' completed {} times",
                    name,
                    count));
        }
        names.insert(name);
    }
    if (names != expected) {
        fail(context, "observed event set differs from expectation");
    }
}

/// Check the probe's function calls, fault ordering, and cleanup events against the case.
void verify_probe_observation(
        const probe_observation& observation,
        std::string_view expected_profile,
        const fs::path& expected_target_path,
        std::string_view expected_site_id,
        std::string_view expected_fault_key,
        std::string_view expected_relation,
        const json& expected_probe_contract,
        const std::set<std::string, std::less<>>&
            expected_destructors,
        const std::set<std::string, std::less<>>&
            expected_cleanup,
        const std::set<std::string, std::less<>>&
            expected_postgres_cleanup) {
    require_probe_contract(
        expected_probe_contract,
        expected_relation,
        "expected probe contract");
    const probe_contract_values contract =
        contract_for_relation(
            expected_relation, "expected probe relation");
    if (observation.profile != expected_profile
        || fs::canonical(observation.mapped_target_path)
            != fs::canonical(expected_target_path)) {
        fail(
            "probe observation",
            "profile or mapped target identity differs");
    }
    const bool backend_scoped_vacuum =
        expected_fault_key == k_backend_scoped_vacuum_fault_key;
    const std::uint64_t expected_start_count =
        backend_scoped_vacuum ? 0U : 1U;
    const std::uint64_t expected_abort_count =
        backend_scoped_vacuum ? 0U : 1U;
    if (observation.start_count != expected_start_count
        || observation.end_count != 0
        || observation.abort_count != expected_abort_count
        || observation.inside_target) {
        fail(
            "probe observation",
            "FMGR/backend lifecycle differs");
    }
    if (observation.site_id != expected_site_id
        || observation.fault_key != expected_fault_key
        || observation.relation != expected_relation) {
        fail(
            "probe observation",
            "site, fault, or relation echo differs");
    }
    const std::uint64_t expected_visit_sequence =
        contract.ordering == "trigger_before_visit" ? 2U : 1U;
    const std::uint64_t expected_trigger_sequence =
        contract.ordering == "visit_only"
        ? 0U
        : (contract.ordering == "trigger_before_visit"
               ? 1U
               : 2U);
    if (observation.site_visit_count
            != contract.expected_visit_count
        || observation.fault_trigger_count
            != contract.expected_trigger_count
        || observation.site_visit_sequence
            != expected_visit_sequence
        || observation.fault_trigger_sequence
            != expected_trigger_sequence) {
        fail(
            "probe observation",
            "causal visit/trigger contract differs");
    }
    if (observation.invalid_count != 0
        || observation.overflow_count != 0) {
        fail(
            "probe observation",
            "integrity counters differ");
    }
    verify_event_set(
        observation.destructor_events,
        expected_destructors,
        "probe destructor events");
    verify_event_set(
        observation.cleanup_events,
        expected_cleanup,
        "probe cleanup-completion events");
    verify_event_set(
        observation.postgres_cleanup_events,
        expected_postgres_cleanup,
        "probe PostgreSQL-cleanup events");
}

/// Serialize nullable PostgreSQL result rows without losing nulls or order.
[[nodiscard]] json result_rows_json(
        const std::vector<
            std::vector<std::optional<std::string>>>& value) {
    json result = json::array();
    for (const auto& row : value) {
        json result_row = json::array();
        for (const auto& cell : row) {
            if (cell.has_value()) {
                result_row.push_back(*cell);
            } else {
                result_row.push_back(nullptr);
            }
        }
        result.push_back(std::move(result_row));
    }
    return result;
}

/// Run SQL statements in order, requiring success  and a usable transaction state.
/// Each statement must leave the session idle unless open transactions are allowed.
void execute_sql_list(
        const pg_connection& connection,
        const std::vector<std::string>& statements,
        std::string_view context,
        bool allow_open_transaction = false) {
    for (std::size_t index = 0;
         index < statements.size();
         ++index) {
        require_success(
            connection.execute(statements[index]),
            fmt::format("{}[{}]", context, index));
        if (allow_open_transaction) {
            const PGTransactionStatusType status =
                connection.transaction_status();
            if (status != PQTRANS_IDLE
                && status != PQTRANS_INTRANS) {
                fail(
                    fmt::format("{}[{}]", context, index),
                    "PostgreSQL setup transaction is not usable");
            }
        } else {
            require_idle(
                connection,
                fmt::format("{}[{}]", context, index));
        }
    }
}

/// Return the marker kind expected for a fault that ends the backend.
[[nodiscard]] std::optional<std::string_view>
terminal_guard_kind(std::string_view fault_key) noexcept {
    if (fault_key
        == "scoped_spi_session.emergency_finish_error") {
        return "finish_error";
    }
    if (fault_key
        == "scoped_spi_session.emergency_finish_non_ok") {
        return "finish_non_ok";
    }
    return std::nullopt;
}

/// Server-log file identity and the byte offset where a test begins.
struct server_log_checkpoint final {
    std::uint64_t device{};
    std::uint64_t inode{};
    std::uint64_t offset{};
};

/// Record the identity and current size of the owned server-log file
[[nodiscard]] server_log_checkpoint capture_server_log(
        const fs::path& path) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_uid != ::getuid()
        || metadata.st_nlink != 1
        || metadata.st_size < 0) {
        fail(
            "server log",
            "cannot capture exact regular private-cluster log");
    }
    return {
        .device = static_cast<std::uint64_t>(metadata.st_dev),
        .inode = static_cast<std::uint64_t>(metadata.st_ino),
        .offset = static_cast<std::uint64_t>(metadata.st_size),
    };
}

/// Read bytes added since the checkpoint, rejecting a replaced or truncated log
/// and an inerval larger than the allowed limit.
[[nodiscard]] std::string read_server_log_suffix(
        const fs::path& path,
        const server_log_checkpoint& checkpoint) {
    constexpr std::uint64_t maximum_suffix = 1024 * 1024;
    const int descriptor = ::open(
        path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        fail("server log", "cannot open sealed server log");
    }
    struct descriptor_owner final {
        int value;
        ~descriptor_owner() {
            if (value >= 0) ::close(value);
        }
    } owner{descriptor};
    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_nlink != 1
        || static_cast<std::uint64_t>(metadata.st_dev)
            != checkpoint.device
        || static_cast<std::uint64_t>(metadata.st_ino)
            != checkpoint.inode
        || metadata.st_size < 0
        || static_cast<std::uint64_t>(metadata.st_size)
            < checkpoint.offset
        || static_cast<std::uint64_t>(metadata.st_size)
                - checkpoint.offset
            > maximum_suffix) {
        fail(
            "server log",
            "server log was replaced, truncated, or exceeded its bound");
    }
    const std::size_t size = static_cast<std::size_t>(
        static_cast<std::uint64_t>(metadata.st_size)
        - checkpoint.offset);
    std::string result(size, '\0');
    std::size_t read_offset = 0;
    while (read_offset < result.size()) {
        const ssize_t count = ::pread(
            descriptor,
            result.data() + read_offset,
            result.size() - read_offset,
            static_cast<off_t>(
                checkpoint.offset + read_offset));
        if (count > 0) {
            read_offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        fail("server log", "cannot read exact appended interval");
    }
    return result;
}

/// Wait for this backend's exit-code-1 log record and reject  crash diagnostics.
[[nodiscard]] json wait_for_terminal_server_log(
        const fs::path& path,
        const server_log_checkpoint& checkpoint,
        std::uint64_t backend_pid) {
    const std::string expected_exit = fmt::format(
        "server process (PID {}) exited with exit code 1",
        backend_pid);
    std::string interval;
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        interval = read_server_log_suffix(path, checkpoint);
        if (interval.contains(expected_exit)) break;
        std::this_thread::sleep_for(
            std::chrono::milliseconds{50});
    }
    std::string lower = interval;
    std::ranges::transform(
        lower,
        lower.begin(),
        [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
    const bool fatal = interval.contains("FATAL:");
    const bool panic = interval.contains("PANIC:");
    const bool signal =
        lower.contains("terminated by signal")
        || lower.contains("was terminated by signal")
        || lower.contains("segmentation fault");
    const bool core_dump =
        lower.contains("core dump") || lower.contains("dumped core");
    if (!interval.contains(expected_exit)
        || fatal || panic || signal || core_dump
        || interval.find('\0') != std::string::npos) {
        fail(
            "terminal server log",
            "intended exit record is absent or crash substitution appears");
    }
    return json{
        {"path", path.string()},
        {"device", checkpoint.device},
        {"inode", checkpoint.inode},
        {"offset", checkpoint.offset},
        {"interval_size", interval.size()},
        {"interval_sha256", sha256_bytes(interval)},
        {"interval_text", interval},
        {"exit_record", expected_exit},
        {"fatal_present", fatal},
        {"panic_present", panic},
        {"signal_present", signal},
        {"core_dump_present", core_dump}};
}

/// Require a terminal marker path to be absent before arming a probe.
void require_terminal_marker_absent(const fs::path& path) {
    struct stat metadata {};
    if (::lstat(path.c_str(), &metadata) == 0 || errno != ENOENT) {
        fail(
            "terminal marker",
            "token-keyed marker existed before probe arm");
    }
}

/// Read the cleanup marker and check its file metadata and test case identifiers.
[[nodiscard]] json read_terminal_marker(
        const fs::path& path,
        std::string_view token,
        std::string_view site_id,
        std::string_view fault_key,
        std::string_view guard_kind,
        std::uint64_t backend_pid) {
    constexpr std::size_t maximum_marker_size = 2048;
    for (std::size_t attempt = 0; attempt < 100; ++attempt) {
        struct stat pending {};
        if (::lstat(path.c_str(), &pending) == 0) break;
        if (errno != ENOENT) {
            fail("terminal marker", "cannot inspect marker path");
        }
        std::this_thread::sleep_for(
            std::chrono::milliseconds{20});
    }
    const int descriptor = ::open(
        path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        fail("terminal marker", "durable marker is absent");
    }
    struct descriptor_owner final {
        int value;
        ~descriptor_owner() {
            if (value >= 0) ::close(value);
        }
    } owner{descriptor};
    struct stat metadata {};
    if (::fstat(descriptor, &metadata) != 0
        || !S_ISREG(metadata.st_mode)
        || metadata.st_uid != ::getuid()
        || (metadata.st_mode & 07777) != 0600
        || metadata.st_nlink != 1
        || metadata.st_size <= 0
        || static_cast<std::uint64_t>(metadata.st_size)
            > maximum_marker_size) {
        fail("terminal marker", "marker metadata differs");
    }
    std::string raw(
        static_cast<std::size_t>(metadata.st_size), '\0');
    std::size_t offset = 0;
    while (offset < raw.size()) {
        const ssize_t count = ::read(
            descriptor,
            raw.data() + offset,
            raw.size() - offset);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        fail("terminal marker", "marker read is incomplete");
    }
    const std::string expected = fmt::format(
        "{{\"schema\":\"bioseq.postgres_error_safety.terminal_marker\","
        "\"schema_revision\":1,"
        "\"token\":\"{}\",\"site_id\":\"{}\","
        "\"fault_key\":\"{}\",\"guard_kind\":\"{}\","
        "\"backend_pid\":{}}}\n",
        token,
        site_id,
        fault_key,
        guard_kind,
        backend_pid);
    if (raw != expected) {
        fail("terminal marker", "marker payload differs");
    }
    return json{
        {"path", path.string()},
        {"sha256", sha256_bytes(raw)},
        {"payload", raw},
        {"schema", "bioseq.postgres_error_safety.terminal_marker"},
        {"schema_revision", 1},
        {"token", token},
        {"site_id", site_id},
        {"fault_key", fault_key},
        {"guard_kind", guard_kind},
        {"backend_pid", backend_pid},
        {"mode", 0600},
        {"uid", static_cast<std::uint64_t>(metadata.st_uid)},
        {"link_count",
         static_cast<std::uint64_t>(metadata.st_nlink)}};
}

}  // namespace bioseq::tests::integration::safety_runtime_harness
