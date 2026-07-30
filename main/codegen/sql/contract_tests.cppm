module;

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql:contract_tests;

import :baseline_renderer;
import :migration;
import :parser;
import :policy;
import bioseq.codegen.clang_structural;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;
import bioseq.codegen.sql.physical_ledger;
import bioseq.codegen.sql.renderer;
import bioseq.codegen.sql.signature;
import bioseq.codegen.support;

export namespace bioseq::codegen::sql {

inline void run_generator_contract_self_tests() {
    const auto require = [](bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error(
                "SQL generator self-test failed: " + std::string{message});
        }
    };
    const auto require_failure = [](auto operation, std::string_view expected) {
        try {
            operation();
        } catch (const std::exception& error) {
            if (std::string_view{error.what()}.contains(expected)) {
                return;
            }
            throw std::runtime_error(
                "SQL generator self-test received unexpected diagnostic: "
                + std::string{error.what()}
                + "; expected diagnostic containing: "
                + std::string{expected});
        }
        throw std::runtime_error(
            "SQL generator self-test expected failure containing: "
            + std::string{expected});
    };
    const auto require_equal = [](
        std::string_view actual,
        std::string_view expected,
        std::string_view message) {
        if (actual == expected) return;
        std::size_t difference = 0;
        while (difference < actual.size()
               && difference < expected.size()
               && actual[difference] == expected[difference]) {
            ++difference;
        }
        const auto byte_at = [difference](std::string_view value) {
            return difference < value.size()
                ? static_cast<unsigned int>(
                      static_cast<unsigned char>(value[difference]))
                : 256U;
        };
        throw std::runtime_error(
            "SQL generator self-test failed: " + std::string{message}
            + " at byte " + std::to_string(difference)
            + " (actual byte " + std::to_string(byte_at(actual))
            + ", expected byte " + std::to_string(byte_at(expected))
            + ", actual size " + std::to_string(actual.size())
            + ", expected size " + std::to_string(expected.size())
            + ")");
    };

    require(
        detail::manifest_repository_path(
            "/tmp/relocated/main/bioseq/backend/fixture.cppm",
            "fixture")
            == "main/bioseq/backend/fixture.cppm"
        && detail::manifest_repository_path(
            "fixture.cppm", "relative fixture")
            == "fixture.cppm",
        "repository manifest paths are relocation-stable");
    require_failure(
        [] {
            static_cast<void>(detail::manifest_repository_path(
                "/tmp/outside/fixture.cppm", "escaped fixture"));
        },
        "cannot be reduced to the reviewed main source tree");
    require_failure(
        [] {
            static_cast<void>(detail::manifest_repository_path(
                "main/bioseq/../codegen/fixture.cppm",
                "non-normalized fixture"));
        },
        "not an already-normalized relative path");

    sql_graph external_handler_graph;
    external_handler_graph.entities.push_back(normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::function,
            .name = "bioseq_external_handler",
            .cpp_qualified_name =
                "bioseq::external::sql_routines::"
                "bioseq_external_handler",
            .source_module = "bioseq.pg.external.relation",
            .source_path = "main/bioseq/backend/pg/external/relation.cppm",
            .schema = "bioseq",
            .result_sql_type = "table_am_handler",
        },
        .stable_identity =
            "sql:function:bioseq.bioseq_external_handler(internal)",
        .installed_identity =
            "sql:function:bioseq.bioseq_external_handler(internal)",
    });
    external_handler_graph.legacy_fmgr_macro_owners.push_back({
        .macro_name = "POSTGRES_FN",
        .symbol = "bioseq_external_handler",
        .source_module = "bioseq.pg.external.relation",
        .source_path =
            "main/bioseq/backend/pg/external/relation.cppm",
        .source_line = 1,
    });
    const detail::routine_inventory external_handler_inventory =
        detail::build_routine_inventory(external_handler_graph);
    require(
        external_handler_inventory.c_routine_identities.empty()
            && external_handler_inventory.link_symbols.empty()
            && external_handler_inventory.symbol_entries.empty(),
        "the reviewed external handler remains outside the generated "
        "main-extension fmgr inventory");
    require(
        render_fmgr_entrypoint_modules(external_handler_graph).empty(),
        "the reviewed external handler emits no generated fmgr module");
    require_failure(
        [external_handler_graph] {
            sql_graph impostor = external_handler_graph;
            impostor.entities.front().source.cpp_qualified_name =
                "fixture::bioseq_external_handler";
            static_cast<void>(
                detail::build_routine_inventory(impostor));
        },
        "unexpected natural SQL routine");
    const detail::fmgr_ownership_inventory external_handler_ownership =
        detail::build_fmgr_ownership_inventory(
            external_handler_graph,
            external_handler_inventory,
            {},
            false);
    require(
        external_handler_ownership.generated_wrapper_symbols.empty()
            && external_handler_ownership.legacy_macro_symbols.empty()
            && external_handler_ownership.external_legacy_macro_symbols
                == std::vector<std::string>{
                    "bioseq_external_handler"},
        "the reviewed external handler retains its one-symbol handwritten "
        "fmgr ownership partition");

    constexpr std::string_view derived_column_source = R"CPP(
export module fixture.derived_column.sql_rows;
namespace fixture {
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
})]] relation {
    std::optional<std::string> namespace_name;
};
}
)CPP";
    const sql_graph derived_column_graph = build_sql_graph(
        collect_source_text(
            "derived_column.cppm", derived_column_source));
    const sql_graph relocated_derived_column_graph = build_sql_graph(
        collect_source_text(
            "derived_column.cppm", derived_column_source),
        "relocated_extension");
    constexpr std::string_view sibling_context_source = R"CPP(
export module fixture.sibling_context.sql_rows;
namespace fixture {
struct [[bioseq_sql::table]]
cache_probe {
    std::int64_t generation_id;
};
}
)CPP";
    const sql_graph sibling_context_graph = build_sql_graph(
        collect_source_text(
            "sibling_context.cppm", sibling_context_source),
        "bioseq_cache",
        "bioseq_cache",
        "bioseq.cache.sql.generated",
        "bioseq.pg_sql_boundary");
    require(
        sibling_context_graph.extension_name == "bioseq_cache"
            && sibling_context_graph.extension_schema
                == "bioseq_cache"
            && std::ranges::any_of(
                sibling_context_graph.entities,
                [](const normalized_entity& entity) {
                    return entity.source.kind == entity_kind::table
                        && entity.source.schema == "$extension"
                        && entity.installed_identity
                            == "sql:table:bioseq_cache.cache_probe";
                }),
        "owner-derived sibling context binds unguarded annotations to "
        "the configured extension identity");
    const rendered_sql_artifact sibling_context_sql =
        render_sql_graph(sibling_context_graph);
    require(
        std::ranges::any_of(
            sibling_context_sql.statements,
            [](const rendered_statement& statement) {
                return statement.statement_kind
                    == entity_kind::table;
            }),
        "a natural sibling owner renders physical SQL without a "
        "migration-ledger binding");
    require_failure(
        [&sibling_context_graph] {
            static_cast<void>(
                render_physical_ledger(sibling_context_graph));
        },
        "cannot publish an empty physical ledger");
    const rendered_sql_artifact derived_column_sql =
        render_sql_graph(derived_column_graph);
    const rendered_sql_artifact relocated_derived_column_sql =
        render_sql_graph(relocated_derived_column_graph);
    require(
        derived_column_sql.sql.contains(
            "CREATE TABLE bioseq_external.relation"),
        "a fixed-schema natural row renders in its exact fixed schema");
    require(
        derived_column_sql.sql.contains("    namespace_name text"),
        "a natural row derives its SQL column name from the C++ member");
    const std::vector<rendered_row_codec_module> derived_column_codecs =
        render_row_codec_modules(derived_column_graph);
    require(
        derived_column_codecs.size() == 1,
        "a natural std::string column has one generated codec owner");
    const std::string& derived_text_codec =
        derived_column_codecs.front().content;
    require(
        derived_text_codec.contains(
                "static constexpr bool text_string = true")
            && derived_text_codec.contains(
                "std::optional<std::string>")
            && derived_text_codec.contains(
                "bioseq::pg_utils::encode_text_to_memory_context(")
            && derived_text_codec.contains(
                "bioseq::pg_utils::copy_text_datum_to_string(")
            && derived_text_codec.contains(
                "std::is_trivially_destructible_v<")
            && !derived_text_codec.contains(
                "cstring_to_text_with_len(")
            && !derived_text_codec.contains("DatumGetTextPP("),
        "a natural std::string column generates owning text encode, deform, "
        "and update contracts only through the audited PostgreSQL bridges");
    require_failure(
        [&derived_text_codec] {
            std::string mutation = derived_text_codec;
            const std::size_t bridge = mutation.find(
                "bioseq::pg_utils::encode_text_to_memory_context(");
            if (bridge == std::string::npos) {
                throw std::runtime_error{
                    "text encode bridge mutation target is absent"};
            }
            mutation.replace(
                bridge,
                std::string_view{
                    "bioseq::pg_utils::encode_text_to_memory_context("}
                    .size(),
                "cstring_to_text_with_len(");
            detail::validate_row_codec_text_lifetime_contract(
                mutation, 1);
        },
        "direct PostgreSQL text leaf");
    require_failure(
        [&derived_text_codec] {
            std::string mutation = derived_text_codec;
            const std::size_t bridge = mutation.find(
                "bioseq::pg_utils::copy_text_datum_to_string(");
            if (bridge == std::string::npos) {
                throw std::runtime_error{
                    "text deform bridge mutation target is absent"};
            }
            mutation.replace(
                bridge,
                std::string_view{
                    "bioseq::pg_utils::copy_text_datum_to_string("}
                    .size(),
                "DatumGetTextPP(");
            detail::validate_row_codec_text_lifetime_contract(
                mutation, 1);
        },
        "direct PostgreSQL text leaf");
    require_failure(
        [&derived_text_codec] {
            std::string mutation = derived_text_codec;
            const std::size_t bridge = mutation.find(
                "bioseq::pg_utils::encode_text_to_memory_context(");
            if (bridge == std::string::npos) {
                throw std::runtime_error{
                    "text ownership mutation target is absent"};
            }
            mutation.replace(
                bridge,
                std::string_view{
                    "bioseq::pg_utils::encode_text_to_memory_context("}
                    .size(),
                "bioseq::pg_utils::unbridged_text_encode(");
            detail::validate_row_codec_text_lifetime_contract(
                mutation, 1);
        },
        "incomplete encode/deform bridge projection");

    constexpr std::string_view reaper_text_source = R"CPP(
export module bioseq.storage.manager.sql_rows;
namespace bioseq::storage::catalog_rows {
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]] minimizer_generation_reaper_state {
    std::string last_canonical_filename;
};
}
)CPP";
    const sql_graph reaper_text_graph = build_sql_graph(
        collect_source_text(
            "minimizer_generation_reaper_state.cppm",
            reaper_text_source));
    const auto reaper_text_codecs =
        render_row_codec_modules(reaper_text_graph);
    require(
        reaper_text_codecs.size() == 1
            && reaper_text_codecs.front().module_name
                == "bioseq.storage.manager.sql_row_codecs"
            && reaper_text_codecs.front().content.contains(
                "minimizer_generation_reaper_state_codec")
            && reaper_text_codecs.front().content.contains(
                "minimizer_generation_reaper_state::"
                "last_canonical_filename> final")
            && reaper_text_codecs.front().content.contains(
                "bioseq::pg_utils::encode_text_to_memory_context(")
            && reaper_text_codecs.front().content.contains(
                "bioseq::pg_utils::copy_text_datum_to_string(")
            && !reaper_text_codecs.front().content.contains(
                "cstring_to_text_with_len(")
            && !reaper_text_codecs.front().content.contains(
                "DatumGetTextPP("),
        "the reaper cursor owning-text field emits the same bridge-only "
        "encode, update, and deform lifetime contract used for inline and "
        "external-toast Datums");
    require(
        derived_column_sql.sql == relocated_derived_column_sql.sql,
        "a fixed-schema natural row renders byte-identically under extension "
        "schema relocation");
    require(
        std::ranges::all_of(
            relocated_derived_column_graph.entities,
            [](const normalized_entity& entity) {
                return entity.stable_identity
                    == entity.installed_identity;
            }),
        "a fixed-schema natural row keeps identical stable and installed "
        "identities under extension schema relocation");

    const std::vector<normalized_entity> reference_candidates{
        normalized_entity{.source = collected_entity{
            .kind = entity_kind::function,
            .cpp_qualified_name = "fixture::alpha",
        }},
        normalized_entity{.source = collected_entity{
            .kind = entity_kind::function,
            .cpp_qualified_name = "fixture::beta",
        }},
        normalized_entity{.source = collected_entity{
            .kind = entity_kind::function,
            .cpp_qualified_name = "fixture::letter",
        }},
        normalized_entity{.source = collected_entity{
            .kind = entity_kind::function,
            .cpp_qualified_name =
                "fixture::letter::letter_lt",
        }},
    };
    const auto resolved_references =
        [&reference_candidates](std::string expression) {
            return detail::option_reference_candidates(
                reference_candidates,
                entity_kind::opclass,
                option_value{
                    .name = "functions",
                    .expression = std::move(expression),
                });
        };
    require(
        resolved_references(
            "references(fixture::alpha, beta)")
            == std::vector<std::size_t>{0, 1},
        "mixed qualified and unqualified references resolve "
        "independently");
    require(
        resolved_references(
            "fixture::letter::letter_lt")
            == std::vector<std::size_t>{3}
            && resolved_references("letter::letter_lt")
                == std::vector<std::size_t>{3}
            && resolved_references(
                   "::fixture::letter::letter_lt")
                == std::vector<std::size_t>{3}
            && resolved_references(
                   "other::letter::letter_lt")
                .empty(),
        "declaration references match whole qualified identifiers");

    constexpr std::array<std::string_view, 4>
        fmgr_owner_macros{
            "CXX_PG_FUNCTION_INFO_V1",
            "PG_FUNCTION_INFO_V1",
            "POSTGRES_FN",
            "E_POSTGRES_FN",
        };
    for (const std::string_view macro : fmgr_owner_macros) {
        const std::filesystem::path source_path{
            std::string{macro} + ".cppm"};
        const std::string source =
            "export module fixture.macro_only;\n"
            + std::string{macro} + "(fixture_macro);\n";
        clang_structural::translation_unit unit =
            clang_structural::scan_translation_unit(
                source_path, source);
        require(
            !unit.tokens.empty()
                && unit.declarations.empty()
                && !unit.source_sha256.empty(),
            "macro-only structural translation unit is scanned");
        collected_program macro_program;
        macro_program.translation_units.push_back(std::move(unit));
        const sql_graph macro_graph =
            build_sql_graph(std::move(macro_program));
        require(
            macro_graph.legacy_fmgr_macro_owners.size() == 1
                && macro_graph.legacy_fmgr_macro_owners.front().macro_name
                    == macro
                && macro_graph.legacy_fmgr_macro_owners.front().symbol
                    == "fixture_macro"
                && macro_graph.legacy_fmgr_macro_owners.front().source_module
                    == "fixture.macro_only"
                && macro_graph.legacy_fmgr_macro_owners.front().source_path
                    == source_path
                && macro_graph.legacy_fmgr_macro_owners.front().source_line
                    == 2,
            "macro-only fmgr owner retains deterministic provenance");
    }
    require_failure(
        [&require] {
            constexpr std::string_view malformed_source =
                "export module fixture.malformed_macro;\n"
                "POSTGRES_FN(fixture_macro, extra);\n";
            clang_structural::translation_unit unit =
                clang_structural::scan_translation_unit(
                    std::filesystem::path{"malformed_macro.cppm"},
                    malformed_source);
            require(
                !unit.tokens.empty(),
                "malformed macro-only translation unit is scanned");
            collected_program macro_program;
            macro_program.translation_units.push_back(std::move(unit));
            static_cast<void>(
                build_sql_graph(std::move(macro_program)));
        },
        "malformed fmgr ownership macro");

    {
        constexpr std::string_view alias_tail_source =
            "export module fixture.alias_tail;\n"
            "using sequence_alias [[bioseq_sql::type({"
            ".schema = bioseq::sql::fixed_schema<\"bioseq\">})]] "
            "= std::int64_t;\n";
        const clang_structural::translation_unit unit =
            clang_structural::scan_translation_unit(
                std::filesystem::path{"alias_tail.cppm"},
                alias_tail_source);
        require(
            unit.declarations.size() == 1
                && unit.declarations.front().kind
                    == clang_structural::declaration_kind::alias
                && unit.declarations.front().name == "sequence_alias"
                && unit.declarations.front().qualified_name
                    == "sequence_alias"
                && unit.declarations.front().attributes.size() == 1
                && unit.declarations.front().attributes.front().name
                    == "type",
            "alias-tail SQL attributes associate with the using declaration");
    }
    {
        constexpr std::string_view clang_ast_source = R"CPP(
export module fixture.clang_ast;
/// @brief One compiler-owned SQL row.
/// @remark The fields retain declaration order.
export struct [[bioseq_sql::table({
    .user_catalog_table = true,
})]] row {
    [[bioseq_sql::column({
        .sql_type = 1,
    })]]
    int first;
    long second;
};
#if defined(BIOSEQ_TEST_INDEXES)
export {
[[bioseq_sql::function]]
int test_only(int value);
}
#endif
struct settings final {
    settings() = delete;

    /** @brief Enables fixture diagnostic logging. */
    [[bioseq_sql::guc({
        .context = 1,
    })]]
    static inline bool logging = false;
};

[[bioseq_sql::function]]
/**
 * @brief Reports whether a public fixture value is present.
 * @ingroup bioseq_sql_public_api
 */
bool public_value(int value);
)CPP";
        const clang_structural::translation_unit active =
            clang_structural::scan_translation_unit_from_code(
                "clang_ast.cppm",
                clang_ast_source,
                {
                    "-std=c++20",
                    "-xc++",
                    "-fparse-all-comments",
                    "-DBIOSEQ_TEST_INDEXES=1",
                },
                "test_indexes");
        const auto row = std::ranges::find(
            active.declarations,
            std::string{"row"},
            &clang_structural::declaration_record::name);
        const auto test_only = std::ranges::find(
            active.declarations,
            std::string{"test_only"},
            &clang_structural::declaration_record::name);
        const auto logging = std::ranges::find(
            active.declarations,
            std::string{"logging"},
            &clang_structural::declaration_record::name);
        const auto public_value = std::ranges::find(
            active.declarations,
            std::string{"public_value"},
            &clang_structural::declaration_record::name);
        const auto aggregate_row = std::ranges::find(
            active.aggregate_records,
            std::string{"row"},
            &clang_structural::aggregate_record::name);
        require(
            active.schema_version
                == clang_structural::model_schema_version,
            "Clang AST collection retains its POD schema version");
        require(
            active.module_name == "fixture.clang_ast",
            "Clang AST collection retains the provided module name");
        require(
            active.compiler_profile == "test_indexes",
            "Clang AST collection retains the compiler profile");
        require(
            !active.clang_version.empty(),
            "Clang AST collection retains the compiler version");
        require(
            row != active.declarations.end(),
            "Clang AST collection retains the annotated record");
        require(
            !row->stable_usr.empty(),
            "Clang AST collection retains the record USR");
        require(
            row->exported,
            "Clang AST collection retains direct export state");
        require(
            row->attributes.size() == 1,
            "Clang AST collection binds exactly one record attribute");
        require(
            row->attributes.front().target_usr == row->stable_usr,
            "Clang AST attribute targets the record USR");
        require(
            row->attributes.front().range.begin.byte_offset
                < row->attributes.front().range.end.byte_offset,
            "Clang AST attribute retains its exact source range");
        require(
            row->attributes.front().argument_range.begin.byte_offset
                < row->attributes.front().argument_range.end.byte_offset,
            "Clang AST attribute retains its exact argument range");
        require(
            row->documentation.associated_usr == row->stable_usr,
            "Clang Doxygen node targets the record USR");
        require(
            row->documentation.has_explicit_brief,
            "Clang Doxygen node retains explicit brief presence");
        require(
            row->documentation.brief
                == "One compiler-owned SQL row.",
            "Clang Doxygen node retains exact brief text");
        require(
            logging != active.declarations.end()
                && !logging->stable_usr.empty()
                && logging->documentation.associated_usr
                    == logging->stable_usr
                && logging->documentation.has_explicit_brief
                && logging->documentation.brief
                    == "Enables fixture diagnostic logging.",
            "Clang Doxygen fallback associates documentation preceding a "
            "multiline SQL attribute with its declaration");
        require(
            public_value != active.declarations.end()
                && public_value->documentation.has_explicit_brief
                && public_value->documentation.brief
                    == "Reports whether a public fixture value is present."
                && public_value->documentation.groups
                    == std::vector<std::string>{
                        "bioseq_sql_public_api"},
            "Clang AST documentation merges raw @ingroup metadata "
            "without replacing the parsed brief");
        require(
            aggregate_row != active.aggregate_records.end()
                && aggregate_row->fields.size() == 2
                && aggregate_row->fields[0].name
                    == "first"
                && aggregate_row->fields[0].ordinal == 0
                && aggregate_row->fields[1].name
                    == "second"
                && aggregate_row->fields[1].ordinal == 1
                && !aggregate_row->fields[0].stable_usr.empty()
                && test_only != active.declarations.end()
                && test_only->active_condition.contains(
                    "defined(BIOSEQ_TEST_INDEXES)"),
            "Clang AST collection retains field order and active "
            "compiler condition");
        const clang_structural::translation_unit inactive =
            clang_structural::scan_translation_unit_from_code(
                "clang_ast.cppm",
                clang_ast_source,
                {
                    "-std=c++20",
                    "-xc++",
                    "-fparse-all-comments",
                },
                "production");
        require(
            std::ranges::none_of(
                inactive.declarations,
                [](const clang_structural::declaration_record& declaration) {
                    return declaration.name == "test_only";
                }),
            "inactive profile declarations and attributes are absent "
            "from the Clang AST");
        require_failure(
            [] {
                static_cast<void>(
                    clang_structural::scan_translation_unit_from_code(
                        "invalid_attribute_target.cppm",
                        "export module fixture.invalid_attribute_target;\n"
                        "[[bioseq_sql::function]];\n",
                        {"-std=c++20", "-xc++"}));
            },
            "Clang");
    }
    {
        constexpr std::string_view class_nttp_source = R"CPP(
export module fixture.class_nttp_usr;
export namespace fixture {
struct marker {
    int value;
    consteval bool operator==(const marker&) const = default;
};
inline constexpr marker first{1};
inline constexpr marker second{2};
template<auto Value, class Lane>
struct wrapper {};

[[bioseq_sql::function]]
void structural(
    [[bioseq_sql::param]]
    wrapper<first, int> input) {}

[[bioseq_sql::function]]
void integral(wrapper<1, int> input) {}

[[bioseq_sql::function]]
void overloaded(wrapper<first, int> input) {}
[[bioseq_sql::function]]
void overloaded(wrapper<second, int> input) {}
}
)CPP";
        const auto scan_class_nttp =
            [](std::string_view source, std::string_view profile) {
                return clang_structural::
                    scan_translation_unit_from_code(
                        "class_nttp_usr.cppm",
                        source,
                        {
                            "-std=c++20",
                            "-xc++",
                            "-Wno-unknown-attributes",
                        },
                        profile);
            };
        const clang_structural::translation_unit production =
            scan_class_nttp(class_nttp_source, "production");
        const auto structural = std::ranges::find(
            production.declarations,
            std::string{"structural"},
            &clang_structural::declaration_record::name);
        const auto integral = std::ranges::find(
            production.declarations,
            std::string{"integral"},
            &clang_structural::declaration_record::name);
        require(
            structural != production.declarations.end()
                && structural->stable_usr.starts_with(
                    "clang-fallback:function:"
                    "fixture.class_nttp_usr:")
                && structural->attributes.size() == 1
                && structural->attributes.front().target_usr
                    == structural->stable_usr
                && structural->parameters.size() == 1
                && structural->parameters.front().stable_usr
                    == structural->stable_usr + "#param:0"
                && structural->parameters.front().attributes.size()
                    == 1
                && structural->parameters.front()
                       .attributes.front().target_usr
                    == structural->parameters.front().stable_usr,
            "class-valued auto-NTTP functions and parameters use the "
            "compiler-owned canonical identity");
        require(
            integral != production.declarations.end()
                && !integral->stable_usr.empty()
                && !integral->stable_usr.starts_with(
                    "clang-fallback:function:"),
            "integral auto-NTTP functions retain the raw Clang USR");
        std::vector<std::string> overload_identities;
        for (const auto& declaration : production.declarations) {
            if (declaration.name == "overloaded") {
                overload_identities.push_back(
                    declaration.stable_usr);
            }
        }
        std::ranges::sort(overload_identities);
        require(
            overload_identities.size() == 2
                && !overload_identities[0].empty()
                && !overload_identities[1].empty()
                && overload_identities[0]
                    != overload_identities[1]
                && std::ranges::all_of(
                    overload_identities,
                    [](const std::string& identity) {
                        return identity.starts_with(
                            "clang-fallback:function:"
                            "fixture.class_nttp_usr:");
                    }),
            "distinct class structural values have distinct "
            "compiler-owned identities");
        const clang_structural::translation_unit test_indexes =
            scan_class_nttp(class_nttp_source, "test_indexes");
        const auto profile_structural = std::ranges::find(
            test_indexes.declarations,
            std::string{"structural"},
            &clang_structural::declaration_record::name);
        require(
            profile_structural
                    != test_indexes.declarations.end()
                && profile_structural->stable_usr
                    == structural->stable_usr
                && profile_structural->parameters.front().stable_usr
                    == structural->parameters.front().stable_usr,
            "compiler-owned identities are profile invariant");

        constexpr std::string_view renamed_parameter_source = R"CPP(
