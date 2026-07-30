module;

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module bioseq.codegen.sql.renderer:fmgr_contract_tests;

import :fmgr;
import :model;
import :sql;
import bioseq.codegen.support;
import bioseq.codegen.sql.collector;
import bioseq.codegen.sql.graph;

export namespace bioseq::codegen::sql {

namespace fmgr_contract_test_detail {

inline void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error{
            "fmgr renderer contract failed: " + std::string{message}};
    }
}

[[nodiscard]] inline normalized_entity routine(
        std::string identity,
        std::string cpp_name,
        std::string name,
        std::vector<routine_parameter> parameters,
        std::string cpp_result,
        std::string sql_result) {
    return normalized_entity{
        .source = collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = std::move(name),
            .cpp_qualified_name = std::move(cpp_name),
            .source_module = "bioseq.fixture",
            .source_path = "fixture.cppm",
            .source_range = {
                .begin = {.line = 10},
                .end = {.line = 10},
            },
            .schema = "$extension",
            .cpp_return_type = std::move(cpp_result),
            .parameters = std::move(parameters),
            .result_sql_type = std::move(sql_result),
        },
        .stable_identity = identity,
        .installed_identity = std::move(identity),
    };
}

[[nodiscard]] inline option_value option(
        std::string name,
        std::string expression) {
    return {
        .name = std::move(name),
        .expression = std::move(expression),
    };
}

struct owner_pair final {
    collected_entity natural;
    collected_entity install;
};

[[nodiscard]] inline owner_pair matching_owner_pair() {
    return {
        .natural = collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = "probe",
            .cpp_qualified_name = "fixture::probe",
            .source_module = "bioseq.fixture",
            .source_path = "fixture.cppm",
            .schema = "$extension",
            .cpp_return_type = "std::int32_t",
            .options = {
                option(
                    "create",
                    "bioseq::sql::create_mode::or_replace"),
                option(
                    "volatility",
                    "bioseq::sql::volatility::immutable"),
                option(
                    "parallel",
                    "bioseq::sql::parallel_safety::safe"),
                option(
                    "security",
                    "bioseq::sql::security_mode::invoker"),
                option("cost", "7.0"),
                option("rows", "3.0"),
                option("has_cost", "true"),
                option("has_rows", "true"),
                option("support", "\"probe_support\""),
            },
            .parameters = {
                routine_parameter{
                    .name = "value",
                    .cpp_type = "std::int32_t",
                    .sql_type = "integer",
                },
            },
            .result_sql_type = "integer",
        },
        .install = collected_entity{
            .kind = entity_kind::function,
            .annotation_name = "function",
            .name = "statement_probe",
            .source_module = "bioseq.install_owner",
            .source_path = "install_owner.cppm",
            .initializer_expression =
                "bioseq::sql::checked_sql{\"CREATE FUNCTION probe;\"}",
            .options = {
                option("sql_identity", "\"probe(integer)\""),
                option("create_or_replace", "true"),
                option(
                    "language",
                    "bioseq::sql::migration_routine_language::c"),
                option("c_symbol", "\"probe\""),
                option(
                    "argument_declarations",
                    "\"value integer\""),
                option("returns", "\"integer\""),
                option(
                    "volatility_value",
                    "bioseq::sql::migration_volatility::immutable"),
                option(
                    "null_input",
                    "bioseq::sql::migration_null_input_policy::strict"),
                option(
                    "security",
                    "bioseq::sql::migration_security_mode::invoker"),
                option(
                    "parallel",
                    "bioseq::sql::migration_parallel_safety::safe"),
                option("configuration", "\"none\""),
                option("cost", "\"7\""),
                option("rows", "\"3\""),
                option("support", "\"probe_support\""),
                option("baseline_physical_id", "\"P900\""),
            },
            .typed_install_owner = true,
        },
    };
}

[[nodiscard]] inline collected_result_row reaper_result_row() {
    return collected_result_row{
        .name = "reaper_event",
        .cpp_qualified_name = "bioseq::storage::reaper_event",
        .source_module = "bioseq.storage.manager.sql_rows",
        .source_path = "storage_pg/sql_rows.cppm",
        .fields = {
            collected_result_field{
                .name = "action",
                .cpp_type = "std::string",
                .sql_type = "text",
            },
            collected_result_field{
                .name = "target",
                .cpp_type = "std::string",
                .sql_type = "text",
            },
        },
    };
}

[[nodiscard]] inline collected_result_row diagnostics_result_row() {
    return collected_result_row{
        .name = "teardown_diagnostics_row",
        .cpp_qualified_name =
            "bioseq::table::teardown_diagnostics_row",
        .source_module = "bioseq.table.sql_rows",
        .source_path = "table/sql_rows.cppm",
        .fields = {
            collected_result_field{
                .name = "lock_fallback_classifications",
                .cpp_type = "std::int64_t",
                .sql_type = "bigint",
            },
            collected_result_field{
                .name = "missing_relation_observations",
                .cpp_type = "std::int64_t",
                .sql_type = "bigint",
            },
        },
    };
}

inline void set_option(
    collected_entity& entity,
    std::string_view name,
    std::string expression);

[[nodiscard]] inline owner_pair matching_row_owner_pair(
        routine_result_form form) {
    owner_pair result = matching_owner_pair();
    result.natural.parameters.clear();
    set_option(result.natural, "has_cost", "false");
    set_option(result.natural, "has_rows", "false");
    set_option(result.natural, "support", "\"none\"");
    set_option(result.install, "cost", "\"none\"");
    set_option(result.install, "rows", "\"none\"");
    set_option(result.install, "support", "\"none\"");
    set_option(
        result.install,
        "null_input",
        "bioseq::sql::migration_null_input_policy::called");

    if (form == routine_result_form::returns_table) {
        result.natural.name = "reap_orphan_seq_sub_tables";
        result.natural.cpp_qualified_name =
            "bioseq::storage::reap_orphan_seq_sub_tables";
        result.natural.cpp_return_type =
            "bioseq::pg_utils::returns_table<"
            "bioseq::storage::reaper_event>";
        result.natural.result_sql_type =
            "table (action text, target text)";
        result.natural.result = routine_result_contract{
            .form = routine_result_form::returns_table,
            .cpp_type = "bioseq::storage::reaper_event",
            .sql_type = "table (action text, target text)",
            .row_cpp_name = "bioseq::storage::reaper_event",
        };
        set_option(
            result.install,
            "sql_identity",
            "\"reap_orphan_seq_sub_tables()\"");
        set_option(
            result.install,
            "c_symbol",
            "\"reap_orphan_seq_sub_tables\"");
        set_option(
            result.install,
            "argument_declarations",
            "\"none\"");
        set_option(
            result.install,
            "returns",
            "\"table (action text, target text)\"");
    } else {
        result.natural.name = "teardown_diagnostics";
        result.natural.cpp_qualified_name =
            "bioseq::table::teardown_diagnostics";
        result.natural.cpp_return_type =
            "bioseq::pg_utils::named_out<"
            "bioseq::table::teardown_diagnostics_row>";
        result.natural.result_sql_type = "record";
        result.natural.result = routine_result_contract{
            .form = routine_result_form::named_out,
            .cpp_type =
                "bioseq::table::teardown_diagnostics_row",
            .sql_type = "record",
            .row_cpp_name =
                "bioseq::table::teardown_diagnostics_row",
        };
        set_option(
            result.install,
            "sql_identity",
            "\"teardown_diagnostics()\"");
        set_option(
            result.install,
            "c_symbol",
            "\"teardown_diagnostics\"");
        set_option(
            result.install,
            "argument_declarations",
            "\"out lock_fallback_classifications bigint, "
            "out missing_relation_observations bigint\"");
        set_option(result.install, "returns", "\"record\"");
    }
    return result;
}

inline void set_option(
        collected_entity& entity,
        std::string_view name,
        std::string expression) {
    for (option_value& value : entity.options) {
        if (value.name == name) {
            value.expression = std::move(expression);
            return;
        }
    }
    entity.options.push_back(option(
        std::string{name}, std::move(expression)));
}

[[nodiscard]] inline owner_pair matching_sql_owner_pair() {
    return {
        .natural = collected_entity{
            .kind = entity_kind::sql_function,
            .annotation_name = "sql_function",
            .name = "sql_probe",
            .cpp_qualified_name = "fixture::sql_probe",
            .source_module = "bioseq.fixture",
            .source_path = "fixture.cppm",
            .schema = "$extension",
            .cpp_return_type =
                "bioseq::sql::sql_function_body<std::int32_t>",
            .initializer_expression = R"CPP(
                static_cast<void>(value);
                return {
                    .body = bioseq::sql::checked_sql{
                        R"SQL(select value+1)SQL"},
                };
            )CPP",
            .options = {
                option(
                    "create",
                    "bioseq::sql::create_mode::or_replace"),
                option(
                    "volatility",
                    "bioseq::sql::volatility::immutable"),
                option(
                    "parallel",
                    "bioseq::sql::parallel_safety::safe"),
                option(
                    "security",
                    "bioseq::sql::security_mode::invoker"),
            },
            .parameters = {
                routine_parameter{
                    .name = "value",
                    .cpp_type = "std::int32_t",
                    .sql_type = "integer",
                },
            },
            .result_sql_type = "integer",
        },
        .install = collected_entity{
            .kind = entity_kind::sql_function,
            .annotation_name = "sql_function",
            .name = "statement_sql_probe",
            .source_module = "bioseq.install_owner",
            .source_path = "install_owner.cppm",
            .initializer_expression = R"CPP(
                bioseq::sql::checked_sql{R"BASE(
                    CREATE OR REPLACE FUNCTION sql_probe(value integer)
                    RETURNS integer LANGUAGE SQL AS $$
                        SELECT value + 1; -- canonical commentary
                    $$ IMMUTABLE PARALLEL SAFE;
                )BASE"}
            )CPP",
            .options = {
                option("sql_identity", "\"sql_probe(integer)\""),
                option("create_or_replace", "true"),
                option(
                    "language",
                    "bioseq::sql::migration_routine_language::sql"),
                option("c_symbol", "\"none\""),
                option(
                    "argument_declarations",
                    "\"value integer\""),
                option("returns", "\"integer\""),
                option(
                    "volatility_value",
                    "bioseq::sql::migration_volatility::immutable"),
                option(
                    "null_input",
                    "bioseq::sql::migration_null_input_policy::strict"),
                option(
                    "security",
                    "bioseq::sql::migration_security_mode::invoker"),
                option(
                    "parallel",
                    "bioseq::sql::migration_parallel_safety::safe"),
                option("configuration", "\"none\""),
                option("cost", "\"none\""),
                option("rows", "\"none\""),
                option("support", "\"none\""),
                option("baseline_physical_id", "\"P904\""),
            },
            .typed_install_owner = true,
        },
    };
}

