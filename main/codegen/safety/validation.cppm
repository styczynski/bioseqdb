module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.safety:validation;

import :model;

export namespace bioseq::codegen::safety {

struct ledger_policy {
    std::size_t expected_parent_count{272};
    std::size_t expected_symbol_count{270};
    std::string expected_postgresql_tag{"REL_17_10"};
    std::string expected_postgresql_revision{
        "25c49f3a4a742ba283f5cc43cc7f1d361552e917"};
    std::set<std::string, std::less<>> shared_symbols{
        "from_array",
        "mmsearch",
    };
    std::set<std::string, std::less<>> required_provenance_records{
        "pg17.elog_macro",
        "pg17.ereport_macro",
        "pg17.errfinish",
        "pg17.errstart",
        "pg17.pg_re_throw",
        "pg17.pg_try_rethrow_macros",
        "pg17.rethrow_declarations",
        "pg17.rethrow_error",
        "pg17.severity_levels",
    };
    std::set<std::string, std::less<>>
        pinned_pg_no_longjmp_records{
            "pg17.pfree",
        };
    std::map<std::string, provenance_kind, std::less<>>
        required_provenance_kinds{
            {
                "pg17.severity_levels",
                provenance_kind::terminal_severity_seed,
            },
            {
                "pg17.ereport_macro",
                provenance_kind::recoverable_error_seed,
            },
            {
                "pg17.elog_macro",
                provenance_kind::recoverable_error_seed,
            },
            {
                "pg17.pg_try_rethrow_macros",
                provenance_kind::bridge_primitive,
            },
            {
                "pg17.rethrow_declarations",
                provenance_kind::bridge_primitive,
            },
            {
                "pg17.errstart",
                provenance_kind::recoverable_error_seed,
            },
            {
                "pg17.errfinish",
                provenance_kind::recoverable_error_seed,
            },
            {
                "pg17.rethrow_error",
                provenance_kind::recoverable_error_seed,
            },
            {
                "pg17.pg_re_throw",
                provenance_kind::recoverable_error_seed,
            },
            {
                "pg17.pfree",
                provenance_kind::pg_no_longjmp_leaf,
            },
        };
    std::map<
        std::string,
        std::set<postgres_severity>,
        std::less<>>
        required_provenance_severities{
            {
                "pg17.severity_levels",
                {
                    postgres_severity::recoverable_error,
                    postgres_severity::fatal,
                    postgres_severity::panic,
                },
            },
            {
                "pg17.ereport_macro",
                {postgres_severity::recoverable_error},
            },
            {
                "pg17.elog_macro",
                {postgres_severity::recoverable_error},
            },
            {"pg17.pg_try_rethrow_macros", {}},
            {"pg17.rethrow_declarations", {}},
            {
                "pg17.errstart",
                {
                    postgres_severity::recoverable_error,
                    postgres_severity::fatal,
                    postgres_severity::panic,
                },
            },
            {
                "pg17.errfinish",
                {
                    postgres_severity::recoverable_error,
                    postgres_severity::fatal,
                    postgres_severity::panic,
                },
            },
            {
                "pg17.rethrow_error",
                {postgres_severity::recoverable_error},
            },
            {
                "pg17.pg_re_throw",
                {
                    postgres_severity::recoverable_error,
                    postgres_severity::fatal,
                },
            },
            {"pg17.pfree", {}},
        };
    std::map<std::string, std::string, std::less<>>
        required_provenance_fragment_sha256{
            {
                "pg17.severity_levels",
                "b1c2030376e5858d484b65928856171de3b9f62957f2d26566795150027b5f6d",
            },
            {
                "pg17.ereport_macro",
                "790c1855de5610cfc507aa3221efcb40b241e59fb659d00463e9e25366330743",
            },
            {
                "pg17.elog_macro",
                "288f9d80a16a31057b6004c07b060a9cb7565bd197d31d0b20e2727d6317d848",
            },
            {
                "pg17.pg_try_rethrow_macros",
                "d96fc354e6c3e8eb0f0b531e32d0b97de0e2865f656f1557152ead276f8cdb49",
            },
            {
                "pg17.rethrow_declarations",
                "e32a0526deb30bfa66316389c51c4ce1e338e92b30e54a086fe0a92968fbf03a",
            },
            {
                "pg17.errstart",
                "a20f4d33c3d85bd4ffd35d9ff943e814e06d9c0825fa0ebce8696594858083af",
            },
            {
                "pg17.errfinish",
                "f04234bff935be4007825804dccd11adf5fb78c3685e8c6f13aadefc1c7bcac5",
            },
            {
                "pg17.rethrow_error",
                "13286a15f4aa4d8f6b063d7b0a9ffb67cc75306886278ddbdf2d94189c2cbff5",
            },
            {
                "pg17.pg_re_throw",
                "c3c4b2cd89e2e9190ec7fd82b861f16b856cb7dda3b93ed5d9e8d8c42a659893",
            },
            {
                "pg17.pfree",
                "894b9cdaa6d1b9d5bbab45ca5c41c8c03f05ba078578bbd475a744049c585496",
            },
        };
    std::map<std::string, std::pair<std::size_t, std::size_t>,
             std::less<>>
        required_provenance_byte_spans{
            {"pg17.severity_levels", {593, 2155}},
            {"pg17.ereport_macro", {6037, 6985}},
            {"pg17.elog_macro", {9575, 9819}},
            {"pg17.pg_try_rethrow_macros", {15244, 16531}},
            {"pg17.rethrow_declarations", {18571, 18740}},
            {"pg17.errstart", {11150, 14888}},
            {"pg17.errfinish", {15040, 19122}},
            {"pg17.rethrow_error", {57136, 58661}},
            {"pg17.pg_re_throw", {58665, 60663}},
            {"pg17.pfree", {3488, 3520}},
        };
    std::map<std::string, std::string, std::less<>>
        required_provenance_files{
            {"pg17.severity_levels", "src/include/utils/elog.h"},
            {"pg17.ereport_macro", "src/include/utils/elog.h"},
            {"pg17.elog_macro", "src/include/utils/elog.h"},
            {
                "pg17.pg_try_rethrow_macros",
                "src/include/utils/elog.h",
            },
            {
                "pg17.rethrow_declarations",
                "src/include/utils/elog.h",
            },
            {
                "pg17.errstart",
                "src/backend/utils/error/elog.c",
            },
            {
                "pg17.errfinish",
                "src/backend/utils/error/elog.c",
            },
            {
                "pg17.rethrow_error",
                "src/backend/utils/error/elog.c",
            },
            {
                "pg17.pg_re_throw",
                "src/backend/utils/error/elog.c",
            },
            {
                "pg17.pfree",
                "src/include/utils/palloc.h",
            },
        };
    std::map<std::string, std::string, std::less<>>
        required_provenance_file_sha256{
            {
                "src/include/utils/elog.h",
                "e6b0864bf3fae574d5f6676ca3797dd7bccd70b69b5ebf1a0cdb86f635853e0d",
            },
            {
                "src/backend/utils/error/elog.c",
                "c0fa4ad0198a13bd121080688b81d1faf8a352fb0852b01675b090f50c4db55c",
            },
            {
                "src/include/utils/palloc.h",
                "f6a9f047b0f343811a07f997c34aebd1b8b07e5fd60b1db82f7cae5ed2594f5a",
            },
            {
                "src/backend/utils/mmgr/mcxt.c",
                "4f4a96524812c55ff3b63a0cb22b69af258f8e69c7ddc20b2250a8c1769edc37",
            },
        };
    std::map<std::string, std::string, std::less<>>
        required_definition_files{
            {
                "pg17.pfree",
                "src/backend/utils/mmgr/mcxt.c",
            },
        };
    std::map<std::string, std::pair<std::size_t, std::size_t>,
             std::less<>>
        required_definition_byte_spans{
            {"pg17.pfree", {47020, 47352}},
        };
    std::map<std::string, std::string, std::less<>>
        required_definition_fragment_sha256{
            {
                "pg17.pfree",
                "bc4585116ec9c6c32bc607184b57831d1b48a2918f3430a07808456f6e322a1a",
            },
        };
};

namespace detail {

[[noreturn]] inline void safety_failure(
    std::string_view context,
    std::string_view message) {
    throw std::runtime_error(
        "PostgreSQL safety evidence rejected "
        + std::string{context} + ": " + std::string{message});
}

[[nodiscard]] inline bool lowercase_sha256(std::string_view value) noexcept {
    return value.size() == 64
        && std::ranges::all_of(
            value,
            [](unsigned char character) {
                return std::isdigit(character)
                    || (character >= 'a' && character <= 'f');
            });
}

[[nodiscard]] inline bool revision_sha1(std::string_view value) noexcept {
    return value.size() == 40
        && std::ranges::all_of(
            value,
            [](unsigned char character) {
                return std::isdigit(character)
                    || (character >= 'a' && character <= 'f');
            });
}

[[nodiscard]] inline bool valid_source_range(
    const source_range& value) noexcept {
    return value.begin.line > 0 && value.begin.column > 0
        && value.end.line > 0 && value.end.column > 0
        && value.end.byte_offset > value.begin.byte_offset
        && (value.end.line > value.begin.line
            || (value.end.line == value.begin.line
                && value.end.column >= value.begin.column));
}

[[nodiscard]] inline bool absolute_source_path(
    std::string_view value) {
    if (value.empty()) return false;
    const std::filesystem::path path{value};
    return path.is_absolute()
        && path.lexically_normal() == path;
}

[[nodiscard]] inline bool postgresql_source_path(
    std::string_view value) {
    if (!value.starts_with("src/")) return false;
    const std::filesystem::path path{value};
    return path.is_relative()
        && path.lexically_normal() == path
        && std::ranges::none_of(
            path,
            [](const std::filesystem::path& component) {
                return component == "..";
            });
}

[[nodiscard]] inline bool library_provenance_kind(
    provenance_kind kind) noexcept {
    return kind == provenance_kind::standard_library_declaration
        || kind == provenance_kind::system_library_declaration
        || kind == provenance_kind::compiler_builtin_declaration;
}

[[nodiscard]] inline bool target_provenance_kind(
    provenance_kind kind) noexcept {
    return kind == provenance_kind::pg_no_longjmp_leaf
        || library_provenance_kind(kind);
}

[[nodiscard]] inline std::set<postgres_severity>
severity_set(const std::vector<postgres_severity>& severities) {
    return std::set<postgres_severity>(
        severities.begin(), severities.end());
}

[[nodiscard]] inline std::string provenance_target_key(
    const provenance_record& record) {
    return record.target_usr + "\n"
        + record.target_qualified_name + "\n"
        + record.authority_id;
}

[[nodiscard]] inline std::string provenance_location_key(
    const provenance_record& record) {
    return record.upstream_file + "\n"
        + std::to_string(record.upstream_span.begin.byte_offset)
        + ":" + std::to_string(record.upstream_span.begin.line)
        + ":" + std::to_string(record.upstream_span.begin.column)
        + "-"
        + std::to_string(record.upstream_span.end.byte_offset)
        + ":" + std::to_string(record.upstream_span.end.line)
        + ":" + std::to_string(record.upstream_span.end.column);
}

[[nodiscard]] inline std::string routine_leaf(
    std::string_view identity) {
    const std::size_t arguments = identity.find('(');
    const std::string_view qualified =
        arguments == std::string_view::npos
        ? identity
        : identity.substr(0, arguments);
    const std::size_t separator = qualified.rfind('.');
    return std::string{
        separator == std::string_view::npos
            ? qualified
            : qualified.substr(separator + 1)};
}

[[nodiscard]] inline std::string declaration_leaf(
    std::string_view value) {
    while (value.starts_with("::")) value.remove_prefix(2);
    const std::size_t arguments = value.find('(');
    if (arguments != std::string_view::npos) {
        value = value.substr(0, arguments);
    }
    const std::size_t separator = value.rfind("::");
    return std::string{
        separator == std::string_view::npos
            ? value
            : value.substr(separator + 2)};
}

[[nodiscard]] inline bool same_source_range(
    const source_range& left,
    const source_range& right) noexcept {
    return left.begin.byte_offset == right.begin.byte_offset
        && left.begin.line == right.begin.line
        && left.begin.column == right.begin.column
        && left.end.byte_offset == right.end.byte_offset
        && left.end.line == right.end.line
        && left.end.column == right.end.column;
}

[[nodiscard]] inline bool has_terminal_severity(
    const std::vector<postgres_severity>& values) noexcept {
    return std::ranges::any_of(
        values,
        [](postgres_severity value) {
            return value == postgres_severity::fatal
                || value == postgres_severity::panic;
        });
}

[[nodiscard]] inline const provenance_record* find_provenance(
    const provenance_bundle& bundle,
    std::string_view record_id) noexcept {
    const auto record = std::ranges::find(
        bundle.records,
        record_id,
        &provenance_record::record_id);
    return record == bundle.records.end()
        ? nullptr
        : std::addressof(*record);
}

inline void validate_crossing_type(
    const crossing_type& crossing,
    std::string_view site_id) {
    if (crossing.canonical_type.empty()
        || !absolute_source_path(crossing.source_file)) {
        safety_failure(site_id, "crossing type has no canonical spelling");
    }
    if (!valid_source_range(crossing.declaration_span)) {
        safety_failure(site_id, "crossing type has an invalid source span");
    }
    if (crossing.crosses_postgres_longjmp
        && !crossing.trivially_destructible) {
        safety_failure(
            site_id,
            "a nontrivially destructible type crosses PostgreSQL longjmp");
    }
}

inline void validate_allowlist_reference(
    const provenance_reference& reference,
    const provenance_bundle& bundle,
    const error_site& site) {
    const provenance_record* record =
        find_provenance(bundle, reference.record_id);
    if (record == nullptr
        || record->kind != provenance_kind::pg_no_longjmp_leaf) {
        safety_failure(
            site.site_id,
            "no-longjmp evidence does not name a reviewed PG leaf");
    }
    if (reference.postgresql_revision != bundle.postgresql_revision
        || reference.upstream_file != record->upstream_file
        || reference.fragment_sha256 != record->fragment_sha256
        || !lowercase_sha256(reference.fragment_sha256)
        || !valid_source_range(reference.upstream_span)
        || !same_source_range(
            reference.upstream_span,
            record->upstream_span)
        || reference.rationale.empty()
        || reference.rationale != record->rationale) {
        safety_failure(
            site.site_id,
            "no-longjmp evidence is stale or incomplete");
    }
    if (has_terminal_severity(record->severities)) {
        safety_failure(
            site.site_id,
            "terminal severity cannot be placed on the no-longjmp allowlist");
    }
}

inline void validate_bridge_evidence(
    const error_site& site,
    const provenance_bundle& bundle) {
    if (site.bridge == bridge_kind::none) {
        if (site.bridge_evidence) {
            safety_failure(
                site.site_id,
                "direct site carries bridge primitive evidence");
        }
        return;
    }
    if (!site.bridge_evidence) {
        safety_failure(
            site.site_id,
            "bridged site lacks bridge primitive evidence");
    }
    const bridge_transition_proof& evidence =
        *site.bridge_evidence;
    std::string expected_implementation;
    std::string expected_module;
    std::string expected_source_suffix;
    std::string expected_region;
    std::string expected_target;
    std::string expected_contract_id;
    std::string expected_contract_sha256;
    if (site.bridge == bridge_kind::invoke_pg_as_cpp) {
        expected_implementation =
            "bioseq::postgres::support::invoke_pg_as_cpp";
        expected_module =
            "bioseq.postgres.support:error_boundary";
        expected_source_suffix =
            "/main/common/postgres/support/error_boundary.cppm";
        expected_region =
            "bioseq.postgres.support.error_boundary/"
            "invoke_pg_as_cpp";
        expected_contract_id =
            "bioseq.postgres.support.error_boundary/error_capture";
        expected_contract_sha256 =
            "f217b3f824a03a5d3cf977fb25c8ace"
            "4a8765904e13e46dcfcc0fbd8c67f3099";
    } else if (
        site.bridge == bridge_kind::cross_transaction_callback) {
        expected_implementation =
            "bioseq::postgres::support::"
            "invoke_pg_across_transactions_as_cpp";
        expected_module =
            "bioseq.postgres.support:error_boundary";
        expected_source_suffix =
            "/main/common/postgres/support/error_boundary.cppm";
        expected_region =
            "bioseq.postgres.support.error_boundary/"
            "invoke_pg_across_transactions_as_cpp";
        expected_contract_id =
            "bioseq.postgres.support.error_boundary/error_capture";
        expected_contract_sha256 =
            "f217b3f824a03a5d3cf977fb25c8ace"
            "4a8765904e13e46dcfcc0fbd8c67f3099";
    } else if (site.bridge == bridge_kind::subtransaction) {
        static const std::map<
            std::string,
            std::pair<std::string, std::string>,
            std::less<>> exact_regions{
                {
                    "bioseq::postgres::support::subtransaction::begin",
                    {
                        "BeginInternalSubTransaction",
                        "begin",
                    },
                },
                {
                    "bioseq::postgres::support::subtransaction::commit",
                    {
                        "ReleaseCurrentSubTransaction",
                        "commit",
                    },
                },
                {
                    "bioseq::postgres::support::subtransaction::rollback",
                    {
                        "RollbackAndReleaseCurrentSubTransaction",
                        "rollback",
                    },
                },
                {
                    "bioseq::postgres::support::subtransaction::~subtransaction",
                    {
                        "RollbackAndReleaseCurrentSubTransaction",
                        "~subtransaction",
                    },
                },
            };
        const auto region = exact_regions.find(
            evidence.implementation_qualified_name);
        if (region == exact_regions.end()) {
            safety_failure(
                site.site_id,
                "SQL subtransaction bridge is not an exact "
                "reviewed implementation");
        }
        expected_implementation =
            evidence.implementation_qualified_name;
        expected_module = "bioseq.pg_sql_boundary";
        expected_source_suffix =
            "/main/common/pg_sql_boundary/"
            "call_context_implementation.cppm";
        expected_region =
            "bioseq.postgres.support.sql_boundary.context/"
            + region->second.second;
        expected_target = region->second.first;
    } else if (
        site.bridge == bridge_kind::generated_boundary_tail) {
        expected_implementation =
            "bioseq::postgres::support::invoke_postgres_callback";
        expected_module = "bioseq.pg_sql_boundary";
        expected_source_suffix =
            "/main/common/pg_sql_boundary/index.cppm";
        expected_region =
            "bioseq.postgres.support.sql_boundary/rethrow_tail";
    } else {
        safety_failure(
            site.site_id,
            "bridge kind has no exact protected implementation");
    }
    const std::string_view expected_record =
        site.bridge == bridge_kind::generated_boundary_tail
        ? std::string_view{"pg17.rethrow_declarations"}
        : std::string_view{"pg17.pg_try_rethrow_macros"};
    const provenance_record* record =
        find_provenance(bundle, evidence.primitive.record_id);
    const bool target_matches =
        expected_target.empty()
        || evidence.target_qualified_name == expected_target
        || (!expected_target.contains("::")
            && evidence.target_qualified_name.ends_with(
                std::string{"::"} + expected_target));
    if (record == nullptr
        || record->record_id != expected_record
        || record->kind != provenance_kind::bridge_primitive
        || evidence.primitive.postgresql_revision
            != bundle.postgresql_revision
        || evidence.primitive.upstream_file
            != record->upstream_file
        || !valid_source_range(
            evidence.primitive.upstream_span)
        || !same_source_range(
            evidence.primitive.upstream_span,
            record->upstream_span)
        || evidence.primitive.fragment_sha256
            != record->fragment_sha256
        || !lowercase_sha256(
            evidence.primitive.fragment_sha256)
        || evidence.primitive.rationale.empty()
        || evidence.primitive.rationale != record->rationale
        || evidence.target_usr.empty()
        || evidence.target_qualified_name.empty()
        || !absolute_source_path(
            evidence.transition_source_file)
        || !valid_source_range(
            evidence.transition_source_span)
        || evidence.implementation_usr.empty()
        || evidence.implementation_qualified_name.empty()
        || evidence.implementation_module.empty()
        || !absolute_source_path(
            evidence.implementation_source_file)
        || !valid_source_range(
            evidence.implementation_source_span)
        || evidence.implementation_qualified_name
            != expected_implementation
        || evidence.implementation_module != expected_module
        || !evidence.implementation_source_file.ends_with(
            expected_source_suffix)
        || !lowercase_sha256(
            evidence.implementation_body_sha256)
        || evidence.implementation_contract_id
            != expected_contract_id
        || evidence.implementation_contract_sha256
            != expected_contract_sha256
        || evidence.protected_region_id != expected_region
        || !target_matches
        || !record->severities.empty()) {
        safety_failure(
            site.site_id,
            "bridge evidence does not match the pinned primitive, "
            "target, and transition range");
    }
}

inline void validate_error_site(
    const error_site& site,
    const provenance_bundle& bundle) {
    if (site.site_id.empty()
        || !absolute_source_path(site.source_file)
        || site.callee_or_macro.empty()
        || site.transitive_call_path.empty()
        || !valid_source_range(site.expanded_source_span)) {
        safety_failure(
            site.site_id.empty() ? "<unnamed-site>" : site.site_id,
            "site identity, span, callee, and call path are required");
    }
    if (!site.severity_constant_propagated
        || site.possible_severities.size() != 1
        || site.possible_severities.front()
            != postgres_severity::recoverable_error) {
        safety_failure(
            site.site_id,
            has_terminal_severity(site.possible_severities)
                ? "FATAL/PANIC is reachable"
                : "severity is unresolved or is not exactly ERROR");
    }
    if (site.resolution != resolution_status::direct
        && site.resolution != resolution_status::resolved_indirect) {
        safety_failure(
            site.site_id,
            "call target resolution is " + std::string{
                resolution_name(site.resolution)});
    }
    if (site.classification == site_classification::unclassified) {
        safety_failure(site.site_id, "recoverable ERROR is unclassified");
    }
    validate_bridge_evidence(site, bundle);
    for (const call_path_step& step : site.transitive_call_path) {
        if (step.usr.empty() || step.qualified_declaration.empty()
            || !absolute_source_path(step.source_file)
            || !valid_source_range(step.source_span)) {
            safety_failure(
                site.site_id,
                "transitive call path contains an unresolved step");
        }
    }
    for (const crossing_type& crossing : site.crossing_types) {
        validate_crossing_type(crossing, site.site_id);
    }
    for (const crossing_type& crossing :
         site.bridge_callable_crossing_types) {
        validate_crossing_type(crossing, site.site_id);
        if (!crossing.trivially_destructible) {
            safety_failure(
                site.site_id,
                "bridge callable contains a nontrivial crossing type");
        }
    }

    switch (site.classification) {
        case site_classification::bridged:
            if (site.bridge != bridge_kind::invoke_pg_as_cpp
                || !site.analyzer_entered_bridge_callable
                || site.generated_boundary_tail) {
                safety_failure(
                    site.site_id,
                    "bridged proof does not enter invoke_pg_as_cpp");
            }
            break;
        case site_classification::specialized_bridge:
            if ((site.bridge != bridge_kind::subtransaction
                 && site.bridge
                     != bridge_kind::cross_transaction_callback)
                || (site.bridge == bridge_kind::subtransaction
                    && site.analyzer_entered_bridge_callable)
                || (site.bridge
                        == bridge_kind::cross_transaction_callback
                    && !site.analyzer_entered_bridge_callable)
                || site.generated_boundary_tail) {
                safety_failure(
                    site.site_id,
                    "specialized bridge is not an audited transaction bridge");
            }
            break;
        case site_classification::lifetime_free_direct:
            if (site.bridge != bridge_kind::none
                || !site.bridge_callable_crossing_types.empty()
                || site.analyzer_entered_bridge_callable
                || site.generated_boundary_tail
                || std::ranges::any_of(
                    site.crossing_types,
                    [](const crossing_type& crossing) {
                        return !crossing.trivially_destructible;
                    })) {
                safety_failure(
                    site.site_id,
                    "direct leaf is not lifetime-free");
            }
            break;
        case site_classification::terminal_after_unwind:
            if (site.bridge != bridge_kind::generated_boundary_tail
                || !site.generated_boundary_tail
                || site.analyzer_entered_bridge_callable
                || !site.bridge_callable_crossing_types.empty()
                || std::ranges::any_of(
                    site.crossing_types,
                    [](const crossing_type& crossing) {
                        return !crossing.trivially_destructible;
                    })
                || (site.callee_or_macro != "ReThrowError"
                    && site.callee_or_macro != "ereport")) {
                safety_failure(
                    site.site_id,
                    "terminal tail is not the generated post-unwind boundary");
            }
            break;
        case site_classification::unclassified:
            safety_failure(site.site_id, "site is unclassified");
    }
    if (site.no_longjmp_allowlist_evidence) {
        validate_allowlist_reference(
            *site.no_longjmp_allowlist_evidence, bundle, site);
    }
}

inline void validate_audited_stop(
    const audited_stop& stop,
    const provenance_bundle& bundle) {
    if (stop.stop_id.empty()
        || !absolute_source_path(stop.source_file)
        || stop.target_usr.empty()
        || stop.target_qualified_name.empty()
        || stop.transitive_call_path.empty()
        || !valid_source_range(stop.expanded_source_span)
        || (stop.resolution != resolution_status::direct
            && stop.resolution
                != resolution_status::resolved_indirect)) {
        safety_failure(
            stop.stop_id.empty()
                ? "<unnamed-audited-stop>"
                : stop.stop_id,
            "audited stop lacks resolved identity, range, or call path");
    }
    for (const call_path_step& step :
         stop.transitive_call_path) {
        if (step.usr.empty()
            || step.qualified_declaration.empty()
            || !absolute_source_path(step.source_file)
            || !valid_source_range(step.source_span)) {
            safety_failure(
                stop.stop_id,
                "audited stop call path contains an unresolved step");
        }
    }
    const provenance_record* record =
        find_provenance(bundle, stop.evidence.record_id);
    provenance_kind required_kind{
        provenance_kind::pg_no_longjmp_leaf};
    switch (stop.domain) {
        case audited_stop_domain::postgresql_server:
            required_kind = provenance_kind::pg_no_longjmp_leaf;
            break;
        case audited_stop_domain::standard_library:
            required_kind =
                provenance_kind::standard_library_declaration;
            break;
        case audited_stop_domain::system_library:
            required_kind =
                provenance_kind::system_library_declaration;
            break;
        case audited_stop_domain::compiler_builtin:
            required_kind =
                provenance_kind::compiler_builtin_declaration;
            break;
    }
    if (record == nullptr || record->kind != required_kind
        || stop.evidence.postgresql_revision
            != bundle.postgresql_revision
        || stop.evidence.upstream_file != record->upstream_file
        || !valid_source_range(stop.evidence.upstream_span)
        || !same_source_range(
            stop.evidence.upstream_span,
            record->upstream_span)
        || stop.evidence.fragment_sha256
            != record->fragment_sha256
        || !lowercase_sha256(
            stop.evidence.fragment_sha256)
        || stop.evidence.rationale.empty()
        || stop.evidence.rationale != record->rationale
        || declaration_leaf(record->spelling)
            != declaration_leaf(
                stop.target_qualified_name)
        || record->target_usr != stop.target_usr
        || record->target_qualified_name
            != stop.target_qualified_name
        || record->authority_id.empty()
        || !record->severities.empty()) {
        safety_failure(
            stop.stop_id,
            "audited stop does not exactly match reviewed provenance");
    }
    const bool library =
        stop.domain != audited_stop_domain::postgresql_server;
    if ((library
            && (stop.library_version.empty()
                || stop.library_version
                    != record->library_version
                || !stop.user_code_callback_free
                || !record->user_code_callback_free))
        || (!library
            && (!stop.library_version.empty()
                || stop.user_code_callback_free))) {
        safety_failure(
            stop.stop_id,
            "audited stop has an invalid library/callback contract");
    }
}

inline void validate_analysis(
    const analysis_completeness& analysis,
    std::string_view identity) {
    if (!analysis.template_instantiation_complete
        || !analysis.macro_expansion_complete || !analysis.cfg_complete
        || !analysis.lifetime_analysis_complete
        || !analysis.call_graph_fixed_point_complete) {
        safety_failure(identity, "analysis closure is incomplete");
    }
    if (!analysis.unresolved_indirect_targets.empty()
        || !analysis.unavailable_bodies.empty()
        || !analysis.control_flow_assembly_sites.empty()) {
        safety_failure(
            identity,
            "analysis contains unresolved targets, bodies, or control flow");
    }
}

}  // namespace detail

inline void validate_provenance_bundle(
    const provenance_bundle& bundle,
    const ledger_policy& policy = {}) {
    if (bundle.schema_version != 2 || bundle.postgresql_major != 17
        || bundle.postgresql_tag
            != policy.expected_postgresql_tag
        || bundle.postgresql_revision
            != policy.expected_postgresql_revision
        || !detail::revision_sha1(bundle.postgresql_revision)) {
        detail::safety_failure(
            "provenance",
            "bundle is not pinned to an exact PostgreSQL 17 revision");
    }
    std::set<std::string, std::less<>> record_ids;
    std::map<std::string, std::string, std::less<>>
        whole_file_hashes;
    std::map<std::string, std::string, std::less<>>
        target_authorities;
    std::map<std::string, std::string, std::less<>>
        target_locations;
    bool recoverable_seed = false;
    bool terminal_seed = false;
    bool bridge_seed = false;
    for (const provenance_record& record : bundle.records) {
        if (record.record_id.empty()
            || !record_ids.insert(record.record_id).second
            || record.spelling.empty() || record.upstream_file.empty()
            || !detail::valid_source_range(record.upstream_span)
            || !detail::lowercase_sha256(record.upstream_file_sha256)
            || !detail::lowercase_sha256(record.fragment_sha256)
            || record.rationale.empty()) {
            detail::safety_failure(
                record.record_id.empty()
                    ? "provenance"
                    : record.record_id,
                "record is incomplete, duplicated, or unhashed");
        }
        const auto [file_hash, inserted_file_hash] =
            whole_file_hashes.emplace(
                record.upstream_file,
                record.upstream_file_sha256);
        if (!inserted_file_hash
            && file_hash->second
                != record.upstream_file_sha256) {
            detail::safety_failure(
                record.record_id,
                "one provenance file has conflicting whole-file hashes");
        }
        recoverable_seed |=
            record.kind == provenance_kind::recoverable_error_seed;
        terminal_seed |=
            record.kind == provenance_kind::terminal_severity_seed;
        bridge_seed |= record.kind == provenance_kind::bridge_primitive;
        if (record.kind == provenance_kind::terminal_severity_seed
            && !detail::has_terminal_severity(record.severities)) {
            detail::safety_failure(
                record.record_id,
                "terminal seed does not contain FATAL or PANIC");
        }
        if (record.kind == provenance_kind::pg_no_longjmp_leaf
            && !record.severities.empty()) {
            detail::safety_failure(
                record.record_id,
                "PG no-longjmp evidence must be severity-free");
        }
        if (std::ranges::any_of(
                record.severities,
                [](postgres_severity severity) {
                    return severity == postgres_severity::unresolved;
                })) {
            detail::safety_failure(
                record.record_id,
                "reviewed provenance cannot contain unresolved severity");
        }
        const bool library_declaration =
            detail::library_provenance_kind(record.kind);
        const bool exact_target_record =
            detail::target_provenance_kind(record.kind);
        if (exact_target_record
            && (record.target_usr.empty()
                || record.target_qualified_name.empty()
                || record.authority_id.empty())) {
            detail::safety_failure(
                record.record_id,
                "target provenance lacks exact USR, qualified name, "
                "or authority");
        }
        if (!exact_target_record
            && (!record.target_usr.empty()
                || !record.target_qualified_name.empty()
                || !record.authority_id.empty())) {
            detail::safety_failure(
                record.record_id,
                "non-target provenance carries target authority keys");
        }
        if (exact_target_record) {
            const bool inserted_target =
                target_authorities.emplace(
                    detail::provenance_target_key(record),
                    record.record_id).second;
            if (!inserted_target) {
                detail::safety_failure(
                    record.record_id,
                    "multiple provenance records claim one target "
                    "and authority");
            }
            const auto [location, inserted_location] =
                target_locations.emplace(
                    detail::provenance_location_key(record),
                    record.record_id);
            if (!inserted_location
                && location->second != record.record_id) {
                detail::safety_failure(
                    record.record_id,
                    "multiple target records claim one declaration "
                    "source range");
            }
        }
        if (library_declaration
            && (record.library_version.empty()
                || record.authority_id != record.library_version
                || !record.authority_id.starts_with("clang:"))) {
            detail::safety_failure(
                record.record_id,
                "library provenance has no exact compiler/library "
                "authority");
        }
        if (!library_declaration
            && (!record.library_version.empty()
                || record.user_code_callback_free)) {
            detail::safety_failure(
                record.record_id,
                "non-library provenance carries a library version "
                "or callback contract");
        }
        if (record.kind == provenance_kind::pg_no_longjmp_leaf
            && (record.authority_id
                    != bundle.postgresql_revision
                || !detail::postgresql_source_path(
                    record.upstream_file)
                || !record.definition
                || !detail::postgresql_source_path(
                    record.definition->upstream_file)
                || !detail::valid_source_range(
                    record.definition->upstream_span)
                || !detail::lowercase_sha256(
                    record.definition->upstream_file_sha256)
                || !detail::lowercase_sha256(
                    record.definition->fragment_sha256))) {
            detail::safety_failure(
                record.record_id,
                "PostgreSQL leaf provenance lacks an exact "
                "declaration and definition chain");
        }
        if (record.kind != provenance_kind::pg_no_longjmp_leaf
            && record.definition) {
            detail::safety_failure(
                record.record_id,
                "non-PostgreSQL-leaf provenance carries a "
                "definition chain");
        }
        if (record.definition) {
            const auto [definition_hash, inserted_definition_hash] =
                whole_file_hashes.emplace(
                    record.definition->upstream_file,
                    record.definition->upstream_file_sha256);
            if (!inserted_definition_hash
                && definition_hash->second
                    != record.definition->upstream_file_sha256) {
                detail::safety_failure(
                    record.record_id,
                    "one provenance file has conflicting "
                    "whole-file hashes");
            }
        }
        if ((record.kind
                 == provenance_kind::standard_library_declaration
             || record.kind
                 == provenance_kind::system_library_declaration)
            && !detail::absolute_source_path(
                record.upstream_file)) {
            detail::safety_failure(
                record.record_id,
                "standard/system declaration provenance path is "
                "not absolute and normalized");
        }
        if (record.kind
                == provenance_kind::compiler_builtin_declaration
            && record.upstream_file
                != "clang-builtin:" + record.authority_id) {
            detail::safety_failure(
                record.record_id,
                "compiler builtin provenance has a foreign "
                "authority path");
        }
        if (!exact_target_record
            && !policy.required_provenance_records.contains(
                record.record_id)) {
            detail::safety_failure(
                record.record_id,
                "unreviewed or stale non-target provenance record");
        }
    }
    if (!recoverable_seed || !terminal_seed || !bridge_seed) {
        detail::safety_failure(
            "provenance",
            "recoverable, terminal, and bridge seed classes are required");
    }
    for (const std::string& required :
         policy.required_provenance_records) {
        const provenance_record* record =
            detail::find_provenance(bundle, required);
        if (record == nullptr) {
            detail::safety_failure(
                "provenance",
                "required record is missing: " + required);
        }
        const auto expected_fragment =
            policy.required_provenance_fragment_sha256.find(required);
        const auto expected_kind =
            policy.required_provenance_kinds.find(required);
        const auto expected_severities =
            policy.required_provenance_severities.find(required);
        const auto expected_span =
            policy.required_provenance_byte_spans.find(required);
        const auto expected_file =
            policy.required_provenance_files.find(required);
        if (expected_fragment
                == policy.required_provenance_fragment_sha256.end()
            || expected_kind
                == policy.required_provenance_kinds.end()
            || expected_severities
                == policy.required_provenance_severities.end()
            || expected_span
                == policy.required_provenance_byte_spans.end()
            || expected_file
                == policy.required_provenance_files.end()
            || record->fragment_sha256
                != expected_fragment->second
            || record->kind != expected_kind->second
            || detail::severity_set(record->severities)
                != expected_severities->second
            || detail::severity_set(record->severities).size()
                != record->severities.size()
            || record->upstream_file != expected_file->second
            || !policy.required_provenance_file_sha256.contains(
                record->upstream_file)
            || record->upstream_file_sha256
                != policy.required_provenance_file_sha256.at(
                    record->upstream_file)
            || record->upstream_span.begin.byte_offset
                != expected_span->second.first
            || record->upstream_span.end.byte_offset
                != expected_span->second.second) {
            detail::safety_failure(
                required,
                "required PostgreSQL seed kind, severity, hash, "
                "or byte span is stale");
        }
    }
    for (const std::string& pinned :
         policy.pinned_pg_no_longjmp_records) {
        const provenance_record* record =
            detail::find_provenance(bundle, pinned);
        if (record == nullptr) continue;
        const auto expected_kind =
            policy.required_provenance_kinds.find(pinned);
        const auto expected_severities =
            policy.required_provenance_severities.find(pinned);
        const auto expected_fragment =
            policy.required_provenance_fragment_sha256.find(pinned);
        const auto expected_span =
            policy.required_provenance_byte_spans.find(pinned);
        const auto expected_file =
            policy.required_provenance_files.find(pinned);
        const auto expected_definition_fragment =
            policy.required_definition_fragment_sha256.find(pinned);
        const auto expected_definition_span =
            policy.required_definition_byte_spans.find(pinned);
        const auto expected_definition_file =
            policy.required_definition_files.find(pinned);
        if (expected_kind
                == policy.required_provenance_kinds.end()
            || expected_severities
                == policy.required_provenance_severities.end()
            || expected_fragment
                == policy.required_provenance_fragment_sha256.end()
            || expected_span
                == policy.required_provenance_byte_spans.end()
            || expected_file
                == policy.required_provenance_files.end()
            || expected_definition_fragment
                == policy.required_definition_fragment_sha256.end()
            || expected_definition_span
                == policy.required_definition_byte_spans.end()
            || expected_definition_file
                == policy.required_definition_files.end()
            || record->kind != expected_kind->second
            || detail::severity_set(record->severities)
                != expected_severities->second
            || detail::severity_set(record->severities).size()
                != record->severities.size()
            || record->fragment_sha256
                != expected_fragment->second
            || record->upstream_file != expected_file->second
            || record->upstream_span.begin.byte_offset
                != expected_span->second.first
            || record->upstream_span.end.byte_offset
                != expected_span->second.second
            || !policy.required_provenance_file_sha256.contains(
                record->upstream_file)
            || record->upstream_file_sha256
                != policy.required_provenance_file_sha256.at(
                    record->upstream_file)
            || !record->definition
            || record->definition->upstream_file
                != expected_definition_file->second
            || record->definition->upstream_span.begin.byte_offset
                != expected_definition_span->second.first
            || record->definition->upstream_span.end.byte_offset
                != expected_definition_span->second.second
            || record->definition->fragment_sha256
                != expected_definition_fragment->second
            || !policy.required_provenance_file_sha256.contains(
                record->definition->upstream_file)
            || record->definition->upstream_file_sha256
                != policy.required_provenance_file_sha256.at(
                    record->definition->upstream_file)
            || record->definition->upstream_file
                == record->upstream_file) {
            detail::safety_failure(
                pinned,
                "pinned PostgreSQL leaf declaration/definition "
                "provenance is stale");
        }
    }
}

inline void validate_safety_ledger(
    const safety_ledger& ledger,
    const provenance_bundle& provenance,
    std::string_view reviewed_provenance_sha256,
    const ledger_policy& policy = {}) {
    validate_provenance_bundle(provenance, policy);
    if (ledger.schema_version != 2 || ledger.postgresql_major != 17
        || ledger.postgresql_revision != provenance.postgresql_revision
        || ledger.provenance_bundle_sha256
            != reviewed_provenance_sha256
        || !detail::lowercase_sha256(
            ledger.provenance_bundle_sha256)
        || !detail::lowercase_sha256(
            ledger.source_list_sha256)
        || !detail::lowercase_sha256(
            ledger.compile_database_sha256)
        || !detail::lowercase_sha256(
            ledger.producer_executable_sha256)
        || !detail::lowercase_sha256(ledger.compile_graph_sha256)
        || !detail::lowercase_sha256(
            ledger.pcm_manifest_sha256)
        || !detail::lowercase_sha256(
            ledger.sql_manifest_sha256)
        || !detail::lowercase_sha256(
            ledger.compiler_facts_sha256)
        || (ledger.callback_root_count == 0
                ? !ledger.callback_closure_sha256.empty()
                : !detail::lowercase_sha256(
                      ledger.callback_closure_sha256))
        || ledger.clang_version.empty()
        || ledger.analyzer_version.empty()
        || ledger.generator_revision.empty()) {
        detail::safety_failure(
            profile_name(ledger.profile),
            "ledger provenance is stale or not PostgreSQL 17");
    }
    if (ledger.parents.size() != policy.expected_parent_count) {
        detail::safety_failure(
            profile_name(ledger.profile),
            "expected " + std::to_string(policy.expected_parent_count)
                + " parent rows, found "
                + std::to_string(ledger.parents.size()));
    }
    if (ledger.symbol_groups.size() != policy.expected_symbol_count) {
        detail::safety_failure(
            profile_name(ledger.profile),
            "expected " + std::to_string(policy.expected_symbol_count)
                + " symbol groups, found "
                + std::to_string(ledger.symbol_groups.size()));
    }

    std::set<std::string, std::less<>> parent_identities;
    std::set<std::string, std::less<>> site_ids;
    std::set<std::string, std::less<>> stop_ids;
    std::set<std::string, std::less<>>
        used_stopping_provenance_records;
    std::map<std::string, const safety_parent_row*, std::less<>>
        parents_by_identity;
    for (const safety_parent_row& parent : ledger.parents) {
        const std::string_view context =
            parent.sql_identity.empty()
            ? std::string_view{"<unnamed-parent>"}
            : std::string_view{parent.sql_identity};
        if (parent.profile != ledger.profile || parent.owner_usr.empty()
            || parent.qualified_declaration.empty()
            || !detail::absolute_source_path(
                parent.owner_source_file)
            || parent.generated_wrapper.empty()
            || parent.public_symbol.empty()
            || !detail::valid_source_range(parent.owner_source_span)
            || !detail::lowercase_sha256(
                parent.transitive_closure_sha256)
            || !parent_identities.insert(parent.sql_identity).second) {
            detail::safety_failure(
                context,
                "parent identity, ownership, span, wrapper, or hash is invalid");
        }
        if (parent.terminal_site_count != 0) {
            detail::safety_failure(
                context, "terminal-severity site count is not zero");
        }
        if (parent.recoverable_site_count != parent.sites.size()) {
            detail::safety_failure(
                context, "recoverable site count does not match site rows");
        }
        if (parent.audited_stop_count
            != parent.audited_stops.size()) {
            detail::safety_failure(
                context,
                "audited stop count does not match stop rows");
        }
        detail::validate_analysis(parent.analysis, context);
        for (const error_site& site : parent.sites) {
            if (!site_ids.insert(site.site_id).second
                || stop_ids.contains(site.site_id)) {
                detail::safety_failure(
                    context, "site_id is not globally unique");
            }
            detail::validate_error_site(site, provenance);
            if (site.no_longjmp_allowlist_evidence) {
                used_stopping_provenance_records.insert(
                    site.no_longjmp_allowlist_evidence->record_id);
            }
        }
        for (const audited_stop& stop :
             parent.audited_stops) {
            if (!stop_ids.insert(stop.stop_id).second
                || site_ids.contains(stop.stop_id)) {
                detail::safety_failure(
                    context,
                    "audited stop_id is not globally unique");
            }
            detail::validate_audited_stop(stop, provenance);
            used_stopping_provenance_records.insert(
                stop.evidence.record_id);
        }
        parents_by_identity.emplace(parent.sql_identity, &parent);
    }
    for (const provenance_record& record : provenance.records) {
        const bool stopping_authority =
            record.kind == provenance_kind::pg_no_longjmp_leaf
            || (detail::library_provenance_kind(record.kind)
                && record.user_code_callback_free);
        if (stopping_authority
            && !used_stopping_provenance_records.contains(
                record.record_id)) {
            detail::safety_failure(
                record.record_id,
                "reviewed stopping provenance record is stale or "
                "unused by this profile ledger");
        }
    }

    std::set<std::string, std::less<>> grouped_identities;
    std::set<std::string, std::less<>> public_symbols;
    std::set<std::string, std::less<>> actual_shared_symbols;
    for (const symbol_group& group : ledger.symbol_groups) {
        if (group.public_symbol.empty() || group.generated_wrapper.empty()
            || group.finfo_symbol != "pg_finfo_" + group.public_symbol
            || !public_symbols.insert(group.public_symbol).second
            || group.sql_identities.empty()) {
            detail::safety_failure(
                group.public_symbol.empty()
                    ? "<unnamed-symbol>"
                    : group.public_symbol,
                "symbol owner is incomplete or duplicated");
        }
        if (group.sql_identities.size() > 1) {
            actual_shared_symbols.insert(group.public_symbol);
        }
        for (const std::string& identity : group.sql_identities) {
            if (!parent_identities.contains(identity)
                || !grouped_identities.insert(identity).second
                || detail::routine_leaf(identity)
                    != group.public_symbol) {
                detail::safety_failure(
                    group.public_symbol,
                    "symbol sharing crosses SQL names or misses a parent row");
            }
            const safety_parent_row* parent =
                parents_by_identity.at(identity);
            if (parent->public_symbol != group.public_symbol
                || parent->generated_wrapper
                    != group.generated_wrapper) {
                detail::safety_failure(
                    identity,
                    "parent and symbol-group ownership disagree");
            }
        }
    }
    if (grouped_identities != parent_identities) {
        detail::safety_failure(
            profile_name(ledger.profile),
            "symbol groups do not cover exactly the parent identities");
    }
    if (actual_shared_symbols != policy.shared_symbols) {
        detail::safety_failure(
            profile_name(ledger.profile),
            "same-name overload sharing set is not exact");
    }
}

inline void validate_profile_pair(
    const safety_ledger& production,
    const safety_ledger& test_indexes,
    const provenance_bundle& provenance,
    std::string_view reviewed_provenance_sha256,
    const ledger_policy& policy = {}) {
    if (production.profile != compilation_profile::production
        || test_indexes.profile
            != compilation_profile::test_indexes) {
        detail::safety_failure(
            "profile-pair",
            "production and test_indexes ledgers are both required");
    }
    validate_safety_ledger(
        production, provenance, reviewed_provenance_sha256, policy);
    validate_safety_ledger(
        test_indexes, provenance, reviewed_provenance_sha256, policy);

    std::map<std::string, std::string, std::less<>>
        production_symbols;
    std::map<std::string, std::string, std::less<>>
        test_symbols;
    for (const safety_parent_row& parent : production.parents) {
        production_symbols.emplace(
            parent.sql_identity, parent.public_symbol);
    }
    for (const safety_parent_row& parent : test_indexes.parents) {
        test_symbols.emplace(parent.sql_identity, parent.public_symbol);
    }
    if (production_symbols != test_symbols) {
        detail::safety_failure(
            "profile-pair",
            "profile identity or public-symbol closure differs");
    }
}

}  // namespace bioseq::codegen::safety
