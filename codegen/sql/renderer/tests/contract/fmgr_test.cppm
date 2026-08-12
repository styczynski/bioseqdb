module;

#include <functional>
#include <set>
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
        std::string source_module = "bioseq.postgres.fixture",
        std::string routine_name = "contract_probe") {
    const std::string stable_identity =
        "sql:function:$extension." + routine_name + "(integer)";
    const std::string installed_identity =
        "sql:function:bioseq." + routine_name + "(integer)";
    return normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = routine_name,
            .cpp_qualified_name =
                "bioseq::postgres::fixture::" + routine_name,
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
        .stable_identity = stable_identity,
        .installed_identity = installed_identity,
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
        render_fmgr_entrypoint_modules(
            graph,
            {{"fixture_owner", "bioseq.postgres.fixture"}});
    require(
        modules.size() == 1,
        "one natural owner did not produce one FMgr module");
    const rendered_fmgr_entrypoint_module& module = modules.front();
    require(
        module.owner_key == "fixture_owner"
            && module.source_module == "bioseq.postgres.fixture"
            && module.module_name
                == "bioseq.postgres.sql.generated.production."
                   "fixture_owner.entrypoints",
        "explicit FMgr ownership did not use registry identities");
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
        module.content.starts_with(
            "module;\n\n"
            "#include <bioseq/pg_compat.h>\n\n")
            && !module.content.contains(
                "#include \"common.h\"")
            && module.content.contains(
                "bioseq::postgres::support::invoke_postgres_callback")
            && module.content.contains(
                "export module bioseq.postgres.sql.generated.production."),
        "rendered FMgr source did not use its canonical PostgreSQL "
        "include closure and support/module identities");

    sql_graph sequence_storage_graph;
    sequence_storage_graph.compiler_profile = "production";
    sequence_storage_graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine(
            "bioseq.postgres.storage.manager:sequence_storage",
            "sequence_storage_probe"));
    const auto sequence_storage_modules = render_fmgr_entrypoint_modules(
        sequence_storage_graph,
        {{"storage_manager_sequence_storage_manager",
          "bioseq.postgres.storage.manager:sequence_storage"}});
    require(
        sequence_storage_modules.size() == 1
            && sequence_storage_modules.front().owner_key
                == "storage_manager_sequence_storage_manager"
            && sequence_storage_modules.front().source_module
                == "bioseq.postgres.storage.manager:sequence_storage"
            && sequence_storage_modules.front().module_name
                == "bioseq.postgres.sql.generated.production."
                   "storage_manager_sequence_storage_manager.entrypoints"
            && sequence_storage_modules.front().content.contains(
                "import bioseq.postgres.storage.manager;\n")
            && !sequence_storage_modules.front().content.contains(
                "import bioseq.postgres.storage.manager:sequence_storage;\n"),
        "generated sequence-storage FMGr wrapper did not import its public "
        "manager umbrella");

    const std::vector<rendered_fmgr_entrypoint_module>
        vector_registry_modules =
            render_support_vector_registry_modules("production");
    require(
        vector_registry_modules.size() == 2
            && vector_registry_modules[0].owner_key
                == "vector_comparison_registry"
            && vector_registry_modules[0].content.starts_with(
                "module;\n\n"
                "#include <bioseq/pg_compat.h>\n\n")
            && !vector_registry_modules[0].content.contains(
                "#include \"common.h\"")
            && vector_registry_modules[0].content.contains(
                "PG_CATALOG_NAMESPACE, BPCHAROID, BPCHAROID, BOOLOID")
            && vector_registry_modules[1].owner_key
                == "vector_aggregate_registry"
            && vector_registry_modules[1].content.starts_with(
                "module;\n\n"
                "#include <bioseq/pg_compat.h>\n\n")
            && !vector_registry_modules[1].content.contains(
                "#include \"common.h\"")
            && vector_registry_modules[1].content.contains(
                "PG_CATALOG_NAMESPACE, ANYOID, INT8OID, INT8OID"),
        "generated vector registries lost their canonical PostgreSQL "
        "include closure or catalog-OID contract");

    require(
        fmgr_detail::registered_module_closure_contains(
            "bioseq.postgres.fixture", "bioseq.postgres.fixture")
            && fmgr_detail::registered_module_closure_contains(
                "bioseq.postgres.fixture",
                "bioseq.postgres.fixture.child")
            && fmgr_detail::registered_module_closure_contains(
                "bioseq.postgres.fixture",
                "bioseq.postgres.fixture:partition")
            && !fmgr_detail::registered_module_closure_contains(
                "bioseq.postgres.fixture",
                "bioseq.postgres.fixture_extra")
            && !fmgr_detail::registered_module_closure_contains(
                "bioseq.postgres.fixture", "bioseq.postgres.fixtures"),
        "registered FMgr module closure lost delimiter boundaries");

    fmgr_contract_test_detail::require_failure(
        [&graph] {
            static_cast<void>(render_fmgr_entrypoint_modules(graph, {}));
        },
        "has no configured owner mapping",
        "missing configured FMgr ownership was accepted");

    fmgr_contract_test_detail::require_failure(
        [&graph] {
            static_cast<void>(render_fmgr_entrypoint_modules(
                graph,
                {
                    {"fixture_owner", "bioseq.postgres.fixture"},
                    {"unused_owner", "bioseq.postgres.unused"},
                }));
        },
        "has no generated routine",
        "unused configured FMgr ownership was accepted");

    fmgr_contract_test_detail::require_failure(
        [&graph] {
            static_cast<void>(render_fmgr_entrypoint_modules(
                graph,
                {
                    {"duplicate", "bioseq.postgres.fixture"},
                    {"duplicate", "bioseq.postgres.other"},
                }));
        },
        "not one-to-one",
        "duplicate configured FMgr owner key was accepted");

    fmgr_contract_test_detail::require_failure(
        [&graph] {
            static_cast<void>(render_fmgr_entrypoint_modules(
                graph,
                {
                    {"first", "bioseq.postgres.fixture"},
                    {"second", "bioseq.postgres.fixture"},
                }));
        },
        "not one-to-one",
        "duplicate configured FMgr source module was accepted");

    fmgr_contract_test_detail::require_failure(
        [&graph] {
            static_cast<void>(render_fmgr_entrypoint_modules(
                graph,
                {
                    {"broad", "bioseq.postgres.fixture"},
                    {"child", "bioseq.postgres.fixture.child"},
                }));
        },
        "closures overlap",
        "overlapping configured FMgr module closures were accepted");

    sql_graph table_graph;
    table_graph.compiler_profile = "production";
    table_graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine(
            "bioseq.postgres.table:access_hooks", "hooks_probe"));
    table_graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine(
            "bioseq.postgres.table:access_method", "method_probe"));
    table_graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine(
            "bioseq.postgres.table:custom_scan", "scan_probe"));
    const auto table_modules = render_fmgr_entrypoint_modules(
        table_graph,
        {{"table_access_hooks", "bioseq.postgres.table"}});
    require(
        table_modules.size() == 1
            && table_modules.front().owner_key == "table_access_hooks"
            && table_modules.front().wrappers.size() == 3
            && table_modules.front().module_name
                == "bioseq.postgres.table:sql_entrypoints"
            && table_modules.front().content.contains(
                "import :access_hooks;")
            && table_modules.front().content.contains(
                "import :access_method;")
            && table_modules.front().content.contains(
                "import :custom_scan;"),
        "registered table root did not retain its three exact partition "
        "owners");

    sql_graph vector_graph;
    vector_graph.compiler_profile = "production";
    vector_graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine(
            "bioseq.postgres.vectorization.types.integer",
            "integer_probe"));
    vector_graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine(
            "bioseq.postgres.vectorization.types.date",
            "date_probe"));
    vector_graph.entities.push_back(
        fmgr_contract_test_detail::scalar_routine(
            "bioseq.postgres.vectorization.types.aggregates",
            "aggregate_probe"));
    const auto vector_modules = render_fmgr_entrypoint_modules(
        vector_graph,
        {
            {"vector_types_integer",
             "bioseq.postgres.vectorization.types.integer"},
            {"vector_types_date",
             "bioseq.postgres.vectorization.types.date"},
            {"vector_types_aggregates",
             "bioseq.postgres.vectorization.types.aggregates"},
        });
    std::set<std::string, std::less<>> vector_contracts;
    for (const rendered_fmgr_entrypoint_module& vector_module :
         vector_modules) {
        require(
            vector_module.wrappers.size() == 1,
            "vector FMgr owner did not retain one exact wrapper");
        vector_contracts.emplace(
            vector_module.owner_key + "="
            + vector_module.source_module + "="
            + vector_module.wrappers.front().public_symbol);
    }
    require(
        vector_modules.size() == 3
            && vector_contracts
                == std::set<std::string, std::less<>>{
                    "vector_types_integer="
                    "bioseq.postgres.vectorization.types.integer="
                    "integer_probe",
                    "vector_types_date="
                    "bioseq.postgres.vectorization.types.date="
                    "date_probe",
                    "vector_types_aggregates="
                    "bioseq.postgres.vectorization.types.aggregates="
                    "aggregate_probe"},
        "explicit vector FMgr source modules did not retain their three "
        "registered owner/source/wrapper contracts");
}

}  // namespace bioseq::codegen::sql
