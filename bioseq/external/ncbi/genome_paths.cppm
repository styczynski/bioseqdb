/**
 * @file external/ncbi/genome_paths.cppm
 * @brief NCBI genome downloads, annotation reports, and assembly checks.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:genome_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Requests for genome  data packages as ZIP downloads. */
inline constexpr std::string_view genome_download_paths =
    R"bioseq_openapi(  /genome/accession/{accessions}/download:
    get:
      summary: Get a genome data package by genome assembly accession
      description: 'Download a genome data package including sequence, annotation, and a detailed data report by genome assembly accession.'
      tags:
        - Genome
      operationId: download_assembly_package
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
      - name: chromosomes
        description: 'Only return genomic fasta sequence for the specified chromosomes.'
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
      - name: include_annotation_type
        description: 'Specify which sequence, annotation, and report files to include in the data package. The assembly data report is always included, and its inclusion is not affected by this parameter.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2AnnotationForAssemblyType'
        examples:
          example-0:
            value: GENOME_FASTA
            summary: Include the genome sequence only
          example-1:
            value:
              - GENOME_FASTA
              - PROT_FASTA
              - GENOME_GFF
              - SEQUENCE_REPORT
            summary: Include genomic and protein sequences, a gff3 annotation file, and the sequence report
      - name: hydrated
        description: 'Specify whether to download a hydrated (with sequence and annotation files included) or dehydrated data package. A dehydrated data package includes the assembly data report and `fetch.txt`, which includes paths to the requested data files on NCBI servers.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetRequestResolution'
        examples:
          example-0:
            value: DATA_REPORT_ONLY
            summary: Download a dehydrated data package, which is recommended for large downloads
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /genome/download:
    post:
      summary: Get a genome data package by genome assembly accession
      description: 'Download a genome data package including sequence, annotation, and a detailed data report by genome assembly accession.'
      tags:
        - Genome
      operationId: download_assembly_package_post
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
              $ref: '#/components/schemas/v2AssemblyDatasetRequest'
            examples:
              Single GCF accession example:
                description: Tribolium castaneum reference genome, icTRiCast1.1, with genome sequence, annotation and sequence report files
                value:
                  accessions:
                  - GCF_031307605.1
                  include_annotation_type:
                  - GENOME_FASTA
                  - GENOME_GFF
                  - SEQUENCE_REPORT
      parameters:
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
)bioseq_openapi";

/** @brief Requests for genome annotation data packages. */
inline constexpr std::string_view genome_annotation_download_path =
    R"bioseq_openapi(  /genome/accession/{accession}/annotation_report/download:
    get:
      summary: Get a genome annotation data package by genome assembly accession
      description: 'Download an annotation data package including sequence and a detailed annotation report by genome assembly accession.'
      tags:
        - Genome
      operationId: download_genome_annotation_package
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
      - name: annotation_ids
        description: 'Limit to one or more features annotated on the genome by specifying a number corresponding to a row_id (unstable).'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: 1
            summary: row_id 1
      - name: symbols
        description: 'Limit to annotated features matching the given gene symbol (case-sensitive).'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Qsox1
            summary: Mouse gene Qsox1
      - name: locations
        description: 'Limit to features annotated at a specific location on the genome, by specifying a chromosome name or accession and optional start-stop range.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: 1:1-1000
            summary: Chromosome 1, nucleotides 1-1000
          example-1:
            value: NC_000067.7:3,000,000-4,000,000
            summary: Mouse chromosome 1, nucleotides 3,000,000-4,000,000
      - name: gene_types
        description: 'Limit to features of a specified gene locus type.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: protein-coding
            summary: Protein-coding genes
      - name: search_text
        description: 'Limit to features that match the specified gene symbol, gene name or protein name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: kinase
            summary: Genes matching the term 'kinase'
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
      - name: include_annotation_type
        description: 'Specify which sequence files to include in the data package. Options include gene (GENOME_FASTA), transcript (RNA_FASTA), and protein (PROT_FASTA) sequences.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2GenomeAnnotationRequestAnnotationType'
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /genome/annotation_report/download:
    post:
      summary: Get a genome annotation data package by genome assembly accession
      description: 'Download an annotation data package including sequence and a detailed annotation report by genome assembly accession.'
      tags:
        - Genome
      operationId: download_genome_annotation_package_by_post
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
              $ref: '#/components/schemas/v2GenomeAnnotationRequest'
            examples:
              Single GCF accession example:
                description: Mouse reference genome assembly, GRCm39
                value:
                  accession: "GCF_000001635.27"
      parameters:
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
)bioseq_openapi";

