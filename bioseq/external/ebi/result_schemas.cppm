/**
 * @file external/ebi/result_schemas.cppm
 * @brief OpenAPI schemas for EBI Search results and their fields.
 */
module;

#include <string_view>

export module bioseq.external.ebi:result_schemas;

export namespace bioseq::external::ebi::descriptor {

/** Opens components.schemas and defines search results, entries, and suppoting result types. */
inline constexpr std::string_view result_schemas = R"bioseq_openapi(components:
  schemas:
    WSSuggestion:
      type: object
      properties:
        suggestion:
          type: string
        formatted:
          type: string
    WSSuggestionResult:
      type: object
      properties:
        suggestions:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSSuggestion"
      xml:
        name: result
    WSDiagnostics:
      type: object
      properties:
        message:
          type: string
        expectedTokens:
          type: array
          xml:
            wrapped: true
          items:
            type: string
            xml:
              name: expectedToken
    WSError:
      type: object
      properties:
        diagnostics:
          "$ref": "#/components/schemas/WSDiagnostics"
      xml:
        name: result
    WSDeepLinkFragment:
      type: object
      properties:
        id:
          type: string
          xml:
            attribute: true
        fragments:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSFragment"
    WSDomain:
      type: object
      properties:
        id:
          type: string
          xml:
            attribute: true
        name:
          type: string
          xml:
            attribute: true
        alias:
          type: string
          xml:
            attribute: true
        description:
          type: string
          xml:
            attribute: true
        hitCount:
          type: integer
          format: int64
        indexInfos:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSIndexInfo"
        fieldInfos:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSFieldInfo"
        subdomains:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSDomain"
        referenceEntryCount:
          type: integer
          format: int64
          xml:
            attribute: true
    WSEntry:
      type: object
      properties:
        acc:
          type: string
          xml:
            attribute: true
        id:
          type: string
          xml:
            attribute: true
        source:
          type: string
          xml:
            attribute: true
        score:
          type: number
          format: float
          xml:
            attribute: true
        fields:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSField"
        fieldURLs:
          uniqueItems: true
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSURL"
        viewURLs:
          uniqueItems: true
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSURL"
        referenceCount:
          type: integer
          format: int64
        references:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSEntry"
        viewURLList:
          type: array
          items:
            "$ref": "#/components/schemas/WSURL"
        fieldMap:
          type: object
          additionalProperties:
            "$ref": "#/components/schemas/WSField"
        explanation:
          "$ref": "#/components/schemas/WSExplanation"
        firstfieldurl:
          "$ref": "#/components/schemas/WSURL"
        entryUrl:
          "$ref": "#/components/schemas/WSURL"
        highlights:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSHighlight"
        deepLinkFragments:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSDeepLinkFragment"
        referenceFacets:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSFacet"
    WSExplanation:
      type: object
      properties:
        match:
          type: boolean
          xml:
            attribute: true
        value:
          type: number
          format: double
          xml:
            attribute: true
        description:
          type: string
        explanation:
          type: array
          xml:
            name: details
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSExplanation"
    WSFacet:
      type: object
      properties:
        id:
          type: string
          xml:
            attribute: true
        label:
          type: string
          xml:
            attribute: true
        total:
          type: number
          format: double
          xml:
            attribute: true
        facetValues:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSFacetValue"
    WSFacetValue:
      type: object
      properties:
        label:
          type: string
        value:
          type: string
        count:
          type: integer
          format: int32
        children:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSFacetValue"
    WSField:
      type: object
      properties:
        id:
          type: string
          xml:
            attribute: true
        firstvalue:
          type: string
        values:
          type: array
          xml:
            wrapped: true
          items:
            type: string
            xml:
              name: value
    WSFieldInfo:
      type: object
      properties:
        id:
          type: string
          xml:
            attribute: true
        name:
          type: string
          xml:
            attribute: true
        description:
          type: string
          xml:
            attribute: true
        options:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSOption"
    WSFragment:
      type: object
      properties:
        prefix:
          type: string
          xml:
            attribute: true
        suffix:
          type: string
          xml:
            attribute: true
        token:
          type: string
          xml:
            attribute: true
    WSHighlight:
      type: object
      properties:
        id:
          type: string
          xml:
            attribute: true
        values:
          type: array
          xml:
            wrapped: true
          items:
            type: string
            xml:
              name: value
    WSIndexInfo:
      type: object
      properties:
        name:
          type: string
          xml:
            attribute: true
        value:
          type: string
    WSOption:
      type: object
      properties:
        name:
          type: string
          xml:
            attribute: true
        value:
          type: string
    WSSearchResult:
      type: object
      properties:
        hitCount:
          type: integer
          format: int64
        entries:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSEntry"
        facets:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSFacet"
        domains:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSDomain"
        searchPosition:
          type: string
        feedtitle:
          type: string
        requestedDomainId:
          type: string
        queryString:
          type: string
        selectedFacetValues:
          type: array
          items:
            type: string
        cache:
          type: boolean
        suggestedQuery:
          type: string
        entrysize:
          type: integer
          format: int32
        explain:
          type: array
          xml:
            name: explainText
            wrapped: true
          items:
            type: string
            xml:
              name: explain
      xml:
        name: result
    WSURL:
      type: object
      properties:
        name:
          type: string
          xml:
            attribute: true
        value:
          type: string
    WSStreamingResult:
      type: object
      properties:
        entries:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSEntry"
      xml:
        name: result
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
