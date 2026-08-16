/**
 * @file external/ncbi/genome_project_sample_paths.cppm
 * @brief NCBI genome project and sample catalog endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:genome_project_sample_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Assembly-report paths filtered by BioProject or BioSample accession. */
inline constexpr std::string_view genome_project_sample_catalog_paths =
    R"bioseq_openapi(  /genome/bioproject/{bioprojects}/dataset_report:
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
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
