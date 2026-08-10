// Jinja2Cpp renderer, compiled at C++20 outside the module graph (see
// template_renderer_shim.h). This is the only translation unit that includes
// Jinja2Cpp headers.

#include "template_renderer_shim.h"

#include <jinja2cpp/template.h>
#include <jinja2cpp/template_env.h>
#include <jinja2cpp/filesystem_handler.h>
#include <jinja2cpp/value.h>
#include <jinja2cpp/binding/boost_json.h>

#include <boost/json.hpp>

#include <exception>
#include <memory>
#include <string>

namespace bioseq::codegen::website::detail {

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
    // context must outlive the render: Jinja reflects it lazily by reference.
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
    jinja2::TemplateEnv env;
    auto filesystem = std::make_shared<jinja2::RealFileSystem>(templates_dir);
    env.AddFilesystemHandler(std::string(), filesystem);
    // Bind the context into the environment's *global* scope, not the per-render
    // external scope: Jinja2Cpp clones a non-scoped {% block %} with
    // RenderContext::Clone(false), which drops the external scope but keeps the
    // global one. Passing values to RenderAsString would leave them invisible to
    // every overridden child block (page/nav/entities), so globals are required.
    for (const auto& member : context.as_object()) {
        env.AddGlobal(std::string(member.key()), jinja2::Reflect(member.value()));
    }
    auto loaded = env.LoadTemplate(template_name);
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

}  // namespace bioseq::codegen::website::detail
