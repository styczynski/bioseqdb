/**
 * @file external/ncbi/specialist_schemas.cppm
 * @brief NCBI types for gene lookup, assembly dat, and protein structures.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:specialist_schemas;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Gene and assembly requests with protein-structure reports and scores. */
inline constexpr std::string_view specialist_schemas =
    R"bioseq_openapi(    ncbieukgenev2GeneIdRequest:
      type: object
      properties:
        gene_ids:
          type: array
          items:
            type: integer
    ncbigsupgcolv2AssemblyAccessionsReply:
      type: object
      properties:
        accession:
          type: array
          items:
            type: string
    ncbigsupgcolv2AssemblyCheckMHistogramRequest:
      type: object
      properties:
        tax_id:
          type: integer
          title: 'species level'
    ncbigsupgcolv2AssemblyDataReportDraftRequest:
      type: object
      properties:
        accession:
          type: string
        _bypass_cache:
          type: string
          title: 'buf:lint:ignore FIELD_LOWER_SNAKE_CASE'
    ncbigsupgcolv2AssemblyDataReportsRequest:
      type: object
      properties:
        accessions:
          type: array
          items:
            type: string
    ncbigsupgcolv2SequenceAccessionRequest:
      type: object
      properties:
        accession:
          type: string
          title: 'Could be WGS or sequence accession'
    ncbiprotddv2ChainFootprint:
      type: object
      properties:
        query_from:
          type: integer
        query_to:
          type: integer
        dependent_from:
          type: integer
        dependent_to:
          type: integer
    ncbiprotddv2ParsedAbstract:
      type: object
      properties:
        pmid:
          type: integer
          title: 'The pubmed ID of the article'
        title:
          type: string
          title: 'Title of the article'
        authors:
          type: array
          items:
            $ref: '#/components/schemas/ncbiprotddv2ParsedAbstractAuthor'
        epub:
          $ref: '#/components/schemas/ncbiprotddv2ParsedAbstractEpub'
          title: 'Electronic publication information'
        abstract_text:
          type: string
          title: 'Body of the abstract'
    ncbiprotddv2ParsedAbstractAuthor:
      type: object
      properties:
        surname:
          type: string
          title: 'Author''s surname'
        given_name_initials:
          type: string
          title: 'Abbreviated initials of the author''s given name'
    ncbiprotddv2ParsedAbstractEpub:
      type: object
      properties:
        journal:
          type: string
          title: 'Name of the journal'
        year:
          type: integer
          title: 'Year of publication'
        volume:
          type: integer
          title: 'Volume of publication'
        pages:
          type: string
          title: 'Pages of publication'
    ncbiprotddv2PubmedAbstractRequest:
      type: object
      properties:
        pmid:
          type: integer
          title: 'The pubmed ID of the article'
    ncbiprotddv2QueryStructureDefinition:
      type: object
      properties:
        sdid:
          type: integer
          title: 'Structure Domain ID: Unique ID for mmdb_id + chain_id + domain_number'
        mmdb_id:
          type: integer
          title: 'The MMDB ID of the structure'
        pdb_id:
          type: string
          title: 'The PDB ID of the structure'
        description:
          type: string
          title: 'Description of the structure'
        chain_id:
          type: string
          title: 'Chain name. I.e. AA'
        domain_number:
          type: integer
          title: 'Domain Number 0 is the entire chain'
        from:
          type: integer
          title: 'Domain start interval residue number.'
        to:
          type: integer
          title: 'Domain end interval residue number.'
    ncbiprotddv2SdidRequest:
      type: object
      properties:
        sdid:
          type: integer
          title: 'Structure Domain ID'
    ncbiprotddv2SimilarStructureReport:
      type: object
      properties:
        sdid:
          type: integer
          title: 'Structure Domain ID'
        structure_title:
          type: string
          title: 'PDB Structure Title'
        protein_chain_name:
          type: string
          title: 'Protein Chain Name'
        chain_id:
          type: string
          title: 'Chain ID'
        domain_number:
          type: integer
          title: 'Domain Number 0 is the entire chain'
        mmdb_id:
          type: integer
          title: 'The MMDB ID of the structure'
        pdb_id:
          type: string
          title: 'The PDB ID of the structure'
        vast_score:
          $ref: '#/components/schemas/ncbiprotddv2VastScore'
        align_id:
          type: integer
          title: 'Alignment Id'
        superkingdom_id:
          type: integer
          title: 'Superkingdom ID'
        tax_id:
          type: integer
          title: 'NCBI Taxonomy ID'
        footprints:
          type: array
          items:
            $ref: '#/components/schemas/ncbiprotddv2ChainFootprint'
    ncbiprotddv2SimilarStructureReportPage:
      type: object
      properties:
        similar_structures:
          type: array
          items:
            $ref: '#/components/schemas/ncbiprotddv2SimilarStructureReport'
        next_page_token:
          type: string
        total_count:
          type: integer
    ncbiprotddv2SimilarStructureRequest:
      type: object
      properties:
        sdid:
          type: string
        page_token:
          type: string
          title: 'Only needed for paging GetSimilarStructures RPC'
        redundancy_level:
          $ref: '#/components/schemas/ncbiprotddv2RedundancyLevel'
          title: 'Sort and filter fields here. Redundancy Level'
        sort_by:
          $ref: '#/components/schemas/ncbiprotddv2SortById'
        hits_per_page:
          type: integer
        sort_direction:
          $ref: '#/components/schemas/v2SortDirection'
    ncbiprotddv2StructureDataReport:
      type: object
      properties:
        pdb_id:
          type: string
          title: 'The PDB ID of the structure'
        mmdb_id:
          type: integer
          title: 'The MMDB ID of the structure'
        is_obsolete:
          type: boolean
          title: 'Marks the structure as obsolete'
        publication_pmid:
          type: array
          items:
            type: integer
        deposition_date:
          type: string
          title: 'YYYY-MM-DD formatted date of publication Example: "2023-10-01"'
        update_date:
          type: string
          title: 'YYYY-MM-DD formatted last date updated'
        experiment:
          $ref: '#/components/schemas/ncbiprotddv2StructureDataReportExperiment'
          title: 'relevant experiment data'
        chains:
          type: array
          items:
            $ref: '#/components/schemas/ncbiprotddv2StructureDataReportBiounitChain'
        ligand_chains:
          type: array
          items:
            $ref: '#/components/schemas/ncbiprotddv2StructureDataReportLigandChain'
        asymmetric_chains:
          type: array
          items:
            $ref: '#/components/schemas/ncbiprotddv2StructureDataReportBiounitChain'
        asymmetric_ligands:
          type: array
          items:
            $ref: '#/components/schemas/ncbiprotddv2StructureDataReportLigandChain'
        title:
          type: string
          title: 'title of the MMDB Page'
        default_biounit:
          type: integer
          title: 'default biounit of the structure'
    ncbiprotddv2StructureDataReportBiounitChain:
      type: object
      properties:
        chain_id:
          type: string
          title: 'Chain name. I.e. AA'
        tax_id:
          type: integer
          title: 'taxonomy id'
        kind:
          $ref: '#/components/schemas/ncbiprotddv2StructureDataReportKind'
          title: 'Kind of molecule. DNA, RNA, Protein, etc.'
        molecule_group:
          type: integer
          title: 'Which group of identical chains does this chain belong to'
        sdid:
          type: integer
          title: 'Unique structure identifier of the chain'
    ncbiprotddv2StructureDataReportExperiment:
      type: object
      properties:
        experimental_method:
          type: string
          title: 'The experimental method used to characterize the protein structure. I.e. x-ray diffraction, etc.'
        resolution:
          type: number
          format: float
          title: 'The resolution (in Angstroms) of a protein structure resolved by diffraction or electron microscopy.'
    ncbiprotddv2StructureDataReportLigandChain:
      type: object
      properties:
        chain_id:
          type: string
          title: 'Chain name. I.e. ZINC ION'
        kind:
          $ref: '#/components/schemas/ncbiprotddv2StructureDataReportKind'
          title: 'Kind of molecule. DNA, RNA, Protein, etc.'
        sid:
          type: integer
          title: 'Unique substance identifier'
        sdid:
          type: integer
          title: 'Unique structure identifiers'
    ncbiprotddv2StructureRequest:
      type: object
      properties:
        pdb_id:
          type: string
          title: 'The PDB ID of the structure'
        mmdb_id:
          type: integer
          title: 'The MMDB ID of the structure'
    ncbiprotddv2VastScore:
      type: object
      properties:
        vast_score:
          type: number
          format: float
          title: 'Vast Similarity Score as computed by the VAST algorithm'
        align_length:
          type: integer
          title: 'Alignment Length'
        pct_identity:
          type: number
          format: float
          title: 'Percentage Sequence Identity'
        rmsd:
          type: number
          format: float
          title: 'Root Mean Square Deviation in Angstroms'
        p_value:
          type: number
          format: float
          title: 'Statistical Significance of the similarity'
    
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
