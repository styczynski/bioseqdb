/**
 * @file external/ebi/search_paths.cppm
 * @brief OpenAPI definitions for EBI queries, suggestions, and frequent terms.
 */
module;

#include <string_view>

export module bioseq.external.ebi:search_paths;

export namespace bioseq::external::ebi::descriptor {

/** Path for query suggestions within a domain. */
inline constexpr std::string_view autocomplete_path = R"bioseq_openapi(  "/{domain}/autocomplete":
    get:
      tags:
      - Auto complete
      summary: Auto complete
      description: It returns a list of suggested queries based on a given term
      operationId: autocomplete
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
      - name: term
        in: query
        description: A term to get suggestions, whose length should be at least 3
          characters
        required: true
        schema:
          maxLength: 254
          minLength: 3
          type: string
        example: tpi
      - name: formatted
        in: query
        description: Whether to include suggestions in a highlighted form
        schema:
          type: string
          default: 'false'
      - name: format
        in: query
        description: Response format
        schema:
          pattern: "(?i)xml|json|csv|tsv|^$"
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
          description: A list of suggestions
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSSuggestionResult"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSSuggestionResult"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSSuggestionResult"
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSSuggestionResult"
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSSuggestionResult"
)bioseq_openapi";

/** Root and domain search paths, including metadata responses when no query is supplied. */
inline constexpr std::string_view search_paths = R"bioseq_openapi(  "/":
    get:
      tags:
      - Search
      summary: All EBI search
      description: If a query parameter is specified, it will return the numbers of
        hits in a domain hierarchy. Otherwise, return meta-data of all domains available
        in EBI Search
      operationId: rootsearch
      parameters:
      - name: Accept
        in: header
        description: Accept request HTTP header
        schema:
          type: string
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
      - name: format
        in: query
        description: Response format
        schema:
          pattern: "(?i)xml|json|csv|tsv|^$"
          type: string
      responses:
        '200':
          description: The numbers of hits in a domain hierarchy if query parameter
            is in present.
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
          description: Meta-data of all domains if query parameter is NOT in present
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
  "/{domain}":
    get:
      tags:
      - Search
      summary: Domain search
      description: If a query parameter is specified, it will return search results.
        Otherwise, return meta-data of the specified domain
      operationId: search
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
      responses:
        '200':
          description: Search results on a specified domain if query parameter is
            in present.
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSSearchResult"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSSearchResult"
            text/xml; charset=UTF-8:
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
        '400':
          description: Invalid parameters
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSError"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSError"
            text/xml; charset=UTF-8:
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
          description: Meta-data of a specified domain if query parameter is NOT in
            present
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            text/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSDomainMetadataResult"
)bioseq_openapi";

/** Path for the most frequent terms in a domain field. */
inline constexpr std::string_view top_terms_path =
    R"bioseq_openapi(  "/{domain}/topterms/{fieldid}":
    get:
      tags:
      - Top terms
      summary: Top terms
      description: It returns a list of top terms of a field
      operationId: topterms
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
        example: pride
      - name: fieldid
        in: path
        description: A single field identifier
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
        example: data_protocol
      - name: size
        in: query
        description: The number of entries to retrieve. limit 100
        schema:
          type: string
          default: '15'
      - name: excludes
        in: query
        description: Comma separated values of terms to be excluded (e.g. stop-words)
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
        example: to,the
      - name: excludesets
        in: query
        description: Comma separated values of stop-word sets to be excluded
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
      - name: format
        in: query
        description: Response format
        schema:
          pattern: "(?i)xml|json|csv|tsv|^$"
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
          description: A list of top terms
          content:
            application/xml; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSTopTermsResult"
            application/json; charset=UTF-8:
              schema:
                "$ref": "#/components/schemas/WSTopTermsResult"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSTopTermsResult"
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSTopTermsResult"
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSTopTermsResult"
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
