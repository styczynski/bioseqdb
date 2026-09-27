module;

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

export module bioseq.codegen.sql.graph:model;

import bioseq.codegen.sql.collector;

export namespace bioseq::codegen::sql {

struct normalized_entity {
    collected_entity source;
    std::string stable_identity;
    std::string installed_identity;
    std::vector<std::string> dependency_identities;
    std::string planner_support_identity;
    std::string type_role_owner_identity;
    std::string type_element_identity;
    std::string opclass_storage_identity;
    std::size_t strongly_connected_component{};
};

struct handwritten_fmgr_export_owner {
    std::string macro_name;
    std::string symbol;
    std::string source_module;
    std::filesystem::path source_path;
    std::size_t source_line{};
};

struct sql_graph {
    std::string extension_name{"bioseq"};
    std::string extension_schema{"public"};
    std::string generated_module_prefix{"bioseq.postgres.sql.generated"};
    std::string fmgr_boundary_module{
        "bioseq.postgres.support.sql_boundary"};
    std::string fmgr_prelude{"bioseq_common"};
    std::string compiler_profile;
    std::string clang_release;
    std::string compilation_signature;
    std::vector<handwritten_fmgr_export_owner>
        handwritten_fmgr_export_owners;
    std::vector<collected_result_row> result_rows;
    std::vector<normalized_entity> entities;
};

}  // namespace bioseq::codegen::sql
