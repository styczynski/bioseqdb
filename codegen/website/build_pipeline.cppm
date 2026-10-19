module;

#include <algorithm>
#include <array>
#include <boost/json.hpp>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/syscall.h>
#elif defined(__APPLE__)
#include <sys/stdio.h>
#endif
#include <unistd.h>

#if defined(__linux__)
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif
#endif

module bioseq.codegen.website:build_pipeline;

import :front_matter;
import :markdown;
import :markdown_directives;
import :doxygen_reference;
import :cpp_reference_index;
import :reference_index;
import :html_renderer;
import :serialization;
import :search_index;
import :template_renderer;
import :release_assembly;
import bioseq.codegen.website.postgres;
import bioseq.codegen.website.model;
import bioseq.codegen.support;
import bioseq.codegen.sql.signature;
import bioseq.common.hashing;

// Builds the website from tutorials, C++ documentation, and PostgreSQL results.
// Commands split parsing, execution, rendering, and publication into stages.
// Integration self-tests check the parsers and renderers used by those stages.
namespace bioseq::codegen::website {

namespace {

[[nodiscard]] std::string hex64(std::uint64_t value);

void validate_landing_result(const landing_example_view& view);

[[nodiscard]] std::string canonical_result_type_key(
    std::string_view value) {
    const auto canonical_scalar = [](std::string_view scalar) {
        std::string canonical =
            bioseq::codegen::sql::canonical_routine_sql_type(scalar);
        std::string array_suffix;
        while (canonical.ends_with("[]")) {
            canonical.resize(canonical.size() - 2);
            array_suffix += "[]";
        }
        canonical = bioseq::codegen::sql::canonical_routine_sql_type(canonical);
        if (canonical == "timestamptz") canonical = "timestampwithtimezone";
        else if (canonical == "timestamp") canonical = "timestampwithouttimezone";
        else if (canonical == "timetz") canonical = "timewithtimezone";
        else if (canonical == "time") canonical = "timewithouttimezone";
        else if (canonical == "varchar") canonical = "charactervarying";
        else if (canonical == "bpchar" || canonical == "char") canonical = "character";
        return canonical + array_suffix;
    };
    const std::vector<bioseq::codegen::sql::parsed_routine_argument> columns =
        bioseq::codegen::sql::parse_table_result_columns(value);
    const std::string scalar = canonical_scalar(value);
    if (columns.empty() && !scalar.starts_with("table(")) {
        return "scalar:" + scalar;
    }

    std::string result = "table:" + std::to_string(columns.size()) + ':';
    const auto append_component = [&result](std::string_view component) {
        result += std::to_string(component.size());
        result.push_back(':');
        result.append(component);
    };
    for (const auto& column : columns) {
        append_component(column.name);
        append_component(canonical_scalar(column.sql_type));
    }
    return result;
}

[[nodiscard]] std::size_t count_kind(
    const content_node& node,
    content_node_kind kind) {
    std::size_t total = node.kind == kind ? 1 : 0;
    for (const content_node& child : node.children) {
        total += count_kind(child, kind);
    }
    return total;
}

void expect(bool condition, std::string_view detail) {
    if (!condition) {
        throw std::runtime_error("website self-test failed: " + std::string(detail));
    }
}

void bind_public_manifest_identity(
    reference_entity& entity,
    const sql_manifest_public_entity& manifest_entity) {
    entity.attributes["stable_identity"] =
        manifest_entity.stable_identity;
    entity.attributes["installed_identity"] =
        manifest_entity.installed_identity;
    entity.canonical_id = manifest_entity.stable_identity;
}

[[nodiscard]] std::string stable_sql_qualified_name(
    const reference_entity& entity) {
    const auto stable = entity.attributes.find("stable_identity");
    const std::string_view identity =
        stable == entity.attributes.end()
        ? std::string_view(entity.canonical_id)
        : std::string_view(stable->second);
    if (!identity.starts_with("sql:")) {
        return entity.qualified_name;
    }
    const std::size_t body = identity.find(':', 4);
    if (body == std::string_view::npos || body + 1 == identity.size()) {
        throw std::runtime_error(
            "invalid stable SQL identity for website route: "
            + std::string(identity));
    }
    const std::string_view qualified = identity.substr(body + 1);
    return std::string(qualified.substr(0, qualified.find('(')));
}

[[nodiscard]] std::string reference_entity_slug(
    std::string_view qualified) {
    const std::string_view name = qualified.substr(0, qualified.find('('));
    std::vector<std::string> parts;
    std::string current;
    for (const char character : name) {
        if (character == ':' || character == '.' || character == ' ') {
            if (!current.empty()) {
                parts.push_back(std::move(current));
                current.clear();
            }
        } else {
            current.push_back(character);
        }
    }
    if (!current.empty()) {
        parts.push_back(std::move(current));
    }
    std::string joined;
    const std::size_t begin = parts.size() > 2 ? parts.size() - 2 : 0;
    for (std::size_t index = begin; index < parts.size(); ++index) {
        if (!joined.empty()) {
            joined.push_back('-');
        }
        joined += parts[index];
    }
    std::string slug;
    bool previous_dash = false;
    for (const char character : joined) {
        const char lowered = (character >= 'A' && character <= 'Z')
            ? static_cast<char>(character - 'A' + 'a') : character;
        const bool ok = (lowered >= 'a' && lowered <= 'z')
            || (lowered >= '0' && lowered <= '9');
        if (ok) {
            slug.push_back(lowered);
            previous_dash = false;
        } else if (!previous_dash) {
            slug.push_back('-');
            previous_dash = true;
        }
    }
    while (!slug.empty() && slug.back() == '-') {
        slug.pop_back();
    }
    std::size_t lead = 0;
    while (lead < slug.size() && slug[lead] == '-') {
        ++lead;
    }
    slug.erase(0, lead);
    return slug.empty() ? std::string("entity") : slug;
}

[[nodiscard]] std::string reference_entity_relative_route(
    std::string_view version,
    const reference_entity& entity) {
    const std::string domain =
        entity.domain == entity_domain::sql ? "sql" : "cpp";
    const std::string route_name =
        entity.domain == entity_domain::sql
        ? stable_sql_qualified_name(entity)
        : entity.qualified_name;
    return std::string(version) + "/reference/" + domain + "/"
        + reference_kind_token(entity.kind) + "/"
        + reference_entity_slug(route_name) + "-"
        + hex64(bioseq::common::fnv1a_64(entity.canonical_id)) + "/";
}

}  // namespace

// Called by verify_website_pipeline aftrr the reference helpers are defined
void verify_reference_pipeline();
namespace {
void verify_blog_pipeline();
void verify_stable_assets();
void verify_generated_html_template_markers();
void verify_stylesheet_bundles();
void rewrite_entity_link_routes(
    std::vector<content_node>& nodes,
    const std::map<std::string, std::string, std::less<>>& identity_by_reference,
    const std::map<std::string, std::string, std::less<>>& route_by_id,
    const std::string& tooltip_url);
void decorate_code_entity_ranges(
    std::vector<content_node>& nodes,
    const std::map<std::string, std::string, std::less<>>& route_by_id,
    const std::map<std::string, std::string, std::less<>>& qualified_by_id,
    const std::string& tooltip_url,
    const std::string& where);
}  // namespace

/// @brief Checks parser, renderer, and serializer integration.
///
/// Exercises Markdown and Doxygen parsing, entity matching, JSON output, and
/// templates with, small examples. PostgreSQL execution is tested  separately.
void verify_website_pipeline() {
    verify_blog_pipeline();
    verify_stable_assets();
    verify_generated_html_template_markers();
    verify_stylesheet_bundles();
    const auto algorithm = parse_markdown_content(
        "```pseudocode\nRequire: $x$\nFor $i$\n  If $x_i$\n    Return `score` // saved score\n  End If\nEnd For\n```\n");
    const auto algorithm_html = render_content_html(algorithm.children);
    expect(algorithm_html.contains("class=\"algorithm-panel\"")
        && algorithm_html.contains("data-line=\"6\"")
        && algorithm_html.contains("algorithm-line-body\">    <span")
        && algorithm_html.contains("class=\"algorithm-keyword\">Return")
        && algorithm_html.contains("class=\"algorithm-comment\"")
        && algorithm_html.contains("data-math=\"inline\">x_i"),
        "algorithm listing keeps indentation, line numbers, keywords, math and comments");
    std::vector<content_node> linked_sql = parse_markdown_content(
        "```sql\n-- λ bioseq.score(ignore)\nSELECT bioseq.score(t), bioseq.score(t), mmsearch(t,q), 'bioseq.score(t)', $$mmsearch(t,q)$$, x::seq;\n```\n\n`bioseq.seq`\n").children;
    const std::map<std::string, std::string, std::less<>> names{
        {"sql:function:bioseq.score(anyelement)", "bioseq.score"},
        {"sql:overload_group:public.mmsearch", "public.mmsearch"},
        {"sql:type:bioseq.seq", "bioseq.seq"}};
    const std::map<std::string, std::string, std::less<>> routes{
        {"sql:function:bioseq.score(anyelement)", "/score/"},
        {"sql:overload_group:public.mmsearch", "/mmsearch/"},
        {"sql:type:bioseq.seq", "/seq/"}};
    decorate_code_entity_ranges(linked_sql, routes, names, "/entities.json", "self-test");
    const auto ranges = boost::json::parse(linked_sql.front().attributes.at("entity_ranges")).as_array();
    expect(ranges.size() == 4 && ranges[0].as_object().at("start").as_int64() == 33
        && render_content_html(linked_sql).contains("sql:type:bioseq.seq"),
        "automatic SQL links repeat names, resolve bare functions, skip comments/literals, and count UTF-16 offsets");
    auto settings_table = parse_markdown_content(
        "<table><tr><td><code>bioseq.index.insert_maintenance.enabled</code></td>"
        "<td>Original description.</td></tr></table>\n").children;
    decorate_code_entity_ranges(settings_table,
        {{"sql:guc:bioseq.index.insert_maintenance.enabled", "/setting/"}},
        {{"sql:guc:bioseq.index.insert_maintenance.enabled", "bioseq.index.insert_maintenance.enabled"}},
        "/entities.json", "setting-table-test");
    const auto settings_html = render_content_html(settings_table);
    expect(settings_html.contains("<code><a class=\"entity-ref\" href=\"/setting/\"")
        && settings_html.contains("data-entity-data-url=\"/entities.json\"")
        && settings_html.contains("<td>Original description.</td>"),
        "authored configuration table code cells link to setting documentation without changing prose");
    // MD4C.
    const content_node document =
        parse_markdown_content("# Sequence type\n\nThe `bioseq.seq` column stores DNA.\n");
    expect(count_kind(document, content_node_kind::heading) == 1, "markdown heading");
    expect(count_kind(document, content_node_kind::code_span) == 1, "markdown code span");
    const auto project_markup = parse_markdown_content(
        "<a id=\"equation-anchor\"></a>\n\n$x_i + 1$\n\n$$x^2$$\n\n"
        "![A diagram](/static/figures/example.svg)\n\n"
        "<div class=\"project-benchmark\" data-table=\"e2e\" onclick=\"alert(1)\"></div>\n");
    const auto project_html = render_content_html(project_markup.children);
    expect(project_html.contains("id=\"equation-anchor\"")
               && project_html.contains("data-math=\"inline\"")
               && project_html.contains("data-math=\"display\"")
               && project_html.contains("alt=\"A diagram\"")
               && project_html.contains("data-table=\"e2e\"")
               && !project_html.contains("onclick"),
           "project anchors, math, figures and inert table mounts survive rendering");
    const auto unsafe_markup = parse_markdown_content(
        "<a href=\"javascript:alert(1)\">bad</a>\n\n<script>alert(1)</script>\n");
    const auto unsafe_html = render_content_html(unsafe_markup.children);
    expect(!unsafe_html.contains("href=\"javascript:")
               && !unsafe_html.contains("<script>"),
           "authored HTML excludes executable markup and links");

    // Markdown extensions.
    expect(admonition_class("WARNING") == "warning", "admonition class");
    // MD4C lowercases admonition tags. Accept that spelling so warning
    // callouts keep their severity.
    expect(admonition_class("warning") == "warning", "admonition class (md4c tag)");
    expect(admonition_class("nonsense").empty(), "admonition class (unknown)");
    // Check the full Markdown-to-HTML path so the parser must pass the
    // admonition kind to the renderer.
    for (const auto& [markup, expected] : {
             std::pair{"> [!NOTE]\n> Body.\n", "admonition-note"},
             std::pair{"> [!TIP]\n> Body.\n", "admonition-tip"},
             std::pair{"> [!IMPORTANT]\n> Body.\n", "admonition-important"},
             std::pair{"> [!WARNING]\n> Body.\n", "admonition-warning"},
             std::pair{"> [!CAUTION]\n> Body.\n", "admonition-caution"}}) {
        const content_node parsed = parse_markdown_content(markup);
        expect(render_content_html(parsed.children).contains(expected),
               "admonition severity survives render");
    }
    // Doxygen can put lists inside a paragraph. Check that HTML places the
    // list outside p, including when text appears before and after the list.
    content_node list_text{
        .kind = content_node_kind::paragraph,
        .children = {{.kind = content_node_kind::text, .text = "Zero copy."}}};
    content_node list_item{
        .kind = content_node_kind::list_item,
        .children = {list_text}};
    content_node unordered{
        .kind = content_node_kind::unordered_list,
        .children = {list_item}};
    content_node mixed_paragraph{
        .kind = content_node_kind::paragraph,
        .children = {
            {.kind = content_node_kind::text, .text = "Wins:"},
            unordered,
            {.kind = content_node_kind::text, .text = "Done."}}};
    expect(render_content_html({mixed_paragraph})
               == "<p class=\"doc-paragraph\">Wins:</p>"
                  "<ul><li><p class=\"doc-paragraph\">Zero copy.</p></li></ul>"
                  "<p class=\"doc-paragraph\">Done.</p>",
           "mixed Doxygen paragraph renders block list outside p");
    content_node ordered_list = unordered;
    ordered_list.kind = content_node_kind::ordered_list;
    content_node list_only{
        .kind = content_node_kind::paragraph,
        .children = {ordered_list}};
    expect(render_content_html({list_only})
               == "<ol><li><p class=\"doc-paragraph\">Zero copy.</p></li></ol>",
           "list-only Doxygen paragraph renders without empty p wrapper");
    const auto reference =
        parse_entity_query("entity:sql:type:bioseq.seq", source_span{});
    expect(reference.has_value() && reference->domain == entity_query_domain::sql,
           "entity reference domain");

    // Front matter parsed with yaml-cpp.
    const front_matter_sections split = separate_front_matter(
        "---\ntitle: T\ndescription: d\norder: 1\nkeywords: [k]\n---\nbody\n");
    const tutorial_metadata metadata =
        parse_tutorial_metadata(split.yaml, "smoke.md", "smoke");
    expect(metadata.title == "T" && metadata.slug == "smoke", "frontmatter parse");
    const auto landing_copy_fixture = parse_landing_content(
        "hero:\n  title: >-\n    A human title & <test>\n", "landing.yaml");
    expect(landing_copy_fixture.at("hero").at("title") == "A human title & <test>",
           "landing copy retains authored text and folded YAML paragraphs");
    expect(render_inline_json_template("{{ landing.hero.title }}",
               "{\"landing\":{\"hero\":{\"title\":"
                   + json_text(landing_copy_fixture.at("hero").at("title")) + "}}}")
               == "A human title &amp; &lt;test&gt;",
           "landing copy is escaped as text rather than interpreted as HTML");
    const std::string qa_fixture =
        "title: Questions\ndescription: Answers\nintro: Start here\n"
        "author: Author\ndate: '2026-10-18'\nnavigation_label: Topics\n"
        "permalink_label: Link\ncontact_label: Contact\ncontact_text: Ask me\n"
        "contact_action: Write\ngroups:\n  - id: basics\n    title: Basics\n"
        "    questions:\n      - id: is-it-postgres\n        question: Is it PostgreSQL?\n"
        "        answer: >-\n          Yes, with **sequence** columns.\n";
    const auto qa_fixture_content = parse_qa_content(qa_fixture, "qa.yaml");
    expect(qa_fixture_content.groups.size() == 1
        && qa_fixture_content.groups.front().questions.front().id == "is-it-postgres"
        && qa_fixture_content.groups.front().questions.front().answer == "Yes, with **sequence** columns.",
        "Q&A YAML retains ordered questions, stable anchors, and authored Markdown");
    for (const auto& [before, after] : {
            std::pair{"id: is-it-postgres", "id: basics"},
            std::pair{"id: is-it-postgres", "id: ../bad"},
            std::pair{"answer: >-", "unknown: >-"},
            std::pair{"'2026-10-18'", "'2026-02-30'"},
            std::pair{"question: Is it PostgreSQL?", "question: [wrong, type]"}}) {
        auto invalid_qa = qa_fixture;
        invalid_qa.replace(invalid_qa.find(before), std::string_view(before).size(), after);
        bool rejected = false;
        try { (void)parse_qa_content(invalid_qa, "qa.yaml"); }
        catch (const std::exception&) { rejected = true; }
        expect(rejected, "Q&A rejects duplicate anchors, invalid IDs, unknown fields, bad dates, and nontext questions");
    }
    const auto blog_social_fixture = parse_landing_content(
        "medium_posts:\n  introducing-bioseqdb: https://medium.com/@styczynski/article\n"
        "linkedin_posts:\n  introducing-bioseqdb: https://www.linkedin.com/feed/update/urn:li:activity:123\n",
        "blog.yaml");
    expect(blog_social_fixture.at("medium_posts").contains("introducing-bioseqdb")
               && blog_social_fixture.at("linkedin_posts").contains("introducing-bioseqdb"),
           "blog social destinations accept the documented article slugs");
    for (const std::string invalid : {
             "hero:\n  title: one\n  title: two\n",
             "hero: [title]\n", "hero:\n  title: null\n",
             "hero:\n  title: [one, two]\n",
             "hero:\n  introducing-bioseqdb: wrong-group\n",
             "medium_posts:\n  ../article: https://medium.com/article\n",
             "linkedin_posts:\n  wrong_slug: https://www.linkedin.com/article\n"}) {
        bool rejected = false;
        try { (void)parse_landing_content(invalid, "landing.yaml"); }
        catch (const std::exception&) { rejected = true; }
        expect(rejected, "landing copy rejects duplicate or non-text entries");
    }

    // Runnable SQL fence grammar.
    const auto fence = parse_runnable_sql_fence("sql runnable id=probe timeout=5s");
    expect(fence.has_value() && fence->local_id == "probe", "runnable fence parse");
    const sql_example_sections sections = parse_sql_example_sections(
        "-- bioseq:setup\nCREATE TABLE t(v int);\n-- bioseq:visible\n"
        "SELECT 1;\n-- bioseq:teardown\nDROP TABLE t;\n");
    expect(sections.visible == "SELECT 1;", "SQL section split");
    landing_example_view landing_result;
    landing_result.slot = "fixture";
    landing_result.has_result = true;
    landing_result.result.shape = result_shape::rows;
    landing_result.result.columns = {{.name = "score", .type_name = "integer"}};
    landing_result.result.rows = {{{.is_null = false, .text = "1024"}}};
    validate_landing_result(landing_result);
    const auto rejects_landing_result = [](const landing_example_view& view) {
        try { validate_landing_result(view); }
        catch (const std::runtime_error&) { return true; }
        return false;
    };
    landing_result.result.rows.front().front().is_null = true;
    expect(rejects_landing_result(landing_result), "landing rejects NULL placeholders");
    landing_result.result.rows.front().clear();
    expect(rejects_landing_result(landing_result), "landing rejects missing result columns");
    landing_result.result.rows.clear();
    expect(rejects_landing_result(landing_result), "landing rejects empty tables");
    landing_result.result.rows = {{{.is_null = false, .text = "1024"}}};
    landing_result.has_result = false;
    expect(rejects_landing_result(landing_result), "landing requires a captured table");

    // Doxygen XML parsed with pugixml.
    const auto compounds = parse_doxygen_compound_index(
        "<doxygenindex><compound refid=\"ns\" kind=\"namespace\">"
        "<name>bioseq</name></compound></doxygenindex>");
    expect(compounds.size() == 1 && compounds.front().name == "bioseq", "doxygen index");

    // Check Doxygen formatting and links. Unsafe links lose their target,
    // and  unknown tags produce diagnostics while keeping their text.
    const auto description = parse_doxygen_content(
        "<detaileddescription><para>A <s>gone</s> "
        "<computeroutput>bioseq.seq</computeroutput> "
        "<ulink url=\"https://bioseq.test/x\">ok</ulink> "
        "<ulink url=\"javascript:alert(1)\">bad</ulink> "
        "range<ndash/>value <mystery>kept-text</mystery>.</para>"
        "<simplesect kind=\"return\"><para>The decoded value.</para></simplesect>"
        "</detaileddescription>",
        "bioseq::io::seq");
    content_node description_root;
    description_root.kind = content_node_kind::root;
    description_root.children = description.nodes;
    expect(count_kind(description_root, content_node_kind::strikethrough) == 1,
           "doxygen s to strikethrough");
    expect(count_kind(description_root, content_node_kind::code_span) == 1,
           "doxygen computeroutput to code span");
    expect(count_kind(description_root, content_node_kind::link) == 1,
           "doxygen keeps only the safe ulink");
    expect(description.diagnostics.size() == 2,
           "doxygen unsafe-link and unknown-tag diagnostics");
    const std::string description_html = render_content_html(description.nodes);
    expect(description_html.find("admonition-note") != std::string::npos
               && description_html.find("Returns") != std::string::npos,
           "doxygen return section keeps its semantic callout label");

    // Attach Doxygen text to a Clang declaration with the same signature.
    reference_entity clang_seq;
    clang_seq.domain = entity_domain::cpp;
    clang_seq.kind = entity_kind::struct_;
    clang_seq.qualified_name = "bioseq::io::seq";
    clang_seq.normalized_signature = "bioseq::io::seq";
    cpp_reference_index structural;
    structural.entities.push_back(clang_seq);
    reference_entity documented;
    documented.domain = entity_domain::cpp;
    documented.qualified_name = "bioseq::io::seq";
    documented.normalized_signature = "bioseq::io::seq";
    documented.summary_text = "sequence";
    const auto merged = merge_cpp_reference_entities(structural, {documented});
    expect(merged.size() == 1
               && merged.front().canonical_id == "cpp:struct:bioseq::io::seq"
               && merged.front().summary_text == "sequence",
           "entity merge attaches docs by normalized signature");

    // Entity IDs include the language and kind. Function IDs also include
    // the signature so constructor overloads keep different IDs.
    reference_entity ctor_default;
    ctor_default.domain = entity_domain::cpp;
    ctor_default.kind = entity_kind::function;
    ctor_default.qualified_name = "bioseq::io::view::view";
    ctor_default.normalized_signature = "bioseq::io::view::view()";
    reference_entity ctor_args;
    ctor_args.domain = entity_domain::cpp;
    ctor_args.kind = entity_kind::function;
    ctor_args.qualified_name = "bioseq::io::view::view";
    ctor_args.normalized_signature = "bioseq::io::view::view(const char *, std::size_t)";
    expect(make_reference_entity_id(ctor_default) == "cpp:function:bioseq::io::view::view()"
               && make_reference_entity_id(ctor_args)
                      == "cpp:function:bioseq::io::view::view(const char *, std::size_t)"
               && make_reference_entity_id(ctor_default) != make_reference_entity_id(ctor_args),
           "canonical id keys function overloads by normalized signature");

    // Missing documentation for a required entity must fail coverage.
    reference_entity undocumented;
    undocumented.domain = entity_domain::cpp;
    undocumented.kind = entity_kind::struct_;
    undocumented.qualified_name = "bioseq::io::internal";
    undocumented.canonical_id = "cpp:struct:bioseq::io::internal";
    const auto coverage = evaluate_reference_coverage(
        {merged.front(), undocumented}, {"cpp:struct:bioseq::io::internal"}, {});
    expect(coverage.public_total == 2 && coverage.documented_total == 1
               && coverage.fatal_missing.size() == 1,
           "coverage flags required undocumented entity");

    // The SQL manifest selects puilic documentation. $extension identities
    // stay the same  across installations, while installed identities name
    // the schema used in the live catalog.
    const sql_manifest_public_projection sql_manifest =
        parse_sql_manifest_public_projection(
            R"json({
              "schema_revision": 1,
              "authority_mode": "annotated_cpp",
              "extension_schema": "public",
              "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
              "entities": [
                {
                  "identity": "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)",
                  "installed_identity": "sql:function:public.mmsearch(bioseq.seq,bioseq.seq)",
                  "kind": "function",
                  "name": "mmsearch",
                  "source_module": "bioseq.pg",
                  "source_path": "bioseq/backend/pg/extension/index.cppm",
                  "source_line": 42,
                  "public_documentation": true,
                  "brief": "Aligns a query sequence to a target sequence.",
                  "details": "Returns the best alignment.",
                  "parameters": [
                    {"name":"query","sql_type":"bioseq.seq","has_default":false,"documentation":"Query sequence."},
                    {"name":"target","sql_type":"bioseq.seq","has_default":false}
                  ],
                  "result_sql_type": "double precision"
                },
                {
                  "identity": "sql:sequence:bioseq_internal.sequence_source_epoch_seq",
                  "installed_identity": "sql:sequence:bioseq_internal.sequence_source_epoch_seq",
                  "kind": "sequence",
                  "public_documentation": false
                },
                {
                  "identity": "sql:function:$extension.count_transition(bigint,\"any\")",
                  "installed_identity": "sql:function:public.count_transition(bigint,\"any\")",
                  "kind": "function",
                  "cpp_declaration": "fixture::count_transition",
                  "public_documentation": false,
                  "parameters": [
                    {"name":"state","sql_type":"bigint","has_default":false},
                    {"name":"value","sql_type":"quoted_any","has_default":false}
                  ]
                },
                {
                  "identity": "sql:aggregate:$extension.vcount:transition=fixture::count_transition",
                  "installed_identity": "sql:aggregate:public.vcount:transition=fixture::count_transition",
                  "kind": "aggregate",
                  "name": "vcount",
                  "public_documentation": false,
                  "options": {"transition":"fixture::count_transition"}
                },
                {
                  "identity": "sql:guc-value:bioseq.external.enabled",
                  "installed_identity": "sql:guc-value:bioseq.external.enabled",
                  "kind": "guc_value",
                  "public_documentation": false
                }
              ]
            })json",
            "sql-manifest.json");
    expect(sql_manifest.extension_name == "bioseq"
               && sql_manifest.entities.size() == 1
               && sql_manifest.entities.front().stable_identity
                    == "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)"
               && sql_manifest.entities.front().installed_identity
                    == "sql:function:public.mmsearch(bioseq.seq,bioseq.seq)"
               && sql_manifest.catalog_schemas
                    == std::vector<std::string>{"public"}
               && sql_manifest.restricted_sequence_names
                    == std::vector<std::string>{
                        "bioseq_internal.sequence_source_epoch_seq"}
               && std::ranges::find(
                    sql_manifest.stable_identities,
                    "sql:guc-value:bioseq.external.enabled")
                    != sql_manifest.stable_identities.end(),
           "SQL manifest derives public catalog and internal ACL identities");
    expect(sql_manifest.documentation_entities.size() == 4
               && sql_manifest.documentation_schemas
                    == std::vector<std::string>{"bioseq_internal", "public"}
               && sql_manifest.entities.front().parameters.front().description.front().text
                    == "Query sequence.",
           "automatic documentation preserves source parameters without expanding runnable ACL grants");
    expect(std::ranges::any_of(sql_manifest.documentation_entities, [](const auto& entity) {
               return entity.kind == "aggregate"
                   && entity.catalog_identity == "sql:aggregate:public.vcount(\"any\")";
           }), "aggregate catalog identities use emitted SQL types rather than annotation aliases");
    reference_entity typed_entity;
    typed_entity.domain = entity_domain::sql;
    typed_entity.kind = entity_kind::sql_type;
    typed_entity.canonical_id = "sql:type:bioseq.options";
    typed_entity.qualified_name = "bioseq.options";
    typed_entity.fields.push_back(parameter_description{
        .name = "kmer_size", .type = "integer", .default_value = "15",
        .description = {content_node{.kind = content_node_kind::text,
                                     .text = "Length of each minimizer k-mer."}},
    });
    typed_entity.enum_values = {"first", "second"};
    const auto restored_type = deserialize_reference_entities(
        serialize_sql_entities({typed_entity}));
    const auto type_tooltip = parse_json_value(serialize_reference_tooltips({typed_entity}));
    const auto& tooltip_record = type_tooltip.at("entities").at(typed_entity.canonical_id);
    expect(restored_type.size() == 1 && restored_type.front().fields.size() == 1
               && restored_type.front().fields.front().type == "integer"
               && restored_type.front().fields.front().default_value == "15"
               && restored_type.front().enum_values == typed_entity.enum_values
               && tooltip_record.at("fields").as_array().size() == 1
               && tooltip_record.at("enum_values").as_array().size() == 2,
           "SQL fields and enum values survive model serialization and tooltip publication");
    typed_entity.attributes["stable_identity"] = typed_entity.canonical_id;
    typed_entity.attributes["sql_manifest_script_sha256"] = "internal-hash";
    typed_entity.attributes["constraint positive"] = "CHECK (kmer_size > 0)";
    const std::string public_type_view = serialize_reference_entity(typed_entity);
    expect(public_type_view.find("internal-hash") == std::string::npos
               && public_type_view.find("stable_identity") == std::string::npos
               && public_type_view.find("constraint positive") != std::string::npos,
           "entity pages retain useful constraints and omit build identity metadata");
    const auto sql_manifest_rejects = [](std::string_view json) {
        try {
            (void)parse_sql_manifest_public_projection(
                json, "sql-manifest.json");
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    const auto manifest_with_hidden_guc =
        [](std::string_view identity_kind) {
            const std::string identity =
                "sql:" + std::string(identity_kind) + ":bioseq.test_guc";
            return std::string{R"json({
              "schema_revision": 1,
              "authority_mode": "annotated_cpp",
              "extension_schema": "public",
              "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
              "entities": [{
                "identity": ")json"}
                + identity
                + R"json(",
                "installed_identity": ")json"
                + identity
                + R"json(",
                "kind": "guc_value",
                "public_documentation": false
              }, {
                "identity": "sql:type:$extension.test_type",
                "installed_identity": "sql:type:public.test_type",
                "kind": "type",
                "name": "test_type",
                "source_module": "bioseq.test",
                "source_path": "bioseq/test.cppm",
                "source_line": 1,
                "public_documentation": true,
                "brief": "Test type.",
                "details": "",
                "parameters": [],
                "result_sql_type": "public.test_type"
              }]
            })json";
        };
    expect(sql_manifest_rejects(
               manifest_with_hidden_guc("guc_value"))
               && sql_manifest_rejects(
                   manifest_with_hidden_guc("function")),
           "SQL manifest rejects incorrect guc_value identity prefixes");
    expect(sql_manifest_rejects(
               R"json({
                 "schema_revision": 1,
                 "authority_mode": "annotated_cpp",
                 "extension_name": null,
                 "extension_schema": "public",
                 "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "entities": []
               })json")
               && sql_manifest_rejects(
               R"json({
                 "schema_revision": 1,
                 "authority_mode": "annotated_cpp",
                 "extension_name": "",
                 "extension_schema": "public",
                 "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "entities": []
               })json")
               && sql_manifest_rejects(
               R"json({
                 "schema_revision": 1,
                 "authority_mode": "annotated_cpp",
                 "extension_name": 7,
                 "extension_schema": "public",
                 "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "entities": []
               })json"),
           "SQL manifest rejects present invalid extension names");
    expect(sql_manifest_rejects(
               R"json({
                 "schema_revision": 2,
                 "authority_mode": "annotated_cpp",
                 "extension_name": "bioseq",
                 "extension_schema": "public",
                 "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "entities": []
               })json")
               && sql_manifest_rejects(
               R"json({
                 "schema_revision": 1,
                 "authority_mode": "annotated_cpp",
                 "extension_name": "bioseq",
                 "extension_schema": "public",
                 "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "entities": [{
                   "identity": "sql:type:$extension.t",
                   "installed_identity": "sql:type:public.t",
                   "kind": "type",
                   "public_documentation": "yes",
                   "brief": "T.",
                   "details": ""
                 }]
               })json")
               && sql_manifest_rejects(
               R"json({
                 "schema_revision": 1,
                 "authority_mode": "annotated_cpp",
                 "extension_name": "bioseq",
                 "extension_schema": "public",
                 "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "entities": [
                   {"identity":"sql:type:bioseq.a",
                    "installed_identity":"sql:type:bioseq.a",
                    "kind":"type","public_documentation":false},
                   {"identity":"sql:type:bioseq.b",
                    "installed_identity":"sql:type:bioseq.a",
                    "kind":"type","public_documentation":false}
                 ]
               })json")
               && sql_manifest_rejects(
               R"json({
                 "schema_revision": 1,
                 "manifest_version": 1,
                 "authority_mode": "annotated_cpp",
                 "extension_name": "bioseq",
                 "extension_schema": "public",
                 "script_sha256": "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                 "entities": []
               })json"),
           "SQL manifest rejects revision drift, legacy keys, malformed visibility, and duplicate identities");

    // Check overload-group links and page data.
    verify_reference_pipeline();

    // Clang 22 must include exported declarations and exclude the hidden namespace.
    const cpp_reference_index clang_index = scan_cpp_reference_source(
        "export module bioseq.demo;\n"
        "export namespace demo {\n"
        "  struct Sequence { int length; };\n"
        "  int len(const Sequence& value);\n"
        "}\n"
        "namespace hidden { struct Internal { int y; }; }\n",
        {"-std=c++20", "-xc++"});
    const auto has_entity = [&clang_index](std::string_view name) -> bool {
        return std::ranges::any_of(
            clang_index.entities, [name](const reference_entity& candidate) -> bool {
                return candidate.qualified_name == name;
            });
    };
    expect(has_entity("demo::Sequence"), "clang records exported struct");
    expect(has_entity("demo::len"), "clang records exported function");
    expect(!has_entity("hidden::Internal"), "clang excludes non-exported namespace");
    expect(clang_index.clang_release.starts_with("22"), "clang version is 22");

    // Check the module name and empty import list of a leaf module
    const module_graph_unit demo_unit = scan_module_graph_from_code(
        "export module bioseq.demo.leaf;\n"
        "export namespace demo { struct Node { int value; }; }\n",
        {"-std=c++20", "-xc++"});
    expect(demo_unit.provided_module == "bioseq.demo.leaf",
           "module graph records the provided module");
    expect(demo_unit.exported_imports.empty(),
           "a unit with no import has no public edge");

    // Check search JSON fields, the final newline, and HTML-safe JSON escapes.
    search_index_entry probe_entry = build_entity_search_entry(merged.front());
    probe_entry.route = "/v0.1/reference/cpp/struct/seq/";
    const std::string search_json = serialize_search_index({probe_entry});
    expect(search_json.ends_with("\n"), "search json newline");
    expect(search_json.find("\"name_tokens\"") != std::string::npos,
           "search record carries normalized name tokens");
    const std::string escaped = apply_html_safe_escapes("a<b>&c");
    expect(escaped.find("\\u003c") != std::string::npos, "json html-safe escape");

    // Saving and reloading entities must keep detailed Doxygen text,
    // parameter descriptions, and links to C++ implementations. Rendering
    // uses these fields for reference pages and tooltips.
    reference_entity persisted;
    persisted.canonical_id = "cpp:function:bioseq::demo(int)";
    persisted.domain = entity_domain::cpp;
    persisted.kind = entity_kind::function;
    persisted.qualified_name = "bioseq::demo";
    persisted.display_name = "demo";
    persisted.normalized_signature = "int bioseq::demo(int value)";
    persisted.summary_text = "Demonstrates entity persistence.";
    content_node detail;
    detail.kind = content_node_kind::paragraph;
    detail.children.push_back(content_node{
        .kind = content_node_kind::text,
        .text = "Detailed Doxygen prose."});
    content_node doxygen_reference;
    doxygen_reference.kind = content_node_kind::entity_link;
    doxygen_reference.target = "doxygen-ref-demo";
    doxygen_reference.children.push_back(content_node{
        .kind = content_node_kind::text,
        .text = "demo target"});
    detail.children.push_back(std::move(doxygen_reference));
    persisted.detail_content.push_back(detail);
    parameter_description parameter;
    parameter.name = "value";
    parameter.type = "int";
    parameter.description.push_back(content_node{
        .kind = content_node_kind::text,
        .text = "Input value."});
    persisted.parameters.push_back(parameter);
    persisted.implementation_entity_id = "cpp:function:bioseq::demo_impl(int)";
    const std::vector<reference_entity> persisted_round_trip =
        deserialize_reference_entities(serialize_cpp_reference_entities({persisted}, "22.0.0"));
    expect(persisted_round_trip.size() == 1
               && persisted_round_trip.front().detail_content.size() == 1
               && persisted_round_trip.front().detail_content.front().children.size() == 2
               && persisted_round_trip.front().detail_content.front().children.front().text
                      == "Detailed Doxygen prose."
               && persisted_round_trip.front().parameters.size() == 1
               && persisted_round_trip.front().parameters.front().description.size() == 1
               && persisted_round_trip.front().parameters.front().description.front().text
                      == "Input value."
               && persisted_round_trip.front().implementation_entity_id
               == persisted.implementation_entity_id,
           "entity artifact retains detailed documentation and implementation link");
    site_model executed_model;
    executed_model.schema = "bioseq.website.site_model";
    executed_model.schema_revision = 1;
    executed_model.parser_identity = "bioseq.website.parser";
    executed_model.parser_revision = 1;
    executed_model.binding_schema = "bioseq.website.bindings";
    executed_model.binding_schema_revision = 1;
    executed_model.version.path_component = "v0.1";
    executed_model.version.parts = {0, 1};
    executed_model.cpp_entities.push_back(persisted);
    const site_model rendered_model =
        deserialize_site_model(serialize_site_model(executed_model));
    expect(rendered_model.cpp_entities.size() == 1
               && render_content_html(rendered_model.cpp_entities.front().detail_content)
                      .find("Detailed Doxygen prose.") != std::string::npos
               && rendered_model.cpp_entities.front().parameters.front()
                      .description.front().text == "Input value."
               && rendered_model.cpp_entities.front().implementation_entity_id
                      == persisted.implementation_entity_id,
           "execute site model preserves rich entity documentation for render");
    std::vector<content_node> routed_doxygen =
        rendered_model.cpp_entities.front().detail_content;
    const std::map<std::string, std::string, std::less<>> test_reference_ids{
        {"doxygen-ref-demo", persisted.canonical_id}};
    const std::map<std::string, std::string, std::less<>> test_routes{
        {persisted.canonical_id, "/v0.1/reference/cpp/function/demo/"}};
    rewrite_entity_link_routes(
        routed_doxygen, test_reference_ids, test_routes,
        "/v0.1/entities.test.json");
    const std::string routed_doxygen_html = render_content_html(routed_doxygen);
    expect(routed_doxygen_html.find(
               "href=\"/v0.1/reference/cpp/function/demo/\"") != std::string::npos
               && routed_doxygen_html.find(
                      "data-entity-id=\"cpp:function:bioseq::demo(int)\"")
                      != std::string::npos
               && routed_doxygen_html.find(
                      "data-entity-data-url=\"/v0.1/entities.test.json\"")
                      != std::string::npos,
           "persisted Doxygen refid resolves to a live routed entity tooltip");

    // These search-normalization examples also appear in
    // website/tests/fixtures/search_tokens_golden.json for  the JavaScript tests.
    // Lowercase ASCII while preserving non-ASCII bytes.
    expect(normalize_search_text("Hello, World!") == "hello world",
           "ascii lowercasing + punctuation split");
    expect(normalize_search_text("bioseq.symbol_count") == "bioseq symbol count",
           "dotted identifier splits into tokens");
    // Split the string literals so a following ASCII hex digit ('e') does not get
    // swallowed into the \x escape (\x9Fe would be one out-of-range escape).
    expect(normalize_search_text("Stra\xC3\x9F" "e") == "stra\xC3\x9F" "e"
               && normalize_search_text("STRASSE") == "strasse",
           "eszett is preserved so Strasse != Strasse");
    expect(normalize_search_text("I\xC4\xB1") == "i\xC4\xB1",
           "ASCII I lowercases but dotless i is preserved");
    expect(normalize_search_text("\xC3\xA9") != normalize_search_text("e\xCC\x81"),
           "composed accent differs from decomposed");
    expect(!is_valid_utf8("\xFF"), "invalid utf-8 rejected");

    // Sort search records by route and reject duplicate IDs.
    const std::string ordered = serialize_search_index(
        {search_index_entry{.id = "b", .route = "/z/"}, search_index_entry{.id = "a", .route = "/a/"}});
    expect(ordered.find("\"/a/\"") < ordered.find("\"/z/\""),
           "records sort by route");
    bool duplicate_rejected = false;
    try {
        static_cast<void>(serialize_search_index(
            {search_index_entry{.id = "dup", .route = "/a/"},
             search_index_entry{.id = "dup", .route = "/b/"}}));
    } catch (const std::exception&) {
        duplicate_rejected = true;
    }
    expect(duplicate_rejected, "duplicate search id is fatal");

    // Parse numeric version components.
    expect((parse_release_components("v0.1") == std::vector<std::uint32_t>{0, 1}),
           "version parts");

    // SHA-256 content addressing against NIST test vectors.
    expect(bioseq::common::sha256_hex("abc")
           == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "sha256 abc vector");
    expect(bioseq::common::sha256_hex("")
           == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "sha256 empty vector");

    // Compare result types using the SQL signature parser. PostgreSQL type
    // aliases may match, but TABLE column names, order, and, count must agree.
    const std::string canonical_table_result = canonical_result_type_key(
        "table (created_at timestamptz, n int8)");
    expect(canonical_table_result == canonical_result_type_key(
               "TABLE(created_at timestamp with time zone, n bigint)")
               && canonical_table_result != canonical_result_type_key(
                   "TABLE(created_on timestamp with time zone, n bigint)")
               && canonical_table_result != canonical_result_type_key(
                   "TABLE(n bigint, created_at timestamp with time zone)")
               && canonical_table_result != canonical_result_type_key(
                   "TABLE(created_at timestamp with time zone)")
               && canonical_table_result != canonical_result_type_key(
                   "TABLE(created_at timestamp without time zone, n bigint)"),
           "SQL TABLE result parity preserves names, order, count, and types");
    expect(canonical_result_type_key("char") == canonical_result_type_key("character")
               && canonical_result_type_key("time") == canonical_result_type_key("time without time zone")
               && canonical_result_type_key("timestamp") == canonical_result_type_key("timestamp without time zone")
               && canonical_result_type_key("timetz[]") == canonical_result_type_key("time with time zone[]")
               && canonical_result_type_key("varchar[][]") == canonical_result_type_key("character varying[][]")
               && canonical_result_type_key("int4[]") == canonical_result_type_key("integer[]")
               && canonical_result_type_key("char") != canonical_result_type_key("\"char\"")
               && canonical_result_type_key("timestamp[]") != canonical_result_type_key("timestamptz[]"),
           "SQL catalog matching resolves character/time aliases and arrays without merging distinct types");
    expect(canonical_result_type_key("float8")
                   == canonical_result_type_key("double precision")
               && canonical_result_type_key("int")
                   == canonical_result_type_key("integer")
               && canonical_result_type_key("bool")
                   == canonical_result_type_key("boolean")
               && canonical_result_type_key("varchar")
                   == canonical_result_type_key("character varying")
               && canonical_result_type_key("bpchar")
                   == canonical_result_type_key("character")
               && canonical_result_type_key("timestamptz")
                   == canonical_result_type_key(
                       "timestamp with time zone"),
           "SQL scalar result parity retains exact PostgreSQL aliases");

    // Jinja2Cpp through the separately compiled renderer.
    const std::string rendered = render_inline_template(
        "Hello {{ project }}",
        std::map<std::string, std::string, std::less<>>{{"project", "bioseq"}});
    expect(rendered == "Hello bioseq", "template render");

    // Render a JSON array of objects through a template loop, as page
    // templates do for entities and tutorials.
    const std::string page = render_inline_json_template(
        "{% for e in items %}{{ e.name }}={{ e.n }};{% endfor %}",
        R"({"items":[{"name":"a","n":1},{"name":"b","n":2}]})");
    expect(page == "a=1;b=2;", "template json loop render");

    // Construct the PostgreSQL xecutor to check that it links. The constructor
    // stores the connection string without connecting to PostgreSQL.
    postgres_tutorial_executor executor("host='/nonexistent-bioseq-socket'");
    (void)executor;
}

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string hex64(std::uint64_t value) {
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string out(16, '0');
    for (int index = 15; index >= 0; --index) {
        out[index] = digits[value & 0xF];
        value >>= 4;
    }
    return out;
}

