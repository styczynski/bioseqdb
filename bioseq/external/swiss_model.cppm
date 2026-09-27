/**
 * @file external/swiss_model.cppm
 * @brief SWISS-MODEL OpenAPI descriptor consumed by external client codegen.
 */
module;

#include <string_view>

export module bioseq.external.swiss_model;

constexpr static std::string_view spec = R"bioseq_openapi(
---
info:
  title: SWISS-MODEL Repository API
  version: '2.0'
  description: SWISS-MODEL Repository API service. The service returns a list of homology
    models, PDB structures and the target sequence for a UniProtKB Accession.
openapi: 3.0.1
paths:
  "/uniprot/{identifier}.json":
    get:
      tags:
      - Fetch metadata / coordinates
      parameters:
      - name: identifier
        description: UniProtKB accession number (AC) or entry name (ID)
        in: path
        required: true
        schema:
          type: string
      - name: provider
        in: query
        required: false
        schema:
          type: string
          enum:
          - swissmodel
          - pdb
        explode: true
      - name: template
        description: Template is 4 letter PDB code, or 4 letter code with assembly
          ID and chain for SMTL entries
        in: query
        required: false
        schema:
          type: string
      - name: range
        description: Return models with residues which map to UniProt residue range,
          use the format 'from-to'
        in: query
        required: false
        schema:
          type: string
          pattern: "^[0-9]+-[0-9]+$"
      responses:
        '200':
          description: Response includes query parameter, UniProt sequence summary
            and list of structures which may be empty
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/SWISS-MODEL Repository"
  "/uniprot/{identifiers}.json":
    get:
      tags:
      - Fetch metadata / coordinates
      parameters:
      - name: identifiers
        description: UniProtKB accession number (AC) or entry name (ID), comma separated
          list, maximum 250
        in: path
        required: true
        schema:
          type: string
      - name: provider
        in: query
        required: false
        schema:
          type: string
          enum:
          - swissmodel
          - pdb
        explode: true
      - name: template
        description: Template is 4 letter PDB code, or 4 letter code with assembly
          ID and chain for SMTL entries
        in: query
        required: false
        schema:
          type: string
      - name: range
        description: Return models with residues which map to UniProt residue range,
          use the format 'from-to'
        in: query
        required: false
        schema:
          type: string
          pattern: "^[0-9]+-[0-9]+$"
      responses:
        '200':
          description: Response includes query parameter, UniProt sequence summary
            and list of structures which may be empty
          content:
            application/json:
              schema:
                "$ref": "#/components/schemas/SWISS-MODEL Repository Resultset"
  "/uniprot/{identifier}.pdb":
    get:
      tags:
      - Fetch metadata / coordinates
      parameters:
      - name: identifier
        description: UniProtKB accession number (AC) or entry name (ID)
        in: path
        required: true
        schema:
          type: string
      - name: sort
        in: query
        required: false
        schema:
          type: string
          enum:
          - seqid
          - seqsim
      - name: provider
        in: query
        required: false
        schema:
          type: string
          enum:
          - swissmodel
          - pdb
        explode: true
      - name: template
        description: Template is 4 letter PDB code, or 4 letter code with assembly
          ID and chain for SMTL entries
        in: query
        required: false
        schema:
          type: string
      - name: range
        description: Specify a UniProt sequence residue range
        in: query
        required: false
        schema:
          type: string
          pattern: "^[0-9]+-[0-9]+$"
      responses:
        '200':
          description: Single PDB format coordinates as attachment
          content:
            application/json:
              schema:
                type: string
                format: binary
        '404':
          description: If no model or structure exists for given UniProt identifier
servers:
- url: https://swissmodel.expasy.org/repository
  description: SWISS-MODEL Repository
