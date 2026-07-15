module;

#include "template_renderer_shim.h"

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:template_renderer;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Jinja2Cpp rendering (§14). Templates are rendered through the C++20 shim
// declared in template_renderer_shim.h so no Jinja2Cpp header enters the C++26
// module graph. Autoescape and HTML escaping happen inside the shim/templates.
namespace bioseq::codegen::website {

/// @brief Renders a template against a flat string binding map.
/// @param template_text Jinja template source.
/// @param bindings String-valued bindings.
/// @return The rendered document, throwing on any template/render error.
[[nodiscard]] std::string render_template(
    std::string_view template_text,
    const std::map<std::string, std::string, std::less<>>& bindings) {
    std::vector<std::pair<std::string, std::string>> flat;
    flat.reserve(bindings.size());
    for (const auto& [key, value] : bindings) {
        flat.emplace_back(key, value);
    }
    std::string result;
    std::string error;
    const int status =
        detail::render_jinja(std::string(template_text), flat, result, error);
    if (status != 0) {
        throw std::runtime_error("template render failed: " + error);
    }
    return result;
}

}  // namespace bioseq::codegen::website
