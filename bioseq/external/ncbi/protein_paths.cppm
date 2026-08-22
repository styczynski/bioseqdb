/**
 * @file external/ncbi/protein_paths.cppm
 * @brief NCBI protein endpoint fragments.
 */
module;

#include <string_view>

export module bioseq.external.ncbi:protein_paths;

export namespace bioseq::external::ncbi::descriptor {

/** @brief Protein accession download path entries. */
inline constexpr std::string_view protein_download_paths =
    R"bioseq_openapi(  /protein/accession/{accessions}/download:
    get:
      summary: Get a prokaryote gene data package by RefSeq protein accession
      description: 'Download a prokaryote gene data package including sequence, annotation and data reports by RefSeq non-redundant protein accession.'
      tags:
        - Prokaryote
      operationId: download_prokaryote_gene_package
      responses:
        default:
          description: An unexpected error response.
          content:
            text/plain:
              schema:
                $ref: '#/components/schemas/rpcStatus'
        '200':
          description: A successful response
          content:
            application/zip:
              schema:
                format: binary
                type: string
                description: Zip compressed stream
      parameters:
      - name: accessions
        description: 'One or more RefSeq non-redundant protein accessions (preceded by `WP_`)'
        in: path
        required: true
        schema:
          type: array
          items:
            type: string
        examples:
          example-0:
            value: WP_015878339.1
            summary: DNA-deoxyinosine glycosylase
          example-1:
            value: WP_001040085.1
            summary: type II CRISPR RNA-guided endonuclease Cas9
      - name: include_annotation_type
        description: 'Specify which sequence files to include in the data package. Note that RNA, CDS, 5'' UTR and 3'' UTR sequences are not available for WP_ accessions'
        in: query
        required: false
        schema:
          type: array
          items:
            $ref: '#/components/schemas/v2Fasta'
        examples:
          example-0:
            value: FASTA_GENE
            summary: Include gene sequence file, which includes gene sequences for all genomes on which the protein is annotated
          example-1:
            value:
              - FASTA_GENE
              - FASTA_PROTEIN
            summary: Include gene and protein sequence files
      - name: gene_flank_config.length
        in: query
        required: false
        schema:
          type: integer
      - name: taxon
        description: 'Limit gene sequences and the genome assemblies described by the [annotation report file](https://www.ncbi.nlm.nih.gov/datasets/docs/v2/reference-docs/data-reports/prokaryote-gene-location/) to the specified NCBI Taxonomy ID or name (common or scientific) at any taxonomic rank.'
        in: query
        required: false
        schema:
          type: string
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
  /protein/accession/download:
    post:
      summary: Get a prokaryote gene data package by RefSeq protein accession
      description: 'Download a prokaryote gene data package including sequence, annotation and data reports by RefSeq non-redundant protein accession.'
      tags:
        - Prokaryote
      operationId: download_prokaryote_gene_package_post
      responses:
        default:
          description: An unexpected error response.
          content:
            text/plain:
              schema:
                $ref: '#/components/schemas/rpcStatus'
        '200':
          description: A successful response
          content:
            application/zip:
              schema:
                format: binary
                type: string
                description: Zip compressed stream
      requestBody:
        required: true
        content:
          application/json:
            schema:
              $ref: '#/components/schemas/v2ProkaryoteGeneRequest'
            examples:
              Multiple Prokaryote (WP_) accessions example:
                description: DNA-deoxyinosine glycosylase (WP_015878339.1) and type II CRISPR RNA-guided endonuclease Cas9 (WP_001040085.1), with gene and protein sequence files
                value:
                  accessions:
                  - WP_015878339.1
                  - WP_001040085.1
                  include_annotation_type:
                  - FASTA_GENE
                  - FASTA_PROTEIN
      parameters:
      - name: filename
        description: Output file name.
        in: query
        required: false
        schema:
          type: string
          default: ncbi_dataset.zip
)bioseq_openapi";

} // namespace bioseq::external::ncbi::descriptor