/** @brief Requests that check whether assembly accessions are valid. */
inline constexpr std::string_view genome_check_paths =
    R"bioseq_openapi(  /genome/accession/{accessions}/check:
    get:
      summary: Check the validity of a genome assembly accession
      description: 'Check the validity of a genome assembly accession. Output in JSON format.'
      tags:
        - Genome
      operationId: check_assembly_availability
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
                $ref: '#/components/schemas/v2AssemblyDatasetAvailability'
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
  /genome/check:
    post:
      summary: Check the validity of a genome assembly accession
      description: 'Check the validity of a genome assembly accession. Output in JSON format.'
      tags:
        - Genome
      operationId: check_assembly_availability_post
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
                $ref: '#/components/schemas/v2AssemblyDatasetAvailability'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2AssemblyDatasetRequest'
            examples:
              Single GCF accession example:
                description: Tribolium castaneum reference genome, icTRiCast1.1, with genome sequence, annotation and sequence report files
                value:
                  accessions:
                  - GCF_031307605.1
                  include_annotation_type:
                  - GENOME_FASTA
                  - GENOME_GFF
                  - SEQUENCE_REPORT
)bioseq_openapi";

/** @brief Annotation reports, report summaries, and package previews. */
inline constexpr std::string_view genome_annotation_paths =
    R"bioseq_openapi(  /genome/accession/{accession}/annotation_report:
    get:
      summary: Get genome annotation reports by genome assembly accession
      description: 'Get genome annotation reports by genome assembly accession, where each report represents a single feature annotated on the genome. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_annotation_report
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
                $ref: '#/components/schemas/v2reportsGenomeAnnotationReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsGenomeAnnotationReportPage'
            text/tab-separated-values:
              schema:
                type: string
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
      - name: annotation_ids
        description: 'Limit to one or more features annotated on the genome by specifying a number corresponding to a row_id (unstable).'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: 1
            summary: row_id 1
      - name: symbols
        description: 'Limit to annotated features matching the given gene symbol (case-sensitive).'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Qsox1
            summary: Mouse gene Qsox1
      - name: locations
        description: 'Limit to features annotated at a specific location on the genome, by specifying a chromosome name or accession and optional start-stop range.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: 1:1-1000
            summary: Chromosome 1, nucleotides 1-1000
          example-1:
            value: NC_000067.7:3,000,000-4,000,000
            summary: Mouse chromosome 1, nucleotides 3,000,000-4,000,000
      - name: gene_types
        description: 'Limit to features of a specified gene locus type.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: protein-coding
            summary: Protein-coding genes
      - name: search_text
        description: 'Limit to features that match the specified gene symbol, gene name or protein name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: kinase
            summary: Genes matching the term 'kinase'
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
      - name: page_size
        description: 'The maximum number of features to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        in: query
        required: false
        schema:
          type: integer
          default: 20
      - name: table_format
        description: 'Optional pre-defined template for processing a tabular data request'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2GenomeAnnotationRequestGenomeAnnotationTableFormat'
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
  /genome/annotation_report:
    post:
      summary: Get genome annotation reports by genome assembly accession
      description: 'Get genome annotation reports by genome assembly accession, where each report represents a single feature annotated on the genome. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_annotation_report_by_post
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
                $ref: '#/components/schemas/v2reportsGenomeAnnotationReportPage'
            application/x-ndjson:
              schema:
                $ref: '#/components/schemas/v2reportsGenomeAnnotationReportPage'
            text/tab-separated-values:
              schema:
                type: string
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2GenomeAnnotationRequest'
            examples:
              Single GCF accession example:
                description: Mouse reference genome assembly, GRCm39
                value:
                  accession: "GCF_000001635.27"
  /genome/accession/{accession}/annotation_summary:
    get:
      summary: Get genome annotation report summary information by genome assembly accession
      description: 'Get genome annotation report summary information by genome assembly accession in JSON format, including chromosome names and gene types.'
      tags:
        - Genome
      operationId: annotation_report_facets_by_accession
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
                $ref: '#/components/schemas/v2GenomeAnnotationTableSummaryReply'
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
  /genome/annotation_summary:
    post:
      summary: Get genome annotation report summary information by genome assembly accession
      description: 'Get genome annotation report summary information by genome assembly accession in JSON format, including chromosome names and gene types.'
      tags:
        - Genome
      operationId: annotation_report_facets_by_post
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
                $ref: '#/components/schemas/v2GenomeAnnotationTableSummaryReply'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2GenomeAnnotationRequest'
            examples:
              Single GCF accession example:
                description: Mouse reference genome assembly, GRCm39
                value:
                  accession: "GCF_000001635.27"
  /genome/accession/{accession}/annotation_report/download_summary:
    get:
      summary: Get a download summary (preview) of a genome annotation data package by genome assembly accession
      description: 'Get a downlaod summary (preview) of a genome annotation data package, including counts and file sizes, in JSON format.'
      tags:
        - Genome
      operationId: genome_annotation_download_summary
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
      - name: annotation_ids
        description: 'Limit to one or more features annotated on the genome by specifying a number corresponding to a row_id (unstable).'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: 1
            summary: row_id 1
      - name: symbols
        description: 'Limit to annotated features matching the given gene symbol (case-sensitive).'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Qsox1
            summary: Mouse gene Qsox1
      - name: locations
        description: 'Limit to features annotated at a specific location on the genome, by specifying a chromosome name or accession and optional start-stop range.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: 1:1-1000
            summary: Chromosome 1, nucleotides 1-1000
          example-1:
            value: NC_000067.7:3,000,000-4,000,000
            summary: Mouse chromosome 1, nucleotides 3,000,000-4,000,000
      - name: gene_types
        description: 'Limit to features of a specified gene locus type.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: protein-coding
            summary: Protein-coding genes
      - name: search_text
        description: 'Limit to features that match the specified gene symbol, gene name or protein name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: kinase
            summary: Genes matching the term 'kinase'
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
      - name: include_annotation_type
        description: 'Specify which sequence files to include in the data package. Options include gene (GENOME_FASTA), transcript (RNA_FASTA), and protein (PROT_FASTA) sequences.'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2GenomeAnnotationRequestAnnotationType'
  /genome/annotation_report/download_summary:
    post:
      summary: Get a download summary (preview) of a genome annotation data package by genome assembly accession
      description: 'Get a download summary (preview) of a genome annotation data package, including counts and file sizes, by genome assembly accession, in JSON format.'
      tags:
        - Genome
      operationId: genome_annotation_download_summary_by_post
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
              $ref: '#/components/schemas/v2GenomeAnnotationRequest'
            examples:
              Single GCF accession example:
                description: Mouse reference genome assembly, GRCm39
                value:
                  accession: "GCF_000001635.27"
)bioseq_openapi";

/** @brief Requests for the revision history of an assembly. */
inline constexpr std::string_view genome_revision_path =
    R"bioseq_openapi(  /genome/accession/{accession}/revision_history:
    get:
      summary: Get a revision history for a genome assembly by genome assembly accession
      description: 'Get a revision history, or list of all versions of a genome assembly, in JSON format.'
      tags:
        - Genome
      operationId: assembly_revision_history_by_get
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
                $ref: '#/components/schemas/v2AssemblyRevisionHistory'
      parameters:
      - name: accession
        description: 'One genome assembly accession'
        in: path
        required: true
        schema:
          type: string
        examples:
          example-0:
            value: GCF_000001405.40
            summary: Human reference genome assembly, GRCh38.p14
  /genome/revision_history:
    post:
      summary: Get a revision history for a genome assembly by genome assembly accession
      description: 'Get a revision history, or list of all versions of a genome assembly, in JSON format.'
      tags:
        - Genome
      operationId: assembly_revision_history_by_post
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
                $ref: '#/components/schemas/v2AssemblyRevisionHistory'
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2AssemblyRevisionHistoryRequest'
            examples:
              Single GCF accession example:
                description: Human reference genome assembly, GRCh38.p14
                value:
                  accession: "GCF_000001405.40"
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