/// @brief Makes a source path  relative to an explicit source root.
/// @param source_path Absolute path or path relative to source_root.
/// @param source_root Absolute root used for source links.
/// @return A normalized path using forward slashes. Symlinks are not resolved.
/// @throws std::runtime_error If the root is not absolute or the normalized path
/// leaves it, names the root itself, or contains backslashes.
[[nodiscard]] std::string canonical_source_path(
    const fs::path& source_path, const fs::path& source_root) {
    const fs::path normalized_root = source_root.lexically_normal();
    if (normalized_root.empty() || !normalized_root.is_absolute()) {
        throw std::runtime_error(
            "source root must be an explicit absolute path: "
            + source_root.generic_string());
    }

    const fs::path normalized_source = source_path.is_absolute()
        ? source_path.lexically_normal()
        : (normalized_root / source_path).lexically_normal();
    const fs::path relative = normalized_source.lexically_relative(normalized_root);
    if (relative.empty() || relative.is_absolute()) {
        throw std::runtime_error(
            "source path cannot be expressed relative to source root: "
            + source_path.generic_string());
    }
    for (const fs::path& component : relative) {
        if (component == ".." || component == ".") {
            throw std::runtime_error(
                "source path escapes or aliases the source root: "
                + source_path.generic_string());
        }
    }
    const std::string result = relative.generic_string();
    if (result.empty() || result.starts_with('/') || result.contains('\\')) {
        throw std::runtime_error(
            "source path is not a canonical repository-relative path: "
            + source_path.generic_string());
    }
    return result;
}

/// @brief Percent-encodes a value inserted into a source URL.
/// @param value Raw UTF-8 bytes to encode.
/// @param preserve_slashes Whether to keep path separators unescaped
/// @return RFC 3986 unreserved characters and optional slashes, with other bytes encoded.
[[nodiscard]] std::string encode_source_url_value(
    std::string_view value, bool preserve_slashes) {
    static constexpr std::string_view hexadecimal = "0123456789ABCDEF";
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char byte : value) {
        const bool unreserved =
            (byte >= 'a' && byte <= 'z')
            || (byte >= 'A' && byte <= 'Z')
            || (byte >= '0' && byte <= '9')
            || byte == '-' || byte == '.' || byte == '_' || byte == '~';
        if (unreserved || (preserve_slashes && byte == '/')) {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hexadecimal[byte >> 4]);
        encoded.push_back(hexadecimal[byte & 0x0F]);
    }
    return encoded;
}

/// @brief Renders a validated source URL template for one declaration.
/// @param source_url_template Template containing revision/path/line exactly once.
/// @param revision Repository revision bound into the source link.
/// @param source_path Canonical path relative to the configured source root.
/// @param line One-based declaration line.
/// @return The fully rendered source URL.
[[nodiscard]] std::string render_source_url(
    std::string source_url_template, std::string_view revision,
    std::string_view source_path, std::size_t line) {
    const auto replace_placeholder =
        [&source_url_template](std::string_view placeholder,
                               std::string_view replacement) -> void {
            const std::size_t position = source_url_template.find(placeholder);
            if (position == std::string::npos) {
                throw std::runtime_error(
                    "validated source URL template lost placeholder "
                    + std::string(placeholder));
            }
            source_url_template.replace(position, placeholder.size(), replacement);
        };
    replace_placeholder("{revision}", encode_source_url_value(revision, false));
    replace_placeholder("{path}", encode_source_url_value(source_path, true));
    replace_placeholder("{line}", std::to_string(line));
    return source_url_template;
}

// Marks invalid command-line options so commands return exit  code 2.
// Other parsing and validation errors return 3.
struct cli_usage_error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[nodiscard]] bool contains_key(
    std::span<std::string_view const> set, std::string_view key) {
    for (const std::string_view candidate : set) {
        if (candidate == key) {
            return true;
        }
    }
    return false;
}

/// @brief Parses the allowed value options and flags.
///
/// Rejects unknown options, positional arguments, and missing values.
/// Repeatable options keep their values in argument order, joined by newlines.
/// Flags store "true", and value options consume the next argument.
[[nodiscard]] std::map<std::string, std::string, std::less<>> parse_options(
    std::span<std::string_view const> args,
    std::span<std::string_view const> value_keys,
    std::span<std::string_view const> flag_keys,
    std::span<std::string_view const> repeatable_value_keys = {}) {
    std::map<std::string, std::string, std::less<>> options;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const std::string_view token = args[index];
        if (!token.starts_with("--")) {
            throw cli_usage_error("unexpected positional argument: "
                                  + std::string(token));
        }
        const std::string key(token.substr(2));
        if (contains_key(flag_keys, key)) {
            if (!options.emplace(key, "true").second) {
                throw cli_usage_error("repeated option --" + key);
            }
            continue;
        }
        if (!contains_key(value_keys, key)) {
            throw cli_usage_error("unknown option --" + key);
        }
        if (index + 1 >= args.size()) {
            throw cli_usage_error("missing value for option --" + key);
        }
        const std::string_view value = args[index + 1];
        if (contains_key(repeatable_value_keys, key)
            && (value.contains('\n') || value.contains('\r'))) {
            throw cli_usage_error(
                "repeatable option --" + key + " contains a line break");
        }
        const auto [position, inserted] =
            options.emplace(key, std::string(value));
        if (!inserted) {
            if (!contains_key(repeatable_value_keys, key)) {
                throw cli_usage_error("repeated option --" + key);
            }
            position->second.push_back('\n');
            position->second.append(value);
        }
        ++index;
    }
    return options;
}

[[nodiscard]] const std::string& require_option(
    const std::map<std::string, std::string, std::less<>>& options,
    std::string_view key) {
    const auto found = options.find(key);
    if (found == options.end()) {
        throw cli_usage_error("missing required option --" + std::string(key));
    }
    return found->second;
}

[[nodiscard]] std::vector<std::string> require_option_values(
    const std::map<std::string, std::string, std::less<>>& options,
    std::string_view key) {
    const std::string& joined = require_option(options, key);
    std::vector<std::string> values;
    std::size_t begin = 0;
    while (begin <= joined.size()) {
        const std::size_t end = joined.find('\n', begin);
        const std::size_t count = end == std::string::npos
            ? joined.size() - begin : end - begin;
        if (count == 0) {
            throw cli_usage_error(
                "empty value for repeatable option --" + std::string(key));
        }
        values.emplace_back(joined.substr(begin, count));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return values;
}

/// @brief Writes an artifact through a temporary file and requests mode 0600.
///
/// New temporary files use umask 0077. Rename replaces the destination aftnr
/// writing. If the content is unchanged, keep its modification time and
/// request  the private permissions again.
void write_private_artifact(const fs::path& path, std::string_view content) {
    std::error_code exists_error;
    if (fs::exists(path, exists_error) && !exists_error) {
        if (read_artifact_bytes(path) == content) {
            std::error_code permission_error;
            fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                            fs::perm_options::replace, permission_error);
            return;
        }
    }
    if (path.has_parent_path()) {
        fs::create_directories(path.parent_path());
    }
    const fs::path temp = fs::path(path).concat(".tmp");
    const ::mode_t previous_mask = ::umask(0077);
    try {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            throw std::runtime_error("cannot open for write: " + temp.string());
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.flush();
        if (!out) {
            throw std::runtime_error("write failed: " + temp.string());
        }
    } catch (...) {
        ::umask(previous_mask);
        throw;
    }
    ::umask(previous_mask);
    fs::rename(temp, path);
    std::error_code permission_error;
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, permission_error);
}

[[nodiscard]] std::string content_relative_path(
    const fs::path& path, const fs::path& source) {
    std::error_code error;
    const fs::path relative = fs::relative(path, source, error);
    const fs::path chosen = (error || relative.empty()) ? path : relative;
    return chosen.generic_string();
}

void collect_examples_and_queries(
    const content_node& node,
    const std::string& slug,
    const std::string& version_component,
    std::vector<sql_example>& examples,
    std::vector<unresolved_entity_query>& queries) {
    if (node.kind == content_node_kind::code_block) {
        const auto language = node.attributes.find("language");
        const auto info = node.attributes.find("info");
        if (language != node.attributes.end() && language->second == "sql"
            && info != node.attributes.end()) {
            if (const auto fence = parse_runnable_sql_fence(info->second)) {
                const sql_example_sections sections = parse_sql_example_sections(node.text);
                sql_example example;
                example.local_id = fence->local_id;
                example.canonical_id = version_component + "/tutorial/" + slug + "#"
                    + fence->local_id;
                example.setup_sql = sections.setup;
                example.visible_sql = sections.visible;
                example.teardown_sql = sections.teardown;
                example.expectation = fence->expectation;
                example.expected_sqlstate = fence->expected_sqlstate;
                example.timeout = fence->timeout;
                examples.push_back(std::move(example));
            }
        }
    }
    if (node.kind == content_node_kind::entity_link) {
        // Keep the source location when Markdown parsing supplied one.
        const source_span span = node.source ? *node.source : source_span{};
        if (auto query = parse_entity_query(node.target, span)) {
            queries.push_back(std::move(*query));
        }
    }
    for (const content_node& child : node.children) {
        collect_examples_and_queries(child, slug, version_component, examples, queries);
    }
}

// Keep only visible SQL in runnable code blocks after collecting the full
// examples for execution. This prevents setup and teardown from appearing
// in rendered tutorial content.
void strip_runnable_fence_hidden_sql(std::vector<content_node>& nodes) {
    for (content_node& node : nodes) {
        if (node.kind == content_node_kind::code_block) {
            const auto language = node.attributes.find("language");
            const auto info = node.attributes.find("info");
            if (language != node.attributes.end() && language->second == "sql"
                && info != node.attributes.end()
                && parse_runnable_sql_fence(info->second)) {
                node.text = parse_sql_example_sections(node.text).visible;
            }
        }
        strip_runnable_fence_hidden_sql(node.children);
    }
}

// Attach each annotation to an earlier code block in this top-level list.
// Match against the displayed source, then store the range and commentary
// on that, block. Remove the directive from the document. Missing or overlapping
// matches fail the build, as  does an annotated block with no annotations.
void process_annotations(std::vector<content_node>& nodes, const std::string& where) {
    struct block_ref {
        std::string id;
        std::size_t node_index;
        std::string text;   // decoded, displayed source (visible section for runnable)
        bool annotated;
        std::vector<std::pair<std::size_t, std::size_t>> ranges;  // byte offset and length
        std::size_t count;  // annotations attached so far
    };
    std::vector<block_ref> blocks;
    const auto find_block = [&blocks](const std::string& id) -> block_ref* {
        for (block_ref& block : blocks) {
            if (block.id == id) {
                return &block;
            }
        }
        return nullptr;
    };
    // Index ordinary and runnable code blocks in document order
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const content_node& node = nodes[index];
        if (node.kind != content_node_kind::code_block) {
            continue;
        }
        const auto info = node.attributes.find("info");
        if (info == node.attributes.end() || info->second.empty()) {
            continue;
        }
        const fence_descriptor fence = parse_fence_info(info->second);
        if (fence.kind == fence_kind::annotation) {
            continue;
        }
        std::string text = fence.kind == fence_kind::runnable
            ? parse_sql_example_sections(node.text).visible : node.text;
        blocks.push_back(block_ref{.id = fence.id, .node_index = index,
                                   .text = std::move(text), .annotated = fence.annotated,
                                   .ranges = {}, .count = 0});
    }
    // Attach each annotation to its earlier target blok.
    std::vector<std::size_t> directive_nodes;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const content_node& node = nodes[index];
        if (node.kind != content_node_kind::code_block) {
            continue;
        }
        const auto info = node.attributes.find("info");
        if (info == node.attributes.end() || info->second.empty()) {
            continue;
        }
        const fence_descriptor fence = parse_fence_info(info->second);
        if (fence.kind != fence_kind::annotation) {
            continue;
        }
        block_ref* target = find_block(fence.target);
        if (target == nullptr) {
            throw std::runtime_error("annotation target '" + fence.target
                + "' names no displayed block in " + where);
        }
        if (target->node_index >= index) {
            throw std::runtime_error("annotation target '" + fence.target
                + "' must be a prior displayed block in " + where);
        }
        std::vector<std::size_t> positions;
        for (std::size_t pos = target->text.find(fence.match); pos != std::string::npos;
             pos = target->text.find(fence.match, pos + 1)) {
            positions.push_back(pos);
        }
        if (positions.empty()) {
            throw std::runtime_error("annotation match '" + fence.match
                + "' not found in block '" + fence.target + "' in " + where);
        }
        std::size_t start = 0;
        if (fence.occurrence) {
            if (*fence.occurrence > positions.size()) {
                throw std::runtime_error("annotation occurrence "
                    + std::to_string(*fence.occurrence) + " is out of range in " + where);
            }
            start = positions[*fence.occurrence - 1];
        } else {
            if (positions.size() != 1) {
                throw std::runtime_error("annotation match '" + fence.match
                    + "' appears " + std::to_string(positions.size())
                    + " times; add occurrence=N in " + where);
            }
            start = positions.front();
        }
        const std::size_t len = fence.match.size();
        for (const auto& [existing_start, existing_len] : target->ranges) {
            if (start < existing_start + existing_len && existing_start < start + len) {
                throw std::runtime_error("annotation ranges overlap in block '"
                    + fence.target + "' in " + where);
            }
        }
        target->ranges.emplace_back(start, len);
        const std::size_t number = ++target->count;
        content_node commentary = parse_markdown_content(node.text);
        for (const content_node& child : commentary.children) {
            if (child.kind != content_node_kind::code_block) {
                continue;
            }
            const auto child_info = child.attributes.find("info");
            if (child_info != child.attributes.end() && !child_info->second.empty()
                && parse_fence_info(child_info->second).kind != fence_kind::ordinary) {
                throw std::runtime_error("annotation commentary may not contain runnable "
                    "SQL or another annotation in " + where);
            }
        }
        content_node annotation;
        annotation.kind = content_node_kind::code_annotation;
        annotation.attributes["number"] = std::to_string(number);
        annotation.attributes["start"] = std::to_string(start);
        annotation.attributes["len"] = std::to_string(len);
        annotation.attributes["title"] = fence.title;
        annotation.attributes["match"] = fence.match;
        annotation.children = std::move(commentary.children);
        nodes[target->node_index].children.push_back(std::move(annotation));
        directive_nodes.push_back(index);
    }
    for (const block_ref& block : blocks) {
        if (block.annotated && block.count == 0) {
            throw std::runtime_error("block '" + block.id
                + "' is marked annotated but has no annotation in " + where);
        }
    }
    for (auto directive = directive_nodes.rbegin(); directive != directive_nodes.rend();
         ++directive) {
        nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(*directive));
    }
}

// Read the landing front matter slots mapping in its written order.
// Each indented slot_name: block_id line binds a template slot to a runnable
// block on the landing page. A new unindented line ends the mapping.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> parse_landing_slots(
    std::string_view yaml) {
    std::vector<std::pair<std::string, std::string>> slots;
    bool in_slots = false;
    std::size_t begin = 0;
    while (begin <= yaml.size()) {
        const std::size_t end = yaml.find('\n', begin);
        const std::size_t stop = end == std::string_view::npos ? yaml.size() : end;
        std::string_view line = yaml.substr(begin, stop - begin);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        const bool indented = !line.empty() && (line.front() == ' ' || line.front() == '\t');
        const std::string trimmed = trim_ascii_whitespace(std::string(line));
        if (!in_slots) {
            if (trimmed == "slots:") {
                in_slots = true;
            }
        } else if (trimmed.empty()) {
            // blank line inside the block is ignored
        } else if (!indented) {
            break;  // a new top-level key ends the slots block
        } else {
            const std::size_t colon = trimmed.find(':');
            if (colon == std::string::npos) {
                throw std::runtime_error("landing slots: malformed mapping line '"
                                         + trimmed + "'");
            }
            std::string slot = trim_ascii_whitespace(trimmed.substr(0, colon));
            std::string block = trim_ascii_whitespace(trimmed.substr(colon + 1));
            if (slot.empty() || block.empty()) {
                throw std::runtime_error("landing slots: empty slot or block id in '"
                                         + trimmed + "'");
            }
            slots.emplace_back(std::move(slot), std::move(block));
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return slots;
}

// Execution statistics for the build report. They stay outside the site
// model and its hashes so elapsed times do not  change published files.
// Commands use this process-local accumulator without concurrent updates.
struct example_run_stats {
    std::vector<sql_example_measurement> measurements;
    std::uint64_t peak_rows = 0;
    std::uint64_t peak_cells = 0;
    std::uint64_t peak_payload_bytes = 0;
    std::uint64_t catalog_ms = 0;
};

[[nodiscard]] example_run_stats& example_stats() {
    static example_run_stats instance;
    return instance;
}

// Record elapsed time and the largest row count, cell count, and text
// payload seen in any result set from this example.
void record_example_run(std::string_view example_id, std::chrono::milliseconds elapsed,
                        const sql_example_result& outcome) {
    example_run_stats& stats = example_stats();
    stats.measurements.push_back(
        sql_example_measurement{std::string(example_id), elapsed});
    for (const sql_result_set& set : outcome.visible_results) {
        const std::uint64_t rows = set.rows.size();
        const std::uint64_t cols = set.columns.size();
        std::uint64_t payload = 0;
        for (const std::vector<sql_cell>& row : set.rows) {
            for (const sql_cell& cell : row) {
                payload += cell.text.size();
            }
        }
        stats.peak_rows = std::max(stats.peak_rows, rows);
        stats.peak_cells = std::max(stats.peak_cells, rows * cols);
        stats.peak_payload_bytes = std::max(stats.peak_payload_bytes, payload);
    }
}

// Escape a string for build-report JSON, which is written separately
// from the website model.
[[nodiscard]] std::string report_json_string(std::string_view value) {
    std::string out = "\"";
    for (const char raw : value) {
        const unsigned char byte = static_cast<unsigned char>(raw);
        switch (raw) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (byte < 0x20) {
                static constexpr std::string_view hex = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[byte >> 4]);
                out.push_back(hex[byte & 0x0F]);
            } else {
                out.push_back(raw);
            }
        }
    }
    out.push_back('"');
    return out;
}

// Write this stage's report under --report-dir when it is configured.
// These report files are separate from the site manifest.
void write_stage_report(
    const std::map<std::string, std::string, std::less<>>& options,
    std::string_view stage, const std::string& json_body) {
    const auto found = options.find("report-dir");
    if (found == options.end() || found->second.empty()) {
        return;
    }
    std::error_code error;
    fs::create_directories(fs::path(found->second), error);
    publish_artifact_if_changed(
        fs::path(found->second) / (std::string(stage) + ".json"), json_body);
}

struct loaded_sql_manifest_projection {
    std::string path;
    std::string text;
    sql_manifest_public_projection projection;
    bool documentation_only{};
};

[[nodiscard]] std::vector<loaded_sql_manifest_projection>
load_sql_manifest_projections(
    const std::map<std::string, std::string, std::less<>>& options) {
    std::vector<loaded_sql_manifest_projection> loaded;
    std::set<std::string, std::less<>> extension_names;
    std::set<std::string, std::less<>> stable_identities;
    std::set<std::string, std::less<>> installed_identities;
    for (const std::string& path :
         require_option_values(options, "sql-manifest")) {
        std::string manifest_text = read_artifact_bytes(path);
        sql_manifest_public_projection projection =
            parse_sql_manifest_public_projection(manifest_text, path);
        if (!extension_names.insert(projection.extension_name).second) {
            throw std::runtime_error(
                "duplicate SQL manifest extension '"
                + projection.extension_name + "'");
        }
        for (const std::string& identity : projection.stable_identities) {
            if (!stable_identities.insert(identity).second) {
                throw std::runtime_error(
                    "cross-manifest duplicate stable SQL identity '"
                    + identity + "'");
            }
        }
        for (const std::string& identity : projection.installed_identities) {
            if (!installed_identities.insert(identity).second) {
                throw std::runtime_error(
                    "cross-manifest duplicate installed SQL identity '"
                    + identity + "'");
            }
        }
        loaded.push_back(loaded_sql_manifest_projection{
            .path = path,
            .text = std::move(manifest_text),
            .projection = std::move(projection),
        });
    }
    return loaded;
}

void configure_manifest_acl_targets(
    const std::map<std::string, std::string, std::less<>>& options,
    postgres_isolation_settings& settings) {
    if (!settings.requires_extension) {
        return;
    }
    if (!options.contains("sql-manifest")) {
        throw std::runtime_error(
            "extension-backed website isolation requires --sql-manifest "
            "for ACL target discovery");
    }
    std::set<std::string, std::less<>> schemas;
    std::set<std::string, std::less<>> documented_functions;
    std::set<std::string, std::less<>> restricted_sequences;
    for (const loaded_sql_manifest_projection& manifest :
         load_sql_manifest_projections(options)) {
        settings.extension_names.push_back(
            manifest.projection.extension_name);
        schemas.insert(manifest.projection.catalog_schemas.begin(),
                       manifest.projection.catalog_schemas.end());
        for (const sql_manifest_public_entity& entity :
             manifest.projection.entities) {
            if (entity.kind == "function"
                || entity.kind == "sql_function") {
                documented_functions.insert(entity.installed_identity);
            }
        }
        restricted_sequences.insert(
            manifest.projection.restricted_sequence_names.begin(),
            manifest.projection.restricted_sequence_names.end());
    }
    settings.documented_function_schemas.assign(
        schemas.begin(), schemas.end());
    settings.documented_function_identities.assign(
        documented_functions.begin(), documented_functions.end());
    settings.restricted_sequence_relations.assign(
        restricted_sequences.begin(), restricted_sequences.end());
    if (settings.restricted_sequence_relations.empty()) {
        throw std::runtime_error(
            "SQL manifest set has no internal sequence for ACL validation");
    }
}

// A landing example must demonstrate real data. Fail generation if a planner
// or fixture change would otherwise publish an empty table or NULL placeholders.
void validate_landing_result(const landing_example_view& view) {
    if (!view.has_result || view.result.shape != result_shape::rows
        || view.result.columns.empty() || view.result.rows.empty()) {
        throw std::runtime_error("landing example '" + view.slot
            + "' must return a nonempty table");
    }
    for (std::size_t row_index = 0; row_index < view.result.rows.size(); ++row_index) {
        const auto& row = view.result.rows[row_index];
        if (row.size() != view.result.columns.size()) {
            throw std::runtime_error("landing example '" + view.slot
                + "' has a result row with missing columns");
        }
        for (std::size_t column_index = 0; column_index < row.size(); ++column_index) {
            if (row[column_index].is_null) {
                throw std::runtime_error("landing example '" + view.slot
                    + "' returned NULL in column '" + view.result.columns[column_index].name
                    + "', row " + std::to_string(row_index + 1));
            }
        }
    }
}

// Bind landing-page slots to runnable examples and capture  their results.
// Every block must belong to exactly one slot. Unknown IDs and duplicate
// slots fail the build. With a database connection, run each exampl and keep
// its last table result. Otherwise, return visible SQL with no result table
[[nodiscard]] std::vector<landing_example_view> collect_landing_views(
    const fs::path& source, const std::string& version_string,
    const std::map<std::string, std::string, std::less<>>& options) {
    const fs::path landing_md = source / version_string / "landing_examples.md";
    std::error_code probe;
    if (!fs::exists(landing_md, probe) || probe) {
        return {};
    }
    const front_matter_sections split = separate_front_matter(read_artifact_bytes(landing_md));
    const content_node document = parse_markdown_content(split.body);
    std::vector<sql_example> examples;
    std::vector<unresolved_entity_query> queries;
    collect_examples_and_queries(document, "landing", version_string, examples, queries);

    // Index examples by, block ID and reject duplicates.
    std::map<std::string, const sql_example*, std::less<>> by_block;
    for (const sql_example& example : examples) {
        if (!by_block.emplace(example.local_id, &example).second) {
            throw std::runtime_error("landing slots: duplicate runnable block id '"
                                     + example.local_id + "'");
        }
    }
    const std::vector<std::pair<std::string, std::string>> slots =
        parse_landing_slots(split.yaml);
    std::map<std::string, int, std::less<>> slot_names;
    std::set<std::string> referenced_blocks;
    for (const auto& [slot, block] : slots) {
        if (++slot_names[slot] > 1) {
            throw std::runtime_error("landing slots: duplicate slot '" + slot + "'");
        }
        if (!by_block.contains(block)) {
            throw std::runtime_error("landing slots: slot '" + slot
                + "' references unknown block id '" + block + "'");
        }
        if (!referenced_blocks.insert(block).second) {
            throw std::runtime_error("landing slots: block id '" + block
                + "' is bound to more than one slot");
        }
    }
    for (const sql_example& example : examples) {
        if (!referenced_blocks.contains(example.local_id)) {
            throw std::runtime_error("landing slots: runnable block '"
                + example.local_id + "' is not bound to any slot");
        }
    }

    const bool have_connection =
        options.contains("conninfo") || options.contains("isolated");
    std::optional<postgres_isolation_settings> isolation;
    std::string conninfo;
    if (options.contains("isolated")) {
        postgres_isolation_settings settings;
        settings.administrator_connection_info =
            require_option(options, "admin-conninfo");
        settings.socket_directory = require_option(options, "socket-dir");
        settings.server_port = std::stoi(require_option(options, "port"));
        settings.template_database = require_option(options, "template-db");
        configure_manifest_acl_targets(options, settings);
        isolation = std::move(settings);
    } else if (const auto found = options.find("conninfo"); found != options.end()) {
        conninfo = found->second;
    }

    std::vector<landing_example_view> views;
    for (const auto& [slot, block] : slots) {
        const sql_example& example = *by_block.at(block);
        landing_example_view view;
        view.slot = slot;
        view.visible_sql = example.visible_sql;
        if (have_connection) {
            std::expected<sql_example_result, diagnostic> outcome =
                std::unexpected(diagnostic{});
            const auto started = std::chrono::steady_clock::now();
            if (isolation) {
                postgres_tutorial_executor executor("");
                outcome = executor.execute_in_isolated_database(example, *isolation);
            } else {
                postgres_tutorial_executor executor(conninfo);
                outcome = executor.execute(example);
            }
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            if (!outcome) {
                throw std::runtime_error("landing example '" + view.slot + "': ["
                    + outcome.error().code + "] " + outcome.error().message);
            }
            record_example_run("landing:" + example.canonical_id, elapsed, *outcome);
            // A landing example can load data before its final SELECT. Show the
            // last table result so an intermediate bioseq.load() result does not
            // take the place of the  final query output.
            for (auto result = outcome->visible_results.rbegin();
                 result != outcome->visible_results.rend(); ++result) {
                if (result->shape == result_shape::rows) {
                    view.result = *result;
                    view.has_result = true;
                    break;
                }
            }
            validate_landing_result(view);
        }
        views.push_back(std::move(view));
    }
    return views;
}

