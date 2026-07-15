module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

module bioseq.codegen.external:model;

import bioseq.codegen.support;

namespace fs = std::filesystem;

namespace bioseq::codegen::external {

enum class parameter_location : std::uint8_t { path, query, header };
enum class parameter_kind : std::uint8_t { string, integer, number, boolean };

struct openapi_parameter {
    std::string name;
    parameter_location location{};
    parameter_kind kind{};
    bool required{};
    bool is_array{};
};

struct openapi_operation {
    std::string operation_id;
    std::string path;
    std::string accept;
    std::string api_key_name;
    std::optional<parameter_location> api_key_location;
    std::vector<openapi_parameter> parameters;
};

enum class integration_mode : std::uint8_t { single, batch };

using integration_scalar = std::variant<std::string, std::int64_t, double, bool>;

struct integration_parameter {
    std::string name;
    parameter_location location{};
    parameter_kind kind{};
    integration_scalar value;
};

struct integration_header_expectation {
    std::string name;
    std::string value_contains;
};

struct integration_case {
    std::string name;
    std::string operation_id;
    std::string accept;
    integration_mode mode{};
    std::size_t count{1};
    std::size_t pre_delay_without_credentials_ms{};
    long expected_status{200};
    std::vector<integration_parameter> parameters;
    std::vector<std::string> body_contains;
    std::vector<integration_header_expectation> header_contains;
};

struct openapi_document {
    std::string title;
    std::string provider_name;
    std::string server_url;
    std::string host;
    std::uint16_t port{443};
    std::string base_path;
    std::vector<openapi_operation> get_operations;
    std::vector<integration_case> integration_cases;
};

struct annotation_request {
    fs::path source;
    fs::path shadow_source;
    std::string annotation_name;
    std::string declared_type;
    std::string argument_expression;
    std::string spec;
};

struct generated_api {
    std::string client_name;
    std::string declared_type;
    openapi_document document;
};

struct generated_output {
    std::string name;
    std::function<void(const fs::path&, std::string_view)> emit;
};

struct annotation_handler {
    std::function<generated_output(const annotation_request&)> generate;
    std::function<std::string(const fs::path&)> output_name;
    std::function<fs::path(std::string_view)> owned_output_path;
    std::function<bool(const fs::path&, std::string_view)> outputs_exist;
    std::function<void(std::ostringstream&, std::string_view)> append_cmake_inventory;
};

using annotation_handlers = std::unordered_map<std::string, annotation_handler>;

struct command_line {
    fs::path generator_binary;
    fs::path source_root;
    fs::path output_root;
    fs::path stamp;
    std::string clang_binary;
    std::string compiler_id;
};

struct source_state {
    std::uintmax_t size{};
    std::int64_t ticks{};
    std::uint64_t hash{};
    bool annotated{};
    std::string annotation_name;
    std::string output_name;
    std::string owned_output_path;
};

}
