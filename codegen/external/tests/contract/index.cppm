module;

#include <cstdint>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <yaml-cpp/yaml.h>

module bioseq.codegen.external:tests.contract;

import :builder;
import :openapi_parser;
import :client_renderer;
import :model;
import bioseq.codegen.support;
import bioseq.common.http.validation;

namespace http_validation = bioseq::common::http::validation;

namespace bioseq::codegen::external {

/// @brief Exercises pure parser/renderer contracts without network or project sources.
void run_generator_contract_self_tests() {
    const auto require = [](bool condition, std::string_view message) {
        if (!condition) {
            throw std::runtime_error("generator self-test failed: " + std::string(message));
        }
    };
    const auto require_failure = [](auto operation, std::string_view expected) {
        try {
            operation();
        } catch (const std::exception& error) {
            if (std::string_view{error.what()}.contains(expected)) return;
            throw std::runtime_error(
                "generator self-test received unexpected diagnostic: "
                + std::string(error.what()));
        }
        throw std::runtime_error(
            "generator self-test expected failure containing: " + std::string(expected));
    };

    const auto server = parse_https_server("https://api.example.org:8443/v1/");
    require(server.host == "api.example.org", "explicit-port host parsing");
    require(server.port == 8443, "explicit-port value parsing");
    require(server.base_path == "/v1", "server path normalization");
    require(
        synthesized_get_operation_id("/uniprot/{identifier}.json")
            == "get_uniprot_identifier_json",
        "missing operationId synthesis");
    require(
        synthesized_get_operation_id("/uniprot/{identifiers}.json")
            == "get_uniprot_identifiers_json",
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
    require(
        collision_contract.get_operations.size() == 2
            && collision_contract.get_operations[0].operation_id
                != collision_contract.get_operations[1].operation_id
            && collision_contract.get_operations[0].operation_id.contains("_path_")
            && collision_contract.get_operations[1].operation_id.contains("_path_"),
        "collision-free synthesized operationId suffixes");
    const auto stable_collision_id = std::ranges::find_if(
        collision_contract.get_operations,
        [](const openapi_operation& operation) {
            return operation.path == std::string_view{"/a-b"};
        });
    require(
        stable_collision_id != collision_contract.get_operations.end()
            && stable_collision_id->operation_id
                == synthesized_get_operation_id("/a-b")
                    + synthesized_path_suffix("/a-b"),
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
    require(
        reordered_collision_contract.get_operations.size()
                == collision_contract.get_operations.size()
            && reordered_collision_contract.get_operations[0].operation_id
                == collision_contract.get_operations[0].operation_id
            && reordered_collision_contract.get_operations[1].operation_id
                == collision_contract.get_operations[1].operation_id,
        "synthesized operationIds are independent of YAML path order");
    require_failure(
        [] { (void)parse_https_server("https://user@example.org/v1"); },
        "userinfo");
    require_failure(
        [] { (void)parse_https_server("https://a..example.org/v1"); },
        "DNS host");
    require_failure(
        [] { (void)parse_https_server("https://example.org/v1\nnext"); },
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
    require(contract.get_operations.size() == 1
                && contract.get_operations.front().accept
                    == "application/vnd.example+json;version=2"
                && contract.get_operations.front().parameters.size() == 4,
            "OpenAPI operation/media lowering");
    require(contract.get_operations.front().api_key_name == "X-Api-Key",
            "OpenAPI API-key lowering");
    require(contract.integration_cases.size() == 1
                && contract.integration_cases.front().parameters.size() == 5
                && contract.integration_cases.front().count == 2
                && contract.integration_cases.front().accept == "application/json"
                && contract.integration_cases.front().header_contains.size() == 1,
            "typed batch fixture lowering");
    const generated_api contract_api{
        .client_name = "contract",
        .declared_type = "bioseq_external_api_client",
        .document = contract,
    };
    require(render_module(contract_api).contains(
                "transport_(\"api.example.org\", 8443)"),
            "explicit-port client rendering");
    const std::string rendered_contract_test = render_test(contract_api);
    require(rendered_contract_test.contains(
                "std::numeric_limits<std::int64_t>::min()"),
            "typed fixture rendering");
    require(rendered_contract_test.contains(
                "TEST_CASE(\"generated contract external API client performs live requests\""),
            "descriptor-specific generated Catch identity");
    require(rendered_contract_test.contains("[feature:external_source]"),
            "generated external-source feature taxonomy");
    require(!rendered_contract_test.contains("[feature:io]"),
            "retired generated external IO feature taxonomy");
    require(render_module(contract_api).contains(
                "accept.empty() ? operation->accept : accept"),
            "explicit Accept precedence rendering");
    require(render_module(contract_api).contains("get_with_accept"),
            "unambiguous Accept entry point rendering");
    require(rendered_contract_test.contains("response_header_contains"),
            "response-header expectation rendering");
    YAML::Node invalid_metadata = YAML::Load(std::string(valid_openapi));
    invalid_metadata["x-bioseq-integration"][0]["unknown"] = true;
    const std::string invalid_metadata_spec = YAML::Dump(invalid_metadata);
    require_failure(
        [invalid_metadata_spec] { (void)parse_openapi(invalid_metadata_spec); },
        "unknown integration case key");
    YAML::Node invalid_accept = YAML::Load(std::string(valid_openapi));
    invalid_accept["x-bioseq-integration"][0]["accept"] =
        "text/plain\r\nInjected: yes";
    const std::string invalid_accept_spec = YAML::Dump(invalid_accept);
    require_failure(
        [invalid_accept_spec] { (void)parse_openapi(invalid_accept_spec); },
        "integration Accept override is invalid");

    const YAML::Node plain_json = YAML::Load(R"yaml(
operation:
  responses:
    '200':
      content:
        'application/json; charset=UTF-8': {}
)yaml");
    require(
        preferred_response_media_type(plain_json, plain_json["operation"])
            == "application/json",
        "plain JSON charset normalization");
    const YAML::Node vendor_json = YAML::Load(R"yaml(
operation:
  responses:
    '200':
      content:
        'application/vnd.example+json;version=2': {}
)yaml");
    require(
        preferred_response_media_type(vendor_json, vendor_json["operation"])
            == "application/vnd.example+json;version=2",
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
        [&compound_security] {
            (void)effective_api_key_requirement(
                compound_security, compound_security["operation"]);
        },
        "compound");
    const YAML::Node anonymous_security = YAML::Load(R"yaml(
security: [{}]
operation: {}
)yaml");
    require(
        !effective_api_key_requirement(
            anonymous_security, anonymous_security["operation"]),
        "anonymous security alternative");
    require(
        !http_validation::is_valid_header_name("Bad Header"),
        "header token rejection");
    require(
        integration_scalar_literal(integration_scalar{
            std::numeric_limits<std::int64_t>::min()})
            == "std::numeric_limits<std::int64_t>::min()",
        "INT64_MIN literal rendering");
}

}
