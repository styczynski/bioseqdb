/**
 * @file external/providers/configured.cppm
 * @brief Configured production provider pack.
 */
/// Concrete provider adapters in dispatch order.
export module bioseq.external.providers.configured;

import bioseq.external.provider.pack;
import bioseq.external.providers.ncbi;
import bioseq.external.providers.ucsc;
import bioseq.external.providers.uniprot;
import bioseq.external.providers.ebi;

export namespace bioseq::external::providers {

/// Configured provider pack in stable compile-time layout order.
using configured_external_api_provider = bioseq::external::provider::
    external_api_provider<ncbi_provider, ucsc_provider, uniprot_provider, ebi_provider>;

} // namespace bioseq::external::providers
