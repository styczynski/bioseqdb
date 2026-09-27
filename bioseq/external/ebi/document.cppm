/**
 * @file external/ebi/document.cppm
 * @brief Opening section of the EBI Search OpenAPI document
 */
module;

#include <string_view>

export module bioseq.external.ebi:document;

export namespace bioseq::external::ebi::descriptor {

/** API metadata and server URLs, followed by the opening paths mapping. */
inline constexpr std::string_view document_preamble = R"bioseq_openapi(
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
)bioseq_openapi";

} // namespace bioseq::external::ebi::descriptor
