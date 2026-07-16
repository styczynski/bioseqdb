module;

#include <algorithm>
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

// Defined after the reference index it exercises; driven by run_website_smoke.
void run_website_reference_smoke();

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
    expect(merged.size() == 1
               && merged.front().canonical_id == "cpp:struct:bioseq::io::seq"
               && merged.front().summary_text == "sequence",
           "entity merge attaches docs by normalized signature");

    // canonical IDs (§12.1): domain- and kind-tagged, `::`-preserving, and
    // signature-keyed for functions so overloads (e.g. two constructors) stay
    // distinct. Golden-locked here alongside the merge above.
    documented_entity ctor_default;
    ctor_default.domain = entity_domain::cpp;
    ctor_default.kind = entity_kind::function;
    ctor_default.qualified_name = "bioseq::io::view::view";
    ctor_default.normalized_signature = "bioseq::io::view::view()";
    documented_entity ctor_args;
    ctor_args.domain = entity_domain::cpp;
    ctor_args.kind = entity_kind::function;
    ctor_args.qualified_name = "bioseq::io::view::view";
    ctor_args.normalized_signature = "bioseq::io::view::view(const char *, std::size_t)";
    expect(canonical_entity_id(ctor_default) == "cpp:function:bioseq::io::view::view()"
               && canonical_entity_id(ctor_args)
                      == "cpp:function:bioseq::io::view::view(const char *, std::size_t)"
               && canonical_entity_id(ctor_default) != canonical_entity_id(ctor_args),
           "canonical id keys function overloads by normalized signature");

    // coverage (§11.4): a required-but-undocumented entity is fatal-listed.
    documented_entity undocumented;
    undocumented.domain = entity_domain::cpp;
    undocumented.kind = entity_kind::struct_;
    undocumented.qualified_name = "bioseq::io::internal";
    undocumented.canonical_id = "cpp:struct:bioseq::io::internal";
    const auto coverage = compute_coverage(
        {merged.front(), undocumented}, {"cpp:struct:bioseq::io::internal"}, {});
    expect(coverage.public_total == 2 && coverage.documented_total == 1
               && coverage.fatal_missing.size() == 1,
           "coverage flags required undocumented entity");

    // sql_api.yaml allowlist parsing (§9/§12.2): a closed schema of unique nonempty
    // sequences - unquoted identifiers and exact canonical IDs.
    const sql_api_configuration api = parse_sql_api(
        "schema: bioseq.website.sql_api/v1\n"
        "include_schemas: [bioseq, public]\n"
        "public_entities:\n"
        "  - sql:type:bioseq.seq\n"
        "  - sql:function:bioseq.len(bioseq.seq)\n",
        "sql_api.yaml");
    expect(api.include_schemas.size() == 2 && api.public_entities.size() == 2
               && api.include_schemas.front() == "bioseq"
               && api.public_entities.back() == "sql:function:bioseq.len(bioseq.seq)",
           "sql_api.yaml parses include schemas and public entities");
    const auto sql_api_rejects = [](std::string_view yaml) {
        try {
            (void)parse_sql_api(yaml, "sql_api.yaml");
            return false;
        } catch (const std::exception&) {
            return true;
        }
    };
    expect(sql_api_rejects("schema: s\ninclude_schemas: [bioseq]\n"
                           "public_entities: [sql:type:bioseq.seq]\nsurprise: 1\n")
               && sql_api_rejects("schema: s\ninclude_schemas: [bioseq]\n"
                                  "public_entities: [sql:type:bioseq.seq, "
                                  "sql:type:bioseq.seq]\n")
               && sql_api_rejects("schema: s\ninclude_schemas: []\n"
                                  "public_entities: [sql:type:bioseq.seq]\n"),
           "sql_api.yaml rejects unknown keys, duplicates, and empty sequences");

    // Overload-group routing/rendering is checked in run_website_reference_smoke,
    // defined after the reference index it exercises.
    run_website_reference_smoke();

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
    search_entry probe_entry = search_entry_from_entity(merged.front());
    probe_entry.route = "/v0.1/reference/cpp/struct/seq/";
    const std::string search_json = build_search_index({probe_entry});
    expect(search_json.ends_with("\n"), "search json newline");
    expect(search_json.find("\"name_tokens\"") != std::string::npos,
           "search record carries normalized name tokens");
    const std::string escaped = apply_html_safe_escapes("a<b>&c");
    expect(escaped.find("\\u003c") != std::string::npos, "json html-safe escape");

    // §12.5 shared normalizer golden corpus (mirrors
    // main/website/tests/fixtures/search_tokens_golden.json, also driven against the
    // JS normalizer in browser_test.py). Byte-preserving, ASCII-only casing.
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

    // Sort-by-route + duplicate-id rejection in the index builder.
    const std::string ordered = build_search_index(
        {search_entry{.id = "b", .route = "/z/"}, search_entry{.id = "a", .route = "/a/"}});
    expect(ordered.find("\"/a/\"") < ordered.find("\"/z/\""),
           "records sort by route");
    bool duplicate_rejected = false;
    try {
        static_cast<void>(build_search_index(
            {search_entry{.id = "dup", .route = "/a/"},
             search_entry{.id = "dup", .route = "/b/"}}));
    } catch (const std::exception&) {
        duplicate_rejected = true;
    }
    expect(duplicate_rejected, "duplicate search id is fatal");

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

// Rewrites each runnable SQL code block so only its visible phase is rendered.
// A runnable fence's body carries the hidden setup/teardown SQL that the executor
// needs but that must never reach the published page (§5.1); this runs after the
// example has been collected, on the render-only copy of the document.
void strip_runnable_fence_hidden_sql(std::vector<document_node>& nodes) {
    for (document_node& node : nodes) {
        if (node.kind == document_node_kind::code_block) {
            const auto language = node.attributes.find("language");
            const auto info = node.attributes.find("info");
            if (language != node.attributes.end() && language->second == "sql"
                && info != node.attributes.end()
                && parse_runnable_fence(info->second)) {
                node.text = split_sql_phases(node.text).visible;
            }
        }
        strip_runnable_fence_hidden_sql(node.children);
    }
}

