/**
 * @file external/ucsc.cppm
 * @brief OpenAPI description used to generate the UCSC client.
 */
module;

#include <string_view>

export module bioseq.external.ucsc;

// Embed the UCSC OpenAPI document here for client generation.
constexpr static std::string_view spec = R"bioseq_openapi(
openapi: 3.0.0
info:
  title: UCSC Genome Browser REST API
  description: >-
    The UCSC Genome Browser REST API provides programmatic access to genomic data,
    sequences, annotation tracks, genome assemblies, and track hubs. All endpoints
    return JSON and require no authentication. Rate limiting of one request per second
    is recommended to avoid the botDelay system.
  version: "1.0.0"
  contact:
    name: UCSC Genome Browser
    url: https://genome.ucsc.edu
servers:
  - url: https://api.genome.ucsc.edu
    description: UCSC Primary API Server
  - url: https://genome-euro.ucsc.edu/cgi-bin/hubApi
    description: UCSC Europe Mirror
  - url: https://genome-asia.ucsc.edu/cgi-bin/hubApi
    description: UCSC Asia Mirror
tags:
  - name: Genomes
    description: Genome assembly discovery and search
  - name: Tracks
    description: Annotation track listings and data retrieval
  - name: Sequences
    description: DNA sequence retrieval
  - name: Hubs
    description: Track hub management
  - name: Search
    description: Genome browser search
