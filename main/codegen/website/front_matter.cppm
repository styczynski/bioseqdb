module;

#include <yaml-cpp/yaml.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:front_matter;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Strict tutorial frontmatter and site configuration parsing (§9, §10.1).
// yaml-cpp is the sole frontmatter/config parser and stays confined to this
// unit. Unknown/missing keys are fatal so authoring errors surface before any
// cluster starts.
namespace bioseq::codegen::website {

struct front_matter_split {
    std::string yaml;
    std::string body;
    std::size_t body_first_line{};
};

namespace {

[[nodiscard]] bool is_lower_snake(std::string_view value) {
    if (value.empty() || (value.front() >= '0' && value.front() <= '9')) {
        return false;
    }
    for (const char character : value) {
        const bool ok = (character >= 'a' && character <= 'z')
            || (character >= '0' && character <= '9') || character == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string require_scalar(
    const YAML::Node& node, std::string_view key, std::string_view path) {
    if (!node || !node.IsScalar()) {
        throw std::runtime_error("site config key '" + std::string(key)
                                 + "' must be a scalar: " + std::string(path));
    }
    return node.as<std::string>();
}

}  // namespace

/// @brief Splits a leading `---` fenced YAML block from the Markdown body.
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
    const std::size_t body_limit =
        body_begin == std::string_view::npos ? source.size() : body_begin;
    for (std::size_t index = 0; index < body_limit; ++index) {
        if (source[index] == '\n') {
            ++line;
        }
    }
    split.body_first_line = line + 1;
    return split;
}

/// @brief Parses and validates strict tutorial frontmatter (§10.1).
/// @param yaml Raw YAML frontmatter text.
/// @param path Source path for diagnostics.
/// @param default_slug Snake-case filename stem used when slug is omitted.
/// @return The validated metadata; throws on any unknown or missing key.
[[nodiscard]] tutorial_metadata parse_front_matter(
    std::string_view yaml,
    std::string_view path,
    std::string_view default_slug) {
    YAML::Node root = YAML::Load(std::string(yaml));
    if (!root.IsMap()) {
        throw std::runtime_error("tutorial frontmatter must be a YAML mapping: "
                                 + std::string(path));
    }
    tutorial_metadata metadata;
    metadata.source.path = path;
    metadata.slug = std::string(default_slug);
    bool has_title = false, has_description = false, has_order = false, has_keywords = false;
    for (const auto& entry : root) {
        const auto key = entry.first.as<std::string>();
        if (key == "title") {
            metadata.title = entry.second.as<std::string>();
            has_title = true;
        } else if (key == "slug") {
            metadata.slug = entry.second.as<std::string>();
            if (!is_lower_snake(metadata.slug)) {
                throw std::runtime_error("tutorial slug must be lower_snake_case: "
                                         + std::string(path));
            }
        } else if (key == "description") {
            metadata.description = entry.second.as<std::string>();
            has_description = true;
        } else if (key == "order") {
            metadata.order = entry.second.as<std::uint32_t>();
            has_order = true;
        } else if (key == "keywords") {
            if (!entry.second.IsSequence()) {
                throw std::runtime_error("tutorial keywords must be a sequence: "
                                         + std::string(path));
            }
            for (const auto& keyword : entry.second) {
                if (!keyword.IsScalar()) {
                    throw std::runtime_error("tutorial keyword must be scalar: "
                                             + std::string(path));
                }
                metadata.keywords.push_back(keyword.as<std::string>());
            }
            has_keywords = true;
        } else if (key == "social_image") {
            metadata.social_image = entry.second.as<std::string>();
        } else if (key == "draft") {
            metadata.draft = entry.second.as<bool>();
        } else {
            throw std::runtime_error("unknown tutorial frontmatter key '" + key
                                     + "' in " + std::string(path));
        }
    }
    if (!has_title || !has_description || !has_order || !has_keywords) {
        throw std::runtime_error("tutorial frontmatter requires title, description, "
                                 "order, and keywords: " + std::string(path));
    }
    if (trim(metadata.description).empty()) {
        throw std::runtime_error("tutorial description must not be empty: "
                                 + std::string(path));
    }
    return metadata;
}

/// @brief Validated site configuration (§9 site.yaml closed schema).
struct site_configuration {
    std::string title;
    std::string short_description;
    std::string canonical_origin;
    std::string default_base_path;
    std::string repository_url;
    std::string source_url_template;
    std::string default_social_image;
    std::optional<std::string> status_label;
    std::string copyright_holder;
    std::uint32_t copyright_year{};
    std::map<std::string, std::string, std::less<>> navigation;
    std::vector<std::pair<std::string, std::string>> cpp_public_modules;
    std::vector<std::string> required_documentation_entities;
};

namespace {

void validate_base_path(std::string_view value, std::string_view path) {
    if (value == "/") {
        return;
    }
    if (value.size() < 2 || value.front() != '/' || value.back() != '/') {
        throw std::runtime_error("default_base_path must start and end with '/': "
                                 + std::string(path));
    }
    if (value.find("//") != std::string_view::npos
        || value.find("..") != std::string_view::npos
        || value.find('?') != std::string_view::npos
        || value.find('#') != std::string_view::npos) {
        throw std::runtime_error("default_base_path has an illegal segment: "
                                 + std::string(path));
    }
}

[[nodiscard]] std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t total = 0;
    std::size_t position = text.find(needle);
    while (position != std::string_view::npos) {
        ++total;
        position = text.find(needle, position + needle.size());
    }
    return total;
}

}  // namespace

/// @brief Parses and validates main/website/site.yaml (§9).
/// @param yaml Raw site.yaml text.
/// @param path Source path for diagnostics.
/// @return The validated, closed-schema configuration.
[[nodiscard]] site_configuration parse_site_config(
    std::string_view yaml, std::string_view path) {
    YAML::Node root = YAML::Load(std::string(yaml));
    if (!root.IsMap()) {
        throw std::runtime_error("site.yaml must be a YAML mapping: " + std::string(path));
    }
    site_configuration config;
    static constexpr std::string_view known_keys[] = {
        "title", "short_description", "canonical_origin", "default_base_path",
        "repository_url", "source_url_template", "default_social_image",
        "status_label", "copyright_holder", "copyright_year", "navigation",
        "cpp_public_modules", "required_documentation_entities"};
    for (const auto& entry : root) {
        const auto key = entry.first.as<std::string>();
        bool known = false;
        for (const std::string_view candidate : known_keys) {
            if (key == candidate) {
                known = true;
                break;
            }
        }
        if (!known) {
            throw std::runtime_error("unknown site.yaml key '" + key + "' in "
                                     + std::string(path));
        }
    }
    config.title = require_scalar(root["title"], "title", path);
    config.short_description =
        require_scalar(root["short_description"], "short_description", path);
    config.canonical_origin =
        require_scalar(root["canonical_origin"], "canonical_origin", path);
    if (!config.canonical_origin.starts_with("https://")
        || config.canonical_origin.back() == '/'
        || config.canonical_origin.find('?') != std::string::npos
        || config.canonical_origin.find('#') != std::string::npos) {
        throw std::runtime_error("canonical_origin must be an absolute HTTPS origin "
                                 "with no path/query/fragment/trailing slash");
    }
    config.default_base_path =
        require_scalar(root["default_base_path"], "default_base_path", path);
    validate_base_path(config.default_base_path, path);
    config.repository_url = require_scalar(root["repository_url"], "repository_url", path);
    if (!config.repository_url.starts_with("https://")) {
        throw std::runtime_error("repository_url must be HTTPS");
    }
    config.source_url_template =
        require_scalar(root["source_url_template"], "source_url_template", path);
    if (!config.source_url_template.starts_with("https://")
        || count_occurrences(config.source_url_template, "{revision}") != 1
        || count_occurrences(config.source_url_template, "{path}") != 1
        || count_occurrences(config.source_url_template, "{line}") != 1) {
        throw std::runtime_error("source_url_template must be HTTPS and contain "
                                 "{revision}, {path}, and {line} exactly once");
    }
    config.default_social_image =
        require_scalar(root["default_social_image"], "default_social_image", path);
    if (root["status_label"]) {
        config.status_label = require_scalar(root["status_label"], "status_label", path);
    }
    config.copyright_holder =
        require_scalar(root["copyright_holder"], "copyright_holder", path);
    config.copyright_year = root["copyright_year"].as<std::uint32_t>();
    if (config.copyright_year < 2000 || config.copyright_year > 2100) {
        throw std::runtime_error("copyright_year must be an integer 2000-2100");
    }
    const YAML::Node navigation = root["navigation"];
    if (!navigation || !navigation.IsMap() || navigation.size() != 3) {
        throw std::runtime_error("navigation must have exactly documentation, "
                                 "tutorials, and reference");
    }
    for (const std::string_view key : {"documentation", "tutorials", "reference"}) {
        config.navigation.emplace(
            std::string(key), require_scalar(navigation[std::string(key)], key, path));
    }
    const YAML::Node modules = root["cpp_public_modules"];
    if (!modules || !modules.IsSequence() || modules.size() == 0) {
        throw std::runtime_error("cpp_public_modules must be a nonempty sequence");
    }
    for (const auto& item : modules) {
        config.cpp_public_modules.emplace_back(
            require_scalar(item["module"], "module", path),
            require_scalar(item["primary_interface"], "primary_interface", path));
    }
    const YAML::Node required = root["required_documentation_entities"];
    if (required) {
        for (const auto& item : required) {
            config.required_documentation_entities.push_back(item.as<std::string>());
        }
    }
    return config;
}

}  // namespace bioseq::codegen::website
