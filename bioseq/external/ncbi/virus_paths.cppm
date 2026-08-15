/**
 * @file external/ncbi/virus_paths.cppm
 * @brief NCBI virus endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:virus_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Virus genome, protein, and report path entries. */
inline constexpr std::string_view virus_report_paths =
    R"bioseq_openapi(  /virus/taxon/{taxon}/genome:
    get:
      summary: Get a download summary of a virus genome data package by taxon
      description: 'Get a download summary of a virus genome data package, including counts and file sizes, in JSON format.'
      tags:
        - Virus
      operationId: virus_genome_summary
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
      - name: accessions
        description: 'One or more nucleotide sequence accessions'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NC_038294.1
            summary: Betacoronavirus England 1 isolate H123990006, complete genome
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
      - name: refseq_only
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
      - name: annotated_only
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
            summary: Inclue all genomes
      - name: released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: host
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
      - name: pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: geo_location
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
      - name: usa_state
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
      - name: complete_only
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
      - name: include_sequence
        description: 'Specify which sequence files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2ViralSequenceType'
        examples:
          example-0:
            value:
              - GENOME
              - CDS
              - PROTEIN
            summary: Include genomic, CDS, and protein sequence files
      - name: aux_report
        description: 'Specify which report files to include in the data package. The virus data report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusDatasetReportType'
        examples:
          example-0:
            value: BIOSAMPLE
            summary: Include the BioSample report
  /virus/genome:
    post:
      summary: Get a download summary of a virus genome data package
      description: 'Get a download summary of a virus genome data package, including counts and file sizes, in JSON format.'
      tags:
        - Virus
      operationId: virus_genome_summary_by_post
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
              $ref: '#/components/schemas/v2VirusDatasetRequest'
            examples:
              Single Virus accession example:
                description: Betacoronavirus England 1 isolate H123990006, complete genome
                value:
                  accessions:
                    - NC_038294.1
  /virus/taxon/sars2/protein/{proteins}:
    get:
      summary: Get a download summary of a SARS-CoV-2 protein data package by protein name
      description: 'Get a download summary of a SARS-CoV-2 protein data package, including counts and file sizes, in JSON format.'
      tags:
        - Virus
      operationId: sars2_protein_summary
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
      - name: proteins
        description: 'One or more SARS-CoV-2 protein names'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: spike protein
            summary: SARS-CoV-2 spike protein
          example-1:
            value:
              - spike protein
              - envelope protein
              - RdRp
            summary: SARS-CoV-2 spike, envelope, and RNA-dependent RNA polymerase proteins
      - name: refseq_only
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
            summary: Both GenBank & RefSeq genomes
      - name: annotated_only
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
            summary: All genomes
      - name: released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: host
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
      - name: pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes with the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: geo_location
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
      - name: usa_state
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
      - name: complete_only
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
      - name: include_sequence
        description: 'Specify which sequence files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2ViralSequenceType'
        examples:
          example-0:
            value:
              - CDS
              - PROTEIN
            summary: Include CDS and proetin sequence files
      - name: aux_report
        description: 'Specify which report files to include in the data package. The virus data report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusDatasetReportType'
        examples:
          example-0:
            value: ANNOTATION
            summary: Include the annotation report
  /virus/taxon/sars2/protein:
    post:
      summary: Get a download summary of a SARS-CoV-2 protein data package by protein name
      description: 'Get a download summary of a SARS-CoV-2 protein data package, including counts and file sizes, in JSON format.'
      tags:
        - Virus
      operationId: sars2_protein_summary_by_post
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
              $ref: '#/components/schemas/v2Sars2ProteinDatasetRequest'
            examples:
              SARS-CoV-2 virus RefSeq protein example:
                description: SARS-CoV-2 spike surface glycoprotein (RefSeq)
                value:
                  proteins:
                   - spike
                  refseq_only: true
                  include_sequence:
                   - PROTEIN
  /virus/taxon/{taxon}/genome/table:
    get:
      deprecated: true
      summary: Get virus genome metadata in a tabular format
      description: 'Get virus genome metadata in tabular format for virus genomes by taxon.'
      tags:
        - Virus
      operationId: virus_genome_table
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
                $ref: '#/components/schemas/v2TabularOutput'
      parameters:
      - name: accessions
        description: 'One or more nucleotide sequence accessions'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: NC_038294.1
            summary: Betacoronavirus England 1 isolate H123990006, complete genome
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
      - name: refseq_only
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
      - name: annotated_only
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
            summary: Inclue all genomes
      - name: released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: host
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
      - name: pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: geo_location
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
      - name: usa_state
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
      - name: complete_only
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
        description: 'Specify which fields to include in the tabular report'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusTableField'
        examples:
          example-0:
            value:
              - protein_accession
              - protein_name
            summary: SARS-CoV-2 protein accession and name
      - name: include_sequence
        description: 'Specify which sequence files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2ViralSequenceType'
        examples:
          example-0:
            value:
              - GENOME
              - CDS
              - PROTEIN
            summary: Include genomic, CDS, and protein sequence files
      - name: aux_report
        description: 'Specify which report files to include in the data package. The virus data report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusDatasetReportType'
        examples:
          example-0:
            value: BIOSAMPLE
            summary: Include the BioSample report
      - name: format
        description: 'Choose download format (tsv, csv or jsonl)'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2TableFormat'
          default: tsv
        examples:
          example-0:
            value: tsv
            summary: TSV
          example-1:
            value: csv
            summary: CSV
          example-2:
            value: jsonl
            summary: JSON Lines
  /virus/taxon/sars2/protein/{proteins}/table:
    get:
      summary: Get SARS-CoV-2 protein metadata in a tabular format by protein name
      description: 'Get SARS-CoV-2 protein metadata in a tabular format by protein name.'
      tags:
        - Virus
      operationId: sars2_protein_table
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
                $ref: '#/components/schemas/v2TabularOutput'
      parameters:
      - name: proteins
        description: 'One or more SARS-CoV-2 protein names'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: spike protein
            summary: SARS-CoV-2 spike protein
          example-1:
            value:
              - spike protein
              - envelope protein
              - RdRp
            summary: SARS-CoV-2 spike, envelope, and RNA-dependent RNA polymerase proteins
      - name: refseq_only
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
            summary: Both GenBank & RefSeq genomes
      - name: annotated_only
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
            summary: All genomes
      - name: released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: host
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
      - name: pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes with the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: geo_location
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
      - name: usa_state
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
      - name: complete_only
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
        description: 'Specify which fields to include in the tabular report'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusTableField'
        examples:
          example-0:
            value:
              - protein_accession
              - protein_name
            summary: Protein accession and name
      - name: format
        description: 'Specify output format'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2TableFormat'
          default: tsv
        examples:
          example-0:
            value: tsv
            summary: TSV, tabular format
          example-1:
            value: jsonl
            summary: JSON Lines format
  /virus/taxon/{taxon}/dataset_report:
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
  /virus/taxon/{taxon}/genome/download:
    get:
      summary: Get a virus genome data package by taxon
      description: 'Download a virus genome data package including sequence, annotation, BioSample data and a detailed data report by taxon.'
      tags:
        - Virus
      operationId: virus_genome_download
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
      - name: refseq_only
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
      - name: annotated_only
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
            summary: Inclue all genomes
      - name: released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: host
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
      - name: pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: geo_location
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
      - name: usa_state
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
      - name: complete_only
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
      - name: include_sequence
        description: 'Specify which sequence files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2ViralSequenceType'
        examples:
          example-0:
            value:
              - GENOME
              - CDS
              - PROTEIN
            summary: Include genomic, CDS, and protein sequence files
      - name: aux_report
        description: 'Specify which report files to include in the data package. The virus data report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusDatasetReportType'
        examples:
          example-0:
            value: BIOSAMPLE
            summary: Include the BioSample report
      - name: use_psg
        description: 'Experimental approach to retrieving sequence data.'
        in: query
        required: false
        schema:
          type: boolean
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /virus/accession/{accessions}/genome/download:
    get:
      summary: Get a virus genome data package by nucleotide accession
      description: 'Download a virus genome data package including sequence, annotation, BioSample data and a detailed data report by nucleotide accession.'
      tags:
        - Virus
      operationId: virus_genome_download_accession
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
      - name: taxons
        description: 'NCBI Taxonomy IDs or names (common or scientific) at any taxonomic rank'
        in: query
        required: false
        schema:
          type: array
          items:
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
      - name: refseq_only
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
      - name: annotated_only
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
            summary: Inclue all genomes
      - name: released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: host
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
      - name: pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: geo_location
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
      - name: usa_state
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
      - name: complete_only
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
      - name: include_sequence
        description: 'Specify which sequence files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2ViralSequenceType'
        examples:
          example-0:
            value:
              - GENOME
              - CDS
              - PROTEIN
            summary: Include genomic, CDS, and protein sequence files
      - name: aux_report
        description: 'Specify which report files to include in the data package. The virus data report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusDatasetReportType'
        examples:
          example-0:
            value: BIOSAMPLE
            summary: Include the BioSample report
      - name: use_psg
        description: 'Experimental approach to retrieving sequence data.'
        in: query
        required: false
        schema:
          type: boolean
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /virus/genome/download:
    post:
      summary: Get a virus genome data package
      description: 'Download a virus genome data package including sequence, annotation, BioSample data and a detailed data report by nucleotide accession.'
      tags:
        - Virus
      operationId: virus_genome_download_post
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
              $ref: '#/components/schemas/v2VirusDatasetRequest'
            examples:
              Single Virus accession example:
                description: Betacoronavirus England 1 isolate H123990006, complete genome
                value:
                  accessions:
                    - NC_038294.1
      parameters:
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /virus/taxon/sars2/protein/{proteins}/download:
    get:
      summary: Get a SARS-CoV-2 protein data package by protein name
      description: 'Download a SARS-CoV-2 protein data package including sequence, annotation, BioSample data and a detailed data report by protein name.'
      tags:
        - Virus
      operationId: sars2_protein_download
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
      - name: proteins
        description: 'One or more SARS-CoV-2 protein names'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: spike protein
            summary: SARS-CoV-2 spike protein
          example-1:
            value:
              - spike protein
              - envelope protein
              - RdRp
            summary: SARS-CoV-2 spike, envelope, and RNA-dependent RNA polymerase proteins
      - name: refseq_only
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
            summary: Both GenBank & RefSeq genomes
      - name: annotated_only
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
            summary: All genomes
      - name: released_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: updated_since
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-15"
            summary: January 15th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: host
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
      - name: pangolin_classification
        description: 'Limit to SARS-CoV-2 genomes with the specified Pango lineage.'
        in: query
        required: false
        schema:
          type: string
        examples:
          example-0:
            value: LP.8.1
            summary: SARS-CoV-2 Pango lineage LP.8.1
      - name: geo_location
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
      - name: usa_state
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
      - name: complete_only
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
      - name: include_sequence
        description: 'Specify which sequence files to include in the data package.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2ViralSequenceType'
        examples:
          example-0:
            value:
              - CDS
              - PROTEIN
            summary: Include CDS and proetin sequence files
      - name: aux_report
        description: 'Specify which report files to include in the data package. The virus data report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusDatasetReportType'
        examples:
          example-0:
            value: ANNOTATION
            summary: Include the annotation report
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /virus/taxon/sars2/protein/download:
    post:
      summary: Get a SARS-CoV-2 protein data package
      description: 'Download a SARS-CoV-2 protein data package including sequence, annotation, BioSample data and a detailed data report.'
      tags:
        - Virus
      operationId: sars2_protein_download_post
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
              $ref: '#/components/schemas/v2Sars2ProteinDatasetRequest'
            examples:
              SARS-CoV-2 virus RefSeq protein example:
                description: SARS-CoV-2 spike surface glycoprotein (RefSeq)
                value:
                  proteins:
                   - spike
                  refseq_only: true
                  include_sequence:
                   - PROTEIN
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
