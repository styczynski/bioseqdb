/**
 * @file external/uniprot/integration.cppm
 * @brief UniProt client name and requests used by generated integration tests.
 */
module;

#include <string_view>

export module bioseq.external.uniprot:integration;

export namespace bioseq::external::uniprot::descriptor {

/** @brief Client name and example requests with expected responses. */
inline constexpr std::string_view integration_contracts =
    R"bioseq_openapi(x-bioseq-client-name: UniProt
x-bioseq-integration:
  - name: accession-p05067
    operationId: getByAccession
    accept: application/json
    parameters:
      - {name: accession, in: path, value: P05067}
      - {name: fields, in: query, value: accession}
    expect:
      status: 200
      bodyContains:
        - '"primaryAccession"'
        - '"P05067"'
  - name: accession-p05067-fasta
    operationId: getByAccession
    accept: text/plain;format=fasta
    mode: batch
    count: 2
    parameters:
      - {name: accession, in: path, value: P05067}
    expect:
      status: 200
      bodyContains:
        - '>sp|P05067|'
        - MLPGLALL
      headerContains:
        - {name: Content-Type, valueContains: text/plain}
  - name: reviewed-pagination
    operationId: searchCursor
    parameters:
      - {name: query, in: query, value: 'reviewed:true'}
      - {name: fields, in: query, value: accession}
      - {name: size, in: query, value: 1}
    expect:
      status: 200
      bodyContains:
        - '"results"'
        - '"primaryAccession"'
      headerContains:
        - {name: Link, valueContains: 'rel="next"'}
)bioseq_openapi";

} // namespace bioseq::external::uniprot::descriptor
