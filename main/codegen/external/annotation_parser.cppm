/**
 * @file annotation_parser.cppm
 * @brief cppast-backed extraction of registered external-codegen annotations.
 */
module;

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
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
/// @return Exact expression spelling retained by cppast.
/// @throws std::runtime_error When cppast exposes an unsupported expression kind.
[[nodiscard]] std::string extract_expression_source(
    const cppast::cpp_expression& expression) {
    if (expression.kind() == cppast::cpp_expression_kind::literal_t) {
        return static_cast<const cppast::cpp_literal_expression&>(expression).value();
    }
    if (expression.kind() == cppast::cpp_expression_kind::unexposed_t) {
        return static_cast<const cppast::cpp_unexposed_expression&>(expression)
            .expression()
            .as_string();
    }
    throw std::runtime_error("unsupported cppast expression kind");
}

/// @brief Uses cppast/libclang to resolve one registered annotation and its specification.
/// @param source_path Original descriptor path used in diagnostics.
/// @param parser_source_path Normalized parser input path.
/// @param clang_binary Clang binary used by cppast.
/// @param registry Registered annotation processors.
/// @return The single resolved annotation request.
/// @throws std::runtime_error When parsing fails or the annotation contract is ambiguous.
[[nodiscard]] annotation_request parse_source_annotation(
    const fs::path& source_path,
    const fs::path& parser_source_path,
    const std::string& clang_binary,
    const annotation_registry& registry) {
    cppast::cpp_entity_index entity_index;
    cppast::libclang_compile_config compile_config(clang_binary);
    compile_config.set_flags(cppast::cpp_standard::cpp_20);
    compile_config.fast_preprocessing(false);

    cppast::libclang_parser parser;
    auto parsed_file = parser.parse(
        entity_index,
        parser_source_path.string(),
        compile_config);
    if (!parsed_file || parser.error()) {
        throw std::runtime_error(
            "cppast/libclang failed to parse " + source_path.string());
    }

    std::map<std::string, std::string, std::less<>> initializers;

    /// @brief One annotated class discovered during source traversal.
    struct annotation_match {
        /// @brief Registered attribute spelling.
        std::string annotation_name;
        /// @brief Annotated class name.
        std::string declared_type;
        /// @brief Attribute argument expression spelling.
        std::string argument_expression;
    };

    std::vector<annotation_match> annotation_matches;
    cppast::visit(
        *parsed_file,
        [&initializers, &annotation_matches, &registry](
            const cppast::cpp_entity& entity,
            cppast::visitor_info visitor_state) {
            if (!visitor_state.is_new_entity()) {
                return;
            }

            if (entity.kind() == cppast::cpp_entity_kind::variable_t) {
                const auto& variable = static_cast<const cppast::cpp_variable&>(entity);
                if (const auto initializer = variable.default_value()) {
                    initializers.emplace(
                        variable.name(),
                        extract_expression_source(initializer.value()));
                }
            }

            if (entity.kind() != cppast::cpp_entity_kind::class_t) {
                return;
            }
            for (const auto& registry_entry : registry) {
                const std::string& annotation_name = registry_entry.first;
                const auto attribute = cppast::has_attribute(entity, annotation_name);
                if (!attribute) {
                    continue;
                }
                if (!attribute.value().arguments()) {
                    throw std::runtime_error(
                        annotation_name + " requires one argument");
                }
                annotation_matches.push_back(annotation_match{
                    .annotation_name = annotation_name,
                    .declared_type = entity.name(),
                    .argument_expression = trim_ascii_whitespace(
                        attribute.value().arguments().value().as_string()),
                });
            }
        });

    if (annotation_matches.size() != 1) {
        throw std::runtime_error(
            source_path.string()
            + " must contain exactly one registered Bioseq annotation");
    }

    const annotation_match& match = annotation_matches.front();
    const auto initializer = initializers.find(match.argument_expression);
    if (initializer == initializers.end()) {
        throw std::runtime_error(
            "cannot resolve annotation argument '" + match.argument_expression
            + "' in " + source_path.string());
    }

    return annotation_request{
        .source_path = source_path,
        .parser_source_path = parser_source_path,
        .annotation_name = match.annotation_name,
        .declared_type = match.declared_type,
        .argument_expression = match.argument_expression,
        .specification = decode_cpp_raw_string(initializer->second),
    };
}

} // namespace bioseq::codegen::external
