/**
 * @file external/ncbi/taxonomy_paths.cppm
 * @brief NCBI taxonomy report, download, and lookup requests.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:taxonomy_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Requests for taxonomy data packages by taxon ID. */
inline constexpr std::string_view taxonomy_download_paths =
    R"bioseq_openapi(  /taxonomy/taxon/{tax_ids}/download:
    get:
      summary: Get a taxonomy data package by Taxonomy ID
      description: 'Download a taxonomy data package, including taxonomy and names reports, as a compressed zip archive.'
      tags:
        - Taxonomy
      operationId: download_taxonomy_package
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
      - name: tax_ids
        in: path
        required: true
        schema:
          type: array
          items:
            type: integer
        examples:
          example-0:
            value: 9606
            summary: NCBI Taxonomy ID for human
          example-1:
            value:
              - 9606
              - 10090
            summary: NCBI Taxonomy IDs for human and mouse
      - name: aux_reports
        description: 'Specify additional report files to include in the data package. The taxonomy report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2TaxonomyDatasetRequestTaxonomyReportType'
        examples:
          example-0:
            value:
              - NAMES_REPORT
              - TAXONOMY_SUMMARY
            summary: Add the names report (JSON Lines format) and taxonomy summary (TSV) files.
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /taxonomy/download:
    post:
      summary: Get a taxonomy data package by Taxonomy ID
      description: 'Download a taxonomy data package, including taxonomy and names reports, as a compressed zip archive.'
      tags:
        - Taxonomy
      operationId: download_taxonomy_package_by_post
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
              $ref: '#/components/schemas/v2TaxonomyDatasetRequest'
            examples:
              Multiple TaxID examples:
                description: Taxonomy report, names report, and taxonomy summary table for human and mouse
                value:
                  tax_ids:
                  - 9606
                  - 10090
                  aux_reports:
                   - NAMES_REPORT
                   - TAXONOMY_SUMMARY
      parameters:
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
)bioseq_openapi";

