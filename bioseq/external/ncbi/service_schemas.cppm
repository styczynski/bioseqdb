/**
 * @file external/ncbi/service_schemas.cppm
 * @brief NCBI service request and response schema fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:service_schemas;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Security schemes and service request and response schemas. */
inline constexpr std::string_view service_schemas = R"bioseq_openapi(components:
  securitySchemes:
    ApiKeyAuth:
      type: apiKey
      in: query
      name: api_key
    ApiKeyAuthHeader:
      type: apiKey
      in: header
      name: api-key
  schemas:
    v2Accessions:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
    v2AssemblyAccessions:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
    v2AssemblyCheckMHistogramReply:
      type: object
      properties:
        species_taxid:
          type: integer
        histogram_intervals:
          type: array
          items:
            $ref: '#/components/schemas/v2AssemblyCheckMHistogramReplyHistogramInterval'
    v2AssemblyCheckMHistogramReplyHistogramInterval:
      type: object
      properties:
        start_pos:
          type: number
          format: float
          title: 'Starting position for this interval'
        stop_pos:
          type: number
          format: float
          title: 'ending position for this interval'
        count:
          type: number
          format: float
          title: 'number of elements in this interval'
    v2AssemblyCheckMHistogramRequest:
      type: object
      properties:
        species_taxon:
          type: string
          title: 'Bacterial species taxonomy ID or taxonomic name'
    v2AssemblyDataReportDraftRequest:
      type: object
      properties:
        accession:
          type: string
    v2AssemblyDatasetAvailability:
      type: object
      properties:
        valid_assemblies:
          type: array
          items:
            type: string
        invalid_assemblies:
          type: array
          items:
            type: string
        reason:
          type: string
    v2AssemblyDatasetDescriptorsFilter:
      type: object
      properties:
        reference_only:
          type: boolean
          title: 'If true, limit to reference genome assemblies.'
        assembly_source:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblySource'
          title: 'Limit to either RefSeq (GCF_) or GenBank (GCA_) genome assemblies.'
        has_annotation:
          type: boolean
          title: 'Limit to annotated genome assemblies.'
        exclude_paired_reports:
          type: boolean
          title: 'If true, for GenBank (GCA_)/RefSeq (GCF_) pairs, returns the RefSeq copy. When no RefSeq copy exists, the GenBank assembly is returned.'
        exclude_atypical:
          type: boolean
          title: 'If true, exclude [atypical genome assemblies](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/data-processing/policies-annotation/genome-processing/genome_notes/#atypical-assemblies), i.e., genomes that have assembly issues or are otherwise atypical.'
        assembly_version:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterAssemblyVersion'
          title: 'Limit to the most recent (current) version of a genome assembly.'
        assembly_level:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyLevel'
        first_release_date:
          type: string
          format: date-time
          title: 'Limit to genomes released on or after the specified date'
        last_release_date:
          type: string
          format: date-time
          title: 'Limit to genomes released on or before the specified date'
        search_text:
          type: array
          items:
            type: string
        is_metagenome_derived:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterMetagenomeDerivedFilter'
          title: 'Exclude or limit to metagenome-assembled genomes (MAGs).'
        is_type_material:
          type: boolean
          title: 'If true, limit to genomes derived from type material.'
        is_ictv_exemplar:
          type: boolean
          title: 'If true, limit to genomes derived from an ICTV exemplar (only applies to virus genomes).'
        exclude_multi_isolate:
          type: boolean
          title: 'If true, exclude genomes that were sequenced as part of large multi-isolate projects.'
        type_material_category:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilterTypeMaterialCategory'
          title: 'Limit to genomes derived from specific type material categories.'
    v2AssemblyDatasetReportsRequest:
      type: object
      properties:
        taxons:
          type: array
          items:
            type: string
        bioprojects:
          type: array
          items:
            type: string
        biosample_ids:
          type: array
          items:
            type: string
        assembly_names:
          type: array
          items:
            type: string
        wgs_accessions:
          type: array
          items:
            type: string
        accessions:
          type: array
          items:
            type: string
        filters:
          $ref: '#/components/schemas/v2AssemblyDatasetDescriptorsFilter'
        tax_exact_match:
          type: boolean
          title: 'If true, only return assemblies matching the specified taxon, but not child taxa. For example, if querying by "Canis lupus", return Canis lupus but not Canis lupus familiaris genomes.'
        chromosomes:
          type: array
          items:
            type: string
        table_fields:
          type: array
          items:
            type: string
        returned_content:
          $ref: '#/components/schemas/v2AssemblyDatasetReportsRequestContentType'
          title: 'Return complete genome assembly reports, or abbreviated reports with assembly accessions with or without paired assembly information.'
        page_size:
          type: integer
          title: 'The maximum number of genome assembly reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        sort:
          type: array
          items:
            $ref: '#/components/schemas/v2SortField'
        include_tabular_header:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
          title: 'Specify when to include the table header when requesting a tabular report.'
        table_format:
          type: string
          title: 'Optional pre-defined template for processing a tabular data request'
    v2AssemblyDatasetRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
        chromosomes:
          type: array
          items:
            type: string
        include_annotation_type:
          type: array
          items:
            $ref: '#/components/schemas/v2AnnotationForAssemblyType'
        hydrated:
          $ref: '#/components/schemas/v2AssemblyDatasetRequestResolution'
          title: 'Specify whether to download a hydrated (with sequence and annotation files included) or dehydrated data package. A dehydrated data package includes the assembly data report and `fetch.txt`, which includes paths to the requested data files on NCBI servers.'
        include_tsv:
          type: boolean
          title: 'Set to true to include a TSV representation of the data-report.'
    v2AssemblyLinksReply:
      type: object
      properties:
        assembly_links:
          type: array
          items:
            $ref: '#/components/schemas/v2AssemblyLinksReplyAssemblyLink'
    v2AssemblyLinksReplyAssemblyLink:
      type: object
      properties:
        accession:
          type: string
          title: 'The matching assembly accession'
        assembly_link_type:
          $ref: '#/components/schemas/v2AssemblyLinksReplyAssemblyLinkType'
          title: 'The type of link'
        resource_link:
          type: string
          title: 'A link to the resource'
        linked_identifiers:
          type: array
          items:
            type: string
    v2AssemblyLinksRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
        assembly_link_types:
          type: array
          items:
            $ref: '#/components/schemas/v2AssemblyLinksReplyAssemblyLinkType'
    v2AssemblyRevisionHistory:
      type: object
      properties:
        assembly_revisions:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyRevision'
        total_count:
          type: integer
    v2AssemblyRevisionHistoryRequest:
      type: object
      properties:
        accession:
          type: string
          title: 'One genome assembly accession'
    v2AssemblySequenceReportsRequest:
      type: object
      properties:
        accession:
          type: string
          title: 'A single genome assembly accession'
        chromosomes:
          type: array
          items:
            type: string
        role_filters:
          type: array
          items:
            type: string
        table_fields:
          type: array
          items:
            type: string
        count_assembly_unplaced:
          type: boolean
          title: 'Include the count of unplaced scaffold sequences'
        page_size:
          type: integer
          title: 'The maximum number of genome assemblies to return. Maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        include_tabular_header:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
        table_format:
          type: string
          title: 'Optional pre-defined template for processing a tabular data request'
    v2BioSampleDatasetReportsRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
    v2BiocollectionsSortField:
      type: object
      properties:
        field:
          type: string
        direction:
          $ref: '#/components/schemas/v2SortDirection'
    v2DatasetRequest:
      type: object
      properties:
        genome_v2:
          $ref: '#/components/schemas/v2AssemblyDatasetRequest'
        gene_v2:
          $ref: '#/components/schemas/v2GeneDatasetRequest'
        virus_v2:
          $ref: '#/components/schemas/v2VirusDatasetRequest'
    v2DownloadSummary:
      type: object
      properties:
        record_count:
          type: integer
          title: 'The number of records for the requested filter.'
        assembly_count:
          type: integer
          title: 'For backwards compatability with old VirusDatasetSummary'
        resource_updated_on:
          type: string
          format: date-time
          title: 'The latest date on which the resource was updated.'
        hydrated:
          $ref: '#/components/schemas/v2DownloadSummaryHydrated'
        dehydrated:
          $ref: '#/components/schemas/v2DownloadSummaryDehydrated'
        errors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsError'
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
        available_files:
          $ref: '#/components/schemas/v2DownloadSummaryAvailableFiles'
    v2DownloadSummaryAvailableFiles:
      type: object
      properties:
        all_genomic_fasta:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
        genome_gff:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
        genome_gbff:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
        rna_fasta:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
        prot_fasta:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
        genome_gtf:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
        cds_fasta:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
        sequence_report:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
        annotation_report:
          $ref: '#/components/schemas/v2DownloadSummaryFileSummary'
    v2DownloadSummaryDehydrated:
      type: object
      properties:
        estimated_file_size_mb:
          type: integer
        url:
          type: string
        cli_download_command_line:
          type: string
        cli_rehydrate_command_line:
          type: string
    v2DownloadSummaryFileSummary:
      type: object
      properties:
        file_count:
          type: integer
        size_mb:
          type: number
          format: float
    v2DownloadSummaryHydrated:
      type: object
      properties:
        estimated_file_size_mb:
          type: integer
        url:
          type: string
        cli_download_command_line:
          type: string
    v2ElementFlankConfig:
      type: object
      properties:
        length:
          type: integer
    v2GeneChromosomeSummaryReply:
      type: object
      properties:
        gene_chromosome_summaries:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneChromosomeSummaryReplyGeneChromosomeSummary'
    v2GeneChromosomeSummaryReplyGeneChromosomeSummary:
      type: object
      properties:
        name:
          type: string
        count:
          type: integer
        accession:
          type: string
    v2GeneChromosomeSummaryRequest:
      type: object
      properties:
        taxon:
          type: string
          title: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank, with an anntoated genome'
        annotation_name:
          type: string
          title: 'Annotation name corresponding to the provided taxon'
    v2GeneCountsByTaxonReply:
      type: object
      properties:
        report:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneCountsByTaxonReplyGeneTypeAndCount'
    v2GeneCountsByTaxonReplyGeneTypeAndCount:
      type: object
      properties:
        gene_type:
          type: string
        count:
          type: integer
    v2GeneCountsByTaxonRequest:
      type: object
      properties:
        taxon:
          type: string
          title: 'Taxon for provided gene symbol'
    v2GeneDatasetReportsRequest:
      type: object
      properties:
        returned_content:
          $ref: '#/components/schemas/v2GeneDatasetReportsRequestContentType'
          title: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        gene_ids:
          type: array
          items:
            type: integer
        accessions:
          type: array
          items:
            type: string
        symbols_for_taxon:
          $ref: '#/components/schemas/v2GeneDatasetReportsRequestSymbolsForTaxon'
        taxon:
          type: string
          title: 'NCBI Taxonomy ID or name (common or scientific) that the genes are annotated at'
        locus_tags:
          type: array
          items:
            type: string
        table_fields:
          type: array
          items:
            type: string
        table_format:
          type: string
          title: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        include_tabular_header:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
          title: 'Specify when to include the table header when requesting a tabular report.'
        page_size:
          type: integer
          title: 'The maximum number of gene reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        query:
          type: string
          title: 'Limit to genes that match the specified gene symbol, name (description), alias, locus tag or protein name.'
        types:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneType'
        accession_filter:
          type: array
          items:
            type: string
        tax_search_subtree:
          type: boolean
          title: 'If true, include genes from taxonomic ranks below the requested taxon.'
        sort:
          type: array
          items:
            $ref: '#/components/schemas/v2SortField'
    v2GeneDatasetReportsRequestSymbolsForTaxon:
      type: object
      properties:
        symbols:
          type: array
          items:
            type: string
        taxon:
          type: string
          title: 'NCBI Taxonomy ID or name (common or scientific, any taxonomic rank) for the provided gene symbol'
    v2GeneDatasetRequest:
      type: object
      properties:
        gene_ids:
          type: array
          items:
            type: integer
        include_annotation_type:
          type: array
          items:
            $ref: '#/components/schemas/v2Fasta'
        returned_content:
          $ref: '#/components/schemas/v2GeneDatasetRequestContentType'
          title: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        fasta_filter:
          type: array
          items:
            type: string
        accession_filter:
          type: array
          items:
            type: string
        aux_report:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneDatasetRequestGeneDatasetReportType'
        tabular_reports:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneDatasetRequestGeneDatasetReportType'
        table_fields:
          type: array
          items:
            type: string
        table_report_type:
          $ref: '#/components/schemas/v2GeneDatasetRequestGeneDatasetReportType'
          title: 'Specify the report from which the table fields will be taken. This is currently deprecated - use tabular_reports instead.'
    v2GeneLinksReply:
      type: object
      properties:
        gene_links:
          type: array
          items:
            $ref: '#/components/schemas/v2GeneLinksReplyGeneLink'
    v2GeneLinksReplyGeneLink:
      type: object
      properties:
        gene_id:
          type: integer
          title: 'The matching gene id'
        gene_link_type:
          $ref: '#/components/schemas/v2GeneLinksReplyGeneLinkType'
          title: 'The type of link, e.g. gdv link or ortholog set'
        resource_link:
          type: string
          title: 'A link to a gene resource'
        resource_id:
          type: string
          title: 'A resource ID, provided if gene-id is not used for the link, e.g. the ortholog id.'
    v2GeneLinksRequest:
      type: object
      properties:
        gene_ids:
          type: array
          items:
            type: integer
    v2GenePubmedIdsRequest:
      type: object
      properties:
        gene_ids:
          type: integer
    v2GenePubmedIdsResponse:
      type: object
      properties:
        pubmed_ids:
          type: array
          items:
            type: integer
    v2GenomeAnnotationRequest:
      type: object
      properties:
        accession:
          type: string
          title: 'A single genome assembly accession'
        annotation_ids:
          type: array
          items:
            type: string
        symbols:
          type: array
          items:
            type: string
        locations:
          type: array
          items:
            type: string
        gene_types:
          type: array
          items:
            type: string
        search_text:
          type: array
          items:
            type: string
        sort:
          type: array
          items:
            $ref: '#/components/schemas/v2SortField'
        include_annotation_type:
          type: array
          items:
            $ref: '#/components/schemas/v2GenomeAnnotationRequestAnnotationType'
        page_size:
          type: integer
          title: 'The maximum number of features to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        table_fields:
          type: array
          items:
            type: string
        table_format:
          $ref: '#/components/schemas/v2GenomeAnnotationRequestGenomeAnnotationTableFormat'
          title: 'Optional pre-defined template for processing a tabular data request'
        include_tabular_header:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
          title: 'Specify when to include the table header when requesting a tabular report.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
    v2GenomeAnnotationTableSummaryReply:
      type: object
      properties:
        accession:
          type: string
          title: 'Assembly from which available values are taken'
        chromosomes:
          type: array
          items:
            type: string
        gene_types:
          type: array
          items:
            type: string
        empty_columns:
          type: array
          items:
            type: string
    v2GetBiocollectionsRequest:
      type: object
      properties:
        collection_ids:
          type: array
          items:
            type: string
        page_size:
          type: integer
          title: 'The maximum number of biocollection reports to return. Default is 1000 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token returned from a prior call with more than `page_size` results. Use this token with the same request parameters to retrieve the next page. When `page_token` is empty, all results have been retrieved.'
        sort:
          type: array
          items:
            $ref: '#/components/schemas/v2BiocollectionsSortField'
    v2HttpBody:
      type: object
      properties:
        content_type:
          type: string
          title: 'The HTTP Content-Type header value specifying the content type of the body.'
        data:
          type: string
          format: byte
          title: 'The HTTP request/response body as raw binary.'
    v2MicroBiggeDatasetRequest:
      type: object
      properties:
        opaque_solr_query:
          type: string
        files:
          type: array
          items:
            $ref: '#/components/schemas/v2MicroBiggeDatasetRequestFileType'
        element_flank_config:
          $ref: '#/components/schemas/v2ElementFlankConfig'
    v2OrganelleDownloadRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
        exclude_sequence:
          type: boolean
          title: 'If true, exclude the genome sequence from the data package.'
        include_annotation_type:
          type: array
          items:
            $ref: '#/components/schemas/v2AnnotationForOrganelleType'
    v2OrganelleMetadataRequest:
      type: object
      properties:
        taxons:
          type: array
          items:
            type: string
        accessions:
          type: array
          items:
            type: string
        organelle_types:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsOrganelleType'
        first_release_date:
          type: string
          format: date-time
          title: 'Limit results to organelle genomes released on or after the specified date.'
        last_release_date:
          type: string
          format: date-time
          title: 'Limit results to organelle genomes released on or before the specified date.'
        tax_exact_match:
          type: boolean
          title: 'If true, only return assemblies with the given NCBI Taxonomy ID, or name. Otherwise, assemblies from taxonomy subtree are included, too.'
        sort:
          type: array
          items:
            $ref: '#/components/schemas/v2OrganelleSort'
        returned_content:
          $ref: '#/components/schemas/v2OrganelleMetadataRequestContentType'
          title: 'Return complete organelle reports or nucleotide accessions only.'
        page_size:
          type: integer
          title: 'The maximum number of organelle assemblies to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token is returned from an `OrganelleMetadata` call with more than `page_size` results. Use this token, along with the previous `OrganelleMetadata` parameters, to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        table_format:
          $ref: '#/components/schemas/v2OrganelleMetadataRequestOrganelleTableFormat'
          title: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        include_tabular_header:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
          title: 'Specify when to include the table header when requesting a tabular report.'
    v2OrganelleSort:
      type: object
      properties:
        field:
          type: string
        direction:
          $ref: '#/components/schemas/v2SortDirection'
    v2OrganismQueryRequest:
      type: object
      properties:
        organism_query:
          type: string
        taxon_query:
          type: string
          title: 'Taxonomy ID or name (common or scientific) at any taxonomic rank'
        tax_rank_filter:
          $ref: '#/components/schemas/v2OrganismQueryRequestTaxRankFilter'
          title: 'Optionally return results for taxonomic ranks above species using `higher_taxon`'
        taxon_resource_filter:
          $ref: '#/components/schemas/v2OrganismQueryRequestTaxonResourceFilter'
          title: 'Limit to taxonomy nodes with gene, genome or organelle data'
        exact_match:
          type: boolean
          title: 'If true, only return results that exactly match the provided taxonomic name'
    v2OrthologRequest:
      type: object
      properties:
        gene_id:
          type: integer
          title: 'A single NCBI GeneID'
        returned_content:
          $ref: '#/components/schemas/v2OrthologRequestContentType'
          title: 'Return complete gene reports, or abbreviated reports with either GeneIDs only or GeneIDs, transcript and protein counts.'
        taxon_filter:
          type: array
          items:
            type: string
        page_size:
          type: integer
          title: 'The maximum number of gene reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
    v2ProkaryoteGeneRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
        include_annotation_type:
          type: array
          items:
            $ref: '#/components/schemas/v2Fasta'
        gene_flank_config:
          $ref: '#/components/schemas/v2ProkaryoteGeneRequestGeneFlankConfig'
        taxon:
          type: string
          title: 'Limit gene sequences and the genome assemblies described by the [annotation report file](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/reference-docs/data-reports/prokaryote-gene-location/) to the specified NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank.'
    v2ProkaryoteGeneRequestGeneFlankConfig:
      type: object
      properties:
        length:
          type: integer
    v2PubmedList:
      type: object
      properties:
        pubmed_ids:
          type: array
          items:
            type: integer
    v2RefGeneCatalogDatasetRequest:
      type: object
      properties:
        opaque_solr_query:
          type: string
        files:
          type: array
          items:
            $ref: '#/components/schemas/v2RefGeneCatalogDatasetRequestFileType'
        element_flank_config:
          $ref: '#/components/schemas/v2ElementFlankConfig'
    v2Sars2ProteinDatasetRequest:
      type: object
      properties:
        proteins:
          type: array
          items:
            type: string
        refseq_only:
          type: boolean
          title: 'If true, limit results to RefSeq genomes.'
        annotated_only:
          type: boolean
          title: 'If true, limit results to annotated genomes.'
        released_since:
          type: string
          format: date-time
        updated_since:
          type: string
          format: date-time
        host:
          type: string
          title: 'Limit to genomes isolated from the specified host species (NCBI Taxonomy ID, common or scientific name).'
        pangolin_classification:
          type: string
          title: 'Limit to SARS-CoV-2 genomes with the specified Pango lineage.'
        geo_location:
          type: string
          title: 'Limit to genomes collected from the specififed geographic location.'
        usa_state:
          type: string
          title: 'Limit to genomes collected from the specified U.S. state (two-letter abbreviation).'
        complete_only:
          type: boolean
          title: 'Limit to genomes designated as complete, as defined by the submitter.'
        table_fields:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusTableField'
        include_sequence:
          type: array
          items:
            $ref: '#/components/schemas/v2ViralSequenceType'
        aux_report:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusDatasetReportType'
        format:
          $ref: '#/components/schemas/v2TableFormat'
          title: 'Specify output format'
    v2SciNameAndIds:
      type: object
      properties:
        sci_name_and_ids:
          type: array
          items:
            $ref: '#/components/schemas/v2SciNameAndIdsSciNameAndId'
    v2SciNameAndIdsSciNameAndId:
      type: object
      properties:
        sci_name:
          type: string
          title: 'Scientific name'
        tax_id:
          type: string
          format: uint64
          title: 'NCBI Taxonomy ID'
        common_name:
          type: string
          title: 'Common name'
        matched_term:
          type: string
          title: 'Matched term'
        rank:
          $ref: '#/components/schemas/v2reportsRankType'
          title: 'Rank'
        group_name:
          type: string
          title: 'Group name (blast group)'
    v2SeqRange:
      type: object
      properties:
        accession:
          type: string
          title: 'NCBI Accession.version of the sequence'
        begin:
          type: string
          format: uint64
        end:
          type: string
          format: uint64
        orientation:
          $ref: '#/components/schemas/v2reportsOrientation'
    v2SeqReply:
      type: object
      properties:
        accession:
          type: string
          title: 'Accession.version of the retrieved sequence'
        seq_length:
          type: string
          format: uint64
          title: 'Total length of the sequence (not just the amount retrieved)'
        mol_type:
          $ref: '#/components/schemas/v2MolType'
          title: 'Molecule type - dna, rna, protein'
        defline:
          type: string
          title: 'Defline for the retrieved sequence data'
        sequence:
          type: string
          title: 'The retrieved sequence (includes line-breaks)'
        begin:
          type: string
          format: uint64
          title: 'starting index of the retrieved sequence'
        end:
          type: string
          format: uint64
          title: 'ending index of retrieved sequence - may be less than the ''end'' requested if the requested size was greater than the maximum retrieval length.'
    v2SequenceAccessionRequest:
      type: object
      properties:
        accession:
          type: string
          title: 'Nucleotide sequence accession associated with a genome assembly'
    v2SequenceReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsSequenceInfo'
        total_count:
          type: integer
          title: 'The total count of available datasets (ignoring the cutoff parameter). Only provided for the first page of results (when `page_token` is empty in the request).'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2SequenceRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
        include_all_versions:
          type: boolean
          title: 'When true, include previous versions as well as the latest. Default behavior when this field is omitted is to return latest version only (i.e., `include_all_versions` defaults to false).'
    v2SleepReply:
      type: object
      properties:
        reply:
          type: string
    v2SleepRequest:
      type: object
      properties:
        sleep_msec:
          type: integer
        error_rate:
          type: number
          format: float
          title: 'From 0 to 100, chance of an error'
    v2SortField:
      type: object
      properties:
        field:
          type: string
        direction:
          $ref: '#/components/schemas/v2SortDirection'
    v2TabularOutput:
      type: object
      properties:
        data:
          type: string
    v2TaxonomyDatasetRequest:
      type: object
      properties:
        tax_ids:
          type: array
          items:
            type: integer
        aux_reports:
          type: array
          items:
            $ref: '#/components/schemas/v2TaxonomyDatasetRequestTaxonomyReportType'
    v2TaxonomyFilteredSubtreeRequest:
      type: object
      properties:
        taxons:
          type: array
          items:
            type: string
        specified_limit:
          type: boolean
          title: 'Limit to specified species'
        exclude_extinct:
          type: boolean
          title: 'Limit to species that are not extinct'
        levels:
          type: integer
          title: 'Number of taxonomic levels to return with a valid range of 1-5. Values above 5 will return 5 levels and if level is 0 or not specified, 1 level will be returned.'
        rank_limits:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRankType'
        include_incertae_sedis:
          type: boolean
          title: 'Include nodes with ranks not in ''rank_limits'' if their names meet criteria for incertae sedis (of unknown origin).'
    v2TaxonomyFilteredSubtreeResponse:
      type: object
      properties:
        root_nodes:
          type: array
          items:
            type: integer
        edges:
          $ref: '#/components/schemas/v2TaxonomyFilteredSubtreeResponseEdgesEntry'
        warnings:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsWarning'
        errors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsError'
    v2TaxonomyFilteredSubtreeResponseEdge:
      type: object
      properties:
        visible_children:
          type: array
          items:
            type: integer
        children_status:
          $ref: '#/components/schemas/v2TaxonomyFilteredSubtreeResponseEdgeChildStatus'
        rank:
          $ref: '#/components/schemas/v2reportsRankType'
        scientific_name:
          type: string
        curator_common_name:
          type: string
        assembly_count:
          type: integer
    v2TaxonomyFilteredSubtreeResponseEdgesEntry:
      description: A map.
      properties:
        default:
          $ref: '#/components/schemas/v2TaxonomyFilteredSubtreeResponseEdge'
      additionalProperties:
        $ref: '#/components/schemas/v2TaxonomyFilteredSubtreeResponseEdge'
    v2TaxonomyImageMetadataRequest:
      type: object
      properties:
        taxon:
          type: string
    v2TaxonomyImageMetadataResponse:
      type: object
      properties:
        tax_id:
          type: string
        src:
          type: string
        license:
          type: string
        attribution:
          type: string
        source:
          type: string
        image_sizes:
          type: array
          items:
            $ref: '#/components/schemas/v2ImageSize'
        format:
          type: string
        license_url:
          type: string
    v2TaxonomyImageRequest:
      type: object
      properties:
        taxon:
          type: string
        image_size:
          $ref: '#/components/schemas/v2ImageSize'
    v2TaxonomyLinksRequest:
      type: object
      properties:
        taxon:
          type: string
    v2TaxonomyLinksResponse:
      type: object
      properties:
        tax_id:
          type: string
        encyclopedia_of_life:
          type: string
        global_biodiversity_information_facility:
          type: string
        inaturalist:
          type: string
        viralzone:
          type: string
        wikipedia:
          type: string
        generic_links:
          type: array
          items:
            $ref: '#/components/schemas/v2TaxonomyLinksResponseGenericLink'
    v2TaxonomyLinksResponseGenericLink:
      type: object
      properties:
        link_name:
          type: string
        link_url:
          type: string
    v2TaxonomyMatch:
      type: object
      properties:
        warnings:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsWarning'
        errors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsError'
        query:
          type: array
          items:
            type: string
        taxonomy:
          $ref: '#/components/schemas/v2TaxonomyNode'
    v2TaxonomyMetadataRequest:
      type: object
      properties:
        taxons:
          type: array
          items:
            type: string
        returned_content:
          $ref: '#/components/schemas/v2TaxonomyMetadataRequestContentType'
          title: 'Return complete taxonomy reports, Taxonomy IDs only, or reports without assembly and gene counts (metadata).'
        page_size:
          type: integer
          title: 'The maximum number of taxons to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        include_tabular_header:
          $ref: '#/components/schemas/v2IncludeTabularHeader'
          title: 'Specify when to include the table header when requesting a tabular report.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
        table_format:
          $ref: '#/components/schemas/v2TaxonomyMetadataRequestTableFormat'
          title: 'Specify a predefined set of fields for the tabular report using built-in templates. Use of this parameter requires the HTTP header, `accept: text/tab-separated-values`.'
        children:
          type: boolean
          title: 'If true, return results for child taxa.'
        ranks:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRankType'
    v2TaxonomyMetadataResponse:
      type: object
      properties:
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
        taxonomy_nodes:
          type: array
          items:
            $ref: '#/components/schemas/v2TaxonomyMatch'
    v2TaxonomyNode:
      type: object
      properties:
        tax_id:
          type: integer
          title: 'NCBI Taxonomy identifier'
        organism_name:
          type: string
          title: 'Scientific name'
        common_name:
          type: string
          title: 'Common name'
        genbank_common_name:
          type: string
          title: 'GenBank common name'
        acronyms:
          type: array
          items:
            type: string
        genbank_acronym:
          type: string
          title: 'GenBank acronym'
        blast_name:
          type: string
          title: 'blast name for organism'
        lineage:
          type: array
          items:
            type: integer
        children:
          type: array
          items:
            type: integer
        descendent_with_described_species_names_count:
          type: integer
          title: 'The number of descendants (grand total) that are specified.'
        rank:
          $ref: '#/components/schemas/v2reportsRankType'
          title: 'The rank of the organism.  An enumerated set of values.'
        has_described_species_name:
          type: boolean
          title: 'If the taxonomy node has a proper species name'
        counts:
          type: array
          items:
            $ref: '#/components/schemas/v2TaxonomyNodeCountByType'
        min_ord:
          type: integer
          title: 'ordinal values for any node are the full range of the ordinal values in the subtree underneath this node, and are used for faster range-based lookups'
        max_ord:
          type: integer
        extinct:
          type: boolean
          title: 'True if organism is extinct'
        genomic_moltype:
          type: string
          title: 'genomic molecule type (dsDNA, ssDNA, ssDNA(-), ssRNA)'
    v2TaxonomyNodeCountByType:
      type: object
      properties:
        type:
          $ref: '#/components/schemas/v2reportsCountType'
        count:
          type: integer
    v2TaxonomyRelatedIdRequest:
      type: object
      properties:
        tax_id:
          type: integer
        include_lineage:
          type: boolean
          title: 'If true, include parent nodes'
        include_subtree:
          type: boolean
          title: 'This field is deprecated because all requests include the subtree, so it has no effect'
        ranks:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRankType'
        page_size:
          type: integer
          title: 'The maximum number of taxids to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
    v2TaxonomyTaxIdsPage:
      type: object
      properties:
        tax_ids:
          type: array
          items:
            type: integer
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2VersionReply:
      type: object
      properties:
        version:
          type: string
        major_ver:
          type: integer
        minor_ver:
          type: integer
        patch_ver:
          type: integer
    v2VirusAnnotationFilter:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
        taxon:
          type: string
          title: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank'
        taxons:
          type: array
          items:
            type: string
        refseq_only:
          type: boolean
          title: 'If true, limit to RefSeq genomes.'
        annotated_only:
          type: boolean
          title: 'If true, limit to annotated genomes.'
        released_since:
          type: string
          format: date-time
        updated_since:
          type: string
          format: date-time
        host:
          type: string
          title: 'Limit to genomes isolated from the specified host species (NCBI Taxonomy ID, common or scientific name).'
        pangolin_classification:
          type: string
          title: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        geo_location:
          type: string
          title: 'Limit to genomes collected from the specififed geographic location.'
        usa_state:
          type: string
          title: 'Limit to genomes collected from the specified U.S. state (two-letter abbreviation).'
        complete_only:
          type: boolean
          title: 'Limit to genomes designated as complete, as defined by the submitter.'
    v2VirusAnnotationReportRequest:
      type: object
      properties:
        filter:
          $ref: '#/components/schemas/v2VirusAnnotationFilter'
        table_fields:
          type: array
          items:
            type: string
        table_format:
          type: string
          title: 'Optional pre-defined template for processing a tabular data request'
        page_size:
          type: integer
          title: 'The maximum number of virus data reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
    v2VirusAvailability:
      type: object
      properties:
        valid_accessions:
          type: array
          items:
            type: string
        invalid_accessions:
          type: array
          items:
            type: string
        message:
          type: string
    v2VirusAvailabilityRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
    v2VirusDataReportRequest:
      type: object
      properties:
        filter:
          $ref: '#/components/schemas/v2VirusDatasetFilter'
          title: 'All the supported filters for virus requests'
        returned_content:
          $ref: '#/components/schemas/v2VirusDataReportRequestContentType'
          title: 'Return complete virus reports or nucleotide accessions only'
        table_fields:
          type: array
          items:
            type: string
        table_format:
          type: string
          title: 'Optional pre-defined template for processing a tabular data request'
        page_size:
          type: integer
          title: 'The maximum number of virus data reports to return. Default is 20 and maximum is 1000. If the number of results exceeds the page size, `page_token` can be used to retrieve the remaining results.'
        page_token:
          type: string
          title: 'A page token is returned when the results count exceeds `page size`. Use this token along with previous request parameters to retrieve the next page of results. When `page_token` is empty, all results have been retrieved.'
    v2VirusDatasetFilter:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
        taxon:
          type: string
          title: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank'
        taxons:
          type: array
          items:
            type: string
        refseq_only:
          type: boolean
          title: 'If true, limit results to RefSeq genomes.'
        annotated_only:
          type: boolean
          title: 'If true, limit results to annotated genomes.'
        released_since:
          type: string
          format: date-time
        updated_since:
          type: string
          format: date-time
        host:
          type: string
          title: 'Limit to genomes isolated from the specified host species (NCBI Taxonomy ID, common or scientific name).'
        pangolin_classification:
          type: string
          title: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        geo_location:
          type: string
          title: 'Limit to genomes collected from the specified geographic location.'
        usa_state:
          type: string
          title: 'Limit to genomes collected from the specified U.S. state (two-letter abbreviation).'
        complete_only:
          type: boolean
          title: 'Limit to genomes designated as complete, as defined by the submitter.'
    v2VirusDatasetRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
        taxon:
          type: string
          title: 'NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank'
        taxons:
          type: array
          items:
            type: string
        refseq_only:
          type: boolean
          title: 'If true, limit results to RefSeq genomes.'
        annotated_only:
          type: boolean
          title: 'If true, limit results to annotated genomes.'
        released_since:
          type: string
          format: date-time
        updated_since:
          type: string
          format: date-time
        host:
          type: string
          title: 'Limit to genomes isolated from the specified host species (NCBI Taxonomy ID, common or scientific name).'
        pangolin_classification:
          type: string
          title: 'Limit to SARS-CoV-2 genomes from the specified Pango lineage.'
        geo_location:
          type: string
          title: 'Limit to genomes collected from the specified geographic location.'
        usa_state:
          type: string
          title: 'Limit to genomes collected from the specified U.S. state (two-letter abbreviation).'
        complete_only:
          type: boolean
          title: 'Limit to genomes designated as complete, as defined by the submitter.'
        table_fields:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusTableField'
        include_sequence:
          type: array
          items:
            $ref: '#/components/schemas/v2ViralSequenceType'
        aux_report:
          type: array
          items:
            $ref: '#/components/schemas/v2VirusDatasetReportType'
        format:
          $ref: '#/components/schemas/v2TableFormat'
          title: 'Choose download format (tsv, csv or jsonl)'
        use_psg:
          type: boolean
          title: 'Experimental approach to retrieving sequence data.'
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