[[nodiscard]] inline collected_result_row storage_shard_result_row() {
    return collected_result_row{
        .name = "list_seq_storage_shard",
        .cpp_qualified_name =
            "bioseq::sql_functions::result_rows::"
            "list_seq_storage_shard",
        .source_module = "bioseq.sql.functions.sql_rows",
        .source_path = "pg/sql/functions_sql_rows.cppm",
        .fields = {
            collected_result_field{
                .name = "sub_relation",
                .cpp_type = "std::uint32_t",
                .sql_type = "regclass",
            },
            collected_result_field{
                .name = "sidecar_relation",
                .cpp_type = "std::uint32_t",
                .sql_type = "regclass",
            },
            collected_result_field{
                .name = "column_attno",
                .cpp_type = "std::int16_t",
                .sql_type = "smallint",
            },
            collected_result_field{
                .name = "bucket_idx",
                .cpp_type = "std::int16_t",
                .sql_type = "smallint",
            },
            collected_result_field{
                .name = "shard_idx",
                .cpp_type = "std::int32_t",
                .sql_type = "integer",
            },
            collected_result_field{
                .name = "bytes_written",
                .cpp_type = "std::int64_t",
                .sql_type = "bigint",
            },
            collected_result_field{
                .name = "is_full",
                .cpp_type = "bool",
                .sql_type = "boolean",
            },
        },
    };
}

[[nodiscard]] inline owner_pair matching_storage_shard_sql_owner_pair() {
    constexpr std::string_view body =
        "SELECT rel::regclass AS sub_relation, "
        "rel::regclass AS sidecar_relation, "
        "0::int2 AS column_attno, 0::int2 AS bucket_idx, "
        "0::int4 AS shard_idx, 0::bigint AS bytes_written, "
        "false AS is_full";
    const std::string row_cpp_name =
        "bioseq::sql_functions::result_rows::"
        "list_seq_storage_shard";
    const std::string natural_return_sql =
        "table (sub_relation regclass, sidecar_relation regclass, "
        "column_attno smallint, bucket_idx smallint, shard_idx integer, "
        "bytes_written bigint, is_full boolean)";
    const std::string install_return_sql =
        "table (sub_relation regclass, sidecar_relation regclass, "
        "column_attno int2, bucket_idx int2, shard_idx int4, "
        "bytes_written bigint, is_full bool)";
    return {
        .natural = collected_entity{
            .kind = entity_kind::sql_function,
            .annotation_name = "sql_function",
            .name = "list_seq_storage_shards",
            .cpp_qualified_name =
                "bioseq::sql_functions::list_seq_storage_shards",
            .source_module = "bioseq.sql.functions",
            .source_path = "pg/sql/functions.cppm",
            .schema = "bioseq",
            .cpp_return_type =
                "bioseq::sql::sql_function_body<"
                "bioseq::pg_utils::returns_table<"
                + row_cpp_name + ">>",
            .initializer_expression =
                "return {.body = bioseq::sql::checked_sql{R\"SQL("
                + std::string{body} + ")SQL\"},};",
            .options = {
                option(
                    "create",
                    "bioseq::sql::create_mode::or_replace"),
                option(
                    "volatility",
                    "bioseq::sql::volatility::stable"),
            },
            .parameters = {
                routine_parameter{
                    .name = "rel",
                    .cpp_type = "std::optional<std::uint32_t>",
                    .sql_type = "regclass",
                    .nullable = true,
                },
            },
            .result_sql_type = natural_return_sql,
            .result = routine_result_contract{
                .form = routine_result_form::returns_table,
                .cpp_type = row_cpp_name,
                .sql_type = natural_return_sql,
                .row_cpp_name = row_cpp_name,
            },
        },
        .install = collected_entity{
            .kind = entity_kind::sql_function,
            .annotation_name = "sql_function",
            .name = "statement_p064",
            .source_module = "bioseq.install_owner",
            .source_path = "install_owner.cppm",
            .initializer_expression =
                "bioseq::sql::checked_sql{R\"BASE("
                "CREATE OR REPLACE FUNCTION "
                "bioseq.list_seq_storage_shards(rel regclass) "
                "RETURNS " + install_return_sql
                + " LANGUAGE SQL STABLE AS $$"
                + std::string{body} + ";$$;)BASE\"}",
            .options = {
                option(
                    "sql_identity",
                    "\"bioseq.list_seq_storage_shards(regclass)\""),
                option("create_or_replace", "true"),
                option(
                    "language",
                    "bioseq::sql::migration_routine_language::sql"),
                option("c_symbol", "\"none\""),
                option(
                    "argument_declarations",
                    "\"rel regclass\""),
                option(
                    "returns",
                    "\"" + install_return_sql + "\""),
                option(
                    "volatility_value",
                    "bioseq::sql::migration_volatility::stable"),
                option(
                    "null_input",
                    "bioseq::sql::migration_null_input_policy::called"),
                option(
                    "security",
                    "bioseq::sql::migration_security_mode::invoker"),
                option(
                    "parallel",
                    "bioseq::sql::migration_parallel_safety::unsafe"),
                option("configuration", "\"none\""),
                option("cost", "\"none\""),
                option("rows", "\"none\""),
                option("support", "\"none\""),
                option("baseline_physical_id", "\"P906\""),
            },
            .typed_install_owner = true,
        },
    };
}

[[nodiscard]] inline owner_pair matching_aligner_options_sql_owner_pair() {
    constexpr std::array<std::string_view, 36> names{
        "kmer_size",
        "window_size",
        "max_gap",
        "max_gap_ref",
        "bandwidth",
        "bandwidth_long",
        "max_chain_skip",
        "max_chain_iter",
        "min_seed_count",
        "min_chain_score",
        "chain_gap_scale",
        "chain_skip_scale",
        "mask_level",
        "mask_len",
        "pri_ratio",
        "best_n",
        "match_score",
        "mismatch_pen",
        "gap_open1",
        "gap_ext1",
        "gap_open2",
        "gap_ext2",
        "sc_ambi",
        "zdrop",
        "end_bonus",
        "max_occ",
        "mid_occ",
        "block_min_size",
        "block_max_size",
        "chaining_algorithm",
        "base_alignment_algorithm",
        "minimizer_algorithm",
        "vulkan_enabled",
        "vulkan_seed_hits_enabled",
        "vulkan_index_cache_enabled",
        "index_read_requirement",
    };
    std::vector<routine_parameter> parameters;
    parameters.reserve(names.size());
    std::string identity{"aligner_options("};
    std::string declarations;
    std::string row_values;
    for (std::size_t index = 0; index < names.size(); ++index) {
        const bool floating =
            (index >= 10 && index <= 12) || index == 14;
        const bool boolean = index >= 32 && index <= 34;
        const std::string sql_type = floating
            ? "double precision"
            : boolean ? "boolean" : "integer";
        const std::string cpp_type = floating
            ? "std::optional<double>"
            : boolean
            ? "std::optional<bool>"
            : "std::optional<std::int32_t>";
        if (index != 0) {
            identity += ",";
            declarations += ", ";
            row_values += ", ";
        }
        identity += sql_type;
        declarations += std::string{names[index]} + " "
            + sql_type + " default null";
        row_values += names[index];
        parameters.push_back(routine_parameter{
            .name = std::string{names[index]},
            .cpp_type = cpp_type,
            .sql_type = sql_type,
            .default_expression = "std::nullopt",
            .has_default = true,
            .nullable = true,
        });
    }
    identity += ")";
    const std::string body =
        "SELECT " + row_values;
    return {
        .natural = collected_entity{
            .kind = entity_kind::sql_function,
            .annotation_name = "sql_function",
            .name = "aligner_options",
            .cpp_qualified_name =
                "bioseq::sql_functions::aligner_options",
            .source_module = "bioseq.sql.functions",
            .source_path = "pg/sql/functions.cppm",
            .schema = "$extension",
            .cpp_return_type =
                "bioseq::sql::sql_function_body<"
                "bioseq::table::composite_rows::"
                "bioseq_aligner_options>",
            .initializer_expression =
                "return {.body = bioseq::sql::checked_sql{R\"SQL("
                + body + ")SQL\"},};",
            .options = {
                option(
                    "create",
                    "bioseq::sql::create_mode::or_replace"),
                option(
                    "volatility",
                    "bioseq::sql::volatility::immutable"),
            },
            .parameters = std::move(parameters),
            .result_sql_type = "bioseq_aligner_options",
            .result = routine_result_contract{
                .form = routine_result_form::scalar,
                .cpp_type =
                    "bioseq::table::composite_rows::"
                    "bioseq_aligner_options",
                .sql_type = "bioseq_aligner_options",
            },
        },
        .install = collected_entity{
            .kind = entity_kind::sql_function,
            .annotation_name = "sql_function",
            .name = "statement_p376",
            .source_module = "bioseq.install_owner",
            .source_path = "install_owner.cppm",
            .initializer_expression =
                "bioseq::sql::checked_sql{R\"BASE("
                "CREATE OR REPLACE FUNCTION aligner_options("
                + declarations
                + ") RETURNS bioseq_aligner_options AS $$"
                + body + "$$ LANGUAGE SQL IMMUTABLE;)BASE\"}",
            .options = {
                option("sql_identity", "\"" + identity + "\""),
                option("create_or_replace", "true"),
                option(
                    "language",
                    "bioseq::sql::migration_routine_language::sql"),
                option("c_symbol", "\"none\""),
                option(
                    "argument_declarations",
                    "\"" + declarations + "\""),
                option("returns", "\"bioseq_aligner_options\""),
                option(
                    "volatility_value",
                    "bioseq::sql::migration_volatility::immutable"),
                option(
                    "null_input",
                    "bioseq::sql::migration_null_input_policy::called"),
                option(
                    "security",
                    "bioseq::sql::migration_security_mode::invoker"),
                option(
                    "parallel",
                    "bioseq::sql::migration_parallel_safety::unsafe"),
                option("configuration", "\"none\""),
                option("cost", "\"none\""),
                option("rows", "\"none\""),
                option("support", "\"none\""),
                option("baseline_physical_id", "\"P907\""),
            },
            .typed_install_owner = true,
        },
    };
}

