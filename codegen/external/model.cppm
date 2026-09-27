/**
 * @file model.cppm
 * @brief Data used to read API descriptions and generate HTTP clients.
 */
module;

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
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

/// @brief Supported locations for parameters in generated HTTP requests.
enum class parameter_location : std::uint8_t {
    path,   ///< Value substituted into a request path such as /records/{id}.
    query,  ///< Percent-encoded query-string parameter.
    header, ///< HTTP request-header field.
};

/// @brief Scalar types accepted in OpenAPI parameters.
enum class parameter_kind : std::uint8_t {
    string,  ///< Text value.
    integer, ///< Signed integer value.
    number,  ///< Floating-point value.
    boolean, ///< Boolean value.
};

/// @brief An OpenAPI parameter and the rules for passing it to the client.
struct openapi_parameter {
    /// @brief Exact parameter name from the OpenAPI document.
    std::string name;
    /// @brief HTTP request component that carries the parameter.
    parameter_location location{};
    /// @brief Type of each supplied value.
    parameter_kind kind{};
    /// @brief Whether callers must supply the parameter.
    bool required{};
    /// @brief Whether callers can supply several values for this parameter.
    bool is_array{};
};

/// @brief One HTTP GET operation from the API description.
struct openapi_operation {
    /// @brief Name used to select this operation in the generated client.
    std::string operation_id;
    /// @brief Request path, with placeholders for path parameters.
    std::string path;
    /// @brief Preferred response media type.
    std::string accept;
    /// API-key field name, empty when this operation has no API-key requirement.
    std::string api_key_name;
    /// @brief API-key location when the operation requires authentication.
    std::optional<parameter_location> api_key_location;
    /// @brief Parameters in the order collected from the path and operation.
    std::vector<openapi_parameter> parameters;
};

/// @brief How an integration test calls the client.
enum class integration_mode : std::uint8_t {
    single, ///< Invoke the client's single-request API.
    batch,  ///< Invoke the client's batch API.
};

/// @brief A test parameter value stored as text, an integer, a number or a boolean.
using integration_scalar = std::variant<std::string, std::int64_t, double, bool>;

/// @brief A parameter value used by an integration test.
struct integration_parameter {
    /// @brief Parameter name matched against an operation descriptor.
    std::string name;
    /// @brief HTTP request component that carries the parameter.
    parameter_location location{};
    /// @brief Type expected by the operation.
    parameter_kind kind{};
    /// @brief Value written into the generated test.
    integration_scalar value;
};

/// @brief A header value that an integration test expects in the response.
struct integration_header_expectation {
    /// @brief Case-insensitive HTTP header name.
    std::string name;
    /// @brief Required substring in the header value.
    std::string value_contains;
};

/// @brief An integration test case for a generated client.
struct integration_case {
    /// @brief Human-readable test case name.
    std::string name;
    /// @brief Operation invoked by the case.
    std::string operation_id;
    /// @brief Optional Accept header for this test.
    std::string accept;
    /// @brief Whether the case invokes one request or a request batch.
    integration_mode mode{};
    /// @brief Number of requests in a batch invocation.
    std::size_t count{1};
    /// @brief Delay before an unauthenticated attempt, in milliseconds.
    std::size_t pre_delay_without_credentials_ms{};
    /// @brief Expected HTTP response status.
    long expected_status{200};
    /// @brief Parameter values passed to the operation.
    std::vector<integration_parameter> parameters;
    /// @brief Required response-body substrings.
    std::vector<std::string> body_contains;
    /// @brief Header names and value substrings expected in the response.
    std::vector<integration_header_expectation> header_contains;
};

/// @brief The parts of an OpenAPI document needed to generate a client and its tests.
struct openapi_document {
    /// @brief OpenAPI document title.
    std::string title;
    /// @brief Provider name exposed by the generated client.
    std::string provider_name;
    /// @brief Exact HTTPS server URL from the document.
    std::string server_url;
    /// @brief Validated DNS host used by the transport.
    std::string host;
    /// @brief Validated HTTPS port.
    std::uint16_t port{443};
    /// @brief Server path prefix without trailing slashes.
    std::string base_path;
    /// @brief GET operations sorted by operation identifier.
    std::vector<openapi_operation> get_operations;
    /// @brief Validated integration cases in declaration order.
    std::vector<integration_case> integration_cases;
};

