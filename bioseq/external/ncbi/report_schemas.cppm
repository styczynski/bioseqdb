/**
 * @file external/ncbi/report_schemas.cppm
 * @brief NCBI report schema fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:report_schemas;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Genome, gene, organelle, taxonomy, and virus report schemas. */
inline constexpr std::string_view report_schemas = R"bioseq_openapi(    v2reportsANIMatch:
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
    v2reportsGeneCounts:
      type: object
      properties:
        total:
          type: integer
          title: 'Total number of annotated genes'
        protein_coding:
          type: integer
          title: 'Count of annotated genes that encode a protein'
        non_coding:
          type: integer
          title: 'Count of transcribed non-coding genes (e.g. lncRNAs, miRNAs, rRNAs, etc...) excludes transcribed pseudogenes'
        pseudogene:
          type: integer
          title: 'Count of transcribed and non-transcribed pseudogenes'
        other:
          type: integer
          title: 'Count of genic region GeneIDs and non-genic regulatory GeneIDs'
    v2reportsGeneDataReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGeneReportMatch'
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
        total_count:
          type: integer
          title: 'The total count of available genes (ignoring the page_size parameter).'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2reportsGeneDescriptor:
      type: object
      properties:
        gene_id:
          type: string
          format: uint64
          title: 'NCBI Gene ID'
        symbol:
          type: string
          title: 'Gene symbol'
        description:
          type: string
          title: 'Gene name'
        tax_id:
          type: string
          format: uint64
          title: 'NCBI Taxonomy ID for the organism'
        taxname:
          type: string
          title: 'Taxonomic name of the organism'
        common_name:
          type: string
          title: 'Common name of the organism'
        type:
          $ref: '#/components/schemas/v2reportsGeneType'
          title: 'Type of gene'
        rna_type:
          $ref: '#/components/schemas/v2reportsRnaType'
        orientation:
          $ref: '#/components/schemas/v2reportsOrientation'
          title: 'Direction of the gene relative to the genome coordinates'
        reference_standards:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGenomicRegion'
        genomic_regions:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGenomicRegion'
        chromosomes:
          type: array
          items:
            type: string
        nomenclature_authority:
          $ref: '#/components/schemas/v2reportsNomenclatureAuthority'
        swiss_prot_accessions:
          type: array
          items:
            type: string
        ensembl_gene_ids:
          type: array
          items:
            type: string
        omim_ids:
          type: array
          items:
            type: string
        synonyms:
          type: array
          items:
            type: string
        alternate_names:
          type: array
          items:
            type: string
        replaced_gene_id:
          type: string
          format: uint64
          title: 'The NCBI Gene ID for the gene that was merged into the current gene record'
        annotations:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAnnotation'
        transcript_count:
          type: integer
          title: 'Number of transcripts encoded by the gene'
        protein_count:
          type: integer
          title: 'Number of proteins encoded by the gene'
        transcript_type_counts:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTranscriptTypeCount'
        gene_groups:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGeneGroup'
        summary:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGeneSummary'
        gene_ontology:
          $ref: '#/components/schemas/v2reportsGeneOntology'
        locus_tag:
          type: string
        map_locations:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMapLocation'
    v2reportsGeneGroup:
      type: object
      properties:
        id:
          type: string
          title: 'Gene group identifier, currently these only include gene ortholog sets'
        method:
          type: string
          title: 'Method used to calculate the gene group, currently this only includes "NCBI Ortholog"'
    v2reportsGeneNeighbor:
      type: object
      properties:
        gene_id:
          type: string
          format: uint64
          title: 'NCBI Gene ID'
        symbol:
          type: string
          title: 'Gene symbol'
        description:
          type: string
          title: 'Gene name'
        gene_type:
          $ref: '#/components/schemas/v2reportsGeneType'
          title: 'Type of gene'
        genomic_range:
          $ref: '#/components/schemas/v2reportsRange'
          title: 'Location range and orientation of gene'
    v2reportsGeneNeighborGenomicLocation:
      type: object
      properties:
        genomic_location:
          $ref: '#/components/schemas/v2reportsGenomicLocation'
        gene_neighbors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGeneNeighbor'
    v2reportsGeneNeighborsAnnotation:
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
            $ref: '#/components/schemas/v2reportsGeneNeighborGenomicLocation'
    v2reportsGeneNeighborsDescriptor:
      type: object
      properties:
        tax_id:
          type: string
          format: uint64
          title: 'NCBI Taxonomy ID for the organism'
        gene_id:
          type: string
          format: uint64
          title: 'NCBI Gene ID'
        symbol:
          type: string
          title: 'Gene symbol'
        description:
          type: string
          title: 'Gene name'
        gene_type:
          $ref: '#/components/schemas/v2reportsGeneType'
          title: 'Type of gene'
        annotations:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGeneNeighborsAnnotation'
    v2reportsGeneNeighborsReports:
      type: object
      properties:
        genes:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGeneNeighborsDescriptor'
    v2reportsGeneOntology:
      type: object
      properties:
        assigned_by:
          type: string
          title: 'The database that made the annotation'
        molecular_functions:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsProcessMetadata'
        biological_processes:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsProcessMetadata'
        cellular_components:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsProcessMetadata'
    v2reportsGeneReportMatch:
      type: object
      properties:
        gene:
          $ref: '#/components/schemas/v2reportsGeneDescriptor'
        product:
          $ref: '#/components/schemas/v2reportsProductDescriptor'
        query:
          type: array
          items:
            type: string
        warnings:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsWarning'
        warning:
          $ref: '#/components/schemas/v2reportsWarning'
        errors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsError'
    v2reportsGeneSummary:
      type: object
      properties:
        source:
          type: string
          title: 'Source of the gene summary'
        description:
          type: string
          title: 'Gene summary text itself that describes the gene'
        date:
          type: string
          title: 'Date that the gene summary was last updated'
    v2reportsGeneral:
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
        status:
          type: string
          title: 'Publication status (for example, "in press", "published", or "accepted").'
    v2reportsGenomeAnnotation:
      type: object
      properties:
        gene_id:
          type: string
          format: uint64
          title: 'NCBI GeneID'
        symbol:
          type: string
          title: 'Gene symbol'
        description:
          type: string
          title: 'Gene name'
        name:
          type: string
        tax_id:
          type: string
          format: uint64
          title: 'NCBI Taxonomy ID for the organism'
        taxname:
          type: string
          title: 'Taxonomic name of the organism'
        common_name:
          type: string
          title: 'Common name of the organism'
        type:
          $ref: '#/components/schemas/v2reportsGeneType'
          title: 'Deprecated'
        gene_type:
          type: string
          title: 'Gene locus type'
        rna_type:
          $ref: '#/components/schemas/v2reportsRnaType'
        orientation:
          $ref: '#/components/schemas/v2reportsOrientation'
        locus_tag:
          type: string
        reference_standards:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGenomicRegion'
        genomic_regions:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGenomicRegion'
        transcripts:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTranscript'
        proteins:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsProtein'
        chromosomes:
          type: array
          items:
            type: string
        swiss_prot_accessions:
          type: array
          items:
            type: string
        ensembl_gene_ids:
          type: array
          items:
            type: string
        omim_ids:
          type: array
          items:
            type: string
        synonyms:
          type: array
          items:
            type: string
        annotations:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAnnotation'
    v2reportsGenomeAnnotationReportMatch:
      type: object
      properties:
        annotation:
          $ref: '#/components/schemas/v2reportsGenomeAnnotation'
        query:
          type: array
          items:
            type: string
        warning:
          $ref: '#/components/schemas/v2reportsWarning'
        errors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsError'
        row_id:
          type: string
    v2reportsGenomeAnnotationReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGenomeAnnotationReportMatch'
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
        total_count:
          type: integer
          title: 'The total count of available genes (ignoring the page_size parameter).'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2reportsGenomicLocation:
      type: object
      properties:
        genomic_accession_version:
          type: string
        sequence_name:
          type: string
        genomic_range:
          $ref: '#/components/schemas/v2reportsRange'
        exons:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRange'
    v2reportsGenomicRegion:
      type: object
      properties:
        gene_range:
          $ref: '#/components/schemas/v2reportsSeqRangeSet'
          title: 'The range of this Gene record on this genomic region.'
        type:
          $ref: '#/components/schemas/v2reportsGenomicRegionGenomicRegionType'
          title: 'Type of genomic region'
    v2reportsGetBiocollectionsReport:
      type: object
      properties:
        institution:
          $ref: '#/components/schemas/v2reportsBiocollectionInstitution'
          title: 'A single institution and its collections. Each report contains exactly one institution record and the list of biocollections owned by that institution.'
    v2reportsInfraspecificName:
      type: object
      properties:
        strain:
          type: string
          title: 'A specific genetic variant or lineage within a species.'
        substrain:
          type: string
          title: 'A subdivision of a strain.'
        variety:
          type: string
          title: 'A taxonomic rank below species.'
        serotype:
          type: string
          title: 'A distinct variation within a species of bacteria or virus based on surface antigens.'
        serogroup:
          type: string
          title: 'A group of serotypes.'
        biotype:
          type: string
          title: 'A group of organisms sharing a specific genetic makeup, often referring to physiological characteristics.'
        isolate:
          type: string
          title: 'An organism obtained from a specific source, typically indicating a particular collection event.'
        forma:
          type: string
          title: 'A taxonomic rank below variety.'
        forma_specialis:
          type: string
          title: 'A taxonomic rank for parasitic fungi or bacteria that are adapted to specific hosts.'
        ecotype:
          type: string
          title: 'A genetically distinct geographical variety or population within a species.'
        teleomorph:
          type: string
          title: 'The sexual reproductive stage of a fungus.'
        breed:
          type: string
          title: 'A domesticated group within a species'
    v2reportsInfraspecificNames:
      type: object
      properties:
        breed:
          type: string
          title: 'A homogenous group of animals within a domesticated species'
        cultivar:
          type: string
          title: 'A variety of plant within a species produced and maintained by cultivation'
        ecotype:
          type: string
          title: 'A population or subspecies occupying a distinct habitat'
        isolate:
          type: string
          title: 'The individual isolate from which the sequences in the genome assembly were derived'
        sex:
          type: string
          title: 'Physical sex of sampled organism'
        strain:
          type: string
          title: 'A genetic variant, subtype or culture within a species'
    v2reportsIsolate:
      type: object
      properties:
        name:
          type: string
          title: 'BioSample harmonized attribute names https://www.ncbi.nlm.nih.gov/biosample/docs/attributes/'
        source:
          type: string
          title: 'Source material from which the viral specimen was isolated'
        collection_date:
          type: string
          title: 'The collection date for the sample from which the viral nucleotide sequence was derived'
    v2reportsJournal:
      type: object
      properties:
        pmid:
          type: integer
          title: 'PubMed identifier for this journal article.'
        pmcid:
          type: string
          title: 'PubMed Central identifier for this journal article.'
        doi:
          type: string
          title: 'Digital Object Identifier for this journal article.'
        date:
          type: string
          title: 'Publication date of the journal article.'
        journal_name:
          type: string
          title: 'Name of the journal in which the article was published.'
        volume:
          type: string
          title: 'Volume number of the journal issue.'
        issue:
          type: string
          title: 'Issue number of the journal volume.'
        pages:
          type: string
          title: 'Page range within the journal issue.'
        authors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAuthor'
    v2reportsLineageOrganism:
      type: object
      properties:
        tax_id:
          type: integer
          title: 'NCBI Taxonomy identifier'
        name:
          type: string
          title: 'Scientific name'
    v2reportsLinkedAssembly:
      type: object
      properties:
        linked_assembly:
          type: string
          title: 'The linked assembly accession'
        assembly_type:
          $ref: '#/components/schemas/v2reportsLinkedAssemblyType'
          title: 'The linked assembly type'
    v2reportsMapLocation:
      type: object
      properties:
        map_type:
          $ref: '#/components/schemas/v2reportsMapLocationMapType'
          title: 'Type of mapping; can be "Cytogenetic" or "Genetic"'
        map_value:
          type: string
          title: 'Location of the gene in mapping coordinates'
    v2reportsMaturePeptide:
      type: object
      properties:
        accession_version:
          type: string
        name:
          type: string
        length:
          type: integer
    v2reportsMedline:
      type: object
      properties:
        medline_uid:
          type: string
        entry_month:
          type: string
        article:
          $ref: '#/components/schemas/v2reportsArticle'
        pmid:
          type: integer
        status:
          $ref: '#/components/schemas/v2reportsMedlineStatus'
    v2reportsMeeting:
      type: object
      properties:
        number:
          type: integer
          title: 'Meeting number or edition identifier.'
        date:
          type: string
          title: 'Date of the meeting.'
        place:
          type: string
          title: 'Location where the meeting took place.'
    v2reportsMessage:
      type: object
      properties:
        error:
          $ref: '#/components/schemas/v2reportsError'
        warning:
          $ref: '#/components/schemas/v2reportsWarning'
    v2reportsNameAndAuthority:
      type: object
      properties:
        name:
          type: string
          title: 'This could be the scientific name, common name, synonym, etc. depending on the context.'
        authority:
          type: string
          title: 'The authority that this name was created by. The authority is typically represented by the author(s) name and the year in which it was published.'
        type_strains:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTaxonomyTypeMaterial'
        curator_synonym:
          type: string
          title: 'The primary synonym of the scientific name.'
        homotypic_synonyms:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsNameAndAuthority'
        heterotypic_synonyms:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsNameAndAuthority'
        other_synonyms:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsNameAndAuthority'
        informal_names:
          type: array
          items:
            type: string
        basionym:
          $ref: '#/components/schemas/v2reportsNameAndAuthority'
          title: 'The originally described name, no longer in use. Attached to the type material and species description.'
        publications:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsNameAndAuthorityPublication'
        notes:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsNameAndAuthorityNote'
        formal:
          type: boolean
          title: 'Indicates whether the name is formal (i.e. compliant)'
    v2reportsNameAndAuthorityNote:
      type: object
      properties:
        name:
          type: string
          title: 'Name of the notation.'
        note:
          type: string
          title: 'Note text.'
        note_classifier:
          $ref: '#/components/schemas/v2reportsNameAndAuthorityNoteClassifier'
          title: 'Note classification'
    v2reportsNameAndAuthorityPublication:
      type: object
      properties:
        name:
          type: string
          title: 'Name of the publication (article, book, etc.).'
        citation:
          type: string
          title: 'Citation to the publication.'
        pmid:
          type: string
          format: uint64
    v2reportsNomenclatureAuthority:
      type: object
      properties:
        authority:
          type: string
          title: 'The nomenclature authority for this gene record'
        identifier:
          type: string
          title: 'The nomenclature authority identifier for this gene record'
    v2reportsOrganelle:
      type: object
      properties:
        description:
          $ref: '#/components/schemas/v2reportsOrganelleType'
        genbank:
          $ref: '#/components/schemas/v2reportsSequenceInformation'
        refseq:
          $ref: '#/components/schemas/v2reportsSequenceInformation'
        organism:
          $ref: '#/components/schemas/v2reportsOrganism'
          title: 'Taxon for the organelle'
        bioprojects:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsBioProject'
        biosample:
          $ref: '#/components/schemas/v2reportsOrganelleBiosample'
          title: 'The associated BioSample.'
        gene_counts:
          $ref: '#/components/schemas/v2reportsOrganelleGeneCounts'
        length:
          type: integer
          title: 'Genome length'
        topology:
          $ref: '#/components/schemas/v2reportsOrganelleTopology'
        gene_count:
          type: integer
    v2reportsOrganelleBiosample:
      type: object
      properties:
        accession:
          type: string
          title: 'BioSample Accession identifier'
    v2reportsOrganelleDataReports:
      type: object
      properties:
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsOrganelle'
        total_count:
          type: integer
          title: 'The total count of available datasets (ignoring the cutoff parameter). Only provided for the first page of results (when `page_token` is empty in the request).'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2reportsOrganelleGeneCounts:
      type: object
      properties:
        total:
          type: integer
          title: 'Total number of annotated genes'
        protein_coding:
          type: integer
          title: 'Count of annotated genes that encode a protein'
        rrna:
          type: integer
          title: 'Count of ribosomal RNAs'
        trna:
          type: integer
          title: 'Count of transfer RNAs'
        lncrna:
          type: integer
          title: 'Count of long, non-coding RNAs'
    v2reportsOrganelleInfo:
      type: object
      properties:
        assembly_name:
          type: string
          title: 'Name of associated nuclear assembly'
        infraspecific_name:
          type: string
          title: 'The strain, breed, cultivar or ecotype of the organism from which the sequences in the assembly were derived'
        bioproject:
          type: array
          items:
            type: string
        description:
          type: string
          title: 'Long description of the organelle genome'
        total_seq_length:
          type: string
          format: uint64
          title: 'Sequence length of the organelle genome'
        submitter:
          type: string
          title: 'Name of submitter'
    v2reportsOrganism:
      type: object
      properties:
        tax_id:
          type: integer
          title: 'NCBI Taxonomy identifier'
        sci_name:
          type: string
          title: 'Scientific name'
        organism_name:
          type: string
          title: 'Scientific name'
        common_name:
          type: string
          title: 'Common name'
        lineage:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsLineageOrganism'
        strain:
          type: string
        pangolin_classification:
          type: string
        infraspecific_names:
          $ref: '#/components/schemas/v2reportsInfraspecificNames'
    v2reportsPairedAssembly:
      type: object
      properties:
        accession:
          type: string
          title: 'The GenColl assembly accession of the GenBank or RefSeq assembly paired with this one'
        status:
          $ref: '#/components/schemas/v2reportsAssemblyStatus'
          title: 'GenColl Assembly status from paired record'
        annotation_name:
          type: string
          title: 'Annotation name from paired record'
        only_genbank:
          type: string
          title: 'Sequences that are only included in the GenBank assembly'
        only_refseq:
          type: string
          title: 'Sequences that are only included in the RefSeq assembly'
        changed:
          type: string
          title: 'Sequences present on both the GenBank and the RefSeq assemblies that have been changed, e.g., contaminated sequence in the GenBank assembly has been replaced with a gap'
        manual_diff:
          type: string
          title: 'Manually curated description of differences between the GenBank and RefSeq assemblies'
        refseq_genbank_are_different:
          type: boolean
          title: 'boolean indication on whether there are any differences between the GenBank and RefSeq assemblies'
        differences:
          type: string
          title: 'Concatenation of all differences between the GenBank and RefSeq assemblies, including manually curated description and other fields'
    v2reportsPatent:
      type: object
      properties:
        patent_id:
          type: string
          title: 'Patent identifier'
        title:
          type: string
        authors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsAuthor'
        claim_no:
          type: integer
        issued_date:
          type: string
          title: 'Publication date'
        assignee:
          type: string
    v2reportsProceedings:
      type: object
      properties:
        meeting:
          $ref: '#/components/schemas/v2reportsMeeting'
        book:
          $ref: '#/components/schemas/v2reportsBook'
    v2reportsProcessMetadata:
      type: object
      properties:
        name:
          type: string
          title: 'Gene ontology term name'
        go_id:
          type: string
          title: 'Gene ontology identifier'
        evidence_code:
          type: string
          title: 'Indicates how the annotation is supported'
        qualifier:
          type: string
          title: 'Explicitly link gene products to GO terms'
        reference:
          $ref: '#/components/schemas/v2reportsReference'
          title: 'Source of evidence supporting the GO annotation'
    v2reportsProductDescriptor:
      type: object
      properties:
        gene_id:
          type: string
          format: uint64
          title: 'NCBI Gene ID'
        symbol:
          type: string
          title: 'gene symbol'
        description:
          type: string
          title: 'gene name'
        tax_id:
          type: string
          format: uint64
          title: 'NCBI Taxonomy ID for the organism'
        taxname:
          type: string
          title: 'Taxonomic name of the organism'
        common_name:
          type: string
          title: 'Common name of the organism'
        type:
          $ref: '#/components/schemas/v2reportsGeneType'
          title: 'Type of gene'
        rna_type:
          $ref: '#/components/schemas/v2reportsRnaType'
          title: 'Type of RNA'
        transcripts:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTranscript'
        transcript_count:
          type: integer
          title: 'Number of transcripts encoded by the gene'
        protein_count:
          type: integer
          title: 'Number of proteins encoded by the gene'
        transcript_type_counts:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTranscriptTypeCount'
    v2reportsProtein:
      type: object
      properties:
        accession_version:
          type: string
          title: 'RefSeq protein accession with version'
        name:
          type: string
          title: 'Protein name'
        length:
          type: integer
          title: 'Protein length in amino acids'
        isoform_name:
          type: string
          title: 'Protein isoform name'
        ensembl_protein:
          type: string
          title: 'Ensembl protein accession with version'
        mature_peptides:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMaturePeptide'
    v2reportsProteinConservedDomain:
      type: object
      properties:
        accession:
          type: string
          title: 'Domain family or superfamily accession, like ''cd01234'' or ''cl01234'''
        name:
          type: string
          title: 'The short name of the domain family'
        description:
          type: string
          title: 'The description for the domain/superfamily (defline)'
        start:
          type: integer
          title: 'The start coordinate of the annotation on the query sequence'
        stop:
          type: integer
          title: 'The stop coordination (inclusive) of the annotation on the query protein'
        specific:
          type: boolean
          title: 'Whether this hit is specific for a particular model'
        partial:
          type: boolean
          title: 'Whether this domain is fully aligned to the protein'
        evalue:
          type: number
          format: float
          title: 'The Expect Value of the alignment between query sequence and the domain consensus'
        bit_score:
          type: number
          format: float
          title: 'The bit_score of the alignment between query sequence and the domain consensus'
    v2reportsProteinDataReport:
      type: object
      properties:
        accession:
          type: string
          title: 'Protein accession, as input'
        description:
          type: string
          title: 'Protein name as in defline'
        length:
          type: integer
          title: 'Length of the protein'
        gene_id:
          type: integer
          title: 'Gene ID associated with the protein'
        identical_protein_group:
          type: integer
          title: 'Non-redundent protein group ID, aka IPG/PIG'
        tax_id:
          type: integer
          title: 'Taxonomy ID'
        conserved_domains:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsProteinConservedDomain'
        functional_sites:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsFunctionalSite'
        protein_families:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsProteinFamily'
    v2reportsProteinDataReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsProteinDataReport'
    v2reportsProteinFamily:
      type: object
      properties:
        method:
          type: string
          title: 'The method used to assign the protein family'
        identifier:
          type: integer
          title: 'The numeric ID of the model assigned to the protein using the particular method'
        name:
          type: string
          title: 'The name of the assigned model'
        description:
          type: string
          title: 'The more detailed description of the assigned model'
        type:
          type: string
          title: 'Sub-category specific to this method'
    v2reportsRange:
      type: object
      properties:
        begin:
          type: string
          format: uint64
          title: 'Sequence start position'
        end:
          type: string
          format: uint64
          title: 'Sequence stop position'
        orientation:
          $ref: '#/components/schemas/v2reportsOrientation'
          title: 'Direction relative to the genome'
        order:
          type: integer
          title: 'The position of this sequence in a group of sequences'
        ribosomal_slippage:
          type: integer
          title: 'When ribosomal slippage is desired, fill out slippage amount between this and previous range.'
    v2reportsReference:
      type: object
      properties:
        pmids:
          type: array
          items:
            type: string
            format: uint64
    v2reportsSampleInfo:
      type: object
      properties:
        clone:
          type: string
          title: 'Name for the clone or subculture from which the sample was taken.'
        subclone:
          type: string
          title: 'Name for the derived clone or subculture from which the sample was taken.'
        haplotype:
          type: string
          title: 'Name of the haplotype.'
        genotype:
          type: string
          title: 'Observed genotype.'
        sex:
          type: string
          title: 'Physical sex of sampled organism.'
        cell_line:
          type: string
          title: 'Name of the cell line.'
        cell_type:
          type: string
          title: 'Type of cell of the sample or from which the sample was obtained.'
        tissue:
          type: string
          title: 'Tissue from which the sample was derived.'
        clone_lib:
          type: string
          title: 'Formal identifier that points to source institute and clone library identifier.'
        dev_stage:
          type: string
          title: 'Developmental stage at the time of sampling.'
        lab_host:
          type: string
          title: 'Scientific name and description of the laboratory host used to propagate the source organism or material from which the sample was obtained.'
        tissue_lib:
          type: string
          title: 'Formal identifier that points to source institute and tissue library identifier.'
        geo_loc_name:
          type: string
          title: 'Geographical origin of the sample.'
        environmental_sample:
          type: boolean
          title: 'True when the sample is environmental.'
        isolation_source:
          type: string
          title: 'Source of the sample.'
        lat_lon:
          type: string
          title: 'Geographic coordinates of the location where the sample was collected.'
        collection_date:
          type: string
          title: 'Date on which the sample was collected.'
        collected_by:
          type: string
          title: 'Name of persons or institute who collected the sample.'
        identified_by:
          type: string
          title: 'Name of the taxonomist who identified the specimen.'
        metagenomic:
          type: boolean
          title: 'True when the sample is metagenomic.'
        mating_type:
          type: string
          title: 'Mating type of the sampled organism.'
        phenotype:
          type: string
          title: 'Phenotype of sampled organism.'
        altitude:
          type: string
          title: 'The altitude of the sample above sea level.'
        metagenome_name:
          type: string
          title: 'Name of the metagenome.'
        metagenome_taxid:
          type: integer
          title: 'NCBI taxonomy identifier for the metagenome.'
    v2reportsSeqRangeSet:
      type: object
      properties:
        accession_version:
          type: string
          title: 'NCBI Accession.version of the sequence'
        range:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRange'
    v2reportsSeqRangeSetFasta:
      type: object
      properties:
        seq_id:
          type: string
          title: 'Seq_id may include location info in addition to a sequence accession'
        accession_version:
          type: string
          title: 'Accession and version of the viral nucleotide sequence'
        title:
          type: string
        sequence_hash:
          type: string
          title: 'Unique identifier for identical sequences'
        range:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsRange'
    v2reportsSequenceDataReport:
      type: object
      properties:
        accession:
          type: string
          title: 'Sequence accession requested by the user (typically accession.version).'
        organism_name:
          type: string
          title: 'Scientific organism name associated with this sequence.'
        length:
          type: integer
          title: 'Sequence length measured in the units field.'
        units:
          type: string
          title: 'Units for sequence length, for example "nucleotides" or "amino acids".'
        molecule_type:
          type: string
          title: 'Molecule type such as "ss-RNA", "mRNA", or "protein".'
        database_provider:
          type: string
          title: 'Upstream source database for this record, for example "RefSeq".'
        description:
          type: string
          title: 'Human-readable sequence definition line.'
        source_mrna:
          type: string
          title: 'Source mRNA accession when this report describes a protein sequence.'
        tax_id:
          type: integer
          title: 'NCBI Taxonomy identifier for this rank.'
        submissions:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsSubmission'
        references:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsSequenceReference'
        bioproject_accession:
          type: string
          title: 'NCBI BioProject accession linked to this sequence record.'
        biosample_accessions:
          type: array
          items:
            type: string
        origin_type:
          $ref: '#/components/schemas/v2reportsSequenceDataReportOriginType'
          title: 'Biological context regarding how the material for the sequence originated.'
        specimen_voucher:
          $ref: '#/components/schemas/v2reportsSpecimenVoucher'
          title: 'Specimen voucher information for this sequence record.'
        infraspecific_names:
          $ref: '#/components/schemas/v2reportsInfraspecificName'
          title: 'Infraspecific Names'
        sample_info:
          $ref: '#/components/schemas/v2reportsSampleInfo'
          title: 'Sample Info'
        genome_type:
          $ref: '#/components/schemas/v2reportsSequenceDataReportGenomeType'
          title: 'Genome type for this sequence record.'
        errors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsError'
    v2reportsSequenceDataReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsSequenceDataReport'
        total_count:
          type: integer
          title: 'Total number of reports available across all pages.'
    v2reportsSequenceInfo:
      type: object
      properties:
        assembly_accession:
          type: string
          title: 'The genome assembly accession'
        chr_name:
          type: string
          title: 'The name of the associated chromosome. The name "Un" indicates that the chromosome is unknown.'
        ucsc_style_name:
          type: string
          title: 'Name ascribed to this sequence by the UC Santa Cruz genome browser'
        sort_order:
          type: integer
          title: 'A sort order value assigned to the sequence'
        assigned_molecule_location_type:
          type: string
          title: 'The type of molecule represented by the sequence'
        refseq_accession:
          type: string
          title: 'The RefSeq accession of the sequence'
        assembly_unit:
          type: string
          title: 'Name of the assembly unit, or set of sequences that comprise the assembly.'
        length:
          type: integer
          title: 'The length of the sequence in nucleotides'
        genbank_accession:
          type: string
          title: 'The GenBank accession of the sequence'
        gc_count:
          type: string
          format: uint64
          title: 'The number of GC base-pairs in the chromosome'
        gc_percent:
          type: number
          format: float
          title: 'The percentage of GC base-pairs in the chromosome'
        unlocalized_count:
          type: integer
          title: 'Number of scaffolds that are unlocalized on a given chromosome'
        assembly_unplaced_count:
          type: integer
          title: 'Number of unplaced scaffolds for a given assembly accession'
        role:
          type: string
        sequence_name:
          type: string
          title: 'The sequence name'
    v2reportsSequenceInformation:
      type: object
      properties:
        accession:
          type: string
          title: 'The accession.version of the organelle genomic nucleotide sequence.'
        submission_date:
          type: string
          title: 'Date record was submitted to GenBank or Curated into RefSeq (ISO 8601)'
        submitter:
          type: string
          title: 'The submitter, e.g. NCBI Genome Project for RefSeq'
    v2reportsSequenceReference:
      type: object
      properties:
        article:
          $ref: '#/components/schemas/v2reportsArticle'
          title: 'Publication metadata for this sequence record.'
        journal:
          $ref: '#/components/schemas/v2reportsJournal'
        book:
          $ref: '#/components/schemas/v2reportsBook'
        proceedings:
          $ref: '#/components/schemas/v2reportsProceedings'
        thesis:
          $ref: '#/components/schemas/v2reportsThesis'
        patent:
          $ref: '#/components/schemas/v2reportsPatent'
        general:
          $ref: '#/components/schemas/v2reportsGeneral'
        medline:
          $ref: '#/components/schemas/v2reportsMedline'
    v2reportsSpecimenVoucher:
      type: object
      properties:
        id:
          type: string
          title: 'Specimen voucher identifier.'
        biocollection_code:
          type: string
          title: 'Institution code for the specimen voucher.'
    v2reportsSubmission:
      type: object
      properties:
        date:
          type: string
          title: 'Submission date'
        institution:
          type: string
          title: 'Submitting institution or organization.'
        address:
          type: string
          title: 'Submitter mailing address or contact address.'
        authors:
          type: array
          items:
            type: string
    v2reportsTaxData:
      type: object
      properties:
        name:
          type: string
          title: 'Taxonomic name'
        id:
          type: integer
          title: 'NCBI Taxonomy identifier'
    v2reportsTaxonomyDataReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTaxonomyReportMatch'
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
        total_count:
          type: integer
          title: 'The total count of available taxons - deprecated because it not supported for multiple-page results.'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2reportsTaxonomyNamesDataReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTaxonomyNamesReportMatch'
        messages:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsMessage'
        total_count:
          type: integer
          title: 'The total count of available taxons - deprecated because it not supported for multiple-page results.'
        next_page_token:
          type: string
          title: 'A page token.  A token that can be used in a future request as `page_token` to retrieve the next page. If this field is emtpy then there are no subsequent pages available.'
    v2reportsTaxonomyNamesDescriptor:
      type: object
      properties:
        tax_id:
          type: string
          format: uint64
          title: 'NCBI Taxonomy identifier'
        rank:
          $ref: '#/components/schemas/v2reportsRankType'
          title: 'The taxonomic rank of the taxonomic node.'
        current_scientific_name:
          $ref: '#/components/schemas/v2reportsNameAndAuthority'
          title: 'The currently accepted name chosen out of all synonyms for the taxonomic node.'
        group_name:
          type: string
          title: 'Group name for the species.'
        curator_common_name:
          type: string
          title: 'The canonical common name.'
        other_common_names:
          type: array
          items:
            type: string
        general_notes:
          type: array
          items:
            type: string
        links_from_type:
          type: string
          title: 'TBD'
        citations:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTaxonomyNamesDescriptorCitation'
        current_scientific_name_is_formal:
          type: boolean
          title: 'Specify if the current scientific name is considered ''formal'' or not.'
    v2reportsTaxonomyNamesDescriptorCitation:
      type: object
      properties:
        full_citation:
          type: string
        short_citation:
          type: string
        pmid:
          type: string
          format: uint64
    v2reportsTaxonomyNamesReportMatch:
      type: object
      properties:
        taxonomy:
          $ref: '#/components/schemas/v2reportsTaxonomyNamesDescriptor'
          title: 'Detailed taxonomic information related to the requested node.'
        query:
          type: array
          items:
            type: string
        warning:
          $ref: '#/components/schemas/v2reportsWarning'
          title: 'Warnings associated with this request.  Any warning associated with this request.  This is represented by a Warning object which is separately documented.'
        errors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsError'
    v2reportsTaxonomyNode:
      type: object
      properties:
        tax_id:
          type: integer
          title: 'NCBI Taxonomy identifier'
        rank:
          $ref: '#/components/schemas/v2reportsRankType'
          title: 'The taxonomic rank of the taxonomic node.'
        current_scientific_name:
          $ref: '#/components/schemas/v2reportsNameAndAuthority'
          title: 'The currently accepted name chosen out of all synonyms for the taxonomic node.'
        basionym:
          $ref: '#/components/schemas/v2reportsNameAndAuthority'
          title: 'The originally described name, no longer in use. Attached to the type material and species description.'
        curator_common_name:
          type: string
          title: 'The canonical common name.'
        group_name:
          type: string
          title: 'A common name describing large, well-known taxa.'
        has_type_material:
          type: boolean
          title: 'A boolean that indicates whether or not type material is available for the species.'
        classification:
          $ref: '#/components/schemas/v2reportsClassification'
          title: 'A subset of parent nodes including well-established ranks.'
        parents:
          type: array
          items:
            type: integer
        children:
          type: array
          items:
            type: integer
        counts:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsTaxonomyNodeCountByType'
        genomic_moltype:
          type: string
          title: 'Genomic molecule type (dsDNA, ssDNA, ssDNA(-), ssRNA)'
        current_scientific_name_is_formal:
          type: boolean
          title: 'Specify if the current scientific name is considered ''formal'' or not.'
        secondary_tax_ids:
          type: array
          items:
            type: integer
        extinct:
          type: boolean
          title: 'True if organism is extinct'
    v2reportsTaxonomyNodeCountByType:
      type: object
      properties:
        type:
          $ref: '#/components/schemas/v2reportsCountType'
        count:
          type: integer
    v2reportsTaxonomyReportMatch:
      type: object
      properties:
        taxonomy:
          $ref: '#/components/schemas/v2reportsTaxonomyNode'
        query:
          type: array
          items:
            type: string
        warning:
          $ref: '#/components/schemas/v2reportsWarning'
        errors:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsError'
    v2reportsTaxonomyTypeMaterial:
      type: object
      properties:
        type_strain_name:
          type: string
          title: 'The strain name of the type material.'
        type_strain_id:
          type: string
          title: 'The strain ID of the type material.'
        bio_collection_id:
          type: string
          title: 'The biocollection ID of the type material.'
        bio_collection_name:
          type: string
          title: 'The biocollection name of the type material.'
        collection_type:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsCollectionType'
        type_class:
          type: string
          title: 'Type material classification.'
    v2reportsThesis:
      type: object
      properties:
        book:
          $ref: '#/components/schemas/v2reportsBook'
          title: 'Book-style bibliographic metadata for this thesis/manuscript/letter citation.'
        id:
          type: string
          title: 'Identifier associated with the citation record.'
        classification_type:
          $ref: '#/components/schemas/v2reportsThesisType'
          title: 'Classification of this citation entry.'
    v2reportsTranscript:
      type: object
      properties:
        accession_version:
          type: string
          title: 'RefSeq transcript accession with version'
        name:
          type: string
          title: 'RefSeq transcript name'
        length:
          type: integer
          title: 'RefSeq transcript length in nucleotides'
        cds:
          $ref: '#/components/schemas/v2reportsSeqRangeSet'
        genomic_locations:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsGenomicLocation'
        ensembl_transcript:
          type: string
          title: 'Ensembl transcript accession with version'
        protein:
          $ref: '#/components/schemas/v2reportsProtein'
        type:
          $ref: '#/components/schemas/v2reportsTranscriptTranscriptType'
          title: 'Indicates transcript protein-coding potential and whether it was computationally predicted'
        select_category:
          $ref: '#/components/schemas/v2reportsTranscriptSelectCategory'
    v2reportsTranscriptTypeCount:
      type: object
      properties:
        type:
          $ref: '#/components/schemas/v2reportsTranscriptTranscriptType'
          title: 'Type of transcript'
        count:
          type: integer
          title: 'Number of transcripts of a particular type'
    v2reportsTypeMaterial:
      type: object
      properties:
        type_label:
          type: string
        type_display_text:
          type: string
    v2reportsVirusAnnotationReport:
      type: object
      properties:
        accession:
          type: string
        isolate_name:
          type: string
        genes:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsVirusGene'
    v2reportsVirusAnnotationReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsVirusAnnotationReport'
        total_count:
          type: integer
          title: 'The total count of available assemblies (ignoring the page_size parameter).'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2reportsVirusAssembly:
      type: object
      properties:
        accession:
          type: string
          title: 'The accession.version of the viral nucleotide sequence. Includes both GenBank and RefSeq accessions'
        is_complete:
          type: boolean
        is_annotated:
          type: boolean
          title: 'The viral genome has been annotated by either the submitter (GenBank) or by NCBI (RefSeq)'
        isolate:
          $ref: '#/components/schemas/v2reportsIsolate'
        source_database:
          type: string
          title: 'Indicates if the source of the viral nucleotide record is from a GenBank submitter or from NCBI-derived curation (RefSeq)'
        protein_count:
          type: integer
          title: 'The total count of annotated proteins including both proteins and polyproteins but not processed mature peptides'
        host:
          $ref: '#/components/schemas/v2reportsOrganism'
          title: 'Taxon from which the virus sample was isolated'
        virus:
          $ref: '#/components/schemas/v2reportsOrganism'
          title: 'Viral taxon'
        bioprojects:
          type: array
          items:
            type: string
        location:
          $ref: '#/components/schemas/v2reportsVirusAssemblyCollectionLocation'
        update_date:
          type: string
          title: 'Date the viral nucleotide accession was last updated in NCBI Virus'
        release_date:
          type: string
          title: 'Date the viral nucleotide accession was first released in NCBI Virus'
        nucleotide_completeness:
          type: string
          title: 'Value describing if the of the viral nucleotide sequence represents a complete or partial genome'
        completeness:
          $ref: '#/components/schemas/v2reportsVirusAssemblyCompleteness'
          title: 'Indicates whether the viral nucleotide sequence represents a complete or partial genome'
        length:
          type: integer
          title: 'Length of the viral nucleotide sequence'
        gene_count:
          type: integer
          title: 'Total count of genes annotated on the viral nucleotide sequence'
        mature_peptide_count:
          type: integer
          title: 'Total count of processed mature peptides annotated on the viral nucleotide sequence'
        biosample:
          type: string
          title: 'Associated Biosample accessions'
        mol_type:
          type: string
          title: 'ICTV (International Committee on Taxonomy of Viruses) viral classification based on nucleic acid composition, strandedness and method of replication'
        nucleotide:
          $ref: '#/components/schemas/v2reportsSeqRangeSetFasta'
          title: 'The whole genomic nucleotide record of the CDS feature.'
        purpose_of_sampling:
          $ref: '#/components/schemas/v2reportsPurposeOfSampling'
          title: 'SARS-CoV-2 only, indicates whether the sequence was collected randomly for epedimiology studies'
        sra_accessions:
          type: array
          items:
            type: string
        submitter:
          $ref: '#/components/schemas/v2reportsVirusAssemblySubmitterInfo'
          title: 'Name, affiliation, and country of the submitter(s)'
        lab_host:
          type: string
          title: 'This sequence is from viruses passaged in this host'
        is_lab_host:
          type: boolean
          title: 'If true, this sequence is from viruses passaged in a laboratory'
        is_vaccine_strain:
          type: boolean
          title: 'If true, this sequence is derived from a virus used as a vaccine or potential vaccine'
        segment:
          type: string
          title: 'The virus segment'
    v2reportsVirusAssemblyCollectionLocation:
      type: object
      properties:
        geographic_location:
          type: string
          title: 'Country of virus specimen collection'
        geographic_region:
          type: string
          title: 'Region of virus specimen collection'
        usa_state:
          type: string
          title: 'Two letter abbreviation of the state of the virus specifime collection (if United States)'
    v2reportsVirusAssemblySubmitterInfo:
      type: object
      properties:
        names:
          type: array
          items:
            type: string
        affiliation:
          type: string
          title: 'The submitter''s organization and/or institution'
        country:
          type: string
          title: 'The country representing the submitter''s affilation'
    v2reportsVirusDataReportPage:
      type: object
      properties:
        reports:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsVirusAssembly'
        total_count:
          type: integer
          title: 'The total count of available assemblies (ignoring the page_size parameter).'
        next_page_token:
          type: string
          title: 'A token that can be sent as `page_token` to retrieve the next page. If this field is omitted, there are no subsequent pages.'
    v2reportsVirusGene:
      type: object
      properties:
        name:
          type: string
        gene_id:
          type: integer
        nucleotide:
          $ref: '#/components/schemas/v2reportsSeqRangeSetFasta'
          title: 'The interval on the genomic nucleotide record of the CDS feature.'
        cds:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsVirusPeptide'
    v2reportsVirusPeptide:
      type: object
      properties:
        accession:
          type: string
          title: 'Protein accession and version'
        name:
          type: string
          title: 'Protein name'
        other_names:
          type: array
          items:
            type: string
        nucleotide:
          $ref: '#/components/schemas/v2reportsSeqRangeSetFasta'
          title: 'The interval on the genomic nucleotide record of this mature-peptide feature'
        protein:
          $ref: '#/components/schemas/v2reportsSeqRangeSetFasta'
          title: 'The full polyprotein record or interval on the polyprotein for mature-peptide features'
        pdb_ids:
          type: array
          items:
            type: string
        cdd:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsConservedDomain'
        uni_prot_kb:
          $ref: '#/components/schemas/v2reportsVirusPeptideUniProtId'
          title: 'UniProt identifier'
        mature_peptide:
          type: array
          items:
            $ref: '#/components/schemas/v2reportsVirusPeptide'
        protein_completeness:
          $ref: '#/components/schemas/v2reportsVirusPeptideViralPeptideCompleteness'
          title: 'Protein completeness'
    v2reportsVirusPeptideUniProtId:
      type: object
      properties:
        id:
          type: string
          title: 'UniProt ID'
        name:
          type: string
          title: 'UniProt name'
    v2reportsWGSInfo:
      type: object
      properties:
        wgs_project_accession:
          type: string
          title: 'WGS project accession'
        master_wgs_url:
          type: string
          title: 'WGS project Nucleotide web address'
        wgs_contigs_url:
          type: string
          title: 'WGS project Sequence set browser web address'
    v2reportsWarning:
      type: object
      properties:
        gene_warning_code:
          $ref: '#/components/schemas/v2reportsWarningGeneWarningCode'
        reason:
          type: string
        message:
          type: string
        replaced_id:
          $ref: '#/components/schemas/v2reportsWarningReplacedId'
        unrecognized_identifier:
          type: string
    v2reportsWarningReplacedId:
      type: object
      properties:
        requested:
          type: string
        returned:
          type: string
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
