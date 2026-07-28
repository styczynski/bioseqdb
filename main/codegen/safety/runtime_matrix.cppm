module;

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.safety:runtime_matrix;

import :model;
import :validation;

export namespace bioseq::codegen::safety {

struct runtime_policy {
    std::uint32_t schema_version{2};
    std::vector<coverage_key> explicitly_required;
};

namespace detail {

inline constexpr std::string_view
    backend_scoped_vacuum_fault_key{
        "invoke_pg_as_cpp.vacuum_cleanup_restore"};

[[nodiscard]] inline std::string canonical_coverage_key(
    const coverage_key& key) {
    return std::string{profile_name(key.profile)}
        + ":" + std::string{coverage_dimension_name(key.dimension)}
        + ":" + key.value;
}

inline void add_coverage(
    std::map<std::string, coverage_key, std::less<>>& result,
    compilation_profile profile,
    coverage_dimension dimension,
    std::string_view value) {
    if (value.empty()) {
        safety_failure("runtime-matrix", "coverage value is empty");
    }
    coverage_key key{
        .profile = profile,
        .dimension = dimension,
        .value = std::string{value},
    };
    result.emplace(canonical_coverage_key(key), std::move(key));
}

inline void add_site_coverage(
    std::map<std::string, coverage_key, std::less<>>& result,
    compilation_profile profile,
    const error_site& site) {
    add_coverage(
        result,
        profile,
        coverage_dimension::classification,
        classification_name(site.classification));
    if (site.bridge != bridge_kind::none) {
        add_coverage(
            result,
            profile,
            coverage_dimension::bridge,
            bridge_name(site.bridge));
    }
    for (const std::string& family : site.converter_families) {
        add_coverage(
            result,
            profile,
            coverage_dimension::converter_family,
            family);
    }
    for (const std::string& family : site.owner_families) {
        add_coverage(
            result,
            profile,
            coverage_dimension::owner_family,
            family);
    }
    for (const std::string& phase : site.cleanup_phases) {
        add_coverage(
            result,
            profile,
            coverage_dimension::cleanup_phase,
            phase);
    }
}

[[nodiscard]] inline std::set<std::string, std::less<>>
runtime_event_set(
    const std::vector<std::string>& events,
    std::string_view event_kind) {
    std::set<std::string, std::less<>> result;
    for (const std::string& event : events) {
        const bool valid_name =
            !event.empty()
            && event.size() <= 64
            && ((event.front() >= 'a' && event.front() <= 'z')
                || (event.front() >= '0' && event.front() <= '9'))
            && std::ranges::all_of(
                event,
                [](const char character) {
                    return (character >= 'a' && character <= 'z')
                        || (character >= '0' && character <= '9')
                        || character == '_'
                        || character == '.';
                });
        if (!valid_name || !result.insert(event).second) {
            safety_failure(
                "runtime-matrix",
                std::string{event_kind}
                    + " is invalid or duplicated");
        }
    }
    return result;
}

}  // namespace detail

[[nodiscard]] inline std::vector<coverage_key>
derive_runtime_site_coverage(
    compilation_profile profile,
    const error_site& site) {
    std::map<std::string, coverage_key, std::less<>> coverage;
    detail::add_site_coverage(coverage, profile, site);
    std::vector<coverage_key> result;
    result.reserve(coverage.size());
    for (auto& [canonical, key] : coverage) {
        static_cast<void>(canonical);
        result.push_back(std::move(key));
    }
    return result;
}

[[nodiscard]] inline std::vector<coverage_key>
derive_required_runtime_coverage(
    const safety_ledger& production,
    const safety_ledger& test_indexes,
    const runtime_policy& policy) {
    std::map<std::string, coverage_key, std::less<>> required;
    for (const safety_ledger* ledger :
        {&production, &test_indexes}) {
        for (const safety_parent_row& parent : ledger->parents) {
            for (const error_site& site : parent.sites) {
                detail::add_site_coverage(
                    required, ledger->profile, site);
            }
        }
    }
    for (const coverage_key& key : policy.explicitly_required) {
        const std::string canonical =
            detail::canonical_coverage_key(key);
        if (!required.contains(canonical)) {
            detail::safety_failure(
                "runtime-policy",
                "required coverage key is absent from the exact ledgers");
        }
    }
    std::vector<coverage_key> result;
    result.reserve(required.size());
    for (auto& [canonical, key] : required) {
        static_cast<void>(canonical);
        result.push_back(std::move(key));
    }
    return result;
}

inline void validate_runtime_matrix(
    const runtime_matrix& matrix,
    const safety_ledger& production,
    const safety_ledger& test_indexes,
    const runtime_policy& policy,
    std::string_view production_ledger_sha256,
    std::string_view test_indexes_ledger_sha256,
    std::string_view production_sql_manifest_sha256,
    std::string_view test_indexes_sql_manifest_sha256,
    std::string_view runtime_policy_sha256,
    std::string_view production_dso_sha256,
    std::string_view test_indexes_dso_sha256,
    std::string_view runtime_producer_executable_sha256,
    std::string_view production_test_executable_sha256,
    std::string_view test_indexes_test_executable_sha256) {
    if (matrix.schema_version != 2 || policy.schema_version != 2) {
        detail::safety_failure(
            "runtime-matrix", "unsupported schema version");
    }
    if (!detail::lowercase_sha256(
            matrix.production_ledger_sha256)
        || !detail::lowercase_sha256(
            matrix.test_indexes_ledger_sha256)
        || !detail::lowercase_sha256(
            matrix.production_sql_manifest_sha256)
        || !detail::lowercase_sha256(
            matrix.test_indexes_sql_manifest_sha256)
        || !detail::lowercase_sha256(
            matrix.runtime_policy_sha256)
        || matrix.production_ledger_sha256
            != production_ledger_sha256
        || matrix.test_indexes_ledger_sha256
            != test_indexes_ledger_sha256
        || matrix.production_sql_manifest_sha256
            != production_sql_manifest_sha256
        || matrix.test_indexes_sql_manifest_sha256
            != test_indexes_sql_manifest_sha256
        || production.sql_manifest_sha256
            != matrix.production_sql_manifest_sha256
        || test_indexes.sql_manifest_sha256
            != matrix.test_indexes_sql_manifest_sha256
        || matrix.runtime_policy_sha256
            != runtime_policy_sha256) {
        detail::safety_failure(
            "runtime-matrix",
            "ledger, SQL-manifest, or reviewed-policy artifact "
            "hash is stale");
    }
    if (!detail::lowercase_sha256(
            matrix.production_dso_sha256)
        || !detail::lowercase_sha256(
            matrix.test_indexes_dso_sha256)
        || !detail::lowercase_sha256(
            matrix.runtime_producer_executable_sha256)
        || !detail::lowercase_sha256(
            matrix.production_test_executable_sha256)
        || !detail::lowercase_sha256(
            matrix.test_indexes_test_executable_sha256)
        || matrix.production_dso_sha256
            != production_dso_sha256
        || matrix.test_indexes_dso_sha256
            != test_indexes_dso_sha256
        || matrix.runtime_producer_executable_sha256
            != runtime_producer_executable_sha256
        || matrix.production_test_executable_sha256
            != production_test_executable_sha256
        || matrix.test_indexes_test_executable_sha256
            != test_indexes_test_executable_sha256) {
        detail::safety_failure(
            "runtime-matrix",
            "runtime DSO/producer/test executable artifact hash "
            "is stale");
    }
    const std::vector<coverage_key> derived =
        derive_required_runtime_coverage(
            production, test_indexes, policy);
    std::set<std::string, std::less<>> required;
    for (const coverage_key& key : derived) {
        required.insert(detail::canonical_coverage_key(key));
    }
    std::set<std::string, std::less<>> declared;
    for (const coverage_key& key : matrix.required_coverage) {
        const std::string canonical =
            detail::canonical_coverage_key(key);
        if (key.value.empty() || !declared.insert(canonical).second) {
            detail::safety_failure(
                "runtime-matrix",
                "declared coverage key is empty or duplicated");
        }
    }
    if (declared != required) {
        detail::safety_failure(
            "runtime-matrix",
            "declared coverage differs from ledger and reviewed policy");
    }

    std::set<std::string, std::less<>> case_ids;
    std::set<std::string, std::less<>> observed_coverage;
    std::set<std::string, std::less<>> invocation_fault_keys;
    for (const runtime_case& test_case : matrix.cases) {
        const std::string_view context =
            test_case.case_id.empty()
            ? std::string_view{"<unnamed-runtime-case>"}
            : std::string_view{test_case.case_id};
        const safety_ledger& active_ledger =
            test_case.profile
                    == compilation_profile::production
            ? production
            : test_indexes;
        const std::string_view active_dso_sha256 =
            test_case.profile
                    == compilation_profile::production
            ? std::string_view{matrix.production_dso_sha256}
            : std::string_view{matrix.test_indexes_dso_sha256};
        const std::string_view active_test_executable_sha256 =
            test_case.profile
                    == compilation_profile::production
            ? std::string_view{
                matrix.production_test_executable_sha256}
            : std::string_view{
                matrix.test_indexes_test_executable_sha256};
        const auto parent = std::ranges::find(
            active_ledger.parents,
            test_case.sql_identity,
            &safety_parent_row::sql_identity);
        const error_site* active_site = nullptr;
        if (parent != active_ledger.parents.end()) {
            const auto site = std::ranges::find(
                parent->sites,
                test_case.site_id,
                &error_site::site_id);
            if (site != parent->sites.end()) {
                active_site = &*site;
            }
        }
        const bool site_exists = active_site != nullptr;
        const std::string invocation_fault_key =
            std::string{profile_name(test_case.profile)}
            + "\n" + test_case.invocation_key
            + "\n" + test_case.fault_key;
        if (test_case.case_id.empty()
            || !case_ids.insert(test_case.case_id).second
            || test_case.coverage.empty()
            || test_case.sql_identity.empty()
            || test_case.site_id.empty()
            || !site_exists
            || test_case.invocation_key.empty()
            || test_case.fault_key.empty()
            || !invocation_fault_keys.insert(
                invocation_fault_key).second
            || !detail::lowercase_sha256(
                test_case.ledger_sha256)
            || !detail::lowercase_sha256(
                test_case.sql_manifest_sha256)
            || !detail::lowercase_sha256(
                test_case.active_dso_sha256)
            || test_case.active_dso_sha256
                != active_dso_sha256
            || !detail::lowercase_sha256(
                test_case.producer_executable_sha256)
            || test_case.producer_executable_sha256
                != matrix.runtime_producer_executable_sha256
            || !detail::lowercase_sha256(
                test_case.test_executable_sha256)
            || test_case.test_executable_sha256
                != active_test_executable_sha256
            || test_case.immutable_test_log_path.empty()
            || !detail::lowercase_sha256(
                test_case.immutable_test_log_sha256)
            || test_case.immutable_result_path.empty()
            || !detail::lowercase_sha256(
                test_case.immutable_result_sha256)
            || (test_case.profile
                    == compilation_profile::production
                && (test_case.ledger_sha256
                        != matrix.production_ledger_sha256
                    || test_case.sql_manifest_sha256
                        != matrix
                            .production_sql_manifest_sha256))
            || (test_case.profile
                    == compilation_profile::test_indexes
                && (test_case.ledger_sha256
                        != matrix.test_indexes_ledger_sha256
                    || test_case.sql_manifest_sha256
                        != matrix
                            .test_indexes_sql_manifest_sha256))) {
            detail::safety_failure(
                context,
                "case identity/site/fault key, coverage, or "
                "artifact binding is invalid");
        }
        if (test_case.exercised_terminal_severity) {
            detail::safety_failure(
                context,
                "runtime tests must not exercise FATAL or PANIC");
        }
        const auto expected_sentinels =
            detail::runtime_event_set(
                test_case.expected_destructor_sentinels,
                "destructor sentinel");
        const auto observed_sentinels =
            detail::runtime_event_set(
                test_case.observed_destructor_sentinels,
                "destructor sentinel");
        const bool lifetime_free_direct =
            active_site->classification
            == site_classification::lifetime_free_direct;
        if (expected_sentinels != observed_sentinels
            || (lifetime_free_direct
                && !expected_sentinels.empty())
            || (!lifetime_free_direct
                && expected_sentinels.empty())) {
            detail::safety_failure(
                context,
                "destructor sentinels did not unwind exactly once");
        }
        const auto expected_cleanup =
            detail::runtime_event_set(
                test_case.expected_cleanup_events,
                "cleanup event");
        const auto observed_cleanup =
            detail::runtime_event_set(
                test_case.observed_cleanup_events,
                "cleanup event");
        const auto expected_postgres_cleanup =
            detail::runtime_event_set(
                test_case.expected_postgres_cleanup_events,
                "PostgreSQL cleanup event");
        const auto observed_postgres_cleanup =
            detail::runtime_event_set(
                test_case.observed_postgres_cleanup_events,
                "PostgreSQL cleanup event");
        const bool backend_scoped_vacuum =
            test_case.fault_key
            == detail::backend_scoped_vacuum_fault_key;
        const bool exact_postgres_cleanup =
            backend_scoped_vacuum
            ? (expected_postgres_cleanup.empty()
               && test_case.expected_fmgr_abort_count == 0)
            : (!expected_postgres_cleanup.empty()
               && expected_postgres_cleanup.contains(
                   "fmgr_abort_observed")
               && test_case.expected_fmgr_abort_count == 1);
        if (expected_cleanup != observed_cleanup
            || expected_postgres_cleanup
                != observed_postgres_cleanup
            || !exact_postgres_cleanup
            || test_case.observed_fmgr_abort_count
                != test_case.expected_fmgr_abort_count) {
            detail::safety_failure(
                context,
                "cleanup/FMGR ABORT evidence is not exact");
        }
        const bool valid_sqlstate =
            test_case.expected_sqlstate.size() == 5
            && std::ranges::all_of(
                test_case.expected_sqlstate,
                [](const char character) {
                    return (character >= '0' && character <= '9')
                        || (character >= 'A' && character <= 'Z');
                });
        if (!valid_sqlstate
            || test_case.expected_sqlstate
                != test_case.observed_sqlstate
            || test_case.expected_message.empty()
            || test_case.expected_message
                != test_case.observed_message) {
            detail::safety_failure(
                context,
                "SQLSTATE or PostgreSQL message was not preserved");
        }
        if (test_case.expected_recovery_postcondition.empty()
            || test_case.expected_recovery_postcondition
                != test_case.observed_recovery_postcondition) {
            detail::safety_failure(
                context, "recovery postcondition did not hold");
        }
        std::set<std::string, std::less<>> case_coverage;
        std::set<std::string, std::less<>> active_site_coverage;
        for (const coverage_key& key :
             derive_runtime_site_coverage(
                 test_case.profile, *active_site)) {
            active_site_coverage.insert(
                detail::canonical_coverage_key(key));
        }
        for (const coverage_key& key : test_case.coverage) {
            const std::string canonical =
                detail::canonical_coverage_key(key);
            if (key.profile != test_case.profile
                || !required.contains(canonical)
                || !active_site_coverage.contains(canonical)
                || !case_coverage.insert(canonical).second) {
                detail::safety_failure(
                    context,
                    "case coverage is cross-profile, unknown, "
                    "duplicated, or unsupported by its exact ledger site");
            }
            observed_coverage.insert(canonical);
        }
    }
    if (observed_coverage != required) {
        detail::safety_failure(
            "runtime-matrix",
            "classification/family join has missing coverage keys");
    }
}

}  // namespace bioseq::codegen::safety
