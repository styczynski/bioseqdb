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

import :markdown_extensions;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// MD4C driver (§10.2). Uses MD4C's typed callbacks to project CommonMark plus
// the enabled extensions onto the owned document_node tree; md4c (not
// md4c-html) is linked so this unit owns safe emission. No MD4C pointer ever
// escapes this translation unit.
namespace bioseq::codegen::website {

namespace {

struct markdown_builder {
    document_node root{.kind = document_node_kind::document};
    std::vector<document_node*> stack{&root};

    document_node& current() { return *stack.back(); }

    document_node& push(document_node_kind kind) {
        current().children.push_back(document_node{.kind = kind});
        document_node& child = current().children.back();
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

int on_enter_block(MD_BLOCKTYPE type, void* detail, void* userdata) {
    auto& builder = *static_cast<markdown_builder*>(userdata);
    switch (type) {
    case MD_BLOCK_DOC: break;
    case MD_BLOCK_P: builder.push(document_node_kind::paragraph); break;
    case MD_BLOCK_QUOTE: builder.push(document_node_kind::block_quote); break;
    case MD_BLOCK_UL: builder.push(document_node_kind::unordered_list); break;
    case MD_BLOCK_OL: builder.push(document_node_kind::ordered_list); break;
    case MD_BLOCK_LI: builder.push(document_node_kind::list_item); break;
    case MD_BLOCK_H: {
        document_node& node = builder.push(document_node_kind::heading);
        const auto* header = static_cast<MD_BLOCK_H_DETAIL*>(detail);
        node.attributes.emplace("level", std::to_string(header->level));
        break;
    }
    case MD_BLOCK_CODE: {
        document_node& node = builder.push(document_node_kind::code_block);
        const auto* code = static_cast<MD_BLOCK_CODE_DETAIL*>(detail);
        node.attributes.emplace("language", std::string(attribute_text(code->lang)));
        node.attributes.emplace("info", std::string(attribute_text(code->info)));
        break;
    }
    case MD_BLOCK_TABLE: builder.push(document_node_kind::table); break;
    case MD_BLOCK_TR: builder.push(document_node_kind::table_row); break;
    case MD_BLOCK_TH:
    case MD_BLOCK_TD: builder.push(document_node_kind::table_cell); break;
    case MD_BLOCK_ADMONITION: builder.push(document_node_kind::admonition); break;
    default: builder.push(document_node_kind::paragraph); break;
    }
    return 0;
}

int on_leave_block(MD_BLOCKTYPE type, void* /*detail*/, void* userdata) {
    auto& builder = *static_cast<markdown_builder*>(userdata);
    if (type != MD_BLOCK_DOC) {
        builder.pop();
    }
    return 0;
}

int on_enter_span(MD_SPANTYPE type, void* detail, void* userdata) {
    auto& builder = *static_cast<markdown_builder*>(userdata);
    switch (type) {
    case MD_SPAN_EM: builder.push(document_node_kind::emphasis); break;
    case MD_SPAN_STRONG: builder.push(document_node_kind::strong_text); break;
    case MD_SPAN_DEL: builder.push(document_node_kind::strikethrough); break;
    case MD_SPAN_MARK: builder.push(document_node_kind::highlight); break;
    case MD_SPAN_CODE: builder.push(document_node_kind::code_span); break;
    case MD_SPAN_A: {
        document_node& node = builder.push(document_node_kind::link);
        const auto* anchor = static_cast<MD_SPAN_A_DETAIL*>(detail);
        node.target = std::string(attribute_text(anchor->href));
        if (auto query = parse_entity_reference(node.target, source_span{})) {
            node.kind = document_node_kind::entity_link;
        }
        break;
    }
    case MD_SPAN_IMG: {
        document_node& node = builder.push(document_node_kind::image);
        const auto* image = static_cast<MD_SPAN_IMG_DETAIL*>(detail);
        node.target = std::string(attribute_text(image->src));
        break;
    }
    default: builder.push(document_node_kind::emphasis); break;
    }
    return 0;
}

int on_leave_span(MD_SPANTYPE /*type*/, void* /*detail*/, void* userdata) {
    auto& builder = *static_cast<markdown_builder*>(userdata);
    builder.pop();
    return 0;
}

int on_text(MD_TEXTTYPE type, const MD_CHAR* text, MD_SIZE size, void* userdata) {
    auto& builder = *static_cast<markdown_builder*>(userdata);
    std::string_view value(text, size);
    if (type == MD_TEXT_BR || type == MD_TEXT_SOFTBR) {
        builder.current().children.push_back(
            document_node{.kind = document_node_kind::line_break});
        return 0;
    }
    document_node& parent = builder.current();
    if (!parent.children.empty()
        && parent.children.back().kind == document_node_kind::text
        && type != MD_TEXT_CODE) {
        parent.children.back().text.append(value);
    } else {
        parent.children.push_back(
            document_node{.kind = document_node_kind::text, .text = std::string(value)});
    }
    return 0;
}

}  // namespace

/// @brief Parses Markdown body text into an owned document tree.
/// @param markdown Body text (frontmatter already removed).
/// @return The document-rooted node with its rendered children.
[[nodiscard]] document_node parse_markdown(std::string_view markdown) {
    markdown_builder builder;
    MD_PARSER parser{};
    parser.abi_version = 0;
    parser.flags = MD_FLAG_TABLES | MD_FLAG_STRIKETHROUGH | MD_FLAG_TASKLISTS
        | MD_FLAG_WIKILINKS | MD_FLAG_LATEXMATHSPANS | MD_DIALECT_GITHUB;
    parser.enter_block = &on_enter_block;
    parser.leave_block = &on_leave_block;
    parser.enter_span = &on_enter_span;
    parser.leave_span = &on_leave_span;
    parser.text = &on_text;
    const int status = md_parse(markdown.data(),
                                static_cast<MD_SIZE>(markdown.size()), &parser, &builder);
    if (status != 0) {
        throw std::runtime_error("MD4C failed to parse tutorial Markdown");
    }
    return std::move(builder.root);
}

}  // namespace bioseq::codegen::website
