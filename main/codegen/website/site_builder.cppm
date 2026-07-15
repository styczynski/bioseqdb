module;

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

module bioseq.codegen.website:site_builder;

import :front_matter;
import :markdown;
import :markdown_extensions;
import :doxygen_xml;
import :clang_entity_index;
import :entity_index;
import :json;
import :search;
import :template_renderer;
import :version_assembler;
import bioseq.codegen.website.postgres;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Website build orchestration (§5). Owns the phase command dispatch and the
// cross-partition integration smoke. The plan/source_graph/cpp_index/execute/
// render/publish/assemble bodies are filled across Phases 5-10; this phase
// stands up the CLI surface, exit-code contract, and a functional smoke that
// links every integrated dependency (MD4C, pugixml, Boost.JSON, Jinja2Cpp,
// yaml-cpp, libpq) into one binary.
namespace bioseq::codegen::website {

namespace {

[[nodiscard]] std::size_t count_kind(
    const document_node& node,
    document_node_kind kind) {
    std::size_t total = node.kind == kind ? 1 : 0;
    for (const document_node& child : node.children) {
        total += count_kind(child, kind);
    }
    return total;
}

void expect(bool condition, std::string_view detail) {
    if (!condition) {
        throw std::runtime_error("website self-test failed: " + std::string(detail));
    }
}

}  // namespace

/// @brief Exercises every integrated parser/renderer/serializer once.
///
/// This is the functional half of the website self-test: it links and runs
/// MD4C, pugixml, Boost.JSON, Jinja2Cpp, yaml-cpp, and the entity/search merge
/// so a regression in any dependency integration fails the build's self-test.
void run_website_smoke() {
    // MD4C.
    const document_node document =
        parse_markdown("# Sequence type\n\nThe `bioseq.seq` column stores DNA.\n");
    expect(count_kind(document, document_node_kind::heading) == 1, "markdown heading");
    expect(count_kind(document, document_node_kind::code_span) == 1, "markdown code span");

    // markdown extensions.
    expect(admonition_class("WARNING") == "warning", "admonition class");
    const auto reference =
        parse_entity_reference("entity:sql:type:bioseq.seq", source_span{});
    expect(reference.has_value() && reference->domain == entity_query_domain::sql,
           "entity reference domain");

    // yaml-cpp frontmatter.
    const front_matter_split split = split_front_matter(
        "---\ntitle: T\nslug: s\ndescription: d\norder: 1\n---\nbody\n");
    const tutorial_metadata metadata = parse_front_matter(split.yaml, "smoke.md");
    expect(metadata.title == "T" && metadata.slug == "s", "frontmatter parse");

    // pugixml.
    const auto compounds = parse_doxygen_index(
        "<doxygenindex><compound refid=\"ns\" kind=\"namespace\">"
        "<name>bioseq</name></compound></doxygenindex>");
    expect(compounds.size() == 1 && compounds.front().name == "bioseq", "doxygen index");

    // entity merge + canonical id.
    documented_entity documented;
    documented.domain = entity_domain::cpp;
    documented.qualified_name = "bioseq::io::seq";
    documented.summary_text = "sequence";
    const clang_structural_index structural;
    const auto merged = merge_cpp_entities(structural, {documented});
    expect(merged.size() == 1 && merged.front().canonical_id == "cpp:bioseq.io.seq",
           "entity merge canonical id");

    // Boost.JSON canonical + HTML-safe (reached through the json partition, not
    // by naming boost types here).
    const std::string search_json = build_search_index(merged);
    expect(search_json.ends_with("\n"), "search json newline");
    const std::string escaped = apply_html_safe_escapes("a<b>&c");
    expect(escaped.find("\\u003c") != std::string::npos, "json html-safe escape");

    // version ordering.
    expect((parse_version_parts("v0.1") == std::vector<std::uint32_t>{0, 1}),
           "version parts");

    // Jinja2Cpp (through the C++20 shim).
    const std::string rendered = render_template(
        "Hello {{ project }}",
        std::map<std::string, std::string, std::less<>>{{"project", "bioseq"}});
    expect(rendered == "Hello bioseq", "template render");

    // PostgreSQL policy layer constructs (libpq is link-proven by the executor;
    // live catalog/execution arrives in Phase 7). No connection is attempted.
    private_cluster_executor executor("host='/nonexistent-bioseq-socket'");
    (void)executor;
}

/// @brief Dispatches a website subcommand and returns its process exit code.
/// @param args Arguments after the leading `website` token (subcommand first).
/// @return Exit code per §7: 2 usage, 3 parse/validation (phase stubs).
[[nodiscard]] int dispatch_website(std::span<std::string_view const> args) {
    if (args.empty()) {
        std::println(stderr, "usage: bioseq_codegen website <command> [options]");
        return 2;
    }
    const std::string_view command = args.front();
    static constexpr std::string_view known[] = {
        "plan", "source_graph", "cpp_index", "execute", "render", "publish", "assemble"};
    for (const std::string_view candidate : known) {
        if (command == candidate) {
            std::println(stderr,
                         "website {}: not yet implemented (phase in progress)", command);
            return 3;
        }
    }
    std::println(stderr, "unknown website command: {}", command);
    return 2;
}

}  // namespace bioseq::codegen::website
