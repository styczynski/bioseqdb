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
    const document_node* terminal_result{nullptr};

    void render_children(const std::vector<document_node>& children) {
        for (std::size_t index = 0; index < children.size(); ++index) {
            const document_node& child = children[index];
            const document_node* previous_result = terminal_result;
            const bool annotated_code = child.kind == document_node_kind::code_block
                && std::ranges::any_of(child.children, [](const document_node& nested) {
                       return nested.kind == document_node_kind::code_annotation;
                   });
            if (child.kind == document_node_kind::code_block
                && !annotated_code
                && index + 1 < children.size()
                && children[index + 1].kind == document_node_kind::table
                && attribute_or(children[index + 1], "class", "") == "result-table") {
                terminal_result = &children[index + 1];
                render_node(child);
                terminal_result = previous_result;
                ++index;
            } else {
                render_node(child);
            }
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

    // Doxygen may place block elements such as itemized/ordered lists directly
    // inside a <para>. HTML <p> accepts phrasing content only and is implicitly
    // closed by a block element, so wrapping the whole model paragraph would emit
    // an unmatched trailing </p>. Keep the closed set of phrasing node kinds here;
    // every other model node is rendered as a sibling block.
    [[nodiscard]] static bool paragraph_child_is_block(document_node_kind kind) {
        switch (kind) {
        case document_node_kind::text:
        case document_node_kind::emphasis:
        case document_node_kind::strong_text:
        case document_node_kind::strikethrough:
        case document_node_kind::highlight:
        case document_node_kind::link:
        case document_node_kind::image:
        case document_node_kind::entity_link:
        case document_node_kind::code_span:
        case document_node_kind::footnote:
        case document_node_kind::line_break:
            return false;
        default:
            return true;
        }
    }

    void render_node(const document_node& node) {
        switch (node.kind) {
        case document_node_kind::document:
            render_children(node.children);
            break;
        case document_node_kind::paragraph: {
            bool paragraph_open = false;
            const auto open_paragraph = [&] {
                if (!paragraph_open) {
                    out += "<p class=\"doc-paragraph\">";
                    paragraph_open = true;
                }
            };
            const auto close_paragraph = [&] {
                if (paragraph_open) {
                    out += "</p>";
                    paragraph_open = false;
                }
            };
            if (node.children.empty()) {
                open_paragraph();
            }
            for (const document_node& child : node.children) {
                if (paragraph_child_is_block(child.kind)) {
                    close_paragraph();
                    render_node(child);
                } else {
                    open_paragraph();
                    render_node(child);
                }
            }
            close_paragraph();
            break;
        }
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
                out += " class=\"hx\" id=\"";
                append_escaped_attribute(out, anchor->second);
                out += level > 2 ? "\" data-toc-sub data-title=\""
                                 : "\" data-toc data-title=\"";
                std::string title;
                const auto collect_text = [&](const document_node& item,
                                              auto&& self_collect) -> void {
                    title += item.text;
                    for (const document_node& child : item.children) {
                        self_collect(child, self_collect);
                    }
                };
                collect_text(node, collect_text);
                append_escaped_attribute(out, title);
                out += "\">";
                render_children(node.children);
                out += "<a data-hash href=\"#";
                append_escaped_attribute(out, anchor->second);
                out += "\" aria-label=\"Link to this section\">#</a>";
                out += "</";
                out += tag;
                out.push_back('>');
            } else {
                out.push_back('<');
                out += tag;
                out += " class=\"hx\">";
                render_children(node.children);
                out += "</";
                out += tag;
                out.push_back('>');
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
            // <code> innerHTML).
            struct anno_span { std::size_t start; std::size_t len; std::size_t number; };
            std::vector<anno_span> spans;
            std::vector<const document_node*> cards;
            for (const document_node& child : node.children) {
                if (child.kind == document_node_kind::code_annotation) {
                    const std::size_t start =
                        parse_index(attribute_or(child, "start", "0"));
                    const std::size_t len =
                        parse_index(attribute_or(child, "len", "0"));
                    // Invalid ranges must not influence the focus window or produce
                    // a card with no corresponding underline. process_annotations
                    // already guarantees valid ranges; this remains a safe boundary
                    // for a deserialized/tampered model.
                    if (len > 0 && start <= node.text.size()
                        && len <= node.text.size() - start) {
                        spans.push_back({start, len,
                                         parse_index(attribute_or(child, "number", "0"))});
                        cards.push_back(&child);
                    }
                }
            }
            std::ranges::sort(spans, [](const anno_span& lhs, const anno_span& rhs) {
                return lhs.start < rhs.start;
            });
            const bool annotated = !spans.empty();

            if (annotated) {
                // The supplied documentation design treats an annotated type as an
                // anatomy component, not a terminal. Focus the smallest single-line
                // call expression that encloses every source range. For example, the
                // declaration `CREATE TABLE ... bioseq.seq('dna4', 64)` becomes the
                // centered `bioseq.seq('dna4', 64)` expression while its two authored
                // annotation ranges and prose remain fully data-driven.
                std::size_t marked_start = spans.front().start;
                std::size_t marked_end = spans.front().start + spans.front().len;
                for (const anno_span& span : spans) {
                    marked_start = std::min(marked_start, span.start);
                    marked_end = std::max(marked_end, span.start + span.len);
                }
                const std::size_t line_start_position =
                    node.text.rfind('\n', marked_start);
                const std::size_t line_start = line_start_position == std::string::npos
                    ? 0 : line_start_position + 1;
                const std::size_t newline = node.text.find('\n', marked_end);
                const std::size_t line_end = newline == std::string::npos
                    ? node.text.size() : newline;
                std::size_t focus_start = line_start;
                std::size_t focus_end = line_end;
                const auto is_name_character = [](char character) {
                    return (character >= 'a' && character <= 'z')
                        || (character >= 'A' && character <= 'Z')
                        || (character >= '0' && character <= '9')
                        || character == '_' || character == '.' || character == ':';
                };
                std::size_t best_width = line_end - line_start + 1;
                for (std::size_t open = line_start; open < line_end; ++open) {
                    if (node.text[open] != '(') {
                        continue;
                    }
                    std::size_t name_start = open;
                    while (name_start > line_start
                           && is_name_character(node.text[name_start - 1])) {
                        --name_start;
                    }
                    if (name_start == open || name_start > marked_start) {
                        continue;
                    }
                    std::size_t depth = 0;
                    std::size_t close = std::string::npos;
                    for (std::size_t cursor = open; cursor < line_end; ++cursor) {
                        if (node.text[cursor] == '(') {
                            ++depth;
                        } else if (node.text[cursor] == ')' && --depth == 0) {
                            close = cursor;
                            break;
                        }
                    }
                    if (close == std::string::npos || close + 1 < marked_end) {
                        continue;
                    }
                    const std::size_t width = close + 1 - name_start;
                    if (width < best_width) {
                        best_width = width;
                        focus_start = name_start;
                        focus_end = close + 1;
                    }
                }
                while (focus_start < focus_end
                       && (node.text[focus_start] == ' ' || node.text[focus_start] == '\t')) {
                    ++focus_start;
                }
                while (focus_end > focus_start
                       && (node.text[focus_end - 1] == ' ' || node.text[focus_end - 1] == '\t')) {
                    --focus_end;
                }

                out += "<figure class=\"doc-code annotated-type\" data-annotated-type>"
                       "<div class=\"annotated-type-expression\"><code id=\"";
                append_escaped_attribute(out, id);
                out += "\" data-code tabindex=\"0\" aria-label=\"Annotated type expression\">";
                std::size_t cursor = focus_start;
                const std::size_t open = node.text.find('(', focus_start);
                for (const anno_span& span : spans) {
                    if (span.start < focus_start || span.start + span.len > focus_end
                        || span.start < cursor) {
                        continue;
                    }
                    if (cursor < span.start) {
                        if (cursor == focus_start && open != std::string::npos
                            && open < span.start && open < focus_end) {
                            out += "<span class=\"annotated-type-name\">";
                            append_escaped_text(
                                out, std::string_view(node.text).substr(cursor, open - cursor));
                            out += "</span>";
                            append_escaped_text(
                                out, std::string_view(node.text).substr(open, span.start - open));
                        } else {
                            append_escaped_text(
                                out, std::string_view(node.text).substr(
                                         cursor, span.start - cursor));
                        }
                    }
                    out += "<mark class=\"anno annotated-type-value annotated-type-value-";
                    out += std::to_string(span.number);
                    out += "\" data-anno=\"";
                    out += std::to_string(span.number);
                    out += "\">";
                    append_escaped_text(
                        out, std::string_view(node.text).substr(span.start, span.len));
                    out += "</mark>";
                    cursor = span.start + span.len;
                }
                if (cursor < focus_end) {
                    append_escaped_text(
                        out, std::string_view(node.text).substr(cursor, focus_end - cursor));
                }
                out += "</code></div>";
                std::ranges::sort(cards, [this](const document_node* lhs,
                                                const document_node* rhs) {
                    return parse_index(attribute_or(*lhs, "number", "0"))
                        < parse_index(attribute_or(*rhs, "number", "0"));
                });
                out += "<ol class=\"anno-cards annotated-type-cards\" data-split>";
                for (const document_node* card : cards) {
                    const std::size_t number =
                        parse_index(attribute_or(*card, "number", "0"));
                    out += "<li class=\"anno-card annotated-type-card annotated-type-card-";
                    out += std::to_string(number);
                    out += "\" data-anno-card=\"";
                    out += std::to_string(number);
                    out += "\"><span class=\"anno-card-num\" aria-hidden=\"true\">";
                    out += std::to_string(number);
                    out += "</span><div class=\"anno-card-body\"><p class=\"anno-card-title\">";
                    append_escaped_text(out, attribute_or(*card, "title", ""));
                    out += "</p>";
                    render_children(card->children);
                    out += "</div></li>";
                }
                out += "</ol></figure>";
                break;
            }

            out += "<figure class=\"doc-code\" data-term><figcaption class=\"doc-code-bar\">"
                   "<span class=\"term-dot term-dot-red\" aria-hidden=\"true\"></span>"
                   "<span class=\"term-dot term-dot-amber\" aria-hidden=\"true\"></span>"
                   "<span class=\"term-dot term-dot-blue\" aria-hidden=\"true\"></span>"
                   "<span class=\"doc-code-lang\">";
            append_escaped_text(out, language.label);
            out += "</span><button type=\"button\" class=\"copy-button\" data-copy data-copy-target=\"";
            append_escaped_attribute(out, id);
            out += "\" aria-label=\"Copy code to clipboard\">Copy</button></figcaption>"
                   "<pre data-code><code id=\"";
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
            if (terminal_result != nullptr) {
                const std::size_t row_count = terminal_result->children.empty()
                    ? 0 : terminal_result->children.size() - 1;
                out += "<div class=\"terminal-result\" data-result>"
                       "<div class=\"terminal-result-label\">Result · ";
                out += std::to_string(row_count);
                out += row_count == 1 ? " row</div>" : " rows</div>";
                render_node(*terminal_result);
                out += "</div>";
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
                out += "\" data-entity=\"";
                append_escaped_attribute(out, entity_id);
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
            // Markdown stores one of the five authored severities in `class`;
            // Doxygen stores semantic kinds (`return`, `param:*`, `warning`, ...)
            // in `kind`. Preserve the useful label while collapsing the latter to
            // the supplied callout colour vocabulary (never emit punctuation such
            // as ':' into a CSS class token).
            std::string raw_kind(attribute_or(node, "class", ""));
            if (raw_kind.empty()) {
                raw_kind = std::string(attribute_or(node, "kind", "note"));
            }
            raw_kind = to_ascii_lower(raw_kind);
            std::string tone = "note";
            std::string label = "Note";
            if (raw_kind == "tip") {
                tone = "tip";
                label = "Tip";
            } else if (raw_kind == "important") {
                tone = "important";
                label = "Important";
            } else if (raw_kind == "warning" || raw_kind == "attention") {
                tone = "warning";
                label = "Warning";
            } else if (raw_kind == "caution" || raw_kind == "error"
                       || raw_kind == "exception") {
                tone = "caution";
                label = raw_kind == "exception" ? "Exception" : "Caution";
            } else if (raw_kind == "return" || raw_kind == "retval") {
                label = "Returns";
            } else if (raw_kind.starts_with("param:")) {
                label = "Parameters";
            } else if (raw_kind == "pre") {
                label = "Precondition";
            } else if (raw_kind == "post") {
                label = "Postcondition";
            } else if (raw_kind == "remark") {
                label = "Remark";
            } else if (raw_kind == "see") {
                label = "See also";
            }
            out += "<aside class=\"admonition admonition-";
            append_escaped_attribute(out, tone);
            out += "\" data-callout=\"";
            append_escaped_attribute(out, tone);
            out += "\"><span class=\"admonition-icon\" aria-hidden=\"true\"></span>"
                   "<div class=\"admonition-body\"><p class=\"admonition-label\">";
            append_escaped_text(out, label);
            out += "</p>";
            render_children(node.children);
            out += "</div></aside>";
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
            if (table_class == "result-table") {
                out += " aria-label=\"Executed SQL result\"";
            }
            out.push_back('>');
            if (table_class == "result-table" && !node.children.empty()) {
                // PostgreSQL result nodes always store the column header first.
                // Preserve that semantic split in the DOM, matching the supplied
                // terminal-result table and improving screen-reader navigation.
                out += "<thead>";
                render_node(node.children.front());
                out += "</thead><tbody>";
                for (std::size_t index = 1; index < node.children.size(); ++index) {
                    render_node(node.children[index]);
                }
                out += "</tbody>";
            } else {
                render_children(node.children);
            }
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
