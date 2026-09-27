/**
 * @file external/uniprot/document.cppm
 * @brief UniProt OpenAPI metadata fragments.
 */
module;

#include <string_view>

export module bioseq.external.uniprot:document;

export namespace bioseq::external::uniprot::descriptor {

/** @brief OpenAPI metadata, server definition, description, and paths mapping prefix. */
inline constexpr std::string_view document_preamble = R"bioseq_openapi(
---
openapi: 3.0.1
info:
  title: OpenAPI definition
  version: v0
servers:
- url: https://rest.uniprot.org/
  description: UniProt REST API Server
tags:
- name: UniProtKB
  description: The UniProt Knowledgebase (UniProtKB) acts as the global hub of accurate,
    consistent and expertly curated information on protein sequence and function.
    Each UniProtKB entry is described by a stable protein identifier (accession ID)
    and contains core data consisting of the amino acid sequence, protein name or
    description, taxonomic information and links to relevant scientific publications.
    Further annotation is added when available, such as protein function, subcellular
    location and the position of protein features such as active sites, domains and
    post-translational modifications. Where possible these annotations are described
    using established biological ontologies, classifications and cross-references.
    A clear indication of the quality of annotation in the form of evidence attribution
    of experimental and computational data is added to each piece of data.
paths:
)bioseq_openapi";

} // namespace bioseq::external::uniprot::descriptor
