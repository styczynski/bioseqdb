module;

#include <algorithm>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.safety:contract_tests;

import :analyzer_contract_tests;
import :dso;
import :fmgr_ledger;
import :model;
import :runtime_matrix;
import :serialization;
import :validation;

export namespace bioseq::codegen::safety {

namespace detail {

[[nodiscard]] inline source_range fixture_span(
    std::size_t begin = 1,
    std::size_t end = 2) {
    return {
        .begin = {
            .byte_offset = begin,
            .line = begin,
            .column = 1,
        },
        .end = {
            .byte_offset = end,
            .line = end,
            .column = 1,
        },
    };
}

[[nodiscard]] inline provenance_record fixture_provenance_record(
    std::string id,
    provenance_kind kind,
    std::vector<postgres_severity> severities = {}) {
    return {
        .record_id = std::move(id),
        .kind = kind,
        .spelling = "fixture",
        .upstream_file = "src/include/utils/elog.h",
        .upstream_span = fixture_span(),
        .upstream_file_sha256 = std::string(64, 'a'),
        .fragment_sha256 = std::string(64, 'b'),
        .rationale = "Contract fixture provenance.",
        .severities = std::move(severities),
    };
}

[[nodiscard]] inline provenance_bundle fixture_provenance() {
    provenance_bundle bundle{
        .postgresql_tag = "REL_17_10",
        .postgresql_revision =
            "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
    };
    bundle.records = {
        fixture_provenance_record(
            "pg17.severity_levels",
            provenance_kind::terminal_severity_seed,
            {
                postgres_severity::recoverable_error,
                postgres_severity::fatal,
                postgres_severity::panic,
            }),
        fixture_provenance_record(
            "pg17.ereport_macro",
            provenance_kind::recoverable_error_seed,
            {postgres_severity::recoverable_error}),
        fixture_provenance_record(
            "pg17.elog_macro",
            provenance_kind::recoverable_error_seed,
            {postgres_severity::recoverable_error}),
        fixture_provenance_record(
            "pg17.pg_try_rethrow_macros",
            provenance_kind::bridge_primitive),
        fixture_provenance_record(
            "pg17.rethrow_declarations",
            provenance_kind::bridge_primitive),
        fixture_provenance_record(
            "pg17.errstart",
            provenance_kind::recoverable_error_seed,
            {
                postgres_severity::recoverable_error,
                postgres_severity::fatal,
                postgres_severity::panic,
            }),
        fixture_provenance_record(
            "pg17.errfinish",
            provenance_kind::recoverable_error_seed,
            {
                postgres_severity::recoverable_error,
                postgres_severity::fatal,
                postgres_severity::panic,
            }),
        fixture_provenance_record(
            "pg17.rethrow_error",
            provenance_kind::recoverable_error_seed,
            {postgres_severity::recoverable_error}),
        fixture_provenance_record(
            "pg17.pg_re_throw",
            provenance_kind::recoverable_error_seed,
            {
                postgres_severity::recoverable_error,
                postgres_severity::fatal,
            }),
    };
    const auto apply_exact_seed =
        [&bundle](std::string_view record_id,
                  std::string_view upstream_file,
                  std::size_t begin,
                  std::size_t end,
                  std::string_view upstream_file_sha256,
                  std::string_view fragment_sha256) {
            for (provenance_record& record : bundle.records) {
                if (record.record_id != record_id) continue;
                record.upstream_file = upstream_file;
                record.upstream_span = fixture_span(begin, end);
                record.upstream_file_sha256 =
                    upstream_file_sha256;
                record.fragment_sha256 = fragment_sha256;
                return;
            }
            throw std::runtime_error(
                "missing fixture provenance record");
        };
    constexpr std::string_view elog_h_sha256 =
        "e6b0864bf3fae574d5f6676ca3797dd7bccd70b69b5ebf1a0cdb86f635853e0d";
    constexpr std::string_view elog_c_sha256 =
        "c0fa4ad0198a13bd121080688b81d1faf8a352fb0852b01675b090f50c4db55c";
    apply_exact_seed(
        "pg17.severity_levels",
        "src/include/utils/elog.h",
        593,
        2155,
        elog_h_sha256,
        "b1c2030376e5858d484b65928856171de3b9f62957f2d26566795150027b5f6d");
    apply_exact_seed(
        "pg17.ereport_macro",
        "src/include/utils/elog.h",
        6037,
        6985,
        elog_h_sha256,
        "790c1855de5610cfc507aa3221efcb40b241e59fb659d00463e9e25366330743");
    apply_exact_seed(
        "pg17.elog_macro",
        "src/include/utils/elog.h",
        9575,
        9819,
        elog_h_sha256,
        "288f9d80a16a31057b6004c07b060a9cb7565bd197d31d0b20e2727d6317d848");
    apply_exact_seed(
        "pg17.pg_try_rethrow_macros",
        "src/include/utils/elog.h",
        15244,
        16531,
        elog_h_sha256,
        "d96fc354e6c3e8eb0f0b531e32d0b97de0e2865f656f1557152ead276f8cdb49");
    apply_exact_seed(
        "pg17.rethrow_declarations",
        "src/include/utils/elog.h",
        18571,
        18740,
        elog_h_sha256,
        "e32a0526deb30bfa66316389c51c4ce1e338e92b30e54a086fe0a92968fbf03a");
    apply_exact_seed(
        "pg17.errstart",
        "src/backend/utils/error/elog.c",
        11150,
        14888,
        elog_c_sha256,
        "a20f4d33c3d85bd4ffd35d9ff943e814e06d9c0825fa0ebce8696594858083af");
    apply_exact_seed(
        "pg17.errfinish",
        "src/backend/utils/error/elog.c",
        15040,
        19122,
        elog_c_sha256,
        "f04234bff935be4007825804dccd11adf5fb78c3685e8c6f13aadefc1c7bcac5");
    apply_exact_seed(
        "pg17.rethrow_error",
        "src/backend/utils/error/elog.c",
        57136,
        58661,
        elog_c_sha256,
        "13286a15f4aa4d8f6b063d7b0a9ffb67cc75306886278ddbdf2d94189c2cbff5");
    apply_exact_seed(
        "pg17.pg_re_throw",
        "src/backend/utils/error/elog.c",
        58665,
        60663,
        elog_c_sha256,
        "c3c4b2cd89e2e9190ec7fd82b861f16b856cb7dda3b93ed5d9e8d8c42a659893");
    return bundle;
}

[[nodiscard]] inline provenance_record fixture_pfree_provenance() {
    return {
        .record_id = "pg17.pfree",
        .kind = provenance_kind::pg_no_longjmp_leaf,
        .spelling = "pfree",
        .upstream_file = "src/include/utils/palloc.h",
        .upstream_span = fixture_span(3488, 3520),
        .upstream_file_sha256 =
            "f6a9f047b0f343811a07f997c34aebd1b8b07e5fd60b1db82f7cae5ed2594f5a",
        .fragment_sha256 =
            "894b9cdaa6d1b9d5bbab45ca5c41c8c03f05ba078578bbd475a744049c585496",
        .rationale =
            "Releases a valid live PostgreSQL-owned allocation.",
        .target_usr = "c:@F@pfree#*v#",
        .target_qualified_name = "pfree",
        .authority_id =
            "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
        .definition = provenance_definition{
            .upstream_file =
                "src/backend/utils/mmgr/mcxt.c",
            .upstream_span = fixture_span(47020, 47352),
            .upstream_file_sha256 =
                "4f4a96524812c55ff3b63a0cb22b69af258f8e69c7ddc20b2250a8c1769edc37",
            .fragment_sha256 =
                "bc4585116ec9c6c32bc607184b57831d1b48a2918f3430a07808456f6e322a1a",
        },
    };
}

[[nodiscard]] inline analysis_completeness fixture_analysis() {
    return {
        .template_instantiation_complete = true,
        .macro_expansion_complete = true,
        .cfg_complete = true,
        .lifetime_analysis_complete = true,
        .call_graph_fixed_point_complete = true,
    };
}

[[nodiscard]] inline crossing_type fixture_crossing_type() {
    return {
        .canonical_type = "int",
        .origin = crossing_origin::automatic_object,
        .source_file = "/fixture/fixture.cppm",
        .declaration_span = fixture_span(),
        .trivially_destructible = true,
        .crosses_postgres_longjmp = true,
    };
}

[[nodiscard]] inline std::optional<bridge_transition_proof>
fixture_bridge_evidence(bridge_kind bridge) {
    if (bridge == bridge_kind::none) return std::nullopt;
    const bool tail =
        bridge == bridge_kind::generated_boundary_tail;
    std::string target_usr;
    std::string target_name;
    std::string implementation_usr;
    std::string implementation_name;
    std::string implementation_module;
    std::string implementation_source;
    std::string protected_region;
    switch (bridge) {
        case bridge_kind::invoke_pg_as_cpp:
            target_usr = "c:@N@fixture@F@bridge";
            target_name = "fixture::bridge";
            implementation_usr =
                "c:@N@bioseq@N@pg_utils@F@invoke_pg_as_cpp";
            implementation_name =
                "bioseq::pg_utils::invoke_pg_as_cpp";
            implementation_module =
                "bioseq.pg_support:error_boundary";
            implementation_source =
                "/fixture/main/common/pg_support/"
                "error_boundary.cppm";
            protected_region =
                "bioseq.pg_utils.error_bridge/invoke_pg_as_cpp";
            break;
        case bridge_kind::sql_call_context:
            target_usr = "c:@N@fixture@F@bridge";
            target_name = "fixture::bridge";
            implementation_usr =
                "c:@N@bioseq@N@pg_utils@F@"
                "invoke_pg_across_transactions_as_cpp";
            implementation_name =
                "bioseq::pg_utils::"
                "invoke_pg_across_transactions_as_cpp";
            implementation_module =
                "bioseq.pg_support:error_boundary";
            implementation_source =
                "/fixture/main/common/pg_support/"
                "error_boundary.cppm";
            protected_region =
                "bioseq.pg_utils.error_bridge/"
                "invoke_pg_across_transactions_as_cpp";
            break;
        case bridge_kind::sql_subtransaction:
            target_usr = "c:@F@BeginInternalSubTransaction";
            target_name = "BeginInternalSubTransaction";
            implementation_usr =
                "c:@N@bioseq@N@pg_utils@S@sql_subtransaction@"
                "F@begin";
            implementation_name =
                "bioseq::pg_utils::sql_subtransaction::begin";
            implementation_module = "bioseq.pg_sql_boundary";
            implementation_source =
                "/fixture/main/common/pg_sql_boundary/"
                "call_context_implementation.cpp";
            protected_region =
                "bioseq.pg_utils.sql_boundary.context/begin";
            break;
        case bridge_kind::generated_boundary_tail:
            target_usr = "c:@F@ReThrowError";
            target_name = "ReThrowError";
            implementation_usr =
                "c:@N@bioseq@N@pg_utils@F@invoke_sql_boundary";
            implementation_name =
                "bioseq::pg_utils::invoke_sql_boundary";
            implementation_module =
                "bioseq.pg_sql_boundary";
            implementation_source =
                "/fixture/main/common/pg_sql_boundary/"
                "index.cppm";
            protected_region =
                "bioseq.pg_utils.sql_boundary/rethrow_tail";
            break;
        case bridge_kind::none: break;
    }
    return bridge_transition_proof{
        .primitive = {
            .record_id =
                tail
                ? "pg17.rethrow_declarations"
                : "pg17.pg_try_rethrow_macros",
            .postgresql_revision =
                "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
            .upstream_file = "src/include/utils/elog.h",
            .upstream_span =
                tail
                ? fixture_span(18571, 18740)
                : fixture_span(15244, 16531),
            .fragment_sha256 =
                tail
                ? "e32a0526deb30bfa66316389c51c4ce1e338e92b30e54a086fe0a92968fbf03a"
                : "d96fc354e6c3e8eb0f0b531e32d0b97de0e2865f656f1557152ead276f8cdb49",
            .rationale = "Contract fixture provenance.",
        },
        .target_usr = std::move(target_usr),
        .target_qualified_name = std::move(target_name),
        .transition_source_file = "/fixture/fixture.cppm",
        .transition_source_span = fixture_span(),
        .implementation_usr = std::move(implementation_usr),
        .implementation_qualified_name =
            std::move(implementation_name),
        .implementation_module =
            std::move(implementation_module),
        .implementation_source_file =
            std::move(implementation_source),
        .implementation_source_span = fixture_span(),
        .implementation_body_sha256 = std::string(64, 'd'),
        .implementation_contract_id =
            bridge == bridge_kind::invoke_pg_as_cpp
                    || bridge == bridge_kind::sql_call_context
            ? "bioseq.pg_utils.error_bridge/error_capture/v1"
            : "",
        .implementation_contract_sha256 =
            bridge == bridge_kind::invoke_pg_as_cpp
                    || bridge == bridge_kind::sql_call_context
            ? "cbf583abe710257bf2a1999718c076fa"
              "2d70e203ae424bbba083eee0b4fcc83c"
            : "",
        .protected_region_id = std::move(protected_region),
    };
}

[[nodiscard]] inline error_site fixture_site(
    std::string id,
    std::string callee,
    site_classification classification,
    bridge_kind bridge,
    bool entered_bridge,
    bool generated_tail) {
    return {
        .site_id = std::move(id),
        .source_file = "/fixture/fixture.cppm",
        .expanded_source_span = fixture_span(),
        .callee_or_macro = std::move(callee),
        .transitive_call_path = {{
            .usr = "c:@F@fixture",
            .qualified_declaration = "fixture::owner",
            .source_file = "/fixture/fixture.cppm",
            .source_span = fixture_span(),
        }},
        .possible_severities = {
            postgres_severity::recoverable_error,
        },
        .severity_constant_propagated = true,
        .resolution = resolution_status::direct,
        .classification = classification,
        .bridge = bridge,
        .crossing_types = {fixture_crossing_type()},
        .bridge_callable_crossing_types =
            entered_bridge
            ? std::vector<crossing_type>{fixture_crossing_type()}
            : std::vector<crossing_type>{},
        .bridge_evidence = fixture_bridge_evidence(bridge),
        .analyzer_entered_bridge_callable = entered_bridge,
        .generated_boundary_tail = generated_tail,
        .converter_families = {"fixture_converter"},
        .owner_families = {"fixture_owner"},
        .cleanup_phases = {"fixture_cleanup"},
    };
}

[[nodiscard]] inline safety_parent_row fixture_parent(
    std::string identity,
    std::string symbol,
    std::string wrapper,
    compilation_profile profile,
    error_site site) {
    return {
        .sql_identity = std::move(identity),
        .owner_usr = "c:@F@fixture",
        .qualified_declaration = "fixture::owner",
        .owner_source_file = "/fixture/fixture.cppm",
        .owner_source_span = fixture_span(),
        .generated_wrapper = std::move(wrapper),
        .public_symbol = std::move(symbol),
        .profile = profile,
        .transitive_closure_sha256 = std::string(64, 'd'),
        .recoverable_site_count = 1,
        .terminal_site_count = 0,
        .analysis = fixture_analysis(),
        .sites = {std::move(site)},
    };
}

[[nodiscard]] inline safety_ledger fixture_ledger(
    compilation_profile profile) {
    const std::string suffix{
        profile == compilation_profile::production
            ? "production"
            : "test_indexes"};
    safety_ledger ledger{
        .profile = profile,
        .postgresql_revision =
            "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
        .provenance_bundle_sha256 = std::string(64, 'c'),
        .source_list_sha256 = std::string(64, '4'),
        .compile_database_sha256 = std::string(64, '5'),
        .producer_executable_sha256 = std::string(64, '6'),
        .compile_graph_sha256 = std::string(64, 'e'),
        .pcm_manifest_sha256 = std::string(64, '7'),
        .sql_manifest_sha256 = std::string(
            64,
            profile == compilation_profile::production
                ? '4'
                : '5'),
        .compiler_facts_sha256 = std::string(64, '9'),
        .clang_version = "fixture-clang-v1",
        .analyzer_version = "fixture-analyzer-v2",
        .generator_revision = "fixture-generator-v2",
    };
    ledger.parents = {
        fixture_parent(
            "bioseq.from_array(bigint)",
            "from_array",
            suffix + "::from_array",
            profile,
            fixture_site(
                suffix + ".site.bridged",
                "SPI_execute",
                site_classification::bridged,
                bridge_kind::invoke_pg_as_cpp,
                true,
                false)),
        fixture_parent(
            "bioseq.from_array(text)",
            "from_array",
            suffix + "::from_array",
            profile,
            fixture_site(
                suffix + ".site.specialized",
                "SPI_finish",
                site_classification::specialized_bridge,
                bridge_kind::sql_subtransaction,
                false,
                false)),
        fixture_parent(
            "public.mmsearch(bigint)",
            "mmsearch",
            suffix + "::mmsearch",
            profile,
            fixture_site(
                suffix + ".site.direct",
                "palloc",
                site_classification::lifetime_free_direct,
                bridge_kind::none,
                false,
                false)),
        fixture_parent(
            "public.mmsearch(text)",
            "mmsearch",
            suffix + "::mmsearch",
            profile,
            fixture_site(
                suffix + ".site.tail",
                "ReThrowError",
                site_classification::terminal_after_unwind,
                bridge_kind::generated_boundary_tail,
                false,
                true)),
    };
    ledger.parents[1].sites.push_back(
        fixture_site(
            suffix + ".site.sql_call_context",
            "SPI_execute",
            site_classification::specialized_bridge,
            bridge_kind::sql_call_context,
            true,
            false));
    ledger.parents[1].recoverable_site_count =
        ledger.parents[1].sites.size();
    ledger.symbol_groups = {
        {
            .public_symbol = "from_array",
            .finfo_symbol = "pg_finfo_from_array",
            .generated_wrapper = suffix + "::from_array",
            .sql_identities = {
                "bioseq.from_array(bigint)",
                "bioseq.from_array(text)",
            },
        },
        {
            .public_symbol = "mmsearch",
            .finfo_symbol = "pg_finfo_mmsearch",
            .generated_wrapper = suffix + "::mmsearch",
            .sql_identities = {
                "public.mmsearch(bigint)",
                "public.mmsearch(text)",
            },
        },
    };
    return ledger;
}

[[nodiscard]] inline runtime_matrix fixture_runtime_matrix(
    const safety_ledger& production,
    const safety_ledger& test_indexes,
    const runtime_policy& policy) {
    runtime_matrix matrix{
        .production_ledger_sha256 = std::string(64, '1'),
        .test_indexes_ledger_sha256 = std::string(64, '2'),
        .production_sql_manifest_sha256 =
            std::string(64, '4'),
        .test_indexes_sql_manifest_sha256 =
            std::string(64, '5'),
        .runtime_policy_sha256 = std::string(64, '3'),
        .production_dso_sha256 = std::string(64, 'a'),
        .test_indexes_dso_sha256 = std::string(64, 'b'),
        .runtime_producer_executable_sha256 =
            std::string(64, 'c'),
        .production_test_executable_sha256 =
            std::string(64, 'd'),
        .test_indexes_test_executable_sha256 =
            std::string(64, 'e'),
        .required_coverage = derive_required_runtime_coverage(
            production, test_indexes, policy),
    };
    for (const compilation_profile profile : {
             compilation_profile::production,
             compilation_profile::test_indexes,
         }) {
        const safety_ledger& active_ledger =
            profile == compilation_profile::production
            ? production
            : test_indexes;
        std::size_t case_index = 0;
        for (const safety_parent_row& parent : active_ledger.parents) {
            for (const error_site& site : parent.sites) {
                const std::string case_suffix =
                    std::string{profile_name(profile)}
                    + "." + std::to_string(case_index++);
                const bool lifetime_free =
                    site.classification
                    == site_classification::lifetime_free_direct;
                const std::vector<std::string> sentinels =
                    lifetime_free
                    ? std::vector<std::string>{}
                    : std::vector<std::string>{"caller", "fields"};
                matrix.cases.push_back({
                    .case_id =
                        "fixture.runtime." + case_suffix,
                    .profile = profile,
                    .ledger_sha256 =
                        profile
                                == compilation_profile::production
                        ? matrix.production_ledger_sha256
                        : matrix.test_indexes_ledger_sha256,
                    .sql_manifest_sha256 =
                        profile
                                == compilation_profile::production
                        ? matrix.production_sql_manifest_sha256
                        : matrix.test_indexes_sql_manifest_sha256,
                    .coverage = derive_runtime_site_coverage(
                        profile, site),
                    .expected_destructor_sentinels = sentinels,
                    .observed_destructor_sentinels = sentinels,
                    .expected_cleanup_events = {
                        "memory_context.restore"},
                    .observed_cleanup_events = {
                        "memory_context.restore"},
                    .expected_postgres_cleanup_events = {
                        "fmgr_abort_observed"},
                    .observed_postgres_cleanup_events = {
                        "fmgr_abort_observed"},
                    .expected_fmgr_abort_count = 1,
                    .observed_fmgr_abort_count = 1,
                    .expected_sqlstate = "XX000",
                    .observed_sqlstate = "XX000",
                    .expected_message = "fixture error",
                    .observed_message = "fixture error",
                    .expected_recovery_postcondition = "SPI usable",
                    .observed_recovery_postcondition = "SPI usable",
                    .exercised_terminal_severity = false,
                    .sql_identity = parent.sql_identity,
                    .site_id = site.site_id,
                    .invocation_key =
                        "fixture-invocation-" + case_suffix,
                    .fault_key =
                        "fixture-fault-" + case_suffix,
                    .active_dso_sha256 =
                        profile
                                == compilation_profile::production
                        ? matrix.production_dso_sha256
                        : matrix.test_indexes_dso_sha256,
                    .producer_executable_sha256 =
                        matrix.runtime_producer_executable_sha256,
                    .test_executable_sha256 =
                        profile
                                == compilation_profile::production
                        ? matrix.production_test_executable_sha256
                        : matrix.test_indexes_test_executable_sha256,
                    .immutable_test_log_path =
                        "fixture/" + case_suffix + ".log",
                    .immutable_test_log_sha256 =
                        std::string(64, 'f'),
                    .immutable_result_path =
                        "fixture/" + case_suffix + ".result.json",
                    .immutable_result_sha256 =
                        std::string(64, '0'),
                });
            }
        }
    }
    return matrix;
}

[[nodiscard]] inline dso_symbol_occurrence fixture_export(
    std::string name) {
    return {
        .name = std::move(name),
        .binding = "GLOBAL",
        .symbol_type = "FUNC",
        .source_tool = "fixture-nm",
        .defined = true,
        .dynamic_visible = true,
    };
}

[[nodiscard]] inline safety_ledger fixture_dso_ledger() {
    safety_ledger ledger{
        .profile = compilation_profile::production,
    };
    for (const std::string symbol : {
             "vbpchareq",
             "vbpcharne",
             "vbpchargt",
             "vbpcharlt",
             "vbpcharle",
             "vbpcharge",
             "seq_concat",
             "array_cat",
         }) {
        const std::string identity = "public." + symbol + "(internal)";
        ledger.parents.push_back({
            .sql_identity = identity,
            .public_symbol = symbol,
        });
        ledger.symbol_groups.push_back({
            .public_symbol = symbol,
            .finfo_symbol = "pg_finfo_" + symbol,
            .generated_wrapper = "fixture::" + symbol,
            .sql_identities = {identity},
        });
    }
    return ledger;
}

[[nodiscard]] inline dso_inventory fixture_dso_inventory(
    const safety_ledger& ledger,
    const std::vector<std::string>& external) {
    dso_inventory inventory;
    for (const symbol_group& group : ledger.symbol_groups) {
        inventory.symbols.push_back(
            fixture_export(group.public_symbol));
        inventory.symbols.push_back(
            fixture_export(group.finfo_symbol));
    }
    for (const std::string& symbol : external) {
        inventory.symbols.push_back(fixture_export(symbol));
        inventory.symbols.push_back(
            fixture_export("pg_finfo_" + symbol));
    }
    return inventory;
}

template<class Operation>
inline void require_failure(
    Operation&& operation,
    std::string_view expected) {
    try {
        std::invoke(std::forward<Operation>(operation));
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()}.contains(expected)) return;
        throw std::runtime_error(
            "safety self-test received unexpected diagnostic: "
            + std::string{error.what()});
    }
    throw std::runtime_error(
        "safety self-test expected failure containing: "
        + std::string{expected});
}

}  // namespace detail

