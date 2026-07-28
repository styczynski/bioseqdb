/* Operator classes for bioseq.letter and bioseq.seq. */
export module bioseq.types.pg_compact_sequence:sql_catalog;

import bioseq.codegen.sql.annotation_model;
import :content;
import :letter;

namespace bioseq::pg_types::sql_catalog {

[[bioseq_sql::opclass({
    .access_method = bioseq::sql::pg_catalog_reference::btree,
    .is_default = true,
    .operators = bioseq::sql::opclass_operators({
        bioseq::sql::opclass_operator(
            1, bioseq::pg_types::letter::letter_lt),
        bioseq::sql::opclass_operator(
            2, bioseq::pg_types::letter::letter_le),
        bioseq::sql::opclass_operator(
            3, bioseq::pg_types::letter::letter_eq),
        bioseq::sql::opclass_operator(
            4, bioseq::pg_types::letter::letter_ge),
        bioseq::sql::opclass_operator(
            5, bioseq::pg_types::letter::letter_gt),
    }),
    .functions = bioseq::sql::opclass_functions({
        bioseq::sql::opclass_function(
            1, bioseq::pg_types::letter::letter_cmp),
    }),
})]]
inline constexpr bioseq::sql::opclass_tag letter_btree_ops{};

[[bioseq_sql::opclass({
    .access_method = bioseq::sql::pg_catalog_reference::hash,
    .is_default = true,
    .operators = bioseq::sql::opclass_operators({
        bioseq::sql::opclass_operator(
            1, bioseq::pg_types::letter::letter_eq),
    }),
    .functions = bioseq::sql::opclass_functions({
        bioseq::sql::opclass_function(
            1, bioseq::pg_types::letter::letter_hash),
        bioseq::sql::opclass_function(
            2, bioseq::pg_types::letter::letter_hash_extended),
    }),
})]]
inline constexpr bioseq::sql::opclass_tag letter_hash_ops{};

[[bioseq_sql::opclass({
    .access_method = bioseq::sql::pg_catalog_reference::btree,
    .is_default = true,
    .operators = bioseq::sql::opclass_operators({
        bioseq::sql::opclass_operator(
            1, bioseq::pg_types::sql_routines::seq_lt),
        bioseq::sql::opclass_operator(
            2, bioseq::pg_types::sql_routines::seq_le),
        bioseq::sql::opclass_operator(
            3, bioseq::pg_types::sql_routines::seq_eq),
        bioseq::sql::opclass_operator(
            4, bioseq::pg_types::sql_routines::seq_ge),
        bioseq::sql::opclass_operator(
            5, bioseq::pg_types::sql_routines::seq_gt),
    }),
    .functions = bioseq::sql::opclass_functions({
        bioseq::sql::opclass_function(
            1, bioseq::pg_types::sql_routines::seq_cmp),
    }),
})]]
inline constexpr bioseq::sql::opclass_tag seq_ops{};

[[bioseq_sql::opclass({
    .access_method = bioseq::sql::pg_catalog_reference::hash,
    .is_default = true,
    .operators = bioseq::sql::opclass_operators({
        bioseq::sql::opclass_operator(
            1, bioseq::pg_types::sql_routines::seq_eq),
    }),
    .functions = bioseq::sql::opclass_functions({
        bioseq::sql::opclass_function(
            1, bioseq::pg_types::sql_routines::seq_hash),
        bioseq::sql::opclass_function(
            2, bioseq::pg_types::sql_routines::seq_hash_extended),
    }),
})]]
inline constexpr bioseq::sql::opclass_tag seq_hash_ops{};

}  // namespace bioseq::pg_types::sql_catalog
