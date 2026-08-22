/**
 * @file external/ncbi/taxonomy_report_schemas.cppm
 * @brief NCBI sequence, taxonomy, and virus report schema fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:taxonomy_report_schemas;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Sequence, taxonomy, and virus report schemas. */
inline constexpr std::string_view sequence_taxonomy_virus_report_schemas =
    R"bioseq_openapi(    v2reportsRange:
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
