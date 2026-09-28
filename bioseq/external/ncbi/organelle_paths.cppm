/**
 * @file external/ncbi/organelle_paths.cppm
 * @brief NCBI organelle download and  report requests.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:organelle_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Organelle data packages requested by nucleotide accession. */
inline constexpr std::string_view organelle_download_paths =
    R"bioseq_openapi(  /organelle/accession/{accessions}/download:
    get:
      summary: Get an organelle data package by nucleotide accession
      description: 'Download an organelle data package including sequence, annotation, and detailed data reports as a compressed zip archive.'
      tags:
        - Organelle
      operationId: download_organelle_package
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
      - name: accessions
        description: 'NCBI organelle assembly accessions'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NC_001643.1
            summary: Chimpanzee mitochondrial genome
          example-1:
            value:
              - NC_001643.1
              - NC_002082.1
            summary: Chimpanzee and common Gibbon mitochondrial genomes
      - name: include_annotation_type
        description: 'Specify which sequence files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2AnnotationForOrganelleType'
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /organelle/download:
    post:
      summary: Get an organelle data package
      description: 'Download an organelle data package including sequence, annotation, and detailed data reports as a compressed zip archive.'
      tags:
        - Organelle
      operationId: download_organelle_package_by_post
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
              $ref: '#/components/schemas/v2OrganelleDownloadRequest'
            examples:
              Single organelle accession example:
                description: Organelle accession (just one)
                value:
                  accessions:
                    - NC_001643.1
      parameters:
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
)bioseq_openapi";

/** @brief Organelle reports requested by accession or taxon. */
inline constexpr std::string_view organelle_report_paths =
    R"bioseq_openapi(  /organelle/accessions/{accessions}/dataset_report:
    get:
      summary: Get an organelle data report by nucleotide accession
      description: 'Get an organelle data report in JSON format.'
      tags:
        - Organelle
      operationId: organelle_datareport_by_accession
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
                $ref: '#/components/schemas/v2reportsOrganelleDataReports'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsOrganelleDataReports'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: accessions
        description: 'One or more organelle nucleotide accessions'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NC_001643.1
            summary: Chimpanzee mitochondrial genome
          example-1:
            value:
              - NC_001643.1
              - NC_002082.1
            summary: Chimpanzee and common gibbon mitochondrial genomes
      - name: organelle_types
        description: 'Limit results to the specified organelle types.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsOrganelleType'
      - name: first_release_date
        description: 'Limit results to organelle genomes released on or after the specified date.'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2015-01-10"
            summary: Jan 10, 2015, in ISO 8601 YYYY-MM-DD format
      - name: last_release_date
        description: 'Limit results to organelle genomes released on or before the specified date.'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2021-01-10"
            summary: Jan 10, 2021, in ISO 8601 YYYY-MM-DD format
      - name: sort.field
        in: query
        required: false
        schema:
          type: string
      - name: sort.direction
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2SortDirection'
      - name: returned_content
        description: 'Return complete organelle reports or nucleotide accessions only.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2OrganelleMetadataRequestContentType'
        examples:
          example-0:
            value: ASSM_ACC
            summary: Return nucleotide accessions only
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2OrganelleMetadataRequestOrganelleTableFormat'
        examples:
          example-0:
            value: SUMMARY
            summary: This template specifies a basic set of fields available from the organelle report
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /organelle/taxon/{taxons}/dataset_report:
    get:
      summary: Get an organelle data report by taxon
      description: 'Get an organelle data report  in JSON format.'
      tags:
        - Organelle
      operationId: organelle_datareport_by_taxon
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
                $ref: '#/components/schemas/v2reportsOrganelleDataReports'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsOrganelleDataReports'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: taxons
        description: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: "9443"
            summary: Primate organelle genomes
          example-1:
            value:
              - "9606"
              - "10090"
            summary: Human and mouse organelle genomes
      - name: organelle_types
        description: 'Limit results to the specified organelle types.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsOrganelleType'
      - name: first_release_date
        description: 'Limit results to organelle genomes released on or after the specified date.'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2015-01-10"
            summary: Jan 10, 2015, in ISO 8601 YYYY-MM-DD format
      - name: last_release_date
        description: 'Limit results to organelle genomes released on or before the specified date.'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2021-01-10"
            summary: Jan 10, 2021, in ISO 8601 YYYY-MM-DD format
      - name: tax_exact_match
        description: 'If true, only return assemblies with the given NCBI Taxonomy ID, or name. Otherwise, assemblies from taxonomy subtree are included, too.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
      - name: sort.field
        in: query
        required: false
        schema:
          type: string
      - name: sort.direction
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2SortDirection'
      - name: returned_content
        description: 'Return complete organelle reports or nucleotide accessions only.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2OrganelleMetadataRequestContentType'
        examples:
          example-0:
            value: ASSM_ACC
            summary: Return nucleotide accessions only
      - name: page_size
        description: 'The maximum number of organelle assemblies to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        in: query
        required: false
        schema:
          type: integer
      - name: page_token
        description: 'A page token is returned from an `OrganelleMetadata` call with more than `page_size` results. Use this token, along with the previous `OrganelleMetadata` parameters, to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        in: query
        required: false
        schema:
          type: string
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2OrganelleMetadataRequestOrganelleTableFormat'
        examples:
          example-0:
            value: SUMMARY
            summary: This template specifies a basic set of fields available from the organelle report
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /organelle/dataset_report:
    post:
      summary: Get an organelle data report
      description: 'Get an organelle data report in JSON format.'
      tags:
        - Organelle
      operationId: organelle_datareport_by_post
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
                $ref: '#/components/schemas/v2reportsOrganelleDataReports'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsOrganelleDataReports'
            text/tab-separated-values:
              schema:
                type: string
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2OrganelleMetadataRequest'
            examples:
              Single TaxID example:
                description: Primate organelle genomes
                value:
                  taxons:
                    - "9443"
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
