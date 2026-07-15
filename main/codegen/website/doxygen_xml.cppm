module;

#include <pugixml.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:doxygen_xml;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Doxygen XML reader (§11.2). pugixml is the sole XML parser and stays in this
// unit's global module fragment. Doxygen owns documentation-comment parsing;
// this stage projects its compound/member XML onto documented_entity records
// that entity_index later merges with the Clang structural index.
namespace bioseq::codegen::website {

/// @brief One compound reference discovered in a Doxygen index.xml.
struct doxygen_compound_ref {
    std::string refid;
    std::string kind;
    std::string name;
};

/// @brief Parses a Doxygen index.xml into its compound inventory.
/// @param xml Raw index.xml text.
/// @return The listed compounds in document order.
[[nodiscard]] std::vector<doxygen_compound_ref> parse_doxygen_index(std::string_view xml) {
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

/// @brief Extracts the plain-text brief description of a memberdef/compounddef.
/// @param node Doxygen definition node.
/// @return Flattened brief text, or empty when undocumented.
[[nodiscard]] std::string doxygen_brief_text(const pugi::xml_node& node) {
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
    return trim(text);
}

/// @brief Parses one Doxygen compound XML file into documented entities.
/// @param xml Raw compound XML text (for example a namespace or class file).
/// @return The compound plus its documented members.
[[nodiscard]] std::vector<documented_entity> parse_doxygen_compound(std::string_view xml) {
    pugi::xml_document document;
    const pugi::xml_parse_result result =
        document.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw std::runtime_error(std::string("cannot parse Doxygen compound XML: ")
                                 + result.description());
    }
    std::vector<documented_entity> entities;
    const pugi::xml_node root = document.child("doxygen");
    for (pugi::xml_node compound : root.children("compounddef")) {
        documented_entity entity;
        entity.domain = entity_domain::cpp;
        entity.qualified_name = compound.child_value("compoundname");
        entity.display_name = entity.qualified_name;
        entity.summary_text = doxygen_brief_text(compound);
        entity.attributes.emplace("refid", compound.attribute("id").as_string());
        entity.attributes.emplace("doxygen_kind", compound.attribute("kind").as_string());
        entities.push_back(std::move(entity));
        for (pugi::xml_node section : compound.children("sectiondef")) {
            for (pugi::xml_node member : section.children("memberdef")) {
                documented_entity member_entity;
                member_entity.domain = entity_domain::cpp;
                member_entity.display_name = member.child_value("name");
                member_entity.qualified_name =
                    entities.front().qualified_name + "::" + member_entity.display_name;
                member_entity.summary_text = doxygen_brief_text(member);
                member_entity.attributes.emplace("refid",
                                                  member.attribute("id").as_string());
                member_entity.attributes.emplace("doxygen_kind",
                                                  member.attribute("kind").as_string());
                entities.push_back(std::move(member_entity));
            }
        }
    }
    return entities;
}

}  // namespace bioseq::codegen::website
