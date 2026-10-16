/// @file
/// @brief Declares SQL tables whose rows come from external providers.
module;

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module bioseq.postgres.external.sql_catalog;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.storage.metadata.sql_rows;

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG

export namespace bioseq::postgres::external::sql_rows {

[[bioseq_sql::schema]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
    .scope = bioseq::sql::acl_scope::all_tables_in_schema,
})]]
/** @brief Schema for tables backed by external providers.
 *
 * Available only when built with BIOSEQ_BUILD_EXTERNAL_API_CLIENTS=ON.
 * Schema and table access are revoked from PUBLIC by default. The owner can
 * grant schema USAGE and table SELECT to a reader; the tables remain read-only.
 * ```sql
 * -- List the external relations visible to the current database role.
 * SELECT table_name
 * FROM information_schema.tables
 * WHERE table_schema = 'bioseq_external'
 * ORDER BY table_name;
 * ```
 */
inline constexpr bioseq::sql::schema_entity bioseq_external{};

/** @brief Taxonomy records, including parent taxa and lineage.
 *
 * Use a taxon_id filter to look up an NCBI taxonomy record.
 * Optional metadata is NULL when the provider does not supply it.
 * ```sql
 * -- Look up a butterfly taxon without retrieving its sequence.
 * SELECT taxon_id, scientific_name, rank
 * FROM bioseq_external.taxonomy
 * WHERE provider = 'ncbi'
 *   AND taxon_id = 1608923
 * LIMIT 5;
 * ```
 */
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
    .relation_options = bioseq::sql::checked_sql{"entity = 'taxonomy'"},
}), bioseq_sql::depends(
    bioseq::sql::type_of<bioseq::storage::metadata::catalog_rows::external_relation_options>)]]
taxonomy final {
    /// @brief Provider identifier used in SQL filters, such as ncbi, ucsc, ebi, or uniprot.
    std::string provider;
    /// @brief Normalized NCBI taxonomy identifier when supplied by the provider.
    std::optional<std::int64_t> taxon_id;
    /// @brief Taxon identifier in the provider's own namespace.
    std::string provider_taxon_id;
    /// @brief Scientific name reported for the organism or taxon.
    std::optional<std::string> scientific_name;
    /// @brief Common name supplied for the taxon, if available.
    std::optional<std::string> common_name;
    /// @brief Taxonomic rank reported by the provider, such as species or subspecies.
    std::optional<std::string> rank;
    /// @brief Normalized NCBI identifier of the parent taxon, when known.
    std::optional<std::int64_t> parent_taxon_id;
    /// @brief Array of ancestor taxonomy identifiers supplied by the provider.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"bigint[]">,
    })]]
    std::optional<std::vector<std::int64_t>> lineage;
    /// @brief Alternative names reported for the taxon.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"text[]">,
    })]]
    std::optional<std::vector<std::string>> synonyms;
    /// @brief Additional provider metadata as a JSON object or array, or NULL when unavailable.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> provider_attributes;
    /// @brief Logical provider data version when supplied; this is separate from the accession version.
    std::optional<std::string> provider_version;
    /// @brief JSON object identifying the provider, currently with a provider key.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    /// @brief Timestamp with time zone recording when the provider request completed.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

/** @brief Provider records with identifiers and biological metadata.
 *
 * For NCBI, this table returns genome assembly reports from Datasets.
 * An equality filter on scientific_name or taxon_id selects the organism.
 * It is an assembly catalog, not the Entrez Nucleotide text-search index.
 * Optional metadata is NULL when the provider adapter does not populate it.
 * ```sql
 * -- List assembly metadata for one butterfly subspecies.
 * SELECT accession, scientific_name, assembly_level, assembly_status
 * FROM bioseq_external.catalog
 * WHERE provider = 'ncbi'
 *   AND scientific_name = 'Heliconius erato demophoon'
 * LIMIT 5;
 * ```
 */
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
    .relation_options = bioseq::sql::checked_sql{"entity = 'catalog'"},
}), bioseq_sql::depends(
    bioseq::sql::type_of<bioseq::storage::metadata::catalog_rows::external_relation_options>)]]
