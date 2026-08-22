/**
 * @file external/ncbi/gene_identity_paths.cppm
 * @brief NCBI gene identity report endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:gene_identity_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Deprecated gene-report paths keyed by GeneID, accession, symbol, or taxon. */
inline constexpr std::string_view gene_identity_paths =
    R"bioseq_openapi(  /gene/id/{gene_ids}:
    get:
      deprecated: true
      summary: Get gene reports by GeneID
      description: 'Get a gene summary by GeneID. By default, in paged JSON format, but also available as tabular (accept: text/tab-separated-values) or json-lines (accept: application/x-ndjson)'
      tags:
        - Gene
      operationId: gene_reports_by_id
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
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: returned_content
        description: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2GeneDatasetReportsRequestContentType'
        examples:
          example-0:
            value: IDS_ONLY
            summary: Return abbreviated report with GeneIDs
          example-1:
            value: COUNTS_ONLY
            summary: Return abbreviated report with GeneIDs, transcript and protein counts
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
      - name: locus_tags
        description: 'Gene locus tags'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: b0001
            summary: E. coli thrL, thr operon leader peptide
          example-1:
            value:
              - YER095W
              - YML032C
            summary: Saccharomyces cerevisiae recombinases, RAD51 and RAD52
      - name: table_fields
        description: 'Specify which fields to include in the tabular report. Additional fields are described here: [Create a table from the gene data reports](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/command-line-tools/using-dataformat/gene-data-reports/). Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - gene-id
              - gene-type
              - description
            summary: GeneID, type and description
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
      - name: page_size
        description: 'The maximum number of gene reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: accession_filter
        description: 'Filter tabular product report to only include the selected accessions. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - NM_001407959.1
              - NM_001408458.1
            summary: Human BRCA1 transcripts, encoding isoforms 78 and 116, respectively
      - name: tax_search_subtree
        description: 'If true, include genes from taxonomic ranks below the requested taxon.'
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
  /gene/accession/{accessions}:
    get:
      deprecated: true
      summary: Get gene metadata by RefSeq Accession
      description: 'Get a gene summary by RefSeq Accession. By default, in paged JSON format, but also available as tabular (accept: text/tab-separated-values) or json-lines (accept: application/x-ndjson)'
      tags:
        - Gene
      operationId: gene_metadata_by_accession
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
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: returned_content
        description: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2GeneDatasetReportsRequestContentType'
        examples:
          example-0:
            value: IDS_ONLY
            summary: Return abbreviated report with GeneIDs
          example-1:
            value: COUNTS_ONLY
            summary: Return abbreviated report with GeneIDs, transcript and protein counts
      - name: accessions
        description: 'One or more RefSeq nucleotide or protein accessions'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NM_021803.4
            summary: Human interleukin 21 mRNA
          example-1:
            value:
              - NP_068575.1
              - NP_851564.1
            summary: Human interleukin 21 and interleukin 21 receptor proteins
      - name: locus_tags
        description: 'Gene locus tags'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: b0001
            summary: E. coli thrL, thr operon leader peptide
          example-1:
            value:
              - YER095W
              - YML032C
            summary: Saccharomyces cerevisiae recombinases, RAD51 and RAD52
      - name: table_fields
        description: 'Specify which fields to include in the tabular report. Additional fields are described here: [Create a table from the gene data reports](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/command-line-tools/using-dataformat/gene-data-reports/). Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - gene-id
              - gene-type
              - description
            summary: GeneID, type and description
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
      - name: page_size
        description: 'The maximum number of gene reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: accession_filter
        description: 'Filter tabular product report to only include the selected accessions. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - NM_001407959.1
              - NM_001408458.1
            summary: Human BRCA1 transcripts, encoding isoforms 78 and 116, respectively
      - name: tax_search_subtree
        description: 'If true, include genes from taxonomic ranks below the requested taxon.'
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
  /gene/symbol/{symbols}/taxon/{taxon}:
    get:
      deprecated: true
      summary: Get gene metadata by gene symbol
      description: 'Get a gene summary by by gene symbol. By default, in paged JSON format, but also available as tabular (accept: text/tab-separated-values) or json-lines (accept: application/x-ndjson)'
      tags:
        - Gene
      operationId: gene_metadata_by_tax_and_symbol
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
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: returned_content
        description: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2GeneDatasetReportsRequestContentType'
        examples:
          example-0:
            value: IDS_ONLY
            summary: Return abbreviated report with GeneIDs
          example-1:
            value: COUNTS_ONLY
            summary: Return abbreviated report with GeneIDs, transcript and protein counts
      - name: symbols
        description: 'One or more gene symbols'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: GNAS
            summary: Human GNAS, GNAS complex locus
      - name: taxon
        description: 'NCBI Taxonomy ID or name (common or scientific, any taxonomic rank) for the provided gene symbol'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy ID for human
          example-1:
            value: human
            summary: Common Name
          example-2:
            value: Homo sapiens
            summary: Scientific Name
      - name: locus_tags
        description: 'Gene locus tags'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: b0001
            summary: E. coli thrL, thr operon leader peptide
          example-1:
            value:
              - YER095W
              - YML032C
            summary: Saccharomyces cerevisiae recombinases, RAD51 and RAD52
      - name: table_fields
        description: 'Specify which fields to include in the tabular report. Additional fields are described here: [Create a table from the gene data reports](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/command-line-tools/using-dataformat/gene-data-reports/). Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - gene-id
              - gene-type
              - description
            summary: GeneID, type and description
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
      - name: page_size
        description: 'The maximum number of gene reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: accession_filter
        description: 'Filter tabular product report to only include the selected accessions. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - NM_001407959.1
              - NM_001408458.1
            summary: Human BRCA1 transcripts, encoding isoforms 78 and 116, respectively
      - name: tax_search_subtree
        description: 'If true, include genes from taxonomic ranks below the requested taxon.'
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
  /gene/taxon/{taxon}:
    get:
      deprecated: true
      summary: Get gene reports by taxonomic identifier
      description: 'Get a gene summary for a specified NCBI Taxonomy ID or name (common or scientific). By default, in paged JSON format, but also available as tabular (accept: text/tab-separated-values) or json-lines (accept: application/x-ndjson)'
      tags:
        - Gene
      operationId: gene_reports_by_taxon
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
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: returned_content
        description: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2GeneDatasetReportsRequestContentType'
        examples:
          example-0:
            value: IDS_ONLY
            summary: Return abbreviated report with GeneIDs
          example-1:
            value: COUNTS_ONLY
            summary: Return abbreviated report with GeneIDs, transcript and protein counts
      - name: taxon
        description: 'NCBI Taxonomy ID or name (common or scientific) that the genes are annotated at'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "9606"
            summary: NCBI Taxonomy Identifier
          example-1:
            value: human
            summary: Common Name
          example-2:
            value: Homo sapiens
            summary: Scientific Name
      - name: locus_tags
        description: 'Gene locus tags'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: b0001
            summary: E. coli thrL, thr operon leader peptide
          example-1:
            value:
              - YER095W
              - YML032C
            summary: Saccharomyces cerevisiae recombinases, RAD51 and RAD52
      - name: table_fields
        description: 'Specify which fields to include in the tabular report. Additional fields are described here: [Create a table from the gene data reports](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/command-line-tools/using-dataformat/gene-data-reports/). Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - gene-id
              - gene-type
              - description
            summary: GeneID, type and description
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
      - name: page_size
        description: 'The maximum number of gene reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: query
        description: 'Limit to genes that match the specified gene symbol, name (description), alias, locus tag or protein name.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: A2M immunoglobulin
            summary: Gene name or description
      - name: types
        description: 'Limit to genes matching the specified gene type.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneType'
        examples:
          example-0:
            value: PROTEIN_CODING
            summary: Protein coding genes
          example-1:
            value: ncRNA
            summary: Non-coding RNA genes, including microRNAs, long non-coding RNAs and other types of non-coding RNA genes
          example-2:
            value: PSEUDO
            summary: Pseudogenes
      - name: accession_filter
        description: 'Filter tabular product report to only include the selected accessions. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - NM_001407959.1
              - NM_001408458.1
            summary: Human BRCA1 transcripts, encoding isoforms 78 and 116, respectively
      - name: tax_search_subtree
        description: 'If true, include genes from taxonomic ranks below the requested taxon.'
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
  /gene:
    post:
      deprecated: true
      summary: Get gene metadata as JSON
      description: 'Get a gene summary. By default, in paged JSON format, but also available as tabular (accept: text/tab-separated-values) or json-lines (accept: application/x-ndjson)'
      tags:
        - Gene
      operationId: gene_metadata_by_post
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
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsGeneDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2GeneDatasetReportsRequest'
            examples:
              Multiple GeneID examples:
                description: Human interleukin 21 and interleukin 21 receptor genes
                value:
                  gene_ids:
                  - 59067
                  - 50615
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
