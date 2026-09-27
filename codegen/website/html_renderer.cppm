module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

module bioseq.codegen.website:html_renderer;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Converts content_node trees to HTML for template *_html fields.
// Escapes text and attribute values and checks link targets before writing
// markup, so Markdown and Doxygen text cannot supply arbitrary HTML tags.
namespace bioseq::codegen::website {

namespace {

// Escapes text placed inside an HTML element.
void append_escaped_text(std::string& out, std::string_view text) {
    for (const char character : text) {
        switch (character) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        // Encode braces so JSON or SQL examples cannot be mistaken for
        // unresolved template expressions in the generated page.
        case '{': out += "&#123;"; break;
        case '}': out += "&#125;"; break;
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

// Prism grammar and display label for a code-block language.
// Known aliases share a grammar. Unknown languages use plain text.
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

// Builds the code-block ID used by the Copy button. Include its position
// on the page so repeated blocks have different IDs.
[[nodiscard]] std::string code_block_id(std::string_view text, std::size_t ordinal) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char character : text) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    static constexpr std::string_view digits = "0123456789abcdef";
    std::string id = "code-";
    for (int shift = 60; shift >= 0; shift -= 4) {
        id.push_back(digits[(hash >> shift) & 0xF]);
    }
    id.push_back('-');
    id += std::to_string(ordinal);
    return id;
}

// Reads leading decimal digits from an annotation attribute.
// Returns zero if the string does not start with a digit.
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

// Accepts relative links, fragments, paths, and HTTP(S) or mailto URLs.
// Ordinary links and images use # for rejected targets.
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

struct html_content_writer {
    std::string out;
    std::size_t code_ordinal{0};
    const content_node* terminal_result{nullptr};

