/**
 * @file external/ncbi/virus_paths.cppm
 * @brief NCBI virus summary and table endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:virus_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Virus genome and protein summary and table path entries. */
inline constexpr std::string_view virus_summary_paths =
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
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
