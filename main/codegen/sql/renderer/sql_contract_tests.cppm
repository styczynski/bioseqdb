module;

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:sql_contract_tests;

import :model;
import :sql;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

namespace sql_contract_test_detail {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{
            "SQL renderer contract failed: " + std::string{message}};
    }
}

inline void require_equal(
    std::string_view actual,
    std::string_view expected,
    std::string_view message) {
    if (actual == expected) return;
    throw std::runtime_error{
        "SQL renderer contract failed: " + std::string{message}
        + "\nexpected:\n" + std::string{expected}
        + "\nactual:\n" + std::string{actual}};
}

inline void require_failure(
    auto operation,
    std::string_view expected) {
    try {
        operation();
    } catch (const std::exception& error) {
        if (std::string_view{error.what()}.contains(expected)) {
            return;
        }
        throw std::runtime_error{
            "SQL renderer contract received unexpected diagnostic: "
            + std::string{error.what()}};
    }
    throw std::runtime_error{
        "SQL renderer contract expected failure containing: "
        + std::string{expected}};
}

}  // namespace sql_contract_test_detail

/**
 * Exercise compatibility-comment removal without changing quoted SQL text.
 */
inline void run_sql_renderer_contract_tests() {
    using sql_contract_test_detail::require;
    using sql_contract_test_detail::require_equal;
    using sql_contract_test_detail::require_failure;

    constexpr std::string_view compatibility_sql = R"SQL(
-- Removed compatibility narrative.
SELECT 'it''s -- preserved' AS literal_value,
       E'escaped quote: \' -- preserved' AS escaped_value,
       "quoted--identifier" AS quoted_name; -- Removed inline note.
SELECT $body$
BEGIN
    -- Preserved inside the tagged body.
    RETURN 'body -- value';
END
$body$;
SELECT $$-- Preserved inside the anonymous body.
SELECT 1;
$$;
SELECT 1 /* block /* nested -- preserved */ comment */ + 2;
-- Removed trailing narrative.
)SQL";
    constexpr std::string_view expected_sql = R"SQL(SELECT 'it''s -- preserved' AS literal_value,
       E'escaped quote: \' -- preserved' AS escaped_value,
       "quoted--identifier" AS quoted_name;
SELECT $body$
BEGIN
    -- Preserved inside the tagged body.
    RETURN 'body -- value';