export module fixture.class_nttp_usr;
export namespace fixture {
struct marker {
    int value;
    consteval bool operator==(const marker&) const = default;
};
inline constexpr marker first{1};
template<auto Value, class Lane>
struct wrapper {};
[[bioseq_sql::function]]
void structural([[bioseq_sql::param]]
                wrapper<first, int> renamed) {}
}
)CPP";
        const clang_structural::translation_unit renamed =
            scan_class_nttp(
                renamed_parameter_source, "production");
        const auto renamed_structural = std::ranges::find(
            renamed.declarations,
            std::string{"structural"},
            &clang_structural::declaration_record::name);
        require(
            renamed_structural != renamed.declarations.end()
                && renamed_structural->stable_usr
                    == structural->stable_usr
                && renamed_structural->parameters.front().stable_usr
                    == structural->parameters.front().stable_usr,
            "whitespace and parameter renaming preserve "
            "compiler-owned identities");
    }
    {
        constexpr std::string_view adjacent_function_source = R"CPP(
export module fixture.adjacent_functions;
[[bioseq_sql::function]]
void first(std::optional<std::vector<int>> value, int count) {}
[[bioseq_sql::function]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]]
void second(std::optional<std::vector<int>> value, int count) {}
[[bioseq_sql::function]]
void shifted(
    std::integral_constant<int, (8 >> 1)> value,
    int count) {}
)CPP";
        const clang_structural::translation_unit unit =
            clang_structural::scan_translation_unit(
                std::filesystem::path{"adjacent_functions.cppm"},
                adjacent_function_source);
        require(
            unit.declarations.size() == 3
                && unit.declarations[0].name == "first"
                && unit.declarations[0].attributes.size() == 1
                && unit.declarations[0].parameters.size() == 2
                && unit.declarations[1].name == "second"
                && unit.declarations[1].attributes.size() == 2
                && unit.declarations[1].parameters.size() == 2
                && unit.declarations[2].name == "shifted"
                && unit.declarations[2].parameters.size() == 2,
            "template closers and shifts preserve function ownership "
            "and parameter boundaries");
    }
    {
        std::vector<clang_structural::translation_unit> units;
        units.push_back(clang_structural::scan_translation_unit(
            "fixture_type.cppm",
            R"CPP(
export module fixture.type;
export namespace fixture {
struct [[bioseq_sql::type({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]] seq {};
struct [[bioseq_sql::type({
    .schema = bioseq::sql::extension_schema,
})]] bioseq_aligner_options {};
}
)CPP"));
        units.push_back(clang_structural::scan_translation_unit(
            "fixture_type_function.cppm",
            R"CPP(
export module fixture.type_function;
namespace fixture {
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::type_of<fixture::seq>,
})]]
seq identity(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<fixture::seq>,
    })]]
    seq value);
[[bioseq_sql::function]]
bool mmsearch(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<fixture::seq>,
    })]]
    seq target,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<fixture::seq>,
    })]]
    seq query,
    [[bioseq_sql::param({
        .sql_type =
            bioseq::sql::type_of<fixture::bioseq_aligner_options>,
    })]]
    bioseq_aligner_options options);
}
)CPP"));
        const collected_program program =
            collect_translation_units(std::move(units));
        const auto routine = std::ranges::find(
            program.entities,
            std::string{"identity"},
            &collected_entity::name);
        require(
            routine != program.entities.end()
                && routine->parameters.size() == 1
                && routine->parameters.front().sql_type
                    == "bioseq.seq"
                && routine->result.sql_type == "bioseq.seq"
                && routine->result_sql_type == "bioseq.seq",
            "type_of resolves to the annotated schema-qualified SQL type");
        const auto mmsearch = std::ranges::find(
            program.entities,
            std::string{"mmsearch"},
            &collected_entity::name);
        require(
            mmsearch != program.entities.end()
                && mmsearch->parameters.size() == 3
                && mmsearch->parameters[2].sql_type
                    == "bioseq_aligner_options"
                && mmsearch->parameters[2].stable_identity_sql_type
                    == "$extension.bioseq_aligner_options"
                && detail::stable_identity(*mmsearch)
                    == "sql:function:$extension.mmsearch("
                       "bioseq.seq,bioseq.seq,"
                       "$extension.bioseq_aligner_options)"
                && detail::installed_identity(
                    detail::stable_identity(*mmsearch), "public")
                    == "sql:function:public.mmsearch("
                       "bioseq.seq,bioseq.seq,"
                       "public.bioseq_aligner_options)",
            "type_of preserves relocatable extension-owned routine "
            "argument identities");
    }
    {
        collected_program public_api = collect_source_text(
            "public_api.cppm",
            R"CPP(
export module fixture.public_api;
namespace fixture {
/**
 * @brief Biological sequence value.
 * @ingroup bioseq_sql_public_api
 */
struct [[bioseq_sql::type({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]] seq {};

/**
 * @brief Per-call aligner options.
 * @ingroup bioseq_sql_public_api
 */
struct [[bioseq_sql::type]] bioseq_aligner_options {};

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
/**
 * @brief Sequence length.
 * @ingroup bioseq_sql_public_api
 */
long len([[bioseq_sql::param({
    .sql_type = bioseq::sql::type_of<fixture::seq>,
})]] Datum input);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
/**
 * @brief Symbol count.
 * @ingroup bioseq_sql_public_api
 */
long symbol_count(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<fixture::seq>,
    })]] Datum input,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::text,
    })]] std::string symbol);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
/**
 * @brief Query start.
 * @ingroup bioseq_sql_public_api
 */
long query_start([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::anyelement,
})]] Datum value);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
/**
 * @brief Query end.
 * @ingroup bioseq_sql_public_api
 */
long query_end([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::anyelement,
})]] Datum value);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
/**
 * @brief Target start.
 * @ingroup bioseq_sql_public_api
 */
long target_start([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::anyelement,
})]] Datum value);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
/**
 * @brief Target end.
 * @ingroup bioseq_sql_public_api
 */
long target_end([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::anyelement,
})]] Datum value);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
/**
 * @brief Alignment CIGAR.
 * @ingroup bioseq_sql_public_api
 */
std::string cigar([[bioseq_sql::param({
    .sql_type = bioseq::sql::pg::anyelement,
})]] Datum value);

[[bioseq_sql::function]]
/**
 * @brief Similarity predicate.
 * @ingroup bioseq_sql_public_api
 */
bool mmsearch(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<fixture::seq>,
    })]] Datum target,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<fixture::seq>,
    })]] Datum query);

[[bioseq_sql::function]]
/**
 * @brief Configured similarity predicate.
 * @ingroup bioseq_sql_public_api
 */
bool mmsearch(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<fixture::seq>,
    })]] Datum target,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::type_of<fixture::seq>,
    })]] Datum query,
    [[bioseq_sql::param({
        .sql_type =
            bioseq::sql::type_of<fixture::bioseq_aligner_options>,
    })]] Datum options);
}
)CPP");
        const sql_graph public_graph =
            build_sql_graph(std::move(public_api), "alternate_schema");
        std::set<std::string, std::less<>> stable_public;
        std::set<std::string, std::less<>> installed_public;
        for (const normalized_entity& entity : public_graph.entities) {
            if (!entity.source.public_documentation) continue;
            stable_public.insert(entity.stable_identity);
            installed_public.insert(entity.installed_identity);
        }
        const std::set<std::string, std::less<>> expected_stable{
            "sql:type:bioseq.seq",
            "sql:function:bioseq.len(bioseq.seq)",
            "sql:function:bioseq.symbol_count(bioseq.seq,text)",
            "sql:function:bioseq.query_start(anyelement)",
            "sql:function:bioseq.query_end(anyelement)",
            "sql:function:bioseq.target_start(anyelement)",
            "sql:function:bioseq.target_end(anyelement)",
            "sql:function:bioseq.cigar(anyelement)",
            "sql:type:$extension.bioseq_aligner_options",
            "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)",
            "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq,"
                "$extension.bioseq_aligner_options)",
        };
        std::set<std::string, std::less<>> expected_installed =
            expected_stable;
        std::set<std::string, std::less<>> relocated_installed;
        for (std::string identity : expected_installed) {
            relocated_installed.insert(detail::installed_identity(
                std::move(identity), "alternate_schema"));
        }
        const rendered_sql_artifact public_sql =
            render_sql_graph(public_graph);
        const std::vector<rendered_fmgr_entrypoint_module>
            public_fmgr = render_fmgr_entrypoint_modules(public_graph);
        const std::string public_manifest = render_graph_manifest(
            public_graph, public_sql, public_fmgr);
        if (stable_public != expected_stable) {
            std::string difference =
                "SQL generator self-test failed: public API stable "
                "identity mismatch";
            for (const std::string& identity : expected_stable) {
                if (!stable_public.contains(identity)) {
                    difference += "\nmissing: " + identity;
                }
            }
            for (const std::string& identity : stable_public) {
                if (!expected_stable.contains(identity)) {
                    difference += "\nunexpected: " + identity;
                }
            }
            throw std::runtime_error(std::move(difference));
        }
        require(
            installed_public == relocated_installed,
            "public API collection relocates extension-owned installed "
            "identities");
        require(
            public_manifest.contains("\"public_entity_count\": 11")
                && public_manifest.contains(
                    "\"sql_identity\": \"mmsearch(bioseq.seq,"
                    "bioseq.seq,bioseq_aligner_options)\"")
                && !public_manifest.contains(
                    "\"sql_identity\": \"mmsearch(bioseq.seq,"
                    "bioseq.seq,$extension.bioseq_aligner_options)\""),
            "public API manifest records all eleven website identities "
            "with canonical runtime routine spellings");
        require(
            public_sql.sql.contains(
                "CREATE TYPE bioseq_aligner_options"),
            "extension-owned public composite type is emitted "
            "unqualified");
        require(
            public_sql.sql.contains("FUNCTION mmsearch(")
                && !public_sql.sql.contains(
                    "FUNCTION public.mmsearch(")
                && !public_sql.sql.contains(
                    "FUNCTION alternate_schema.mmsearch("),
            "extension-owned public functions are emitted unqualified");
    }

    const std::vector<option_value> shift_options = parse_options(
        clang_structural::attribute_record{
            .qualified_name = "bioseq_sql::guc",
            .argument_text =
                "{.minimum = 0, .maximum = 1 << 20, "
                ".context = bioseq::sql::guc_context::user_set}",
        });
    require(
        shift_options.size() == 3
            && shift_options[0].name == "minimum"
            && shift_options[0].expression == "0"
            && shift_options[1].name == "maximum"
            && shift_options[1].expression == "1 << 20"
            && shift_options[2].name == "context"
            && shift_options[2].expression
                == "bioseq::sql::guc_context::user_set",
        "shift operators do not consume the following designated option");

    const normalized_entity extension_schema_routine{
        .source = collected_entity{
            .kind = entity_kind::function,
            .name = "mmsearch",
            .schema = "$extension",
        },
        .stable_identity =
            "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)",
        .installed_identity =
            "sql:function:public.mmsearch(bioseq.seq,bioseq.seq)",
    };
    require_equal(
        detail::manifest_routine_identity(extension_schema_routine),
        "mmsearch(bioseq.seq,bioseq.seq)",
        "extension-schema routine ledger identities remain relocatable");
    normalized_entity extension_typed_routine =
        extension_schema_routine;
    extension_typed_routine.stable_identity =
        "sql:function:$extension.mmsearch("
        "bioseq.seq,bioseq.seq,$extension.bioseq_aligner_options)";
    extension_typed_routine.installed_identity =
        "sql:function:public.mmsearch("
        "bioseq.seq,bioseq.seq,public.bioseq_aligner_options)";
    require_equal(
        detail::manifest_routine_identity(extension_typed_routine),
        "mmsearch(bioseq.seq,bioseq.seq,bioseq_aligner_options)",
        "extension-owned routine argument identities remain relocatable");
    const normalized_entity escaped_typed_routine{
        .source = collected_entity{
            .kind = entity_kind::function,
            .name = "vanycount",
            .options = {
                option_value{
                    .name = "sql_identity",
                    .expression =
                        "\"vanycount(int8,\\\"any\\\")\"",
                },
            },
            .typed_install_owner = true,
        },
        .stable_identity = "sql:install-owner:statement_p092",
        .installed_identity = "sql:install-owner:statement_p092",
    };
    require_equal(
        detail::manifest_routine_identity(escaped_typed_routine),
        "vanycount(int8,\"any\")",
        "ordinary C++ string escapes are decoded in manifest options");

    {
        sql_graph transitional_policy_graph;
        transitional_policy_graph.entities.push_back({
            .source = collected_entity{
                .typed_install_owner = true,
            },
            .stable_identity = "sql:install-owner:fixture",
        });
        transitional_policy_graph.legacy_fmgr_macro_owners.push_back({
            .macro_name = "PG_FUNCTION_INFO_V1",
            .symbol = "fixture_main_legacy",
            .source_module = "fixture.policy",
            .source_path = "fixture_policy.cppm",
            .source_line = 1,
        });
        validate_codegen_completion_policy(
            transitional_policy_graph, false);

        sql_graph typed_owner_graph;
        typed_owner_graph.entities =
            transitional_policy_graph.entities;
        require_failure(
            [&typed_owner_graph] {
                validate_codegen_completion_policy(
                    typed_owner_graph, true);
            },
            "transitional install owners");

        sql_graph main_legacy_graph;
        main_legacy_graph.legacy_fmgr_macro_owners =
            transitional_policy_graph.legacy_fmgr_macro_owners;
        require_failure(
            [&main_legacy_graph] {
                validate_codegen_completion_policy(
                    main_legacy_graph, true);
            },
            "main-extension legacy fmgr macro owners");

        sql_graph compatibility_source_graph;
        compatibility_source_graph.entities.push_back({
            .source = collected_entity{
                .kind = entity_kind::table,
                .physical_statements = {
                    physical_statement_binding{
                        .kind = entity_kind::table,
                        .facet = physical_statement_facet::primary,
                        .render_source =
                            physical_render_source::compatibility_sql,
                        .baseline_physical_id = "P382",
                        .baseline_ordinal = 382,
                    },
                },
            },
            .stable_identity =
                "sql:table:bioseq.compatibility_fixture",
        });
        require_failure(
            [&compatibility_source_graph] {
                validate_codegen_completion_policy(
                    compatibility_source_graph, true);
            },
            "compatibility source is 'P382'");

        sql_graph external_only_graph;
        for (const std::string_view symbol : {
                 "bioseq_external_handler",
             }) {
            external_only_graph.legacy_fmgr_macro_owners.push_back({
                .macro_name = "PG_FUNCTION_INFO_V1",
                .symbol = std::string{symbol},
                .source_module = "fixture.external",
                .source_path = "fixture_external.cppm",
                .source_line = 1,
            });
        }
        validate_codegen_completion_policy(external_only_graph, true);
    }

    constexpr std::string_view baseline =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n"
        "\n"
        "CREATE OR REPLACE FUNCTION bioseq.f(value int) RETURNS int "
        "AS $$ SELECT value $$ LANGUAGE SQL IMMUTABLE;\n"
        "CREATE OR REPLACE FUNCTION bioseq.f(value int) RETURNS int "
        "AS $$ SELECT value + 1 $$ LANGUAGE SQL IMMUTABLE;\n"
        "CREATE SCHEMA bioseq;\n";
    install_script parsed = parse_install_script(baseline);
    validate_install_script(parsed);
    require(parsed.statements.size() == 3, "physical statement count");
    require(
        parsed.statements[0].disposition
            == statement_disposition::
                superseded_by_later_create_or_replace,
        "last CREATE OR REPLACE wins in baseline migration");
    require(
        parsed.statements[1].routine_identity
            == std::optional<std::string>{"bioseq.f(int)"},
        "routine identity drops input names");
    {
        const std::vector<parsed_routine_argument> arguments =
            parse_routine_argument_declarations(
                "IN name text, INOUT running bigint DEFAULT 0, "
                "OUT output_name name, VARIADIC values text[]");
        require(
            arguments.size() == 4
                && arguments[0].mode
                    == routine_argument_mode::in
                && arguments[0].name == "name"
                && arguments[0].sql_type == "text"
                && arguments[1].mode
                    == routine_argument_mode::inout
                && arguments[1].input_bearing()
                && arguments[1].output_bearing()
                && arguments[1].has_default
                && arguments[1].default_expression == "0"
                && arguments[2].mode
                    == routine_argument_mode::out
                && !arguments[2].input_bearing()
                && arguments[2].sql_type == "name"
                && arguments[3].mode
                    == routine_argument_mode::variadic
                && arguments[3].sql_type == "text[]",
            "shared routine signature parser preserves modes, defaults, "
            "names, and SQL types");
        const std::vector<parsed_routine_argument> table_columns =
            parse_returns_table_columns(
                "TABLE (name text, total double precision)");
        require(
            table_columns.size() == 2
                && table_columns[0].name == "name"
                && table_columns[0].sql_type == "text"
                && table_columns[0].output_bearing()
                && table_columns[1].name == "total"
                && table_columns[1].sql_type
                    == "double precision",
            "RETURNS TABLE uses the shared name/type parser");
    }
    constexpr std::string_view signature_baseline =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n"
        "CREATE FUNCTION bioseq.named_text(name text, "
        "OUT output_name name) RETURNS record "
        "AS 'MODULE_PATHNAME', 'named_text' LANGUAGE C;\n"
        "CREATE FUNCTION bioseq.type_only(name) RETURNS name "
        "AS 'MODULE_PATHNAME', 'type_only' LANGUAGE C;\n"
        "CREATE FUNCTION bioseq.modes(IN lhs app.custom_type, "
        "INOUT running app.state_type, OUT label app.output_type, "
        "VARIADIC rest pg_catalog.text[]) RETURNS record "
        "AS 'MODULE_PATHNAME', 'modes' LANGUAGE C;\n"
        "CREATE FUNCTION bioseq.defaulted("
        "payload app.custom_type[] "
        "DEFAULT ARRAY[]::app.custom_type[]) RETURNS app.custom_type[] "
        "AS 'MODULE_PATHNAME', 'defaulted' LANGUAGE C;\n"
        "CREATE FUNCTION bioseq.qualified("
        "\"Ext\".\"Custom Type\"[], pg_catalog.int4[]) RETURNS void "
        "AS 'MODULE_PATHNAME', 'qualified' LANGUAGE C;\n";
    const install_script signature_script =
        parse_install_script(signature_baseline);
    require(
        signature_script.statements.size() == 5
            && signature_script.statements[0].routine_identity
                == std::optional<std::string>{
                    "bioseq.named_text(text)"}
            && signature_script.statements[1].routine_identity
                == std::optional<std::string>{
                    "bioseq.type_only(name)"}
            && signature_script.statements[2].routine_identity
                == std::optional<std::string>{
                    "bioseq.modes(app.custom_type,app.state_type,"
                    "pg_catalog.text[])"}
            && signature_script.statements[3].routine_identity
                == std::optional<std::string>{
                    "bioseq.defaulted(app.custom_type[])"}
            && signature_script.statements[4].routine_identity
                == std::optional<std::string>{
                    "bioseq.qualified(\"Ext\".\"Custom Type\"[],"
                    "pg_catalog.int4[])"},
        "routine identity includes IN, INOUT, and VARIADIC types, "
        "excludes names, OUT types, modes, and defaults, and preserves "
        "schema-qualified array and custom types");
    const std::string signature_owner =
        render_migration_owner(signature_script);
    require(
        signature_owner.contains(
            ".sql_identity = \"bioseq.named_text(text)\"")
            && signature_owner.contains(
                ".sql_identity = "
                "\"bioseq.defaulted(app.custom_type[])\"")
            && !signature_owner.contains(
                "bioseq.named_text(name text)")
            && !signature_owner.contains(
                "bioseq.defaulted(payload app.custom_type[])"),
        "migration owner serializes catalog identities from input types");
    constexpr std::string_view named_replace_baseline =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n"
        "CREATE OR REPLACE FUNCTION bioseq.identity_probe("
        "name app.custom_type[] DEFAULT ARRAY[]::app.custom_type[]) "
        "RETURNS void AS 'MODULE_PATHNAME', 'identity_probe' LANGUAGE C;\n"
        "CREATE OR REPLACE FUNCTION bioseq.identity_probe("
        "index_name app.custom_type[] = "
        "ARRAY[NULL, NULL]::app.custom_type[]) "
        "RETURNS void AS 'MODULE_PATHNAME', 'identity_probe' LANGUAGE C;\n";
    install_script named_replace_script =
        parse_install_script(named_replace_baseline);
    validate_install_script(named_replace_script);
    require(
        named_replace_script.statements.size() == 2
            && named_replace_script.statements[0].routine_identity
                == std::optional<std::string>{
                    "bioseq.identity_probe(app.custom_type[])"}
            && named_replace_script.statements[1].routine_identity
                == named_replace_script.statements[0].routine_identity
            && named_replace_script.statements[0].disposition
                == statement_disposition::
                    superseded_by_later_create_or_replace
            && named_replace_script.statements[1].disposition
                == statement_disposition::retained,
        "last CREATE OR REPLACE wins when names and defaults differ");
    const auto collect_result_fixture = [](
        std::string_view row_source,
        std::string_view owner_source) {
        std::vector<clang_structural::translation_unit> units;
        units.push_back(
            clang_structural::scan_translation_unit(
                "fixture_result.sql_rows.cppm",
                row_source));
        units.push_back(
            clang_structural::scan_translation_unit(
                "fixture_functions.cppm",
                owner_source));
        return collect_translation_units(std::move(units));
    };
    constexpr std::string_view result_row_source = R"CPP(