catalog final {
    /// @brief Provider identifier used in SQL filters, such as ncbi, ucsc, ebi, or uniprot.
    std::string provider;
    /// @brief Provider record collection, such as assembly, gene, or sequence; its spelling is provider-specific.
    std::string domain;
    /// @brief Record category, such as assembly, gene, nucleotide, or protein; NCBI catalog rows are assemblies.
    std::string entity_kind;
    /// @brief Molecule category: nucleotide, protein, or unknown; NULL is used when catalog metadata omits it.
    std::optional<std::string> molecule_kind;
    /// @brief Optional namespace that qualifies the record identifier within the provider.
    std::optional<std::string> namespace_name;
    /// @brief Original identifier retained by the provider adapter before any accession normalization.
    std::string original_identifier;
    /// @brief Record accession used by the provider; it may include a version suffix.
    std::string accession;
    /// @brief Separate accession version when supplied by the adapter; NULL does not imply an unversioned accession string.
    std::optional<std::int32_t> accession_version;
    /// @brief Alternative record identities represented as a JSON array, when supplied.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> aliases;
    /// @brief Normalized NCBI taxonomy identifier when supplied by the provider.
    std::optional<std::int64_t> taxon_id;
    /// @brief Scientific name reported for the organism or taxon.
    std::optional<std::string> scientific_name;
    /// @brief Provider record title; the NCBI assembly adapter uses the assembly name when available.
    std::optional<std::string> title;
    /// @brief JSON array of gene identifiers and annotations associated with this record, when supplied.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> genes;
    /// @brief Provider keywords associated with the record, when supplied.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"text[]">,
    })]]
    std::optional<std::vector<std::string>> keywords;
    /// @brief JSON array of genomic intervals associated with the record, using zero-based, half-open coordinates.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> locations;
    /// @brief NCBI taxonomy identifier of the host organism, when relevant and supplied.
    std::optional<std::int64_t> host_taxon_id;
    /// @brief Geographical origin reported by the provider, when supplied.
    std::optional<std::string> geography;
    /// @brief Provider lineage label for the record, when supplied; not the taxonomy ancestor array.
    std::optional<std::string> lineage_name;
    /// @brief Sequence length in symbols when supplied; NULL does not mean an empty sequence.
    std::optional<std::int64_t> sequence_length;
    /// @brief Assembly accession associated with the record or interval, when supplied.
    std::optional<std::string> assembly_accession;
    /// @brief Provider assembly database identifier or name, when supplied; its meaning depends on the provider.
    std::optional<std::string> assembly_database;
    /// @brief Assembly source database reported by the provider, such as RefSeq or GenBank.
    std::optional<std::string> assembly_source;
    /// @brief Assembly completeness level reported by the provider, such as chromosome, scaffold, or contig.
    std::optional<std::string> assembly_level;
    /// @brief Provider status of the assembly record, when supplied.
    std::optional<std::string> assembly_status;
    /// @brief Provider release date for the record, when supplied.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::date,
    })]]
    std::optional<std::int32_t> release_date;
    /// @brief Provider date of the latest record update, when supplied.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::date,
    })]]
    std::optional<std::int32_t> update_date;
    /// @brief Whether the provider marks this as a reference record; NULL means the adapter did not supply a value.
    std::optional<bool> is_reference;
    /// @brief Whether the provider marks the record as reviewed; NULL means the adapter did not supply a value.
    std::optional<bool> is_reviewed;
    /// @brief Whether the provider marks the record as annotated; NULL means the adapter did not supply a value.
    std::optional<bool> is_annotated;
    /// @brief Whether the provider marks the record as a fragment; NULL means the adapter did not supply a value.
    std::optional<bool> is_fragment;
    /// @brief Whether the adapter marks the record as retrievable; this is not a guarantee of access through the sequences table.
    bool retrievable;
    /// @brief Additional provider metadata as a JSON object or array, or NULL when unavailable.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> provider_attributes;
    /// @brief Logical provider data version when supplied; this is separate from the accession version.
    std::optional<std::string> provider_version;
    /// @brief JSON object identifying the provider, currently with a provider key.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    /// @brief Timestamp with time zone recording when the provider request completed.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

