module;

#include <algorithm>
#include <functional>
#include <iterator>
#include <ranges>
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

[[nodiscard]] inline collected_entity support_consumer() {
    return collected_entity{
        .kind = entity_kind::function,
        .annotation_name = "function",
        .name = "uses_fixture_support",
        .cpp_qualified_name = "bioseq::fixture::uses_fixture_support",
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "fixture_api",
        .options = {{
            .name = "support",
            .expression = "\"fixture_support\"",
        }},
        .parameters = {{
            .name = "value",
            .cpp_type = "std::int32_t",
            .sql_type = "integer",
        }},
        .result_sql_type = "boolean",
    };
}

[[nodiscard]] inline collected_entity support_provider(
        std::string schema = "$extension",
        std::string cpp_suffix = {}) {
    return collected_entity{
        .kind = entity_kind::function,
        .annotation_name = "function",
        .name = "fixture_support",
        .cpp_qualified_name =
            "bioseq::fixture::fixture_support" + std::move(cpp_suffix),
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = std::move(schema),
        .parameters = {{
            .name = "request",
            .cpp_type = "Datum",
            .sql_type = "internal",
        }},
        .result_sql_type = "internal",
    };
}

[[nodiscard]] inline collected_program support_dependency_program() {
    collected_program result;
    result.entities.push_back(support_consumer());
    result.entities.push_back(support_provider());
    return result;
}

[[nodiscard]] inline collected_entity fixture_schema(std::string name) {
    return collected_entity{
        .kind = entity_kind::schema,
        .annotation_name = "schema",
        .name = std::move(name),
        .cpp_qualified_name = "bioseq::fixture::schema",
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
    };
}

[[nodiscard]] inline collected_entity fixture_type(
    std::string name,
    std::string cpp_name,
    std::string schema,
    bool base_type = false) {
    collected_entity result{
        .kind = entity_kind::type,
        .annotation_name = "type",
        .name = std::move(name),
        .cpp_qualified_name = std::move(cpp_name),
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = std::move(schema),
    };
    if (base_type) {
        result.options.push_back(option_value{
            .name = "internal_length",
            .expression = "4",
        });
    }
    return result;
}

[[nodiscard]] inline collected_entity fixture_type_column(
    std::string enclosing_cpp_name,
    std::string schema) {
    return collected_entity{
        .kind = entity_kind::column,
        .annotation_name = "column",
        .name = "value",
        .cpp_qualified_name = enclosing_cpp_name + "::value",
        .enclosing_cpp_name = std::move(enclosing_cpp_name),
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = std::move(schema),
        .cpp_return_type = "std::int32_t",
        .companion = true,
    };
}

[[nodiscard]] inline collected_entity fixture_function(
    std::string name,
    std::string schema,
    std::vector<routine_parameter> parameters,
    std::string result_sql_type) {
    return collected_entity{
        .kind = entity_kind::function,
        .annotation_name = "function",
        .name = name,
        .cpp_qualified_name = "bioseq::fixture::" + name,
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = std::move(schema),
        .parameters = std::move(parameters),
        .result_sql_type = result_sql_type,
        .result = routine_result_contract{
            .sql_type = std::move(result_sql_type),
        },
    };
}

[[nodiscard]] inline collected_entity fixture_type_role(
    std::string annotation_name,
    std::string function_name,
    std::string type_cpp_name) {
    return collected_entity{
        .kind = entity_kind::type_role,
        .annotation_name = std::move(annotation_name),
        .name = function_name,
        .cpp_qualified_name = "bioseq::fixture::" + function_name,
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "$extension",
        .options = {{
            .name = "for_type",
            .expression = "bioseq::sql::type_of<" + type_cpp_name + ">",
        }},
        .companion = true,
    };
}

[[nodiscard]] inline collected_entity fixture_table(
    std::string name,
    std::string cpp_name) {
    return collected_entity{
        .kind = entity_kind::table,
        .annotation_name = "table",
        .name = std::move(name),
        .cpp_qualified_name = std::move(cpp_name),
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "fixture_types",
    };
}

[[nodiscard]] inline collected_entity fixture_table_check(
    std::string name,
    std::string owner_cpp_name,
    std::string dependency_cpp_name) {
    const std::string sql_name = name;
    const std::string check_cpp_name = owner_cpp_name + "::" + sql_name;
    return collected_entity{
        .kind = entity_kind::check,
        .annotation_name = "check",
        .name = std::move(name),
        .cpp_qualified_name = check_cpp_name,
        .enclosing_cpp_name = std::move(owner_cpp_name),
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "fixture_types",
        .options = {
            {
                .name = "name",
                .expression = "\"" + sql_name + "\"",
            },
            {
                .name = "expression",
                .expression = "bioseq::sql::checked_sql{\"value > 0\"}",
            },
        },
        .dependencies = {{
            .expression = std::move(dependency_cpp_name),
        }},
        .companion = true,
    };
}

[[nodiscard]] inline collected_program inline_constraint_program(
    bool reciprocal = false) {
    constexpr std::string_view first_cpp{"bioseq::fixture::first_table"};
    constexpr std::string_view second_cpp{"bioseq::fixture::second_table"};
    collected_program result;
    result.entities.push_back(fixture_schema("fixture_types"));
    result.entities.push_back(fixture_table(
        "first_table", std::string{first_cpp}));
    result.entities.push_back(fixture_type_column(
        std::string{first_cpp}, "fixture_types"));
    result.entities.push_back(fixture_table(
        "second_table", std::string{second_cpp}));
    result.entities.push_back(fixture_type_column(
        std::string{second_cpp}, "fixture_types"));
    result.entities.push_back(fixture_table_check(
        "first_check",
        std::string{first_cpp},
        std::string{second_cpp}));
    if (reciprocal) {
        result.entities.push_back(fixture_table_check(
            "second_check",
            std::string{second_cpp},
            std::string{first_cpp}));
    }
    return result;
}

