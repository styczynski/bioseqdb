module;

#include <pugixml.hpp>

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:doxygen_reference;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Doxygen XML reader (§11.2). pugixml is the sole XML parser and stays in this
// unit's global module fragment. Doxygen owns documentation-comment parsing;
// this stage projects its compound/member XML onto reference_entity records
// and typed content_node trees that entity_index later merges with the Clang
// structural index. Doxygen text is escaped and reconstructed through the typed
// renderer downstream; it never becomes trusted arbitrary HTML.
namespace bioseq::codegen::website {

/// @brief One compound reference discovered in a Doxygen index.xml.
struct doxygen_compound_ref {
    std::string refid;
    std::string kind;
    std::string name;
};

/// @brief Typed conversion of a Doxygen description element.
struct doxygen_description_result {
    std::vector<content_node> nodes;
    std::vector<diagnostic> diagnostics;
};

/// @brief Parses a Doxygen index.xml into its compound inventory.
/// @param xml Raw index.xml text.
/// @return The listed compounds in document order.
[[nodiscard]] std::vector<doxygen_compound_ref> parse_doxygen_compound_index(std::string_view xml) {
    pugi::xml_document document;
    const pugi::xml_parse_result result =
        document.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw std::runtime_error(std::string("cannot parse Doxygen index.xml: ")
                                 + result.description());
    }
    std::vector<doxygen_compound_ref> compounds;
    const pugi::xml_node index = document.child("doxygenindex");
    for (pugi::xml_node compound : index.children("compound")) {
        doxygen_compound_ref reference;
        reference.refid = compound.attribute("refid").as_string();
        reference.kind = compound.attribute("kind").as_string();
        reference.name = compound.child_value("name");
        compounds.push_back(std::move(reference));
    }
    return compounds;
}

namespace {

// Doxygen punctuation entity element → exact Unicode text (§11.2). Encoded as
// UTF-8 bytes so goldens carry the same characters Doxygen 1.9.8 emits.
[[nodiscard]] std::optional<std::string_view> punctuation_replacement(
    std::string_view name) {
    if (name == "ndash") { return std::string_view("\xE2\x80\x93"); }  // U+2013
    if (name == "mdash") { return std::string_view("\xE2\x80\x94"); }  // U+2014
    if (name == "lsquo") { return std::string_view("\xE2\x80\x98"); }  // U+2018
    if (name == "rsquo") { return std::string_view("\xE2\x80\x99"); }  // U+2019
    if (name == "ldquo") { return std::string_view("\xE2\x80\x9C"); }  // U+201C
    if (name == "rdquo") { return std::string_view("\xE2\x80\x9D"); }  // U+201D
    return std::nullopt;
}

// External links must be http/https; file:, javascript:, data:, and
// protocol-relative URLs are rejected (§11.2).
[[nodiscard]] bool is_safe_external_url(std::string_view url) {
    if (url.starts_with("//")) {
        return false;
    }
    return url.starts_with("http://") || url.starts_with("https://");
}

struct conversion_context {
    std::string owning_entity;
    std::vector<diagnostic>* diagnostics;
};

void report(conversion_context& context, std::string code, std::string message) {
    if (context.diagnostics == nullptr) {
        return;
    }
    diagnostic entry;
    entry.severity = diagnostic_severity::warning;
    entry.operation = diagnostic_operation::doxygen_conversion;
    entry.code = std::move(code);
    entry.message = std::move(message);
    if (!context.owning_entity.empty()) {
        entry.subject_identifier = context.owning_entity;
    }
    context.diagnostics->push_back(std::move(entry));
}

void append_text(std::vector<content_node>& out, std::string text) {
    if (text.empty()) {
        return;
    }
    content_node node;
    node.kind = content_node_kind::text;
    node.text = std::move(text);
    out.push_back(std::move(node));
}

// Concatenates raw text of a code context (verbatim / programlisting),
// expanding <sp/> to a space and terminating each <codeline> with a newline.
[[nodiscard]] std::string gather_code_text(const pugi::xml_node& node) {
    std::string text;
    for (pugi::xml_node child : node.children()) {
        const std::string_view name = child.name();
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            text += child.value();
        } else if (name == "sp") {
            text.push_back(' ');
        } else if (name == "codeline") {
            text += gather_code_text(child);
            text.push_back('\n');
        } else {
            text += gather_code_text(child);
        }
    }
    return text;
}