// Keep published Markdown readable without exposing database fixtures. Fence
// lengths and both CommonMark fence characters are respected.
[[nodiscard]] std::string blog_public_markdown(std::string_view source) {
    const auto container_content = [](std::string_view line) {
        std::size_t offset = 0;
        const auto spaces = [&] {
            while (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t')) ++offset;
        };
        spaces();
        for (;;) {
            if (offset < line.size() && line[offset] == '>') { ++offset; spaces(); continue; }
            const auto marker = offset;
            if (offset + 1 < line.size() && (line[offset] == '-' || line[offset] == '+' || line[offset] == '*')
                && (line[offset + 1] == ' ' || line[offset + 1] == '\t')) {
                offset += 2;
                spaces();
                continue;
            }
            while (offset < line.size() && line[offset] >= '0' && line[offset] <= '9') ++offset;
            if (offset > marker && offset + 1 < line.size() && (line[offset] == '.' || line[offset] == ')')
                && (line[offset + 1] == ' ' || line[offset + 1] == '\t')) {
                offset += 2;
                spaces();
                continue;
            }
            offset = marker;
            break;
        }
        return std::pair{std::string(line.substr(0, offset)), trim_ascii_whitespace(line.substr(offset))};
    };
    std::istringstream input{std::string(source)};
    std::string output, line, fence, block, opening, opening_prefix, content_prefix;
    bool runnable = false;
    int section = 0;
    while (std::getline(input, line)) {
        auto [prefix, trimmed] = container_content(line);
        if (!fence.empty()) {
            // Strip only containers belonging to this fence. A literal > ```
            // inside a top-level code block is code, never its closing fence.
            std::size_t offset = 0;
            for (const char expected : content_prefix) {
                if (expected == '>') {
                    if (offset < line.size() && line[offset] == '>') ++offset;
                    else break;
                } else if (offset < line.size() && (line[offset] == ' ' || line[offset] == '\t')) ++offset;
            }
            trimmed = trim_ascii_whitespace(std::string_view(line).substr(offset));
        }
        if (fence.empty()) {
            const std::size_t count = trimmed.empty() ? 0 : trimmed.find_first_not_of(trimmed.front());
            const std::size_t length = count == std::string::npos ? trimmed.size() : count;
            if (!trimmed.empty() && (trimmed.front() == '`' || trimmed.front() == '~') && length >= 3) {
                fence = trimmed.substr(0, length);
                const std::string info = trim_ascii_whitespace(trimmed.substr(length));
                runnable = info.starts_with("sql") && parse_runnable_sql_fence(info).has_value();
                opening = line;
                opening_prefix = prefix;
                content_prefix = prefix;
                for (char& character : content_prefix)
                    if (character != '>' && character != '\t') character = ' ';
                block.clear();
                section = 0;
            } else output += line + "\n";
        } else if (trimmed.size() >= fence.size()
                   && trimmed.find_first_not_of(fence.front()) == std::string::npos) {
            if (runnable && section != 3) throw std::runtime_error("invalid runnable sections in blog Markdown");
            output += runnable ? opening_prefix + fence + "sql\n" + block : opening + "\n" + block;
            output += line + "\n";
            fence.clear();
        } else if (!runnable) block += line + "\n";
        else if (trimmed == "-- bioseq:setup") section = 1;
        else if (trimmed == "-- bioseq:visible") section = 2;
        else if (trimmed == "-- bioseq:teardown") section = 3;
        else if (section == 2) block += line + "\n";
    }
    if (!fence.empty()) {
        if (runnable) {
            if (section != 3) throw std::runtime_error("invalid runnable sections in blog Markdown");
            output += opening_prefix + fence + "sql\n" + block + content_prefix + fence + "\n";
        } else output += opening + "\n" + block;
    }
    return output;
}

[[nodiscard]] std::string blog_search_text(const std::vector<content_node>& nodes) {
    std::string text;
    const auto collect = [&text](const auto& list, auto&& self) -> void {
        for (const auto& node : list) {
            if (node.kind == content_node_kind::raw_html
                || node.kind == content_node_kind::image) continue;
            if (!node.text.empty()) {
                if (!text.empty()) text += ' ';
                text += node.text;
            }
            self(node.children, self);
        }
    };
    collect(nodes, collect);
    return text;
}

[[nodiscard]] std::uint32_t blog_reading_minutes(const std::vector<content_node>& nodes) {
    std::size_t words = 0;
    const auto count = [&words](const std::vector<content_node>& items, auto&& self) -> void {
        for (const auto& node : items) {
            if (node.kind == content_node_kind::raw_html || node.kind == content_node_kind::image)
                continue;
            bool in_word = false;
            for (const unsigned char character : node.text) {
                const bool letter = character > 127 || (character >= '0' && character <= '9')
                    || (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
                if (letter && !in_word) ++words;
                in_word = letter;
            }
            self(node.children, self);
        }
    };
    count(nodes, count);
    return static_cast<std::uint32_t>(std::max<std::size_t>(1, (words + 199) / 200));
}

void autolink_blog_keywords(std::vector<content_node>& nodes,
                           const std::vector<blog_keyword>& keywords) {
    struct spelling { std::string text; std::size_t keyword; };
    std::vector<spelling> spellings;
    const auto lowercase = [](std::string value) {
        for (char& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        return value;
    };
    for (std::size_t i = 0; i < keywords.size(); ++i)
        for (const auto& alias : keywords[i].aliases)
            spellings.push_back({lowercase(alias), i});
    std::ranges::stable_sort(spellings, [](const auto& left, const auto& right) {
        return left.text.size() > right.text.size();
    });
    const auto word_at = [](std::string_view text, std::size_t offset) {
        while (offset > 0 && (static_cast<unsigned char>(text[offset]) & 0xc0) == 0x80) --offset;
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first < 128) return (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9') || first == '_';
        std::uint32_t point = first;
        std::size_t length = 1;
        if ((first & 0xe0) == 0xc0) { point = first & 0x1f; length = 2; }
        else if ((first & 0xf0) == 0xe0) { point = first & 0x0f; length = 3; }
        else if ((first & 0xf8) == 0xf0) { point = first & 7; length = 4; }
        for (std::size_t i = 1; i < length && offset + i < text.size(); ++i)
            point = (point << 6) | (static_cast<unsigned char>(text[offset + i]) & 0x3f);
        // Unicode spaces and punctuation delimit English terms just as ASCII
        // spaces do; non-ASCII letters still protect whole-word boundaries.
        return !(point == 0xa0 || point == 0xab || point == 0xbb || point == 0x1680
            || (point >= 0x2000 && point <= 0x206f) || point == 0x3000
            || (point >= 0x3001 && point <= 0x303f) || point == 0xfeff);
    };
    std::set<std::size_t> used;
    const auto walk = [&](std::vector<content_node>& list, auto&& self) -> void {
        std::vector<content_node> replaced;
        for (auto& node : list) {
            if (node.kind == content_node_kind::link || node.kind == content_node_kind::entity_link
                || node.kind == content_node_kind::code_span || node.kind == content_node_kind::code_block
                || node.kind == content_node_kind::heading || node.kind == content_node_kind::raw_html
                || node.kind == content_node_kind::image || node.kind == content_node_kind::math_inline
                || node.kind == content_node_kind::math_display) {
                replaced.push_back(std::move(node));
                continue;
            }
            if (node.kind != content_node_kind::text) {
                self(node.children, self);
                replaced.push_back(std::move(node));
                continue;
            }
            const auto lower = lowercase(node.text);
            std::size_t emitted = 0;
            for (std::size_t offset = 0; offset < lower.size(); ++offset) {
                if (offset != 0 && word_at(lower, offset - 1)) continue;
                for (const auto& spelling : spellings) {
                    const auto end = offset + spelling.text.size();
                    if (used.contains(spelling.keyword) || end > lower.size()
                        || lower.compare(offset, spelling.text.size(), spelling.text) != 0
                        || (end < lower.size() && word_at(lower, end))) continue;
                    if (offset > emitted) replaced.push_back(content_node{
                        .kind = content_node_kind::text, .text = node.text.substr(emitted, offset - emitted)});
                    const auto& keyword = keywords[spelling.keyword];
                    content_node link;
                    link.kind = content_node_kind::link;
                    link.target = keyword.url;
                    link.attributes["glossary_term"] = keyword.term;
                    link.attributes["glossary_description"] = keyword.description;
                    link.children.push_back(content_node{.kind = content_node_kind::text,
                        .text = node.text.substr(offset, spelling.text.size())});
                    replaced.push_back(std::move(link));
                    used.insert(spelling.keyword);
                    emitted = end;
                    offset = end - 1;
                    break;
                }
            }
            if (emitted < node.text.size()) {
                node.text.erase(0, emitted);
                replaced.push_back(std::move(node));
            }
        }
        list = std::move(replaced);
    };
    walk(nodes, walk);
}

void verify_blog_pipeline() {
    const auto metadata = parse_blog_metadata(
        "title: Introducing BioseqDB!\ndescription: Sequence queries\ndate: 2026-10-01\nkeywords: [SQL, DNA]\n",
        "blog.md");
    expect(metadata.slug == "introducing-bioseqdb" && metadata.publication_date == "2026-10-01"
        && metadata.category == "bioseqdb",
           "blog routes derive from titles, dates retain ISO form and category defaults to bioseqdb");
    const auto other_metadata = parse_blog_metadata(
        "title: General notes\ndescription: Postgres notes\ndate: 2026-10-01\nkeywords: [SQL]\ncategory: others\n",
        "others.md");
    expect(other_metadata.category == "others", "general database posts retain their separate category");
    for (const auto& extra : {"date: 2026-02-30\n", "date: tomorrow\n", "date: 2026-10-01\nslug: ../escape\n",
                              "date: 2026-10-01\ntitle: duplicate\n", "date: 2026-10-01\ncategory: unknown\n"}) {
        bool rejected = false;
        try { (void)parse_blog_metadata(std::string("title: Test\ndescription: Test\nkeywords: [SQL]\n") + extra, "bad.md"); }
        catch (const std::exception&) { rejected = true; }
        expect(rejected, "blog metadata rejects invalid dates, routes and duplicate keys");
    }
    const std::string raw = "Before\n\n~~~~sql runnable id=blog_probe\n-- bioseq:setup\nCREATE TABLE hidden_fixture(x int);\n"
        "-- bioseq:visible\nSELECT 42;\n-- bioseq:teardown\nDROP TABLE hidden_fixture;\n~~~~\nAfter\n";
    const auto published = blog_public_markdown(raw);
    expect(published == "Before\n\n~~~~sql\nSELECT 42;\n~~~~\nAfter\n",
           "raw blog Markdown preserves prose and visible SQL without hidden fixtures");
    auto search_nodes = parse_markdown_content(raw).children;
    strip_runnable_fence_hidden_sql(search_nodes);
    const auto searchable = blog_search_text(search_nodes);
    expect(searchable.contains("Before") && searchable.contains("SELECT 42;")
        && searchable.contains("After") && !searchable.contains("hidden_fixture"),
        "blog body search includes visible examples but excludes hidden database fixtures");
    tutorial category_round_trip;
    category_round_trip.metadata = other_metadata;
    category_round_trip.search_text = searchable;
    site_model category_model;
    category_model.schema = "bioseq.website.site_model";
    category_model.schema_revision = 1;
    category_model.parser_identity = "bioseq.website.parser";
    category_model.parser_revision = 1;
    category_model.binding_schema = "bioseq.website.bindings";
    category_model.binding_schema_revision = 1;
    category_model.version = {"v0.1", {0, 1}};
    category_model.blog_posts.push_back(std::move(category_round_trip));
    const auto restored = deserialize_site_model(serialize_site_model(category_model)).blog_posts.front();
    expect(restored.metadata.category == "others" && restored.search_text == searchable,
        "category and visible search body survive the executed model round trip");
    for (const auto& nested : {
        std::string("> ```sql runnable id=quoted\n> -- bioseq:setup\n> SELECT 'hidden';\n> -- bioseq:visible\n> SELECT 42;\n> -- bioseq:teardown\n> SELECT 'hidden';\n> ```\n"),
        std::string("- > ```sql runnable id=mixed\n  > -- bioseq:setup\n  > SELECT 'hidden';\n  > -- bioseq:visible\n  > SELECT 42;\n  > -- bioseq:teardown\n  > SELECT 'hidden';\n  > ```\n"),
        std::string("- ```sql runnable id=listed\n  -- bioseq:setup\n  SELECT 'hidden';\n  -- bioseq:visible\n  SELECT 42;\n  -- bioseq:teardown\n  SELECT 'hidden';\n  ```\n")}) {
        const auto public_source = blog_public_markdown(nested);
        expect(!public_source.contains("hidden") && !public_source.contains("bioseq:")
            && public_source.contains("SELECT 42;"), "nested runnable Markdown never leaks fixtures");
    }
    const std::string ordinary = "> ```sql\n> SELECT 1;\n> ```\n\n```text\n> ```\nopaque\n```\n";
    expect(blog_public_markdown(ordinary) == ordinary, "ordinary nested code remains unchanged");
    const auto rebased = rewrite_markdown_destinations(
        "[Docs][reference] [Home](</>)\n\n[reference]: /v0.1/tutorials/intro/\n\n`[Keep](/unchanged)`\n",
        [](std::string_view target) { return "/mounted" + std::string(target); });
    expect(rebased.contains("[reference]: /mounted/v0.1/tutorials/intro/")
        && rebased.contains("[Home](</mounted/>)") && rebased.contains("`[Keep](/unchanged)`"),
        "public Markdown rebases reference and angle destinations without changing code");
    const auto glossary = parse_blog_keywords(
        "keywords:\n  - term: alignment\n    aliases: [sequence alignment]\n"
        "    url: https://en.wikipedia.org/wiki/Sequence_alignment\n"
        "    description: 'Read <this> & learn'\n", "keywords.yaml");
    auto nodes = parse_markdown_content(
        "## alignment\n\n`alignment` [alignment](https://example.org) sequence alignment; alignment.\n").children;
    autolink_blog_keywords(nodes, glossary);
    const auto html = render_content_html(nodes);
    const auto first = html.find("class=\"glossary-link\"");
    expect(first != std::string::npos && html.find("class=\"glossary-link\"", first + 1) == std::string::npos
        && html.contains(">sequence alignment</a>; alignment.")
        && html.contains("Read &lt;this&gt; &amp; learn"),
        "glossary prefers longest aliases, skips code/links/headings, escapes text and links first occurrence only");
    auto punctuation = parse_markdown_content("“alignment”—comparison").children;
    autolink_blog_keywords(punctuation, glossary);
    expect(render_content_html(punctuation).contains(">alignment</a>"),
           "typographic punctuation remains a whole-word boundary");
    bool unsafe_rejected = false;
    try { (void)parse_blog_keywords("keywords:\n  - term: SQL\n    url: javascript:alert(1)\n    description: Unsafe\n", "bad.yaml"); }
    catch (const std::exception&) { unsafe_rejected = true; }
    expect(unsafe_rejected, "glossary rejects unsafe URL schemes");
    site_view site;
    site.title = "BioseqDB";
    const auto context = boost::json::parse(build_page_context(site,
        R"({"page_type":"blog","title":"A &amp; B","description":"&lt;SQL&gt;","canonical":"https://example.org/blog/test/","date":"2026-10-01","modified_date":"2026-10-18","thumbnail_url":"https://example.org/blog/test/thumbnail.jpg","reading_minutes":1,"structured_title":"A & B </script><b> --> {{ value }} {% statement %} {# comment #}","structured_description":"<SQL>","structured_author":"A \"B\"","structured_keywords":["<DNA>"]})", "", ""));
    const auto structured = boost::json::parse(context.at("page").at("jsonld").as_string());
    const auto& article = structured.at("@graph").as_array().front();
    expect(article.at("headline").as_string() == "A & B </script><b> --> {{ value }} {% statement %} {# comment #}"
        && article.at("description").as_string() == "<SQL>"
        && article.at("datePublished").as_string() == "2026-10-01"
        && article.at("dateModified").as_string() == "2026-10-18"
        && article.at("image").as_string() == "https://example.org/blog/test/thumbnail.jpg"
        && !std::string(context.at("page").at("jsonld").as_string()).contains("</script>")
        && !std::string(context.at("page").at("jsonld").as_string()).contains("{{")
        && !std::string(context.at("page").at("jsonld").as_string()).contains("{%")
        && !std::string(context.at("page").at("jsonld").as_string()).contains("{#"),
        "BlogPosting structured data retains original text, source dates and page screenshot safely");
    const std::string pagemap(context.at("page").at("pagemap_html").as_string());
    expect(pagemap.starts_with("<!--\n<PageMap>") && pagemap.ends_with("</PageMap>\n-->")
        && pagemap.find("-->") == pagemap.size() - 3
        && pagemap.contains("A &amp; B &lt;/script&gt;&lt;b&gt; &#45;&#45;&gt;")
        && pagemap.contains("&#123;&#123; value }}")
        && pagemap.contains("name=\"page_count\">1</Attribute>")
        && pagemap.contains("name=\"width\">1200</Attribute>"),
        "PageMap is safe XML in one HTML comment and counts one HTML document");
    expect(structured.at("@graph").as_array()[2].at("@type").as_string() == "Organization"
        && structured.at("@graph").as_array().back().at("itemListElement").as_array().front()
            .at("item").as_string() == "https://example.org/",
        "Article publisher and absolute fallback breadcrumbs are generated");
    const auto docs_context = boost::json::parse(build_page_context(site,
        R"({"page_type":"tutorial","title":"A &amp; B &amp;lt;","canonical":"https://example.org/v0.1/tutorials/example/","breadcrumb":[{"name":"Home","url":"/"},{"name":"A &amp; B","url":"/v0.1/tutorials/example/"}]})", "", ""));
    const auto docs_structured = boost::json::parse(docs_context.at("page").at("jsonld").as_string());
    expect(docs_structured.at("@graph").as_array().front().at("headline").as_string() == "A & B &lt;"
        && !docs_structured.at("@graph").as_array().front().as_object().contains("datePublished")
        && docs_structured.at("@graph").as_array().back().at("itemListElement").as_array().back()
            .at("item").as_string() == "https://example.org/v0.1/tutorials/example/",
        "Documentation metadata decodes template entities once and omits unknown dates");
    const auto faq_context = boost::json::parse(build_page_context(site,
        R"({"page_type":"qa","title":"Questions","canonical":"https://example.org/qa/","structured_questions":[{"question":"Is this an extension?","answer":"Yes. It runs inside PostgreSQL.","url":"/qa/#extension"}]})", "", ""));
    const auto faq_structured = boost::json::parse(faq_context.at("page").at("jsonld").as_string());
    const auto& faq = faq_structured.at("@graph").as_array().front();
    expect(faq.at("@type").as_string() == "FAQPage"
        && faq.at("mainEntity").as_array().front().at("acceptedAnswer").at("text").as_string()
            == "Yes. It runs inside PostgreSQL."
        && faq.at("mainEntity").as_array().front().at("url").as_string()
            == "https://example.org/qa/#extension",
        "Editorial questions use FAQPage with the visible answers and stable question links");
    std::string long_text;
    for (int i = 0; i < 201; ++i) long_text += "word ";
    expect(blog_reading_minutes(parse_markdown_content(long_text).children) == 2,
           "reading time rounds up at 200 visible words per minute");
}

/// @brief Parses tutorials and builds the website plan.
///
/// Reads Markdown files under <source>/<version>/tutorials, collects runnable
/// SQL and entity references, and assigns tutorial order and navigation.
/// The signature includes the base path, version, and input content hashes.
/// Execution and the plan command use this same parser.
[[nodiscard]] site_plan create_site_plan(
    const fs::path& source,
    const std::string& version_string,
    const site_configuration& config,
    const std::string& site_yaml,
    const std::string& site_config_relpath,
    std::string base_path,
    std::string revision,
    bool include_drafts) {
    release_identifier version;
    version.path_component = version_string;
    version.parts = parse_release_components(version_string);

    const fs::path tutorials_dir = source / version_string / "tutorials";
    std::vector<fs::path> markdown_files;
    for (const auto& entry : fs::directory_iterator(tutorials_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") {
            markdown_files.push_back(entry.path());
        }
    }
    std::ranges::sort(markdown_files);

    std::vector<source_input> inputs;
    inputs.push_back(source_input{
        .path = site_config_relpath,
        .content_hash = hex64(bioseq::common::fnv1a_64(site_yaml))});
    const auto landing_relative =
        (fs::path(site_config_relpath).parent_path() / "landing.yaml").generic_string();
    const auto contact_relative =
        (fs::path(site_config_relpath).parent_path() / "contact.yaml").generic_string();
    const auto blog_relative =
        (fs::path(site_config_relpath).parent_path() / "blog.yaml").generic_string();
    const auto qa_relative =
        (fs::path(site_config_relpath).parent_path() / "qa.yaml").generic_string();
    for (const auto& relative : {landing_relative, contact_relative, blog_relative,
                                 qa_relative,
                                 version_string + "/landing_examples.md"}) {
        const fs::path path = source / relative;
        if (!fs::is_regular_file(path)) continue;
        const auto contents = read_artifact_bytes(path);
        if (relative == landing_relative || relative == contact_relative || relative == blog_relative) {
            (void)parse_landing_content(contents, path.generic_string());
        }
        if (relative == qa_relative) (void)parse_qa_content(contents, path.generic_string());
        inputs.push_back(source_input{
            .path = relative,
            .content_hash = hex64(bioseq::common::fnv1a_64(contents))});
    }

    std::vector<tutorial> tutorials;
    for (const fs::path& markdown_path : markdown_files) {
        const std::string text = read_artifact_bytes(markdown_path);
        const front_matter_sections split = separate_front_matter(text);
        const std::string stem = markdown_path.stem().string();
        tutorial item;
        item.metadata = parse_tutorial_metadata(
            split.yaml, markdown_path.generic_string(), stem);
        item.version = version;
        item.source_path = content_relative_path(markdown_path, source);
        const content_node document = parse_markdown_content(split.body);
        item.content = document.children;
        collect_examples_and_queries(document, item.metadata.slug,
                                     version.path_component, item.sql_examples,
                                     item.entity_queries);
        // Attach annotations and check top-level fence attributes before saving
        // the content used for rendering.
        process_annotations(item.content, item.source_path);
        // The render copy must not leak hidden setup/teardown SQL.
        strip_runnable_fence_hidden_sql(item.content);
        std::map<std::string, int, std::less<>> local_ids;
        for (const sql_example& example : item.sql_examples) {
            if (++local_ids[example.local_id] > 1) {
                throw std::runtime_error("duplicate runnable id '" + example.local_id
                                         + "' in " + item.source_path);
            }
        }
        inputs.push_back(source_input{
            .path = item.source_path,
            .content_hash = hex64(bioseq::common::fnv1a_64(text))});
        if (item.metadata.draft && !include_drafts) {
            continue;
        }
        tutorials.push_back(std::move(item));
    }

    std::ranges::sort(tutorials, [](const tutorial& lhs, const tutorial& rhs) {
        return lhs.metadata.order < rhs.metadata.order;
    });
    std::map<std::string, int, std::less<>> slug_counts;
    for (std::size_t index = 0; index < tutorials.size(); ++index) {
        if (index > 0
            && tutorials[index].metadata.order == tutorials[index - 1].metadata.order) {
            throw std::runtime_error("tutorials share order "
                + std::to_string(tutorials[index].metadata.order));
        }
        if (++slug_counts[tutorials[index].metadata.slug] > 1) {
            throw std::runtime_error("duplicate tutorial slug "
                + tutorials[index].metadata.slug);
        }
        if (index > 0) {
            tutorials[index].previous_slug = tutorials[index - 1].metadata.slug;
        }
        if (index + 1 < tutorials.size()) {
            tutorials[index].next_slug = tutorials[index + 1].metadata.slug;
        }
    }

    std::vector<blog_keyword> glossary;
    const fs::path glossary_path = source / "blog_keywords.yaml";
    if (fs::is_regular_file(glossary_path)) {
        const auto yaml = read_artifact_bytes(glossary_path);
        glossary = parse_blog_keywords(yaml, glossary_path.generic_string());
        inputs.push_back({content_relative_path(glossary_path, source),
                          hex64(bioseq::common::fnv1a_64(yaml))});
    }
    std::vector<tutorial> posts;
    std::vector<fs::path> post_files;
    if (fs::is_directory(source / "blog")) {
        for (const auto& entry : fs::recursive_directory_iterator(source / "blog"))
            if (entry.is_regular_file() && entry.path().extension() == ".md")
                post_files.push_back(entry.path());
    }
    std::ranges::sort(post_files);
    std::set<std::string> blog_slugs;
    for (const auto& path : post_files) {
        const auto markdown = read_artifact_bytes(path);
        const auto split = separate_front_matter(markdown);
        tutorial item;
        item.metadata = parse_blog_metadata(split.yaml, path.generic_string());
        item.source_path = content_relative_path(path, source);
        item.version = version;
        inputs.push_back({item.source_path, hex64(bioseq::common::fnv1a_64(markdown))});
        if (item.metadata.draft && !include_drafts) continue;
        if (!blog_slugs.insert(item.metadata.slug).second)
            throw std::runtime_error("duplicate blog slug " + item.metadata.slug);
        const auto document = parse_markdown_content(split.body);
        item.content = document.children;
        collect_examples_and_queries(document, item.metadata.slug, "blog",
                                     item.sql_examples, item.entity_queries);
        std::set<std::string> local_ids;
        for (auto& example : item.sql_examples) {
            example.canonical_id = "blog/" + item.metadata.slug + "#" + example.local_id;
            if (!local_ids.insert(example.local_id).second)
                throw std::runtime_error("duplicate runnable id '" + example.local_id + "' in " + item.source_path);
        }
        process_annotations(item.content, item.source_path);
        strip_runnable_fence_hidden_sql(item.content);
        item.metadata.reading_minutes = blog_reading_minutes(item.content);
        item.search_text = blog_search_text(item.content);
        item.public_markdown = blog_public_markdown(markdown);
        autolink_blog_keywords(item.content, glossary);
        posts.push_back(std::move(item));
    }
    std::ranges::sort(posts, [](const tutorial& left, const tutorial& right) {
        if (left.metadata.publication_date != right.metadata.publication_date)
            return left.metadata.publication_date > right.metadata.publication_date;
        return left.metadata.slug < right.metadata.slug;
    });

    std::uint64_t signature = bioseq::common::fnv1a_64(base_path);
    signature = bioseq::common::hash_combine_64(signature, version.path_component);
    for (const source_input& input : inputs) {
        signature = bioseq::common::hash_combine_64(signature, input.content_hash);
    }

    site_plan plan;
    plan.schema = "bioseq.website.plan";
    plan.schema_revision = 1;
    plan.parser_identity = "bioseq.website.parser";
    plan.parser_revision = 1;
    plan.binding_schema = "bioseq.website.bindings";
    plan.binding_schema_revision = 1;
    plan.version = version;
    plan.base_path = std::move(base_path);
    plan.source_revision = std::move(revision);
    plan.site_title = config.title;
    plan.tutorials = std::move(tutorials);
    plan.blog_posts = std::move(posts);
    plan.inputs = std::move(inputs);
    plan.signature = hex64(signature);
    return plan;
}

// Helpers for building document nodes from SQL results.
[[nodiscard]] content_node make_text_node(std::string value) {
    content_node node;
    node.kind = content_node_kind::text;
    node.text = std::move(value);
    return node;
}

[[nodiscard]] content_node make_emphasis_node(std::string value) {
    content_node node;
    node.kind = content_node_kind::emphasis;
    node.children.push_back(make_text_node(std::move(value)));
    return node;
}

[[nodiscard]] content_node make_paragraph_node(std::vector<content_node> children) {
    content_node node;
    node.kind = content_node_kind::paragraph;
    node.children = std::move(children);
    return node;
}

[[nodiscard]] content_node make_cell_node(bool header, content_node child) {
    content_node node;
    node.kind = content_node_kind::table_cell;
    if (header) {
        node.attributes.emplace("scope", "col");
    }
    node.children.push_back(std::move(child));
    return node;
}

[[nodiscard]] content_node make_row_node(std::vector<content_node> cells) {
    content_node node;
    node.kind = content_node_kind::table_row;
    node.children = std::move(cells);
    return node;
}

// Hide these internal EXPLAIN fields in public examples. Keep timing,
// algorithm choices, fallback counts, and  result row counts.
constexpr std::array<std::string_view, 8> private_explain_fields{
    "Test-build-only", "DSM Test Fixture", "Fully Attested Alignment Jobs",
    "Bioseq Batch ABI Version", "Bioseq Owned RestrictInfo Path Copies",
    "Bioseq Match Placeholder Sites", "Bioseq CPU Prepared Target",
    "Bioseq Requirement Executable"};

[[nodiscard]] std::size_t omit_private_explain_fields(boost::json::value& value) {
    std::size_t omitted = 0;
    if (value.is_object()) {
        for (const std::string_view field : private_explain_fields) {
            omitted += value.as_object().erase(field);
        }
        for (auto& member : value.as_object()) {
            omitted += omit_private_explain_fields(member.value());
        }
    } else if (value.is_array()) {
        for (auto& child : value.as_array()) {
            omitted += omit_private_explain_fields(child);
        }
    }
    return omitted;
}

[[nodiscard]] bool is_private_explain_line(std::string_view line) {
    const auto first = line.find_first_not_of(" \t*");
    if (first == std::string_view::npos) return false;
    line.remove_prefix(first);
    for (const std::string_view field : private_explain_fields) {
        if (line.starts_with(field) && line.size() > field.size()
            && line[field.size()] == ':') return true;
    }
    return false;
}

// Turn visible SQL results into document nodes for later rendering.
// Multiple tables get numbered labels. Notices and errors become callouts,
// and a caption records PostgreSQL and extension versions when available.
[[nodiscard]] std::vector<content_node> make_result_nodes(
    const sql_example_result& result, const std::string& extension_version,
    const std::string& server_version) {
    std::vector<content_node> nodes;
    std::size_t rows_total = 0;
    for (const sql_result_set& set : result.visible_results) {
        if (set.shape == result_shape::rows) {
            ++rows_total;
        }
    }
    std::size_t rows_index = 0;
    for (const sql_result_set& set : result.visible_results) {
        if (set.shape == result_shape::rows) {
            ++rows_index;
            if (rows_total > 1) {
                nodes.push_back(make_paragraph_node(
                    {make_emphasis_node("Result " + std::to_string(rows_index))}));
            }
            auto display_rows = set.rows;
            std::size_t omitted = 0;
            if (set.columns.size() == 1 && set.columns.front().name == "QUERY PLAN") {
                if (set.columns.front().type_name == "json"
                    || set.columns.front().type_name == "jsonb") {
                    for (auto& row : display_rows) {
                        if (row.size() != 1 || row.front().is_null) continue;
                        auto plan = boost::json::parse(row.front().text);
                        const auto count = omit_private_explain_fields(plan);
                        omitted += count;
                        if (count != 0) row.front().text = boost::json::serialize(plan);
                    }
                } else {
                    omitted = std::erase_if(display_rows, [](const auto& row) {
                        return row.size() == 1 && !row.front().is_null
                            && is_private_explain_line(row.front().text);
                    });
                }
            }
            if (omitted != 0) {
                nodes.push_back(make_paragraph_node({make_emphasis_node(
                    "EXPLAIN excerpt: " + std::to_string(omitted)
                    + " internal implementation diagnostics omitted. Execution timings, "
                      "algorithm selections and fallback counters are retained.")}));
            }
            content_node table;
            table.kind = content_node_kind::table;
            table.attributes.emplace("class", "result-table");
            std::vector<content_node> header_cells;
            for (const sql_column& column : set.columns) {
                content_node header = make_cell_node(true, make_text_node(column.name));
                header.children.push_back(
                    content_node{.kind = content_node_kind::line_break});
                content_node type;
                type.kind = content_node_kind::code_span;
                type.text = column.type_name;
                header.children.push_back(std::move(type));
                header_cells.push_back(std::move(header));
            }
            table.children.push_back(make_row_node(std::move(header_cells)));
            for (const std::vector<sql_cell>& row : display_rows) {
                std::vector<content_node> cells;
                for (const sql_cell& cell : row) {
                    cells.push_back(cell.is_null
                        ? make_cell_node(false, make_emphasis_node("NULL"))
                        : make_cell_node(false, make_text_node(cell.text)));
                }
                table.children.push_back(make_row_node(std::move(cells)));
            }
            nodes.push_back(std::move(table));
        } else if (set.shape == result_shape::command) {
            std::vector<content_node> command{
                make_emphasis_node(set.command_tag)};
            if (set.affected_rows) {
                command.push_back(make_text_node(
                    " — " + std::to_string(*set.affected_rows)
                    + (*set.affected_rows == 1 ? " row affected" : " rows affected")));
            }
            nodes.push_back(make_paragraph_node(std::move(command)));
        } else {
            content_node admonition;
            admonition.kind = content_node_kind::admonition;
            admonition.attributes.emplace("class", "warning");
            admonition.children.push_back(make_paragraph_node({make_text_node(
                "SQLSTATE " + set.sqlstate.value_or("") + ": " + set.primary_message)}));
            nodes.push_back(std::move(admonition));
        }
    }
    if (!result.notices.empty()) {
        content_node admonition;
        admonition.kind = content_node_kind::admonition;
        admonition.attributes.emplace("class", "note");
        for (const sql_notice& notice : result.notices) {
            admonition.children.push_back(make_paragraph_node(
                {make_text_node(notice.severity + ": " + notice.message)}));
        }
        nodes.push_back(std::move(admonition));
    }
    if (!extension_version.empty() || !server_version.empty()) {
        nodes.push_back(make_paragraph_node({make_emphasis_node(
            "Executed against Bioseq extension "
            + (extension_version.empty() ? std::string("(unknown)") : extension_version)
            + " on PostgreSQL "
            + (server_version.empty() ? std::string("(unknown)") : server_version))}));
    }
    return nodes;
}

// Insert results after their runnable code blocks, including blocks nested
// in lists or callouts. A block without a captured result keeps only its
// visibl SQL.
void inject_tutorial_results(
    std::vector<content_node>& nodes,
    const std::map<std::string, sql_example_result, std::less<>>& results,
    const std::string& extension_version, const std::string& server_version) {
    std::vector<content_node> rebuilt;
    rebuilt.reserve(nodes.size());
    for (content_node& node : nodes) {
        inject_tutorial_results(node.children, results, extension_version, server_version);
        std::optional<std::string> runnable_id;
        if (node.kind == content_node_kind::code_block) {
            const auto language = node.attributes.find("language");
            const auto info = node.attributes.find("info");
            if (language != node.attributes.end() && language->second == "sql"
                && info != node.attributes.end()) {
                if (const auto fence = parse_runnable_sql_fence(info->second)) {
                    runnable_id = fence->local_id;
                    node.attributes["runnable_id"] = *runnable_id;
                }
            }
        }
        rebuilt.push_back(std::move(node));
        if (runnable_id) {
            if (const auto found = results.find(*runnable_id); found != results.end()) {
                for (content_node& result_node :
                     make_result_nodes(found->second, extension_version, server_version)) {
                    rebuilt.push_back(std::move(result_node));
                }
            }
        }
    }
    nodes = std::move(rebuilt);
}

// Database settings shared by SQL examples. They describe a separate
// database per example, a direct connection, or a build without SQL execution
struct execution_target {
    bool have_connection{};
    std::optional<postgres_isolation_settings> isolation;
    std::string conninfo;
    std::string probe_conninfo;  // connection used to read PostgreSQL and extension versions
};

[[nodiscard]] execution_target resolve_execution_target(
    const std::map<std::string, std::string, std::less<>>& options) {
    execution_target target;
    target.have_connection = options.contains("conninfo") || options.contains("isolated");
    if (options.contains("isolated")) {
        postgres_isolation_settings settings;
        settings.administrator_connection_info =
            require_option(options, "admin-conninfo");
        settings.socket_directory = require_option(options, "socket-dir");
        settings.server_port = std::stoi(require_option(options, "port"));
        settings.template_database = require_option(options, "template-db");
        configure_manifest_acl_targets(options, settings);
        if (const auto found = options.find("conninfo"); found != options.end()) {
            target.probe_conninfo = found->second;
        } else {
            target.probe_conninfo = settings.administrator_connection_info;
        }
        target.isolation = std::move(settings);
    } else if (const auto found = options.find("conninfo"); found != options.end()) {
        target.conninfo = found->second;
        target.probe_conninfo = found->second;
    }
    return target;
}

[[nodiscard]] std::expected<sql_example_result, diagnostic> run_one_example(
    const execution_target& target, const sql_example& example) {
    if (target.isolation) {
        postgres_tutorial_executor executor("");
        return executor.execute_in_isolated_database(example, *target.isolation);
    }
    postgres_tutorial_executor executor(target.conninfo);
    return executor.execute(example);
}

// Maps an entity spelling and language  to an entity ID. Empty values mark
// names shared by different entities, which the resolver rejects as ambiguous.
// The id domain accepts exact IDs across both C++ and SQL.
struct entity_reference_index {
    std::map<std::string, std::string, std::less<>> by_key;

    static std::string domain_key(entity_query_domain domain) {
        switch (domain) {
        case entity_query_domain::sql: return "sql\x1f";
        case entity_query_domain::cpp: return "cpp\x1f";
        case entity_query_domain::canonical_id: return "id\x1f";
        default: return "any\x1f";
        }
    }

    void add(entity_query_domain domain, std::string_view spelling,
             const std::string& canonical_id) {
        if (spelling.empty()) {
            return;
        }
        std::string key = domain_key(domain) + std::string(spelling);
        const auto [iterator, inserted] = by_key.try_emplace(key, canonical_id);
        if (!inserted && iterator->second != canonical_id) {
            iterator->second.clear();  // two entities share this spelling: ambiguous
        }
    }
};

[[nodiscard]] entity_reference_index build_entity_reference_index(
    const std::vector<reference_entity>& cpp_entities,
    const std::vector<reference_entity>& sql_entities) {
    entity_reference_index index;
    // Give a SQL overload group the shared function name. Individual overloads
    // keep their signature IDs so a name-only link opens the group page.
    std::set<std::string, std::less<>> grouped_names;
    std::set<std::string, std::less<>> real_spellings;
    for (const reference_entity& entity : sql_entities) {
        if (entity.kind == entity_kind::overload_group) {
            grouped_names.insert(entity.qualified_name);
        }
    }
    for (const reference_entity& entity : cpp_entities) {
        real_spellings.insert(entity.qualified_name);
    }
    for (const reference_entity& entity : sql_entities) {
        real_spellings.insert(entity.qualified_name);
    }
    const auto add_entity = [&grouped_names, &index](
                                const reference_entity& entity) -> void {
        const entity_query_domain domain = entity.domain == entity_domain::sql
            ? entity_query_domain::sql : entity_query_domain::cpp;
        index.add(domain, entity.canonical_id, entity.canonical_id);
        index.add(entity_query_domain::canonical_id, entity.canonical_id,
                  entity.canonical_id);
        if (const auto installed =
                entity.attributes.find("installed_identity");
            installed != entity.attributes.end()
            && !installed->second.empty()) {
            index.add(domain, installed->second, entity.canonical_id);
            index.add(entity_query_domain::canonical_id,
                      installed->second, entity.canonical_id);
        }
        const bool name_owned_by_group = entity.kind != entity_kind::overload_group
            && entity.domain == entity_domain::sql
            && grouped_names.contains(entity.qualified_name);
        if (!name_owned_by_group) {
            index.add(domain, entity.qualified_name, entity.canonical_id);
            index.add(domain, entity.display_name, entity.canonical_id);
            // The any, domain resolves #name in prose. Matching C++ and SQL names
            // become ambiguous here.
            index.add(entity_query_domain::any, entity.qualified_name,
                      entity.canonical_id);
            index.add(entity_query_domain::any, entity.display_name,
                      entity.canonical_id);
        }
    };
    for (const reference_entity& entity : cpp_entities) {
        add_entity(entity);
    }
    for (const reference_entity& entity : sql_entities) {
        add_entity(entity);
    }
    // Accept bioseq.<name> as an alias for an extension SQL entity in another
    // schema when that name is free. For example, #bioseq.mmsearch can resolve
    // to the public.mmsearch overload  group.
    for (const reference_entity& entity : sql_entities) {
        const std::size_t dot = entity.qualified_name.find('.');
        const std::string schema =
            dot == std::string::npos ? std::string() : entity.qualified_name.substr(0, dot);
        const std::string bare = dot == std::string::npos
            ? entity.qualified_name : entity.qualified_name.substr(dot + 1);
        const bool member_of_group = entity.kind != entity_kind::overload_group
            && grouped_names.contains(entity.qualified_name);
        if (schema == "bioseq" || member_of_group) {
            continue;  // already bioseq-qualified, or the group owns the alias
        }
        const std::string alias = "bioseq." + bare;
        if (real_spellings.contains(alias)) {
            continue;  // a real entity already owns this spelling
        }
        index.add(entity_query_domain::sql, alias, entity.canonical_id);
        index.add(entity_query_domain::any, alias, entity.canonical_id);
    }
    return index;
}

// Recognize #name at the start of text or after whitespace. Names use
// ASCII identifiers joined by . or ::. Convert them to entity links and
// keep the remaining text unchanged.
void split_shorthand_text(const std::string& text,
                          std::vector<content_node>& out) {
    const auto is_start = [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z') || character == '_';
    };
    const auto is_part = [is_start](char character) -> bool {
        return is_start(character) || (character >= '0' && character <= '9');
    };
    std::string pending;
    const auto flush = [&out, &pending]() -> void {
        if (pending.empty()) {
            return;
        }
        if (!out.empty() && out.back().kind == content_node_kind::text) {
            out.back().text += pending;
        } else {
            out.push_back(content_node{.kind = content_node_kind::text, .text = pending});
        }
        pending.clear();
    };
    std::size_t index = 0;
    while (index < text.size()) {
        const bool boundary = index == 0
            || static_cast<unsigned char>(text[index - 1]) <= ' ';
        if (text[index] == '#' && boundary && index + 1 < text.size()
            && is_start(text[index + 1])) {
            std::size_t cursor = index + 1;
            std::string name;
            while (cursor < text.size()) {
                if (is_part(text[cursor])) {
                    name.push_back(text[cursor]);
                    ++cursor;
                } else if (text[cursor] == '.' && cursor + 1 < text.size()
                           && is_start(text[cursor + 1])) {
                    name.push_back('.');
                    ++cursor;
                } else if (text[cursor] == ':' && cursor + 2 < text.size()
                           && text[cursor + 1] == ':' && is_start(text[cursor + 2])) {
                    name += "::";
                    cursor += 2;
                } else {
                    break;
                }
            }
            if (!name.empty()) {
                flush();
                content_node node;
                node.kind = content_node_kind::entity_link;
                node.target = name;
                node.attributes.emplace("shorthand", "1");
                out.push_back(std::move(node));
                index = cursor;
                continue;
            }
        }
        pending.push_back(text[index]);
        ++index;
    }
    flush();
}

// Convert #name in prose to entity links. Leave headings, code, and
// existing links unchanged.
void apply_entity_shorthand(std::vector<content_node>& nodes, bool in_prose) {
    std::vector<content_node> rebuilt;
    for (content_node& node : nodes) {
        if (in_prose && node.kind == content_node_kind::text) {
            split_shorthand_text(node.text, rebuilt);
            continue;
        }
        const bool child_prose = in_prose
            && node.kind != content_node_kind::heading
            && node.kind != content_node_kind::code_span
            && node.kind != content_node_kind::code_block
            && node.kind != content_node_kind::link
            && node.kind != content_node_kind::entity_link;
        apply_entity_shorthand(node.children, child_prose);
        rebuilt.push_back(std::move(node));
    }
    nodes = std::move(rebuilt);
}

// Find each tutorial link's entity ID before rendering. Missing and
// ambiguous targets fail the build. Page URLs are filled in later so the
// saved content can be rendered under another base path.
void resolve_entity_links(std::vector<content_node>& nodes,
                          const entity_reference_index& index,
                          const std::string& where) {
    for (content_node& node : nodes) {
        if (node.kind == content_node_kind::entity_link) {
            // #name searches bsth languages. The explicit wiki-link form selects
            // the domain supplied by  the author
            const bool shorthand = node.attributes.contains("shorthand");
            const std::string display = shorthand ? "#" + node.target
                                                  : "[[" + node.target + "]]";
            unresolved_entity_query query;
            if (shorthand) {
                query.domain = entity_query_domain::any;
                query.spelling = node.target;
            } else if (std::optional<unresolved_entity_query> parsed =
                           parse_entity_query(
                               node.target, node.source ? *node.source : source_span{})) {
                query = std::move(*parsed);
            } else {
                throw std::runtime_error(
                    "malformed entity reference " + display + " in " + where);
            }
            const std::string key =
                entity_reference_index::domain_key(query.domain) + query.spelling;
            const auto found = index.by_key.find(key);
            if (found == index.by_key.end()) {
                throw std::runtime_error(
                    "unresolved entity reference " + display + " in " + where);
            }
            if (found->second.empty()) {
                throw std::runtime_error(
                    "ambiguous entity reference " + display + " in " + where
                    + " (use an explicit sql:/cpp: form)");
            }
            node.attributes["entity_id"] = found->second;
            // Use an explicit label when supplied. For wiki links, hide the domain
            // prefix from the label. Other links keep their Markdown text, with the
            // entity spelling as a fallback for an empty label.
            if (query.label) {
                node.attributes["entity_label"] = *query.label;
            } else if (!shorthand
                       && (node.target.starts_with("sql:")
                           || node.target.starts_with("cpp:")
                           || node.target.starts_with("id:"))) {
                // MD4C uses the wiki-link target as its text. Remove the domain prefix
                // from the displayed label.
                node.attributes["entity_label"] = query.spelling;
            } else if (node.text.empty() && node.children.empty()) {
                node.attributes["entity_label"] = query.spelling;
            }
        }
        resolve_entity_links(node.children, index, where);
    }
}

// Collect referenced entity IDs for documentation checks. Public entities
// linked from tutorials must have a summary for the build to pass.
void collect_referenced_entity_ids(const std::vector<content_node>& nodes,
                                   std::set<std::string>& referenced) {
    for (const content_node& node : nodes) {
        if (const auto found = node.attributes.find("entity_id");
            found != node.attributes.end()) {
            referenced.insert(found->second);
        }
        collect_referenced_entity_ids(node.children, referenced);
    }
}

// An external table may require an accession or other remote lookup predicate.
// Its generated row-type page demonstrates the shape without starting a scan;
// filtered retrieval examples remain on the authored table reference page.
[[nodiscard]] std::pair<std::string, std::string> relation_row_example(
    const reference_entity& relation) {
    const auto method = relation.attributes.find("access_method");
    const bool external = method != relation.attributes.end()
        && (method->second == "bioseq_external" || method->second == "\"bioseq_external\"");
    return {
        external
            ? "Inspect the composite row shape without contacting the external provider. WHERE false returns no rows; use the table's documented examples for filtered data retrieval."
            : "Read whole rows as values of this composite type.",
        "SELECT row_value FROM " + relation.qualified_name + " AS row_value"
            + (external ? " WHERE false;" : " LIMIT 5;")};
}

/// @brief Builds site data that can later  be rendered without PostgreSQL.
///
/// Reads tutorials and reference entities, checks SQL catalog data against the
/// manifests, and resolves links. If a connection is configured, execute the
/// examples and insert their results into tutorial content. Clear hidden SQL
/// before saving and fail if required documentation is missing.
[[nodiscard]] site_model create_site_model(
    const fs::path& source, const std::string& version_string,
    const site_configuration& config, const std::string& site_yaml,
    const std::string& site_config_relpath, std::string base_path,
    std::string revision, bool include_drafts,
    const std::map<std::string, std::string, std::less<>>& options) {
    site_plan plan = create_site_plan(
        source, version_string, config, site_yaml, site_config_relpath,
        std::move(base_path), std::move(revision), include_drafts);

    site_model model;
    model.schema = "bioseq.website.site_model";
    model.schema_revision = 1;
    model.parser_identity = plan.parser_identity;
    model.parser_revision = plan.parser_revision;
    model.binding_schema = plan.binding_schema;
    model.binding_schema_revision = plan.binding_schema_revision;
    model.version = plan.version;
    model.site_title = plan.site_title;
    model.source_revision = plan.source_revision;
    model.inputs = plan.inputs;
    model.plan_signature = plan.signature;

    if (const auto found = options.find("cpp-entities"); found != options.end()) {
        model.cpp_entities = deserialize_reference_entities(read_artifact_bytes(found->second));
    }
    if (const auto found = options.find("sql-entities"); found != options.end()) {
        model.sql_entities = deserialize_reference_entities(read_artifact_bytes(found->second));
    }
    if (const auto found = options.find("cpp-signature"); found != options.end()) {
        model.cpp_signature = found->second;
    }
    if (const auto found = options.find("staged-extension-sha256"); found != options.end()) {
        model.staged_extension_sha256 = found->second;
    }

    // Documentation follows every generated declaration and live extension member.
    // The public manifest projection remains the separate runnable-example ACL.
    if (!options.contains("sql-manifest")) {
        throw std::runtime_error("website SQL projection requires the generated --sql-manifest");
    }
    std::vector<loaded_sql_manifest_projection> manifests = load_sql_manifest_projections(options);
    // The configuration appendix also describes settings compiled only into
    // test builds. Import those declarations as documentation without adding
    // catalog schemas, executable routines, or grants to the runtime projection.
    if (const auto extra = options.find("sql-documentation-manifest"); extra != options.end()) {
        std::set<std::string, std::less<>> documented_ids;
        for (const auto& manifest : manifests)
            for (const auto& entity : manifest.projection.documentation_entities)
                documented_ids.insert(entity.stable_identity);
        auto text = read_artifact_bytes(extra->second);
        auto projection = parse_sql_manifest_public_projection(text, extra->second);
        std::erase_if(projection.documentation_entities, [&](const auto& entity) {
            return entity.kind != "guc" || documented_ids.contains(entity.stable_identity);
        });
        for (auto& entity : projection.documentation_entities) {
            entity.public_documentation = false;
            entity.attributes["Availability"] = "Test builds only";
        }
        projection.entities.clear();
        projection.catalog_schemas.clear();
        projection.documentation_schemas.clear();
        projection.restricted_sequence_names.clear();
        manifests.push_back(loaded_sql_manifest_projection{
            .path = extra->second, .text = std::move(text), .projection = std::move(projection),
            .documentation_only = true});
    }
    struct manifest_binding {
        const sql_manifest_public_entity* entity;
        const sql_manifest_public_projection* projection;
    };
    std::map<std::string, manifest_binding, std::less<>> documented_by_catalog_identity;
    const auto catalog_identity_key = [](std::string_view identity) {
        const auto open = identity.find('(');
        if (open == std::string_view::npos || !identity.ends_with(')')) return std::string(identity);
        std::string result(identity.substr(0,open + 1));
        const auto arguments = bioseq::codegen::sql::parse_routine_argument_declarations(
            identity.substr(open + 1, identity.size() - open - 2));
        for (const auto& argument : arguments) {
            const auto type = canonical_result_type_key(argument.sql_type);
            result += std::to_string(type.size()) + ':' + type;
        }
        result += ')';
        return result;
    };
    for (const loaded_sql_manifest_projection& manifest : manifests) {
        model.inputs.push_back(source_input{
            .path = "generated/" + manifest.projection.extension_name
                + (manifest.documentation_only ? "-additional-settings-manifest.json" : "-sql-manifest.json"),
            .content_hash = bioseq::common::sha256_hex(manifest.text),
        });
        for (const sql_manifest_public_entity& entity : manifest.projection.documentation_entities) {
            documented_by_catalog_identity.emplace(catalog_identity_key(entity.catalog_identity),
                manifest_binding{std::addressof(entity), std::addressof(manifest.projection)});
        }
    }
    std::set<std::string, std::less<>> matched;
    const auto apply_source_documentation = [](reference_entity& entity,
            const sql_manifest_public_entity& source,
            const sql_manifest_public_projection& projection) {
        if (!source.name.empty()) entity.display_name = source.name;
        if (!source.brief.empty()) entity.summary_text = source.brief;
        else if (!source.details.empty())
            entity.summary_text = source.details.substr(0, source.details.find("\n\n"));
        if (!source.details.empty()) entity.detail_content = {content_node{
            .kind = content_node_kind::paragraph,
            .children = {content_node{.kind = content_node_kind::text, .text = source.details}},
        }};
        entity.detail_content.insert(entity.detail_content.end(),
            source.additional_documentation.begin(), source.additional_documentation.end());
        if (entity.fields.empty() && entity.return_type
            && entity.return_type->starts_with("TABLE(") && entity.return_type->ends_with(')')) {
            const auto columns = bioseq::codegen::sql::parse_routine_argument_declarations(
                std::string_view(*entity.return_type).substr(6, entity.return_type->size() - 7));
            for (const auto& column : columns) entity.fields.push_back(parameter_description{
                .name = column.name, .type = column.sql_type});
        }
        // Preserve catalog argument types/defaults while attaching authored names and docs.
        if (entity.parameters.empty()) entity.parameters = source.parameters;
        else for (std::size_t index = 0; index < entity.parameters.size()
                 && index < source.parameters.size(); ++index) {
            entity.parameters[index].name = source.parameters[index].name;
            entity.parameters[index].description = source.parameters[index].description;
        }
        for (parameter_description& field : entity.fields) {
            const auto authored = std::ranges::find(source.fields, field.name,
                &parameter_description::name);
            if (authored != source.fields.end() && !authored->description.empty())
                field.description = authored->description;
        }
        entity.module_name = source.source_module;
        if (!source.source_path.empty() && source.source_line != 0) {
            source_span declaration;
            declaration.path = source.source_path;
            declaration.first_line = source.source_line;
            entity.declaration = std::move(declaration);
        }
        for (const auto& [key,value] : source.attributes) entity.attributes.try_emplace(key,value);
        entity.attributes["sql_kind"] = source.kind;
        entity.attributes["documentation_authority"] = "generated_sql_manifest";
        entity.attributes["sql_manifest_script_sha256"] = projection.script_sha256;
        if (entity.qualified_name.starts_with("bioseq_internal."))
            entity.attributes["visibility"] = "Internal";
        bind_public_manifest_identity(entity,source);
        entity.attributes.erase("catalog_comment");
    };
    for (reference_entity& entity : model.sql_entities) {
        const auto selected = documented_by_catalog_identity.find(catalog_identity_key(entity.canonical_id));
        if (selected == documented_by_catalog_identity.end()) {
            entity.attributes.erase("catalog_comment");
            continue;
        }
        const auto& source = *selected->second.entity;
        // Preserve strict source/catalog agreement for the previously public API.
        if (source.public_documentation) {
            const auto comment = entity.attributes.find("catalog_comment");
            if (comment == entity.attributes.end() || comment->second != source.brief)
                throw std::runtime_error("SQL manifest/catalog COMMENT mismatch for '" + source.installed_identity + "'");
            if (!source.result_sql_type.empty() && entity.return_type
                && canonical_result_type_key(*entity.return_type) != canonical_result_type_key(source.result_sql_type))
                throw std::runtime_error("SQL manifest/catalog result-type mismatch for '" + source.installed_identity + "'");
            const auto expected_kind = source.kind == "type" ? entity_kind::sql_type : entity_kind::sql_function;
            if (entity.kind != expected_kind || entity.qualified_name != source.qualified_name)
                throw std::runtime_error("SQL manifest/catalog identity-shape mismatch for '" + source.installed_identity + "'");
        }
        matched.insert(source.stable_identity);
        apply_source_documentation(entity,source,*selected->second.projection);
    }
    for (const loaded_sql_manifest_projection& manifest : manifests) {
        for (const sql_manifest_public_entity& source : manifest.projection.documentation_entities) {
            if (matched.contains(source.stable_identity)) continue;
            if (source.public_documentation)
                throw std::runtime_error("SQL manifest public entity '" + source.stable_identity
                    + "' is not a pg_depend member of extension " + manifest.projection.extension_name
                    + " captured from the live catalog");
            reference_entity entity;
            entity.domain = entity_domain::sql;
            entity.kind = source.kind == "type" ? entity_kind::sql_type
                : source.kind == "function" || source.kind == "sql_function" ? entity_kind::sql_function
                : source.kind == "schema" ? entity_kind::sql_schema : entity_kind::sql_object;
            entity.qualified_name = source.qualified_name;
            entity.display_name = source.name.empty() ? source.qualified_name : source.name;
            const auto prefix = source.catalog_identity.find(':',4);
            entity.normalized_signature = source.catalog_identity.substr(prefix + 1);
            entity.summary_text = "SQL " + source.kind + " " + entity.normalized_signature + ".";
            if (!source.result_sql_type.empty()) entity.return_type = source.result_sql_type;
            entity.fields = source.fields;
            entity.attributes["extension"] = manifest.projection.extension_name;
            entity.attributes["declaration"] = "Generated extension SQL";
            apply_source_documentation(entity,source,manifest.projection);
            model.sql_entities.push_back(std::move(entity));
        }
    }
    model.omitted_sql_entities.clear();

    // PostgreSQL creates a composite row type for each annotated relation and
    // indexes for its declared constraints. Their reference pages inherit the
    // source declaration and field meanings instead of becoming empty pages.
    std::map<std::string, const reference_entity*, std::less<>> relations;
    for (const auto& entity : model.sql_entities) {
        const auto kind = entity.attributes.find("sql_kind");
        if (kind != entity.attributes.end() && (kind->second == "table" || kind->second == "view"))
            relations.emplace(entity.qualified_name, &entity);
    }
    for (auto& entity : model.sql_entities) {
        if (entity.attributes.contains("documentation_authority")
            && entity.attributes.at("documentation_authority") == "generated_sql_manifest") continue;
        std::string owner_name;
        if (entity.kind == entity_kind::sql_type && relations.contains(entity.qualified_name))
            owner_name = entity.qualified_name;
        else if (const auto table = entity.attributes.find("table"); table != entity.attributes.end())
            owner_name = table->second;
        const auto owner = relations.find(owner_name);
        if (owner == relations.end()) continue;
        const auto& source = *owner->second;
        entity.declaration = source.declaration;
        entity.module_name = source.module_name;
        entity.attributes["documentation_authority"] = "generated_sql_manifest_relation";
        entity.attributes["documentation_owner"] = source.canonical_id;
        for (auto& field : entity.fields) {
            const auto found = std::ranges::find(source.fields, field.name, &parameter_description::name);
            if (found != source.fields.end()) field.description = found->description;
        }
        const bool row_type = entity.kind == entity_kind::sql_type;
        entity.summary_text = (row_type ? "Composite row type for " : "Index on ") + owner_name + ". " + source.summary_text;
        entity.detail_content.push_back(content_node{.kind = content_node_kind::paragraph,
            .children = {
                content_node{.kind = content_node_kind::text, .text = "Defined by "},
                content_node{.kind = content_node_kind::entity_link, .text = owner_name,
                    .target = source.canonical_id, .attributes = {{"entity_id", source.canonical_id}}},
                content_node{.kind = content_node_kind::text, .text = "."}}});
        entity.detail_content.push_back(reference_heading("Examples", "examples"));
        const auto row_example = relation_row_example(source);
        entity.detail_content.push_back(reference_paragraph(row_type
            ? row_example.first
            : "Inspect the index definition installed by the extension."));
        entity.detail_content.push_back(content_node{.kind = content_node_kind::code_block,
            .text = row_type ? row_example.second
                : "SELECT pg_get_indexdef('" + entity.qualified_name + "'::regclass);",
            .attributes = {{"language", "sql"}}});
    }

    // Group every overloaded SQL function under its shared qualified name.
    {
        std::map<std::string, std::vector<const reference_entity*>, std::less<>> by_name;
        for (const reference_entity& entity : model.sql_entities) {
            if (entity.kind == entity_kind::sql_function) {
                by_name[entity.qualified_name].push_back(&entity);
            }
        }
        std::vector<reference_entity> groups;
        for (const auto& [name, members] : by_name) {
            if (members.size() < 2) {
                continue;
            }
            reference_entity group;
            group.domain = entity_domain::sql;
            group.kind = entity_kind::overload_group;
            group.qualified_name = name;
            const std::size_t dot = name.rfind('.');
            group.display_name = dot == std::string::npos ? name : name.substr(dot + 1);
            const std::string stable_name =
                stable_sql_qualified_name(*members.front());
            for (const reference_entity* member : members) {
                if (stable_sql_qualified_name(*member) != stable_name) {
                    throw std::runtime_error(
                        "SQL overload group '" + name
                        + "' has inconsistent stable member identities");
                }
            }
            group.canonical_id =
                "sql:overload_group:" + stable_name;
            group.attributes["stable_identity"] = group.canonical_id;
            group.attributes["installed_identity"] =
                "sql:overload_group:" + name;
            group.summary_text = members.front()->summary_text.empty()
                ? "Overloaded SQL function with " + std::to_string(members.size()) + " signatures."
                : members.front()->summary_text;
            // The first overload's source example is an actual invocation of
            // this function family; keep it visible on its search landing page.
            group.detail_content = members.front()->detail_content;
            for (const reference_entity* member : members) {
                group.overload_ids.push_back(member->canonical_id);
            }
            groups.push_back(std::move(group));
        }
        model.sql_entities.insert(model.sql_entities.end(),
                                  std::make_move_iterator(groups.begin()),
                                  std::make_move_iterator(groups.end()));
    }

    const execution_target target = resolve_execution_target(options);
    if (target.have_connection && !target.probe_conninfo.empty()) {
        postgres_tutorial_executor probe(target.probe_conninfo);
        const auto catalog_started = std::chrono::steady_clock::now();
        const std::expected<sql_catalog_snapshot, diagnostic> snapshot = probe.read_catalog();
        example_stats().catalog_ms = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - catalog_started).count());
        if (snapshot) {
            model.postgres_server_version = snapshot->server_version;
            model.extension_version = snapshot->extension_version;
            if (model.staged_extension_sha256.empty()) {
                model.staged_extension_sha256 = snapshot->staged_artifact_sha256;
            }
        }
    }

    model.landing_examples = collect_landing_views(source, version_string, options);

    // Resolve tutorial references now so broken links fail before rendering.
    const entity_reference_index reference_index =
        build_entity_reference_index(model.cpp_entities, model.sql_entities);

    const auto execute_document = [&](tutorial& item) {
        apply_entity_shorthand(item.content, true);
        resolve_entity_links(item.content, reference_index, item.source_path);
        std::map<std::string, sql_example_result, std::less<>> results;
        if (target.have_connection) {
            for (const sql_example& example : item.sql_examples) {
                const auto started = std::chrono::steady_clock::now();
                std::expected<sql_example_result, diagnostic> outcome =
                    run_one_example(target, example);
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                if (!outcome) {
                    throw std::runtime_error("tutorial '" + item.metadata.slug
                        + "' example '" + example.local_id + "': ["
                        + outcome.error().code + "] " + outcome.error().message);
                }
                record_example_run(item.metadata.slug + ":" + example.local_id, elapsed,
                                   *outcome);
                results.emplace(example.local_id, std::move(*outcome));
            }
        }
        inject_tutorial_results(item.content, results, model.extension_version,
                                model.postgres_server_version);
        // Remove setup and teardown SQL before saving the model. Pages need only
        // the visible SQL and its captured results.
        for (sql_example& example : item.sql_examples) {
            example.setup_sql.clear();
            example.teardown_sql.clear();
        }
    };
    for (auto& item : plan.tutorials) execute_document(item);
    for (auto& item : plan.blog_posts) execute_document(item);
    model.tutorials = std::move(plan.tutorials);
    model.blog_posts = std::move(plan.blog_posts);

    // Count documentation for public C++ and SQL entities. Exclude generated
    // SQL overload-group pages from the totals.
    std::vector<reference_entity> coverage_entities = model.cpp_entities;
    for (const reference_entity& entity : model.sql_entities) {
        if (entity.kind != entity_kind::overload_group) {
            coverage_entities.push_back(entity);
        }
    }
    // Require summaries for public entities linked from tutorials.
    std::set<std::string> referenced;
    for (const tutorial& item : model.tutorials) {
        collect_referenced_entity_ids(item.content, referenced);
    }
    for (const tutorial& item : model.blog_posts)
        collect_referenced_entity_ids(item.content, referenced);
    const std::vector<std::string> referenced_ids(referenced.begin(), referenced.end());
    const reference_coverage_report coverage = evaluate_reference_coverage(
        coverage_entities, config.required_documentation_entities, referenced_ids);
    model.coverage_public_total = static_cast<std::uint32_t>(coverage.public_total);
    model.coverage_documented_total =
        static_cast<std::uint32_t>(coverage.documented_total);
    model.coverage_missing = coverage.missing_summary;
    model.coverage_missing_parameters = coverage.missing_parameters;
    model.coverage_missing_returns = coverage.missing_returns;
    model.coverage_fatal_missing = coverage.fatal_missing;
    for (const reference_coverage_group& group : coverage.groups) {
        model.coverage_groups.push_back(coverage_group_view{
            group.group, static_cast<std::uint32_t>(group.public_total),
            static_cast<std::uint32_t>(group.documented_total)});
    }
    // Fail when a required or  tutorial-linked public entity has no summary.
    // This also applies the SQL coverage checks after catalog validation.
    if (!coverage.fatal_missing.empty()) {
        std::string joined;
        for (const std::string& id : coverage.fatal_missing) {
            joined += (joined.empty() ? "" : ", ") + id;
        }
        throw std::runtime_error(
            "required/referenced documentation missing for: " + joined);
    }
    return model;
}

}  // namespace

/// @brief Checks reference links and page data with small examples.
///
/// Name-only SQL links must open the overload group. Exact IDs must still
/// select individual overloads, including when the extension schema changes.
/// Also checks EXPLAIN filterin and annotated code rendering.
void verify_reference_pipeline() {
    reference_entity sequence_relation;
    sequence_relation.qualified_name = "bioseq_external.sequences";
    sequence_relation.attributes["access_method"] = "\"bioseq_external\"";
    const auto external_row_example = relation_row_example(sequence_relation);
    expect(external_row_example.second
               == "SELECT row_value FROM bioseq_external.sequences AS row_value WHERE false;"
               && external_row_example.first.contains("without contacting")
               && external_row_example.first.contains("returns no rows"),
           "external sequence row examples do not start an accession-free provider scan");
    sequence_relation.qualified_name = "other_schema.sequences";
    sequence_relation.attributes["access_method"] = "bioseq_external";
    expect(relation_row_example(sequence_relation).second.ends_with(" WHERE false;"),
           "external row examples follow the access method independently of schema spelling");
    sequence_relation.attributes["access_method"] = "\"heap\"";
    expect(relation_row_example(sequence_relation).second
               == "SELECT row_value FROM other_schema.sequences AS row_value LIMIT 5;",
           "ordinary stored row types retain their readable sample query");
    auto explain_probe = boost::json::parse(R"JSON([
        {"Plan":{"Bioseq Batch ABI Version":1,"Plans":[
            {"Fully Attested Alignment Jobs":2,"Scalar Fallbacks":3,
             "Actual Rows":4,"Executed Architecture":"AVX2"}]}}
    ])JSON");
    expect(omit_private_explain_fields(explain_probe) == 2,
           "public EXPLAIN removes only closed implementation fields recursively");
    const auto excerpt = boost::json::serialize(explain_probe);
    expect(excerpt.contains("\"Scalar Fallbacks\":3")
               && excerpt.contains("\"Actual Rows\":4")
               && excerpt.contains("\"Executed Architecture\":\"AVX2\"")
               && !excerpt.contains("Fully Attested Alignment Jobs"),
           "public EXPLAIN retains fallback, row-count and execution evidence");
    expect(is_private_explain_line("  Bioseq CPU Prepared Target: true")
               && !is_private_explain_line("  Scalar Fallbacks: 3")
               && !is_private_explain_line("Output: Bioseq CPU Prepared Target"),
           "text EXPLAIN filtering matches complete field names only");
    const sql_manifest_public_entity public_manifest_a{
        .stable_identity =
            "sql:function:$extension.mmsearch(bioseq.seq,bioseq.seq)",
        .installed_identity =
            "sql:function:public.mmsearch(bioseq.seq,bioseq.seq)",
    };
    reference_entity overload_a;
    overload_a.domain = entity_domain::sql;
    overload_a.kind = entity_kind::sql_function;
    overload_a.qualified_name = "public.mmsearch";
    overload_a.display_name = "mmsearch";
    overload_a.canonical_id = public_manifest_a.installed_identity;
    bind_public_manifest_identity(overload_a, public_manifest_a);

    const sql_manifest_public_entity public_manifest_b{
        .stable_identity =
            "sql:function:$extension.mmsearch("
            "bioseq.seq,bioseq.seq,$extension.bioseq_aligner_options)",
        .installed_identity =
            "sql:function:public.mmsearch("
            "bioseq.seq,bioseq.seq,public.bioseq_aligner_options)",
    };
    reference_entity overload_b = overload_a;
    bind_public_manifest_identity(overload_b, public_manifest_b);

    reference_entity mmsearch_group;
    mmsearch_group.domain = entity_domain::sql;
    mmsearch_group.kind = entity_kind::overload_group;
    mmsearch_group.qualified_name = "public.mmsearch";
    mmsearch_group.display_name = "mmsearch";
    mmsearch_group.canonical_id =
        "sql:overload_group:$extension.mmsearch";
    mmsearch_group.attributes["stable_identity"] =
        mmsearch_group.canonical_id;
    mmsearch_group.attributes["installed_identity"] =
        "sql:overload_group:public.mmsearch";

    const entity_reference_index grouped_index = build_entity_reference_index(
        {}, {overload_a, overload_b, mmsearch_group});
    const auto lookup = [&grouped_index](
                            entity_query_domain domain,
                            std::string_view spelling) -> std::string {
        const auto found = grouped_index.by_key.find(
            entity_reference_index::domain_key(domain) + std::string(spelling));
        return found == grouped_index.by_key.end() ? std::string() : found->second;
    };
    expect(lookup(entity_query_domain::sql, "public.mmsearch")
               == "sql:overload_group:$extension.mmsearch"
           && lookup(entity_query_domain::sql, "mmsearch")
               == "sql:overload_group:$extension.mmsearch"
           && lookup(entity_query_domain::canonical_id,
                     "sql:function:public.mmsearch(bioseq.seq,bioseq.seq)")
               == public_manifest_a.stable_identity
           && lookup(entity_query_domain::canonical_id,
                     public_manifest_a.stable_identity)
               == public_manifest_a.stable_identity,
           "installed aliases resolve to stable overload/member identities");

    // #mmsearch and #bioseq.mmsearch must resolve to the same overload group.
    expect(lookup(entity_query_domain::any, "mmsearch")
               == "sql:overload_group:$extension.mmsearch"
           && lookup(entity_query_domain::any, "bioseq.mmsearch")
               == "sql:overload_group:$extension.mmsearch",
           "#mmsearch shorthand and the bioseq.mmsearch alias resolve to the group");

    const sql_manifest_public_entity relocated_manifest_a{
        .stable_identity = public_manifest_a.stable_identity,
        .installed_identity =
            "sql:function:unusual_schema.mmsearch(bioseq.seq,bioseq.seq)",
    };
    reference_entity relocated_a = overload_a;
    relocated_a.qualified_name = "unusual_schema.mmsearch";
    bind_public_manifest_identity(relocated_a, relocated_manifest_a);
    reference_entity relocated_group = mmsearch_group;
    relocated_group.qualified_name = "unusual_schema.mmsearch";
    relocated_group.attributes["installed_identity"] =
        "sql:overload_group:unusual_schema.mmsearch";
    const entity_reference_index relocated_index =
        build_entity_reference_index({}, {relocated_a, relocated_group});
    const auto relocated_lookup = [&relocated_index](
                                      entity_query_domain domain,
                                      std::string_view spelling) -> std::string {
            const auto found = relocated_index.by_key.find(
                entity_reference_index::domain_key(domain)
                + std::string(spelling));
            return found == relocated_index.by_key.end()
                ? std::string()
                : found->second;
        };
    expect(relocated_lookup(
               entity_query_domain::canonical_id,
               relocated_manifest_a.installed_identity)
               == public_manifest_a.stable_identity,
           "unusual-schema installed identity aliases to the stable member");

    overload_a.route_path =
        "/" + reference_entity_relative_route("v0.1", overload_a);
    relocated_a.route_path =
        "/" + reference_entity_relative_route("v0.1", relocated_a);
    const search_index_entry public_search =
        build_entity_search_entry(overload_a);
    const search_index_entry relocated_search =
        build_entity_search_entry(relocated_a);
    expect(overload_a.canonical_id == relocated_a.canonical_id
               && overload_a.route_path == relocated_a.route_path
               && public_search.id == relocated_search.id
               && public_search.route == relocated_search.route,
           "public and unusual schemas keep identical website IDs, routes, and search keys");

    std::vector<content_node> shorthand_split;
    split_shorthand_text("see #bioseq.mmsearch and ##notshorthand", shorthand_split);
    int shorthand_links = 0;
    std::string shorthand_target;
    for (const content_node& node : shorthand_split) {
        if (node.kind == content_node_kind::entity_link) {
            ++shorthand_links;
            shorthand_target = node.target;
        }
    }
    expect(shorthand_links == 1 && shorthand_target == "bioseq.mmsearch",
           "shorthand splitter extracts a single #name entity reference");

    reference_entity group_view = mmsearch_group;
    group_view.overload_members = {
        {"public.mmsearch(bioseq.seq,bioseq.seq)", "/v0.1/reference/sql/function/a/"},
        {"public.mmsearch(bioseq.seq,bioseq.seq,public.bioseq_aligner_options)",
         "/v0.1/reference/sql/function/b/"}};
    const std::string group_json = serialize_reference_entity(group_view);
    expect(group_json.find("\"overloads\"") != std::string::npos
               && group_json.find("bioseq_aligner_options") != std::string::npos,
           "overload-group page view serializes member signatures");

    // Container page data must include children and the  implementation link.
    reference_entity container;
    container.domain = entity_domain::cpp;
    container.kind = entity_kind::struct_;
    container.qualified_name = "bioseq::io::seq";
    container.child_members = {{"read", "/v0.1/reference/cpp/function/read-abc/"},
                               {"size", "/v0.1/reference/cpp/function/size-def/"}};
    container.implemented_by_route = "/v0.1/reference/cpp/function/impl-ghi/";
    const std::string container_json = serialize_reference_entity(container);
    expect(container_json.find("\"members\"") != std::string::npos
               && container_json.find("read-abc") != std::string::npos
               && container_json.find("\"implemented_by\"") != std::string::npos,
           "container page view serializes child index and implemented-by link");

    // Quoted fence attributes keep spaces, commas, and parentheses. Reject
    // repeated scalar attributes and unknown names.
    const fence_descriptor ordinary_fence = parse_fence_info("sql id=demo annotated");
    expect(ordinary_fence.kind == fence_kind::ordinary && ordinary_fence.id == "demo"
               && ordinary_fence.annotated,
           "ordinary fence parses id + annotated");
    const fence_descriptor directive_fence = parse_fence_info(
        "annotation target=demo match=\"bioseq.seq('dna4', 64)\" title=\"The type\"");
    expect(directive_fence.kind == fence_kind::annotation
               && directive_fence.target == "demo"
               && directive_fence.match == "bioseq.seq('dna4', 64)"
               && directive_fence.title == "The type",
           "annotation directive keeps a spaced/parenthesized match verbatim");
    const fence_descriptor entity_fence =
        parse_fence_info("cpp entity=\"cpp:function:bioseq::io::seq::read(int, char)\"");
    expect(entity_fence.entities.size() == 1
               && entity_fence.entities.front().find(',') != std::string::npos,
           "entity= carries a signature containing a comma as one value");
    const auto is_fatal = [](std::string_view info) {
        try {
            static_cast<void>(parse_fence_info(info));
        } catch (const std::exception&) {
            return true;
        }
        return false;
    };
    expect(is_fatal("sql id=a id=b"), "duplicate scalar attribute is fatal");
    expect(is_fatal("sql bogus=1"), "unknown attribute is fatal");
    expect(is_fatal("text runnable"), "runnable is only valid on sql");

    // Attach the annotation to its earlier target, preserving the matched
    // source range and removing the directive block.
    std::vector<content_node> annotated_doc;
    content_node display_block;
    display_block.kind = content_node_kind::code_block;
    display_block.attributes["language"] = "sql";
    display_block.attributes["info"] = "sql id=demo annotated";
    display_block.text = "CREATE TABLE t (s bioseq.seq('dna4', 64));";
    annotated_doc.push_back(std::move(display_block));
    content_node directive_block;
    directive_block.kind = content_node_kind::code_block;
    directive_block.attributes["language"] = "annotation";
    directive_block.attributes["info"] =
        "annotation target=demo match=\"bioseq.seq('dna4', 64)\" title=\"The type\"";
    directive_block.text = "The alphabet and length live in the type modifier.";
    annotated_doc.push_back(std::move(directive_block));
    process_annotations(annotated_doc, "self-test");
    expect(annotated_doc.size() == 1 && !annotated_doc[0].children.empty()
               && annotated_doc[0].children[0].kind == content_node_kind::code_annotation,
           "annotation directive folds onto its target and the directive node is removed");
    const content_node& folded = annotated_doc[0].children[0];
    const std::size_t folded_start = std::stoul(folded.attributes.at("start"));
    const std::size_t folded_len = std::stoul(folded.attributes.at("len"));
    expect(folded.attributes.at("number") == "1"
               && folded.attributes.at("title") == "The type"
               && annotated_doc[0].text.substr(folded_start, folded_len)
                      == "bioseq.seq('dna4', 64)",
           "folded annotation carries number/title and a range over the matched source");
    const std::string annotated_html = render_content_html(annotated_doc);
    expect(annotated_html.find("data-annotated-type") != std::string::npos
               && annotated_html.find("data-code") != std::string::npos
               && annotated_html.find("bioseq.seq") != std::string::npos
               && annotated_html.find("CREATE TABLE") == std::string::npos
               && annotated_html.find("data-anno=\"1\"") != std::string::npos
               && annotated_html.find("data-anno-card=\"1\"") != std::string::npos,
           "annotated block renders the focused type-anatomy component and paired card");
    std::vector<content_node> annotated_with_result = annotated_doc;
    content_node annotated_result;
    annotated_result.kind = content_node_kind::table;
    annotated_result.attributes["class"] = "result-table";
    content_node annotated_result_header;
    annotated_result_header.kind = content_node_kind::table_row;
    annotated_result.children.push_back(std::move(annotated_result_header));
    annotated_with_result.push_back(std::move(annotated_result));
    const std::string annotated_result_html =
        render_content_html(annotated_with_result);
    expect(annotated_result_html.find("data-annotated-type") != std::string::npos
               && annotated_result_html.find("class=\"result-table\"")
                      != std::string::npos,
           "annotated runnable preserves its executed result after the anatomy card");

    content_node terminal_code;
    terminal_code.kind = content_node_kind::code_block;
    terminal_code.text = "SELECT 1 AS value;";
    terminal_code.attributes["language"] = "sql";
    terminal_code.attributes["runnable_id"] = "self_test_example";
    expect(render_content_html({terminal_code}).contains(
               "data-sql-example=\"self_test_example\""),
           "runnable identity survives HTML rendering");
    content_node terminal_table;
    terminal_table.kind = content_node_kind::table;
    terminal_table.attributes["class"] = "result-table";
    for (const auto& [header, text] : {
             std::pair{true, std::string("value")},
             std::pair{false, std::string("1")}}) {
        content_node row;
        row.kind = content_node_kind::table_row;
        content_node cell;
        cell.kind = content_node_kind::table_cell;
        if (header) {
            cell.attributes["scope"] = "col";
        }
        cell.children.push_back(content_node{
            .kind = content_node_kind::text,
            .text = text});
        row.children.push_back(std::move(cell));
        terminal_table.children.push_back(std::move(row));
    }
    const std::string terminal_html =
        render_content_html({terminal_code, terminal_table});
    expect(terminal_html.find("data-term") != std::string::npos
               && terminal_html.find("data-result") != std::string::npos
               && terminal_html.find("Result \xC2\xB7 1 row") != std::string::npos
               && terminal_html.find("<thead>") != std::string::npos
               && terminal_html.find("<tbody>") != std::string::npos,
           "ordinary runnable nests a semantic executed-result table in its terminal");
    expect(render_content_html({make_text_node("{\"plan\":{}}")})
               == "&#123;\"plan\":&#123;&#125;&#125;",
           "literal JSON braces remain visible without resembling template delimiters");

    // Blank lines let normal Markdown fences live inside authored comparison
    // panels. Their setup, results and SQL links must use the usual pipeline.
    const std::string comparison_source =
        "<div class=\"tool-comparison\" role=\"group\" aria-label=\"Count sequences\">\n"
        "<div class=\"tool-comparison-panel\">\n"
        "<p class=\"tool-comparison-label\">BioseqDB</p>\n\n"
        "```sql runnable id=comparison_probe\n"
        "-- bioseq:setup\nSELECT 'private_fixture';\n"
        "-- bioseq:visible\nSELECT 'ACGT'::bioseq.seq;\n"
        "-- bioseq:teardown\nSELECT 'private_cleanup';\n```\n\n"
        "</div>\n<div class=\"tool-comparison-panel\">\n"
        "<p class=\"tool-comparison-label\">SeqKit</p>\n\n"
        "```sh\nseqkit seq sequences.fa\n```\n\n</div>\n</div>\n";
    auto comparison = parse_markdown_content(comparison_source);
    std::vector<sql_example> comparison_examples;
    std::vector<unresolved_entity_query> comparison_queries;
    collect_examples_and_queries(comparison, "comparison", "v0.1",
                                comparison_examples, comparison_queries);
    expect(comparison_examples.size() == 1
               && comparison_examples.front().visible_sql == "SELECT 'ACGT'::bioseq.seq;",
           "comparison panels retain runnable SQL collection");
    strip_runnable_fence_hidden_sql(comparison.children);
    sql_example_result comparison_result;
    comparison_result.example_id = "comparison_probe";
    comparison_result.visible_results.push_back(sql_result_set{
        .shape = result_shape::rows,
        .columns = {{.name = "seq", .type_name = "bioseq.seq"}},
        .rows = {{{.text = "ACGT"}}}});
    inject_tutorial_results(comparison.children,
                            {{"comparison_probe", comparison_result}}, "0.1", "17");
    decorate_code_entity_ranges(comparison.children,
        {{"sql:type:bioseq.seq", "/sequence/"}},
        {{"sql:type:bioseq.seq", "bioseq.seq"}}, "/entities.json", "comparison");
    const std::string comparison_html = render_content_html(comparison.children);
    const auto left = comparison_html.find(">BioseqDB</p>");
    const auto result = comparison_html.find("class=\"result-table\"");
    const auto right = comparison_html.find(">SeqKit</p>");
    expect(left < result && result < right && right != std::string::npos
               && comparison_html.contains("data-sql-example=\"comparison_probe\"")
               && comparison_html.contains("sql:type:bioseq.seq")
               && !comparison_html.contains("private_fixture")
               && !comparison_html.contains("private_cleanup"),
           "comparison SQL results and entity links stay in the first panel");
    const auto comparison_markdown = blog_public_markdown(comparison_source);
    expect(comparison_markdown.contains("class=\"tool-comparison\"")
               && comparison_markdown.contains("```sql\nSELECT 'ACGT'::bioseq.seq;")
               && comparison_markdown.contains("seqkit seq sequences.fa")
               && !comparison_markdown.contains("private_fixture")
               && !comparison_markdown.contains("private_cleanup"),
           "comparison Markdown preserves both tools while hiding SQL fixtures");
}

/// @brief Runs website plan to parse tutorials and save the build plan.
///
/// Validates site configuration, and tutorial metadata, collects runnable SQL,
/// and assigns tutorial order and navigation before writing the private plan.
/// @param args Options after plan
/// @return 0 on  success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_plan_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({
        "source", "version", "site-config", "templates", "static",
        "base-path", "source-revision", "output", "report-dir"});
    static constexpr auto flag_keys = std::to_array<std::string_view>({"include-drafts"});
    const auto options = parse_options(args, value_keys, flag_keys);
    const auto stage_started = std::chrono::steady_clock::now();
    const fs::path source = require_option(options, "source");
    const std::string version_string = require_option(options, "version");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path output = require_option(options, "output");
    const bool include_drafts = options.contains("include-drafts");

    const std::string site_yaml = read_artifact_bytes(site_config_path);
    const site_configuration config =
        parse_site_configuration(site_yaml, site_config_path.generic_string());
    std::string base_path = config.default_base_path;
    if (const auto found = options.find("base-path"); found != options.end()) {
        base_path = found->second;
    }
    std::string revision;
    if (const auto found = options.find("source-revision"); found != options.end()) {
        revision = found->second;
    }

    const site_plan plan = create_site_plan(
        source, version_string, config, site_yaml,
        content_relative_path(site_config_path, source),
        std::move(base_path), std::move(revision), include_drafts);
    const std::string serialized = serialize_site_plan(plan);
    write_private_artifact(output, serialized);
    std::uint64_t example_count = 0;
    for (const tutorial& item : plan.tutorials) {
        example_count += item.sql_examples.size();
    }
    for (const tutorial& item : plan.blog_posts) example_count += item.sql_examples.size();
    const auto stage_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stage_started).count();
    write_stage_report(options, "plan",
        "{\"stage\":\"plan\",\"wall_ms\":" + std::to_string(stage_ms)
            + ",\"tutorial_count\":" + std::to_string(plan.tutorials.size())
            + ",\"example_count\":" + std::to_string(example_count) + "}\n");
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website plan: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website plan: {}", error.what());
    return 3;
}

namespace {

[[nodiscard]] std::vector<std::string> read_lines(const fs::path& path) {
    const std::string content = read_artifact_bytes(path);
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= content.size()) {
        const std::size_t end = content.find('\n', begin);
        const std::size_t stop = end == std::string::npos ? content.size() : end;
        std::string line = content.substr(begin, stop - begin);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!trim_ascii_whitespace(line).empty()) {
            lines.push_back(std::move(line));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return lines;
}

[[nodiscard]] bool is_page_compound(std::string_view kind) {
    return kind == "namespace" || kind == "class" || kind == "struct"
        || kind == "union" || kind == "concept";
}

}  // namespace

/// @brief Runs website cpp_index to combine C++ declarations with Doxygen text.
///
/// Scans the supplied source units with Clang and the compile database. Writes
/// the entity index and coverage report, then fails if required documentation
/// is missing.
/// @param args Options after cpp_index.
/// @return 0 on success, 2 for invalid command-line usrge, or 3 for other failures.
[[nodiscard]] int execute_cpp_index_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({
        "doxygen-xml", "compile-db", "reachable-units", "source-root", "site-config",
        "output", "coverage-output", "report-dir"});
    const auto options = parse_options(args, value_keys, {});
    const fs::path doxygen_xml = require_option(options, "doxygen-xml");
    const fs::path compile_db = require_option(options, "compile-db");
    const fs::path reachable_units = require_option(options, "reachable-units");
    const fs::path source_root = require_option(options, "source-root");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path output = require_option(options, "output");
    const fs::path coverage_output = require_option(options, "coverage-output");

    const site_configuration config =
        parse_site_configuration(read_artifact_bytes(site_config_path), site_config_path.generic_string());

    // Measure Doxygen parsing time, input bytes, and included compounds.
    const auto doxygen_started = std::chrono::steady_clock::now();
    std::uint64_t doxygen_xml_bytes = 0;
    std::error_code walk_error;
    for (const auto& entry : fs::recursive_directory_iterator(doxygen_xml, walk_error)) {
        if (entry.is_regular_file()) {
            doxygen_xml_bytes += entry.file_size();
        }
    }
    std::uint64_t doxygen_compound_count = 0;
    std::vector<reference_entity> documented;
    const std::string index_xml = read_artifact_bytes(doxygen_xml / "index.xml");
    for (const doxygen_compound_ref& compound : parse_doxygen_compound_index(index_xml)) {
        if (!is_page_compound(compound.kind)) {
            continue;
        }
        const fs::path compound_path = doxygen_xml / (compound.refid + ".xml");
        std::error_code error;
        if (!fs::exists(compound_path, error)) {
            continue;
        }
        ++doxygen_compound_count;
        for (reference_entity& entity :
             parse_doxygen_entities(read_artifact_bytes(compound_path))) {
            documented.push_back(std::move(entity));
        }
    }
    const auto doxygen_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - doxygen_started).count();

    const auto clang_started = std::chrono::steady_clock::now();
    std::vector<fs::path> units;
    for (const std::string& line : read_lines(reachable_units)) {
        const fs::path input{line};
        // Resolve relative source paths against the configured source root.
        // Clang needs these  absolute paths to find their compile commands.
        units.push_back(fs::absolute(input.is_absolute()
            ? input : source_root / input).lexically_normal());
    }
    const cpp_reference_index structural =
        scan_cpp_reference_units(units, compile_db);
    const auto clang_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - clang_started).count();

    std::vector<reference_entity> merged =
        merge_cpp_reference_entities(structural, documented);
    for (reference_entity& entity : merged) {
        if (entity.declaration) {
            entity.declaration->path = canonical_source_path(
                entity.declaration->path, source_root);
        }
    }
    const reference_coverage_report coverage = evaluate_reference_coverage(
        merged, config.required_documentation_entities, {});

    write_stage_report(options, "cpp_index",
        "{\"stage\":\"cpp_index\",\"doxygen_wall_ms\":" + std::to_string(doxygen_ms)
            + ",\"doxygen_xml_bytes\":" + std::to_string(doxygen_xml_bytes)
            + ",\"doxygen_compound_count\":" + std::to_string(doxygen_compound_count)
            + ",\"clang_wall_ms\":" + std::to_string(clang_ms)
            + ",\"entity_count\":" + std::to_string(merged.size()) + "}\n");

    publish_artifact_if_changed(output, serialize_cpp_reference_entities(merged, structural.clang_release));
    std::vector<coverage_group_view> coverage_groups;
    for (const reference_coverage_group& group : coverage.groups) {
        coverage_groups.push_back(coverage_group_view{
            group.group, static_cast<std::uint32_t>(group.public_total),
            static_cast<std::uint32_t>(group.documented_total)});
    }
    publish_artifact_if_changed(coverage_output,
                       serialize_coverage(coverage.public_total, coverage.documented_total,
                                          coverage.missing_summary, coverage.missing_parameters,
                                          coverage.missing_returns, coverage.fatal_missing,
                                          coverage_groups));
    if (!coverage.fatal_missing.empty()) {
        std::string joined;
        for (const std::string& id : coverage.fatal_missing) {
            joined += (joined.empty() ? "" : ", ") + id;
        }
        throw std::runtime_error("required documentation missing for: " + joined);
    }
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website cpp_index: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website cpp_index: {}", error.what());
    return 3;
}

