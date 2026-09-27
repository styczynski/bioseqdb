// expected-failure: BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE

module;

extern "C" {
#include "catalog/pg_class_d.h"
}

export module bioseq.authored.catalog_handle_namespace_alias;

import bioseq.postgres.support.sql_catalog_entities;
import bioseq.codegen.sql.annotation_model;

namespace pg_catalog_api = bioseq::postgres::support;

inline constexpr pg_catalog_api::relation_entity_handle<
    RELKIND_SEQUENCE,
    pg_catalog_api::fixed_catalog_name{"aliased_seq"},
    pg_catalog_api::sql_schema_binding::fixed,
    pg_catalog_api::fixed_catalog_name{"bioseq_internal"}>
    aliased_seq{};