    void render_children(const std::vector<content_node>& children) {
        for (std::size_t index = 0; index < children.size(); ++index) {
            const content_node& child = children[index];
            const content_node* previous_result = terminal_result;
            const bool annotated_code = child.kind == content_node_kind::code_block
                && std::ranges::any_of(child.children, [](const content_node& nested) {
                       return nested.kind == content_node_kind::code_annotation;
                   });
            if (child.kind == content_node_kind::code_block
                && !annotated_code
                && index + 1 < children.size()
                && children[index + 1].kind == content_node_kind::table
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

    void wrap(std::string_view tag, const content_node& node) {
        out.push_back('<');
        out += tag;
        out.push_back('>');
        render_children(node.children);
        out += "</";
        out += tag;
        out.push_back('>');
    }

    [[nodiscard]] std::string_view attribute_or(
        const content_node& node, std::string_view key, std::string_view fallback) const {
        const auto found = node.attributes.find(key);
        return found == node.attributes.end() ? fallback : std::string_view(found->second);
    }

    // Doxygen paragraphs can contain lists and other blocks. Close the HTML
    // paragraph before rendering those children, since placing them inside p
    // would make the browser close it early and leave an unmatched closing tag.
    [[nodiscard]] static bool paragraph_child_is_block(content_node_kind kind) {
        switch (kind) {
        case content_node_kind::text:
        case content_node_kind::emphasis:
        case content_node_kind::strong_text:
        case content_node_kind::strikethrough:
        case content_node_kind::highlight:
        case content_node_kind::link:
        case content_node_kind::image:
        case content_node_kind::entity_link:
        case content_node_kind::code_span:
        case content_node_kind::footnote:
        case content_node_kind::line_break:
            return false;
        default:
            return true;
        }
    }

    void render_node(const content_node& node) {
        switch (node.kind) {
        case content_node_kind::root:
            render_children(node.children);
            break;
        case content_node_kind::paragraph: {
            bool paragraph_open = false;
            const auto open_paragraph = [this, &paragraph_open]() -> void {
                if (!paragraph_open) {
                    out += "<p class=\"doc-paragraph\">";
                    paragraph_open = true;
                }
            };
            const auto close_paragraph = [this, &paragraph_open]() -> void {
                if (paragraph_open) {
                    out += "</p>";
                    paragraph_open = false;
                }
            };
            if (node.children.empty()) {
                open_paragraph();
            }
            for (const content_node& child : node.children) {
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
        case content_node_kind::text: append_escaped_text(out, node.text); break;
        case content_node_kind::heading: {
            // The page template supplies h1. Keep body headings at h2 or below.
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
            // Use the generated anchor as the heading ID for table-of-contents links.
            const auto anchor = node.attributes.find("anchor");
            if (anchor != node.attributes.end() && !anchor->second.empty()) {
                out.push_back('<');
                out += tag;
                out += " class=\"hx\" id=\"";
                append_escaped_attribute(out, anchor->second);
                out += level > 2 ? "\" data-toc-sub data-title=\""
                                 : "\" data-toc data-title=\"";
                std::string title;
                const auto collect_text = [&title](
                                              const content_node& item,
                                              auto&& self_collect) -> void {
                    title += item.text;
                    for (const content_node& child : item.children) {
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
        case content_node_kind::emphasis: wrap("em", node); break;
        case content_node_kind::strong_text: wrap("strong", node); break;
        case content_node_kind::strikethrough: wrap("del", node); break;
        case content_node_kind::highlight: wrap("mark", node); break;
        case content_node_kind::code_span:
            out += "<code>";
            append_escaped_text(out, node.text);
            render_children(node.children);
            out += "</code>";
            break;
        case content_node_kind::code_block: {
            // Choose the code language and block ID. Ordinary blocks get a language
            // label and Copy button, with a Prism class for supported languages.
            const code_language_view language =
                classify_code_language(attribute_or(node, "language", ""));
            const std::string id = code_block_id(node.text, code_ordinal++);
            // Annotation children identify text ranges and numbered explanation
            // cards. Render their marks here and omit Prism highlighting, which
            // would replace the code element contents and remove those marks.
            struct anno_span { std::size_t start; std::size_t len; std::size_t number; };
            std::vector<anno_span> spans;
            std::vector<const content_node*> cards;
            for (const content_node& child : node.children) {
                if (child.kind == content_node_kind::code_annotation) {
                    const std::size_t start =
                        parse_index(attribute_or(child, "start", "0"));
                    const std::size_t len =
                        parse_index(attribute_or(child, "len", "0"));
                    // Ignore invalid annotation ranges before choosing the displayed text
                    // or adding cards. This also checks models loaded from saved JSON.
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
                // Show the smallest call expression that contains all annotations,
                // or keep the surrounding lines if none is found. For example, a
                // CREATE TABLE column can focus on bioseq.seq('dna4', 64) while
                // keeping its numbered marks and cards.
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

                out += "<figure class=\"doc-code annotated-type\" data-annotated-type";
                if (const auto runnable = node.attributes.find("runnable_id");
                    runnable != node.attributes.end()) {
                    out += " data-sql-example=\"";
                    append_escaped_attribute(out, runnable->second);
                    out.push_back('"');
                }
                out += "><div class=\"annotated-type-expression\"><code id=\"";
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
                std::ranges::sort(cards, [this](const content_node* lhs,
                                                const content_node* rhs) {
                    return parse_index(attribute_or(*lhs, "number", "0"))
                        < parse_index(attribute_or(*rhs, "number", "0"));
                });
                out += "<ol class=\"anno-cards annotated-type-cards\" data-split>";
                for (const content_node* card : cards) {
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

            out += "<figure class=\"doc-code\" data-term";
            if (const auto runnable = node.attributes.find("runnable_id");
                runnable != node.attributes.end()) {
                out += " data-sql-example=\"";
                append_escaped_attribute(out, runnable->second);
                out.push_back('"');
            }
            out += "><figcaption class=\"doc-code-bar\">"
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
            // The browser adds entity links to these source ranges after Prism
            // finishes syntax highlighting.
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
                        continue;  // skip overlapping or out-of-range marks
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
        case content_node_kind::link: {
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
        case content_node_kind::image: {
            const bool safe = is_safe_href(node.target);
            out += "<img src=\"";
            append_escaped_attribute(out, safe ? node.target : std::string("#"));
            out += "\" alt=\"";
            append_escaped_attribute(out, node.text);
            out += "\">";
            break;
        }
        case content_node_kind::entity_link: {
            // Resolved references become links with tooltip data. If the entity ID
            // or a usable route is missing, show the label as plain code.
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
        case content_node_kind::block_quote: wrap("blockquote", node); break;
        case content_node_kind::admonition: {
            // Markdown uses class for its callout kind. Doxygen uses kind for
            // warnings, parameters, and return descriptions. Choose a readable label
            // and one of the supported CSS styles for either form.
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
        case content_node_kind::ordered_list: wrap("ol", node); break;
        case content_node_kind::unordered_list: wrap("ul", node); break;
        case content_node_kind::list_item: wrap("li", node); break;
        case content_node_kind::table: {
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
                // Put the first result row in thead because it contains column names.
                // Keep data rows in tbody so screen readers can distinguish them.
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
        case content_node_kind::table_row: wrap("tr", node); break;
        case content_node_kind::table_cell: {
            // A nonempty scope attribute marks a header cell. Other cells use td.
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
        case content_node_kind::footnote: wrap("span", node); break;
        case content_node_kind::line_break: out += "<br>"; break;
        case content_node_kind::code_annotation:
            render_children(node.children);
            break;
        }
    }
};

}  // namespace

/// @brief Renders document nodes as HTML for a template content field.
/// @param nodes Tutorial or Doxygen body nodes.
/// @return HTML with text and attributes escaped and link targets checked.
[[nodiscard]] std::string render_content_html(const std::vector<content_node>& nodes) {
    html_content_writer writer;
    writer.render_children(nodes);
    return std::move(writer.out);
}

}  // namespace bioseq::codegen::website
