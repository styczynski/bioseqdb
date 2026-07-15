// Jinja2Cpp renderer, compiled at C++20 outside the module graph (see
// template_renderer_shim.h). This is the only translation unit that includes
// Jinja2Cpp headers.

#include "template_renderer_shim.h"

#include <jinja2cpp/template.h>
#include <jinja2cpp/value.h>

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

}  // namespace bioseq::codegen::website::detail
