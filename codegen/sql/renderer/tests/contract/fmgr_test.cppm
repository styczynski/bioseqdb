module;

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:tests.contract.fmgr;

import :fmgr;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

namespace fmgr_contract_test_detail {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{
            "FMgr renderer contract failed: " + std::string{message}};
    }
}

template <class p_operation>
inline void require_failure(
        p_operation&& operation,
        std::string_view expected,
        std::string_view message) {
    try {
        std::invoke(std::forward<p_operation>(operation));
    } catch (const std::exception& error) {
        if (std::string_view{error.what()}.contains(expected)) {
            return;
        }
        throw std::runtime_error{
            "FMgr renderer contract received an unexpected diagnostic: "
            + std::string{error.what()}};
    }
    throw std::runtime_error{
        "FMgr renderer contract expected failure: "
        + std::string{message}};
}

[[nodiscard]] inline normalized_entity scalar_routine(
        std::string source_module = "bioseq.postgres.fixture") {
    return normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = "contract_probe",
            .cpp_qualified_name =
                "bioseq::postgres::fixture::contract_probe",
            .source_module = std::move(source_module),
            .source_path = "bioseq/backend/pg/fixture.cppm",
            .source_range = {
                .begin = {.line = 17},
                .end = {.line = 17},
            },
            .schema = "$extension",
            .cpp_return_type = "std::int32_t",
            .parameters = {
                routine_parameter{
                    .name = "value",
                    .cpp_type = "std::optional<std::int32_t>",
                    .sql_type = "integer",
                    .nullable = true,
                },
            },
            .result_sql_type = "integer",
        },
        .stable_identity =
            "sql:function:$extension.contract_probe(integer)",
        .installed_identity =
            "sql:function:bioseq.contract_probe(integer)",
    };
}

}  // namespace fmgr_contract_test_detail

inline void run_fmgr_renderer_contract_tests() {
    using fmgr_contract_test_detail::require;

    const fmgr_detail::normalized_cpp_type normalized =
        fmgr_detail::normalize_cpp_type(
            "const std::__1::optional<std::int32_t>&");
    require(
        normalized.canonical == "std::int32_t"
            && normalized.nullable,
        "C++ type normalization lost libc++/optional canonicalization");
    require(
        fmgr_detail::canonical_sql_type(" INT4 ") == "integer"
            && fmgr_detail::canonical_sql_type("float8")
                == "doubleprecision",
        "SQL type aliases are not canonicalized");

    sql_graph graph;
    graph.compiler_profile = "production";
    graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine());
    const std::vector<rendered_fmgr_entrypoint_module> modules =
        render_fmgr_entrypoint_modules(graph);
    require(
        modules.size() == 1,
        "one natural owner did not produce one FMgr module");
    const rendered_fmgr_entrypoint_module& module = modules.front();
    require(
        module.owner_key == "postgres_fixture"
            && module.source_module == "bioseq.postgres.fixture"
            && module.module_name
                == "bioseq.postgres.sql.generated.production."
                   "postgres_fixture.entrypoints",
        "natural FMgr ownership did not use canonical module identities");
    require(
        module.wrappers.size() == 1
            && module.wrappers.front().public_symbol
                == "contract_probe"
            && module.wrappers.front().overloads.size() == 1
            && module.wrappers.front().overloads.front().sql_arity == 1
            && module.wrappers.front().overloads.front().sql_identity
                == "contract_probe(integer)",
        "rendered FMgr wrapper lost its natural routine contract");
    require(
        module.content.contains(
            "bioseq::postgres::support::invoke_postgres_callback")
            && module.content.contains(
                "export module bioseq.postgres.sql.generated.production."),
        "rendered FMgr source did not use canonical support/module identities");

    sql_graph configured_graph;
    configured_graph.compiler_profile = "test_indexes";
    configured_graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine());
    const auto configured = render_fmgr_entrypoint_modules(
        configured_graph,
        {{"fixture_owner", "bioseq.postgres.fixture"}});
    require(
        configured.size() == 1
            && configured.front().owner_key == "fixture_owner"
            && configured.front().module_name
                == "bioseq.postgres.sql.generated.production."
                   "fixture_owner.entrypoints",
        "configured FMgr owner did not retain its one-to-one identity");

    fmgr_contract_test_detail::require_failure(
        [&configured_graph] {
            static_cast<void>(render_fmgr_entrypoint_modules(
                configured_graph,
                {
                    {"duplicate", "bioseq.postgres.fixture"},
                    {"duplicate", "bioseq.postgres.other"},
                }));
        },
        "not one-to-one",
        "duplicate configured FMgr ownership was accepted");
}

}  // namespace bioseq::codegen::sql
