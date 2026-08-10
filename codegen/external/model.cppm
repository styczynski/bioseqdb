/**
 * @file model.cppm
 * @brief Shared semantic model for external-client generation.
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
    path, ///< Substitution within the origin-form path template.
    query, ///< Percent-encoded query-string parameter.
    header, ///< HTTP request-header field.
};

/// @brief Supported scalar value categories in an OpenAPI parameter schema.
enum class parameter_kind : std::uint8_t {
    string, ///< Text value.
    integer, ///< Signed integral value.
    number, ///< Floating-point value.
    boolean, ///< Boolean value.
};

/// @brief Normalized OpenAPI parameter metadata used by the client renderer.
struct openapi_parameter {
    /// @brief Exact parameter name from the OpenAPI document.
    std::string name;
    /// @brief HTTP request component that carries the parameter.
    parameter_location location{};
    /// @brief Scalar kind accepted by the generated client.
    parameter_kind kind{};
    /// @brief Whether callers must supply the parameter.
    bool required{};
    /// @brief Whether one request value represents a comma-separated array.
    bool is_array{};
};

/// @brief Normalized metadata for one generated HTTP GET operation.
struct openapi_operation {
    /// @brief Stable OpenAPI operation identifier exposed by the client.
    std::string operation_id;
    /// @brief Origin-form path template used by the HTTPS transport.
    std::string path;
    /// @brief Preferred response media type.
    std::string accept;
    /// @brief API-key parameter name, empty when authentication is not required.
    std::string api_key_name;
    /// @brief API-key location when the operation requires authentication.
    std::optional<parameter_location> api_key_location;
    /// @brief Parameters in deterministic renderer order.
    std::vector<openapi_parameter> parameters;
};

/// @brief Invocation shape exercised by a generated integration case.
enum class integration_mode : std::uint8_t {
    single, ///< Invoke the client's single-request API.
    batch, ///< Invoke the client's batch API.
};

/// @brief Owning scalar value parsed from an integration-case declaration.
using integration_scalar = std::variant<std::string, std::int64_t, double, bool>;

/// @brief Concrete parameter supplied by a generated integration case.
struct integration_parameter {
    /// @brief Parameter name matched against an operation descriptor.
    std::string name;
    /// @brief HTTP request component that carries the parameter.
    parameter_location location{};
    /// @brief Scalar kind enforced by the generated client.
    parameter_kind kind{};
    /// @brief Owning fixture value rendered into generated C++.
    integration_scalar value;
};

/// @brief Expected response-header fragment for an integration case.
struct integration_header_expectation {
    /// @brief Case-insensitive HTTP header name.
    std::string name;
    /// @brief Required substring in the header value.
    std::string value_contains;
};

/// @brief Fully validated integration case emitted beside a generated client.
struct integration_case {
    /// @brief Human-readable test case name.
    std::string name;
    /// @brief Operation invoked by the case.
    std::string operation_id;
    /// @brief Optional response-media override.
    std::string accept;
    /// @brief Whether the case invokes one request or a request batch.
    integration_mode mode{};
    /// @brief Number of requests in a batch invocation.
    std::size_t count{1};
    /// @brief Delay before an unauthenticated attempt, in milliseconds.
    std::size_t pre_delay_without_credentials_ms{};
    /// @brief Expected HTTP response status.
    long expected_status{200};
    /// @brief Concrete operation parameters.
    std::vector<integration_parameter> parameters;
    /// @brief Required response-body substrings.
    std::vector<std::string> body_contains;
    /// @brief Required response-header fragments.
    std::vector<integration_header_expectation> header_contains;
};

/// @brief Renderer-ready projection of one OpenAPI document.
struct openapi_document {
    /// @brief OpenAPI document title.
    std::string title;
    /// @brief Provider identity exposed by the generated client.
    std::string provider_name;
    /// @brief Exact HTTPS server URL from the document.
    std::string server_url;
    /// @brief Validated DNS host used by the transport.
    std::string host;
    /// @brief Validated HTTPS port.
    std::uint16_t port{443};
    /// @brief Normalized server path prefix.
    std::string base_path;
    /// @brief Supported GET operations in deterministic identifier order.
    std::vector<openapi_operation> get_operations;
    /// @brief Validated integration cases in declaration order.
    std::vector<integration_case> integration_cases;
};

/// @brief One registered source annotation resolved by cppast.
struct annotation_request {
    /// @brief Original annotated module path.
    fs::path source_path;
    /// @brief Normalized source path parsed by cppast.
    fs::path parser_source_path;
    /// @brief Registered annotation spelling.
    std::string annotation_name;
    /// @brief Annotated C++ type name.
    std::string declared_type;
    /// @brief Attribute argument expression spelling.
    std::string argument_expression;
    /// @brief Decoded embedded specification.
    std::string specification;
};

/// @brief Complete semantic input for rendering one external API client.
struct generated_api {
    /// @brief Output identity derived from the descriptor filename.
    std::string client_name;
    /// @brief Public generated client type name.
    std::string declared_type;
    /// @brief Validated OpenAPI and integration metadata.
    openapi_document document;
};

/// @brief Deferred, type-erased artifact emission prepared from one annotation.
struct generated_artifact {
    /// @brief Output identity used for collision detection and ordering.
    std::string output_name;
    /// @brief Emits the complete artifact set beneath a generator-owned root.
    std::function<void(const fs::path&, std::string_view)> emit_artifacts;
};

/// @brief Operations required to process one registered annotation kind.
struct annotation_processor {
    /// @brief Validates a parsed request and prepares deferred artifact emission.
    std::function<generated_artifact(const annotation_request&)> prepare_artifact;
    /// @brief Derives the output identity without parsing the embedded specification.
    std::function<std::string(const fs::path&)> derive_output_name;
    /// @brief Maps an output identity to its generator-owned relative path.
    std::function<fs::path(std::string_view)> derive_owned_output_path;
    /// @brief Checks whether the complete current artifact set already exists.
    std::function<bool(const fs::path&, std::string_view)> has_complete_output;
    /// @brief Appends the output to the generated CMake inventory.
    std::function<void(std::ostringstream&, std::string_view)> append_build_inventory;
};

/// @brief Deterministically ordered annotation processors keyed by attribute spelling.
using annotation_registry = std::map<std::string, annotation_processor, std::less<>>;

/// @brief Validated command-line configuration for one generation invocation.
struct generation_options {
    /// @brief Running generator binary used to invalidate incremental state.
    fs::path generator_binary;
    /// @brief Root recursively scanned for annotated module sources.
    fs::path source_root;
    /// @brief Generator-owned artifact root.
    fs::path output_root;
    /// @brief Completion stamp updated after successful publication.
    fs::path stamp;
    /// @brief Clang executable used by cppast.
    std::string clang_binary;
    /// @brief Compiler identity recorded in generated provenance.
    std::string compiler_id;
};

/// @brief Current incremental metadata for one scanned module source.
struct source_snapshot {
    /// @brief Source size in bytes.
    std::uintmax_t file_size_bytes{};
    /// @brief Filesystem write-time ticks.
    std::int64_t last_write_ticks{};
    /// @brief FNV-1a hash of the exact source bytes.
    std::uint64_t content_hash{};
    /// @brief Whether the source contains a registered annotation.
    bool has_annotation{};
    /// @brief Registered annotation spelling when present.
    std::string annotation_name;
    /// @brief Annotation-derived output identity when present.
    std::string output_name;
    /// @brief Validated generator-owned relative output path when present.
    std::string owned_output_path;
};

/// @brief Source snapshots keyed by generic path relative to the scan root.
using source_snapshots = std::unordered_map<std::string, source_snapshot>;

} // namespace bioseq::codegen::external
