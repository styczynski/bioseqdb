module;

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

export module bioseq.codegen.website.model;

import bioseq.codegen.support;

// Data shared by website parsing, SQL execution, and rendering. These types
// own their text and trees so later stages do not need the parser libraries
// or live PostgreSQL connections that produced them.
export namespace bioseq::codegen::website {

using bioseq::codegen::diagnostic;
using bioseq::codegen::source_span;

/// @brief Content kinds used for Markdown and Doxygen documents.
enum class content_node_kind : std::uint8_t {
    root, paragraph, text, heading, emphasis, strong_text, strikethrough,
    highlight,
    link, image, entity_link, code_span, code_block, admonition, block_quote,
    ordered_list, unordered_list, list_item, table, table_row, table_cell,
    footnote, line_break, code_annotation
};

/// @brief A  document node that owns its text, attributes, and children.
struct content_node {
    content_node_kind kind{};
    std::string text;
    std::string target;
    std::map<std::string, std::string, std::less<>> attributes;
    std::vector<content_node> children;
    std::optional<source_span> source;
};

/// @brief Selects C++, SQL, an exact entity ID, or a search across both languages.
enum class entity_query_domain : std::uint8_t { any, cpp, sql, canonical_id };

/// @brief An entity link from a tutorial before its target is found.
struct unresolved_entity_query {
    entity_query_domain domain{};
    std::string spelling;
    std::optional<std::string> label;
    source_span source;
};

/// @brief A release path label with numbers used to sort releases.
struct release_identifier {
    std::string path_component;        // for example v0.1
    std::vector<std::uint32_t> parts;  // numeric components used for version ordering
};

/// @brief Tutorial metadata read from its front matter.
struct tutorial_metadata {
    std::string title;
    std::string slug;
    std::string description;
    std::uint32_t order{};
    std::vector<std::string> keywords;
    std::optional<std::string> social_image;
    bool draft{};
    source_span source;
};

/// @brief Languages recognized in fenced code blocks.
enum class code_language : std::uint8_t { text, bash, sql, cpp, cmake, json, yaml };

/// @brief An explanation attached to madching text in a named code block.
struct code_annotation {
    std::string target_block_id;
    std::string title;
    std::vector<content_node> commentary;
    std::string exact_match;
    std::optional<std::size_t> occurrence;
    source_span source;
};

/// @brief Expected outcome of a runnable SQL example.
enum class sql_expectation : std::uint8_t { success, error };

/// @brief Runnable SQL example split into hidden and visible sections.
struct sql_example {
    std::string local_id;
    std::string canonical_id;
    std::string setup_sql;
    std::string visible_sql;
    std::string teardown_sql;
    sql_expectation expectation{sql_expectation::success};
    std::optional<std::string> expected_sqlstate;
    std::chrono::milliseconds timeout{15'000};
    source_span source;
};

/// @brief Fully parsed tutorial with examples, annotations, and entity queries.
struct tutorial {
    tutorial_metadata metadata;
    release_identifier version;
    std::string source_path;  // path relative to the content root
    std::vector<content_node> content;
    std::vector<sql_example> sql_examples;
    std::vector<code_annotation> annotations;
    std::vector<unresolved_entity_query> entity_queries;
    std::optional<std::string> previous_slug;
    std::optional<std::string> next_slug;
};

/// @brief An input path and content hash recorded in the site  plan.
struct source_input {
    std::string path;          // relative input path using forward slashes
    std::string content_hash;  // input content hash
};

/// @brief Parsed tutorials and build inputs saved by website plan.
struct site_plan {
    std::string schema;
    std::uint32_t schema_revision{};
    std::string parser_identity;
    std::uint32_t parser_revision{};
    std::string binding_schema;
    std::uint32_t binding_schema_revision{};
    release_identifier version;
    std::string base_path;
    std::string source_revision;
    std::string site_title;
    std::vector<tutorial> tutorials;
    std::vector<source_input> inputs;
    std::string signature;
};

/// @brief Whether a SQL result contains rows, command status, or an error.
enum class result_shape : std::uint8_t { rows, command, error };
/// @brief Execution stage associated with a captured SQL notice.
enum class sql_execution_stage : std::uint8_t {
    session_setup, setup, visible, teardown, cleanup
};

/// @brief A result column name and its PostgreSQL type.
struct sql_column {
    std::string name;
    std::uint32_t type_oid{};
    std::string type_name;
};

/// @brief A SQL cell with owned text and a separate NULL flag
struct sql_cell {
    bool is_null{};
    std::string text;
};

/// @brief Rows, command status, or an error captured from SQL execution.
struct sql_result_set {
    result_shape shape{};
    std::vector<sql_column> columns;
    std::vector<std::vector<sql_cell>> rows;
    std::string command_tag;
    std::optional<std::uint64_t> affected_rows;
    std::optional<std::string> sqlstate;
    std::string primary_message;
    std::string detail;
    std::string hint;
};

/// @brief A PostgreSQL notice and the execution stage that produced it.
struct sql_notice {
    sql_execution_stage stage{};
    std::string severity;
    std::string sqlstate;
    std::string message;
};

/// @brief Visible results and notices produced by one runnable example.
struct sql_example_result {
    std::string example_id;
    std::vector<sql_result_set> visible_results;
    std::vector<sql_notice> notices;
};

/// @brief Elapsed execution time kept outside the published site data.
struct sql_example_measurement {
    std::string example_id;
    std::chrono::milliseconds elapsed{};
};

/// @brief Whether an entity belongs to C++ or SQL.
enum class entity_domain : std::uint8_t { cpp, sql };
/// @brief Entity  kind used for page routes, coverage counts, and search.
enum class entity_kind : std::uint8_t {
    namespace_, module, class_, struct_, union_, enum_, enum_value, concept_,
    type_alias, function, variable, sql_schema, sql_type, sql_function,
    overload_group
};

/// @brief A module interface and the modules it imports.
struct module_graph_unit {
    std::string source_path;
    std::string provided_module;
    std::vector<std::string> exported_imports;  // public re-export edges
    std::vector<std::string> internal_imports;  // imports without export
};

/// @brief A PostgreSQL C entry point found by, scanning C++ declarations.
struct pg_entrypoint_candidate {
    std::string symbol;
    std::string normalized_signature;
    source_span declaration;
    std::string owning_module;
};

/// @brief Reference-page description of one callable parameter.
struct parameter_description {
    std::string name;
    std::string type;
    std::optional<std::string> default_value;
    std::vector<content_node> description;
};

/// @brief A C++ or SQL entity with its documentation and reference links.
struct reference_entity {
    std::string canonical_id;
    entity_domain domain{};
    entity_kind kind{};
    std::string qualified_name;
    std::string display_name;
    std::string normalized_signature;
    std::string summary_text;
    std::vector<content_node> detail_content;
    std::vector<parameter_description> parameters;
    std::optional<std::string> return_type;
    std::optional<std::string> module_name;
    std::optional<source_span> declaration;
    std::optional<std::string> implementation_entity_id;
    std::vector<std::string> overload_ids;
    // Overload signatures and URLs for the group page. Filled while rendering
    // and omitted froc saved entity data.
    std::vector<std::pair<std::string, std::string>> overload_members;
    // Child names and URLs for class, module, and namespace pages.
    // Filled while rendering.
    std::vector<std::pair<std::string, std::string>> child_members;
    // URL for the Implemented by link. Resolved from implementation_entity_id
    // while rendering.
    std::string implemented_by_route;
    std::map<std::string, std::string, std::less<>> attributes;
    std::string route_path;  // route of the reference page
};

/// @brief SQL catalog entities and the PostgreSQL and  extension versions.
struct sql_catalog_snapshot {
    std::vector<reference_entity> entities;
    std::string server_version;
    std::string extension_version;
    std::string staged_artifact_sha256;
};

// A landing-page example with visible SQL and an optional table result.
// Execution selects the last table result for the page. Read result only
// when has_result is true. Setup and teardown SQL stay outside this view.
struct landing_example_view {
    std::string slot;
    std::string visible_sql;
    bool has_result{};
    sql_result_set result;
};

// Per-module / SQL-domain documentation totals for the coverage report.
struct coverage_group_view {
    std::string group;
    std::uint32_t public_total{};
    std::uint32_t documented_total{};
};

// Site data saved by website execute and read by website render It contains
// resolved tutorial content and SQL results so pages can be rendered again
// without running PostgreSQL. Execution clears hidden setup and teardown SQL
// before saving. Rendering adds the deployment base path to  page routes.
struct site_model {
    std::string schema;
    std::uint32_t schema_revision{};
    std::string parser_identity;
    std::uint32_t parser_revision{};
    std::string binding_schema;
    std::uint32_t binding_schema_revision{};
    release_identifier version;
    std::string site_title;
    std::string source_revision;
    std::vector<tutorial> tutorials;               // content stripped of hidden SQL
    std::vector<landing_example_view> landing_examples;
    std::vector<reference_entity> cpp_entities;   // routes resolved during rendering
    std::vector<reference_entity> sql_entities;   // public SQL reference pages
    std::vector<reference_entity> omitted_sql_entities;  // inventory-only
    std::uint32_t coverage_public_total{};
    std::uint32_t coverage_documented_total{};
    std::vector<std::string> coverage_missing;             // optional entries missing a summary
    std::vector<std::string> coverage_missing_parameters;  // entities missing parameter documentatio
    std::vector<std::string> coverage_missing_returns;     // entities missing return documentation
    std::vector<std::string> coverage_fatal_missing;       // required/referenced, undocumented
    std::vector<coverage_group_view> coverage_groups;      // totals by module / SQL domain
    std::string postgres_server_version;
    std::string extension_version;
    std::string staged_extension_sha256;
    std::vector<source_input> inputs;   // path + content hash, for the site manifest
    std::string plan_signature;       // hash identifying the plan input
    std::string cpp_signature;        // hash identifying the C++ entity input
};

/// @brief Operations needed to read catalog data and execute SQL examples.
template <typename p_executor>
concept sql_example_executor =
    requires(p_executor& executor, sql_example const& example) {
        { executor.read_catalog() }
            -> std::same_as<std::expected<sql_catalog_snapshot, diagnostic>>;
        { executor.execute(example) }
            -> std::same_as<std::expected<sql_example_result, diagnostic>>;
    };

/// @brief Search fields for a documented entity or page.
struct search_record {
    std::string canonical_id;
    std::string title;
    std::string qualified_name;
    std::string kind;
    std::string summary;
    std::string route_path;
    std::vector<std::string> aliases;
    std::vector<std::string> keywords;
};

// Site fields shared by page templates. Links within the site already
// include the deployment base path so templates can insert them directly.
struct site_view {
    std::string title;
    std::string base_path;
    std::string repository_url;
    std::string copyright_holder;
    std::uint32_t copyright_year{};
    std::optional<std::string> status_label;
    std::string version;
    std::string assets_base_url;
    std::string css_url;
    std::string js_url;
    std::string favicon_url;
    std::string prism_js_url;
    std::string prism_css_url;
    std::string social_image_url;
    std::string documentation_url;
    std::string tutorials_url;
    std::string reference_url;
    std::string search_index_url;
    std::string versions_url;
    std::string versions_page_url;
    std::string nav_documentation;
    std::string nav_tutorials;
    std::string nav_reference;
};

}  // namespace bioseq::codegen::website
