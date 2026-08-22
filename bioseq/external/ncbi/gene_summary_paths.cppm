/**
 * @file external/ncbi/gene_summary_paths.cppm
 * @brief NCBI gene summary and link endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:gene_summary_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Gene download-summary, count, ortholog, link, and chromosome paths. */
inline constexpr std::string_view gene_summary_paths =
    R"bioseq_openapi(  /gene/id/{gene_ids}/download_summary:
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
