module;

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

export module bioseq.codegen.website.model;

import bioseq.codegen.support;

// Source-neutral data model shared by every website stage. It never exposes
// MD4C, Doxygen, pugixml, Clang, or libpq types; parsers project their inputs
// onto these owned trees before any cross-stage artifact is written (see §8).
export namespace bioseq::codegen::website {

using bioseq::codegen::diagnostic;
using bioseq::codegen::source_span;

enum class document_node_kind : std::uint8_t {
    document, paragraph, text, heading, emphasis, strong_text, strikethrough,
    highlight,
    link, image, entity_link, code_span, code_block, admonition, block_quote,
    ordered_list, unordered_list, list_item, table, table_row, table_cell,
    footnote, line_break, code_annotation
};

// Owned document tree. Defined before every container that stores it.
struct document_node {
    document_node_kind kind{};
    std::string text;
    std::string target;
    std::map<std::string, std::string, std::less<>> attributes;
    std::vector<document_node> children;
    std::optional<source_span> source;
};

enum class entity_query_domain : std::uint8_t { any, cpp, sql, canonical_id };

struct unresolved_entity_query {
    entity_query_domain domain{};
    std::string spelling;
    std::optional<std::string> label;
    source_span source;
};

struct site_version {
    std::string path_component;        // for example v0.1
    std::vector<std::uint32_t> parts;  // numeric sort, never lexical sort
};

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

enum class code_language : std::uint8_t { text, bash, sql, cpp, cmake, json, yaml };

struct code_annotation {
    std::string target_block_id;
    std::string title;
    std::vector<document_node> commentary;
    std::string exact_match;
    std::optional<std::size_t> occurrence;
    source_span source;
};

enum class sql_expectation : std::uint8_t { success, error };

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

struct tutorial {
    tutorial_metadata metadata;
    site_version version;
    std::string source_path;  // repository-relative source path
    std::vector<document_node> content;
    std::vector<sql_example> sql_examples;
    std::vector<code_annotation> annotations;
    std::vector<unresolved_entity_query> entity_queries;
    std::optional<std::string> previous_slug;
    std::optional<std::string> next_slug;
};

// Aggregate written by the `website plan` stage (schema bioseq.website.plan/v1).
struct plan_input {
    std::string path;          // repository-relative forward-slash path
    std::string content_hash;  // content digest (SHA-256 once §6 promotion lands)
};

struct website_plan {
    std::string schema_version;
    std::string parser_version;         // Markdown/frontmatter parser schema
    std::string binding_schema_version; // template binding-schema version (§5.1)
    site_version version;
    std::string base_path;
    std::string source_revision;
    std::string site_title;
    std::vector<tutorial> tutorials;
    std::vector<plan_input> inputs;
    std::string signature;
};

// SQL result model, independent of libpq.
enum class sql_result_kind : std::uint8_t { rows, command, error };
enum class sql_phase : std::uint8_t { session_setup, setup, visible, teardown, cleanup };

struct sql_column {
    std::string name;
    std::uint32_t type_oid{};
    std::string type_name;
};

struct sql_cell {
    bool is_null{};
    std::string text;
};

struct sql_result_set {
    sql_result_kind kind{};
    std::vector<sql_column> columns;
    std::vector<std::vector<sql_cell>> rows;
    std::string command_tag;
    std::optional<std::uint64_t> affected_rows;
    std::optional<std::string> sqlstate;
    std::string primary_message;
    std::string detail;
    std::string hint;
};

struct sql_notice {
    sql_phase phase{};
    std::string severity;
    std::string sqlstate;
    std::string message;
};

struct sql_example_result {
    std::string example_id;
    std::vector<sql_result_set> visible_results;
    std::vector<sql_notice> notices;
};

struct sql_example_measurement {
    std::string example_id;
    std::chrono::milliseconds elapsed{};
};

// Entity/search model, shared across Doxygen, Clang, SQL, Markdown, templates.
enum class entity_domain : std::uint8_t { cpp, sql };
enum class entity_kind : std::uint8_t {
    namespace_, module, class_, struct_, union_, enum_, enum_value, concept_,
    type_alias, function, variable, sql_schema, sql_type, sql_function,
    overload_group
};

struct pg_entrypoint_candidate {
    std::string symbol;
    std::string normalized_signature;
    source_span declaration;
    std::string owning_module;
};

struct parameter_documentation {
    std::string name;
    std::string type;
    std::optional<std::string> default_value;
    std::vector<document_node> description;
};

struct documented_entity {
    std::string canonical_id;
    entity_domain domain{};
    entity_kind kind{};
    std::string qualified_name;
    std::string display_name;
    std::string normalized_signature;
    std::string summary_text;
    std::vector<document_node> documentation;
    std::vector<parameter_documentation> parameters;
    std::optional<std::string> return_type;
    std::optional<std::string> module_name;
    std::optional<source_span> declaration;
    std::optional<std::string> implementation_entity_id;
    std::vector<std::string> overload_ids;
    std::map<std::string, std::string, std::less<>> attributes;
    std::string route_path;  // leading-slash, base-independent site route
};

struct sql_catalog_snapshot {
    std::vector<documented_entity> entities;
    std::string server_version;
    std::string extension_version;
    std::string staged_artifact_sha256;
};

template <typename executor_t>
concept tutorial_sql_executor =
    requires(executor_t& executor, sql_example const& example) {
        { executor.read_catalog() }
            -> std::same_as<std::expected<sql_catalog_snapshot, diagnostic>>;
        { executor.execute(example) }
            -> std::same_as<std::expected<sql_example_result, diagnostic>>;
    };

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

// Site-wide fields bound into every rendered page's context (§14, §15.1). All
// URLs are pre-resolved (base-path aware) so templates never build paths.
struct site_view {
    std::string title;
    std::string base_path;
    std::string repository_url;
    std::string copyright_holder;
    std::uint32_t copyright_year{};
    std::optional<std::string> status_label;
    std::string version;
    std::string css_url;
    std::string js_url;
    std::string favicon_url;
    std::string documentation_url;
    std::string tutorials_url;
    std::string reference_url;
    std::string nav_documentation;
    std::string nav_tutorials;
    std::string nav_reference;
};

struct site_build_request {
    site_version version;
    std::filesystem::path plan_path;
    std::filesystem::path cpp_entity_index_path;
    std::filesystem::path template_directory;
    std::filesystem::path static_directory;
    std::filesystem::path output_directory;
    std::filesystem::path site_config;
    std::string base_path;
};

}  // namespace bioseq::codegen::website
