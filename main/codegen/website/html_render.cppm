module;

#include <cstddef>
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
            wrap(tag, node);
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
            const std::string_view language = attribute_or(node, "language", "");
            out += "<pre><code";
            if (!language.empty()) {
                out += " class=\"language-";
                append_escaped_attribute(out, language);
                out.push_back('"');
            }
            out.push_back('>');
            append_escaped_text(out, node.text);
            out += "</code></pre>";
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
        case document_node_kind::entity_link:
            // Resolution to a real entity route arrives with the link resolver;
            // until then the reference renders as inert code showing its label
            // (the child text / target), never a live link and never doubled.
            out += "<code class=\"entity-ref\">";
            if (node.children.empty() && node.text.empty()) {
                append_escaped_text(out, node.target);
            } else {
                append_escaped_text(out, node.text);
                render_children(node.children);
            }
            out += "</code>";
            break;
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
        case document_node_kind::table:
            out += "<table>";
            render_children(node.children);
            out += "</table>";
            break;
        case document_node_kind::table_row: wrap("tr", node); break;
        case document_node_kind::table_cell: wrap("td", node); break;
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