/// @brief Runs website execute-sql for one SQL example.
///
/// Runs setup and visible SQL, attempts teardown, and prints the captured
/// results. It can use a direct connection or a separate database and role.
/// @param args Options after execute-sql.
/// @return 0 on success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_sql_example_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({
        "conninfo", "setup", "visible", "teardown", "expect-sqlstate",
        "admin-conninfo", "socket-dir", "port", "template-db", "timeout-ms",
        "sql-manifest"});
    static constexpr auto flag_keys = std::to_array<std::string_view>({
        "isolated", "allow-plain-template"});
    static constexpr auto repeatable_value_keys = std::to_array<std::string_view>({
        "sql-manifest"});
    const auto options = parse_options(
        args, value_keys, flag_keys, repeatable_value_keys);

    sql_example example;
    example.canonical_id = "adhoc";
    example.local_id = "adhoc";
    if (const auto found = options.find("timeout-ms"); found != options.end()) {
        example.timeout = std::chrono::milliseconds(std::stoll(found->second));
    }
    if (const auto found = options.find("setup"); found != options.end()) {
        example.setup_sql = found->second;
    }
    example.visible_sql = require_option(options, "visible");
    if (const auto found = options.find("teardown"); found != options.end()) {
        example.teardown_sql = found->second;
    }
    if (const auto found = options.find("expect-sqlstate"); found != options.end()) {
        example.expectation = sql_expectation::error;
        example.expected_sqlstate = found->second;
    }

    // --isolated gives the example its own database and restricted role.
    // Direct execution uses the supplied connection string.
    std::expected<sql_example_result, diagnostic> outcome =
        std::unexpected(diagnostic{});
    if (options.contains("isolated")) {
        postgres_isolation_settings settings;
        settings.administrator_connection_info =
            require_option(options, "admin-conninfo");
        settings.socket_directory = require_option(options, "socket-dir");
        settings.server_port = std::stoi(require_option(options, "port"));
        settings.template_database = require_option(options, "template-db");
        settings.requires_extension = !options.contains("allow-plain-template");
        configure_manifest_acl_targets(options, settings);
        postgres_tutorial_executor executor("");
        outcome = executor.execute_in_isolated_database(example, settings);
    } else {
        postgres_tutorial_executor executor(require_option(options, "conninfo"));
        outcome = executor.execute(example);
    }
    if (!outcome) {
        std::println(stderr, "website execute-sql: [{}] {}",
                     outcome.error().code, outcome.error().message);
        return 3;
    }
    for (const sql_result_set& result : outcome->visible_results) {
        if (result.shape == result_shape::error) {
            std::println("ERROR SQLSTATE={}: {}",
                         result.sqlstate.value_or(""), result.primary_message);
        } else if (result.shape == result_shape::rows) {
            std::string header;
            for (const sql_column& column : result.columns) {
                header += (header.empty() ? "" : " | ") + column.name
                    + " [" + column.type_name + "]";
            }
            std::println("{}", header);
            for (const std::vector<sql_cell>& row : result.rows) {
                std::string line;
                for (const sql_cell& cell : row) {
                    line += (line.empty() ? "" : " | ")
                        + (cell.is_null ? std::string("NULL") : cell.text);
                }
                std::println("{}", line);
            }
            std::println("({} rows)", result.rows.size());
        } else {
            if (result.affected_rows) {
                std::println("{} ({} rows affected)", result.command_tag,
                             *result.affected_rows);
            } else {
                std::println("{}", result.command_tag);
            }
        }
    }
    for (const sql_notice& notice : outcome->notices) {
        std::println(stderr, "NOTICE {}: {}", notice.severity, notice.message);
    }
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website execute-sql: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website execute-sql: {}", error.what());
    return 3;
}