export module fixture.result.sql_rows;
export namespace fixture {
struct result_row {
    std::string name;
    std::optional<std::int64_t> total;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    Datum payload;
    static inline int ignored = 0;
    void helper() const;
};
}
)CPP";
    constexpr std::string_view result_owner_source = R"CPP(
export module fixture.functions;
namespace fixture {
[[bioseq_sql::function]]
bioseq::pg_utils::returns_table<result_row> rows(int limit);
[[bioseq_sql::function]]
bioseq::pg_utils::named_out<result_row> one();
}
)CPP";
    const collected_program result_program =
        collect_result_fixture(
            result_row_source, result_owner_source);
    require(
        result_program.result_rows.size() == 1
            && result_program.result_rows.front().cpp_qualified_name
                == "fixture::result_row"
            && result_program.result_rows.front().source_module
                == "fixture.result.sql_rows"
            && result_program.result_rows.front().fields.size() == 3
            && result_program.result_rows.front().fields[0].name
                == "name"
            && result_program.result_rows.front().fields[0].sql_type
                == "text"
            && result_program.result_rows.front().fields[1].name
                == "total"
            && result_program.result_rows.front().fields[1].sql_type
                == "bigint"
            && result_program.result_rows.front().fields[1].nullable
            && result_program.result_rows.front().fields[2].name
                == "payload"
            && result_program.result_rows.front().fields[2].sql_type
                == "jsonb",
        "an unannotated exported aggregate is registered once from "
        "result-wrapper template arguments");
    const auto table_result = std::ranges::find_if(
        result_program.entities,
        [](const collected_entity& entity) {
            return entity.name == "rows";
        });
    const auto named_result = std::ranges::find_if(
        result_program.entities,
        [](const collected_entity& entity) {
            return entity.name == "one";
        });
    require(
        result_program.entities.size() == 2
            && table_result != result_program.entities.end()
            && table_result->result.form
                == routine_result_form::returns_table
            && table_result->result.row_cpp_name
                == "fixture::result_row"
            && table_result->result_sql_type
                == "table (name text, total bigint, payload jsonb)"
            && named_result != result_program.entities.end()
            && named_result->result.form
                == routine_result_form::named_out
            && named_result->result.row_cpp_name
                == "fixture::result_row"
            && named_result->result_sql_type == "record"
            && std::ranges::none_of(
                result_program.entities,
                [](const collected_entity& entity) {
                    return entity.kind == entity_kind::table
                        || entity.kind == entity_kind::type
                        || entity.kind == entity_kind::column;
                }),
        "result rows create no SQL graph entity and preserve exact "
        "RETURNS TABLE and named-OUT contracts");
    require_failure(
        [collect_result_fixture, result_owner_source] {
            constexpr std::string_view invalid_row = R"CPP(
export module fixture.result.sql_rows;
export namespace fixture {
struct result_row {
    std::string_view borrowed;
};
}
)CPP";
            static_cast<void>(
                collect_result_fixture(
                    invalid_row, result_owner_source));
        },
        "cannot use borrowed std::string_view storage");
    require_failure(
        [collect_result_fixture, result_owner_source] {
            constexpr std::string_view invalid_row = R"CPP(
export module fixture.result.sql_rows;
export namespace fixture {
struct result_row {
    std::int64_t value = 0;
};
}
)CPP";
            static_cast<void>(
                collect_result_fixture(
                    invalid_row, result_owner_source));
        },
        "cannot have a default member initializer");
    require_failure(
        [result_owner_source] {
            std::vector<clang_structural::translation_unit> units;
            units.push_back(
                clang_structural::scan_translation_unit(
                    "fixture_functions.cppm",
                    result_owner_source));
            static_cast<void>(
                collect_translation_units(std::move(units)));
        },
        "was not found in an imported .sql_rows module");
    const std::string migrated = render_install_script(parsed);
    require(
        !migrated.contains("SELECT value $$")
            && migrated.contains("SELECT value + 1"),
        "superseded baseline routine is not rendered");
    const std::string baseline_manifest =
        render_manifest(parsed, migrated);
    require(
        baseline_manifest.contains("\"physical_statement_count\": 3")
            && baseline_manifest.contains("\"statement_count\": 2")
            && baseline_manifest.contains(
                "superseded_by_later_create_or_replace"),
        "baseline disposition manifest");
    const std::string migration_owner = render_migration_owner(parsed);
    require(
        migration_owner.contains(
            "export module bioseq.sql.install_owner")
            && migration_owner.contains(
                ".baseline_physical_id = \"P002\"")
            && !migration_owner.contains(
                ".baseline_physical_id = \"P001\""),
        "typed install owner retains only effective definitions");
    require(
        migration_owner.contains("[[bioseq_sql::sql_function")
            && migration_owner.contains(
                "migration_routine_language::sql")
            && migration_owner.contains(
                ".argument_declarations = R\"")
            && migration_owner.contains(".returns = R\"")
            && migration_owner.contains(".c_symbol = \"none\""),
        "typed SQL routine metadata inventory");
    const collected_program migrated_owner = collect_source_text(
        "install_owner.cppm", migration_owner);
    const sql_graph migrated_graph = build_sql_graph(migrated_owner);
    validate_codegen_completion_policy(migrated_graph, false);
    require_failure(
        [&migrated_graph] {
            validate_codegen_completion_policy(
                migrated_graph, true);
        },
        "transitional install owners");
    const rendered_sql_artifact migrated_artifact =
        render_sql_graph(migrated_graph);
    const std::string migrated_source_manifest =
        render_graph_manifest(
            migrated_graph,
            migrated_artifact,
            render_fmgr_entrypoint_modules(migrated_graph));
    std::string sanitized_migrated =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n\n";
    for (const sql_statement& statement : parsed.statements) {
        if (statement.disposition
            != statement_disposition::retained) {
            continue;
        }
        sanitized_migrated +=
            detail::sanitize_compatibility_sql(statement.text);
        sanitized_migrated += "\n\n";
    }
    if (sanitized_migrated.ends_with("\n\n")) {
        sanitized_migrated.pop_back();
    }
    require(
        migrated_artifact.statements.size() == 2
            && migrated_artifact.sql.contains("SELECT value + 1")
            && !migrated_artifact.sql.contains("SELECT value $$"),
        "checked C++ migration owner retains the effective statement set");
    require_equal(
        migrated_artifact.sql,
        sanitized_migrated,
        "checked C++ migration owner emits the sanitized effective baseline");
    require(
        migrated_source_manifest.contains(
            "\"typed_entity_count\": 2")
            && migrated_source_manifest.contains(
                "\"transitional_install_owner_entity_count\": 2")
            && migrated_source_manifest.contains(
                "\"natural_owner_entity_count\": 0")
            && migrated_source_manifest.contains(
                "\"natural_authority_complete\": false")
            && migrated_source_manifest.contains(
                "\"migration_install_action_count\": 0")
            && migrated_source_manifest.contains(
                "\"routine_entity_count\": 1")
            && migrated_source_manifest.contains(
                "\"c_wrapper_entity_count\": 0"),
        "migration escape-hatch inventory");

    constexpr std::string_view c_routine_baseline =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n"
        "\n"
        "CREATE FUNCTION bioseq.c_probe(value bigint) RETURNS bigint "
        "AS 'MODULE_PATHNAME', 'c_probe_impl' LANGUAGE C STABLE STRICT "
        "PARALLEL RESTRICTED SECURITY DEFINER COST 7 ROWS 3 "
        "SUPPORT c_probe_support;\n";
    install_script c_routine = parse_install_script(c_routine_baseline);
    validate_install_script(c_routine);
    require(
        c_routine.statements.front().routine
            && c_routine.statements.front().routine->identity
                == "bioseq.c_probe(bigint)"
            && c_routine.statements.front().routine->argument_declarations
                == "value bigint"
            && c_routine.statements.front().routine->returns == "bigint"
            && c_routine.statements.front().routine->language == "c"
            && c_routine.statements.front().routine->c_symbol
                == "c_probe_impl"
            && c_routine.statements.front().routine->volatility == "stable"
            && c_routine.statements.front().routine->null_input == "strict"
            && c_routine.statements.front().routine->security == "definer"
            && c_routine.statements.front().routine->parallel == "restricted"
            && c_routine.statements.front().routine->cost == "7"
            && c_routine.statements.front().routine->rows == "3"
            && c_routine.statements.front().routine->support
                == "c_probe_support",
        "exact C routine boundary metadata");

    constexpr std::string_view config_dump_baseline =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n"
        "\n"
        "SELECT pg_extension_config_dump("
        "'bioseq_internal.example', 'WHERE FALSE');\n"
        "SELECT pg_catalog.pg_extension_config_dump("
        "'bioseq_internal.qualified_example', 'WHERE FALSE');\n";
    install_script config_dumps =
        parse_install_script(config_dump_baseline);
    validate_install_script(config_dumps);
    const std::string config_dump_owner =
        render_migration_owner(config_dumps);
    require(
        config_dump_owner.contains("[[bioseq_sql::config_dump")
            && !config_dump_owner.contains(
                "[[bioseq_sql::install_action"),
        "qualified and unqualified extension config dumps are typed");

    constexpr std::string_view unsupported_baseline =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n"
        "\n"
        "INSERT INTO pg_catalog.pg_depend VALUES (1);\n"
        "RESET bioseq.logging;\n"
        "SELECT 1;\n";
    install_script unsupported =
        parse_install_script(unsupported_baseline);
    validate_install_script(unsupported);
    const std::string unsupported_owner =
        render_migration_owner(unsupported);
    require(
        unsupported_owner.contains("[[bioseq_sql::install_action")
            && unsupported_owner.contains(
                "postgresql_catalog_dependency_dml_has_no_typed_model")
            && unsupported_owner.contains(
                "session_reset_has_no_typed_model")
            && unsupported_owner.contains(
                "top_level_select_has_no_typed_model"),
        "unsupported construct has a specific escape-hatch ledger reason");

    constexpr std::string_view source = R"CPP(
export module fixture.sql;

// [[bioseq_sql::function]] in a comment is inert.
inline constexpr auto inert = R"RAW([[bioseq_sql::table]])RAW";

namespace sql_schemas {
[[bioseq_sql::schema]]
inline constexpr int bioseq = 0;
}

namespace fixture {
/**
 * @brief Jobs stored by the SQL fixture.
 * @ingroup bioseq_sql_public_api
 * @remark This comment is generated from an explicit remark.
 */
struct [[bioseq_sql::table({
    .schema = sql_schemas::bioseq,
    .create = bioseq::sql::create_mode::if_not_exists,
})]] jobs {
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bigint,
    })]]
    std::int64_t job_id = 1;
};

[[bioseq_sql::sequence({
    .schema = sql_schemas::bioseq,
    .increment = 2,
})]]
inline constexpr std::int64_t job_id_seq = 1;

/**
 * @brief Returns a job identifier.
 * @param value Candidate identifier.
 * @return The identifier.
 */
[[bioseq_sql::function({
    .schema = sql_schemas::bioseq,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::depends(job_id_seq)]]
std::int64_t current_job(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::pg::bigint,
    })]]
    std::int64_t value)
{
    return value;
}
}
)CPP";
    const collected_program collected =
        collect_source_text("fixture.cppm", source);
    require(collected.entities.size() == 5, "collected entity count");
    require(
        collected.entities.back().parameters.size() == 1
            && collected.entities.back().parameters.front().cpp_type
                == "std::int64_t",
        "parameter attributes are excluded from the C++ type");
    const sql_graph graph = build_sql_graph(collected, "public");
    require(graph.entities.size() == 5, "normalized graph entity count");
    validate_codegen_completion_policy(graph, true);
    {
        sql_graph incomplete_graph = graph;
        incomplete_graph.legacy_fmgr_macro_owners.push_back({
            .macro_name = "PG_FUNCTION_INFO_V1",
            .symbol = "current_job",
            .source_module = "fixture.sql",
            .source_path = "fixture.cppm",
            .source_line = 1,
        });
        require_failure(
            [&incomplete_graph] {
                validate_codegen_completion_policy(
                    incomplete_graph, true);
            },
            "legacy fmgr macro owners");
    }
    const rendered_sql_artifact artifact = render_sql_graph(graph);
    require(
        artifact.sql.contains("CREATE SCHEMA bioseq;")
            && artifact.sql.contains("CREATE TABLE IF NOT EXISTS bioseq.jobs")
            && artifact.sql.contains("CREATE SEQUENCE bioseq.job_id_seq")
            && artifact.sql.contains(
                "CREATE FUNCTION bioseq.current_job(value bigint)")
            && artifact.sql.contains(
                "-- This comment is generated from an explicit remark.")
            && artifact.sql.contains("COMMENT ON TABLE bioseq.jobs"),
        "deterministic source-mode SQL rendering");
    require(
        artifact.sql.find("CREATE SCHEMA bioseq;")
                < artifact.sql.find("CREATE SEQUENCE bioseq.job_id_seq")
            && artifact.sql.find("CREATE SEQUENCE bioseq.job_id_seq")
                < artifact.sql.find(
                    "CREATE FUNCTION bioseq.current_job"),
        "dependency DAG renders prerequisites before consumers");
    const std::vector<rendered_fmgr_entrypoint_module> fmgr_modules =
        render_fmgr_entrypoint_modules(graph);
    const std::string manifest =
        render_graph_manifest(graph, artifact, fmgr_modules);
    require_failure(
        [&graph, &artifact, &fmgr_modules] {
            static_cast<void>(render_graph_manifest(
                graph, artifact, fmgr_modules, true));
        },
        "reviewed DOC0388 fmgr migration ledger");
    require(
        manifest.contains("\"authority_mode\": \"annotated_cpp\"")
            && manifest.contains("\"public_documentation\": true")
            && manifest.contains("\"typed_entity_count\": 5")
            && manifest.contains(
                "\"transitional_install_owner_entity_count\": 0")
            && manifest.contains("\"natural_owner_entity_count\": 5")
            && manifest.contains(
                "\"natural_authority_complete\": true")
            && manifest.contains("\"routine_entity_count\": 1")
            && manifest.contains(
                "\"c_link_symbol_inventory_count\": 1")
            && manifest.contains("\"c_wrapper_entity_count\": 1")
            && manifest.contains(
                "\"routine_boundary_contract_count\": 1")
            && manifest.contains(
                "\"boundary_owner\": \"generated\"")
            && manifest.contains(
                "\"converter_key\": \"")
            && manifest.contains("\"source_sha256\":")
            && manifest.contains(
                "\"sql_identity\": \"bioseq.current_job(bigint)\"")
            && manifest.contains(
                "\"routine_identity\": "
                "\"bioseq.current_job(bigint)\"")
            && manifest.contains("\"generated_wrapper\": true")
            && !manifest.contains("\"sql_identity\": \"sql:function:")
            && manifest.contains(
                "sql:function:bioseq.current_job(bigint)"),
        "source manifest contract and normalized generated-boundary join");
    require(
        artifact.sql == render_sql_graph(graph).sql
            && manifest == render_graph_manifest(
                graph, render_sql_graph(graph), fmgr_modules),
        "source rendering determinism");

    constexpr std::string_view parameter_default_source = R"CPP(
