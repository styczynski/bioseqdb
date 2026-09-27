/**
 * @file external/uniprot/xml_schemas.cppm
 * @brief UniProt XML-oriented schema fragments.
 */
module;

#include <string_view>

export module bioseq.external.uniprot:xml_schemas;

export namespace bioseq::external::uniprot::descriptor {

/** @brief XML-derived protein annotation schemas. */
inline constexpr std::string_view xml_schemas = R"bioseq_openapi(    Absorption:
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
)bioseq_openapi";

} // namespace bioseq::external::uniprot::descriptor
