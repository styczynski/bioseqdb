module;

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1 << 1)
#endif

module bioseq.codegen.website:site_builder;

import :front_matter;
import :markdown;
import :markdown_extensions;
import :doxygen_xml;
import :clang_entity_index;
import :entity_index;
import :html_render;
import :json;
import :search;
import :template_renderer;
import :version_assembler;
import bioseq.codegen.website.postgres;
import bioseq.codegen.website.model;
import bioseq.codegen.support;
import bioseq.common.hashing;

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
        "---\ntitle: T\ndescription: d\norder: 1\nkeywords: [k]\n---\nbody\n");
    const tutorial_metadata metadata =
        parse_front_matter(split.yaml, "smoke.md", "smoke");
    expect(metadata.title == "T" && metadata.slug == "smoke", "frontmatter parse");

    // runnable SQL fence grammar (§10.7).
    const auto fence = parse_runnable_fence("sql runnable id=probe timeout=5s");
    expect(fence.has_value() && fence->local_id == "probe", "runnable fence parse");
    const sql_phase_bodies phases = split_sql_phases(
        "-- bioseq:setup\nCREATE TABLE t(v int);\n-- bioseq:visible\n"
        "SELECT 1;\n-- bioseq:teardown\nDROP TABLE t;\n");
    expect(phases.visible == "SELECT 1;", "sql phase split");

    // pugixml.
    const auto compounds = parse_doxygen_index(
        "<doxygenindex><compound refid=\"ns\" kind=\"namespace\">"
        "<name>bioseq</name></compound></doxygenindex>");
    expect(compounds.size() == 1 && compounds.front().name == "bioseq", "doxygen index");

    // Doxygen mixed-content mapping (§11.2): s->strikethrough, computeroutput->
    // code span, safe ulink kept, unsafe ulink dropped to text + diagnostic,
    // unknown tag diagnosed but text preserved, ndash -> exact Unicode.
    const auto description = parse_doxygen_description(
        "<detaileddescription><para>A <s>gone</s> "
        "<computeroutput>bioseq.seq</computeroutput> "
        "<ulink url=\"https://bioseq.test/x\">ok</ulink> "
        "<ulink url=\"javascript:alert(1)\">bad</ulink> "
        "range<ndash/>value <mystery>kept-text</mystery>.</para>"
        "</detaileddescription>",
        "bioseq::io::seq");
    document_node description_root;
    description_root.kind = document_node_kind::document;
    description_root.children = description.nodes;
    expect(count_kind(description_root, document_node_kind::strikethrough) == 1,
           "doxygen s to strikethrough");
    expect(count_kind(description_root, document_node_kind::code_span) == 1,
           "doxygen computeroutput to code span");
    expect(count_kind(description_root, document_node_kind::link) == 1,
           "doxygen keeps only the safe ulink");
    expect(description.diagnostics.size() == 2,
           "doxygen unsafe-link and unknown-tag diagnostics");

    // entity merge (§11.6): a Clang page-owner is enriched with its unique
    // Doxygen documentation match (rule 1: exact normalized signature).
    documented_entity clang_seq;
    clang_seq.domain = entity_domain::cpp;
    clang_seq.kind = entity_kind::struct_;
    clang_seq.qualified_name = "bioseq::io::seq";
    clang_seq.normalized_signature = "bioseq::io::seq";
    clang_structural_index structural;
    structural.entities.push_back(clang_seq);
    documented_entity documented;
    documented.domain = entity_domain::cpp;
    documented.qualified_name = "bioseq::io::seq";
    documented.normalized_signature = "bioseq::io::seq";
    documented.summary_text = "sequence";
    const auto merged = merge_cpp_entities(structural, {documented});
    expect(merged.size() == 1 && merged.front().canonical_id == "cpp:bioseq.io.seq"
               && merged.front().summary_text == "sequence",
           "entity merge attaches docs by normalized signature");

    // coverage (§11.4): a required-but-undocumented entity is fatal-listed.
    documented_entity undocumented;
    undocumented.domain = entity_domain::cpp;
    undocumented.qualified_name = "bioseq::io::internal";
    undocumented.canonical_id = "cpp:bioseq.io.internal";
    const auto coverage = compute_coverage(
        {merged.front(), undocumented}, {"cpp:bioseq.io.internal"}, {});
    expect(coverage.public_total == 2 && coverage.documented_total == 1
               && coverage.fatal_missing.size() == 1,
           "coverage flags required undocumented entity");

    // Clang 22 structural/export pass (§11.3): exported module members are
    // recorded with kind/signature; a non-exported namespace is excluded.
    const clang_structural_index clang_index = parse_clang_structural_index_from_code(
        "export module bioseq.demo;\n"
        "export namespace demo {\n"
        "  struct Sequence { int length; };\n"
        "  int len(const Sequence& value);\n"
        "}\n"
        "namespace hidden { struct Internal { int y; }; }\n",
        {"-std=c++20", "-xc++"});
    const auto has_entity = [&](std::string_view name) {
        return std::ranges::any_of(
            clang_index.entities, [&](const documented_entity& candidate) {
                return candidate.qualified_name == name;
            });
    };
    expect(has_entity("demo::Sequence"), "clang records exported struct");
    expect(has_entity("demo::len"), "clang records exported function");
    expect(!has_entity("hidden::Internal"), "clang excludes non-exported namespace");
    expect(clang_index.clang_version.starts_with("22"), "clang version is 22");

    // Clang 22 module-graph pass (§11.3): the provided module name is recovered and
    // a unit with no imports yields no public edge (plain imports are validated
    // against the real module graph in the source_graph command / integration).
    const module_graph_unit demo_unit = scan_module_graph_from_code(
        "export module bioseq.demo.leaf;\n"
        "export namespace demo { struct Node { int value; }; }\n",
        {"-std=c++20", "-xc++"});
    expect(demo_unit.provided_module == "bioseq.demo.leaf",
           "module graph records the provided module");
    expect(demo_unit.exported_imports.empty(),
           "a unit with no import has no public edge");

    // Boost.JSON canonical + HTML-safe (reached through the json partition, not
    // by naming boost types here).
    const std::string search_json = build_search_index(merged);
    expect(search_json.ends_with("\n"), "search json newline");
    const std::string escaped = apply_html_safe_escapes("a<b>&c");
    expect(escaped.find("\\u003c") != std::string::npos, "json html-safe escape");

    // version ordering.
    expect((parse_version_parts("v0.1") == std::vector<std::uint32_t>{0, 1}),
           "version parts");

    // SHA-256 (§16 content addressing) against NIST test vectors.
    expect(bioseq::codegen::sha256_hex("abc")
           == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "sha256 abc vector");
    expect(bioseq::codegen::sha256_hex("")
           == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "sha256 empty vector");

    // Jinja2Cpp (through the C++20 shim).
    const std::string rendered = render_template(
        "Hello {{ project }}",
        std::map<std::string, std::string, std::less<>>{{"project", "bioseq"}});
    expect(rendered == "Hello bioseq", "template render");

    // Structured JSON-context rendering (§14): loop over a list of objects,
    // reflected from Boost.JSON — the surface entity/tutorial pages render with.
    const std::string page = render_template_json(
        "{% for e in items %}{{ e.name }}={{ e.n }};{% endfor %}",
        R"({"items":[{"name":"a","n":1},{"name":"b","n":2}]})");
    expect(page == "a=1;b=2;", "template json loop render");

    // PostgreSQL policy layer constructs (libpq is link-proven by the executor;
    // live catalog/execution arrives in Phase 7). No connection is attempted.
    private_cluster_executor executor("host='/nonexistent-bioseq-socket'");
    (void)executor;
}