[[nodiscard]] inline collected_program nested_operator_program() {
    constexpr std::string_view type_cpp{
        "bioseq::fixture::fixture_letter"};
    constexpr std::string_view routine_cpp{
        "bioseq::fixture::fixture_letter::fixture_letter_lt"};
    std::vector<routine_parameter> parameters{
        routine_parameter{
            .name = "left",
            .cpp_type = "bioseq::fixture::fixture_letter",
            .sql_type = "fixture_types.fixture_letter",
        },
        routine_parameter{
            .name = "right",
            .cpp_type = "bioseq::fixture::fixture_letter",
            .sql_type = "fixture_types.fixture_letter",
        },
    };

    collected_program result;
    result.entities.push_back(fixture_schema("fixture_types"));
    result.entities.push_back(fixture_type(
        "fixture_letter", std::string{type_cpp}, "fixture_types"));

    collected_entity function = fixture_function(
        "fixture_letter_lt", "fixture_types", parameters, "boolean");
    function.cpp_qualified_name = routine_cpp;
    function.enclosing_cpp_name = type_cpp;
    result.entities.push_back(std::move(function));

    result.entities.push_back(collected_entity{
        .kind = entity_kind::operator_,
        .annotation_name = "operator_",
        .name = "<",
        .cpp_qualified_name = std::string{routine_cpp},
        .enclosing_cpp_name = std::string{type_cpp},
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "fixture_types",
        .options = {{
            .name = "symbol",
            .expression = "\"<\"",
        }},
        .parameters = std::move(parameters),
        .companion = true,
    });
    return result;
}

[[nodiscard]] inline collected_program opclass_program(
    std::string indexed_type = "fixture_types.fixture_letter",
    bool include_annotated_type = true) {
    constexpr std::string_view less_cpp{
        "bioseq::fixture::fixture_letter::fixture_letter_lt"};
    constexpr std::string_view compare_cpp{
        "bioseq::fixture::fixture_letter::fixture_letter_cmp"};
    collected_program result = nested_operator_program();
    if (!include_annotated_type) {
        std::erase_if(
            result.entities,
            [](const collected_entity& entity) {
                return entity.kind == entity_kind::schema
                    || entity.kind == entity_kind::type;
            });
    }
    result.entities.push_back(fixture_schema("fixture_operators"));
    for (collected_entity& entity : result.entities) {
        if (entity.cpp_qualified_name != less_cpp) continue;
        entity.schema = "fixture_operators";
        for (routine_parameter& parameter : entity.parameters) {
            parameter.sql_type = indexed_type;
        }
    }

    std::vector<routine_parameter> compare_parameters{
        routine_parameter{
            .name = "left",
            .cpp_type = "bioseq::fixture::fixture_letter",
            .sql_type = indexed_type,
        },
        routine_parameter{
            .name = "right",
            .cpp_type = "bioseq::fixture::fixture_letter",
            .sql_type = indexed_type,
        },
    };
    collected_entity compare = fixture_function(
        "fixture_letter_cmp", "fixture_operators",
        std::move(compare_parameters), "integer");
    compare.cpp_qualified_name = compare_cpp;
    compare.enclosing_cpp_name =
        "bioseq::fixture::fixture_letter";
    result.entities.push_back(std::move(compare));

    result.entities.push_back(collected_entity{
        .kind = entity_kind::opclass,
        .annotation_name = "opclass",
        .name = "fixture_letter_ops",
        .cpp_qualified_name = "bioseq::fixture::fixture_letter_ops",
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "$extension",
        .options = {
            {
                .name = "access_method",
                .expression =
                    "bioseq::sql::pg_catalog_reference::btree",
            },
            {
                .name = "is_default",
                .expression = "true",
            },
            {
                .name = "operators",
                .expression =
                    "bioseq::sql::opclass_operators({"
                    "bioseq::sql::opclass_operator(1, "
                    "bioseq::fixture::fixture_letter::fixture_letter_lt)})",
            },
            {
                .name = "functions",
                .expression =
                    "bioseq::sql::opclass_functions({"
                    "bioseq::sql::opclass_function(1, "
                    "bioseq::fixture::fixture_letter::fixture_letter_cmp)})",
            },
        },
    });
    return result;
}

[[nodiscard]] inline collected_program view_dependency_program() {
    constexpr std::string_view storage_cpp{
        "bioseq::fixture::catalog_views::storage"};
    collected_program result;
    result.entities.push_back(fixture_schema("fixture_types"));
    result.entities.push_back(collected_entity{
        .kind = entity_kind::view,
        .annotation_name = "view",
        .name = "storage",
        .cpp_qualified_name = std::string{storage_cpp},
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "fixture_types",
    });
    result.entities.push_back(collected_entity{
        .kind = entity_kind::grant,
        .annotation_name = "grant",
        .name = "storage",
        .cpp_qualified_name = std::string{storage_cpp},
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "fixture_types",
        .companion = true,
    });
    result.entities.push_back(collected_entity{
        .kind = entity_kind::view,
        .annotation_name = "view",
        .name = "chunk",
        .cpp_qualified_name =
            "bioseq::fixture::catalog_views::chunk",
        .source_module = "bioseq.postgres.fixture",
        .source_path = "bioseq/backend/pg/fixture.cppm",
        .schema = "fixture_types",
        .dependencies = {{
            .expression = std::string{storage_cpp},
        }},
    });
    return result;
}

