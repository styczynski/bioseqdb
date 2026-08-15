/**
 * @file external/ncbi/genome_paths.cppm
 * @brief NCBI genome endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:genome_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Genome catalog, dataset-report, and download-summary path entries. */
inline constexpr std::string_view genome_catalog_paths =
    R"bioseq_openapi(  /genome/accession/{accessions}/download_summary:
    get:
      summary: Get a download summary (preview) of a genome data package by genome assembly accession
      description: 'Get a download summary (preview) of a genome data package, including counts and file sizes, in JSON format.'
      tags:
        - Genome
      operationId: genome_download_summary
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
  /genome/download_summary:
    post:
      summary: Get a download summary (preview) of a genome data package by genome assembly accession
      description: 'Get a downlaod summary (preview) of a genome data package, including counts and file sizes, in JSON format.'
      tags:
        - Genome
      operationId: genome_download_summary_by_post
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
  /genome/accession/{accessions}/dataset_report:
    get:
      summary: Get a genome assembly report by genome assembly accession
      description: 'Get a genome assembly report by assembly accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_dataset_report
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
            summary: Human reference genome assembly, GRCh38.p14
          example-1:
            value:
              - GCF_000001405.40
              - GCF_000001635.27
            summary: Human and mouse reference genomes, GRCh38.p14 and GRCm39
      - name: filters.reference_only
        description: 'If true, limit to reference genome assemblies.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to reference genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filters.assembly_source
        description: 'Limit to either RefSeq (GCF_) or GenBank (GCA_) genome assemblies.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblySource'
          default: all
        examples:
          example-0:
            value: refseq
            summary: Limit to RefSeq genomes
          example-1:
            value: genbank
            summary: Limit to GenBank genomes
          example-2:
            value: all
            summary: Include all genomes
      - name: filters.has_annotation
        description: 'Limit to annotated genome assemblies.'
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
      - name: filters.exclude_paired_reports
        description: 'If true, for GenBank (GCA_)/RefSeq (GCF_) pairs, returns the RefSeq copy. When no RefSeq copy exists, the GenBank assembly is returned.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes, including both members of a GenBank/RefSeq pair
          example-1:
            value: true
            summary: Returns the RefSeq copy for GenBank/RefSeq pairs, otherwise returns the GenBank assembly
      - name: filters.exclude_atypical
        description: 'If true, exclude [atypical genome assemblies](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/data-processing/policies-annotation/genome-processing/genome_notes/#atypical-assemblies), i.e., genomes that have assembly issues or are otherwise atypical.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes that are not atypical
      - name: filters.assembly_version
        description: 'Limit to the most recent (current) version of a genome assembly.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblyVersion'
          default: current
        examples:
          example-0:
            value: current
            summary: Limit to the most recent version of a genome assembly
          example-1:
            value: all_assemblies
            summary: Include all genome assemblies
      - name: filters.assembly_level
        description: 'Limit to genomes at the specified [assembly level](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/glossary/#assembly-level)'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyLevel'
        examples:
          example-0:
            value:
              - chromosome
              - complete_genome
            summary: Chromosome-level and complete genomes
          example-1:
            value:
              - scaffold
              - chromosome
              - complete_genome
            summary: Scaffold-level genomes and higher
      - name: filters.first_release_date
        description: 'Limit to genomes released on or after the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2024-01-10"
            summary: January 10th, 2024, in ISO 8601 YYYY-MM-DD format
      - name: filters.last_release_date
        description: 'Limit to genomes released on or before the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-10"
            summary: January 10th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filters.search_text
        description: 'Limit to genomes that match the specified submitter name, assembly name, infraspecific name (or modifier), or organism name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Genome Reference Consortium
            summary: Limit to genomes that match this submitter name
          example-1:
            value: GRCh38
            summary: Limit to genomes that match this assembly name
          example-2:
            value: C57BL/6J
            summary: Limit to genomes that match this strain name, a type of infraspecific name
      - name: filters.is_metagenome_derived
        description: 'Exclude or limit to metagenome-assembled genomes (MAGs).'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterMetagenomeDerivedFilter'
        examples:
          example-0:
            value: metagenome_derived_exclude
            summary: Exclude metagenome-assembled genomes, or MAGs
          example-1:
            value: metagenome_derived_only
            summary: Limit to metagenome-assembled genomes, or MAGs
      - name: filters.is_type_material
        description: 'If true, limit to genomes derived from type material.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from type material
      - name: filters.is_ictv_exemplar
        description: 'If true, limit to genomes derived from an ICTV exemplar (only applies to virus genomes).'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from an ICTV exemplar
      - name: filters.exclude_multi_isolate
        description: 'If true, exclude genomes that were sequenced as part of large multi-isolate projects.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Exclude genomes that were sequenced as part of large multi-isolate projects
      - name: filters.type_material_category
        description: 'Limit to genomes derived from specific type material categories.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterTypeMaterialCategory'
        examples:
          example-0:
            value: TYPE_MATERIAL_CLADE
            summary: Limit to genomes designated as clade exemplar
          example-1:
            value: TYPE_MATERIAL_NEOTYPE
            summary: Limit to genomes derived from neotype material
      - name: table_fields
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: accession
            summary: Genome assembly accession
          example-1:
            value:
              - accession
              - assminfo-name
            summary: Genome assembly accession and name
      - name: returned_content
        description: 'Return complete genome assembly reports, or abbreviated reports with assembly accessions with or without paired assembly information.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetReportsRequestContentType'
        examples:
          example-0:
            value: ASSM_ACC
            summary: Return abbreviated report with genome assembly accessions
          example-1:
            value: PAIRED_ACC
            summary: Return abbreviated report with genome assembly accessions and paired assembly information
      - name: page_size
        description: 'The maximum number of genome assembly reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /genome/taxon/{taxons}/dataset_report:
    get:
      summary: Get a genome assembly report by taxon
      description: 'Get a genome assembly report by taxon. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_dataset_reports_by_taxon
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
      parameters:
      - name: taxons
        description: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank'
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
              - chimpanzee
              - eastern gorilla
            summary: Common names
          example-2:
            value: Homo sapiens
            summary: Scientific name
      - name: filters.reference_only
        description: 'If true, limit to reference genome assemblies.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to reference genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filters.assembly_source
        description: 'Limit to either RefSeq (GCF_) or GenBank (GCA_) genome assemblies.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblySource'
          default: all
        examples:
          example-0:
            value: refseq
            summary: Limit to RefSeq genomes
          example-1:
            value: genbank
            summary: Limit to GenBank genomes
          example-2:
            value: all
            summary: Include all genomes
      - name: filters.has_annotation
        description: 'Limit to annotated genome assemblies.'
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
      - name: filters.exclude_paired_reports
        description: 'If true, for GenBank (GCA_)/RefSeq (GCF_) pairs, returns the RefSeq copy. When no RefSeq copy exists, the GenBank assembly is returned.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes, including both members of a GenBank/RefSeq pair
          example-1:
            value: true
            summary: Returns the RefSeq copy for GenBank/RefSeq pairs, otherwise returns the GenBank assembly
      - name: filters.exclude_atypical
        description: 'If true, exclude [atypical genome assemblies](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/data-processing/policies-annotation/genome-processing/genome_notes/#atypical-assemblies), i.e., genomes that have assembly issues or are otherwise atypical.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes that are not atypical
      - name: filters.assembly_version
        description: 'Limit to the most recent (current) version of a genome assembly.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblyVersion'
          default: current
        examples:
          example-0:
            value: current
            summary: Limit to the most recent version of a genome assembly
          example-1:
            value: all_assemblies
            summary: Include all genome assemblies
      - name: filters.assembly_level
        description: 'Limit to genomes at the specified [assembly level](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/glossary/#assembly-level)'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyLevel'
        examples:
          example-0:
            value:
              - chromosome
              - complete_genome
            summary: Chromosome-level and complete genomes
          example-1:
            value:
              - scaffold
              - chromosome
              - complete_genome
            summary: Scaffold-level genomes and higher
      - name: filters.first_release_date
        description: 'Limit to genomes released on or after the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2024-01-10"
            summary: January 10th, 2024, in ISO 8601 YYYY-MM-DD format
      - name: filters.last_release_date
        description: 'Limit to genomes released on or before the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-10"
            summary: January 10th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filters.search_text
        description: 'Limit to genomes that match the specified submitter name, assembly name, infraspecific name (or modifier), or organism name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Genome Reference Consortium
            summary: Limit to genomes that match this submitter name
          example-1:
            value: GRCh38
            summary: Limit to genomes that match this assembly name
          example-2:
            value: C57BL/6J
            summary: Limit to genomes that match this strain name, a type of infraspecific name
      - name: filters.is_metagenome_derived
        description: 'Exclude or limit to metagenome-assembled genomes (MAGs).'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterMetagenomeDerivedFilter'
        examples:
          example-0:
            value: metagenome_derived_exclude
            summary: Exclude metagenome-assembled genomes, or MAGs
          example-1:
            value: metagenome_derived_only
            summary: Limit to metagenome-assembled genomes, or MAGs
      - name: filters.is_type_material
        description: 'If true, limit to genomes derived from type material.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from type material
      - name: filters.is_ictv_exemplar
        description: 'If true, limit to genomes derived from an ICTV exemplar (only applies to virus genomes).'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from an ICTV exemplar
      - name: filters.exclude_multi_isolate
        description: 'If true, exclude genomes that were sequenced as part of large multi-isolate projects.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Exclude genomes that were sequenced as part of large multi-isolate projects
      - name: filters.type_material_category
        description: 'Limit to genomes derived from specific type material categories.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterTypeMaterialCategory'
        examples:
          example-0:
            value: TYPE_MATERIAL_CLADE
            summary: Limit to genomes designated as clade exemplar
          example-1:
            value: TYPE_MATERIAL_NEOTYPE
            summary: Limit to genomes derived from neotype material
      - name: tax_exact_match
        description: 'If true, only return assemblies matching the specified taxon, but not child taxa. For example, if querying by "Canis lupus", return Canis lupus but not Canis lupus familiaris genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
      - name: table_fields
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: accession
            summary: Genome assembly accession
          example-1:
            value:
              - accession
              - assminfo-name
            summary: Genome assembly accession and name
      - name: returned_content
        description: 'Return complete genome assembly reports, or abbreviated reports with assembly accessions with or without paired assembly information.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetReportsRequestContentType'
        examples:
          example-0:
            value: ASSM_ACC
            summary: Return abbreviated report with genome assembly accessions
          example-1:
            value: PAIRED_ACC
            summary: Return abbreviated report with genome assembly accessions and paired assembly information
      - name: page_size
        description: 'The maximum number of genome assembly reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /genome/bioproject/{bioprojects}/dataset_report:
    get:
      summary: Get genome assembly reports by BioProject accession
      description: 'Get genome assembly reports by BioProject accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_dataset_reports_by_bioproject
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
      parameters:
      - name: bioprojects
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: PRJNA489243
            summary: Vertebrate Genomes Project
          example-1:
            value: PRJNA31257
            summary: Human Genome Project
      - name: filters.reference_only
        description: 'If true, limit to reference genome assemblies.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to reference genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filters.assembly_source
        description: 'Limit to either RefSeq (GCF_) or GenBank (GCA_) genome assemblies.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblySource'
          default: all
        examples:
          example-0:
            value: refseq
            summary: Limit to RefSeq genomes
          example-1:
            value: genbank
            summary: Limit to GenBank genomes
          example-2:
            value: all
            summary: Include all genomes
      - name: filters.has_annotation
        description: 'Limit to annotated genome assemblies.'
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
      - name: filters.exclude_paired_reports
        description: 'If true, for GenBank (GCA_)/RefSeq (GCF_) pairs, returns the RefSeq copy. When no RefSeq copy exists, the GenBank assembly is returned.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes, including both members of a GenBank/RefSeq pair
          example-1:
            value: true
            summary: Returns the RefSeq copy for GenBank/RefSeq pairs, otherwise returns the GenBank assembly
      - name: filters.exclude_atypical
        description: 'If true, exclude [atypical genome assemblies](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/data-processing/policies-annotation/genome-processing/genome_notes/#atypical-assemblies), i.e., genomes that have assembly issues or are otherwise atypical.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes that are not atypical
      - name: filters.assembly_version
        description: 'Limit to the most recent (current) version of a genome assembly.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblyVersion'
          default: current
        examples:
          example-0:
            value: current
            summary: Limit to the most recent version of a genome assembly
          example-1:
            value: all_assemblies
            summary: Include all genome assemblies
      - name: filters.assembly_level
        description: 'Limit to genomes at the specified [assembly level](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/glossary/#assembly-level)'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyLevel'
        examples:
          example-0:
            value:
              - chromosome
              - complete_genome
            summary: Chromosome-level and complete genomes
          example-1:
            value:
              - scaffold
              - chromosome
              - complete_genome
            summary: Scaffold-level genomes and higher
      - name: filters.first_release_date
        description: 'Limit to genomes released on or after the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2024-01-10"
            summary: January 10th, 2024, in ISO 8601 YYYY-MM-DD format
      - name: filters.last_release_date
        description: 'Limit to genomes released on or before the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-10"
            summary: January 10th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filters.search_text
        description: 'Limit to genomes that match the specified submitter name, assembly name, infraspecific name (or modifier), or organism name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Genome Reference Consortium
            summary: Limit to genomes that match this submitter name
          example-1:
            value: GRCh38
            summary: Limit to genomes that match this assembly name
          example-2:
            value: C57BL/6J
            summary: Limit to genomes that match this strain name, a type of infraspecific name
      - name: filters.is_metagenome_derived
        description: 'Exclude or limit to metagenome-assembled genomes (MAGs).'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterMetagenomeDerivedFilter'
        examples:
          example-0:
            value: metagenome_derived_exclude
            summary: Exclude metagenome-assembled genomes, or MAGs
          example-1:
            value: metagenome_derived_only
            summary: Limit to metagenome-assembled genomes, or MAGs
      - name: filters.is_type_material
        description: 'If true, limit to genomes derived from type material.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from type material
      - name: filters.is_ictv_exemplar
        description: 'If true, limit to genomes derived from an ICTV exemplar (only applies to virus genomes).'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from an ICTV exemplar
      - name: filters.exclude_multi_isolate
        description: 'If true, exclude genomes that were sequenced as part of large multi-isolate projects.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Exclude genomes that were sequenced as part of large multi-isolate projects
      - name: filters.type_material_category
        description: 'Limit to genomes derived from specific type material categories.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterTypeMaterialCategory'
        examples:
          example-0:
            value: TYPE_MATERIAL_CLADE
            summary: Limit to genomes designated as clade exemplar
          example-1:
            value: TYPE_MATERIAL_NEOTYPE
            summary: Limit to genomes derived from neotype material
      - name: tax_exact_match
        description: 'If true, only return assemblies matching the specified taxon, but not child taxa. For example, if querying by "Canis lupus", return Canis lupus but not Canis lupus familiaris genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
      - name: table_fields
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: accession
            summary: Genome assembly accession
          example-1:
            value:
              - accession
              - assminfo-name
            summary: Genome assembly accession and name
      - name: returned_content
        description: 'Return complete genome assembly reports, or abbreviated reports with assembly accessions with or without paired assembly information.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetReportsRequestContentType'
        examples:
          example-0:
            value: ASSM_ACC
            summary: Return abbreviated report with genome assembly accessions
          example-1:
            value: PAIRED_ACC
            summary: Return abbreviated report with genome assembly accessions and paired assembly information
      - name: page_size
        description: 'The maximum number of genome assembly reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /genome/biosample/{biosample_ids}/dataset_report:
    get:
      summary: Get genome assembly reports by BioSample accession
      description: 'Get genome assembly reports by BioSample accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_dataset_reports_by_biosample_id
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
      parameters:
      - name: biosample_ids
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: SAMN15960293
            summary: BioSample Identifier
      - name: filters.reference_only
        description: 'If true, limit to reference genome assemblies.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to reference genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filters.assembly_source
        description: 'Limit to either RefSeq (GCF_) or GenBank (GCA_) genome assemblies.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblySource'
          default: all
        examples:
          example-0:
            value: refseq
            summary: Limit to RefSeq genomes
          example-1:
            value: genbank
            summary: Limit to GenBank genomes
          example-2:
            value: all
            summary: Include all genomes
      - name: filters.has_annotation
        description: 'Limit to annotated genome assemblies.'
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
      - name: filters.exclude_paired_reports
        description: 'If true, for GenBank (GCA_)/RefSeq (GCF_) pairs, returns the RefSeq copy. When no RefSeq copy exists, the GenBank assembly is returned.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes, including both members of a GenBank/RefSeq pair
          example-1:
            value: true
            summary: Returns the RefSeq copy for GenBank/RefSeq pairs, otherwise returns the GenBank assembly
      - name: filters.exclude_atypical
        description: 'If true, exclude [atypical genome assemblies](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/data-processing/policies-annotation/genome-processing/genome_notes/#atypical-assemblies), i.e., genomes that have assembly issues or are otherwise atypical.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes that are not atypical
      - name: filters.assembly_version
        description: 'Limit to the most recent (current) version of a genome assembly.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblyVersion'
          default: current
        examples:
          example-0:
            value: current
            summary: Limit to the most recent version of a genome assembly
          example-1:
            value: all_assemblies
            summary: Include all genome assemblies
      - name: filters.assembly_level
        description: 'Limit to genomes at the specified [assembly level](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/glossary/#assembly-level)'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyLevel'
        examples:
          example-0:
            value:
              - chromosome
              - complete_genome
            summary: Chromosome-level and complete genomes
          example-1:
            value:
              - scaffold
              - chromosome
              - complete_genome
            summary: Scaffold-level genomes and higher
      - name: filters.first_release_date
        description: 'Limit to genomes released on or after the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2024-01-10"
            summary: January 10th, 2024, in ISO 8601 YYYY-MM-DD format
      - name: filters.last_release_date
        description: 'Limit to genomes released on or before the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-10"
            summary: January 10th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filters.search_text
        description: 'Limit to genomes that match the specified submitter name, assembly name, infraspecific name (or modifier), or organism name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Genome Reference Consortium
            summary: Limit to genomes that match this submitter name
          example-1:
            value: GRCh38
            summary: Limit to genomes that match this assembly name
          example-2:
            value: C57BL/6J
            summary: Limit to genomes that match this strain name, a type of infraspecific name
      - name: filters.is_metagenome_derived
        description: 'Exclude or limit to metagenome-assembled genomes (MAGs).'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterMetagenomeDerivedFilter'
        examples:
          example-0:
            value: metagenome_derived_exclude
            summary: Exclude metagenome-assembled genomes, or MAGs
          example-1:
            value: metagenome_derived_only
            summary: Limit to metagenome-assembled genomes, or MAGs
      - name: filters.is_type_material
        description: 'If true, limit to genomes derived from type material.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from type material
      - name: filters.is_ictv_exemplar
        description: 'If true, limit to genomes derived from an ICTV exemplar (only applies to virus genomes).'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from an ICTV exemplar
      - name: filters.exclude_multi_isolate
        description: 'If true, exclude genomes that were sequenced as part of large multi-isolate projects.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Exclude genomes that were sequenced as part of large multi-isolate projects
      - name: filters.type_material_category
        description: 'Limit to genomes derived from specific type material categories.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterTypeMaterialCategory'
        examples:
          example-0:
            value: TYPE_MATERIAL_CLADE
            summary: Limit to genomes designated as clade exemplar
          example-1:
            value: TYPE_MATERIAL_NEOTYPE
            summary: Limit to genomes derived from neotype material
      - name: tax_exact_match
        description: 'If true, only return assemblies matching the specified taxon, but not child taxa. For example, if querying by "Canis lupus", return Canis lupus but not Canis lupus familiaris genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
      - name: table_fields
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: accession
            summary: Genome assembly accession
          example-1:
            value:
              - accession
              - assminfo-name
            summary: Genome assembly accession and name
      - name: returned_content
        description: 'Return complete genome assembly reports, or abbreviated reports with assembly accessions with or without paired assembly information.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetReportsRequestContentType'
        examples:
          example-0:
            value: ASSM_ACC
            summary: Return abbreviated report with genome assembly accessions
          example-1:
            value: PAIRED_ACC
            summary: Return abbreviated report with genome assembly accessions and paired assembly information
      - name: page_size
        description: 'The maximum number of genome assembly reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /genome/wgs/{wgs_accessions}/dataset_report:
    get:
      summary: Get a genome assembly data report by WGS accession
      description: 'Get a genome assembly data report by WGS (whole genome shotgun) accession. By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_dataset_reports_by_wgs
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
      parameters:
      - name: wgs_accessions
        description: 'One or more WGS accessions associated with a genome assembly'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: JAXUCZ01
            summary: Norway rat (Rattus norvegicus) WGS accession
          example-1:
            value:
              - JAHLSK02
              - JAAKGM02
            summary: Human WGS accessions
      - name: filters.reference_only
        description: 'If true, limit to reference genome assemblies.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to reference genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filters.assembly_source
        description: 'Limit to either RefSeq (GCF_) or GenBank (GCA_) genome assemblies.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblySource'
          default: all
        examples:
          example-0:
            value: refseq
            summary: Limit to RefSeq genomes
          example-1:
            value: genbank
            summary: Limit to GenBank genomes
          example-2:
            value: all
            summary: Include all genomes
      - name: filters.has_annotation
        description: 'Limit to annotated genome assemblies.'
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
      - name: filters.exclude_paired_reports
        description: 'If true, for GenBank (GCA_)/RefSeq (GCF_) pairs, returns the RefSeq copy. When no RefSeq copy exists, the GenBank assembly is returned.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes, including both members of a GenBank/RefSeq pair
          example-1:
            value: true
            summary: Returns the RefSeq copy for GenBank/RefSeq pairs, otherwise returns the GenBank assembly
      - name: filters.exclude_atypical
        description: 'If true, exclude [atypical genome assemblies](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/data-processing/policies-annotation/genome-processing/genome_notes/#atypical-assemblies), i.e., genomes that have assembly issues or are otherwise atypical.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes that are not atypical
      - name: filters.assembly_version
        description: 'Limit to the most recent (current) version of a genome assembly.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblyVersion'
          default: current
        examples:
          example-0:
            value: current
            summary: Limit to the most recent version of a genome assembly
          example-1:
            value: all_assemblies
            summary: Include all genome assemblies
      - name: filters.assembly_level
        description: 'Limit to genomes at the specified [assembly level](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/glossary/#assembly-level)'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyLevel'
        examples:
          example-0:
            value:
              - chromosome
              - complete_genome
            summary: Chromosome-level and complete genomes
          example-1:
            value:
              - scaffold
              - chromosome
              - complete_genome
            summary: Scaffold-level genomes and higher
      - name: filters.first_release_date
        description: 'Limit to genomes released on or after the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2024-01-10"
            summary: January 10th, 2024, in ISO 8601 YYYY-MM-DD format
      - name: filters.last_release_date
        description: 'Limit to genomes released on or before the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-10"
            summary: January 10th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filters.search_text
        description: 'Limit to genomes that match the specified submitter name, assembly name, infraspecific name (or modifier), or organism name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Genome Reference Consortium
            summary: Limit to genomes that match this submitter name
          example-1:
            value: GRCh38
            summary: Limit to genomes that match this assembly name
          example-2:
            value: C57BL/6J
            summary: Limit to genomes that match this strain name, a type of infraspecific name
      - name: filters.is_metagenome_derived
        description: 'Exclude or limit to metagenome-assembled genomes (MAGs).'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterMetagenomeDerivedFilter'
        examples:
          example-0:
            value: metagenome_derived_exclude
            summary: Exclude metagenome-assembled genomes, or MAGs
          example-1:
            value: metagenome_derived_only
            summary: Limit to metagenome-assembled genomes, or MAGs
      - name: filters.is_type_material
        description: 'If true, limit to genomes derived from type material.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from type material
      - name: filters.is_ictv_exemplar
        description: 'If true, limit to genomes derived from an ICTV exemplar (only applies to virus genomes).'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from an ICTV exemplar
      - name: filters.exclude_multi_isolate
        description: 'If true, exclude genomes that were sequenced as part of large multi-isolate projects.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Exclude genomes that were sequenced as part of large multi-isolate projects
      - name: filters.type_material_category
        description: 'Limit to genomes derived from specific type material categories.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterTypeMaterialCategory'
        examples:
          example-0:
            value: TYPE_MATERIAL_CLADE
            summary: Limit to genomes designated as clade exemplar
          example-1:
            value: TYPE_MATERIAL_NEOTYPE
            summary: Limit to genomes derived from neotype material
      - name: tax_exact_match
        description: 'If true, only return assemblies matching the specified taxon, but not child taxa. For example, if querying by "Canis lupus", return Canis lupus but not Canis lupus familiaris genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
      - name: table_fields
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: accession
            summary: Genome assembly accession
          example-1:
            value:
              - accession
              - assminfo-name
            summary: Genome assembly accession and name
      - name: returned_content
        description: 'Return complete genome assembly reports, or abbreviated reports with assembly accessions with or without paired assembly information.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetReportsRequestContentType'
        examples:
          example-0:
            value: ASSM_ACC
            summary: Return abbreviated report with genome assembly accessions
          example-1:
            value: PAIRED_ACC
            summary: Return abbreviated report with genome assembly accessions and paired assembly information
      - name: page_size
        description: 'The maximum number of genome assembly reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /genome/assembly_name/{assembly_names}/dataset_report:
    get:
      summary: Get genome assembly reports by assembly name
      description: 'Get genome assembly reports by assembly name (exact matches only). By default, in paged JSON format, but also available in tabular (accept: text/tab-separated-values) or JSON Lines (accept: application/x-ndjson) formats.'
      tags:
        - Genome
      operationId: genome_dataset_reports_by_assembly_name
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
      parameters:
      - name: assembly_names
        description: 'One or more assembly names (exact match only)'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: HanXRQr2.0-SUNRISE
            summary: Helianthus annuus, or common sunflower, reference genome
          example-1:
            value: mCamDro1.pat
            summary: Camelus dromedarius, or Arabian camel, reference genome
      - name: filters.reference_only
        description: 'If true, limit to reference genome assemblies.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: true
            summary: Limit to reference genomes
          example-1:
            value: false
            summary: Include all genomes
      - name: filters.assembly_source
        description: 'Limit to either RefSeq (GCF_) or GenBank (GCA_) genome assemblies.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblySource'
          default: all
        examples:
          example-0:
            value: refseq
            summary: Limit to RefSeq genomes
          example-1:
            value: genbank
            summary: Limit to GenBank genomes
          example-2:
            value: all
            summary: Include all genomes
      - name: filters.has_annotation
        description: 'Limit to annotated genome assemblies.'
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
      - name: filters.exclude_paired_reports
        description: 'If true, for GenBank (GCA_)/RefSeq (GCF_) pairs, returns the RefSeq copy. When no RefSeq copy exists, the GenBank assembly is returned.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes, including both members of a GenBank/RefSeq pair
          example-1:
            value: true
            summary: Returns the RefSeq copy for GenBank/RefSeq pairs, otherwise returns the GenBank assembly
      - name: filters.exclude_atypical
        description: 'If true, exclude [atypical genome assemblies](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/data-processing/policies-annotation/genome-processing/genome_notes/#atypical-assemblies), i.e., genomes that have assembly issues or are otherwise atypical.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes that are not atypical
      - name: filters.assembly_version
        description: 'Limit to the most recent (current) version of a genome assembly.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblyVersion'
          default: current
        examples:
          example-0:
            value: current
            summary: Limit to the most recent version of a genome assembly
          example-1:
            value: all_assemblies
            summary: Include all genome assemblies
      - name: filters.assembly_level
        description: 'Limit to genomes at the specified [assembly level](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/glossary/#assembly-level)'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyLevel'
        examples:
          example-0:
            value:
              - chromosome
              - complete_genome
            summary: Chromosome-level and complete genomes
          example-1:
            value:
              - scaffold
              - chromosome
              - complete_genome
            summary: Scaffold-level genomes and higher
      - name: filters.first_release_date
        description: 'Limit to genomes released on or after the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2024-01-10"
            summary: January 10th, 2024, in ISO 8601 YYYY-MM-DD format
      - name: filters.last_release_date
        description: 'Limit to genomes released on or before the specified date'
        in: query
        required: false
        schema:
          type: string
          format: date-time
        examples:
          example-0:
            value: "2025-01-10"
            summary: January 10th, 2025, in ISO 8601 YYYY-MM-DD format
      - name: filters.search_text
        description: 'Limit to genomes that match the specified submitter name, assembly name, infraspecific name (or modifier), or organism name.'
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: Genome Reference Consortium
            summary: Limit to genomes that match this submitter name
          example-1:
            value: GRCh38
            summary: Limit to genomes that match this assembly name
          example-2:
            value: C57BL/6J
            summary: Limit to genomes that match this strain name, a type of infraspecific name
      - name: filters.is_metagenome_derived
        description: 'Exclude or limit to metagenome-assembled genomes (MAGs).'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterMetagenomeDerivedFilter'
        examples:
          example-0:
            value: metagenome_derived_exclude
            summary: Exclude metagenome-assembled genomes, or MAGs
          example-1:
            value: metagenome_derived_only
            summary: Limit to metagenome-assembled genomes, or MAGs
      - name: filters.is_type_material
        description: 'If true, limit to genomes derived from type material.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from type material
      - name: filters.is_ictv_exemplar
        description: 'If true, limit to genomes derived from an ICTV exemplar (only applies to virus genomes).'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Limit to genomes derived from an ICTV exemplar
      - name: filters.exclude_multi_isolate
        description: 'If true, exclude genomes that were sequenced as part of large multi-isolate projects.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
        examples:
          example-0:
            value: false
            summary: Include all genomes
          example-1:
            value: true
            summary: Exclude genomes that were sequenced as part of large multi-isolate projects
      - name: filters.type_material_category
        description: 'Limit to genomes derived from specific type material categories.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterTypeMaterialCategory'
        examples:
          example-0:
            value: TYPE_MATERIAL_CLADE
            summary: Limit to genomes designated as clade exemplar
          example-1:
            value: TYPE_MATERIAL_NEOTYPE
            summary: Limit to genomes derived from neotype material
      - name: tax_exact_match
        description: 'If true, only return assemblies matching the specified taxon, but not child taxa. For example, if querying by "Canis lupus", return Canis lupus but not Canis lupus familiaris genomes.'
        in: query
        required: false
        schema:
          type: boolean
          default: false
      - name: table_fields
        in: query
        required: false
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: accession
            summary: Genome assembly accession
          example-1:
            value:
              - accession
              - assminfo-name
            summary: Genome assembly accession and name
      - name: returned_content
        description: 'Return complete genome assembly reports, or abbreviated reports with assembly accessions with or without paired assembly information.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2AssemblyDatasetReportsRequestContentType'
        examples:
          example-0:
            value: ASSM_ACC
            summary: Return abbreviated report with genome assembly accessions
          example-1:
            value: PAIRED_ACC
            summary: Return abbreviated report with genome assembly accessions and paired assembly information
      - name: page_size
        description: 'The maximum number of genome assembly reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
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
      - name: include_tabular_header
        description: 'Specify when to include the table header when requesting a tabular report.'
        in: query
        required: false
        schema:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
  /genome/dataset_report:
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

/** @brief Genome accession download path entries. */
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

/** @brief Genome annotation-report download path entry. */
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

/** @brief Genome accession validation path entries. */
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

/** @brief Genome annotation-report path entries. */
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

/** @brief Genome revision-history path entry. */
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