/// @brief Runs website catalog to save extension types and functions.
///
/// Generated manifests choose the extensions and schemas to inspect. Capture
/// live identities, properties, and comments for later checks against those
/// manifests, which  determine the public documentation.
/// @param args Options after catalog.
/// @return 0 on success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_catalog_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({
        "conninfo", "sql-manifest", "output"});
    static constexpr auto repeatable_value_keys = std::to_array<std::string_view>({
        "sql-manifest"});
    const auto options = parse_options(
        args, value_keys, {}, repeatable_value_keys);
    const std::string conninfo = require_option(options, "conninfo");
    const fs::path output = require_option(options, "output");
    const std::vector<loaded_sql_manifest_projection> manifests =
        load_sql_manifest_projections(options);
    if (std::ranges::none_of(
            manifests,
            [](const loaded_sql_manifest_projection& manifest) {
                return !manifest.projection.documentation_schemas.empty();
            })) {
        throw std::runtime_error(
            "SQL manifest documentation projections have no installed catalog schemas");
    }

    std::vector<reference_entity> entities;
    for (const loaded_sql_manifest_projection& manifest : manifests) {
        const sql_manifest_public_projection& projection =
            manifest.projection;
        for (reference_entity& entity : load_extension_type_entities(
                 conninfo, projection.extension_name,
                 projection.documentation_schemas)) {
            entities.push_back(std::move(entity));
        }
        for (reference_entity& entity : load_extension_function_entities(
                 conninfo, projection.extension_name,
                 projection.documentation_schemas)) {
            entities.push_back(std::move(entity));
        }
        for (reference_entity& entity : load_extension_relation_entities(
                 conninfo, projection.extension_name,
                 projection.documentation_schemas)) {
            entities.push_back(std::move(entity));
        }
    }
    std::ranges::sort(entities, [](const reference_entity& lhs,
                                   const reference_entity& rhs) {
        return lhs.canonical_id < rhs.canonical_id;
    });
    publish_artifact_if_changed(output, serialize_sql_entities(entities));
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website catalog: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website catalog: {}", error.what());
    return 3;
}

/// @brief Runs website render-fragment for one template and JSON context.
/// @param args Options after render-fragment
/// @return 0 on success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_template_render_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({"template", "context", "output"});
    const auto options = parse_options(args, value_keys, {});
    const fs::path template_path = require_option(options, "template");
    const fs::path context_path = require_option(options, "context");
    const fs::path output = require_option(options, "output");
    const std::string document =
        render_inline_json_template(read_artifact_bytes(template_path), read_artifact_bytes(context_path));
    publish_artifact_if_changed(output, document);
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website render-fragment: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website render-fragment: {}", error.what());
    return 3;
}

namespace {

// A static file and its SHA-256, used to name the asset bzndle by its contents.
struct static_asset {
    std::string relative_path;  // normalized forward-slash path under static/
    std::string content;
    std::string sha256;
};

// Hash static files in path order to choose the /assets/<hash>/ directory.
// Include each path with its file hash so renaming a file changes the bundle
// name  too. Reject symlinks.
[[nodiscard]] std::vector<static_asset> collect_static_assets(
    const fs::path& static_root, std::string& bundle_sha256) {
    std::vector<static_asset> assets;
    for (const auto& entry : fs::recursive_directory_iterator(static_root)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("static asset tree must not contain symlinks: "
                                     + entry.path().string());
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        static_asset asset;
        asset.relative_path =
            fs::relative(entry.path(), static_root).generic_string();
        asset.content = read_artifact_bytes(entry.path());
        asset.sha256 = bioseq::common::sha256_hex(asset.content);
        assets.push_back(std::move(asset));
    }
    std::ranges::sort(assets, [](const static_asset& lhs, const static_asset& rhs) {
        return lhs.relative_path < rhs.relative_path;
    });
    std::string manifest_bytes;
    for (const static_asset& asset : assets) {
        manifest_bytes += asset.relative_path;
        manifest_bytes.push_back('\0');
        manifest_bytes += asset.sha256;
        manifest_bytes.push_back('\n');
    }
    bundle_sha256 = bioseq::common::sha256_hex(manifest_bytes);
    return assets;
}

// CSP hashes encode the binary SHA-256 digest, not its printable hex form.
[[nodiscard]] std::string csp_script_hash(std::string_view script) {
    const auto hex = bioseq::common::sha256_hex(script);
    constexpr std::string_view digits = "0123456789abcdef";
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    std::uint32_t buffer = 0;
    unsigned bits = 0;
    for (std::size_t at = 0; at < hex.size(); at += 2) {
        buffer = (buffer << 8) | static_cast<unsigned>((digits.find(hex[at]) << 4) | digits.find(hex[at + 1]));
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            encoded += alphabet[(buffer >> bits) & 63];
        }
    }
    if (bits != 0) encoded += alphabet[(buffer << (6 - bits)) & 63];
    while (encoded.size() % 4 != 0) encoded += '=';
    return "'sha256-" + encoded + "'";
}

[[nodiscard]] std::string inline_theme_bootstrap(std::string html,
    std::string_view asset_base, std::string_view theme_script) {
    const std::string tag = "<script src=\"" + std::string(asset_base) + "js/theme.js\"></script>";
    const auto at = html.find(tag);
    if (at == std::string::npos || theme_script.find("</script") != std::string_view::npos)
        throw std::runtime_error("missing or unsafe pre-paint theme bootstrap");
    html.replace(at, tag.size(), "<script data-theme-bootstrap>" + std::string(theme_script) + "</script>");
    constexpr std::string_view directive = "script-src 'self' https://www.googletagmanager.com;";
    const auto policy = html.find(directive);
    if (policy == std::string::npos)
        throw std::runtime_error("missing script policy for pre-paint theme bootstrap");
    html.insert(policy + directive.size() - 1, " " + csp_script_hash(theme_script));
    return html;
}

// Bundle each page's authored styles without changing their cascade or relative
// asset URLs. Keep the existing shared-style insertion point as a second bundle:
// optional KaTeX/benchmark CSS still belongs between content and shared styles.
struct stylesheet_bundle_result {
    std::string html;
    std::vector<static_asset> assets;
};

[[nodiscard]] stylesheet_bundle_result bundle_page_stylesheets(
    std::string html, std::string_view asset_base,
    const std::vector<static_asset>& assets) {
    struct link_record {
        std::size_t start;
        std::size_t length;
        std::string path;
        bool shared;
    };
    std::vector<link_record> links;
    constexpr std::string_view prefix = "<link rel=\"stylesheet\" href=\"";
    const auto head_end = html.find("</head>");
    for (std::size_t at = 0; (at = html.find(prefix, at)) != std::string::npos;) {
        if (at >= head_end) break;
        const auto url_start = at + prefix.size();
        const auto url_end = html.find('"', url_start);
        const auto tag_end = html.find('>', url_end);
        if (url_end == std::string::npos || tag_end == std::string::npos)
            throw std::runtime_error("unterminated stylesheet link");
        const std::string_view href(html.data() + url_start, url_end - url_start);
        if (!href.starts_with(asset_base))
            throw std::runtime_error("page stylesheet is outside its static bundle");
        const auto tail = html.substr(url_end + 1, tag_end - url_end - 1);
        if (tail != "" && tail != " data-shared-styles")
            throw std::runtime_error("stylesheet bundling requires unconditional style links");
        links.push_back({at, tag_end - at + 1, std::string(href.substr(asset_base.size())),
                         tail == " data-shared-styles"});
        at = tag_end + 1;
    }
    stylesheet_bundle_result result;
    result.html = std::move(html);
    // Replacements run backwards so link offsets remain valid.
    std::size_t last = links.size();
    while (last != 0) {
        std::size_t first = last - 1;
        while (first > 0 && !links[first].shared) --first;
        std::string content;
        for (std::size_t index = first; index < last; ++index) {
            const auto& link = links[index];
            const auto source = std::ranges::find(assets, link.path, &static_asset::relative_path);
            if (source == assets.end())
                throw std::runtime_error("missing page stylesheet: " + link.path);
            // Every authored CSS file lives directly under css/. The vendored
            // Prism theme contains no relative references. Reject new exceptions
            // instead of silently breaking a later font/image import.
            if ((!link.path.starts_with("css/") || link.path.find('/', 4) != std::string::npos)
                && (source->content.find("url(") != std::string::npos
                    || source->content.find("URL(") != std::string::npos))
                throw std::runtime_error("cannot relocate stylesheet URLs: " + link.path);
            if (source->content.find("@import") != std::string::npos
                || source->content.find("@charset") != std::string::npos)
                throw std::runtime_error("page stylesheets must not contain imports or charset rules");
            content += "/* " + link.path + " */\n";
            content += source->content;
            content += '\n';
        }
        const auto digest = bioseq::common::sha256_hex(content);
        const std::string path = "css/page-" + digest + ".css";
        for (std::size_t index = last; index-- > first;) {
            const auto& link = links[index];
            const std::string replacement = index == first
                ? "<link rel=\"stylesheet\" href=\"" + std::string(asset_base) + path + "\""
                    + (link.shared ? " data-shared-styles" : "") + ">"
                : "";
            result.html.replace(link.start, link.length, replacement);
        }
        result.assets.push_back({path, std::move(content), digest});
        last = first;
    }
    return result;
}

void verify_stylesheet_bundles() {
    expect(csp_script_hash("abc") == "'sha256-ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0='",
        "inline script CSP uses the binary SHA-256 digest encoded as base64");
    const auto theme = inline_theme_bootstrap(
        "<head><meta content=\"script-src 'self' https://www.googletagmanager.com;\">"
        "<script src=\"/assets/hash/js/theme.js\"></script></head>", "/assets/hash/", "abc");
    expect(theme.find("<script data-theme-bootstrap>abc</script>") != std::string::npos
        && theme.find(csp_script_hash("abc")) != std::string::npos,
        "pre-paint theme executes without a blocking network request or an unsafe-inline policy");
    const std::vector<static_asset> assets{
        {"css/site.css", "@font-face{src:url('../fonts/example.woff2')} .x{color:red}", ""},
        {"vendor/prism/prism.css", ".token{color:orange}", ""},
        {"css/shared.css", ".x{color:blue}", ""},
        {"css/theme.css", ".x{color:green}", ""},
    };
    for (const std::string base : {"/assets/hash/", "/bioseqdb/assets/hash/"}) {
        const std::string html = "<head><title>Example</title>"
            "<link rel=\"stylesheet\" href=\"" + base + "css/site.css\">"
            "<link rel=\"stylesheet\" href=\"" + base + "vendor/prism/prism.css\">"
            "<link rel=\"stylesheet\" href=\"" + base + "css/shared.css\" data-shared-styles>"
            "<link rel=\"stylesheet\" href=\"" + base + "css/theme.css\"></head><body>Example</body>";
        const auto result = bundle_page_stylesheets(html, base, assets);
        expect(result.assets.size() == 2, "stylesheets retain the optional-content insertion boundary");
        expect(result.assets[1].content.find("color:red") < result.assets[1].content.find("color:orange")
            && result.assets[0].content.find("color:blue") < result.assets[0].content.find("color:green"),
            "stylesheet bundles preserve the authored cascade");
        expect(result.assets[1].content.find("url('../fonts/example.woff2')") != std::string::npos
            && result.html.find(base + "css/page-") != std::string::npos
            && result.html.ends_with("</head><body>Example</body>"),
            "bundling preserves font paths, deployment prefixes, and non-style markup");
        expect(result.html.find("data-shared-styles") != std::string::npos,
            "optional styles retain their DOM insertion marker");
    }
    auto invalid = assets;
    invalid[1].content = "a{background:url('./image.png')}";
    bool rejected = false;
    try {
        (void)bundle_page_stylesheets("<head><link rel=\"stylesheet\" href=\"/assets/hash/vendor/prism/prism.css\"></head>",
            "/assets/hash/", invalid);
    } catch (const std::runtime_error&) { rejected = true; }
    expect(rejected, "bundling rejects styles whose relative URLs would change meaning");
}

[[nodiscard]] std::vector<std::pair<std::string, const static_asset*>> resolve_stable_assets(
    const std::map<std::string, std::string, std::less<>>& aliases,
    const std::vector<static_asset>& assets,
    const std::map<std::string, std::string, std::less<>>& published) {
    std::vector<std::pair<std::string, const static_asset*>> resolved;
    std::set<std::string> routes;
    for (const auto& [route, hash] : published) routes.insert(route);
    for (const auto& [destination, source] : aliases) {
        const auto asset = std::ranges::find(assets, source, &static_asset::relative_path);
        if (asset == assets.end())
            throw std::runtime_error("stable asset source was not found under static/: " + source);
        const auto route = "/" + destination;
        for (const auto& existing : routes) {
            if (route == existing || route.starts_with(existing + "/") || existing.starts_with(route + "/"))
                throw std::runtime_error("stable asset path collides with published content: " + destination);
        }
        routes.insert(route);
        resolved.emplace_back(destination, &*asset);
    }
    return resolved;
}

