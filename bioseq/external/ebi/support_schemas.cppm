/**
 * @file external/ebi/support_schemas.cppm
 * @brief EBI Search support schema fragments.
 */
module;

#include <string_view>

export module bioseq.external.ebi:support_schemas;

export namespace bioseq::external::ebi::descriptor {

/** @brief Streaming, domain, notification, sequence-tool, and term schemas. */
inline constexpr std::string_view support_schemas =
    R"bioseq_openapi(    StreamingResponseWSStreamingResult:
      type: object
    WSDomainMetadataResult:
      type: object
      properties:
        domains:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSDomain"
      xml:
        name: result
    ConstrainedReferencedDomainsRequest:
      type: object
      properties:
        queries:
          type: array
          items:
            "$ref": "#/components/schemas/ConstrainedReferencedDomainsRequestItem"
    ConstrainedReferencedDomainsRequestItem:
      type: object
      properties:
        alias:
          type: string
        domainId:
          type: string
        query:
          type: string
    FrontEndLogResponse:
      type: object
      properties:
        status:
          type: string
    FrontEndLogRequest:
      type: object
      properties:
        logMessage:
          type: string
        logType:
          type: string
    EmailResponse:
      type: object
      properties:
        status:
          type: boolean
    EmailRequest:
      type: object
      properties:
        emailSubject:
          type: string
        emailBody:
          type: string
        userEmailId:
          type: string
    WSSeqToolsHelp:
      type: object
      properties:
        valueMappers:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSValueMapper"
        sequenceToolReaders:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSSequenceToolReader"
      xml:
        name: seqToolHelp
    WSSequenceToolReader:
      type: object
      properties:
        id:
          type: string
        description:
          type: string
        resultsOutputFormats:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSSequenceToolResultsOutputFormat"
      xml:
        name: sequenceToolReader
    WSSequenceToolResultsOutputFormat:
      type: object
      properties:
        id:
          type: string
        description:
          type: string
      xml:
        name: resultsOutputFormat
    WSValueMapper:
      type: object
      properties:
        id:
          type: string
        description:
          type: string
      xml:
        name: valueMapper
    WSTermStats:
      type: object
      properties:
        text:
          type: string
        docFreq:
          type: integer
          format: int64
    WSTopTermsResult:
      type: object
      properties:
        totalTermCount:
          type: integer
          format: int64
        topTerms:
          type: array
          xml:
            wrapped: true
          items:
            "$ref": "#/components/schemas/WSTermStats"
      xml:
        name: result
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
