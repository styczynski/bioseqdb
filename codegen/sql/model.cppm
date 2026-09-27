module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

export module bioseq.codegen.sql:model;

export namespace bioseq::codegen::sql {

enum class command_action : std::uint8_t {
    generate,
    prepare,
};

enum class generation_stage : std::uint8_t {
    support,
    core,
    final,
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

struct command_line {
    command_action action{command_action::generate};
    generation_stage stage{generation_stage::final};
    bool stage_specified{};
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
    std::filesystem::path output;
    std::filesystem::path manifest;
    std::filesystem::path stamp;
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
    std::vector<std::pair<std::string, std::string>>
        entity_handle_owners;
    std::vector<configured_row_codec_output> row_codec_outputs;
    std::vector<std::pair<std::string, std::string>>
        row_codec_owners;
    std::vector<configured_fmgr_entrypoint_output>
        fmgr_entrypoint_outputs;
    std::vector<std::pair<std::string, std::string>> fmgr_owners;
    std::string prepare_authority;
    std::string compiler_profile;
    std::string clang_sha256;
    std::string clang_scan_deps_sha256;
    std::string llvm_config_sha256;
    std::string compiler_module_inventory_sha256;
    std::string guc_active_profile{"production"};
    std::string extension_name{"bioseq"};
    std::string extension_schema{"public"};
    std::string generated_module_prefix{"bioseq.postgres.sql.generated"};
    std::string fmgr_boundary_module{
        "bioseq.postgres.support.sql_boundary"};
    std::string fmgr_prelude{"bioseq_common"};
    bool require_complete_codegen{};
};

}  // namespace bioseq::codegen::sql