[[nodiscard]] std::string flatten_text(const std::vector<content_node>& nodes) {
    std::string text;
    for (const content_node& node : nodes) {
        text += node.text;
        text += flatten_text(node.children);
    }
    return text;
}

std::vector<content_node> convert_children(
    const pugi::xml_node& parent, const std::string& xml_path,
    conversion_context& context);

[[nodiscard]] content_node convert_element(
    const pugi::xml_node& element, const std::string& parent_path,
    conversion_context& context) {
    const std::string name = element.name();
    const std::string xml_path = parent_path + "/" + name;
    content_node node;

    if (const auto punctuation = punctuation_replacement(name)) {
        node.kind = content_node_kind::text;
        node.text = std::string(*punctuation);
        return node;
    }

    if (name == "ref") {
        node.kind = content_node_kind::entity_link;
        node.target = element.attribute("refid").as_string();
        if (const std::string kindref = element.attribute("kindref").as_string();
            !kindref.empty()) {
            node.attributes.emplace("kindref", kindref);
        }
        node.children = convert_children(element, xml_path, context);
        return node;
    }
    if (name == "ulink") {
        node.children = convert_children(element, xml_path, context);
        const std::string url = element.attribute("url").as_string();
        if (is_safe_external_url(url)) {
            node.kind = content_node_kind::link;
            node.target = url;
        } else {
            report(context, "unsafe_link",
                   "rejected non-http(s) Doxygen ulink '" + url + "' at " + xml_path);
            content_node text_node;
            text_node.kind = content_node_kind::text;
            text_node.text = flatten_text(node.children);
            return text_node;
        }
        return node;
    }
    if (name == "image") {
        node.kind = content_node_kind::image;
        node.target = element.attribute("name").as_string();
        node.children = convert_children(element, xml_path, context);
        return node;
    }
    if (name == "verbatim" || name == "programlisting") {
        node.kind = content_node_kind::code_block;
        node.text = gather_code_text(element);
        return node;
    }
    if (name == "linebreak") {
        node.kind = content_node_kind::line_break;
        return node;
    }
    if (name == "simplesect") {
        node.kind = content_node_kind::admonition;
        node.attributes.emplace("kind", element.attribute("kind").as_string());
        node.children = convert_children(element, xml_path, context);
        return node;
    }
    if (name == "parameterlist") {
        node.kind = content_node_kind::admonition;
        node.attributes.emplace(
            "kind", "param:" + std::string(element.attribute("kind").as_string()));
        node.children = convert_children(element, xml_path, context);
        return node;
    }

    // Simple wrapping elements: map the tag, then convert mixed content.
    if (name == "para") {
        node.kind = content_node_kind::paragraph;
    } else if (name == "emphasis") {
        node.kind = content_node_kind::emphasis;
    } else if (name == "bold") {
        node.kind = content_node_kind::strong_text;
    } else if (name == "s") {
        node.kind = content_node_kind::strikethrough;
    } else if (name == "computeroutput") {
        node.kind = content_node_kind::code_span;
    } else if (name == "itemizedlist") {
        node.kind = content_node_kind::unordered_list;
    } else if (name == "orderedlist") {
        node.kind = content_node_kind::ordered_list;
    } else if (name == "listitem" || name == "parameteritem") {
        node.kind = content_node_kind::list_item;
    } else if (name == "table") {
        node.kind = content_node_kind::table;
    } else if (name == "row") {
        node.kind = content_node_kind::table_row;
    } else if (name == "entry") {
        node.kind = content_node_kind::table_cell;
    } else if (name == "parametername") {
        node.kind = content_node_kind::strong_text;
    } else if (name == "parameternamelist" || name == "parameterdescription"
               || name == "briefdescription" || name == "detaileddescription") {
        node.kind = content_node_kind::paragraph;  // transparent container
    } else {
        report(context, "unknown_element",
               "unknown Doxygen XML element <" + name + "> at " + xml_path);
        node.kind = content_node_kind::paragraph;  // passthrough: never drop text
    }
    node.children = convert_children(element, xml_path, context);
    return node;
}

