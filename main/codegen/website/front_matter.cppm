module;

#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:front_matter;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Strict tutorial frontmatter parser (§10.1). yaml-cpp is the sole frontmatter
// parser and stays confined to this unit's global module fragment. Unknown keys
// and missing required keys are fatal so authoring errors surface before a
// cluster ever starts.
namespace bioseq::codegen::website {

struct front_matter_split {
    std::string yaml;
    std::string body;
    std::size_t body_first_line{};
};

/// @brief Splits a leading `---` fenced YAML block from the Markdown body.
/// @param source Whole tutorial file text.
/// @return The YAML text, the remaining body, and the body's first line number.
[[nodiscard]] front_matter_split split_front_matter(std::string_view source) {
    if (!source.starts_with("---")) {
        throw std::runtime_error("tutorial is missing its --- frontmatter block");
    }
    const std::size_t first_newline = source.find('\n');
    if (first_newline == std::string_view::npos) {
        throw std::runtime_error("tutorial frontmatter block is not terminated");
    }
    const std::size_t closing = source.find("\n---", first_newline);
    if (closing == std::string_view::npos) {
        throw std::runtime_error("tutorial frontmatter block is not terminated by ---");
    }
    const std::size_t body_begin = source.find('\n', closing + 1);
    front_matter_split split;
    split.yaml = std::string(source.substr(first_newline + 1, closing - first_newline - 1));
    if (body_begin != std::string_view::npos) {
        split.body = std::string(source.substr(body_begin + 1));
    }
    std::size_t line = 1;
    for (std::size_t index = 0;
         index < (body_begin == std::string_view::npos ? source.size() : body_begin);
         ++index) {
        if (source[index] == '\n') {
            ++line;
        }
    }
    split.body_first_line = line + 1;
    return split;
}

/// @brief Parses and validates a tutorial's strict frontmatter.
/// @param yaml Raw YAML frontmatter text.
/// @param path Source path used for diagnostics.
/// @return The validated metadata; throws on any unknown or missing key.
[[nodiscard]] tutorial_metadata parse_front_matter(
    std::string_view yaml,
    std::string_view path) {
    YAML::Node root = YAML::Load(std::string(yaml));
    if (!root.IsMap()) {
        throw std::runtime_error("tutorial frontmatter must be a YAML mapping: "
                                 + std::string(path));
    }
    tutorial_metadata metadata;
    metadata.source.path = path;
    for (const auto& entry : root) {
        const auto key = entry.first.as<std::string>();
        if (key == "title") {
            metadata.title = entry.second.as<std::string>();
        } else if (key == "slug") {
            metadata.slug = entry.second.as<std::string>();
        } else if (key == "description") {
            metadata.description = entry.second.as<std::string>();
        } else if (key == "order") {
            metadata.order = entry.second.as<std::uint32_t>();
        } else if (key == "keywords") {
            for (const auto& keyword : entry.second) {
                metadata.keywords.push_back(keyword.as<std::string>());
            }
        } else if (key == "social_image") {
            metadata.social_image = entry.second.as<std::string>();
        } else if (key == "draft") {
            metadata.draft = entry.second.as<bool>();
        } else {
            throw std::runtime_error("unknown tutorial frontmatter key '" + key
                                     + "' in " + std::string(path));
        }
    }
    if (metadata.title.empty() || metadata.slug.empty() || metadata.description.empty()) {
        throw std::runtime_error("tutorial frontmatter requires title, slug, and "
                                 "description: " + std::string(path));
    }
    return metadata;
}

}  // namespace bioseq::codegen::website