// Processes §10.6 annotation directives over a tutorial's top-level blocks (fences
// are top-level only, §10.5). Each `annotation target=... match=... title=...`
// directive is matched against a PRIOR displayed block's decoded source and folded
// into that block as a numbered code_annotation child (source range + parsed
// commentary); the directive node is then removed from the render tree. A block
// flagged `annotated` must gain at least one annotation, and a target's ranges may
// not overlap - all fatal.
void process_annotations(std::vector<document_node>& nodes, const std::string& where) {
    struct block_ref {
        std::string id;
        std::size_t node_index;
        std::string text;   // decoded, displayed source (visible phase for runnable)
        bool annotated;
        std::vector<std::pair<std::size_t, std::size_t>> ranges;  // start,len
        std::size_t count;  // annotations attached so far
    };
    std::vector<block_ref> blocks;
    const auto find_block = [&](const std::string& id) -> block_ref* {
        for (block_ref& block : blocks) {
            if (block.id == id) {
                return &block;
            }
        }
        return nullptr;
    };
    // Pass 1: index every displayed block (ordinary/runnable) in document order.
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const document_node& node = nodes[index];
        if (node.kind != document_node_kind::code_block) {
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
            ? split_sql_phases(node.text).visible : node.text;
        blocks.push_back(block_ref{.id = fence.id, .node_index = index,
                                   .text = std::move(text), .annotated = fence.annotated,
                                   .ranges = {}, .count = 0});
    }
    // Pass 2: fold each annotation directive into its (prior) target block.
    std::vector<std::size_t> directive_nodes;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const document_node& node = nodes[index];
        if (node.kind != document_node_kind::code_block) {
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
        document_node commentary = parse_markdown(node.text);
        for (const document_node& child : commentary.children) {
            if (child.kind != document_node_kind::code_block) {
                continue;
            }
            const auto child_info = child.attributes.find("info");
            if (child_info != child.attributes.end() && !child_info->second.empty()
                && parse_fence_info(child_info->second).kind != fence_kind::ordinary) {
                throw std::runtime_error("annotation commentary may not contain runnable "
                    "SQL or another annotation in " + where);
            }
        }
        document_node annotation;
        annotation.kind = document_node_kind::code_annotation;
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

// Parses the landing frontmatter `slots:` mapping (§15.2): each entry maps a
// template slot name to the landing-page-local id of a runnable block. Returns the
// mapping in declared order. The YAML here is a flat two-space-indented block of
// `slot_name: block_id` lines directly under a `slots:` key.
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
        const std::string trimmed = trim(std::string(line));
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
            std::string slot = trim(trimmed.substr(0, colon));
            std::string block = trim(trimmed.substr(colon + 1));
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

// Executes the landing page's runnable slots (§15.2) into database-independent
// views. The template slot name comes from the authoritative frontmatter `slots:`
// mapping, and every §15.2 binding error is fatal: a slot referencing an unknown
// block id, a block referenced by no slot, or a duplicate slot/block. When no
// connection is configured, each slot renders its visible SQL with no result table;
// when a connection is given, a failing example fails the build. Hidden
// setup/teardown never leaves the returned model.
[[nodiscard]] std::vector<landing_example_view> collect_landing_views(
    const fs::path& source, const std::string& version_string,
    const std::map<std::string, std::string, std::less<>>& options) {
    const fs::path landing_md = source / version_string / "landing_examples.md";
    std::error_code probe;
    if (!fs::exists(landing_md, probe) || probe) {
        return {};
    }
    const front_matter_split split = split_front_matter(read_file(landing_md));
    const document_node document = parse_markdown(split.body);
    std::vector<sql_example> examples;
    std::vector<unresolved_entity_query> queries;
    collect_examples_and_queries(document, "landing", version_string, examples, queries);

    // Map block id -> example, rejecting duplicate block ids (§15.2).
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
    std::optional<isolation_settings> isolation;
    std::string conninfo;
    if (options.contains("isolated")) {
        isolation_settings settings;
        settings.admin_conninfo = require_option(options, "admin-conninfo");
        settings.socket_directory = require_option(options, "socket-dir");
        settings.port = std::stoi(require_option(options, "port"));
        settings.template_database = require_option(options, "template-db");
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
            if (isolation) {
                private_cluster_executor executor("");
                outcome = executor.execute_isolated(example, *isolation);
            } else {
                private_cluster_executor executor(conninfo);
                outcome = executor.execute(example);
            }
            if (!outcome) {
                throw std::runtime_error("landing example '" + view.slot + "': ["
                    + outcome.error().code + "] " + outcome.error().message);
            }
            if (!outcome->visible_results.empty()) {
                view.result = outcome->visible_results.front();
                view.has_result = true;
            }
        }
        views.push_back(std::move(view));
    }
    return views;
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
        // Fold §10.6 annotation directives into their target blocks before the render
        // copy is finalized (this also validates every fence's info string, §10.5).
        process_annotations(item.content, item.source_path);
        // The render copy must not leak hidden setup/teardown SQL (§5.1).
        strip_runnable_fence_hidden_sql(item.content);
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

// Small owned-node constructors for the executed-result document subtree (§13.6).
[[nodiscard]] document_node make_text_node(std::string value) {
    document_node node;
    node.kind = document_node_kind::text;
    node.text = std::move(value);
    return node;
}

[[nodiscard]] document_node make_emphasis_node(std::string value) {
    document_node node;
    node.kind = document_node_kind::emphasis;
    node.children.push_back(make_text_node(std::move(value)));
    return node;
}

[[nodiscard]] document_node make_paragraph_node(std::vector<document_node> children) {
    document_node node;
    node.kind = document_node_kind::paragraph;
    node.children = std::move(children);
    return node;
}

[[nodiscard]] document_node make_cell_node(bool header, document_node child) {
    document_node node;
    node.kind = document_node_kind::table_cell;
    if (header) {
        node.attributes.emplace("scope", "col");
    }
    node.children.push_back(std::move(child));
    return node;
}

[[nodiscard]] document_node make_row_node(std::vector<document_node> cells) {
    document_node node;
    node.kind = document_node_kind::table_row;
    node.children = std::move(cells);
    return node;
}

// Renders one executed example's visible result model (§13.4) to owned document
// nodes so the render stage emits result tables/notices/error admonitions without a
// database (§13.6). Multiple row result sets are labelled Result 1..N in order, a
// §13.6 provenance caption names the extension/server versions when known, and hidden
// SQL never enters this subtree — only the visible-phase result model does.
[[nodiscard]] std::vector<document_node> make_result_nodes(
    const sql_example_result& result, const std::string& extension_version,
    const std::string& server_version) {
    std::vector<document_node> nodes;
    std::size_t rows_total = 0;
    for (const sql_result_set& set : result.visible_results) {
        if (set.kind == sql_result_kind::rows) {
            ++rows_total;
        }
    }
    std::size_t rows_index = 0;
    for (const sql_result_set& set : result.visible_results) {
        if (set.kind == sql_result_kind::rows) {
            ++rows_index;
            if (rows_total > 1) {
                nodes.push_back(make_paragraph_node(
                    {make_emphasis_node("Result " + std::to_string(rows_index))}));
            }
            document_node table;
            table.kind = document_node_kind::table;
            table.attributes.emplace("class", "result-table");
            std::vector<document_node> header_cells;
            for (const sql_column& column : set.columns) {
                header_cells.push_back(make_cell_node(true, make_text_node(column.name)));
            }
            table.children.push_back(make_row_node(std::move(header_cells)));
            for (const std::vector<sql_cell>& row : set.rows) {
                std::vector<document_node> cells;
                for (const sql_cell& cell : row) {
                    cells.push_back(cell.is_null
                        ? make_cell_node(false, make_emphasis_node("NULL"))
                        : make_cell_node(false, make_text_node(cell.text)));
                }
                table.children.push_back(make_row_node(std::move(cells)));
            }
            nodes.push_back(std::move(table));
        } else if (set.kind == sql_result_kind::command) {
            nodes.push_back(make_paragraph_node({make_emphasis_node(set.command_tag)}));
        } else {
            document_node admonition;
            admonition.kind = document_node_kind::admonition;
            admonition.attributes.emplace("class", "warning");
            admonition.children.push_back(make_paragraph_node({make_text_node(
                "SQLSTATE " + set.sqlstate.value_or("") + ": " + set.primary_message)}));
            nodes.push_back(std::move(admonition));
        }
    }
    if (!result.notices.empty()) {
        document_node admonition;
        admonition.kind = document_node_kind::admonition;
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

// Walks the resolved tutorial content and inserts each runnable block's executed
// result nodes immediately after it (§13.6). Recurses so runnable blocks nested in
// admonitions/lists are covered; a block with no captured result (no cluster) is
// left as its visible SQL only.
void inject_tutorial_results(
    std::vector<document_node>& nodes,
    const std::map<std::string, sql_example_result, std::less<>>& results,
    const std::string& extension_version, const std::string& server_version) {
    std::vector<document_node> rebuilt;
    rebuilt.reserve(nodes.size());
    for (document_node& node : nodes) {
        inject_tutorial_results(node.children, results, extension_version, server_version);
        std::optional<std::string> runnable_id;
        if (node.kind == document_node_kind::code_block) {
            const auto language = node.attributes.find("language");
            const auto info = node.attributes.find("info");
            if (language != node.attributes.end() && language->second == "sql"
                && info != node.attributes.end()) {
                if (const auto fence = parse_runnable_fence(info->second)) {
                    runnable_id = fence->local_id;
                }
            }
        }
        rebuilt.push_back(std::move(node));
        if (runnable_id) {
            if (const auto found = results.find(*runnable_id); found != results.end()) {
                for (document_node& result_node :
                     make_result_nodes(found->second, extension_version, server_version)) {
                    rebuilt.push_back(std::move(result_node));
                }
            }
        }
    }
    nodes = std::move(rebuilt);
}

// Resolves the private-cluster execution target from the shared CLI options: either
// a per-example isolated clone (§13.3) or a direct conninfo, or neither (no cluster,
// examples render visible SQL only).
struct execution_target {
    bool have_connection{};
    std::optional<isolation_settings> isolation;
    std::string conninfo;
    std::string probe_conninfo;  // a plain connection usable for catalog provenance
};

[[nodiscard]] execution_target resolve_execution_target(
    const std::map<std::string, std::string, std::less<>>& options) {
    execution_target target;
    target.have_connection = options.contains("conninfo") || options.contains("isolated");
    if (options.contains("isolated")) {
        isolation_settings settings;
        settings.admin_conninfo = require_option(options, "admin-conninfo");
        settings.socket_directory = require_option(options, "socket-dir");
        settings.port = std::stoi(require_option(options, "port"));
        settings.template_database = require_option(options, "template-db");
        target.probe_conninfo = settings.admin_conninfo;
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
        private_cluster_executor executor("");
        return executor.execute_isolated(example, *target.isolation);
    }
    private_cluster_executor executor(target.conninfo);
    return executor.execute(example);
}

// Domain-scoped index from an entity reference spelling to its canonical id (§12.1).
// Each entity is indexed by qualified name, canonical id, and display name; a key
// that maps to two different entities is marked ambiguous (empty) so resolution
// rejects it (§10.4). The `id:` domain matches any entity by canonical id.
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
    const std::vector<documented_entity>& cpp_entities,
    const std::vector<documented_entity>& sql_entities) {
    entity_reference_index index;
    // A name an overload_group owns must resolve to the GROUP page, not to any one
    // member overload (§12.3): collect grouped names so each member skips indexing
    // its shared qualified/display name and only the group claims the bare name.
    std::set<std::string, std::less<>> grouped_names;
    std::set<std::string, std::less<>> real_spellings;
    for (const documented_entity& entity : sql_entities) {
        if (entity.kind == entity_kind::overload_group) {
            grouped_names.insert(entity.qualified_name);
        }
    }
    for (const documented_entity& entity : cpp_entities) {
        real_spellings.insert(entity.qualified_name);
    }
    for (const documented_entity& entity : sql_entities) {
        real_spellings.insert(entity.qualified_name);
    }
    const auto add_entity = [&](const documented_entity& entity) {
        const entity_query_domain domain = entity.domain == entity_domain::sql
            ? entity_query_domain::sql : entity_query_domain::cpp;
        index.add(domain, entity.canonical_id, entity.canonical_id);
        index.add(entity_query_domain::canonical_id, entity.canonical_id,
                  entity.canonical_id);
        const bool name_owned_by_group = entity.kind != entity_kind::overload_group
            && entity.domain == entity_domain::sql
            && grouped_names.contains(entity.qualified_name);
        if (!name_owned_by_group) {
            index.add(domain, entity.qualified_name, entity.canonical_id);
            index.add(domain, entity.display_name, entity.canonical_id);
            // The `any` domain backs the `#name` prose shorthand (§10.5): a name that
            // exists in both C++ and SQL collides here and is reported as ambiguous.
            index.add(entity_query_domain::any, entity.qualified_name,
                      entity.canonical_id);
            index.add(entity_query_domain::any, entity.display_name,
                      entity.canonical_id);
        }
    };
    for (const documented_entity& entity : cpp_entities) {
        add_entity(entity);
    }
    for (const documented_entity& entity : sql_entities) {
        add_entity(entity);
    }
    // Extension-qualified authoring alias `bioseq.<name>` for public-schema SQL
    // entities owned by the extension, when no real entity has that spelling (§10.4):
    // `#bioseq.mmsearch` resolves to the public.mmsearch overload group.
    for (const documented_entity& entity : sql_entities) {
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

// Splits one prose text run into text + shorthand entity_link nodes (§10.5). A
// shorthand is `#` at a text-start/whitespace boundary, followed by an ASCII
// identifier whose components join with `.` or `::`; anything else stays literal.
void split_shorthand_text(const std::string& text,
                          std::vector<document_node>& out) {
    const auto is_start = [](char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z') || character == '_';
    };
    const auto is_part = [&](char character) {
        return is_start(character) || (character >= '0' && character <= '9');
    };
    std::string pending;
    const auto flush = [&]() {
        if (pending.empty()) {
            return;
        }
        if (!out.empty() && out.back().kind == document_node_kind::text) {
            out.back().text += pending;
        } else {
            out.push_back(document_node{.kind = document_node_kind::text, .text = pending});
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
                document_node node;
                node.kind = document_node_kind::entity_link;
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

// Rewrites `#name` prose shorthands into entity_link nodes (§10.5), only inside
// prose: headings, code, links, and existing entity links are left untouched.
void apply_entity_shorthand(std::vector<document_node>& nodes, bool in_prose) {
    std::vector<document_node> rebuilt;
    for (document_node& node : nodes) {
        if (in_prose && node.kind == document_node_kind::text) {
            split_shorthand_text(node.text, rebuilt);
            continue;
        }
        const bool child_prose = in_prose
            && node.kind != document_node_kind::heading
            && node.kind != document_node_kind::code_span
            && node.kind != document_node_kind::code_block
            && node.kind != document_node_kind::link
            && node.kind != document_node_kind::entity_link;
        apply_entity_shorthand(node.children, child_prose);
        rebuilt.push_back(std::move(node));
    }
    nodes = std::move(rebuilt);
}

// Resolves every entity_link in a tutorial's content against the entity graph and
// records the target entity's canonical id on the node (§10.4, §12.1). An
// unresolved or ambiguous reference is fatal so a typo can never ship as inert
// text. Routes stay base-independent: render binds the base-aware URL later.
void resolve_entity_links(std::vector<document_node>& nodes,
                          const entity_reference_index& index,
                          const std::string& where) {
    for (document_node& node : nodes) {
        if (node.kind == document_node_kind::entity_link) {
            // A `#name` prose shorthand (§10.5) resolves name-only against every
            // domain; the explicit `[[domain:...]]` wiki form names its own domain.
            const bool shorthand = node.attributes.contains("shorthand");
            const std::string display = shorthand ? "#" + node.target
                                                  : "[[" + node.target + "]]";
            unresolved_entity_query query;
            if (shorthand) {
                query.domain = entity_query_domain::any;
                query.spelling = node.target;
            } else if (std::optional<unresolved_entity_query> parsed =
                           parse_entity_reference(
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
            // A `spelling|label` reference names its own display text; otherwise the
            // node's Markdown link text is the label. Only when neither exists does the
            // spelling become the label (so a resolved route never shows as raw text).
            if (query.label) {
                node.attributes["entity_label"] = *query.label;
            } else if (node.text.empty() && node.children.empty()) {
                node.attributes["entity_label"] = query.spelling;
            }
        }
        resolve_entity_links(node.children, index, where);
    }
}

// Collects the canonical IDs that resolved entity links target across a document
// tree (§10.4). These "referenced" entities join the required set for coverage:
// a public entity a tutorial links to must be documented or the build fails.
void collect_referenced_entity_ids(const std::vector<document_node>& nodes,
                                   std::set<std::string>& referenced) {
    for (const document_node& node : nodes) {
        if (const auto found = node.attributes.find("entity_id");
            found != node.attributes.end()) {
            referenced.insert(found->second);
        }
        collect_referenced_entity_ids(node.children, referenced);
    }
}

/// @brief Builds the resolved, database-independent site model (§5, §180).
///
/// Rebuilds the deterministic plan from source, reads the merged C++/SQL entity
/// graphs, executes the landing and tutorial runnable examples on the private
/// cluster (when one is configured), injects each tutorial result inline (§13.6),
/// captures server/extension provenance, and computes documentation coverage. The
/// returned model carries only normalized projections — never hidden setup/teardown
/// SQL, credentials, OIDs, timings, or machine paths — so `render` can emit staging
/// without a cluster and at more than one base path.
[[nodiscard]] site_model build_site_model(
    const fs::path& source, const std::string& version_string,
    const site_configuration& config, const std::string& site_yaml,
    const std::string& site_config_relpath, std::string base_path,
    std::string revision, bool include_drafts,
    const std::map<std::string, std::string, std::less<>>& options) {
    website_plan plan = build_plan_from_source(
        source, version_string, config, site_yaml, site_config_relpath,
        std::move(base_path), std::move(revision), include_drafts);

    site_model model;
    model.schema_version = "bioseq.website.site_model/v1";
    model.parser_version = plan.parser_version;
    model.binding_schema_version = plan.binding_schema_version;
    model.version = plan.version;
    model.site_title = plan.site_title;
    model.source_revision = plan.source_revision;
    model.inputs = plan.inputs;
    model.plan_signature = plan.signature;

    if (const auto found = options.find("cpp-entities"); found != options.end()) {
        model.cpp_entities = deserialize_entities(read_file(found->second));
    }
    if (const auto found = options.find("sql-entities"); found != options.end()) {
        model.sql_entities = deserialize_entities(read_file(found->second));
    }
    if (const auto found = options.find("cpp-signature"); found != options.end()) {
        model.cpp_signature = found->second;
    }
    if (const auto found = options.find("staged-extension-sha256"); found != options.end()) {
        model.staged_extension_sha256 = found->second;
    }

    // Apply the sql_api.yaml public-API allowlist (§11.4/§12.2): only the exact
    // allowlisted canonical IDs that live in an included schema become SQL pages;
    // every other captured extension member is recorded inventory-only. A listed ID
    // that is unknown, in a non-included schema, or otherwise unmatched is fatal - the
    // generator never rewrites canonical IDs to match an unexpected catalog (§13.2).
    if (const auto found = options.find("sql-api"); found != options.end()) {
        const std::string sql_api_yaml = read_file(found->second);
        const sql_api_configuration api = parse_sql_api(sql_api_yaml, found->second);
        const std::set<std::string, std::less<>> included_schemas(
            api.include_schemas.begin(), api.include_schemas.end());
        const std::set<std::string, std::less<>> allowlist(
            api.public_entities.begin(), api.public_entities.end());
        const auto schema_of = [](const documented_entity& entity) {
            const std::size_t dot = entity.qualified_name.find('.');
            return dot == std::string::npos
                ? std::string() : entity.qualified_name.substr(0, dot);
        };
        std::vector<documented_entity> kept;
        std::vector<documented_entity> omitted;
        std::set<std::string, std::less<>> matched;
        for (documented_entity& entity : model.sql_entities) {
            if (allowlist.contains(entity.canonical_id)
                && included_schemas.contains(schema_of(entity))) {
                matched.insert(entity.canonical_id);
                kept.push_back(std::move(entity));
            } else {
                omitted.push_back(std::move(entity));
            }
        }
        for (const std::string& id : api.public_entities) {
            if (!matched.contains(id)) {
                throw std::runtime_error("sql_api.yaml public_entities lists '" + id
                    + "', which is not a pg_depend member of extension bioseq in an "
                    "included schema");
            }
        }
        model.sql_entities = std::move(kept);
        model.omitted_sql_entities = std::move(omitted);
    }

    // Synthesize an overload-group page for every allowlisted SQL function name that
    // carries more than one signature (§12.1/§12.3): the group owns the bare-name
    // reference and lists each member's identity signature. Grouping runs on the
    // post-allowlist kept set, so an inventory-only overload never creates a page.
    {
        std::map<std::string, std::vector<const documented_entity*>, std::less<>> by_name;
        for (const documented_entity& entity : model.sql_entities) {
            if (entity.kind == entity_kind::sql_function) {
                by_name[entity.qualified_name].push_back(&entity);
            }
        }
        std::vector<documented_entity> groups;
        for (const auto& [name, members] : by_name) {
            if (members.size() < 2) {
                continue;
            }
            documented_entity group;
            group.domain = entity_domain::sql;
            group.kind = entity_kind::overload_group;
            group.qualified_name = name;
            const std::size_t dot = name.rfind('.');
            group.display_name = dot == std::string::npos ? name : name.substr(dot + 1);
            group.canonical_id = "sql:overload_group:" + name;
            group.summary_text = "Overloaded SQL function with "
                + std::to_string(members.size()) + " signatures.";
            for (const documented_entity* member : members) {
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
        private_cluster_executor probe(target.probe_conninfo);
        if (const std::expected<sql_catalog_snapshot, diagnostic> snapshot =
                probe.read_catalog()) {
            model.postgres_server_version = snapshot->server_version;
            model.extension_version = snapshot->extension_version;
            if (model.staged_extension_sha256.empty()) {
                model.staged_extension_sha256 = snapshot->staged_artifact_sha256;
            }
        }
    }

    model.landing_examples = collect_landing_views(source, version_string, options);

    // Resolve every tutorial's typed entity references against the merged graph
    // (§10.4/§12.1); an unresolved reference fails the build here, before render.
    const entity_reference_index reference_index =
        build_entity_reference_index(model.cpp_entities, model.sql_entities);

    for (tutorial& item : plan.tutorials) {
        apply_entity_shorthand(item.content, true);
        resolve_entity_links(item.content, reference_index, item.source_path);
        std::map<std::string, sql_example_result, std::less<>> results;
        if (target.have_connection) {
            for (const sql_example& example : item.sql_examples) {
                std::expected<sql_example_result, diagnostic> outcome =
                    run_one_example(target, example);
                if (!outcome) {
                    throw std::runtime_error("tutorial '" + item.metadata.slug
                        + "' example '" + example.local_id + "': ["
                        + outcome.error().code + "] " + outcome.error().message);
                }
                results.emplace(example.local_id, std::move(*outcome));
            }
        }
        inject_tutorial_results(item.content, results, model.extension_version,
                                model.postgres_server_version);
        // The site_model is a published-adjacent artifact: it must never carry hidden
        // setup/teardown SQL (§180). Those phases were consumed by execution above;
        // strip them so only the visible projection survives into the model.
        for (sql_example& example : item.sql_examples) {
            example.setup_sql.clear();
            example.teardown_sql.clear();
        }
    }
    model.tutorials = std::move(plan.tutorials);

    // Coverage spans both the configured public C++ modules and the allowlisted SQL
    // domain (§11.4); synthetic overload-group aggregates are routing pages, not
    // documentable entities, so they are excluded from the totals.
    std::vector<documented_entity> coverage_entities = model.cpp_entities;
    for (const documented_entity& entity : model.sql_entities) {
        if (entity.kind != entity_kind::overload_group) {
            coverage_entities.push_back(entity);
        }
    }
    // Every entity a tutorial links to must be documented (§11.4/§11.6): collect the
    // resolved references so an undocumented referenced entity is fatal, not a warning.
    std::set<std::string> referenced;
    for (const tutorial& item : model.tutorials) {
        collect_referenced_entity_ids(item.content, referenced);
    }
    const std::vector<std::string> referenced_ids(referenced.begin(), referenced.end());
    const coverage_report coverage = compute_coverage(
        coverage_entities, config.required_documentation_entities, referenced_ids);
    model.coverage_public_total = static_cast<std::uint32_t>(coverage.public_total);
    model.coverage_documented_total =
        static_cast<std::uint32_t>(coverage.documented_total);
    model.coverage_missing = coverage.missing_summary;
    model.coverage_missing_parameters = coverage.missing_parameters;
    model.coverage_missing_returns = coverage.missing_returns;
    model.coverage_fatal_missing = coverage.fatal_missing;
    for (const coverage_group_report& group : coverage.groups) {
        model.coverage_groups.push_back(coverage_group_view{
            group.group, static_cast<std::uint32_t>(group.public_total),
            static_cast<std::uint32_t>(group.documented_total)});
    }
    // §11.4: the build fails when a required or tutorial-referenced public entity is
    // undocumented. C++ required coverage is also gated at cpp_index; this is the
    // single point that enforces the same policy over the live allowlisted SQL domain.
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

/// @brief Self-tests overload-group routing and group-page rendering (§12.3).
///
/// A name carrying several allowlisted signatures gets a synthetic overload_group
/// that owns the bare-name reference (so a name-only link resolves to the group
/// page instead of being rejected as ambiguous), while every concrete overload
/// stays addressable by its canonical id; the group page view lists each member's
/// identity signature with its route.
void run_website_reference_smoke() {
    documented_entity overload_a;
    overload_a.domain = entity_domain::sql;
    overload_a.kind = entity_kind::sql_function;
    overload_a.qualified_name = "public.mmsearch";
    overload_a.display_name = "mmsearch";
    overload_a.canonical_id = "sql:function:public.mmsearch(bioseq.seq,bioseq.seq)";
    documented_entity overload_b = overload_a;
    overload_b.canonical_id =
        "sql:function:public.mmsearch(bioseq.seq,bioseq.seq,public.bioseq_aligner_options)";
    documented_entity mmsearch_group;
    mmsearch_group.domain = entity_domain::sql;
    mmsearch_group.kind = entity_kind::overload_group;
    mmsearch_group.qualified_name = "public.mmsearch";
    mmsearch_group.display_name = "mmsearch";
    mmsearch_group.canonical_id = "sql:overload_group:public.mmsearch";
    const entity_reference_index grouped_index = build_entity_reference_index(
        {}, {overload_a, overload_b, mmsearch_group});
    const auto lookup = [&](entity_query_domain domain, std::string_view spelling) {
        const auto found = grouped_index.by_key.find(
            entity_reference_index::domain_key(domain) + std::string(spelling));
        return found == grouped_index.by_key.end() ? std::string() : found->second;
    };
    expect(lookup(entity_query_domain::sql, "public.mmsearch")
               == "sql:overload_group:public.mmsearch"
           && lookup(entity_query_domain::sql, "mmsearch")
               == "sql:overload_group:public.mmsearch"
           && lookup(entity_query_domain::canonical_id,
                     "sql:function:public.mmsearch(bioseq.seq,bioseq.seq)")
               == "sql:function:public.mmsearch(bioseq.seq,bioseq.seq)",
           "overload-group name resolves to the group; members keep canonical ids");

    // §10.5 shorthand resolves name-only against the `any` domain, and the
    // extension-qualified `bioseq.<name>` alias resolves to the same target.
    expect(lookup(entity_query_domain::any, "mmsearch")
               == "sql:overload_group:public.mmsearch"
           && lookup(entity_query_domain::any, "bioseq.mmsearch")
               == "sql:overload_group:public.mmsearch",
           "#mmsearch shorthand and the bioseq.mmsearch alias resolve to the group");
    std::vector<document_node> shorthand_split;
    split_shorthand_text("see #bioseq.mmsearch and ##notshorthand", shorthand_split);
    int shorthand_links = 0;
    std::string shorthand_target;
    for (const document_node& node : shorthand_split) {
        if (node.kind == document_node_kind::entity_link) {
            ++shorthand_links;
            shorthand_target = node.target;
        }
    }
    expect(shorthand_links == 1 && shorthand_target == "bioseq.mmsearch",
           "shorthand splitter extracts a single #name entity reference");

    documented_entity group_view = mmsearch_group;
    group_view.overload_members = {
        {"public.mmsearch(bioseq.seq,bioseq.seq)", "/v0.1/reference/sql/function/a/"},
        {"public.mmsearch(bioseq.seq,bioseq.seq,public.bioseq_aligner_options)",
         "/v0.1/reference/sql/function/b/"}};
    const std::string group_json = serialize_single_entity(group_view);
    expect(group_json.find("\"overloads\"") != std::string::npos
               && group_json.find("bioseq_aligner_options") != std::string::npos,
           "overload-group page view serializes member signatures");

    // Container pages (§12.3) carry a child index; an implementation cross-link
    // (§12.2) resolves to the implementing entity's route.
    documented_entity container;
    container.domain = entity_domain::cpp;
    container.kind = entity_kind::struct_;
    container.qualified_name = "bioseq::io::seq";
    container.child_members = {{"read", "/v0.1/reference/cpp/function/read-abc/"},
                               {"size", "/v0.1/reference/cpp/function/size-def/"}};
    container.implemented_by_route = "/v0.1/reference/cpp/function/impl-ghi/";
    const std::string container_json = serialize_single_entity(container);
    expect(container_json.find("\"members\"") != std::string::npos
               && container_json.find("read-abc") != std::string::npos
               && container_json.find("\"implemented_by\"") != std::string::npos,
           "container page view serializes child index and implemented-by link");

    // §10.5 fence info grammar: quoted values carry commas/spaces/parens verbatim;
    // duplicate scalar and unknown attributes are fatal.
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

    // §10.6 folding: the directive attaches to a PRIOR target as a numbered
    // code_annotation child pointing at the matched source range, and is removed.
    std::vector<document_node> annotated_doc;
    document_node display_block;
    display_block.kind = document_node_kind::code_block;
    display_block.attributes["language"] = "sql";
    display_block.attributes["info"] = "sql id=demo annotated";
    display_block.text = "CREATE TABLE t (s bioseq.seq('dna4', 64));";
    annotated_doc.push_back(std::move(display_block));
    document_node directive_block;
    directive_block.kind = document_node_kind::code_block;
    directive_block.attributes["language"] = "annotation";
    directive_block.attributes["info"] =
        "annotation target=demo match=\"bioseq.seq('dna4', 64)\" title=\"The type\"";
    directive_block.text = "The alphabet and length live in the type modifier.";
    annotated_doc.push_back(std::move(directive_block));
    process_annotations(annotated_doc, "self-test");
    expect(annotated_doc.size() == 1 && !annotated_doc[0].children.empty()
               && annotated_doc[0].children[0].kind == document_node_kind::code_annotation,
           "annotation directive folds onto its target and the directive node is removed");
    const document_node& folded = annotated_doc[0].children[0];
    const std::size_t folded_start = std::stoul(folded.attributes.at("start"));
    const std::size_t folded_len = std::stoul(folded.attributes.at("len"));
    expect(folded.attributes.at("number") == "1"
               && folded.attributes.at("title") == "The type"
               && annotated_doc[0].text.substr(folded_start, folded_len)
                      == "bioseq.seq('dna4', 64)",
           "folded annotation carries number/title and a range over the matched source");
}

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
    std::vector<coverage_group_view> coverage_groups;
    for (const coverage_group_report& group : coverage.groups) {
        coverage_groups.push_back(coverage_group_view{
            group.group, static_cast<std::uint32_t>(group.public_total),
            static_cast<std::uint32_t>(group.documented_total)});
    }
    write_if_different(coverage_output,
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

/// @brief Runs `website execute-sql`: runs one ad-hoc SQL example on a live cluster.
///
/// Connects to the private Unix-socket cluster, runs the setup/visible/teardown
/// SQL through private_cluster_executor, and renders the visible result model to
/// stdout (§13.5-13.6). A focused live-execution entrypoint used by the acceptance
/// harness; the model-producing `execute` and the batch runner sit on top of this.
/// @param args Options after the `execute-sql` token.
/// @return Zero on success; two on usage errors; three on execution failure.
[[nodiscard]] int run_execute_sql(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {
        "conninfo", "setup", "visible", "teardown", "expect-sqlstate",
        "admin-conninfo", "socket-dir", "port", "template-db", "timeout-ms"};
    static constexpr std::string_view flag_keys[] = {"isolated"};
    const auto options = parse_options(args, value_keys, flag_keys);

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
        std::println(stderr, "website execute-sql: [{}] {}",
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
    std::println(stderr, "website execute-sql: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website execute-sql: {}", error.what());
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

/// @brief Runs `website render-fragment`: renders a template against a JSON context.
///
/// Reads a Jinja template and a JSON page-data context and writes the rendered
/// document (§14). The low-level render primitive; the full model-driven `render`
/// stage is separate.
/// @param args Options after the `render-fragment` token.
/// @return Zero on success; two on usage errors; three on render failure.
[[nodiscard]] int run_render_fragment(std::span<std::string_view const> args) try {
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
    std::println(stderr, "website render-fragment: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website render-fragment: {}", error.what());
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
    // Hidden runnable-example phases must never reach a published page (§5.1).
    for (const std::string_view marker :
         {"bioseq:setup", "bioseq:visible", "bioseq:teardown"}) {
        if (html.find(marker) != std::string_view::npos) {
            fail("leaks a hidden runnable-example SQL phase marker");
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

// Emits a JSON string value, or null when empty, for the site manifest provenance.
[[nodiscard]] std::string manifest_optional(std::string_view value) {
    return value.empty() ? std::string("null")
                         : "\"" + std::string(value) + "\"";
}

// Binds each resolved entity_link to its base-aware route and tooltip data URL
// (§12.1/§12.4). The resolver (execute) recorded the canonical id on the node; here
// the render stage maps it to the route it assigned at this base path, so the same
// model links correctly at more than one base path.
void rewrite_entity_link_routes(
    std::vector<document_node>& nodes,
    const std::map<std::string, std::string, std::less<>>& route_by_id,
    const std::string& tooltip_url) {
    for (document_node& node : nodes) {
        if (node.kind == document_node_kind::entity_link) {
            const auto identity = node.attributes.find("entity_id");
            if (identity != node.attributes.end()) {
                const auto route = route_by_id.find(identity->second);
                if (route != route_by_id.end()) {
                    node.target = route->second;
                    node.attributes["tooltip_url"] = tooltip_url;
                }
            }
        }
        rewrite_entity_link_routes(node.children, route_by_id, tooltip_url);
    }
}

// Minimal JSON string-body escaper for the entity-ranges data attribute payload.
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

// Resolves each displayed block's `entity="<canonical-id>"` fence attributes into
// non-overlapping source-text ranges (§12.6). A listed id must exist and its
// qualified spelling must occur exactly once in the block source (absent or
// ambiguous is fatal); overlapping ranges are fatal. The ranges are emitted as a
// data attribute the client decorator wraps in entity links after Prism, so copied
// source and textContent are unchanged.
void decorate_code_entity_ranges(
    std::vector<document_node>& nodes,
    const std::map<std::string, std::string, std::less<>>& route_by_id,
    const std::map<std::string, std::string, std::less<>>& qualified_by_id,
    const std::string& tooltip_url,
    const std::string& where) {
    struct code_range {
        std::size_t start;
        std::size_t len;
        std::string id;
        std::string route;
    };
    for (document_node& node : nodes) {
        if (node.kind == document_node_kind::code_block) {
            const auto info = node.attributes.find("info");
            if (info != node.attributes.end() && !info->second.empty()) {
                const fence_descriptor fence = parse_fence_info(info->second);
                std::vector<code_range> ranges;
                for (const std::string& id : fence.entities) {
                    const auto qualified = qualified_by_id.find(id);
                    if (qualified == qualified_by_id.end()) {
                        throw std::runtime_error("code entity '" + id
                            + "' does not exist in " + where);
                    }
                    const std::string& spelling = qualified->second;
                    const std::size_t first = node.text.find(spelling);
                    if (spelling.empty() || first == std::string::npos) {
                        throw std::runtime_error("code entity spelling '" + spelling
                            + "' is not present in the block in " + where);
                    }
                    if (node.text.find(spelling, first + 1) != std::string::npos) {
                        throw std::runtime_error("code entity spelling '" + spelling
                            + "' is ambiguous (appears more than once) in " + where);
                    }
                    const auto route = route_by_id.find(id);
                    ranges.push_back(code_range{first, spelling.size(), id,
                        route != route_by_id.end() ? route->second : std::string()});
                }
                if (!ranges.empty()) {
                    std::ranges::sort(ranges, [](const code_range& lhs,
                                                 const code_range& rhs) {
                        return lhs.start < rhs.start;
                    });
                    for (std::size_t index = 1; index < ranges.size(); ++index) {
                        if (ranges[index].start
                            < ranges[index - 1].start + ranges[index - 1].len) {
                            throw std::runtime_error("code entity ranges overlap in " + where);
                        }
                    }
                    std::string json = "[";
                    for (std::size_t index = 0; index < ranges.size(); ++index) {
                        if (index != 0) {
                            json += ",";
                        }
                        json += "{\"start\":" + std::to_string(ranges[index].start)
                            + ",\"len\":" + std::to_string(ranges[index].len)
                            + ",\"id\":\"" + json_escape(ranges[index].id) + "\""
                            + ",\"route\":\"" + json_escape(ranges[index].route) + "\""
                            + ",\"tooltip\":\"" + json_escape(tooltip_url) + "\"}";
                    }
                    json += "]";
                    node.attributes["entity_ranges"] = std::move(json);
                }
            }
        }
        decorate_code_entity_ranges(node.children, route_by_id, qualified_by_id,
                                    tooltip_url, where);
    }
}

/// @brief Renders a resolved site model into an owner-marked staging tree (§14, §16).
///
/// Consumes only the database-independent `site_model` plus the site config, the
/// requested base path, and the template/static roots — never a cluster — so the
/// same model can be rendered at more than one base path (§180). It content-
/// addresses the static bundle, renders every §16 route through the shared frame,
/// records server/extension provenance in the manifest (§16), and enforces exact
/// manifest/tree set equality. Returns the site-manifest SHA-256; performs no swap.
[[nodiscard]] std::string render_model_to_staging(
    const site_model& model, const site_configuration& config,
    const std::string& base_path, const std::string& revision,
    const fs::path& templates_dir, const fs::path& static_dir, const fs::path& staging) {
    const std::string version = model.version.path_component;
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
    site.prism_js_url = url(bundle_prefix + "vendor/prism/prism.min.js");
    site.prism_css_url = url(bundle_prefix + "vendor/prism/prism.min.css");
    // The configured default social image lives in the immutable bundle under
    // media/; drop the authoring "static/" prefix to get its bundle-relative path
    // and bind it as an absolute OpenGraph/Twitter image URL (§15.5).
    std::string social_relative = config.default_social_image;
    if (social_relative.starts_with("static/")) {
        social_relative.erase(0, std::string_view("static/").size());
    }
    site.social_image_url = config.canonical_origin + url(bundle_prefix + social_relative);
    site.documentation_url = url(version + "/");
    site.tutorials_url = url(version + "/tutorials/");
    site.reference_url = url(version + "/reference/");
    site.search_index_url = url(version + "/search-index.json");
    site.versions_url = url("versions.json");
    site.versions_page_url = url("versions/");
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

    // Assign each entity a readable, collision-checked route (§12.1). Routes are
    // recomputed here (not carried in the model) so a base-independent model renders
    // at any base path; the canonical ID and output path collision checks stay.
    // Route segments reuse the centralized §12.1 canonical-ID kind token so the
    // path kind and the canonical-ID kind can never drift apart.
    const auto kind_token = [](entity_kind kind) { return entity_kind_token(kind); };
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
    std::vector<documented_entity> cpp_entities = model.cpp_entities;
    std::vector<documented_entity> sql_entities = model.sql_entities;
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

    // The merged entity graph, its canonical-id -> base-aware route map, and the
    // content-addressed tooltip dataset URL (§12.4) are computed once here: tutorials
    // bind their resolved entity links to these routes, and the search index and
    // tooltip artifact below reuse the same values.
    std::vector<documented_entity> all_entities = cpp_entities;
    all_entities.insert(all_entities.end(), sql_entities.begin(), sql_entities.end());
    std::map<std::string, std::string, std::less<>> route_by_id;
    std::map<std::string, std::string, std::less<>> qualified_by_id;
    for (const documented_entity& entity : all_entities) {
        route_by_id.emplace(entity.canonical_id, entity.route_path);
        qualified_by_id.emplace(entity.canonical_id, entity.qualified_name);
    }

    // Bind each overload-group's members to their identity signature + base-aware
    // route so the group page can link to every concrete overload (§12.3).
    for (documented_entity& group : sql_entities) {
        if (group.kind != entity_kind::overload_group) {
            continue;
        }
        for (const std::string& member_id : group.overload_ids) {
            const auto member = std::ranges::find_if(
                sql_entities, [&](const documented_entity& candidate) {
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

    // Container child index (§12.3): group every entity under its parent's qualified
    // name so class/struct pages list their members and module/namespace pages list
    // their children. The parent is the qualified name minus its last top-level
    // `::` component (template/parameter `::` are skipped via depth tracking).
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
    for (const documented_entity& entity : all_entities) {
        if (entity.kind == entity_kind::overload_group) {
            continue;  // routing aggregate, not a structural child
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
    const auto attach_relations = [&](std::vector<documented_entity>& list) {
        for (documented_entity& entity : list) {
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

    const std::string tooltip_json = serialize_entity_tooltips(all_entities);
    const std::string tooltip_url =
        url(version + "/entities." + bioseq::codegen::sha256_hex(tooltip_json) + ".json");

    // Root marketing landing page: the executed runnable slots (§15.2) are bound as
    // the examples.<slot> collection; hidden setup/teardown never reaches the page.
    // Shared breadcrumb serializer and tutorial-backlink reverse map (§12.3/§15.1).
    // Each crumb is {label, href}; the last crumb is the current page.
    const auto breadcrumb_json =
        [&](const std::vector<std::pair<std::string, std::string>>& crumbs) {
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

    // Version-global left navigation (§15.1): every tutorial plus the two reference
    // indexes. Bound as page.doc_nav on documentation pages so the frame is uniform.
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

    // On-this-page: assign a stable [a-z0-9-] anchor to every body heading and
    // return its table-of-contents JSON. Dedupes collisions with a numeric suffix.
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
    const auto node_plain_text = [](const document_node& node, auto&& self) -> std::string {
        std::string text = node.text;
        for (const document_node& child : node.children) {
            text += self(child, self);
        }
        return text;
    };
    const auto build_on_this_page = [&](std::vector<document_node>& nodes) {
        std::set<std::string> used;
        std::string toc = "[";
        bool first = true;
        const auto walk = [&](std::vector<document_node>& list, auto&& self_walk) -> void {
            for (document_node& node : list) {
                if (node.kind == document_node_kind::heading) {
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

    const std::string landing_examples_json =
        serialize_landing_examples(model.landing_examples);
    render("index.html", "landing.html.j2",
           "{\"title\":" + json_text(config.title)
               + ",\"description\":" + json_text(config.short_description)
               + ",\"canonical\":" + json_text(canonical("index.html"))
               + ",\"page_type\":" + json_text("landing")
               + ",\"eyebrow\":" + json_text("PostgreSQL-native")
               + ",\"headline\":" + json_text(config.title)
               + ",\"lede\":" + json_text(config.short_description)
               + ",\"install\":" + json_text("CREATE EXTENSION bioseq;") + "}",
           "examples", landing_examples_json);

    // Host-served not-found page (§16 layout). Version-agnostic root artifact.
    render("error.html", "error.html.j2",
           "{\"title\":" + json_text("Page not found")
               + ",\"description\":" + json_text("The requested page does not exist.")
               + ",\"canonical\":" + json_text(canonical("error.html")) + "}",
           "", "");

    // Version overview / documentation home, distinct from the landing page.
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

    // Tutorials index and one page per published tutorial (§10, §16). Each tutorial's
    // content already carries its executed result subtrees (§13.6), injected by
    // execute, so rendering here is database-free.
    std::string tutorial_cards = "[";
    for (std::size_t index = 0; index < model.tutorials.size(); ++index) {
        const tutorial& item = model.tutorials[index];
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
               + json_text(canonical(version + "/tutorials/index.html"))
               + ",\"breadcrumb\":" + breadcrumb_json({{"Home", url("")},
                     {version, url(version + "/")}, {"Tutorials", url(version + "/tutorials/")}})
               + ",\"doc_nav\":" + doc_nav + "}",
           "tutorials", tutorial_cards);
    for (const tutorial& item : model.tutorials) {
        const std::string rel =
            version + "/tutorials/" + item.metadata.slug + "/index.html";
        std::vector<document_node> body_nodes = item.content;
        rewrite_entity_link_routes(body_nodes, route_by_id, tooltip_url);
        decorate_code_entity_ranges(body_nodes, route_by_id, qualified_by_id,
                                    tooltip_url, item.metadata.slug);
        const std::string on_this_page = build_on_this_page(body_nodes);
        const std::string body = render_document_html(body_nodes);
        std::string page = "{\"title\":" + json_text(item.metadata.title)
            + ",\"description\":" + json_text(item.metadata.description)
            + ",\"canonical\":" + json_text(canonical(rel))
            + ",\"page_type\":" + json_text("tutorial")
            + ",\"breadcrumb\":" + breadcrumb_json({
                  {"Home", url("")},
                  {version, url(version + "/")},
                  {"Tutorials", url(version + "/tutorials/")},
                  {item.metadata.title, url(version + "/tutorials/" + item.metadata.slug + "/")}})
            + ",\"doc_nav\":" + doc_nav
            + ",\"on_this_page\":" + on_this_page
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
               + ",\"sql_url\":" + json_text(url(version + "/reference/sql/"))
               + ",\"breadcrumb\":" + breadcrumb_json({{"Home", url("")},
                     {version, url(version + "/")}, {"Reference", url(version + "/reference/")}})
               + ",\"doc_nav\":" + doc_nav + "}",
           "", "");
    const auto render_domain = [&](std::string_view domain, std::string_view label,
                                   const std::vector<documented_entity>& entities) {
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
               "entities", serialize_entity_array(entities));
        for (const documented_entity& entity : entities) {
            const std::string rel = entity_rel(entity) + "index.html";
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
                      {entity.qualified_name, url(entity_rel(entity))}});
            page += ",\"doc_nav\":" + doc_nav;
            // Section table of contents (§15.1): fixed entity sections that are
            // present, matched to the h2 anchors in entity.html.j2.
            const bool has_backlinks =
                backlinks_by_id.find(entity.canonical_id) != backlinks_by_id.end();
            std::string entity_toc = "[";
            bool toc_first = true;
            const auto add_toc = [&](std::string_view text, std::string_view anchor) {
                if (!toc_first) {
                    entity_toc.push_back(',');
                }
                toc_first = false;
                entity_toc += "{\"level\":2,\"text\":" + json_text(std::string(text))
                    + ",\"anchor\":" + json_text(std::string(anchor)) + "}";
            };
            if (!entity.parameters.empty()) {
                add_toc("Parameters", "parameters");
            }
            if (!entity.attributes.empty()) {
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
            // Detailed documentation body (trusted HTML, §12.3).
            page += ",\"detail_html\":";
            page += entity.documentation.empty()
                ? std::string("null")
                : json_quote(render_document_html(entity.documentation));
            // C++ source link: repository blob at the configured revision (§12.3).
            if (entity.declaration) {
                const std::string spath = entity.declaration->path.generic_string();
                const auto marker = spath.rfind("/main/");
                const std::string relpath = marker != std::string::npos
                    ? spath.substr(marker + 1)
                    : entity.declaration->path.filename().string();
                const std::string blob = config.repository_url + "/blob/" + revision + "/"
                    + relpath + "#L" + std::to_string(entity.declaration->first_line);
                const std::string source_label =
                    entity.module_name ? *entity.module_name : relpath;
                page += ",\"source_url\":" + json_text(blob)
                    + ",\"source_label\":" + json_text(source_label);
            } else {
                page += ",\"source_url\":null,\"source_label\":null";
            }
            // SQL provenance: extension version + actual schema + server version (§13.6).
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
            // Tutorial backlinks (§12.3): tutorials that reference this entity.
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
            render(rel, "entity.html.j2", page, "entity", serialize_single_entity(entity));
        }
    };
    render_domain("cpp", "C++", cpp_entities);
    render_domain("sql", "SQL", sql_entities);

    // Documentation-coverage page and machine-readable report (§11.4), taken from
    // the resolved model so execute and render agree exactly. Missing docs are broken
    // out into summary/parameter/return dimensions and totalled per module/SQL domain.
    const auto id_array = [&](const std::vector<std::string>& ids) {
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
               + ",\"fatal_missing\":" + id_array(model.coverage_fatal_missing) + "}",
           "", "");
    writer.emit(route(version + "/documentation-coverage.json"),
                serialize_coverage(model.coverage_public_total,
                                   model.coverage_documented_total,
                                   model.coverage_missing, model.coverage_missing_parameters,
                                   model.coverage_missing_returns, model.coverage_fatal_missing,
                                   model.coverage_groups));

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
    // Project the one canonical graph, the tutorial plan, and the versioned index
    // pages into the deterministic per-version search index (§12.5).
    std::vector<search_entry> search_entries;
    search_entries.reserve(all_entities.size() + model.tutorials.size() + 5);
    for (const documented_entity& entity : all_entities) {
        search_entries.push_back(search_entry_from_entity(entity));
    }
    for (const tutorial& item : model.tutorials) {
        search_entry entry;
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
    const auto add_page_entry = [&](std::string id, std::string title, std::string_view rel,
                                    std::string summary) {
        search_entry entry;
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
    add_page_entry("page:" + version + ":tutorials", "Tutorials", version + "/tutorials/",
                   "Bioseq tutorials for " + version);
    add_page_entry("page:" + version + ":reference", "API Reference", version + "/reference/",
                   "Bioseq C++ and SQL API reference");
    add_page_entry("page:" + version + ":reference-cpp", "C++ Reference",
                   version + "/reference/cpp/", "Bioseq C++ API reference");
    add_page_entry("page:" + version + ":reference-sql", "SQL Reference",
                   version + "/reference/sql/", "Bioseq SQL API reference");
    writer.emit(route(version + "/search-index.json"),
                build_search_index(std::move(search_entries)));
    // Content-addressed per-version tooltip dataset (§12.4, §16): the same bytes the
    // resolved entity links point at, named by their own SHA-256 for long caching.
    writer.emit(route(version + "/entities." + bioseq::codegen::sha256_hex(tooltip_json)
                          + ".json"),
                tooltip_json);
    // catalog-inventory.json records the extension-owned candidates that the
    // sql_api.yaml allowlist did NOT promote to pages (§11.4) - the inverse of the
    // published SQL surface - so a reviewer can audit what stays inventory-only.
    if (!model.omitted_sql_entities.empty()) {
        writer.emit(route(version + "/catalog-inventory.json"),
                    serialize_sql_entities(model.omitted_sql_entities));
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
    writer.emit(route("sitemap.xml"), sitemap);

    // Serialize the manifest last, then require exact set equality (§16). The live
    // PostgreSQL server/extension provenance (§16) comes from the resolved model.
    {
        std::string manifest_json;
        manifest_json += "{\n  \"schema\": \"bioseq.website.site_manifest/v1\",\n";
        manifest_json += "  \"hash_algorithm\": \"sha256\",\n";
        manifest_json += "  \"version\": \"" + version + "\",\n";
        manifest_json += "  \"source_revision\": \"" + revision + "\",\n";
        manifest_json += "  \"base_path\": \"" + base_path + "\",\n";
        manifest_json += "  \"generator_signature\": \"" + model.plan_signature + "\",\n";
        manifest_json += "  \"parser_version\": \"" + model.parser_version + "\",\n";
        manifest_json += "  \"binding_schema\": \"" + model.binding_schema_version + "\",\n";
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
    return bioseq::codegen::sha256_hex(read_file(staging / "site-manifest.json"));
}

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
        "templates", "static", "output", "base-path", "source-revision",
        "conninfo", "admin-conninfo", "socket-dir", "port", "template-db",
        "staged-extension-sha256", "cpp-signature",
        "staging", "model-validation-stamp", "html-validation-stamp"};
    static constexpr std::string_view flag_keys[] = {"include-drafts", "isolated"};
    const auto options = parse_options(args, value_keys, flag_keys);
    const fs::path output = require_option(options, "output");
    const bool swap_only = options.contains("staging");
    const fs::path staging = swap_only
        ? fs::path(require_option(options, "staging"))
        : fs::path(output.string() + ".staging");

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

    // Every validator stamp publish must confirm before the exchange (§14, §17.3):
    // render binds them to staging; publish rechecks their presence (fail-closed).
    std::vector<std::string> validation_stamps;
    if (const auto found = options.find("model-validation-stamp"); found != options.end()) {
        validation_stamps.push_back(found->second);
    }
    if (const auto found = options.find("html-validation-stamp"); found != options.end()) {
        validation_stamps.push_back(found->second);
    }

    std::string manifest_hash;
    if (swap_only) {
        // Spec publish (§168): verify the owner-marked staging, the validator stamps,
        // and the full staged manifest, then atomically exchange. No rendering, no
        // cluster, so a validator that rejected staging blocks the swap entirely.
        const fs::path staging_manifest = staging / "site-manifest.json";
        if (!fs::exists(staging_manifest)) {
            throw std::runtime_error("publish: staging has no site-manifest.json: "
                                     + staging.string());
        }
        const std::string manifest_text = read_file(staging_manifest);
        manifest_hash = bioseq::codegen::sha256_hex(manifest_text);
        if (!fs::exists(staging.string() + ".owner.json")) {
            throw std::runtime_error("publish: staging is not owner-marked: "
                                     + staging.string() + ".owner.json");
        }
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
        for (const std::string& declared_route : manifest_output_routes(manifest_text)) {
            declared.insert(declared_route);
        }
        if (present != declared) {
            throw std::runtime_error("publish: staging manifest set mismatch (declares "
                + std::to_string(declared.size()) + ", tree has "
                + std::to_string(present.size()) + ")");
        }
    } else {
        // Convenience all-in-one path (dev / MCP): build the resolved model and render
        // staging in one process, then swap. The spec CMake pipeline instead uses
        // execute -> render -> publish --staging so validators gate the exchange.
        const std::string site_yaml = read_file(require_option(options, "site-config"));
        const site_configuration config =
            parse_site_config(site_yaml, require_option(options, "site-config"));
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
        const site_model model = build_site_model(
            source, version_string, config, site_yaml,
            plan_relative(site_config_path, source), base_path, revision,
            options.contains("include-drafts"), options);
        manifest_hash = render_model_to_staging(model, config, base_path, revision,
            fs::path(require_option(options, "templates")),
            fs::path(require_option(options, "static")), staging);
    }

    // Durable write-ahead publish (§14): stamp an fsynced 0600 prepublish descriptor
    // plus an owner marker adjacent to staging, then swap atomically, then record the
    // success marker and drop the descriptor/owner. A crash between any two steps is
    // resolved by the crash-recovery pass on the next run.
    const std::string nonce =
        std::to_string(::getpid()) + "-" + manifest_hash.substr(0, 16);
    const fs::path owner_marker = staging.string() + ".owner.json";
    write_private_artifact(owner_marker,
        "{\"schema\":\"bioseq.website.owner/v1\",\"nonce\":\"" + nonce
        + "\",\"staging\":\"" + staging.filename().string() + "\"}\n");
    std::string stamps_json = "[";
    for (std::size_t index = 0; index < validation_stamps.size(); ++index) {
        stamps_json += (index > 0 ? ", " : "") + json_quote(validation_stamps[index]);
    }
    stamps_json.push_back(']');
    const fs::path prepublish = output.string() + ".prepublish.json";
    write_private_artifact(prepublish,
        "{\n  \"schema\": \"bioseq.website.prepublish/v1\",\n"
        "  \"operation\": \"publish\",\n  \"nonce\": \"" + nonce + "\",\n"
        "  \"staging\": \"" + staging.filename().string() + "\",\n"
        "  \"new_manifest_sha256\": \"" + manifest_hash + "\",\n"
        "  \"validation_stamps\": " + stamps_json + "\n}\n");
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
    std::println("published site to {}", output.string());
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website publish: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website publish: {}", error.what());
    return 3;
}

/// @brief Runs `website execute`: resolves + executes the site into a site_model (§180).
///
/// Rebuilds the plan from source, reads the merged C++/SQL entity artifacts, opens
/// the private cluster (when configured) to execute the landing and tutorial runnable
/// examples and capture server/extension provenance, and writes the resolved
/// database-independent site model (schema site_model/v1, mode 0600). Setup/teardown
/// SQL, credentials, OIDs, timings, and machine paths never enter the artifact, so
/// `render` can consume it without a database and at more than one base path.
/// @param args Options after the `execute` token.
/// @return Zero on success; two on usage errors; three on parse/execution failure.
[[nodiscard]] int run_execute(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {
        "source", "version", "site-config", "cpp-entities", "sql-entities",
        "sql-api", "base-path", "source-revision", "output-model", "cpp-signature",
        "staged-extension-sha256", "conninfo", "admin-conninfo", "socket-dir",
        "port", "template-db"};
    static constexpr std::string_view flag_keys[] = {"include-drafts", "isolated"};
    const auto options = parse_options(args, value_keys, flag_keys);
    const fs::path source = require_option(options, "source");
    const std::string version_string = require_option(options, "version");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path output_model = require_option(options, "output-model");

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
    const site_model model = build_site_model(
        source, version_string, config, site_yaml,
        plan_relative(site_config_path, source), std::move(base_path),
        std::move(revision), options.contains("include-drafts"), options);
    write_private_artifact(output_model, serialize_site_model(model));
    std::println("executed site model for {} -> {}", version_string, output_model.string());
    return 0;
} catch (const cli_usage_error& error) {
    std::println(stderr, "website execute: {}", error.what());
    return 2;
} catch (const std::exception& error) {
    std::println(stderr, "website execute: {}", error.what());
    return 3;
}

/// @brief Runs `website render`: renders a resolved model into staging without a DB.
///
/// Reads the site_model/v1 artifact (verifying its schema), renders every §16 route
/// into an owner-marked staging directory at the requested base path, and writes the
/// built-in model-validation stamp bound to the staged manifest hash (§14, §17.3
/// step 8). It never opens a cluster, so the same executed model can be rendered at
/// more than one base path; only `publish` may exchange staging with the live site.
/// @param args Options after the `render` token.
/// @return Zero on success; two on usage errors; three on parse/render failure.
[[nodiscard]] int run_render(std::span<std::string_view const> args) try {
    static constexpr std::string_view value_keys[] = {
        "model", "site-config", "templates", "static", "output", "base-path",
        "source-revision", "model-validation-stamp"};
    const auto options = parse_options(args, value_keys, {});
    const fs::path model_path = require_option(options, "model");
    const fs::path site_config_path = require_option(options, "site-config");
    const fs::path templates_dir = require_option(options, "templates");
    const fs::path static_dir = require_option(options, "static");
    const fs::path staging = require_option(options, "output");

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
    const site_model model = deserialize_site_model(read_file(model_path));
    const std::string manifest_hash = render_model_to_staging(
        model, config, base_path, revision, templates_dir, static_dir, staging);

    // Owner-mark staging (§168) so only publish may exchange it, and write the
    // built-in model-validation stamp bound to the staged manifest hash.
    const std::string nonce =
        std::to_string(::getpid()) + "-" + manifest_hash.substr(0, 16);
    write_private_artifact(staging.string() + ".owner.json",
        "{\"schema\":\"bioseq.website.owner/v1\",\"nonce\":\"" + nonce
        + "\",\"staging\":\"" + staging.filename().string() + "\"}\n");
    if (const auto found = options.find("model-validation-stamp"); found != options.end()) {
        write_if_different(fs::path(found->second), manifest_hash + "\n");
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
        "reachable-units", "compile-db", "site-config", "output",
        "reachable-output"};
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

    // The reachable interface-unit closure is the sole Doxygen INPUT inventory and
    // the exact unit list cpp_index parses (§11.1): emit it as a newline-delimited
    // file so those stages consume the closure instead of a recursive directory glob.
    if (const auto found = options.find("reachable-output"); found != options.end()) {
        std::string reachable_text;
        for (const module_graph_unit& unit : closure) {
            reachable_text += unit.source_path;
            reachable_text.push_back('\n');
        }
        write_if_different(fs::path(found->second), reachable_text);
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
    if (command == "execute-sql") {
        return run_execute_sql(rest);
    }
    if (command == "render") {
        return run_render(rest);
    }
    if (command == "render-fragment") {
        return run_render_fragment(rest);
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
