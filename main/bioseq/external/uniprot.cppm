module;

#include <string_view>

export module bioseq.external.uniprot;

constexpr static std::string_view spec = R"bioseq_openapi(
---
openapi: 3.0.1
info:
  title: OpenAPI definition
  version: v0
servers:
- url: https://rest.uniprot.org/
  description: UniProt REST API Server
tags:
- name: UniProtKB
  description: The UniProt Knowledgebase (UniProtKB) acts as the global hub of accurate,
    consistent and expertly curated information on protein sequence and function.
    Each UniProtKB entry is described by a stable protein identifier (accession ID)
    and contains core data consisting of the amino acid sequence, protein name or
    description, taxonomic information and links to relevant scientific publications.
    Further annotation is added when available, such as protein function, subcellular
    location and the position of protein features such as active sites, domains and
    post-translational modifications. Where possible these annotations are described
    using established biological ontologies, classifications and cross-references.
    A clear indication of the quality of annotation in the form of evidence attribution
    of experimental and computational data is added to each piece of data.
paths:
  "/uniprotkb/{accession}":
    get:
      tags:
      - UniProtKB
      summary: Get UniProtKB entry by a single accession.
      description: Search UniProtKB by protein entry accession to return all data
        associated with that entry. Specify <tt>fields</tt> to return only data for
        specific sections of that entry that are of interest to you
      operationId: getByAccession
      parameters:
      - name: accession
        in: path
        description: Unique and stable identifier for each UniProtKB entry
        required: true
        schema:
          pattern: "([OPQ][0-9][A-Z0-9]{3}[0-9]|[A-NR-Z]([0-9][A-Z][A-Z0-9]{2}){1,2}[0-9])(((-[0-9]{1,3})?(\\[\\d+-\\d+\\])?)|(\\.[0-9]{1,3})|(_[A-Z0-9]{2,5}))?|[A-Z0-9]{2,5}_[A-Z0-9]{2,5}"
          type: string
        example: P05067
      - name: fields
        in: query
        description: List of entry sections (fields) to be returned, separated by
          commas.  <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid fields</a>
        required: false
        schema:
          type: string
        example: accession,protein_name,cc_function,ft_binding
      - name: version
        in: query
        description: Version of the entry. Versions are integers 1 or above; enter
          <tt>last</tt> for the latest version.</br>Please note that when passing
          <tt>version</tt> file formats are restricted to <tt>fasta</tt> and <tt>txt</tt>
          only
        required: false
        schema:
          type: string
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/UniProtKBEntry"
            application/xml:
              schema:
                "$ref": "#/components/schemas/Entry"
            text/plain;format=tsv: {}
            text/plain;format=flatfile: {}
            text/plain;format=list: {}
            application/vnd.openxmlformats-officedocument.spreadsheetml.sheet: {}
            text/plain;format=fasta: {}
            text/plain;format=gff: {}
            application/rdf+xml: {}
            text/turtle: {}
            application/n-triples: {}
  "/uniprotkb/stream":
    get:
      tags:
      - UniProtKB
      summary: Download UniProtKB entries retrieved by a search query. (Max. 10 million
        entries)
      description: The stream endpoint uses a request query to return all entries
        associated with the search term in a single download. Specify <tt>fields</tt>
        to return only data for specific sections of that entry that are of interest
        to you The stream endpoint has a maximum limit of 10 million entries. For
        larger requests, please use the 'UniProtKB asynchronous download job' requests
        described below. The 'UniProtKB asynchronous download job' requests can be
        used for any size -- the asynchronous download jobs can be paused and resumed
        at your convenience, unlike the stream endpoint.
      operationId: stream
      parameters:
      - name: query
        in: query
        description: Criteria to search UniProtKB. Advanced queries can be built with
          parentheses and conditionals such as AND, OR and NOT. <a href='https://rest.uniprot.org/configure/uniprotkb/search-fields'
          target='_blank' rel='noopener noreferrer'>List of valid search fields</a>
        required: true
        schema:
          type: string
        example: insulin AND reviewed:true
      - name: fields
        in: query
        description: List of entry sections (fields) to be returned, separated by
          commas.  <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid fields</a>
        required: false
        schema:
          type: string
        example: accession,protein_name,cc_function,ft_binding
      - name: sort
        in: query
        description: Specify field by which to sort results. <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid sort fields</a>
        required: false
        schema:
          type: string
        example: accession desc
      - name: includeIsoform
        in: query
        description: Specify <tt>true</tt> to include isoforms, default is <tt>false</tt>.
        required: false
        schema:
          pattern: true|false
          type: boolean
      - name: download
        in: query
        description: Specify <tt>true</tt> to download as file, default is <tt>false</tt>.
        required: false
        schema:
          pattern: "^true$|^false$"
          type: boolean
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/StreamResult"
            application/xml:
              schema:
                type: array
                items:
                  "$ref": "#/components/schemas/Entry"
            text/plain;format=tsv: {}
            text/plain;format=flatfile: {}
            text/plain;format=list: {}
            application/vnd.openxmlformats-officedocument.spreadsheetml.sheet: {}
            text/plain;format=fasta: {}
            text/plain;format=gff: {}
  "/uniprotkb/search":
    get:
      tags:
      - UniProtKB
      summary: Retrieve UniProtKB entries by a search query.
      description: The search endpoint uses a request query to return all entries
        associated with the search term in a paginated list of entries. Use ‘size’
        to specify the number of entries per page of results. Specify <tt>fields</tt>
        to return only data for specific sections of that entry that are of interest
        to you
      operationId: searchCursor
      parameters:
      - name: query
        in: query
        description: Criteria to search UniProtKB. Advanced queries can be built with
          parentheses and conditionals such as AND, OR and NOT. <a href='https://rest.uniprot.org/configure/uniprotkb/search-fields'
          target='_blank' rel='noopener noreferrer'>List of valid search fields</a>
        required: true
        schema:
          type: string
        example: insulin AND reviewed:true
      - name: fields
        in: query
        description: List of entry sections (fields) to be returned, separated by
          commas.  <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid fields</a>
        required: false
        schema:
          type: string
        example: accession,protein_name,cc_function,ft_binding
      - name: sort
        in: query
        description: Specify field by which to sort results. <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid sort fields</a>
        required: false
        schema:
          type: string
        example: accession desc
      - name: includeIsoform
        in: query
        description: Specify <tt>true</tt> to include isoforms, default is <tt>false</tt>.
        required: false
        schema:
          pattern: true|false
          type: boolean
      - name: size
        in: query
        description: Specify the number of entries per page of results (Pagination
          size). Default is 25, max is 500
        required: false
        schema:
          maximum: 500
          minimum: 0
          type: integer
          format: int32
        example: 50
      responses:
        default:
          description: UniProtKBEntry
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/SearchResult"
            application/xml:
              schema:
                type: array
                items:
                  "$ref": "#/components/schemas/Entry"
            text/plain;format=tsv: {}
            text/plain;format=flatfile: {}
            text/plain;format=list: {}
            application/vnd.openxmlformats-officedocument.spreadsheetml.sheet: {}
            text/plain;format=fasta: {}
            text/plain;format=gff: {}
