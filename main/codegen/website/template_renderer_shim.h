#pragma once

// Global-module boundary for the Jinja2Cpp renderer. Jinja2Cpp 1.3.2 headers are
// not C++26-clean (nonstd::expected aliases std::expected, and its polymorphic
// value trips a libstdc++ <optional> constraint), so the renderer is compiled in
// template_renderer_impl.cpp at C++20 and reached through this declaration. The
// website module never includes a Jinja2Cpp header.

#include <string>
#include <utility>
#include <vector>

namespace bioseq::codegen::website::detail {

/// @brief Renders a Jinja template against flat string bindings.
/// @param template_text Jinja template source.
/// @param bindings Ordered name/value string bindings.
/// @param rendered_output Receives the rendered output on success.
/// @param diagnostic_output Receives the diagnostic message on failure.
/// @return Zero on success; one on load failure; two on render failure.
int render_inline_template_impl(const std::string& template_text,
                 const std::vector<std::pair<std::string, std::string>>& bindings,
                 std::string& rendered_output,
                 std::string& diagnostic_output);

/// @brief Renders a Jinja template against a JSON object context.
///
/// The JSON context (an object) is reflected directly into Jinja values through
/// Jinja2Cpp's Boost.JSON binding, so structured page data — lists of entities,
/// nested tutorials — drives `{% for %}`/`{{ obj.field }}` templates without
/// leaking any Jinja2Cpp type across the module boundary.
/// @param template_text Jinja template source.
/// @param json_context A JSON object serialized as text.
/// @param rendered_output Receives the rendered output on success.
/// @param diagnostic_output Receives the diagnostic message on failure.
/// @return Zero on success; one on load, two on render, three on context failure.
int render_inline_json_template_impl(const std::string& template_text,
                      const std::string& json_context,
                      std::string& rendered_output,
                      std::string& diagnostic_output);

/// @brief Renders a named template from a templates root against a JSON context.
///
/// Creates one TemplateEnv rooted only at @p templates_dir (§14) so `{% extends
/// %}`/`{% include %}` resolve against the sibling templates; the JSON object is
/// reflected in as the render context. Absolute/parent-escaping include paths
/// are rejected by the loader.
/// @param templates_dir Filesystem root of the template set.
/// @param template_name Logical template name (for example "landing.html.j2").
/// @param json_context A JSON object serialized as text.
/// @param rendered_output Receives the rendered output on success.
/// @param diagnostic_output Receives the diagnostic message on failure.
/// @return Zero on success; one on load, two on render, three on context failure.
int render_template_file_impl(const std::string& templates_dir,
                      const std::string& template_name,
                      const std::string& json_context,
                      std::string& rendered_output,
                      std::string& diagnostic_output);

}  // namespace bioseq::codegen::website::detail
