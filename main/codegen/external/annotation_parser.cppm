module;

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <cppast/cpp_attribute.hpp>
#include <cppast/cpp_class.hpp>
#include <cppast/cpp_entity_index.hpp>
#include <cppast/cpp_entity_kind.hpp>
#include <cppast/cpp_expression.hpp>
#include <cppast/cpp_file.hpp>
#include <cppast/cpp_variable.hpp>
#include <cppast/libclang_parser.hpp>
#include <cppast/visitor.hpp>

module bioseq.codegen.external:annotation_parser;

import :model;
import bioseq.codegen.support;

namespace fs = std::filesystem;

namespace bioseq::codegen::external {

/// @brief Extracts source text from a supported cppast expression.
/// @param expression Parsed initializer expression.
/// @return Expression spelling.
[[nodiscard]] std::string expression_text(const cppast::cpp_expression& expression) {
    if (expression.kind() == cppast::cpp_expression_kind::literal_t) {
        return static_cast<const cppast::cpp_literal_expression&>(expression).value();
    }
    if (expression.kind() == cppast::cpp_expression_kind::unexposed_t) {
        return static_cast<const cppast::cpp_unexposed_expression&>(expression)
            .expression().as_string();
    }
    throw std::runtime_error("unsupported cppast expression kind");
}

/// @brief Uses cppast/libclang to resolve one registered annotation and its spec.
/// @param source Original descriptor path.
/// @param shadow Normalized parser input path.
/// @param clang_binary Clang binary used by cppast.
/// @param handlers Registered annotation contracts.
/// @return Resolved annotation request.
[[nodiscard]] annotation_request parse_annotation(
    const fs::path& source,
    const fs::path& shadow,
    const std::string& clang_binary,
    const annotation_handlers& handlers) {
    cppast::cpp_entity_index index;
    cppast::libclang_compile_config config(clang_binary);
    config.set_flags(cppast::cpp_standard::cpp_20);
    config.fast_preprocessing(false);
    cppast::libclang_parser parser;
    auto file = parser.parse(index, shadow.string(), config);
    if (!file || parser.error()) {
        throw std::runtime_error("cppast/libclang failed to parse " + source.string());
    }

    std::unordered_map<std::string, std::string> initializers;
    struct annotated_class {
        std::string annotation_name;
        std::string declared_type;
        std::string argument;
    };
    std::vector<annotated_class> annotated_classes;
    cppast::visit(*file, [&initializers, &annotated_classes, &handlers](
                             const cppast::cpp_entity& entity,
                             cppast::visitor_info info) {
        if (!info.is_new_entity()) {
            return;
        }
        if (entity.kind() == cppast::cpp_entity_kind::variable_t) {
            const auto& variable = static_cast<const cppast::cpp_variable&>(entity);
            if (const auto value = variable.default_value()) {
                initializers.emplace(variable.name(), expression_text(value.value()));
            }
        }
        if (entity.kind() == cppast::cpp_entity_kind::class_t) {
            for (const auto& [annotation_name, handler] : handlers) {
                (void)handler;
                if (const auto attribute = cppast::has_attribute(entity, annotation_name)) {
                    if (!attribute.value().arguments()) {
                        throw std::runtime_error(annotation_name + " requires one argument");
                    }
                    annotated_classes.push_back(annotated_class{
                        .annotation_name = annotation_name,
                        .declared_type = entity.name(),
                        .argument = trim(attribute.value().arguments().value().as_string()),
                    });
                }
            }
        }
    });

    if (annotated_classes.size() != 1) {
        throw std::runtime_error(
            source.string() + " must contain exactly one registered Bioseq annotation");
    }
    const auto& matched = annotated_classes.front();
    const auto initializer = initializers.find(matched.argument);
    if (initializer == initializers.end()) {
        throw std::runtime_error(
            "cannot resolve annotation argument '" + matched.argument + "' in " + source.string());
    }
    return annotation_request{
        .source = source,
        .shadow_source = shadow,
        .annotation_name = matched.annotation_name,
        .declared_type = matched.declared_type,
        .argument_expression = matched.argument,
        .spec = decode_raw_string(initializer->second),
    };
}

}