[[nodiscard]] inline std::size_t count_occurrences(
        std::string_view text,
        std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t cursor = 0;
         (cursor = text.find(needle, cursor)) != std::string_view::npos;
         cursor += needle.size()) {
        ++count;
    }
    return count;
}

using owner_mutation = void (*)(
    collected_entity& natural,
    collected_entity& install);

inline void require_reconciliation_rejected(
        owner_mutation mutation,
        std::string_view label) {
    owner_pair owners = matching_owner_pair();
    mutation(owners.natural, owners.install);
    collected_program program{
        .entities = {
            std::move(owners.natural),
            std::move(owners.install),
        },
    };
    bool rejected = false;
    try {
        reconcile_natural_routine_owners(program);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    require(rejected, label);
}

}  // namespace fmgr_contract_test_detail

/**
 * Exercise deterministic owner grouping, overload dispatch, nullable
 * lowering, context injection, and actual-artifact symbol provenance.
 */
inline void run_fmgr_renderer_contract_tests() {
    using fmgr_contract_test_detail::require;
    using fmgr_contract_test_detail::require_reconciliation_rejected;
    using fmgr_contract_test_detail::set_option;

    {
        normalized_entity extension_function =
            fmgr_contract_test_detail::routine(
                "sql:function:$extension.vanycount(bigint,\"any\")",
                "fixture::vanycount",
                "vanycount",
                {},
                "std::int64_t",
                "bigint");
        extension_function.installed_identity =
            "sql:function:public.vanycount(bigint,\"any\")";
        normalized_entity fixed_function =
            fmgr_contract_test_detail::routine(
                "sql:function:pg_catalog.alter_fixture("
                "regclass,integer,name)",
                "fixture::alter_fixture",
                "alter_fixture",
                {},
                "void",
                "void");
        fixed_function.source.schema = "pg_catalog";
        normalized_entity fixed_procedure =
            fmgr_contract_test_detail::routine(
                "sql:procedure:bioseq.vacuum_fixture(regclass)",
                "fixture::vacuum_fixture",
                "vacuum_fixture",
                {},
                "void",
                "void");
        fixed_procedure.source.kind = entity_kind::procedure;
        fixed_procedure.source.annotation_name = "procedure";
        fixed_procedure.source.schema = "bioseq";
        normalized_entity extension_typed_function =
            fmgr_contract_test_detail::routine(
                "sql:function:$extension.mmsearch("
                "bioseq.seq,bioseq.seq,"
                "$extension.bioseq_aligner_options)",
                "fixture::mmsearch",
                "mmsearch",
                {},
                "void",
                "void");
        normalized_entity quoted_extension_types =
            fmgr_contract_test_detail::routine(
                "sql:function:$extension.quoted_fixture("
                "$extension.alpha,$extension.\"Quoted.Type\"[],"
                "pg_catalog.name)",
                "fixture::quoted_fixture",
                "quoted_fixture",
                {},
                "void",
                "void");
        normalized_entity lookalike_identifiers =
            fmgr_contract_test_detail::routine(
                "sql:function:fixed.$extensionary_fixture("
                "fixed.$extensionary_type)",
                "fixture::extensionary_fixture",
                "$extensionary_fixture",
                {},
                "void",
                "void");
        lookalike_identifiers.source.schema = "fixed";

        const std::string extension_identity =
            fmgr_detail::relocatable_routine_identity(
                extension_function);
        require(
            extension_identity == "vanycount(bigint,\"any\")"
                && fmgr_detail::relocatable_routine_identity(
                       fixed_function)
                    == "pg_catalog.alter_fixture("
                       "regclass,integer,name)"
                && fmgr_detail::relocatable_routine_identity(
                       fixed_procedure)
                    == "bioseq.vacuum_fixture(regclass)",
            "runtime routine identity removes only graph-only prefixes");
        require(
            fmgr_detail::relocatable_routine_identity(
                extension_typed_function)
                    == "mmsearch(bioseq.seq,bioseq.seq,"
                       "bioseq_aligner_options)"
                && fmgr_detail::relocatable_routine_identity(
                       quoted_extension_types)
                    == "quoted_fixture(alpha,\"Quoted.Type\"[],"
                       "pg_catalog.name)"
                && fmgr_detail::relocatable_routine_identity(
                       lookalike_identifiers)
                    == "fixed.$extensionary_fixture("
                       "fixed.$extensionary_type)",
            "runtime routine identity removes every relocatable schema "
            "qualifier without rewriting fixed or quoted spellings");
        require(
            fmgr_detail::generated_boundary_probe_site_id(
                extension_identity)
                != fmgr_detail::generated_boundary_probe_site_id(
                    extension_function.stable_identity)
                && fmgr_detail::generated_boundary_probe_site_id(
                       extension_identity)
                    != fmgr_detail::generated_boundary_probe_site_id(
                        extension_function.installed_identity),
            "boundary probe hashing uses the relocatable identity");

        normalized_entity malformed = extension_function;
        malformed.stable_identity =
            "sql:procedure:$extension.vanycount(bigint,\"any\")";
        bool rejected_malformed_identity = false;
        try {
            static_cast<void>(
                fmgr_detail::relocatable_routine_identity(malformed));
        } catch (const std::runtime_error&) {
            rejected_malformed_identity = true;
        }
        require(
            rejected_malformed_identity,
            "runtime routine identity rejects a mismatched kind prefix");

        normalized_entity residual_marker = extension_function;
        residual_marker.stable_identity =
            "sql:function:fixed.vanycount("
            "fixed.foo$extension.bar)";
        bool rejected_residual_marker = false;
        try {
            static_cast<void>(
                fmgr_detail::relocatable_routine_identity(
                    residual_marker));
        } catch (const std::runtime_error&) {
            rejected_residual_marker = true;
        }
        require(
            rejected_residual_marker,
            "runtime routine identity rejects a non-syntactic residual "
            "extension marker");
    }

    require(
        fmgr_reconciliation_detail::find_keyword(
            "dispatch_authoritative_default_batches bigint",
            "default") == std::string_view::npos
            && fmgr_reconciliation_detail::find_keyword(
                "value integer DEFAULT 7",
                "default") != std::string_view::npos,
        "SQL keyword matching respects identifier boundaries");

    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_owner_pair();
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && !program.entities.front()
                        .compatibility_sql_initializer.empty()
                && program.entities.front()
                        .compatibility_baseline_physical_id
                    == "P900"
                && program.entities.front()
                        .physical_statements.size()
                    == 1
                && program.entities.front()
                        .physical_statements.front()
                        .baseline_physical_id
                    == "P900"
                && program.entities.front()
                        .physical_statements.front()
                        .facet
                    == physical_statement_facet::primary
                && program.entities.front()
                        .physical_statements.front()
                        .render_source
                    == physical_render_source::natural_projection,
            "matching full routine contract reconciles");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_owner_pair();
        owners.natural.name = "vanycount";
        owners.natural.cpp_qualified_name = "fixture::vanycount";
        owners.natural.parameters = {
            routine_parameter{
                .name = "value",
                .cpp_type = "std::int64_t",
                .sql_type = "bigint",
            },
            routine_parameter{
                .name = "polymorphic",
                .cpp_type = "bioseq::pg::any",
                .sql_type = "\"any\"",
            },
        };
        set_option(
            owners.install,
            "sql_identity",
            "\"vanycount(int8,\\\"any\\\")\"");
        set_option(owners.install, "c_symbol", "\"vanycount\"");
        set_option(
            owners.install,
            "argument_declarations",
            "\"value int8, polymorphic \\\"any\\\"\"");
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front().name == "vanycount"
                && program.entities.front().parameters.size() == 2,
            "escaped quoted SQL types decode before routine "
            "reconciliation");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_sql_owner_pair();
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front().kind
                    == entity_kind::sql_function
                && program.entities.front()
                       .physical_statements.size()
                    == 1
                && program.entities.front()
                       .physical_statements.front()
                       .render_source
                    == physical_render_source::natural_projection,
            "SQL-language routine body and metadata reconcile");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_sql_owner_pair();
        owners.natural.initializer_expression = R"CPP(
            return {
                .body = bioseq::sql::checked_sql{
                    R"SQL(SELECT value + 2)SQL"},
            };
        )CPP";
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
        };
        bool rejected = false;
        try {
            reconcile_natural_routine_owners(program);
        } catch (const std::runtime_error& error) {
            rejected =
                std::string_view{error.what()}.contains("SQL body");
        }
        require(
            rejected,
            "SQL-language routine body mismatch is rejected");
    }
    {
        const collected_entity install{
            .initializer_expression = R"CPP(
                bioseq::sql::checked_sql{R"OWNER(
                    CREATE FUNCTION tricky_sql(
                        value text DEFAULT $default$not the body$default$,
                        escaped text DEFAULT
                            E'prefix \' $fake$not the body$fake$ suffix'
                    )
                    RETURNS text
                    /* outer /* nested */ $comment$not the body$comment$ */
                    LANGUAGE SQL
                    AS /* body delimiter */ $actual$
                        SELECT E'it\'s; -- string data';
                    $actual$;
                )OWNER"}
            )CPP",
        };
        const std::string body =
            fmgr_reconciliation_detail::typed_sql_function_body(
                install, "tricky_sql(text,text)");
        const std::vector<std::string> tokens =
            fmgr_reconciliation_detail::canonical_sql_body_tokens(body);
        require(
            tokens
                == std::vector<std::string>{
                    "select",
                    "e",
                    R"SQL('it\'s; -- string data')SQL",
                }
                && !body.contains("not the body"),
            "SQL body extraction anchors to top-level AS across "
            "dollar defaults, E strings, and nested comments");
    }
    {
        const std::vector<std::string> terminated =
            fmgr_reconciliation_detail::canonical_sql_body_tokens(
                "SELECT 1;");
        const std::vector<std::string> unterminated =
            fmgr_reconciliation_detail::canonical_sql_body_tokens(
                "SELECT 1");
        const std::vector<std::string> two_statements =
            fmgr_reconciliation_detail::canonical_sql_body_tokens(
                "SELECT 1; SELECT 2;");
        const std::vector<std::string> fused_statements =
            fmgr_reconciliation_detail::canonical_sql_body_tokens(
                "SELECT 1 SELECT 2;");
        require(
            terminated == unterminated
                && two_statements != fused_statements,
            "SQL body canonicalization ignores only one trailing "
            "terminator and preserves internal statement boundaries");
    }
    {
        normalized_entity collision_probe{
            .source = collected_entity{
                .kind = entity_kind::sql_function,
                .annotation_name = "sql_function",
                .name = "delimiter_probe",
                .cpp_qualified_name = "fixture::delimiter_probe",
                .source_module = "bioseq.fixture",
                .source_path = "fixture.cppm",
                .schema = "$extension",
                .cpp_return_type =
                    "bioseq::sql::sql_function_body<std::string>",
                .initializer_expression = R"CPP(
                    return {
                        .body = bioseq::sql::checked_sql{
                            R"BODY(SELECT '$sql$', '$bioseq_sql$')BODY"},
                    };
                )CPP",
                .result_sql_type = "text",
                .result = routine_result_contract{
                    .form = routine_result_form::scalar,
                    .cpp_type = "std::string",
                    .sql_type = "text",
                },
            },
            .stable_identity =
                "sql:sql_function:$extension.delimiter_probe()",
            .installed_identity =
                "sql:sql_function:public.delimiter_probe()",
        };
        const sql_graph graph{.extension_schema = "public"};
        const std::string rendered =
            detail::render_entity_statement(collision_probe, graph, {});
        require(
            rendered.contains("AS $bioseq_sql_1$\n")
                && fmgr_contract_test_detail::count_occurrences(
                    rendered, "$bioseq_sql_1$") == 2,
            "SQL function renderer selects a collision-free "
            "dollar-quote delimiter");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::
                matching_storage_shard_sql_owner_pair();
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
            .result_rows = {
                fmgr_contract_test_detail::storage_shard_result_row(),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front().result.form
                    == routine_result_form::returns_table,
            "production-shaped SQL RETURNS TABLE contract reconciles");
        normalized_entity normalized{
            .source = program.entities.front(),
            .stable_identity =
                "sql:sql_function:bioseq."
                "list_seq_storage_shards(regclass)",
            .installed_identity =
                "sql:sql_function:bioseq."
                "list_seq_storage_shards(regclass)",
        };
        const sql_graph graph{.extension_schema = "public"};
        const std::string rendered =
            detail::render_entity_statement(normalized, graph, {});
        require(
            rendered.contains(
                "FUNCTION bioseq.list_seq_storage_shards(rel regclass)")
                && rendered.contains(
                    "RETURNS table (sub_relation regclass, "
                    "sidecar_relation regclass, column_attno smallint, "
                    "bucket_idx smallint, shard_idx integer, "
                    "bytes_written bigint, is_full boolean)")
                && rendered.contains("\nSTABLE")
                && !rendered.contains("COMMENT ON"),
            "production-shaped SQL RETURNS TABLE rendering preserves "
            "the seven-column result and creates no implicit comment");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::
                matching_aligner_options_sql_owner_pair();
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front().parameters.size() == 36,
            "production-shaped aligner_options contract reconciles all "
            "36 nullable defaults");
        normalized_entity normalized{
            .source = program.entities.front(),
            .stable_identity =
                "sql:sql_function:$extension.aligner_options",
            .installed_identity =
                "sql:sql_function:public.aligner_options",
        };
        const sql_graph graph{.extension_schema = "public"};
        const std::string rendered =
            detail::render_entity_statement(normalized, graph, {});
        require(
            rendered.contains("FUNCTION aligner_options(")
                && !rendered.contains(
                    "FUNCTION public.aligner_options(")
                && fmgr_contract_test_detail::count_occurrences(
                    rendered, " DEFAULT NULL") == 36
                && rendered.contains(
                    "\nRETURNS bioseq_aligner_options")
                && rendered.contains(
                    "\nSELECT kmer_size, window_size, max_gap")
                && !rendered.contains("SELECT ROW(")
                && !rendered.contains("::bioseq_aligner_options")
                && rendered.contains("\nIMMUTABLE")
                && !rendered.contains("\nSTRICT")
                && !rendered.contains("COMMENT ON"),
            "production-shaped aligner_options rendering preserves all "
            "36 defaults, schema-neutral composite projection, CALLED "
            "semantics, and no implicit comment");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_owner_pair();
        owners.natural.kind = entity_kind::procedure;
        owners.natural.annotation_name = "procedure";
        owners.natural.cpp_return_type = "void";
        owners.natural.result_sql_type = "void";
        owners.natural.result = routine_result_contract{
            .form = routine_result_form::void_,
            .cpp_type = "void",
            .sql_type = "void",
        };
        owners.install.kind = entity_kind::procedure;
        owners.install.annotation_name = "procedure";
        set_option(owners.install, "returns", "\"void\"");
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front().kind
                    == entity_kind::procedure
                && program.entities.front().result.form
                    == routine_result_form::void_,
            "procedure void result reconciles without a RETURNS clause");
        normalized_entity normalized{
            .source = program.entities.front(),
            .stable_identity = "sql:procedure:$extension.probe(integer)",
            .installed_identity = "sql:procedure:public.probe(integer)",
        };
        const sql_graph graph{.extension_schema = "public"};
        const std::string rendered =
            detail::render_entity_statement(normalized, graph, {});
        require(
            rendered.contains("PROCEDURE probe(value integer)")
                && rendered.contains("\nSECURITY INVOKER")
                && !rendered.contains("\nVOLATILE")
                && !rendered.contains("\nIMMUTABLE")
                && !rendered.contains("\nPARALLEL")
                && !rendered.contains("\nSTRICT")
                && !rendered.contains("\nCOST")
                && !rendered.contains("\nROWS")
                && !rendered.contains("\nSUPPORT"),
            "procedure rendering omits function-only attributes");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_owner_pair();
        fmgr_contract_test_detail::set_option(
            owners.install,
            "argument_declarations",
            "\"integer\"");
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front().parameters.size() == 1
                && program.entities.front().parameters.front().name
                    == "value",
            "unnamed baseline arguments adopt natural parameter names");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_owner_pair();
        fmgr_contract_test_detail::set_option(
            owners.install,
            "sql_identity",
            "\"bioseq.reap_orphans(name integer)\"");
        collected_program program{
            .entities = {std::move(owners.install)},
        };
        bool rejected = false;
        try {
            reconcile_natural_routine_owners(program);
        } catch (const std::runtime_error& error) {
            rejected = std::string_view{error.what()}.contains(
                "sql_identity inconsistent with "
                "argument_declarations");
        }
        require(
            rejected,
            "transition-only routine identities are checked against "
            "their authored argument declarations");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_owner_pair();
        owners.natural.documentation.has_explicit_brief = true;
        owners.natural.documentation.brief =
            "Describes the probe function.";
        collected_entity comment{
            .kind = entity_kind::comment,
            .annotation_name = "comment",
            .name = "statement_probe_comment",
            .source_module = "bioseq.install_owner",
            .source_path = "install_owner.cppm",
            .initializer_expression =
                "bioseq::sql::checked_sql{\"COMMENT ON FUNCTION "
                "probe(integer) IS 'legacy';\"}",
            .options = {
                fmgr_contract_test_detail::option(
                    "sql_identity",
                    "\"comment on function probe(int4) is 'legacy';\""),
                fmgr_contract_test_detail::option(
                    "baseline_physical_id", "\"P901\""),
            },
            .typed_install_owner = true,
        };
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(comment),
            },
        };
        detail::reconcile_natural_comment_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front()
                       .physical_statements.size()
                    == 1
                && program.entities.front()
                       .physical_statements.front()
                       .facet
                    == physical_statement_facet::catalog_comment
                && program.entities.front()
                       .physical_statements.front()
                       .render_source
                    == physical_render_source::natural_projection,
            "canonical function comment targets reconcile to the natural "
            "Doxygen owner");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_owner_pair();
        owners.natural.documentation.has_explicit_brief = true;
        owners.natural.documentation.brief =
            "Describes the probe function.";
        const auto make_comment = [](std::string name,
                                     std::string physical_id) {
            return collected_entity{
                .kind = entity_kind::comment,
                .annotation_name = "comment",
                .name = std::move(name),
                .source_module = "bioseq.install_owner",
                .source_path = "install_owner.cppm",
                .initializer_expression =
                    "bioseq::sql::checked_sql{\"COMMENT ON FUNCTION "
                    "probe(integer) IS 'legacy';\"}",
                .options = {
                    fmgr_contract_test_detail::option(
                        "sql_identity",
                        "\"comment on function probe(integer) "
                        "is 'legacy';\""),
                    fmgr_contract_test_detail::option(
                        "baseline_physical_id",
                        "\"" + physical_id + "\""),
                },
                .typed_install_owner = true,
            };
        };
        collected_program program{
            .entities = {
                std::move(owners.natural),
                make_comment("comment_one", "P902"),
                make_comment("comment_two", "P903"),
            },
        };
        bool rejected = false;
        try {
            detail::reconcile_natural_comment_owners(program);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(
            rejected,
            "duplicate typed comments cannot claim one natural target");
    }

    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_row_owner_pair(
                routine_result_form::returns_table);
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
            .result_rows = {
                fmgr_contract_test_detail::reaper_result_row(),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front().result.form
                    == routine_result_form::returns_table,
            "RETURNS TABLE columns reconcile independently of inputs");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_row_owner_pair(
                routine_result_form::named_out);
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
            .result_rows = {
                fmgr_contract_test_detail::diagnostics_result_row(),
            },
        };
        reconcile_natural_routine_owners(program);
        require(
            program.entities.size() == 1
                && program.entities.front().parameters.empty()
                && program.entities.front().result.form
                    == routine_result_form::named_out,
            "named OUT columns are excluded from routine identity inputs");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_row_owner_pair(
                routine_result_form::named_out);
        fmgr_contract_test_detail::set_option(
            owners.install,
            "argument_declarations",
            "\"out missing_relation_observations bigint, "
            "out lock_fallback_classifications bigint\"");
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
            .result_rows = {
                fmgr_contract_test_detail::diagnostics_result_row(),
            },
        };
        bool rejected = false;
        try {
            reconcile_natural_routine_owners(program);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(
            rejected,
            "named OUT result column order is contractual");
    }
    {
        fmgr_contract_test_detail::owner_pair owners =
            fmgr_contract_test_detail::matching_row_owner_pair(
                routine_result_form::returns_table);
        fmgr_contract_test_detail::set_option(
            owners.install,
            "returns",
            "\"table (action text, target bigint)\"");
        collected_program program{
            .entities = {
                std::move(owners.natural),
                std::move(owners.install),
            },
            .result_rows = {
                fmgr_contract_test_detail::reaper_result_row(),
            },
        };
        bool rejected = false;
        try {
            reconcile_natural_routine_owners(program);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        require(
            rejected,
            "RETURNS TABLE result column types are contractual");
    }

    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            natural.kind = entity_kind::procedure;
        },
        "function/procedure mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity&, collected_entity& install) {
            set_option(
                install,
                "language",
                "bioseq::sql::migration_routine_language::sql");
        },
        "routine language mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity&, collected_entity& install) {
            set_option(install, "c_symbol", "\"other_probe\"");
        },
        "C symbol mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(
                natural,
                "create",
                "bioseq::sql::create_mode::create");
        },
        "create mode mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(
                natural,
                "volatility",
                "bioseq::sql::volatility::stable");
        },
        "volatility mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(
                natural,
                "parallel",
                "bioseq::sql::parallel_safety::restricted");
        },
        "parallel mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(
                natural,
                "security",
                "bioseq::sql::security_mode::definer");
        },
        "security mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(natural, "leakproof", "true");
        },
        "leakproof mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(natural, "cost", "8.0");
        },
        "cost mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(natural, "rows", "4.0");
        },
        "rows mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(natural, "support", "\"other_support\"");
        },
        "support mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            set_option(
                natural,
                "configuration",
                "bioseq::sql::checked_sql{\"SET search_path = "
                "pg_catalog\"}");
        },
        "SET configuration mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity&, collected_entity& install) {
            set_option(
                install,
                "argument_declarations",
                "\"other integer\"");
        },
        "parameter name mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity&, collected_entity& install) {
            set_option(
                install,
                "argument_declarations",
                "\"out value integer\"");
        },
        "parameter mode mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity&, collected_entity& install) {
            set_option(
                install,
                "argument_declarations",
                "\"value bigint\"");
        },
        "parameter type mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity& install) {
            set_option(
                install,
                "argument_declarations",
                "\"value integer default 7\"");
            natural.parameters.front().has_default = true;
            natural.parameters.front().default_expression = "8";
        },
        "parameter default mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity&) {
            natural.parameters.front().nullable = true;
        },
        "parameter nullability mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity& natural, collected_entity& install) {
            natural.parameters.front().nullable = true;
            set_option(
                install,
                "null_input",
                "bioseq::sql::migration_null_input_policy::called");
            natural.parameters.front().nullable = false;
        },
        "derived STRICT/CALLED mismatch is rejected");
    require_reconciliation_rejected(
        [](collected_entity&, collected_entity& install) {
            set_option(install, "returns", "\"bigint\"");
        },
        "result form/type mismatch is rejected");

    {
        const auto date_argument = fmgr_detail::scalar_converter(
            "bioseq::pg_utils::vector_operand<"
            "bioseq::sql::pg::date, DateADT>",
            "date",
            false);
        const auto character_argument = fmgr_detail::scalar_converter(
            "bioseq::pg_utils::vector_operand<"
            "bioseq::sql::pg::character, "
            "bioseq::pg_utils::bpchar_view>",
            "char",
            false);
        const auto boolean_result = fmgr_detail::scalar_converter(
            "bioseq::pg_utils::vector_result<"
            "bioseq::sql::pg::boolean, bool>",
            "boolean",
            true);
        const auto nullable_column = fmgr_detail::scalar_converter(
            "std::optional<bioseq::pg_utils::vector_column<"
            "bioseq::sql::pg::smallint, std::int16_t>>",
            "smallint",
            false);
        const auto nullable_any_mask = fmgr_detail::scalar_converter(
            "std::optional<bioseq::pg_utils::vector_null_mask>",
            "quoted_any",
            false);
        const auto nullable_array_state = fmgr_detail::scalar_converter(
            "std::optional<"
            "bioseq::pg_utils::int64_array_aggregate_state>",
            "internal",
            false);
        const auto int128_state_result = fmgr_detail::scalar_converter(
            "std::optional<"
            "bioseq::pg_utils::int128_aggregate_state>",
            "internal",
            true);
        const auto numeric_result = fmgr_detail::scalar_converter(
            "std::optional<bioseq::pg_utils::numeric_result>",
            "numeric",
            true);
        const auto date_scalar = fmgr_detail::scalar_converter(
            "std::optional<DateADT>",
            "date",
            false);
        const auto array_input = fmgr_detail::scalar_converter(
            "std::optional<ArrayType*>",
            "bioseq.column_mapping[]",
            false);
        const auto array_result = fmgr_detail::scalar_converter(
            "ArrayType*",
            "bioseq.letter[]",
            true);
        const auto jsonb_argument = fmgr_detail::scalar_converter(
            "bioseq::pg_utils::sql_jsonb",
            "jsonb",
            false);
        const auto nullable_jsonb_result =
            fmgr_detail::scalar_converter(
                "std::optional<bioseq::pg_utils::sql_jsonb>",
                "jsonb",
                true);
        const auto bytea_argument = fmgr_detail::scalar_converter(
            "bioseq::pg_utils::sql_bytea",
            "bytea",
            false);
        const auto bytea_result = fmgr_detail::scalar_converter(
            "bioseq::pg_utils::sql_bytea",
            "bytea",
            true);
        const auto int64_array_result =
            fmgr_detail::scalar_converter(
                "bioseq::pg_utils::sql_int64_array",
                "bigint[]",
                true);
        const auto nullable_oid_result =
            fmgr_detail::scalar_converter(
                "std::optional<std::uint32_t>",
                "oid",
                true);
        const auto nullable_interval_result =
            fmgr_detail::scalar_converter(
                "std::optional<bioseq::pg_utils::sql_interval>",
                "interval",
                true);
        const auto timestamptz_result =
            fmgr_detail::scalar_converter(
                "bioseq::pg_utils::sql_timestamptz",
                "timestamptz",
                true);
        const auto nullable_timestamptz_result =
            fmgr_detail::scalar_converter(
                "std::optional<bioseq::pg_utils::sql_timestamptz>",
                "timestamptz",
                true);
        const auto tid_argument = fmgr_detail::scalar_converter(
            "bioseq::pg_utils::sql_tid",
            "tid",
            false);
        require(
            date_argument.key
                    == "argument.vector_operand[date,DateADT]"
                && character_argument.key
                    == "argument.vector_operand[character,bpchar_view]"
                && boolean_result.key
                    == "result.vector_result[boolean,bool]"
                && nullable_column.key
                    == "argument.vector_column[smallint,int16_t].nullable"
                && nullable_any_mask.key
                    == "argument.vector_null_mask[any].nullable"
                && nullable_array_state.key
                    == "argument.aggregate_state.int64_array.nullable"
                && int128_state_result.key
                    == "result.aggregate_state.int128.nullable"
                && numeric_result.key
                    == "result.numeric.owning.nullable"
                && date_scalar.key
                    == "argument.date.by_value.nullable"
                && array_input.key
                    == "argument.array.detoasted["
                       "bioseq.column_mapping[]].nullable"
                && array_result.key
                    == "result.array.owning[bioseq.letter[]]"
                && jsonb_argument.key
                    == "argument.jsonb.owning"
                && nullable_jsonb_result.key
                    == "result.jsonb.owning.nullable"
                && bytea_argument.key
                    == "argument.bytea.owning"
                && bytea_result.key
                    == "result.bytea.owning"
                && int64_array_result.key
                    == "result.int64_array.owning"
                && nullable_oid_result.key
                    == "result.oid.by_value[oid].nullable"
                && nullable_interval_result.key
                    == "result.interval.owning.nullable"
                && timestamptz_result.key
                    == "result.timestamptz.owning"
                && nullable_timestamptz_result.key
                    == "result.timestamptz.owning.nullable"
                && tid_argument.key
                    == "argument.tid.by_value"
                && infer_sql_type_from_cpp(
                    "bioseq::pg_utils::sql_timestamptz")
                    == "timestamptz"
                && infer_sql_type_from_cpp(
                    "std::optional<"
                    "bioseq::pg_utils::sql_timestamptz>")
                    == "timestamptz"
                && infer_sql_type_from_cpp(
                    "bioseq::pg_utils::vector_operand<"
                    "bioseq::sql::pg::character, "
                    "bioseq::pg_utils::bpchar_view>")
                    == "char"
                && infer_sql_type_from_cpp(
                    "bioseq::pg_utils::vector_result<"
                    "bioseq::sql::pg::boolean, bool>")
                    == "boolean",
            "typed vector converters preserve logical SQL/lane identity");

        bool rejected_lane_mismatch = false;
        try {
            static_cast<void>(fmgr_detail::scalar_converter(
                "bioseq::pg_utils::vector_operand<"
                "bioseq::sql::pg::integer, DateADT>",
                "integer",
                false));
        } catch (const std::runtime_error&) {
            rejected_lane_mismatch = true;
        }
        require(
            rejected_lane_mismatch,
            "typed vector converters reject logical/lane drift");
        bool rejected_ambiguous_array = false;
        try {
            static_cast<void>(fmgr_detail::scalar_converter(
                "ArrayType*",
                "infer",
                false));
        } catch (const std::runtime_error&) {
            rejected_ambiguous_array = true;
        }
        require(
            rejected_ambiguous_array,
            "typed array converters reject ambiguous SQL ownership");
        require(
            fmgr_detail::expected_vector_associations().size() == 84,
            "closed vector registry has exactly 84 scalar associations");
        std::vector<fmgr_detail::vector_association>
            comparison_associations;
        for (const auto& [vector_name, types] :
             fmgr_detail::expected_vector_associations()) {
            comparison_associations.push_back(
                fmgr_detail::vector_association{
                    .scalar_name = vector_name.substr(1),
                    .vector_name = vector_name,
                    .left_type = types.first,
                    .right_type = types.second,
                    .result_type = "boolean",
                });
        }
        fmgr_detail::validate_vector_associations(
            comparison_associations);
        const auto comparison_contract_rejected =
            [](std::vector<fmgr_detail::vector_association> rows) {
                try {
                    fmgr_detail::validate_vector_associations(
                        std::move(rows));
                } catch (const std::runtime_error&) {
                    return true;
                }
                return false;
            };
        auto missing_comparison = comparison_associations;
        missing_comparison.pop_back();
        require(
            comparison_contract_rejected(
                std::move(missing_comparison)),
            "closed comparison registry rejects a missing association");
        auto duplicate_comparison = comparison_associations;
        duplicate_comparison.back() = duplicate_comparison.front();
        require(
            comparison_contract_rejected(
                std::move(duplicate_comparison)),
            "closed comparison registry rejects a duplicate association");
        auto wrong_comparison = comparison_associations;
        wrong_comparison.front().right_type = "date";
        require(
            comparison_contract_rejected(
                std::move(wrong_comparison)),
            "closed comparison registry rejects association drift");

        std::vector<fmgr_detail::vector_aggregate_association>
            aggregate_associations;
        for (const auto& [key, association] :
             fmgr_detail::expected_vector_aggregate_associations()) {
            static_cast<void>(key);
            aggregate_associations.push_back(association);
        }
        require(
            aggregate_associations.size() == 16,
            "closed vector registry has exactly 16 aggregate associations");
        fmgr_detail::validate_vector_aggregate_associations(
            aggregate_associations);
        const auto aggregate_contract_rejected =
            [](std::vector<
                   fmgr_detail::vector_aggregate_association> rows) {
                try {
                    fmgr_detail::validate_vector_aggregate_associations(
                        std::move(rows));
                } catch (const std::runtime_error&) {
                    return true;
                }
                return false;
            };
        auto missing_aggregate = aggregate_associations;
        missing_aggregate.pop_back();
        require(
            aggregate_contract_rejected(
                std::move(missing_aggregate)),
            "closed aggregate registry rejects a missing association");
        auto duplicate_aggregate = aggregate_associations;
        duplicate_aggregate.back() = duplicate_aggregate.front();
        require(
            aggregate_contract_rejected(
                std::move(duplicate_aggregate)),
            "closed aggregate registry rejects a duplicate association");
        auto wrong_aggregate = aggregate_associations;
        wrong_aggregate.front().transition_name = "wrong_transition";
        require(
            aggregate_contract_rejected(
                std::move(wrong_aggregate)),
            "closed aggregate registry rejects association drift");

        const auto production_registries =
            render_support_vector_registry_modules("production");
        const auto test_registries =
            render_support_vector_registry_modules("test_indexes");
        require(
            production_registries.size() == 2
                && test_registries.size() == 2,
            "SUPPORT owns exactly two closed vector registries");
        for (std::size_t index = 0;
             index < production_registries.size();
             ++index) {
            require(
                production_registries[index].owner_key
                    == test_registries[index].owner_key
                    && production_registries[index].module_name
                        == "bioseq.sql.generated.production."
                            + production_registries[index].owner_key
                    && test_registries[index].module_name
                        == "bioseq.sql.generated.test_indexes."
                            + test_registries[index].owner_key
                    && production_registries[index].content.contains(
                        "export module "
                        + production_registries[index].module_name)
                    && test_registries[index].content.contains(
                        "export module "
                        + test_registries[index].module_name)
                    && production_registries[index]
                        .content_sha256.size() == 64
                    && test_registries[index]
                        .content_sha256.size() == 64,
                "SUPPORT registry identity is profile-qualified and sealed");
        }
        require(
            production_registries[0].content.contains(
                "std::array<association, 84>")
                && production_registries[1].content.contains(
                    "std::array<association, 16>"),
            "SUPPORT registry bytes contain the complete closed tables");
    }

    {
        sql_graph typed_date_graph;
        typed_date_graph.entities.push_back(
            fmgr_contract_test_detail::routine(
                "sql:function:public.typed_date_column(date)",
                "fixture::typed_date_column",
                "typed_date_column",
                {
                    routine_parameter{
                        .name = "values",
                        .cpp_type =
                            "std::optional<"
                            "bioseq::pg_utils::vector_column<"
                            "bioseq::sql::pg::date, DateADT>>",
                        .sql_type = "date",
                        .nullable = true,
                    },
                },
                "std::optional<DateADT>",
                "date"));
        const auto typed_date_modules =
            render_fmgr_entrypoint_modules(typed_date_graph);
        require(
            typed_date_modules.size() == 1
                && typed_date_modules.front().content.contains(
                    "import bioseq.codegen.sql.annotation_model;\n")
                && typed_date_modules.front().content.contains(
                    "vector_column<bioseq::sql::pg::date,DateADT>"),
            "generated typed-vector entrypoints directly import the "
            "annotation constants that their converter spellings use");
    }

    {
        sql_graph array_graph;
        array_graph.entities.push_back(
            fmgr_contract_test_detail::routine(
                "sql:function:public.array_probe("
                "bioseq.column_mapping[])",
                "fixture::array_probe",
                "array_probe",
                {
                    routine_parameter{
                        .name = "columns",
                        .cpp_type = "std::optional<ArrayType*>",
                        .sql_type = "bioseq.column_mapping[]",
                        .nullable = true,
                    },
                },
                "std::int32_t",
                "integer"));
        array_graph.entities.push_back(
            fmgr_contract_test_detail::routine(
                "sql:function:public.mixed_array_probe("
                "bioseq.column_mapping[],integer,"
                "bioseq.column_mapping[])",
                "fixture::mixed_array_probe",
                "mixed_array_probe",
                {
                    routine_parameter{
                        .name = "required_columns",
                        .cpp_type = "ArrayType*",
                        .sql_type = "bioseq.column_mapping[]",
                    },
                    routine_parameter{
                        .name = "limit",
                        .cpp_type = "std::int32_t",
                        .sql_type = "integer",
                    },
                    routine_parameter{
                        .name = "optional_columns",
                        .cpp_type = "std::optional<ArrayType*>",
                        .sql_type = "bioseq.column_mapping[]",
                        .nullable = true,
                    },
                },
                "std::int32_t",
                "integer"));
        const auto array_modules =
            render_fmgr_entrypoint_modules(array_graph);
        require(
            array_modules.size() == 1
                && array_modules.front().content.contains(
                    "sql_nullable_array_argument(fcinfo, 0)")
                && array_modules.front().content.contains(
                    "sql_array_argument_0.optional()")
                && array_modules.front().content.contains(
                    "invoke_pg_as_cpp([&sql_array_argument_0]")
                && array_modules.front().content.contains(
                    "sql_array_argument(fcinfo, 0)")
                && array_modules.front().content.contains(
                    "sql_nullable_array_argument(fcinfo, 2)")
                && array_modules.front().content.contains(
                    "invoke_pg_as_cpp([fcinfo, "
                    "&sql_array_argument_0, &sql_array_argument_2]")
                && !array_modules.front().content.contains(
                    "import bioseq.codegen.sql.annotation_model;\n")
                && !array_modules.front().content.contains("[&]")
                && !array_modules.front().content.contains("[=]"),
            "nullable array wrappers retain a detoast guard across both "
            "SQL NULL and non-null owner calls with deterministic explicit "
            "captures");
    }

    {
        sql_graph nullable_oid_graph;
        nullable_oid_graph.entities.push_back(
            fmgr_contract_test_detail::routine(
                "sql:function:public.optional_oid_fixture()",
                "fixture::optional_oid_fixture",
                "optional_oid_fixture",
                {},
                "std::__1::optional<Oid>",
                "oid"));
        const auto nullable_oid_modules =
            render_fmgr_entrypoint_modules(nullable_oid_graph);
        require(
            nullable_oid_modules.size() == 1
                && nullable_oid_modules.front().wrappers.size() == 1
                && nullable_oid_modules.front().wrappers.front()
                    .overloads.front().result_converter_key
                    == "result.oid.by_value[oid].nullable"
                && nullable_oid_modules.front().content.contains(
                    "sql_result_to_datum(\n"
                    "        fcinfo, ::fixture::optional_oid_fixture()"),
            "libc++ optional OID results use the exact nullable OID "
            "converter");

        nullable_oid_graph.entities.front().source.result_sql_type =
            "integer";
        bool rejected_mismatched_optional_oid = false;
        try {
            static_cast<void>(
                render_fmgr_entrypoint_modules(nullable_oid_graph));
        } catch (const std::runtime_error&) {
            rejected_mismatched_optional_oid = true;
        }
        require(
            rejected_mismatched_optional_oid,
            "nullable OID results reject a mismatched SQL type");
    }

    {
        sql_graph void_graph;
        normalized_entity void_function =
            fmgr_contract_test_detail::routine(
                "sql:function:pg_catalog.alter_fixture(integer)",
                "fixture::alter_fixture",
                "alter_fixture",
                {
                    routine_parameter{
                        .name = "value",
                        .cpp_type = "std::int32_t",
                        .sql_type = "integer",
                    },
                },
                "void",
                "void");
        void_function.source.result = routine_result_contract{
            .form = routine_result_form::void_,
            .cpp_type = "void",
            .sql_type = "void",
        };
        normalized_entity procedure =
            fmgr_contract_test_detail::routine(
                "sql:procedure:public.vacuum_fixture(integer)",
                "fixture::vacuum_fixture",
                "vacuum_fixture",
                {
                    routine_parameter{
                        .name = "value",
                        .cpp_type = "std::int32_t",
                        .sql_type = "integer",
                    },
                },
                "void",
                "void");
        procedure.source.kind = entity_kind::procedure;
        procedure.source.annotation_name = "procedure";
        procedure.source.result = routine_result_contract{
            .form = routine_result_form::void_,
            .cpp_type = "void",
            .sql_type = "void",
        };
        void_graph.entities = {
            std::move(void_function),
            std::move(procedure),
        };

        const auto void_modules =
            render_fmgr_entrypoint_modules(void_graph);
        require(
            void_modules.size() == 1
                && void_modules.front().wrappers.size() == 2
                && fmgr_contract_test_detail::count_occurrences(
                    void_modules.front().content,
                    "return bioseq::pg_utils::sql_void_result(fcinfo);")
                    == 2
                && void_modules.front().content.contains(
                    "::fixture::alter_fixture(")
                && void_modules.front().content.contains(
                    "::fixture::vacuum_fixture(")
                && !void_modules.front().content.contains(
                    "sql_result_to_datum(\n"
                    "        fcinfo, ::fixture::alter_fixture")
                && !void_modules.front().content.contains(
                    "sql_result_to_datum(\n"
                    "        fcinfo, ::fixture::vacuum_fixture")
                && void_modules.front().wrappers[0]
                    .overloads.front().result_converter_key
                    == "result.void"
                && void_modules.front().wrappers[1]
                    .overloads.front().result_converter_key
                    == "result.void",
            "void SQL functions and procedures invoke their C++ owner before "
            "returning PostgreSQL void without a scalar converter");
    }

    sql_graph graph;
    graph.entities.push_back(
        fmgr_contract_test_detail::routine(
            "sql:function:$extension.mmsearch(internal,integer)",
            "fixture::mmsearch",
            "mmsearch",
            {
                routine_parameter{
                    .name = "value",
                    .cpp_type = "Datum",
                    .sql_type = "internal",
                },
                routine_parameter{
                    .name = "count",
                    .cpp_type = "std::int32_t",
                    .sql_type = "integer",
                },
            },
            "bool",
            "boolean"));
    graph.entities.back().installed_identity =
        "sql:function:public.mmsearch(internal,integer)";
    graph.entities.push_back(
        fmgr_contract_test_detail::routine(
            "sql:function:$extension.mmsearch(internal,integer,bigint)",
            "fixture::mmsearch",
            "mmsearch",
            {
                routine_parameter{
                    .name = "value",
                    .cpp_type = "std::optional<Datum>",
                    .sql_type = "internal",
                    .nullable = true,
                },
                routine_parameter{
                    .name = "count",
                    .cpp_type = "std::int32_t",
                    .sql_type = "integer",
                },
                routine_parameter{
                    .name = "limit",
                    .cpp_type = "std::int64_t",
                    .sql_type = "bigint",
                },
            },
            "std::optional<bool>",
            "boolean"));
    graph.entities.back().installed_identity =
        "sql:function:public.mmsearch(internal,integer,bigint)";
    graph.entities.push_back(
        fmgr_contract_test_detail::routine(
            "sql:function:$extension.context_probe(integer)",
            "fixture::context_probe",
            "context_probe",
            {
                routine_parameter{
                    .name = "value",
                    .cpp_type = "std::int32_t",
                    .sql_type = "integer",
                },
                routine_parameter{
                    .name = "context",
                    .cpp_type =
                        "bioseq::pg_utils::sql_call_context",
                    .injected_call_context = true,
                },
            },
            "std::int64_t",
            "bigint"));
    graph.entities.back().installed_identity =
        "sql:function:public.context_probe(integer)";
    graph.entities.push_back(
        fmgr_contract_test_detail::routine(
            "sql:function:$extension.context_only()",
            "fixture::context_only",
            "context_only",
            {
                routine_parameter{
                    .name = "context",
                    .cpp_type =
                        "bioseq::pg_utils::sql_call_context",
                    .injected_call_context = true,
                },
            },
            "std::int32_t",
            "integer"));
    graph.entities.back().installed_identity =
        "sql:function:public.context_only()";

    const std::vector<rendered_fmgr_entrypoint_module> modules =
        render_fmgr_entrypoint_modules(graph);
    require(modules.size() == 1, "one owner module");
    const rendered_fmgr_entrypoint_module& module = modules.front();
    require(
        module.owner_key == "fixture"
            && module.module_name
                == "bioseq.sql.generated.production.fixture.entrypoints"
            && module.source_module == "bioseq.fixture"
            && module.content_sha256.size() == 64,
        "configure-stable owner identity");
    require(module.wrappers.size() == 3, "three physical wrappers");
    require(
        module.content.contains("PG_FUNCTION_INFO_V1(mmsearch)")
            && module.content.contains("switch (PG_NARGS())")
            && module.content.contains("case 2:")
            && module.content.contains("case 3:")
            && module.content.contains(
                "sql_context_argument(fcinfo)")
            && module.content.contains(
                "invoke_sql_boundary(\n"
                "        bioseq::pg_utils::"
                "sql_boundary_probe_descriptor{")
            && module.content.contains(
                ".fault_key = "
                "\"generated_boundary_tail.std_exception\"")
            && !module.content.contains(
                "const auto boundary_probe")
            && module.content.contains(
                "default:\n"
                "            ereport(\n"
                "                ERROR,\n"
                "                (errcode(ERRCODE_INTERNAL_ERROR),\n"
                "                 errmsg(\"bioseq SQL callback "
                "raised a C++ exception\"),")
            && module.content.contains(
                "\"generated fmgr overload wrapper mmsearch "
                "received an unsupported arity\")));\n"
                "            pg_unreachable();"),
        "generated wrapper body");
    const auto probe_wrapper = std::ranges::find(
        module.wrappers,
        std::string{"mmsearch"},
        &rendered_fmgr_wrapper::public_symbol);
    require(
        probe_wrapper != module.wrappers.end()
            && probe_wrapper->overloads.size() == 2,
        "shared physical wrapper retains both SQL identities");
    for (const rendered_fmgr_wrapper& wrapper :
         module.wrappers) {
        const std::string wrapper_body =
            fmgr_detail::render_public_wrapper(wrapper);
        if (wrapper.overloads.size() == 1) {
            require(
                !wrapper_body.contains("PG_NARGS()")
                    && wrapper_body.contains(
                        "return "
                        + fmgr_detail::overload_helper_name(
                            wrapper.public_symbol,
                            wrapper.overloads.front().sql_arity)
                        + "(fcinfo);"),
                "catalog-fixed single-overload wrapper has no "
                "runtime arity branch");
        } else {
            require(
                wrapper_body.contains("switch (PG_NARGS())"),
                "multi-overload wrapper dispatches by runtime arity");
        }
    }
    require(
        std::set<std::string, std::less<>>{
            probe_wrapper->overloads[0].sql_identity,
            probe_wrapper->overloads[1].sql_identity,
        } == std::set<std::string, std::less<>>{
            "mmsearch(internal,integer)",
            "mmsearch(internal,integer,bigint)",
        },
        "emitted runtime identities remain relocatable across the "
        "configured installation schema");
    std::set<std::string, std::less<>> probe_site_ids;
    for (const rendered_fmgr_overload& overload :
         probe_wrapper->overloads) {
        std::string probe_material{
            "bioseq.safety_runtime_probe.site.v2\n"};
        probe_material += overload.sql_identity;
        probe_material +=
            "\ngenerated_boundary_tail.std_exception\n";
        const std::string expected_site_id =
            "probe:v2:" + sha256_hex(probe_material);
        require(
            overload.boundary_probe_site_id == expected_site_id
                && overload.boundary_probe_site_id.size() == 73
                && overload.boundary_fault_key
                    == "generated_boundary_tail.std_exception"
                && fmgr_contract_test_detail::count_occurrences(
                       module.content,
                       overload.boundary_probe_site_id)
                    == 1
                && module.content.contains(
                    "return bioseq::pg_utils::"
                    "invoke_sql_boundary(\n"
                    "                bioseq::pg_utils::"
                    "sql_boundary_probe_descriptor{\n"
                    "                    .site_id = \""
                    + overload.boundary_probe_site_id + "\"")
                && probe_site_ids.insert(
                       overload.boundary_probe_site_id)
                       .second,
            "each SQL identity owns one deterministic full-digest "
            "boundary probe");
    }
    require(
        fmgr_detail::generated_boundary_probe_site_id(
            probe_wrapper->overloads.front().sql_identity
            + ".changed")
            != probe_wrapper->overloads.front()
                   .boundary_probe_site_id,
            "boundary probe identity changes with its SQL identity");
    std::size_t installed_overload_count = 0;
    for (const rendered_fmgr_wrapper& wrapper :
         module.wrappers) {
        installed_overload_count += wrapper.overloads.size();
        for (const rendered_fmgr_overload& overload :
             wrapper.overloads) {
            require(
                fmgr_contract_test_detail::count_occurrences(
                    module.content,
                    overload.boundary_probe_site_id)
                    == 1,
                "installed SQL identity has exactly one emitted "
                "literal boundary descriptor");
        }
    }
    require(
        fmgr_contract_test_detail::count_occurrences(
            module.content,
            "bioseq::pg_utils::invoke_sql_boundary(")
                == installed_overload_count
            && fmgr_contract_test_detail::count_occurrences(
                   module.content,
                   "bioseq::pg_utils::"
                   "sql_boundary_probe_descriptor{")
                == installed_overload_count,
        "every installed arity owns exactly one direct "
        "descriptor-bound specialization");
    require(
        !module.content.contains("[&]")
            && !module.content.contains("[=]"),
        "generated code has no capture-all lambda");

    const std::vector<std::string> symbols =
        emitted_fmgr_wrapper_symbols(modules);
    require(
        symbols
            == std::vector<std::string>{
                "context_only", "context_probe", "mmsearch"},
        "actual artifact symbol provenance");

    const std::vector<rendered_fmgr_entrypoint_module> second =
        render_fmgr_entrypoint_modules(graph);
    require(
        second.front().content == module.content,
        "byte-deterministic second rendering");

    sql_graph row_graph;
    row_graph.result_rows = {
        fmgr_contract_test_detail::reaper_result_row(),
        fmgr_contract_test_detail::diagnostics_result_row(),
    };
    normalized_entity reaper =
        fmgr_contract_test_detail::routine(
            "sql:function:public.reap_orphan_seq_sub_tables()",
            "bioseq::storage::reap_orphan_seq_sub_tables",
            "reap_orphan_seq_sub_tables",
            {},
            "bioseq::pg_utils::returns_table<"
            "bioseq::storage::reaper_event>",
            "table (action text, target text)");
    reaper.source.source_module =
        "bioseq.storage.manager:sequence_storage_manager";
    reaper.source.result = routine_result_contract{
        .form = routine_result_form::returns_table,
        .cpp_type = "bioseq::storage::reaper_event",
        .sql_type = "table (action text, target text)",
        .row_cpp_name = "bioseq::storage::reaper_event",
    };
    normalized_entity diagnostics =
        fmgr_contract_test_detail::routine(
            "sql:function:bioseq_internal.teardown_diagnostics()",
            "bioseq::table::teardown_diagnostics",
            "teardown_diagnostics",
            {},
            "bioseq::pg_utils::named_out<"
            "bioseq::table::teardown_diagnostics_row>",
            "record");
    diagnostics.source.source_module =
        "bioseq.table:access_hooks";
    diagnostics.source.schema = "bioseq_internal";
    diagnostics.source.result = routine_result_contract{
        .form = routine_result_form::named_out,
        .cpp_type = "bioseq::table::teardown_diagnostics_row",
        .sql_type = "record",
        .row_cpp_name =
            "bioseq::table::teardown_diagnostics_row",
    };
    normalized_entity table_handler =
        fmgr_contract_test_detail::routine(
            "sql:function:bioseq.bioseq_handler(internal)",
            "bioseq::table::sql_routines::bioseq_handler",
            "bioseq_handler",
            {
                routine_parameter{
                    .name = "handler_context",
                    .cpp_type =
                        "std::optional<bioseq::pg_utils::"
                        "table_am_handler_context>",
                    .sql_type = "internal",
                },
            },
            "const TableAmRoutine*",
            "table_am_handler");
    table_handler.source.source_module =
        "bioseq.table:access_method";
    table_handler.source.schema = "bioseq";
    normalized_entity custom_probe =
        fmgr_contract_test_detail::routine(
            "sql:function:bioseq_internal.custom_probe()",
            "bioseq::table::sql_routines::custom_probe",
            "custom_probe",
            {},
            "bool",
            "boolean");
    custom_probe.source.source_module =
        "bioseq.table:custom_scan";
    custom_probe.source.schema = "bioseq_internal";
    row_graph.entities = {
        std::move(reaper),
        std::move(diagnostics),
        std::move(table_handler),
        std::move(custom_probe),
    };

    const std::vector<rendered_fmgr_entrypoint_module> row_modules =
        render_fmgr_entrypoint_modules(row_graph);
    require(row_modules.size() == 2, "two result-row owner modules");
    const auto storage_module = std::ranges::find(
        row_modules,
        std::string{"storage_manager_sequence_storage_manager"},
        &rendered_fmgr_entrypoint_module::owner_key);
    const auto table_module = std::ranges::find(
        row_modules,
        std::string{"table_access_hooks"},
        &rendered_fmgr_entrypoint_module::owner_key);
    require(
        storage_module != row_modules.end()
            && storage_module->module_name
                == "bioseq.sql.generated.production."
                   "storage_manager_sequence_storage_manager.entrypoints"
            && storage_module->content.contains(
                "import bioseq.storage.manager;")
            && storage_module->content.contains(
                "import bioseq.storage.manager.sql_rows;")
            && storage_module->content.contains(
                "column_names{\"action\", \"target\"}")
            && storage_module->content.contains(
                "row.action,\n            row.target")
            && storage_module->content.contains(
                "invoke_reap_orphan_seq_sub_tables_arity_0_"
                "result_encoder{}"),
        "RETURNS TABLE encoder and safe public-module imports");
    require(
        table_module != row_modules.end()
            && table_module->module_name
                == "bioseq.table:sql_entrypoints"
            && table_module->content.contains(
                "import :access_hooks;")
            && table_module->content.contains(
                "import :access_method;")
            && table_module->content.contains(
                "import :custom_scan;")
            && table_module->content.contains(
                "import bioseq.table.sql_rows;")
            && table_module->content.contains(
                "sql_argument<std::optional<bioseq::pg_utils::"
                "table_am_handler_context>>(fcinfo, 0)")
            && table_module->content.contains(
                "::bioseq::table::sql_routines::bioseq_handler")
            && table_module->content.contains(
                "column_names{\"lock_fallback_classifications\", "
                "\"missing_relation_observations\"}")
            && table_module->content.contains(
                "invoke_teardown_diagnostics_arity_0_result_encoder{}"),
        "named OUT encoder and same-primary partition mapping");
    const auto table_diagnostics_wrapper = std::ranges::find(
        table_module->wrappers,
        std::string{"teardown_diagnostics"},
        &rendered_fmgr_wrapper::public_symbol);
    require(
        storage_module->wrappers.front()
            .overloads.front()
            .result_converter_key
            == "result.returns_table["
               "bioseq::storage::reaper_event]"
               "(action:text,target:text)"
            && table_diagnostics_wrapper
                != table_module->wrappers.end()
            && table_diagnostics_wrapper
                ->overloads.front()
                .result_converter_key
                == "result.named_out["
                   "bioseq::table::teardown_diagnostics_row]"
                   "(lock_fallback_classifications:bigint,"
                   "missing_relation_observations:bigint)",
        "result-row converter provenance is exact");

    sql_graph subscript_graph;
    normalized_entity subscript_handler =
        fmgr_contract_test_detail::routine(
            "sql:function:bioseq.seq_subscript_handler(internal)",
            "bioseq::pg_types::sql_routines::seq_subscript_handler",
            "seq_subscript_handler",
            {
                routine_parameter{
                    .name = "input",
                    .cpp_type =
                        "std::optional<bioseq::pg_utils::"
                        "subscript_handler_context>",
                    .sql_type = "internal",
                },
            },
            "void*",
            "internal");
    subscript_handler.source.source_module =
        "bioseq.types.pg_compact_sequence:subscript";
    subscript_handler.source.schema = "bioseq";
    subscript_graph.entities = {std::move(subscript_handler)};
    const std::vector<rendered_fmgr_entrypoint_module>
        subscript_modules =
            render_fmgr_entrypoint_modules(subscript_graph);
    require(
        subscript_modules.size() == 1
            && subscript_modules.front().content.contains(
                "sql_argument<std::optional<bioseq::pg_utils::"
                "subscript_handler_context>>(fcinfo, 0)")
            && subscript_modules.front().wrappers.size() == 1
            && subscript_modules.front()
                .wrappers.front()
                .overloads.front()
                .argument_converter_keys
                == std::vector<std::string>{
                    "argument.subscript_handler.context.nullable"},
        "subscripting handlers retain the catalog INTERNAL argument while "
        "using their zero-argument runtime context marker");

    const rendered_sql_artifact row_sql =
        render_sql_graph(row_graph);
    require(
        row_sql.sql.contains(
            "FUNCTION reap_orphan_seq_sub_tables()"
            "\nRETURNS TABLE (action text, target text)")
            && !row_sql.sql.contains(
                "FUNCTION public.reap_orphan_seq_sub_tables()")
            && row_sql.sql.contains(
                "FUNCTION bioseq_internal.teardown_diagnostics("
                "OUT lock_fallback_classifications bigint, "
                "OUT missing_relation_observations bigint)"
                "\nRETURNS record"),
        "result-row SQL forms are rendered from graph metadata");

    sql_graph ambiguous = graph;
    ambiguous.entities.push_back(
        fmgr_contract_test_detail::routine(
            "sql:function:public.mmsearch(text,integer)",
            "fixture::mmsearch",
            "mmsearch",
            {
                routine_parameter{
                    .name = "value",
                    .cpp_type = "Datum",
                    .sql_type = "text",
                },
                routine_parameter{
                    .name = "count",
                    .cpp_type = "std::int32_t",
                    .sql_type = "integer",
                },
            },
            "bool",
            "boolean"));
    bool rejected_same_arity = false;
    try {
        static_cast<void>(
            render_fmgr_entrypoint_modules(ambiguous));
    } catch (const std::runtime_error&) {
        rejected_same_arity = true;
    }
    require(
        rejected_same_arity,
        "same-arity overload dispatch is rejected");

    sql_graph unauthorized_dispatch = graph;
    for (normalized_entity& entity :
         unauthorized_dispatch.entities) {
        if (entity.source.name != "mmsearch") continue;
        entity.source.name = "probe";
        entity.source.cpp_qualified_name = "fixture::probe";
        entity.stable_identity.replace(
            entity.stable_identity.find("mmsearch"),
            std::string{"mmsearch"}.size(),
            "probe");
        entity.installed_identity.replace(
            entity.installed_identity.find("mmsearch"),
            std::string{"mmsearch"}.size(),
            "probe");
    }
    bool rejected_unaudited_dispatch = false;
    try {
        static_cast<void>(
            render_fmgr_entrypoint_modules(
                unauthorized_dispatch));
    } catch (const std::runtime_error& error) {
        rejected_unaudited_dispatch =
            std::string_view{error.what()}.contains(
                "only for the audited from_array/mmsearch wrappers");
    }
    require(
        rejected_unaudited_dispatch,
        "runtime arity dispatch is closed to the two audited "
        "production wrappers");

    sql_graph wrong_allowed_arities = graph;
    auto three_argument_mmsearch =
        std::ranges::find_if(
            wrong_allowed_arities.entities,
            [](const normalized_entity& entity) {
                return entity.source.name == "mmsearch"
                    && entity.source.parameters.size() == 3;
            });
    require(
        three_argument_mmsearch
            != wrong_allowed_arities.entities.end(),
        "three-argument audited overload fixture exists");
    three_argument_mmsearch->source.parameters.push_back(
        routine_parameter{
            .name = "enabled",
            .cpp_type = "bool",
            .sql_type = "boolean",
        });
    three_argument_mmsearch->stable_identity =
        "sql:function:$extension.mmsearch("
        "internal,integer,bigint,boolean)";
    three_argument_mmsearch->installed_identity =
        "sql:function:public.mmsearch("
        "internal,integer,bigint,boolean)";
    bool rejected_wrong_allowed_arities = false;
    try {
        static_cast<void>(
            render_fmgr_entrypoint_modules(
                wrong_allowed_arities));
    } catch (const std::runtime_error& error) {
        rejected_wrong_allowed_arities =
            std::string_view{error.what()}.contains(
                "must have exactly arities 2 and 3");
    }
    require(
        rejected_wrong_allowed_arities,
        "audited overload wrappers retain the exact benchmarked "
        "two-arity set");
}

}  // namespace bioseq::codegen::sql
