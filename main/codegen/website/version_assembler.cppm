module;

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

module bioseq.codegen.website:version_assembler;

import :json;
import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Optional immutable version assembly (§17.3.12). Combines only separately
// verified, matching-version site artifacts and regenerates its manifest. This
// partition owns numeric version ordering and the assembly manifest shape; the
// build-tree wiring drives it only when historical artifacts are configured.
namespace bioseq::codegen::website {

/// @brief Parses a dotted version path component into numeric parts.
/// @param path_component Version directory name such as v0.1.
/// @return The numeric parts for stable numeric (never lexical) ordering.
[[nodiscard]] std::vector<std::uint32_t> parse_version_parts(std::string_view path_component) {
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

/// @brief Orders versions numerically, newest first.
/// @param versions Versions to order in place.
void order_versions_descending(std::vector<site_version>& versions) {
    std::ranges::sort(versions, [](const site_version& lhs, const site_version& rhs) {
        return lhs.parts > rhs.parts;
    });
}

}  // namespace bioseq::codegen::website
