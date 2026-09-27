/**
 * @file external/ebi/entry_paths.cppm
 * @brief OpenAPI definitions for retrieving EBI Search entries and related data.
 */
module;

#include <string_view>

export module bioseq.external.ebi:entry_paths;

export namespace bioseq::external::ebi::descriptor {

/** Paths for entry lookup, cross-references, metadata downloads, and raw data. */
inline constexpr std::string_view entry_lookup_paths =
    R"bioseq_openapi(  "/{domain}/entry/{entryids}/xref/{targetdomainid}":
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
)bioseq_openapi";

/** Paths for finding similar entries in the same domain or a selected target domain. */
inline constexpr std::string_view related_entry_paths =
    R"bioseq_openapi(  "/{domain}/entry/{entryid}/morelikethis":
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
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
