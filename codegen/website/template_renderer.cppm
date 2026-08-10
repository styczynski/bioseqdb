module;

#include <boost/json.hpp>
#include <exception>
#include <functional>
#include <jinja2cpp/binding/boost_json.h>
#include <jinja2cpp/filesystem_handler.h>
#include <jinja2cpp/template.h>
#include <jinja2cpp/template_env.h>
#include <jinja2cpp/value.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module bioseq.codegen.website:template_renderer;

import bioseq.codegen.website.model;
import bioseq.codegen.support;

// Jinja2Cpp declarations stay in the global module fragment so their headers do
// not become part of the website module interface. HTML escaping remains the
// templates' responsibility.
namespace bioseq::codegen::website {
namespace detail {

int render_inline_template_impl(const std::string& template_text,
                                const std::vector<std::pair<std::string, std::string>>& bindings,
                                std::string& rendered_output,
                                std::string& diagnostic_output) {
    jinja2::Template template_instance;
    const auto loaded = template_instance.Load(template_text);
    if (!loaded) {
        diagnostic_output = loaded.error().ToString();
        return 1;
    }
    jinja2::ValuesMap values;
    for (const auto& [key, value] : bindings) {
        values.emplace(key, value);
    }
    const auto rendered = template_instance.RenderAsString(values);
    if (!rendered) {
        diagnostic_output = rendered.error().ToString();
        return 2;
    }
    rendered_output = rendered.value();
    return 0;
}

int render_inline_json_template_impl(const std::string& template_text,
                                     const std::string& json_context,
                                     std::string& rendered_output,
                                     std::string& diagnostic_output) {
    jinja2::Template template_instance;
    const auto loaded = template_instance.Load(template_text);
    if (!loaded) {
        diagnostic_output = loaded.error().ToString();
        return 1;
    }
    boost::json::value context;
    try {
        context = boost::json::parse(json_context);
    } catch (const std::exception& error) {
        diagnostic_output = std::string("invalid JSON context: ") + error.what();
        return 3;
    }
    if (!context.is_object()) {
        diagnostic_output = "JSON render context must be an object";
        return 3;
    }
    // The context must outlive rendering because Jinja reflects it lazily.
    jinja2::ValuesMap values;
    for (const auto& member : context.as_object()) {
        values.emplace(std::string(member.key()), jinja2::Reflect(member.value()));
    }
    const auto rendered = template_instance.RenderAsString(values);
    if (!rendered) {
        diagnostic_output = rendered.error().ToString();
        return 2;
    }
    rendered_output = rendered.value();
    return 0;
}

int render_template_file_impl(const std::string& templates_dir,
                              const std::string& template_name,
                              const std::string& json_context,
                              std::string& rendered_output,
                              std::string& diagnostic_output) {
    boost::json::value context;
    try {
        context = boost::json::parse(json_context);
    } catch (const std::exception& error) {
        diagnostic_output = std::string("invalid JSON context: ") + error.what();
        return 3;
    }
    if (!context.is_object()) {
        diagnostic_output = "JSON render context must be an object";
        return 3;
    }
    jinja2::TemplateEnv environment;
    auto filesystem = std::make_shared<jinja2::RealFileSystem>(templates_dir);
    environment.AddFilesystemHandler(std::string(), filesystem);
    // Bind the context into the global scope. Jinja2Cpp clones a non-scoped
    // block without its external scope, but retains environment globals.
    for (const auto& member : context.as_object()) {
        environment.AddGlobal(std::string(member.key()), jinja2::Reflect(member.value()));
    }
    auto loaded = environment.LoadTemplate(template_name);
    if (!loaded) {
        diagnostic_output = loaded.error().ToString();
        return 1;
    }
    const auto rendered = loaded.value().RenderAsString(jinja2::ValuesMap{});
    if (!rendered) {
        diagnostic_output = rendered.error().ToString();
        return 2;
    }
    rendered_output = rendered.value();
    return 0;
}

} // namespace detail

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
        detail::render_inline_template_impl(std::string(template_text), flat, result, error);
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
    const int status = detail::render_template_file_impl(std::string(templates_dir),
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
    const int status = detail::render_inline_json_template_impl(
        std::string(template_text), std::string(json_context), result, error);
    if (status != 0) {
        throw std::runtime_error("template render failed: " + error);
    }
    return result;
}

} // namespace bioseq::codegen::website
