/**
 * @file external/uniprot/annotation_schemas.cppm
 * @brief UniProt protein annotation schemas for client generation.
 */
module;

#include <string_view>

export module bioseq.external.uniprot:annotation_schemas;

export namespace bioseq::external::uniprot::descriptor {

/** @brief Schemas for protein comments, reactions, and  cellular locations. */
inline constexpr std::string_view annotation_schemas =
    R"bioseq_openapi(    AlternativeProductsComment:
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
)bioseq_openapi";

} // namespace bioseq::external::uniprot::descriptor