std::vector<content_node> convert_children(
    const pugi::xml_node& parent, const std::string& xml_path,
    conversion_context& context) {
    std::vector<content_node> out;
    for (pugi::xml_node child : parent.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            append_text(out, child.value());
        } else if (child.type() == pugi::node_element) {
            out.push_back(convert_element(child, xml_path, context));
        }
    }
    return out;
}

// Truncates a derived summary at a Unicode-safe sentence/length boundary.
[[nodiscard]] std::string truncate_summary(std::string_view text) {
    static constexpr std::size_t max_bytes = 200;
    if (const std::size_t sentence = text.find(". ");
        sentence != std::string_view::npos && sentence + 1 <= max_bytes) {
        return std::string(text.substr(0, sentence + 1));
    }
    if (text.size() <= max_bytes) {
        return std::string(text);
    }
    std::size_t cut = max_bytes;
    while (cut > 0
           && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
        --cut;  // never split a UTF-8 continuation sequence
    }
    return std::string(text.substr(0, cut)) + "\xE2\x80\xA6";  // U+2026
}

}  // namespace

/// @brief Converts a Doxygen description XML fragment into a typed node tree.
/// @param xml A single <briefdescription>/<detaileddescription> element text.
/// @param owning_entity Qualified entity name used in unknown-tag diagnostics.
/// @return The converted nodes and any element diagnostics.
[[nodiscard]] doxygen_description_result parse_doxygen_content(
    std::string_view xml, std::string_view owning_entity) {
    pugi::xml_document document;
    const pugi::xml_parse_result result =
        document.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw std::runtime_error(std::string("cannot parse Doxygen description: ")
                                 + result.description());
    }
    doxygen_description_result output;
    conversion_context context{std::string(owning_entity), &output.diagnostics};
    const pugi::xml_node root = document.first_child();
    output.nodes = convert_children(root, std::string(root.name()), context);
    return output;
}

/// @brief Extracts the plain-text brief description of a memberdef/compounddef.
/// @param node Doxygen definition node.
/// @return Flattened brief text, or empty when undocumented.
[[nodiscard]] std::string extract_doxygen_brief(const pugi::xml_node& node) {
    const pugi::xml_node brief = node.child("briefdescription");
    if (!brief) {
        return {};
    }
    std::string text;
    for (pugi::xml_node para : brief.children("para")) {
        if (!text.empty()) {
            text.push_back(' ');
        }
        text += para.text().get();
    }
    return trim_ascii_whitespace(text);
}

namespace {

[[nodiscard]] entity_kind doxygen_compound_kind(std::string_view kind) {
    if (kind == "class") { return entity_kind::class_; }
    if (kind == "struct") { return entity_kind::struct_; }
    if (kind == "union") { return entity_kind::union_; }
    if (kind == "concept") { return entity_kind::concept_; }
    return entity_kind::namespace_;
}

[[nodiscard]] entity_kind doxygen_member_kind(std::string_view kind) {
    if (kind == "function") { return entity_kind::function; }
    if (kind == "typedef") { return entity_kind::type_alias; }
    if (kind == "enum") { return entity_kind::enum_; }
    return entity_kind::variable;
}

// Concatenates the full mixed-content text of an element (for example a
// Doxygen <type> holding `const <ref>Sequence</ref> &`).
[[nodiscard]] std::string element_text(const pugi::xml_node& node) {
    std::string text;
    for (pugi::xml_node child : node.children()) {
        if (child.type() == pugi::node_pcdata || child.type() == pugi::node_cdata) {
            text += child.value();
        } else if (child.type() == pugi::node_element) {
            text += element_text(child);
        }
    }
    return trim_ascii_whitespace(text);
}

[[nodiscard]] std::optional<source_span> doxygen_location(const pugi::xml_node& node) {
    const pugi::xml_node location = node.child("location");
    if (!location) {
        return std::nullopt;
    }
    source_span span;
    span.path = location.attribute("file").as_string();
    span.first_line = location.attribute("line").as_ullong();
    span.first_column = location.attribute("column").as_ullong();
    span.last_line = span.first_line;
    span.last_column = span.first_column;
    return span;
}

}  // namespace

