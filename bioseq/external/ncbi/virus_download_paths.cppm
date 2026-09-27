/**
 * @file external/ncbi/virus_download_paths.cppm
 * @brief NCBI virus package download endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:virus_download_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Virus genome and SARS-CoV-2 protein package-download paths. */
inline constexpr std::string_view virus_download_paths =
    R"bioseq_openapi(  /virus/taxon/{taxon}/genome/download:
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