inline void run_safety_contract_self_tests() {
    run_safety_analyzer_contract_self_tests();
    validate_doc0388_fmgr_migration_ledger();
    const std::vector<std::string> retired_symbols =
        doc0388_retired_c_symbols();
    if (retired_symbols.size() != 117) {
        throw std::runtime_error(
            "DOC0388 retired-symbol fixture cardinality changed");
    }
    if (std::ranges::any_of(
            retired_symbols,
            [](const std::string& symbol) {
                return symbol == "seq_concat";
            })) {
        throw std::runtime_error(
            "DOC0388 shared seq_concat target was classified as absent");
    }
    const std::size_t normalized_identity_count =
        std::ranges::count_if(
            doc0388_fmgr_migration_ledger,
            [](const fmgr_migration_entry& entry) {
                return entry.disposition
                        == fmgr_migration_disposition::retired_symbol
                    || entry.disposition
                        == fmgr_migration_disposition::shared_symbol_split;
            });
    if (normalized_identity_count != 118) {
        throw std::runtime_error(
            "DOC0388 symbol-normalization cardinality changed");
    }
    std::vector<std::string> target_identities;
    std::vector<std::string> target_symbols;
    target_identities.reserve(
        doc0388_fmgr_migration_ledger.size());
    target_symbols.reserve(
        doc0388_fmgr_migration_ledger.size());
    for (const fmgr_migration_entry& entry :
         doc0388_fmgr_migration_ledger) {
        target_identities.emplace_back(entry.sql_identity);
        if (std::find(
                target_symbols.begin(),
                target_symbols.end(),
                std::string{entry.target_symbol})
            == target_symbols.end()) {
            target_symbols.emplace_back(entry.target_symbol);
        }
    }
    validate_doc0388_fmgr_target(
        target_identities, target_symbols);

    const provenance_bundle provenance = detail::fixture_provenance();
    {
        provenance_bundle paired = provenance;
        paired.records.push_back(
            detail::fixture_pfree_provenance());
        validate_provenance_bundle(paired);
        provenance_bundle missing_definition = paired;
        missing_definition.records.back().definition.reset();
        detail::require_failure(
            [&missing_definition] {
                validate_provenance_bundle(
                    missing_definition);
            },
            "declaration and definition chain");
        provenance_bundle stale_definition = paired;
        stale_definition.records.back()
            .definition->fragment_sha256 = std::string(64, '0');
        detail::require_failure(
            [&stale_definition] {
                validate_provenance_bundle(stale_definition);
            },
            "declaration/definition provenance is stale");
    }
    {
        provenance_bundle bad = provenance;
        bad.records.front().user_code_callback_free = true;
        detail::require_failure(
            [&bad] {
                validate_provenance_bundle(bad);
            },
            "non-library provenance carries");
    }
    {
        provenance_bundle bad = provenance;
        bad.records.front().severities = {
            postgres_severity::fatal,
            postgres_severity::panic,
        };
        detail::require_failure(
            [&bad] {
                validate_provenance_bundle(bad);
            },
            "seed kind, severity");
    }
    {
        provenance_bundle bad = provenance;
        provenance_record conflicting =
            bad.records.front();
        conflicting.record_id = "pg17.conflicting_elog_header";
        conflicting.upstream_file_sha256 =
            std::string(64, '0');
        bad.records.push_back(std::move(conflicting));
        detail::require_failure(
            [&bad] {
                validate_provenance_bundle(bad);
            },
            "conflicting whole-file hashes");
    }
    {
        provenance_bundle bad = provenance;
        const auto external_record = [](
            std::string id,
            std::string source_file) {
            return provenance_record{
                .record_id = std::move(id),
                .kind =
                    provenance_kind::standard_library_declaration,
                .spelling = "std::fixture_leaf",
                .upstream_file = std::move(source_file),
                .upstream_span = detail::fixture_span(100, 200),
                .upstream_file_sha256 = std::string(64, 'e'),
                .fragment_sha256 = std::string(64, 'f'),
                .rationale =
                    "Reviewed callback-free fixture declaration.",
                .library_version = "clang:fixture",
                .target_usr = "c:@N@std@F@fixture_leaf#",
                .target_qualified_name = "std::fixture_leaf",
                .authority_id = "clang:fixture",
                .user_code_callback_free = true,
            };
        };
        bad.records.push_back(external_record(
            "libcxx.fixture_leaf.first",
            "/fixture/libcxx/include/fixture_leaf.h"));
        bad.records.push_back(external_record(
            "libcxx.fixture_leaf.second",
            "/fixture/libcxx/include/other_fixture_leaf.h"));
        detail::require_failure(
            [&bad] {
                validate_provenance_bundle(bad);
            },
            "multiple provenance records claim one target");
    }
    {
        provenance_bundle bad = provenance;
        bad.records.push_back({
            .record_id = "libcxx.relative_fixture",
            .kind =
                provenance_kind::standard_library_declaration,
            .spelling = "std::relative_fixture",
            .upstream_file = "libcxx/include/relative_fixture.h",
            .upstream_span = detail::fixture_span(100, 200),
            .upstream_file_sha256 = std::string(64, 'e'),
            .fragment_sha256 = std::string(64, 'f'),
            .rationale =
                "Adversarial relative standard-library record.",
            .library_version = "clang:fixture",
            .target_usr =
                "c:@N@std@F@relative_fixture#",
            .target_qualified_name =
                "std::relative_fixture",
            .authority_id = "clang:fixture",
            .user_code_callback_free = true,
        });
        detail::require_failure(
            [&bad] {
                validate_provenance_bundle(bad);
            },
            "path is not absolute");
    }
    safety_ledger production =
        detail::fixture_ledger(compilation_profile::production);
    safety_ledger test_indexes =
        detail::fixture_ledger(compilation_profile::test_indexes);
    const ledger_policy policy{
        .expected_parent_count = 4,
        .expected_symbol_count = 2,
    };
    validate_profile_pair(
        production,
        test_indexes,
        provenance,
        std::string(64, 'c'),
        policy);
    {
        safety_ledger bad = production;
        bad.parents.front().owner_source_file =
            "relative/fixture.cppm";
        detail::require_failure(
            [&bad, &provenance, &policy] {
                validate_safety_ledger(
                    bad,
                    provenance,
                    std::string(64, 'c'),
                    policy);
            },
            "parent identity, ownership, span, wrapper, or hash");
    }
    {
        provenance_bundle stale = provenance;
        stale.records.push_back({
            .record_id = "libcxx.unused_fixture",
            .kind =
                provenance_kind::standard_library_declaration,
            .spelling = "std::unused_fixture",
            .upstream_file =
                "/fixture/libcxx/include/unused_fixture.h",
            .upstream_span = detail::fixture_span(100, 200),
            .upstream_file_sha256 = std::string(64, 'e'),
            .fragment_sha256 = std::string(64, 'f'),
            .rationale =
                "Reviewed but deliberately unused fixture record.",
            .library_version = "clang:fixture",
            .target_usr = "c:@N@std@F@unused_fixture#",
            .target_qualified_name = "std::unused_fixture",
            .authority_id = "clang:fixture",
            .user_code_callback_free = true,
        });
        detail::require_failure(
            [&production, &stale, &policy] {
                validate_safety_ledger(
                    production,
                    stale,
                    std::string(64, 'c'),
                    policy);
            },
            "stale or unused");
    }
    const std::string rendered_production =
        render_safety_ledger_json(production);
    const std::string rendered_test_indexes =
        render_safety_ledger_json(test_indexes);
    if (render_safety_ledger_json(
            parse_safety_ledger_json(rendered_production))
            != rendered_production
        || render_safety_ledger_json(
               parse_safety_ledger_json(
                   rendered_test_indexes))
            != rendered_test_indexes) {
        throw std::runtime_error(
            "full safety-ledger parser changed canonical bytes");
    }
    {
        safety_ledger malformed_production = production;
        safety_ledger malformed_test_indexes = test_indexes;
        malformed_production.symbol_groups.front()
            .sql_identities.pop_back();
        malformed_test_indexes.symbol_groups.front()
            .sql_identities.pop_back();
        detail::require_failure(
            [&] {
                validate_profile_pair(
                    malformed_production,
                    malformed_test_indexes,
                    provenance,
                    std::string(64, 'c'),
                    policy);
            },
            "symbol groups do not cover exactly");
    }

    {
        safety_ledger bad = production;
        bad.parents.front().sites.front().resolution =
            resolution_status::unresolved_indirect;
        detail::require_failure(
            [&bad, &provenance, &policy] {
                validate_safety_ledger(
                    bad,
                    provenance,
                    std::string(64, 'c'),
                    policy);
            },
            "unresolved_indirect");
    }
    {
        safety_ledger bad = production;
        bad.parents.front().sites.front().possible_severities = {
            postgres_severity::fatal,
        };
        detail::require_failure(
            [&bad, &provenance, &policy] {
                validate_safety_ledger(
                    bad,
                    provenance,
                    std::string(64, 'c'),
                    policy);
            },
            "FATAL/PANIC");
    }
    {
        safety_ledger bad = production;
        crossing_type& crossing = bad.parents.front()
            .sites.front().bridge_callable_crossing_types.front();
        crossing.canonical_type = "std::string";
        crossing.trivially_destructible = false;
        detail::require_failure(
            [&bad, &provenance, &policy] {
                validate_safety_ledger(
                    bad,
                    provenance,
                    std::string(64, 'c'),
                    policy);
            },
            "nontrivially destructible");
    }
    {
        safety_ledger bad = production;
        bad.parents.front().sites.front()
            .bridge_evidence->primitive.record_id =
            "pg17.rethrow_declarations";
        detail::require_failure(
            [&bad, &provenance, &policy] {
                validate_safety_ledger(
                    bad,
                    provenance,
                    std::string(64, 'c'),
                    policy);
            },
            "bridge evidence does not match");
    }

    const runtime_policy runtime_policy_fixture{
        .explicitly_required = {
            {
                .profile = compilation_profile::production,
                .dimension = coverage_dimension::converter_family,
                .value = "fixture_converter",
            },
            {
                .profile = compilation_profile::test_indexes,
                .dimension = coverage_dimension::converter_family,
                .value = "fixture_converter",
            },
        },
    };
    runtime_matrix matrix = detail::fixture_runtime_matrix(
        production, test_indexes, runtime_policy_fixture);
    validate_runtime_matrix(
        matrix,
        production,
        test_indexes,
        runtime_policy_fixture,
        std::string(64, '1'),
        std::string(64, '2'),
        std::string(64, '4'),
        std::string(64, '5'),
        std::string(64, '3'),
        std::string(64, 'a'),
        std::string(64, 'b'),
        std::string(64, 'c'),
        std::string(64, 'd'),
        std::string(64, 'e'));
    {
        runtime_matrix backend_scoped = matrix;
        runtime_case& test_case = backend_scoped.cases.front();
        test_case.fault_key =
            "invoke_pg_as_cpp.vacuum_cleanup_restore";
        test_case.expected_postgres_cleanup_events.clear();
        test_case.observed_postgres_cleanup_events.clear();
        test_case.expected_fmgr_abort_count = 0;
        test_case.observed_fmgr_abort_count = 0;
        validate_runtime_matrix(
            backend_scoped,
            production,
            test_indexes,
            runtime_policy_fixture,
            std::string(64, '1'),
            std::string(64, '2'),
            std::string(64, '4'),
            std::string(64, '5'),
            std::string(64, '3'),
            std::string(64, 'a'),
            std::string(64, 'b'),
            std::string(64, 'c'),
            std::string(64, 'd'),
            std::string(64, 'e'));

        runtime_matrix fabricated_fmgr = backend_scoped;
        runtime_case& fabricated_case =
            fabricated_fmgr.cases.front();
        fabricated_case.expected_postgres_cleanup_events = {
            "fmgr_abort_observed"};
        fabricated_case.observed_postgres_cleanup_events = {
            "fmgr_abort_observed"};
        fabricated_case.expected_fmgr_abort_count = 1;
        fabricated_case.observed_fmgr_abort_count = 1;
        detail::require_failure(
            [&fabricated_fmgr,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    fabricated_fmgr,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "cleanup/FMGR ABORT evidence");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().observed_destructor_sentinels.pop_back();
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "destructor sentinels");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().coverage.push_back({
            .profile = compilation_profile::production,
            .dimension = coverage_dimension::classification,
            .value = "lifetime_free_direct",
        });
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "unsupported by its exact ledger site");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().observed_cleanup_events.clear();
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "cleanup/FMGR ABORT evidence");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().observed_postgres_cleanup_events.clear();
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "cleanup/FMGR ABORT evidence");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().observed_fmgr_abort_count = 0;
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "cleanup/FMGR ABORT evidence");
    }
    {
        runtime_matrix bad = matrix;
        for (runtime_case& test_case : bad.cases) {
            std::erase_if(
                test_case.coverage,
                [](const coverage_key& key) {
                    return key.profile
                            == compilation_profile::production
                        && key.dimension
                            == coverage_dimension::classification
                        && key.value == "bridged";
                });
        }
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "missing coverage keys");
    }
    {
        runtime_matrix bad = matrix;
        bad.production_ledger_sha256 = std::string(64, '4');
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "artifact hash is stale");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().sql_manifest_sha256 =
            std::string(64, '5');
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "artifact binding is invalid");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().exercised_terminal_severity = true;
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "must not exercise FATAL or PANIC");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().site_id = "fabricated-site";
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "identity/site/fault key");
    }
    {
        runtime_matrix bad = matrix;
        bad.cases.front().immutable_result_sha256 =
            "not-a-hash";
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "artifact binding is invalid");
    }
    {
        runtime_matrix bad = matrix;
        bad.production_dso_sha256 = std::string(64, 'f');
        detail::require_failure(
            [&bad,
             &production,
             &test_indexes,
             &runtime_policy_fixture] {
                validate_runtime_matrix(
                    bad,
                    production,
                    test_indexes,
                    runtime_policy_fixture,
                    std::string(64, '1'),
                    std::string(64, '2'),
                    std::string(64, '4'),
                    std::string(64, '5'),
                    std::string(64, '3'),
                    std::string(64, 'a'),
                    std::string(64, 'b'),
                    std::string(64, 'c'),
                    std::string(64, 'd'),
                    std::string(64, 'e'));
            },
            "DSO/producer/test executable");
    }

    safety_ledger dso_ledger = detail::fixture_dso_ledger();
    dso_ledger.symbol_groups.front().generated_wrapper.clear();
    dso_ledger.symbol_groups[6].generated_wrapper.clear();
    dso_contract dso_policy{
        .expected_main_identity_count = 8,
        .expected_main_symbol_count = 8,
        .expected_external_symbol_count = 1,
        .expected_retired_symbol_count = 1,
        .external_symbols = {
            "bioseq_external_handler",
        },
        .retired_symbols = {"retired_fixture"},
        .legacy_macro_symbols = {
            "seq_concat",
            "vbpchareq",
        },
        .require_complete_codegen = false,
    };
    dso_inventory inventory = detail::fixture_dso_inventory(
        dso_ledger, dso_policy.external_symbols);
    validate_dso_closure(dso_ledger, inventory, dso_policy);
    {
        dso_inventory bad = inventory;
        bad.symbols.pop_back();
        detail::require_failure(
            [&dso_ledger, &bad, &dso_policy] {
                validate_dso_closure(
                    dso_ledger, bad, dso_policy);
            },
            "268 bioseq symbols plus the bioseq_external handler");
    }
    {
        safety_ledger bad = dso_ledger;
        bad.symbol_groups.front().generated_wrapper =
            "fixture::vbpchareq";
        detail::require_failure(
            [&bad, &inventory, &dso_policy] {
                validate_dso_closure(
                    bad, inventory, dso_policy);
            },
            "generated and legacy fmgr ownership overlap");
    }
    {
        dso_contract bad = dso_policy;
        bad.legacy_macro_symbols.pop_back();
        detail::require_failure(
            [&dso_ledger, &inventory, &bad] {
                validate_dso_closure(
                    dso_ledger, inventory, bad);
            },
            "does not exactly cover");
    }
    {
        dso_contract bad = dso_policy;
        bad.require_complete_codegen = true;
        detail::require_failure(
            [&dso_ledger, &inventory, &bad] {
                validate_dso_closure(
                    dso_ledger, inventory, bad);
            },
            "still contains legacy fmgr owners");
    }
    {
        safety_ledger final_ledger = detail::fixture_dso_ledger();
        dso_contract final_policy = dso_policy;
        final_policy.legacy_macro_symbols.clear();
        final_policy.require_complete_codegen = true;
        validate_dso_closure(
            final_ledger, inventory, final_policy);
    }
}

}  // namespace bioseq::codegen::safety
