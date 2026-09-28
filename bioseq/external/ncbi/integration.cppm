/**
 * @file external/ncbi/integration.cppm
 * @brief Client name and live request checks for generated NCBI code
 */
module;

#include <string_view>

export module bioseq.external.ncbi:integration;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Names the client and defines version and accession-lookup checks. */
inline constexpr std::string_view integration_contracts =
    R"bioseq_openapi(x-bioseq-client-name: NCBI
x-bioseq-integration:
  - name: version
    operationId: version
    mode: single
    expect:
      status: 200
      bodyContains:
        - version
  - name: accession-lookup
    operationId: assembly_accessions_for_sequence_accession
    mode: single
    parameters:
      - name: accession
        in: path
        value: NC_000001.11
    expect:
      status: 200
      bodyContains:
        - GCF_
  - name: concurrent-version
    operationId: version
    mode: batch
    count: 2
    preDelayWithoutCredentialsMs: 1100
    expect:
      status: 200
      bodyContains:
        - version
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
