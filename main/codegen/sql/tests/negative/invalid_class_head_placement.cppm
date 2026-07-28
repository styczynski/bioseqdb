// expected-failure: BIOSEQ_SQL_E_CLASS_HEAD_PLACEMENT

export module bioseq.codegen.sql.invalid_class_head_placement;

import bioseq.codegen.sql.annotation_model;

[[bioseq_sql::type({})]]
export struct invalid_letter {};
