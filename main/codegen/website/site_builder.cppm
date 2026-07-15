module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <print>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sys/stat.h>

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
        .path = plan_relative(site_config_path, source),
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
    plan.base_path = base_path;
    plan.source_revision = revision;
    plan.site_title = config.title;
    plan.tutorials = std::move(tutorials);
    plan.inputs = std::move(inputs);
    plan.signature = hex64(signature);

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
    static constexpr std::string_view known[] = {
        "source_graph", "cpp_index", "execute", "render", "publish", "assemble"};
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
