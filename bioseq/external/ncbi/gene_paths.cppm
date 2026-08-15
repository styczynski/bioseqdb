/**
 * @file external/ncbi/gene_paths.cppm
 * @brief NCBI gene endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:gene_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Gene identifier and symbol download path entries. */
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

/** @brief Gene report and ortholog path entries. */
inline constexpr std::string_view gene_report_paths = R"bioseq_openapi(  /gene/id/{gene_ids}:
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
  /gene/id/{gene_ids}/dataset_report:
    get:
      summary: Get a gene data report by GeneID
      description: 'Get a gene data report by GeneID. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_dataset_reports_by_id
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
  /gene/accession/{accessions}/dataset_report:
    get:
      summary: Get a gene data report by RefSeq nucleotide or protein accession
      description: 'Get a gene data report by RefSeq nucleotide or protein accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_dataset_report_by_accession
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
  /gene/symbol/{symbols}/taxon/{taxon}/dataset_report:
    get:
      summary: Get a gene data report by symbol and taxon
      description: 'Get a gene data report by gene symbol and taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_dataset_report_by_tax_and_symbol
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
      - name: tax_search_subtree
        description: 'If true, include genes from taxonomic ranks below the requested taxon.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
  /gene/taxon/{taxon}/dataset_report:
    get:
      summary: Get a gene data report by taxon
      description: 'Get a gene data report by taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_dataset_reports_by_taxon
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
      - name: tax_search_subtree
        description: 'If true, include genes from taxonomic ranks below the requested taxon.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
  /gene/locus_tag/{locus_tags}/dataset_report:
    get:
      summary: Get a gene data report by locus tag
      description: 'Get a gene data report by gene locus tag. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_dataset_reports_by_locus_tag
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
      - name: locus_tags
        description: 'Gene locus tags'
        in: path
        required: true
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
      - name: sort.field
        in: query
        required: false
        schema:
          type: string
  /gene/dataset_report:
    post:
      summary: Get a gene data report
      description: 'Get a gene data report. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_dataset_report
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
  /gene/id/{gene_ids}/product_report:
    get:
      summary: Get a gene product report by GeneID
      description: 'Get a gene product report by GeneID. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_product_reports_by_id
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
  /gene/accession/{accessions}/product_report:
    get:
      summary: Get a gene product report by RefSeq nucleotide or protein accession
      description: 'Get a gene product report by RefSeq nucleotide or protein accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_product_report_by_accession
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
  /gene/symbol/{symbols}/taxon/{taxon}/product_report:
    get:
      summary: Get a gene product report by symbol and taxon
      description: 'Get a gene product report by symbol and taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_product_report_by_tax_and_symbol
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
      - name: tax_search_subtree
        description: 'If true, include genes from taxonomic ranks below the requested taxon.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
  /gene/taxon/{taxon}/product_report:
    get:
      summary: Get a gene product report by taxon
      description: 'Get a gene product report by taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_product_reports_by_taxon
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
      - name: tax_search_subtree
        description: 'If true, include genes from taxonomic ranks below the requested taxon.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
  /gene/locus_tag/{locus_tags}/product_report:
    get:
      summary: Get a gene product report by locus tag
      description: 'Get a gene product report by gene locus tag. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_product_reports_by_locus_tags
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
      - name: locus_tags
        description: 'Gene locus tags'
        in: path
        required: true
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
      - name: table_format
        description: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: summary
            summary: This template, valid for both gene report types, specifies a basic set of fields available from the report
          example-1:
            value: gene-ontology
            summary: This template, only valid for the gene data report, specifies fields that describe gene ontology information
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
      - name: sort.field
        in: query
        required: false
        schema:
          type: string
  /gene/product_report:
    post:
      summary: Get a gene product report
      description: 'Get a gene product report. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Gene
      operationId: gene_product_report
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
  /gene/id/{gene_ids}/download_summary:
    get:
      summary: Get a download summary of a gene data package by GeneID
      description: 'Get a download summary of a gene data package, including counts and estimated package size, in JSON format.'
      tags:
        - Gene
      operationId: gene_download_summary_by_id
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
                $ref: '#/components/schemas/v2DownloadSummary'
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
      - name: returned_content
        description: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2GeneDatasetRequestContentType'
        examples:
          example-0:
            value: IDS_ONLY
            summary: Return abbreviated report with GeneIDs
          example-1:
            value: COUNTS_ONLY
            summary: Return abbreviated report with GeneIDs, transcript and protein counts
      - name: fasta_filter
        description: 'Limit the FASTA sequences in the datasets package to these transcript and protein accessions (deprecated)'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
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
  /gene/download_summary:
    post:
      summary: Get a download summary of a gene data package
      description: 'Get a download summary of a gene data package, including counts and estimated package size, in JSON format.'
      tags:
        - Gene
      operationId: gene_download_summary_by_post
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
                $ref: '#/components/schemas/v2DownloadSummary'
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
  /gene/taxon/{taxon}/counts:
    get:
      summary: Get gene counts by taxon
      description: 'Get gene counts by taxon in JSON format.'
      tags:
        - Gene
      operationId: gene_counts_for_taxon
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
                $ref: '#/components/schemas/v2GeneCountsByTaxonReply'
      parameters:
      - name: taxon
        description: 'Taxon for provided gene symbol'
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
            summary: Common name
          example-2:
            value: Homo sapiens
            summary: Scientific name
  /gene/taxon/counts:
    post:
      summary: Get gene counts by taxon
      description: 'Get gene counts by taxon in JSON format.'
      tags:
        - Gene
      operationId: gene_counts_for_taxon_by_post
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
                $ref: '#/components/schemas/v2GeneCountsByTaxonReply'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2GeneCountsByTaxonRequest'
            examples:
              Single TaxID example:
                description: NCBI Taxonomy ID for human
                value:
                  taxon: "9606"
  /gene/id/{gene_id}/orthologs:
    get:
      summary: Get a gene data report for a gene ortholog set by GeneID
      description: 'Get a gene data report for a gene ortholog set by GeneID in JSON format.'
      tags:
        - Gene
      operationId: gene_orthologs_by_id
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
      parameters:
      - name: gene_id
        description: 'A single NCBI GeneID'
        in: path
        required: true
        schema:
          type: integer
        examples:
          example-0:
            value: 2778
            summary: Human GNAS, GNAS complex locus
          example-1:
            value: 2
            summary: Human A2M, alpha-2-macroglobulin
      - name: returned_content
        description: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2OrthologRequestContentType'
        examples:
          example-0:
            value: IDS_ONLY
            summary: Return abbreviated report with GeneIDs
          example-1:
            value: COUNTS_ONLY
            summary: Return abbreviated report with GeneIDs, transcript and protein counts
      - name: taxon_filter
        description: 'Limit to genes from the specified NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - "9606"
              - "10090"
            summary: NCBI Taxonomy IDs for human and mouse
          example-1:
            value: house mouse
            summary: Common name
          example-2:
            value: Homo sapiens
            summary: Scientific name
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
  /gene/orthologs:
    post:
      summary: Get a gene data report for a gene ortholog set by GeneID
      description: 'Get a gene data report for a gene ortholog set by GeneID in JSON format.'
      tags:
        - Gene
      operationId: gene_orthologs_by_post
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
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2OrthologRequest'
            examples:
              Single GeneID example:
                description: Primate gene orthologs of the human gene, GNAS complex locus
                value:
                  gene_id: 2778
                  taxon_filter:
                   - primates
  /gene/id/{gene_ids}/links:
    get:
      summary: Get gene links by GeneID
      description: 'Get links to available gene resources in JSON format.'
      tags:
        - Gene
      operationId: gene_links_by_id
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
                $ref: '#/components/schemas/v2GeneLinksReply'
      parameters:
      - name: gene_ids
        description: 'One or more NCBI GeneIDs, limited to 100'
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
            summary: Human IL21 and ILR21R, interleukin 21 and interleukin 21 receptor
  /gene/links:
    post:
      summary: Get gene links by GeneID
      description: 'Get links to available gene resources in JSON format.'
      tags:
        - Gene
      operationId: gene_links_by_id_by_post
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
                $ref: '#/components/schemas/v2GeneLinksReply'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2GeneLinksRequest'
            examples:
              Multiple GeneID examples:
                description: Human interleukin 21 and interleukin 21 receptor genes
                value:
                  gene_ids:
                  - 59067
                  - 50615
  /gene/taxon/{taxon}/annotation/{annotation_name}/chromosome_summary:
    get:
      summary: Get gene counts per chromosome by taxon and annotation name
      description: 'Get gene counts per chromosome by taxon and annotation name in JSON format.'
      tags:
        - Gene
      operationId: gene_chromosome_summary
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
                $ref: '#/components/schemas/v2GeneChromosomeSummaryReply'
      parameters:
      - name: taxon
        description: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank, with an anntoated genome'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "9117"
            summary: NCBI Taxonomy ID for Grus americana
          example-1:
            value: Whooping crane
            summary: Common Name
          example-2:
            value: Grus americana
            summary: Scientific Name
      - name: annotation_name
        description: 'Annotation name corresponding to the provided taxon'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: GCF_028858705.1-RS_2023_03
            summary: NCBI Grus americana Annotation Release GCF_028858705.1-RS_2023_03
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
