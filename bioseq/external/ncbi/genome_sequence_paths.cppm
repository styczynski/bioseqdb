/**
 * @file external/ncbi/genome_sequence_paths.cppm
 * @brief NCBI assembly lookup and sequence-report requests.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:genome_sequence_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Assembly and sequence reports, external links, and CheckM histograms. */
inline constexpr std::string_view genome_sequence_catalog_paths =
    R"bioseq_openapi(  /genome/dataset_report:
    post:
      summary: Get a genome assembly report
      description: 'Get a genome assembly report. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_dataset_report_by_post
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
                $ref: '#/components/schemas/v2reportsAssemblyDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsAssemblyDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2AssemblyDatasetReportsRequest'
            examples:
              Single GCF accession example:
                description: Human reference genome assembly, GRCh38.p14
                value:
                  accessions:
                    - GCF_000001405.40
  /genome/sequence_accession/{accession}/sequence_assemblies:
    get:
      summary: Get a genome assembly accession for a nucleotide sequence accession
      description: 'Get a genome assembly accession for a nucleotide sequence accession in JSON format.'
      tags:
        - Genome
      operationId: assembly_accessions_for_sequence_accession
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
                $ref: '#/components/schemas/v2AssemblyAccessions'
      parameters:
      - name: accession
        description: 'Nucleotide sequence accession associated with a genome assembly'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: NC_000001.11
            summary: Human chromosome 1 from GRCh38.p14
  /genome/sequence_assemblies:
    post:
      summary: Get a genome assembly accession for a nucleotide sequence accession
      description: 'Get a genome assembly accession for a nucleotide sequence accession in JSON format.'
      tags:
        - Genome
      operationId: assembly_accessions_for_sequence_accession_by_post
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
                $ref: '#/components/schemas/v2AssemblyAccessions'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2SequenceAccessionRequest'
            examples:
              Single chromosome accession example:
                description: Human chromosome 1 from GRCh38.p14
                value:
                  accession: "NC_000001.11"
  /genome/accession/{accession}/sequence_reports:
    get:
      summary: Get a genome sequence report by genome assembly accession
      description: 'Get a genome sequence report by genome assembly accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_sequence_report
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
                $ref: '#/components/schemas/v2SequenceReportPage'
            application/vnd.openxmlformats-officedocument.spreadsheetml.sheet:
              schema:
                $ref: '#/components/schemas/v2SequenceReportPage'
            text/tab-separated-values:
              schema:
                type: string
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2SequenceReportPage'
      parameters:
      - name: accession
        description: 'A single genome assembly accession'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: GCF_000001635.27
            summary: Mouse reference genome assembly, GRCm39
          example-1:
            value: GCF_000001405.40
            summary: Human reference genome assembly, GRCh38.p14
      - name: chromosomes
        description: 'Limit to sequences with the specified chromosome names'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - "1"
              - MT
            summary: Chromosome 1 and mitochondrial genome sequence
          example-1:
            value:
              - X
              - Y
            summary: Human sex chromosomes X and Y
      - name: role_filters
        description: 'Limit to sequences with the specified "role", where possible roles are `assembled-molecule`, `unlocalized-scaffold`, or `unplaced-scaffold`'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: assembled-molecule
            summary: Assembled sequences
      - name: table_fields
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value:
              - accession
              - chr-name
            summary: Genome assembly accession and chromosome name
          example-1:
            value: ucsc-style-name
            summary: Sequence name from the UCSC Genome Browser
      - name: count_assembly_unplaced
        description: 'Include the count of unplaced scaffold sequences'
        in: query
        required: false
        schema:
          type: boolean
          default: false
      - name: page_size
        description: 'The maximum number of genome assemblies to return. Maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        in: query
        required: false
        schema:
          type: integer
      - name: page_token
        description: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        in: query
        required: false
        schema:
          type: string
      - name: include_tabular_header
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /genome/sequence_reports:
    post:
      summary: Get a genome sequence report by genome assembly accession
      description: 'Get a genome sequence report by genome assembly accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_sequence_report_by_post
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
                $ref: '#/components/schemas/v2SequenceReportPage'
            application/vnd.openxmlformats-officedocument.spreadsheetml.sheet:
              schema:
                $ref: '#/components/schemas/v2SequenceReportPage'
            text/tab-separated-values:
              schema:
                type: string
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2SequenceReportPage'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2AssemblySequenceReportsRequest'
            examples:
              Single GCF accession example:
                description: Human reference genome assembly, GRCh38.p14
                value:
                  accession: "GCF_000001405.40"
  /genome/accession/{accessions}/links:
    get:
      summary: Get assembly links by genome assembly accession
      description: 'Get links to assembly resources by genome assembly accession in JSON format.'
      tags:
        - Genome
      operationId: genome_links_by_accession
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
                $ref: '#/components/schemas/v2AssemblyLinksReply'
      parameters:
      - name: accessions
        description: 'One or more genome assembly accessions, limited to 100'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: GCF_000001405.40
            summary: Human reference genome, GRCh38.p14
          example-1:
            value:
              - GCF_000001405.40
              - GCF_000001635.27
            summary: Human and mouse reference genomes, GRCh38.p14 and GRCm39
      - name: assembly_link_types
        description: 'Zero or more assembly link types to retrieve. If no values are provided, all links are retrieved'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2AssemblyLinksReplyAssemblyLinkType'
  /genome/links:
    post:
      summary: Get assembly links by genome assembly accession
      description: 'Get links to assembly resources by genome assembly accession in JSON format.'
      tags:
        - Genome
      operationId: genome_links_by_accession_by_post
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
                $ref: '#/components/schemas/v2AssemblyLinksReply'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2AssemblyLinksRequest'
            examples:
              Single GCF accession example:
                description: Human reference genome assembly, GRCh38.p14
                value:
                  accessions:
                    - GCF_000001405.40
  /genome/taxon/{species_taxon}/checkm_histogram:
    get:
      summary: Get CheckM histogram data by species taxon
      description: 'Get CheckM histogram data by species taxon. This data is used for rendering CheckM histograms on bacterial genome pages.'
      tags:
        - Genome
      operationId: checkm_histogram_by_taxon
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
                $ref: '#/components/schemas/v2AssemblyCheckMHistogramReply'
      parameters:
      - name: species_taxon
        description: 'Bacterial species taxonomy ID or taxonomic name'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "202956"
            summary: Acinetobacter towneri
  /genome/checkm_histogram:
    post:
      summary: Get CheckM histogram data by species taxon
      description: 'Get CheckM histogram data by species taxon. This data is used for rendering CheckM histograms on bacterial genome pages.'
      tags:
        - Genome
      operationId: checkm_histogram_by_taxon_by_post
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
                $ref: '#/components/schemas/v2AssemblyCheckMHistogramReply'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2AssemblyCheckMHistogramRequest'
            examples:
              Single TaxID example:
                description: Acinetobacter towneri (taxid 202956)
                value:
                  species_taxon: "202956"
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
