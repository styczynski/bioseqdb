/**
 * @file external/ncbi/gene_report_schemas.cppm
 * @brief NCBI gene and protein report schema fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:gene_report_schemas;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Gene, genome-annotation, organelle, and protein report schemas. */
inline constexpr std::string_view gene_protein_report_schemas =
    R"bioseq_openapi(    v2reportsGeneCounts:
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
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