namespace {

namespace fs = std::filesystem;

[[nodiscard]] std::string hex64(std::uint64_t value) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int index = 15; index >= 0; --index) {
        out[index] = digits[value & 0xF];
        value >>= 4;
    }
    return out;
}

// A CLI misuse (unknown/repeated/positional/missing-value option). Distinct from
// std::runtime_error so the dispatcher can return exit 2 (usage) versus exit 3
// (parse/validation) per the §7 exit-code contract.
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

/// @brief Parses options against a closed set of value-taking keys and flags.
///
/// Unknown, repeated, positional, and value-less usages are all fatal usage
/// errors. Flags map to `"true"`; value keys consume the following token.
[[nodiscard]] std::map<std::string, std::string, std::less<>> parse_options(
    std::span<std::string_view const> args,
    std::span<std::string_view const> value_keys,
    std::span<std::string_view const> flag_keys) {
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
        if (!options.emplace(key, std::string(args[index + 1])).second) {
            throw cli_usage_error("repeated option --" + key);
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

/// @brief Writes a private artifact atomically with mode 0600 (§5.1).
///
/// The temp file is created under a 0077 umask so the private permissions hold
/// from creation — there is never a window where the file is world-readable.
/// The content is only rewritten when it differs, keeping mtimes stable for
/// incremental builds while still asserting the permission bits every run.
void write_private_artifact(const fs::path& path, std::string_view content) {
    std::error_code exists_error;
    if (fs::exists(path, exists_error) && !exists_error) {
        if (read_file(path) == content) {
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

[[nodiscard]] std::string plan_relative(const fs::path& path, const fs::path& source) {
    std::error_code error;
    const fs::path relative = fs::relative(path, source, error);
    const fs::path chosen = (error || relative.empty()) ? path : relative;
    return chosen.generic_string();
}

void collect_examples_and_queries(
    const document_node& node,
    const std::string& slug,
    const std::string& version_component,
    std::vector<sql_example>& examples,
    std::vector<unresolved_entity_query>& queries) {
    if (node.kind == document_node_kind::code_block) {
        const auto language = node.attributes.find("language");
        const auto info = node.attributes.find("info");
        if (language != node.attributes.end() && language->second == "sql"
            && info != node.attributes.end()) {
            if (const auto fence = parse_runnable_fence(info->second)) {
                const sql_phase_bodies phases = split_sql_phases(node.text);
                sql_example example;
                example.local_id = fence->local_id;
                example.canonical_id = version_component + "/tutorial/" + slug + "#"
                    + fence->local_id;
                example.setup_sql = phases.setup;
                example.visible_sql = phases.visible;
                example.teardown_sql = phases.teardown;
                example.expectation = fence->expectation;
                example.expected_sqlstate = fence->expected_sqlstate;
                example.timeout = fence->timeout;
                examples.push_back(std::move(example));
            }
        }
    }
    if (node.kind == document_node_kind::entity_link) {
        // Carry the node's own source span when the Markdown stage recorded one;
        // span population is refined as MD4C offsets are threaded through.
        const source_span span = node.source ? *node.source : source_span{};
        if (auto query = parse_entity_reference(node.target, span)) {
            queries.push_back(std::move(*query));
        }
    }
    for (const document_node& child : node.children) {
        collect_examples_and_queries(child, slug, version_component, examples, queries);
    }
}

/// @brief Builds the deterministic website plan from validated source (§10).
///
/// Parses every tutorial's frontmatter/Markdown under <source>/<version>/tutorials,
/// extracts runnable SQL and typed entity queries, resolves order and prev/next
/// links, and folds the site config plus every input's content hash into the plan
/// signature. Shared by `plan` (which serializes it) and `publish` (which renders
/// from it), so both stages observe an identical model.
[[nodiscard]] website_plan build_plan_from_source(
    const fs::path& source,
    const std::string& version_string,
    const site_configuration& config,
    const std::string& site_yaml,
    const std::string& site_config_relpath,
    std::string base_path,
    std::string revision,
    bool include_drafts) {
    site_version version;
    version.path_component = version_string;
    version.parts = parse_version_parts(version_string);

    const fs::path tutorials_dir = source / version_string / "tutorials";
    std::vector<fs::path> markdown_files;
    for (const auto& entry : fs::directory_iterator(tutorials_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".md") {
            markdown_files.push_back(entry.path());
        }
    }
    std::ranges::sort(markdown_files);

    std::vector<plan_input> inputs;
    inputs.push_back(plan_input{
        .path = site_config_relpath,
        .content_hash = hex64(bioseq::common::fnv1a_64(site_yaml))});

    std::vector<tutorial> tutorials;
    for (const fs::path& markdown_path : markdown_files) {
        const std::string text = read_file(markdown_path);
        const front_matter_split split = split_front_matter(text);
        const std::string stem = markdown_path.stem().string();
        tutorial item;
        item.metadata = parse_front_matter(
            split.yaml, markdown_path.generic_string(), stem);
        item.version = version;
        item.source_path = plan_relative(markdown_path, source);
        const document_node document = parse_markdown(split.body);
        item.content = document.children;
        collect_examples_and_queries(document, item.metadata.slug,
                                     version.path_component, item.sql_examples,
                                     item.entity_queries);
        std::map<std::string, int, std::less<>> local_ids;
        for (const sql_example& example : item.sql_examples) {
            if (++local_ids[example.local_id] > 1) {
                throw std::runtime_error("duplicate runnable id '" + example.local_id
                                         + "' in " + item.source_path);
            }
        }
        inputs.push_back(plan_input{
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

    std::uint64_t signature = bioseq::common::fnv1a_64(base_path);
    signature = bioseq::common::hash_combine_64(signature, version.path_component);
    for (const plan_input& input : inputs) {
        signature = bioseq::common::hash_combine_64(signature, input.content_hash);
    }

    website_plan plan;
    plan.schema_version = "bioseq.website.plan/v1";
    plan.parser_version = "bioseq.website.parser/v1";
    plan.binding_schema_version = "bioseq.website.bindings/v1";
    plan.version = version;
    plan.base_path = std::move(base_path);
    plan.source_revision = std::move(revision);
    plan.site_title = config.title;
    plan.tutorials = std::move(tutorials);
    plan.inputs = std::move(inputs);
    plan.signature = hex64(signature);
    return plan;
}

}  // namespace

/// @brief Runs `website plan`: validates config, parses tutorials, writes plan.
///
/// Strictly parses site.yaml and every tutorial's frontmatter/Markdown, extracts
/// runnable SQL and typed unresolved entity queries, resolves ordering and
/// previous/next links, and writes the deterministic private plan artifact
/// (schema bioseq.website.plan/v1) with mode 0600 (§5.1, §10).
/// @param args Options after the `plan` token.
/// @return Zero on success; two on CLI usage errors; three on parse/validation.
[[nodiscard]] int run_plan(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {
        "source", "version", "site-config", "templates", "static",
        "base-path", "source-revision", "output"};
    static constexpr std::string_view flag_keys[] = {"include-drafts"};
    const auto options = parse_options(args, value_keys, flag_keys);
    const fs::path source = require_option(options, "source");
    const std::string version_string = require_option(options, "version");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path output = require_option(options, "output");
    const bool include_drafts = options.contains("include-drafts");

    const std::string site_yaml = read_file(site_config_path);
    const site_configuration config =
        parse_site_config(site_yaml, site_config_path.generic_string());
    std::string base_path = config.default_base_path;
    if (const auto found = options.find("base-path"); found != options.end()) {
        base_path = found->second;
    }
    std::string revision;
    if (const auto found = options.find("source-revision"); found != options.end()) {
        revision = found->second;
    }

    const website_plan plan = build_plan_from_source(
        source, version_string, config, site_yaml,
        plan_relative(site_config_path, source),
        std::move(base_path), std::move(revision), include_drafts);
    const std::string serialized = serialize_plan(plan);
    write_private_artifact(output, serialized);
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
    const std::string content = read_file(path);
    std::vector<std::string> lines;
    std::size_t begin = 0;
    while (begin <= content.size()) {
        const std::size_t end = content.find('\n', begin);
        const std::size_t stop = end == std::string::npos ? content.size() : end;
        std::string line = content.substr(begin, stop - begin);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!trim(line).empty()) {
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

/// @brief Runs `website cpp_index`: Doxygen + Clang → cpp_entities + coverage.
///
/// Parses the Doxygen XML tree for documentation, runs the Clang 22 structural
/// pass over the reachable interface units against the main compile database,
/// merges them (§11.6), computes documentation coverage (§11.4), and writes the
/// canonical cpp_entities/v1 and documentation_coverage/v1 artifacts. Missing
/// required documentation is fatal.
/// @param args Options after the `cpp_index` token.
/// @return Zero on success; two on usage errors; three on parse/validation.
[[nodiscard]] int run_cpp_index(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {
        "doxygen-xml", "compile-db", "reachable-units", "site-config",
        "output", "coverage-output"};
    const auto options = parse_options(args, value_keys, {});
    const fs::path doxygen_xml = require_option(options, "doxygen-xml");
    const fs::path compile_db = require_option(options, "compile-db");
    const fs::path reachable_units = require_option(options, "reachable-units");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path output = require_option(options, "output");
    const fs::path coverage_output = require_option(options, "coverage-output");

    const site_configuration config =
        parse_site_config(read_file(site_config_path), site_config_path.generic_string());

    std::vector<documented_entity> documented;
    const std::string index_xml = read_file(doxygen_xml / "index.xml");
    for (const doxygen_compound_ref& compound : parse_doxygen_index(index_xml)) {
        if (!is_page_compound(compound.kind)) {
            continue;
        }
        const fs::path compound_path = doxygen_xml / (compound.refid + ".xml");
        std::error_code error;
        if (!fs::exists(compound_path, error)) {
            continue;
        }
        for (documented_entity& entity :
             parse_doxygen_compound(read_file(compound_path))) {
            documented.push_back(std::move(entity));
        }
    }

    std::vector<fs::path> units;
    for (const std::string& line : read_lines(reachable_units)) {
        units.emplace_back(line);
    }
    const clang_structural_index structural =
        parse_clang_structural_index(units, compile_db);

    const std::vector<documented_entity> merged =
        merge_cpp_entities(structural, documented);
    const coverage_report coverage = compute_coverage(
        merged, config.required_documentation_entities, {});

    write_if_different(output, serialize_cpp_entities(merged, structural.clang_version));
    write_if_different(coverage_output,
                       serialize_coverage(coverage.public_total, coverage.documented_total,
                                          coverage.missing_summary, coverage.fatal_missing));
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

/// @brief Runs `website execute`: runs one SQL example against a live cluster.
///
/// Connects to the private Unix-socket cluster, runs the setup/visible/teardown
/// SQL through private_cluster_executor, and renders the visible result model to
/// stdout (§13.5-13.6). A focused live-execution entrypoint; database-per-example
/// isolation and the plan-driven batch runner are layered on top of this.
/// @param args Options after the `execute` token.
/// @return Zero on success; two on usage errors; three on execution failure.
[[nodiscard]] int run_execute(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {
        "conninfo", "setup", "visible", "teardown", "expect-sqlstate",
        "admin-conninfo", "socket-dir", "port", "template-db"};
    static constexpr std::string_view flag_keys[] = {"isolated"};
    const auto options = parse_options(args, value_keys, flag_keys);

    sql_example example;
    example.canonical_id = "adhoc";
    example.local_id = "adhoc";
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

    // In --isolated mode the example runs as a fresh restricted role inside a
    // per-example database cloned from the extension-bearing template (§13.3);
    // otherwise it runs directly against the supplied conninfo.
    std::expected<sql_example_result, diagnostic> outcome =
        std::unexpected(diagnostic{});
    if (options.contains("isolated")) {
        isolation_settings settings;
        settings.admin_conninfo = require_option(options, "admin-conninfo");
        settings.socket_directory = require_option(options, "socket-dir");
        settings.port = std::stoi(require_option(options, "port"));
        settings.template_database = require_option(options, "template-db");
        private_cluster_executor executor("");
        outcome = executor.execute_isolated(example, settings);
    } else {
        private_cluster_executor executor(require_option(options, "conninfo"));
        outcome = executor.execute(example);
    }
    if (!outcome) {
        std::println(stderr, "website execute: [{}] {}",
                     outcome.error().code, outcome.error().message);
        return 3;
    }
    for (const sql_result_set& result : outcome->visible_results) {
        if (result.kind == sql_result_kind::error) {
            std::println("ERROR SQLSTATE={}: {}",
                         result.sqlstate.value_or(""), result.primary_message);
        } else if (result.kind == sql_result_kind::rows) {
            std::string header;
            for (const sql_column& column : result.columns) {
                header += (header.empty() ? "" : " | ") + column.name;
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
            std::println("{}", result.command_tag);
        }
    }
    for (const sql_notice& notice : outcome->notices) {
        std::println(stderr, "NOTICE {}: {}", notice.severity, notice.message);
    }
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website execute: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website execute: {}", error.what());
    return 3;
}

/// @brief Runs `website catalog`: captures live SQL entities to an artifact.
///
/// Connects to the private cluster, reads schema-scoped type and function
/// entities with their COMMENT documentation (§12.2), and writes the canonical
/// sql_entities/v1 artifact. A focused live catalog-capture entrypoint.
/// @param args Options after the `catalog` token.
/// @return Zero on success; two on usage errors; three on capture failure.
[[nodiscard]] int run_catalog(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {"conninfo", "schemas", "output"};
    const auto options = parse_options(args, value_keys, {});
    const std::string conninfo = require_option(options, "conninfo");
    const std::string schemas_csv = require_option(options, "schemas");
    const fs::path output = require_option(options, "output");

    std::vector<std::string> schemas;
    std::size_t begin = 0;
    while (begin <= schemas_csv.size()) {
        const std::size_t end = schemas_csv.find(',', begin);
        const std::size_t stop = end == std::string::npos ? schemas_csv.size() : end;
        std::string schema = trim(schemas_csv.substr(begin, stop - begin));
        if (!schema.empty()) {
            schemas.push_back(std::move(schema));
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }

    std::vector<documented_entity> entities;
    for (documented_entity& entity : read_sql_type_catalog(conninfo, schemas)) {
        entities.push_back(std::move(entity));
    }
    for (documented_entity& entity : read_sql_function_catalog(conninfo, schemas)) {
        entities.push_back(std::move(entity));
    }
    std::ranges::sort(entities, [](const documented_entity& lhs,
                                   const documented_entity& rhs) {
        return lhs.canonical_id < rhs.canonical_id;
    });
    write_if_different(output, serialize_sql_entities(entities));
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website catalog: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website catalog: {}", error.what());
    return 3;
}

/// @brief Runs `website render`: renders a template against a JSON context.
///
/// Reads a Jinja template and a JSON page-data context (for example the
/// cpp_entities/plan artifact) and writes the rendered document (§14). The
/// structured render primitive entity/tutorial/landing pages are produced with.
/// @param args Options after the `render` token.
/// @return Zero on success; two on usage errors; three on render failure.
[[nodiscard]] int run_render(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {"template", "context", "output"};
    const auto options = parse_options(args, value_keys, {});
    const fs::path template_path = require_option(options, "template");
    const fs::path context_path = require_option(options, "context");
    const fs::path output = require_option(options, "output");
    const std::string document =
        render_template_json(read_file(template_path), read_file(context_path));
    write_if_different(output, document);
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website render: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website render: {}", error.what());
    return 3;
}

namespace {

// One static file discovered under the immutable asset root, with its content
// and per-file SHA-256, used to derive the content-addressed bundle hash (§16).
struct static_asset {
    std::string relative_path;  // normalized forward-slash path under static/
    std::string content;
    std::string sha256;
};

// Walks main/website/static and hashes every regular file. The bundle hash is
// SHA-256 over sorted `<relpath>\0<file-sha256>\n` records so a byte change in
// any asset (or a rename) yields a new immutable /assets/<hash>/ directory.
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
        asset.content = read_file(entry.path());
        asset.sha256 = bioseq::codegen::sha256_hex(asset.content);
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
    bundle_sha256 = bioseq::codegen::sha256_hex(manifest_bytes);
    return assets;
}

// Collects every regular file under root as site-relative "/" routes. Rejects
// symlinks and special files so publish set-equality can never bless a tree that
// smuggles a non-regular entry (§16).
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

// Flushes a file's bytes to stable storage (§14 durable write-ahead ordering).
void fsync_file(const fs::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor >= 0) {
        ::fsync(descriptor);
        ::close(descriptor);
    }
}

// Flushes a directory entry (a rename/create) to stable storage (§14).
void fsync_directory(const fs::path& path) {
    const int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (descriptor >= 0) {
        ::fsync(descriptor);
        ::close(descriptor);
    }
}

// Atomically replaces output with staging (§14). When output is absent the
// staging directory is renamed into place; when it exists the two sibling
// directories are exchanged with renameat2(RENAME_EXCHANGE) and the now-old tree
// is removed, so a reader never observes a partially written site.
void atomic_publish(const fs::path& staging, const fs::path& output) {
    std::error_code error;
    if (!fs::exists(output, error)) {
        fs::rename(staging, output, error);
        if (error) {
            throw std::runtime_error("publish rename failed: " + error.message());
        }
        return;
    }
    const long result = ::syscall(SYS_renameat2, AT_FDCWD, staging.c_str(),
                                  AT_FDCWD, output.c_str(),
                                  static_cast<unsigned int>(RENAME_EXCHANGE));
    if (result != 0) {
        throw std::runtime_error(std::string("renameat2 exchange failed: ")
                                 + std::strerror(errno));
    }
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

// Fails the build if a generated HTML page violates the §14/§15.4/§15.5/§20.7
// invariants: no unrendered template placeholder, no javascript: URL, no inline
// event handler or style, no external script/stylesheet resource, exactly one
// <h1>, the meta CSP present, and frame-ancestors kept out of the meta element.
void validate_generated_html(std::string_view route, std::string_view html) {
    const auto fail = [&](std::string_view why) {
        throw std::runtime_error("generated HTML validation failed for "
                                 + std::string(route) + ": " + std::string(why));
    };
    if (html.find("{{") != std::string_view::npos
        || html.find("}}") != std::string_view::npos) {
        fail("contains an unrendered template placeholder");
    }
    if (html.find("javascript:") != std::string_view::npos) {
        fail("contains a javascript: URL");
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
    if (html.find("src=\"http") != std::string_view::npos
        || html.find("src='http") != std::string_view::npos) {
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

// Writes one rendered artifact into staging at a site-relative "/" route,
// recording its route and content SHA-256 for the manifest. The manifest itself
// is written afterward and is never one of these entries (§16).
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
        manifest.emplace(std::string(route), bioseq::codegen::sha256_hex(content));
    }
};

}  // namespace

/// @brief Runs `website publish`: renders the full content-addressed site tree.
///
/// Rebuilds the plan from source, reads the C++/SQL entity artifacts, content-
/// addresses main/website/static into one immutable /assets/<sha256>/ bundle,
/// renders every §16 route through the shared template frame, writes the site
/// manifest (SHA-256 of every output but itself), validates exact set equality
/// between the manifest and the staged tree, and atomically swaps staging over
/// the output root (§14, §16).
/// @param args Options after the `publish` token.
/// @return Zero on success; two on usage errors; three on parse/validation.
[[nodiscard]] int run_publish(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {
        "source", "version", "site-config", "cpp-entities", "sql-entities",
        "templates", "static", "output", "base-path", "source-revision"};
    static constexpr std::string_view flag_keys[] = {"include-drafts"};
    const auto options = parse_options(args, value_keys, flag_keys);
    const fs::path source = require_option(options, "source");
    const std::string version_string = require_option(options, "version");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path templates_dir = require_option(options, "templates");
    const fs::path static_dir = require_option(options, "static");
    const fs::path output = require_option(options, "output");
    const bool include_drafts = options.contains("include-drafts");

    // Crash recovery (§14): a prepublish descriptor beside output means a prior
    // publish was interrupted after the write-ahead stamp. If output already matches
    // the descriptor's manifest (hash + exact set), complete the publish by writing
    // the success marker and dropping the descriptor; otherwise clear only the
    // nonce-owned transient state and fail so the build DAG recreates staging.
    {
        const fs::path prepublish = output.string() + ".prepublish.json";
        std::error_code probe;
        if (fs::exists(prepublish, probe) && !probe) {
            const std::string descriptor = read_file(prepublish);
            const std::string expected =
                manifest_string_field(descriptor, "new_manifest_sha256");
            const fs::path site_manifest = output / "site-manifest.json";
            bool output_valid = false;
            if (!expected.empty() && fs::exists(site_manifest, probe) && !probe) {
                const std::string manifest_text = read_file(site_manifest);
                if (bioseq::codegen::sha256_hex(manifest_text) == expected) {
                    std::set<std::string> present;
                    collect_site_files(output, present);
                    present.erase("/site-manifest.json");
                    std::set<std::string> declared;
                    for (const std::string& declared_route :
                         manifest_output_routes(manifest_text)) {
                        declared.insert(declared_route);
                    }
                    output_valid = (present == declared);
                }
            }
            std::error_code cleanup;
            const fs::path leftover_staging = output.string() + ".staging";
            fs::remove_all(leftover_staging, cleanup);
            fs::remove(leftover_staging.string() + ".owner.json", cleanup);
            if (output_valid) {
                write_if_different(output.string() + ".success", expected + "\n");
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

    const std::string site_yaml = read_file(site_config_path);
    const site_configuration config =
        parse_site_config(site_yaml, site_config_path.generic_string());
    std::string base_path = config.default_base_path;
    if (const auto found = options.find("base-path"); found != options.end()) {
        base_path = found->second;
    }
    std::string revision;
    if (const auto found = options.find("source-revision"); found != options.end()) {
        revision = found->second;
    }

    const website_plan plan = build_plan_from_source(
        source, version_string, config, site_yaml,
        plan_relative(site_config_path, source), base_path, revision, include_drafts);

    std::vector<documented_entity> cpp_entities;
    if (const auto found = options.find("cpp-entities"); found != options.end()) {
        cpp_entities = deserialize_entities(read_file(found->second));
    }
    std::vector<documented_entity> sql_entities;
    if (const auto found = options.find("sql-entities"); found != options.end()) {
        sql_entities = deserialize_entities(read_file(found->second));
    }

    const std::string version = version_string;
    const auto url = [&](std::string_view rel) { return base_path + std::string(rel); };
    const auto route = [&](std::string_view rel) { return "/" + std::string(rel); };
    const auto canonical = [&](std::string_view rel) {
        std::string trimmed(rel);
        if (trimmed.ends_with("index.html")) {
            trimmed.erase(trimmed.size() - std::string_view("index.html").size());
        }
        return config.canonical_origin + url(trimmed);
    };

    // Content-address the immutable static bundle and stage it (§16).
    std::string bundle_sha256;
    const std::vector<static_asset> assets =
        collect_static_assets(static_dir, bundle_sha256);
    const std::string bundle_prefix = "assets/" + bundle_sha256 + "/";

    const fs::path staging = output.string() + ".staging";
    std::error_code staging_error;
    fs::remove_all(staging, staging_error);
    fs::create_directories(staging);
    staging_writer writer{.root = staging, .manifest = {}};
    for (const static_asset& asset : assets) {
        writer.emit(route(bundle_prefix + asset.relative_path), asset.content);
    }

    // Resolve the base-aware site view bound into every page's frame (§15.1).
    site_view site;
    site.title = config.title;
    site.base_path = base_path;
    site.repository_url = config.repository_url;
    site.copyright_holder = config.copyright_holder;
    site.copyright_year = config.copyright_year;
    site.status_label = config.status_label;
    site.version = version;
    site.css_url = url(bundle_prefix + "css/site.css");
    site.js_url = url(bundle_prefix + "js/main.js");
    site.favicon_url = url(bundle_prefix + "favicon.svg");
    site.documentation_url = url(version + "/");
    site.tutorials_url = url(version + "/tutorials/");
    site.reference_url = url(version + "/reference/");
    site.nav_documentation = config.navigation.at("documentation");
    site.nav_tutorials = config.navigation.at("tutorials");
    site.nav_reference = config.navigation.at("reference");

    const auto render = [&](std::string_view rel, std::string_view template_name,
                            std::string_view page_object, std::string_view key,
                            std::string_view collection) {
        const std::string context =
            build_page_context(site, page_object, key, collection);
        const std::string html =
            render_page(templates_dir.string(), template_name, context);
        writer.emit(route(rel), html);
    };

    // Assign each entity a readable, collision-checked route (§12.1): a domain and
    // kind path plus a slug and the 16-hex fnv1a_64 of the (Doxygen/OID-independent)
    // canonical ID. Both the canonical ID and the output path are checked for
    // collisions before any page is rendered, per §12.1.
    const auto kind_token = [](entity_kind kind) -> std::string {
        switch (kind) {
        case entity_kind::namespace_: return "namespace";
        case entity_kind::module: return "module";
        case entity_kind::class_: return "class";
        case entity_kind::struct_: return "struct";
        case entity_kind::union_: return "union";
        case entity_kind::enum_: return "enum";
        case entity_kind::enum_value: return "enumerator";
        case entity_kind::concept_: return "concept";
        case entity_kind::type_alias: return "type_alias";
        case entity_kind::function: return "function";
        case entity_kind::variable: return "variable";
        case entity_kind::sql_schema: return "schema";
        case entity_kind::sql_type: return "type";
        case entity_kind::sql_function: return "function";
        case entity_kind::overload_group: return "overload_group";
        }
        return "entity";
    };
    const auto slugify = [](std::string_view qualified) -> std::string {
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
    };
    const auto entity_rel = [&](const documented_entity& entity) {
        const std::string domain = entity.domain == entity_domain::sql ? "sql" : "cpp";
        return version + "/reference/" + domain + "/" + kind_token(entity.kind) + "/"
            + slugify(entity.qualified_name) + "-"
            + hex64(bioseq::common::fnv1a_64(entity.canonical_id)) + "/";
    };
    std::set<std::string> seen_routes;
    std::set<std::string> seen_ids;
    const auto assign_routes = [&](std::vector<documented_entity>& list) {
        for (documented_entity& entity : list) {
            if (!seen_ids.insert(entity.canonical_id).second) {
                throw std::runtime_error("duplicate canonical id: " + entity.canonical_id);
            }
            const std::string rel = entity_rel(entity);
            if (!seen_routes.insert(rel).second) {
                throw std::runtime_error("entity route collision at " + rel);
            }
            entity.route_path = url(rel);
        }
    };
    assign_routes(cpp_entities);
    assign_routes(sql_entities);

    // Root marketing landing page for the primary version (§16).
    render("index.html", "landing.html.j2",
           "{\"title\":" + json_text(config.title)
               + ",\"description\":" + json_text(config.short_description)
               + ",\"canonical\":" + json_text(canonical("index.html"))
               + ",\"eyebrow\":" + json_text("PostgreSQL-native")
               + ",\"headline\":" + json_text(config.title)
               + ",\"lede\":" + json_text(config.short_description)
               + ",\"install\":" + json_text("CREATE EXTENSION bioseq;") + "}",
           "", "");

    // Version overview / documentation home, distinct from the landing page.
    render(version + "/index.html", "version_home.html.j2",
           "{\"title\":" + json_text(config.title + " " + version)
               + ",\"description\":" + json_text(config.short_description)
               + ",\"canonical\":" + json_text(canonical(version + "/index.html"))
               + ",\"coverage_url\":"
               + json_text(url(version + "/documentation-coverage/")) + "}",
           "", "");

    // Tutorials index and one page per published tutorial (§10, §16).
    std::string tutorial_cards = "[";
    for (std::size_t index = 0; index < plan.tutorials.size(); ++index) {
        const tutorial& item = plan.tutorials[index];
        if (index > 0) {
            tutorial_cards.push_back(',');
        }
        tutorial_cards += "{\"route\":"
            + json_text(url(version + "/tutorials/" + item.metadata.slug + "/"))
            + ",\"title\":" + json_text(item.metadata.title)
            + ",\"description\":" + json_text(item.metadata.description) + "}";
    }
    tutorial_cards.push_back(']');
    render(version + "/tutorials/index.html", "tutorials_index.html.j2",
           "{\"title\":" + json_text("Tutorials")
               + ",\"description\":" + json_text("Bioseq tutorials for " + version)
               + ",\"canonical\":"
               + json_text(canonical(version + "/tutorials/index.html")) + "}",
           "tutorials", tutorial_cards);
    for (const tutorial& item : plan.tutorials) {
        const std::string rel =
            version + "/tutorials/" + item.metadata.slug + "/index.html";
        const std::string body = render_document_html(item.content);
        std::string page = "{\"title\":" + json_text(item.metadata.title)
            + ",\"description\":" + json_text(item.metadata.description)
            + ",\"canonical\":" + json_text(canonical(rel))
            + ",\"body_html\":" + json_quote(body);
        page += ",\"previous_url\":";
        page += item.previous_slug
            ? json_text(url(version + "/tutorials/" + *item.previous_slug + "/"))
            : "null";
        page += ",\"next_url\":";
        page += item.next_slug
            ? json_text(url(version + "/tutorials/" + *item.next_slug + "/"))
            : "null";
        page.push_back('}');
        render(rel, "tutorial.html.j2", page, "", "");
    }

    // Reference landing plus per-domain entity indexes and entity pages (§12.3).
    render(version + "/reference/index.html", "reference_index.html.j2",
           "{\"title\":" + json_text("API Reference")
               + ",\"description\":" + json_text("Bioseq C++ and SQL API reference")
               + ",\"canonical\":"
               + json_text(canonical(version + "/reference/index.html"))
               + ",\"cpp_url\":" + json_text(url(version + "/reference/cpp/"))
               + ",\"sql_url\":" + json_text(url(version + "/reference/sql/")) + "}",
           "", "");
    const auto render_domain = [&](std::string_view domain, std::string_view label,
                                   const std::vector<documented_entity>& entities) {
        const std::string index_rel =
            version + "/reference/" + std::string(domain) + "/index.html";
        render(index_rel, "entity_index.html.j2",
               "{\"title\":" + json_text(std::string(label) + " API")
                   + ",\"description\":"
                   + json_text(std::string(label) + " API reference")
                   + ",\"canonical\":" + json_text(canonical(index_rel)) + "}",
               "entities", serialize_entity_array(entities));
        for (const documented_entity& entity : entities) {
            const std::string rel = entity_rel(entity) + "index.html";
            render(rel, "entity.html.j2",
                   "{\"title\":" + json_text(entity.qualified_name)
                       + ",\"description\":"
                       + json_text(entity.summary_text.empty()
                                        ? entity.qualified_name : entity.summary_text)
                       + ",\"canonical\":" + json_text(canonical(rel)) + "}",
                   "entity", serialize_single_entity(entity));
        }
    };
    render_domain("cpp", "C++", cpp_entities);
    render_domain("sql", "SQL", sql_entities);

    // Documentation-coverage page and machine-readable report (§11.4).
    const coverage_report coverage = compute_coverage(
        cpp_entities, config.required_documentation_entities, {});
    std::string missing_json = "[";
    for (std::size_t index = 0; index < coverage.missing_summary.size(); ++index) {
        if (index > 0) {
            missing_json.push_back(',');
        }
        missing_json += json_text(coverage.missing_summary[index]);
    }
    missing_json.push_back(']');
    render(version + "/documentation-coverage/index.html", "coverage.html.j2",
           "{\"title\":" + json_text("Documentation coverage")
               + ",\"description\":" + json_text("Public API documentation coverage")
               + ",\"canonical\":"
               + json_text(canonical(version + "/documentation-coverage/index.html"))
               + ",\"public_total\":" + std::to_string(coverage.public_total)
               + ",\"documented_total\":" + std::to_string(coverage.documented_total)
               + ",\"missing\":" + missing_json + "}",
           "", "");
    writer.emit(route(version + "/documentation-coverage.json"),
                serialize_coverage(coverage.public_total, coverage.documented_total,
                                   coverage.missing_summary, coverage.fatal_missing));

    // Versions manifest, versions page, and per-version search index (§15.1, §12.5).
    const std::vector<std::string> version_labels{version};
    writer.emit(route("versions.json"),
                serialize_versions(version_labels, base_path));
    render("versions/index.html", "versions.html.j2",
           "{\"title\":" + json_text("Versions")
               + ",\"description\":" + json_text("Documented Bioseq versions")
               + ",\"canonical\":" + json_text(canonical("versions/index.html")) + "}",
           "versions",
           "[{\"route\":" + json_text(url(version + "/")) + ",\"label\":"
               + json_text(version) + "}]");
    std::vector<documented_entity> all_entities = cpp_entities;
    all_entities.insert(all_entities.end(), sql_entities.begin(), sql_entities.end());
    writer.emit(route(version + "/search-index.json"),
                build_search_index(all_entities));
    // Content-addressed per-version tooltip dataset (§12.4, §16): named by its own
    // SHA-256 so its immutable bytes are long-cacheable.
    const std::string tooltip_json = serialize_entity_tooltips(all_entities);
    writer.emit(route(version + "/entities." + bioseq::codegen::sha256_hex(tooltip_json)
                          + ".json"),
                tooltip_json);
    if (!sql_entities.empty()) {
        writer.emit(route(version + "/catalog-inventory.json"),
                    serialize_sql_entities(sql_entities));
    }

    // Host-agnostic SEO / security deployment artifacts (§15.5).
    writer.emit(route("robots.txt"),
                "User-agent: *\nAllow: /\nSitemap: " + config.canonical_origin
                    + url("sitemap.xml") + "\n");
    writer.emit(route("security-headers.txt"),
                "Content-Security-Policy: default-src 'self'; script-src 'self'; "
                "style-src 'self'; font-src 'self'; img-src 'self' data:; "
                "media-src 'self'; connect-src 'self'; object-src 'none'; "
                "base-uri 'self'; form-action 'none'; frame-ancestors 'none'\n"
                "X-Content-Type-Options: nosniff\n"
                "Referrer-Policy: strict-origin-when-cross-origin\n"
                "Permissions-Policy: geolocation=(), microphone=(), camera=()\n");
    std::string sitemap =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<urlset xmlns=\"http://www.sitemaps.org/schemas/sitemap/0.9\">\n";
    for (const auto& [manifest_route, digest] : writer.manifest) {
        if (!manifest_route.ends_with("index.html")) {
            continue;
        }
        std::string rel = manifest_route.substr(1);
        sitemap += "  <url><loc>" + canonical(rel) + "</loc></url>\n";
    }
    sitemap += "</urlset>\n";
    // The sitemap enumerates the pages produced above; it lists itself as a file
    // but not as a <url>, matching the canonical page set.
    writer.emit(route("sitemap.xml"), sitemap);

    // Serialize the manifest last, then require exact set equality (§16).
    {
        std::string manifest_json;
        manifest_json += "{\n  \"schema\": \"bioseq.website.site_manifest/v1\",\n";
        manifest_json += "  \"hash_algorithm\": \"sha256\",\n";
        manifest_json += "  \"version\": \"" + version + "\",\n";
        manifest_json += "  \"source_revision\": \"" + revision + "\",\n";
        manifest_json += "  \"base_path\": \"" + base_path + "\",\n";
        manifest_json += "  \"generator_signature\": \"" + plan.signature + "\",\n";
        manifest_json += "  \"parser_version\": \"" + plan.parser_version + "\",\n";
        manifest_json += "  \"binding_schema\": \"" + plan.binding_schema_version + "\",\n";
        manifest_json += "  \"asset_bundle_sha256\": \"" + bundle_sha256 + "\",\n";
        // The live PostgreSQL server/extension identity is captured by the catalog
        // stage; publish does not open a cluster, so it records null placeholders
        // that the assembled/full-pipeline manifest fills from that artifact.
        manifest_json += "  \"postgres_server_version\": null,\n";
        manifest_json += "  \"extension_version\": null,\n";
        manifest_json += "  \"staged_extension_sha256\": null,\n";
        manifest_json += "  \"inputs\": [\n";
        for (std::size_t index = 0; index < plan.inputs.size(); ++index) {
            manifest_json += "    {\"path\": \"" + plan.inputs[index].path
                + "\", \"content_hash\": \"" + plan.inputs[index].content_hash + "\"}";
            manifest_json += (index + 1 == plan.inputs.size()) ? "\n" : ",\n";
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

    // Durable write-ahead publish (§14): stamp an fsynced 0600 prepublish
    // descriptor plus an owner marker adjacent to staging, then swap atomically,
    // then record the success marker and drop the descriptor/owner. A crash between
    // any two steps is resolved by the crash-recovery pass on the next run.
    const std::string manifest_hash =
        bioseq::codegen::sha256_hex(read_file(staging / "site-manifest.json"));
    const std::string nonce =
        std::to_string(::getpid()) + "-" + manifest_hash.substr(0, 16);
    const fs::path owner_marker = staging.string() + ".owner.json";
    write_private_artifact(owner_marker,
        "{\"schema\":\"bioseq.website.owner/v1\",\"nonce\":\"" + nonce
        + "\",\"staging\":\"" + staging.filename().string() + "\"}\n");
    const fs::path prepublish = output.string() + ".prepublish.json";
    write_private_artifact(prepublish,
        "{\n  \"schema\": \"bioseq.website.prepublish/v1\",\n"
        "  \"operation\": \"publish\",\n  \"nonce\": \"" + nonce + "\",\n"
        "  \"staging\": \"" + staging.filename().string() + "\",\n"
        "  \"new_manifest_sha256\": \"" + manifest_hash + "\",\n"
        "  \"validation_stamps\": []\n}\n");
    fsync_file(prepublish);
    fsync_directory(parent_or_dot(output));

    atomic_publish(staging, output);
    fsync_directory(parent_or_dot(output));

    write_if_different(output.string() + ".success", manifest_hash + "\n");
    fsync_file(output.string() + ".success");
    std::error_code cleanup;
    fs::remove(prepublish, cleanup);
    fs::remove(owner_marker, cleanup);
    fsync_directory(parent_or_dot(output));
    std::println("published {} outputs to {} (bundle {})",
                 writer.manifest.size(), output.string(), bundle_sha256);
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website publish: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website publish: {}", error.what());
    return 3;
}

/// @brief Runs `website assemble`: combines verified single-version sites (§16).
///
/// Verifies every input site root against its sibling `<root>.success` marker
/// before copying anything, combines the per-version subtrees and asset bundles
/// (root-level files come from the newest/primary version), regenerates the
/// cross-version versions.json, and writes the immutable assembled manifest
/// (schema assembled_manifest/v1) with the same exact set-equality rule before an
/// atomic swap into place.
/// @param args Options after the `assemble` token.
/// @return Zero on success; two on usage errors; three on validation failure.
[[nodiscard]] int run_assemble(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {"site-roots", "output"};
    const auto options = parse_options(args, value_keys, {});
    const std::string roots_list = require_option(options, "site-roots");
    const fs::path output = require_option(options, "output");

    std::vector<fs::path> roots;
    std::size_t begin = 0;
    while (begin <= roots_list.size()) {
        const std::size_t end = roots_list.find(';', begin);
        const std::size_t stop = end == std::string::npos ? roots_list.size() : end;
        std::string entry = trim(roots_list.substr(begin, stop - begin));
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
        const std::string manifest_text = read_file(root / "site-manifest.json");
        const std::string manifest_hash = bioseq::codegen::sha256_hex(manifest_text);
        const fs::path marker = root.string() + ".success";
        std::error_code error;
        if (!fs::exists(marker, error) || error) {
            throw std::runtime_error("missing success marker for " + root.string());
        }
        const std::string marker_bytes = read_file(marker);
        std::string marker_hash = trim(marker_bytes);
        if (marker_hash != manifest_hash) {
            throw std::runtime_error("success marker does not match site-manifest.json for "
                                     + root.string());
        }
        input_site site;
        site.root = root;
        site.version = manifest_string_field(manifest_text, "version");
        site.base_path = manifest_string_field(manifest_text, "base_path");
        site.outputs = manifest_output_routes(manifest_text);
        site.manifest_hash = manifest_hash;
        site.success_hash = bioseq::codegen::sha256_hex(marker_bytes);
        site.parts = parse_version_parts(site.version);
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
            const bool versioned =
                route.starts_with(version_prefix) || route.starts_with("/assets/");
            if (!versioned && !is_primary) {
                continue;
            }
            writer.emit(route, read_file(site.root / route.substr(1)));
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
    writer.emit("/versions.json", serialize_versions(version_labels, base_path));

    std::vector<std::pair<std::string, std::string>> outputs(
        writer.manifest.begin(), writer.manifest.end());
    const std::string assembled = serialize_assembled_manifest(
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
        bioseq::codegen::sha256_hex(read_file(output / "site-manifest.json"));
    write_if_different(output.string() + ".success", manifest_hash + "\n");
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

/// @brief Runs `website source_graph`: Clang export-import reachable closure (§11.3).
///
/// Scans each reachable module interface unit for its provided module and its
/// `export import` (public) versus plain `import` (internal) edges, then computes
/// the closure reachable from the configured primary modules by following only
/// public edges into other Bioseq-owned modules. Plain imports never create a
/// public edge; an exported edge to a missing Bioseq module is fatal. Emits the
/// roots, edges, and sorted reachable closure as source_graph/v1.
/// @param args Options after the `source_graph` token.
/// @return Zero on success; two on usage errors; three on validation failure.
[[nodiscard]] int run_source_graph(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {
        "reachable-units", "compile-db", "site-config", "output"};
    const auto options = parse_options(args, value_keys, {});
    const fs::path reachable_units = require_option(options, "reachable-units");
    const fs::path compile_db = require_option(options, "compile-db");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path output = require_option(options, "output");

    const site_configuration config =
        parse_site_config(read_file(site_config_path), site_config_path.generic_string());

    std::vector<fs::path> units;
    for (const std::string& line : read_lines(reachable_units)) {
        units.emplace_back(line);
    }
    const std::vector<module_graph_unit> scanned =
        scan_module_graph(units, compile_db);

    // Map each provided module to its unit; duplicate providers are fatal (§11.3).
    std::map<std::string, module_graph_unit, std::less<>> universe;
    for (const module_graph_unit& unit : scanned) {
        if (unit.provided_module.empty()) {
            continue;
        }
        if (!universe.emplace(unit.provided_module, unit).second) {
            throw std::runtime_error("duplicate module provider: " + unit.provided_module);
        }
    }

    // Seed the closure with the configured primary modules; each must be present.
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

    // Follow only public (export import) edges into other Bioseq-owned modules. A
    // "bioseq."-prefixed exported edge with no provider is fatal; std/third-party
    // exported edges remain external dependencies and are not part of the closure.
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

    write_if_different(output,
                       serialize_source_graph(clang_api_version(), roots, closure));
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
/// @return Exit code per §7: 2 usage, 3 parse/validation, 0 success.
[[nodiscard]] int dispatch_website(std::span<std::string_view const> args) {
    if (args.empty()) {
        std::println(stderr, "usage: bioseq_codegen website <command> [options]");
        return 2;
    }
    const std::string_view command = args.front();
    const std::span<std::string_view const> rest = args.subspan(1);
    if (command == "plan") {
        return run_plan(rest);
    }
    if (command == "cpp_index") {
        return run_cpp_index(rest);
    }
    if (command == "execute") {
        return run_execute(rest);
    }
    if (command == "render") {
        return run_render(rest);
    }
    if (command == "catalog") {
        return run_catalog(rest);
    }
    if (command == "publish") {
        return run_publish(rest);
    }
    if (command == "assemble") {
        return run_assemble(rest);
    }
    if (command == "source_graph") {
        return run_source_graph(rest);
    }
    std::println(stderr, "unknown website command: {}", command);
    return 2;
}

}  // namespace bioseq::codegen::website
