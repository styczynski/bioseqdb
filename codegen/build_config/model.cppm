/**
 * @file
 * @brief Stores build settings, allowed values, overrides and generated output.
 */
module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

export module bioseq.codegen.build_config:model;

export namespace bioseq::codegen::build_config {

enum class value_kind : std::uint8_t {
    boolean,
    signed_integer,
    unsigned_integer,
};

enum class domain_kind : std::uint8_t {
    boolean,
    closed_range,
    enumeration,
};

struct value_domain final {
    domain_kind kind{domain_kind::boolean};
    std::int64_t minimum{};
    std::int64_t maximum{};
    std::vector<std::int64_t> values;
    std::string spelling;
};

struct config_entry final {
    std::string name;
    std::string macro_name;
    std::string cpp_type;
    std::string unit;
    std::string default_expression;
    std::int64_t default_value{};
    value_kind kind{value_kind::boolean};
    value_domain domain;
    std::size_t attribute_begin{};
    std::size_t attribute_end{};
    std::size_t initializer_begin{};
    std::size_t initializer_end{};
};

struct config_model final {
    std::filesystem::path source_path;
    std::string source;
    std::string source_sha256;
    std::string module_name;
    std::size_t module_declaration_begin{};
    std::vector<config_entry> entries;
};

struct config_override final {
    std::string macro_name;
    std::string expression;
};

struct resolved_entry final {
    config_entry entry;
    std::string expression;
    std::int64_t value{};
};

struct generation_result final {
    std::string module_source;
    std::string compile_contract_source;
    std::string manifest_json;
    std::vector<resolved_entry> entries;
};

struct command_line final {
    std::filesystem::path source;
    std::filesystem::path output;
    std::filesystem::path compile_contract;
    std::filesystem::path manifest;
    std::filesystem::path stamp;
    std::vector<config_override> overrides;
};

} // namespace bioseq::codegen::build_config
