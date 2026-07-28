// expected-failure: BIOSEQ_SQL_E_CATALOG_HANDLE_PROVENANCE

module;

extern "C" {
#include "catalog/pg_class_d.h"
}

export module bioseq.authored.catalog_handle_specialization;

import bioseq.pg_utils.sql_catalog_entities;
import bioseq.codegen.sql.annotation_model;

inline constexpr bioseq::pg_utils::relation_entity_handle<
    RELKIND_SEQUENCE,
    bioseq::pg_utils::fixed_catalog_name{"authored_seq"},
    bioseq::pg_utils::sql_schema_binding::fixed,
    bioseq::pg_utils::fixed_catalog_name{"bioseq_internal"}>
    authored_seq{};
