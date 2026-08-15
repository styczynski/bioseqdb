module;

#include <cstdint>
#include <filesystem>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <yaml-cpp/yaml.h>

module bioseq.codegen.external:tests.contract;

import :builder;
import :annotation_parser;
import :openapi_parser;
import :client_renderer;
import :model;
import bioseq.codegen.support;
import bioseq.common.hashing;
import bioseq.common.http.validation;

namespace http_validation = bioseq::common::http::validation;

namespace bioseq::codegen::external {

/// @brief Exercises pure parser/renderer contracts without network or project sources.
auto run_generator_contract_self_tests() -> void {
    const auto require = [](bool condition, std::string_view message) -> void {
        if (!condition) {
            throw std::runtime_error("generator self-test failed: " + std::string(message));
        }
    };
    const auto require_failure = [](auto operation, std::string_view expected) -> void {
        try {
            operation();
        } catch (const std::exception& error) {
            if (std::string_view{error.what()}.contains(expected)) {
                return;
            }
            throw std::runtime_error("generator self-test received unexpected diagnostic: " +
                                     std::string(error.what()));
        }
        throw std::runtime_error("generator self-test expected failure containing: " +
                                 std::string(expected));
    };

    const annotation_registry annotation_processors = make_annotation_registry();
    const auto make_descriptor =
        [](std::string path, std::string text, std::int64_t write_ticks = 1) -> descriptor_source {
        const std::uintmax_t file_size = text.size();
        const std::uint64_t content_hash = bioseq::common::fnv1a_64(text);
        return descriptor_source{
            .path = path,
            .relative_path = path,
            .text = std::move(text),
            .file_size_bytes = file_size,
            .last_write_ticks = write_ticks,
            .content_hash = content_hash,
        };
    };
    const auto make_fragment_primary = [](std::string_view imports,
                                          std::string_view elements,
                                          std::string_view factory =
                                              "std::to_array<std::string_view>") -> std::string {
        return "export module contract.primary;\n" + std::string(imports) +
               "\nstatic constexpr auto spec_fragments = " + std::string(factory) + "({\n" +
               std::string(elements) +
               "\n});\n[[bioseq::api_client(spec)]] "
               "[[bioseq::api_client_fragments(\n"
               "    spec_fragments)]] "
               "export struct bioseq_external_api_client;\n";
    };
    const std::string fragment_imports = "import :alpha;\nimport :beta;";
    const std::string fragment_elements = "contract::primary::descriptor::preamble,\n"
                                          "contract::primary::descriptor::middle,\n"
                                          "contract::primary::descriptor::tail,";
    const std::string alpha_partition = R"source(
export module contract.primary:alpha;
export namespace contract::primary::descriptor {
inline constexpr std::string_view preamble = R"fragment(first)fragment";
inline constexpr std::string_view middle = R"fragment(second)fragment";
}
)source";
    const std::string beta_partition = R"source(
export module contract.primary:beta;
export namespace contract::primary::descriptor {
inline constexpr std::string_view tail = R"fragment(third)fragment";
}
)source";

    std::vector<descriptor_source> fragment_sources{
        make_descriptor("primary.cppm", make_fragment_primary(fragment_imports, fragment_elements)),
        make_descriptor("alpha.cppm", alpha_partition, 2),
        make_descriptor("beta.cppm", beta_partition, 3),
    };
    module_provider_index fragment_providers = build_module_provider_index(fragment_sources);
    const std::string fragment_parser_source =
        prepare_annotation_parser_translation_unit(fragment_sources.front().text);
    require(fragment_parser_source.starts_with("struct [[bioseq::api_client(spec)]]") &&
                fragment_parser_source.contains("api_client_fragments(\n    spec_fragments)") &&
                !fragment_parser_source.contains("contract::primary::descriptor::preamble") &&
                !fragment_parser_source.contains("export struct"),
            "fragment parser shadow isolates the attributed primary declaration");
    const auto resolved_fragments =
        resolve_api_client_fragment_contract(fragment_sources.front().path,
                                             fragment_sources.front().text,
                                             "spec_fragments",
                                             fragment_providers,
                                             annotation_processors);
    require(resolved_fragments.first == "firstsecondthird",
            "fragment constants concatenate in declared array order");
    require(resolved_fragments.second.size() == 2 &&
                resolved_fragments.second[0] == fragment_sources[1].path &&
                resolved_fragments.second[1] == fragment_sources[2].path,
            "fragment closure retains ordered unique provider paths");

    const source_snapshot original_closure = make_specification_closure_snapshot(
        fragment_sources.front(), resolved_fragments.second, fragment_sources);
    std::vector<descriptor_source> content_changed_sources = fragment_sources;
    content_changed_sources[1].text.front() = 'X';
    content_changed_sources[1].content_hash =
        bioseq::common::fnv1a_64(content_changed_sources[1].text);
    require(make_specification_closure_snapshot(
                content_changed_sources.front(), resolved_fragments.second, content_changed_sources)
                    .content_hash != original_closure.content_hash,
            "fragment content invalidates the primary closure");
    std::vector<descriptor_source> mtime_changed_sources = fragment_sources;
    ++mtime_changed_sources[1].last_write_ticks;
    require(make_specification_closure_snapshot(
                mtime_changed_sources.front(), resolved_fragments.second, mtime_changed_sources)
                    .content_hash != original_closure.content_hash,
            "fragment mtime invalidates the primary closure");
    std::vector<descriptor_source> path_changed_sources = fragment_sources;
    path_changed_sources[1].path = "renamed_alpha.cppm";
    path_changed_sources[1].relative_path = "renamed_alpha.cppm";
    std::vector<std::filesystem::path> path_changed_dependencies = resolved_fragments.second;
    path_changed_dependencies[0] = path_changed_sources[1].path;
    require(make_specification_closure_snapshot(
                path_changed_sources.front(), path_changed_dependencies, path_changed_sources)
                    .content_hash != original_closure.content_hash,
            "fragment path invalidates the primary closure");

    const auto resolve_fixture = [&annotation_processors, &make_descriptor](
                                     std::string primary, std::vector<descriptor_source> partitions)
        -> std::pair<std::string, std::vector<std::filesystem::path>> {
        std::vector<descriptor_source> sources;
        sources.reserve(partitions.size() + 1);
        sources.push_back(make_descriptor("primary.cppm", std::move(primary)));
        for (descriptor_source& partition : partitions) {
            sources.push_back(std::move(partition));
        }
        const module_provider_index providers = build_module_provider_index(sources);
        return resolve_api_client_fragment_contract(sources.front().path,
                                                    sources.front().text,
                                                    "spec_fragments",
                                                    providers,
                                                    annotation_processors);
    };
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition,
         &beta_partition]() -> void {
            (void)resolve_fixture(make_fragment_primary(fragment_imports, ""),
                                  {
                                      make_descriptor("alpha.cppm", alpha_partition),
                                      make_descriptor("beta.cppm", beta_partition),
                                  });
        },
        "must not be empty");
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition,
         &beta_partition]() -> void {
            (void)resolve_fixture(make_fragment_primary(fragment_imports,
                                                        "contract::primary::descriptor::preamble,\n"
                                                        "contract::primary::descriptor::preamble,"),
                                  {
                                      make_descriptor("alpha.cppm", alpha_partition),
                                      make_descriptor("beta.cppm", beta_partition),
                                  });
        },
        "duplicate constant");
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition,
         &beta_partition]() -> void {
            (void)resolve_fixture(
                make_fragment_primary(fragment_imports, "foreign::descriptor::value,"),
                {
                    make_descriptor("alpha.cppm", alpha_partition),
                    make_descriptor("beta.cppm", beta_partition),
                    make_descriptor("foreign.cppm",
                                    R"source(export module contract.foreign:value;
export namespace foreign::descriptor {
inline constexpr std::string_view value = R"fragment(foreign)fragment";
}
)source"),
                });
        },
        "foreign fragment constant");
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition,
         &beta_partition]() -> void {
            (void)resolve_fixture(
                make_fragment_primary(fragment_imports,
                                      "contract::primary::descriptor::unimported,"),
                {
                    make_descriptor("alpha.cppm", alpha_partition),
                    make_descriptor("beta.cppm", beta_partition),
                    make_descriptor("gamma.cppm",
                                    R"source(export module contract.primary:gamma;
export namespace contract::primary::descriptor {
inline constexpr std::string_view unimported = R"fragment(foreign)fragment";
}
)source"),
                });
        },
        "not explicitly imported");
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition,
         &beta_partition]() -> void {
            (void)resolve_fixture(
                make_fragment_primary(fragment_imports, "contract::primary::descriptor::missing,"),
                {
                    make_descriptor("alpha.cppm", alpha_partition),
                    make_descriptor("beta.cppm", beta_partition),
                });
        },
        "missing");
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition]() -> void {
            (void)resolve_fixture(
                make_fragment_primary(fragment_imports, "contract::primary::descriptor::tail,"),
                {
                    make_descriptor("alpha.cppm", alpha_partition),
                    make_descriptor("beta.cppm",
                                    R"source(export module contract.primary:beta;
export namespace contract::primary::descriptor {
inline constexpr std::string_view tail = "not raw";
}
)source"),
                });
        },
        "non-raw initializer");
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition,
         &beta_partition]() -> void {
            (void)resolve_fixture(
                make_fragment_primary(fragment_imports, "contract::primary::descriptor::tail,"),
                {
                    make_descriptor("alpha.cppm", alpha_partition),
                    make_descriptor("beta.cppm", beta_partition),
                    make_descriptor("duplicate_beta.cppm", beta_partition),
                });
        },
        "ambiguous module provider");
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition]() -> void {
            (void)resolve_fixture(
                make_fragment_primary(fragment_imports, "contract::primary::descriptor::tail,"),
                {
                    make_descriptor("alpha.cppm", alpha_partition),
                    make_descriptor("beta.cppm",
                                    R"source(export module contract.primary:beta;
export namespace contract::primary::descriptor {
inline constexpr std::string_view tail = R"fragment(third)fragment";
}
static constexpr std::string_view spec = R"fragment(spec)fragment";
[[bioseq::api_client(spec)]] export struct nested_client;
)source"),
                });
        },
        "fragment unit registers api_client");
    require_failure(
        [&resolve_fixture,
         &make_fragment_primary,
         &fragment_imports,
         &make_descriptor,
         &alpha_partition,
         &beta_partition]() -> void {
            (void)resolve_fixture(make_fragment_primary(fragment_imports,
                                                        "contract::primary::descriptor::tail,",
                                                        "std::array<std::string_view, 1>"),
                                  {
                                      make_descriptor("alpha.cppm", alpha_partition),
                                      make_descriptor("beta.cppm", beta_partition),
                                  });
        },
        "must resolve to exactly one same-file");

    const auto server = parse_https_server("https://api.example.org:8443/v1/");
    require(server.host == "api.example.org", "explicit-port host parsing");
    require(server.port == 8443, "explicit-port value parsing");
    require(server.base_path == "/v1", "server path normalization");
    require(synthesized_get_operation_id("/uniprot/{identifier}.json") ==
                "get_uniprot_identifier_json",
            "missing operationId synthesis");
    require(synthesized_get_operation_id("/uniprot/{identifiers}.json") ==
                "get_uniprot_identifiers_json",
            "synthesized operationId uniqueness");
    const auto collision_contract = parse_openapi(R"yaml(
openapi: 3.0.0
info: {title: Collision API}
servers: [{url: 'https://api.example.org'}]
paths:
  /a-b:
    get: {responses: {'200': {}}}
  /a_b:
    get: {responses: {'200': {}}}
x-bioseq-client-name: Collision
x-bioseq-integration:
  - name: collision-suffix
    operationId: get_a_b_path_2f612d62
    expect: {status: 200, bodyContains: [ok]}
)yaml");
    require(collision_contract.get_operations.size() == 2 &&
                collision_contract.get_operations[0].operation_id !=
                    collision_contract.get_operations[1].operation_id &&
                collision_contract.get_operations[0].operation_id.contains("_path_") &&
                collision_contract.get_operations[1].operation_id.contains("_path_"),
            "collision-free synthesized operationId suffixes");
    const auto stable_collision_id = std::ranges::find_if(
        collision_contract.get_operations, [](const openapi_operation& operation) -> bool {
            return operation.path == std::string_view{"/a-b"};
        });
    require(stable_collision_id != collision_contract.get_operations.end() &&
                stable_collision_id->operation_id ==
                    synthesized_get_operation_id("/a-b") + synthesized_path_suffix("/a-b"),
            "synthesized operationId depends only on its exact path");
    const auto reordered_collision_contract = parse_openapi(R"yaml(
openapi: 3.0.0
info: {title: Collision API}
servers: [{url: 'https://api.example.org'}]
paths:
  /a_b:
    get: {responses: {'200': {}}}
  /a-b:
    get: {responses: {'200': {}}}
x-bioseq-client-name: Collision
x-bioseq-integration:
  - name: collision-suffix
    operationId: get_a_b_path_2f612d62
    expect: {status: 200, bodyContains: [ok]}
)yaml");
    require(reordered_collision_contract.get_operations.size() ==
                    collision_contract.get_operations.size() &&
                reordered_collision_contract.get_operations[0].operation_id ==
                    collision_contract.get_operations[0].operation_id &&
                reordered_collision_contract.get_operations[1].operation_id ==
                    collision_contract.get_operations[1].operation_id,
            "synthesized operationIds are independent of YAML path order");
    require_failure([]() -> void { (void)parse_https_server("https://user@example.org/v1"); },
                    "userinfo");
    require_failure([]() -> void { (void)parse_https_server("https://a..example.org/v1"); },
                    "DNS host");
    require_failure([]() -> void { (void)parse_https_server("https://example.org/v1\nnext"); },
                    "base path");

    constexpr std::string_view valid_openapi = R"yaml(
