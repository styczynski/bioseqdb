module;

#include <md4c.h>

#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:markdown;

import :markdown_directives;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Converts Markdown into content_node trees through MD4C callbacks.
// Copies parser data into owned strings and nodes so later stages can
// render the content without keeping MD4C objects alive.
namespace bioseq::codegen::website {

namespace {

struct markdown_tree_builder {
    content_node root{.kind = content_node_kind::root};
    std::vector<content_node*> stack{&root};

    content_node& current() { return *stack.back(); }

    content_node& push(content_node_kind kind) {
        current().children.push_back(content_node{.kind = kind});
        content_node& child = current().children.back();
        stack.push_back(&child);
        return child;
    }

    void pop() {
        if (stack.size() > 1) {
            stack.pop_back();
        }
    }
};

[[nodiscard]] std::string_view attribute_text(const MD_ATTRIBUTE& attribute) {
    return attribute.text ? std::string_view(attribute.text, attribute.size)
                          : std::string_view{};
}

int enter_markdown_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& builder = *static_cast<markdown_tree_builder*>(userdata);
    switch (type) {
    case MD_BLOCK_DOC: break;
    case MD_BLOCK_P: builder.push(content_node_kind::paragraph); break;
    case MD_BLOCK_QUOTE: builder.push(content_node_kind::block_quote); break;
    case MD_BLOCK_UL: builder.push(content_node_kind::unordered_list); break;
    case MD_BLOCK_OL: builder.push(content_node_kind::ordered_list); break;
    case MD_BLOCK_LI: builder.push(content_node_kind::list_item); break;
    case MD_BLOCK_H: {
        content_node& node = builder.push(content_node_kind::heading);
        const auto* header = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        node.attributes.emplace("level", std::to_string(header->level));
        break;
    }
    case MD_BLOCK_CODE: {
        content_node& node = builder.push(content_node_kind::code_block);
        const auto* code = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        node.attributes.emplace("language", std::string(attribute_text(code->lang)));
        node.attributes.emplace("info", std::string(attribute_text(code->info)));
        break;
    }
    case MD_BLOCK_TABLE: builder.push(content_node_kind::table); break;
    case MD_BLOCK_TR: builder.push(content_node_kind::table_row); break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: builder.push(content_node_kind::table_cell); break;
    case MD_BLOCK_ADMONITION: {
        content_node& node = builder.push(content_node_kind::admonition);
        const auto* admonition = static_cast<MD_BLOCK_ADMONITION_DETAIL*>(detail);
        // Keep the admonition kind so warnings and tips get the right styling.
        // The shared mapping leaves unknown kinds unset, allowing the renderer
        // to fall  back to the note style.
        if (std::string severity = admonition_class(attribute_text(admonition->type));
            !severity.empty()) {
            node.attributes.emplace("class", std::move(severity));
        }
        break;
    }
    default: builder.push(content_node_kind::paragraph); break;
    }
    return 0;
}

int leave_markdown_block(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto& builder = *static_cast<markdown_tree_builder*>(userdata);
    if (type != MD_BLOCK_DOC) {
        builder.pop();
    }
    return 0;
}

int enter_markdown_span(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& builder = *static_cast<markdown_tree_builder*>(userdata);
    switch (type) {
    case MD_SPAN_EM: builder.push(content_node_kind::emphasis); break;
    case MD_SPAN_STRONG: builder.push(content_node_kind::strong_text); break;
    case MD_SPAN_DEL: builder.push(content_node_kind::strikethrough); break;
    case MD_SPAN_MARK: builder.push(content_node_kind::highlight); break;
    case MD_SPAN_CODE: builder.push(content_node_kind::code_span); break;
    case MD_SPAN_A: {
        content_node& node = builder.push(content_node_kind::link);
        const auto* anchor = static_cast<MD_SPAN_A_DETAIL*>(detail);
        node.target = std::string(attribute_text(anchor->href));
        if (auto query = parse_entity_query(node.target, source_span{})) {
            node.kind = content_node_kind::entity_link;
        }
        break;
    }
    case MD_SPAN_IMG: {
        content_node& node = builder.push(content_node_kind::image);
        const auto* image = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        node.target = std::string(attribute_text(image->src));
        break;
    }
    case MD_SPAN_WIKILINK: {
        content_node& node = builder.push(content_node_kind::entity_link);
        const auto* wiki = static_cast<MD_SPAN_WIKILINK_DETAIL*>(detail);
        node.target = std::string(attribute_text(wiki->target));
        break;
    }
    default: builder.push(content_node_kind::emphasis); break;
    }
    return 0;
}

int leave_markdown_span(MD_SPANTYPE /*type*/, void* /*detail*/, void* userdata) {
    auto& builder = *static_cast<markdown_tree_builder*>(userdata);
    builder.pop();
    return 0;
}

int append_markdown_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& builder = *static_cast<markdown_tree_builder*>(userdata);
    std::string_view value(text, size);
    content_node& parent = builder.current();
    if (parent.kind == content_node_kind::code_block) {
        // Keep code-block text and newlines exactly as the parser supplies them.
        parent.text.append(value);
        return 0;
    }
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        parent.children.push_back(
            content_node{.kind = content_node_kind::line_break});
        return 0;
    }
    if (!parent.children.empty()
        && parent.children.back().kind == content_node_kind::text
        && type != MD_TEXT_CODE) {
        parent.children.back().text.append(value);
    } else {
        parent.children.push_back(
            content_node{.kind = content_node_kind::text, .text = std::string(value)});
    }
    return 0;
}

}  // namespace

/// @brief Parses Markdown into a content tree.
/// @param markdown Body text with frontmatter removed.
/// @return Root node containing the parsed blocks.
[[nodiscard]] content_node parse_markdown_content(std::string_view markdown) {
    markdown_tree_builder builder;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS
        | MD_FLAG_WIKILINKS | MD_FLAG_LATEXMATHSPANS | MD_DIALECT_GITHUB;
    parser.enter_block = &enter_markdown_block;
    parser.leave_block = &leave_markdown_block;
    parser.enter_span = &enter_markdown_span;
    parser.leave_span = &leave_markdown_span;
    parser.text = &append_markdown_text;
    const int status = md_parse(markdown.data(),
                                static_cast<MD_SIZE>(markdown.size()), &parser, &builder);
    if (status != 0) {
        throw std::runtime_error("MD4C failed to parse tutorial Markdown");
    }
    return std::move(builder.root);
}

}  // namespace bioseq::codegen::website