void verify_stable_assets() {
    const std::string config_yaml =
        "title: BioseqDB\nshort_description: Sequences\ncanonical_origin: https://bioseqdb.com\n"
        "default_base_path: /mounted/\nrepository_url: https://github.com/styczynski/bioseqdb\n"
        "source_url_template: https://github.com/styczynski/bioseqdb/blob/{revision}/{path}#L{line}\n"
        "default_social_image: static/media/social_card.png\ncopyright_holder: BioseqDB\ncopyright_year: 2026\n"
        "navigation: {documentation: Docs, tutorials: Tutorials, reference: Reference}\n"
        "cpp_public_modules: [{module: test, primary_interface: test.cppm}]\n"
        "required_documentation_entities: []\n";
    const auto config = parse_site_configuration(config_yaml
        + "stable_assets: {bioseq-mark.svg: bioseq-mark.svg}\n", "site.yaml");
    const std::vector<static_asset> assets{{"bioseq-mark.svg", "<svg/>", "hash"}};
    const auto aliases = resolve_stable_assets(config.stable_assets, assets, {{"/assets/hash/bioseq-mark.svg", "hash"}});
    expect(aliases.size() == 1 && aliases.front().first == "bioseq-mark.svg"
        && aliases.front().second->content == "<svg/>" && config.default_base_path == "/mounted/",
        "stable asset alias preserves source bytes beside the hashed bundle and uses a deployment-relative route");
    for (const auto& mapping : {"../escape.svg: bioseq-mark.svg", "/absolute.svg: bioseq-mark.svg",
             "mark.svg: ../outside.svg", "assets/mark.svg: bioseq-mark.svg",
             "site-manifest.json: bioseq-mark.svg", "mark.svg?query: bioseq-mark.svg",
             "nested//mark.svg: bioseq-mark.svg"}) {
        bool rejected = false;
        try { (void)parse_site_configuration(config_yaml + "stable_assets: {" + mapping + "}\n", "bad.yaml"); }
        catch (const std::exception&) { rejected = true; }
        expect(rejected, "stable assets reject traversal, absolute paths, reserved routes and invalid URL paths");
    }
    bool missing_rejected = false;
    try { (void)resolve_stable_assets({{"missing.svg", "missing.svg"}}, assets, {}); }
    catch (const std::exception&) { missing_rejected = true; }
    expect(missing_rejected, "stable asset source must exist in the collected bundle");
    for (const auto& occupied : {"/bioseq-mark.svg", "/bioseq-mark.svg/child"}) {
        bool rejected = false;
        try { (void)resolve_stable_assets(config.stable_assets, assets, {{occupied, "hash"}}); }
        catch (const std::exception&) { rejected = true; }
        expect(rejected, "stable aliases cannot overwrite or shadow generated publication routes");
    }
    bool nested_rejected = false;
    try { (void)resolve_stable_assets({{"parent", "bioseq-mark.svg"}, {"parent/mark.svg", "bioseq-mark.svg"}}, assets, {}); }
    catch (const std::exception&) { nested_rejected = true; }
    expect(nested_rejected, "stable alias paths cannot shadow one another");
}

// Collect file routes for comparison with, the site manifest. Reject
// symlinks and special files so every listed output is a regular file.
void collect_site_files(const fs::path& root, std::set<std::string>& routes) {
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_symlink()) {
            throw std::runtime_error("site tree must not contain symlinks: "
                                     + entry.path().string());
        }
        if (entry.is_directory()) {
            continue;
        }
        if (!entry.is_regular_file()) {
            throw std::runtime_error("site tree contains a non-regular file: "
                                     + entry.path().string());
        }
        routes.insert("/" + fs::relative(entry.path(), root).generic_string());
    }
}

[[nodiscard]] fs::path parent_or_dot(const fs::path& path) {
    const fs::path parent = path.parent_path();
    return parent.empty() ? fs::path(".") : parent;
}

// Attempt to flush a file before the next publication step. Errors are ignored.
void fsync_file(const fs::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor >= 0) {
        ::fsync(descriptor);
        ::close(descriptor);
    }
}

// Attempt to flush directory changes after a create or rename. Errors are ignored.
void fsync_directory(const fs::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor >= 0) {
        ::fsync(descriptor);
        ::close(descriptor);
    }
}

// Publish the complete staging tree with a directory rename. If output
// exists, atomically swap the two directories, then remove the old tree.
// Platforms without an atomic directory swap  fail instead of copying files.
void atomic_publish(const fs::path& staging, const fs::path& output) {
    std::error_code error;
    if (!fs::exists(output, error)) {
        fs::rename(staging, output, error);
        if (error) {
            throw std::runtime_error("publish rename failed: " + error.message());
        }
        return;
    }
#if defined(__linux__)
    const long result = ::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(),
                                  AT_FDCWD, output.c_str(),
                                  static_cast<unsigned int>(RENAME_EXCHANGE));
    if (result != 0) {
        throw std::runtime_error(std::string("renameat2 exchange failed: ")
                                 + std::strerror(errno));
    }
#elif defined(__APPLE__)
    const int result = ::renamex_np(
        staging.c_str(), output.c_str(), static_cast<unsigned int>(RENAME_SWAP));
    if (result != 0) {
        throw std::runtime_error(std::string("renamex_np swap failed: ")
                                 + std::strerror(errno));
    }
#else
    throw std::runtime_error(
        "atomic directory exchange is unsupported on this platform");
#endif
    fs::remove_all(staging, error);
}

[[nodiscard]] std::size_t count_substring(std::string_view text, std::string_view needle) {
    std::size_t total = 0;
    std::size_t position = text.find(needle);
    while (position != std::string_view::npos) {
        ++total;
        position = text.find(needle, position + needle.size());
    }
    return total;
}

// Check generated HTML for leftover template markers, forbidden resource
// URLs, and selected inline handlers or styles. Also check the h1 count and
// required CSP text. These are string checks on the generated markup.
void validate_generated_html(std::string_view route, std::string_view html) {
    const auto fail = [route](std::string_view why) -> void {
        throw std::runtime_error("generated HTML validation failed for "
                                 + std::string(route) + ": " + std::string(why));
    };
    // Opening delimiters identify leftover Jinja expressions, statements, or
    // comments. Adjacent closing braces are also valid nested JSON objects.
    if (html.find("{{") != std::string_view::npos
        || html.find("{%") != std::string_view::npos
        || html.find("{#") != std::string_view::npos) {
        fail("contains an unrendered template placeholder");
    }
    if (html.find("javascript:") != std::string_view::npos) {
        fail("contains a javascript: URL");
    }
    // Reject SQL section markers tht indicate a hidden section reached the page.
    for (const std::string_view marker :
         {"bioseq:setup", "bioseq:visible", "bioseq:teardown"}) {
        if (html.find(marker) != std::string_view::npos) {
            fail("leaks a hidden runnable-example SQL section marker");
        }
    }
    for (const std::string_view handler :
         {" onclick=", " onload=", " onerror=", " onmouseover=", " onfocus=",
          " onsubmit=", " onchange="}) {
        if (html.find(handler) != std::string_view::npos) {
            fail("contains an inline event handler");
        }
    }
    if (html.find(" style=") != std::string_view::npos) {
        fail("contains an inline style attribute");
    }
    // The analytics loader and Shields badge images are configured by the
    // site owner. Other remote script/media resources remain forbidden.
    std::string resource_html(html);
    const std::string analytics_tag =
        "<script async src=\"https://www.googletagmanager.com/gtag/js?id=G-989VYH5HT7\"></script>";
    if (const auto at = resource_html.find(analytics_tag); at != std::string::npos) {
        resource_html.erase(at, analytics_tag.size());
    }
    constexpr std::string_view badge_prefix = "<img src=\"https://img.shields.io/";
    std::size_t badge_at = 0;
    while ((badge_at = resource_html.find(badge_prefix, badge_at)) != std::string::npos) {
        const auto url_begin = badge_at + std::string_view("<img src=\"").size();
        const auto url_end = resource_html.find('"', badge_at + badge_prefix.size());
        if (url_end == std::string::npos) break;
        const auto url = resource_html.substr(url_begin, url_end - url_begin);
        if (url.find_first_of("<>") == std::string::npos) {
            resource_html.replace(url_begin, url_end - url_begin, "shields-badge");
        }
        ++badge_at;
    }
    if (resource_html.find("src=\"http") != std::string::npos
        || resource_html.find("src='http") != std::string::npos) {
        fail("references an external script/media resource");
    }
    if (html.find("<link") != std::string_view::npos
        && html.find("stylesheet\" href=\"http") != std::string_view::npos) {
        fail("references an external stylesheet");
    }
    if (count_substring(html, "<h1") != 1) {
        fail("must contain exactly one <h1>");
    }
    if (html.find("Content-Security-Policy") == std::string_view::npos) {
        fail("missing the meta Content-Security-Policy");
    }
    if (html.find("frame-ancestors") != std::string_view::npos) {
        fail("frame-ancestors must not appear in the meta CSP");
    }
}

void verify_generated_html_template_markers() {
    const std::string opening = "<!doctype html><html><head>"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'self'\">"
        "</head><body><h1>Example</h1>";
    const std::string closing = "</body></html>";
    validate_generated_html("/example/", opening
        + "<script type=\"application/ld+json\">{\"publisher\":{\"logo\":{\"url\":\"/logo.svg\"}}}</script>"
        + closing);
    for (const std::string_view marker : {"{{ page.title }}", "{% if page %}", "{# comment #}"}) {
        bool rejected = false;
        try { validate_generated_html("/example/", opening + std::string(marker) + closing); }
        catch (const std::runtime_error&) { rejected = true; }
        expect(rejected, "generated HTML rejects opening template markers but accepts nested JSON objects");
    }
}

// Write files into staging and record their routes and SHA-256 hashes.
// HTML files pass the markup checks before writing The caller writes the
// manifest separately after all outputs are recorded.
struct staging_writer {
    fs::path root;
    std::map<std::string, std::string, std::less<>> manifest;

    void emit(std::string_view route, std::string_view content) {
        if (!route.starts_with("/")) {
            throw std::runtime_error("site route must be absolute: " + std::string(route));
        }
        if (route.ends_with(".html")) {
            validate_generated_html(route, content);
        }
        const fs::path destination = root / route.substr(1);
        fs::create_directories(destination.parent_path());
        {
            std::ofstream out(destination, std::ios::binary | std::ios::trunc);
            if (!out) {
                throw std::runtime_error("cannot write " + destination.string());
            }
            out.write(content.data(), static_cast<std::streamsize>(content.size()));
            if (!out) {
                throw std::runtime_error("cannot finish writing " + destination.string());
            }
        }
        manifest.emplace(std::string(route), bioseq::common::sha256_hex(content));
    }
};

// Quote nonempty manifest metadata, or  emit null. Values must already be JSON-safe.
[[nodiscard]] std::string manifest_optional(std::string_view value) {
    return value.empty() ? std::string("null")
                         : "\"" + std::string(value) + "\"";
}

// Add page URLs and tooltip links to resolved entity references. Tutorial
// links already carry entity IDs. Doxygen links use refid aliases, which
// are looked up here. Resolve URLs during rendering for the chosen base path.
void rewrite_entity_link_routes(
    std::vector<content_node>& nodes,
    const std::map<std::string, std::string, std::less<>>& identity_by_reference,
    const std::map<std::string, std::string, std::less<>>& route_by_id,
    const std::string& tooltip_url) {
    for (content_node& node : nodes) {
        if (node.kind == content_node_kind::entity_link) {
            const auto identity = node.attributes.find("entity_id");
            std::string canonical_id;
            if (identity != node.attributes.end()) {
                canonical_id = identity->second;
            } else if (const auto alias = identity_by_reference.find(node.target);
                       alias != identity_by_reference.end()) {
                canonical_id = alias->second;
                node.attributes["entity_id"] = canonical_id;
            }
            if (!canonical_id.empty()) {
                const auto route = route_by_id.find(canonical_id);
                if (route != route_by_id.end()) {
                    node.target = route->second;
                    node.attributes["tooltip_url"] = tooltip_url;
                }
            }
        }
        rewrite_entity_link_routes(
            node.children, identity_by_reference, route_by_id, tooltip_url);
    }
}

// Escape quotes, backslashes, and whitespace used in entity-range JSON strings.
[[nodiscard]] std::string json_escape(std::string_view text) {
    std::string out;
    for (const char character : text) {
        switch (character) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(character); break;
        }
    }
    return out;
}

// Link catalog identifiers in SQL and inline code without changing the source text.
// Explicit entity= annotations disambiguate a spelling; overload groups otherwise
// own a function name. Quoted strings and comments are not parsed as executable SQL.
void decorate_code_entity_ranges(
    std::vector<content_node>& nodes,
    const std::map<std::string, std::string, std::less<>>& route_by_id,
    const std::map<std::string, std::string, std::less<>>& qualified_by_id,
    const std::string& tooltip_url,
    const std::string& where) {
    std::map<std::string, std::string, std::less<>> ids;
    const auto priority = [](std::string_view id) {
        if (id.starts_with("sql:type:")) return 4;
        if (id.find("overload") != std::string_view::npos) return 3;
        if (id.starts_with("sql:function:")) return 2;
        return 1;
    };
    for (const auto& [id, qualified] : qualified_by_id) {
        if (!id.starts_with("sql:") || qualified.empty()) continue;
        const auto add = [&](const std::string& spelling) {
            const auto existing = ids.find(spelling);
            if (existing == ids.end() || priority(id) > priority(existing->second))
                ids[spelling] = id;
        };
        add(qualified);
        if (id.starts_with("sql:guc:")) {
            if (qualified.starts_with("bioseq.")) add(qualified.substr(7));
        } else if (const auto dot = qualified.rfind('.'); dot != std::string::npos)
            add(qualified.substr(dot + 1));
    }
    const auto identifier = [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '$';
    };
    // Browser text ranges count UTF-16 units, while source strings contain UTF-8.
    const auto utf16_length = [](std::string_view text) {
        std::size_t count = 0;
        for (unsigned char c : text) {
            if ((c & 0xc0) != 0x80) count += c >= 0xf0 ? 2 : 1;
        }
        return count;
    };
    const auto walk = [&](std::vector<content_node>& list, auto&& self) -> void {
        for (content_node& node : list) {
            if (node.kind == content_node_kind::raw_html && node.text.contains("<code>")) {
                // Imported configuration tables retain authored HTML. Split
                // only plain code-cell text into a typed link node, leaving
                // table markup and wording untouched by the linker.
                std::vector<content_node> fragments;
                std::size_t consumed = 0;
                std::size_t cursor = 0;
                while ((cursor = node.text.find("<code>", cursor)) != std::string::npos) {
                    const auto begin = cursor + 6;
                    const auto end = node.text.find("</code>", begin);
                    if (end == std::string::npos) break;
                    const auto spelling = node.text.substr(begin, end - begin);
                    const auto found = ids.find(spelling);
                    const auto last_anchor = node.text.rfind("<a ", cursor);
                    const auto last_close = node.text.rfind("</a>", cursor);
                    const bool inside_link = last_anchor != std::string::npos
                        && (last_close == std::string::npos || last_anchor > last_close);
                    if (found != ids.end() && found->second.starts_with("sql:guc:")
                        && !inside_link && route_by_id.contains(found->second)) {
                        fragments.push_back(content_node{.kind = content_node_kind::raw_html,
                            .text = node.text.substr(consumed, begin - consumed)});
                        fragments.push_back(content_node{.kind = content_node_kind::entity_link,
                            .text = spelling, .target = route_by_id.at(found->second),
                            .attributes = {{"entity_id", found->second}, {"tooltip_url", tooltip_url}}});
                        consumed = end;
                    }
                    cursor = end + 7;
                }
                if (!fragments.empty()) {
                    fragments.push_back(content_node{.kind = content_node_kind::raw_html,
                        .text = node.text.substr(consumed)});
                    node.kind = content_node_kind::root;
                    node.text.clear();
                    node.children = std::move(fragments);
                }
            }
            const bool inline_code = node.kind == content_node_kind::code_span;
            const auto language = node.attributes.find("language");
            if (inline_code || (node.kind == content_node_kind::code_block
                && language != node.attributes.end() && language->second == "sql")) {
                auto names = ids;
                if (const auto info = node.attributes.find("info"); info != node.attributes.end()) {
                    for (const auto& id : parse_fence_info(info->second).entities) {
                        const auto found = qualified_by_id.find(id);
                        if (found == qualified_by_id.end())
                            throw std::runtime_error("code entity '" + id + "' does not exist in " + where);
                        if (node.text.find(found->second) == std::string::npos)
                            throw std::runtime_error("code entity spelling '" + found->second
                                + "' is not present in the block in " + where);
                        names[found->second] = id;
                    }
                }
                std::string json = "[";
                std::string source_text = node.text;
                if (inline_code) {
                    const auto append = [&source_text](const content_node& child, auto&& self_append) -> void {
                        source_text += child.text;
                        for (const auto& nested : child.children) self_append(nested, self_append);
                    };
                    for (const auto& child : node.children) append(child, append);
                }
                const std::string_view text = source_text;
                for (std::size_t pos = 0; pos < text.size();) {
                    if (text.substr(pos).starts_with("--")) {
                        const auto end = text.find('\n', pos);
                        pos = end == std::string_view::npos ? text.size() : end;
                        continue;
                    }
                    if (text.substr(pos).starts_with("/*")) {
                        std::size_t depth = 1;
                        pos += 2;
                        while (pos < text.size() && depth) {
                            if (text.substr(pos).starts_with("/*")) { ++depth; pos += 2; }
                            else if (text.substr(pos).starts_with("*/")) { --depth; pos += 2; }
                            else ++pos;
                        }
                        continue;
                    }
                    if (text[pos] == '\'') {
                        ++pos;
                        while (pos < text.size()) {
                            if (text[pos++] != '\'') continue;
                            if (pos < text.size() && text[pos] == '\'') { ++pos; continue; }
                            break;
                        }
                        continue;
                    }
                    if (text[pos] == '$') {
                        auto end = pos + 1;
                        while (end < text.size() && identifier(text[end]) && text[end] != '$') ++end;
                        if (end < text.size() && text[end] == '$') {
                            const auto delimiter = text.substr(pos, end - pos + 1);
                            const auto close = text.find(delimiter, end + 1);
                            pos = close == std::string_view::npos ? text.size() : close + delimiter.size();
                            continue;
                        }
                    }
                    if (!identifier(text[pos]) && text[pos] != '"') { ++pos; continue; }
                    const auto begin = pos;
                    std::string spelling;
                    do {
                        if (text[pos] == '"') {
                            ++pos;
                            while (pos < text.size() && text[pos] != '"') spelling += text[pos++];
                            if (pos < text.size()) ++pos;
                        } else {
                            while (pos < text.size() && identifier(text[pos])) {
                                const char c = text[pos++];
                                spelling += c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
                            }
                        }
                        if (pos + 1 >= text.size() || text[pos] != '.'
                            || (!identifier(text[pos + 1]) && text[pos + 1] != '"')) break;
                        spelling += '.';
                        ++pos;
                    } while (pos < text.size());
                    auto next = pos;
                    while (next < text.size() && (text[next] == ' ' || text[next] == '\n')) ++next;
                    const auto found = names.find(spelling);
                    if (found == names.end()) continue;
                    const bool qualified = spelling.find('.') != std::string::npos;
                    const bool invocation = next < text.size() && text[next] == '(';
                    auto previous = begin;
                    while (previous > 0 && (text[previous - 1] == ' ' || text[previous - 1] == '\n')) --previous;
                    const bool cast_type = found->second.starts_with("sql:type:")
                        && previous >= 2 && text.substr(previous - 2, 2) == "::";
                    if (!qualified && !inline_code && !invocation && !cast_type) continue;
                    const auto route = route_by_id.find(found->second);
                    if (route == route_by_id.end()) continue;
                    if (json.size() > 1) json += ',';
                    json += "{\"start\":" + std::to_string(utf16_length(text.substr(0, begin)))
                        + ",\"len\":" + std::to_string(utf16_length(text.substr(begin, pos - begin)))
                        + ",\"id\":\"" + json_escape(found->second) + "\""
                        + ",\"route\":\"" + json_escape(route->second) + "\""
                        + ",\"tooltip\":\"" + json_escape(tooltip_url) + "\"}";
                }
                json += ']';
                if (json.size() > 2) node.attributes["entity_ranges"] = std::move(json);
            }
            self(node.children, self);
        }
    };
    walk(nodes, walk);
}

[[nodiscard]] landing_content load_landing_content(
    const fs::path& site_config_path, std::string_view filename = "landing.yaml") {
    const fs::path copy_path = site_config_path.parent_path() / filename;
    if (!fs::is_regular_file(copy_path)) return {};
    return parse_landing_content(read_artifact_bytes(copy_path), copy_path.generic_string());
}

[[nodiscard]] qa_content load_qa_content(const fs::path& site_config_path) {
    const fs::path path = site_config_path.parent_path() / "qa.yaml";
    if (!fs::is_regular_file(path)) return {};
    return parse_qa_content(read_artifact_bytes(path), path.generic_string());
}