openapi: 3.0.0
info: {title: Contract API}
servers: [{url: 'https://api.example.org:8443/v1/'}]
security:
  - ApiKey: []
paths:
  /items/{id}:
    get:
      operationId: fetchItem
      parameters:
        - {name: id, in: path, required: true, schema: {type: integer}}
        - name: tag
          in: query
          schema: {type: array, items: {type: string}}
        - {name: enabled, in: query, schema: {type: boolean}}
        - {name: ratio, in: query, schema: {type: number}}
        - {name: Accept, in: header, required: true, schema: {type: string}}
      responses:
        '200':
          content:
            'application/vnd.example+json;version=2': {}
components:
  securitySchemes:
    ApiKey: {type: apiKey, in: header, name: X-Api-Key}
x-bioseq-client-name: Contract
x-bioseq-integration:
  - name: typed-batch
    operationId: fetchItem
    accept: application/json
    mode: batch
    count: 2
    parameters:
      - {name: id, in: path, value: -9223372036854775808}
      - {name: tag, in: query, value: first}
      - {name: tag, in: query, value: second}
      - {name: enabled, in: query, value: true}
      - {name: ratio, in: query, value: 0.5}
    expect:
      status: 200
      bodyContains: [ok]
      headerContains:
        - {name: Link, valueContains: 'rel="next"'}
)yaml";
    const auto contract = parse_openapi(valid_openapi);
    require(contract.host == "api.example.org" && contract.port == 8443,
            "OpenAPI authority lowering");
    require(contract.get_operations.size() == 1 &&
                contract.get_operations.front().accept ==
                    "application/vnd.example+json;version=2" &&
                contract.get_operations.front().parameters.size() == 4,
            "OpenAPI operation/media lowering");
    require(contract.get_operations.front().api_key_name == "X-Api-Key",
            "OpenAPI API-key lowering");
    require(contract.integration_cases.size() == 1 &&
                contract.integration_cases.front().parameters.size() == 5 &&
                contract.integration_cases.front().count == 2 &&
                contract.integration_cases.front().accept == "application/json" &&
                contract.integration_cases.front().header_contains.size() == 1,
            "typed batch fixture lowering");
    const generated_api contract_api{
        .client_name = "contract",
        .declared_type = "bioseq_external_api_client",
        .document = contract,
    };
    require(render_module(contract_api).contains("transport_(\"api.example.org\", 8443)"),
            "explicit-port client rendering");
    const std::string rendered_contract_test = render_test(contract_api);
    require(rendered_contract_test.contains("std::numeric_limits<std::int64_t>::min()"),
            "typed fixture rendering");
    require(rendered_contract_test.contains(
                "TEST_CASE(\"generated contract external API client performs live requests\""),
            "descriptor-specific generated Catch identity");
    require(rendered_contract_test.contains("[feature:external_source]"),
            "generated external-source feature taxonomy");
    require(!rendered_contract_test.contains("[feature:io]"),
            "retired generated external IO feature taxonomy");
    require(render_module(contract_api).contains("accept.empty() ? operation->accept : accept"),
            "explicit Accept precedence rendering");
    require(render_module(contract_api).contains("get_with_accept"),
            "unambiguous Accept entry point rendering");
    require(rendered_contract_test.contains("response_header_contains"),
            "response-header expectation rendering");
    YAML::Node invalid_metadata = YAML::Load(std::string(valid_openapi));
    invalid_metadata["x-bioseq-integration"][0]["unknown"] = true;
    const std::string invalid_metadata_spec = YAML::Dump(invalid_metadata);
    require_failure(
        [invalid_metadata_spec]() -> void { (void)parse_openapi(invalid_metadata_spec); },
        "unknown integration case key");
    YAML::Node invalid_accept = YAML::Load(std::string(valid_openapi));
    invalid_accept["x-bioseq-integration"][0]["accept"] = "text/plain\r\nInjected: yes";
    const std::string invalid_accept_spec = YAML::Dump(invalid_accept);
    require_failure([invalid_accept_spec]() -> void { (void)parse_openapi(invalid_accept_spec); },
                    "integration Accept override is invalid");

    const YAML::Node plain_json = YAML::Load(R"yaml(
operation:
  responses:
    '200':
      content:
        'application/json; charset=UTF-8': {}
)yaml");
    require(preferred_response_media_type(plain_json, plain_json["operation"]) ==
                "application/json",
            "plain JSON charset normalization");
    const YAML::Node vendor_json = YAML::Load(R"yaml(
operation:
  responses:
    '200':
      content:
        'application/vnd.example+json;version=2': {}
)yaml");
    require(preferred_response_media_type(vendor_json, vendor_json["operation"]) ==
                "application/vnd.example+json;version=2",
            "JSON media parameter preservation");

    const YAML::Node compound_security = YAML::Load(R"yaml(
components:
  securitySchemes:
    first: {type: apiKey, in: header, name: X-First-Key}
    second: {type: apiKey, in: query, name: second_key}
security:
  - first: []
    second: []
operation: {}
)yaml");
    require_failure(
        [&compound_security]() -> void {
            (void)effective_api_key_requirement(compound_security, compound_security["operation"]);
        },
        "compound");
    const YAML::Node anonymous_security = YAML::Load(R"yaml(
security: [{}]
operation: {}
)yaml");
    require(!effective_api_key_requirement(anonymous_security, anonymous_security["operation"]),
            "anonymous security alternative");
    require(!http_validation::is_valid_header_name("Bad Header"), "header token rejection");
    require(
        integration_scalar_literal(integration_scalar{std::numeric_limits<std::int64_t>::min()}) ==
            "std::numeric_limits<std::int64_t>::min()",
        "INT64_MIN literal rendering");
}
} // namespace bioseq::codegen::external