export module fixture.parameter_defaults;
namespace fixture {
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
})]]
std::int32_t annotated_defaults(
    [[bioseq_sql::param({
        .default_value = bioseq::sql::checked_sql{"'auto'"},
    })]]
    std::string_view mode,
    [[bioseq_sql::param({
        .default_value = bioseq::sql::checked_sql{"'dna4'"},
    })]]
    std::string_view alphabet,
    [[bioseq_sql::param({
        .default_value = bioseq::sql::checked_sql{"3"},
    })]]
    std::int32_t retries,
    [[bioseq_sql::param({
        .default_value = bioseq::sql::checked_sql{"NULL"},
    })]]
    std::optional<std::string_view> fallback)
{
    return retries;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .volatility = bioseq::sql::volatility::immutable,
})]]
std::int32_t cpp_defaults(
    std::int32_t retries = 3,
    bool enabled = true,
    std::optional<std::int32_t> fallback = std::nullopt)
{
    return retries;
}
}
)CPP";
    const clang_structural::translation_unit parameter_default_structure =
        clang_structural::scan_translation_unit(
            "parameter_defaults.cppm", parameter_default_source);
    const std::array<std::string_view, 4> annotated_parameter_names{
        "mode", "alphabet", "retries", "fallback"};
    require(
        std::ranges::all_of(
            annotated_parameter_names,
            [&parameter_default_structure](std::string_view name) {
                return std::ranges::any_of(
                    parameter_default_structure.declarations,
                    [name](
                        const clang_structural::declaration_record&
                            declaration) {
                        return declaration.kind
                                == clang_structural::declaration_kind::
                                    parameter
                            && declaration.name == name;
                    });
            }),
        "structural collection binds every parameter-prefix attribute, "
        "including the final parameter");
    const collected_program parameter_defaults =
        collect_source_text(
            "parameter_defaults.cppm", parameter_default_source);
    const auto annotated_defaults = std::ranges::find(
        parameter_defaults.entities,
        std::string{"annotated_defaults"},
        &collected_entity::name);
    require(
        annotated_defaults != parameter_defaults.entities.end()
            && annotated_defaults->parameters.size() == 4
            && annotated_defaults->parameters[0].has_default
            && annotated_defaults->parameters[0].default_expression
                == "bioseq::sql::checked_sql{\"'auto'\"}"
            && annotated_defaults->parameters[1].default_expression
                == "bioseq::sql::checked_sql{\"'dna4'\"}"
            && annotated_defaults->parameters[2].default_expression
                == "bioseq::sql::checked_sql{\"3\"}"
            && annotated_defaults->parameters[3].default_expression
                == "bioseq::sql::checked_sql{\"NULL\"}",
        "collector retains explicit checked SQL parameter defaults");
    const auto cpp_defaults = std::ranges::find(
        parameter_defaults.entities,
        std::string{"cpp_defaults"},
        &collected_entity::name);
    require(
        cpp_defaults != parameter_defaults.entities.end()
            && cpp_defaults->parameters.size() == 3
            && cpp_defaults->parameters[0].has_default
            && cpp_defaults->parameters[0].default_expression == "3"
            && cpp_defaults->parameters[1].default_expression == "true"
            && cpp_defaults->parameters[2].default_expression
                == "std::nullopt",
        "collector preserves declaration-derived C++ parameter defaults");
    const sql_graph parameter_default_graph =
        build_sql_graph(parameter_defaults, "public");
    const auto normalized_annotated_defaults = std::ranges::find(
        parameter_default_graph.entities,
        std::string{"annotated_defaults"},
        [](const normalized_entity& entity) {
            return entity.source.name;
        });
    require(
        normalized_annotated_defaults
                != parameter_default_graph.entities.end()
            && normalized_annotated_defaults
                   ->source.parameters[0].default_expression
                == "bioseq::sql::checked_sql{\"'auto'\"}"
            && normalized_annotated_defaults
                   ->source.parameters[3].default_expression
                == "bioseq::sql::checked_sql{\"NULL\"}",
        "normalized graph preserves explicit parameter default authority");
    const rendered_sql_artifact parameter_default_artifact =
        render_sql_graph(parameter_default_graph);
    require(
        parameter_default_artifact.sql.contains(
            "CREATE FUNCTION bioseq.annotated_defaults("
            "mode text DEFAULT 'auto', "
            "alphabet text DEFAULT 'dna4', "
            "retries integer DEFAULT 3, "
            "fallback text DEFAULT NULL)")
            && parameter_default_artifact.sql.contains(
                "CREATE FUNCTION bioseq.cpp_defaults("
                "retries integer DEFAULT 3, "
                "enabled boolean DEFAULT TRUE, "
                "fallback integer DEFAULT NULL)"),
        "renderer emits explicit SQL and declaration-derived C++ defaults");
    require_failure(
        [] {
            static_cast<void>(collect_source_text(
                "enable_without_default.cppm",
                R"CPP(
export module fixture.enable_without_default;
[[bioseq_sql::function]]
std::int32_t invalid(
    [[bioseq_sql::param({.has_default = true})]]
    std::string_view mode)
{
    return 0;
}
)CPP"));
        },
        "unknown option '.has_default'");
    require_failure(
        [] {
            static_cast<void>(collect_source_text(
                "unchecked_parameter_default.cppm",
                R"CPP(
export module fixture.unchecked_parameter_default;
[[bioseq_sql::function]]
std::int32_t invalid(
    [[bioseq_sql::param({
        .default_value = 3,
    })]]
    std::int32_t retries)
{
    return retries;
}
)CPP"));
        },
        ".default_value must be a checked_sql expression");
    require_failure(
        [] {
            static_cast<void>(collect_source_text(
                "duplicate_parameter_default.cppm",
                R"CPP(
export module fixture.duplicate_parameter_default;
[[bioseq_sql::function]]
std::int32_t invalid(
    [[bioseq_sql::param({
        .default_value = bioseq::sql::checked_sql{"3"},
    })]]
    std::int32_t retries = 3)
{
    return retries;
}
)CPP"));
        },
        "explicit SQL default duplicates or conflicts with its C++ "
        "default expression");

    constexpr std::string_view bootstrap_natural = R"CPP(
export module fixture.bootstrap;
namespace bioseq {
struct settings {
    [[bioseq_sql::guc]]
    static inline bool logging = false;
};
namespace sql_entities {
[[bioseq_sql::session_setting]]
inline constexpr auto logging_off = bioseq::sql::set_after_guard(
    ::bioseq::settings::logging, false);
[[bioseq_sql::schema]]
inline constexpr bioseq::sql::schema_entity bioseq_internal{};
[[bioseq_sql::schema]]
inline constexpr bioseq::sql::schema_entity bioseq{};
}
}
)CPP";
    constexpr std::string_view bootstrap_install = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::session_setting({
    .baseline_physical_id = "P001",
    .sql_identity = "set bioseq.logging = off;",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p001 = {
    .sql = bioseq::sql::checked_sql{"SET bioseq.logging = off;"},
    .kind = bioseq::sql::migration_statement_kind::session_setting,
    .sql_identity = "set bioseq.logging = off;",
};
[[bioseq_sql::schema({
    .baseline_physical_id = "P002",
    .sql_identity = "create schema bioseq_internal;",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p002 = {
    .sql = bioseq::sql::checked_sql{"CREATE SCHEMA bioseq_internal;"},
    .kind = bioseq::sql::migration_statement_kind::schema,
    .sql_identity = "create schema bioseq_internal;",
};
[[bioseq_sql::schema({
    .baseline_physical_id = "P003",
    .sql_identity = "create schema bioseq;",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p003 = {
    .sql = bioseq::sql::checked_sql{"CREATE SCHEMA bioseq;"},
    .kind = bioseq::sql::migration_statement_kind::schema,
    .sql_identity = "create schema bioseq;",
};
}
)CPP";
    const auto collect_bootstrap_transition =
        [bootstrap_natural](std::string_view install_source) {
            collected_program result = collect_source_text(
                "bootstrap.cppm", bootstrap_natural);
            collected_program install = collect_source_text(
                "bootstrap_install_owner.cppm", install_source);
            for (auto& unit : install.translation_units) {
                result.translation_units.push_back(std::move(unit));
            }
            for (auto& entity : install.entities) {
                result.entities.push_back(std::move(entity));
            }
            return result;
        };
    const sql_graph bootstrap_graph = build_sql_graph(
        collect_bootstrap_transition(bootstrap_install));
    const rendered_sql_artifact bootstrap_artifact =
        render_sql_graph(bootstrap_graph);
    require(
        bootstrap_graph.entities.size() == 4
            && std::ranges::none_of(
                bootstrap_graph.entities,
                [](const normalized_entity& entity) {
                    return entity.source.typed_install_owner;
                })
            && bootstrap_artifact.statements.size() == 3
            && bootstrap_artifact.statements[0].baseline_physical_id
                == "P001"
            && bootstrap_artifact.statements[0].render_source
                == physical_render_source::natural_projection
            && bootstrap_artifact.statements[0].text
                == "SET bioseq.logging = off;"
            && bootstrap_artifact.statements[1].baseline_physical_id
                == "P002"
            && bootstrap_artifact.statements[1].text
                == "CREATE SCHEMA bioseq_internal;"
            && bootstrap_artifact.statements[2].baseline_physical_id
                == "P003"
            && bootstrap_artifact.statements[2].text
                == "CREATE SCHEMA bioseq;",
        "P001-P003 reconcile to one natural projected statement each");
    for (const normalized_entity& entity : bootstrap_graph.entities) {
        if (entity.source.kind != entity_kind::session_setting
            && entity.source.kind != entity_kind::schema) {
            continue;
        }
        require(
            entity.source.physical_statements.size() == 1
                && entity.source.physical_statements.front().render_source
                    == physical_render_source::natural_projection,
            "bootstrap natural owner retains exact physical provenance");
    }

    require_failure(
        [&collect_bootstrap_transition, bootstrap_install] {
            std::string mismatch{bootstrap_install};
            const std::size_t value =
                mismatch.find("set bioseq.logging = off;");
            mismatch.replace(
                value,
                std::string_view{"set bioseq.logging = off;"}.size(),
                "set bioseq.logging = on;");
            static_cast<void>(build_sql_graph(
                collect_bootstrap_transition(mismatch)));
        },
        "natural session setting owner disagrees");
    require_failure(
        [&collect_bootstrap_transition, bootstrap_install] {
            std::string malformed{bootstrap_install};
            const std::size_t action =
                malformed.find("set bioseq.logging = off;");
            malformed.replace(
                action,
                std::string_view{"set bioseq.logging = off;"}.size(),
                "reset bioseq.logging;");
            static_cast<void>(build_sql_graph(
                collect_bootstrap_transition(malformed)));
        },
        "must be exactly SET");
    require_failure(
        [&collect_bootstrap_transition, bootstrap_install] {
            std::string mismatch{bootstrap_install};
            const std::size_t name =
                mismatch.find("set bioseq.logging = off;");
            mismatch.replace(
                name,
                std::string_view{"set bioseq.logging = off;"}.size(),
                "set bioseq.other_logging = off;");
            static_cast<void>(build_sql_graph(
                collect_bootstrap_transition(mismatch)));
        },
        "natural session setting owner disagrees");
    require_failure(
        [&collect_bootstrap_transition, bootstrap_install] {
            std::string mismatch{bootstrap_install};
            const std::size_t mode =
                mismatch.find("create schema bioseq_internal;");
            mismatch.replace(
                mode,
                std::string_view{
                    "create schema bioseq_internal;"}.size(),
                "create schema if not exists bioseq_internal;");
            static_cast<void>(build_sql_graph(
                collect_bootstrap_transition(mismatch)));
        },
        "natural schema owner 'bioseq_internal' disagrees");

    constexpr std::string_view transition_natural = R"CPP(
export module bioseq.storage.metadata:core;
namespace fixture {
[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .increment = 2,
})]]
inline constexpr std::int64_t fixture_seq = 1;
}
)CPP";
    constexpr std::string_view transition_install_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::sequence({
    .baseline_physical_id = "P001",
    .sql_identity = "create sequence bioseq_internal.fixture_seq start with 1 increment by 2;",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p001 = {
    .sql = bioseq::sql::checked_sql{
        "\nCREATE SEQUENCE bioseq_internal.fixture_seq START WITH 1 INCREMENT BY 2;"},
    .kind = bioseq::sql::migration_statement_kind::sequence,
    .sql_identity =
        "create sequence bioseq_internal.fixture_seq start with 1 increment by 2;",
};
}
)CPP";
    collected_program transition = collect_source_text(
        "transition_natural.cppm", transition_natural);
    collected_program compatibility = collect_source_text(
        "transition_install_owner.cppm", transition_install_owner);
    for (auto& unit : compatibility.translation_units) {
        transition.translation_units.push_back(std::move(unit));
    }
    for (auto& entity : compatibility.entities) {
        transition.entities.push_back(std::move(entity));
    }
    const sql_graph transition_graph =
        build_sql_graph(std::move(transition));
    require(
        transition_graph.entities.size() == 1
            && transition_graph.entities.front().stable_identity
                == "sql:sequence:bioseq_internal.fixture_seq"
            && !transition_graph.entities.front()
                    .source.typed_install_owner
            && transition_graph.entities.front()
                    .source.compatibility_baseline_physical_id
                == "P001"
            && transition_graph.entities.front()
                    .source.compatibility_order_identity
                == "sql:install-owner:statement_p001"
            && transition_graph.entities.front()
                    .source.physical_statements.size()
                == 1
            && transition_graph.entities.front()
                    .source.physical_statements.front()
                    .baseline_physical_id
                == "P001"
            && transition_graph.entities.front()
                    .source.physical_statements.front()
                    .render_source
                == physical_render_source::natural_projection,
        "natural owner reconciles one equal typed install descriptor");
    const rendered_sql_artifact transition_artifact =
        render_sql_graph(transition_graph);
    require(
        transition_artifact.statements.size() == 1
            && transition_artifact.statements.front()
                   .baseline_physical_id
                == "P001"
            && transition_artifact.statements.front().facet
                == physical_statement_facet::primary
            && transition_artifact.statements.front().render_source
                == physical_render_source::natural_projection,
        "natural transition owner emits exactly one SQL statement");
    constexpr std::string_view expected_transition_body =
        "CREATE SEQUENCE bioseq_internal.fixture_seq\n"
        "    AS bigint\n"
        "    INCREMENT BY 2\n"
        "    START 1;";
    require_equal(
        transition_artifact.statements.front().text,
        expected_transition_body,
        "natural transition owner uses its projected sequence descriptor");
    const std::vector<rendered_entity_handle_module> transition_modules =
        render_entity_handle_modules(transition_graph);
    require(
        transition_modules.size() == 1
            && transition_modules.front().owner_key
                == "storage_metadata_core"
            && transition_modules.front().module_name
                == "bioseq.sql.generated.core.storage_metadata_core."
                   "entity_handles"
            && transition_modules.front().handles.size() == 1
            && transition_modules.front().content.contains(
                "export import "
                "bioseq.pg_utils.sql_catalog_entities;")
            && transition_modules.front().content.contains(
                "RELKIND_SEQUENCE")
            && transition_modules.front().content.contains(
                "fixed_catalog_name{\"fixture_seq\"}")
            && transition_modules.front().content.contains(
                "fixed_catalog_name{\"bioseq_internal\"}"),
        "profile-invariant fixed-sequence entity-handle module");
    const std::string transition_manifest =
        render_graph_manifest(
            transition_graph,
            transition_artifact,
            render_fmgr_entrypoint_modules(transition_graph));
    require(
        transition_manifest.contains(
            "\"transitional_install_owner_entity_count\": 0")
            && transition_manifest.contains(
                "\"natural_owner_entity_count\": 1")
            && transition_manifest.contains(
                "\"generated_cpp_profile_invariant\": true")
            && transition_manifest.contains(
                "\"entity_handle_count\": 1")
            && transition_manifest.contains(
                "\"entity_handle_module_count\": 1")
            && transition_manifest.contains(
                "\"compatibility_sql\": {\"descriptor_equal\": true")
            && transition_manifest.contains(
                "\"baseline_physical_id\": \"P001\"")
            && transition_manifest.contains(
                "\"physical_statements\": [")
            && transition_manifest.contains(
                "\"render_source\": \"natural_projection\""),
        "natural sequence ownership and provenance manifest");

    constexpr std::string_view natural_row = R"CPP(export module bioseq.storage.metadata.sql_rows;
namespace bioseq::storage::metadata::sql_rows {
/**
 * @brief Per-chunk-group storage metadata.
 */
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::if_not_exists,
}), bioseq_sql::primary_key({
    .columns = {"storage_id", "stripe_num", "chunk_group_num"},
})]] chunk_group {
    std::int64_t storage_id;
    std::int64_t stripe_num;
    std::int32_t chunk_group_num;
    std::int64_t row_count;
    std::int64_t deleted_rows = 0;
};
}
)CPP";
    constexpr std::string_view unlogged_table_install_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::table({
    .baseline_physical_id = "P039",
    .sql_identity = "create unlogged table bioseq_internal.minimizer_generation_storage ( generation_id bigint not null ) using heap;",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p039 = {
    .sql = bioseq::sql::checked_sql{
        "CREATE UNLOGGED TABLE "
        "bioseq_internal.minimizer_generation_storage "
        "(generation_id bigint NOT NULL) USING heap;"},
    .kind = bioseq::sql::migration_statement_kind::table,
    .sql_identity =
        "create unlogged table "
        "bioseq_internal.minimizer_generation_storage "
        "( generation_id bigint not null ) using heap;",
};
}
)CPP";
    collected_program unlogged_table_program =
        collect_source_text("sql_rows.cppm", natural_row);
    collected_program unlogged_table_compatibility =
        collect_source_text(
            "unlogged_install_owner.cppm",
            unlogged_table_install_owner);
    for (auto& unit :
         unlogged_table_compatibility.translation_units) {
        unlogged_table_program.translation_units.push_back(
            std::move(unit));
    }
    for (auto& entity : unlogged_table_compatibility.entities) {
        unlogged_table_program.entities.push_back(
            std::move(entity));
    }
    const sql_graph unlogged_table_graph =
        build_sql_graph(std::move(unlogged_table_program));
    require(
        std::ranges::any_of(
            unlogged_table_graph.entities,
            [](const normalized_entity& entity) {
                return entity.source.typed_install_owner
                    && entity.stable_identity
                        == "sql:install-owner:statement_p039";
            }),
        "P039 CREATE UNLOGGED TABLE transition remains valid while "
        "unrelated natural table owners are reconciled");

    constexpr std::string_view invalid_table_modifier_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::table({
    .baseline_physical_id = "P900",
    .sql_identity = "create materialized table bioseq_internal.invalid_table ( value bigint not null );",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p900 = {
    .sql = bioseq::sql::checked_sql{
        "CREATE MATERIALIZED TABLE "
        "bioseq_internal.invalid_table (value bigint NOT NULL);"},
    .kind = bioseq::sql::migration_statement_kind::table,
    .sql_identity =
        "create materialized table bioseq_internal.invalid_table "
        "( value bigint not null );",
};
}
)CPP";
    std::string invalid_table_modifier_diagnostic;
    try {
        const collected_program invalid = collect_source_text(
            "invalid_table_modifier.cppm",
            invalid_table_modifier_owner);
        static_cast<void>(build_sql_graph(invalid));
    } catch (const std::exception& error) {
        invalid_table_modifier_diagnostic = error.what();
    }
    require(
        invalid_table_modifier_diagnostic.contains(
            "fixture_install::statement_p900")
            && invalid_table_modifier_diagnostic.contains(
                "baseline physical ID P900")
            && invalid_table_modifier_diagnostic.contains(
                "invalid_table_modifier.cppm")
            && invalid_table_modifier_diagnostic.contains(
                "create materialized table "
                "bioseq_internal.invalid_table")
            && invalid_table_modifier_diagnostic.contains(
                "CREATE [UNLOGGED|TEMPORARY] TABLE"),
        "invalid CREATE TABLE modifiers fail with owner, transition, "
        "source, identity, and expected grammar context");

    constexpr std::string_view missing_table_target_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::table({
    .baseline_physical_id = "P901",
    .sql_identity = "create unlogged table;",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p901 = {
    .sql = bioseq::sql::checked_sql{"CREATE UNLOGGED TABLE;"},
    .kind = bioseq::sql::migration_statement_kind::table,
    .sql_identity = "create unlogged table;",
};
}
)CPP";
    require_failure(
        [missing_table_target_owner] {
            const collected_program missing = collect_source_text(
                "missing_table_target.cppm",
                missing_table_target_owner);
            static_cast<void>(build_sql_graph(missing));
        },
        "has no qualified table target");

    constexpr std::string_view row_install_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::table({
    .baseline_physical_id = "P015",
    .sql_identity = "create table if not exists bioseq_internal.chunk_group ( storage_id bigint not null, stripe_num bigint not null, chunk_group_num int not null, row_count bigint not null, deleted_rows bigint not null default 0, primary key (storage_id, stripe_num, chunk_group_num) );",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p015 = {
    .sql = bioseq::sql::checked_sql{R"SQL(
CREATE TABLE IF NOT EXISTS bioseq_internal.chunk_group (
    storage_id bigint NOT NULL,
    stripe_num bigint NOT NULL,
    chunk_group_num int NOT NULL,
    row_count bigint NOT NULL,
    deleted_rows bigint NOT NULL DEFAULT 0,
    PRIMARY KEY (storage_id, stripe_num, chunk_group_num)
);)SQL"},
    .kind = bioseq::sql::migration_statement_kind::table,
    .sql_identity = "create table if not exists bioseq_internal.chunk_group ( storage_id bigint not null, stripe_num bigint not null, chunk_group_num int not null, row_count bigint not null, deleted_rows bigint not null default 0, primary key (storage_id, stripe_num, chunk_group_num) );",
};

[[bioseq_sql::comment({
    .baseline_physical_id = "P016",
    .sql_identity = "comment on table bioseq_internal.chunk_group is 'Per-chunk-group ' 'storage metadata.';",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p016 = {
    .sql = bioseq::sql::checked_sql{R"SQL(
COMMENT ON TABLE bioseq_internal.chunk_group IS
    'Per-chunk-group storage metadata.';)SQL"},
    .kind = bioseq::sql::migration_statement_kind::comment,
    .sql_identity = "comment on table bioseq_internal.chunk_group is 'Per-chunk-group ' 'storage metadata.';",
};
}
)CPP";
    collected_program row_program =
        collect_source_text("sql_rows.cppm", natural_row);
    require(
        row_program.entities.size() == 7,
        "natural table collects five implicit columns and one primary key");
    const auto& row_declarations =
        row_program.translation_units.front().declarations;
    const auto row_record = std::ranges::find_if(
        row_declarations,
        [](const auto& declaration) {
            return declaration.qualified_name
                == "bioseq::storage::metadata::sql_rows::chunk_group";
        });
    const auto first_row_field = std::ranges::find_if(
        row_declarations,
        [](const auto& declaration) {
            return declaration.qualified_name.ends_with(
                "chunk_group::storage_id");
        });
    const auto last_row_field = std::ranges::find_if(
        row_declarations,
        [](const auto& declaration) {
            return declaration.qualified_name.ends_with(
                "chunk_group::deleted_rows");
        });
    require(
        row_record != row_declarations.end()
            && row_record->range.begin.line == 6
            && row_record->range.begin.column == 1
            && first_row_field != row_declarations.end()
            && first_row_field->range.begin.line == 12
            && first_row_field->range.begin.column == 5
            && last_row_field != row_declarations.end()
            && last_row_field->range.begin.line == 16
            && last_row_field->range.begin.column == 5,
        "natural row scanner preserves exact record and field positions");
    collected_program row_compatibility =
        collect_source_text("row_install_owner.cppm", row_install_owner);
    for (auto& unit : row_compatibility.translation_units) {
        row_program.translation_units.push_back(std::move(unit));
    }
    for (auto& entity : row_compatibility.entities) {
        row_program.entities.push_back(std::move(entity));
    }
    const sql_graph row_graph =
        build_sql_graph(std::move(row_program));
    require(
        row_graph.entities.size() == 7
            && std::ranges::count_if(
                   row_graph.entities,
                   [](const normalized_entity& entity) {
                       return entity.source.kind
                           == entity_kind::table;
                   })
                == 1
            && std::ranges::find_if(
                   row_graph.entities,
                   [](const normalized_entity& entity) {
                       return entity.source.kind
                           == entity_kind::table;
                   })
                    ->source.physical_statements.size()
                == 2,
        "natural row replaces its equal typed table install owner");
    const rendered_sql_artifact row_artifact =
        render_sql_graph(row_graph);
    require(
        row_artifact.statements.size() == 2
            && row_artifact.statements[0]
                   .baseline_physical_id
                == "P015"
            && row_artifact.statements[0].facet
                == physical_statement_facet::primary
            && row_artifact.statements[0].render_source
                == physical_render_source::natural_projection
            && row_artifact.statements[0].text.contains(
                "CREATE TABLE IF NOT EXISTS "
                "bioseq_internal.chunk_group")
            && row_artifact.statements[1]
                   .baseline_physical_id
                == "P016"
            && row_artifact.statements[1].statement_kind
                == entity_kind::comment
            && row_artifact.statements[1].facet
                == physical_statement_facet::catalog_comment
            && row_artifact.statements[1].render_source
                == physical_render_source::natural_projection
            && row_artifact.statements[1].text.contains(
                "COMMENT ON TABLE bioseq_internal.chunk_group IS "
                "'Per-chunk-group storage metadata.';")
            && std::ranges::count_if(
                   row_artifact.statements,
                   [](const rendered_statement& statement) {
                       return statement.text.contains(
                           "COMMENT ON TABLE "
                           "bioseq_internal.chunk_group");
                   })
                == 1
            && row_artifact.sql.contains(
            "CREATE TABLE IF NOT EXISTS "
            "bioseq_internal.chunk_group")
            && row_artifact.sql.contains(
                "COMMENT ON TABLE bioseq_internal.chunk_group IS "
                "'Per-chunk-group storage metadata.';")
            && !row_artifact.sql.contains(
                "pg_extension_config_dump("
                "'bioseq_internal.chunk_group'"),
        "natural chunk_group preserves SQL and does not gain config-dump "
        "semantics");

    constexpr std::string_view mismatched_row_comment_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::comment({
    .baseline_physical_id = "P016",
    .sql_identity = "comment on table bioseq_internal.chunk_group is 'Wrong metadata description.';",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p016 = {
    .sql = bioseq::sql::checked_sql{
        "COMMENT ON TABLE bioseq_internal.chunk_group IS "
        "'Wrong metadata description.';"},
    .kind = bioseq::sql::migration_statement_kind::comment,
    .sql_identity = "comment on table bioseq_internal.chunk_group is 'Wrong metadata description.';",
};
}
)CPP";
    collected_program projected_comment =
        collect_source_text("sql_rows.cppm", natural_row);
    collected_program mismatched_comment_compatibility =
        collect_source_text(
            "row_comment_owner.cppm",
            mismatched_row_comment_owner);
    for (auto& unit :
         mismatched_comment_compatibility.translation_units) {
        projected_comment.translation_units.push_back(
            std::move(unit));
    }
    for (auto& entity :
         mismatched_comment_compatibility.entities) {
        projected_comment.entities.push_back(std::move(entity));
    }
    const sql_graph projected_comment_graph =
        build_sql_graph(std::move(projected_comment));
    const rendered_sql_artifact projected_comment_artifact =
        render_sql_graph(projected_comment_graph);
    const auto projected_comment_statement = std::ranges::find(
        projected_comment_artifact.statements,
        physical_statement_facet::catalog_comment,
        &rendered_statement::facet);
    require(
        projected_comment_statement
                != projected_comment_artifact.statements.end()
            && projected_comment_artifact.sql.contains(
                "CREATE TABLE IF NOT EXISTS "
                "bioseq_internal.chunk_group")
            && projected_comment_statement->baseline_physical_id
                == "P016"
            && projected_comment_statement->text.contains(
                "'Per-chunk-group storage metadata.'")
            && !projected_comment_statement->text.contains(
                "Wrong metadata description"),
        "table comments use natural documentation while retaining P "
        "provenance");

    require_failure(
        [natural_row, row_install_owner] {
            collected_program duplicate =
                collect_source_text("sql_rows.cppm", natural_row);
            for (std::string_view source_path :
                 {"row_owner_a.cppm", "row_owner_b.cppm"}) {
                collected_program compatibility = collect_source_text(
                    std::string{source_path}, row_install_owner);
                for (auto& unit : compatibility.translation_units) {
                    duplicate.translation_units.push_back(
                        std::move(unit));
                }
                for (auto& entity : compatibility.entities) {
                    duplicate.entities.push_back(std::move(entity));
                }
            }
            static_cast<void>(build_sql_graph(std::move(duplicate)));
        },
        "duplicate baseline physical ID 'P015'");

    constexpr std::string_view duplicate_row_comment_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace duplicate_fixture_install {
[[bioseq_sql::comment({
    .baseline_physical_id = "P017",
    .sql_identity = "comment on table bioseq_internal.chunk_group is 'Per-chunk-group storage metadata.';",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p016_duplicate = {
    .sql = bioseq::sql::checked_sql{
        "COMMENT ON TABLE bioseq_internal.chunk_group IS "
        "'Per-chunk-group storage metadata.';"},
    .kind = bioseq::sql::migration_statement_kind::comment,
    .sql_identity = "comment on table bioseq_internal.chunk_group is 'Per-chunk-group storage metadata.';",
};
}
)CPP";
    require_failure(
        [natural_row, row_install_owner, duplicate_row_comment_owner] {
            collected_program duplicate =
                collect_source_text("sql_rows.cppm", natural_row);
            for (const auto& [source_path, source] :
                 std::array{
                     std::pair{
                         std::string_view{"row_owner.cppm"},
                         row_install_owner},
                     std::pair{
                         std::string_view{"row_comment_duplicate.cppm"},
                         duplicate_row_comment_owner},
                 }) {
                collected_program compatibility =
                    collect_source_text(
                        std::string{source_path}, source);
                for (auto& unit : compatibility.translation_units) {
                    duplicate.translation_units.push_back(
                        std::move(unit));
                }
                for (auto& entity : compatibility.entities) {
                    duplicate.entities.push_back(std::move(entity));
                }
            }
            static_cast<void>(build_sql_graph(std::move(duplicate)));
        },
        "matches more than one typed comment install owner");

    constexpr std::string_view malformed_physical_id_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace malformed_fixture_install {
[[bioseq_sql::install_action({
    .baseline_physical_id = "P17",
    .sql_identity = "select 1;",
    .migration_ledger_reason = "contract fixture",
})]]
constexpr bioseq::sql::typed_install_statement statement_p17 = {
    .sql = bioseq::sql::checked_sql{"SELECT 1;"},
    .kind = bioseq::sql::migration_statement_kind::session_setting,
    .sql_identity = "select 1;",
};
}
)CPP";
    require_failure(
        [malformed_physical_id_owner] {
            const collected_program malformed =
                collect_source_text(
                    "malformed_physical_id.cppm",
                    malformed_physical_id_owner);
            static_cast<void>(build_sql_graph(malformed));
        },
        "must use canonical PNNN decimal spelling");
    const std::vector<rendered_row_codec_module> row_modules =
        render_row_codec_modules(row_graph);
    require(
        row_modules.size() == 1
            && row_modules.front().owner_key == "storage_metadata"
            && row_modules.front().module_name
                == "bioseq.storage.metadata.sql_row_codecs"
            && row_modules.front().row_cpp_names.size() == 1
            && row_modules.front().content.contains(
                "struct chunk_group_codec final")
            && row_modules.front().content.contains(
                ".attribute_number = 5")
            && row_modules.front().content.contains(
                ".name = \"deleted_rows\"")
            && row_modules.front().content.contains(
                ".type_oid = INT8OID")
            && row_modules.front().content.contains(
                "#include \"utils/xid8.h\"")
            && row_modules.front().content.contains(
                "const row_type& row,\n"
                "            MemoryContext) {\n"
                "        return encoded_row{row};")
            && row_modules.front().content.contains(
                "MemoryContext result_context")
            && row_modules.front().content.contains(
                "Datum* values;")
            && row_modules.front().content.contains(
                "bool* nulls;")
            && row_modules.front().content.contains(
                "const_encoded_tuple_data")
            && row_modules.front().content.contains(
                "require_row_attribute_not_null")
            && row_modules.front().content.contains(
                "class update final")
            && row_modules.front().content.contains(
                "scan_key("),
        "configure-known generated row codec exposes typed tuple, update, "
        "scan, descriptor, and lifetime contracts");
    const std::string row_manifest =
        render_graph_manifest(
            row_graph,
            row_artifact,
            render_fmgr_entrypoint_modules(row_graph));
    require(
        row_manifest.contains("\"physical_statement_count\": 2")
            && row_manifest.contains(
                "\"baseline_physical_statement_count\": 2")
            && row_manifest.contains(
                "\"baseline_physical_id\": \"P016\"")
            && row_manifest.contains(
                "\"facet\": \"catalog_comment\"")
            && row_manifest.contains(
                "\"render_source\": \"natural_projection\"")
            && row_manifest.contains("\"row_codec_count\": 1")
            && row_manifest.contains(
                "\"row_codec_contract_count\": 1")
            && row_manifest.contains(
                "\"row_codec_contracts\": [")
            && row_manifest.contains(
                "\"sql_name\": \"deleted_rows\"")
            && row_manifest.contains(
                "\"row_codec_module_count\": 1")
            && row_manifest.contains(
                "\"owner_key\": \"storage_metadata\"")
            && row_manifest.contains(
                "\"source_module\": "
                "\"bioseq.storage.metadata.sql_rows\"")
            && row_manifest.contains(
                "\"module_name\": "
                "\"bioseq.storage.metadata.sql_row_codecs\"")
            && row_manifest.contains(
                "\"row_cpp_names\": [")
            && row_manifest.contains(
                "\"bioseq::storage::metadata::sql_rows::chunk_group\"")
            && row_manifest.contains(
                "\"row_entity_identities\": [")
            && row_manifest.contains(
                "\"sql:table:bioseq_internal.chunk_group\"")
            && row_manifest.contains(
                "\"descriptor_sha256\": \"")
            && row_manifest.contains(
                "\"facet\": \"row_codec\"")
            && row_manifest.contains(
                "\"profile_invariant\": true")
            && row_manifest
                == render_graph_manifest(
                    row_graph,
                    row_artifact,
                    render_fmgr_entrypoint_modules(row_graph)),
        "row-codec manifest provenance and descriptor hash are "
        "deterministic");

    constexpr std::string_view composite_natural_row = R"CPP(
export module bioseq.storage.load_pg.sql_rows;
namespace bioseq::storage::load_pg::composite_rows {
struct [[bioseq_sql::type({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]] column_mapping {
    std::optional<text*> column_name;
    std::optional<text*> data_source;
};
}
)CPP";
    constexpr std::string_view composite_install_owner = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::type({
    .baseline_physical_id = "P382",
    .sql_identity = "create type bioseq.column_mapping as ( column_name text, data_source text );",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p382 = {
    .sql = bioseq::sql::checked_sql{
        "CREATE TYPE bioseq.column_mapping AS (column_name text, data_source text);"},
    .kind = bioseq::sql::migration_statement_kind::type,
    .sql_identity =
        "create type bioseq.column_mapping as "
        "( column_name text, data_source text );",
    .create_or_replace = false,
};
}
)CPP";
    const auto collect_composite_transition =
        [](std::string_view natural_source,
           std::string_view install_source) {
            collected_program result = collect_source_text(
                "load_pg_sql_rows.cppm", natural_source);
            collected_program install = collect_source_text(
                "column_mapping_install_owner.cppm",
                install_source);
            for (auto& unit : install.translation_units) {
                result.translation_units.push_back(std::move(unit));
            }
            for (auto& entity : install.entities) {
                result.entities.push_back(std::move(entity));
            }
            for (auto& row : install.result_rows) {
                result.result_rows.push_back(std::move(row));
            }
            return result;
        };
    const sql_graph composite_graph = build_sql_graph(
        collect_composite_transition(
            composite_natural_row, composite_install_owner));
    const auto composite_type = std::ranges::find_if(
        composite_graph.entities,
        [](const normalized_entity& entity) {
            return entity.source.kind == entity_kind::type;
        });
    require(
        composite_graph.entities.size() == 3
            && std::ranges::count_if(
                   composite_graph.entities,
                   [](const normalized_entity& entity) {
                       return entity.source.kind
                           == entity_kind::type;
                   })
                == 1
            && composite_type != composite_graph.entities.end()
            && !composite_type->source.typed_install_owner
            && composite_type->stable_identity
                == "sql:type:bioseq.column_mapping"
            && composite_type->source.physical_statements.size() == 1
            && composite_type->source.physical_statements.front()
                   .baseline_physical_id
                == "P382"
            && composite_type->source.physical_statements.front().facet
                == physical_statement_facet::primary
            && composite_type->source.physical_statements.front()
                   .render_source
                == physical_render_source::natural_projection,
        "P382 reconciles to one natural composite type and two fields");
    validate_codegen_completion_policy(composite_graph, true);

    const rendered_sql_artifact composite_artifact =
        render_sql_graph(composite_graph);
    require(
        composite_artifact.statements.size() == 1
            && composite_artifact.statements.front()
                   .baseline_physical_id
                == "P382"
            && composite_artifact.statements.front().statement_kind
                == entity_kind::type
            && composite_artifact.statements.front().facet
                == physical_statement_facet::primary
            && composite_artifact.statements.front().render_source
                == physical_render_source::natural_projection,
        "P382 renders exactly one naturally projected type statement");
    require_equal(
        composite_artifact.statements.front().text,
        "CREATE TYPE bioseq.column_mapping AS (\n"
        "    column_name text,\n"
        "    data_source text\n"
        ");",
        "P382 preserves the composite field order and SQL types");

    const std::vector<rendered_row_codec_module> composite_modules =
        render_row_codec_modules(composite_graph);
    require(
        composite_modules.size() == 1
            && composite_modules.front().owner_key
                == "storage_load_pg"
            && composite_modules.front().source_module
                == "bioseq.storage.load_pg.sql_rows"
            && composite_modules.front().module_name
                == "bioseq.storage.load_pg.sql_row_codecs"
            && composite_modules.front().cpp_namespace
                == "bioseq::storage::load_pg::sql_row_codecs"
            && composite_modules.front().row_cpp_names
                == std::vector<std::string>{
                    "bioseq::storage::load_pg::composite_rows::"
                    "column_mapping"}
            && composite_modules.front().content.contains(
                "struct column_mapping_codec final")
            && composite_modules.front().content.contains(
                "{.attribute_number = 1, .name = \"column_name\", "
                ".type_oid = TEXTOID, .nullable = true}")
            && composite_modules.front().content.contains(
                "{.attribute_number = 2, .name = \"data_source\", "
                ".type_oid = TEXTOID, .nullable = true}"),
        "column_mapping generates a nullable text row codec in source "
        "order");
    constexpr std::string_view optional_text_field =
        "using value_type = std::optional<text*>;";
    const std::size_t first_optional_text =
        composite_modules.front().content.find(optional_text_field);
    require(
        first_optional_text != std::string::npos
            && composite_modules.front().content.find(
                   optional_text_field,
                   first_optional_text + optional_text_field.size())
                != std::string::npos,
        "column_mapping codec retains both optional text field types");

    const std::string composite_manifest =
        render_graph_manifest(
            composite_graph,
            composite_artifact,
            render_fmgr_entrypoint_modules(composite_graph));
    require(
        composite_manifest.contains(
            "\"baseline_physical_id\": \"P382\"")
            && composite_manifest.contains(
                "\"render_source\": \"natural_projection\"")
            && composite_manifest.contains(
                "\"row_codec_count\": 1")
            && composite_manifest.contains(
                "\"owner_key\": \"storage_load_pg\"")
            && composite_manifest.contains(
                "\"source_module\": "
                "\"bioseq.storage.load_pg.sql_rows\"")
            && composite_manifest.contains(
                "\"module_name\": "
                "\"bioseq.storage.load_pg.sql_row_codecs\"")
            && composite_manifest.contains(
                "\"bioseq::storage::load_pg::composite_rows::"
                "column_mapping\"")
            && composite_manifest.contains(
                "\"sql:type:bioseq.column_mapping\""),
        "P382 manifest records natural SQL and row-codec provenance");

    const auto composite_variant = [](
        std::string_view source,
        std::string_view original,
        std::string_view replacement) {
        std::string result{source};
        const std::size_t position = result.find(original);
        if (position == std::string::npos) {
            throw std::runtime_error(
                "composite contract fixture replacement was not found");
        }
        result.replace(position, original.size(), replacement);
        return result;
    };
    require_failure(
        [composite_variant, composite_natural_row,
         collect_composite_transition, composite_install_owner] {
            const std::string reordered = composite_variant(
                composite_natural_row,
                "    std::optional<text*> column_name;\n"
                "    std::optional<text*> data_source;",
                "    std::optional<text*> data_source;\n"
                "    std::optional<text*> column_name;");
            static_cast<void>(build_sql_graph(
                collect_composite_transition(
                    reordered, composite_install_owner)));
        },
        "field 1 name/order mismatch");
    require_failure(
        [composite_variant, composite_natural_row,
         collect_composite_transition, composite_install_owner] {
            const std::string wrong_type = composite_variant(
                composite_natural_row,
                "std::optional<text*> data_source",
                "std::optional<std::int32_t> data_source");
            static_cast<void>(build_sql_graph(
                collect_composite_transition(
                    wrong_type, composite_install_owner)));
        },
        "field 'data_source' SQL type mismatch");
    require_failure(
        [composite_variant, composite_natural_row,
         collect_composite_transition, composite_install_owner] {
            const std::string not_nullable = composite_variant(
                composite_natural_row,
                "std::optional<text*> column_name",
                "text* column_name");
            static_cast<void>(build_sql_graph(
                collect_composite_transition(
                    not_nullable, composite_install_owner)));
        },
        "field 'column_name' nullability mismatch");

    constexpr std::string_view operator_collection_source = R"CPP(
export module fixture.operator_collection;
namespace fixture {
[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
[[bioseq_sql::operator_({.symbol = "="})]]
bool letter_eq(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.letter">,
    })]]
    Datum right)
{
    return left == right;
}

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
[[bioseq_sql::operator_({.symbol = "="})]]
bool seq_eq(
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
    })]]
    Datum left,
    [[bioseq_sql::param({
        .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
    })]]
    Datum right)
{
    return left == right;
}
}
)CPP";
    const collected_program operator_collection =
        collect_source_text(
            "operator_collection.cppm",
            operator_collection_source);
    require(
        std::ranges::count_if(
            operator_collection.entities,
            [](const collected_entity& entity) {
                return entity.kind == entity_kind::operator_
                    && entity.schema == "bioseq"
                    && entity.parameters.size() == 2;
            })
            == 2,
        "operator companions inherit the co-located routine schema "
        "and operands");
    const sql_graph overloaded_operator_graph =
        build_sql_graph(operator_collection);
    require(
        std::ranges::count(
            overloaded_operator_graph.entities,
            std::string{
                "sql:operator:bioseq.=(bioseq.letter,bioseq.letter)"},
            &normalized_entity::stable_identity)
                == 1
            && std::ranges::count(
                   overloaded_operator_graph.entities,
                   std::string{
                       "sql:operator:bioseq.=(bioseq.seq,bioseq.seq)"},
                   &normalized_entity::stable_identity)
                == 1,
        "operator identities include operand types and remain distinct "
        "for overloaded symbols");
    require_failure(
        [] {
            static_cast<void>(collect_source_text(
                "invalid_cast_target.cppm",
                R"CPP(
export module fixture.invalid_cast_target;
[[bioseq_sql::cast]]
inline constexpr int invalid_cast_target = 0;
)CPP"));
        },
        "bioseq_sql::cast is not valid on a variable declaration");

    collected_program cast_transition;
    cast_transition.entities.push_back(collected_entity{
        .kind = entity_kind::function,
        .annotation_name = "function",
        .name = "seq_cast",
        .cpp_qualified_name = "fixture::seq_cast",
        .source_module = "fixture.cast",
        .schema = "bioseq",
        .parameters = {
            routine_parameter{
                .name = "value",
                .sql_type = "bioseq.seq",
            },
            routine_parameter{
                .name = "typmod",
                .sql_type = "integer",
            },
            routine_parameter{
                .name = "explicit_cast",
                .sql_type = "boolean",
            },
        },
        .result_sql_type = "bioseq.seq",
    });
    cast_transition.entities.push_back(collected_entity{
        .kind = entity_kind::cast,
        .annotation_name = "cast",
        .name = "seq_cast",
        .cpp_qualified_name = "fixture::seq_cast",
        .source_module = "fixture.cast",
        .schema = "bioseq",
        .options = {
            option_value{
                .name = "assignment",
                .expression = "true",
            },
        },
        .companion = true,
    });
    cast_transition.entities.push_back(collected_entity{
        .kind = entity_kind::cast,
        .annotation_name = "cast",
        .name = "statement_p900",
        .cpp_qualified_name = "fixture_install::statement_p900",
        .source_module = "fixture.install_owner",
        .schema = "$extension",
        .initializer_expression =
            "bioseq::sql::checked_sql{\"CREATE CAST "
            "(bioseq.seq AS bioseq.seq) WITH FUNCTION "
            "bioseq.seq_cast(bioseq.seq, integer, boolean) "
            "AS ASSIGNMENT;\"}",
        .options = {
            option_value{
                .name = "baseline_physical_id",
                .expression = "\"P900\"",
            },
            option_value{
                .name = "sql_identity",
                .expression =
                    "\"create cast (bioseq.seq as bioseq.seq) "
                    "with function bioseq.seq_cast("
                    "bioseq.seq, integer, boolean) as assignment;\"",
            },
        },
        .typed_install_owner = true,
    });
    {
        collected_program mismatch = cast_transition;
        collected_entity& natural_cast = mismatch.entities[1];
        natural_cast.options = {
            option_value{
                .name = "implicit",
                .expression = "true",
            },
        };
        bool rejected_with_context = false;
        try {
            static_cast<void>(build_sql_graph(std::move(mismatch)));
        } catch (const std::exception& error) {
            const std::string_view diagnostic{error.what()};
            rejected_with_context =
                diagnostic.contains("cast context")
                && diagnostic.contains("P900");
        }
        require(
            rejected_with_context,
            "cast transition drift names the differing property and "
            "physical statement");
    }
    const sql_graph cast_graph =
        build_sql_graph(std::move(cast_transition));
    const rendered_sql_artifact cast_artifact =
        render_sql_graph(cast_graph);
    require(
        cast_graph.entities.size() == 2,
        "three-argument typmod cast leaves one function and one cast "
        "owner");
    const auto rendered_cast =
        std::ranges::find(
            cast_artifact.statements,
            std::string{"P900"},
            &rendered_statement::baseline_physical_id);
    require(
        rendered_cast != cast_artifact.statements.end()
            && std::ranges::count(
                   cast_artifact.statements,
                   std::string{"P900"},
                   &rendered_statement::baseline_physical_id)
                == 1,
        "three-argument typmod cast renders one P900 physical "
        "statement");
    require(
        rendered_cast->baseline_physical_id == "P900",
        "three-argument typmod cast retains its physical statement ID");
    require(
        rendered_cast->render_source
            == physical_render_source::natural_projection,
        "three-argument typmod cast uses natural projection");
    require_equal(
        rendered_cast->text,
        "CREATE CAST (bioseq.seq AS bioseq.seq)\n"
        "    WITH FUNCTION bioseq.seq_cast("
        "bioseq.seq, integer, boolean)\n"
        "    AS ASSIGNMENT;",
        "three-argument typmod cast retains the full function "
        "signature");

    collected_program operator_transition;
    const auto natural_comparison = [](
        std::string name,
        std::string cpp_name) {
        return collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = std::move(name),
            .cpp_qualified_name = std::move(cpp_name),
            .source_module = "fixture.operator",
            .schema = "bioseq",
            .parameters = {
                routine_parameter{
                    .name = "left",
                    .sql_type = "bioseq.seq",
                },
                routine_parameter{
                    .name = "right",
                    .sql_type = "bioseq.seq",
                },
            },
            .result_sql_type = "boolean",
        };
    };
    operator_transition.entities.push_back(
        natural_comparison("seq_eq", "fixture::seq_eq"));
    operator_transition.entities.push_back(
        natural_comparison("seq_ne", "fixture::seq_ne"));
    operator_transition.entities.push_back(collected_entity{
        .kind = entity_kind::operator_,
        .annotation_name = "operator_",
        .name = "=",
        .cpp_qualified_name = "fixture::seq_eq",
        .source_module = "fixture.operator",
        .schema = "bioseq",
        .options = {
            option_value{
                .name = "commutator",
                .expression = "fixture::seq_eq",
            },
            option_value{
                .name = "negator",
                .expression = "fixture::seq_ne",
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
    });
    operator_transition.entities.push_back(collected_entity{
        .kind = entity_kind::operator_,
        .annotation_name = "operator_",
        .name = "<>",
        .cpp_qualified_name = "fixture::seq_ne",
        .source_module = "fixture.operator",
        .schema = "bioseq",
        .companion = true,
    });
    operator_transition.entities.push_back(collected_entity{
        .kind = entity_kind::operator_,
        .annotation_name = "operator_",
        .name = "statement_p901",
        .cpp_qualified_name = "fixture_install::statement_p901",
        .source_module = "fixture.install_owner",
        .schema = "$extension",
        .initializer_expression =
            "bioseq::sql::checked_sql{\"CREATE OPERATOR bioseq.= "
            "(LEFTARG = bioseq.seq, RIGHTARG = bioseq.seq, "
            "FUNCTION = bioseq.seq_eq, "
            "COMMUTATOR = OPERATOR(bioseq.=), "
            "NEGATOR = OPERATOR(bioseq.<>), "
            "RESTRICT = pg_catalog.eqsel, "
            "JOIN = pg_catalog.eqjoinsel, HASHES, MERGES);\"}",
        .options = {
            option_value{
                .name = "baseline_physical_id",
                .expression = "\"P901\"",
            },
            option_value{
                .name = "sql_identity",
                .expression =
                    "\"create operator bioseq.= (leftarg = "
                    "bioseq.seq, rightarg = bioseq.seq, function = "
                    "bioseq.seq_eq, commutator = operator(bioseq.=), "
                    "negator = operator(bioseq.<>), restrict = "
                    "pg_catalog.eqsel, join = pg_catalog.eqjoinsel, "
                    "hashes, merges);\"",
            },
        },
        .typed_install_owner = true,
    });
    {
        collected_program mismatch = operator_transition;
        collected_entity& natural_operator = mismatch.entities[2];
        std::erase_if(
            natural_operator.options,
            [](const option_value& option) {
                return option.name == "hashes";
            });
        bool rejected_with_context = false;
        try {
            static_cast<void>(build_sql_graph(std::move(mismatch)));
        } catch (const std::exception& error) {
            const std::string_view diagnostic{error.what()};
            rejected_with_context =
                diagnostic.contains("HASHES")
                && diagnostic.contains("P901");
        }
        require(
            rejected_with_context,
            "operator transition drift names the differing property and "
            "physical statement");
    }
    const sql_graph operator_graph =
        build_sql_graph(std::move(operator_transition));
    const rendered_sql_artifact operator_artifact =
        render_sql_graph(operator_graph);
    const auto rendered_operator =
        std::ranges::find(
            operator_artifact.statements,
            std::string{"P901"},
            &rendered_statement::baseline_physical_id);
    require(
        operator_graph.entities.size() == 4
            && rendered_operator != operator_artifact.statements.end()
            && std::ranges::count(
                   operator_artifact.statements,
                   std::string{"P901"},
                   &rendered_statement::baseline_physical_id)
                == 1
            && rendered_operator->render_source
                == physical_render_source::natural_projection
            && rendered_operator->text.contains(
                "COMMUTATOR = OPERATOR(bioseq.=)")
            && rendered_operator->text.contains(
                "NEGATOR = OPERATOR(bioseq.<>)")
            && rendered_operator->text.contains(
                "RESTRICT = pg_catalog.eqsel")
            && rendered_operator->text.contains(
                "JOIN = pg_catalog.eqjoinsel")
            && rendered_operator->text.contains(
                "HASHES")
            && rendered_operator->text.contains(
                "MERGES"),
        "operator metadata reconciles to one natural physical owner");

    const auto typed_catalog_owner = [](
        entity_kind kind,
        std::string name,
        std::string physical_id,
        std::string sql_identity) {
        const std::string cpp_name =
            "fixture_install::" + name;
        return collected_entity{
            .kind = kind,
            .annotation_name = std::string{kind_name(kind)},
            .name = std::move(name),
            .cpp_qualified_name = cpp_name,
            .source_module = "bioseq.sql.install_owner",
            .initializer_expression =
                "bioseq::sql::checked_sql{\""
                + sql_identity + "\"}",
            .options = {
                option_value{
                    .name = "baseline_physical_id",
                    .expression =
                        "\"" + physical_id + "\"",
                },
                option_value{
                    .name = "sql_identity",
                    .expression =
                        "\"" + sql_identity + "\"",
                },
            },
            .typed_install_owner = true,
        };
    };
    {
        const detail::aggregate_descriptor compact_clause =
            detail::typed_aggregate_descriptor(
                typed_catalog_owner(
                    entity_kind::aggregate,
                    "statement_p899",
                    "P899",
                    R"CPP(create aggregate vcount(*) (sfunc = vemptycount, stype = int8, initcond=\"0\");)CPP"));
        require(
            compact_clause.initial_condition == "0",
            "aggregate clauses accept SQL without whitespace around '='");
    }
    collected_program catalog_transition;
    catalog_transition.entities.push_back(collected_entity{
        .kind = entity_kind::type,
        .annotation_name = "type",
        .name = "sample",
        .cpp_qualified_name = "fixture::sample",
        .source_module = "fixture.catalog",
        .schema = "bioseq",
        .options = {
            option_value{
                .name = "internal_length",
                .expression = "4",
            },
            option_value{
                .name = "passed_by_value",
                .expression = "true",
            },
            option_value{
                .name = "alignment",
                .expression =
                    "bioseq::sql::type_alignment::int4",
            },
        },
    });
    const auto catalog_routine = [](
        std::string name,
        std::string cpp_name,
        std::vector<routine_parameter> parameters,
        std::string result_type) {
        return collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = std::move(name),
            .cpp_qualified_name = std::move(cpp_name),
            .source_module = "fixture.catalog",
            .schema = "bioseq",
            .parameters = std::move(parameters),
            .result_sql_type = std::move(result_type),
        };
    };
    {
        collected_program quoted_any_program;
        quoted_any_program.entities.push_back(catalog_routine(
            "vanycount",
            "fixture::vanycount",
            {
                routine_parameter{
                    .name = "state",
                    .sql_type = "bigint",
                },
                routine_parameter{
                    .name = "value",
                    .sql_type = "quoted_any",
                },
            },
            "bigint"));
        require(
            detail::graph_stable_identity(
                quoted_any_program.entities.front(),
                quoted_any_program)
                == "sql:function:bioseq.vanycount(bigint,\"any\")",
            "quoted PostgreSQL pseudo-types use their installed spelling "
            "in routine identities");
    }
    {
        collected_program aggregate_identities;
        aggregate_identities.entities.push_back(catalog_routine(
            "vemptycount",
            "fixture::vemptycount",
            {routine_parameter{
                .name = "state",
                .sql_type = "bigint",
            }},
            "bigint"));
        aggregate_identities.entities.push_back(catalog_routine(
            "vanycount",
            "fixture::vanycount",
            {
                routine_parameter{
                    .name = "state",
                    .sql_type = "bigint",
                },
                routine_parameter{
                    .name = "value",
                    .sql_type = "any",
                },
            },
            "bigint"));
        aggregate_identities.entities.push_back(collected_entity{
            .kind = entity_kind::aggregate,
            .annotation_name = "aggregate",
            .name = "vcount",
            .cpp_qualified_name = "fixture::count_all::vcount",
            .source_module = "fixture.catalog",
            .schema = "$extension",
            .options = {option_value{
                .name = "transition",
                .expression = "fixture::vemptycount",
            }},
        });
        aggregate_identities.entities.push_back(collected_entity{
            .kind = entity_kind::aggregate,
            .annotation_name = "aggregate",
            .name = "vcount",
            .cpp_qualified_name = "fixture::count_any::vcount",
            .source_module = "fixture.catalog",
            .schema = "$extension",
            .options = {option_value{
                .name = "transition",
                .expression = "fixture::vanycount",
            }},
        });
        const std::string count_all_identity =
            detail::graph_stable_identity(
                aggregate_identities.entities[2],
                aggregate_identities);
        const std::string count_any_identity =
            detail::graph_stable_identity(
                aggregate_identities.entities[3],
                aggregate_identities);
        require(
            count_all_identity
                == "sql:aggregate:$extension.vcount(*)",
            "zero-input aggregates have a signature-qualified "
            "stable identity");
        require(
            count_any_identity
                == "sql:aggregate:$extension.vcount(any)",
            "overloaded aggregates have a signature-qualified "
            "stable identity");
        require(
            count_all_identity != count_any_identity,
            "aggregate stable identities distinguish overloads");
    }
    catalog_transition.entities.push_back(catalog_routine(
        "sample_in",
        "fixture::sample_in",
        {routine_parameter{
            .name = "value",
            .sql_type = "cstring",
        }},
        "bioseq.sample"));
    catalog_transition.entities.push_back(collected_entity{
        .kind = entity_kind::type_role,
        .annotation_name = "type_input",
        .name = "sample_in",
        .cpp_qualified_name = "fixture::sample_in",
        .source_module = "fixture.catalog",
        .schema = "bioseq",
        .options = {option_value{
            .name = "for_type",
            .expression =
                "bioseq::sql::type_of<fixture::sample>",
        }},
        .companion = true,
    });
    catalog_transition.entities.push_back(catalog_routine(
        "sample_out",
        "fixture::sample_out",
        {routine_parameter{
            .name = "value",
            .sql_type = "bioseq.sample",
        }},
        "cstring"));
    catalog_transition.entities.push_back(collected_entity{
        .kind = entity_kind::type_role,
        .annotation_name = "type_output",
        .name = "sample_out",
        .cpp_qualified_name = "fixture::sample_out",
        .source_module = "fixture.catalog",
        .schema = "bioseq",
        .options = {option_value{
            .name = "for_type",
            .expression =
                "bioseq::sql::type_of<fixture::sample>",
        }},
        .companion = true,
    });
    catalog_transition.entities.push_back(catalog_routine(
        "letter_cast",
        "bioseq::pg_types::sql_routines::letter",
        {routine_parameter{
            .name = "value",
            .sql_type = "bioseq.sample",
        }},
        "bioseq.sample"));
    catalog_transition.entities.push_back(catalog_routine(
        "sample_lt",
        "bioseq::pg_types::letter::letter_lt",
        {
            routine_parameter{
                .name = "left",
                .sql_type = "bioseq.sample",
            },
            routine_parameter{
                .name = "right",
                .sql_type = "bioseq.sample",
            },
        },
        "boolean"));
    catalog_transition.entities.push_back(collected_entity{
        .kind = entity_kind::operator_,
        .annotation_name = "operator_",
        .name = "<",
        .cpp_qualified_name =
            "bioseq::pg_types::letter::letter_lt",
        .source_module = "fixture.catalog",
        .schema = "bioseq",
        .companion = true,
    });
    catalog_transition.entities.push_back(catalog_routine(
        "sample_cmp",
        "fixture::sample_cmp",
        {
            routine_parameter{
                .name = "left",
                .sql_type = "bioseq.sample",
            },
            routine_parameter{
                .name = "right",
                .sql_type = "bioseq.sample",
            },
        },
        "integer"));
    catalog_transition.entities.push_back(collected_entity{
        .kind = entity_kind::opclass,
        .annotation_name = "opclass",
        .name = "sample_ops",
        .cpp_qualified_name = "fixture::sample_ops",
        .source_module = "fixture.catalog",
        .schema = "$extension",
        .options = {
            option_value{
                .name = "access_method",
                .expression =
                    "bioseq::sql::pg_catalog_reference::btree",
            },
            option_value{
                .name = "is_default",
                .expression = "true",
            },
            option_value{
                .name = "operators",
                .expression =
                    "bioseq::sql::opclass_operators({"
                    "bioseq::sql::opclass_operator("
                    "1, bioseq::pg_types::letter::letter_lt)})",
            },
            option_value{
                .name = "functions",
                .expression =
                    "bioseq::sql::opclass_functions({"
                    "bioseq::sql::opclass_function("
                    "1, fixture::sample_cmp)})",
            },
        },
    });
    catalog_transition.entities.push_back(catalog_routine(
        "fixture_handler",
        "fixture::fixture_handler",
        {},
        "table_am_handler"));
    catalog_transition.entities.push_back(collected_entity{
        .kind = entity_kind::access_method,
        .annotation_name = "access_method",
        .name = "fixture",
        .cpp_qualified_name = "fixture::fixture_handler",
        .source_module = "fixture.catalog",
        .schema = "$extension",
        .companion = true,
    });
    catalog_transition.entities.push_back(catalog_routine(
        "secret",
        "fixture::secret",
        {routine_parameter{
            .name = "value",
            .sql_type = "integer",
        }},
        "integer"));
    catalog_transition.entities.push_back(collected_entity{
        .kind = entity_kind::revoke,
        .annotation_name = "revoke",
        .name = "secret",
        .cpp_qualified_name = "fixture::secret",
        .source_module = "fixture.catalog",
        .schema = "bioseq",
        .options = {
            option_value{
                .name = "privileges",
                .expression =
                    "bioseq::sql::privileges::execute",
            },
            option_value{
                .name = "from",
                .expression = "bioseq::sql::public_role",
            },
        },
        .companion = true,
    });
    catalog_transition.entities.push_back(typed_catalog_owner(
        entity_kind::type,
        "statement_p900",
        "P900",
        "create type bioseq.sample;"));
    catalog_transition.entities.push_back(typed_catalog_owner(
        entity_kind::type,
        "statement_p901",
        "P901",
        "create type bioseq.sample (input = bioseq.sample_in, "
        "output = bioseq.sample_out, internallength = 4, "
        "passedbyvalue, alignment = int4);"));
    catalog_transition.entities.push_back(typed_catalog_owner(
        entity_kind::opclass,
        "statement_p902",
        "P902",
        "create operator class bioseq.sample_ops default "
        "for type bioseq.sample using btree as "
        "operator 1 bioseq.<, "
        "function 1 bioseq.sample_cmp("
        "bioseq.sample, bioseq.sample);"));
    catalog_transition.entities.push_back(typed_catalog_owner(
        entity_kind::access_method,
        "statement_p903",
        "P903",
        "create access method fixture type table "
        "handler bioseq.fixture_handler;"));
    catalog_transition.entities.push_back(typed_catalog_owner(
        entity_kind::revoke,
        "statement_p904",
        "P904",
        "revoke all on function bioseq.secret(integer) "
        "from public;"));

    {
        collected_program mismatch = catalog_transition;
        option_value& identity =
            mismatch.entities[mismatch.entities.size() - 4]
                .options[1];
        identity.expression =
            "\"create type bioseq.sample ("
            "input = bioseq.sample_in, "
            "output = bioseq.sample_out, internallength = 8, "
            "passedbyvalue, alignment = int4);\"";
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural base type");
    }
    {
        collected_program mismatch = catalog_transition;
        option_value& identity =
            mismatch.entities[mismatch.entities.size() - 3]
                .options[1];
        identity.expression =
            "\"create operator class bioseq.sample_ops default "
            "for type bioseq.sample using hash as "
            "operator 1 bioseq.<, "
            "function 1 bioseq.sample_cmp("
            "bioseq.sample, bioseq.sample);\"";
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural operator class");
    }
    {
        collected_program mismatch = catalog_transition;
        option_value& identity =
            mismatch.entities[mismatch.entities.size() - 2]
                .options[1];
        identity.expression =
            "\"create access method fixture type index "
            "handler bioseq.fixture_handler;\"";
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural access method");
    }
    {
        collected_program mismatch = catalog_transition;
        option_value& identity =
            mismatch.entities.back().options[1];
        identity.expression =
            "\"revoke all on function bioseq.secret(integer) "
            "from catalog_admin;\"";
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural revoke owner");
    }
    const sql_graph public_schema_catalog_graph =
        build_sql_graph(catalog_transition, "public");
    const rendered_sql_artifact public_schema_catalog_artifact =
        render_sql_graph(public_schema_catalog_graph);
    const auto public_schema_opclass = std::ranges::find_if(
        public_schema_catalog_graph.entities,
        [](const normalized_entity& entity) {
            return entity.source.kind == entity_kind::opclass;
        });
    require(
        public_schema_opclass
                != public_schema_catalog_graph.entities.end()
            && public_schema_opclass->stable_identity
                == "sql:opclass:bioseq.sample_ops"
            && public_schema_catalog_artifact.sql.contains(
                "CREATE OPERATOR CLASS bioseq.sample_ops DEFAULT")
            && !public_schema_catalog_artifact.sql.contains(
                "CREATE OPERATOR CLASS public.sample_ops"),
        "operator-class schema follows its indexed type rather than "
        "the install-resolved extension schema");

    const sql_graph catalog_graph =
        build_sql_graph(catalog_transition, "bioseq");
    const rendered_sql_artifact catalog_artifact =
        render_sql_graph(catalog_graph);
    const auto catalog_opclass = std::ranges::find_if(
        catalog_graph.entities,
        [](const normalized_entity& entity) {
            return entity.source.kind == entity_kind::opclass;
        });
    require(
        catalog_opclass != catalog_graph.entities.end()
            && catalog_opclass->stable_identity
                == "sql:opclass:bioseq.sample_ops"
            && std::ranges::none_of(
                catalog_graph.entities,
                [](const normalized_entity& entity) {
                    return entity.source.typed_install_owner;
                })
            && std::ranges::count_if(
                   catalog_artifact.statements,
                   [](const rendered_statement& statement) {
                       return statement.baseline_physical_id
                                  >= "P900"
                           && statement.baseline_physical_id
                                  <= "P904"
                           && statement.render_source
                                  == physical_render_source::
                                      natural_projection;
                   })
                == 5
            && std::ranges::any_of(
                catalog_artifact.statements,
                [](const rendered_statement& statement) {
                    return statement.baseline_physical_id == "P900"
                        && statement.facet
                            == physical_statement_facet::
                                base_type_shell;
                })
            && std::ranges::any_of(
                catalog_artifact.statements,
                [](const rendered_statement& statement) {
                    return statement.baseline_physical_id == "P901"
                        && statement.facet
                            == physical_statement_facet::
                                base_type_definition;
                })
            && catalog_artifact.sql.contains(
                "CREATE OPERATOR CLASS bioseq.sample_ops DEFAULT")
            && catalog_artifact.sql.contains(
                "CREATE ACCESS METHOD fixture")
            && catalog_artifact.sql.contains(
                "REVOKE EXECUTE ON FUNCTION "
                "bioseq.secret(integer) FROM PUBLIC"),
        "base type, operator class, access method, and revoke owners "
        "adopt exact natural physical statements");

    const collected_program direct_list_view =
        collect_source_text(
            "direct_list_view.cppm",
            R"CPP(
export module fixture.direct_list_view;
namespace fixture {
[[bioseq_sql::view({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
})]]
inline constexpr bioseq::sql::checked_sql direct_list_view{R"SQL(
SELECT 1 AS value
)SQL"};
}
)CPP");
    const auto collected_direct_list_view = std::ranges::find_if(
        direct_list_view.entities,
        [](const collected_entity& entity) {
            return entity.kind == entity_kind::view;
        });
    require(
        collected_direct_list_view
                != direct_list_view.entities.end()
            && collected_direct_list_view->initializer_expression
                .contains("SELECT 1 AS value"),
        "annotated direct-list variables retain their SQL initializer");

    const collected_program distinct_schema_acls =
        collect_source_text(
            "distinct_schema_acls.cppm",
            R"CPP(
export module fixture.distinct_schema_acls;
namespace fixture {
[[bioseq_sql::schema]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
    .scope = bioseq::sql::acl_scope::all_tables_in_schema,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline constexpr bioseq::sql::schema_entity private_schema{};
}
)CPP");
    require(
        std::ranges::count_if(
            distinct_schema_acls.entities,
            [](const collected_entity& entity) {
                return entity.kind == entity_kind::revoke;
            })
            == 2,
        "distinct repeated ACL annotations remain separate companions");
    const sql_graph distinct_schema_acl_graph =
        build_sql_graph(distinct_schema_acls);
    std::set<std::string, std::less<>> distinct_schema_acl_identities;
    for (const normalized_entity& entity :
         distinct_schema_acl_graph.entities) {
        if (entity.source.kind == entity_kind::revoke) {
            distinct_schema_acl_identities.insert(
                entity.stable_identity);
        }
    }
    require(
        distinct_schema_acl_identities.size() == 2,
        "distinct repeated ACL annotations have descriptor-qualified "
        "stable identities");
    sql_graph ledger_bound_schema_acl_graph =
        distinct_schema_acl_graph;
    std::vector<physical_ledger_entry> schema_acl_ledger{
        physical_ledger_entry{
            .baseline_physical_id = "P001",
            .baseline_ordinal = 1,
            .facet = physical_statement_facet::primary,
            .owner_identity = "sql:schema:private_schema",
        },
    };
    std::size_t schema_acl_ordinal = 2;
    for (const std::string& identity :
         distinct_schema_acl_identities) {
        schema_acl_ledger.push_back(physical_ledger_entry{
            .baseline_physical_id =
                schema_acl_ordinal == 2 ? "P002" : "P003",
            .baseline_ordinal = schema_acl_ordinal++,
            .facet = physical_statement_facet::primary,
            .owner_identity = identity,
        });
    }
    apply_physical_ledger(
        ledger_bound_schema_acl_graph, schema_acl_ledger);
    const rendered_sql_artifact ledger_bound_schema_acl_sql =
        render_sql_graph(ledger_bound_schema_acl_graph);
    require(
        ledger_bound_schema_acl_sql.statements.size() == 3
            && ledger_bound_schema_acl_sql.statements.front()
                   .baseline_physical_id == "P001"
            && ledger_bound_schema_acl_sql.statements.back()
                   .baseline_physical_id == "P003"
            && std::ranges::all_of(
                ledger_bound_schema_acl_sql.statements,
                [](const rendered_statement& statement) {
                    return statement.render_source
                        == physical_render_source::
                            natural_projection;
                }),
        "non-owning physical ledger binds and orders only natural "
        "projections");
    const std::string serialized_schema_acl_ledger =
        render_physical_ledger(ledger_bound_schema_acl_graph);
    require(
        serialized_schema_acl_ledger.contains(
            "P001\tprimary\tsql:schema:private_schema\n")
            && serialized_schema_acl_ledger.contains(
                "P002\tprimary\tsql:revoke:")
            && serialized_schema_acl_ledger.contains(
                "P003\tprimary\tsql:revoke:"),
        "physical ledger serialization is contiguous and names natural "
        "owners without SQL compatibility text");
    sql_graph checked_schema_acl_graph =
        distinct_schema_acl_graph;
    const physical_ledger_material checked_schema_acl_material =
        apply_checked_physical_ledger(
            checked_schema_acl_graph,
            serialized_schema_acl_ledger,
            "checked-schema-acl-ledger.tsv");
    require(
        checked_schema_acl_material.exact_bytes
                == serialized_schema_acl_ledger
            && checked_schema_acl_material.sha256
                == sha256_hex(serialized_schema_acl_ledger)
            && checked_schema_acl_material.sha256
                != sha256_hex(std::string_view{}),
        "checked physical ledger seals its exact canonical input bytes");
    require_failure(
        [distinct_schema_acl_graph,
         serialized_schema_acl_ledger]() mutable {
            sql_graph graph = distinct_schema_acl_graph;
            static_cast<void>(apply_checked_physical_ledger(
                graph,
                serialized_schema_acl_ledger + "\n",
                "noncanonical-schema-acl-ledger.tsv"));
        },
        "is not in canonical generated form");
    require_failure(
        [distinct_schema_acl_graph,
         serialized_schema_acl_ledger]() mutable {
            sql_graph graph = distinct_schema_acl_graph;
            std::string tampered = serialized_schema_acl_ledger;
            constexpr std::string_view existing =
                "sql:schema:private_schema";
            const std::size_t offset = tampered.find(existing);
            tampered.replace(
                offset,
                existing.size(),
                "sql:schema:missing_schema");
            static_cast<void>(apply_checked_physical_ledger(
                graph,
                std::move(tampered),
                "tampered-schema-acl-ledger.tsv"));
        },
        "references missing natural owner");

    command_line transition_ledger_command{
        .action = command_action::generate,
        .mode = authority_mode::annotated_cpp,
        .stage = generation_stage::final,
        .physical_ledger_output = "candidate-ledger.tsv",
        .require_complete_codegen = true,
    };
    validate_final_physical_ledger_policy(
        transition_ledger_command);
    command_line missing_ledger_command =
        transition_ledger_command;
    missing_ledger_command.physical_ledger_output.clear();
    require_failure(
        [&missing_ledger_command] {
            validate_final_physical_ledger_policy(
                missing_ledger_command);
        },
        "requires an explicit physical ledger state");
    command_line sibling_complete_command =
        missing_ledger_command;
    sibling_complete_command.extension_name = "bioseq_cache";
    validate_final_physical_ledger_policy(
        sibling_complete_command);
    sibling_complete_command.require_checked_physical_ledger =
        true;
    require_failure(
        [&sibling_complete_command] {
            validate_final_physical_ledger_policy(
                sibling_complete_command);
        },
        "requires an explicit physical ledger state");
    command_line checked_ledger_command =
        transition_ledger_command;
    checked_ledger_command.physical_ledger_output.clear();
    checked_ledger_command.physical_ledger =
        "checked-ledger.tsv";
    validate_final_physical_ledger_policy(
        checked_ledger_command);
    command_line activated_cutover_command =
        checked_ledger_command;
    activated_cutover_command.require_checked_physical_ledger = true;
    validate_final_physical_ledger_policy(
        activated_cutover_command);
    command_line rejected_transition_command =
        transition_ledger_command;
    rejected_transition_command.require_checked_physical_ledger = true;
    require_failure(
        [&rejected_transition_command] {
            validate_final_physical_ledger_policy(
                rejected_transition_command);
        },
        "checked-ledger cutover requires exactly --physical-ledger");

    const sql_graph mixed_ledger_graph =
        build_sql_graph(collect_source_text(
            "mixed_physical_ledger.cppm",
            R"CPP(
export module fixture.mixed_physical_ledger;
namespace fixture {
[[bioseq_sql::schema]]
inline constexpr bioseq::sql::schema_entity first_schema{};
[[bioseq_sql::schema]]
inline constexpr bioseq::sql::schema_entity unbound_schema{};
[[bioseq_sql::schema]]
inline constexpr bioseq::sql::schema_entity second_schema{};
}
)CPP"));
    sql_graph bound_mixed_ledger_graph =
        mixed_ledger_graph;
    apply_physical_ledger(
        bound_mixed_ledger_graph,
        {
            physical_ledger_entry{
                .baseline_physical_id = "P001",
                .baseline_ordinal = 1,
                .facet = physical_statement_facet::primary,
                .owner_identity = "sql:schema:second_schema",
            },
            physical_ledger_entry{
                .baseline_physical_id = "P002",
                .baseline_ordinal = 2,
                .facet = physical_statement_facet::primary,
                .owner_identity = "sql:schema:first_schema",
            },
        });
    const rendered_sql_artifact mixed_ledger_artifact =
        render_sql_graph(bound_mixed_ledger_graph);
    require(
        mixed_ledger_artifact.statements.size() == 3
            && mixed_ledger_artifact.statements[0]
                   .baseline_physical_id == "P001"
            && mixed_ledger_artifact.statements[0]
                   .entity_identity
                == "sql:schema:second_schema"
            && mixed_ledger_artifact.statements[1]
                   .baseline_physical_id == "P002"
            && mixed_ledger_artifact.statements[1]
                   .entity_identity
                == "sql:schema:first_schema"
            && mixed_ledger_artifact.statements[2]
                   .baseline_physical_id.empty()
            && mixed_ledger_artifact.statements[2]
                   .entity_identity
                == "sql:schema:unbound_schema",
        "mixed physical ordering keeps P-ledger order and places "
        "unbound natural statements deterministically afterward");
    require_failure(
        [] {
            static_cast<void>(collect_source_text(
                "duplicate_schema_acls.cppm",
                R"CPP(
export module fixture.duplicate_schema_acls;
namespace fixture {
[[bioseq_sql::schema]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
inline constexpr bioseq::sql::schema_entity private_schema{};
}
)CPP"));
        },
        "redundant repeated SQL annotation");
    require_failure(
        [] {
            static_cast<void>(collect_source_text(
                "misspelled_function_option.cppm",
                R"CPP(
export module fixture.misspelled_function_option;
namespace fixture {
[[bioseq_sql::function({
    .volatlity = bioseq::sql::volatility::stable,
})]]
int sample(int value);
}
)CPP"));
        },
        "unknown option '.volatlity'");

    collected_program view_grant_transition;
    view_grant_transition.entities.push_back(collected_entity{
        .kind = entity_kind::view,
        .annotation_name = "view",
        .name = "sample_view",
        .cpp_qualified_name = "fixture::sample_view",
        .source_module = "fixture.catalog",
        .schema = "bioseq",
        .initializer_expression =
            R"CPP(bioseq::sql::checked_sql{R"SQL(SELECT 1 AS value)SQL"})CPP",
        .options = {
            option_value{
                .name = "create",
                .expression =
                    "bioseq::sql::create_mode::or_replace",
            },
            option_value{
                .name = "security_barrier",
                .expression = "true",
            },
            option_value{
                .name = "security_invoker",
                .expression = "true",
            },
        },
    });
    view_grant_transition.entities.push_back(collected_entity{
        .kind = entity_kind::grant,
        .annotation_name = "grant",
        .name = "sample_view",
        .cpp_qualified_name = "fixture::sample_view",
        .source_module = "fixture.catalog",
        .schema = "bioseq",
        .options = {
            option_value{
                .name = "privileges",
                .expression =
                    "bioseq::sql::privileges::select",
            },
            option_value{
                .name = "to",
                .expression = "bioseq::sql::public_role",
            },
            option_value{
                .name = "grant_option",
                .expression = "true",
            },
        },
        .companion = true,
    });
    view_grant_transition.entities.push_back(typed_catalog_owner(
        entity_kind::view,
        "statement_p905",
        "P905",
        "create or replace view bioseq.sample_view "
        "with (security_barrier, security_invoker = true) "
        "as select 1 as value;"));
    view_grant_transition.entities.push_back(typed_catalog_owner(
        entity_kind::grant,
        "statement_p906",
        "P906",
        "grant select on bioseq.sample_view to public "
        "with grant option;"));
    {
        collected_program mismatch = view_grant_transition;
        mismatch.entities[2].options[1].expression =
            "\"create or replace view bioseq.sample_view "
            "with (security_barrier, security_invoker = true) "
            "as select 2 as value;\"";
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural view owner");
    }
    {
        collected_program mismatch = view_grant_transition;
        mismatch.entities[2].options[1].expression =
            "\"create or replace view bioseq.sample_view "
            "with (security_barrier) "
            "as select 1 as value;\"";
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural view owner");
    }
    {
        collected_program mismatch = view_grant_transition;
        mismatch.entities[3].options[1].expression =
            "\"grant select on bioseq.sample_view to catalog_admin "
            "with grant option;\"";
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural grant owner");
    }
    const sql_graph view_grant_graph =
        build_sql_graph(view_grant_transition, "bioseq");
    const rendered_sql_artifact view_grant_artifact =
        render_sql_graph(view_grant_graph);
    require(
        std::ranges::none_of(
            view_grant_graph.entities,
            [](const normalized_entity& entity) {
                return entity.source.typed_install_owner;
            })
            && std::ranges::count_if(
                   view_grant_artifact.statements,
                   [](const rendered_statement& statement) {
                       return (statement.baseline_physical_id == "P905"
                               || statement.baseline_physical_id == "P906")
                           && statement.render_source
                                  == physical_render_source::
                                      natural_projection;
                   })
                == 2
            && view_grant_artifact.sql.contains(
                "CREATE OR REPLACE VIEW bioseq.sample_view "
                "WITH (security_barrier = true, "
                "security_invoker = true)")
            && view_grant_artifact.sql.contains(
                "GRANT SELECT ON TABLE bioseq.sample_view TO PUBLIC "
                "WITH GRANT OPTION;"),
        "views and omitted-TABLE view grants reconcile and render from "
        "natural owners");

    collected_program dependency_transition;
    dependency_transition.entities.push_back(catalog_routine(
        "fixture_handler",
        "fixture::fixture_handler",
        {},
        "table_am_handler"));
    dependency_transition.entities.push_back(collected_entity{
        .kind = entity_kind::access_method,
        .annotation_name = "access_method",
        .name = "fixture",
        .cpp_qualified_name = "fixture::fixture_handler",
        .source_module = "fixture.catalog",
        .schema = "$extension",
        .companion = true,
    });
    dependency_transition.entities.push_back(collected_entity{
        .kind = entity_kind::catalog_dependencies,
        .annotation_name = "am_catalog_dependencies",
        .name = "fixture_handler",
        .cpp_qualified_name = "fixture::fixture_handler",
        .source_module = "fixture.catalog",
        .schema = "$extension",
        .options = {option_value{
            .name = "roots",
            .expression =
                "bioseq::sql::entities<fixture::chunk>("
                "fixture::epoch_seq)",
        }},
        .companion = true,
    });
    dependency_transition.entities.push_back(collected_entity{
        .kind = entity_kind::table,
        .annotation_name = "table",
        .name = "chunk",
        .cpp_qualified_name = "fixture::chunk",
        .source_module = "fixture.catalog.sql_rows",
        .schema = "bioseq_internal",
        .options = {option_value{
            .name = "access_method",
            .expression = "\"fixture\"",
        }},
    });
    dependency_transition.entities.push_back(collected_entity{
        .kind = entity_kind::column,
        .annotation_name = "column",
        .name = "id",
        .cpp_qualified_name = "fixture::chunk::id",
        .enclosing_cpp_name = "fixture::chunk",
        .source_module = "fixture.catalog.sql_rows",
        .schema = "$extension",
        .cpp_return_type = "std::int64_t",
        .companion = true,
    });
    dependency_transition.entities.push_back(collected_entity{
        .kind = entity_kind::primary_key,
        .annotation_name = "primary_key",
        .name = "primary_key",
        .cpp_qualified_name = "fixture::chunk",
        .enclosing_cpp_name = "fixture::chunk",
        .source_module = "fixture.catalog.sql_rows",
        .schema = "$extension",
        .options = {option_value{
            .name = "columns",
            .expression = "{\"id\"}",
        }},
        .companion = true,
    });
    dependency_transition.entities.push_back(collected_entity{
        .kind = entity_kind::index,
        .annotation_name = "index",
        .name = "chunk_lookup_idx",
        .cpp_qualified_name = "fixture::chunk",
        .enclosing_cpp_name = "fixture::chunk",
        .source_module = "fixture.catalog.sql_rows",
        .schema = "$extension",
        .options = {
            option_value{
                .name = "name",
                .expression = "\"chunk_lookup_idx\"",
            },
            option_value{
                .name = "expression",
                .expression =
                    "bioseq::sql::checked_sql{\"id\"}",
            },
        },
        .companion = true,
    });
    dependency_transition.entities.push_back(collected_entity{
        .kind = entity_kind::sequence,
        .annotation_name = "sequence",
        .name = "epoch_seq",
        .cpp_qualified_name = "fixture::epoch_seq",
        .source_module = "fixture.catalog",
        .schema = "bioseq_internal",
        .cpp_return_type = "std::int64_t",
        .initializer_expression = "1",
    });
    dependency_transition.entities.push_back(typed_catalog_owner(
        entity_kind::access_method,
        "statement_p907",
        "P907",
        "create access method fixture type table "
        "handler bioseq.fixture_handler;"));
    const std::string dependency_sql =
        detail::catalog_dependencies_sql(
            "fixture",
            {
                {"bioseq_internal", "chunk"},
                {"bioseq_internal", "chunk_lookup_idx"},
                {"bioseq_internal", "chunk_pkey"},
                {"bioseq_internal", "epoch_seq"},
            });
    const std::string relocatable_dependency_sql =
        detail::catalog_dependencies_sql(
            "fixture",
            {
                {"$extension", "chunk"},
                {"$extension", "chunk_lookup_idx"},
            });
    require(
        relocatable_dependency_sql.contains(
            "n.nspname = pg_catalog.current_schema()")
            && !relocatable_dependency_sql.contains("'$extension'"),
        "extension-owned catalog dependency roots resolve through the "
        "active extension schema");
    std::string legacy_dependency_sql = dependency_sql;
    legacy_dependency_sql.replace(
        legacy_dependency_sql.find("'chunk_lookup_idx'"),
        std::string_view{"'chunk_lookup_idx'"}.size(),
        "'temporary_relation_name'");
    legacy_dependency_sql.replace(
        legacy_dependency_sql.find("'epoch_seq'"),
        std::string_view{"'epoch_seq'"}.size(),
        "'chunk_lookup_idx'");
    legacy_dependency_sql.replace(
        legacy_dependency_sql.find("'temporary_relation_name'"),
        std::string_view{"'temporary_relation_name'"}.size(),
        "'epoch_seq'");
    collected_entity typed_dependencies = typed_catalog_owner(
        entity_kind::catalog_dependencies,
        "statement_p908",
        "P908",
        legacy_dependency_sql);
    typed_dependencies.options[1].expression =
        "R\"DEP(" + legacy_dependency_sql + ")DEP\"";
    typed_dependencies.initializer_expression =
        "bioseq::sql::checked_sql{R\"DEP("
        + legacy_dependency_sql + ")DEP\"}";
    dependency_transition.entities.push_back(
        std::move(typed_dependencies));
    {
        collected_program mismatch = dependency_transition;
        option_value& identity =
            mismatch.entities.back().options[1];
        const std::size_t relation =
            identity.expression.find("chunk_lookup_idx");
        identity.expression.replace(
            relation,
            std::string_view{"chunk_lookup_idx"}.size(),
            "chunk_other_idx");
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural AM catalog dependency owner");
    }
    {
        collected_program mismatch = dependency_transition;
        option_value& identity =
            mismatch.entities.back().options[1];
        const std::size_t projection =
            identity.expression.find("SELECT * FROM edges");
        identity.expression.replace(
            projection,
            std::string_view{"SELECT * FROM edges"}.size(),
            "SELECT classid FROM edges");
        require_failure(
            [mismatch = std::move(mismatch)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(mismatch), "bioseq"));
            },
            "natural AM catalog dependency owner");
    }
    {
        collected_program unnamed_unique = dependency_transition;
        unnamed_unique.entities.insert(
            unnamed_unique.entities.begin() + 7,
            collected_entity{
                .kind = entity_kind::unique,
                .annotation_name = "unique",
                .name = "unique",
                .cpp_qualified_name = "fixture::chunk",
                .enclosing_cpp_name = "fixture::chunk",
                .source_module = "fixture.catalog.sql_rows",
                .schema = "$extension",
                .options = {option_value{
                    .name = "columns",
                    .expression = "{\"id\"}",
                }},
                .companion = true,
            });
        require_failure(
            [unnamed_unique = std::move(unnamed_unique)]() mutable {
                static_cast<void>(build_sql_graph(
                    std::move(unnamed_unique), "bioseq"));
            },
            "requires an explicit .name");
    }
    require_failure(
        [] {
            static_cast<void>(
                detail::relation_child_catalog_name(
                    collected_entity{
                        .kind = entity_kind::primary_key,
                    },
                    collected_entity{
                        .kind = entity_kind::table,
                        .name =
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                            "aaaaaaaaaaaaaaaaaaaaaaaaaaa",
                        .schema = "bioseq_internal",
                    }));
        },
        "63-byte identifier limit");
    const sql_graph dependency_graph =
        build_sql_graph(dependency_transition, "bioseq");
    const auto dependency_table = std::ranges::find(
        dependency_graph.entities,
        std::string{"sql:table:bioseq_internal.chunk"},
        &normalized_entity::stable_identity);
    const auto dependency_access_method = std::ranges::find(
        dependency_graph.entities,
        entity_kind::access_method,
        [](const normalized_entity& entity) {
            return entity.source.kind;
        });
    require(
        dependency_table != dependency_graph.entities.end()
            && dependency_access_method
                != dependency_graph.entities.end()
            && std::ranges::contains(
                dependency_table->dependency_identities,
                dependency_access_method->stable_identity)
            && dependency_access_method < dependency_table,
        "a table using a naturally owned access method depends on and "
        "follows that access method in the natural SQL graph");
    const rendered_sql_artifact dependency_artifact =
        render_sql_graph(dependency_graph);
    const auto dependency_statement = std::ranges::find(
        dependency_artifact.statements,
        std::string{"P908"},
        &rendered_statement::baseline_physical_id);
    require(
        dependency_statement != dependency_artifact.statements.end()
            && dependency_statement->render_source
                == physical_render_source::natural_projection
            && dependency_statement->text.contains(
                "'chunk_lookup_idx'")
            && dependency_statement->text.contains("'chunk_pkey'")
            && dependency_statement->text.contains("'epoch_seq'")
            && dependency_statement->text.contains(
                "'n'::\"char\""),
        "AM catalog dependencies expand roots, derive unnamed primary "
        "keys, decode quoted SQL identities, and render naturally");

    constexpr std::string_view owned_row_source = R"CPP(
export module fixture.owned.sql_rows;
namespace fixture::catalog_rows {
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
}), bioseq_sql::unique({
    .name = "owned_row_owner_unique",
    .placement = bioseq::sql::constraint_placement::alter_table,
    .columns = {"owner"},
}), bioseq_sql::unique({
    .name = "owned_row_label_unique",
    .placement = bioseq::sql::constraint_placement::alter_table,
    .columns = {"label"},
})]] owned_row {
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::regclass,
    })]]
    Oid owner;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::name,
    })]]
    NameData label;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::bytea,
    })]]
    std::optional<bytea*> payload;
};
}
)CPP";
    const collected_program owned_row_collected =
        collect_source_text("owned_rows.cppm", owned_row_source);
    require(
        owned_row_collected.entities.size() == 6
            && std::ranges::count_if(
                   owned_row_collected.entities,
                   [](const collected_entity& entity) {
                       return entity.kind == entity_kind::unique;
                   })
                == 2,
        "repeatable named table constraints remain distinct");
    const sql_graph owned_row_graph =
        build_sql_graph(owned_row_collected);
    const rendered_sql_artifact owned_row_sql =
        render_sql_graph(owned_row_graph);
    require(
        owned_row_sql.statements.size() == 3
            && owned_row_sql.sql.contains(
                "ALTER TABLE bioseq_internal.owned_row")
            && owned_row_sql.sql.contains(
                "CONSTRAINT owned_row_owner_unique")
            && owned_row_sql.sql.contains(
                "CONSTRAINT owned_row_label_unique"),
        "repeatable natural ALTER TABLE constraints render");
    const auto owned_row_modules =
        render_row_codec_modules(owned_row_graph);
    require(
        owned_row_modules.size() == 1
            && owned_row_modules.front().content.contains(
                ".type_oid = REGCLASSOID")
            && owned_row_modules.front().content.contains(
                ".type_oid = NAMEOID")
            && owned_row_modules.front().content.contains(
                ".type_oid = BYTEAOID")
            && owned_row_modules.front().content.contains(
                "using value_type = std::optional<bytea*>")
            && owned_row_modules.front().content.contains(
                "MemoryContext value_context")
            && owned_row_modules.front().content.contains(
                "copy_pg_datum_to_memory_context<NameData>")
            && owned_row_modules.front().content.contains(
                "copy_pg_datum_to_memory_context<bytea>")
            && owned_row_modules.front().content.contains(
                "by_reference_datum_copy_policy datum_copy_policy")
            && owned_row_modules.front().content.contains(
                ".type_length = static_cast<int16>(NAMEDATALEN)")
            && owned_row_modules.front().content.contains(
                ".type_length = static_cast<int16>(-1)")
            && owned_row_modules.front().content.contains(
                ".type_by_value = false")
            && owned_row_modules.front().content.contains(
                "field<Member>::datum_copy_policy")
            && !owned_row_modules.front().content.contains(
                "field<Member>::type_oid,\n")
            && !owned_row_modules.front().content.contains(
                "NAMEOID, result_context")
            && !owned_row_modules.front().content.contains(
                "BYTEAOID, result_context")
            && owned_row_modules.front().content.contains(
                "requires field<Member>::by_reference")
            && !owned_row_modules.front().content.contains(
                "const row_type& row,\n"
                "            MemoryContext) {\n"
                "        return encoded_row{row};")
            && owned_row_modules.front().content.contains(
                "comparison_procedure(Comparison)"),
        "closed row conversions encode explicit by-reference ownership");

    constexpr std::string_view redundant_column_marker = R"CPP(