components:
  schemas:
    SWISS-MODEL Repository:
      type: object
      properties:
        api_version:
          type: string
        query:
          type: object
          properties:
            identifier:
              type: string
            provider:
              type: string
            template:
              type: string
            from:
              type: integer
            to:
              type: integer
        query_date:
          type: string
        result:
          type: object
          "$ref": "#/components/schemas/result"
    SWISS-MODEL Repository Resultset:
      type: object
      properties:
        api_version:
          type: string
        query:
          type: object
          properties:
            identifiers:
              type: string
            provider:
              type: string
            template:
              type: string
            from:
              type: integer
            to:
              type: integer
        query_date:
          type: string
        resultset:
          type: array
          items:
            "$ref": "#/components/schemas/result"
    result:
      type: object
      properties:
        sequence:
          type: string
        sequence_length:
          type: integer
        crc64:
          description: Cyclic redundancy check of full UniProtKB sequence.
          type: string
        md5:
          description: MD5 hash of full UniProtKB sequence.
          type: string
        uniprot_entries:
          type: array
          items:
            type: object
            properties:
              ac:
                type: string
              id:
                type: string
        structures:
          type: array
          items:
            type: object
            properties:
              chains:
                type: array
                items:
                  type: object
                  properties:
                    id:
                      type: string
                    segments:
                      type: array
                      items:
                        type: object
                        properties:
                          pdb:
                            type: object
                            properties:
                              aligned_sequence:
                                type: string
                              description:
                                type: string
                              from:
                                type: integer
                              to:
                                type: integer
                          smtl:
                            type: object
                            properties:
                              aligned_sequence:
                                type: string
                              description:
                                type: string
                              from:
                                type: integer
                              to:
                                type: integer
                          uniprot:
                            type: object
                            properties:
                              aligned_sequence:
                                type: string
                              from:
                                type: integer
                              to:
                                type: integer
              coordinates:
                description: A URL to retrieve coordinates in PDB format. Note the
                  model in SMR may have been updated since this metadata file was
                  created.
                type: string
              coverage:
                type: number
              crc64:
                type: string
              created_date:
                description: For models, this is the creation date. For PDB Structures,
                  this is the release date.
                type: string
              from:
                description: The minimum UniProtKB residue this model maps to.
                type: integer
              gmqe:
                type: number
              identity:
                type: number
              in_complex_with:
                type: array
                items:
                  type: object
                  properties:
                    chain_id:
                      type: string
                    description:
                      type: string
                    uniprot_ac:
                      type: string
              method:
                type: string
              oligo-state:
                type: string
              provider:
                type: string
              qmean:
                description: A qmean avg_local_score >= -5 is required for a model
                  to be in SWISS-MODEL Repository.
                type: object
                properties:
                  acc_agreement_norm_score:
                    type: number
                  acc_agreement_z_score:
                    type: number
                  avg_local_score:
                    type: number
                  avg_local_score_error:
                    type: number
                  cbeta_norm_score:
                    type: number
                  cbeta_z_score:
                    type: number
                  interaction_norm_score:
                    type: number
                  interaction_z_score:
                    type: number
                  packing_norm_score:
                    type: number
                  packing_z_score:
                    type: number
                  qmean4_norm_score:
                    type: number
                  qmean4_z_score:
                    type: number
                  qmean6_norm_score:
                    type: number
                  qmean6_z_score:
                    type: number
                  ss_agreement_norm_score:
                    type: number
                  ss_agreement_z_score:
                    type: number
                  torsion_norm_score:
                    type: number
                  torsion_z_score:
                    type: number
              similarity:
                type: number
              template:
                type: string
              to:
                description: The maximum UniProtKB residue this model maps to.
                type: integer
x-bioseq-client-name: SWISS-MODEL
x-bioseq-integration:
  - name: uniprot-q6gzx4
    operationId: get_uniprot_identifier_json_path_2f756e6970726f742f7b6964656e7469666965727d2e6a736f6e
    parameters:
      - {name: identifier, in: path, value: Q6GZX4}
    expect:
      status: 200
      bodyContains:
        - '"api_version"'
        - Q6GZX4
        - '"structures"'
)bioseq_openapi";

/** @brief Code-generation anchor for the SWISS-MODEL client. */
[[bioseq::api_client(spec)]] export struct bioseq_external_api_client;
