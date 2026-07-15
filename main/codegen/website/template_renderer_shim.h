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
/// @param out_result Receives the rendered document on success.
/// @param out_error Receives the diagnostic message on failure.
/// @return Zero on success; one on load failure; two on render failure.
int render_jinja(const std::string& template_text,
                 const std::vector<std::pair<std::string, std::string>>& bindings,
                 std::string& out_result,
                 std::string& out_error);

}  // namespace bioseq::codegen::website::detail
