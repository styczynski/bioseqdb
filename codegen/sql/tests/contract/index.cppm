module;

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql:tests.contract;

import :policy;
import bioseq.codegen.sql.graph;
import :tests.contract.guc;
import bioseq.codegen.sql.renderer;
import bioseq.codegen.structural_analysis.tests.contract;

export namespace bioseq::codegen::sql {

namespace contract_test_detail {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{
            "SQL generator contract failed: " + std::string{message}};
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
            "SQL generator contract received an unexpected diagnostic: "
            + std::string{error.what()}};
    }
    throw std::runtime_error{
        "SQL generator contract expected failure: "
        + std::string{message}};
}

[[nodiscard]] inline normalized_entity sequence_entity(
        std::string name,
        std::string source_module) {
    const std::string stable_identity =
        "sql:sequence:bioseq_internal." + name;
    return normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::sequence,
            .annotation_name = "sequence",
            .name = std::move(name),
            .cpp_qualified_name = "bioseq::fixture::sequence",
            .source_module = std::move(source_module),
            .source_path = "bioseq/backend/pg/fixture.cppm",
            .schema = "bioseq_internal",
        },
        .stable_identity = stable_identity,
        .installed_identity = stable_identity,
    };
}

[[nodiscard]] inline sql_graph row_codec_graph(
        std::string source_module) {
    constexpr std::string_view row_identity{
        "sql:table:bioseq_internal.fixture_row"};
    sql_graph result;
    result.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::table,
            .annotation_name = "table",
            .name = "fixture_row",
            .cpp_qualified_name = "bioseq::fixture::fixture_row",
            .source_module = source_module,
            .source_path = "bioseq/backend/pg/fixture.cppm",
            .schema = "bioseq_internal",
        },
        .stable_identity = std::string{row_identity},
        .installed_identity = std::string{row_identity},
    });
    result.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::column,
            .annotation_name = "column",
            .name = "value",
            .cpp_qualified_name = "bioseq::fixture::fixture_row::value",
            .enclosing_cpp_name = "bioseq::fixture::fixture_row",
            .source_module = std::move(source_module),
            .source_path = "bioseq/backend/pg/fixture.cppm",
            .schema = "bioseq_internal",
            .cpp_return_type = "std::int64_t",
        },
        .stable_identity =
            "sql:column:bioseq_internal.fixture_row.value",
        .installed_identity =
            "sql:column:bioseq_internal.fixture_row.value",
        .dependency_identities = {std::string{row_identity}},
    });
    return result;
}

}  // namespace contract_test_detail

/**
 * Runs the current SQL generator contracts.
 */
