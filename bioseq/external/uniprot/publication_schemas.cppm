/**
 * @file external/uniprot/publication_schemas.cppm
 * @brief UniProt publication schema fragments.
 */
module;

#include <string_view>

export module bioseq.external.uniprot:publication_schemas;

export namespace bioseq::external::uniprot::descriptor {

/** @brief Literature, citation, author, and journal schemas. */
inline constexpr std::string_view publication_schemas = R"bioseq_openapi(    Literature:
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
)bioseq_openapi";

} // namespace bioseq::external::uniprot::descriptor
