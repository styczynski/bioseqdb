module;

#include <string>
#include <vector>

export module bioseq.codegen.sql.renderer:model;

import bioseq.codegen.sql.collector;

export namespace bioseq::codegen::sql {

struct rendered_statement {
    std::string id;
    std::string entity_identity;
    std::string text;
    entity_kind statement_kind{entity_kind::phase};
};

struct rendered_sql_artifact {
    std::string sql;
    std::vector<rendered_statement> statements;
};

struct rendered_entity_handle {
    std::string entity_identity;
    std::string installed_identity;
    std::string leaf_name;
    std::string schema_name;
    std::string schema_binding;
    std::string expected_relkind;
    std::string variable_name;
};

struct rendered_entity_handle_module {
    std::string owner_key;
    std::string source_module;
    std::string module_name;
    std::string cpp_namespace;
    std::string content;
    std::vector<rendered_entity_handle> handles;
    bool profile_invariant{true};
};

struct rendered_row_codec_module {
    std::string owner_key;
    std::string source_module;
    std::string module_name;
    std::string cpp_namespace;
    std::string content;
    std::vector<std::string> row_cpp_names;
    bool profile_invariant{true};
};

}  // namespace bioseq::codegen::sql
