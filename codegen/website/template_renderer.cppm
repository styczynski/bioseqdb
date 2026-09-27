module;

#include <functional>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Narrow link boundary to the RTTI-enabled renderer module. Its BMI cannot
// be imported into LLVM's no-RTTI graph; both objects use the same host STL.
// No repository-owned production header or textual implementation is needed.
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


/// @brief Renders a template against a flat string binding map.
/// @param template_text Jinja template source.
/// @param bindings String-valued bindings.
/// @return The rendered output, throwing on any template/render error.
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

/// @brief Renders a named template from a templates root against a JSON context.
/// @param templates_dir Filesystem root of the template set.
/// @param template_name Logical template name (for example "landing.html.j2").
/// @param json_context A JSON object serialized as text (page data).
/// @return The rendered output; throws on any load/context/render error.
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

/// @brief Renders a template against a structured JSON object context.
/// @param template_text Jinja template source.
/// @param json_context A JSON object serialized as text (page data).
/// @return The rendered output, throwing on any template/context/render error.
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
