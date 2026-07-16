module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

module bioseq.codegen.website:html_render;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Trusted document_node -> sanitized HTML stage (§14). This is one of the four
// closed render stages permitted to produce trusted_html fragments: it walks the
// owned document tree and emits HTML5 with every text/attribute value escaped and
// every href scheme-validated here, so no raw Markdown/Doxygen text ever reaches
// a template ValuesMap. The output is bound to templates through a *_html field.
namespace bioseq::codegen::website {

namespace {

// Escapes text for an HTML element body (§14 plain_text projection).
void append_escaped_text(std::string& out, std::string_view text) {
    for (const char character : text) {
        switch (character) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        default: out.push_back(character); break;
        }
    }
}

// Escapes text for a double-quoted HTML attribute value.
void append_escaped_attribute(std::string& out, std::string_view text) {
    for (const char character : text) {
        switch (character) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out.push_back(character); break;
        }
    }
}

[[nodiscard]] std::string to_ascii_lower(std::string_view value) {
    std::string lowered(value);
    for (char& character : lowered) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return lowered;
}

// A fenced language token normalized to its Prism grammar class and a visible
// label (§10.5). Aliases collapse to one grammar; an unrecognized token carries an
// empty class so the block renders as plain, unhighlighted text.
struct code_language_view {
    std::string_view prism_class;
    std::string_view label;
};

[[nodiscard]] code_language_view classify_code_language(std::string_view raw) {
    const std::string lowered = to_ascii_lower(raw);
    if (lowered == "bash" || lowered == "shell" || lowered == "sh") return {"bash", "Bash"};
    if (lowered == "sql") return {"sql", "SQL"};
    if (lowered == "cpp" || lowered == "c++" || lowered == "cppm" || lowered == "cxx") {
        return {"cpp", "C++"};
    }
    if (lowered == "cmake") return {"cmake", "CMake"};
    if (lowered == "json") return {"json", "JSON"};
    if (lowered == "yaml" || lowered == "yml") return {"yaml", "YAML"};
    return {"", "Text"};
}

// A stable, page-unique DOM id for one code block, derived from its exact source
// text plus its ordinal on the page: the Copy button targets it and it must never
// collide (the html5lib validator rejects duplicate ids, §20.7). Identical blocks
// on one page differ by ordinal.
[[nodiscard]] std::string code_block_id(std::string_view text, std::size_t ordinal) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    static constexpr char digits[] = "0123456789abcdef";
    std::string id = "code-";
    for (int shift = 60; shift >= 0; shift -= 4) {
        id.push_back(digits[(hash >> shift) & 0xF]);
    }
    id.push_back('-');
    id += std::to_string(ordinal);
    return id;
}

// Parses a small non-negative decimal from a node attribute (annotation range /
// number). Stops at the first non-digit; absent/garbage yields 0.
[[nodiscard]] std::size_t parse_index(std::string_view text) {
    std::size_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            break;
        }
        value = value * 10 + static_cast<std::size_t>(character - '0');
    }
    return value;
}

// Accepts only inert href schemes: relative/fragment/absolute-path links and the
// http(s)/mailto schemes. Anything else (javascript:, data:, vbscript:, unknown
// scheme) collapses to a harmless fragment so a hostile link cannot execute.
[[nodiscard]] bool is_safe_href(std::string_view href) {
    if (href.empty()) {
        return false;
    }
    if (href.front() == '/' || href.front() == '#' || href.front() == '.'
        || href.front() == '?') {
        return true;
    }
    const std::size_t colon = href.find(':');
    const std::size_t slash = href.find('/');
    // No scheme before the first path separator: a bare relative reference.
    if (colon == std::string_view::npos || (slash != std::string_view::npos && slash < colon)) {
        return true;
    }
    const std::string scheme = to_ascii_lower(href.substr(0, colon));
    return scheme == "https" || scheme == "http" || scheme == "mailto";
}

struct html_writer {
    std::string out;
    std::size_t code_ordinal{0};

    void render_children(const std::vector<document_node>& children) {
        for (const document_node& child : children) {
            render_node(child);
        }
    }

    void wrap(std::string_view tag, const document_node& node) {
        out.push_back('<');
        out += tag;
        out.push_back('>');
        render_children(node.children);
        out += "</";
        out += tag;
        out.push_back('>');
    }

    [[nodiscard]] std::string_view attribute_or(
        const document_node& node, std::string_view key, std::string_view fallback) const {
        const auto found = node.attributes.find(key);
        return found == node.attributes.end() ? fallback : std::string_view(found->second);
    }