export module bad.redundant.sql_rows;
struct [[bioseq_sql::table]]
bad_row {
    [[bioseq_sql::column]]
    std::int64_t value;
};
)CPP";
    require_failure(
        [redundant_column_marker] {
            static_cast<void>(collect_source_text(
                "redundant_column.cppm",
                redundant_column_marker));
        },
        "redundant bioseq_sql::column marker");

    constexpr std::string_view transition_mismatch = R"CPP(
export module bioseq.sql.install_owner;
namespace fixture_install {
[[bioseq_sql::sequence({
    .baseline_physical_id = "P001",
    .sql_identity = "create sequence bioseq_internal.fixture_seq start with 1 increment by 3;",
    .create_or_replace = false,
})]]
constexpr bioseq::sql::typed_install_statement statement_p001 = {
    .sql = bioseq::sql::checked_sql{
        "CREATE SEQUENCE bioseq_internal.fixture_seq "
        "START WITH 1 INCREMENT BY 3;"},
    .kind = bioseq::sql::migration_statement_kind::sequence,
    .sql_identity =
        "create sequence bioseq_internal.fixture_seq start with 1 increment by 3;",
};
}
)CPP";
    require_failure(
        [transition_natural, transition_mismatch] {
            collected_program value = collect_source_text(
                "transition_natural.cppm", transition_natural);
            collected_program mismatch = collect_source_text(
                "transition_mismatch.cppm", transition_mismatch);
            for (auto& unit : mismatch.translation_units) {
                value.translation_units.push_back(std::move(unit));
            }
            for (auto& entity : mismatch.entities) {
                value.entities.push_back(std::move(entity));
            }
            static_cast<void>(build_sql_graph(std::move(value)));
        },
        "disagrees with its typed install-owner descriptor");

    constexpr std::string_view conditional_sequence = R"CPP(
