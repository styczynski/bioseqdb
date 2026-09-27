/**
 * @file external/ncbi/gene_paths.cppm
 * @brief OpenAPI requests for downloading NCBI gene data packages.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:gene_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Gene  package downloads using GeneIDs in the path or a JSON request. */
inline constexpr std::string_view gene_download_paths =
    R"bioseq_openapi(  /gene/id/{gene_ids}/download:
    get:
      summary: Get a gene data package by GeneID
      description: 'Download a gene data package including sequence, annotation and data reports, as a compressed zip archive, by GeneID.'
      tags:
        - Gene
      operationId: download_gene_package
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
            application/zip:
              schema:
                format: binary
                type: string
                description: Zip compressed stream
      parameters:
      - name: gene_ids
        description: 'One or more NCBI GeneIDs'
        in: path
        required: true
        schema:
          type: array
          items:
            type: integer
        examples:
          example-0:
            value: 59067
            summary: Human IL21, interleukin 21
          example-1:
            value:
              - 59067
              - 50615
            summary: Human IL21 and IL21R, interleukin 21 and interleukin 21 receptor
      - name: include_annotation_type
        description: 'Specify which sequence files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2Fasta'
        examples:
          example-0:
            value:
              - FASTA_RNA
              - FASTA_PROTEIN
            summary: Include RNA and protein sequence files
          example-1:
            value:
              - FASTA_CDS
              - FASTA_5P_UTR
              - FASTA_3P_UTR
            summary: Include coding sequence, and 5' and 3' UTRs
      - name: accession_filter
        description: 'Limit the contents of the sequence files and tabular product report to the specified RNA and protein accessions'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NM_021803.4
            summary: Human IL21 MANE Select transcript
      - name: aux_report
        description: 'Specify additional report files to include in the data package. The gene data report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneDatasetRequestGeneDatasetReportType'
        examples:
          example-0:
            value: PRODUCT_REPORT
            summary: Add the gene product report
      - name: tabular_reports
        description: 'Specify which tabular report files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneDatasetRequestGeneDatasetReportType'
        examples:
          example-0:
            value:
              - DATASET_REPORT
              - PRODUCT_REPORT
            summary: Include tabular gene and gene product reports
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /gene/download:
    post:
      summary: Get a gene data package
      description: 'Download a gene data package including sequence, annotation and data reports, as a compressed zip archive.'
      tags:
        - Gene
      operationId: download_gene_package_post
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
            application/zip:
              schema:
                format: binary
                type: string
                description: Zip compressed stream
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2GeneDatasetRequest'
            examples:
              Multiple GeneID examples:
                description: Human interleukin 21 and interleukin 21 receptor genes, with RNA and protein sequence files, and both gene and product reports
                value:
                  gene_ids:
                  - 59067
                  - 50615
                  include_annotation_type:
                  - FASTA_RNA
                  - FASTA_PROTEIN
                  aux_report:
                  - DATASET_REPORT
                  - PRODUCT_REPORT
      parameters:
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
