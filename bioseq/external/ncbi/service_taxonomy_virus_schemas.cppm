/**
 * @file external/ncbi/service_taxonomy_virus_schemas.cppm
 * @brief Request and response types for NCBI sequence, taxonomy, and virus services.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:service_taxonomy_virus_schemas;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Sequence, taxonomy, and virus service request and response schemas. */
inline constexpr std::string_view service_taxonomy_virus_schemas =
    R"bioseq_openapi(    v2Sars2ProteinDatasetRequest:
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
