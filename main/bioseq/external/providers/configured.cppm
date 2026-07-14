// DOC0372 — the configured provider pack. This is the single place that names
// the concrete adapters compiled into the extension; the PG bridge and the
// live tests depend only on this alias, never on individual adapters. Adding a
// provider is one import + one pack argument here (the compile-time uniqueness
// and concept checks live in external_api_provider).
export module bioseq.external.providers.configured;

import bioseq.external.provider.pack;
import bioseq.external.providers.ncbi;
import bioseq.external.providers.ucsc;
import bioseq.external.providers.uniprot;
import bioseq.external.providers.ebi;

export namespace bioseq::external::providers {

// Order is stable and intentional: NCBI (richest metadata) first, then UCSC,
// UniProt, EBI. Dispatch is by stable provider id, so order affects only the
// compile-time pack layout, not runtime routing.
using configured_external_api_provider =
    bioseq::external::provider::external_api_provider<ncbi_provider,
                                                      ucsc_provider,
                                                      uniprot_provider,
                                                      ebi_provider>;

}  // namespace bioseq::external::providers
