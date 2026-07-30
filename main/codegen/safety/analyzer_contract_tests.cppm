module;

#include <algorithm>
#include <cstddef>
#include <functional>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

export module bioseq.codegen.safety:analyzer_contract_tests;

import :analyzer;
import :json;
import :manifest;
import :serialization;
import bioseq.codegen.support;

export namespace bioseq::codegen::safety {

namespace analyzer_test_detail {

[[nodiscard]] inline source_range span(
    std::size_t line,
    std::size_t begin_offset,
    std::size_t end_offset) {
    return {
        .begin = {
            .byte_offset = begin_offset,
            .line = line,
            .column = 1,
        },
        .end = {
            .byte_offset = end_offset,
            .line = line,
            .column = end_offset - begin_offset + 1,
        },
    };
}

[[nodiscard]] inline located_source_range located(
    std::string file,
    source_range value) {
    return {
        .source_file = std::move(file),
        .span = value,
    };
}

inline void mark_complete(
    function_flow& function,
    std::size_t callsite_count) {
    function.template_instantiation_complete = true;
    function.macro_expansion_complete = true;
    function.cfg_complete = true;
    function.lifetime_analysis_complete = true;
    function.implicit_call_edges_complete = true;
    function.body_sha256 = std::string(64, '1');
    function.cfg_sha256 = std::string(64, '2');
    function.cfg_block_count = 1;
    function.cfg_element_count = callsite_count + 1;
    function.ast_callsite_count = callsite_count;
    function.cfg_implicit_callsite_count = 0;
    function.emitted_callsite_count = callsite_count;
    function.lifetime_object_ids.clear();
    for (const flow_call_site& call : function.calls) {
        for (const flow_object& object : call.live_objects) {
            function.lifetime_object_ids.push_back(
                object.stable_object_id);
        }
        for (const flow_object& object :
             call.callable_capture_objects) {
            function.lifetime_object_ids.push_back(
                object.stable_object_id);
        }
    }
    std::ranges::sort(function.lifetime_object_ids);
    function.lifetime_object_ids.erase(
        std::unique(
            function.lifetime_object_ids.begin(),
            function.lifetime_object_ids.end()),
        function.lifetime_object_ids.end());
    std::string material;
    for (const std::string& object_id :
         function.lifetime_object_ids) {
        material += std::to_string(object_id.size()) + ":"
            + object_id + "\n";
    }
    function.lifetime_inventory_sha256 =
        bioseq::codegen::sha256_hex(material);
}

[[nodiscard]] inline flow_object nontrivial_object(
    std::string id,
    std::string file,
    source_range declaration_span,
    crossing_origin origin =
        crossing_origin::temporary) {
    return {
        .stable_object_id = std::move(id),
        .identity_kind =
            object_identity_kind::synthetic_temporary,
        .canonical_type = "std::string",
        .origin = origin,
        .declaration = located(
            std::move(file), declaration_span),
        .trivially_destructible = false,
    };
}

[[nodiscard]] inline provenance_record provenance_seed(
    std::string id,
    provenance_kind kind,
    std::string file,
    std::size_t begin,
    std::size_t end,
    std::string file_hash,
    std::string fragment_hash,
    std::vector<postgres_severity> severities) {
    return {
        .record_id = std::move(id),
        .kind = kind,
        .spelling = "fixture",
        .upstream_file = std::move(file),
        .upstream_span = span(1, begin, end),
        .upstream_file_sha256 = std::move(file_hash),
        .fragment_sha256 = std::move(fragment_hash),
        .rationale = "Pinned analyzer contract fixture.",
        .severities = std::move(severities),
    };
}

[[nodiscard]] inline provenance_bundle provenance() {
    constexpr std::string_view elog_h =
        "e6b0864bf3fae574d5f6676ca3797dd7bccd70b69b5ebf1a0cdb86f635853e0d";
    constexpr std::string_view elog_c =
        "c0fa4ad0198a13bd121080688b81d1faf8a352fb0852b01675b090f50c4db55c";
    provenance_bundle result{
        .postgresql_tag = "REL_17_10",
        .postgresql_revision =
            "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
    };
    result.records = {
        provenance_seed(
            "pg17.severity_levels",
            provenance_kind::terminal_severity_seed,
            "src/include/utils/elog.h",
            593,
            2155,
            std::string{elog_h},
            "b1c2030376e5858d484b65928856171de3b9f62957f2d26566795150027b5f6d",
            {
                postgres_severity::recoverable_error,
                postgres_severity::fatal,
                postgres_severity::panic,
            }),
        provenance_seed(
            "pg17.ereport_macro",
            provenance_kind::recoverable_error_seed,
            "src/include/utils/elog.h",
            6037,
            6985,
            std::string{elog_h},
            "790c1855de5610cfc507aa3221efcb40b241e59fb659d00463e9e25366330743",
            {postgres_severity::recoverable_error}),
        provenance_seed(
            "pg17.elog_macro",
            provenance_kind::recoverable_error_seed,
            "src/include/utils/elog.h",
            9575,
            9819,
            std::string{elog_h},
            "288f9d80a16a31057b6004c07b060a9cb7565bd197d31d0b20e2727d6317d848",
            {postgres_severity::recoverable_error}),
        provenance_seed(
            "pg17.pg_try_rethrow_macros",
            provenance_kind::bridge_primitive,
            "src/include/utils/elog.h",
            15244,
            16531,
            std::string{elog_h},
            "d96fc354e6c3e8eb0f0b531e32d0b97de0e2865f656f1557152ead276f8cdb49",
            {}),
        provenance_seed(
            "pg17.rethrow_declarations",
            provenance_kind::bridge_primitive,
            "src/include/utils/elog.h",
            18571,
            18740,
            std::string{elog_h},
            "e32a0526deb30bfa66316389c51c4ce1e338e92b30e54a086fe0a92968fbf03a",
            {}),
        provenance_seed(
            "pg17.errstart",
            provenance_kind::recoverable_error_seed,
            "src/backend/utils/error/elog.c",
            11150,
            14888,
            std::string{elog_c},
            "a20f4d33c3d85bd4ffd35d9ff943e814e06d9c0825fa0ebce8696594858083af",
            {
                postgres_severity::recoverable_error,
                postgres_severity::fatal,
                postgres_severity::panic,
            }),
        provenance_seed(
            "pg17.errfinish",
            provenance_kind::recoverable_error_seed,
            "src/backend/utils/error/elog.c",
            15040,
            19122,
            std::string{elog_c},
            "f04234bff935be4007825804dccd11adf5fb78c3685e8c6f13aadefc1c7bcac5",
            {
                postgres_severity::recoverable_error,
                postgres_severity::fatal,
                postgres_severity::panic,
            }),
        provenance_seed(
            "pg17.rethrow_error",
            provenance_kind::recoverable_error_seed,
            "src/backend/utils/error/elog.c",
            57136,
            58661,
            std::string{elog_c},
            "13286a15f4aa4d8f6b063d7b0a9ffb67cc75306886278ddbdf2d94189c2cbff5",
            {postgres_severity::recoverable_error}),
        provenance_seed(
            "pg17.pg_re_throw",
            provenance_kind::recoverable_error_seed,
            "src/backend/utils/error/elog.c",
            58665,
            60663,
            std::string{elog_c},
            "c3c4b2cd89e2e9190ec7fd82b861f16b856cb7dda3b93ed5d9e8d8c42a659893",
            {
                postgres_severity::recoverable_error,
                postgres_severity::fatal,
            }),
    };
    result.records.push_back(provenance_seed(
        "pg17.get_typlenbyvalalign",
        provenance_kind::pg_no_longjmp_leaf,
        "src/backend/utils/cache/lsyscache.c",
        1200,
        1500,
        std::string(64, 'c'),
        std::string(64, 'd'),
        {}));
    result.records.back().spelling = "get_typlenbyvalalign";
    result.records.back().target_usr =
        "c:@F@get_typlenbyvalalign#";
    result.records.back().target_qualified_name =
        "get_typlenbyvalalign";
    result.records.back().authority_id =
        result.postgresql_revision;
    result.records.back().definition = provenance_definition{
        .upstream_file =
            "src/backend/utils/cache/lsyscache.c",
        .upstream_span = span(1, 1600, 1900),
        .upstream_file_sha256 = std::string(64, 'c'),
        .fragment_sha256 = std::string(64, 'e'),
    };
    result.records.back().rationale =
        "Reviewed PostgreSQL leaf cannot raise ERROR.";
    result.records.push_back(provenance_seed(
        "libcxx.is_constant_evaluated",
        provenance_kind::standard_library_declaration,
        "/fixture/libcxx/include/__type_traits/"
        "is_constant_evaluated.h",
        100,
        240,
        std::string(64, 'e'),
        std::string(64, 'f'),
        {}));
    result.records.back().spelling =
        "std::is_constant_evaluated";
    result.records.back().target_usr =
        "c:@N@std@F@is_constant_evaluated#";
    result.records.back().target_qualified_name =
        "std::is_constant_evaluated";
    result.records.back().authority_id =
        "clang:22.0.0-fixture";
    result.records.back().rationale =
        "Reviewed libc++ declaration for the compiler profile.";
    result.records.back().library_version =
        "clang:22.0.0-fixture";
    result.records.back().user_code_callback_free = true;
    return result;
}

[[nodiscard]] inline safety_manifest manifest() {
    safety_manifest result{
        .authority_mode = "annotated_cpp",
        .c_routine_identities = {"public.fixture()"},
        .c_link_symbols = {"fixture"},
        .c_finfo_symbols = {"pg_finfo_fixture"},
    };
    result.owners = {{
        .public_symbol = "fixture",
        .finfo_symbol = "pg_finfo_fixture",
        .generated_wrapper_module =
            "bioseq.sql.generated.production.fixture.entrypoints",
        .sql_identities = {"public.fixture()"},
        .implementation_declarations = {
            "fixture_owner::implementation"},
        .overloads = {{
            .sql_identity = "public.fixture()",
            .cpp_declaration = "fixture_owner::implementation",
            .source_path = "fixture.cppm",
            .source_line = 10,
            .argument_converter_keys = {
                "argument.int32.by_value"},
            .result_converter_key = "result.int32.by_value",
        }},
    }};
    return result;
}

[[nodiscard]] inline compiler_analysis_facts facts() {
    function_flow wrapper{
        .stable_usr = "c:@F@fixture#",
        .qualified_name = "fixture",
        .source_file =
            "/fixture/generated/fixture.entrypoints.cppm",
        .module_name =
            "bioseq.sql.generated.production.fixture.entrypoints",
        .declaration_span = span(1, 0, 10),
        .body_span = span(1, 11, 30),
        .body_available = true,
    };
    wrapper.calls = {{
        .stable_site_id = "wrapper-to-owner",
        .caller_usr = wrapper.stable_usr,
        .spelling = located(
            wrapper.source_file, span(2, 31, 40)),
        .expansion = located(
            wrapper.source_file, span(2, 31, 40)),
        .resolution = resolution_status::direct,
        .target_set_complete = true,
        .targets = {{
            .target_usr =
                "c:@N@fixture_owner@F@implementation#",
            .target_qualified_name =
                "fixture_owner::implementation",
            .domain = callee_domain::project,
        }},
    }};
    mark_complete(wrapper, wrapper.calls.size());
    function_flow implementation{
        .stable_usr = "c:@N@fixture_owner@F@implementation#",
        .qualified_name = "fixture_owner::implementation",
        .source_file = "/fixture/fixture.cppm",
        .module_name = "fixture.owner",
        .declaration_span = span(10, 100, 130),
        .body_span = span(10, 131, 320),
        .body_available = true,
    };
    implementation.calls = {{
        .stable_site_id = "ereport-site",
        .caller_usr = implementation.stable_usr,
        .spelling = located(
            "postgres/src/include/utils/elog.h",
            span(141, 6037, 6060)),
        .expansion = located(
            implementation.source_file, span(12, 150, 190)),
        .macro_chain = {{
            .name = "ereport",
            .invocation = located(
                implementation.source_file,
                span(12, 150, 190)),
            .definition = located(
                "postgres/src/include/utils/elog.h",
                span(141, 6037, 6985)),
            .definition_sha256 = std::string(64, '3'),
        }},
        .resolution = resolution_status::direct,
        .target_set_complete = true,
        .targets = {{
            .target_usr = "c:@F@errstart#I#",
            .target_qualified_name = "errstart",
            .domain = callee_domain::postgresql_server,
        }},
        .constant_integer_arguments = {{
            .ordinal = 0,
            .value = 20,
        }},
        .live_objects = {{
            .stable_object_id = "c:fixture.cppm@120@x",
            .declaration_usr = "c:fixture.cppm@120@x",
            .identity_kind =
                object_identity_kind::declaration_usr,
            .canonical_type = "int",
            .origin = crossing_origin::automatic_object,
            .declaration = located(
                implementation.source_file,
                span(11, 120, 125)),
            .trivially_destructible = true,
        }},
    }, {
        .stable_site_id = "stdlib-site",
        .caller_usr = implementation.stable_usr,
        .spelling = located(
            implementation.source_file, span(13, 200, 220)),
        .expansion = located(
            implementation.source_file, span(13, 200, 220)),
        .resolution = resolution_status::direct,
        .target_set_complete = true,
        .targets = {{
            .target_usr =
                "c:@N@std@F@is_constant_evaluated#",
            .target_qualified_name =
                "std::is_constant_evaluated",
            .domain = callee_domain::standard_library,
            .external_mode =
                external_call_mode::audited_no_user_code,
            .external_provenance =
                external_declaration_provenance{
                    .record_id =
                        "libcxx.is_constant_evaluated",
                    .source_file =
                        "/fixture/libcxx/include/__type_traits/"
                        "is_constant_evaluated.h",
                    .declaration_span = span(1, 100, 240),
                    .declaration_sha256 =
                        std::string(64, 'f'),
                    .library_version =
                        "clang:22.0.0-fixture",
                    .user_code_callback_free = true,
                },
        }},
    }, {
        .stable_site_id = "allowlisted-pg-site",
        .caller_usr = implementation.stable_usr,
        .spelling = located(
            implementation.source_file, span(14, 230, 250)),
        .expansion = located(
            implementation.source_file, span(14, 230, 250)),
        .resolution = resolution_status::direct,
        .target_set_complete = true,
        .targets = {{
            .target_usr = "c:@F@get_typlenbyvalalign#",
            .target_qualified_name = "get_typlenbyvalalign",
            .domain = callee_domain::postgresql_server,
            .pg_no_longjmp_allowlist_evidence =
                provenance_reference{
                    .record_id =
                        "pg17.get_typlenbyvalalign",
                    .postgresql_revision =
                        "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
                    .upstream_file =
                        "src/backend/utils/cache/lsyscache.c",
                    .upstream_span = span(1, 1200, 1500),
                    .fragment_sha256 =
                        std::string(64, 'd'),
                    .rationale =
                        "Reviewed PostgreSQL leaf cannot raise "
                        "ERROR.",
                },
        }},
    }};
    mark_complete(implementation, implementation.calls.size());
    return {
        .profile = compilation_profile::production,
        .compiler_profile = "production",
        .clang_version = "22.0.0-fixture",
        .source_list_sha256 = std::string(64, '7'),
        .compile_database_sha256 = std::string(64, '8'),
        .producer_executable_sha256 = std::string(64, '9'),
        .compile_graph_sha256 = std::string(64, 'a'),
        .pcm_manifest_sha256 = std::string(64, 'b'),
        .sql_manifest_sha256 = std::string(64, 'c'),
        .root_usrs = {wrapper.stable_usr},
        .functions = {
            std::move(wrapper),
            std::move(implementation),
        },
        .runtime_probes = {},
    };
}

[[nodiscard]] inline ledger_policy policy() {
    return {
        .expected_parent_count = 1,
        .expected_symbol_count = 1,
        .shared_symbols = {},
    };
}

inline void mark_invoke_bridge(flow_call_site& call) {
    call.bridge_transition = bridge_kind::invoke_pg_as_cpp;
    call.analyzer_entered_bridge_callable = true;
    call.bridge_primitive_evidence =
        bridge_transition_evidence{
            .primitive = {
                .record_id =
                    "pg17.pg_try_rethrow_macros",
                .postgresql_revision =
                    "25c49f3a4a742ba283f5cc43cc7f1d361552e917",
                .upstream_file =
                    "src/include/utils/elog.h",
                .upstream_span = span(1, 15244, 16531),
                .fragment_sha256 =
                    "d96fc354e6c3e8eb0f0b531e32d0b97de0e2865f656f1557152ead276f8cdb49",
                .rationale =
                    "Pinned analyzer contract fixture.",
            },
            .target_usr = call.targets.front().target_usr,
            .target_qualified_name =
                call.targets.front().target_qualified_name,
            .transition_callsite = call.expansion,
            .implementation_usr =
                "c:@N@bioseq@N@pg_utils@FT@>1#T"
                "@invoke_pg_as_cpp#",
            .implementation_qualified_name =
                "bioseq::pg_utils::invoke_pg_as_cpp",
            .implementation_module =
                "bioseq.pg_support:error_boundary",
            .implementation_definition = located(
                "/fixture/main/common/pg_support/"
                "error_boundary.cppm",
                span(1, 100, 300)),
            .implementation_body_sha256 =
                std::string(64, 'e'),
            .implementation_contract_id =
                "bioseq.pg_utils.error_bridge/error_capture/v1",
            .implementation_contract_sha256 =
                "cbf583abe710257bf2a1999718c076fa"
                "2d70e203ae424bbba083eee0b4fcc83c",
            .protected_region_id =
                "bioseq.pg_utils.error_bridge/invoke_pg_as_cpp",
        };
}

template<class Operation>
inline void require_failure(
    Operation&& operation,
    std::string_view expected,
    const std::source_location call_site =
        std::source_location::current()) {
    try {
        std::invoke(std::forward<Operation>(operation));
    } catch (const std::runtime_error& error) {
        if (std::string_view{error.what()}.contains(expected)) return;
        throw std::runtime_error(
            "analyzer contract at line "
            + std::to_string(call_site.line())
            + " received unexpected diagnostic: "
            + std::string{error.what()});
    }
    throw std::runtime_error(
        "analyzer contract at line "
        + std::to_string(call_site.line())
        + " expected failure containing: "
        + std::string{expected});
}

}  // namespace analyzer_test_detail

inline void run_safety_analyzer_contract_self_tests() {
    const auto require_converter_families = [](
            std::vector<std::string> arguments,
            std::string result,
            std::vector<std::string> expected,
            std::string_view label) {
        const safety_manifest_overload overload{
            .argument_converter_keys = std::move(arguments),
            .result_converter_key = std::move(result),
        };
        if (analyzer_detail::converter_families(overload)
            != expected) {
            throw std::runtime_error(
                "converter-family contract changed for "
                + std::string{label});
        }
    };
    require_converter_families(
        {
            "argument.int64.by_value.nullable",
            "argument.vector_null_mask[any].nullable",
        },
        "result.int64.by_value.nullable",
        {"polymorphic", "scalar", "vector"},
        "polymorphic vector aggregate");
    require_converter_families(
        {
            "argument.aggregate_state.int64_array.nullable",
            "argument.vector_column[smallint,int16_t].nullable",
        },
        "result.aggregate_state.int128.nullable",
        {"aggregate_state", "vector"},
        "vector aggregate state");
    require_converter_families(
        {},
        "result.returns_table[fixture]"
        "(nthash_vector_batches:bigint)",
        {"set_returning_function"},
        "set-returning payload");
    require_converter_families(
        {"argument.vector_null_mask[anyelement].nullable"},
        "result.company.nonpolymorphic",
        {"scalar", "vector"},
        "converter token false positives");
    require_converter_families(
        {"argument.bioseq.seq.pointer"},
        "result.bioseq.letter.by_value",
        {"base_type"},
        "Bioseq base types");

    const auto require_cleanup_phases = [](
            bridge_kind bridge,
            std::vector<const function_flow*> path,
            std::vector<std::string> expected,
            std::string_view label) {
        analyzer_detail::reachable_site site{};
        site.active_bridge = bridge;
        site.path = std::move(path);
        if (analyzer_detail::cleanup_phases(site) != expected) {
            throw std::runtime_error(
                "cleanup-phase contract changed for "
                + std::string{label});
        }
    };
    require_cleanup_phases(
        bridge_kind::invoke_pg_as_cpp,
        {},
        {"memory_context_restore", "resource_owner_restore"},
        "invoke_pg_as_cpp");
    require_cleanup_phases(
        bridge_kind::sql_call_context,
        {},
        {"memory_context_restore", "resource_owner_restore"},
        "sql_call_context");
    require_cleanup_phases(
        bridge_kind::sql_subtransaction,
        {},
        {
            "subtransaction_abort",
            "subtransaction_begin",
            "subtransaction_commit",
        },
        "sql_subtransaction");
    require_cleanup_phases(
        bridge_kind::generated_boundary_tail,
        {},
        {},
        "generated boundary tail");

    function_flow scoped_connect{};
    scoped_connect.qualified_name =
        "bioseq::pg_utils::scoped_spi_session::connect";
    scoped_connect.module_name =
        "bioseq.pg_sql_boundary:call_context";
    function_flow scoped_finish = scoped_connect;
    scoped_finish.qualified_name =
        "bioseq::pg_utils::scoped_spi_session::finish";
    require_cleanup_phases(
        bridge_kind::none,
        {&scoped_connect},
        {"spi_connect", "spi_finish"},
        "scoped SPI connect");
    require_cleanup_phases(
        bridge_kind::none,
        {&scoped_finish},
        {"spi_connect", "spi_finish"},
        "scoped SPI finish");
    require_cleanup_phases(
        bridge_kind::invoke_pg_as_cpp,
        {&scoped_connect},
        {
            "memory_context_restore",
            "resource_owner_restore",
            "spi_connect",
            "spi_finish",
        },
        "scoped SPI under invoke bridge");
    function_flow connect_extra = scoped_connect;
    connect_extra.qualified_name += "_extra";
    function_flow wrong_module = scoped_connect;
    wrong_module.module_name = "fixture.context";
    function_flow spi_like{};
    spi_like.qualified_name = "fixture::SPI_connect";
    spi_like.module_name =
        "bioseq.pg_sql_boundary:call_context";
    require_cleanup_phases(
        bridge_kind::none,
        {&connect_extra, &wrong_module, &spi_like},
        {},
        "SPI-like false positives");
    function_flow unrelated_path{};
    unrelated_path.qualified_name =
        "fixture::renamed_lowercase_helper";
    unrelated_path.module_name = "fixture.helpers";
    require_cleanup_phases(
        bridge_kind::invoke_pg_as_cpp,
        {&unrelated_path},
        {"memory_context_restore", "resource_owner_restore"},
        "bridge cleanup independent of helper spelling");

    const provenance_bundle provenance =
        analyzer_test_detail::provenance();
    const ledger_policy policy = analyzer_test_detail::policy();
    const safety_manifest manifest =
        analyzer_test_detail::manifest();
    const compiler_analysis_facts facts =
        analyzer_test_detail::facts();
    const safety_ledger ledger =
        analyze_postgres_error_safety(
            manifest,
            facts,
            provenance,
            std::string(64, 'b'),
            std::string(64, 'd'),
            "fixture-generator",
            policy);
    if (ledger.parents.size() != 1
        || ledger.parents.front().sites.size() != 1
        || ledger.parents.front().audited_stop_count != 2
        || ledger.parents.front().audited_stops.size() != 2
        || ledger.parents.front().sites.front().classification
            != site_classification::lifetime_free_direct
        || ledger.parents.front().sites.front().converter_families
            != std::vector<std::string>{"scalar"}
        || !ledger.parents.front().sites.front()
                .cleanup_phases.empty()
        || !std::holds_alternative<json_value::object>(
            parse_json(render_safety_ledger_json(ledger)).value)) {
        throw std::runtime_error(
            "positive analyzer contract changed its ledger closure");
    }
    {
        compiler_analysis_facts callback_facts = facts;
        const function_flow& owner =
            callback_facts.functions.front();
        const function_flow& target =
            callback_facts.functions.back();
        const located_source_range registration =
            analyzer_test_detail::located(
                owner.source_file,
                analyzer_test_detail::span(3, 45, 55));
        const std::string stable_material =
            std::string{"table_am\nrelation_vacuum\n"}
            + owner.stable_usr + "\n" + target.stable_usr + "\n"
            + registration.source_file + "\n"
            + std::to_string(
                registration.span.begin.byte_offset)
            + "\n"
            + std::to_string(
                registration.span.end.byte_offset);
        callback_facts.callback_roots.push_back({
            .stable_callback_id =
                "pg-callback:"
                + sha256_hex(stable_material).substr(0, 32),
            .family = "table_am",
            .slot = "relation_vacuum",
            .owner_usr = owner.stable_usr,
            .owner_qualified_name = owner.qualified_name,
            .registration = registration,
            .target_usr = target.stable_usr,
            .target_qualified_name = target.qualified_name,
            .target_definition = analyzer_test_detail::located(
                target.source_file,
                target.declaration_span),
        });
        const safety_ledger callback_ledger =
            analyze_postgres_error_safety(
                manifest,
                callback_facts,
                provenance,
                std::string(64, 'b'),
                std::string(64, 'd'),
                "fixture-generator",
                policy);
        if (callback_ledger.callback_root_count != 1
            || callback_ledger.callback_closure_sha256.empty()
            || parse_safety_ledger_json(
                   render_safety_ledger_json(callback_ledger))
                   .callback_closure_sha256
                != callback_ledger.callback_closure_sha256) {
            throw std::runtime_error(
                "callback-root closure was not bound into the "
                "safety ledger");
        }
        callback_facts.callback_roots.front()
            .stable_callback_id =
                "pg-callback:"
                + std::string(32, '0');
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        callback_facts,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "callback stable identity is stale");
    }
    if (runtime_relation_name(
            runtime_relation::natural_at_site)
            != "natural_at_site"
        || runtime_relation_name(
               runtime_relation::injected_routes_to_site)
            != "injected_routes_to_site"
        || runtime_relation_name(
               runtime_relation::injected_after_site_success)
            != "injected_after_site_success") {
        throw std::runtime_error(
            "runtime-probe relation wire names changed");
    }
    compiler_analysis_facts serialization_facts = facts;
    serialization_facts.runtime_probes = {
        {
            .fault_key = "fixture.routes",
            .relation =
                runtime_relation::injected_routes_to_site,
            .site_call_stable_id = "ereport-site",
            .visit_call_stable_id = "stdlib-site",
            .trigger_call_stable_id =
                "allowlisted-pg-site",
        },
        {
            .fault_key = "fixture.natural",
            .relation = runtime_relation::natural_at_site,
            .site_call_stable_id = "ereport-site",
            .visit_call_stable_id = "stdlib-site",
            .trigger_call_stable_id = std::nullopt,
        },
        {
            .fault_key = "fixture.after",
            .relation =
                runtime_relation::injected_after_site_success,
            .site_call_stable_id = "allowlisted-pg-site",
            .visit_call_stable_id = "stdlib-site",
            .trigger_call_stable_id = "ereport-site",
        },
    };
    const std::string rendered_facts =
        render_compiler_analysis_facts_json(
            serialization_facts);
    const compiler_analysis_facts parsed_facts =
        parse_compiler_analysis_facts_json(rendered_facts);
    compiler_analysis_facts parsed_analysis_facts =
        parsed_facts;
    parsed_analysis_facts.runtime_probes.clear();
    compiler_analysis_facts reordered_facts =
        serialization_facts;
    std::ranges::reverse(reordered_facts.runtime_probes);
    if (render_compiler_analysis_facts_json(parsed_facts)
            != rendered_facts
        || render_compiler_analysis_facts_json(
               reordered_facts)
            != rendered_facts
        || parsed_facts.runtime_probes.size() != 3
        || analyze_postgres_error_safety(
               manifest,
               parsed_analysis_facts,
               provenance,
               std::string(64, 'b'),
               std::string(64, 'd'),
               "fixture-generator",
               policy)
                .parents.front()
                .sites.size()
            != 1) {
        throw std::runtime_error(
            "schema-v2 compiler facts did not round-trip");
    }
    const json_value rendered_facts_document =
        parse_json(rendered_facts);
    const json_value::object& rendered_facts_root =
        json_object(
            rendered_facts_document,
            "rendered compiler analysis facts");
    const json_value::array& rendered_probes = json_array(
        json_field(
            rendered_facts_root,
            "runtime_probes",
            "rendered compiler analysis facts"),
        "rendered compiler analysis facts.runtime_probes");
    if (rendered_facts_root.size() != 14
        || rendered_probes.size() != 3) {
        throw std::runtime_error(
            "compiler-facts runtime-probe field set changed");
    }
    std::size_t natural_probe_count{};
    std::size_t injected_probe_count{};
    for (std::size_t index = 0;
         index < rendered_probes.size();
         ++index) {
        const std::string context =
            "rendered compiler analysis facts.runtime_probes["
            + std::to_string(index) + "]";
        const json_value::object& probe =
            json_object(rendered_probes[index], context);
        if (probe.size() != 5) {
            throw std::runtime_error(
                "runtime-probe fact field set changed");
        }
        static_cast<void>(
            json_string(
                json_field(probe, "fault_key", context),
                context + ".fault_key"));
        static_cast<void>(
            json_string(
                json_field(
                    probe,
                    "site_call_stable_id",
                    context),
                context + ".site_call_stable_id"));
        static_cast<void>(
            json_string(
                json_field(
                    probe,
                    "visit_call_stable_id",
                    context),
                context + ".visit_call_stable_id"));
        const std::string_view relation = json_string(
            json_field(probe, "relation", context),
            context + ".relation");
        const json_value& trigger = json_field(
            probe, "trigger_call_stable_id", context);
        if (relation == "natural_at_site") {
            ++natural_probe_count;
            if (!std::holds_alternative<std::nullptr_t>(
                    trigger.value)) {
                throw std::runtime_error(
                    "natural runtime probe did not render null "
                    "trigger");
            }
        } else {
            ++injected_probe_count;
            static_cast<void>(json_string(
                trigger,
                context + ".trigger_call_stable_id"));
        }
    }
    if (natural_probe_count != 1
        || injected_probe_count != 2
        || json_string(
               json_field(
                   json_object(
                       rendered_probes.front(),
                       "first runtime probe"),
                   "fault_key",
                   "first runtime probe"),
               "first runtime probe.fault_key")
            != "fixture.after") {
        throw std::runtime_error(
            "runtime probes did not render in canonical order");
    }
    {
        compiler_analysis_facts bad = serialization_facts;
        bad.schema_version = 3;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    render_compiler_analysis_facts_json(bad));
            },
            "require schema version 2");
    }
    {
        compiler_analysis_facts bad = serialization_facts;
        bad.runtime_probes.front().relation =
            runtime_relation::natural_at_site;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    render_compiler_analysis_facts_json(bad));
            },
            "natural_at_site relation requires a null");
    }
    {
        json_value bad = rendered_facts_document;
        std::get<json_value::object>(bad.value)
            .erase("runtime_probes");
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    parse_compiler_analysis_facts_json(
                        render_json(bad)));
            },
            "missing JSON field 'runtime_probes'");
    }
    {
        json_value bad = rendered_facts_document;
        std::get<json_value::object>(bad.value).emplace(
            "unexpected", json_value{true});
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    parse_compiler_analysis_facts_json(
                        render_json(bad)));
            },
            "unknown JSON field 'unexpected'");
    }
    {
        json_value bad = rendered_facts_document;
        json_value::object& root =
            std::get<json_value::object>(bad.value);
        json_value::array& probes =
            std::get<json_value::array>(
                root.at("runtime_probes").value);
        std::get<json_value::object>(
            probes.front().value)
            .emplace("unexpected", json_value{true});
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    parse_compiler_analysis_facts_json(
                        render_json(bad)));
            },
            "unknown JSON field 'unexpected'");
    }
    {
        json_value bad = rendered_facts_document;
        json_value::object& root =
            std::get<json_value::object>(bad.value);
        root["schema_version"] =
            json_value{std::size_t{3}};
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    parse_compiler_analysis_facts_json(
                        render_json(bad)));
            },
            "require schema version 2");
    }
    {
        json_value bad = rendered_facts_document;
        json_value::object& root =
            std::get<json_value::object>(bad.value);
        json_value::array& probes =
            std::get<json_value::array>(
                root.at("runtime_probes").value);
        std::get<json_value::object>(
            probes.front().value)["relation"] =
                json_value{"unknown"};
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    parse_compiler_analysis_facts_json(
                        render_json(bad)));
            },
            "unknown runtime-probe relation");
    }
    {
        json_value bad = rendered_facts_document;
        json_value::object& root =
            std::get<json_value::object>(bad.value);
        json_value::array& probes =
            std::get<json_value::array>(
                root.at("runtime_probes").value);
        json_value::object& probe =
            std::get<json_value::object>(
                probes.front().value);
        probe["relation"] =
            json_value{"natural_at_site"};
        probe["trigger_call_stable_id"] =
            json_value{"trigger-site"};
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    parse_compiler_analysis_facts_json(
                        render_json(bad)));
            },
            "natural_at_site relation requires a null");
    }
    {
        json_value bad = rendered_facts_document;
        json_value::object& root =
            std::get<json_value::object>(bad.value);
        json_value::array& probes =
            std::get<json_value::array>(
                root.at("runtime_probes").value);
        json_value::object& probe =
            std::get<json_value::object>(
                probes.front().value);
        probe["relation"] =
            json_value{"injected_routes_to_site"};
        probe["trigger_call_stable_id"] =
            json_value{nullptr};
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    parse_compiler_analysis_facts_json(
                        render_json(bad)));
            },
            "injected relation requires a non-null");
    }
    {
        json_value bad = rendered_facts_document;
        json_value::object& root =
            std::get<json_value::object>(bad.value);
        json_value::array& probes =
            std::get<json_value::array>(
                root.at("runtime_probes").value);
        json_value::object& probe =
            std::get<json_value::object>(
                probes.front().value);
        probe["site_call_stable_id"] = json_value{""};
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    parse_compiler_analysis_facts_json(
                        render_json(bad)));
            },
            "site_call_stable_id cannot be empty");
    }
    {
        safety_ledger bad = ledger;
        bad.parents.front().audited_stops.front()
            .evidence.fragment_sha256 = std::string(64, '0');
        analyzer_test_detail::require_failure(
            [&] {
                validate_safety_ledger(
                    bad,
                    provenance,
                    std::string(64, 'b'),
                    policy);
            },
            "audited stop does not exactly match");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.front().module_name.clear();
        safety_manifest bad_manifest = manifest;
        bad_manifest.owners.front()
            .generated_wrapper_module.clear();
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        bad_manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "generated wrapper module differs");
    }

    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls.front()
            .constant_integer_arguments.front().value = 21;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "FATAL/PANIC");
    }
    {
        compiler_analysis_facts bad = facts;
        flow_object& crossing =
            bad.functions.back().calls.front().live_objects.front();
        crossing.canonical_type = "std::string";
        crossing.trivially_destructible = false;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "not lifetime-free");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.front().calls.front().live_objects.push_back(
            analyzer_test_detail::nontrivial_object(
                "caller-temporary",
                bad.functions.front().source_file,
                analyzer_test_detail::span(2, 41, 48)));
        analyzer_test_detail::mark_complete(
            bad.functions.front(),
            bad.functions.front().calls.size());
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "not lifetime-free");
    }
    {
        compiler_analysis_facts bridged_facts = facts;
        analyzer_test_detail::mark_invoke_bridge(
            bridged_facts.functions.front().calls.front());
        const safety_ledger bridged =
            analyze_postgres_error_safety(
                manifest,
                bridged_facts,
                provenance,
                std::string(64, 'b'),
                std::string(64, 'd'),
                "fixture-generator",
                policy);
        if (bridged.parents.front().sites.front().classification
                != site_classification::bridged
            || !bridged.parents.front().sites.front()
                    .bridge_evidence
            || bridged.parents.front().sites.front()
                    .cleanup_phases
                != std::vector<std::string>{
                    "memory_context_restore",
                    "resource_owner_restore",
                }) {
            throw std::runtime_error(
                "valid pinned bridge evidence was not preserved "
                "in the ledger");
        }
    }
    {
        compiler_analysis_facts bad = facts;
        analyzer_test_detail::mark_invoke_bridge(
            bad.functions.front().calls.front());
        bad.functions.front().calls.front()
            .bridge_primitive_evidence
            ->implementation_contract_sha256 =
                std::string(64, '0');
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "bridge transition is not joined");
    }
    {
        compiler_analysis_facts bad = facts;
        flow_call_site bridged =
            bad.functions.front().calls.front();
        bridged.stable_site_id = "bridged-wrapper-to-owner";
        analyzer_test_detail::mark_invoke_bridge(bridged);
        flow_call_site direct = bridged;
        direct.stable_site_id = "direct-wrapper-to-owner";
        direct.bridge_transition = bridge_kind::none;
        direct.analyzer_entered_bridge_callable = false;
        direct.bridge_primitive_evidence.reset();
        direct.live_objects.push_back(
            analyzer_test_detail::nontrivial_object(
                "direct-path-temporary",
                bad.functions.front().source_file,
                analyzer_test_detail::span(2, 49, 56)));
        bad.functions.front().calls = {
            std::move(bridged), std::move(direct)};
        analyzer_test_detail::mark_complete(
            bad.functions.front(),
            bad.functions.front().calls.size());
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "unsafe direct or bridged path");
    }
    {
        compiler_analysis_facts bad = facts;
        flow_call_site& transition =
            bad.functions.front().calls.front();
        transition.bridge_transition =
            bridge_kind::invoke_pg_as_cpp;
        transition.analyzer_entered_bridge_callable = true;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "bridge/tail transition flags are inconsistent");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls.front()
            .live_objects.front().stable_object_id.clear();
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "lacks stable identity");
    }
    {
        compiler_analysis_facts bad = facts;
        flow_object conflicting =
            bad.functions.back().calls.front()
                .live_objects.front();
        conflicting.canonical_type = "long";
        bad.functions.back().calls[1].live_objects.push_back(
            std::move(conflicting));
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "conflicting lifetime facts");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls.front()
            .spelling.source_file.clear();
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "spelling/expansion ranges");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls.front()
            .macro_chain.front().definition_sha256 = "not-a-hash";
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "hashed provenance");
    }
    {
        compiler_analysis_facts bad = facts;
        flow_call_site& call =
            bad.functions.front().calls.front();
        call.resolution = resolution_status::unresolved_indirect;
        call.target_set_complete = false;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "closure is incomplete");
    }
    {
        compiler_analysis_facts bad = facts;
        flow_call_site& call =
            bad.functions.front().calls.front();
        call.resolution = resolution_status::resolved_indirect;
        call.target_set_complete = false;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "complete target set");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls[1]
            .targets.front()
            .external_provenance->record_id.clear();
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "lacks versioned hash provenance");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls[1]
            .targets.front()
            .external_provenance->declaration_sha256 =
            std::string(64, '0');
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "provenance is stale");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls.front()
            .targets.front().external_provenance =
            bad.functions.back().calls[1]
                .targets.front().external_provenance;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "inapplicable external contract or provenance");
    }
    {
        compiler_analysis_facts bad = facts;
        provenance_bundle callback_provenance = provenance;
        flow_call_target& external =
            bad.functions.back().calls[1].targets.front();
        external.external_provenance
            ->user_code_callback_free = false;
        auto record = std::ranges::find(
            callback_provenance.records,
            "libcxx.is_constant_evaluated",
            &provenance_record::record_id);
        record->user_code_callback_free = false;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        callback_provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "without callback-free provenance");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls[1]
            .targets.front().external_mode =
            external_call_mode::analyze_body;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "closure is incomplete");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls[2]
            .targets.front()
            .pg_no_longjmp_allowlist_evidence
            ->postgresql_revision = std::string(40, '0');
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "no-longjmp evidence is stale");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.back().calls[2].targets.front().domain =
            callee_domain::project;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "inapplicable external contract or provenance");
    }
    {
        compiler_analysis_facts default_pg = facts;
        flow_call_site& call =
            default_pg.functions.back().calls.front();
        call.targets.front().target_usr = "c:@F@palloc#l#";
        call.targets.front().target_qualified_name = "palloc";
        call.macro_chain.clear();
        call.constant_integer_arguments.clear();
        const safety_ledger default_pg_ledger =
            analyze_postgres_error_safety(
                manifest,
                default_pg,
                provenance,
                std::string(64, 'b'),
                std::string(64, 'd'),
                "fixture-generator",
                policy);
        if (default_pg_ledger.parents.front().sites.size() != 1
            || default_pg_ledger.parents.front()
                    .sites.front()
                    .callee_or_macro
                != "palloc") {
            throw std::runtime_error(
                "non-allowlisted PostgreSQL call was not treated "
                "as ERROR-capable");
        }
    }
    {
        compiler_analysis_facts external_macro = facts;
        flow_call_site& external_call =
            external_macro.functions.back().calls[1];
        external_call.spelling =
            external_macro.functions.back().calls.front().spelling;
        external_call.macro_chain =
            external_macro.functions.back().calls.front()
                .macro_chain;
        external_call.macro_chain.front().invocation =
            external_call.expansion;
        const safety_ledger external_macro_ledger =
            analyze_postgres_error_safety(
                manifest,
                external_macro,
                provenance,
                std::string(64, 'b'),
                std::string(64, 'd'),
                "fixture-generator",
                policy);
        if (external_macro_ledger.parents.front().sites.size()
            != 1) {
            throw std::runtime_error(
                "standard-library target inherited a PostgreSQL "
                "macro severity");
        }
    }
    {
        compiler_analysis_facts bad = facts;
        ++bad.functions.back().ast_callsite_count;
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "closure is incomplete");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.root_usrs = {
            "c:@F@plausible_but_not_the_wrapper#"};
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "roots differ");
    }
    {
        const std::vector<
            std::string compiler_analysis_facts::*>
            sealed_hashes{
                &compiler_analysis_facts::source_list_sha256,
                &compiler_analysis_facts::compile_database_sha256,
                &compiler_analysis_facts::
                    producer_executable_sha256,
                &compiler_analysis_facts::compile_graph_sha256,
                &compiler_analysis_facts::pcm_manifest_sha256,
                &compiler_analysis_facts::sql_manifest_sha256,
            };
        for (const auto sealed_hash : sealed_hashes) {
            compiler_analysis_facts bad = facts;
            (bad.*sealed_hash).clear();
            analyzer_test_detail::require_failure(
                [&] {
                    static_cast<void>(
                        analyze_postgres_error_safety(
                            manifest,
                            bad,
                            provenance,
                            std::string(64, 'b'),
                            std::string(64, 'd'),
                            "fixture-generator",
                            policy));
                },
                "metadata is incomplete");
        }
    }
    {
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        facts,
                        provenance,
                        std::string(64, 'b'),
                        {},
                        "fixture-generator",
                        policy));
            },
            "metadata is incomplete");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.erase(bad.functions.begin());
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "no generated Stage-B wrapper");
    }
    {
        compiler_analysis_facts bad = facts;
        bad.functions.front().module_name =
            "bioseq.sql.generated.test_indexes.fixture.entrypoints";
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "wrapper module differs");
    }
    {
        compiler_analysis_facts bad = facts;
        safety_manifest disconnected_manifest = manifest;
        function_flow disconnected = bad.functions.back();
        disconnected.stable_usr =
            "c:@N@fixture_owner@F@implementation#disconnected";
        for (flow_call_site& call : disconnected.calls) {
            call.stable_site_id += ".disconnected";
            call.caller_usr = disconnected.stable_usr;
        }
        disconnected.declaration_span =
            analyzer_test_detail::span(20, 400, 430);
        disconnected.body_span =
            analyzer_test_detail::span(20, 431, 520);
        disconnected_manifest.owners.front()
            .overloads.front().source_line = 20;
        bad.functions.push_back(std::move(disconnected));
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(
                    analyze_postgres_error_safety(
                        disconnected_manifest,
                        bad,
                        provenance,
                        std::string(64, 'b'),
                        std::string(64, 'd'),
                        "fixture-generator",
                        policy));
            },
            "does not reach the manifest implementation owner");
    }

    constexpr std::string_view manifest_json = R"json(
{
  "manifest_version": 1,
  "authority_mode": "annotated_cpp",
  "c_routine_identity_count": 1,
  "c_link_symbol_inventory_count": 1,
  "c_finfo_symbol_inventory_count": 1,
  "c_routine_identities": ["public.fixture()"],
  "c_link_symbols": ["fixture"],
  "c_finfo_symbols": ["pg_finfo_fixture"],
  "fmgr_entrypoint_modules": [{
    "module_name": "bioseq.sql.generated.production.fixture.entrypoints",
    "wrappers": [{
      "symbol": "fixture",
      "finfo_symbol": "pg_finfo_fixture",
      "overloads": [{
        "sql_identity": "public.fixture()",
        "cpp_declaration": "fixture_owner::implementation",
        "source_path": "fixture.cppm",
        "source_line": 10,
        "argument_converter_keys": ["argument.int32.by_value"],
        "result_converter_key": "result.int32.by_value",
        "injected_call_context": false
      }]
    }]
  }],
  "c_symbol_owners": [{
    "symbol": "fixture",
    "finfo_symbol": "pg_finfo_fixture",
    "generated_wrapper": true,
    "routine_identities": ["public.fixture()"],
    "implementation_declarations": ["fixture_owner::implementation"]
  }]
}
)json";
    const manifest_closure_policy manifest_policy{
        .expected_parent_count = 1,
        .expected_symbol_count = 1,
        .shared_symbols = {},
    };
    if (parse_safety_manifest(manifest_json, manifest_policy)
            .owners.front().generated_wrapper_module.empty()) {
        throw std::runtime_error(
            "manifest root join lost generated wrapper provenance");
    }
    const auto manifest_with_consistent_identity =
        [manifest_json](std::string_view replacement) {
            std::string result{manifest_json};
            constexpr std::string_view original{"public.fixture()"};
            std::size_t cursor = 0;
            std::size_t replacement_count = 0;
            while ((cursor = result.find(original, cursor))
                   != std::string::npos) {
                result.replace(
                    cursor, original.size(), replacement);
                cursor += replacement.size();
                ++replacement_count;
            }
            if (replacement_count != 3) {
                throw std::runtime_error(
                    "manifest identity mutation did not cover every "
                    "closure representation");
            }
            return result;
        };
    for (const std::string_view residual_identity : {
             std::string_view{"sql:function:public.fixture()"},
             std::string_view{"$extension.fixture()"},
             std::string_view{"public.fixture($extension.arg)"},
         }) {
        const std::string residual_manifest =
            manifest_with_consistent_identity(residual_identity);
        analyzer_test_detail::require_failure(
            [&] {
                static_cast<void>(parse_safety_manifest(
                    residual_manifest, manifest_policy));
            },
            "must not retain a sql: kind prefix or $extension. token");
    }
    const std::string non_regprocedure_manifest =
        manifest_with_consistent_identity("public.fixture");
    analyzer_test_detail::require_failure(
        [&] {
            static_cast<void>(parse_safety_manifest(
                non_regprocedure_manifest, manifest_policy));
        },
        "must be a regprocedure-like");
    std::string repeated_converter_manifest{manifest_json};
    const std::string converter_marker{
        R"json(["argument.int32.by_value"])json"};
    repeated_converter_manifest.replace(
        repeated_converter_manifest.find(converter_marker),
        converter_marker.size(),
        R"json(["argument.int32.by_value", "argument.int32.by_value"])json");
    if (parse_safety_manifest(
            repeated_converter_manifest, manifest_policy)
            .owners.front().overloads.front()
            .argument_converter_keys.size() != 2) {
        throw std::runtime_error(
            "manifest parser lost positional repeated converter keys");
    }
    std::string empty_converter_manifest{manifest_json};
    empty_converter_manifest.replace(
        empty_converter_manifest.find(converter_marker),
        converter_marker.size(),
        R"json([""])json");
    analyzer_test_detail::require_failure(
        [&] {
            static_cast<void>(parse_safety_manifest(
                empty_converter_manifest, manifest_policy));
        },
        "contains an empty string");
    std::string incomplete_manifest{manifest_json};
    const std::string marker{"\"generated_wrapper\": true"};
    incomplete_manifest.replace(
        incomplete_manifest.find(marker),
        marker.size(),
        "\"generated_wrapper\": false");
    analyzer_test_detail::require_failure(
        [&] {
            static_cast<void>(parse_safety_manifest(
                incomplete_manifest, manifest_policy));
        },
        "not owned by generated boundary code");
    std::string mismatched_owner_manifest{manifest_json};
    const std::string implementation_marker{
        "\"implementation_declarations\": "
        "[\"fixture_owner::implementation\"]"};
    mismatched_owner_manifest.replace(
        mismatched_owner_manifest.find(implementation_marker),
        implementation_marker.size(),
        "\"implementation_declarations\": [\"other\"]");
    analyzer_test_detail::require_failure(
        [&] {
            static_cast<void>(parse_safety_manifest(
                mismatched_owner_manifest, manifest_policy));
        },
        "implementation declarations differ");
    analyzer_test_detail::require_failure(
        [] {
            static_cast<void>(
                parse_json("{\"key\": 1, \"key\": 2}"));
        },
        "duplicate object key");
}

}  // namespace bioseq::codegen::safety
