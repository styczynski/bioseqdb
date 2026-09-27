/**
 * @file external/ncbi/value_schemas.cppm
 * @brief Named options and status types used by the NCBI API.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:value_schemas;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Allowed option values, protobuf Any values, and RPC error status. */
inline constexpr std::string_view value_schemas = R"bioseq_openapi(    v2AnnotationForAssemblyType:
      type: string
      enum:
        - DEFAULT
        - GENOME_GFF
        - GENOME_GBFF
        - RNA_FASTA
        - PROT_FASTA
        - GENOME_GTF
        - CDS_FASTA
        - GENOME_FASTA
        - SEQUENCE_REPORT
      default: DEFAULT
    
    v2AnnotationForOrganelleType:
      type: string
      enum:
        - DEFAULT
        - GENOME_FASTA
        - CDS_FASTA
        - PROTEIN_FASTA
      default: DEFAULT
    
    v2AssemblyDatasetDescriptorsFilterAssemblySource:
      type: string
      enum:
        - all
        - refseq
        - genbank
      default: all
    
    v2AssemblyDatasetDescriptorsFilterAssemblyVersion:
      type: string
      enum:
        - current
        - all_assemblies
      default: current
      title: 'The assembly status - current (latest), or all assemblies, which adds replaced and suppressed'
    
    v2AssemblyDatasetDescriptorsFilterMetagenomeDerivedFilter:
      type: string
      enum:
        - METAGENOME_DERIVED_UNSET
        - metagenome_derived_only
        - metagenome_derived_exclude
      default: METAGENOME_DERIVED_UNSET
    
    v2AssemblyDatasetDescriptorsFilterTypeMaterialCategory:
      type: string
      enum:
        - NONE
        - TYPE_MATERIAL
        - TYPE_MATERIAL_CLADE
        - TYPE_MATERIAL_NEOTYPE
        - TYPE_MATERIAL_REFTYPE
        - PATHOVAR_TYPE
        - TYPE_MATERIAL_SYN
      default: NONE
    
    v2AssemblyDatasetReportsRequestContentType:
      type: string
      enum:
        - COMPLETE
        - ASSM_ACC
        - PAIRED_ACC
      default: COMPLETE
    
    v2AssemblyDatasetRequestResolution:
      type: string
      enum:
        - FULLY_HYDRATED
        - DATA_REPORT_ONLY
      default: FULLY_HYDRATED
    
    v2AssemblyLinksReplyAssemblyLinkType:
      type: string
      enum:
        - DEFAULT
        - GDV_LINK
        - FTP_LINK
        - ASSEMBLY_PUBMED
        - BLAST_LINK
        - ASSEMBLY_NUCCORE_REFSEQ
        - ASSEMBLY_NUCCORE_GENBANK
        - CGV_LINK
      default: DEFAULT
      title: 'Types of assembly links that may be returned'
    
    v2CatalogApiVersion:
      type: string
      enum:
        - UNKNOWN
        - V1
        - V2
      default: UNKNOWN
    
    v2Fasta:
      type: string
      enum:
        - FASTA_UNSPECIFIED
        - FASTA_GENE
        - FASTA_RNA
        - FASTA_PROTEIN
        - FASTA_GENE_FLANK
        - FASTA_CDS
        - FASTA_5P_UTR
        - FASTA_3P_UTR
      default: FASTA_UNSPECIFIED
    
    v2FileFileType:
      type: string
      enum:
        - UNKNOWN
        - FASTA
        - GFF3
        - DATA_REPORT
        - ANNOTATION_REPORT
        - GENOMIC_NUCLEOTIDE_FASTA
        - PROTEIN_FASTA
        - GENBANK_FLAT_FILE
        - GENPEPT_FLAT_FILE
        - README
        - PDB_FILE
        - CDS_NUCLEOTIDE_FASTA
        - RNA_NUCLEOTIDE_FASTA
        - DATA_TABLE
        - SEQUENCE_REPORT
        - GTF
        - PROKARYOTE_GENE_DATA_REPORT
        - PROKARYOTE_GENE_LOCATION_DATA_REPORT
        - GENOMIC_NUCLEOTIDE_WITH_FLANK_FASTA
        - BIOSAMPLE_REPORT
        - CATALOG
        - TAXONOMY_NAMES_REPORT
      default: UNKNOWN
    
    v2GeneDatasetReportsRequestContentType:
      type: string
      enum:
        - COMPLETE
        - IDS_ONLY
        - COUNTS_ONLY
      default: COMPLETE
      title: 'Specify a specific group of values to be returned for dataset reports. These options do not apply to product reports.'
    
    v2GeneDatasetRequestContentType:
      type: string
      enum:
        - COMPLETE
        - IDS_ONLY
      default: COMPLETE
    
    v2GeneDatasetRequestGeneDatasetReportType:
      type: string
      enum:
        - DATASET_REPORT
        - PRODUCT_REPORT
      default: DATASET_REPORT
    
    v2GeneLinksReplyGeneLinkType:
      type: string
      enum:
        - DEFAULT
        - GENE_LINK
        - GDV_LINK
        - ORTHOLOG_LINK
        - MCGV_LINK
      default: DEFAULT
      title: 'Types of gene links that may be returned'
    
    v2GeneType:
      type: string
      enum:
        - UNKNOWN
        - tRNA
        - rRNA
        - snRNA
        - scRNA
        - snoRNA
        - PROTEIN_CODING
        - PSEUDO
        - TRANSPOSON
        - miscRNA
        - ncRNA
        - BIOLOGICAL_REGION
        - OTHER
      default: UNKNOWN
      title: 'NB: GeneType values match Entrez Gene'
    
    v2GenomeAnnotationRequestAnnotationType:
      type: string
      enum:
        - DEFAULT
        - GENOME_FASTA
        - RNA_FASTA
        - PROT_FASTA
      default: DEFAULT
    
    v2GenomeAnnotationRequestGenomeAnnotationTableFormat:
      type: string
      enum:
        - NO_TABLE
        - SUMMARY
        - PRODUCT
      default: NO_TABLE
    
    v2ImageSize:
      type: string
      enum:
        - UNSPECIFIED
        - SMALL
        - MEDIUM
      default: UNSPECIFIED
    
    v2IncludeTabularHeader:
      type: string
      enum:
        - INCLUDE_TABULAR_HEADER_FIRST_PAGE_ONLY
        - INCLUDE_TABULAR_HEADER_ALWAYS
        - INCLUDE_TABULAR_HEADER_NEVER
      default: INCLUDE_TABULAR_HEADER_FIRST_PAGE_ONLY
    
    v2MicroBiggeDatasetRequestFileType:
      type: string
      enum:
        - element_fasta
        - element_flank_fasta
        - contig_fasta
        - protein_fasta
      default: FILE_TYPE_UNSPECIFIED
    
    v2MolType:
      type: string
      enum:
        - MOL_UNSPECIFIED
        - MOL_DNA
        - MOL_RNA
        - MOL_PROTEIN
        - MOL_NA
      default: MOL_UNSPECIFIED
    
    v2OrganelleMetadataRequestContentType:
      type: string
      enum:
        - COMPLETE
        - ASSM_ACC
      default: COMPLETE
    
    v2OrganelleMetadataRequestOrganelleTableFormat:
      type: string
      enum:
        - ORGANELLE_TABLE_FORMAT_NO_TABLE
        - SUMMARY
      default: ORGANELLE_TABLE_FORMAT_NO_TABLE
    
    v2OrganismQueryRequestTaxRankFilter:
      type: string
      enum:
        - species
        - higher_taxon
      default: species
    
    v2OrganismQueryRequestTaxonResourceFilter:
      type: string
      enum:
        - TAXON_RESOURCE_FILTER_ALL
        - TAXON_RESOURCE_FILTER_GENOME
        - TAXON_RESOURCE_FILTER_GENE
        - TAXON_RESOURCE_FILTER_ORGANELLE
      default: TAXON_RESOURCE_FILTER_ALL
    
    v2OrthologRequestContentType:
      type: string
      enum:
        - COMPLETE
        - IDS_ONLY
      default: COMPLETE
    
    v2RefGeneCatalogDatasetRequestFileType:
      type: string
      enum:
        - FILE_TYPE_NUCLEOTIDE
        - FILE_TYPE_NUCLEOTIDE_WITH_FLANK
        - FILE_TYPE_PROTEIN
      default: FILE_TYPE_UNSPECIFIED
    
    v2SortDirection:
      type: string
      enum:
        - SORT_DIRECTION_UNSPECIFIED
        - SORT_DIRECTION_ASCENDING
        - SORT_DIRECTION_DESCENDING
      default: SORT_DIRECTION_UNSPECIFIED
    
    v2TableFormat:
      type: string
      enum:
        - tsv
        - csv
        - jsonl
      default: tsv
    
    v2TaxonomyDatasetRequestTaxonomyReportType:
      type: string
      enum:
        - TAXONOMY_SUMMARY
        - NAMES_REPORT
      default: TAXONOMY_SUMMARY
    
    v2TaxonomyFilteredSubtreeResponseEdgeChildStatus:
      type: string
      enum:
        - UNSPECIFIED
        - HAS_MORE_CHILDREN
        - NO_VISIBLE_CHILDREN
        - ONLY_UNSPECIFIED_CHILDREN
      default: UNSPECIFIED
      title: 'HAS_MORE_CHILDREN means there are children of a valid rank that are not unspecified NO_VISIBLE_CHILDREN means there are no children at all, not even unspecified children ONLY_UNSPECIFIED_CHILDREN Means that there are children, but those children are all unspecified (e.g. environmental samples)'
    
    v2TaxonomyMetadataRequestContentType:
      type: string
      enum:
        - COMPLETE
        - TAXIDS
        - METADATA
      default: COMPLETE
    
    v2TaxonomyMetadataRequestTableFormat:
      type: string
      enum:
        - SUMMARY
      default: SUMMARY
      title: 'Optional pre-defined template for processing a tabular data request'
    
    v2ViralSequenceType:
      type: string
      enum:
        - GENOME
        - CDS
        - PROTEIN
        - NONE
      default: GENOME
    
    v2VirusDataReportRequestContentType:
      type: string
      enum:
        - COMPLETE
        - ACCESSIONS_ONLY
      default: COMPLETE
      title: 'Specify a specific group of values to be returned, currently either all fields or accessions only'
    
    v2VirusDatasetReportType:
      type: string
      enum:
        - DATASET_REPORT
        - ANNOTATION
        - BIOSAMPLE_REPORT
      default: DATASET_REPORT
    
    v2VirusTableField:
      type: string
      enum:
        - unspecified
        - nucleotide_accession
        - species_tax_id
        - species_name
        - genus
        - family
        - nucleotide_length
        - isolate_name
        - sequence_type
        - nuc_completeness
        - geo_location
        - us_state
        - host_name
        - host_tax_id
        - collection_date
        - bioproject
        - biosample
        - polyprotein_name
        - protein_name
        - protein_accession
        - protein_synonym
        - cds_span
      default: unspecified
    
    v2reportsANITypeCategory:
      type: string
      enum:
        - ANI_CATEGORY_UNKNOWN
        - claderef
        - category_na
        - neotype
        - no_type
        - pathovar
        - reftype
        - suspected_type
        - synonym
        - type
      default: ANI_CATEGORY_UNKNOWN
    
    v2reportsAssemblyLevel:
      type: string
      enum:
        - chromosome
        - scaffold
        - contig
        - complete_genome
      default: chromosome
      title: 'The level of the genome assembly: Chromosome, Scaffold, Contig or Complete Genome'
    
    v2reportsAssemblyStatus:
      type: string
      enum:
        - ASSEMBLY_STATUS_UNKNOWN
        - current
        - previous
        - suppressed
        - retired
      default: ASSEMBLY_STATUS_UNKNOWN
    
    v2reportsAverageNucleotideIdentityMatchStatus:
      type: string
      enum:
        - BEST_MATCH_STATUS_UNKNOWN
        - approved_mismatch
        - below_threshold_match
        - below_threshold_mismatch
        - best_match_status
        - derived_species_match
        - genus_match
        - low_coverage
        - mismatch
        - status_na
        - species_match
        - subspecies_match
        - synonym_match
        - lineage_match
        - below_threshold_lineage_match
      default: BEST_MATCH_STATUS_UNKNOWN
    
    v2reportsAverageNucleotideIdentityTaxonomyCheckStatus:
      type: string
      enum:
        - TAXONOMY_CHECK_STATUS_UNKNOWN
        - OK
        - Failed
        - Inconclusive
      default: TAXONOMY_CHECK_STATUS_UNKNOWN
    
    v2reportsCollectionType:
      type: string
      enum:
        - no_collection_type
        - collection_culture_collection
        - specimen_voucher
      default: no_collection_type
    
    v2reportsContentType:
      type: string
      enum:
        - COMPLETE
        - ASSM_ACC
        - PAIRED_ACC
      default: COMPLETE
    
    v2reportsCountType:
      type: string
      enum:
        - COUNT_TYPE_UNSPECIFIED
        - COUNT_TYPE_ASSEMBLY
        - COUNT_TYPE_GENE
        - COUNT_TYPE_tRNA
        - COUNT_TYPE_rRNA
        - COUNT_TYPE_snRNA
        - COUNT_TYPE_scRNA
        - COUNT_TYPE_snoRNA
        - COUNT_TYPE_PROTEIN_CODING
        - COUNT_TYPE_PSEUDO
        - COUNT_TYPE_TRANSPOSON
        - COUNT_TYPE_miscRNA
        - COUNT_TYPE_ncRNA
        - COUNT_TYPE_BIOLOGICAL_REGION
        - COUNT_TYPE_OTHER
        - COUNT_TYPE_ORGANELLE
      default: COUNT_TYPE_UNSPECIFIED
    
    v2reportsErrorAssemblyErrorCode:
      type: string
      enum:
        - UNKNOWN_ASSEMBLY_ERROR_CODE
        - INVALID_BIOPROJECT_IDS
        - NO_ASSEMBLIES_FOR_BIOPROJECTS
        - INVALID_TAXON
        - MISSING_SEARCH_FIELD
        - INVALID_BIOSAMPLE_IDS
        - NO_ASSEMBLIES_FOR_BIOSAMPLE_IDS
        - NO_ASSEMBLIES_FOR_ASSEMBLY_NAMES
        - INVALID_WGS_ACCESSIONS
        - NO_ASSEMBLIES_FOR_WGS_ACCESSIONS
      default: UNKNOWN_ASSEMBLY_ERROR_CODE
    
    v2reportsErrorGeneErrorCode:
      type: string
      enum:
        - UNKNOWN_GENE_ERROR_CODE
        - INCOMPLETE_LOOKUP_SYMBOL
        - INVALID_TAXON_GENE_ARGUMENT
      default: UNKNOWN_GENE_ERROR_CODE
    
    v2reportsErrorOrganelleErrorCode:
      type: string
      enum:
        - UNKNOWN_ORGANELLE_ERROR_CODE
        - INVALID_ORGANELLE_TAXON
        - NO_ORGANELLES_FOR_ACCESSION
      default: UNKNOWN_ORGANELLE_ERROR_CODE
    
    v2reportsErrorSequenceErrorCode:
      type: string
      enum:
        - UNKNOWN_SEQUENCE_ERROR_CODE
        - INVALID_SEQUENCE_ACCESSION
      default: UNKNOWN_SEQUENCE_ERROR_CODE
    
    v2reportsErrorTaxonomyErrorCode:
      type: string
      enum:
        - UNKNOWN_TAXONOMY_ERROR_CODE
        - INVALID_TAXONOMY_TAXON
      default: UNKNOWN_TAXONOMY_ERROR_CODE
    
    v2reportsErrorVirusErrorCode:
      type: string
      enum:
        - UNKNOWN_VIRUS_ERROR_CODE
      default: UNKNOWN_VIRUS_ERROR_CODE
    
    v2reportsGeneType:
      type: string
      enum:
        - UNKNOWN
        - tRNA
        - rRNA
        - snRNA
        - scRNA
        - snoRNA
        - PROTEIN_CODING
        - PSEUDO
        - TRANSPOSON
        - miscRNA
        - ncRNA
        - BIOLOGICAL_REGION
        - OTHER
      default: UNKNOWN
      title: 'NB: GeneType values match Entrez Gene'
    
    v2reportsGenomicRegionGenomicRegionType:
      type: string
      enum:
        - UNKNOWN
        - REFSEQ_GENE
        - PSEUDOGENE
        - BIOLOGICAL_REGION
        - OTHER
      default: UNKNOWN
    
    v2reportsLinkedAssemblyType:
      type: string
      enum:
        - LINKED_ASSEMBLY_TYPE_UNKNOWN
        - alternate_pseudohaplotype_of_diploid
        - principal_pseudohaplotype_of_diploid
        - maternal_haplotype_of_diploid
        - paternal_haplotype_of_diploid
        - haplotype_1
        - haplotype_2
        - haplotype_3
        - haplotype_4
        - haploid
      default: LINKED_ASSEMBLY_TYPE_UNKNOWN
    
    v2reportsMapLocationMapType:
      type: string
      enum:
        - Unknown
        - Cytogenetic
        - Genetic
      default: Unknown
    
    v2reportsMedlineStatus:
      type: string
      enum:
        - uknown
        - publisher
        - premidline
        - medline
      default: uknown
    
    v2reportsNameAndAuthorityNoteClassifier:
      type: string
      enum:
        - no_authority_classifier
        - effective_name
        - nomen_approbbatum
        - ictv_accepted
      default: no_authority_classifier
      title: 'Class of authority  If the authority has any special classification, such as having been effectively and validly published or having been included in an approved list.'
    
    v2reportsOrganelleTopology:
      type: string
      enum:
        - TOPOLOGY_UNKNOWN
        - Circular
        - Linear
        - Tandem
      default: TOPOLOGY_UNKNOWN
    
    v2reportsOrganelleType:
      type: string
      enum:
        - ORGANELLE_TYPE_UNKNOWN
        - Mitochondrion
        - Chloroplast
        - Plastid
        - Kinetoplast
        - Apicoplast
        - Chromatophore
        - Cyanelle
      default: ORGANELLE_TYPE_UNKNOWN
    
    v2reportsOrientation:
      type: string
      enum:
        - none
        - plus
        - minus
      default: none
    
    v2reportsProkaryoteGeneLocationCompleteness:
      type: string
      enum:
        - complete
        - partial
      default: complete
    
    v2reportsPurposeOfSampling:
      type: string
      enum:
        - PURPOSE_OF_SAMPLING_UNKNOWN
        - PURPOSE_OF_SAMPLING_BASELINE_SURVEILLANCE
      default: PURPOSE_OF_SAMPLING_UNKNOWN
    
    v2reportsRankType:
      type: string
      enum:
        - NO_RANK
        - SUPERKINGDOM
        - DOMAIN
        - REALM
        - KINGDOM
        - SUBKINGDOM
        - SUPERPHYLUM
        - SUBPHYLUM
        - PHYLUM
        - CLADE
        - SUPERCLASS
        - CLASS
        - SUBCLASS
        - INFRACLASS
        - COHORT
        - SUBCOHORT
        - SUPERORDER
        - ORDER
        - SUBORDER
        - INFRAORDER
        - PARVORDER
        - SUPERFAMILY
        - FAMILY
        - SUBFAMILY
        - GENUS
        - SUBGENUS
        - SPECIES_GROUP
        - SPECIES_SUBGROUP
        - SPECIES
        - SUBSPECIES
        - TRIBE
        - SUBTRIBE
        - FORMA
        - VARIETAS
        - STRAIN
        - SECTION
        - SUBSECTION
        - PATHOGROUP
        - SUBVARIETY
        - GENOTYPE
        - SEROTYPE
        - ISOLATE
        - MORPH
        - SERIES
        - FORMA_SPECIALIS
        - SEROGROUP
        - BIOTYPE
        - ACELLULAR_ROOT
        - CELLULAR_ROOT
      default: NO_RANK
      title: 'Rank level'
    
    v2reportsRnaType:
      type: string
      enum:
        - rna_UNKNOWN
        - premsg
        - tmRna
      default: rna_UNKNOWN
    
    v2reportsSequenceDataReportGenomeType:
      type: string
      enum:
        - genome_type_unknown
        - genomic
        - chloroplast
        - chromoplast
        - kinetoplast
        - mitochondrion
        - plastid
        - macronuclear
        - extrachromosomal
        - plasmid
        - transposon
        - insertion_sequence
        - cyanelle
        - proviral
        - virion
        - nucleomorph
        - apicoplast
        - leucoplast
        - proplastid
        - endogenous_virus
        - hydrogenosome
        - chromatophore
        - plasmid_in_mitochondrion
        - plasmid_in_plastid
        - nitroplast
      default: genome_type_unknown
    
    v2reportsSequenceDataReportOriginType:
      type: string
      enum:
        - unknown
        - natural
        - naturally_occurring_mutant
        - artificially_mutagenized
        - artificially_engineered
        - purely_synthetic
        - other
      default: unknown
    
    v2reportsSourceDatabase:
      type: string
      enum:
        - SOURCE_DATABASE_UNSPECIFIED
        - SOURCE_DATABASE_GENBANK
        - SOURCE_DATABASE_REFSEQ
      default: SOURCE_DATABASE_UNSPECIFIED
    
    v2reportsThesisType:
      type: string
      enum:
        - unknown
        - manuscript
        - letter
        - thesis
      default: unknown
    
    v2reportsTranscriptSelectCategory:
      type: string
      enum:
        - SELECT_UNKNOWN
        - REFSEQ_SELECT
        - MANE_SELECT
        - MANE_PLUS_CLINICAL
      default: SELECT_UNKNOWN
    
    v2reportsTranscriptTranscriptType:
      type: string
      enum:
        - UNKNOWN
        - PROTEIN_CODING
        - NON_CODING
        - PROTEIN_CODING_MODEL
        - NON_CODING_MODEL
      default: UNKNOWN
    
    v2reportsVirusAssemblyCompleteness:
      type: string
      enum:
        - UNKNOWN
        - COMPLETE
        - PARTIAL
      default: UNKNOWN
    
    v2reportsVirusPeptideViralPeptideCompleteness:
      type: string
      enum:
        - UNKNOWN
        - COMPLETE
        - PARTIAL
      default: UNKNOWN
    
    v2reportsWarningGeneWarningCode:
      type: string
      enum:
        - UNKNOWN_GENE_WARNING_CODE
        - ACCESSION_VERSION_MISMATCH
        - REPLACED_GENE_ID
        - DISCONTINUED_GENE_ID
        - UNRECOGNIZED_GENE_ID
        - UNRECOGNIZED_GENE_SYMBOL
        - UNRECOGNIZED_ACCESSION
        - UNRECOGNIZED_TAX_TOKEN
        - NO_GENE_ANNOTATION_FOUND
        - ABOVE_SPECIES_TAXON
      default: UNKNOWN_GENE_WARNING_CODE
    
    ncbigsupgcolv2ChromosomeLocation:
      type: string
      enum:
        - LOCATION_UNKNOWN
        - APICOPLAST
        - CHLOROPLAST
        - CHROMATOPHORE
        - CHROMOPLAST
        - CYANELLE
        - HYDROGENOSOME
        - KINETOPLAST
        - LEUCOPLAST
        - MACRONUCLEAR
        - MITOCHONDRION
        - NUCLEARPROKARYOTE
        - NUCLEOMORPH
        - OTHER_LOCATION
        - PLASTID
        - PROPLASTID
        - PROVIRALPROPHAGE
        - VIRIONPHAGE
        - VIROID
      default: LOCATION_UNKNOWN
      title: 'Represents the location information for a chromosome'
    
    ncbigsupgcolv2ChromosomeType:
      type: string
      enum:
        - TYPE_UNKNOWN
        - CHROMOSOME
        - EXTRACHROM
        - LINKAGEGROUP
        - OTHER_TYPE
        - PLASMID
        - SEGMENT
      default: TYPE_UNKNOWN
      title: 'Represents the type information for a chromosome'
    
    ncbiprotddv2RedundancyLevel:
      type: string
      enum:
        - NOT_SPECIFIED
        - ALL_SEQUENCES
        - LOW
        - MEDIUM
        - HIGH
      default: NOT_SPECIFIED
    
    ncbiprotddv2SortById:
      type: string
      enum:
        - NONE
        - VAST_SCORE
        - P_VALUE
        - RMSD
        - ALIGNMENT_LENGTH
        - PCT_IDENTITY
        - ACCESSION
        - SDID
        - ALIGNMENT_ID
        - PDB_ID
        - CHAIN_NAME
        - DOMAIN_NUMBER
        - DESCRIPTION
        - TAXONOMY_ID
        - SUPERKINGDOM_ID
        - DOMAIN_NAME
      default: NONE
      title: 'Corresponds to sort @sortBy options in PubVast.dbo.getVastSimilarStructures stored procedure'
    
    ncbiprotddv2StructureDataReportKind:
      type: string
      enum:
        - DNA
        - RNA
        - PROTEIN
        - OTHER_BIOPOLYMER
        - SOLVENT
        - OTHER_NONPOLYMER
        - OTHER
      default: DNA
      title: 'Kind of molecule. DNA, RNA, Protein, etc.'
    protobufAny:
      type: object
      properties:
        type_url:
          type: string
        value:
          type: string
          format: byte
    rpcStatus:
      type: object
      properties:
        code:
          type: integer
          format: int32
        message:
          type: string
        details:
          type: array
          items:
            $ref: '#/components/schemas/protobufAny'
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
