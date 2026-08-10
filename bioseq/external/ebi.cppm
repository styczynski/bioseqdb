module;

#include <string_view>

export module bioseq.external.ebi;

constexpr static std::string_view spec = R"bioseq_openapi(
---
openapi: 3.0.1
info:
  title: 'The EBI Search: RESTful Web services'
  description: This is an API documentation for [EBI Search](https://www.ebi.ac.uk/ebisearch)
    RESTful Web services.
  contact:
    name: Web production in EMBL-EBI
    url: https://www.ebi.ac.uk/ebisearch
    email: www-prod@ebi.ac.uk
  license:
    name: Apache 2.0
    url: https://www.apache.org/licenses/LICENSE-2.0.html
  version: all
externalDocs:
  description: EBI Search RESTful Web services documentation
  url: https://www.ebi.ac.uk/ebisearch/swagger
servers:
- url: https://www.ebi.ac.uk/ebisearch/ws/rest
  description: EBI Search Production server
tags:
- name: Search
  description: Search
- name: Cross-reference search
  description: Cross-reference searching
- name: Auto complete
  description: Auto complete
- name: Top terms
  description: Top terms
- name: Sequence analysis tool results search
  description: Searching against sequence analysis tool results
- name: More like this
  description: More-like-this
- name: Raw data retrieval
  description: Raw data retrieval
paths:
  "/{domain}/autocomplete":
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
  "/{domain}/entry/{entryids}/xref/{targetdomainid}":
    get:
      tags:
      - Cross-reference search
      summary: Cross-reference search
      description: It returns a list of cross references in a target domain associated
        with an entry
      operationId: crossreference
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
      - name: entryids
        in: path
        description: Comma separated values of entry identifiers
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
        example: P53_HUMAN
      - name: targetdomainid
        in: path
        description: A single domain identifier to search on
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
        example: europepmc
      - name: size
        in: query
        description: The number of entries to retrieve. limit 100
        schema:
          type: string
          default: '15'
      - name: start
        in: query
        description: The index of the first entry in the results. limit 1,000,000
        schema:
          type: string
          default: '0'
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
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
      - name: query
        in: query
        description: Query string
        schema:
          type: string
        example: tp53
      - name: via
        in: query
        description: Comma seperated values of domain identifiers associated with
          finding cross-references
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
          description: A list of entries and facets if requried
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
  "/{domain}/download":
    get:
      tags:
      - Metadata download
      summary: Downloading metadata for supported domains
      description: Return a list of entry fields, one per line, for the given domain
      operationId: downloadMetadata
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
        example: epo
      - name: query
        in: query
        description: Query string
        schema:
          type: string
        example: tp53
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
      - name: fields
        in: query
        description: Comma separated values of field identifiers to retrieve
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
      - name: format
        in: query
        description: Response format
        schema:
          pattern: "(?i)csv|tsv|^$"
          type: string
      responses:
        '400':
          description: Invalid parameters
          content:
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSError"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSError"
        default:
          description: The metadata for all matching results in the domain
          content:
            text/tab-separated-values:
              schema:
                "$ref": "#/components/schemas/WSStreamingResult"
            text/csv:
              schema:
                "$ref": "#/components/schemas/WSStreamingResult"
  "/{domain}/entry/{entryids}":
    get:
      tags:
      - Search
      summary: Entry retrieval
      description: It returns entry information associated with entry identifiers
        provided
      operationId: entrySearch
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
      - name: entryids
        in: path
        description: Comma separated values of entry identifiers (max 100)
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
        example: P53_HUMAN
      - name: fields
        in: query
        description: Comma separated values of field identifiers to retrieve
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
        example: descRecName
      - name: fieldurl
        in: query
        description: Whether field links are included. The returned links mean direct
          URLs to the data entries in original portals. Valid with XML/JSON only
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
      - name: format
        in: query
        description: Response format
        schema:
          pattern: "(?i)xml|json|csv|tsv|idlist|acclist|cs_idlist|cs_acclist|^$"
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
  "/{domain}/entry/{entryid}/xref":
    get:
      tags:
      - Cross-reference search
      summary: Finding domains referred by an entry
      description: It returns a list of domains referred by an entry
      operationId: getDomainsReferencedInEntry
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
      - name: entryid
        in: path
        description: A single entry identifier
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
        example: P53_HUMAN
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
          description: A list of domains
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
  "/{domain}/xref":
    get:
      tags:
      - Cross-reference search
      summary: Finding domains referred by a domain
      description: It returns a list of domains referred by a domain
      operationId: getDomainsReferencedInDomain
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
          description: A list of domains
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
  "/{domain}/rawdata/{entryid}":
    get:
      tags:
      - Raw data retrieval
      summary: Retrieving raw data for supported domains
      description: It returns a raw data content for given domain and identifier
      operationId: getRawData
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
          pattern: "(?i)epo|jpo|kipo|taxonomy|uspto|^$"
          type: string
        example: epo
      - name: entryid
        in: path
        description: A single entry identifier
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
        example: CQ852354
      responses:
        '400':
          description: Invalid parameters
          content:
            text/plain:
              schema:
                "$ref": "#/components/schemas/WSError"
        default:
          description: Raw data of requested entry
          content:
            text/plain:
              schema:
                type: string
  "/log":
    post:
      operationId: handleLogging
      requestBody:
        content:
          application/json:
            schema:
              "$ref": "#/components/schemas/FrontEndLogRequest"
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/FrontEndLogResponse"
  "/{domain}/entry/{entryid}/morelikethis":
    get:
      tags:
      - More like this
      summary: More like this in a same domain
      description: It returns similar entries to an entry. In order to get better
        results it is necessary to tune parameters
      operationId: morelikethis
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
      - name: entryid
        in: path
        description: A single entry identifier of a base entry
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
        example: PXD001673
      - name: size
        in: query
        description: The number of entries to retrieve. limit 100
        schema:
          type: string
          default: '15'
      - name: start
        in: query
        description: The index of the first entry in the results. limit 1,000,000
        schema:
          type: string
          default: '0'
      - name: fields
        in: query
        description: Comma separated values of field identifiers to retrieve
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
        example: name
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
      - name: mltfields
        in: query
        description: Comma separated values of field identifiers to be used for generating
          a query
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
      - name: mintermfreq
        in: query
        description: The minimum term frequency. Any terms whose frequency is below
          this value will be ignored from forming a final query
        schema:
          type: string
          default: '1'
      - name: mindocfreq
        in: query
        description: The minimum document frequency. Any terms which do not occur
          in at least this number of entries will be ignored
        schema:
          type: string
          default: '5'
      - name: maxqueryterm
        in: query
        description: The maximum number of query terms that will be included in any
          generated query (max. 25)
        schema:
          type: string
          default: '10'
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
      - name: entryattrs
        in: query
        description: Comma separated values of additional entry attributes. Valid
          with XML/JSON only.
        schema:
          pattern: "(score)+(,(score)|,)*|^$"
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
  "/{domain}/entry/{entryid}/morelikethis/{targetdomainid}":
    get:
      tags:
      - More like this
      summary: More like this
      description: It returns similar entries in a target domain to an entry. In order
        to get better results it is necessary to tune parameters
      operationId: morelikethisInDomain
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
      - name: entryid
        in: path
        description: A single entry identifier of a base entry
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
          type: string
        example: PXD001673
      - name: size
        in: query
        description: The number of entries to retrieve. limit 100
        schema:
          type: string
          default: '15'
      - name: start
        in: query
        description: The index of the first entry in the results. limit 1,000,000
        schema:
          type: string
          default: '0'
      - name: fields
        in: query
        description: Comma separated values of field identifiers to retrieve
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
        example: name
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
      - name: mltfields
        in: query
        description: Comma separated values of field identifiers to be used for generating
          a query
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_:, ]*$"
          type: string
      - name: mintermfreq
        in: query
        description: The minimum term frequency. Any terms whose frequency is below
          this value will be ignored from forming a final query
        schema:
          type: string
          default: '1'
      - name: mindocfreq
        in: query
        description: The minimum document frequency. Any terms which do not occur
          in at least this number of entries will be ignored
        schema:
          type: string
          default: '5'
      - name: maxqueryterm
        in: query
        description: The maximum number of query terms that will be included in any
          generated query (max. 25)
        schema:
          type: string
          default: '10'
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
      - name: entryattrs
        in: query
        description: Comma separated values of additional entry attributes. Valid
          with XML/JSON only.
        schema:
          pattern: "(score)+(,(score)|,)*|^$"
          type: string
      - name: targetdomainid
        in: path
        description: A single domain identifier to search on
        required: true
        schema:
          pattern: "^[a-zA-Z0-9.\\-\\/+=@_: ]*$"
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
  "/":
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
  "/mail":
    post:
      operationId: sendEmail
      requestBody:
        content:
          application/json:
            schema:
              "$ref": "#/components/schemas/EmailRequest"
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/EmailResponse"
  "/{domain}/seqtoolresults":
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
  "/{domain}/topterms/{fieldid}":
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
components:
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
    StreamingResponseWSStreamingResult:
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
x-bioseq-client-name: EBI
x-bioseq-integration:
  - name: root-search
    operationId: rootsearch
    mode: single
    parameters:
      - {name: query, in: query, value: BRCA1}
    expect:
      status: 200
      bodyContains:
        - '"hitCount"'
)bioseq_openapi";

[[bioseq::api_client(spec)]] export struct bioseq_external_api_client;