/// @brief A module source that may contain a client annotation or specification fragment.
struct descriptor_source {
    /// @brief Source path used in diagnostics.
    fs::path path;
    /// @brief Source path relative to the directory being scanned.
    fs::path relative_path;
    /// @brief Contents of the source file.
    std::string text;
    /// @brief Source file size in bytes.
    std::uintmax_t file_size_bytes{};
    /// @brief Last-write time in filesystem clock ticks.
    std::int64_t last_write_ticks{};
    /// @brief FNV-1a hash of the source bytes.
    std::uint64_t content_hash{};
};

/// @brief One registered source annotation resolved by cppast.
struct annotation_request {
    /// @brief Original annotated module path.
    fs::path source_path;
    /// @brief Path to the copy prepared for cppast.
    fs::path parser_source_path;
    /// @brief Registered annotation spelling.
    std::string annotation_name;
    /// @brief Annotated C++ type name.
    std::string declared_type;
    /// @brief Attribute argument expression spelling.
    std::string argument_expression;
    /// @brief OpenAPI text read from the annotation or its fragments.
    std::string specification;
    /// @brief Imported files that supply specification fragments, in first-use order.
    std::vector<fs::path> specification_dependencies;
};

/// @brief Validated API description and names for one generated client.
struct generated_api {
    /// @brief Client name taken from the descriptor filename.
    std::string client_name;
    /// @brief Public generated client type name.
    std::string declared_type;
    /// @brief Parsed operations, server settings and integration tests.
    openapi_document document;
};

/// @brief An output name and a callback that writes its generated files.
struct generated_artifact {
    /// @brief Name used to detect duplicate outputs and sort generated files.
    std::string output_name;
    /// @brief Writes the generated files under the supplied output directory.
    std::function<void(const fs::path&, std::string_view)> emit_artifacts;
};

/// @brief Callbacks for processing one kind of source annotation.
struct annotation_processor {
    /// @brief Checks the annotation and prepares a callback to write its output.
    std::function<generated_artifact(const annotation_request&)> prepare_artifact;
    /// @brief Finds the output name from the source path.
    std::function<std::string(const fs::path&)> derive_output_name;
    /// @brief Returns the output directory relative to the generator root.
    std::function<fs::path(std::string_view)> derive_owned_output_path;
    /// @brief Checks that all expected output files exist.
    std::function<bool(const fs::path&, std::string_view)> has_complete_output;
    /// @brief Adds the generated output to the CMake build list.
    std::function<void(std::ostringstream&, std::string_view)> append_build_inventory;
};

/// @brief Annotation handlers sorted by attribute name.
using annotation_registry = std::map<std::string, annotation_processor, std::less<>>;

/// @brief Command-line settings for one generator run.
struct generation_options {
    /// @brief Running generator binary used to invalidate incremental state.
    fs::path generator_binary;
    /// @brief Root recursively scanned for annotated module sources.
    fs::path source_root;
    /// @brief Directory where the generator writes its files.
    fs::path output_root;
    /// @brief Stamp updated after all outputs are written successfully.
    fs::path stamp;
    /// @brief Clang executable used by cppast.
    std::string clang_binary;
    /// @brief Compiler name recorded in the generated provenance file.
    std::string compiler_id;
};

/// @brief Saved source information used to decide whether generation can be skipped.
struct source_snapshot {
    /// @brief Combined size of the source and any imported specification fragments.
    std::uintmax_t file_size_bytes{};
    /// @brief Latest write time among the source and its specification fragments.
    std::int64_t last_write_ticks{};
    /// @brief FNV-1a hash of the source bytes or of its specification dependency records.
    std::uint64_t content_hash{};
    /// @brief Whether the source contains a registered annotation.
    bool has_annotation{};
    /// @brief Registered annotation spelling when present.
    std::string annotation_name;
    /// @brief Output name derived from the annotation, when present.
    std::string output_name;
    /// @brief Output directory relative to the generator root, when present.
    std::string owned_output_path;
};

/// @brief Saved source information indexed by path relative to the scan root.
using source_snapshots = std::unordered_map<std::string, source_snapshot>;

} // namespace bioseq::codegen::external
