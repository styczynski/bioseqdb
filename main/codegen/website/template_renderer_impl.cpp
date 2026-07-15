// Jinja2Cpp renderer, compiled at C++20 outside the module graph (see
// template_renderer_shim.h). This is the only translation unit that includes
// Jinja2Cpp headers.

#include "template_renderer_shim.h"

#include <jinja2cpp/template.h>
#include <jinja2cpp/value.h>
#include <jinja2cpp/binding/boost_json.h>

#include <boost/json.hpp>

#include <exception>
#include <string>

namespace bioseq::codegen::website::detail {

int render_jinja(const std::string& template_text,
                 const std::vector<std::pair<std::string, std::string>>& bindings,
                 std::string& out_result,
                 std::string& out_error) {
    jinja2::Template document;
    const auto loaded = document.Load(template_text);
    if (!loaded) {
        out_error = loaded.error().ToString();
        return 1;
    }
    jinja2::ValuesMap values;
    for (const auto& [key, value] : bindings) {
        values.emplace(key, value);
    }
    const auto rendered = document.RenderAsString(values);
    if (!rendered) {
        out_error = rendered.error().ToString();
        return 2;
    }
    out_result = rendered.value();
    return 0;
}

int render_jinja_json(const std::string& template_text,
                      const std::string& json_context,
                      std::string& out_result,
                      std::string& out_error) {
    jinja2::Template document;
    const auto loaded = document.Load(template_text);
    if (!loaded) {
        out_error = loaded.error().ToString();
        return 1;
    }
    boost::json::value context;
    try {
        context = boost::json::parse(json_context);
    } catch (const std::exception& error) {
        out_error = std::string("invalid JSON context: ") + error.what();
        return 3;
    }
    if (!context.is_object()) {
        out_error = "JSON render context must be an object";
        return 3;
    }
    // context must outlive the render: Jinja reflects it lazily by reference.
    jinja2::ValuesMap values;
    for (const auto& member : context.as_object()) {
        values.emplace(std::string(member.key()), jinja2::Reflect(member.value()));
    }
    const auto rendered = document.RenderAsString(values);
    if (!rendered) {
        out_error = rendered.error().ToString();
        return 2;
    }
    out_result = rendered.value();
    return 0;
}

}  // namespace bioseq::codegen::website::detail