    void render_node(const document_node& node) {
        switch (node.kind) {
        case document_node_kind::document:
            render_children(node.children);
            break;
        case document_node_kind::paragraph: wrap("p", node); break;
        case document_node_kind::text: append_escaped_text(out, node.text); break;
        case document_node_kind::heading: {
            // The page template owns the single <h1>; body headings start at h2 so
            // the document keeps one top-level heading (§15.4 accessibility).
            int level = 2;
            const auto found = node.attributes.find("level");
            if (found != node.attributes.end()) {
                level = std::atoi(found->second.c_str());
            }
            if (level < 2) {
                level = 2;
            }
            if (level > 6) {
                level = 6;
            }
            const std::string tag = "h" + std::to_string(level);
            // An on-this-page anchor (a generated [a-z0-9-] slug) becomes an id so
            // the table of contents can link to the heading (§15.1).
            const auto anchor = node.attributes.find("anchor");
            if (anchor != node.attributes.end() && !anchor->second.empty()) {
                out.push_back('<');
                out += tag;
                out += " id=\"";
                out += anchor->second;
                out += "\">";
                render_children(node.children);
                out += "</";
                out += tag;
                out.push_back('>');
            } else {
                wrap(tag, node);
            }
            break;
        }
        case document_node_kind::emphasis: wrap("em", node); break;
        case document_node_kind::strong_text: wrap("strong", node); break;
        case document_node_kind::strikethrough: wrap("del", node); break;
        case document_node_kind::highlight: wrap("mark", node); break;
        case document_node_kind::code_span:
            out += "<code>";
            append_escaped_text(out, node.text);
            render_children(node.children);
            out += "</code>";
            break;
        case document_node_kind::code_block: {
            // Every fenced block carries a visible language label, a keyboard-focusable
            // Copy button bound by id to the exact source (no inline handler; main.js
            // wires the click and the polite live region), and a Prism grammar class so
            // the vendored highlighter can decorate it client-side (§10.5).
            const code_language_view language =
                classify_code_language(attribute_or(node, "language", ""));
            const std::string id = code_block_id(node.text, code_ordinal++);
            // §10.6: an annotation directive was folded into a code_annotation child
            // carrying a source range + number. Collect the ranges to underline the
            // matched spans and the cards to render below. An annotated block skips the
            // Prism class so the server-rendered <mark> spans survive (Prism rewrites
            // <code> innerHTML). Copy still returns the exact source: <mark> holds the
            // original substring, so textContent equals the source.
            struct anno_span { std::size_t start; std::size_t len; std::size_t number; };
            std::vector<anno_span> spans;
            std::vector<const document_node*> cards;
            for (const document_node& child : node.children) {
                if (child.kind == document_node_kind::code_annotation) {
                    spans.push_back({parse_index(attribute_or(child, "start", "0")),
                                     parse_index(attribute_or(child, "len", "0")),
                                     parse_index(attribute_or(child, "number", "0"))});
                    cards.push_back(&child);
                }
            }
            std::ranges::sort(spans, [](const anno_span& lhs, const anno_span& rhs) {
                return lhs.start < rhs.start;
            });
            const bool annotated = !spans.empty();
            out += "<figure class=\"doc-code\"><figcaption class=\"doc-code-bar\">"
                   "<span class=\"doc-code-lang\">";
            append_escaped_text(out, language.label);
            out += "</span><button type=\"button\" class=\"copy-button\" data-copy-target=\"";
            append_escaped_attribute(out, id);
            out += "\" aria-label=\"Copy code to clipboard\">Copy</button></figcaption>"
                   "<pre><code id=\"";
            append_escaped_attribute(out, id);
            out.push_back('"');
            if (!language.prism_class.empty() && !annotated) {
                out += " class=\"language-";
                append_escaped_attribute(out, language.prism_class);
                out.push_back('"');
            }
            // §12.6: generator-resolved non-overlapping source ranges the client
            // decorator wraps in entity links after Prism completes.
            const std::string_view entity_ranges = attribute_or(node, "entity_ranges", "");
            if (!entity_ranges.empty()) {
                out += " data-entity-ranges=\"";
                append_escaped_attribute(out, entity_ranges);
                out.push_back('"');
            }
            out.push_back('>');
            if (spans.empty()) {
                append_escaped_text(out, node.text);
            } else {
                const std::string_view text = node.text;
                std::size_t cursor = 0;
                for (const anno_span& span : spans) {
                    if (span.start < cursor || span.start + span.len > text.size()) {
                        continue;  // defensive: never emit an overlapping/out-of-range mark
                    }
                    append_escaped_text(out, text.substr(cursor, span.start - cursor));
                    out += "<mark class=\"anno\" data-anno=\"";
                    out += std::to_string(span.number);
                    out += "\">";
                    append_escaped_text(out, text.substr(span.start, span.len));
                    out += "</mark>";
                    cursor = span.start + span.len;
                }
                append_escaped_text(out, text.substr(cursor));
            }
            out += "</code></pre>";
            if (annotated) {
                std::ranges::sort(cards, [this](const document_node* lhs,
                                                const document_node* rhs) {
                    return parse_index(attribute_or(*lhs, "number", "0"))
                        < parse_index(attribute_or(*rhs, "number", "0"));
                });
                out += "<ol class=\"anno-cards\">";
                for (const document_node* card : cards) {
                    out += "<li class=\"anno-card\"><span class=\"anno-card-num\" "
                           "aria-hidden=\"true\">";
                    out += std::to_string(parse_index(attribute_or(*card, "number", "0")));
                    out += "</span><div class=\"anno-card-body\"><p class=\"anno-card-title\">";
                    append_escaped_text(out, attribute_or(*card, "title", ""));
                    out += "</p>";
                    render_children(card->children);
                    out += "</div></li>";
                }
                out += "</ol>";
            }
            out += "</figure>";
            break;
        }
        case document_node_kind::link: {
            const bool safe = is_safe_href(node.target);
            out += "<a href=\"";
            append_escaped_attribute(out, safe ? node.target : std::string("#"));
            out += "\"";
            if (safe && node.target.starts_with("http")) {
                out += " rel=\"noreferrer\"";
            }
            out.push_back('>');
            render_children(node.children);
            out += "</a>";
            break;
        }
        case document_node_kind::image: {
            const bool safe = is_safe_href(node.target);
            out += "<img src=\"";
            append_escaped_attribute(out, safe ? node.target : std::string("#"));
            out += "\" alt=\"";
            append_escaped_attribute(out, node.text);
            out += "\">";
            break;
        }
        case document_node_kind::entity_link: {
            // A resolved reference (the link resolver set entity_id + a base-aware
            // route in target, §12.1/§12.4) renders as a live cross-link carrying the
            // tooltip data attributes; an unresolved one renders as inert code (the
            // build fails before this in normal operation, §10.4).
            const std::string_view entity_id = attribute_or(node, "entity_id", "");
            const std::string_view label = attribute_or(node, "entity_label", "");
            if (!entity_id.empty() && !node.target.empty() && is_safe_href(node.target)) {
                out += "<a class=\"entity-ref\" href=\"";
                append_escaped_attribute(out, node.target);
                out += "\" data-entity-id=\"";
                append_escaped_attribute(out, entity_id);
                const std::string_view tooltip = attribute_or(node, "tooltip_url", "");
                if (!tooltip.empty()) {
                    out += "\" data-entity-data-url=\"";
                    append_escaped_attribute(out, tooltip);
                }
                out += "\">";
                if (!label.empty()) {
                    append_escaped_text(out, label);
                } else if (node.children.empty() && node.text.empty()) {
                    append_escaped_text(out, node.target);
                } else {
                    append_escaped_text(out, node.text);
                    render_children(node.children);
                }
                out += "</a>";
            } else {
                out += "<code class=\"entity-ref\">";
                if (!label.empty()) {
                    append_escaped_text(out, label);
                } else if (node.children.empty() && node.text.empty()) {
                    append_escaped_text(out, node.target);
                } else {
                    append_escaped_text(out, node.text);
                    render_children(node.children);
                }
                out += "</code>";
            }
            break;
        }
        case document_node_kind::block_quote: wrap("blockquote", node); break;
        case document_node_kind::admonition: {
            const std::string_view kind = attribute_or(node, "class", "note");
            out += "<aside class=\"admonition admonition-";
            append_escaped_attribute(out, kind);
            out += "\"><p class=\"admonition-label\">";
            append_escaped_text(out, kind);
            out += "</p>";
            render_children(node.children);
            out += "</aside>";
            break;
        }
        case document_node_kind::ordered_list: wrap("ol", node); break;
        case document_node_kind::unordered_list: wrap("ul", node); break;
        case document_node_kind::list_item: wrap("li", node); break;
        case document_node_kind::table: {
            const std::string_view table_class = attribute_or(node, "class", "");
            out += "<table";
            if (!table_class.empty()) {
                out += " class=\"";
                append_escaped_attribute(out, table_class);
                out.push_back('"');
            }
            out.push_back('>');
            render_children(node.children);
            out += "</table>";
            break;
        }
        case document_node_kind::table_row: wrap("tr", node); break;
        case document_node_kind::table_cell: {
            // A cell with a scope attribute is a header cell (§15.4 accessible data
            // tables); otherwise it is a plain data cell.
            const std::string_view scope = attribute_or(node, "scope", "");
            if (scope.empty()) {
                wrap("td", node);
            } else {
                out += "<th scope=\"";
                append_escaped_attribute(out, scope);
                out += "\">";
                render_children(node.children);
                out += "</th>";
            }
            break;
        }
        case document_node_kind::footnote: wrap("span", node); break;
        case document_node_kind::line_break: out += "<br>"; break;
        case document_node_kind::code_annotation:
            render_children(node.children);
            break;
        }
    }
};

}  // namespace

/// @brief Renders an owned document node forest to sanitized trusted HTML (§14).
/// @param nodes Document children (a tutorial/Doxygen body).
/// @return HTML5 with every text/attribute escaped and every href validated.
[[nodiscard]] std::string render_document_html(const std::vector<document_node>& nodes) {
    html_writer writer;
    writer.render_children(nodes);
    return std::move(writer.out);
}

}  // namespace bioseq::codegen::website