[[nodiscard]] inline collected_program annotated_type_program() {
    constexpr std::string_view payload_cpp{
        "bioseq::fixture::payload"};
    constexpr std::string_view base_cpp{
        "bioseq::fixture::base_value"};
    collected_program result;
    collected_entity payload_consumer = fixture_function(
        "consume_payload",
        "fixture_api",
        {{
            .name = "values",
            .cpp_type = "void*",
            .sql_type = "fixture_types.fixture_payload(7)[]",
        }},
        "setof fixture_types.fixture_payload");
    payload_consumer.result.form = routine_result_form::set_of;
    payload_consumer.result.sql_type.clear();
    result.entities.push_back(std::move(payload_consumer));
    result.entities.push_back(fixture_function(
        "consume_base",
        "fixture_api",
        {{
            .name = "value",
            .cpp_type = "void*",
            .sql_type = "fixture_types.fixture_base[]",
        }},
        "integer"));
    result.entities.push_back(fixture_schema("fixture_api"));
    result.entities.push_back(fixture_schema("fixture_types"));
    result.entities.push_back(fixture_type(
        "fixture_payload",
        std::string{payload_cpp},
        "fixture_types"));
    result.entities.push_back(fixture_type_column(
        std::string{payload_cpp}, "fixture_types"));
    result.entities.push_back(fixture_type(
        "renamed_element",
        "bioseq::fixture::element_value",
        "fixture_types"));
    result.entities.push_back(fixture_type_column(
        "bioseq::fixture::element_value", "fixture_types"));
    collected_entity base = fixture_type(
        "fixture_base",
        std::string{base_cpp},
        "fixture_types",
        true);
    base.options.push_back(option_value{
        .name = "element",
        .expression =
            "bioseq::sql::type_of<bioseq::fixture::element_value>",
    });
    result.entities.push_back(std::move(base));
    result.entities.push_back(fixture_type(
        "z_fixture_callback_dependency",
        "bioseq::fixture::callback_dependency",
        "fixture_types"));
    result.entities.push_back(fixture_type_column(
        "bioseq::fixture::callback_dependency", "fixture_types"));
    collected_entity payload_reference_column = fixture_type_column(
        std::string{payload_cpp}, "fixture_types");
    payload_reference_column.name = "linked";
    payload_reference_column.cpp_qualified_name =
        std::string{payload_cpp} + "::linked";
    payload_reference_column.options = {{
        .name = "sql_type",
        .expression =
            "bioseq::sql::custom_type<\"fixture_types.z_fixture_callback_dependency\">",
    }};
    payload_reference_column.resolved_sql_type =
        "fixture_types.z_fixture_callback_dependency";
    result.entities.push_back(std::move(payload_reference_column));
    result.entities.push_back(fixture_function(
        "fixture_callback_prerequisite",
        "fixture_types",
        {},
        "integer"));
    collected_entity base_input = fixture_function(
        "fixture_base_in",
        "fixture_types",
        {{
            .name = "value",
            .cpp_type = "const char*",
            .sql_type = "cstring",
        }},
        "fixture_types.fixture_base");
    base_input.dependencies = {
        dependency_reference{
            .expression =
                "bioseq::fixture::callback_dependency",
        },
        dependency_reference{
            .expression =
                "bioseq::fixture::fixture_callback_prerequisite",
        },
    };
    result.entities.push_back(std::move(base_input));
    result.entities.push_back(fixture_function(
        "fixture_base_out",
        "fixture_types",
        {{
            .name = "value",
            .cpp_type = "void*",
            .sql_type = "fixture_types.fixture_base",
        }},
        "cstring"));
    collected_entity base_input_role = fixture_type_role(
        "type_input", "fixture_base_in", std::string{base_cpp});
    base_input_role.dependencies = {
        dependency_reference{
            .expression =
                "bioseq::fixture::callback_dependency",
        },
        dependency_reference{
            .expression =
                "bioseq::fixture::fixture_callback_prerequisite",
        },
    };
    result.entities.push_back(std::move(base_input_role));
    result.entities.push_back(fixture_type_role(
        "type_output", "fixture_base_out", std::string{base_cpp}));
    result.entities.push_back(fixture_function(
        "fixture_base_typmod_in",
        "fixture_types",
        {{
            .name = "modifiers",
            .cpp_type = "void*",
            .sql_type = "cstring[]",
        }},
        "integer"));
    collected_entity typmod_input_role = fixture_type_role(
        "type_typmod_in",
        "fixture_base_typmod_in",
        std::string{base_cpp});
    typmod_input_role.options.front().expression =
        "bioseq::sql::custom_type<\"fixture_types.fixture_base\">";
    result.entities.push_back(std::move(typmod_input_role));
    return result;
}