export module bad.conditional_sequence;
#if BIOSEQ_PROFILE_A
[[bioseq_sql::sequence({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
})]]
inline constexpr std::int64_t conditional_seq = 1;
#endif
)CPP";
    require_failure(
        [conditional_sequence] {
            const collected_program value = collect_source_text(
                "conditional_sequence.cppm", conditional_sequence);
            static_cast<void>(build_sql_graph(value));
        },
        "require an unconditional natural owner");

    constexpr std::string_view nested_namespace = R"CPP(
export module bad.nested;
[[bioseq::sql::function]]
int bad_name();
)CPP";
    require_failure(
        [nested_namespace] {
            static_cast<void>(collect_source_text(
                "bad_nested.cppm", nested_namespace));
        },
        "invalid nested SQL attribute namespace");

    constexpr std::string_view conflicting = R"CPP(
export module bad.conflicting;
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::stable})]]
[[bioseq_sql::function({.volatility = bioseq::sql::volatility::immutable})]]
int conflict(int value);
)CPP";
    require_failure(
        [conflicting] {
            static_cast<void>(collect_source_text(
                "bad_conflicting.cppm", conflicting));
        },
        "conflicting repeated SQL annotation");

    constexpr std::string_view duplicate_owner = R"CPP(
export module bad.duplicate;
namespace duplicate {
[[bioseq_sql::function]] int same(int value);
[[bioseq_sql::function]] int same(int value);
}
)CPP";
    require_failure(
        [duplicate_owner] {
            const collected_program duplicate = collect_source_text(
                "bad_duplicate.cppm", duplicate_owner);
            static_cast<void>(build_sql_graph(duplicate));
        },
        "duplicate target SQL owner");

    constexpr std::string_view unresolved = R"CPP(