/// @brief Renders a saved site model into a staging directory.
///
/// Uses the model, templates, and static files without a database connection.
/// Resolves page URLs for the requested base path and names the asset bundle
/// by its hash. Writes the manifest and checks that its file list matches
/// staging. Returns the manifest SHA-256 for publication.
[[nodiscard]] std::string render_site_model(
    const site_model& model, const site_configuration& config,
    const std::string& base_path, const std::string& revision,
    const fs::path& templates_dir, const fs::path& static_dir, const fs::path& staging,
    const landing_content& landing_copy = {}, const landing_content& contact_copy = {},
    const landing_content& blog_copy = {}, const qa_content& questions = {},
    const boost::json::object& page_dates = {}, const fs::path& thumbnails = {}) {
    const std::string version = model.version.path_component;
    const auto url = [&base_path](std::string_view rel) -> std::string {
        return base_path + std::string(rel);
    };
    const auto route = [](std::string_view rel) -> std::string {
        return "/" + std::string(rel);
    };
    const auto canonical = [&config, &url](
                               std::string_view rel) -> std::string {
        std::string trimmed(rel);
        if (trimmed.ends_with("index.html")) {
            trimmed.erase(trimmed.size() - std::string_view("index.html").size());
        }
        return config.canonical_origin + url(trimmed);
    };
    const std::string first_tutorial_url = model.tutorials.empty()
        ? url(version + "/")
        : url(version + "/tutorials/" + model.tutorials.front().metadata.slug + "/");

    // Name the static bundle by its content hash and copy it into staging.
    std::string bundle_sha256;
    const std::vector<static_asset> assets =
        collect_static_assets(static_dir, bundle_sha256);
    const std::string bundle_prefix = "assets/" + bundle_sha256 + "/";
    const auto theme_asset = std::ranges::find(assets, "js/theme.js", &static_asset::relative_path);
    if (theme_asset == assets.end()) throw std::runtime_error("missing theme bootstrap asset");
    const std::string theme_script = theme_asset->content;
    const std::string theme_csp_hash = csp_script_hash(theme_script);

    std::error_code staging_error;
    fs::remove_all(staging, staging_error);
    fs::create_directories(staging);
    staging_writer writer{.root = staging, .manifest = {}};
    for (const static_asset& asset : assets) {
        writer.emit(route(bundle_prefix + asset.relative_path), asset.content);
    }

    // Prepare shared page fields and URLs for  the chosen base path.
    site_view site;
    site.title = config.title;
    site.base_path = base_path;
    site.repository_url = config.repository_url;
    site.copyright_holder = config.copyright_holder;
    site.copyright_year = config.copyright_year;
    site.status_label = config.status_label;
    site.version = version;
    site.assets_base_url = url(bundle_prefix);
    site.css_url = url(bundle_prefix + "css/site.css");
    site.js_url = url(bundle_prefix + "js/main.js");
    site.favicon_url = url(bundle_prefix + "favicon.svg");
    site.prism_js_url = url(bundle_prefix + "vendor/prism/prism.min.js");
    site.prism_css_url = url(bundle_prefix + "vendor/prism/prism.min.css");
    // Build the social-image URL inside the asset bundle The configured path
    // may start with static/, which is removed before adding the bundle prefix.
    std::string social_relative = config.default_social_image;
    if (social_relative.starts_with("static/")) {
        social_relative.erase(0, std::string_view("static/").size());
    }
    site.social_image_url = config.canonical_origin + url(bundle_prefix + social_relative);
    site.documentation_url = first_tutorial_url;
    site.tutorials_url = first_tutorial_url;
    site.reference_url = url(version + "/reference/");
    site.search_index_url = url(version + "/search-index.json");
    site.versions_url = url("versions.json");
    site.versions_page_url = url("versions/");
    site.nav_documentation = config.navigation.at("documentation");
    site.nav_tutorials = config.navigation.at("tutorials");
    site.nav_reference = config.navigation.at("reference");

    boost::json::object landing_context;
    for (const auto& [section, entries] : landing_copy) {
        boost::json::object values;
        for (const auto& [key, value] : entries) {
            values[key] = boost::json::parse(json_text(value));
        }
        landing_context[section] = std::move(values);
    }
    boost::json::object contact_context;
    for (const auto& [section, entries] : contact_copy) {
        boost::json::object values;
        for (const auto& [key, value] : entries) {
            values[key] = boost::json::parse(json_text(value));
        }
        contact_context[section] = std::move(values);
    }
    boost::json::object blog_context;
    for (const auto& [section, entries] : blog_copy) {
        boost::json::object values;
        for (const auto& [key, value] : entries)
            values[key] = boost::json::parse(json_text(value));
        blog_context[section] = std::move(values);
    }
    const auto contact_text = [&contact_copy](
                                  std::string_view section, std::string_view key,
                                  std::string fallback) -> std::string {
        const auto group = contact_copy.find(section);
        if (group == contact_copy.end()) return fallback;
        const auto value = group->second.find(key);
        return value == group->second.end() ? fallback : value->second;
    };
    const auto landing_meta = [&landing_copy](std::string_view key, const std::string& fallback) {
        const auto meta = landing_copy.find("meta");
        if (meta == landing_copy.end()) return fallback;
        const auto value = meta->second.find(key);
        return value == meta->second.end() ? fallback : value->second;
    };
    // Dates come from source history; declared blog dates remain authoritative.
    std::map<std::string, std::string, std::less<>> page_sources{
        {"index.html", "website/landing.yaml"},
        {"qa/index.html", "website/qa.yaml"},
        {"contact/index.html", "website/contact.yaml"},
        {"contact_thanks/index.html", "website/contact.yaml"},
        {"benchmarks/index.html", "website/v0.1/tutorials/project_benchmarking.md"}};
    for (const tutorial& item : model.tutorials)
        page_sources[version + "/tutorials/" + item.metadata.slug + "/index.html"] =
            "website/" + item.source_path;
    for (const tutorial& item : model.blog_posts)
        page_sources["blog/" + item.metadata.slug + "/index.html"] = "website/" + item.source_path;
    const auto record_sources = [&](const auto& entities) {
        for (const auto& entity : entities)
            if (entity.declaration)
                page_sources[reference_entity_relative_route(version, entity) + "index.html"] =
                    entity.declaration->path.generic_string();
    };
    record_sources(model.cpp_entities);
    record_sources(model.sql_entities);
    const auto render = [&route, &url, &config, &site, &templates_dir, &writer, &landing_context,
                         &contact_context, &blog_context, &page_sources, &page_dates, &thumbnails, &assets, &bundle_prefix, &theme_script](
                            std::string_view rel,
                            std::string_view template_name,
                            std::string_view page_object,
                            std::string_view key,
                            std::string_view collection) -> void {
        auto page = boost::json::parse(page_object).as_object();
        if (!page_dates.empty()) {
            const auto source = page_sources.find(rel);
            const std::string source_path = source == page_sources.end()
                ? "website/site.yaml" : source->second;
            const auto& paths = page_dates.at("paths").as_object();
            const auto* history = paths.if_contains(source_path);
            const auto& dates = (history ? *history : page_dates.at("default")).as_object();
            if (!page.if_contains("date") || page.at("date").as_string().empty())
                page["date"] = dates.at("date");
            page["modified_date"] = dates.at("modified_date");
            // An explicitly dated article cannot have an earlier modification date.
            if (page.at("modified_date").as_string() < page.at("date").as_string())
                page["modified_date"] = page.at("date");
        }
        const fs::path relative_page{std::string(rel)};
        const fs::path thumbnail_relative = relative_page.parent_path() /
            (relative_page.filename() == "index.html" ? "thumbnail.jpg"
                : relative_page.stem().string() + "-thumbnail.jpg");
        // The preview pass advertises the images about to be captured. Plain
        // standalone renders without SEO inputs keep their existing social image.
        if (!page_dates.empty() || !thumbnails.empty())
            page["thumbnail_url"] = config.canonical_origin + url(thumbnail_relative.generic_string());
        if (!thumbnails.empty()) {
            const fs::path image = thumbnails / thumbnail_relative;
            if (!fs::is_regular_file(image))
                throw std::runtime_error("missing rendered page thumbnail: " + image.string());
            writer.emit(route(thumbnail_relative.generic_string()), read_artifact_bytes(image));
        }
        auto context = boost::json::parse(build_page_context(
            site, boost::json::serialize(page), key, collection));
        context.as_object()["landing"] = landing_context;
        context.as_object()["contact"] = contact_context;
        context.as_object()["blog"] = blog_context;
        const auto bundled = bundle_page_stylesheets(
            render_page_template(templates_dir.string(), template_name, boost::json::serialize(context)),
            site.assets_base_url, assets);
        for (const auto& asset : bundled.assets) {
            const auto asset_route = route(bundle_prefix + asset.relative_path);
            if (!writer.manifest.contains(asset_route)) writer.emit(asset_route, asset.content);
        }
        writer.emit(route(rel), inline_theme_bootstrap(bundled.html, site.assets_base_url, theme_script));
    };

    // Rebuild entity routes for this deployment base path and reject duplicate
    // IDs or paths. SQL routes use stable manifest identities, so moving the
    // extension to another schema keeps the URL. Page titles use installed names.
    std::vector<reference_entity> cpp_entities = model.cpp_entities;
    std::vector<reference_entity> sql_entities = model.sql_entities;
    std::set<std::string> seen_routes;
    std::set<std::string> seen_ids;
    const auto assign_routes = [
                                   &seen_ids,
                                   &seen_routes,
                                   &url,
                                   &version](
                                   std::vector<reference_entity>& list)
        -> void {
        for (reference_entity& entity : list) {
            if (!seen_ids.insert(entity.canonical_id).second) {
                throw std::runtime_error("duplicate canonical id: " + entity.canonical_id);
            }
            const std::string rel =
                reference_entity_relative_route(version, entity);
            if (!seen_routes.insert(rel).second) {
                throw std::runtime_error("entity route collision at " + rel);
            }
            entity.route_path = url(rel);
        }
    };
    assign_routes(cpp_entities);
    assign_routes(sql_entities);

    // Build shared lookup maps for reference links, search, and tooltips.
    // Reject a Doxygen refid that points to different entity IDs.
    std::vector<reference_entity> all_entities = cpp_entities;
    all_entities.insert(all_entities.end(), sql_entities.begin(), sql_entities.end());
    std::map<std::string, std::string, std::less<>> route_by_id;
    std::map<std::string, std::string, std::less<>> qualified_by_id;
    std::map<std::string, std::string, std::less<>> identity_by_reference;
    for (const reference_entity& entity : all_entities) {
        route_by_id.emplace(entity.canonical_id, entity.route_path);
        qualified_by_id.emplace(entity.canonical_id, entity.qualified_name);
        identity_by_reference.emplace(entity.canonical_id, entity.canonical_id);
        if (const auto refid = entity.attributes.find("refid");
            refid != entity.attributes.end() && !refid->second.empty()) {
            const auto [existing, inserted] =
                identity_by_reference.emplace(refid->second, entity.canonical_id);
            if (!inserted && existing->second != entity.canonical_id) {
                throw std::runtime_error(
                    "render: Doxygen refid '" + refid->second
                    + "' maps to more than one canonical entity");
            }
        }
    }

    // Add each overload signature and page URL to its  group page.
    for (reference_entity& group : sql_entities) {
        if (group.kind != entity_kind::overload_group) {
            continue;
        }
        for (const std::string& member_id : group.overload_ids) {
            const auto member = std::ranges::find_if(
                sql_entities, [&member_id](
                                  const reference_entity& candidate) -> bool {
                    return candidate.canonical_id == member_id;
                });
            if (member == sql_entities.end()) {
                continue;
            }
            const auto route = route_by_id.find(member_id);
            group.overload_members.emplace_back(
                member->normalized_signature,
                route != route_by_id.end() ? route->second : std::string());
        }
    }

    // Group C++ members under their qualified parent aame for container pages.
    // Find the last :: outside template arguments and parameter lists so a type
    // name inside a signature cannot be mistaken for the parent.
    const auto parent_of = [](std::string_view name) -> std::string {
        int depth = 0;
        std::size_t last = std::string::npos;
        for (std::size_t index = 0; index + 1 < name.size(); ++index) {
            const char character = name[index];
            if (character == '<' || character == '(') {
                ++depth;
            } else if ((character == '>' || character == ')') && depth > 0) {
                --depth;
            } else if (depth == 0 && character == ':' && name[index + 1] == ':') {
                last = index;
            }
        }
        return last == std::string::npos ? std::string()
                                         : std::string(name.substr(0, last));
    };
    std::map<std::string, std::vector<std::pair<std::string, std::string>>, std::less<>>
        children_by_parent;
    for (const reference_entity& entity : all_entities) {
        if (entity.kind == entity_kind::overload_group) {
            continue;  // overload groups have their own member list
        }
        const std::string parent = parent_of(entity.qualified_name);
        if (!parent.empty()) {
            children_by_parent[parent].emplace_back(entity.display_name,
                                                    entity.route_path);
        }
    }
    for (auto& [parent, children] : children_by_parent) {
        std::ranges::sort(children);
    }
    const auto attach_relations = [&children_by_parent, &route_by_id](
                                      std::vector<reference_entity>& list)
        -> void {
        for (reference_entity& entity : list) {
            if (const auto found = children_by_parent.find(entity.qualified_name);
                found != children_by_parent.end()) {
                entity.child_members = found->second;
            }
            if (entity.implementation_entity_id) {
                if (const auto route = route_by_id.find(*entity.implementation_entity_id);
                    route != route_by_id.end()) {
                    entity.implemented_by_route = route->second;
                }
            }
        }
    };
    attach_relations(cpp_entities);
    attach_relations(sql_entities);

    const std::string tooltip_json = serialize_reference_tooltips(all_entities);
    const std::string tooltip_url =
        url(version + "/entities." + bioseq::common::sha256_hex(tooltip_json) + ".json");

    // Build breadcrumb data and an index of tutorials linking to each entity.
    // Breadcrumb objects use name and url, with the current page last.
    const auto breadcrumb_json =
        [](const std::vector<std::pair<std::string, std::string>>& crumbs)
            -> std::string {
            std::string out = "[";
            bool first = true;
            for (const auto& [name, href] : crumbs) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += "{\"name\":" + json_text(name) + ",\"url\":" + json_text(href) + "}";
            }
            out += "]";
            return out;
        };
    std::map<std::string, std::vector<std::pair<std::string, std::string>>, std::less<>>
        backlinks_by_id;
    for (const tutorial& item : model.tutorials) {
        std::set<std::string> refs;
        collect_referenced_entity_ids(item.content, refs);
        const std::string tutorial_route =
            url(version + "/tutorials/" + item.metadata.slug + "/");
        for (const std::string& id : refs) {
            backlinks_by_id[id].push_back({item.metadata.title, tutorial_route});
        }
    }

    for (const tutorial& item : model.blog_posts) {
        std::set<std::string> refs;
        collect_referenced_entity_ids(item.content, refs);
        for (const auto& id : refs)
            backlinks_by_id[id].push_back({item.metadata.title, url("blog/" + item.metadata.slug + "/")});
    }

    // Give documentation pages the same sidebar of tutorials and C++/SQL
    // reference indexes through page.doc_nav.
    std::string doc_nav = "[{\"label\":\"Tutorials\",\"items\":[";
    {
        bool first_nav = true;
        for (const tutorial& item : model.tutorials) {
            if (!first_nav) {
                doc_nav += ",";
            }
            first_nav = false;
            doc_nav += "{\"label\":" + json_text(item.metadata.title) + ",\"url\":"
                + json_text(url(version + "/tutorials/" + item.metadata.slug + "/")) + "}";
        }
    }
    doc_nav += "]},{\"label\":\"Reference\",\"items\":[";
    doc_nav += "{\"label\":" + json_text("C++") + ",\"url\":"
        + json_text(url(version + "/reference/cpp/")) + "},";
    doc_nav += "{\"label\":" + json_text("SQL") + ",\"url\":"
        + json_text(url(version + "/reference/sql/")) + "}]}]";

    // Give  body headings URL anchors and collect the page contents list.
    // Add numeric suffixes when headings would otherwise share an, anchor.
    const auto heading_slug = [](std::string_view text) {
        std::string slug;
        bool previous_dash = false;
        for (const char raw : text) {
            const char lowered =
                (raw >= 'A' && raw <= 'Z') ? static_cast<char>(raw - 'A' + 'a') : raw;
            const bool ok = (lowered >= 'a' && lowered <= 'z')
                || (lowered >= '0' && lowered <= '9');
            if (ok) {
                slug.push_back(lowered);
                previous_dash = false;
            } else if (!previous_dash) {
                slug.push_back('-');
                previous_dash = true;
            }
        }
        while (!slug.empty() && slug.back() == '-') {
            slug.pop_back();
        }
        std::size_t lead = 0;
        while (lead < slug.size() && slug[lead] == '-') {
            ++lead;
        }
        slug.erase(0, lead);
        return slug.empty() ? std::string("section") : slug;
    };
    const auto node_plain_text = [](const content_node& node, auto&& self) -> std::string {
        std::string text = node.text;
        for (const content_node& child : node.children) {
            text += self(child, self);
        }
        return text;
    };
    const auto build_on_this_page = [&heading_slug, &node_plain_text](
                                        std::vector<content_node>& nodes)
        -> std::string {
        std::set<std::string> used;
        std::string toc = "[";
        bool first = true;
        const auto walk = [
                              &first,
                              &heading_slug,
                              &node_plain_text,
                              &toc,
                              &used](std::vector<content_node>& list,
                                     auto&& self_walk) -> void {
            for (content_node& node : list) {
                if (node.kind == content_node_kind::heading) {
                    const std::string text = node_plain_text(node, node_plain_text);
                    const std::string base = heading_slug(text);
                    std::string candidate = base;
                    int suffix = 2;
                    while (!used.insert(candidate).second) {
                        candidate = base + "-" + std::to_string(suffix++);
                    }
                    node.attributes["anchor"] = candidate;
                    int level = 2;
                    const auto level_attr = node.attributes.find("level");
                    if (level_attr != node.attributes.end()) {
                        level = std::atoi(level_attr->second.c_str());
                    }
                    if (level < 2) {
                        level = 2;
                    }
                    if (level > 3) {
                        level = 3;
                    }
                    if (!first) {
                        toc += ",";
                    }
                    first = false;
                    toc += "{\"level\":" + std::to_string(level) + ",\"text\":" + json_text(text)
                        + ",\"anchor\":" + json_text(candidate) + "}";
                }
                self_walk(node.children, self_walk);
            }
        };
        walk(nodes, walk);
        toc += "]";
        return toc;
    };

    // The explorer uses the author's chapter text and hardware tables directly.
    // Select content-tree sections, so revisions have one source of truth.
    std::vector<content_node> methodology;
    for (const tutorial& item : model.tutorials) {
        if (item.metadata.slug != "project_benchmarking") continue;
        enum class section { none, methods, machines, aligners, gpu };
        section active = section::none;
        for (const content_node& source : item.content) {
            if (source.kind == content_node_kind::heading) {
                const auto title = node_plain_text(source, node_plain_text);
                if (title == "Benchmarking methodology") active = section::methods;
                else if (title == "Complete raw results") {
                    active = section::machines;
                    content_node heading = source;
                    heading.children = {content_node{.kind = content_node_kind::text,
                        .text = "Local and AWS machine specifications"}};
                    methodology.push_back(std::move(heading));
                    continue;
                } else if (title == "Comparison with other aligners") active = section::aligners;
                else if (title == "Vulkan on a Vast.ai GPU instance") active = section::gpu;
                else if (title != "Reading comparisons and highlights.") active = section::none;
            }
            if (active == section::none) continue;
            if (active == section::aligners && source.kind == content_node_kind::raw_html
                && source.text.contains("tab-bench-external")) {
                active = section::none;
                continue;
            }
            methodology.push_back(source);
            if ((active == section::machines || active == section::gpu)
                && source.kind == content_node_kind::table) active = section::none;
        }
    }
    const auto rebase_methodology = [&url, &version](content_node& node, auto&& self) -> void {
        if (node.kind == content_node_kind::link && node.target.starts_with("../"))
            node.target = url(version + "/tutorials/" + node.target.substr(3));
        for (auto& child : node.children) self(child, self);
    };
    for (auto& node : methodology) rebase_methodology(node, rebase_methodology);
    build_on_this_page(methodology);
    const std::string methodology_html = render_content_html(methodology);

    boost::json::value landing_data = boost::json::parse(
        serialize_landing_examples(model.landing_examples));
    for (const auto& example : model.landing_examples) {
        std::vector<content_node> sql_nodes{content_node{
            .kind = content_node_kind::code_block, .text = example.visible_sql,
            .attributes = {{"language", "sql"}}}};
        decorate_code_entity_ranges(sql_nodes, route_by_id, qualified_by_id,
                                    tooltip_url, "landing " + example.slot);
        const auto ranges = sql_nodes.front().attributes.find("entity_ranges");
        landing_data.as_object().at(example.slot).as_object()["entity_ranges"] =
            boost::json::parse(json_text(
                ranges == sql_nodes.front().attributes.end() ? "[]" : ranges->second));
    }
    const std::string landing_examples_json = boost::json::serialize(landing_data);
    render("index.html", "landing.html.j2",
           "{\"title\":" + json_text(landing_meta("title", config.title))
               + ",\"description\":" + json_text(landing_meta("description", config.short_description))
               + ",\"canonical\":" + json_text(canonical("index.html"))
               + ",\"page_type\":" + json_text("landing")
               + ",\"eyebrow\":" + json_text("PostgreSQL-native")
               + ",\"headline\":" + json_text(config.title)
               + ",\"lede\":" + json_text(config.short_description)
               + ",\"install\":" + json_text("CREATE EXTENSION bioseq;") + "}",
           "examples", landing_examples_json);

    // Render the benchmark explorer page. Its JavaScript loads the report data
    render("benchmarks/index.html", "benchmarks.html.j2",
           "{\"title\":" + json_text(config.title + " Benchmarks")
               + ",\"description\":"
               + json_text("Explore BioseqDB benchmark configurations and measured CI reports")
               + ",\"canonical\":"
               + json_text(canonical("benchmarks/index.html"))
               + ",\"page_type\":" + json_text("benchmarks")
               + ",\"benchmark_methodology_html\":" + json_quote(methodology_html) + "}",
           "", "");

    // Contact pages share the navigation and assets with every other page.
    render("contact/index.html", "contact.html.j2",
           "{\"title\":" + json_text(contact_text("meta", "title", "Contact · BioseqDB"))
               + ",\"description\":" + json_text(contact_text(
                   "meta", "description", "Contact the BioseqDB maintainer."))
               + ",\"canonical\":" + json_text(canonical("contact/index.html"))
               + ",\"page_type\":" + json_text("contact")
               + ",\"thanks_canonical\":" + json_text(canonical("contact_thanks/index.html"))
               + "}", "", "");
    render("contact_thanks/index.html", "contact_thanks.html.j2",
           "{\"title\":" + json_text(contact_text("thanks", "page_title", "Thank you · BioseqDB"))
               + ",\"description\":" + json_text(contact_text(
                   "thanks", "description", "Your message has been received."))
               + ",\"canonical\":" + json_text(canonical("contact_thanks/index.html"))
               + ",\"page_type\":" + json_text("contact_thanks") + "}", "", "");

    // Render the root-level page used for missing URLs.
    render("error.html", "error.html.j2",
           "{\"title\":" + json_text("Page not found")
               + ",\"description\":" + json_text("The requested page does not exist.")
               + ",\"canonical\":" + json_text(canonical("error.html"))
               + ",\"page_type\":" + json_text("error")
               + ",\"doc_nav\":" + doc_nav + "}",
           "", "");

    // Render the documentation home for this version.
    render(version + "/index.html", "version_home.html.j2",
           "{\"title\":" + json_text(config.title + " " + version)
               + ",\"description\":" + json_text(config.short_description)
               + ",\"canonical\":" + json_text(canonical(version + "/index.html"))
               + ",\"coverage_url\":"
               + json_text(url(version + "/documentation-coverage/"))
               + ",\"breadcrumb\":"
               + breadcrumb_json({{"Home", url("")}, {version, url(version + "/")}})
               + ",\"doc_nav\":" + doc_nav + "}",
           "", "");

    // Render each tutorial with the results already saved by execution.
    // The sidebar provides the tutorial list, and each page gets previous and
    // next links using the saved orer.
    const auto adjacent_tutorial_title = [&model](
        const std::optional<std::string>& slug)
        -> std::optional<std::string> {
        if (!slug) {
            return std::nullopt;
        }
        const auto found = std::ranges::find_if(
            model.tutorials, [&slug](const tutorial& candidate) -> bool {
                return candidate.metadata.slug == *slug;
            });
        if (found == model.tutorials.end()) {
            throw std::runtime_error(
                "render: tutorial pager references unknown slug '" + *slug + "'");
        }
        return found->metadata.title;
    };
    std::string bibliography_html;
    for (const auto& item : model.tutorials) {
        if (item.metadata.slug == "bibliography") {
            bibliography_html = render_content_html(item.content);
            break;
        }
    }
    for (const tutorial& item : model.tutorials) {
        const std::string rel =
            version + "/tutorials/" + item.metadata.slug + "/index.html";
        std::vector<content_node> body_nodes = item.content;
        rewrite_entity_link_routes(
            body_nodes, identity_by_reference, route_by_id, tooltip_url);
        decorate_code_entity_ranges(body_nodes, route_by_id, qualified_by_id,
                                    tooltip_url, item.metadata.slug);
        // Resolve authored assets against this build's fingerprinted bundle.
        const auto rewrite_assets = [&site, &base_path](content_node& node, auto&& self) -> void {
            if (node.kind == content_node_kind::image || node.kind == content_node_kind::link) {
                if (node.target.starts_with("/static/")) {
                    node.target = site.assets_base_url + node.target.substr(8);
                } else if (node.target.starts_with("/v0.1/") || node.target.starts_with("/benchmarks/")) {
                    node.target = base_path + node.target.substr(1);
                }
            }
            for (auto& child : node.children) self(child, self);
        };
        for (auto& node : body_nodes) rewrite_assets(node, rewrite_assets);
        const std::string on_this_page = build_on_this_page(body_nodes);
        const std::string body = render_content_html(body_nodes);
        std::string page = "{\"title\":" + json_text(item.metadata.title)
            + ",\"description\":" + json_text(item.metadata.description)
            + ",\"canonical\":" + json_text(canonical(rel))
            + ",\"page_type\":" + json_text("tutorial")
            + ",\"breadcrumb\":" + breadcrumb_json({
                  {"Home", url("")},
                  {version, url(version + "/")},
                  {"Tutorials", first_tutorial_url},
                  {item.metadata.title, url(version + "/tutorials/" + item.metadata.slug + "/")}})
            + ",\"doc_nav\":" + doc_nav
            + ",\"on_this_page\":" + on_this_page
            + ",\"body_html\":" + json_quote(body);
        page += ",\"external_sources_html\":" + json_quote(
            item.metadata.slug == "bibliography" ? std::string{} :
            render_external_sources_html(body, bibliography_html, config.canonical_origin));
        page += ",\"previous_url\":";
        page += item.previous_slug
            ? json_text(url(version + "/tutorials/" + *item.previous_slug + "/"))
            : "null";
        const std::optional<std::string> previous_title =
            adjacent_tutorial_title(item.previous_slug);
        page += ",\"previous_title\":";
        page += previous_title ? json_text(*previous_title) : "null";
        page += ",\"next_url\":";
        page += item.next_slug
            ? json_text(url(version + "/tutorials/" + *item.next_slug + "/"))
            : "null";
        const std::optional<std::string> next_title =
            adjacent_tutorial_title(item.next_slug);
        page += ",\"next_title\":";
        page += next_title ? json_text(*next_title) : "null";
        page.push_back('}');
        render(rel, "tutorial.html.j2", page, "", "");
    }

    // Blog pages use the exact document renderer and SQL/entity decoration as
    // tutorials, while retaining their own permanent, unversioned routes.
    const auto blog_asset = [&site](std::string value) {
        if (value.empty()) return value;
        if (value.starts_with("/static/")) value.erase(0, 8);
        else if (value.starts_with("static/")) value.erase(0, 7);
        if (value.starts_with('/') || value.contains("..") || value.contains(':'))
            throw std::runtime_error("blog image must use a bundled asset");
        return site.assets_base_url + value;
    };
    const auto blog_copy_text = [&blog_copy](std::string_view key, std::string fallback) {
        const auto group = blog_copy.find("index");
        if (group == blog_copy.end()) return fallback;
        const auto value = group->second.find(key);
        return value == group->second.end() ? fallback : value->second;
    };
    const auto social_text = [](const landing_content& copy, std::string_view section,
                                std::string_view key) -> std::string {
        const auto group = copy.find(section);
        if (group == copy.end()) return {};
        const auto value = group->second.find(key);
        return value == group->second.end() ? std::string{} : value->second;
    };
    boost::json::array blog_posts;
    // Only content-addressed resources are immutable. Stable URLs (HTML,
    // search indexes, thumbnails and logo aliases) must be able to refresh.
    std::string markdown_headers = url("assets/*") + "\n"
        "  Cache-Control: public, max-age=172800, immutable\n\n"
        + tooltip_url + "\n"
        "  Cache-Control: public, max-age=172800, immutable\n\n";
    const std::array<std::string_view, 12> month_names{
        "January", "February", "March", "April", "May", "June",
        "July", "August", "September", "October", "November", "December"};
    for (const tutorial& item : model.blog_posts) {
        const std::string relative = "blog/" + item.metadata.slug + "/";
        const std::string rel = relative + "index.html";
        const auto escaped = [](std::string_view text) { return boost::json::parse(json_text(text)); };
        boost::json::object page;
        page["title"] = escaped(item.metadata.title);
        page["description"] = escaped(item.metadata.description);
        page["canonical"] = escaped(canonical(rel));
        page["url"] = escaped(url(relative));
        page["page_type"] = "blog";
        page["category"] = item.metadata.category;
        page["search_text"] = escaped(item.search_text);
        page["date"] = item.metadata.publication_date;
        const auto& date = item.metadata.publication_date;
        page["date_label"] = std::to_string(std::stoi(date.substr(8, 2))) + " "
            + std::string(month_names.at(std::stoi(date.substr(5, 2)) - 1)) + " " + date.substr(0, 4);
        page["reading_minutes"] = item.metadata.reading_minutes;
        page["author"] = escaped(item.metadata.author);
        page["structured_title"] = item.metadata.title;
        page["structured_description"] = item.metadata.description;
        page["structured_author"] = item.metadata.author;
        boost::json::array structured_keywords;
        for (const auto& keyword : item.metadata.keywords) structured_keywords.push_back(boost::json::value(keyword));
        page["structured_keywords"] = std::move(structured_keywords);
        page["cover_url"] = escaped(blog_asset(item.metadata.cover));
        page["image_alt"] = escaped(item.metadata.image_alt);
        page["social_image_url"] = item.metadata.social_image
            ? escaped(config.canonical_origin + blog_asset(*item.metadata.social_image))
            : escaped(site.social_image_url);
        page["markdown_url"] = escaped(url(relative + "index.md"));
        boost::json::array social_links;
        for (const std::string network : {"medium", "linkedin"}) {
            const auto post_url = social_text(blog_copy, network + "_posts", item.metadata.slug);
            const auto destination = post_url.empty()
                ? social_text(landing_copy, "social", network + "_url") : post_url;
            if (destination.empty()) continue;
            if (!destination.starts_with("https://") || destination.size() <= 8
                || std::ranges::any_of(destination, [](unsigned char value) {
                    return value <= 32 || value == 127;
                })) {
                throw std::runtime_error("blog social link must be an HTTPS URL: "
                    + network + " for " + item.metadata.slug);
            }
            const auto label = social_text(blog_copy, "social", network + "_label");
            const auto tooltip = social_text(blog_copy, "social",
                network + (post_url.empty() ? "_profile_tooltip" : "_post_tooltip"));
            if (label.empty() || tooltip.empty()) {
                throw std::runtime_error("blog social link needs a label and tooltip: " + network);
            }
            social_links.push_back(boost::json::object{
                {"network", network}, {"url", escaped(destination)},
                {"label", escaped(label)}, {"tooltip", escaped(tooltip)}});
        }
        page["social_links"] = std::move(social_links);
        page["breadcrumb"] = boost::json::parse(breadcrumb_json({
            {"Home", url("")}, {"Blog", url("blog/")}, {item.metadata.title, url(relative)}}));
        boost::json::array keywords;
        std::string keywords_text;
        for (const auto& keyword : item.metadata.keywords) {
            keywords.push_back(escaped(keyword));
            if (!keywords_text.empty()) keywords_text += ", ";
            keywords_text += keyword;
        }
        page["keywords"] = std::move(keywords);
        page["keywords_text"] = escaped(keywords_text);
        blog_posts.push_back(page);

        auto body_nodes = item.content;
        rewrite_entity_link_routes(body_nodes, identity_by_reference, route_by_id, tooltip_url);
        decorate_code_entity_ranges(body_nodes, route_by_id, qualified_by_id, tooltip_url,
                                    "blog " + item.metadata.slug);
        const auto rewrite_links = [&site, &base_path](content_node& node, auto&& self) -> void {
            if (node.kind == content_node_kind::image || node.kind == content_node_kind::link) {
                if (node.target.starts_with("/static/"))
                    node.target = site.assets_base_url + node.target.substr(8);
                else if (node.target.starts_with('/') && !node.target.starts_with("//"))
                    node.target = base_path + node.target.substr(1);
            }
            for (auto& child : node.children) self(child, self);
        };
        for (auto& node : body_nodes) rewrite_links(node, rewrite_links);
        page["on_this_page"] = boost::json::parse(build_on_this_page(body_nodes));
        page["body_html"] = render_content_html(body_nodes);
        render(rel, "blog.html.j2", boost::json::serialize(page), "", "");

        const auto markdown = rewrite_markdown_destinations(item.public_markdown,
            [&site, &base_path](std::string_view destination) -> std::string {
                if (destination.starts_with("/static/"))
                    return site.assets_base_url + std::string(destination.substr(8));
                if (destination.starts_with('/') && !destination.starts_with("//"))
                    return base_path + std::string(destination.substr(1));
                return std::string(destination);
            });
        writer.emit(route(relative + "index.md"), markdown);
        markdown_headers += url(relative + "index.md") + "\n"
            "  Content-Type: text/plain; charset=utf-8\n"
            "  Content-Disposition: inline\n"
            "  Link: <" + canonical(rel) + ">; rel=\"canonical\"\n\n";
    }
    for (const auto& [destination, source] : config.stable_assets) {
        markdown_headers += url(destination) + "\n"
            "  Cache-Control: public, max-age=300, must-revalidate\n\n";
    }
    writer.emit(route("_headers"), markdown_headers);
    boost::json::object blog_index_page;
    blog_index_page["title"] = boost::json::parse(json_text(blog_copy_text("title", "BioseqDB blog")));
    blog_index_page["description"] = boost::json::parse(json_text(blog_copy_text(
        "description", "BioseqDB notes, examples and project updates.")));
    blog_index_page["canonical"] = canonical("blog/index.html");
    blog_index_page["page_type"] = "blog_index";
    blog_index_page["posts"] = std::move(blog_posts);
    render("blog/index.html", "blog_index.html.j2", boost::json::serialize(blog_index_page), "", "");

    // Questions remain visible without JavaScript. The same authored answers
    // feed the page, anchored search records, and FAQ structured data.
    if (!questions.groups.empty()) {
        boost::json::object page;
        for (const auto& [key, value] : questions.copy)
            page[key] = boost::json::parse(json_text(value));
        page["canonical"] = canonical("qa/index.html");
        page["page_type"] = "qa";
        page["structured_title"] = questions.copy.at("title");
        page["structured_description"] = questions.copy.at("description");
        page["structured_author"] = questions.copy.at("author");
        page["category"] = "Questions and answers";
        page["breadcrumb"] = boost::json::parse(breadcrumb_json({
            {"Home", url("")}, {"Q&A", url("qa/")}}));
        boost::json::array groups;
        boost::json::array structured_questions;
        for (const auto& group : questions.groups) {
            boost::json::object group_view;
            group_view["id"] = group.id;
            group_view["title"] = boost::json::parse(json_text(group.title));
            boost::json::array entries;
            for (const auto& question : group.questions) {
                auto answer_nodes = parse_markdown_content(question.answer).children;
                std::string answer_text;
                for (const auto& node : answer_nodes) {
                    if (!answer_text.empty()) answer_text += ' ';
                    answer_text += node_plain_text(node, node_plain_text);
                }
                const auto rebase = [&url](content_node& node, auto&& self) -> void {
                    if ((node.kind == content_node_kind::link || node.kind == content_node_kind::image)
                        && node.target.starts_with('/') && !node.target.starts_with("//"))
                        node.target = url(node.target.substr(1));
                    for (auto& child : node.children) self(child, self);
                };
                for (auto& node : answer_nodes) rebase(node, rebase);
                decorate_code_entity_ranges(answer_nodes, route_by_id, qualified_by_id,
                                            tooltip_url, "qa:" + question.id);
                boost::json::object entry;
                entry["id"] = question.id;
                entry["question"] = boost::json::parse(json_text(question.question));
                entry["answer_html"] = render_content_html(answer_nodes);
                entries.push_back(std::move(entry));
                structured_questions.push_back(boost::json::object{
                    {"question", question.question}, {"answer", answer_text},
                    {"url", canonical("qa/index.html") + "#" + question.id}});
            }
            group_view["questions"] = std::move(entries);
            groups.push_back(std::move(group_view));
        }
        page["groups"] = std::move(groups);
        page["structured_questions"] = std::move(structured_questions);
        render("qa/index.html", "qa.html.j2", boost::json::serialize(page), "", "");
    }

    // Render the reference home, C++ and SQL indexes, and  entity pages.
    render(version + "/reference/index.html", "reference_index.html.j2",
           "{\"title\":" + json_text("API Reference")
               + ",\"description\":" + json_text("Bioseq C++ and SQL API reference")
               + ",\"canonical\":"
               + json_text(canonical(version + "/reference/index.html"))
               + ",\"cpp_url\":" + json_text(url(version + "/reference/cpp/"))
               + ",\"sql_url\":" + json_text(url(version + "/reference/sql/"))
               + ",\"breadcrumb\":" + breadcrumb_json({{"Home", url("")},
                     {version, url(version + "/")}, {"Reference", url(version + "/reference/")}})
               + ",\"doc_nav\":" + doc_nav + "}",
           "", "");
    const auto render_domain = [
                                   &backlinks_by_id,
                                   &bibliography_html,
                                   &breadcrumb_json,
                                   &canonical,
                                   &config,
                                   &doc_nav,
                                   &identity_by_reference,
                                   &qualified_by_id,
                                   &model,
                                   &node_plain_text,
                                   &render,
                                   &revision,
                                   &route_by_id,
                                   &tooltip_url,
                                   &url,
                                   &version](
                                   std::string_view domain,
                                   std::string_view label,
                                   const std::vector<reference_entity>& entities)
        -> void {
        const std::string index_rel =
            version + "/reference/" + std::string(domain) + "/index.html";
        render(index_rel, "entity_index.html.j2",
               "{\"title\":" + json_text(std::string(label) + " API")
                   + ",\"description\":"
                   + json_text(std::string(label) + " API reference")
                   + ",\"canonical\":" + json_text(canonical(index_rel))
                   + ",\"breadcrumb\":" + breadcrumb_json({{"Home", url("")},
                         {version, url(version + "/")}, {"Reference", url(version + "/reference/")},
                         {std::string(label) + " API",
                          url(version + "/reference/" + std::string(domain) + "/")}})
                   + ",\"doc_nav\":" + doc_nav + "}",
               "entities", serialize_reference_entity_array(entities));
        for (const reference_entity& entity : entities) {
            const std::string entity_relative_route =
                reference_entity_relative_route(version, entity);
            const std::string rel = entity_relative_route + "index.html";
            std::string page = "{\"title\":" + json_text(entity.qualified_name)
                + ",\"description\":"
                + json_text(entity.summary_text.empty()
                                 ? entity.qualified_name : entity.summary_text)
                + ",\"canonical\":" + json_text(canonical(rel))
                + ",\"page_type\":" + json_text("entity")
                + ",\"breadcrumb\":" + breadcrumb_json({
                      {"Home", url("")},
                      {version, url(version + "/")},
                      {"Reference", url(version + "/reference/")},
                      {std::string(label),
                       url(version + "/reference/" + std::string(domain) + "/")},
                      {entity.qualified_name, url(entity_relative_route)}});
            page += ",\"doc_nav\":" + doc_nav;
            // Add contents links only for sections present on this entity page.
            // Anchor names must match entity.html.j2.
            const bool has_backlinks =
                backlinks_by_id.find(entity.canonical_id) != backlinks_by_id.end();
            std::string entity_toc = "[";
            bool toc_first = true;
            const auto add_toc = [&entity_toc, &toc_first](
                                     std::string_view text,
                                     std::string_view anchor) -> void {
                if (!toc_first) {
                    entity_toc.push_back(',');
                }
                toc_first = false;
                entity_toc += "{\"level\":2,\"text\":" + json_text(std::string(text))
                    + ",\"anchor\":" + json_text(std::string(anchor)) + "}";
            };
            const bool is_setting = entity.canonical_id.starts_with("sql:guc:");
            if (is_setting) add_toc("Setting", "setting");
            const auto detail_toc = [&](const content_node& node, auto&& self) -> void {
                if (node.kind == content_node_kind::heading) {
                    const auto anchor = node.attributes.find("anchor");
                    if (anchor != node.attributes.end()) add_toc(node_plain_text(node, node_plain_text), anchor->second);
                }
                for (const auto& child : node.children) self(child, self);
            };
            for (const auto& node : entity.detail_content) detail_toc(node, detail_toc);
            if (!entity.parameters.empty()) {
                add_toc("Parameters", "parameters");
            }
            if (!entity.fields.empty()) add_toc("Fields", "fields");
            if (!entity.enum_values.empty()) add_toc("Values", "values");
            const bool has_visible_attributes = std::ranges::any_of(
                entity.attributes, [](const auto& attribute) {
                    return reference_attribute_visible(attribute.first);
                });
            if (has_visible_attributes && !is_setting) {
                add_toc(entity.domain == entity_domain::sql ? "Attributes" : "Qualifiers",
                        "attributes");
            }
            if (!entity.overload_members.empty()) {
                add_toc("Overloads", "overloads");
            }
            if (!entity.child_members.empty()) {
                add_toc("Members", "members");
            }
            if (has_backlinks) {
                add_toc("Referenced by", "referenced-by");
            }
            entity_toc.push_back(']');
            page += ",\"on_this_page\":" + entity_toc;
            // Resolve Doxygen references on a copy of the documentation before
            // rendering it. Use the same routes and tooltip data as tutorial links.
            std::vector<content_node> detail_nodes = entity.detail_content;
            rewrite_entity_link_routes(
                detail_nodes, identity_by_reference, route_by_id, tooltip_url);
            decorate_code_entity_ranges(detail_nodes, route_by_id, qualified_by_id,
                                       tooltip_url, entity.canonical_id);
            page += ",\"detail_html\":";
            const auto detail_html = render_content_html(detail_nodes);
            page += detail_nodes.empty()
                ? std::string("null")
                : json_quote(detail_html);
            page += ",\"external_sources_html\":" + json_quote(
                render_external_sources_html(detail_html, bibliography_html, config.canonical_origin));
            // Build declaration links from the stored source path, revision, and
            // line using the configured URL template.
            if (entity.declaration) {
                const std::string source_path =
                    entity.declaration->path.generic_string();
                const std::string source_url = render_source_url(
                    config.source_url_template, revision, source_path,
                    entity.declaration->first_line);
                const std::string source_label =
                    entity.module_name ? *entity.module_name : source_path;
                page += ",\"source_url\":" + json_text(source_url)
                    + ",\"source_label\":" + json_text(source_label);
            } else {
                page += ",\"source_url\":null,\"source_label\":null";
            }
            // Show the extension version, installed schema, and PostgreSQL version.
            if (entity.domain == entity_domain::sql) {
                const auto dot = entity.qualified_name.find('.');
                const std::string schema = dot == std::string::npos
                    ? std::string("public") : entity.qualified_name.substr(0, dot);
                std::string provenance =
                    "Bioseq extension " + model.extension_version + " \xC2\xB7 schema " + schema;
                if (!model.postgres_server_version.empty()) {
                    provenance += " \xC2\xB7 PostgreSQL " + model.postgres_server_version;
                }
                page += ",\"provenance\":" + json_text(provenance);
            } else {
                page += ",\"provenance\":null";
            }
            // Tutorial backlinks: tutorials that reference this entity.
            page += ",\"backlinks\":[";
            if (const auto found = backlinks_by_id.find(entity.canonical_id);
                found != backlinks_by_id.end()) {
                bool first = true;
                for (const auto& [backlink_title, backlink_route] : found->second) {
                    if (!first) {
                        page += ",";
                    }
                    first = false;
                    page += "{\"title\":" + json_text(backlink_title)
                        + ",\"url\":" + json_text(backlink_route) + "}";
                }
            }
            page += "]}";
            render(rel, "entity.html.j2", page, "entity", serialize_reference_entity(entity));
        }
    };
    render_domain("cpp", "C++", cpp_entities);
    render_domain("sql", "SQL", sql_entities);

    // Render the coverage page and JSON report from the saved counts.
    // Include missing summaries, parameter and return  documentation, and
    // totals for each module or SQL group.
    const auto id_array = [](const std::vector<std::string>& ids)
        -> std::string {
        std::string out = "[";
        for (std::size_t index = 0; index < ids.size(); ++index) {
            if (index > 0) {
                out.push_back(',');
            }
            out += json_text(ids[index]);
        }
        out.push_back(']');
        return out;
    };
    std::string groups_json = "[";
    for (std::size_t index = 0; index < model.coverage_groups.size(); ++index) {
        const coverage_group_view& group = model.coverage_groups[index];
        if (index > 0) {
            groups_json.push_back(',');
        }
        groups_json += "{\"group\":" + json_text(group.group)
            + ",\"public_total\":" + std::to_string(group.public_total)
            + ",\"documented_total\":" + std::to_string(group.documented_total) + "}";
    }
    groups_json.push_back(']');
    render(version + "/documentation-coverage/index.html", "coverage.html.j2",
           "{\"title\":" + json_text("Documentation coverage")
               + ",\"description\":" + json_text("Public API documentation coverage")
               + ",\"canonical\":"
               + json_text(canonical(version + "/documentation-coverage/index.html"))
               + ",\"public_total\":" + std::to_string(model.coverage_public_total)
               + ",\"documented_total\":" + std::to_string(model.coverage_documented_total)
               + ",\"groups\":" + groups_json
               + ",\"missing\":" + id_array(model.coverage_missing)
               + ",\"missing_parameters\":" + id_array(model.coverage_missing_parameters)
               + ",\"missing_returns\":" + id_array(model.coverage_missing_returns)
               + ",\"fatal_missing\":" + id_array(model.coverage_fatal_missing)
               + ",\"doc_nav\":" + doc_nav + "}",
           "", "");
    writer.emit(route(version + "/documentation-coverage.json"),
                serialize_coverage(model.coverage_public_total,
                                   model.coverage_documented_total,
                                   model.coverage_missing, model.coverage_missing_parameters,
                                   model.coverage_missing_returns, model.coverage_fatal_missing,
                                   model.coverage_groups));

    // Write the version selector data and versions page.
    const std::vector<std::string> version_labels{version};
    writer.emit(route("versions.json"),
                serialize_release_catalog(version_labels, base_path));
    render("versions/index.html", "versions.html.j2",
           "{\"title\":" + json_text("Versions")
               + ",\"description\":" + json_text("Documented Bioseq versions")
               + ",\"canonical\":" + json_text(canonical("versions/index.html"))
               + ",\"doc_nav\":" + doc_nav + "}",
           "versions",
           "[{\"route\":" + json_text(url(version + "/")) + ",\"label\":"
               + json_text(version) + "}]");
    // Build search records for entities, tutorials, and index pages.
    std::vector<search_index_entry> search_entries;
    search_entries.reserve(all_entities.size() + model.tutorials.size() + 5);
    for (const reference_entity& entity : all_entities) {
        search_entries.push_back(build_entity_search_entry(entity));
    }
    for (const tutorial& item : model.tutorials) {
        search_index_entry entry;
        entry.id = "tutorial:" + item.metadata.slug;
        entry.group = "Tutorials";
        entry.kind = "tutorial";
        entry.title = item.metadata.title;
        entry.route = url(version + "/tutorials/" + item.metadata.slug + "/");
        entry.summary = item.metadata.description;
        std::string names = item.metadata.title;
        for (const std::string& keyword : item.metadata.keywords) {
            names += ' ';
            names += keyword;
        }
        entry.name_source = std::move(names);
        entry.text_source = item.metadata.description + " " + collect_heading_text(item.content);
        search_entries.push_back(std::move(entry));
    }
    for (const tutorial& item : model.blog_posts) {
        search_index_entry entry;
        entry.id = "blog:" + item.metadata.slug;
        entry.group = "Blog";
        entry.kind = "blog";
        entry.title = item.metadata.title;
        entry.route = url("blog/" + item.metadata.slug + "/");
        entry.summary = item.metadata.description;
        entry.name_source = item.metadata.title;
        entry.name_source += " " + item.metadata.category;
        for (const auto& keyword : item.metadata.keywords) entry.name_source += " " + keyword;
        entry.text_source = item.metadata.description + " " + item.search_text;
        search_entries.push_back(std::move(entry));
    }
    for (const auto& group : questions.groups) {
        for (const auto& question : group.questions) {
            search_index_entry entry;
            entry.id = "question:" + question.id;
            entry.group = "Questions";
            entry.kind = "question";
            entry.title = question.question;
            entry.route = url("qa/#" + question.id);
            entry.name_source = question.question + " " + group.title;
            for (const auto& node : parse_markdown_content(question.answer).children) {
                if (!entry.summary.empty()) entry.summary += ' ';
                entry.summary += node_plain_text(node, node_plain_text);
            }
            entry.text_source = entry.summary;
            search_entries.push_back(std::move(entry));
        }
    }
    const auto add_page_entry = [&search_entries, &url](
                                    std::string id,
                                    std::string title,
                                    std::string_view rel,
                                    std::string summary) -> void {
        search_index_entry entry;
        entry.id = std::move(id);
        entry.group = "Pages";
        entry.kind = "page";
        entry.title = std::move(title);
        entry.route = url(rel);
        entry.name_source = entry.title;
        entry.text_source = summary;
        entry.summary = std::move(summary);
        search_entries.push_back(std::move(entry));
    };
    add_page_entry("page:" + version + ":home", config.title + " " + version, version + "/",
                   config.short_description);
    add_page_entry("page:blog", "Blog", "blog/", "BioseqDB notes, examples and project updates.");
    add_page_entry("page:contact", contact_text("page", "title", "Contact me"), "contact/",
                   contact_text("meta", "description", "Contact the BioseqDB maintainer."));
    add_page_entry("page:benchmarks", "Benchmarks", "benchmarks/",
                   "Interactive BioseqDB benchmark configurations and measured CI reports");
    add_page_entry("page:" + version + ":reference", "API Reference", version + "/reference/",
                   "Bioseq C++ and SQL API reference");
    add_page_entry("page:" + version + ":reference-cpp", "C++ Reference",
                   version + "/reference/cpp/", "Bioseq C++ API reference");
    add_page_entry("page:" + version + ":reference-sql", "SQL Reference",
                   version + "/reference/sql/", "Bioseq SQL API reference");
    writer.emit(route(version + "/search-index.json"),
                serialize_search_index(std::move(search_entries)));
    // Save tooltip data under its SHA-256 name, matching the URLs in entity links.
    writer.emit(route(version + "/entities." + bioseq::common::sha256_hex(tooltip_json)
                          + ".json"),
                tooltip_json);
    // Save catalog entries omitted from public documentation so reviewers
    // can see which captured extension objects have no reference page
    if (!model.omitted_sql_entities.empty()) {
        writer.emit(route(version + "/catalog-inventory.json"),
                    serialize_sql_entities(model.omitted_sql_entities));
    }

    // Write the crawler rules, suggested HTTP security headers, and sitemap.
    writer.emit(route("robots.txt"),
                "User-agent: *\nAllow: /\nSitemap: " + config.canonical_origin
                    + url("sitemap.xml") + "\n");
    writer.emit(route("security-headers.txt"),
                "Content-Security-Policy: default-src 'self'; script-src 'self' https://www.googletagmanager.com "
                + theme_csp_hash + "; script-src-attr 'none'; "
                "style-src 'self'; font-src 'self'; img-src 'self' data: https://img.shields.io https://www.googletagmanager.com https://*.google-analytics.com; "
                "media-src 'self'; connect-src 'self' https://raw.githubusercontent.com "
                "https://api.github.com https://www.googletagmanager.com https://*.google-analytics.com https://*.google.com https://formserve.io; object-src 'none'; "
                "base-uri 'none'; frame-src 'none'; worker-src 'self'; manifest-src 'self'; "
                "form-action 'self' https://formserve.io; frame-ancestors 'none'\n"
                "X-Content-Type-Options: nosniff\n"
                "Referrer-Policy: strict-origin-when-cross-origin\n"
                "Permissions-Policy: geolocation=(), microphone=(), camera=()\n");
    std::string sitemap =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n";
    for (const auto& [manifest_route, digest] : writer.manifest) {
        if (!manifest_route.ends_with("index.html")
            || manifest_route == "/contact_thanks/index.html") {
            continue; // Confirmation and error pages are deliberately noindex.
        }
        std::string rel = manifest_route.substr(1);
        sitemap += "  <url><loc>" + canonical(rel) + "</loc></url>\n";
    }
    sitemap += "</urlset>\n";
    writer.emit(route("sitemap.xml"), sitemap);

    // Stable aliases are additional publications of the same bytes; internal
    // URLs continue using the content-addressed bundle. Resolve after pages so
    // a configuration mistake cannot replace generated content.
    for (const auto& [destination, asset] : resolve_stable_assets(config.stable_assets, assets, writer.manifest))
        writer.emit(route(destination), asset->content);

    // Write the manifest after all outputs, then check that its routes match
    // the files in staging. PostgreSQL version data comes from the saved model.
    {
        std::string manifest_json;
        manifest_json += "{\n  \"schema\": \"bioseq.website.site_manifest\",\n";
        manifest_json += "  \"schema_revision\": 1,\n";
        manifest_json += "  \"hash_algorithm\": \"sha256\",\n";
        manifest_json += "  \"version\": \"" + version + "\",\n";
        manifest_json += "  \"source_revision\": \"" + revision + "\",\n";
        manifest_json += "  \"base_path\": \"" + base_path + "\",\n";
        manifest_json += "  \"generator_signature\": \"" + model.plan_signature + "\",\n";
        manifest_json += "  \"parser_identity\": \""
            + model.parser_identity + "\",\n";
        manifest_json += "  \"parser_revision\": "
            + std::to_string(model.parser_revision) + ",\n";
        manifest_json += "  \"binding_schema\": \""
            + model.binding_schema + "\",\n";
        manifest_json += "  \"binding_schema_revision\": "
            + std::to_string(model.binding_schema_revision) + ",\n";
        manifest_json += "  \"asset_bundle_sha256\": \"" + bundle_sha256 + "\",\n";
        manifest_json += "  \"postgres_server_version\": "
            + manifest_optional(model.postgres_server_version) + ",\n";
        manifest_json += "  \"extension_version\": "
            + manifest_optional(model.extension_version) + ",\n";
        manifest_json += "  \"staged_extension_sha256\": "
            + manifest_optional(model.staged_extension_sha256) + ",\n";
        manifest_json += "  \"inputs\": [\n";
        for (std::size_t index = 0; index < model.inputs.size(); ++index) {
            manifest_json += "    {\"path\": \"" + model.inputs[index].path
                + "\", \"content_hash\": \"" + model.inputs[index].content_hash + "\"}";
            manifest_json += (index + 1 == model.inputs.size()) ? "\n" : ",\n";
        }
        manifest_json += "  ],\n";
        manifest_json += "  \"outputs\": {\n";
        std::size_t written = 0;
        for (const auto& [manifest_route, digest] : writer.manifest) {
            manifest_json += "    \"" + manifest_route + "\": \"" + digest + "\"";
            manifest_json += (++written == writer.manifest.size()) ? "\n" : ",\n";
        }
        manifest_json += "  }\n}\n";
        const fs::path manifest_path = staging / "site-manifest.json";
        std::ofstream manifest_out(manifest_path, std::ios::binary | std::ios::trunc);
        manifest_out.write(manifest_json.data(),
                           static_cast<std::streamsize>(manifest_json.size()));
        if (!manifest_out) {
            throw std::runtime_error("cannot write site-manifest.json");
        }
    }

    std::set<std::string> present;
    collect_site_files(staging, present);
    present.erase("/site-manifest.json");
    std::set<std::string> declared;
    for (const auto& [manifest_route, digest] : writer.manifest) {
        declared.insert(manifest_route);
    }
    if (present != declared) {
        throw std::runtime_error(
            "site manifest set mismatch: manifest declares " + std::to_string(declared.size())
            + " outputs but the staged tree has " + std::to_string(present.size()));
    }
    return bioseq::common::sha256_hex(read_artifact_bytes(staging / "site-manifest.json"));
}

}  // namespace