/// @brief Derives an entity summary from brief, else the first detailed para.
/// @param node Doxygen definition node.
/// @return The brief text, or a Unicode-safe-truncated first detailed paragraph.
[[nodiscard]] std::string derive_summary_text(const pugi::xml_node& node) {
    if (std::string brief = extract_doxygen_brief(node); !brief.empty()) {
        return brief;
    }
    const pugi::xml_node detailed = node.child("detaileddescription");
    for (pugi::xml_node para : detailed.children("para")) {
        if (const std::string text = trim_ascii_whitespace(para.text().get()); !text.empty()) {
            return truncate_summary(text);
        }
    }
    return {};
}

/// @brief Parses one Doxygen compound XML file into documented entities.
/// @param xml Raw compound XML text (for example a namespace or class file).
/// @return The compound plus its documented members, with typed documentation.
[[nodiscard]] std::vector<reference_entity> parse_doxygen_entities(std::string_view xml) {
    pugi::xml_document document;
    const pugi::xml_parse_result result =
        document.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw std::runtime_error(std::string("cannot parse Doxygen compound XML: ")
                                 + result.description());
    }
    std::vector<reference_entity> entities;
    const pugi::xml_node root = document.child("doxygen");
    for (pugi::xml_node compound : root.children("compounddef")) {
        const std::string compound_kind = compound.attribute("kind").as_string();
        reference_entity entity;
        entity.domain = entity_domain::cpp;
        entity.kind = doxygen_compound_kind(compound_kind);
        entity.qualified_name = compound.child_value("compoundname");
        entity.display_name = entity.qualified_name;
        entity.normalized_signature = entity.qualified_name;
        entity.summary_text = derive_summary_text(compound);
        entity.declaration = doxygen_location(compound);
        {
            conversion_context context{entity.qualified_name, nullptr};
            const pugi::xml_node detailed = compound.child("detaileddescription");
            entity.detail_content =
                convert_children(detailed, "detaileddescription", context);
        }
        entity.attributes.emplace("refid", compound.attribute("id").as_string());
        entity.attributes.emplace("doxygen_kind", compound_kind);
        const std::string compound_name = entity.qualified_name;
        entities.push_back(std::move(entity));
        for (pugi::xml_node section : compound.children("sectiondef")) {
            for (pugi::xml_node member : section.children("memberdef")) {
                const std::string member_kind = member.attribute("kind").as_string();
                reference_entity member_entity;
                member_entity.domain = entity_domain::cpp;
                member_entity.kind = doxygen_member_kind(member_kind);
                member_entity.display_name = member.child_value("name");
                member_entity.qualified_name =
                    compound_name + "::" + member_entity.display_name;
                member_entity.summary_text = derive_summary_text(member);
                member_entity.declaration = doxygen_location(member);
                if (member_kind == "function") {
                    member_entity.return_type = element_text(member.child("type"));
                    std::string signature = member_entity.qualified_name + "(";
                    bool first = true;
                    for (pugi::xml_node parameter : member.children("param")) {
                        parameter_description info;
                        info.type = element_text(parameter.child("type"));
                        info.name = parameter.child_value("declname");
                        if (!first) {
                            signature += ", ";
                        }
                        signature += info.type;
                        first = false;
                        member_entity.parameters.push_back(std::move(info));
                    }
                    signature += ")";
                    member_entity.normalized_signature = std::move(signature);
                } else {
                    member_entity.normalized_signature = member_entity.qualified_name;
                }
                conversion_context context{member_entity.qualified_name, nullptr};
                const pugi::xml_node detailed = member.child("detaileddescription");
                member_entity.detail_content =
                    convert_children(detailed, "detaileddescription", context);
                member_entity.attributes.emplace("refid",
                                                  member.attribute("id").as_string());
                member_entity.attributes.emplace("doxygen_kind", member_kind);
                entities.push_back(std::move(member_entity));
            }
        }
    }
    return entities;
}

}  // namespace bioseq::codegen::website