/** @brief One gene associated with a provider record.
 *
 * Rows expand a provider record's gene list. A record without gene
 * metadata produces no rows; this table is not a GFF annotation-file reader.
 * The current NCBI assembly adapter does not populate that gene list.
 * ```sql
 * -- Inspect the gene-column layout without making a provider request.
 * SELECT accession, gene_index, gene_count, gene_id, gene_symbol
 * FROM bioseq_external.record_genes
 * WHERE false;
 * ```
 */
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
    .relation_options = bioseq::sql::checked_sql{"entity = 'record_gene'"},
}), bioseq_sql::depends(
    bioseq::sql::type_of<bioseq::storage::metadata::catalog_rows::external_relation_options>)]]
record_genes final {
    /// @brief Provider identifier used in SQL filters, such as ncbi, ucsc, ebi, or uniprot.
    std::string provider;
    /// @brief Provider record collection, such as assembly, gene, or sequence; its spelling is provider-specific.
    std::string domain;
    /// @brief Optional namespace that qualifies the record identifier within the provider.
    std::optional<std::string> namespace_name;
    /// @brief Original identifier retained by the provider adapter before any accession normalization.
    std::string original_identifier;
    /// @brief Record accession used by the provider; it may include a version suffix.
    std::string accession;
    /// @brief Separate accession version when supplied by the adapter; NULL does not imply an unversioned accession string.
    std::optional<std::int32_t> accession_version;
    /// @brief Zero-based position of this gene in the provider record's gene list.
    std::int32_t gene_index;
    /// @brief Number of genes in the provider record's gene list.
    std::int32_t gene_count;
    /// @brief Namespace of gene_id, such as an NCBI Gene identifier namespace or a provider namespace.
    std::string gene_namespace;
    /// @brief Gene identifier within gene_namespace.
    std::string gene_id;
    /// @brief Short gene symbol supplied by the provider.
    std::optional<std::string> gene_symbol;
    /// @brief Descriptive gene name supplied by the provider.
    std::optional<std::string> gene_name;
    /// @brief Provider classification of the gene, when supplied.
    std::optional<std::string> gene_type;
    /// @brief Provider locus tag for the gene, when supplied.
    std::optional<std::string> locus_tag;
    /// @brief Logical provider data version when supplied; this is separate from the accession version.
    std::optional<std::string> provider_version;
    /// @brief JSON object identifying the provider, currently with a provider key.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    /// @brief Timestamp with time zone recording when the provider request completed.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

/** @brief One genomic location associated with a provider record.
 *
 * Rows expand a provider record's genomic locations. Coordinates
 * are zero-based and half-open. The NCBI adapter uses an accession equality
 * filter as a GeneID and requests that gene's assembly annotations.
 * ```sql
 * -- Inspect the interval-column layout without making a provider request.
 * SELECT accession, sequence_alias, start0, end0, strand
 * FROM bioseq_external.record_locations
 * WHERE false;
 * ```
 */
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
    .relation_options = bioseq::sql::checked_sql{"entity = 'record_location'"},
}), bioseq_sql::depends(
    bioseq::sql::type_of<bioseq::storage::metadata::catalog_rows::external_relation_options>)]]
