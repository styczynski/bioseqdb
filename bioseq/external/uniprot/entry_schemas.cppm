/**
 * @file external/uniprot/entry_schemas.cppm
 * @brief UniProt entry schema fragments.
 */
module;

#include <string_view>

export module bioseq.external.uniprot:entry_schemas;

export namespace bioseq::external::uniprot::descriptor {

/** @brief Components prefix and core UniProt entry schemas. */
inline constexpr std::string_view entry_schemas = R"bioseq_openapi(components:
  schemas:
    AlternativeSequence:
      type: object
      properties:
        originalSequence:
          type: string
        alternativeSequences:
          type: array
          items:
            type: string
    Citation:
      type: object
      properties:
        authoringGroups:
          type: array
          items:
            type: string
        authors:
          type: array
          items:
            type: string
        publicationDate:
          type: string
        citationType:
          type: string
          enum:
          - journal article
          - book
          - online journal article
          - patent
          - submission
          - thesis
          - UniProt indexed literatures
          - unpublished observations
        citationCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceCitationDatabase"
        title:
          type: string
        id:
          type: string
      allOf:
      - "$ref": "#/components/schemas/Book"
      - "$ref": "#/components/schemas/ElectronicArticle"
      - "$ref": "#/components/schemas/JournalArticle"
      - "$ref": "#/components/schemas/Literature"
      - "$ref": "#/components/schemas/Patent"
      - "$ref": "#/components/schemas/Submission"
      - "$ref": "#/components/schemas/Thesis"
    Comment:
      type: object
      properties:
        commentType:
          type: string
          enum:
          - FUNCTION
          - CATALYTIC ACTIVITY
          - COFACTOR
          - ACTIVITY REGULATION
          - BIOPHYSICOCHEMICAL PROPERTIES
          - PATHWAY
          - SUBUNIT
          - INTERACTION
          - SUBCELLULAR LOCATION
          - ALTERNATIVE PRODUCTS
          - TISSUE SPECIFICITY
          - DEVELOPMENTAL STAGE
          - INDUCTION
          - DOMAIN
          - PTM
          - RNA EDITING
          - MASS SPECTROMETRY
          - POLYMORPHISM
          - DISEASE
          - DISRUPTION PHENOTYPE
          - ALLERGEN
          - TOXIC DOSE
          - BIOTECHNOLOGY
          - PHARMACEUTICAL
          - MISCELLANEOUS
          - SIMILARITY
          - CAUTION
          - SEQUENCE CAUTION
          - WEB RESOURCE
          - UNKOWN
      allOf:
      - "$ref": "#/components/schemas/AlternativeProductsComment"
      - "$ref": "#/components/schemas/BPCPComment"
      - "$ref": "#/components/schemas/CatalyticActivityComment"
      - "$ref": "#/components/schemas/CofactorComment"
      - "$ref": "#/components/schemas/DiseaseComment"
      - "$ref": "#/components/schemas/FreeTextComment"
      - "$ref": "#/components/schemas/InteractionComment"
      - "$ref": "#/components/schemas/MassSpectrometryComment"
      - "$ref": "#/components/schemas/RnaEditingComment"
      - "$ref": "#/components/schemas/SequenceCautionComment"
      - "$ref": "#/components/schemas/SubcellularLocationComment"
      - "$ref": "#/components/schemas/WebResourceComment"
    CrossReferenceCitationDatabase:
      type: object
      properties:
        database:
          type: string
          enum:
          - PubMed
          - DOI
          - AGRICOLA
        properties:
          type: array
          items:
            "$ref": "#/components/schemas/Property"
        id:
          type: string
    CrossReferenceEvidenceDatabase:
      type: object
      properties:
        database:
          "$ref": "#/components/schemas/EvidenceDatabase"
        properties:
          type: array
          items:
            "$ref": "#/components/schemas/Property"
        id:
          type: string
    CrossReferenceUniprotKBFeatureDatabase:
      type: object
      properties:
        database:
          type: string
          enum:
          - dbSNP
          - ChEBI
        properties:
          type: array
          items:
            "$ref": "#/components/schemas/Property"
        id:
          type: string
    EC:
      type: object
      properties:
        valid:
          type: boolean
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    EntryAudit:
      type: object
      properties:
        firstPublicDate:
          type: string
          format: date
        sequenceVersion:
          type: integer
          format: int32
        lastAnnotationUpdateDate:
          type: string
          format: date
        lastSequenceUpdateDate:
          type: string
          format: date
        entryVersion:
          type: integer
          format: int32
    EntryInactiveReason:
      type: object
      properties:
        deletedReason:
          type: string
          enum:
          - ''
          - Undefined
          - Deleted from sequence source (EMBL)
          - Deleted from sequence source (TAIR)
          - Deleted from sequence source (SGD)
          - Deleted from sequence source (ENSEMBL)
          - Deleted from sequence source (PDB)
          - Deleted from sequence source (RefSeq)
          - Deleted from Swiss-Prot
          - Redundant sequence
          - Not part of a reference proteome
          - Excluded proteome
          - Over-represented sequence
          - Not part of a reference proteome
        inactiveReasonType:
          type: string
          enum:
          - DELETED
          - MERGED
          - DEMERGED
        mergeDemergeTos:
          type: array
          items:
            type: string
    Evidence:
      type: object
      properties:
        evidenceCode:
          type: string
        id:
          type: string
        source:
          type: string
    EvidenceDatabase:
      type: object
      properties:
        name:
          type: string
        reference:
          type: boolean
        evidenceDatabaseDetail:
          "$ref": "#/components/schemas/EvidenceDatabaseDetail"
    EvidenceDatabaseDetail:
      type: object
      properties:
        name:
          type: string
        displayName:
          type: string
        category:
          type: string
          enum:
          - I
          - C
          - A
        uriLink:
          type: string
    EvidenceLine:
      type: object
      properties:
        evidence:
          type: string
        createDate:
          type: string
          format: date
        curator:
          type: string
    FeatureLocation:
      type: object
      properties:
        start:
          "$ref": "#/components/schemas/Position"
        end:
          "$ref": "#/components/schemas/Position"
        sequence:
          type: string
    Gene:
      type: object
      properties:
        orfNames:
          type: array
          items:
            "$ref": "#/components/schemas/ORFName"
        geneName:
          "$ref": "#/components/schemas/GeneName"
        orderedLocusNames:
          type: array
          items:
            "$ref": "#/components/schemas/OrderedLocusName"
        synonyms:
          type: array
          items:
            "$ref": "#/components/schemas/GeneNameSynonym"
    GeneLocation:
      type: object
      properties:
        geneEncodingType:
          type: string
          enum:
          - unknown
          - Hydrogenosome
          - Mitochondrion
          - Nucleomorph
          - Plasmid
          - Plastid
          - Apicoplast
          - Chloroplast
          - Cyanelle
          - Non-photosynthetic plastid
          - Organellar chromatophore
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    GeneName:
      type: object
      properties:
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    GeneNameSynonym:
      type: object
      properties:
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    InternalLine:
      type: object
      properties:
        type:
          type: string
          enum:
          - CL
          - CP
          - CX
          - DR
          - DG
          - GO
          - EV
          - HA
          - HR
          - HW
          - HU
          - HP
          - ID
          - IS
          - NI
          - PM
          - SO
          - YY
          - ZA
          - ZB
          - ZC
          - ZR
          - ZZ
          - ET
          - PE
          - RU
          - TX
          - PROSITE
          - UP
          - ZD
        value:
          type: string
    InternalSection:
      type: object
      properties:
        sourceLines:
          type: array
          items:
            "$ref": "#/components/schemas/SourceLine"
        evidenceLines:
          type: array
          items:
            "$ref": "#/components/schemas/EvidenceLine"
        internalLines:
          type: array
          items:
            "$ref": "#/components/schemas/InternalLine"
    Keyword:
      type: object
      properties:
        category:
          type: string
          enum:
          - Biological process
          - Cellular component
          - Coding sequence diversity
          - Developmental stage
          - Disease
          - Domain
          - Ligand
          - Molecular function
          - PTM
          - Technical term
          - Unknown
        name:
          type: string
        id:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    Ligand:
      type: object
      properties:
        note:
          type: string
        label:
          type: string
        name:
          type: string
        id:
          type: string
    LigandPart:
      type: object
      properties:
        note:
          type: string
        label:
          type: string
        name:
          type: string
        id:
          type: string
    Name:
      type: object
      properties:
        value:
          type: string
        evidence:
          type: array
          items:
            type: integer
            format: int32
    ORFName:
      type: object
      properties:
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    OrderedLocusName:
      type: object
      properties:
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    Organism:
      type: object
      properties:
        lineages:
          type: array
          items:
            type: string
        taxonId:
          type: integer
          format: int64
        commonName:
          type: string
        synonyms:
          type: array
          items:
            type: string
        scientificName:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    OrganismHost:
      type: object
      properties:
        taxonId:
          type: integer
          format: int64
        commonName:
          type: string
        synonyms:
          type: array
          items:
            type: string
        scientificName:
          type: string
    Position:
      type: object
      properties:
        value:
          type: integer
          format: int32
        modifier:
          type: string
          enum:
          - EXACT
          - OUTSIDE
          - UNKNOWN
          - UNSURE
    Property:
      type: object
      properties:
        key:
          type: string
        value:
          type: string
    ProteinDescription:
      type: object
      properties:
        recommendedName:
          "$ref": "#/components/schemas/ProteinName"
        submissionNames:
          type: array
          items:
            "$ref": "#/components/schemas/ProteinSubName"
        includes:
          type: array
          items:
            "$ref": "#/components/schemas/ProteinSection"
        allergenName:
          "$ref": "#/components/schemas/Name"
        biotechName:
          "$ref": "#/components/schemas/Name"
        cdAntigenNames:
          type: array
          items:
            "$ref": "#/components/schemas/Name"
        innNames:
          type: array
          items:
            "$ref": "#/components/schemas/Name"
        alternativeNames:
          type: array
          items:
            "$ref": "#/components/schemas/ProteinName"
        contains:
          type: array
          items:
            "$ref": "#/components/schemas/ProteinSection"
        flag:
          type: string
    ProteinName:
      type: object
      properties:
        shortNames:
          type: array
          items:
            "$ref": "#/components/schemas/Name"
        ecNumbers:
          type: array
          items:
            "$ref": "#/components/schemas/EC"
        valid:
          type: boolean
        fullName:
          "$ref": "#/components/schemas/Name"
    ProteinSection:
      type: object
      properties:
        recommendedName:
          "$ref": "#/components/schemas/ProteinName"
        allergenName:
          "$ref": "#/components/schemas/Name"
        biotechName:
          "$ref": "#/components/schemas/Name"
        cdAntigenNames:
          type: array
          items:
            "$ref": "#/components/schemas/Name"
        innNames:
          type: array
          items:
            "$ref": "#/components/schemas/Name"
        alternativeNames:
          type: array
          items:
            "$ref": "#/components/schemas/ProteinName"
    ProteinSubName:
      type: object
      properties:
        ecNumbers:
          type: array
          items:
            "$ref": "#/components/schemas/EC"
        valid:
          type: boolean
        fullName:
          "$ref": "#/components/schemas/Name"
    ReferenceComment:
      type: object
      properties:
        type:
          type: string
          enum:
          - STRAIN
          - PLASMID
          - TRANSPOSON
          - TISSUE
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    Sequence:
      type: object
      properties:
        type:
          type: string
          xml:
            attribute: true
        ref:
          type: string
          xml:
            attribute: true
    SourceLine:
      type: object
      properties:
        value:
          type: string
    TaxonomyLineage:
      type: object
      properties:
        rank:
          type: string
          enum:
          - strain
          - serotype
          - series
          - morph
          - biotype
          - clade
          - genotype
          - isolate
          - forma specialis
          - pathogroup
          - serogroup
          - section
          - subsection
          - forma
          - varietas
          - subspecies
          - species
          - species subgroup
          - species group
          - subgenus
          - genus
          - subtribe
          - tribe
          - subfamily
          - family
          - superfamily
          - parvorder
          - infraorder
          - suborder
          - order
          - superorder
          - subcohort
          - cohort
          - infraclass
          - subclass
          - class
          - superclass
          - subphylum
          - phylum
          - superphylum
          - subkingdom
          - kingdom
          - superkingdom
          - realm
          - domain
          - subvariety
          - acellular root
          - cellular root
          - no rank
        taxonId:
          type: integer
          format: int64
        hidden:
          type: boolean
        commonName:
          type: string
        synonyms:
          type: array
          items:
            type: string
        scientificName:
          type: string
    UniProtDatabaseAttribute:
      type: object
      properties:
        name:
          type: string
        xmlTag:
          type: string
        uriLink:
          type: string
    UniProtDatabaseDetail:
      type: object
      properties:
        name:
          type: string
        displayName:
          type: string
        category:
          type: string
          enum:
          - Sequence databases
          - 3D structure databases
          - Protein-protein interaction databases
          - Chemistry
          - Protein family/group databases
          - PTM databases
          - Genetic variation databases
          - 2D gel databases
          - Proteomic databases
          - Protocols and materials databases
          - Genome annotation databases
          - Organism-specific databases
          - Phylogenomic databases
          - Enzyme and pathway databases
          - Miscellaneous
          - Gene expression databases
          - Family and domain databases
          - Ontologies
          - Proteomes databases
          - Unknown
        uniProtDataTypes:
          type: array
          items:
            type: string
        uriLink:
          type: string
        attributes:
          type: array
          items:
            "$ref": "#/components/schemas/UniProtDatabaseAttribute"
        implicit:
          type: boolean
        linkedReason:
          type: string
        idMappingName:
          type: string
        type:
          type: string
    UniProtKBCrossReference:
      type: object
      properties:
        isoformId:
          type: string
        database:
          type: string
        properties:
          type: array
          items:
            "$ref": "#/components/schemas/Property"
        id:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    UniProtKBEntry:
      type: object
      properties:
        secondaryAccessions:
          type: array
          items:
            type: string
        proteinDescription:
          "$ref": "#/components/schemas/ProteinDescription"
        uniProtKBCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/UniProtKBCrossReference"
        entryType:
          type: string
          enum:
          - UniProtKB reviewed (Swiss-Prot)
          - UniProtKB unreviewed (TrEMBL)
          - Inactive
          - AA
          - UNKNOWN
        annotationScore:
          type: number
          format: double
        entryAudit:
          "$ref": "#/components/schemas/EntryAudit"
        organismHosts:
          type: array
          items:
            "$ref": "#/components/schemas/OrganismHost"
        genes:
          type: array
          items:
            "$ref": "#/components/schemas/Gene"
        comments:
          type: array
          items:
            "$ref": "#/components/schemas/Comment"
        geneLocations:
          type: array
          items:
            "$ref": "#/components/schemas/GeneLocation"
        keywords:
          type: array
          items:
            "$ref": "#/components/schemas/Keyword"
        sequence:
          "$ref": "#/components/schemas/Sequence"
        internalSection:
          "$ref": "#/components/schemas/InternalSection"
        fragment:
          type: boolean
        primaryAccession:
          type: string
        inactiveReason:
          "$ref": "#/components/schemas/EntryInactiveReason"
        organism:
          "$ref": "#/components/schemas/Organism"
        lineages:
          type: array
          items:
            "$ref": "#/components/schemas/TaxonomyLineage"
        uniProtkbId:
          type: string
        proteinExistence:
          type: string
          enum:
          - '1: Evidence at protein level'
          - '2: Evidence at transcript level'
          - '3: Inferred from homology'
          - '4: Predicted'
          - '5: Uncertain'
          - UNKNOWN
        features:
          type: array
          items:
            "$ref": "#/components/schemas/UniProtKBFeature"
        references:
          type: array
          items:
            "$ref": "#/components/schemas/UniProtKBReference"
        active:
          type: boolean
        extraAttributes:
          type: object
          additionalProperties:
            type: object
    UniProtKBFeature:
      type: object
      properties:
        ligand:
          "$ref": "#/components/schemas/Ligand"
        ligandPart:
          "$ref": "#/components/schemas/LigandPart"
        featureId:
          type: string
        alternativeSequence:
          "$ref": "#/components/schemas/AlternativeSequence"
        featureCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceUniprotKBFeatureDatabase"
        description:
          type: string
        location:
          "$ref": "#/components/schemas/FeatureLocation"
        type:
          type: string
          enum:
          - Initiator methionine
          - Signal
          - Propeptide
          - Transit peptide
          - Chain
          - Peptide
          - Topological domain
          - Transmembrane
          - Intramembrane
          - Domain
          - Repeat
          - Calcium binding
          - Zinc finger
          - DNA binding
          - Nucleotide binding
          - Region
          - Coiled coil
          - Motif
          - Compositional bias
          - Active site
          - Metal binding
          - Binding site
          - Site
          - Non-standard residue
          - Modified residue
          - Lipidation
          - Glycosylation
          - Disulfide bond
          - Cross-link
          - Alternative sequence
          - Natural variant
          - Mutagenesis
          - Sequence uncertainty
          - Sequence conflict
          - Non-adjacent residues
          - Non-terminal residue
          - Helix
          - Turn
          - Beta strand
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    UniProtKBReference:
      type: object
      properties:
        referenceNumber:
          type: integer
          format: int32
        referenceComments:
          type: array
          items:
            "$ref": "#/components/schemas/ReferenceComment"
        citation:
          "$ref": "#/components/schemas/Citation"
        referencePositions:
          type: array
          items:
            type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
)bioseq_openapi";

} // namespace bioseq::external::uniprot::descriptor