/// @brief Runs website publish, to reilace the output  with a staged site.
///
/// With --staging, checks the owner marker, requested validation stamps,
/// and manifest file list before swapping directories. Otherwise, builds and
/// renders the model first. Records a success marker after publication.
/// @param args Options after publish.
/// @return 0 on success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_publish_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({
        "source", "version", "site-config", "cpp-entities", "sql-entities",
        "templates", "static", "output", "base-path", "source-revision",
        "conninfo", "admin-conninfo", "socket-dir", "port", "template-db",
        "staged-extension-sha256", "cpp-signature", "sql-manifest", "sql-documentation-manifest",
        "staging", "model-validation-stamp", "html-validation-stamp", "report-dir"});
    static constexpr auto flag_keys = std::to_array<std::string_view>({"include-drafts", "isolated"});
    static constexpr auto repeatable_value_keys = std::to_array<std::string_view>({
        "sql-manifest"});
    const auto options = parse_options(
        args, value_keys, flag_keys, repeatable_value_keys);
    const fs::path output = require_option(options, "output");
    const bool swap_only = options.contains("staging");
    if (!swap_only) {
        (void)require_option(options, "sql-manifest");
    }
    const fs::path staging = swap_only
        ? fs::path(require_option(options, "staging"))
        : fs::path(output.string() + ".staging");

    // Recover an interrupted publish from its prepublish descriptor. If the
    // output manifest hash and file list match, write the success marker.
    // Otherwise, remove the descriptor and fail  so the build recreates staging.
    {
        const fs::path prepublish = output.string() + ".prepublish.json";
        std::error_code probe;
        if (fs::exists(prepublish, probe) && !probe) {
            const std::string descriptor = read_artifact_bytes(prepublish);
            const std::string expected =
                manifest_string_field(
                    descriptor,
                    "new_manifest_sha256",
                    "bioseq.website.prepublish",
                    1);
            const fs::path site_manifest = output / "site-manifest.json";
            bool output_valid = false;
            if (!expected.empty() && fs::exists(site_manifest, probe) && !probe) {
                const std::string manifest_text = read_artifact_bytes(site_manifest);
                if (bioseq::common::sha256_hex(manifest_text) == expected) {
                    std::set<std::string> present;
                    collect_site_files(output, present);
                    present.erase("/site-manifest.json");
                    std::set<std::string> declared;
                    for (const std::string& declared_route :
                         manifest_output_routes(
                             manifest_text,
                             "bioseq.website.site_manifest",
                             1)) {
                        declared.insert(declared_route);
                    }
                    output_valid = (present == declared);
                }
            }
            std::error_code cleanup;
            if (output_valid) {
                publish_artifact_if_changed(output.string() + ".success", expected + "\n");
                fsync_file(output.string() + ".success");
                fs::remove(prepublish, cleanup);
                fsync_directory(parent_or_dot(output));
                std::println("recovered interrupted publish for {}", output.string());
                return 0;
            }
            fs::remove(prepublish, cleanup);
            fsync_directory(parent_or_dot(output));
            throw std::runtime_error("crash recovery: output does not match the "
                "prepublish descriptor; cleared transient state, rebuild required");
        }
    }

    // Collect requested validation stamps. Publishing existing staging checks
    // that each stamp file is present before the directory swap.
    std::vector<std::string> validation_stamps;
    if (const auto found = options.find("model-validation-stamp"); found != options.end()) {
        validation_stamps.push_back(found->second);
    }
    if (const auto found = options.find("html-validation-stamp"); found != options.end()) {
        validation_stamps.push_back(found->second);
    }

    std::string manifest_hash;
    if (swap_only) {
        // Check the staging owner marker, requested stamp files, and manifest
        // file list before publication. The site has already been rendered.
        const fs::path staging_manifest = staging / "site-manifest.json";
        if (!fs::exists(staging_manifest)) {
            throw std::runtime_error("publish: staging has no site-manifest.json: "
                                     + staging.string());
        }
        const std::string manifest_text = read_artifact_bytes(staging_manifest);
        manifest_hash = bioseq::common::sha256_hex(manifest_text);
        const fs::path owner_path = staging.string() + ".owner.json";
        if (!fs::exists(owner_path)) {
            throw std::runtime_error("publish: staging is not owner-marked: "
                                     + owner_path.string());
        }
        require_artifact_schema(
            read_artifact_bytes(owner_path),
            "bioseq.website.owner",
            1,
            "staging owner marker");
        for (const std::string& stamp : validation_stamps) {
            if (!fs::exists(fs::path(stamp))) {
                throw std::runtime_error(
                    "publish: required validation stamp missing (validator failed "
                    "or did not run before publish): " + stamp);
            }
        }
        std::set<std::string> present;
        collect_site_files(staging, present);
        present.erase("/site-manifest.json");
        std::set<std::string> declared;
        for (const std::string& declared_route : manifest_output_routes(
                 manifest_text, "bioseq.website.site_manifest", 1)) {
            declared.insert(declared_route);
        }
        if (present != declared) {
            throw std::runtime_error("publish: staging manifest set mismatch (declares "
                + std::to_string(declared.size()) + ", tree has "
                + std::to_string(present.size()) + ")");
        }
    } else {
        // The combined command builds and renders before publishing. Separate
        // execute, render, and publish --staging commands allow validation between
        // reddering and the directory swap
        const std::string site_yaml = read_artifact_bytes(require_option(options, "site-config"));
        const site_configuration config =
            parse_site_configuration(site_yaml, require_option(options, "site-config"));
        std::string base_path = config.default_base_path;
        if (const auto found = options.find("base-path"); found != options.end()) {
            base_path = found->second;
        }
        std::string revision;
        if (const auto found = options.find("source-revision"); found != options.end()) {
            revision = found->second;
        }
        const fs::path source = require_option(options, "source");
        const std::string version_string = require_option(options, "version");
        const fs::path site_config_path = require_option(options, "site-config");
        const site_model model = create_site_model(
            source, version_string, config, site_yaml,
            content_relative_path(site_config_path, source), base_path, revision,
            options.contains("include-drafts"), options);
        manifest_hash = render_site_model(model, config, base_path, revision,
            fs::path(require_option(options, "templates")),
            fs::path(require_option(options, "static")), staging,
            load_landing_content(site_config_path),
            load_landing_content(site_config_path, "contact.yaml"),
            load_landing_content(site_config_path, "blog.yaml"),
            load_qa_content(site_config_path));
    }

    // Write a prepublish descriptor before swapping directories so  the next
    // run can check an interrupted publish. After the swap, write the success
    // marker and remove the descriptor and owner marker.
    const std::string nonce =
        std::to_string(::getpid()) + "-" + manifest_hash.substr(0, 16);
    const fs::path owner_marker = staging.string() + ".owner.json";
    write_private_artifact(owner_marker,
        "{\"schema\":\"bioseq.website.owner\",\"schema_revision\":1,"
        "\"nonce\":\"" + nonce
        + "\",\"staging\":\"" + staging.filename().string() + "\"}\n");
    std::string stamps_json = "[";
    for (std::size_t index = 0; index < validation_stamps.size(); ++index) {
        stamps_json += (index > 0 ? ", " : "") + json_quote(validation_stamps[index]);
    }
    stamps_json.push_back(']');
    const fs::path prepublish = output.string() + ".prepublish.json";
    write_private_artifact(prepublish,
        "{\n  \"schema\": \"bioseq.website.prepublish\",\n"
        "  \"schema_revision\": 1,\n"
        "  \"operation\": \"publish\",\n  \"nonce\": \"" + nonce + "\",\n"
        "  \"staging\": \"" + staging.filename().string() + "\",\n"
        "  \"new_manifest_sha256\": \"" + manifest_hash + "\",\n"
        "  \"validation_stamps\": " + stamps_json + "\n}\n");
    fsync_file(prepublish);
    fsync_directory(parent_or_dot(output));

    atomic_publish(staging, output);
    fsync_directory(parent_or_dot(output));

    publish_artifact_if_changed(output.string() + ".success", manifest_hash + "\n");
    fsync_file(output.string() + ".success");
    std::error_code cleanup;
    fs::remove(prepublish, cleanup);
    fs::remove(owner_marker, cleanup);
    fsync_directory(parent_or_dot(output));
    std::println("published site to {}", output.string());

    // Combine saved stage reports with byte counts from the published tree.
    // A missing or empty stage report becomes null. Write build-report.json
    // under the report directory, separate from the site manifest.
    if (const auto found = options.find("report-dir");
        found != options.end() && !found->second.empty()) {
        const fs::path report_dir(found->second);
        const auto fragment = [&report_dir](
                                  std::string_view name) -> std::string {
            std::error_code exists_error;
            const fs::path path = report_dir / (std::string(name) + ".json");
            if (!fs::exists(path, exists_error)) {
                return "null";
            }
            std::string body = read_artifact_bytes(path);
            while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) {
                body.pop_back();
            }
            return body.empty() ? "null" : body;
        };
        std::uint64_t output_bytes = 0;
        std::uint64_t search_index_bytes = 0;
        std::error_code walk;
        for (const auto& entry : fs::recursive_directory_iterator(output, walk)) {
            if (!entry.is_regular_file()) {
                continue;
            }
            const std::uint64_t size = entry.file_size();
            output_bytes += size;
            if (entry.path().filename() == "search-index.json") {
                search_index_bytes += size;
            }
        }
        const std::string report =
            "{\n  \"schema\": \"bioseq.website.build_report\",\n"
            "  \"schema_revision\": 1,\n"
            "  \"plan\": " + fragment("plan") + ",\n"
            "  \"cpp_index\": " + fragment("cpp_index") + ",\n"
            "  \"execute\": " + fragment("execute") + ",\n"
            "  \"render\": " + fragment("render") + ",\n"
            "  \"output_bytes\": " + std::to_string(output_bytes) + ",\n"
            "  \"search_index_bytes\": " + std::to_string(search_index_bytes) + ",\n"
            "  \"warnings\": []\n}\n";
        publish_artifact_if_changed(report_dir / "build-report.json", report);
        std::println("wrote build report to {}",
                     (report_dir / "build-report.json").string());
    }
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website publish: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website publish: {}", error.what());
    return 3;
}

/// @brief Runs website execute to prepare site data and SQL results.
///
/// Builds the model from tutorials and reference artifacts, executes examples
/// when a connection  is configured, and saves the private, model. Hidden SQL
/// is cleared before saving so rendering can run later without PostgreSQL.
/// @param args Options after execute.
/// @return 0 on success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_site_model_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({
        "source", "version", "site-config", "cpp-entities", "sql-entities",
        "sql-manifest", "sql-documentation-manifest", "base-path", "source-revision", "output-model", "cpp-signature",
        "staged-extension-sha256", "conninfo", "admin-conninfo", "socket-dir",
        "port", "template-db", "report-dir"});
    static constexpr auto flag_keys = std::to_array<std::string_view>({"include-drafts", "isolated"});
    static constexpr auto repeatable_value_keys = std::to_array<std::string_view>({
        "sql-manifest"});
    const auto options = parse_options(
        args, value_keys, flag_keys, repeatable_value_keys);
    const fs::path source = require_option(options, "source");
    const std::string version_string = require_option(options, "version");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path output_model = require_option(options, "output-model");
    (void)require_option(options, "sql-manifest");

    const std::string site_yaml = read_artifact_bytes(site_config_path);
    const site_configuration config =
        parse_site_configuration(site_yaml, site_config_path.generic_string());
    std::string base_path = config.default_base_path;
    if (const auto found = options.find("base-path"); found != options.end()) {
        base_path = found->second;
    }
    std::string revision;
    if (const auto found = options.find("source-revision"); found != options.end()) {
        revision = found->second;
    }
    // Start fresh execution statistics for this command. Model building
    // records each example as it runs.
    example_stats() = example_run_stats{};
    const auto stage_started = std::chrono::steady_clock::now();
    const site_model model = create_site_model(
        source, version_string, config, site_yaml,
        content_relative_path(site_config_path, source), std::move(base_path),
        std::move(revision), options.contains("include-drafts"), options);
    write_private_artifact(output_model, serialize_site_model(model));
    const auto stage_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stage_started).count();
    const example_run_stats& stats = example_stats();
    std::string examples_json = "[";
    for (std::size_t index = 0; index < stats.measurements.size(); ++index) {
        const sql_example_measurement& measurement = stats.measurements[index];
        if (index > 0) {
            examples_json.push_back(',');
        }
        examples_json += "{\"id\":" + report_json_string(measurement.example_id)
            + ",\"elapsed_ms\":" + std::to_string(measurement.elapsed.count()) + "}";
    }
    examples_json.push_back(']');
    write_stage_report(options, "execute",
        "{\"stage\":\"execute\",\"wall_ms\":" + std::to_string(stage_ms)
            + ",\"catalog_wall_ms\":" + std::to_string(stats.catalog_ms)
            + ",\"cpp_entity_count\":" + std::to_string(model.cpp_entities.size())
            + ",\"sql_entity_count\":" + std::to_string(model.sql_entities.size())
            + ",\"example_count\":" + std::to_string(stats.measurements.size())
            + ",\"peak_rows\":" + std::to_string(stats.peak_rows)
            + ",\"peak_cells\":" + std::to_string(stats.peak_cells)
            + ",\"peak_payload_bytes\":" + std::to_string(stats.peak_payload_bytes)
            + ",\"examples\":" + examples_json + "}\n");
    std::println("executed site model for {} -> {}", version_string, output_model.string());
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website execute: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website execute: {}", error.what());
    return 3;
}

/// @brief Runs website render to write pages from a saved model.
///
/// Checks the model format and renders into staging at the requested base
/// path. Writes an owner marker  and, when requested, a validation stlmp
/// containing the manifest hash. Publication is a separate command.
/// @param args Options after render
/// @return 0 on success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_render_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({
        "model", "site-config", "templates", "static", "output", "base-path",
        "source-revision", "model-validation-stamp", "report-dir", "page-dates", "thumbnails"});
    const auto options = parse_options(args, value_keys, {});
    const fs::path model_path = require_option(options, "model");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path templates_dir = require_option(options, "templates");
    const fs::path static_dir = require_option(options, "static");
    const fs::path staging = require_option(options, "output");

    const std::string site_yaml = read_artifact_bytes(site_config_path);
    const site_configuration config =
        parse_site_configuration(site_yaml, site_config_path.generic_string());
    std::string base_path = config.default_base_path;
    if (const auto found = options.find("base-path"); found != options.end()) {
        base_path = found->second;
    }
    std::string revision;
    if (const auto found = options.find("source-revision"); found != options.end()) {
        revision = found->second;
    }
    boost::json::object page_dates;
    fs::path thumbnails;
    if (const auto found = options.find("page-dates"); found != options.end()) {
        page_dates = boost::json::parse(read_artifact_bytes(found->second)).as_object();
        if (page_dates.at("schema").as_string() != "bioseq.website.page_dates.v1")
            throw std::runtime_error("unsupported page dates schema");
    }
    if (const auto found = options.find("thumbnails"); found != options.end())
        thumbnails = fs::path(found->second);
    const auto stage_started = std::chrono::steady_clock::now();
    const site_model model = deserialize_site_model(read_artifact_bytes(model_path));
    const std::string manifest_hash = render_site_model(
        model, config, base_path, revision, templates_dir, static_dir, staging,
        load_landing_content(site_config_path),
            load_landing_content(site_config_path, "contact.yaml"),
            load_landing_content(site_config_path, "blog.yaml"),
            load_qa_content(site_config_path), page_dates, thumbnails);
    const auto stage_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - stage_started).count();
    std::uint64_t staged_html = 0;
    std::error_code render_walk;
    for (const auto& entry : fs::recursive_directory_iterator(staging, render_walk)) {
        if (entry.is_regular_file() && entry.path().extension() == ".html") {
            ++staged_html;
        }
    }
    write_stage_report(options, "render",
        "{\"stage\":\"render\",\"render_wall_ms\":" + std::to_string(stage_ms)
            + ",\"page_count\":" + std::to_string(staged_html) + "}\n");

    // Mark staging for publication and write the manifest hash to the
    // requested model-validation stamp.
    const std::string nonce =
        std::to_string(::getpid()) + "-" + manifest_hash.substr(0, 16);
    write_private_artifact(staging.string() + ".owner.json",
        "{\"schema\":\"bioseq.website.owner\",\"schema_revision\":1,"
        "\"nonce\":\"" + nonce
        + "\",\"staging\":\"" + staging.filename().string() + "\"}\n");
    if (const auto found = options.find("model-validation-stamp"); found != options.end()) {
        publish_artifact_if_changed(fs::path(found->second), manifest_hash + "\n");
    }
    std::println("rendered {} -> {} (base {})",
                 model_path.string(), staging.string(), base_path);
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website render: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website render: {}", error.what());
    return 3;
}

/// @brief Runs website assemble to combine single-version sites.
///
/// Checks each input manifest against its success marker, then copies version
/// pages and assets. Shared root files come from the newest version. Rebuilds
/// versions.json, checks the assembled file  list, and swaps staging into place.
/// @param args Options after assemble.
/// @return 0 on success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_assemble_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({"site-roots", "output"});
    const auto options = parse_options(args, value_keys, {});
    const std::string roots_list = require_option(options, "site-roots");
    const fs::path output = require_option(options, "output");

    std::vector<fs::path> roots;
    std::size_t begin = 0;
    while (begin <= roots_list.size()) {
        const std::size_t end = roots_list.find(';', begin);
        const std::size_t stop = end == std::string::npos ? roots_list.size() : end;
        std::string entry = trim_ascii_whitespace(roots_list.substr(begin, stop - begin));
        if (!entry.empty()) {
            roots.emplace_back(std::move(entry));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    if (roots.empty()) {
        throw std::runtime_error("assemble requires at least one --site-roots entry");
    }

    struct input_site {
        fs::path root;
        std::string version;
        std::string base_path;
        std::vector<std::string> outputs;
        std::string manifest_hash;
        std::string success_hash;
        std::vector<std::uint32_t> parts;
    };
    std::vector<input_site> inputs;
    for (const fs::path& root : roots) {
        const std::string manifest_text = read_artifact_bytes(root / "site-manifest.json");
        const std::string manifest_hash = bioseq::common::sha256_hex(manifest_text);
        const fs::path marker = root.string() + ".success";
        std::error_code error;
        if (!fs::exists(marker, error) || error) {
            throw std::runtime_error("missing success marker for " + root.string());
        }
        const std::string marker_bytes = read_artifact_bytes(marker);
        std::string marker_hash = trim_ascii_whitespace(marker_bytes);
        if (marker_hash != manifest_hash) {
            throw std::runtime_error("success marker does not match site-manifest.json for "
                                     + root.string());
        }
        input_site site;
        site.root = root;
        site.version = manifest_string_field(
            manifest_text, "version", "bioseq.website.site_manifest", 1);
        site.base_path = manifest_string_field(
            manifest_text, "base_path", "bioseq.website.site_manifest", 1);
        site.outputs = manifest_output_routes(
            manifest_text, "bioseq.website.site_manifest", 1);
        site.manifest_hash = manifest_hash;
        site.success_hash = bioseq::common::sha256_hex(marker_bytes);
        site.parts = parse_release_components(site.version);
        inputs.push_back(std::move(site));
    }

    std::ranges::sort(inputs, [](const input_site& lhs, const input_site& rhs) {
        return lhs.parts > rhs.parts;
    });
    const std::string primary_version = inputs.front().version;
    const std::string base_path = inputs.front().base_path;
    std::set<std::string> version_set;
    for (const input_site& site : inputs) {
        if (!version_set.insert(site.version).second) {
            throw std::runtime_error("duplicate version among assembled roots: "
                                     + site.version);
        }
    }

    const fs::path staging = output.string() + ".staging";
    std::error_code staging_error;
    fs::remove_all(staging, staging_error);
    fs::create_directories(staging);
    staging_writer writer{.root = staging, .manifest = {}};

    for (const input_site& site : inputs) {
        const std::string version_prefix = "/" + site.version + "/";
        const bool is_primary = site.version == primary_version;
        for (const std::string& route : site.outputs) {
            if (route == "/site-manifest.json" || route == "/versions.json") {
                continue;
            }
            const bool release_scoped =
                route.starts_with(version_prefix) || route.starts_with("/assets/");
            if (!release_scoped && !is_primary) {
                continue;
            }
            writer.emit(route, read_artifact_bytes(site.root / route.substr(1)));
        }
    }

    std::vector<std::string> version_labels;
    std::vector<std::string> input_manifest_hashes;
    std::vector<std::string> input_success_hashes;
    for (const input_site& site : inputs) {
        version_labels.push_back(site.version);
        input_manifest_hashes.push_back(site.manifest_hash);
        input_success_hashes.push_back(site.success_hash);
    }
    writer.emit("/versions.json", serialize_release_catalog(version_labels, base_path));

    std::vector<std::pair<std::string, std::string>> outputs(
        writer.manifest.begin(), writer.manifest.end());
    const std::string assembled = serialize_release_manifest(
        primary_version, version_labels, base_path,
        input_manifest_hashes, input_success_hashes, outputs);
    {
        std::ofstream manifest_out(staging / "site-manifest.json",
                                   std::ios::binary | std::ios::trunc);
        manifest_out.write(assembled.data(),
                           static_cast<std::streamsize>(assembled.size()));
        if (!manifest_out) {
            throw std::runtime_error("cannot write assembled site-manifest.json");
        }
    }

    std::set<std::string> present;
    collect_site_files(staging, present);
    present.erase("/site-manifest.json");
    std::set<std::string> declared;
    for (const auto& [route, digest] : writer.manifest) {
        declared.insert(route);
    }
    if (present != declared) {
        throw std::runtime_error("assembled manifest set mismatch: " + std::to_string(declared.size())
                                 + " declared vs " + std::to_string(present.size()) + " present");
    }

    atomic_publish(staging, output);
    const std::string manifest_hash =
        bioseq::common::sha256_hex(read_artifact_bytes(output / "site-manifest.json"));
    publish_artifact_if_changed(output.string() + ".success", manifest_hash + "\n");
    std::println("assembled {} versions into {} ({} outputs)",
                 inputs.size(), output.string(), writer.manifest.size());
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website assemble: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website assemble: {}", error.what());
    return 3;
}

/// @brief Runs website source_graph to find publicly imported modules.
///
/// Starts from configured modules and follows export import edges in the
/// scanned sources. Missing Bioseq module providers fail the build. Saves
/// the graph and optionally a source-file list for Doxygen and cpp_index,
/// adding PostgreSQL entry-point files to that list.
/// @param args Options after source_graph.
/// @return 0 on success, 2 for invalid command-line usage, or 3 for other failures.
[[nodiscard]] int execute_source_graph_command(std::span<std::string_view const> args) try {
    static constexpr auto value_keys = std::to_array<std::string_view>({
        "reachable-units", "entrypoint-units", "compile-db", "source-root",
        "site-config", "output", "reachable-output"});
    const auto options = parse_options(args, value_keys, {});
    const fs::path reachable_units = require_option(options, "reachable-units");
    const fs::path entrypoint_units = require_option(options, "entrypoint-units");
    const fs::path compile_db = require_option(options, "compile-db");
    const fs::path source_root = require_option(options, "source-root");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path output = require_option(options, "output");

    const fs::path normalized_source_root = source_root.lexically_normal();
    std::set<std::string> normalized_entrypoints;
    for (const std::string& line : read_lines(entrypoint_units)) {
        const fs::path input{line};
        const fs::path candidate = input.is_absolute()
            ? input.lexically_normal()
            : (normalized_source_root / input).lexically_normal();
        const std::string relative =
            canonical_source_path(candidate, normalized_source_root);
        const fs::path normalized =
            (normalized_source_root / relative).lexically_normal();
        std::error_code file_error;
        if (!fs::is_regular_file(normalized, file_error) || file_error) {
            throw std::runtime_error(
                "entrypoint unit is not a regular source file: "
                + normalized.generic_string());
        }
        normalized_entrypoints.insert(relative);
    }
    if (normalized_entrypoints.empty()) {
        throw std::runtime_error("entrypoint unit inventory is empty");
    }

    const site_configuration config =
        parse_site_configuration(read_artifact_bytes(site_config_path), site_config_path.generic_string());

    std::vector<fs::path> units;
    for (const std::string& line : read_lines(reachable_units)) {
        units.emplace_back(line);
    }
    const std::vector<module_graph_unit> scanned =
        scan_module_graph(units, compile_db);

    // Index module providers and reject duplicates.
    std::map<std::string, module_graph_unit, std::less<>> universe;
    for (const module_graph_unit& unit : scanned) {
        if (unit.provided_module.empty()) {
            continue;
        }
        if (!universe.emplace(unit.provided_module, unit).second) {
            throw std::runtime_error("duplicate module provider: " + unit.provided_module);
        }
    }

    // Start with configured public modules. Each needs a scanned provider.
    std::vector<std::string> roots;
    std::vector<std::string> worklist;
    std::set<std::string> reached;
    for (const auto& [module_name, interface] : config.cpp_public_modules) {
        if (!universe.contains(module_name)) {
            throw std::runtime_error("configured primary module not found in scan: "
                                     + module_name);
        }
        roots.push_back(module_name);
        if (reached.insert(module_name).second) {
            worklist.push_back(module_name);
        }
    }

    // Follow exported imports found  in the scan. A missing bioseq. provider
    // is an error. Other unscanned imports remain external dependencies.
    while (!worklist.empty()) {
        const std::string current = std::move(worklist.back());
        worklist.pop_back();
        const auto found = universe.find(current);
        if (found == universe.end()) {
            continue;
        }
        for (const std::string& target : found->second.exported_imports) {
            if (universe.contains(target)) {
                if (reached.insert(target).second) {
                    worklist.push_back(target);
                }
            } else if (target.starts_with("bioseq.")) {
                throw std::runtime_error("exported Bioseq import has no provider: "
                                         + current + " -> " + target);
            }
        }
    }

    std::vector<module_graph_unit> closure;
    for (const std::string& module_name : reached) {
        closure.push_back(universe.at(module_name));
    }
    std::ranges::sort(closure, [](const module_graph_unit& lhs,
                                  const module_graph_unit& rhs) {
        return lhs.provided_module < rhs.provided_module;
    });
    std::ranges::sort(roots);

    std::vector<module_graph_unit> serialized_closure = closure;
    for (module_graph_unit& unit : serialized_closure) {
        unit.source_path = canonical_source_path(unit.source_path, source_root);
    }
    publish_artifact_if_changed(
        output,
        serialize_source_graph(linked_clang_release(), roots, serialized_closure));

    // Add PostgreSQL entrz-point files to, the source list used by Doxygen
    // and cpp_index. Keep them separate from the module-provider graph
    if (const auto found = options.find("reachable-output"); found != options.end()) {
        std::string reachable_text;
        std::set<std::string> emitted_paths;
        for (const module_graph_unit& unit : closure) {
            const fs::path normalized =
                fs::path{unit.source_path}.lexically_normal();
            const std::string normalized_text = normalized.generic_string();
            if (!emitted_paths.insert(normalized_text).second) {
                throw std::runtime_error(
                    "duplicate interface path in reachable closure: "
                    + normalized_text);
            }
            reachable_text += normalized_text;
            reachable_text.push_back('\n');
        }

        for (const std::string& entrypoint : normalized_entrypoints) {
            if (!emitted_paths.insert(entrypoint).second) {
                throw std::runtime_error(
                    "entrypoint unit also entered the interface closure: "
                    + entrypoint);
            }
            reachable_text += entrypoint;
            reachable_text.push_back('\n');
        }
        publish_artifact_if_changed(fs::path(found->second), reachable_text);
    }
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website source_graph: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website source_graph: {}", error.what());
    return 3;
}

/// @brief Dispatches a website subcommand and returns its process exit code.
/// @param args Arguments after the leading `website` token (subcommand first).
/// @return Zero on success, two on usage errors, or three on validation failure.
[[nodiscard]] int dispatch_website_command(std::span<std::string_view const> args) {
    if (args.empty()) {
        std::println(stderr, "usage: bioseq_codegen website <command> [options]");
        return 2;
    }
    const std::string_view command = args.front();
    const std::span<std::string_view const> rest = args.subspan(1);
    if (command == "plan") {
        return execute_plan_command(rest);
    }
    if (command == "cpp_index") {
        return execute_cpp_index_command(rest);
    }
    if (command == "execute") {
        return execute_site_model_command(rest);
    }
    if (command == "execute-sql") {
        return execute_sql_example_command(rest);
    }
    if (command == "render") {
        return execute_render_command(rest);
    }
    if (command == "render-fragment") {
        return execute_template_render_command(rest);
    }
    if (command == "catalog") {
        return execute_catalog_command(rest);
    }
    if (command == "publish") {
        return execute_publish_command(rest);
    }
    if (command == "assemble") {
        return execute_assemble_command(rest);
    }
    if (command == "source_graph") {
        return execute_source_graph_command(rest);
    }
    std::println(stderr, "unknown website command: {}", command);
    return 2;
}

}  // namespace bioseq::codegen::website
