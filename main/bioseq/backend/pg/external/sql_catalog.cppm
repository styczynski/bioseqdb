module;

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module bioseq.pg.external.sql_catalog;

import bioseq.codegen.sql.annotation_model;

#if defined(BIOSEQ_EXTERNAL_PG) && BIOSEQ_EXTERNAL_PG

export namespace bioseq::external::sql_rows {

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
inline constexpr bioseq::sql::schema_entity bioseq_external{};

struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
})]] taxonomy final {
    std::string provider;
    std::optional<std::int64_t> taxon_id;
    std::string provider_taxon_id;
    std::optional<std::string> scientific_name;
    std::optional<std::string> common_name;
    std::optional<std::string> rank;
    std::optional<std::int64_t> parent_taxon_id;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"bigint[]">,
    })]]
    std::optional<std::vector<std::int64_t>> lineage;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"text[]">,
    })]]
    std::optional<std::vector<std::string>> synonyms;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> provider_attributes;
    std::optional<std::string> provider_version;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
})]] catalog final {
    std::string provider;
    std::string domain;
    std::string entity_kind;
    std::optional<std::string> molecule_kind;
    std::optional<std::string> namespace_name;
    std::string original_identifier;
    std::string accession;
    std::optional<std::int32_t> accession_version;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> aliases;
    std::optional<std::int64_t> taxon_id;
    std::optional<std::string> scientific_name;
    std::optional<std::string> title;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> genes;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"text[]">,
    })]]
    std::optional<std::vector<std::string>> keywords;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> locations;
    std::optional<std::int64_t> host_taxon_id;
    std::optional<std::string> geography;
    std::optional<std::string> lineage_name;
    std::optional<std::int64_t> sequence_length;
    std::optional<std::string> assembly_accession;
    std::optional<std::string> assembly_database;
    std::optional<std::string> assembly_source;
    std::optional<std::string> assembly_level;
    std::optional<std::string> assembly_status;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::date,
    })]]
    std::optional<std::int32_t> release_date;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::date,
    })]]
    std::optional<std::int32_t> update_date;
    std::optional<bool> is_reference;
    std::optional<bool> is_reviewed;
    std::optional<bool> is_annotated;
    std::optional<bool> is_fragment;
    bool retrievable;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::optional<std::string> provider_attributes;
    std::optional<std::string> provider_version;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
})]] record_genes final {
    std::string provider;
    std::string domain;
    std::optional<std::string> namespace_name;
    std::string original_identifier;
    std::string accession;
    std::optional<std::int32_t> accession_version;
    std::int32_t gene_index;
    std::int32_t gene_count;
    std::string gene_namespace;
    std::string gene_id;
    std::optional<std::string> gene_symbol;
    std::optional<std::string> gene_name;
    std::optional<std::string> gene_type;
    std::optional<std::string> locus_tag;
    std::optional<std::string> provider_version;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
})]] record_locations final {
    std::string provider;
    std::string domain;
    std::optional<std::string> namespace_name;
    std::string original_identifier;
    std::string accession;
    std::optional<std::int32_t> accession_version;
    std::int32_t location_index;
    std::int32_t location_count;
    std::optional<std::string> assembly_accession;
    std::optional<std::string> assembly_database;
    std::string sequence_alias;
    std::int64_t start0;
    std::int64_t end0;
    std::int16_t strand;
    std::optional<std::string> provider_version;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
})]] sequences final {
    std::string provider;
    std::string domain;
    std::optional<std::string> namespace_name;
    std::string original_identifier;
    std::string accession;
    std::optional<std::int32_t> accession_version;
    std::string molecule_kind;
    std::optional<std::int64_t> taxon_id;
    std::optional<std::string> assembly_accession;
    std::optional<std::string> assembly_database;
    std::optional<std::string> chromosome;
    std::optional<std::int64_t> start0;
    std::optional<std::int64_t> end0;
    std::optional<std::int16_t> strand;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
    })]]
    std::string sequence;
    std::optional<std::string> provider_version;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

struct [[bioseq_sql::table({
    .schema = bioseq::sql::fixed_schema<"bioseq_external">,
    .access_method = "bioseq_external",
})]] sequence_chunks final {
    std::string provider;
    std::string domain;
    std::optional<std::string> namespace_name;
    std::string original_identifier;
    std::string accession;
    std::optional<std::int32_t> accession_version;
    std::string molecule_kind;
    std::optional<std::int64_t> taxon_id;
    std::optional<std::string> assembly_accession;
    std::optional<std::string> assembly_database;
    std::optional<std::string> chromosome;
    std::optional<std::int64_t> start0;
    std::optional<std::int64_t> end0;
    std::optional<std::int16_t> strand;
    std::int64_t chunk_index;
    std::int64_t base_offset;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
    })]]
    std::string sequence_chunk;
    bool end_of_record;
    std::optional<std::string> provider_version;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::jsonb,
    })]]
    std::string provenance;
    [[bioseq_sql::column({
        .sql_type = bioseq::sql::pg::timestamptz,
    })]]
    std::int64_t retrieved_at;
};

}  // namespace bioseq::external::sql_rows

#endif
