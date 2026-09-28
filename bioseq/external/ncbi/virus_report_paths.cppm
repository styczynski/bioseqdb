/**
 * @file external/ncbi/virus_report_paths.cppm
 * @brief NCBI virus reports and accession checks.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:virus_report_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Virus data and annotation reports, plus accession-validity checks. */
inline constexpr std::string_view virus_report_paths =
    R"bioseq_openapi(  /virus/taxon/{taxon}/dataset_report:
    get:
      summary: Get a virus data report by taxon
      description: 'Get a virus data report by taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Virus
      operationId: virus_reports_by_taxon
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
                $ref: '#/components/schemas/v2reportsVirusDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsVirusDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: taxon
        description: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "1335626"
            summary: NCBI Taxonomy ID for MERS, Middle East respiratory syndrome-related coronavirus
          example-1:
            value: "2697049"
            summary: NCBI Taxonomy ID for SARS-CoV-2
          example-2:
            value: "197911"
            summary: NCBI Taxonomy ID for the genus Alphainfluenzavirus
      - name: filter.refseq_only
        description: 'If true, limit results to RefSeq genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to RefSeq genomes
          example-1:
            value: false
            summary: Include both RefSeq and GenBank genomes
      - name: filter.annotated_only
        description: 'If true, limit results to annotated genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to annotated genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filter.released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filter.updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filter.host
        description: 'Limit to genomes isolated from the specified host species (NCBI Taxonomy ID, common or scientific name).'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: 9606
            summary: NCBI Taxonomy ID for human
          example-1:
            value: Felis catus
            summary: Scientific name for domestic cat
      - name: filter.pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: filter.geo_location
        description: 'Limit to genomes collected from the specified geographic location.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: USA
            summary: USA
          example-1:
            value: Asia
            summary: Asia
      - name: filter.usa_state
        description: 'Limit to genomes collected from the specified U.S. state (two-letter abbreviation).'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: CA
            summary: California
          example-1:
            value: TX
            summary: Texas
      - name: filter.complete_only
        description: 'Limit to genomes designated as complete, as defined by the submitter.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to complete genomes
          example-1:
            value: false
            summary: All genomes
      - name: returned_content
        description: 'Return complete virus reports or nucleotide accessions only'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2VirusDataReportRequestContentType'
      - name: table_fields
        description: 'Specify which fields to include in the tabular report'
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
              - is-complete
              - is-annotated
            summary: Virus Data Report Fields
      - name: page_size
        description: 'The maximum number of virus data reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
  /virus/accession/{accessions}/dataset_report:
    get:
      summary: Get a virus data report by nucleotide accession
      description: 'Get a virus data report by nucleotide accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Virus
      operationId: virus_reports_by_acessions
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
                $ref: '#/components/schemas/v2reportsVirusDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsVirusDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: accessions
        description: 'One or more nucleotide sequence accessions'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NC_038294.1
            summary: Betacoronavirus England 1 isolate H123990006, complete genome
      - name: filter.refseq_only
        description: 'If true, limit results to RefSeq genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to RefSeq genomes
          example-1:
            value: false
            summary: Include both RefSeq and GenBank genomes
      - name: filter.annotated_only
        description: 'If true, limit results to annotated genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to annotated genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filter.released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filter.updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filter.host
        description: 'Limit to genomes isolated from the specified host species (NCBI Taxonomy ID, common or scientific name).'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: 9606
            summary: NCBI Taxonomy ID for human
          example-1:
            value: Felis catus
            summary: Scientific name for domestic cat
      - name: filter.pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: filter.geo_location
        description: 'Limit to genomes collected from the specified geographic location.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: USA
            summary: USA
          example-1:
            value: Asia
            summary: Asia
      - name: filter.usa_state
        description: 'Limit to genomes collected from the specified U.S. state (two-letter abbreviation).'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: CA
            summary: California
          example-1:
            value: TX
            summary: Texas
      - name: filter.complete_only
        description: 'Limit to genomes designated as complete, as defined by the submitter.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to complete genomes
          example-1:
            value: false
            summary: All genomes
      - name: returned_content
        description: 'Return complete virus reports or nucleotide accessions only'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2VirusDataReportRequestContentType'
      - name: table_fields
        description: 'Specify which fields to include in the tabular report'
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
              - is-complete
              - is-annotated
            summary: Virus Data Report Fields
      - name: page_size
        description: 'The maximum number of virus data reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
  /virus:
    post:
      summary: Get a virus data report
      description: 'Get a virus data report. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Virus
      operationId: virus_reports_by_post
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
                $ref: '#/components/schemas/v2reportsVirusDataReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsVirusDataReportPage'
            text/tab-separated-values:
              schema:
                type: string
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2VirusDataReportRequest'
            examples:
              RefSeq Virus TaxID example:
                description: Middle East repiratory syndrome-related coronavirus, RefSeq genomes
                value:
                  filter: 
                   taxons:
                   - "1335626"
                   refseq_only: true
  /virus/taxon/{taxon}/annotation_report:
    get:
      summary: Get a virus annotation report by taxon
      description: 'Get virus annotation report by taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Virus
      operationId: virus_annotation_reports_by_taxon
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
                $ref: '#/components/schemas/v2reportsVirusAnnotationReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsVirusAnnotationReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: taxon
        description: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: "1335626"
            summary: MERS
          example-1:
            value: "2697049"
            summary: SARS-COV-2
          example-2:
            value: "11118"
            summary: Coronaviridae
      - name: filter.refseq_only
        description: 'If true, limit to RefSeq genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to RefSeq genomes
          example-1:
            value: false
            summary: Include both RefSeq and GenBank genomes
      - name: filter.annotated_only
        description: 'If true, limit to annotated genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to annotated genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filter.released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filter.updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filter.host
        description: 'Limit to genomes isolated from the specified host species (NCBI Taxonomy ID, common or scientific name).'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: 9606
            summary: NCBI Taxonomy ID for human
          example-1:
            value: Felis catus
            summary: Scientific name for domestic cat
      - name: filter.pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: filter.geo_location
        description: 'Limit to genomes collected from the specififed geographic location.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: USA
            summary: USA
          example-1:
            value: Asia
            summary: Asia
      - name: filter.usa_state
        description: 'Limit to genomes collected from the specified U.S. state (two-letter abbreviation).'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: CA
            summary: California
          example-1:
            value: TX
            summary: Texas
      - name: filter.complete_only
        description: 'Limit to genomes designated as complete, as defined by the submitter.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to complete genomes
          example-1:
            value: false
            summary: All genomes
      - name: table_fields
        description: 'Specify which fields to include in the tabular report. Additional fields are described here: [virus annotation report fields](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/command-line-tools/using-dataformat/virus-data-reports/#virus-annotation-report-fields). Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
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
              - gene-cds-name
              - gene-cds-nuc-fasta-range-start
              - gene-cds-nuc-fasta-range-stop
            summary: Virus genome nucleotide accession, gene CDS name, and annotated location
      - name: page_size
        description: 'The maximum number of virus data reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
  /virus/accession/{accessions}/annotation_report:
    get:
      summary: Get a virus annotation report by nucleotide accession
      description: 'Get a virus annotation report by nucleotide accesion. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Virus
      operationId: virus_annotation_reports_by_acessions
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
                $ref: '#/components/schemas/v2reportsVirusAnnotationReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsVirusAnnotationReportPage'
            text/tab-separated-values:
              schema:
                type: string
      parameters:
      - name: accessions
        description: 'One or more nucleotide sequence accessions'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NC_038294.1
            summary: Betacoronavirus England 1 isolate H123990006, complete genome
      - name: filter.refseq_only
        description: 'If true, limit to RefSeq genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to RefSeq genomes
          example-1:
            value: false
            summary: Include both RefSeq and GenBank genomes
      - name: filter.annotated_only
        description: 'If true, limit to annotated genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to annotated genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filter.released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filter.updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filter.host
        description: 'Limit to genomes isolated from the specified host species (NCBI Taxonomy ID, common or scientific name).'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: 9606
            summary: NCBI Taxonomy ID for human
          example-1:
            value: Felis catus
            summary: Scientific name for domestic cat
      - name: filter.pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: filter.geo_location
        description: 'Limit to genomes collected from the specififed geographic location.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: USA
            summary: USA
          example-1:
            value: Asia
            summary: Asia
      - name: filter.usa_state
        description: 'Limit to genomes collected from the specified U.S. state (two-letter abbreviation).'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: CA
            summary: California
          example-1:
            value: TX
            summary: Texas
      - name: filter.complete_only
        description: 'Limit to genomes designated as complete, as defined by the submitter.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to complete genomes
          example-1:
            value: false
            summary: All genomes
      - name: table_fields
        description: 'Specify which fields to include in the tabular report. Additional fields are described here: [virus annotation report fields](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/command-line-tools/using-dataformat/virus-data-reports/#virus-annotation-report-fields). Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
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
              - gene-cds-name
              - gene-cds-nuc-fasta-range-start
              - gene-cds-nuc-fasta-range-stop
            summary: Virus genome nucleotide accession, gene CDS name, and annotated location
      - name: page_size
        description: 'The maximum number of virus data reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
  /virus/annotation_report:
    post:
      summary: Get a virus annotation report
      description: 'Get a virus annotation report. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Virus
      operationId: virus_annotation_reports_by_post
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
                $ref: '#/components/schemas/v2reportsVirusAnnotationReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsVirusAnnotationReportPage'
            text/tab-separated-values:
              schema:
                type: string
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2VirusAnnotationReportRequest'
            examples:
              RefSeq Virus TaxID example:
                description: Middle East repiratory syndrome-related coronavirus, RefSeq genomes
                value:
                  filter: 
                   taxons:
                   - "1335626"
                   refseq_only: true
  /virus/accession/{accessions}/check:
    get:
      summary: Check the validity of a virus genome nucleotide accession
      description: 'Check the validity of a virus genome nucleotide accession. Output in JSON format.'
      tags:
        - Virus
      operationId: virus_accession_availability
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
                $ref: '#/components/schemas/v2VirusAvailability'
      parameters:
      - name: accessions
        description: 'virus accessions'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NC_038294.1
            summary: Betacoronavirus England 1 isolate H123990006, complete genome
  /virus/check:
    post:
      summary: Check the validity of a virus genome nucleotide accession
      description: 'Check the validity of a virus genome nucleotide accession. Output in JSON format.'
      tags:
        - Virus
      operationId: virus_accession_availability_post
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
                $ref: '#/components/schemas/v2VirusAvailability'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2VirusAvailabilityRequest'
            examples:
              Single Virus accession example:
                description: Betacoronavirus England 1 isolate H123990006, complete genome
                value:
                  accessions:
                    - NC_038294.1
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
