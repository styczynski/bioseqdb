module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

module bioseq.codegen.website:release_assembly;

import :serialization;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Parses release numbers and sorts them for the multi-version website.
namespace bioseq::codegen::website {

/// @brief Extracts numeric parts from a release directory name.
/// @param path_component Release directory name, such as v0.1.
/// @return Numbers used to sort releases by version.
[[nodiscard]] std::vector<std::uint32_t> parse_release_components(std::string_view path_component) {
    std::vector<std::uint32_t> parts;
    std::size_t index = 0;
    while (index < path_component.size() && !std::isdigit(
               static_cast<unsigned char>(path_component[index]))) {
        ++index;
    }
    std::uint32_t value = 0;
    bool in_number = false;
    for (; index < path_component.size(); ++index) {
        const char character = path_component[index];
        if (character >= '0' && character <= '9') {
            value = value * 10 + static_cast<std::uint32_t>(character - '0');
            in_number = true;
        } else if (in_number) {
            parts.push_back(value);
            value = 0;
            in_number = false;
        }
    }
    if (in_number) {
        parts.push_back(value);
    }
    return parts;
}

/// @brief Orders releases numerically, newest first.
/// @param releases Releases to order in place.
void sort_releases_descending(std::vector<release_identifier>& releases) {
    std::ranges::sort(releases, [](const release_identifier& lhs,
                                  const release_identifier& rhs) {
        return lhs.parts > rhs.parts;
    });
}

}  // namespace bioseq::codegen::website