inline void run_generator_contract_self_tests() {
    bioseq::codegen::structural_analysis::run_preparation_contract_self_tests();
    run_guc_generator_contract_self_tests();
    run_fmgr_renderer_contract_tests();
    run_compact_fmgr_contract_tests();

    sql_graph graph;
    contract_test_detail::require(
        graph.generated_module_prefix
                == "bioseq.postgres.sql.generated"
            && graph.fmgr_boundary_module
                == "bioseq.postgres.support.sql_boundary",
        "default generated-module identities are not canonical");

    sql_graph entity_handle_graph;
    entity_handle_graph.entities.push_back(
        contract_test_detail::sequence_entity(
            "row_mask_seq",
            "bioseq.postgres.storage.metadata.sql_entities"));
    entity_handle_graph.entities.push_back(
        contract_test_detail::sequence_entity(
            "minimizer_generation_id_seq",
            "bioseq.postgres.storage.indexing.chunked_storage.sql_entities"));
    entity_handle_graph.entities.push_back(
        contract_test_detail::sequence_entity(
            "sequence_source_epoch_seq",
            "bioseq.postgres.storage.manager.sequence_storage.sql_entities"));
    const std::vector<std::pair<std::string, std::string>>
        entity_handle_owners{
            {
                "storage_metadata_core",
                "bioseq.postgres.storage.metadata.sql_entities",
            },
            {
                "storage_indexing_pg_chunked_storage",
                "bioseq.postgres.storage.indexing.chunked_storage.sql_entities",
            },
            {
                "storage_manager_sequence_storage_manager",
                "bioseq.postgres.storage.manager.sequence_storage.sql_entities",
            },
        };
    const std::vector<rendered_entity_handle_module>
        entity_handle_modules = render_entity_handle_modules(
            entity_handle_graph, entity_handle_owners);
    contract_test_detail::require(
        entity_handle_modules.size() == 3
            && entity_handle_modules[0].owner_key
                == "storage_indexing_pg_chunked_storage"
            && entity_handle_modules[1].owner_key
                == "storage_manager_sequence_storage_manager"
            && entity_handle_modules[2].owner_key
                == "storage_metadata_core",
        "entity-handle rendering did not retain explicit registry facet keys");
    contract_test_detail::require_failure(
        [&entity_handle_graph, &entity_handle_owners] {
            std::vector<std::pair<std::string, std::string>> missing =
                entity_handle_owners;
            missing.pop_back();
            static_cast<void>(render_entity_handle_modules(
                entity_handle_graph, missing));
        },
        "has no configured owner mapping",
        "entity-handle rendering accepted a missing registry mapping");
    contract_test_detail::require_failure(
        [&entity_handle_graph, &entity_handle_owners] {
            auto extra = entity_handle_owners;
            extra.emplace_back(
                "unused_owner", "bioseq.postgres.unused.sql_entities");
            static_cast<void>(render_entity_handle_modules(
                entity_handle_graph, extra));
        },
        "has no generated sequence",
        "entity-handle rendering accepted an unused registry mapping");
    contract_test_detail::require_failure(
        [&entity_handle_graph, &entity_handle_owners] {
            auto duplicate = entity_handle_owners;
            duplicate.emplace_back(
                "storage_metadata_core",
                "bioseq.postgres.other.sql_entities");
            static_cast<void>(render_entity_handle_modules(
                entity_handle_graph, duplicate));
        },
        "not one-to-one",
        "entity-handle rendering accepted a duplicate owner key");
    contract_test_detail::require_failure(
        [&entity_handle_graph, &entity_handle_owners] {
            auto duplicate = entity_handle_owners;
            duplicate.emplace_back(
                "other_metadata_owner",
                "bioseq.postgres.storage.metadata.sql_entities");
            static_cast<void>(render_entity_handle_modules(
                entity_handle_graph, duplicate));
        },
        "not one-to-one",
        "entity-handle rendering accepted a duplicate source module");

    const sql_graph row_codec_graph =
        contract_test_detail::row_codec_graph(
            "bioseq.postgres.storage.indexing.sql_rows");
    const std::vector<std::pair<std::string, std::string>>
        row_codec_owners{{
            "storage_indexing_pg",
            "bioseq.postgres.storage.indexing.sql_rows",
        }};
    const std::vector<rendered_row_codec_module> row_codec_modules =
        render_row_codec_modules(row_codec_graph, row_codec_owners);
    contract_test_detail::require(
        row_codec_modules.size() == 1
            && row_codec_modules.front().owner_key
                == "storage_indexing_pg"
            && row_codec_modules.front().source_module
                == "bioseq.postgres.storage.indexing.sql_rows"
            && row_codec_modules.front().module_name
                == "bioseq.postgres.storage.indexing.sql_row_codecs"
            && row_codec_modules.front().content.starts_with(
                "module;\n\n"
                "#include <bioseq/pg_compat.h>\n"
                "#include \"utils/builtins.h\"\n"
                "#include \"utils/timestamp.h\"\n"
                "#include \"utils/xid8.h\"\n\n")
            && !row_codec_modules.front().content.contains(
                "#include \"common.h\"")
            && row_codec_modules.front().content.contains(
                "static constexpr Oid type_oid = INT8OID;")
            && row_codec_modules.front().content.contains(
                "return F_INT8EQ;")
            && row_codec_modules.front().content.contains(
                "export module "
                "bioseq.postgres.storage.indexing.sql_row_codecs;"),
        "row-codec rendering did not retain its PostgreSQL include "
        "closure, explicit registry authority, and public generated "
        "module identity");
    contract_test_detail::require_failure(
        [&row_codec_graph] {
            static_cast<void>(render_row_codec_modules(
                row_codec_graph, {}));
        },
        "has no configured owner mapping",
        "row-codec rendering accepted a missing registry mapping");
    contract_test_detail::require_failure(
        [&row_codec_graph, &row_codec_owners] {
            auto extra = row_codec_owners;
            extra.emplace_back(
                "unused_owner", "bioseq.postgres.unused.sql_rows");
            static_cast<void>(render_row_codec_modules(
                row_codec_graph, extra));
        },
        "has no generated row",
        "row-codec rendering accepted an unused registry mapping");
    contract_test_detail::require_failure(
        [&row_codec_graph, &row_codec_owners] {
            auto duplicate = row_codec_owners;
            duplicate.emplace_back(
                "storage_indexing_pg",
                "bioseq.postgres.other.sql_rows");
            static_cast<void>(render_row_codec_modules(
                row_codec_graph, duplicate));
        },
        "not one-to-one",
        "row-codec rendering accepted a duplicate owner key");
    contract_test_detail::require_failure(
        [&row_codec_graph, &row_codec_owners] {
            auto duplicate = row_codec_owners;
            duplicate.emplace_back(
                "other_owner",
                "bioseq.postgres.storage.indexing.sql_rows");
            static_cast<void>(render_row_codec_modules(
                row_codec_graph, duplicate));
        },
        "not one-to-one",
        "row-codec rendering accepted a duplicate source module");
    validate_codegen_completion_policy(graph, true);

    sql_graph reviewed_external_graph = graph;
    reviewed_external_graph.handwritten_fmgr_export_owners.push_back({
        .macro_name = "POSTGRES_FN",
        .symbol = "bioseq_external_handler",
        .source_module = "bioseq.postgres.external.relation",
        .source_path =
            "bioseq/backend/pg/external/relation.cppm",
        .source_line = 694,
    });
    validate_codegen_completion_policy(reviewed_external_graph, true);

    sql_graph duplicate_reviewed_external_graph = reviewed_external_graph;
    const handwritten_fmgr_export_owner reviewed_external_owner =
        duplicate_reviewed_external_graph
            .handwritten_fmgr_export_owners.front();
    duplicate_reviewed_external_graph.handwritten_fmgr_export_owners
        .push_back(reviewed_external_owner);
    contract_test_detail::require_failure(
        [&duplicate_reviewed_external_graph] {
            validate_codegen_completion_policy(
                duplicate_reviewed_external_graph, true);
        },
        "only the reviewed external-relation handwritten fmgr export",
        "complete code generation accepted duplicate reviewed external "
        "FMgr owners");

    graph.handwritten_fmgr_export_owners.push_back({
        .macro_name = "PG_FUNCTION_INFO_V1",
        .symbol = "handwritten_fixture_symbol",
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .source_line = 1,
    });
    contract_test_detail::require_failure(
        [&graph] {
            validate_codegen_completion_policy(graph, true);
        },
        "only the reviewed external-relation handwritten fmgr export",
        "complete code generation accepted handwritten FMgr ownership");

    reviewed_external_graph.handwritten_fmgr_export_owners.front()
        .source_module = "bioseq.postgres.external.unreviewed";
    contract_test_detail::require_failure(
        [&reviewed_external_graph] {
            validate_codegen_completion_policy(
                reviewed_external_graph, true);
        },
        "only the reviewed external-relation handwritten fmgr export",
        "complete code generation accepted a drifted external FMgr owner");
}

}  // namespace bioseq::codegen::sql
