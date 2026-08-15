/**
 * @file external/ebi/sequence_tool_paths.cppm
 * @brief EBI Search sequence-tool endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ebi:sequence_tool_paths;

export namespace bioseq::external::ebi::descriptor {

/** @brief Sequence-tool result and help path entries. */
inline constexpr std::string_view sequence_tool_paths =
    R"bioseq_openapi(  "/{domain}/seqtoolresults":
    get:
      tags:
      - Sequence analysis tool results search
      summary: Sequencing tool results search
      description: Get the results from a Sequencing tool search and map them to a
        domain, permitting to execute further text and facets filtering
      operationId: sequencingToolResultsSearch
      parameters:
      - name: Accept
        in: header
        description: Accept request HTTP header
        schema:
          type: string
      - name: domain
        in: path
        description: A single domain identifier
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
        example: uniprot
      - name: query
        in: query
        description: Query string
        schema:
          type: string
        example: tp53
      - name: filter
        in: query
        description: Filter queries (non-scoring queries that do not affect relevance)
        schema:
          type: array
          items:
            type: string
        example: TAXONOMY:9606
      - name: size
        in: query
        description: The number of entries to retrieve. limit 10,000 for list type
          and 100 for other types
        schema:
          type: string
          default: '15'
      - name: start
        in: query
        description: The index of the first entry in the results. Limit 100,000
        schema:
          type: string
          default: '0'
      - name: sortfield
        in: query
        description: A single field identifier to sort on. Not allowed to use with
          'sort' parameter
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
      - name: order
        in: query
        description: Whether to sort in ascending/descending order. Should come along
          with 'sortfield' parameter and not allowed to use with 'sort' parameter
        schema:
          pattern: "(?i)desc(ending)?|asc(ending)?|^$"
          type: string
      - name: sort
        in: query
        description: Comma separated values of sorting criteria. (field_id:order,
          e.g. boost:descending,length:descending). Should not be used in conjunction
          with any of 'sortfield' and 'order' parameters
        schema:
          pattern: "(?i)([^:,]+(:(desc(ending)?|asc(ending)?))?(,([^:,]+(:(desc(ending)?|asc(ending)?))?))*)?"
          type: string
      - name: sortignorenull
        in: query
        description: Ignore null values at sort time. If true, null values will always
          appear on the end
        schema:
          type: string
      - name: fields
        in: query
        description: Comma separated values of field identifiers to retrieve
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
      - name: fieldurl
        in: query
        description: Whether field links are included. The returned links mean direct
          URLs to the data entries in the original portals. Valid with XML/JSON only
        schema:
          type: string
          default: 'false'
      - name: viewurl
        in: query
        description: Whether other view links on an entry are included. Valid with
          XML/JSON only
        schema:
          type: string
          default: 'false'
      - name: facetfields
        in: query
        description: 'Comma separated values of field identifiers associated with
          facets to retrieve. In case of hierarchical facet, the value for the field
          can be a path where the nodes are separated by a ''/''. i.e.: taxonomy_lineage/1/10239/35268
          (taxonomy_lineage is the name of the hierarchical facet and nodes are possible
          values of the facet). Valid with XML/JSON only'
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
      - name: facetcount
        in: query
        description: The number of facet values to retrieve. In case of hierarchical
          facet, the facet count limit the number of children retrieved in a single
          level. Valid with XML/JSON only.
        schema:
          type: string
          default: '0'
      - name: facets
        in: query
        description: A comma separated list of selected facet values
        schema:
          pattern: "^[\\p{L}\\p{M}0-9.\\-\\/+=@_;:,'\\(\\)#& ]*$"
          type: string
      - name: feedtitle
        in: query
        description: RSS feed title and required when a selected format parameter
          is 'rss'
        schema:
          pattern: "^[\\p{L}\\p{M}0-9.\\-\\/+=@_:,() ]*$"
          type: string
      - name: feedmaxdays
        in: query
        description: The number of days for time window. always used with 'feedmasdaysfield'
          parameter. valid when format is 'rss'
        schema:
          type: string
      - name: feedmaxdaysfield
        in: query
        description: A date type field to set time window. always used with 'feedmaxdays'
          parameter. valid when format is 'rss'
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
      - name: entryattrs
        in: query
        description: Comma separated values of additional entry attributes. Valid
          with XML/JSON only.
        schema:
          pattern: "(score)+(,(score)|,)*|^$"
          type: string
      - name: hlfields
        in: query
        description: Comma separated values of field identifiers to apply highlighting.
          Valid with XML/JSON only
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
      - name: hlpretag
        in: query
        description: A string appearing before a highlighted term. Valid with XML/JSON
          only.
        schema:
          pattern: ^[a-zA-Z0-9.\-\/+=<>@_:" ]*$
          type: string
      - name: hlposttag
        in: query
        description: A string appearing after a highlighted term. Valid with XML/JSON
          only.
        schema:
          pattern: ^[a-zA-Z0-9.\-\/+=<>@_:" ]*$
          type: string
      - name: format
        in: query
        description: Response format
        schema:
          pattern: "(?i)xml|json|rss|csv|tsv|idlist|acclist|cs_idlist|cs_acclist|^$"
          type: string
      - name: searchposition
        in: query
        description: The search position from which to start the next request. Set
          to 0 to start a new bulk search.
        schema:
          pattern: "^[0-9a-zA-Z+,/]+=*$"
          type: string
      - name: toolid
        in: query
        description: Identifier of sequencing tool configured in the system. Please
          check the 'seqtoolresults/help' endpoint for more information
        required: true
        schema:
          type: string
      - name: tooloutputformat
        in: query
        description: 'Sequence Tool output format configured in the system (i.e.:
          ids) if missing a default will be used'
        schema:
          type: string
      - name: jobid
        in: query
        description: Identifier of sequencing tool job
        required: true
        schema:
          type: string
      - name: jobresultsstart
        in: query
        description: Sequence Tool results reading start index
        schema:
          type: integer
          format: int32
      - name: jobresultssize
        in: query
        description: Sequence Tool results reading size
        schema:
          type: integer
          format: int32
      - name: jobresultsmapfield
        in: query
        description: Sequence Tool results domain field to map on, default is 'id'
        schema:
          type: string
      - name: valuemappers
        in: query
        description: Comma separated list of 'value mappers' to use on the sequence
          tool results. Value mappers help in translating the values from the Sequence
          Search tool results to values in the EBI Search domain. A list of possible
          values are available in the system configuration
        schema:
          type: string
      responses:
        '400':
          description: Invalid parameters
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSError"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSError"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSError"
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSError"
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSError"
        default:
          description: A list of entries
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSSearchResult"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSSearchResult"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSSearchResult"
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSSearchResult"
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSSearchResult"
  "/{domain}/seqtoolresults/help":
    get:
      tags:
      - Sequence analysis tool results search configuration
      summary: Sequencing tool results search configuration
      description: It returns information about Sequencing Tool configured in the
        system and available to be used
      operationId: sequencingToolResultsSearchHelp
      parameters:
      - name: domain
        in: path
        required: true
        schema:
          type: string
      responses:
        '400':
          description: Invalid parameters
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSError"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSError"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSError"
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSError"
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSError"
        default:
          description: A list of configured tools
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSSeqToolsHelp"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSSeqToolsHelp"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSSeqToolsHelp"
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSSeqToolsHelp"
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSSeqToolsHelp"
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