export module bad.unresolved;
[[bioseq_sql::function]]
[[bioseq_sql::depends(missing_owner)]]
int unresolved(int value);
)CPP";
    require_failure(
        [unresolved] {
            const collected_program value = collect_source_text(
                "bad_unresolved.cppm", unresolved);
            static_cast<void>(build_sql_graph(value));
        },
        "unresolved SQL dependency");

    constexpr std::string_view invalid_type_placement = R"CPP(
export module bad.type_placement;
[[bioseq_sql::type]]
export struct invalid_type {};
)CPP";
    require_failure(
        [invalid_type_placement] {
            static_cast<void>(collect_source_text(
                "bad_type_placement.cppm", invalid_type_placement));
        },
        "class-head placement");

    std::filesystem::path repository_root{__FILE__};
    if (!repository_root.is_absolute()) {
        repository_root =
            std::filesystem::absolute(repository_root);
    }
    for (int parent = 0; parent < 4; ++parent) {
        repository_root = repository_root.parent_path();
    }
    const std::filesystem::path negative_root =
        repository_root / "main/codegen/sql/tests/negative";
    const auto require_collector_fixture_failure =
        [&require_failure, &negative_root](
            std::string_view filename,
            std::string_view diagnostic_id) {
            require_failure(
                [&negative_root, filename] {
                    static_cast<void>(collect_fixture_sources({
                        negative_root / filename,
                    }));
                },
                diagnostic_id);
        };
    constexpr std::string_view provenance_name_decoys = R"CPP(