record_locations final {
    /// @brief Provider identifier used in SQL filters, such as ncbi, ucsc, ebi, or uniprot.
    std::string provider;
    /// @brief Provider record collection, such as assembly, gene, or sequence; its spelling is provider-specific.
    std::string domain;
    /// @brief Optional namespace that qualifies the record identifier within the provider.
    std::optional<std::string> namespace_name;
    /// @brief Original identifier retained by the provider adapter before any accession normalization.
    std::string original_identifier;
    /// @brief Provider record accession; the NCBI location adapter expects a GeneID in an accession equality filter.
    std::string accession;
    /// @brief Separate accession version when supplied by the adapter; NULL does not imply an unversioned accession string.
    std::optional<std::int32_t> accession_version;
    /// @brief Zero-based position of this interval in the provider record's location list.
    std::int32_t location_index;
    /// @brief Number of intervals in the provider record's location list.
    std::int32_t location_count;
    /// @brief Assembly accession associated with the record or interval, when supplied.
    std::optional<std::string> assembly_accession;
    /// @brief Provider assembly database identifier or name, when supplied; its meaning depends on the provider.
    std::optional<std::string> assembly_database;
    /// @brief Sequence accession or name on which start0 and end0 are measured.
    std::string sequence_alias;
    /// @brief Zero-based start of the genomic interval, included in the interval.
    std::int64_t start0;
    /// @brief Zero-based end of the genomic interval, excluded from the interval.
    std::int64_t end0;
    /// @brief Genomic orientation: 1 for forward, -1 for reverse, and 0 for unknown.
    std::int16_t strand;
    /// @brief Logical provider data version when supplied; this is separate from the accession version.
    std::optional<std::string> provider_version;
    /// @brief JSON object identifying the provider, currently with a provider key.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    /// @brief Timestamp with time zone recording when the provider request completed.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

/** @brief A complete sequence or an explicitly requested genomic interval.
 *
 * NCBI requests require an accession equality filter. Supplying both
 * start0 and end0 equality filters downloads only that zero-based, half-open
 * interval: start0 is included and end0 is excluded. The returned sequence
 * has end0 - start0 bases, while its own slice coordinates begin at zero.
 * Without interval filters, the whole sequence is returned and the NCBI
 * adapter leaves start0 and end0 NULL. Other optional metadata may be NULL.
 * ```sql
 * -- Retrieve only 100 bases from the butterfly chromosome used in the tutorial.
 * SELECT accession, start0, end0, sequence::text AS bases
 * FROM bioseq_external.sequences
 * WHERE provider = 'ncbi'
 *   AND accession = 'NC_140406.1'
 *   AND start0 = 600000
 *   AND end0 = 600100;
 * ```
 */
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
    .relation_options = bioseq::sql::checked_sql{"entity = 'sequence'"},
}), bioseq_sql::depends(
    bioseq::sql::type_of<bioseq::storage::metadata::catalog_rows::external_relation_options>)]]
sequences final {
    /// @brief Provider identifier used in SQL filters, such as ncbi, ucsc, ebi, or uniprot.
    std::string provider;
    /// @brief Provider record collection, such as assembly, gene, or sequence; its spelling is provider-specific.
    std::string domain;
    /// @brief Optional namespace that qualifies the record identifier within the provider.
    std::optional<std::string> namespace_name;
    /// @brief Original identifier retained by the provider adapter before any accession normalization.
    std::string original_identifier;
    /// @brief Record accession used by the provider; it may include a version suffix.
    std::string accession;
    /// @brief Separate accession version when supplied by the adapter; NULL does not imply an unversioned accession string.
    std::optional<std::int32_t> accession_version;
    /// @brief Molecule category of the returned sequence: nucleotide, protein, or unknown.
    std::string molecule_kind;
    /// @brief Reserved taxonomy identifier; the current sequence-row writer returns NULL.
    std::optional<std::int64_t> taxon_id;
    /// @brief Assembly accession associated with the record or interval, when supplied.
    std::optional<std::string> assembly_accession;
    /// @brief Provider assembly database identifier or name, when supplied; its meaning depends on the provider.
    std::optional<std::string> assembly_database;
    /// @brief Provider chromosome or sequence name when supplied; the NCBI FASTA adapter currently leaves it NULL.
    std::optional<std::string> chromosome;
    /// @brief Included start of the requested interval on the original sequence; zero-based and NULL for an NCBI whole-record request.
    std::optional<std::int64_t> start0;
    /// @brief Excluded end of the requested interval on the original sequence; NULL for an NCBI whole-record request.
    std::optional<std::int64_t> end0;
    /// @brief Genomic orientation: 1 for forward, -1 for reverse, and 0 for unknown.
    std::optional<std::int16_t> strand;
    /// @brief Returned bases as bioseq.seq; positions within this value start at zero even when start0 is nonzero.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
    })]]
    std::string sequence;
    /// @brief Logical provider data version when supplied; this is separate from the accession version.
    std::optional<std::string> provider_version;
    /// @brief JSON object identifying the provider, currently with a provider key.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    /// @brief Timestamp with time zone recording when the provider request completed.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

