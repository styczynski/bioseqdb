/**
 * @file sql_catalog.cppm
 * @brief Register default B-tree and hash support for letters and sequences.
 */
export module bioseq.postgres.types.compact_sequence:sql_catalog;

import bioseq.codegen.sql.annotation_model;
import :content;
import :letter;

namespace bioseq::pg_types::sql_catalog {

/// @brief Registers default B-tree ordering for alphabet letters.
///
/// ```sql
/// -- Build an explicit BTREE index and query values using this operator class.
/// BEGIN;
/// CREATE TEMP TABLE example_letter_btree_ops (value bioseq.letter('dna4'));
/// INSERT INTO example_letter_btree_ops VALUES ('G'), ('A'), ('C');
/// CREATE INDEX ON example_letter_btree_ops USING btree (value bioseq.letter_btree_ops);
/// SELECT value FROM example_letter_btree_ops ORDER BY value;
/// ROLLBACK;
/// ```
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

/// @brief Registers default hash indexing for alphabet letters.
///
/// ```sql
/// -- Build an explicit HASH index and query values using this operator class.
/// BEGIN;
/// CREATE TEMP TABLE example_letter_hash_ops (value bioseq.letter('dna4'));
/// INSERT INTO example_letter_hash_ops VALUES ('G'), ('A'), ('C');
/// CREATE INDEX ON example_letter_hash_ops USING hash (value bioseq.letter_hash_ops);
/// SELECT value FROM example_letter_hash_ops
/// WHERE value OPERATOR(bioseq.=) 'A'::bioseq.letter('dna4');
/// ROLLBACK;
/// ```
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

/// @brief Registers default B-tree ordering for compact sequences.
///
/// ```sql
/// -- Build an explicit BTREE index and query values using this operator class.
/// BEGIN;
/// CREATE TEMP TABLE example_seq_ops (value bioseq.seq('dna4', 64));
/// INSERT INTO example_seq_ops VALUES ('ACGT'), ('ACGA'), ('TTTT');
/// CREATE INDEX ON example_seq_ops USING btree (value bioseq.seq_ops);
/// SELECT value FROM example_seq_ops ORDER BY value;
/// ROLLBACK;
/// ```
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

/// @brief Registers default hash indexing for compact sequences.
///
/// ```sql
/// -- Build an explicit HASH index and query values using this operator class.
/// BEGIN;
/// CREATE TEMP TABLE example_seq_hash_ops (value bioseq.seq('dna4', 64));
/// INSERT INTO example_seq_hash_ops VALUES ('ACGT'), ('ACGA'), ('TTTT');
/// CREATE INDEX ON example_seq_hash_ops USING hash (value bioseq.seq_hash_ops);
/// SELECT value FROM example_seq_hash_ops
/// WHERE value OPERATOR(bioseq.=) 'ACGT'::bioseq.seq('dna4', 64);
/// ROLLBACK;
/// ```
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

/**
 * @brief Register ordinary MIN for every sequence alphabet using its semantic order.
 * No initial condition lets PostgreSQL initialize from the first non-NULL seq;
 * empty and all-NULL groups return NULL. The same transition combines states.
 */
[[bioseq_sql::aggregate({
    .transition = bioseq::pg_types::sql_routines::seq_smaller,
    .combine = bioseq::pg_types::sql_routines::seq_smaller,
    .sort_operator = bioseq::pg_types::sql_routines::seq_lt,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
inline constexpr bioseq::sql::aggregate_tag min{};

/**
 * @brief Register ordinary MAX without converting sequences to text.
 * CIGAR selects the greatest value by reported matches, matching intervals and
 * alphabet ranks; ordinary alphabets retain their existing sequence ordering.
 */
[[bioseq_sql::aggregate({
    .transition = bioseq::pg_types::sql_routines::seq_larger,
    .combine = bioseq::pg_types::sql_routines::seq_larger,
    .sort_operator = bioseq::pg_types::sql_routines::seq_gt,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
inline constexpr bioseq::sql::aggregate_tag max{};

}  // namespace bioseq::pg_types::sql_catalog
