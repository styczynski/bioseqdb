/**
 * @file external/uniprot/search_schemas.cppm
 * @brief UniProt search response schemas for client generation.
 */
module;

#include <string_view>

export module bioseq.external.uniprot:search_schemas;

export namespace bioseq::external::uniprot::descriptor {

/** @brief Schemas for search results and suggested terms. */
inline constexpr std::string_view search_schemas = R"bioseq_openapi(    StreamResult:
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
)bioseq_openapi";

} // namespace bioseq::external::uniprot::descriptor