paths:
  /findGenome:
    get:
      operationId: findGenome
      summary: Find Genome
      description: Search for genomes by name, accession number, or keywords across NCBI and UCSC assemblies.
      tags:
        - Genomes
      parameters:
        - name: q
          in: query
          required: true
          schema:
            type: string
          description: Search query string (genome name, accession, or keyword).
        - name: browser
          in: query
          required: false
          schema:
            type: string
          description: Filter by browser name.
        - name: year
          in: query
          required: false
          schema:
            type: integer
          description: Filter assemblies by year.
        - name: category
          in: query
          required: false
          schema:
            type: string
          description: Filter by category.
        - name: status
          in: query
          required: false
          schema:
            type: string
          description: Filter by assembly status.
        - name: level
          in: query
          required: false
          schema:
            type: string
          description: Filter by assembly level.
        - name: statsOnly
          in: query
          required: false
          schema:
            type: integer
            enum: [0, 1]
          description: Return only statistics if set to 1.
        - name: maxItemsOutput
          in: query
          required: false
          schema:
            type: integer
            default: 1000000
          description: Maximum number of items to return.
      responses:
        '200':
          description: Matching genomes.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/GenomeSearchResult'

  /list/publicHubs:
    get:
      operationId: listPublicHubs
      summary: List Public Hubs
      description: Returns all publicly available track hubs registered with UCSC.
      tags:
        - Hubs
      responses:
        '200':
          description: List of public track hubs.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/HubListResult'

  /list/ucscGenomes:
    get:
      operationId: listUcscGenomes
      summary: List UCSC Genomes
      description: Returns all genome database assemblies available in the UCSC Genome Browser.
      tags:
        - Genomes
      responses:
        '200':
          description: List of UCSC genome assemblies.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/GenomeListResult'

  /list/genarkGenomes:
    get:
      operationId: listGenarkGenomes
      summary: List GenArk Genomes
      description: Returns genome assemblies from the GenArk repository.
      tags:
        - Genomes
      parameters:
        - name: genome
          in: query
          required: false
          schema:
            type: string
          description: Filter by genome assembly name.
        - name: maxItemsOutput
          in: query
          required: false
          schema:
            type: integer
          description: Maximum number of items to return.
      responses:
        '200':
          description: GenArk genome assemblies.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/GenomeListResult'

  /list/hubGenomes:
    get:
      operationId: listHubGenomes
      summary: List Hub Genomes
      description: Returns the list of genomes available in a specific track hub.
      tags:
        - Hubs
      parameters:
        - name: hubUrl
          in: query
          required: true
          schema:
            type: string
            format: uri
          description: URL of the hub.txt file for the track hub.
      responses:
        '200':
          description: Genomes in the specified hub.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/GenomeListResult'

  /list/files:
    get:
      operationId: listFiles
      summary: List Genome Files
      description: Returns download files available for a genome assembly.
      tags:
        - Genomes
      parameters:
        - name: genome
          in: query
          required: true
          schema:
            type: string
          description: Genome assembly identifier (e.g., hg38, mm39).
        - name: format
          in: query
          required: false
          schema:
            type: string
          description: File format filter (e.g., bigBed, bigWig).
        - name: maxItemsOutput
          in: query
          required: false
          schema:
            type: integer
          description: Maximum number of files to return.
      responses:
        '200':
          description: List of downloadable files for the genome assembly.
          content:
            application/json:
              schema:
                type: object

  /list/tracks:
    get:
      operationId: listTracks
      summary: List Tracks
      description: Returns all data tracks available for a given genome assembly or hub.
      tags:
        - Tracks
      parameters:
        - name: genome
          in: query
          required: true
          schema:
            type: string
          description: Genome assembly identifier (e.g., hg38, mm39). Required unless hubUrl is provided.
        - name: hubUrl
          in: query
          required: false
          schema:
            type: string
            format: uri
          description: URL of the hub.txt file for a track hub.
        - name: trackLeavesOnly
          in: query
          required: false
          schema:
            type: integer
            enum: [0, 1]
            default: 0
          description: Set to 1 to return only leaf tracks (hide composite containers).
      responses:
        '200':
          description: List of tracks for the genome.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/TrackListResult'

  /list/chromosomes:
    get:
      operationId: listChromosomes
      summary: List Chromosomes
      description: Returns the list of chromosomes in a genome assembly or specific track.
      tags:
        - Genomes
      parameters:
        - name: genome
          in: query
          required: true
          schema:
            type: string
          description: Genome assembly identifier (e.g., hg38, mm39).
        - name: hubUrl
          in: query
          required: false
          schema:
            type: string
            format: uri
          description: URL of the hub.txt file for a track hub.
        - name: track
          in: query
          required: false
          schema:
            type: string
          description: Filter chromosomes by a specific track.
      responses:
        '200':
          description: List of chromosomes.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/ChromosomeListResult'

  /list/schema:
    get:
      operationId: getTrackSchema
      summary: Get Track Schema
      description: Returns the schema and field definitions for a specific annotation track.
      tags:
        - Tracks
      parameters:
        - name: genome
          in: query
          required: true
          schema:
            type: string
          description: Genome assembly identifier (e.g., hg38, mm39).
        - name: track
          in: query
          required: true
          schema:
            type: string
          description: Track name to return schema for.
        - name: hubUrl
          in: query
          required: false
          schema:
            type: string
            format: uri
          description: URL of the hub.txt file for a track hub.
      responses:
        '200':
          description: Track schema definition.
          content:
            application/json:
              schema:
                type: object

  /getData/sequence:
    get:
      operationId: getDnaSequence
      summary: Get DNA Sequence
      description: Retrieves a DNA sequence from a genome assembly for a specified genomic region.
      tags:
        - Sequences
      parameters:
        - name: genome
          in: query
          required: true
          schema:
            type: string
          description: Genome assembly identifier (e.g., hg38, mm39).
        - name: chrom
          in: query
          required: true
          schema:
            type: string
          description: Chromosome identifier (e.g., chr1, chrM).
        - name: start
          in: query
          required: false
          schema:
            type: integer
          description: Start position (0-based, half-open coordinates).
        - name: end
          in: query
          required: false
          schema:
            type: integer
          description: End position (1-based, half-open coordinates).
        - name: revComp
          in: query
          required: false
          schema:
            type: integer
            enum: [0, 1]
          description: Set to 1 to return the reverse complement sequence.
        - name: hubUrl
          in: query
          required: false
          schema:
            type: string
            format: uri
          description: URL of the hub.txt file for a sequence hub.
      responses:
        '200':
          description: DNA sequence data.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/SequenceResult'
        '400':
          description: Invalid parameters.

  /getData/track:
    get:
      operationId: getTrackData
      summary: Get Track Data
      description: Retrieves annotation data from a specific track for a genomic region.
      tags:
        - Tracks
      parameters:
        - name: genome
          in: query
          required: true
          schema:
            type: string
          description: Genome assembly identifier (e.g., hg38, mm39).
        - name: track
          in: query
          required: true
          schema:
            type: string
          description: Track name to retrieve data from.
        - name: chrom
          in: query
          required: false
          schema:
            type: string
          description: Chromosome to retrieve data for.
        - name: start
          in: query
          required: false
          schema:
            type: integer
          description: Start position (0-based).
        - name: end
          in: query
          required: false
          schema:
            type: integer
          description: End position (1-based).
        - name: hubUrl
          in: query
          required: false
          schema:
            type: string
            format: uri
          description: URL of the hub.txt file for a track hub.
        - name: maxItemsOutput
          in: query
          required: false
          schema:
            type: integer
            default: 1000000
          description: Maximum number of annotation items to return.
        - name: jsonOutputArrays
          in: query
          required: false
          schema:
            type: integer
            enum: [0, 1]
          description: Set to 1 to format output as JSON arrays instead of objects.
      responses:
        '200':
          description: Track annotation data.
          content:
            application/json:
              schema:
                $ref: '#/components/schemas/TrackDataResult'

  /search:
    get:
      operationId: searchGenomeBrowser
      summary: Search Genome Browser
      description: Searches across track data, names, and descriptions in the Genome Browser.
      tags:
        - Search
      parameters:
        - name: search
          in: query
          required: true
          schema:
            type: string
          description: Search query string.
        - name: genome
          in: query
          required: true
          schema:
            type: string
          description: Genome assembly identifier to search within.
        - name: categories
          in: query
          required: false
          schema:
            type: string
          description: "Restrict search scope (e.g., helpDocs, publicHubs, trackDb)."
      responses:
        '200':
          description: Search results.
          content:
            application/json:
              schema:
                type: object