/** @brief A sequence chunk with its position in the provider record.
 *
 * NCBI accepts the same accession and interval filters as sequences.
 * start0 and end0 describe the whole requested genomic interval, while
 * base_offset starts at zero within that interval. The current NCBI adapter
 * collects the response before emitting chunks; chunk rows do not imply
 * bounded-memory streaming of the provider response.
 * ```sql
 * -- Read chunk positions for a small requested interval, without printing bases.
 * SELECT accession, chunk_index, base_offset, end_of_record
 * FROM bioseq_external.sequence_chunks
 * WHERE provider = 'ncbi'
 *   AND accession = 'NC_140406.1'
 *   AND start0 = 600000
 *   AND end0 = 600100
 * ORDER BY chunk_index;
 * ```
 */
struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
    .relation_options = bioseq::sql::checked_sql{"entity = 'sequence_chunk'"},
}), bioseq_sql::depends(
    bioseq::sql::type_of<bioseq::storage::metadata::catalog_rows::external_relation_options>)]]
sequence_chunks final {
    /// @brief Provider identifier used in SQL filters, such as ncbi, ucsc, ebi, or uniprot.
    std::string provider;
    /// @brief Provider record collection, such as assembly, gene, or sequence; its spelling is provider-specific.
    std::string domain;
    /// @brief Optional namespace that qualifies the record identifier within the provider.
    std::optional<std::string> namespace_name;
    /// @brief Original identifier retained by the provider adapter before any accession normalization.
    std::string original_identifier;
    /// @brief Record accession used by the provider; it may include a version suffix.
    std::string accession;
    /// @brief Separate accession version when supplied by the adapter; NULL does not imply an unversioned accession string.
    std::optional<std::int32_t> accession_version;
    /// @brief Molecule category of the returned sequence: nucleotide, protein, or unknown.
    std::string molecule_kind;
    /// @brief Reserved taxonomy identifier; the current sequence-row writer returns NULL.
    std::optional<std::int64_t> taxon_id;
    /// @brief Assembly accession associated with the record or interval, when supplied.
    std::optional<std::string> assembly_accession;
    /// @brief Provider assembly database identifier or name, when supplied; its meaning depends on the provider.
    std::optional<std::string> assembly_database;
    /// @brief Provider chromosome or sequence name when supplied; the NCBI FASTA adapter currently leaves it NULL.
    std::optional<std::string> chromosome;
    /// @brief Included start of the entire requested interval on the original sequence; shared by its chunks and NULL for an NCBI whole-record request.
    std::optional<std::int64_t> start0;
    /// @brief Excluded end of the entire requested interval on the original sequence; shared by its chunks and NULL for an NCBI whole-record request.
    std::optional<std::int64_t> end0;
    /// @brief Genomic orientation: 1 for forward, -1 for reverse, and 0 for unknown.
    std::optional<std::int16_t> strand;
    /// @brief Zero-based ordinal of this chunk within the retrieved sequence or requested interval.
    std::int64_t chunk_index;
    /// @brief Zero-based position of this chunk within the retrieved sequence or requested interval, not an absolute genomic coordinate.
    std::int64_t base_offset;
    /// @brief Bases in this chunk as bioseq.seq; use base_offset to locate them within the requested interval.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
    })]]
    std::string sequence_chunk;
    /// @brief True for the last emitted chunk of the retrieved sequence or requested interval.
    bool end_of_record;
    /// @brief Logical provider data version when supplied; this is separate from the accession version.
    std::optional<std::string> provider_version;
    /// @brief JSON object identifying the provider, currently with a provider key.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    /// @brief Timestamp with time zone recording when the provider request completed.
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

} // namespace bioseq::postgres::external::sql_rows

#endif