export module decoy.guc_registration_access;
import decoy.relation_entity_handle;
export import decoy.postgres_enum_registration;
)CPP";
    const clang_structural::translation_unit provenance_name_decoy_unit =
        clang_structural::scan_translation_unit(
            "provenance_name_decoys.cppm",
            provenance_name_decoys);
    require(
        !contains_structural_terminal_identifier(
            provenance_name_decoy_unit, "relation_entity_handle")
        && !contains_structural_terminal_identifier(
            provenance_name_decoy_unit, "guc_registration_access")
        && !contains_structural_terminal_identifier(
            provenance_name_decoy_unit, "postgres_enum_registration"),
        "module and import names do not satisfy provenance use checks");
    require_collector_fixture_failure(
        "invalid_nested_annotation_namespace.cpp",
        "BIOSEQ_SQL_E_ATTRIBUTE_NAMESPACE");
    require_collector_fixture_failure(
        "invalid_class_head_placement.cppm",
        "BIOSEQ_SQL_E_CLASS_HEAD_PLACEMENT");
    require_collector_fixture_failure(
        "invalid_guc_enumerator_placement.cppm",
        "BIOSEQ_SQL_E_GUC_ENUMERATOR_PLACEMENT");
    require_collector_fixture_failure(
        "invalid_mixed_guc_alias_initialization.cppm",
        "BIOSEQ_SQL_E_MIXED_INITIALIZER");
    require_collector_fixture_failure(
        "authored_migration_install_owner.cppm",
        "BIOSEQ_SQL_E_MIGRATION_OWNER_PROVENANCE");
    require_failure(
        [&negative_root] {
            static_cast<void>(collect_provenance_checked_fixture(
                negative_root
                    / "authored_catalog_handle_specialization.cppm",
                {}));
        },
        "BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE");
    require_failure(
        [&negative_root] {
            static_cast<void>(collect_provenance_checked_fixture(
                negative_root
                    / "authored_guc_registration_access.cppm",
                {}));
        },
        "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE");
    require_failure(
        [&negative_root] {
            static_cast<void>(collect_provenance_checked_fixture(
                negative_root
                    / "authored_catalog_handle_namespace_alias.cppm",
                {}));
        },
        "BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE");
    require_failure(
        [&negative_root] {
            static_cast<void>(collect_provenance_checked_fixture(
                negative_root
                    / "authored_guc_access_using_declaration.cppm",
                {}));
        },
        "BIOSEQ_SQL_E_GUC_ACCESS_PROVENANCE");
    require_failure(
        [&negative_root] {
            static_cast<void>(collect_provenance_checked_fixture(
                negative_root
                    / "authored_guc_registration_type_alias.cppm",
                {}));
        },
        "BIOSEQ_SQL_E_GUC_REGISTRATION_PROVENANCE");

    constexpr std::string_view procedural =
        "-- complain if script is sourced in psql, rather than via CREATE EXTENSION\n"
        "\\echo Use \"CREATE EXTENSION bioseq\" to load this file. \\quit\n"
        "CREATE FUNCTION bioseq.f() RETURNS void LANGUAGE plpgsql "
        "AS $$ BEGIN NULL; END $$;\n";
    require_failure(
        [procedural] {
            install_script script = parse_install_script(procedural);
            validate_install_script(script);
        },
        "PL/pgSQL");

    run_sql_renderer_contract_tests();
    run_fmgr_renderer_contract_tests();
    run_compact_fmgr_contract_tests();
}

}  // namespace bioseq::codegen::sql