components:
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
    Absorption:
      type: object
      properties:
        note:
          "$ref": "#/components/schemas/Note"
        approximate:
          type: boolean
        max:
          type: integer
          format: int32
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    AlternativeName:
      type: object
      properties:
        fullName:
          "$ref": "#/components/schemas/EvidencedStringType"
        shortName:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        ecNumber:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
    CitationType:
      type: object
      properties:
        title:
          type: string
        editorList:
          "$ref": "#/components/schemas/NameListType"
        authorList:
          "$ref": "#/components/schemas/NameListType"
        locator:
          type: string
        dbReference:
          type: array
          items:
            "$ref": "#/components/schemas/DbReferenceType"
        type:
          type: string
          xml:
            attribute: true
        date:
          type: string
          xml:
            attribute: true
        name:
          type: string
          xml:
            attribute: true
        volume:
          type: string
          xml:
            attribute: true
        first:
          type: string
          xml:
            attribute: true
        last:
          type: string
          xml:
            attribute: true
        publisher:
          type: string
          xml:
            attribute: true
        city:
          type: string
          xml:
            attribute: true
        db:
          type: string
          xml:
            attribute: true
        number:
          type: string
          xml:
            attribute: true
        institute:
          type: string
          xml:
            attribute: true
        country:
          type: string
          xml:
            attribute: true
    CofactorType:
      required:
      - dbReference
      - name
      type: object
      properties:
        name:
          type: string
        dbReference:
          "$ref": "#/components/schemas/DbReferenceType"
        evidence:
          type: array
          items:
            type: integer
            format: int32
    CommentType:
      type: object
      properties:
        molecule:
          "$ref": "#/components/schemas/MoleculeType"
        absorption:
          "$ref": "#/components/schemas/Absorption"
        kinetics:
          "$ref": "#/components/schemas/Kinetics"
        phDependence:
          "$ref": "#/components/schemas/PhDependence"
        redoxPotential:
          "$ref": "#/components/schemas/RedoxPotential"
        temperatureDependence:
          "$ref": "#/components/schemas/TemperatureDependence"
        reaction:
          "$ref": "#/components/schemas/ReactionType"
        physiologicalReaction:
          type: array
          items:
            "$ref": "#/components/schemas/PhysiologicalReactionType"
        cofactor:
          type: array
          items:
            "$ref": "#/components/schemas/CofactorType"
        subcellularLocation:
          type: array
          items:
            "$ref": "#/components/schemas/SubcellularLocationType"
        conflict:
          "$ref": "#/components/schemas/Conflict"
        link:
          type: array
          items:
            "$ref": "#/components/schemas/Link"
        event:
          type: array
          items:
            "$ref": "#/components/schemas/EventType"
        isoform:
          type: array
          items:
            "$ref": "#/components/schemas/IsoformType"
        interactant:
          type: array
          items:
            "$ref": "#/components/schemas/InteractantType"
        organismsDiffer:
          type: boolean
          default: false
        experiments:
          type: integer
          format: int32
        disease:
          "$ref": "#/components/schemas/Disease"
        location:
          type: array
          items:
            "$ref": "#/components/schemas/LocationType"
        text:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        type:
          type: string
          xml:
            attribute: true
        locationType:
          type: string
          xml:
            attribute: true
        name:
          type: string
          xml:
            attribute: true
        mass:
          type: number
          format: float
          xml:
            attribute: true
        error:
          type: string
          xml:
            attribute: true
        method:
          type: string
          xml:
            attribute: true
        evidence:
          type: array
          items:
            type: integer
            format: int32
    Component:
      type: object
      properties:
        recommendedName:
          "$ref": "#/components/schemas/RecommendedName"
        alternativeName:
          type: array
          items:
            "$ref": "#/components/schemas/AlternativeName"
        submittedName:
          type: array
          items:
            "$ref": "#/components/schemas/SubmittedName"
        allergenName:
          "$ref": "#/components/schemas/EvidencedStringType"
        biotechName:
          "$ref": "#/components/schemas/EvidencedStringType"
        cdAntigenName:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        innName:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
    Conflict:
      type: object
      properties:
        sequence:
          "$ref": "#/components/schemas/Sequence"
        type:
          type: string
          xml:
            attribute: true
        ref:
          type: string
          xml:
            attribute: true
    DbReferenceType:
      type: object
      properties:
        molecule:
          "$ref": "#/components/schemas/MoleculeType"
        property:
          type: array
          items:
            "$ref": "#/components/schemas/PropertyType"
        type:
          type: string
          xml:
            attribute: true
        id:
          type: string
          xml:
            attribute: true
        evidence:
          type: array
          items:
            type: integer
            format: int32
    Disease:
      type: object
      properties:
        diseaseId:
          type: string
        acronym:
          type: string
        diseaseCrossReference:
          "$ref": "#/components/schemas/CrossReferenceDiseaseDatabase"
        diseaseAccession:
          type: string
        description:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    Domain:
      type: object
      properties:
        recommendedName:
          "$ref": "#/components/schemas/RecommendedName"
        alternativeName:
          type: array
          items:
            "$ref": "#/components/schemas/AlternativeName"
        submittedName:
          type: array
          items:
            "$ref": "#/components/schemas/SubmittedName"
        allergenName:
          "$ref": "#/components/schemas/EvidencedStringType"
        biotechName:
          "$ref": "#/components/schemas/EvidencedStringType"
        cdAntigenName:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        innName:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
    Entry:
      required:
      - accession
      - name
      - organism
      - protein
      - proteinExistence
      - reference
      - sequence
      type: object
      properties:
        accession:
          type: array
          items:
            type: string
        name:
          type: array
          items:
            type: string
        protein:
          "$ref": "#/components/schemas/ProteinType"
        gene:
          type: array
          items:
            "$ref": "#/components/schemas/GeneType"
        organism:
          "$ref": "#/components/schemas/OrganismType"
        organismHost:
          type: array
          items:
            "$ref": "#/components/schemas/OrganismType"
        geneLocation:
          type: array
          items:
            "$ref": "#/components/schemas/GeneLocationType"
        reference:
          type: array
          items:
            "$ref": "#/components/schemas/ReferenceType"
        comment:
          type: array
          items:
            "$ref": "#/components/schemas/CommentType"
        dbReference:
          type: array
          items:
            "$ref": "#/components/schemas/DbReferenceType"
        proteinExistence:
          "$ref": "#/components/schemas/ProteinExistenceType"
        keyword:
          type: array
          items:
            "$ref": "#/components/schemas/KeywordType"
        feature:
          type: array
          items:
            "$ref": "#/components/schemas/FeatureType"
        evidence:
          type: array
          items:
            "$ref": "#/components/schemas/EvidenceType"
        sequence:
          "$ref": "#/components/schemas/SequenceType"
        dataset:
          type: string
          xml:
            attribute: true
        created:
          type: string
          format: date-time
          xml:
            attribute: true
        modified:
          type: string
          format: date-time
          xml:
            attribute: true
        version:
          type: integer
          format: int32
          xml:
            attribute: true
      xml:
        name: entry
        namespace: http://uniprot.org/uniprot
    EventType:
      type: object
      properties:
        type:
          type: string
          xml:
            attribute: true
    EvidenceType:
      type: object
      properties:
        source:
          "$ref": "#/components/schemas/SourceType"
        importedFrom:
          "$ref": "#/components/schemas/ImportedFromType"
        type:
          type: string
          xml:
            attribute: true
        key:
          type: integer
          xml:
            attribute: true
    EvidencedStringType:
      type: object
      properties:
        value:
          type: string
        evidence:
          type: array
          items:
            type: integer
            format: int32
    FeatureType:
      required:
      - location
      type: object
      properties:
        original:
          type: string
        variation:
          type: array
          items:
            type: string
        location:
          "$ref": "#/components/schemas/LocationType"
        ligand:
          "$ref": "#/components/schemas/LigandType"
        ligandPart:
          "$ref": "#/components/schemas/LigandPartType"
        type:
          type: string
          xml:
            attribute: true
        id:
          type: string
          xml:
            attribute: true
        description:
          type: string
          xml:
            attribute: true
        evidence:
          type: array
          items:
            type: integer
            format: int32
        ref:
          type: string
          xml:
            attribute: true
    GeneLocationType:
      type: object
      properties:
        name:
          type: array
          items:
            "$ref": "#/components/schemas/StatusType"
        type:
          type: string
          xml:
            attribute: true
        evidence:
          type: array
          items:
            type: integer
            format: int32
    GeneNameType:
      type: object
      properties:
        value:
          type: string
        evidence:
          type: array
          items:
            type: integer
            format: int32
        type:
          type: string
          xml:
            attribute: true
    GeneType:
      required:
      - name
      type: object
      properties:
        name:
          type: array
          items:
            "$ref": "#/components/schemas/GeneNameType"
    ImportedFromType:
      required:
      - dbReference
      type: object
      properties:
        dbReference:
          "$ref": "#/components/schemas/DbReferenceType"
    InteractantType:
      type: object
      properties:
        id:
          type: string
        label:
          type: string
        dbReference:
          "$ref": "#/components/schemas/DbReferenceType"
        intactId:
          type: string
          xml:
            attribute: true
    IsoformType:
      required:
      - id
      - name
      - sequence
      type: object
      properties:
        id:
          type: array
          items:
            type: string
        name:
          type: array
          items:
            "$ref": "#/components/schemas/Name"
        sequence:
          "$ref": "#/components/schemas/Sequence"
        text:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
    KeywordType:
      type: object
      properties:
        value:
          type: string
        evidence:
          type: array
          items:
            type: integer
            format: int32
        id:
          type: string
          xml:
            attribute: true
    Kinetics:
      type: object
      properties:
        km:
          type: array
          xml:
            name: KM
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        vmax:
          type: array
          xml:
            name: Vmax
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        text:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
    LigandPartType:
      required:
      - name
      type: object
      properties:
        name:
          type: string
        dbReference:
          "$ref": "#/components/schemas/DbReferenceType"
        label:
          type: string
        note:
          type: string
    LigandType:
      required:
      - name
      type: object
      properties:
        name:
          type: string
        dbReference:
          "$ref": "#/components/schemas/DbReferenceType"
        label:
          type: string
        note:
          type: string
    Lineage:
      required:
      - taxon
      type: object
      properties:
        taxon:
          type: array
          items:
            type: string
    Link:
      type: object
      properties:
        uri:
          type: string
          xml:
            attribute: true
    LocationType:
      type: object
      properties:
        begin:
          "$ref": "#/components/schemas/PositionType"
        end:
          "$ref": "#/components/schemas/PositionType"
        position:
          "$ref": "#/components/schemas/PositionType"
        sequence:
          type: string
          xml:
            attribute: true
    MoleculeType:
      type: object
      properties:
        value:
          type: string
        id:
          type: string
          xml:
            attribute: true
    NameListType:
      type: object
      properties:
        consortiumOrPerson:
          type: array
          items:
            type: object
    OrganismNameType:
      type: object
      properties:
        value:
          type: string
        type:
          type: string
          xml:
            attribute: true
    OrganismType:
      required:
      - dbReference
      - name
      type: object
      properties:
        name:
          type: array
          items:
            "$ref": "#/components/schemas/OrganismNameType"
        dbReference:
          type: array
          items:
            "$ref": "#/components/schemas/DbReferenceType"
        lineage:
          "$ref": "#/components/schemas/Lineage"
        evidence:
          type: array
          items:
            type: integer
            format: int32
    PhDependence:
      type: object
      properties:
        texts:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedValue"
    PhysiologicalReactionType:
      required:
      - dbReference
      type: object
      properties:
        dbReference:
          "$ref": "#/components/schemas/DbReferenceType"
        direction:
          type: string
          xml:
            attribute: true
        evidence:
          type: array
          items:
            type: integer
            format: int32
    PositionType:
      type: object
      properties:
        position:
          type: integer
          xml:
            attribute: true
        status:
          type: string
          xml:
            attribute: true
        evidence:
          type: array
          items:
            type: integer
            format: int32
    PropertyType:
      type: object
      properties:
        type:
          type: string
          xml:
            attribute: true
        value:
          type: string
          xml:
            attribute: true
    ProteinExistenceType:
      type: object
      properties:
        type:
          type: string
          xml:
            attribute: true
    ProteinType:
      type: object
      properties:
        recommendedName:
          "$ref": "#/components/schemas/RecommendedName"
        alternativeName:
          type: array
          items:
            "$ref": "#/components/schemas/AlternativeName"
        submittedName:
          type: array
          items:
            "$ref": "#/components/schemas/SubmittedName"
        allergenName:
          "$ref": "#/components/schemas/EvidencedStringType"
        biotechName:
          "$ref": "#/components/schemas/EvidencedStringType"
        cdAntigenName:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        innName:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        domain:
          type: array
          items:
            "$ref": "#/components/schemas/Domain"
        component:
          type: array
          items:
            "$ref": "#/components/schemas/Component"
    ReactionType:
      required:
      - dbReference
      - text
      type: object
      properties:
        text:
          type: string
        dbReference:
          type: array
          items:
            "$ref": "#/components/schemas/DbReferenceType"
        evidence:
          type: array
          items:
            type: integer
            format: int32
    RecommendedName:
      required:
      - fullName
      type: object
      properties:
        fullName:
          "$ref": "#/components/schemas/EvidencedStringType"
        shortName:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        ecNumber:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
    RedoxPotential:
      type: object
      properties:
        texts:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedValue"
    ReferenceType:
      required:
      - citation
      - scope
      type: object
      properties:
        citation:
          "$ref": "#/components/schemas/CitationType"
        scope:
          type: array
          items:
            type: string
        source:
          "$ref": "#/components/schemas/SourceDataType"
        evidence:
          type: array
          items:
            type: integer
            format: int32
        key:
          type: string
          xml:
            attribute: true
    SequenceType:
      type: object
      properties:
        value:
          type: string
        length:
          type: integer
          format: int32
          xml:
            attribute: true
        mass:
          type: integer
          format: int32
          xml:
            attribute: true
        checksum:
          type: string
          xml:
            attribute: true
        modified:
          type: string
          format: date-time
          xml:
            attribute: true
        version:
          type: integer
          format: int32
          xml:
            attribute: true
        precursor:
          type: boolean
          xml:
            attribute: true
        fragment:
          type: string
          xml:
            attribute: true
    SourceDataType:
      type: object
      properties:
        strainOrPlasmidOrTransposon:
          type: array
          items:
            type: object
    SourceType:
      type: object
      properties:
        dbReference:
          "$ref": "#/components/schemas/DbReferenceType"
        ref:
          type: integer
          xml:
            attribute: true
    StatusType:
      type: object
      properties:
        value:
          type: string
        status:
          type: string
          xml:
            attribute: true
    SubcellularLocationType:
      required:
      - location
      type: object
      properties:
        location:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        topology:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
        orientation:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
    SubmittedName:
      required:
      - fullName
      type: object
      properties:
        fullName:
          "$ref": "#/components/schemas/EvidencedStringType"
        ecNumber:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedStringType"
    TemperatureDependence:
      type: object
      properties:
        texts:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedValue"
    StreamResult:
      type: object
      properties:
        results:
          type: array
          items:
            "$ref": "#/components/schemas/UniProtKBEntry"
        error:
          type: string
    Facet:
      type: object
      properties:
        label:
          type: string
        name:
          type: string
        allowMultipleSelection:
          type: boolean
        values:
          type: array
          items:
            "$ref": "#/components/schemas/FacetItem"
    FacetItem:
      type: object
      properties:
        label:
          type: string
        value:
          type: string
        count:
          type: integer
          format: int64
    ProblemPair:
      type: object
      properties:
        code:
          type: integer
          format: int32
        message:
          type: string
    SearchResult:
      type: object
      properties:
        results:
          type: array
          items:
            "$ref": "#/components/schemas/UniProtKBEntry"
        facets:
          type: array
          items:
            "$ref": "#/components/schemas/Facet"
        matchedFields:
          type: array
          items:
            "$ref": "#/components/schemas/TermInfo"
        suggestions:
          type: array
          items:
            "$ref": "#/components/schemas/Suggestion"
        warnings:
          type: array
          items:
            "$ref": "#/components/schemas/ProblemPair"
    Suggestion:
      type: object
      properties:
        query:
          type: string
        hits:
          type: integer
          format: int64
    TermInfo:
      type: object
      properties:
        name:
          type: string
        hits:
          type: integer
          format: int64
    AlternativeProductsComment:
      type: object
      properties:
        isoforms:
          type: array
          items:
            "$ref": "#/components/schemas/APIsoform"
        note:
          "$ref": "#/components/schemas/Note"
        events:
          type: array
          items:
            type: string
            enum:
            - Alternative promoter usage
            - Alternative splicing
            - Alternative initiation
            - Ribosomal frameshifting
    MichaelisConstant:
      type: object
      properties:
        unit:
          type: string
          enum:
          - M
          - mM
          - uM
          - nM
          - mg/mL
          - mg/ml
        constant:
          type: number
          format: double
        substrate:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    SequenceCautionComment:
      type: object
      properties:
        note:
          type: string
        sequence:
          type: string
        sequenceCautionType:
          type: string
          enum:
          - Frameshift
          - Erroneous initiation
          - Erroneous termination
          - Erroneous gene model prediction
          - Erroneous translation
          - Miscellaneous discrepancy
          - unknown
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
        molecule:
          type: string
    Interactant:
      type: object
      properties:
        chainId:
          type: string
        intActId:
          type: string
        geneName:
          type: string
        uniProtKBAccession:
          type: string
    MaximumVelocity:
      type: object
      properties:
        velocity:
          type: number
          format: double
        enzyme:
          type: string
        unit:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    IsoformName:
      type: object
      properties:
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    Cofactor:
      type: object
      properties:
        cofactorCrossReference:
          "$ref": "#/components/schemas/CrossReferenceCofactorDatabase"
        name:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    SubcellularLocationValue:
      type: object
      properties:
        id:
          type: string
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    FreeTextComment:
      type: object
      properties:
        texts:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedValue"
        molecule:
          type: string
    CatalyticActivityComment:
      type: object
      properties:
        reaction:
          "$ref": "#/components/schemas/Reaction"
        physiologicalReactions:
          type: array
          items:
            "$ref": "#/components/schemas/PhysiologicalReaction"
        molecule:
          type: string
    DiseaseComment:
      type: object
      properties:
        disease:
          "$ref": "#/components/schemas/Disease"
        note:
          "$ref": "#/components/schemas/Note"
        molecule:
          type: string
    CrossReferenceReactionDatabase:
      type: object
      properties:
        database:
          type: string
          enum:
          - ChEBI
          - Rhea
        properties:
          type: array
          items:
            "$ref": "#/components/schemas/Property"
        id:
          type: string
    PhysiologicalReaction:
      type: object
      properties:
        directionType:
          type: string
          enum:
          - left-to-right
          - right-to-left
        reactionCrossReference:
          "$ref": "#/components/schemas/CrossReferenceReactionDatabase"
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    EvidencedValue:
      type: object
      properties:
        value:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    WebResourceComment:
      type: object
      properties:
        resourceUrl:
          type: string
        ftp:
          type: boolean
        resourceName:
          type: string
        note:
          type: string
        molecule:
          type: string
    CofactorComment:
      type: object
      properties:
        cofactors:
          type: array
          items:
            "$ref": "#/components/schemas/Cofactor"
        note:
          "$ref": "#/components/schemas/Note"
        valid:
          type: boolean
        molecule:
          type: string
    RnaEditingComment:
      type: object
      properties:
        positions:
          type: array
          items:
            "$ref": "#/components/schemas/RnaEdPosition"
        note:
          "$ref": "#/components/schemas/Note"
        locationType:
          type: string
          enum:
          - Not_applicable
          - Undetermined
          - Unknown
          - Known
        molecule:
          type: string
    InteractionComment:
      type: object
      properties:
        interactions:
          type: array
          items:
            "$ref": "#/components/schemas/Interaction"
    RnaEdPosition:
      type: object
      properties:
        position:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    Interaction:
      type: object
      properties:
        interactantOne:
          "$ref": "#/components/schemas/Interactant"
        organismDiffer:
          type: boolean
        interactantTwo:
          "$ref": "#/components/schemas/Interactant"
        numberOfExperiments:
          type: integer
          format: int32
    CrossReferenceCofactorDatabase:
      type: object
      properties:
        database:
          type: string
          enum:
          - ChEBI
          - ''
        properties:
          type: array
          items:
            "$ref": "#/components/schemas/Property"
        id:
          type: string
    BPCPComment:
      type: object
      properties:
        absorption:
          "$ref": "#/components/schemas/Absorption"
        phDependence:
          "$ref": "#/components/schemas/PhDependence"
        redoxPotential:
          "$ref": "#/components/schemas/RedoxPotential"
        temperatureDependence:
          "$ref": "#/components/schemas/TemperatureDependence"
        kineticParameters:
          "$ref": "#/components/schemas/KineticParameters"
        molecule:
          type: string
    Reaction:
      type: object
      properties:
        ecNumber:
          type: string
        reactionCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceReactionDatabase"
        name:
          type: string
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
    SubcellularLocationComment:
      type: object
      properties:
        note:
          "$ref": "#/components/schemas/Note"
        subcellularLocations:
          type: array
          items:
            "$ref": "#/components/schemas/SubcellularLocation"
        molecule:
          type: string
    APIsoform:
      type: object
      properties:
        sequenceIds:
          type: array
          items:
            type: string
        note:
          "$ref": "#/components/schemas/Note"
        synonyms:
          type: array
          items:
            "$ref": "#/components/schemas/IsoformName"
        isoformSequenceStatus:
          type: string
          enum:
          - Displayed
          - External
          - Not described
          - Described
        isoformIds:
          type: array
          items:
            type: string
        name:
          "$ref": "#/components/schemas/IsoformName"
    KineticParameters:
      type: object
      properties:
        note:
          "$ref": "#/components/schemas/Note"
        maximumVelocities:
          type: array
          items:
            "$ref": "#/components/schemas/MaximumVelocity"
        michaelisConstants:
          type: array
          items:
            "$ref": "#/components/schemas/MichaelisConstant"
    Note:
      type: object
      properties:
        valid:
          type: boolean
        texts:
          type: array
          items:
            "$ref": "#/components/schemas/EvidencedValue"
    CrossReferenceDiseaseDatabase:
      type: object
      properties:
        database:
          type: string
          enum:
          - MIM
          - ''
        properties:
          type: array
          items:
            "$ref": "#/components/schemas/Property"
        id:
          type: string
    MassSpectrometryComment:
      type: object
      properties:
        molWeight:
          type: number
          format: float
        molWeightError:
          type: number
          format: float
        evidences:
          type: array
          items:
            "$ref": "#/components/schemas/Evidence"
        note:
          type: string
        method:
          type: string
          enum:
          - Electrospray
          - FAB
          - LSI
          - MALDI
          - Plasma desorption
          - SELDI
          - API
          - Unknown
        molecule:
          type: string
    SubcellularLocation:
      type: object
      properties:
        orientation:
          "$ref": "#/components/schemas/SubcellularLocationValue"
        topology:
          "$ref": "#/components/schemas/SubcellularLocationValue"
        location:
          "$ref": "#/components/schemas/SubcellularLocationValue"
    Literature:
      type: object
      properties:
        pubmedId:
          type: integer
          format: int64
        doiId:
          type: string
        literatureAbstract:
          type: string
        completeAuthorList:
          type: boolean
        journal:
          type: string
        firstPage:
          type: string
        lastPage:
          type: string
        volume:
          type: string
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
        citationCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceCitationDatabase"
        title:
          type: string
        id:
          type: string
    Submission:
      type: object
      properties:
        submissionDatabase:
          type: string
          enum:
          - PDB data bank
          - PIR data bank
          - Swiss-Prot
          - UniProtKB
          - EMBL/GenBank/DDBJ databases
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
        citationCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceCitationDatabase"
        title:
          type: string
        id:
          type: string
    ElectronicArticle:
      type: object
      properties:
        journal:
          type: string
        locator:
          type: string
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
        citationCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceCitationDatabase"
        title:
          type: string
        id:
          type: string
    Thesis:
      type: object
      properties:
        institute:
          type: string
        address:
          type: string
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
        citationCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceCitationDatabase"
        title:
          type: string
        id:
          type: string
    Book:
      type: object
      properties:
        bookName:
          type: string
        editors:
          type: array
          items:
            type: string
        firstPage:
          type: string
        lastPage:
          type: string
        volume:
          type: string
        publisher:
          type: string
        address:
          type: string
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
        citationCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceCitationDatabase"
        title:
          type: string
        id:
          type: string
    Patent:
      type: object
      properties:
        patentNumber:
          type: string
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
        citationCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceCitationDatabase"
        title:
          type: string
        id:
          type: string
    JournalArticle:
      type: object
      properties:
        journal:
          type: string
        firstPage:
          type: string
        lastPage:
          type: string
        volume:
          type: string
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
        citationCrossReferences:
          type: array
          items:
            "$ref": "#/components/schemas/CrossReferenceCitationDatabase"
        title:
          type: string
        id:
          type: string
x-bioseq-client-name: UniProt
x-bioseq-integration:
  - name: accession-p05067
    operationId: getByAccession
    accept: application/json
    parameters:
      - {name: accession, in: path, value: P05067}
      - {name: fields, in: query, value: accession}
    expect:
      status: 200
      bodyContains:
        - '"primaryAccession"'
        - '"P05067"'
  - name: accession-p05067-fasta
    operationId: getByAccession
    accept: text/plain;format=fasta
    mode: batch
    count: 2
    parameters:
      - {name: accession, in: path, value: P05067}
    expect:
      status: 200
      bodyContains:
        - '>sp|P05067|'
        - MLPGLALL
      headerContains:
        - {name: Content-Type, valueContains: text/plain}
  - name: reviewed-pagination
    operationId: searchCursor
    parameters:
      - {name: query, in: query, value: 'reviewed:true'}
      - {name: fields, in: query, value: accession}
      - {name: size, in: query, value: 1}
    expect:
      status: 200
      bodyContains:
        - '"results"'
        - '"primaryAccession"'
      headerContains:
        - {name: Link, valueContains: 'rel="next"'}
)bioseq_openapi";

[[bioseq::api_client(spec)]] export struct bioseq_external_api_client;
