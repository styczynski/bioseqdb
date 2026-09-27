/**
 * @file external/ncbi/report_schemas.cppm
 * @brief Data types for NCBI assembly and BioSample reports.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:report_schemas;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Assembly, BioSample, and feature report schemas. */
inline constexpr std::string_view assembly_biosample_report_schemas =
    R"bioseq_openapi(    v2reportsANIMatch:
      type: object
      properties:
        assembly:
          type: string
          title: 'Accession of the submitted organism best match type-strain assembly or best match type-strain assembly'
        organism_name:
          type: string
          title: 'Taxonomic name of the submitted organism or best match type-strain organism'
        category:
          $ref: '#/components/schemas/v2reportsANITypeCategory'
          title: 'Type category'
        ani:
          type: number
          format: float
          title: 'Percent average nucleotide identity'
        assembly_coverage:
          type: number
          format: float
          title: 'Percent coverage of the query assembly by the submitted organism or best match type assembly'
        type_assembly_coverage:
          type: number
          format: float
          title: 'Percent coverage of the submitted organism or best match type assembly by the query assembly'
    v2reportsAdditionalSubmitter:
      type: object
      properties:
        genbank_accession:
          type: string
          title: 'genbank accession of extra sequence'
        refseq_accession:
          type: string
          title: 'genbank accession of extra sequence'
        chr_name:
          type: string
          title: 'chromosome name'
        molecule_type:
          type: string
          title: 'molecule type'
        submitter:
          type: string
          title: 'Name of submitter'
        bioproject_accession:
          type: string
          title: 'Bioproject accession'
    v2reportsAnnotation:
      type: object
      properties:
        assembly_accession:
          type: string
          title: 'Genome assembly accession'
        assembly_name:
          type: string
          title: 'Genome assembly name'
        annotation_name:
          type: string
          title: 'Genome annotation name'
        annotation_release_date:
          type: string
          title: 'Genome annotation release date'
        genomic_locations:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGenomicLocation'
    v2reportsAnnotationInfo:
      type: object
      properties:
        name:
          type: string
          title: 'Annotation name'
        provider:
          type: string
          title: 'Source of the annotation'
        release_date:
          type: string
          title: 'Annotation release date'
        report_url:
          type: string
          title: 'Annotation report web address'
        stats:
          $ref: '#/components/schemas/v2reportsFeatureCounts'
        busco:
          $ref: '#/components/schemas/v2reportsBuscoStat'
        method:
          type: string
          title: 'Software tools used to calculate annotation'
        pipeline:
          type: string
          title: 'NCBI annotation pipeline used to calculate annotation'
        software_version:
          type: string
          title: 'NCBI annotation pipeline software version'
        status:
          type: string
          title: 'Type of annotation'
        release_version:
          type: string
    v2reportsArticle:
      type: object
      properties:
        title:
          type: string
          title: 'Publication title'
        date:
          type: string
          title: 'Publication date'
        authors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAuthor'
        journal:
          $ref: '#/components/schemas/v2reportsJournal'
        book:
          $ref: '#/components/schemas/v2reportsBook'
        proceedings:
          $ref: '#/components/schemas/v2reportsProceedings'
    v2reportsAssemblyDataReport:
      type: object
      properties:
        accession:
          type: string
          title: 'The GenColl assembly accession'
        current_accession:
          type: string
          title: 'The latest GenColl assembly accession for this revision chain'
        paired_accession:
          type: string
          title: 'The GenBank or RefSeq assembly accession paired with this assembly'
        source_database:
          $ref: '#/components/schemas/v2reportsSourceDatabase'
          title: 'Source of the accession. The paired accession, if it exists, is from the other database.'
        organism:
          $ref: '#/components/schemas/v2reportsOrganism'
        assembly_info:
          $ref: '#/components/schemas/v2reportsAssemblyInfo'
          title: 'Metadata for the genome assembly submission'
        assembly_stats:
          $ref: '#/components/schemas/v2reportsAssemblyStats'
          title: 'Global statistics for the genome assembly'
        organelle_info:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsOrganelleInfo'
        additional_submitters:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAdditionalSubmitter'
        annotation_info:
          $ref: '#/components/schemas/v2reportsAnnotationInfo'
          title: 'Metadata and statistics for the genome assembly annotation, when available'
        wgs_info:
          $ref: '#/components/schemas/v2reportsWGSInfo'
          title: 'Metadata pertaining to the Whole Genome Shotgun (WGS) record for the genome assemblies  that are complete genomes. Those that are clone-based do not have WGS-master records.'
        type_material:
          $ref: '#/components/schemas/v2reportsTypeMaterial'
        checkm_info:
          $ref: '#/components/schemas/v2reportsCheckM'
          title: 'Metadata on the completeness and contamination of this assembly'
        average_nucleotide_identity:
          $ref: '#/components/schemas/v2reportsAverageNucleotideIdentity'
    v2reportsAssemblyDataReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAssemblyDataReport'
        content_type:
          $ref: '#/components/schemas/v2reportsContentType'
        total_count:
          type: integer
          title: 'The total count of available datasets (ignoring the page_size parameter).'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
    v2reportsAssemblyInfo:
      type: object
      properties:
        assembly_level:
          type: string
          title: 'The level at which a genome has been assembled'
        assembly_status:
          $ref: '#/components/schemas/v2reportsAssemblyStatus'
          title: 'The GenColl assembly status'
        paired_assembly:
          $ref: '#/components/schemas/v2reportsPairedAssembly'
          title: 'Metadata from the GenBank or RefSeq assembly paired with this one'
        assembly_name:
          type: string
          title: 'The assembly submitter''s name for the genome assembly, when provided. Otherwise, a default name in the  form ASM#####v# is assigned'
        assembly_long_name:
          type: string
        assembly_type:
          type: string
          title: 'Chromosome content of the submitted genome assembly'
        bioproject_lineage:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioProjectLineage'
        bioproject_accession:
          type: string
        submission_date:
          type: string
          title: 'Date the assembly was submitted to NCBI (being replaced by release date - should maintain until most CLI users upgrade)'
        release_date:
          type: string
          title: 'Date the assembly was made available by NCBI. This field is not returned by versions of the datasets Command Line Interface (CLI) program < 15.'
        description:
          type: string
          title: 'Long description for this genome'
        submitter:
          type: string
          title: 'The submitting consortium or organization. Full submitter information is available in the BioProject'
        refseq_category:
          type: string
          title: 'The RefSeq Category, if present, indicates whether the assembly is a reference genome'
        synonym:
          type: string
          title: 'Genome name ascribed to this assembly by the UC Santa Cruz genome browser'
        linked_assembly:
          type: string
          title: 'The accession.version and designation (principal or alternate pseudohaplotype) of a paired genome assembly derived from the same diploid individual'
        linked_assemblies:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsLinkedAssembly'
        atypical:
          $ref: '#/components/schemas/v2reportsAtypicalInfo'
          title: 'Information on atypical genomes - genomes that have assembly issues or are otherwise atypical'
        genome_notes:
          type: array
          items:
            type: string
        sequencing_tech:
          type: string
          title: 'Sequencing technology used to sequence this genome'
        assembly_method:
          type: string
          title: 'Genome assembly method'
        grouping_method:
          type: string
          title: 'Version of the NCBI Virus segmented genome grouping pipeline, which groups  nucleotide sequences for segmented viruses into a single genome assembly. For more  information, see the [NCBI Virus Help Documentation](https://www.ncbi.nlm.nih.gov/labs/virus/vssi/docs/help/#virus_segment_grouping)'
        biosample:
          $ref: '#/components/schemas/v2reportsBioSampleDescriptor'
          title: 'NCBI BioSample from which the sequences in the genome assembly were obtained.'
        blast_url:
          type: string
          title: 'URL to blast page for this assembly'
        comments:
          type: string
          title: 'Freeform comments'
        suppression_reason:
          type: string
          title: 'The reason for the assembly is suppressed, for suppressed assemblies'
        diploid_role:
          $ref: '#/components/schemas/v2reportsLinkedAssemblyType'
    v2reportsAssemblyRevision:
      type: object
      properties:
        genbank_accession:
          type: string
        refseq_accession:
          type: string
        assembly_name:
          type: string
        assembly_level:
          $ref: '#/components/schemas/v2reportsAssemblyLevel'
        release_date:
          type: string
        submission_date:
          type: string
        sequencing_technology:
          type: string
        identical:
          type: boolean
          title: 'Are the RefSeq and GenBank revisions identical?'
    v2reportsAssemblyStats:
      type: object
      properties:
        total_number_of_chromosomes:
          type: integer
          title: 'Count of nuclear chromosomes, organelles and plasmids in a submitted genome assembly'
        total_sequence_length:
          type: string
          format: uint64
          title: 'Total sequence length of the nuclear genome including unplaced and unlocalized sequences'
        total_ungapped_length:
          type: string
          format: uint64
          title: 'Total length of all top-level sequences ignoring gaps. Any stretch of 10 or more Ns in a sequence is treated like a gap'
        number_of_contigs:
          type: integer
          title: 'Total number of sequence contigs in the assembly. Any stretch of 10 or more Ns in a sequence is treated as a gap between two  contigs in a scaffold when counting contigs and calculating contig N50 & L50 values'
        contig_n50:
          type: integer
          title: 'Length such that sequence contigs of this length or longer include half the bases of the assembly'
        contig_l50:
          type: integer
          title: 'Number of sequence contigs that are longer than, or equal to, the N50 length and therefore include half the bases of the assembly'
        number_of_scaffolds:
          type: integer
          title: 'Number of scaffolds including placed, unlocalized, unplaced, alternate loci and patch scaffolds'
        scaffold_n50:
          type: integer
          title: 'Length such that scaffolds of this length or longer include half the bases of the assembly'
        scaffold_l50:
          type: integer
          title: 'Number of scaffolds that are longer than, or equal to, the N50 length and therefore include half the bases of the assembly'
        gaps_between_scaffolds_count:
          type: integer
          title: 'Number of unspanned gaps between scaffolds'
        number_of_component_sequences:
          type: integer
          title: 'Total number of component WGS or clone sequences in the assembly'
        atgc_count:
          type: string
          format: uint64
          title: 'The number of AT and GC base-pairs in the assembly'
        gc_count:
          type: string
          format: uint64
          title: 'The number of GC base-pairs in the assembly'
        gc_percent:
          type: number
          format: float
          title: 'The percentage of GC base-pairs in the assembly'
        genome_coverage:
          type: string
          title: 'Genome assembly coverage'
        number_of_organelles:
          type: integer
          title: 'number of organelles'
    v2reportsAtypicalInfo:
      type: object
      properties:
        is_atypical:
          type: boolean
          title: 'If true there are assembly issues or the assembly is in some way non-standard'
        warnings:
          type: array
          items:
            type: string
    v2reportsAuthor:
      type: object
      properties:
        name:
          type: string
          title: 'Author names.'
        affiliation:
          type: string
          title: 'Author affiliation'
    v2reportsAverageNucleotideIdentity:
      type: object
      properties:
        taxonomy_check_status:
          $ref: '#/components/schemas/v2reportsAverageNucleotideIdentityTaxonomyCheckStatus'
          title: 'Indicates whether the ANI result is consistent with the submitted organism'
        match_status:
          $ref: '#/components/schemas/v2reportsAverageNucleotideIdentityMatchStatus'
          title: 'Indicates the specific type of ANI result that supports the Taxonomy check status'
        submitted_organism:
          type: string
          title: 'Taxonomic name of the query assembly'
        submitted_species:
          type: string
          title: 'Species name of the query assembly'
        category:
          $ref: '#/components/schemas/v2reportsANITypeCategory'
          title: 'Category of type or validated assembly'
        submitted_ani_match:
          $ref: '#/components/schemas/v2reportsANIMatch'
        best_ani_match:
          $ref: '#/components/schemas/v2reportsANIMatch'
        comment:
          type: string
          title: 'Additional information about the ANI result'
    v2reportsBioProject:
      type: object
      properties:
        accession:
          type: string
          title: 'BioProject accession'
        title:
          type: string
          title: 'Title of the BioProject provided by the submitter'
        parent_accession:
          type: string
        parent_accessions:
          type: array
          items:
            type: string
    v2reportsBioProjectLineage:
      type: object
      properties:
        bioprojects:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioProject'
    v2reportsBioSampleAttribute:
      type: object
      properties:
        name:
          type: string
        value:
          type: string
    v2reportsBioSampleContact:
      type: object
      properties:
        lab:
          type: string
          title: 'Submitter lab name.'
    v2reportsBioSampleDataReport:
      type: object
      properties:
        accession:
          type: string
          title: 'BioSample Accession identifier'
        last_updated:
          type: string
          title: 'When the biosample object was last updated.'
        publication_date:
          type: string
          title: 'BioSample object publication date.'
        submission_date:
          type: string
          title: 'BioSample object submission date.'
        sample_ids:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioSampleId'
        description:
          $ref: '#/components/schemas/v2reportsBioSampleDescription'
          title: 'BioSample description.'
        owner:
          $ref: '#/components/schemas/v2reportsBioSampleOwner'
          title: 'BioSample owner.'
        models:
          type: array
          items:
            type: string
        bioprojects:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioProject'
        package:
          type: string
          title: 'Package identifier.'
        attributes:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioSampleAttribute'
        status:
          $ref: '#/components/schemas/v2reportsBioSampleStatus'
          title: 'Current status of the object.'
        age:
          type: string
          title: 'Age at the time of sampling'
        biomaterial_provider:
          type: string
          title: 'Name and address of the lab or PI'
        breed:
          type: string
          title: 'Breed name'
        collected_by:
          type: string
          title: 'Name of persons or institute who collected the sample'
        collection_date:
          type: string
          title: 'Date on which the sample was collected'
        cultivar:
          type: string
          title: 'Cultivated variety of plant'
        dev_stage:
          type: string
          title: 'Developmental stage at the time of sampling'
        ecotype:
          type: string
          title: 'Population within a given species adapted to a local habitat'
        geo_loc_name:
          type: string
          title: 'Geographical origin of the sample'
        host:
          type: string
          title: 'The natural host to the organism'
        host_disease:
          type: string
          title: 'Name of relevant disease'
        identified_by:
          type: string
          title: 'Name of the taxonomist who identified the specimen'
        ifsac_category:
          type: string
          title: 'Interagency Food Safety Analytics Collaboration (IFSAC) category'
        isolate:
          type: string
          title: 'Description of the specific individual from which the sample was derived'
        isolate_name_alias:
          type: string
          title: 'Other IDs associated with this isolate'
        isolation_source:
          type: string
          title: 'Source of the sample'
        lat_lon:
          type: string
          title: 'Geogrpahic coordinates of the location where the sample was collected'
        project_name:
          type: string
          title: 'Name of the project'
        sample_name:
          type: string
          title: 'Sample name in source database'
        serovar:
          type: string
          title: 'Taxonomic name below subspecies. Same as serotype.'
        sex:
          type: string
          title: 'Physical sex of sampled organism'
        source_type:
          type: string
          title: 'Controlled vocabulary describing the isolation source'
        strain:
          type: string
          title: 'Strain name'
        sub_species:
          type: string
          title: 'Sub-species taxonomic name'
        tissue:
          type: string
          title: 'Type of tissue from which the sample was derived'
        serotype:
          type: string
          title: 'Taxonomic name below subspecies. Same as serovar'
    v2reportsBioSampleDataReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioSampleDataReport'
        total_count:
          type: integer
          title: 'The total count of available datasets (ignoring the page_size parameter).'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
    v2reportsBioSampleDescription:
      type: object
      properties:
        title:
          type: string
        organism:
          $ref: '#/components/schemas/v2reportsOrganism'
        comment:
          type: string
    v2reportsBioSampleDescriptor:
      type: object
      properties:
        accession:
          type: string
        last_updated:
          type: string
        publication_date:
          type: string
        submission_date:
          type: string
        sample_ids:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioSampleId'
        description:
          $ref: '#/components/schemas/v2reportsBioSampleDescription'
        owner:
          $ref: '#/components/schemas/v2reportsBioSampleOwner'
        models:
          type: array
          items:
            type: string
        bioprojects:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioProject'
        package:
          type: string
        attributes:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioSampleAttribute'
        status:
          $ref: '#/components/schemas/v2reportsBioSampleStatus'
        age:
          type: string
          title: 'Age at the time of sampling'
        biomaterial_provider:
          type: string
          title: 'Name and address of the lab or PI'
        breed:
          type: string
          title: 'Breed name'
        collected_by:
          type: string
          title: 'Name of persons or institute who collected the sample'
        collection_date:
          type: string
          title: 'Date on which the sample was collected'
        cultivar:
          type: string
          title: 'Cultivated variety of plant'
        dev_stage:
          type: string
          title: 'Developmental stage at the time of sampling'
        ecotype:
          type: string
          title: 'Population within a given species adapted to a local habitat'
        geo_loc_name:
          type: string
          title: 'Geographical origin of the sample'
        host:
          type: string
          title: 'The natural host to the organism'
        host_disease:
          type: string
          title: 'Name of relevant disease'
        identified_by:
          type: string
          title: 'Name of the taxonomist who identified the specimen'
        ifsac_category:
          type: string
          title: 'Interagency Food Safety Analytics Collaboration (IFSAC) category'
        isolate:
          type: string
          title: 'Description of the specific individual from which the sample was derived'
        isolate_name_alias:
          type: string
          title: 'Other IDs associated with this isolate'
        isolation_source:
          type: string
          title: 'Source of the sample'
        lat_lon:
          type: string
          title: 'Geographic coordinates of the location where the sample was collected'
        project_name:
          type: string
          title: 'Name of the project'
        sample_name:
          type: string
          title: 'Sample name in source database'
        serovar:
          type: string
          title: 'Taxonomic name below subspecies. Same as serotype.'
        sex:
          type: string
          title: 'Physical sex of sampled organism'
        source_type:
          type: string
          title: 'Controlled vocabulary describing the isolation source'
        strain:
          type: string
          title: 'Strain name'
        sub_species:
          type: string
          title: 'Sub-species taxonomic name'
        tissue:
          type: string
          title: 'Type of tissue from which the sample was derived'
        serotype:
          type: string
          title: 'Taxonomic name below subspecies. Same as serovar'
    v2reportsBioSampleId:
      type: object
      properties:
        db:
          type: string
        label:
          type: string
        value:
          type: string
    v2reportsBioSampleOwner:
      type: object
      properties:
        name:
          type: string
        contacts:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioSampleContact'
    v2reportsBioSampleStatus:
      type: object
      properties:
        status:
          type: string
        when:
          type: string
    v2reportsBiocollection:
      type: object
      properties:
        bio_collection_id:
          type: string
          format: int64
          title: 'Stable numeric identifier for the biocollection in NameBank.'
        code:
          type: string
          title: 'Canonical collection code as provided by the source institution.'
        ncbi_unique_code:
          type: string
          title: 'NCBI-normalized unique collection code used for cross-source lookup.'
        name:
          type: string
          title: 'Human-readable collection name.'
        type:
          type: string
          title: 'Collection type from NameBank (e.g., museum, culture collection)'
        comments:
          type: string
          title: 'Any additional comments'
    v2reportsBiocollectionInstitution:
      type: object
      properties:
        name:
          type: string
          title: 'Human-readable institution name.'
        country:
          type: string
          title: 'Country where the institution is located.'
        address:
          type: string
          title: 'Mailing or street address for the institution.'
        url:
          type: string
          title: 'Institution website URL.'
        comments:
          type: string
          title: 'Additional notes about the institution.'
        bio_collections:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBiocollection'
    v2reportsBiocollectionsReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGetBiocollectionsReport'
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
        total_count:
          type: integer
          title: 'The total count of available biocollection reports (ignoring page_size) for the full request.'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2reportsBook:
      type: object
      properties:
        title:
          type: string
          title: 'Book title'
        date:
          type: string
          title: 'Publication date'
        authors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAuthor'
        pages:
          type: string
          title: 'Pages of the book where the publication can be found.'
        publisher:
          type: string
          title: 'Publisher of the book.'
    v2reportsBuscoStat:
      type: object
      properties:
        busco_lineage:
          type: string
          title: 'BUSCO Lineage'
        busco_ver:
          type: string
          title: 'BUSCO Version'
        complete:
          type: number
          format: float
          title: 'BUSCO score: Complete'
        single_copy:
          type: number
          format: float
          title: 'BUSCO score: Single Copy'
        duplicated:
          type: number
          format: float
          title: 'BUSCO score: Duplicated'
        fragmented:
          type: number
          format: float
          title: 'BUSCO score: Fragmented'
        missing:
          type: number
          format: float
          title: 'BUSCO score: Missing'
        total_count:
          type: string
          format: uint64
          title: 'BUSCO score: Total Count'
    v2reportsCheckM:
      type: object
      properties:
        checkm_marker_set:
          type: string
          title: 'What taxonomic group is used as the basis for comparison with this assembly with regards to checkM values'
        checkm_species_tax_id:
          type: integer
          title: 'The species-level taxid for this assemblies checkM dataset'
        checkm_marker_set_rank:
          type: string
          title: 'CheckM taxonomic rank of checkm_marker_set'
        checkm_version:
          type: string
          title: 'CheckM software version'
        completeness:
          type: number
          format: float
          title: 'What percent complete is this assembly'
        contamination:
          type: number
          format: float
          title: 'What is the contamination percentage for this assembly'
        completeness_percentile:
          type: number
          format: float
          title: 'The percent of assemblies under the taxonomic grouping ''checkm_marker_set'' that this assembly is as-or-more complete than.'
    v2reportsClassification:
      type: object
      properties:
        superkingdom:
          $ref: '#/components/schemas/v2reportsTaxData'
        kingdom:
          $ref: '#/components/schemas/v2reportsTaxData'
        phylum:
          $ref: '#/components/schemas/v2reportsTaxData'
        class:
          $ref: '#/components/schemas/v2reportsTaxData'
        order:
          $ref: '#/components/schemas/v2reportsTaxData'
        family:
          $ref: '#/components/schemas/v2reportsTaxData'
        genus:
          $ref: '#/components/schemas/v2reportsTaxData'
        species:
          $ref: '#/components/schemas/v2reportsTaxData'
        domain:
          $ref: '#/components/schemas/v2reportsTaxData'
          title: 'NOTE: These are out of order'
        realm:
          $ref: '#/components/schemas/v2reportsTaxData'
        acellular_root:
          $ref: '#/components/schemas/v2reportsTaxData'
    v2reportsConservedDomain:
      type: object
      properties:
        accession:
          type: string
          title: 'cdd accession'
        name:
          type: string
        range:
          $ref: '#/components/schemas/v2reportsRange'
          title: 'range on the protein'
    v2reportsError:
      type: object
      properties:
        assembly_error_code:
          $ref: '#/components/schemas/v2reportsErrorAssemblyErrorCode'
        gene_error_code:
          $ref: '#/components/schemas/v2reportsErrorGeneErrorCode'
        organelle_error_code:
          $ref: '#/components/schemas/v2reportsErrorOrganelleErrorCode'
        virus_error_code:
          $ref: '#/components/schemas/v2reportsErrorVirusErrorCode'
        taxonomy_error_code:
          $ref: '#/components/schemas/v2reportsErrorTaxonomyErrorCode'
        sequence_error_code:
          $ref: '#/components/schemas/v2reportsErrorSequenceErrorCode'
        reason:
          type: string
        message:
          type: string
        invalid_identifiers:
          type: array
          items:
            type: string
    v2reportsFeatureCounts:
      type: object
      properties:
        gene_counts:
          $ref: '#/components/schemas/v2reportsGeneCounts'
          title: 'Counts of gene types'
    v2reportsFunctionalSite:
      type: object
      properties:
        name:
          type: string
          title: 'the name of this functional site'
        type:
          type: string
          title: 'function type'
        specific:
          type: boolean
          title: 'Whether this is a specific site'
        completeness:
          type: number
          format: float
          title: 'The fraction of residues mapped to the protein sequence'
        source_accession:
          type: string
          title: 'The accession of domain that defines this functional site'
        location:
          type: array
          items:
            type: integer
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
