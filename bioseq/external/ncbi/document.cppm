/**
 * @file external/ncbi/document.cppm
 * @brief NCBI Datasets API metadata and service-version request.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:document;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Starts the OpenAPI document with API metadata and the paths section. */
inline constexpr std::string_view document_preamble = R"bioseq_openapi(
openapi: 3.0.1
info:
  title: NCBI Datasets API
  version: v2
  description: '### NCBI Datasets is a resource that lets you easily gather data from
      NCBI.

      The NCBI Datasets version 2 API is updated often to add new features, fix bugs, and enhance usability.

    '
servers:
- url: https://api.ncbi.nlm.nih.gov/datasets/v2
tags:
- name: Genome
  description: '#### Options to download assembled genome data,
    including the associated sequence, annotation and metadata.

    These genome services allow you to get genome metadata as a data report or download genome,
    transcript and protein sequence, annotation and metadata as a genome data package, for assembled genomes.

    '
- name: Gene
  description: '#### Options to download gene data, including the
    associated sequence and metadata.

    These gene services allow you to get gene metadata as a data report or download gene, transcript
    and protein sequence, and metadata, as a gene data package, for all genes in NCBI Gene.

    '
- name: Prokaryote
  description: '#### Options to download prokaryote gene data, including the associated
    sequence and metadata.

    These gene services allow you to download gene and protein sequence, and metadata as a prokaryote
    gene data package, for prokaryotic [RefSeq non-redundant proteins](https://www.ncbi.nlm.nih.gov/refseq/about/nonredundantproteins/).

    '
- name: Virus
  description: '#### Options to download virus genome data, including the
    associated sequence and metadata.

    These virus services allow you to get virus genome metadata as a data report or download genome and protein
    sequence, and metadata, as a virus data package, for virus GenBank genomes.
    '

- name: Version
  description: '#### Retrieve the current version of all NCBI Datasets services.

    NCBI Datasets services follow the [Semantic Versioning 2.0.0 Schema](https://semver.org/spec/v2.0.0.html).
    '

- name: Taxonomy
  description: '#### Options to download taxonomy data.

    These taxonomy services allow you to get taxonomy data as a data report or download taxonomy
    data as a taxonomy data package, for taxonomic nodes in NCBI Taxonomy.

    '

- name: BioSample
  description: '#### Options to download BioSample data.

    This BioSample service allows you to get BioSample data as a data report.

    '

- name: Organelle
  description: '#### Options to download RefSeq organelle genome data, including the associated sequence and metadata.

    These organelle services allow you to get RefSeq organelle genome metadata as a data report or download gene,
    transcript and protein sequence, and metadata, as an organelle data package, for RefSeq organelle genomes.

    '

externalDocs:
  description: Interactively explore genome assembly datasets
  url: https://www.ncbi.nlm.nih.gov/datasets/genomes/
security:
- ApiKeyAuthHeader: []
paths:
)bioseq_openapi";

/** @brief GET /version request and its response type. */
inline constexpr std::string_view version_path = R"bioseq_openapi(  /version:
    get:
      summary: Retrieve service version
      description: 'Retrieve the latest version of the Datasets services.'
      tags:
        - Version
      operationId: version
      responses:
        default:
          description: An unexpected error response.
          content:
            text/plain:
              schema:
                $ref: '#/components/schemas/rpcStatus'
        '200':
          description: A successful response
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/v2VersionReply'
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
