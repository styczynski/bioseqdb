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

export module bioseq.codegen.website.template_renderer_runtime;

// LLVM's module graph is built without RTTI; Jinja's reflection requires it.
// Keep implementation ownership in this independent named module and link
// the ABI entry points without importing an incompatible RTTI-enabled BMI.
namespace bioseq::codegen::website {
namespace detail {

extern "C" int bioseq_render_inline_template(const std::string& template_text,
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

extern "C" int bioseq_render_inline_json_template(const std::string& template_text,
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

extern "C" int bioseq_render_template_file(const std::string& templates_dir,
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
} // namespace bioseq::codegen::website
