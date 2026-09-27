/**
 * @file external/uniprot/operations.cppm
 * @brief UniProt endpoints used for client generation.
 */
module;

#include <string_view>

export module bioseq.external.uniprot:operations;

export namespace bioseq::external::uniprot::descriptor {

/** @brief Endpoints for accession lookup, search, and streamed results. */
inline constexpr std::string_view operation_paths = R"bioseq_openapi(  "/uniprotkb/{accession}":
    get:
      tags:
      - UniProtKB
      summary: Get UniProtKB entry by a single accession.
      description: Search UniProtKB by protein entry accession to return all data
        associated with that entry. Specify <tt>fields</tt> to return only data for
        specific sections of that entry that are of interest to you
      operationId: getByAccession
      parameters:
      - name: accession
        in: path
        description: Unique and stable identifier for each UniProtKB entry
        required: true
        schema:
          pattern: "([OPQ][0-9][A-Z0-9]{3}[0-9]|[A-NR-Z]([0-9][A-Z][A-Z0-9]{2}){1,2}[0-9])(((-[0-9]{1,3})?(\\[\\d+-\\d+\\])?)|(\\.[0-9]{1,3})|(_[A-Z0-9]{2,5}))?|[A-Z0-9]{2,5}_[A-Z0-9]{2,5}"
          type: string
        example: P05067
      - name: fields
        in: query
        description: List of entry sections (fields) to be returned, separated by
          commas.  <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid fields</a>
        required: false
        schema:
          type: string
        example: accession,protein_name,cc_function,ft_binding
      - name: version
        in: query
        description: Version of the entry. Versions are integers 1 or above; enter
          <tt>last</tt> for the latest version.</br>Please note that when passing
          <tt>version</tt> file formats are restricted to <tt>fasta</tt> and <tt>txt</tt>
          only
        required: false
        schema:
          type: string
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/UniProtKBEntry"
            application/xml:
              schema:
                "$ref": "#/components/schemas/Entry"
            text/plain;format=tsv: {}
            text/plain;format=flatfile: {}
            text/plain;format=list: {}
            application/vnd.openxmlformats-officedocument.spreadsheetml.sheet: {}
            text/plain;format=fasta: {}
            text/plain;format=gff: {}
            application/rdf+xml: {}
            text/turtle: {}
            application/n-triples: {}
  "/uniprotkb/stream":
    get:
      tags:
      - UniProtKB
      summary: Download UniProtKB entries retrieved by a search query. (Max. 10 million
        entries)
      description: The stream endpoint uses a request query to return all entries
        associated with the search term in a single download. Specify <tt>fields</tt>
        to return only data for specific sections of that entry that are of interest
        to you The stream endpoint has a maximum limit of 10 million entries. For
        larger requests, please use the 'UniProtKB asynchronous download job' requests
        described below. The 'UniProtKB asynchronous download job' requests can be
        used for any size -- the asynchronous download jobs can be paused and resumed
        at your convenience, unlike the stream endpoint.
      operationId: stream
      parameters:
      - name: query
        in: query
        description: Criteria to search UniProtKB. Advanced queries can be built with
          parentheses and conditionals such as AND, OR and NOT. <a href='https://rest.uniprot.org/configure/uniprotkb/search-fields'
          target='_blank' rel='noopener noreferrer'>List of valid search fields</a>
        required: true
        schema:
          type: string
        example: insulin AND reviewed:true
      - name: fields
        in: query
        description: List of entry sections (fields) to be returned, separated by
          commas.  <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid fields</a>
        required: false
        schema:
          type: string
        example: accession,protein_name,cc_function,ft_binding
      - name: sort
        in: query
        description: Specify field by which to sort results. <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid sort fields</a>
        required: false
        schema:
          type: string
        example: accession desc
      - name: includeIsoform
        in: query
        description: Specify <tt>true</tt> to include isoforms, default is <tt>false</tt>.
        required: false
        schema:
          pattern: true|false
          type: boolean
      - name: download
        in: query
        description: Specify <tt>true</tt> to download as file, default is <tt>false</tt>.
        required: false
        schema:
          pattern: "^true$|^false$"
          type: boolean
      responses:
        default:
          description: default response
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/StreamResult"
            application/xml:
              schema:
                type: array
                items:
                  "$ref": "#/components/schemas/Entry"
            text/plain;format=tsv: {}
            text/plain;format=flatfile: {}
            text/plain;format=list: {}
            application/vnd.openxmlformats-officedocument.spreadsheetml.sheet: {}
            text/plain;format=fasta: {}
            text/plain;format=gff: {}
  "/uniprotkb/search":
    get:
      tags:
      - UniProtKB
      summary: Retrieve UniProtKB entries by a search query.
      description: The search endpoint uses a request query to return all entries
        associated with the search term in a paginated list of entries. Use ‘size’
        to specify the number of entries per page of results. Specify <tt>fields</tt>
        to return only data for specific sections of that entry that are of interest
        to you
      operationId: searchCursor
      parameters:
      - name: query
        in: query
        description: Criteria to search UniProtKB. Advanced queries can be built with
          parentheses and conditionals such as AND, OR and NOT. <a href='https://rest.uniprot.org/configure/uniprotkb/search-fields'
          target='_blank' rel='noopener noreferrer'>List of valid search fields</a>
        required: true
        schema:
          type: string
        example: insulin AND reviewed:true
      - name: fields
        in: query
        description: List of entry sections (fields) to be returned, separated by
          commas.  <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid fields</a>
        required: false
        schema:
          type: string
        example: accession,protein_name,cc_function,ft_binding
      - name: sort
        in: query
        description: Specify field by which to sort results. <a href='https://rest.uniprot.org/configure/uniprotkb/result-fields'
          target='_blank' rel='noopener noreferrer'>List of valid sort fields</a>
        required: false
        schema:
          type: string
        example: accession desc
      - name: includeIsoform
        in: query
        description: Specify <tt>true</tt> to include isoforms, default is <tt>false</tt>.
        required: false
        schema:
          pattern: true|false
          type: boolean
      - name: size
        in: query
        description: Specify the number of entries per page of results (Pagination
          size). Default is 25, max is 500
        required: false
        schema:
          maximum: 500
          minimum: 0
          type: integer
          format: int32
        example: 50
      responses:
        default:
          description: UniProtKBEntry
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/SearchResult"
            application/xml:
              schema:
                type: array
                items:
                  "$ref": "#/components/schemas/Entry"
            text/plain;format=tsv: {}
            text/plain;format=flatfile: {}
            text/plain;format=list: {}
            application/vnd.openxmlformats-officedocument.spreadsheetml.sheet: {}
            text/plain;format=fasta: {}
            text/plain;format=gff: {}
)bioseq_openapi";

} // namespace bioseq::external::uniprot::descriptor
