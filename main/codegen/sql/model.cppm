module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module bioseq.codegen.sql:model;

export namespace bioseq::codegen::sql {

enum class authority_mode : std::uint8_t {
    annotated_cpp,
    legacy_baseline,
};

enum class command_action : std::uint8_t {
    generate,
    update_golden,
    migrate_baseline,
    prepare,
};

enum class generation_stage : std::uint8_t {
    support,
    core,
    final,
};

enum class statement_kind : std::uint8_t {
    alter,
    comment,
    create_access_method,
    create_aggregate,
    create_cast,
    create_function,
    create_index,
    create_operator,
    create_operator_class,
    create_procedure,
    create_schema,
    create_sequence,
    create_table,
    create_type,
    create_view,
    grant,
    insert,
    reset,
    revoke,
    select,
    set,
    unknown,
};

enum class statement_disposition : std::uint8_t {
    retained,
    superseded_by_later_create_or_replace,
};

struct source_span {
    std::size_t byte_begin{};
    std::size_t byte_end{};
    std::size_t line_begin{1};
    std::size_t line_end{1};
};

struct routine_metadata {
    std::string identity;
    std::string argument_declarations;
    std::string returns;
    std::string language;
    std::string c_symbol;
    std::string volatility;
    std::string null_input;
    std::string security;
    std::string parallel;
    std::string configuration;
    std::string cost;
    std::string rows;
    std::string support;
};

struct sql_statement {
    std::size_t physical_ordinal{};
    statement_kind kind{statement_kind::unknown};
    statement_disposition disposition{statement_disposition::retained};
    source_span span{};
    std::string text;
    std::string normalized_text;
    std::string normalized_hash;
    std::optional<std::string> routine_identity;
    std::optional<routine_metadata> routine;
    bool create_or_replace{};
    std::optional<std::size_t> superseded_by_physical_ordinal;
};

struct install_script {
    std::string preamble;
    std::vector<sql_statement> statements;
    std::string trailing_text;
};

struct configured_entity_handle_output {
    std::string owner_key;
    std::filesystem::path path;
};

struct configured_row_codec_output {
    std::string owner_key;
    std::filesystem::path path;
};

struct configured_fmgr_entrypoint_output {
    std::string owner_key;
    std::filesystem::path path;
};

struct configured_fmgr_owner {
    std::string owner_key;
    std::string source_module;
};

struct command_line {
    command_action action{command_action::generate};
    authority_mode mode{authority_mode::legacy_baseline};
    generation_stage stage{generation_stage::final};
    bool stage_specified{};
    std::filesystem::path input;
    std::vector<std::filesystem::path> sources;
    std::filesystem::path source_list;
    std::filesystem::path compile_database;
    std::filesystem::path raw_compile_database;
    std::filesystem::path compile_database_output;
    std::filesystem::path pcm_root;
    std::filesystem::path pcm_manifest;
    std::filesystem::path depfile;
    std::filesystem::path clang_binary;
    std::filesystem::path clang_scan_deps;
    std::filesystem::path llvm_config;
    std::filesystem::path compiler_module_inventory;
    std::filesystem::path migration_owner;
    std::filesystem::path output;
    std::filesystem::path manifest;
    std::filesystem::path golden;
    std::filesystem::path stamp;
    std::filesystem::path physical_ledger;
    std::filesystem::path physical_ledger_output;
    std::filesystem::path guc_parity_ledger;
    std::filesystem::path guc_output_root;
    std::filesystem::path core_hash_output;
    std::filesystem::path core_manifest;
    std::filesystem::path support_manifest;
    std::filesystem::path support_seal;
    std::filesystem::path production_support_manifest;
    std::filesystem::path production_support_seal;
    std::filesystem::path test_indexes_support_manifest;
    std::filesystem::path test_indexes_support_seal;
    std::filesystem::path expected_core_hash;
    std::vector<configured_entity_handle_output> entity_handle_outputs;
    std::vector<configured_row_codec_output> row_codec_outputs;
    std::vector<configured_fmgr_entrypoint_output>
        fmgr_entrypoint_outputs;
    std::vector<configured_fmgr_owner> fmgr_owners;
    std::string prepare_authority;
    std::string compiler_profile;
    std::string clang_sha256;
    std::string clang_scan_deps_sha256;
    std::string llvm_config_sha256;
    std::string compiler_module_inventory_sha256;
    std::string guc_active_profile{"production"};
    std::string extension_name{"bioseq"};
    std::string extension_schema{"public"};
    std::string generated_module_prefix{"bioseq.sql.generated"};
    std::string fmgr_boundary_module{
        "bioseq.pg_utils.sql_boundary"};
    bool update_golden{};
    bool require_complete_codegen{};
    bool require_checked_physical_ledger{};
};

/// Returns the stable manifest spelling for a statement kind.
[[nodiscard]] inline constexpr std::string_view
kind_name(statement_kind kind) noexcept {
    switch (kind) {
        case statement_kind::alter: return "alter";
        case statement_kind::comment: return "comment";
        case statement_kind::create_access_method:
            return "create_access_method";
        case statement_kind::create_aggregate: return "create_aggregate";
        case statement_kind::create_cast: return "create_cast";
        case statement_kind::create_function: return "create_function";
        case statement_kind::create_index: return "create_index";
        case statement_kind::create_operator: return "create_operator";
        case statement_kind::create_operator_class:
            return "create_operator_class";
        case statement_kind::create_procedure: return "create_procedure";
        case statement_kind::create_schema: return "create_schema";
        case statement_kind::create_sequence: return "create_sequence";
        case statement_kind::create_table: return "create_table";
        case statement_kind::create_type: return "create_type";
        case statement_kind::create_view: return "create_view";
        case statement_kind::grant: return "grant";
        case statement_kind::insert: return "insert";
        case statement_kind::reset: return "reset";
        case statement_kind::revoke: return "revoke";
        case statement_kind::select: return "select";
        case statement_kind::set: return "set";
        case statement_kind::unknown: return "unknown";
    }
    return "unknown";
}

[[nodiscard]] inline constexpr std::string_view
disposition_name(statement_disposition disposition) noexcept {
    switch (disposition) {
        case statement_disposition::retained: return "retained";
        case statement_disposition::superseded_by_later_create_or_replace:
            return "superseded_by_later_create_or_replace";
    }
    return "unknown";
}

}  // namespace bioseq::codegen::sql