/** @brief Taxonomy reports and names, tree relationships, links, and images. */
inline constexpr std::string_view taxonomy_report_paths =
    R"bioseq_openapi(  /taxonomy/taxon/{taxons}:
    get:
      deprecated: true
      summary: Use taxonomic identifiers to get taxonomic metadata
      description: 'Using NCBI Taxonomy IDs or names (common or scientific) at any rank, get metadata about a taxonomic node including taxonomic identifiers, lineage information, child nodes, and gene and genome counts in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_metadata
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
                $ref: '#/components/schemas/v2TaxonomyMetadataResponse'
      parameters:
      - name: taxons
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy ID for human
          example-1:
            value:
              - human
              - house mouse
            summary: Common names
          example-2:
            value:
              - Homo sapiens
              - Mus musculus
            summary: Scientific names
      - name: returned_content
        description: 'Return complete taxonomy reports, Taxonomy IDs only, or reports without assembly and gene counts (metadata).'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2TaxonomyMetadataRequestContentType'
        examples:
          example-0:
            value: TAXIDS
            summary: Return Taxonomy IDs only
          example-1:
            value: METADATA
            summary: Return abbreviated taxonomy report without genome assembly and gene counts
      - name: page_size
        description: 'The maximum number of taxons to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        in: query
        required: false
        schema:
          type: integer
          default: 20
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
      - name: page_token
        description: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        in: query
        required: false
        schema:
          type: string
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2TaxonomyMetadataRequestTableFormat'
        examples:
          example-0:
            value: SUMMARY
            summary: This template specifies a basic set of fields available from the taxonomy report
      - name: children
        description: 'If true, return results for child taxa.'
        in: query
        required: false
        schema:
          type: boolean
      - name: ranks
        description: 'Limit results to taxa of the specified ranks.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRankType'
  /taxonomy:
    post:
      deprecated: true
      summary: Get taxonomy metadata by taxon
      description: 'Get taxonomy metadata by taxon in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_metadata_post
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
                $ref: '#/components/schemas/v2TaxonomyMetadataResponse'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2TaxonomyMetadataRequest'
            examples:
              Multiple TaxID and common name examples:
                description: NCBI Taxonomy ID for human and common name for Mus musculus
                value:
                  taxons:
                  - "9606"
                  - house mouse
  /taxonomy/taxon/{taxons}/dataset_report:
    get:
      summary: Get a taxonomy data report by taxon
      description: 'Get a taxonomy data report by taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Taxonomy
      operationId: taxonomy_data_report
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
                $ref: '#/components/schemas/v2reportsTaxonomyDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsTaxonomyDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: taxons
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy ID for human
          example-1:
            value:
              - human
              - house mouse
            summary: Common names
          example-2:
            value:
              - Homo sapiens
              - Mus musculus
            summary: Scientific names
      - name: returned_content
        description: 'Return complete taxonomy reports, Taxonomy IDs only, or reports without assembly and gene counts (metadata).'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2TaxonomyMetadataRequestContentType'
        examples:
          example-0:
            value: TAXIDS
            summary: Return Taxonomy IDs only
          example-1:
            value: METADATA
            summary: Return abbreviated taxonomy report without genome assembly and gene counts
      - name: page_size
        description: 'The maximum number of taxons to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        in: query
        required: false
        schema:
          type: integer
          default: 20
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
      - name: page_token
        description: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        in: query
        required: false
        schema:
          type: string
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2TaxonomyMetadataRequestTableFormat'
        examples:
          example-0:
            value: SUMMARY
            summary: This template specifies a basic set of fields available from the taxonomy report
      - name: children
        description: 'If true, return results for child taxa.'
        in: query
        required: false
        schema:
          type: boolean
      - name: ranks
        description: 'Limit results to taxa of the specified ranks.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRankType'
  /taxonomy/dataset_report:
    post:
      summary: Get a taxonomy data report by taxon
      description: 'Get a taxonomy data report by taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Taxonomy
      operationId: taxonomy_data_report_post
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
                $ref: '#/components/schemas/v2reportsTaxonomyDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsTaxonomyDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2TaxonomyMetadataRequest'
            examples:
              Multiple TaxID and common name examples:
                description: NCBI Taxonomy ID for human and common name for Mus musculus
                value:
                  taxons:
                  - "9606"
                  - house mouse
  /taxonomy/taxon/{taxons}/name_report:
    get:
      summary: Get a taxonomy names report by taxon
      description: 'Get a taxonomy names report, including common names and other synonyms, in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_names
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
                $ref: '#/components/schemas/v2reportsTaxonomyNamesDataReportPage'
      parameters:
      - name: taxons
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy ID for human
          example-1:
            value:
              - human
              - house mouse
            summary: Common names
          example-2:
            value:
              - Homo sapiens
              - Mus musculus
            summary: Scientific names
      - name: page_size
        description: 'The maximum number of taxons to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        in: query
        required: false
        schema:
          type: integer
          default: 20
      - name: page_token
        description: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        in: query
        required: false
        schema:
          type: string
      - name: children
        description: 'If true, return results for child taxa.'
        in: query
        required: false
        schema:
          type: boolean
      - name: ranks
        description: 'Limit results to taxa of the specified ranks.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRankType'
  /taxonomy/name_report:
    post:
      summary: Get a taxonomy names report by taxon
      description: 'Get a taxonomy names report, including common names and other synonyms, in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_names_post
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
                $ref: '#/components/schemas/v2reportsTaxonomyNamesDataReportPage'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2TaxonomyMetadataRequest'
            examples:
              Multiple TaxID and common name examples:
                description: NCBI Taxonomy ID for human and common name for Mus musculus
                value:
                  taxons:
                  - "9606"
                  - house mouse
  /taxonomy/taxon/{tax_id}/related_ids:
    get:
      summary: Get child nodes, and optionally parent nodes, for a given taxon by Taxonomy ID
      description: 'Get child nodes, and optionally parent nodes, in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_related_ids
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
                $ref: '#/components/schemas/v2TaxonomyTaxIdsPage'
      parameters:
      - name: tax_id
        in: path
        required: true
        schema:
          type: integer
        examples:
          example-0:
            value: 9606
            summary: NCBI Taxonomy ID for human
      - name: include_lineage
        description: 'If true, include parent nodes'
        in: query
        required: false
        schema:
          type: boolean
          default: false
      - name: ranks
        description: 'Limit to taxonomic nodes from the given ranks.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRankType'
      - name: page_size
        description: 'The maximum number of taxids to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        in: query
        required: false
        schema:
          type: integer
          default: 20
      - name: page_token
        description: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        in: query
        required: false
        schema:
          type: string
  /taxonomy/related_ids:
    post:
      summary: Get child nodes, and optionally parent nodes, for a given taxon by Taxonomy ID
      description: 'Get child nodes, and optionally parent nodes, in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_related_ids_post
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
                $ref: '#/components/schemas/v2TaxonomyTaxIdsPage'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2TaxonomyRelatedIdRequest'
            examples:
              Single TaxID example:
                description: Child and parent nodes for human
                value:
                  tax_id: 9606
                  include_lineage: true
  /taxonomy/taxon/{taxons}/filtered_subtree:
    get:
      summary: Get a filtered taxonomic subtree by taxon
      description: 'Get a filtered taxonomic subtree, including parent and child nodes, in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_filtered_subtree
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
                $ref: '#/components/schemas/v2TaxonomyFilteredSubtreeResponse'
      parameters:
      - name: taxons
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy ID for human
          example-1:
            value:
              - human
              - mouse
            summary: Common Name
          example-2:
            value:
              - Homo sapiens
              - Mus musculus
            summary: Scientific Name
      - name: specified_limit
        description: 'Limit to specified species'
        in: query
        required: false
        schema:
          type: boolean
      - name: exclude_extinct
        description: 'Limit to species that are not extinct'
        in: query
        required: false
        schema:
          type: boolean
      - name: levels
        description: 'Number of taxonomic levels to return with a valid range of 1-5. Values above 5 will return 5 levels and if level is 0 or not specified, 1 level will be returned.'
        in: query
        required: false
        schema:
          type: integer
      - name: rank_limits
        description: 'Limit to the provided ranks.  If empty, accept any rank.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRankType'
      - name: include_incertae_sedis
        description: 'Include nodes with ranks not in ''rank_limits'' if their names meet criteria for incertae sedis (of unknown origin).'
        in: query
        required: false
        schema:
          type: boolean
  /taxonomy/filtered_subtree:
    post:
      summary: Get a filtered taxonomic subtree by taxon
      description: 'Get a filtered taxonomic subtree, including parent and child nodes, in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_filtered_subtree_post
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
                $ref: '#/components/schemas/v2TaxonomyFilteredSubtreeResponse'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2TaxonomyFilteredSubtreeRequest'
            examples:
              Multiple TaxID example:
                description: NCBI Taxonomy IDs for human and mouse
                value:
                  taxons:
                  - "9606"
                  - "10090"
  /taxonomy/taxon_suggest/{taxon_query}:
    get:
      summary: Get a list of taxonomy names and IDs by partial taxonomic name
      description: 'Get a list of taxonomy names and IDs in JSON format.'
      tags:
        - Taxonomy
      operationId: tax_name_query
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
                $ref: '#/components/schemas/v2SciNameAndIds'
      parameters:
      - name: taxon_query
        description: 'Taxonomy ID or name (common or scientific) at any taxonomic rank'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: hum
            summary: Partial taxonomic name for human
      - name: tax_rank_filter
        description: 'Optionally return results for taxonomic ranks above species using `higher_taxon`'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2OrganismQueryRequestTaxRankFilter'
          default: species
      - name: taxon_resource_filter
        description: 'Limit to taxonomy nodes with gene, genome or organelle data'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2OrganismQueryRequestTaxonResourceFilter'
          default: TAXON_RESOURCE_FILTER_ALL
        examples:
          example-0:
            value: TAXON_RESOURCE_FILTER_GENOME
            summary: Limit to nodes with genome data
          example-1:
            value: TAXON_RESOURCE_FILTER_GENE
            summary: Limit to nodes with gene data
      - name: exact_match
        description: 'If true, only return results that exactly match the provided taxonomic name'
        in: query
        required: false
        schema:
          type: boolean
          default: false
  /taxonomy/taxon_suggest:
    post:
      summary: Get a list of taxonomy names and IDs by partial taxonomic name
      description: 'Get a list of taxonomy names and IDs in JSON format.'
      tags:
        - Taxonomy
      operationId: tax_name_query_by_post
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
                $ref: '#/components/schemas/v2SciNameAndIds'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2OrganismQueryRequest'
            examples:
              Incomplete taxon name example:
                description: Partial taxonomic name for human
                value:
                  taxon_query: "hum"
  /taxonomy/taxon/{taxon}/links:
    get:
      summary: Get external links by Taxonomy ID
      description: 'Get external links associated with a given taxon in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_links
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
                $ref: '#/components/schemas/v2TaxonomyLinksResponse'
      parameters:
      - name: taxon
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy ID for human
  /taxonomy/links:
    post:
      summary: Get external links by Taxonomy ID
      description: 'Get external links associated with a given taxon in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_links_by_post
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
                $ref: '#/components/schemas/v2TaxonomyLinksResponse'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2TaxonomyLinksRequest'
            examples:
              Single TaxID example:
                description: NCBI Taxonomy ID for human
                value:
                  taxon: "9606"
  /taxonomy/taxon/{taxon}/image:
    get:
      summary: Get a taxonomy image by taxon
      description: 'Get an image associated with the specified taxon. By default, in JPEG format, but also available in PNG (accept: image/png), TIFF (accept: accept: image/tiff), and SVG+XML (accept: image/svg+xml) formats.'
      tags:
        - Taxonomy
      operationId: taxonomy_image
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
            image/jpeg:
              schema:
                format: binary
                type: string
                description: Image stream (jpeg/png/tiff)
            image/png:
              schema:
                format: binary
                type: string
                description: Image stream (jpeg/png/tiff)
            image/gif:
              schema:
                $ref: '#/components/schemas/v2HttpBody'
            image/tiff:
              schema:
                format: binary
                type: string
                description: Image stream (jpeg/png/tiff)
            image/svg+xml:
              schema:
                format: binary
                type: string
                description: SVG Image stream
      parameters:
      - name: taxon
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy ID for human
      - name: image_size
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2ImageSize'
  /taxonomy/image:
    post:
      summary: Get a taxonomy image by taxon
      description: 'Get an image associated with the specified taxon. By default, in JPEG format, but also available in PNG (accept: image/png), TIFF (accept: accept: image/tiff), and SVG+XML (accept: image/svg+xml) formats.'
      tags:
        - Taxonomy
      operationId: taxonomy_image_post
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
            image/jpeg:
              schema:
                format: binary
                type: string
                description: Image stream (jpeg/png/tiff)
            image/png:
              schema:
                format: binary
                type: string
                description: Image stream (jpeg/png/tiff)
            image/tiff:
              schema:
                format: binary
                type: string
                description: Image stream (jpeg/png/tiff)
            image/svg+xml:
              schema:
                format: binary
                type: string
                description: SVG Image stream
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2TaxonomyImageRequest'
            examples:
              Single TaxID example:
                description: NCBI Taxonomy ID for human
                value:
                  taxon: "9606"
  /taxonomy/taxon/{taxon}/image/metadata:
    get:
      summary: Get taxonomy image metadata by Taxonomy ID
      description: 'Get taxonomy image metadata, including the image URL and license information, in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_image_metadata
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
                $ref: '#/components/schemas/v2TaxonomyImageMetadataResponse'
      parameters:
      - name: taxon
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy ID for human
  /taxonomy/image/metadata:
    post:
      summary: Get taxonomy image metadata by Taxonomy ID
      description: 'Get taxonomy image metadata, including the image URL and license information, in JSON format.'
      tags:
        - Taxonomy
      operationId: taxonomy_image_metadata_post
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
                $ref: '#/components/schemas/v2TaxonomyImageMetadataResponse'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2TaxonomyImageMetadataRequest'
            examples:
              Single TaxID example:
                description: NCBI Taxonomy ID for human
                value:
                  taxon: "9606"
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