components:
  schemas:
    GenomeSearchResult:
      type: object
      properties:
        totalAssemblyCount:
          type: integer
          description: Total number of matching assemblies.
        assemblyList:
          type: array
          items:
            $ref: '#/components/schemas/GenomeAssembly'

    GenomeListResult:
      type: object
      properties:
        ucscGenomes:
          type: object
          additionalProperties:
            $ref: '#/components/schemas/GenomeAssembly'

    GenomeAssembly:
      type: object
      properties:
        name:
          type: string
          description: Genome assembly identifier.
        organism:
          type: string
          description: Organism name.
        scientificName:
          type: string
          description: Scientific name of the organism.
        description:
          type: string
          description: Assembly description.
        sourceName:
          type: string
          description: Source name for the assembly.
        taxId:
          type: integer
          description: NCBI taxonomy identifier.
        orderKey:
          type: integer
        htmlPath:
          type: string

    HubListResult:
      type: object
      properties:
        publicHubs:
          type: array
          items:
            type: object
            properties:
              hubUrl:
                type: string
                format: uri
              shortLabel:
                type: string
              longLabel:
                type: string
              registrationTime:
                type: string
              dbCount:
                type: integer

    TrackListResult:
      type: object
      properties:
        genome:
          type: string
          description: Genome assembly identifier.
        tracks:
          type: object
          additionalProperties:
            $ref: '#/components/schemas/Track'

    Track:
      type: object
      properties:
        track:
          type: string
          description: Track name identifier.
        shortLabel:
          type: string
          description: Short display label.
        longLabel:
          type: string
          description: Long description label.
        type:
          type: string
          description: Track data type (e.g., bed, bigBed, bigWig).
        visibility:
          type: string
          description: Default visibility setting.
        html:
          type: string
          description: Track description HTML path.

    ChromosomeListResult:
      type: object
      properties:
        genome:
          type: string
        chromosomes:
          type: object
          additionalProperties:
            type: integer
            description: Chromosome size in base pairs.

    SequenceResult:
      type: object
      properties:
        genome:
          type: string
          description: Genome assembly identifier.
        chrom:
          type: string
          description: Chromosome identifier.
        start:
          type: integer
          description: Start position (0-based).
        end:
          type: integer
          description: End position (1-based).
        dna:
          type: string
          description: DNA sequence string.

    TrackDataResult:
      type: object
      properties:
        genome:
          type: string
          description: Genome assembly identifier.
        track:
          type: string
          description: Track name.
        chrom:
          type: string
          description: Chromosome identifier.
        start:
          type: integer
          description: Start position.
        end:
          type: integer
          description: End position.
        itemsReturned:
          type: integer
          description: Number of annotation items returned.
x-bioseq-client-name: UCSC
x-bioseq-integration:
  - name: mitochondrial-sequence
    operationId: getDnaSequence
    mode: single
    parameters:
      - {name: genome, in: query, value: hg38}
      - {name: chrom, in: query, value: chrM}
      - {name: start, in: query, value: 0}
      - {name: end, in: query, value: 10}
    expect:
      status: 200
      bodyContains:
        - '"dna"'
        - GATCACAGGT
)bioseq_openapi";

/** @brief Generate the UCSC client from, spec. */
[[bioseq::api_client(spec)]] export struct bioseq_external_api_client;