[[nodiscard]] inline std::size_t statement_position(
    const rendered_sql_artifact& artifact,
    std::string_view fragment) {
    for (std::size_t index = 0;
         index < artifact.statements.size();
         ++index) {
        if (artifact.statements[index].text.contains(fragment)) {
            return index;
        }
    }
    throw std::runtime_error{
        "SQL generator contract did not render statement fragment '"
        + std::string{fragment} + "'"};
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

    const sql_graph nested_operator_graph = build_sql_graph(
        contract_test_detail::nested_operator_program());
    const rendered_sql_artifact nested_operator_artifact =
        render_sql_graph(nested_operator_graph);
    const std::size_t nested_type =
        contract_test_detail::statement_position(
            nested_operator_artifact,
            "CREATE TYPE fixture_types.fixture_letter;");
    const std::size_t nested_function =
        contract_test_detail::statement_position(
            nested_operator_artifact,
            "FUNCTION fixture_types.fixture_letter_lt(");
    const std::size_t nested_operator =
        contract_test_detail::statement_position(
            nested_operator_artifact,
            "CREATE OPERATOR fixture_types.< (");
    contract_test_detail::require(
        nested_type < nested_function
            && nested_function < nested_operator
            && nested_operator_artifact.statements[nested_operator]
                .text.contains(
                    "FUNCTION = fixture_types.fixture_letter_lt")
            && nested_operator_artifact.statements[nested_operator]
                .text.contains(
                    "LEFTARG = fixture_types.fixture_letter")
            && nested_operator_artifact.statements[nested_operator]
                .text.contains(
                    "RIGHTARG = fixture_types.fixture_letter"),
        "nested operator did not select its exact routine owner after its "
        "enclosing parameter type");
    contract_test_detail::require_failure(
        [&nested_operator_graph] {
            sql_graph ambiguous = nested_operator_graph;
            const auto routine = std::ranges::find(
                ambiguous.entities,
                "fixture_letter_lt",
                [](const normalized_entity& entity) {
                    return std::string_view{entity.source.name};
                });
            const auto operator_entity = std::ranges::find_if(
                ambiguous.entities,
                [](const normalized_entity& entity) {
                    return entity.source.kind == entity_kind::operator_;
                });
            if (routine == ambiguous.entities.end()
                || operator_entity == ambiguous.entities.end()) {
                throw std::runtime_error{
                    "nested-operator ambiguity fixture is incomplete"};
            }
            normalized_entity duplicate = *routine;
            duplicate.source.name = "fixture_letter_lt_duplicate";
            duplicate.stable_identity =
                "sql:function:fixture_types.fixture_letter_lt_duplicate("
                "fixture_types.fixture_letter,fixture_types.fixture_letter)";
            duplicate.installed_identity = duplicate.stable_identity;
            operator_entity->dependency_identities.push_back(
                duplicate.stable_identity);
            ambiguous.entities.insert(
                operator_entity,
                std::move(duplicate));
            static_cast<void>(render_sql_graph(ambiguous));
        },
        "has more than one natural parent",
        "renderer accepted ambiguous exact-declaration operator ownership");

    const sql_graph fixed_opclass_graph = build_sql_graph(
        contract_test_detail::opclass_program(), "public");
    const auto fixed_opclass = std::ranges::find_if(
        fixed_opclass_graph.entities,
        [](const normalized_entity& entity) {
            return entity.source.kind == entity_kind::opclass
                && entity.source.name == "fixture_letter_ops";
        });
    contract_test_detail::require(
        fixed_opclass != fixed_opclass_graph.entities.end()
            && fixed_opclass->source.schema == "fixture_types"
            && fixed_opclass->stable_identity
                == "sql:opclass:fixture_types.fixture_letter_ops"
            && fixed_opclass->installed_identity
                == "sql:opclass:fixture_types.fixture_letter_ops",
        "operator class did not inherit its annotated indexed-type schema");
    const rendered_sql_artifact fixed_opclass_artifact =
        render_sql_graph(fixed_opclass_graph);
    const std::size_t fixed_opclass_type =
        contract_test_detail::statement_position(
            fixed_opclass_artifact,
            "CREATE TYPE fixture_types.fixture_letter;");
    const std::size_t fixed_opclass_operator =
        contract_test_detail::statement_position(
            fixed_opclass_artifact,
            "CREATE OPERATOR fixture_operators.< (");
    const std::size_t fixed_opclass_compare =
        contract_test_detail::statement_position(
            fixed_opclass_artifact,
            "FUNCTION fixture_operators.fixture_letter_cmp(");
    const std::size_t fixed_opclass_statement =
        contract_test_detail::statement_position(
            fixed_opclass_artifact,
            "CREATE OPERATOR CLASS fixture_types.fixture_letter_ops DEFAULT");
    contract_test_detail::require(
        fixed_opclass_type < fixed_opclass_statement
            && fixed_opclass_operator < fixed_opclass_statement
            && fixed_opclass_compare < fixed_opclass_statement
            && fixed_opclass_artifact.statements[fixed_opclass_statement]
                .text.contains(
                    "FOR TYPE fixture_types.fixture_letter USING btree")
            && fixed_opclass_artifact.statements[fixed_opclass_statement]
                .text.contains(
                    "OPERATOR 1 fixture_operators.< "
                    "(fixture_types.fixture_letter, "
                    "fixture_types.fixture_letter)")
            && fixed_opclass_artifact.statements[fixed_opclass_statement]
                .text.contains(
                    "FUNCTION 1 fixture_operators.fixture_letter_cmp("
                    "fixture_types.fixture_letter, "
                    "fixture_types.fixture_letter)"),
        "operator class SQL did not preserve indexed-type schema ownership");
    contract_test_detail::require_failure(
        [] {
            collected_program mixed =
                contract_test_detail::opclass_program();
            for (collected_entity& entity : mixed.entities) {
                if (entity.cpp_qualified_name
                        == "bioseq::fixture::fixture_letter::fixture_letter_lt"
                    && entity.parameters.size() == 2) {
                    entity.parameters[1].sql_type = "integer";
                }
            }
            static_cast<void>(build_sql_graph(std::move(mixed)));
        },
        "opclass 'fixture_letter_ops' has mixed indexed operand types",
        "SQL graph accepted mixed operator-class indexed operand types");
    contract_test_detail::require_failure(
        [] {
            collected_program ambiguous_type =
                contract_test_detail::opclass_program(
                    "fixture_letter",
                    /*include_annotated_type=*/false);
            ambiguous_type.entities.push_back(
                contract_test_detail::fixture_schema("first_types"));
            ambiguous_type.entities.push_back(
                contract_test_detail::fixture_schema("second_types"));
            ambiguous_type.entities.push_back(
                contract_test_detail::fixture_type(
                    "fixture_letter",
                    "bioseq::fixture::first_letter",
                    "first_types"));
            ambiguous_type.entities.push_back(
                contract_test_detail::fixture_type(
                    "fixture_letter",
                    "bioseq::fixture::second_letter",
                    "second_types"));
            static_cast<void>(
                build_sql_graph(std::move(ambiguous_type)));
        },
        "ambiguous annotated SQL type reference 'fixture_letter' "
        "for opclass 'fixture_letter_ops'",
        "SQL graph accepted ambiguous operator-class indexed-type ownership");
    contract_test_detail::require_failure(
        [] {
            collected_program ambiguous =
                contract_test_detail::opclass_program();
            ambiguous.entities.push_back(
                contract_test_detail::fixture_schema(
                    "alternate_operators"));
            const auto function = std::ranges::find_if(
                ambiguous.entities,
                [](const collected_entity& entity) {
                    return entity.kind == entity_kind::function
                        && entity.name == "fixture_letter_lt";
                });
            const auto operator_entity = std::ranges::find_if(
                ambiguous.entities,
                [](const collected_entity& entity) {
                    return entity.kind == entity_kind::operator_
                        && entity.name == "<";
                });
            if (function == ambiguous.entities.end()
                || operator_entity == ambiguous.entities.end()) {
                throw std::runtime_error{
                    "operator-class ambiguity fixture is incomplete"};
            }
            collected_entity alternate_function = *function;
            alternate_function.cpp_qualified_name =
                "bioseq::alternate::fixture_letter_lt";
            alternate_function.enclosing_cpp_name.clear();
            alternate_function.schema = "alternate_operators";
            collected_entity alternate_operator = *operator_entity;
            alternate_operator.cpp_qualified_name =
                "bioseq::alternate::fixture_letter_lt";
            alternate_operator.enclosing_cpp_name.clear();
            alternate_operator.schema = "alternate_operators";
            ambiguous.entities.push_back(std::move(alternate_function));
            ambiguous.entities.push_back(std::move(alternate_operator));
            const auto opclass = std::ranges::find_if(
                ambiguous.entities,
                [](const collected_entity& entity) {
                    return entity.kind == entity_kind::opclass;
                });
            if (opclass == ambiguous.entities.end()) {
                throw std::runtime_error{
                    "operator-class ambiguity owner is absent"};
            }
            for (option_value& option : opclass->options) {
                if (option.name == "operators") {
                    option.expression =
                        "bioseq::sql::opclass_operators({"
                        "bioseq::sql::opclass_operator(1, "
                        "fixture_letter_lt)})";
                }
            }
            static_cast<void>(build_sql_graph(std::move(ambiguous)));
        },
        "ambiguous operator-class routine reference 'fixture_letter_lt'",
        "SQL graph accepted ambiguous operator-class routine ownership");

    const sql_graph extension_opclass_graph = build_sql_graph(
        contract_test_detail::opclass_program(
            "integer", /*include_annotated_type=*/false),
        "fixture_install");
    const auto extension_opclass = std::ranges::find_if(
        extension_opclass_graph.entities,
        [](const normalized_entity& entity) {
            return entity.source.kind == entity_kind::opclass;
        });
    contract_test_detail::require(
        extension_opclass != extension_opclass_graph.entities.end()
            && extension_opclass->source.schema == "$extension"
            && extension_opclass->stable_identity
                == "sql:opclass:$extension.fixture_letter_ops"
            && extension_opclass->installed_identity
                == "sql:opclass:fixture_install.fixture_letter_ops",
        "built-in operator class did not retain relocatable schema ownership");
    const rendered_sql_artifact extension_opclass_artifact =
        render_sql_graph(extension_opclass_graph);
    const std::size_t extension_opclass_statement =
        contract_test_detail::statement_position(
            extension_opclass_artifact,
            "CREATE OPERATOR CLASS fixture_letter_ops DEFAULT");
    contract_test_detail::require(
        extension_opclass_artifact.statements[extension_opclass_statement]
                .text.contains("FOR TYPE integer USING btree")
            && !extension_opclass_artifact.statements[
                    extension_opclass_statement]
                    .text.contains("fixture_types.fixture_letter_ops")
            && !extension_opclass_artifact.statements[
                    extension_opclass_statement]
                    .text.contains("fixture_operators.fixture_letter_ops"),
        "relocatable operator class was qualified by an implementation schema");

    const sql_graph view_dependency_graph = build_sql_graph(
        contract_test_detail::view_dependency_program());
    const auto storage_view = std::ranges::find_if(
        view_dependency_graph.entities,
        [](const normalized_entity& entity) {
            return entity.source.kind == entity_kind::view
                && entity.source.name == "storage";
        });
    const auto chunk_view = std::ranges::find_if(
        view_dependency_graph.entities,
        [](const normalized_entity& entity) {
            return entity.source.kind == entity_kind::view
                && entity.source.name == "chunk";
        });
    contract_test_detail::require(
        storage_view != view_dependency_graph.entities.end()
            && chunk_view != view_dependency_graph.entities.end()
            && storage_view < chunk_view
            && std::ranges::contains(
                chunk_view->dependency_identities,
                storage_view->stable_identity),
        "view dependency selected a co-located companion instead of its "
        "emitting declaration owner");
    contract_test_detail::require_failure(
        [] {
            collected_program ambiguous =
                contract_test_detail::view_dependency_program();
            collected_entity duplicate = ambiguous.entities[1];
            duplicate.name = "storage_duplicate";
            ambiguous.entities.push_back(std::move(duplicate));
            static_cast<void>(build_sql_graph(std::move(ambiguous)));
        },
        "ambiguous SQL dependency reference",
        "view dependency silently selected one of two emitting owners");
    contract_test_detail::require_failure(
        [] {
            collected_program ambiguous =
                contract_test_detail::view_dependency_program();
            collected_entity alternate = ambiguous.entities[2];
            alternate.name = "storage_alternate";
            alternate.cpp_qualified_name =
                "bioseq::alternate::catalog_views::storage";
            ambiguous.entities.push_back(std::move(alternate));
            ambiguous.entities[3].dependencies.front().expression =
                "storage";
            static_cast<void>(build_sql_graph(std::move(ambiguous)));
        },
        "ambiguous SQL dependency reference",
        "unqualified dependency discarded a distinct declaration group's "
        "companion");
    {
        collected_program companion_only;
        collected_entity companion =
            contract_test_detail::view_dependency_program().entities[2];
        companion.name = "storage_grant";
        companion.cpp_qualified_name =
            "bioseq::fixture::catalog_views::storage_grant";
        companion_only.entities.push_back(std::move(companion));
        collected_entity consumer =
            contract_test_detail::view_dependency_program().entities.back();
        consumer.dependencies.front().expression =
            "bioseq::fixture::catalog_views::storage_grant";
        companion_only.entities.push_back(std::move(consumer));
        const sql_graph companion_only_graph =
            build_sql_graph(std::move(companion_only));
        contract_test_detail::require(
            companion_only_graph.entities.size() == 2
                && std::ranges::contains(
                    companion_only_graph.entities.back()
                        .dependency_identities,
                    companion_only_graph.entities.front().stable_identity),
            "companion-only declaration dependency was discarded");
    }

    const sql_graph support_graph = build_sql_graph(
        contract_test_detail::support_dependency_program());
    contract_test_detail::require(
        support_graph.entities.size() == 2
            && support_graph.entities[0].source.name == "fixture_support"
            && support_graph.entities[1].source.name
                == "uses_fixture_support"
            && support_graph.entities[1].dependency_identities
                == std::vector<std::string>{
                    support_graph.entities[0].stable_identity}
            && support_graph.entities[1].planner_support_identity
                == support_graph.entities[0].stable_identity,
        "planner support function was not ordered before its consumer");
    const rendered_sql_artifact support_artifact =
        render_sql_graph(support_graph);
    contract_test_detail::require(
        support_artifact.statements.size() == 2
            && support_artifact.statements[0].entity_identity
                == support_graph.entities[0].stable_identity
            && support_artifact.statements[1].entity_identity
                == support_graph.entities[1].stable_identity
            && support_artifact.statements[1].text.contains(
                "SUPPORT fixture_support"),
        "rendered SQL did not preserve planner-support dependency order");
    collected_program fixed_support_program =
        contract_test_detail::support_dependency_program();
    fixed_support_program.entities.back().schema =
        "fixture_support_api";
    fixed_support_program.entities.push_back(
        contract_test_detail::fixture_schema("fixture_support_api"));
    const sql_graph fixed_support_graph =
        build_sql_graph(std::move(fixed_support_program));
    const rendered_sql_artifact fixed_support_artifact =
        render_sql_graph(fixed_support_graph);
    contract_test_detail::require(
        contract_test_detail::statement_position(
            fixed_support_artifact,
            "CREATE SCHEMA fixture_support_api")
                < contract_test_detail::statement_position(
                    fixed_support_artifact,
                    "FUNCTION fixture_support_api.fixture_support")
            && contract_test_detail::statement_position(
                fixed_support_artifact,
                "FUNCTION fixture_support_api.fixture_support")
                < contract_test_detail::statement_position(
                    fixed_support_artifact,
                    "FUNCTION fixture_api.uses_fixture_support")
            && fixed_support_artifact.sql.contains(
                "SUPPORT fixture_support_api.fixture_support"),
        "fixed-schema planner support provider was not rendered with "
        "its resolved qualified SQL identity");
    contract_test_detail::require_failure(
        [] {
            collected_program missing =
                contract_test_detail::support_dependency_program();
            missing.entities.pop_back();
            static_cast<void>(build_sql_graph(std::move(missing)));
        },
        "unresolved planner support function 'fixture_support(internal)'",
        "SQL graph accepted a missing planner support function");
    contract_test_detail::require_failure(
        [] {
            collected_program wrong_signature =
                contract_test_detail::support_dependency_program();
            wrong_signature.entities.back().parameters.front().sql_type =
                "integer";
            static_cast<void>(
                build_sql_graph(std::move(wrong_signature)));
        },
        "must have signature (internal) -> internal",
        "SQL graph accepted an invalid planner support signature");
    contract_test_detail::require_failure(
        [] {
            collected_program wrong_result =
                contract_test_detail::support_dependency_program();
            wrong_result.entities.back().result_sql_type = "boolean";
            static_cast<void>(
                build_sql_graph(std::move(wrong_result)));
        },
        "must have signature (internal) -> internal",
        "SQL graph accepted an invalid planner support result");
    contract_test_detail::require_failure(
        [] {
            collected_program ambiguous =
                contract_test_detail::support_dependency_program();
            ambiguous.entities.push_back(
                contract_test_detail::support_provider(
                    "fixture", "_alternate"));
            static_cast<void>(build_sql_graph(std::move(ambiguous)));
        },
        "ambiguous planner support function 'fixture_support(internal)'",
        "SQL graph accepted ambiguous planner support ownership");
    contract_test_detail::require_failure(
        [] {
            collected_program self;
            collected_entity function =
                contract_test_detail::support_provider();
            function.options.push_back(option_value{
                .name = "support",
                .expression = "\"fixture_support\"",
            });
            self.entities.push_back(std::move(function));
            static_cast<void>(build_sql_graph(std::move(self)));
        },
        "cannot use itself as planner support",
        "SQL graph accepted a self-referential planner support function");
    contract_test_detail::require_failure(
        [] {
            collected_program reciprocal;
            collected_entity first =
                contract_test_detail::support_provider();
            first.name = "fixture_support_a";
            first.cpp_qualified_name =
                "bioseq::fixture::fixture_support_a";
            first.options.push_back(option_value{
                .name = "support",
                .expression = "\"fixture_support_b\"",
            });
            collected_entity second =
                contract_test_detail::support_provider();
            second.name = "fixture_support_b";
            second.cpp_qualified_name =
                "bioseq::fixture::fixture_support_b";
            second.options.push_back(option_value{
                .name = "support",
                .expression = "\"fixture_support_a\"",
            });
            reciprocal.entities.push_back(std::move(first));
            reciprocal.entities.push_back(std::move(second));
            static_cast<void>(build_sql_graph(std::move(reciprocal)));
        },
        "unsupported SQL dependency cycle",
        "SQL graph accepted reciprocal planner support providers");

    collected_program partial_result_row;
    collected_entity partial_result_field =
        contract_test_detail::fixture_type_column(
            "bioseq::fixture::partial_result_row", "$extension");
    partial_result_field.source_module =
        "bioseq.postgres.fixture.sql_rows";
    partial_result_row.entities.push_back(
        std::move(partial_result_field));
    const sql_graph partial_result_graph =
        build_sql_graph(std::move(partial_result_row));
    contract_test_detail::require(
        partial_result_graph.entities.size() == 1
            && partial_result_graph.entities.front().source.kind
                == entity_kind::column,
        "partial SUPPORT graph discarded result-row field metadata");
    contract_test_detail::require_failure(
        [&partial_result_graph] {
            static_cast<void>(render_sql_graph(partial_result_graph));
        },
        "has no compatible natural parent",
        "final SQL renderer accepted an unregistered result-row field");
    contract_test_detail::require_failure(
        [] {
            collected_program orphan;
            orphan.entities.push_back(
                contract_test_detail::fixture_type_column(
                    "bioseq::fixture::ordinary_orphan", "$extension"));
            static_cast<void>(build_sql_graph(std::move(orphan)));
        },
        "must resolve one emitting table/composite owner",
        "SQL graph accepted an ordinary ownerless inline column");

    const sql_graph annotated_type_graph = build_sql_graph(
        contract_test_detail::annotated_type_program());
    const auto entity_position = [&annotated_type_graph](
                                     std::string_view name) {
        const auto position = std::ranges::find(
            annotated_type_graph.entities,
            name,
            [](const normalized_entity& entity) {
                return std::string_view{entity.source.name};
            });
        contract_test_detail::require(
            position != annotated_type_graph.entities.end(),
            "annotated-type fixture entity is absent");
        return static_cast<std::size_t>(
            std::distance(
                annotated_type_graph.entities.begin(), position));
    };
    contract_test_detail::require(
        entity_position("fixture_types")
                < entity_position("fixture_payload")
            && entity_position("fixture_payload")
                < entity_position("consume_payload")
            && entity_position("fixture_base")
                < entity_position("fixture_base_in")
            && entity_position("fixture_base")
                < entity_position("fixture_base_out")
            && entity_position("fixture_base")
                < entity_position("consume_base"),
        "annotated routine types did not create schema/type dependencies");
    const rendered_sql_artifact annotated_type_artifact =
        render_sql_graph(annotated_type_graph);
    const std::size_t type_schema =
        contract_test_detail::statement_position(
            annotated_type_artifact, "CREATE SCHEMA fixture_types");
    const std::size_t composite =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "CREATE TYPE fixture_types.fixture_payload AS (");
    const std::size_t composite_consumer =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "FUNCTION fixture_api.consume_payload(");
    contract_test_detail::require(
        annotated_type_artifact.statements[composite].text.contains(
            "linked fixture_types.z_fixture_callback_dependency"),
        "composite column did not render its normalized annotated SQL type");
    const std::size_t base_shell =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "CREATE TYPE fixture_types.fixture_base;");
    const std::size_t element_definition =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "CREATE TYPE fixture_types.renamed_element AS (");
    const std::size_t callback_dependency =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "CREATE TYPE fixture_types.z_fixture_callback_dependency AS (");
    const std::size_t callback_prerequisite =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "FUNCTION fixture_types.fixture_callback_prerequisite(");
    const std::size_t base_input =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "FUNCTION fixture_types.fixture_base_in(");
    const std::size_t base_output =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "FUNCTION fixture_types.fixture_base_out(");
    const std::size_t base_definition =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "CREATE TYPE fixture_types.fixture_base (");
    const std::size_t base_consumer =
        contract_test_detail::statement_position(
            annotated_type_artifact,
            "FUNCTION fixture_api.consume_base(");
    contract_test_detail::require(
        annotated_type_artifact.statements[base_definition].text.contains(
            "ELEMENT = fixture_types.renamed_element"),
        "base type did not render its resolved ELEMENT SQL identity");
    contract_test_detail::require(
        type_schema < composite && composite < composite_consumer
            && type_schema < callback_dependency
            && callback_dependency < composite
            && element_definition < base_shell
            && callback_dependency < base_shell
            && callback_prerequisite < base_shell
            && type_schema < base_shell && base_shell < base_input
            && base_input < base_output
            && base_output < base_definition
            && base_definition < base_consumer,
        "rendered annotated types violate schema/type or base-type lifecycle order");
    contract_test_detail::require(
        std::ranges::count_if(
            annotated_type_artifact.statements,
            [](const rendered_statement& statement) {
                return statement.text.contains(
                    "FUNCTION fixture_types.fixture_base_in(");
            }) == 1
            && std::ranges::count_if(
                annotated_type_artifact.statements,
                [](const rendered_statement& statement) {
                    return statement.text.contains(
                        "FUNCTION fixture_types.fixture_base_out(");
                }) == 1,
        "base-type callback functions were omitted or rendered twice");
    contract_test_detail::require_failure(
        [] {
            collected_program missing =
                contract_test_detail::annotated_type_program();
            missing.entities.front().parameters.front().sql_type =
                "missing_schema.fixture_payload[]";
            static_cast<void>(build_sql_graph(std::move(missing)));
        },
        "unresolved annotated SQL type reference",
        "SQL graph accepted a missing qualified annotated type");
    contract_test_detail::require_failure(
        [] {
            collected_program missing =
                contract_test_detail::annotated_type_program();
            missing.entities.front().parameters.front().sql_type =
                "bioseq::sql::type_of<bioseq::fixture::missing>";
            static_cast<void>(build_sql_graph(std::move(missing)));
        },
        "unresolved annotated SQL type reference",
        "SQL graph accepted a missing explicit type_of reference");
    contract_test_detail::require_failure(
        [] {
            collected_program ambiguous =
                contract_test_detail::annotated_type_program();
            ambiguous.entities.front().parameters.front().sql_type =
                "fixture_payload[]";
            ambiguous.entities.push_back(
                contract_test_detail::fixture_schema("alternate_types"));
            ambiguous.entities.push_back(
                contract_test_detail::fixture_type(
                    "fixture_payload",
                    "bioseq::fixture::alternate_payload",
                    "alternate_types"));
            static_cast<void>(build_sql_graph(std::move(ambiguous)));
        },
        "ambiguous annotated SQL type reference",
        "SQL graph accepted an ambiguous unqualified annotated type");
    contract_test_detail::require_failure(
        [] {
            collected_program cyclic =
                contract_test_detail::annotated_type_program();
            const auto prerequisite = std::ranges::find(
                cyclic.entities,
                "fixture_callback_prerequisite",
                &collected_entity::name);
            if (prerequisite == cyclic.entities.end()) {
                throw std::runtime_error{
                    "callback prerequisite fixture is absent"};
            }
            prerequisite->parameters.push_back(routine_parameter{
                .name = "value",
                .cpp_type = "void*",
                .sql_type = "fixture_types.fixture_base",
            });
            static_cast<void>(build_sql_graph(std::move(cyclic)));
        },
        "unsupported base-type callback dependency cycle",
        "SQL graph accepted a base-type callback prerequisite cycle");
    contract_test_detail::require_failure(
        [] {
            collected_program invalid =
                contract_test_detail::annotated_type_program();
            const auto output = std::ranges::find(
                invalid.entities,
                "fixture_base_out",
                &collected_entity::name);
            if (output == invalid.entities.end()) {
                throw std::runtime_error{"callback fixture is absent"};
            }
            output->result.sql_type = "boolean";
            output->result_sql_type = "boolean";
            static_cast<void>(build_sql_graph(std::move(invalid)));
        },
        "has an invalid type_output SQL signature",
        "SQL graph accepted an invalid base-type callback signature");
    contract_test_detail::require_failure(
        [] {
            collected_program invalid =
                contract_test_detail::annotated_type_program();
            const auto output = std::ranges::find(
                invalid.entities,
                "fixture_base_out",
                &collected_entity::name);
            if (output == invalid.entities.end()) {
                throw std::runtime_error{"callback fixture is absent"};
            }
            output->parameters.front().sql_type =
                "fixture_types.fixture_base[]";
            static_cast<void>(build_sql_graph(std::move(invalid)));
        },
        "has an invalid type_output SQL signature",
        "SQL graph accepted an array-shaped base-type callback owner");
    contract_test_detail::require_failure(
        [] {
            collected_program invalid =
                contract_test_detail::annotated_type_program();
            const auto input = std::ranges::find(
                invalid.entities,
                "fixture_base_in",
                &collected_entity::name);
            if (input == invalid.entities.end()) {
                throw std::runtime_error{"callback fixture is absent"};
            }
            input->result.sql_type =
                "setof fixture_types.fixture_base";
            input->result_sql_type =
                "setof fixture_types.fixture_base";
            static_cast<void>(build_sql_graph(std::move(invalid)));
        },
        "has an invalid type_input SQL signature",
        "SQL graph accepted a SETOF base-type callback owner");
    contract_test_detail::require_failure(
        [] {
            collected_program invalid =
                contract_test_detail::annotated_type_program();
            const auto output = std::ranges::find(
                invalid.entities,
                "fixture_base_out",
                &collected_entity::name);
            if (output == invalid.entities.end()) {
                throw std::runtime_error{"callback fixture is absent"};
            }
            output->parameters.front().sql_type =
                "fixture_types.fixture_base(7)";
            static_cast<void>(build_sql_graph(std::move(invalid)));
        },
        "has an invalid type_output SQL signature",
        "SQL graph accepted a typmod-shaped base-type callback owner");
    contract_test_detail::require_failure(
        [] {
            collected_program invalid =
                contract_test_detail::annotated_type_program();
            const auto role = std::ranges::find_if(
                invalid.entities,
                [](const collected_entity& entity) {
                    return entity.kind == entity_kind::type_role
                        && entity.annotation_name == "type_output";
                });
            if (role == invalid.entities.end()) {
                throw std::runtime_error{"callback role fixture is absent"};
            }
            role->options.front().expression =
                "bioseq::sql::type_of<::base_value>";
            static_cast<void>(build_sql_graph(std::move(invalid)));
        },
        "unresolved typed SQL declaration reference",
        "SQL graph suffix-matched an explicitly global callback owner");

    {
        collected_program variable =
            contract_test_detail::annotated_type_program();
        const auto base = std::ranges::find(
            variable.entities,
            "fixture_base",
            &collected_entity::name);
        if (base == variable.entities.end()) {
            throw std::runtime_error{"base-type fixture is absent"};
        }
        const auto length = std::ranges::find(
            base->options,
            "internal_length",
            &option_value::name);
        if (length == base->options.end()) {
            throw std::runtime_error{"base-type length fixture is absent"};
        }
        length->name = "variable_length";
        length->expression = "true";
        const rendered_sql_artifact artifact =
            render_sql_graph(build_sql_graph(std::move(variable)));
        const std::size_t definition =
            contract_test_detail::statement_position(
                artifact, "CREATE TYPE fixture_types.fixture_base (");
        contract_test_detail::require(
            artifact.statements[definition].text.contains(
                "INTERNALLENGTH = VARIABLE"),
            "boolean variable-length base type did not render VARIABLE");
    }
    contract_test_detail::require_failure(
        [] {
            collected_program conflicting =
                contract_test_detail::annotated_type_program();
            const auto base = std::ranges::find(
                conflicting.entities,
                "fixture_base",
                &collected_entity::name);
            if (base == conflicting.entities.end()) {
                throw std::runtime_error{"base-type fixture is absent"};
            }
            base->options.push_back(option_value{
                .name = "variable_length",
                .expression = "true",
            });
            static_cast<void>(render_sql_graph(
                build_sql_graph(std::move(conflicting))));
        },
        "cannot specify both .internal_length and .variable_length",
        "renderer accepted conflicting base-type length selectors");
    contract_test_detail::require_failure(
        [] {
            collected_program incomplete;
            incomplete.entities.push_back(
                contract_test_detail::fixture_schema("fixture_types"));
            collected_entity type = contract_test_detail::fixture_type(
                "incomplete_base",
                "bioseq::fixture::incomplete_base",
                "fixture_types");
            type.options.push_back(option_value{
                .name = "alignment",
                .expression = "bioseq::sql::type_alignment::int4",
            });
            incomplete.entities.push_back(std::move(type));
            static_cast<void>(render_sql_graph(
                build_sql_graph(std::move(incomplete))));
        },
        "requires .internal_length or .variable_length",
        "renderer silently treated an incomplete base type as composite");
    contract_test_detail::require_failure(
        [] {
            collected_program non_type =
                contract_test_detail::annotated_type_program();
            const auto column = std::ranges::find_if(
                non_type.entities,
                [](const collected_entity& entity) {
                    return entity.kind == entity_kind::column
                        && entity.enclosing_cpp_name
                            == "bioseq::fixture::payload"
                        && entity.name == "value";
                });
            if (column == non_type.entities.end()) {
                throw std::runtime_error{"composite column fixture is absent"};
            }
            column->options = {{
                .name = "sql_type",
                .expression =
                    "bioseq::sql::type_of<"
                    "bioseq::fixture::fixture_callback_prerequisite>",
            }};
            static_cast<void>(build_sql_graph(std::move(non_type)));
        },
        "unresolved bioseq::sql::type_of reference",
        "generic type_of bound a non-type SQL declaration");
    contract_test_detail::require_failure(
        [] {
            collected_program self =
                contract_test_detail::annotated_type_program();
            const auto column = std::ranges::find_if(
                self.entities,
                [](const collected_entity& entity) {
                    return entity.kind == entity_kind::column
                        && entity.enclosing_cpp_name
                            == "bioseq::fixture::payload"
                        && entity.name == "value";
                });
            if (column == self.entities.end()) {
                throw std::runtime_error{"composite column fixture is absent"};
            }
            column->options = {{
                .name = "sql_type",
                .expression =
                    "bioseq::sql::type_of<bioseq::fixture::payload>",
            }};
            static_cast<void>(build_sql_graph(std::move(self)));
        },
        "unsupported inline SQL type self-dependency",
        "SQL graph erased a composite's inline self-type dependency");
    contract_test_detail::require_failure(
        [] {
            collected_program self =
                contract_test_detail::annotated_type_program();
            const auto base = std::ranges::find(
                self.entities,
                "fixture_base",
                &collected_entity::name);
            if (base == self.entities.end()) {
                throw std::runtime_error{"base-type fixture is absent"};
            }
            const auto element = std::ranges::find(
                base->options,
                "element",
                &option_value::name);
            if (element == base->options.end()) {
                throw std::runtime_error{"ELEMENT fixture is absent"};
            }
            element->expression =
                "bioseq::sql::type_of<bioseq::fixture::base_value>";
            static_cast<void>(build_sql_graph(std::move(self)));
        },
        "cannot reference itself through .element",
        "SQL graph erased a base type's ELEMENT self-dependency");

    const sql_graph inline_constraint_graph = build_sql_graph(
        contract_test_detail::inline_constraint_program());
    const rendered_sql_artifact inline_constraint_artifact =
        render_sql_graph(inline_constraint_graph);
    const std::size_t second_table =
        contract_test_detail::statement_position(
            inline_constraint_artifact,
            "CREATE TABLE fixture_types.second_table");
    const std::size_t first_table =
        contract_test_detail::statement_position(
            inline_constraint_artifact,
            "CREATE TABLE fixture_types.first_table");
    constexpr std::string_view inline_check_sql{
        "CONSTRAINT first_check CHECK (value > 0)"};
    const std::size_t inline_check =
        inline_constraint_artifact.sql.find(inline_check_sql);
    contract_test_detail::require(
        second_table < first_table
            && inline_constraint_artifact.statements[first_table]
                .text.contains(inline_check_sql)
            && inline_check != std::string::npos
            && inline_constraint_artifact.sql.find(
                inline_check_sql, inline_check + inline_check_sql.size())
                == std::string::npos,
        "inline constraint dependencies were not hoisted to their table");
    contract_test_detail::require_failure(
        [] {
            static_cast<void>(build_sql_graph(
                contract_test_detail::inline_constraint_program(true)));
        },
        "unsupported SQL dependency cycle",
        "SQL graph accepted reciprocal inline constraint dependencies");

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