END
$body$;
SELECT $$-- Preserved inside the anonymous body.
SELECT 1;
$$;
SELECT 1 /* block /* nested -- preserved */ comment */ + 2;)SQL";
    require_equal(
        detail::sanitize_compatibility_sql(compatibility_sql),
        expected_sql,
        "normal-state comments are removed and quoted comments survive");

    require_equal(
        detail::sanitize_compatibility_sql(
            "-- removed\r\nSELECT 1 -- removed\r\n    + 2;\r\n"),
        "SELECT 1\n    + 2;",
        "CRLF comments and trailing whitespace are normalized");

    sql_graph graph;
    graph.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::install_action,
            .annotation_name = "install_action",
            .name = "first_fixture",
            .compatibility_sql_initializer =
                R"CPP(bioseq::sql::checked_sql{R"SQL(
-- Removed before the first statement.
SELECT '-- first literal' AS marker; -- Removed after the statement.
)SQL"})CPP",
        },
        .stable_identity = "sql:install-action:first_fixture",
    });
    graph.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::install_action,
            .annotation_name = "install_action",
            .name = "second_fixture",
            .compatibility_sql_initializer =
                R"CPP(bioseq::sql::checked_sql{R"SQL(
-- Removed before the second statement.
SELECT $$-- second body$$;
)SQL"})CPP",
        },
        .stable_identity = "sql:install-action:second_fixture",
    });

    const rendered_sql_artifact artifact = render_sql_graph(graph);
    constexpr std::string_view expected_artifact =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n"
        "\n"
        "SELECT '-- first literal' AS marker;\n"
        "\n"
        "SELECT $$-- second body$$;\n";
    require_equal(
        artifact.sql,
        expected_artifact,
        "the psql guard survives while compatibility comments are removed");
    require(
        artifact.statements.size() == 2
            && artifact.statements[0].text
                == "SELECT '-- first literal' AS marker;"
            && artifact.statements[0].render_source
                == physical_render_source::compatibility_sql
            && artifact.statements[1].text
                == "SELECT $$-- second body$$;",
        "sanitized compatibility statements retain stable statement records");

    const auto natural_binding = [](
        std::string physical_id,
        entity_kind kind,
        physical_statement_facet facet =
            physical_statement_facet::primary) {
        const std::size_t ordinal =
            parse_baseline_physical_ordinal(physical_id);
        return physical_statement_binding{
            .kind = kind,
            .facet = facet,
            .render_source =
                physical_render_source::natural_projection,
            .baseline_physical_id = std::move(physical_id),
            .baseline_ordinal = ordinal,
        };
    };
    const auto baseline = [](
        std::string physical_id,
        entity_kind kind) {
        const std::size_t ordinal =
            parse_baseline_physical_ordinal(physical_id);
        return baseline_physical_statement{
            .baseline_physical_id = std::move(physical_id),
            .baseline_ordinal = ordinal,
            .kind = kind,
        };
    };
    const auto sequence = [](
        std::string name,
        std::vector<physical_statement_binding> bindings,
        std::string brief = {}) {
        const std::string identity =
            "sql:sequence:public." + name;
        const std::string cpp_qualified_name =
            "fixture::" + name;
        return normalized_entity{
            .source = collected_entity{
                .kind = entity_kind::sequence,
                .annotation_name = "sequence",
                .name = std::move(name),
                .cpp_qualified_name = cpp_qualified_name,
                .source_module =
                    "fixture.sql_renderer_contracts",
                .schema = "public",
                .cpp_return_type = "std::int64_t",
                .initializer_expression = "1",
                .documentation = {
                    .has_explicit_brief = !brief.empty(),
                    .brief = std::move(brief),
                },
                .physical_statements = std::move(bindings),
            },
            .stable_identity = identity,
            .installed_identity = identity,
        };
    };

    sql_graph interleaved;
    interleaved.entities.push_back(sequence(
        "first",
        {
            natural_binding("P001", entity_kind::sequence),
            natural_binding(
                "P004",
                entity_kind::comment,
                physical_statement_facet::catalog_comment),
        },
        "First sequence."));
    interleaved.entities.push_back(sequence(
        "second",
        {natural_binding("P002", entity_kind::sequence)}));
    interleaved.entities.push_back(sequence(
        "third",
        {natural_binding("P003", entity_kind::sequence)}));
    interleaved.expected_baseline_physical_statements = {
        baseline("P001", entity_kind::sequence),
        baseline("P002", entity_kind::sequence),
        baseline("P003", entity_kind::sequence),
        baseline("P004", entity_kind::comment),
    };
    const rendered_sql_artifact interleaved_artifact =
        render_sql_graph(interleaved);
    require(
        interleaved_artifact.statements.size() == 4
            && interleaved_artifact.statements[0]
                   .baseline_physical_id
                == "P001"
            && interleaved_artifact.statements[1]
                   .baseline_physical_id
                == "P002"
            && interleaved_artifact.statements[2]
                   .baseline_physical_id
                == "P003"
            && interleaved_artifact.statements[3]
                   .baseline_physical_id
                == "P004"
            && interleaved_artifact.statements[3].facet
                == physical_statement_facet::catalog_comment
            && interleaved_artifact.statements[3].text
                == "COMMENT ON SEQUENCE public.first IS "
                   "'First sequence.';",
        "numeric P ordering supports interleaved physical ownership");

    sql_graph base_type;
    const std::string type_identity = "sql:type:public.fixture_type";
    const std::string input_identity =
        "sql:function:public.fixture_type_in(cstring)";
    const std::string output_identity =
        "sql:function:public.fixture_type_out(public.fixture_type)";
    base_type.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::type,
            .annotation_name = "type",
            .name = "fixture_type",
            .cpp_qualified_name = "fixture::fixture_type",
            .schema = "public",
            .options = {
                option_value{
                    .name = "internal_length",
                    .expression = "8",
                },
            },
            .physical_statements = {
                natural_binding(
                    "P001",
                    entity_kind::type,
                    physical_statement_facet::base_type_shell),
                natural_binding(
                    "P004",
                    entity_kind::type,
                    physical_statement_facet::base_type_definition),
            },
        },
        .stable_identity = type_identity,
        .installed_identity = type_identity,
    });
    base_type.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = "fixture_type_in",
            .cpp_qualified_name = "fixture::fixture_type_in",
            .schema = "public",
            .result_sql_type = "public.fixture_type",
            .physical_statements = {
                natural_binding("P002", entity_kind::function),
            },
        },
        .stable_identity = input_identity,
        .installed_identity = input_identity,
    });
    base_type.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::type_role,
            .annotation_name = "type_input",
            .name = "fixture_type_in",
            .cpp_qualified_name = "fixture::fixture_type_in",
            .schema = "public",
            .companion = true,
        },
        .stable_identity = "sql:type-role:fixture_type_in",
        .installed_identity = "sql:type-role:fixture_type_in",
        .dependency_identities = {
            input_identity,
            type_identity,
        },
    });
    base_type.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = "fixture_type_out",
            .cpp_qualified_name = "fixture::fixture_type_out",
            .schema = "public",
            .result_sql_type = "cstring",
            .physical_statements = {
                natural_binding("P003", entity_kind::function),
            },
        },
        .stable_identity = output_identity,
        .installed_identity = output_identity,
    });
    base_type.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::type_role,
            .annotation_name = "type_output",
            .name = "fixture_type_out",
            .cpp_qualified_name = "fixture::fixture_type_out",
            .schema = "public",
            .companion = true,
        },
        .stable_identity = "sql:type-role:fixture_type_out",
        .installed_identity = "sql:type-role:fixture_type_out",
        .dependency_identities = {
            output_identity,
            type_identity,
        },
    });
    base_type.expected_baseline_physical_statements = {
        baseline("P001", entity_kind::type),
        baseline("P002", entity_kind::function),
        baseline("P003", entity_kind::function),
        baseline("P004", entity_kind::type),
    };
    const rendered_sql_artifact base_type_artifact =
        render_sql_graph(base_type);
    require(
        base_type_artifact.statements.size() == 4
            && base_type_artifact.statements[0]
                   .baseline_physical_id
                == "P001"
            && base_type_artifact.statements[1]
                   .baseline_physical_id
                == "P002"
            && base_type_artifact.statements[2]
                   .baseline_physical_id
                == "P003"
            && base_type_artifact.statements[3]
                   .baseline_physical_id
                == "P004"
            && std::ranges::count(
                   base_type_artifact.statements,
                   physical_statement_facet::base_type_shell,
                   &rendered_statement::facet)
                == 1
            && std::ranges::count(
                   base_type_artifact.statements,
                   physical_statement_facet::base_type_definition,
                   &rendered_statement::facet)
                == 1
            && std::ranges::count(
                   base_type_artifact.statements,
                   entity_kind::function,
                   &rendered_statement::statement_kind)
                == 2
            && base_type_artifact.sql.contains(
                "CREATE TYPE public.fixture_type;")
            && base_type_artifact.sql.contains(
                "CREATE TYPE public.fixture_type (")
            && base_type_artifact.sql.contains(
                "INPUT = public.fixture_type_in")
            && base_type_artifact.sql.contains(
                "OUTPUT = public.fixture_type_out"),
        "base-type shell, callbacks, and definition are separate "
        "physical statements");

    sql_graph composite_type;
    const std::string composite_identity =
        "sql:type:public.renderer_composite";
    composite_type.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::type,
            .annotation_name = "type",
            .name = "renderer_composite",
            .cpp_qualified_name = "fixture::renderer_composite",
            .schema = "public",
            .physical_statements = {
                natural_binding("P001", entity_kind::type),
            },
        },
        .stable_identity = composite_identity,
        .installed_identity = composite_identity,
    });
    const auto composite_column = [&composite_identity](
                                      std::string name,
                                      std::string cpp_type,
                                      std::size_t byte_offset) {
        return normalized_entity{
            .source = collected_entity{
                .kind = entity_kind::column,
                .annotation_name = "column",
                .name = std::move(name),
                .cpp_qualified_name =
                    "fixture::renderer_composite",
                .enclosing_cpp_name =
                    "fixture::renderer_composite",
                .source_range = {
                    .begin = {.byte_offset = byte_offset},
                    .end = {.byte_offset = byte_offset + 1},
                },
                .schema = "public",
                .cpp_return_type = std::move(cpp_type),
                .companion = true,
            },
            .stable_identity =
                "sql:column:public.renderer_composite:"
                + std::to_string(byte_offset),
            .installed_identity =
                "sql:column:public.renderer_composite:"
                + std::to_string(byte_offset),
            .dependency_identities = {composite_identity},
        };
    };
    composite_type.entities.push_back(
        composite_column("enabled", "bool", 30));
    composite_type.entities.push_back(
        composite_column("score", "double", 20));
    composite_type.entities.push_back(
        composite_column(
            "limit", "std::optional<std::int32_t>", 10));
    composite_type.expected_baseline_physical_statements = {
        baseline("P001", entity_kind::type),
    };
    const rendered_sql_artifact composite_type_artifact =
        render_sql_graph(composite_type);
    require_equal(
        composite_type_artifact.statements.front().text,
        "CREATE TYPE public.renderer_composite AS (\n"
        "    limit integer,\n"
        "    score double precision,\n"
        "    enabled boolean\n"
        ");",
        "composite fields preserve C++ declaration order and inferred "
        "SQL types");

    sql_graph natural_table;
    const std::string table_identity =
        "sql:table:public.renderer_table";
    natural_table.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::table,
            .annotation_name = "table",
            .name = "renderer_table",
            .cpp_qualified_name = "fixture::renderer_table",
            .schema = "public",
            .physical_statements = {
                natural_binding("P001", entity_kind::table),
            },
        },
        .stable_identity = table_identity,
        .installed_identity = table_identity,
    });
    const auto table_column = [&table_identity](
                                  std::string name,
                                  std::string cpp_type,
                                  std::size_t byte_offset) {
        return normalized_entity{
            .source = collected_entity{
                .kind = entity_kind::column,
                .annotation_name = "column",
                .name = std::move(name),
                .cpp_qualified_name = "fixture::renderer_table",
                .enclosing_cpp_name = "fixture::renderer_table",
                .source_range = {
                    .begin = {.byte_offset = byte_offset},
                    .end = {.byte_offset = byte_offset + 1},
                },
                .schema = "public",
                .cpp_return_type = std::move(cpp_type),
                .companion = true,
            },
            .stable_identity =
                "sql:column:public.renderer_table:"
                + std::to_string(byte_offset),
            .installed_identity =
                "sql:column:public.renderer_table:"
                + std::to_string(byte_offset),
            .dependency_identities = {table_identity},
        };
    };
    natural_table.entities.push_back(
        table_column("enabled", "std::optional<bool>", 30));
    natural_table.entities.push_back(
        table_column("score", "double", 20));
    natural_table.entities.push_back(
        table_column("row_id", "std::int64_t", 10));
    natural_table.expected_baseline_physical_statements = {
        baseline("P001", entity_kind::table),
    };
    const rendered_sql_artifact natural_table_artifact =
        render_sql_graph(natural_table);
    require_equal(
        natural_table_artifact.statements.front().text,
        "CREATE TABLE public.renderer_table (\n"
        "    row_id bigint NOT NULL,\n"
        "    score double precision NOT NULL,\n"
        "    enabled boolean\n"
        ");",
        "table columns preserve C++ declaration order and inferred SQL "
        "types");

    const auto typmod_cast_graph = [natural_binding, baseline] {
        sql_graph result;
        const std::string function_identity =
            "sql:function:public.fixture_cast("
            "public.fixture_type,integer,boolean)";
        result.entities.push_back(normalized_entity{
            .source = collected_entity{
                .kind = entity_kind::function,
                .annotation_name = "function",
                .name = "fixture_cast",
                .cpp_qualified_name = "fixture::fixture_cast",
                .schema = "public",
                .parameters = {
                    routine_parameter{
                        .name = "value",
                        .sql_type = "public.fixture_type",
                    },
                    routine_parameter{
                        .name = "typmod",
                        .sql_type = "integer",
                    },
                    routine_parameter{
                        .name = "explicit",
                        .sql_type = "boolean",
                    },
                },
                .result_sql_type = "public.fixture_type",
            },
            .stable_identity = function_identity,
            .installed_identity = function_identity,
        });
        result.entities.push_back(normalized_entity{
            .source = collected_entity{
                .kind = entity_kind::cast,
                .annotation_name = "cast",
                .name = "fixture_cast",
                .cpp_qualified_name = "fixture::fixture_cast",
                .schema = "public",
                .options = {
                    option_value{
                        .name = "assignment",
                        .expression = "true",
                    },
                },
                .companion = true,
                .physical_statements = {
                    natural_binding("P001", entity_kind::cast),
                },
            },
            .stable_identity =
                "sql:cast:public.fixture_type->public.fixture_type",
            .installed_identity =
                "sql:cast:public.fixture_type->public.fixture_type",
            .dependency_identities = {function_identity},
        });
        result.expected_baseline_physical_statements = {
            baseline("P001", entity_kind::cast),
        };
        return result;
    };
    const sql_graph valid_typmod_cast = typmod_cast_graph();
    const rendered_sql_artifact typmod_cast_artifact =
        render_sql_graph(valid_typmod_cast);
    const auto rendered_typmod_cast =
        std::ranges::find(
            typmod_cast_artifact.statements,
            std::string{"P001"},
            &rendered_statement::baseline_physical_id);
    require(
        rendered_typmod_cast
                != typmod_cast_artifact.statements.end()
            && std::ranges::count(
                   typmod_cast_artifact.statements,
                   std::string{"P001"},
                   &rendered_statement::baseline_physical_id)
                == 1
            && rendered_typmod_cast->text
                == "CREATE CAST (public.fixture_type AS "
                   "public.fixture_type)\n"
                   "    WITH FUNCTION public.fixture_cast("
                   "public.fixture_type, integer, boolean)\n"
                   "    AS ASSIGNMENT;",
        "typmod casts retain their PostgreSQL three-argument function "
        "signature");

    sql_graph invalid_typmod_cast = typmod_cast_graph();
    invalid_typmod_cast.entities.front().source.parameters[1].sql_type =
        "bigint";
    require_failure(
        [&invalid_typmod_cast] {
            static_cast<void>(
                render_sql_graph(invalid_typmod_cast));
        },
        "second argument must be integer typmod");

    sql_graph operator_graph;
    const auto comparison_function = [](
        std::string name,
        std::string cpp_name) {
        const std::string identity =
            "sql:function:public." + name
            + "(public.fixture_type,public.fixture_type)";
        return normalized_entity{
            .source = collected_entity{
                .kind = entity_kind::function,
                .annotation_name = "function",
                .name = std::move(name),
                .cpp_qualified_name = std::move(cpp_name),
                .schema = "public",
                .parameters = {
                    routine_parameter{
                        .name = "left",
                        .sql_type = "public.fixture_type",
                    },
                    routine_parameter{
                        .name = "right",
                        .sql_type = "public.fixture_type",
                    },
                },
                .result_sql_type = "boolean",
            },
            .stable_identity = identity,
            .installed_identity = identity,
        };
    };
    operator_graph.entities.push_back(comparison_function(
        "fixture_eq", "fixture::fixture_eq"));
    operator_graph.entities.push_back(comparison_function(
        "fixture_ne", "fixture::fixture_ne"));
    const std::string eq_function_identity =
        operator_graph.entities[0].stable_identity;
    const std::string ne_function_identity =
        operator_graph.entities[1].stable_identity;
    operator_graph.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::operator_,
            .annotation_name = "operator_",
            .name = "=",
            .cpp_qualified_name = "fixture::fixture_eq",
            .schema = "public",
            .options = {
                option_value{
                    .name = "commutator",
                    .expression = "fixture::fixture_eq",
                },
                option_value{
                    .name = "negator",
                    .expression = "fixture::fixture_ne",
                },
                option_value{
                    .name = "restrict",
                    .expression =
                        "bioseq::sql::pg_catalog_reference::eqsel",
                },
                option_value{
                    .name = "join",
                    .expression =
                        "bioseq::sql::pg_catalog_reference::eqjoinsel",
                },
                option_value{
                    .name = "hashes",
                    .expression = "true",
                },
                option_value{
                    .name = "merges",
                    .expression = "true",
                },
            },
            .companion = true,
            .physical_statements = {
                natural_binding("P001", entity_kind::operator_),
            },
        },
        .stable_identity =
            "sql:operator:public.=(public.fixture_type,"
            "public.fixture_type)",
        .installed_identity =
            "sql:operator:public.=(public.fixture_type,"
            "public.fixture_type)",
        .dependency_identities = {
            eq_function_identity,
            ne_function_identity,
        },
    });
    operator_graph.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::operator_,
            .annotation_name = "operator_",
            .name = "<>",
            .cpp_qualified_name = "fixture::fixture_ne",
            .schema = "public",
            .companion = true,
        },
        .stable_identity =
            "sql:operator:public.<>(public.fixture_type,"
            "public.fixture_type)",
        .installed_identity =
            "sql:operator:public.<>(public.fixture_type,"
            "public.fixture_type)",
        .dependency_identities = {
            ne_function_identity,
        },
    });
    operator_graph.expected_baseline_physical_statements = {
        baseline("P001", entity_kind::operator_),
    };
    const rendered_sql_artifact operator_artifact =
        render_sql_graph(operator_graph);
    const auto rendered_operator =
        std::ranges::find(
            operator_artifact.statements,
            std::string{"P001"},
            &rendered_statement::baseline_physical_id);
    require(
        rendered_operator != operator_artifact.statements.end()
            && std::ranges::count(
                   operator_artifact.statements,
                   std::string{"P001"},
                   &rendered_statement::baseline_physical_id)
                == 1
            && rendered_operator->text
                == "CREATE OPERATOR public.= (\n"
                   "    FUNCTION = public.fixture_eq,\n"
                   "    LEFTARG = public.fixture_type,\n"
                   "    RIGHTARG = public.fixture_type,\n"
                   "    COMMUTATOR = OPERATOR(public.=),\n"
                   "    NEGATOR = OPERATOR(public.<>),\n"
                   "    RESTRICT = pg_catalog.eqsel,\n"
                   "    JOIN = pg_catalog.eqjoinsel,\n"
                   "    HASHES,\n"
                   "    MERGES\n"
                   ");",
        "operators retain commutator, negator, selectivity, hash, and "
        "merge metadata");

    sql_graph relocatable_operator_graph = operator_graph;
    relocatable_operator_graph.extension_schema = "alternate_schema";
    for (normalized_entity& entity :
         relocatable_operator_graph.entities) {
        entity.source.schema = "$extension";
        for (routine_parameter& parameter :
             entity.source.parameters) {
            if (parameter.sql_type == "public.fixture_type") {
                parameter.sql_type = "fixture_type";
            }
        }
        while (entity.stable_identity.contains("public.")) {
            entity.stable_identity.replace(
                entity.stable_identity.find("public."),
                std::string_view{"public."}.size(),
                "$extension.");
        }
        entity.installed_identity = entity.stable_identity;
        while (entity.installed_identity.contains("$extension")) {
            entity.installed_identity.replace(
                entity.installed_identity.find("$extension"),
                std::string_view{"$extension"}.size(),
                "alternate_schema");
        }
    }
    const std::string relocatable_eq_identity =
        relocatable_operator_graph.entities[0].stable_identity;
    const std::string relocatable_ne_identity =
        relocatable_operator_graph.entities[1].stable_identity;
    relocatable_operator_graph.entities[2].dependency_identities = {
        relocatable_eq_identity,
        relocatable_ne_identity,
    };
    relocatable_operator_graph.entities[3].dependency_identities = {
        relocatable_ne_identity,
    };
    const rendered_sql_artifact relocatable_operator_artifact =
        render_sql_graph(relocatable_operator_graph);
    const auto relocatable_operator = std::ranges::find(
        relocatable_operator_artifact.statements,
        std::string{"P001"},
        &rendered_statement::baseline_physical_id);
    require(
        relocatable_operator
                != relocatable_operator_artifact.statements.end()
            && relocatable_operator->text
                == "CREATE OPERATOR = (\n"
                   "    FUNCTION = fixture_eq,\n"
                   "    LEFTARG = fixture_type,\n"
                   "    RIGHTARG = fixture_type,\n"
                   "    COMMUTATOR = =,\n"
                   "    NEGATOR = <>,\n"
                   "    RESTRICT = pg_catalog.eqsel,\n"
                   "    JOIN = pg_catalog.eqjoinsel,\n"
                   "    HASHES,\n"
                   "    MERGES\n"
                   ");"
            && !relocatable_operator_artifact.sql.contains(
                "public.fixture_eq")
            && !relocatable_operator_artifact.sql.contains(
                "alternate_schema.fixture_eq"),
        "extension-owned operators and their references use the "
        "CREATE EXTENSION target schema");

    sql_graph relocatable_aggregate;
    relocatable_aggregate.extension_schema = "alternate_schema";
    relocatable_aggregate.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = "vtransition",
            .cpp_qualified_name = "fixture::vtransition",
            .source_module = "fixture.aggregate",
            .schema = "$extension",
            .parameters = {
                routine_parameter{
                    .name = "state",
                    .sql_type = "bigint",
                },
                routine_parameter{
                    .name = "value",
                    .sql_type = "integer",
                },
            },
            .result_sql_type = "bigint",
        },
        .stable_identity =
            "sql:function:$extension.vtransition(bigint,integer)",
        .installed_identity =
            "sql:function:alternate_schema.vtransition(bigint,integer)",
    });
    relocatable_aggregate.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::aggregate,
            .annotation_name = "aggregate",
            .name = "vtotal",
            .cpp_qualified_name = "fixture::vtotal",
            .source_module = "fixture.aggregate",
            .schema = "$extension",
            .options = {
                option_value{
                    .name = "transition",
                    .expression = "fixture::vtransition",
                },
            },
        },
        .stable_identity =
            "sql:aggregate:$extension.vtotal(integer)",
        .installed_identity =
            "sql:aggregate:alternate_schema.vtotal(integer)",
        .dependency_identities = {
            "sql:function:$extension.vtransition(bigint,integer)",
        },
    });
    const rendered_sql_artifact relocatable_aggregate_artifact =
        render_sql_graph(relocatable_aggregate);
    require(
        relocatable_aggregate_artifact.sql.contains(
            "CREATE FUNCTION vtransition(")
            && relocatable_aggregate_artifact.sql.contains(
                "CREATE AGGREGATE vtotal(integer)")
            && relocatable_aggregate_artifact.sql.contains(
                "SFUNC = vtransition")
            && !relocatable_aggregate_artifact.sql.contains(
                "public.vtransition")
            && !relocatable_aggregate_artifact.sql.contains(
                "alternate_schema.vtransition"),
        "extension-owned aggregate calls remain bound to the "
        "CREATE EXTENSION target schema");

    sql_graph multi_statement;
    multi_statement.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::install_action,
            .annotation_name = "install_action",
            .name = "multi_statement",
            .physical_statements = {
                physical_statement_binding{
                    .kind = entity_kind::install_action,
                    .render_source =
                        physical_render_source::compatibility_sql,
                    .baseline_physical_id = "P001",
                    .baseline_ordinal = 1,
                    .compatibility_sql_initializer =
                        "bioseq::sql::checked_sql{\"SELECT 1; "
                        "SELECT 2;\"}",
                },
            },
        },
        .stable_identity =
            "sql:install-action:multi_statement",
    });
    multi_statement.expected_baseline_physical_statements = {
        baseline("P001", entity_kind::install_action),
    };
    require_failure(
        [&multi_statement] {
            static_cast<void>(
                render_sql_graph(multi_statement));
        },
        "must render exactly one top-level SQL statement");
}

}  // namespace bioseq::codegen::sql
