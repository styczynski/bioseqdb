module;

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

export module bioseq.codegen.sql.graph:model;

import bioseq.codegen.clang_structural;
import bioseq.codegen.sql.collector;

export namespace bioseq::codegen::sql {

struct normalized_entity {
    collected_entity source;
    std::string stable_identity;
    std::string installed_identity;
    std::vector<std::string> dependency_identities;
    std::size_t strongly_connected_component{};
};

struct legacy_fmgr_macro_owner {
    std::string macro_name;
    std::string symbol;
    std::string source_module;
    std::filesystem::path source_path;
    std::size_t source_line{};
};

struct baseline_physical_statement {
    std::string baseline_physical_id;
    std::size_t baseline_ordinal{};
    entity_kind kind{entity_kind::install_action};
    std::string order_identity;
    std::string source_module;
    std::filesystem::path source_path;
    std::string source_sha256;
    clang_structural::source_range source_range;
};

struct sql_graph {
    std::string extension_schema{"public"};
    std::string compiler_profile;
    std::string clang_version;
    std::string compilation_signature;
    std::vector<legacy_fmgr_macro_owner> legacy_fmgr_macro_owners;
    std::vector<collected_result_row> result_rows;
    std::vector<normalized_entity> entities;
    std::vector<baseline_physical_statement>
        expected_baseline_physical_statements;
};

}  // namespace bioseq::codegen::sql
