module;

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Call the Jinja renderer through linked functions because its module
// enables RTTI and cannot be imported into  an LLVM build without RTTI.
// Both sides use the same host standard library.
namespace bioseq::codegen::website::detail {
extern "C" int bioseq_render_inline_template(const std::string&,
    const std::vector<std::pair<std::string, std::string>>&, std::string&, std::string&);
extern "C" int bioseq_render_inline_json_template(const std::string&,
    const std::string&, std::string&, std::string&);
extern "C" int bioseq_render_template_file(const std::string&,
    const std::string&, const std::string&, std::string&, std::string&);
}

module bioseq.codegen.website:template_renderer;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

namespace bioseq::codegen::website {


/// @brief Renders Jinja text using string-valued variables.
/// @param template_text Jinja template source.
/// @param bindings Variable names and values.
/// @return Rendered text. Throws if loading or renderxng fails.
[[nodiscard]] std::string
render_inline_template(std::string_view template_text,
                       const std::map<std::string, std::string, std::less<>>& bindings) {
    std::vector<std::pair<std::string, std::string>> flat;
    flat.reserve(bindings.size());
    for (const auto& [key, value] : bindings) {
        flat.emplace_back(key, value);
    }
    std::string result;
    std::string error;
    const int status =
        detail::bioseq_render_inline_template(std::string(template_text), flat, result, error);
    if (status != 0) {
        throw std::runtime_error("template render failed: " + error);
    }
    return result;
}

/// @brief Renders a template file using JSON page data.
/// @param templates_dir Directory containing the templates.
/// @param template_name Template name, such as "landing.html.j2".
/// @param json_context Page data as a JSON object string
/// @return Rendered text. Throws if loading, JSON parsing, or rendering fails.
[[nodiscard]] std::string render_page_template(std::string_view templates_dir,
                                               std::string_view template_name,
                                               std::string_view json_context) {
    std::string result;
    std::string error;
    const int status = detail::bioseq_render_template_file(std::string(templates_dir),
                                                         std::string(template_name),
                                                         std::string(json_context),
                                                         result,
                                                         error);
    if (status != 0) {
        throw std::runtime_error("page render failed: " + error);
    }
    return result;
}

/// @brief Renders Jinja text using a JSON object as its variables.
/// @param template_text Jinja template source.
/// @param json_context Page data as a JSON object  string.
/// @return Rendered text. Throws if the template or context is invalid.
[[nodiscard]] std::string render_inline_json_template(std::string_view template_text,
                                                      std::string_view json_context) {
    std::string result;
    std::string error;
    const int status = detail::bioseq_render_inline_json_template(
        std::string(template_text), std::string(json_context), result, error);
    if (status != 0) {
        throw std::runtime_error("template render failed: " + error);
    }
    return result;
}

} // namespace bioseq::codegen::website
