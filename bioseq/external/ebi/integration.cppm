/**
 * @file external/ebi/integration.cppm
 * @brief EBI client name and a request used to check its integration.
 */
module;

#include <string_view>

export module bioseq.external.ebi:integration;

export namespace bioseq::external::ebi::descriptor {

/** Names the generated client and defines a BRCA1 root-search check expecting hitCount. */
inline constexpr std::string_view integration_contracts = R"bioseq_openapi(x-bioseq-client-name: EBI
x-bioseq-integration:
  - name: root-search
    operationId: rootsearch
    mode: single
    parameters:
      - {name: query, in: query, value: BRCA1}
    expect:
      status: 200
      bodyContains:
        - '"hitCount"'
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
