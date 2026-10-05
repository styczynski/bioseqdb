/**
 * @file index.cppm
 * @brief C++ declarations used to generate Bioseq SQL functions.
 * @defgroup bioseq_sql_public_api Bioseq SQL public API
 *
 * Annotations describe SQL types and planner properties. Codegen creates
 * the C wrappers that PostgreSQL calls. Implementations live in the other
 * extension files. Alignment-result helpers are placeholders whose values
 * are supplied by the owning mmsearch custom scan.
 */
module;

extern "C" {
#include "postgres.h"
}

#undef Min
#undef Max
#undef snprintf
#undef vsnprintf

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

export module bioseq.postgres.extension;

import bioseq.codegen.sql.annotation_model;
import bioseq.postgres.extension.alignment.sql_rows;
import bioseq.postgres.extension.sql_rows;
import bioseq.pipeline.stats;
import bioseq.postgres.support.sql_boundary;
import bioseq.postgres.table.sql_rows;
import bioseq.postgres.table.alignment_execution;

export namespace bioseq::pg::sql_routines {

[[bioseq_sql::function({
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "mmsearch_support",
})]]
/**
 * @brief Request similarity search with default aligner options.
 * The planner must replace  this marker with a Bioseq custom scan.
 * Direct Boolean execution raises an error.
 * @ingroup bioseq_sql_public_api

 * @param el Target sequence column from the Bioseq table scanned by this mmsearch call.
 * @param query Query sequence to align against that target column.
 * @return Predicate consumed by the planner to produce matching alignment rows through a Bioseq custom scan.
 *
 * ```sql
 * -- As self-alignment is not realistically useful, we can compare various sequences from our dataset using self-join.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.score(t.seq) AS score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
bool mmsearch(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        Datum el,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        Datum query);

[[bioseq_sql::function({
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "mmsearch_support",
})]]
/**
 * @brief Request similarity search with per-call aligner options.
 * A null target or query returns SQL NULL. Other calls require a Bioseq
 * custom scan to execute the search.
 * @ingroup bioseq_sql_public_api

 * @param el Target sequence column from the Bioseq table scanned by this mmsearch call.
 * @param query Query sequence to align against that target column.
 * @param opts Per-call aligner options; NULL leaves the configured defaults in effect.
 * @return Predicate consumed by the planner to produce matching alignment rows through a Bioseq custom scan. A NULL target or query returns SQL NULL.
 *
 * ```sql
 * -- As self-alignment is not realistically useful, we can compare various sequences from our dataset using self-join.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.score(t.seq) AS score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq, aligner_options(kmer_size => 15, window_size => 10))
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::optional<bool> mmsearch(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        std::optional<Datum> el,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        std::optional<Datum> query,
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::type_of<
                bioseq::table::composite_rows::bioseq_aligner_options>,
        })]]
        std::optional<Datum> opts);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Base-alignment score of the current mmsearch result, equivalent to dp_score.
 * @param el The same target sequence expression passed as the first argument of the owning mmsearch call; it identifies the current alignment result.
 * @return Base-alignment score for the current result, equivalent to dp_score.
 *
 * ```sql
 * -- This example shows each possible value we can get from the aligner pipeline: score and dp_score are aliases for the same value which is computed minimap2 score.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.score(t.seq) AS score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int32_t score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Score from aligning the bases of the current mmsearch result.
 * ```sql
 * -- This example shows each possible value we can get from the aligner pipeline: score and dp_score are aliases for the same value which is computed minimap2 score.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.dp_score(t.seq) AS dp_score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int32_t dp_score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Query sequence length in bases for the current mmsearch result
 * ```sql
 * -- Read query_len for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.query_len(t.seq) AS query_len, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t query_len(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief Zero-based start  of the alignment on the query sequence.
 * @ingroup bioseq_sql_public_api

 * ```sql
 * -- Read query_start for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.query_start(t.seq) AS query_start, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t query_start(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief Exclusive end offset of the alignment on the query sequene.
 * @ingroup bioseq_sql_public_api

 * ```sql
 * -- Read query_end for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.query_end(t.seq) AS query_end, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t query_end(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Stored target length in bases for the current mmsearch result.
 * ```sql
 * -- Read target_len for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.target_len(t.seq) AS target_len, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t target_len(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief Zero-based alignment start on the original target sequence.
 * @ingroup bioseq_sql_public_api

 * ```sql
 * -- Read target_start for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.target_start(t.seq) AS target_start, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t target_start(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief Exclusive alignment end on the original target sequence.
 * @ingroup bioseq_sql_public_api

 * ```sql
 * -- Read target_end for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.target_end(t.seq) AS target_end, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t target_end(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Number of matching bases in the current mmsearch alignment.
 * ```sql
 * -- num_matches - number of residue matches in minimap2 so practically number of M in CIGAR string
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.num_matches(t.seq) AS num_matches, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t num_matches(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Length, of the reported alignment block, including gaps.
 * ```sql
 * -- block_length is similar to PAF in minimap2 - total alignment block length i.e length of the expanded CIGAR string.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.block_length(t.seq) AS block_length, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t block_length(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .cost = 100000.0,
    .has_cost = true,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Number of unit-cost edits in the reported alignment.
 * ```sql
 * -- edit counts alignment edits.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.edit(t.seq) AS edit, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int64_t edit(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Mapping quality of the current mmsearch alignment.
 * ```sql
 * -- Read mapq for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.mapq(t.seq) AS mapq, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int32_t mapq(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Seed-chain score of the current mmsearch alignment.
 * ```sql
 * -- chain_score describes the seed chain score.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.chain_score(t.seq) AS chain_score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int32_t chain_score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Reported maximum  alignment score. The CPU aligner currently uses the total base-alignment score.
 * ```sql
 * -- Read dp_max_score for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.dp_max_score(t.seq) AS dp_max_score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int32_t dp_max_score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Number of seeds in the reported alignment's chain.
 * ```sql
 * -- seed_count counts how many anchors were there before chaining started.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.seed_count(t.seq) AS seed_count, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int32_t seed_count(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Best secondary chain score competing with this primary alignment.
 * ```sql
 * -- Read secondary_chain_score for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.secondary_chain_score(t.seq) AS secondary_chain_score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::int32_t secondary_chain_score(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Whether the query aligns in reverse orientation relative to the target.
 * ```sql
 * -- Read is_reverse for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.is_reverse(t.seq) AS is_reverse, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
bool is_reverse(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Whether the current alignment is the primary reported match.
 * ```sql
 * -- Read is_primary for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.is_primary(t.seq) AS is_primary, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
bool is_primary(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Whether the current alignment has an available CIGAR string.
 * ```sql
 * -- Read has_cigar for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.has_cigar(t.seq) AS has_cigar, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
bool has_cigar(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/** @brief Estimated divergence of the current mmsearch alignment.
 * ```sql
 * -- Read divergence for each occurrence returned by the alignment join. The helper belongs to the target sequence expression used by mmsearch.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.divergence(t.seq) AS divergence, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
float divergence(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_placeholder_support",
})]]
/**
 * @brief CIGAR operations for the current mmsearch alignment.
 * The operations describe matches, mismatches and gaps when base alignment ran.
 * @ingroup bioseq_sql_public_api

 * @param el The same target sequence expression passed as the first argument of the owning mmsearch call; it identifies the current alignment result.
 * @return CIGAR operation string for the current alignment; selecting it requests base alignment.
 *
 * ```sql
 * -- Requesting cigar enables additional pipeline stage for base alignment, because the pipeline needs to know exactly which bases were substituted/inserted or deleted. However, score alone does not enable it and can return zero (less exact score is used based only on chaining).
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
std::string cigar(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::anyelement,
        })]]
        Datum el);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Prepare this backend's GPU shader cache and return a status report.
 * ```sql
 * -- Prepare this backend's GPU shader cache and return a status report.
 * SELECT * FROM bioseq.gpu_warmup_shader_cache();
 * ```
*/
std::string gpu_warmup_shader_cache();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
/** @brief Report Vulkan availability, device limits and initialization errors.
 * ```sql
 * -- Report Vulkan availability, device limits and initialization errors.
 * SELECT * FROM bioseq.vulkan_runtime_status();
 * ```
*/
bioseq::postgres::support::named_output<
    bioseq::postgres::sql_rows::vulkan_runtime_status_row>
vulkan_runtime_status();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
    .support = "bioseq_symbol_count_support",
})]]
/**
 * @brief Count occurrences of  one alhabet symbol in a sequence.
 *
 * The symbol must be one byte and belong to the sequence alphabet This
 * direct call handles in-memory values. Stored locators need a Bioseq scan,
 * which can read the sequence or use its stored symbol-count index.
 *
 * @ingroup bioseq_sql_public_api

 * ```sql
 * -- Count adenines in each stored sequence. PostgreSQL invokes the support callback while planning this projection.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT id, bioseq.symbol_count(seq, 'A') AS adenines FROM doc_sequences;
 * ROLLBACK;
 * ```
*/
std::int64_t symbol_count(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::custom_type<"bioseq.seq">,
        })]]
        Datum el,
        std::string sym);

[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]]
/** @brief Set symbol_count cost and keep the projection at its source relation.
 * ```sql
 * -- Count adenines in each stored sequence. PostgreSQL invokes the support callback while planning this projection.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * EXPLAIN (COSTS OFF)
 * SELECT id, bioseq.symbol_count(seq, 'A') AS adenines FROM doc_sequences;
 * ROLLBACK;
 * ```
*/
Datum bioseq_symbol_count_support(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::internal,
        })]]
        Datum request_datum);

[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]]
/** @brief Give direct mmsearch calls a high cost so the planner prefers Bioseq custom scans.
 * ```sql
 * -- PostgreSQL invokes this planner support callback when planning the alignment expression. EXPLAIN shows the Bioseq custom scan chosen for the join.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * EXPLAIN (COSTS OFF)
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.score(t.seq) AS score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
Datum mmsearch_support(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::internal,
        })]]
        Datum request_datum);

[[bioseq_sql::function({
    .volatility = bioseq::sql::volatility::immutable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::internal,
})]]
/** @brief Keep alignment-result helpers at their source scan through joins and aggregates.
 * ```sql
 * -- PostgreSQL invokes this planner support callback when planning the alignment expression. EXPLAIN shows the Bioseq custom scan chosen for the join.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * EXPLAIN (COSTS OFF)
 * SELECT t.id AS target_id, q.id AS query_id,
 *   bioseq.score(t.seq) AS score, bioseq.cigar(t.seq) AS cigar
 * FROM doc_sequences AS t JOIN doc_sequences AS q
 *   ON mmsearch(t.seq, q.seq)
 * WHERE t.id = 1 AND q.id = 2;
 * ROLLBACK;
 * ```
*/
Datum bioseq_placeholder_support(
        [[bioseq_sql::param({
            .sql_type = bioseq::sql::pg::internal,
        })]]
        Datum request_datum);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
/**
 * @brief List every supported index type for each sequence  column in a Bioseq table.
 * Include the update policy, total shard size and time since the last update.
 * Missing storage gives zero size. A missing update-log entry gives no age.
 * Internal and temporary tables are excluded.

 * ```sql
 * -- List every supported index type for each sequence  column in a Bioseq table. Include the update policy, total shard size and time since the last update. Missing storage gives zero size. A missing update-log entry gives no age. Internal and temporary tables are excluded.
 * SELECT * FROM bioseq.list_metadata();
 * ```

 * @remark website.field table_name: Bioseq table containing the sequence column, returned as regclass.
 * @remark website.field column_attno: One-based PostgreSQL attribute number of the source sequence column.
 * @remark website.field column_name: Name of the sequence column.
 * @remark website.field index_name: Registered name of the supported sequence index.
 * @remark website.field index_type: Sequence-index layout name.
 * @remark website.field update_policy: UPDATE_ON_INSERT for automatic maintenance or UPDATE_MANUALLY for explicit updates.
 * @remark website.field storage_size: Total size of the matching index storage relations, formatted by pg_size_pretty; zero when none exist.
 * @remark website.field time_since_update: Time elapsed since the recorded index update; NULL when no update-log entry exists.
*/
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::list_metadata_row>
list_metadata();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Return a row per pipeline stage for this backend's latest alignment query.
 * Include partial queries. Return, no rows befoe the first snapshot.

 * ```sql
 * -- Return a row per pipeline stage for this backend's latest alignment query. Include partial queries. Return, no rows befoe the first snapshot.
 * SELECT * FROM bioseq.last_alignment_execution_stats();
 * ```

 * @remark website.field execution_id: Monotonically increasing identifier of the saved alignment query in this PostgreSQL backend.
 * @remark website.field backend_pid: Operating-system process ID of the PostgreSQL backend that executed the query.
 * @remark website.field database_oid: PostgreSQL OID of the database in which the query ran.
 * @remark website.field effective_cpu_workers: CPU worker count recorded for the execution environment.
 * @remark website.field gpu_device_uuid: GPU device UUID encoded as hexadecimal; NULL when no GPU environment was recorded.
 * @remark website.field gpu_driver: Recorded GPU driver identity; NULL when no GPU environment was recorded.
 * @remark website.field gpu_pipeline_cache_policy: Pipeline-cache policy recorded for this GPU execution; NULL when no GPU environment was recorded.
 * @remark website.field gpu_pipeline_cache_seed_sha256: SHA-256 of the GPU pipeline-cache seed, encoded as hexadecimal; NULL without a GPU environment.
 * @remark website.field gpu_pipeline_cache_seed_size: Size of the GPU pipeline-cache seed in bytes; NULL without a GPU environment.
 * @remark website.field gpu_pipeline_cache_manifest_checksum: Pipeline-cache manifest checksum as 16 lowercase hexadecimal digits; NULL without a GPU environment.
 * @remark website.field gpu_pipeline_cache_hit: Whether the recorded GPU pipeline-cache lookup was a hit; NULL without a GPU environment.
 * @remark website.field scan_node_count: Number of Bioseq alignment scan nodes registered for the statement.
 * @remark website.field nested_alignment_executor_count: Number of nested alignment executors recorded under the statement collector.
 * @remark website.field is_completed: True when the saved query fully drained its alignment scans.
 * @remark website.field is_partial: True when alignment execution stopped before all registered scans were fully drained.
 * @remark website.field is_fully_drained: True when all recorded alignment scans were consumed completely.
 * @remark website.field requested_backend: Requested execution backend, such as scalar_cpu, simd_cpu or vulkan_gpu.
 * @remark website.field actual_backend: Backend recorded during execution; none denotes no observation and mixed_invalid denotes conflicting observations.
 * @remark website.field requested_target_index_source: Requested source of target minimizers: none, recomputed, persisted or mixed_invalid.
 * @remark website.field actual_target_index_source: Observed source of target minimizers: none, recomputed, persisted or mixed_invalid.
 * @remark website.field persisted_generation_id: Identifier of the persisted minimizer generation recorded by the query.
 * @remark website.field persisted_source_identity: Recorded persisted-source identity as 16 lowercase hexadecimal digits.
 * @remark website.field has_mixed_persisted_identity: True when the query combined incompatible persisted generation or source identities.
 * @remark website.field stage: Pipeline stage represented by this row, such as query_sketch, seed_lookup or chain_score.
 * @remark website.field eligible_jobs: Number of alignment jobs eligible to execute the stage represented by this row.
 * @remark website.field executed_host_io_jobs: Number of jobs that executed this stage through the host I/O engine.
 * @remark website.field executed_scalar_cpu_jobs: Number of jobs that executed this stage through the scalar CPU engine.
 * @remark website.field executed_simd_cpu_jobs: Number of jobs that executed this stage through the SIMD CPU engine.
 * @remark website.field executed_vulkan_gpu_jobs: Number of jobs that executed this stage through the Vulkan GPU engine.
 * @remark website.field fallback_jobs: Number of jobs whose execution of this stage used a fallback.
 * @remark website.field stage_host_wall_ns: Host wall-clock time attributed to this pipeline stage, in nanoseconds.
 * @remark website.field stage_gpu_device_ns: GPU device time attributed to this pipeline stage, in nanoseconds.
 * @remark website.field target_rows: Query total of target rows processed, repeated on every stage row.
 * @remark website.field attempted_alignment_jobs: Number of target/query alignment jobs attempted by the statement.
 * @remark website.field fully_attested_alignment_jobs: Number of alignment jobs with complete execution-backend evidence.
 * @remark website.field unattested_alignment_jobs: Number of attempted alignment jobs without complete execution-backend evidence.
 * @remark website.field first_unattested_target_row: Target-row ordinal of the first unattested alignment job; NULL when none was recorded.
 * @remark website.field first_unattested_query_ordinal: Query ordinal of the first unattested alignment job; NULL when none was recorded.
 * @remark website.field cpu_prepared_index_builds: Query total of CPU prepared target-index builds, repeated on every stage row.
 * @remark website.field cpu_prepared_index_reuses: Query total of reuses of an already prepared CPU target index, repeated on every stage row.
 * @remark website.field cpu_prepared_unique_keys: Query total of distinct keys in the prepared CPU target-index set, repeated on every stage row.
 * @remark website.field cpu_prepared_duplicate_build_attempts: Query total of attempts to build a CPU target-index key that was already prepared, repeated on every stage row.
 * @remark website.field cpu_prepared_key_set_checksum: Checksum of the prepared CPU index key set as 16 hexadecimal digits; NULL when not recorded. Zero is a valid checksum.
 * @remark website.field cpu_prepared_build_set_checksum: Checksum of the CPU index build set as 16 hexadecimal digits; NULL when not recorded. Zero is a valid checksum.
 * @remark website.field has_operational_identity_conflict: True when operational identities were reported inconsistently or more than once where a single report was required.
 * @remark website.field persisted_index_rows: Query total of target rows served from a persisted minimizer index, repeated on every stage row.
 * @remark website.field missing_persisted_index_rows: Query total of target rows for which a required persisted minimizer index was missing, repeated on every stage row.
 * @remark website.field recomputed_target_index_rows: Query total of target rows whose minimizers were recomputed, repeated on every stage row.
 * @remark website.field persisted_generation_read_sessions: Query total of read sessions opened for persisted minimizer generations, repeated on every stage row.
 * @remark website.field persisted_relation_opens: Query total of relation opens performed while reading persisted indexes, repeated on every stage row.
 * @remark website.field persisted_index_scans: Query total of index scans performed by persisted-index readers, repeated on every stage row.
 * @remark website.field persisted_head_scans: Query total of scans for persisted generation-head records, repeated on every stage row.
 * @remark website.field persisted_head_rows_read: Query total of persisted generation-head rows read, repeated on every stage row.
 * @remark website.field persisted_head_bytes_read: Total bytes of persisted generation-head records read, repeated on every stage row.
 * @remark website.field persisted_manifest_directory_scans: Query total of scans of persisted generation manifest directories, repeated on every stage row.
 * @remark website.field persisted_manifest_rows_read: Query total of persisted manifest rows read, repeated on every stage row.
 * @remark website.field persisted_manifest_bytes_read: Total bytes of persisted manifest records read, repeated on every stage row.
 * @remark website.field persisted_directory_chunks_read: Query total of persisted directory chunks read, repeated on every stage row.
 * @remark website.field persisted_directory_bytes_read: Total bytes of persisted directory payloads read, repeated on every stage row.
 * @remark website.field persisted_metadata_repeated_reads: Query total of repeated reads of persisted generation metadata, repeated on every stage row.
 * @remark website.field persisted_metadata_heap_rows_read: Query total of heap rows read for persisted generation metadata, repeated on every stage row.
 * @remark website.field persisted_metadata_bytes_read: Total bytes of persisted generation metadata read, repeated on every stage row.
 * @remark website.field persisted_payload_heap_rows_read: Query total of heap rows read for persisted minimizer payloads, repeated on every stage row.
 * @remark website.field persisted_payload_bytes_read: Total bytes of persisted minimizer payloads read, repeated on every stage row.
 * @remark website.field persisted_heap_rows_read: Query total of heap rows read across persisted-index operations, repeated on every stage row.
 * @remark website.field persisted_toast_values_read: Query total of TOAST values read while loading persisted indexes, repeated on every stage row.
 * @remark website.field persisted_detoast_bytes: Total bytes of persisted index data detoasted, repeated on every stage row.
 * @remark website.field persisted_explicit_copy_bytes: Total bytes of persisted index data explicitly copied, repeated on every stage row.
 * @remark website.field persisted_wanted_subkeys: Query total of persisted payload subkeys requested by the read plan, repeated on every stage row.
 * @remark website.field persisted_read_subkeys: Query total of requested persisted payload subkeys actually read, repeated on every stage row.
 * @remark website.field persisted_wanted_extents: Query total of persisted payload extents requested by the read plan, repeated on every stage row.
 * @remark website.field persisted_wanted_chunks: Query total of persisted payload chunks requested by the read plan, repeated on every stage row.
 * @remark website.field persisted_skipped_rows: Query total of unrequested rows skipped within persisted payload range scans, repeated on every stage row.
 * @remark website.field persisted_repeated_chunk_reads: Query total of repeated reads of the same persisted payload chunk, repeated on every stage row.
 * @remark website.field persisted_extents_read: Query total of persisted payload extents actually read, repeated on every stage row.
 * @remark website.field persisted_chunks_read: Query total of persisted payload chunks actually read, repeated on every stage row.
 * @remark website.field persisted_bytes_read: Total bytes of persisted payload data delivered by index reads, repeated on every stage row.
 * @remark website.field persisted_bulk_read_calls: Query total of bulk reads issued for persisted index payloads, repeated on every stage row.
 * @remark website.field persisted_planned_request_groups: Query total of groups of persisted payload requests produced by the read planner, repeated on every stage row.
 * @remark website.field persisted_executed_request_groups: Query total of groups of persisted payload requests actually executed, repeated on every stage row.
 * @remark website.field persisted_range_scan_calls: Query total of ordered range scans issued for persisted payloads, repeated on every stage row.
 * @remark website.field persisted_skip_budget_rows: Query total of unrequested payload rows allowed by the planned range-scan skip budget, repeated on every stage row.
 * @remark website.field persisted_hash_build_calls: Query total of hash-index construction calls while preparing persisted minimizer data, repeated on every stage row.
 * @remark website.field persisted_assembly_ns: Total host time spent assembling persisted index payloads in nanoseconds, repeated on every stage row.
 * @remark website.field pair_sort_calls: Query total of sorts of minimizer hash/position pairs, repeated on every stage row.
 * @remark website.field host_pair_materialization_calls: Query total of host materializations of minimizer hash/position pairs, repeated on every stage row.
 * @remark website.field host_biological_hash_filter_calls: Query total of host calls filtering biological minimizer hashes, repeated on every stage row.
 * @remark website.field host_query_hash_readbacks: Query total of readbacks of query hashes from the device to the host, repeated on every stage row.
 * @remark website.field host_query_symbol_packing_calls: Query total of host calls packing query sequence symbols, repeated on every stage row.
 * @remark website.field host_target_symbol_packing_calls: Query total of host calls packing target sequence symbols, repeated on every stage row.
 * @remark website.field host_alignment_job_classification_calls: Query total of host calls classifying alignment jobs, repeated on every stage row.
 * @remark website.field host_anchor_concatenation_calls: Query total of host calls concatenating anchor collections, repeated on every stage row.
 * @remark website.field host_cigar_construction_calls: Query total of host calls constructing CIGAR operations, repeated on every stage row.
 * @remark website.field full_segment_hydrations: Query total of complete target-segment materializations, repeated on every stage row.
 * @remark website.field nthash_vector_batches: Query total of ntHash continuation vector batches; resident vector-stream kernel calls are recorded separately in SIMD JSON, repeated on every stage row.
 * @remark website.field nthash_scalar_backend_branches: Query total of ntHash branches that selected a scalar backend, repeated on every stage row.
 * @remark website.field nthash_scalar_tail_symbols: Query total of trailing ntHash symbols processed by scalar code, repeated on every stage row.
 * @remark website.field nthash_masked_tail_lanes: Query total of masked vector lanes in ntHash tail processing, repeated on every stage row.
 * @remark website.field simd_chain_vector_batches: Query total of SIMD chaining score batches, repeated on every stage row.
 * @remark website.field simd_chain_backtrack_vector_batches: Query total of SIMD chaining backtracking batches, repeated on every stage row.
 * @remark website.field simd_chain_scalar_backend_branches: Query total of chaining branches that selected scalar scoring, repeated on every stage row.
 * @remark website.field simd_chain_backtrack_scalar_branches: Query total of chaining branches that selected scalar backtracking, repeated on every stage row.
 * @remark website.field cpu_seed_positions_rejected_outside_collection: Query total of CPU seed positions rejected because they lie outside the collected target interval, repeated on every stage row.
 * @remark website.field gpu_partition_select_dispatches: Query total of GPU dispatches selecting target partitions, repeated on every stage row.
 * @remark website.field gpu_partition_anchor_merge_dispatches: Query total of GPU dispatches merging anchors from target partitions, repeated on every stage row.
 * @remark website.field gpu_index_build_candidates: Query total of candidate minimizer records considered during GPU index construction, repeated on every stage row.
 * @remark website.field gpu_index_build_canonical_emitted: Query total of canonical minimizer records emitted by GPU index construction, repeated on every stage row.
 * @remark website.field gpu_index_build_overlap_duplicates_rejected: Query total of duplicate records rejected from overlapping GPU index-build partitions, repeated on every stage row.
 * @remark website.field gpu_index_build_noncanonical_extras_rejected: Query total of noncanonical extra records rejected during GPU index construction, repeated on every stage row.
 * @remark website.field gpu_index_build_surviving_extras: Query total of extra records remaining after GPU index-build filtering, repeated on every stage row.
 * @remark website.field gpu_persisted_resident_uploads: Query total of uploads of persisted minimizer data into GPU-resident storage, repeated on every stage row.
 * @remark website.field gpu_persisted_resident_cache_hits: Query total of hits for persisted minimizer data already resident on the GPU, repeated on every stage row.
 * @remark website.field gpu_overlap_anchor_duplicates: Query total of duplicate anchors recorded from overlapping GPU target windows, repeated on every stage row.
 * @remark website.field gpu_seed_dispatches: Query total of GPU seed-lookup dispatches, repeated on every stage row.
 * @remark website.field gpu_chain_dispatches: Query total of GPU chaining-score dispatches, repeated on every stage row.
 * @remark website.field gpu_chain_backtrack_dispatches: Query total of GPU chaining backtracking dispatches, repeated on every stage row.
 * @remark website.field gpu_extend_dispatches: Query total of GPU base-alignment extension dispatches, repeated on every stage row.
 * @remark website.field gpu_target_window_request_dispatches: Query total of GPU dispatches producing target-window read requests, repeated on every stage row.
 * @remark website.field gpu_target_window_logical_units_requested: Query total of logical packed target units requested by GPU-generated target windows, repeated on every stage row.
 * @remark website.field host_target_window_requests_consumed: Query total of GPU target-window requests consumed by the host, repeated on every stage row.
 * @remark website.field host_target_window_physical_units_read: Query total of physical packed target units read by the host for GPU windows, repeated on every stage row.
 * @remark website.field host_target_window_remap_units_filled: Query total of target-unit remapping entries filled by the host for GPU windows, repeated on every stage row.
 * @remark website.field host_target_window_packed_bytes_read: Total bytes of packed target-window data read by the host, repeated on every stage row.
 * @remark website.field gpu_runtime_init_ns: Total host time spent initializing the GPU runtime in nanoseconds, repeated on every stage row.
 * @remark website.field gpu_pipeline_cache_loads: Query total of GPU pipeline-cache seed load operations, repeated on every stage row.
 * @remark website.field gpu_pipeline_cache_seed_bytes: Total bytes of GPU pipeline-cache seed data loaded, repeated on every stage row.
 * @remark website.field gpu_pipeline_cache_load_ns: Total host time spent loading GPU pipeline-cache data in nanoseconds, repeated on every stage row.
 * @remark website.field gpu_pipeline_creation_ns: Total host time spent creating GPU pipelines in nanoseconds, repeated on every stage row.
 * @remark website.field gpu_submit_cpu_ns: Total host time spent submitting GPU work from the CPU in nanoseconds, repeated on every stage row.
 * @remark website.field gpu_wait_cpu_ns: Total host time spent waiting for GPU work on the CPU in nanoseconds, repeated on every stage row.
 * @remark website.field gpu_ordinal_readback_cpu_ns: Total host time spent reading back GPU target ordinals on the CPU in nanoseconds, repeated on every stage row.
 * @remark website.field gpu_result_readback_cpu_ns: Total host time spent reading back final GPU results on the CPU in nanoseconds, repeated on every stage row.
 * @remark website.field gpu_device_critical_path_ns: Recorded GPU device critical-path time in nanoseconds, repeated on every stage row.
 * @remark website.field gpu_timestamp_query_failures: Query total of failed GPU timestamp measurements, repeated on every stage row.
 * @remark website.field gpu_queue_submits: Query total of submissions to a GPU command queue, repeated on every stage row.
 * @remark website.field gpu_host_waits: Query total of host waits for submitted GPU work, repeated on every stage row.
 * @remark website.field gpu_buffer_creates: Query total of GPU buffer creations, repeated on every stage row.
 * @remark website.field gpu_memory_allocations: Query total of GPU memory allocations, repeated on every stage row.
 * @remark website.field gpu_pipeline_creates: Query total of GPU pipeline creations, repeated on every stage row.
 * @remark website.field backend_declines: Query total of execution requests declined by the selected backend, repeated on every stage row.
 * @remark website.field backend_fallbacks: Query total of executions that switched to a fallback backend, repeated on every stage row.
 * @remark website.field are_stats_saturated: True when accumulated counters exceeded the SQL bigint limit and were clamped.
 * @remark website.field persisted_source_parent_rows_read: Query total of parent-table rows read to resolve persisted sequence sources, repeated on every stage row.
 * @remark website.field persisted_source_catalog_rows_read: Query total of catalog rows read to resolve persisted sequence sources, repeated on every stage row.
 * @remark website.field persisted_source_catalog_index_scans: Query total of catalog index scans used to resolve persisted sequence sources, repeated on every stage row.
 * @remark website.field persisted_source_fragment_rows_read: Query total of stored sequence-fragment rows read for persisted sources, repeated on every stage row.
 * @remark website.field persisted_source_decoded_bp: Number of biological sequence letters decoded while reading persisted sources, repeated on every stage row.
 * @remark website.field persisted_pair_index_conversion_calls: Query total of conversions of persisted minimizer records into pair-index form, repeated on every stage row.
 * @remark website.field persisted_segment_rebase_calls: Query total of rebasing operations for persisted target-segment coordinates, repeated on every stage row.
 * @remark website.field cold_index_preparation: JSON counters for opening and preparing persisted indexes. Payload timing includes scanning, callbacks and validation; separate decode and transfer timings are unavailable and reported as null.
 * @remark website.field simd_execution_stats: JSON observations of SIMD algorithms, policies, vector work and execution-environment evidence for the saved query.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::alignment_execution_stats_row>
last_alignment_execution_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
/** @brief Return minimizer-fragment reader counters for the latest alignment query.
 * Return SQL NULL before  the first snapshot.

 * ```sql
 * -- Return minimizer-fragment reader counters for the latest alignment query. Return SQL NULL before  the first snapshot.
 * SELECT * FROM bioseq.last_alignment_minimizer_fragment_reader_stats();
 * ```
*/
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_minimizer_fragment_reader_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
/** @brief Return hash-ordered index-reader counters for the latest alignment query.
 * Return SQL NULL before the first snapshot.

 * ```sql
 * -- Return hash-ordered index-reader counters for the latest alignment query. Return SQL NULL before the first snapshot.
 * SELECT * FROM bioseq.last_alignment_hash_major_index_reader_stats();
 * ```
*/
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_hash_major_index_reader_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
/** @brief Return GPU-operation details for the latest fully consumed alignment query.
 * CPU queries have an empty operation list Return SQL NULL before a query completes.

 * ```sql
 * -- Return GPU-operation details for the latest fully consumed alignment query. CPU queries have an empty operation list Return SQL NULL before a query completes.
 * SELECT * FROM bioseq.last_alignment_persisted_gpu_operation_stats();
 * ```
*/
std::optional<bioseq::postgres::support::jsonb_value>
last_alignment_persisted_gpu_operation_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/**
 * @brief Rebuilds a named sequence index and returns the number of indexed sequences.

 * ```sql
 * -- Build the minimizer index for the two stored sequences and return the indexed sequence count.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT * FROM bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * ROLLBACK;
 * ```
*/
std::int64_t update_index(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string kind);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Return the latest index build's sorted-run count, including runs kept in memory.
 * ```sql
 * -- Return the latest index build's sorted-run count, including runs kept in memory.
 * SELECT * FROM bioseq.last_index_merge_run_count();
 * ```
*/
std::int64_t last_index_merge_run_count();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Reset this backend's diagnostics for selecting indexes during reads.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session.
 * SELECT bioseq_internal.reset_active_read_policy_debug();
 * ```
*/
void reset_active_read_policy_debug();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Report how this backend chose and used index-read policies.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session.
 * SELECT bioseq_internal.active_read_policy_debug();
 * ```

 * @remark website.field resolver_configured_rows: Number of read-policy resolutions satisfied by an explicit index-policy row.
 * @remark website.field resolver_authoritative_defaults: Number of read-policy resolutions that used the registered default policy.
 * @remark website.field resolver_non_relation: Number of read-policy resolutions for a source without a relation identity.
 * @remark website.field dispatch_configured_batches: Number of dispatched batches using an explicitly configured read policy.
 * @remark website.field dispatch_authoritative_default_batches: Number of dispatched batches using the registered default read policy.
 * @remark website.field dispatch_non_relation_batches: Number of dispatched batches whose source has no relation identity.
 * @remark website.field last_dispatch_source: Name of the policy-resolution source recorded for the most recent dispatch.
 * @remark website.field prohibited_file_constructor_entries: Number of entries into the prohibited external-file producer constructor.
 * @remark website.field prohibited_file_writer_entries: Number of entries into the prohibited external-file writer.
*/
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::active_read_policy_debug_row>
active_read_policy_debug();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Return memory and spill statistics  from this backend's latest minimizer build.
 * ```sql
 * -- Return memory and spill statistics  from this backend's latest minimizer build.
 * SELECT * FROM bioseq.last_minimizer_sequence_build_stats();
 * ```

 * @remark website.field backend_pid: Operating-system process ID of the PostgreSQL backend that built the index.
 * @remark website.field db_oid: PostgreSQL OID of the database containing the source table.
 * @remark website.field relfilenode: Physical file identifier of the source table when the build ran.
 * @remark website.field sequence_rowno: Logical row number of the source sequence in its Bioseq column.
 * @remark website.field total_run_count: Total number of sorted minimizer runs produced by the build.
 * @remark website.field resident_runs: Runs currently kept in memory.
 * @remark website.field file_runs: Number of runs currently backed by PostgreSQL temporary files.
 * @remark website.field resident_run_bytes: Bytes reserved for runs currently kept in memory.
 * @remark website.field resident_peak_bytes: Largest total memory reservation for runs at one time.
 * @remark website.field spilled_run_bytes: Run payload bytes spilled from memory, excluding compaction output.
 * @remark website.field postgres_temp_file_bytes: Total temporary-file sizes recorded as run files are written.
 * @remark website.field postgres_temp_file_count: Number of run and compaction temporary files created.
 * @remark website.field merge_reader_buffer_bytes: Sum of buffer bytes reserved across merge-reader setups.
 * @remark website.field merge_pass_count: Number of external merge passes.
 * @remark website.field spill_count_from_pressure: Runs written to a temporary file after a memory reservation was refused.
 * @remark website.field resident_demotions: Runs moved from memory to temporary files.
 * @remark website.field position_spill_bytes: Total bytes of positions written to temporary files while grouping by hash.
 * @remark website.field position_spill_count: Number of position spill files created.
*/
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::last_minimizer_sequence_build_stats_row>
last_minimizer_sequence_build_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Return the latest asynchronous minimizer build and its worker settings.
 * ```sql
 * -- Return the latest asynchronous minimizer build and its worker settings.
 * SELECT * FROM bioseq.scheduler_minimizer_sequence_build_stats();
 * ```

 * @remark website.field backend_pid: Operating-system process ID of the PostgreSQL backend that built the index.
 * @remark website.field db_oid: PostgreSQL OID of the database containing the source table.
 * @remark website.field relfilenode: Physical file identifier of the source table when the build ran.
 * @remark website.field sequence_rowno: Logical row number of the source sequence in its Bioseq column.
 * @remark website.field total_run_count: Total number of sorted minimizer runs produced by the build.
 * @remark website.field resident_runs: Runs currently kept in memory.
 * @remark website.field file_runs: Number of runs currently backed by PostgreSQL temporary files.
 * @remark website.field resident_run_bytes: Bytes reserved for runs currently kept in memory.
 * @remark website.field resident_peak_bytes: Largest total memory reservation for runs at one time.
 * @remark website.field spilled_run_bytes: Run payload bytes spilled from memory, excluding compaction output.
 * @remark website.field postgres_temp_file_bytes: Total temporary-file sizes recorded as run files are written.
 * @remark website.field postgres_temp_file_count: Number of run and compaction temporary files created.
 * @remark website.field merge_reader_buffer_bytes: Sum of buffer bytes reserved across merge-reader setups.
 * @remark website.field merge_pass_count: Number of external merge passes.
 * @remark website.field spill_count_from_pressure: Runs written to a temporary file after a memory reservation was refused.
 * @remark website.field resident_demotions: Runs moved from memory to temporary files.
 * @remark website.field position_spill_bytes: Total bytes of positions written to temporary files while grouping by hash.
 * @remark website.field position_spill_count: Number of position spill files created.
 * @remark website.field generation: Publication generation of the saved asynchronous build statistics.
 * @remark website.field worker_build_ns: Time spent building the minimizer index on the worker, in nanoseconds.
 * @remark website.field pipeline_mode: Pipeline concurrency mode recorded for the worker: 0 for single_thread, 1 for parallel_dispatch or 2 for task_parallel.
 * @remark website.field max_concurrency: Worker concurrency limit recorded for the asynchronous build.
 * @remark website.field index_memory_cap_mb: Index memory cap recorded for the build, in megabytes.
 * @remark website.field shareable_lazy_workers: Whether the build used a shareable lazy sequence worker: 1 for yes or 0 for no.
*/
bioseq::postgres::support::table_rows<
    bioseq::postgres::sql_rows::scheduler_minimizer_sequence_build_stats_row>
scheduler_minimizer_sequence_build_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Arm one PostgreSQL failure in a minimizer worker or its staging code.
 * ```sql
 * -- In a disposable test session as superuser, arm a relation-open failure in the minimizer worker, then disarm it before running work. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.arm_async_minimizer_sequence_worker_failure('relation_open');
 * SELECT bioseq_internal.disarm_async_minimizer_sequence_worker_failure();
 * ```
*/
bool arm_async_minimizer_sequence_worker_failure(std::string failure_point);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Clear the minimizer worker and staging failure selectors.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.disarm_async_minimizer_sequence_worker_failure();
 * ```
*/
void disarm_async_minimizer_sequence_worker_failure();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Return the nsynchronous worker's PostgreSQL call and cleanup counters.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session.
 * SELECT bioseq_internal.async_minimizer_sequence_worker_boundary_stats();
 * ```

 * @remark website.field relation_leaf_calls: Number of PostgreSQL relation-operation calls made through the worker boundary.
 * @remark website.field spi_leaf_calls: Number of PostgreSQL SPI calls made through the worker boundary.
 * @remark website.field materialize_interrupt_leaf_calls: Number of PostgreSQL interrupt checks during source materialization.
 * @remark website.field materialize_scanner_read_leaf_calls: Number of PostgreSQL scanner reads during source materialization.
 * @remark website.field pg_leaf_failures: Number of PostgreSQL failures caught at worker call boundaries.
 * @remark website.field injected_pg_leaf_failures: Number of those PostgreSQL failures caused by an armed test injection.
 * @remark website.field materialize_admissions: Number of memory admissions recorded for source materialization.
 * @remark website.field materialize_failure_unwinds: Number of materialization failures that unwound resource admission.
 * @remark website.field materialize_admission_restored: Number of failed materializations that restored the expected memory reservation.
 * @remark website.field materialize_admission_mismatches: Number of failed materializations whose memory reservation did not match the expected restored state.
 * @remark website.field last_failure_point: Name of the most recently observed worker failure point.
 * @remark website.field armed_failure_point: Name of the worker failure point currently armed for injection.
 * @remark website.field last_failure_sqlstate: XX000 when the last failure was injected; an empty string otherwise.
 * @remark website.field last_failure_message: Injected PostgreSQL failure message when the last failure was injected; an empty string otherwise.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::async_minimizer_sequence_worker_boundary_stats_row>
async_minimizer_sequence_worker_boundary_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Restart this backend's worker pool and reset scheduler counters.
 * ```sql
 * -- Restart this backend's worker pool and reset scheduler counters.
 * SELECT * FROM bioseq.scheduler_restart();
 * ```
*/
void scheduler_restart();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Return current worker settings, queued work and reserved index memory.
 * ```sql
 * -- Return current worker settings, queued work and reserved index memory.
 * SELECT * FROM bioseq.scheduler_stats();
 * ```

 * @remark website.field is_supervisor: Whether shared supervisor state is available to this backend.
 * @remark website.field config_generation: Scheduler configuration generation adopted by the shared supervisor or local pool.
 * @remark website.field pipeline_concurrency_mode: Current worker-pool concurrency mode: single_thread, parallel_dispatch or task_parallel.
 * @remark website.field min_threads: Minimum thread count reported by the scheduler; currently always one.
 * @remark website.field max_threads: Maximum concurrency configured for the worker pool.
 * @remark website.field current_threads: Number of live worker threads in the local pool.
 * @remark website.field batches_scheduled: Number of batches submitted since scheduler statistics were reset.
 * @remark website.field batches_awaiting: Number of batches still in flight, including unfinished work.
 * @remark website.field batches_processed: Number of batches that completed successfully since the statistics reset.
 * @remark website.field queries_waiting: Number of queries belonging to unfinished batches.
 * @remark website.field queries_processed: Number of queries in successfully completed batches since the statistics reset.
 * @remark website.field oldest_waiting_batch_age_secs: Age of the oldest in-flight batch in seconds; zero when no batch is waiting.
 * @remark website.field queries_per_sec: Completed queries per second over the current statistics rate window.
 * @remark website.field uptime_secs: Seconds elapsed since scheduler statistics were restarted.
 * @remark website.field async_persist_jobs_waiting: Number of asynchronous persistence queue slots in waiting or active states.
 * @remark website.field index_memory_reserved_bytes: Bytes currently reserved by the index memory admission controller.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::scheduler_stats_row>
scheduler_stats();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Count physical  storage rows across shards of the named index.
 * ```sql
 * -- Count the physical rows stored for the fragment minimizer index after an explicit build.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq.index_table_row_count('doc_sequences'::regclass, 'minimizers_per_fragment');
 * ROLLBACK;
 * ```
*/
std::int64_t index_table_row_count(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Count stored index, tables, rows and generations for an absence test.
 * Get target cardinality from metadata without decoding source sequences.

 * ```sql
 * -- Check that a newly created table has no persisted fragment minimizer generation before its first index build.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT * FROM bioseq.inspect_cold_no_index_absence('doc_sequences'::regclass, 'minimizers_per_fragment');
 * ROLLBACK;
 * ```
*/
bioseq::postgres::support::named_output<
	bioseq::postgres::sql_rows::inspect_cold_no_index_absence_row>
inspect_cold_no_index_absence(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Check a published minimizer generation against its source sequence.
 * Validate its metadata and return the source locator, layout and algorithm
 * settings. This inspection leaves the stored minimizer payload unread.

 * ```sql
 * -- As the extension owner, inspect the metadata and source identity of a published minimizer generation.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq.inspect_ready_minimizer_generation('doc_sequences'::regclass, 'minimizers_per_fragment', (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1));
 * ROLLBACK;
 * ```

 * @remark website.field generation_id: Identifier of the published minimizer generation.
 * @remark website.field source_identity: Source identity encoded as 16 lowercase hexadecimal digits.
 * @remark website.field compatibility_checksum: Storage-contract checksum encoded as 16 lowercase hexadecimal digits.
 * @remark website.field source_content_checksum: Source-content checksum encoded as 16 lowercase hexadecimal digits.
 * @remark website.field database_oid: PostgreSQL OID of the database containing the source sequence.
 * @remark website.field main_relfilenode: Physical file identifier of the source Bioseq table.
 * @remark website.field column_attno: One-based PostgreSQL attribute number of the source sequence column.
 * @remark website.field sequence_rowno: Logical row number of the source sequence in its Bioseq column.
 * @remark website.field locator_revision: Source-locator payload format revision recorded in the published generation.
 * @remark website.field bucket_idx: Storage bucket containing the source sequence fragments.
 * @remark website.field shard_idx: Shard number within the sequence storage bucket.
 * @remark website.field num_fragments: Number of stored fragments belonging to the source sequence.
 * @remark website.field total_bp_size: Logical length of the sequence in letters, excluding padding.
 * @remark website.field first_sub_row_no: Logical row number of the first stored sequence fragment.
 * @remark website.field padding_letters: Number of padding letters in the stored source sequence.
 * @remark website.field alphabet_code: Numeric alphabet identifier of the source sequence.
 * @remark website.field storage_unit_byte_width: Number of bytes in one source sequence storage unit.
 * @remark website.field kmer_size: Requested k-mer length in sequence letters.
 * @remark website.field window_size: Minimizer selection window size.
 * @remark website.field requested_algorithm_id: Numeric identifier of the requested minimizer algorithm.
 * @remark website.field effective_algorithm_id: Numeric identifier of the minimizer algorithm actually selected.
 * @remark website.field physical_record_id: Identifier of the physical minimizer record representation.
 * @remark website.field highway_owner_id: Identifier of the Highway target that owns the physical record representation.
 * @remark website.field record_position_log2: Base-two logarithm of the position capacity encoded by the physical record.
 * @remark website.field physical_kmer_size: K-mer size encoded in the physical minimizer representation.
 * @remark website.field hash_width_bytes: Width of each stored minimizer hash in bytes.
 * @remark website.field packed_storage_bits: Packed storage width recorded by the minimizer compatibility contract, in bits.
 * @remark website.field payload_layout_abi_revision: ABI revision of the encoded minimizer payload layout.
 * @remark website.field producer_backend_id: Numeric identifier of the backend that produced the generation.
 * @remark website.field producer_identity_revision: Revision of the producer identity used in compatibility checks.
 * @remark website.field gpu_abi_revision: GPU ABI revision recorded in the generation compatibility contract.
 * @remark website.field segment_geometry_revision: Revision of the position-segment geometry contract.
 * @remark website.field position_segment_span_bp: Number of source sequence letters covered by a position segment.
 * @remark website.field position_segment_count: Number of position segments in the minimizer generation.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::inspect_ready_minimizer_generation_row>
inspect_ready_minimizer_generation(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string index_name,
	std::int64_t query_key);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
/**
 * @brief Verify a published index whose minimizer records are ordered by hash.
 * Check the source sequence, metadata and stored streams, then return their
 * identities, sizes and checksums as JSON.

 * ```sql
 * -- As the extension owner, publish a scalar hash-major sequence generation and inspect its identities, sizes and checksums.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * SET LOCAL bioseq.aligner.grouped_soa_generation.enabled = on;
 * SET LOCAL bioseq.index.async_persistence.enabled = off;
 * SET LOCAL bioseq.index.recompute_async_persistence.enabled = off;
 * SET LOCAL bioseq.aligner.minimizer.algorithm = 'scalar';
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_sequence');
 * SELECT * FROM bioseq.inspect_ready_hash_major_minimizer_generation('doc_sequences'::regclass, 'minimizers_per_sequence', (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1));
 * ROLLBACK;
 * ```
*/
bioseq::postgres::support::jsonb_value
inspect_ready_hash_major_minimizer_generation(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string index_name,
	std::int64_t query_key);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
    .parallel = bioseq::sql::parallel_safety::restricted,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::execute,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
/**
 * @brief Test  reading a published index with serial or parallel heap scans.
 * Zero workers selects a serial scan A positive count requests workers
 * sharing one scan. Check row contents and checksums, and return read counts
 * and elapsed time. Missing, duplicate or corrupt rows raise an error.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Read and verify the published hash-major generation with a serial heap scan; zero selects no parallel workers.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq_internal.hash_major_minimizer_parallel_scan_probe('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 0);
 * ROLLBACK;
 * ```
*/
bioseq::postgres::support::jsonb_value hash_major_minimizer_parallel_scan_probe(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int32_t requested_workers);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief SQL test entry point for building a minimizer generation.
 * Return sourct identity, checksums and write counts after publication.
 * The layout checks currently prevent a successful call: this wrapper accepts
 * layouts 3 and 4, but  the builder requires layout 5.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Exercise the legacy fixture wrapper with its accepted layout code 3. The current builder requires layout 5, so this call is expected to fail rather than publish a generation.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT * FROM bioseq_internal.build_minimizer_fixture('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 15::smallint, 10::smallint, 3::smallint, 'scalar');
 * ROLLBACK;
 * ```

 * @remark website.field sidecar_table: Unqualified name of the minimizer storage relation in the bioseq_internal schema.
 * @remark website.field layout: Numeric minimizer payload-layout identifier.
 * @remark website.field generation_id: Identifier of the published minimizer generation.
 * @remark website.field source_identity: Source identity encoded as 16 lowercase hexadecimal digits.
 * @remark website.field source_content_checksum: Source-content checksum encoded as 16 lowercase hexadecimal digits.
 * @remark website.field compatibility_checksum: Storage-contract checksum encoded as 16 lowercase hexadecimal digits.
 * @remark website.field source_num_fragments: Number of fragments in the source sequence used by this generation.
 * @remark website.field source_total_bp: Logical source-sequence length in letters, excluding padding.
 * @remark website.field source_padding_letters: Number of padding letters in the source sequence.
 * @remark website.field writer_rows: Number of rows written to minimizer generation storage.
 * @remark website.field writer_batches: Number of storage write batches used to publish the generation.
 * @remark website.field published_head_bytes: Encoded size of the published generation header, in bytes.
 * @remark website.field control_count: Number of minimizer control records in the generation.
 * @remark website.field position_count: Number of minimizer occurrences encoded in the position payload.
 * @remark website.field directory_entries: Number of directory entries in the published payload.
 * @remark website.field position_segment_count: Number of position segments in the minimizer generation.
 * @remark website.field control_encoded_bytes: Encoded control-payload size in bytes.
 * @remark website.field position_encoded_bytes: Encoded position-payload size in bytes.
 * @remark website.field control_chunks: Number of chunks storing encoded minimizer controls.
 * @remark website.field position_chunks: Number of chunks storing encoded minimizer positions.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_minimizer_fixture_row>
build_minimizer_fixture(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
	[[bioseq_sql::param({
		.default_value = bioseq::sql::checked_sql{"'scalar'"},
	})]]
	std::string requested_backend,
	[[bioseq_sql::param({
		.default_value = bioseq::sql::checked_sql{"0"},
	})]]
	std::int64_t position_segment_span_bp);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief SQL test entry point for checking a minimizer generation and its source.
 * Return payload checksums, lookup results, read counts and cache counts.
 * The layout checks currently prevent a successful call: this wrapper accepts
 * layouts 3 and 4, but the reader requires layout 5.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Exercise the legacy reader wrapper with its accepted layout code 3. The current reader requires layout 5, so this call is expected to fail.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT * FROM bioseq_internal.read_minimizer_fixture('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 15::smallint, 10::smallint, 3::smallint, 'scalar');
 * ROLLBACK;
 * ```

 * @remark website.field sidecar_table: Unqualified name of the minimizer storage relation in the bioseq_internal schema.
 * @remark website.field layout: Numeric minimizer payload-layout identifier.
 * @remark website.field generation_id: Identifier of the published minimizer generation.
 * @remark website.field source_identity: Source identity encoded as 16 lowercase hexadecimal digits.
 * @remark website.field source_content_checksum: Source-content checksum encoded as 16 lowercase hexadecimal digits.
 * @remark website.field compatibility_checksum: Storage-contract checksum encoded as 16 lowercase hexadecimal digits.
 * @remark website.field source_num_fragments: Number of fragments in the source sequence used by this generation.
 * @remark website.field source_total_bp: Logical source-sequence length in letters, excluding padding.
 * @remark website.field source_padding_letters: Number of padding letters in the source sequence.
 * @remark website.field validated_control_bytes: Number of control-payload bytes validated against the stored generation.
 * @remark website.field validated_position_bytes: Number of position-payload bytes validated against the stored generation.
 * @remark website.field validated_control_checksum: Checksum of validated control payload, encoded as 16 lowercase hexadecimal digits.
 * @remark website.field validated_position_checksum: Checksum of validated position payload, encoded as 16 lowercase hexadecimal digits.
 * @remark website.field lookup_count: Total occurrence count for the minimizer hash selected by the fixture lookup.
 * @remark website.field lookup_bp: Base position of the first returned occurrence; zero when the lookup returned no positions.
 * @remark website.field is_lookup_reverse: Whether the first returned occurrence is on the reverse strand; false when no position was returned.
 * @remark website.field are_lookup_positions_complete: Whether the lookup returned the complete positions for the selected hash.
 * @remark website.field relation_opens: Number of storage-relation opens performed by this read.
 * @remark website.field index_scans: Number of index scans performed by this read.
 * @remark website.field head_scans: Number of scans used to read generation headers.
 * @remark website.field manifest_directory_scans: Number of scans used to read generation manifests and directories.
 * @remark website.field planned_request_groups: Number of payload request groups planned by the read session.
 * @remark website.field executed_request_groups: Number of planned payload request groups actually executed.
 * @remark website.field bulk_read_calls: Number of bulk payload read calls issued by the read session.
 * @remark website.field range_scan_calls: Number of range-scan calls used to read minimizer payload.
 * @remark website.field payload_wanted_extents: Number of payload extents requested by the read plan.
 * @remark website.field payload_read_extents: Number of requested payload extents read.
 * @remark website.field payload_wanted_chunks: Number of payload chunks requested by the read plan.
 * @remark website.field payload_delivered_chunks: Number of payload chunks delivered to the reader.
 * @remark website.field payload_delivered_bytes: Number of payload bytes delivered to the reader.
 * @remark website.field payload_heap_rows_read: Number of heap rows read while retrieving payload chunks.
 * @remark website.field payload_skipped_rows: Number of scanned payload rows skipped instead of being delivered.
 * @remark website.field toast_values_read: Number of out-of-line TOAST values read by the session.
 * @remark website.field detoast_bytes: Number of bytes materialized while detoasting stored values.
 * @remark website.field cache_lookups: Number of cache lookups performed by the read session.
 * @remark website.field selective_cold_index_preparation: JSON object containing the selective cold-index preparation counters for this fixture.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::read_minimizer_fixture_row>
read_minimizer_fixture(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
	[[bioseq_sql::param({
		.default_value = bioseq::sql::checked_sql{"'scalar'"},
	})]]
	std::string requested_backend);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Stream a complete stored minimizer generation for an I/O test.
 * Check its expected backend and layout, then, return byte counts, checksums
 * and scan counts. Treat payload bytes as opaque during this scan.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Build a scalar generation, then check its full grouped-SoA payload with matching k=15, w=10, layout=5 and backend.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT * FROM bioseq_internal.build_ready_minimizer_fragment_generation('doc_sequences'::regclass, 'minimizers_per_fragment', (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 'scalar');
 * SELECT * FROM bioseq_internal.scan_grouped_soa_minimizer_generation_test('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 15::smallint, 10::smallint, 5::smallint, 'scalar');
 * ROLLBACK;
 * ```

 * @remark website.field sidecar_table: Unqualified name of the minimizer storage relation in the bioseq_internal schema.
 * @remark website.field selected_backend: Name of the chunk reader backend selected for the full generation scan.
 * @remark website.field generation_id: Identifier of the published minimizer generation.
 * @remark website.field control_bytes: Number of control-payload bytes streamed by the scan.
 * @remark website.field position_bytes: Number of position-payload bytes streamed by the scan.
 * @remark website.field control_checksum: Checksum of the streamed control payload, encoded as 16 lowercase hexadecimal digits.
 * @remark website.field position_checksum: Checksum of the streamed position payload, encoded as 16 lowercase hexadecimal digits.
 * @remark website.field relation_opens: Number of storage-relation opens performed by this read.
 * @remark website.field index_scans: Number of index scans performed by this read.
 * @remark website.field range_scan_calls: Number of range-scan calls used to read minimizer payload.
 * @remark website.field payload_heap_rows: Number of heap rows read while scanning the payload.
 * @remark website.field delivered_chunks: Number of payload chunks delivered by the full-generation scan.
 * @remark website.field delivered_bytes: Number of payload bytes delivered by the full-generation scan.
 * @remark website.field planned_extents: Number of payload extents planned for the full-generation scan.
 * @remark website.field completed_extents: Number of planned extents completed by the full-generation scan.
 * @remark website.field scan_calls: Number of full-generation payload scan calls.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::scan_grouped_soa_minimizer_generation_test_row>
scan_grouped_soa_minimizer_generation_test(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int16_t kmer_size,
	std::int16_t window_size,
	std::int16_t selected_layout,
	std::string requested_backend);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Invalidate a stored sequence for a missing-fragment  test.
 * Validate the requested non-first fragment, then invalidate its source epoch
 * so readers reject the sequence. The fragment rows remain stored.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Invalidate a long sequence at its second fragment so a later reader can test rejection of a missing fragment.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, repeat(s, 64)::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq_internal.remove_sequence_fragment('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 1);
 * ROLLBACK;
 * ```
*/
void remove_sequence_fragment(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::int32_t fragment_ordinal);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Copy a donor index's predicate or expression into the target primary idex.
 * Tests use this catalog change to check rejection of malformed index schemas.
 * The change belongs to the current transaction

 * ```sql
 * -- In a disposable test database as superuser, copy a partial-index predicate into a generation storage primary index to test malformed-catalog rejection. The transaction rollback restores the catalog.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * DO $$
 * DECLARE storage regclass;
 * BEGIN
 *   SELECT format('bioseq_internal.%I', sidecar_table)::regclass INTO storage
 *   FROM bioseq_internal.build_ready_minimizer_fragment_generation('doc_sequences'::regclass, 'minimizers_per_fragment', (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 'scalar');
 *   EXECUTE format('CREATE INDEX doc_donor_partial ON %s (query_key) WHERE query_key > 0', storage);
 *   PERFORM bioseq_internal.copy_primary_index_shape(storage,
 *     'bioseq_internal.doc_donor_partial'::regclass, 'partial');
 * END;
 * $$;
 * ROLLBACK;
 * ```
*/
void copy_primary_index_shape(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid target_table,
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid donor_index,
	std::string shape);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Corrupt the saved source checksum while keeping index metadata consistent.
 * XOR the supplied mask into the checksum and update the  head, manifest and
 * their row checksums. Tests can then check validation against the source sequence.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Corrupt one checksum bit in an isolated generation storage table, then roll back the transaction.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq_internal.rewrite_source_checksum(
 *   format('bioseq_internal.%I', sidecar_table)::regclass,
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 1)
 * FROM bioseq_internal.build_ready_minimizer_fragment_generation('doc_sequences'::regclass, 'minimizers_per_fragment', (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 'scalar');
 * ROLLBACK;
 * ```
*/
void rewrite_source_checksum(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid target_table,
	std::int64_t query_key,
	std::int64_t xor_mask);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Run a read-session failure test against a stored minimizer generation.
 * The selected case checks invalid read order, incompatible metadata or
 * refused memory, and releases session resources on failure.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Request payload before the read-session protocol permits it. This diagnostic intentionally raises an error and tests cleanup.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq_internal.exercise_generation_session_failure('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 'early_payload');
 * ROLLBACK;
 * ```
*/
void exercise_generation_session_failure(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t query_key,
	std::string failure_mode);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Exercise creation, clearing or rollback of minimizer-generation storage.
 * Return the relation OIDs before and after the operation, whether  the handle
 * was rebound, and whether cached data remains. Missing OIDs become SQL NULL.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Exercise clearing a generation inside a savepoint and rolling it back; the returned row describes the restored storage identity.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq_internal.inspect_generation_storage_transition('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 'clear_generation_savepoint_rollback');
 * ROLLBACK;
 * ```

 * @remark website.field before_oid: OID of the generation storage relation before the requested operation; NULL if absent.
 * @remark website.field operation_oid: OID returned by the requested generation storage operation; NULL if no relation was returned.
 * @remark website.field after_oid: OID of the generation storage relation after the operation; NULL if absent.
 * @remark website.field is_same_handle_rebound: Whether the same storage handle was rebound during the operation.
 * @remark website.field has_generation_schema_after: Whether the resulting relation has the generation-storage schema.
 * @remark website.field is_chunk_cache_hit: Whether the probe found the expected chunk in cache; NULL when the operation did not probe it.
 * @remark website.field is_assembled_cache_hit: Whether the probe found the expected assembled object in cache; NULL when the operation did not probe it.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::generation_storage_state_row>
inspect_generation_storage_transition(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string operation);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Build a stored minimizer-fragment index for one sequence in a SQL test.
 * Publish and verdfy the generation, then return build and backend counters.
 * Keep the active index choice and last-update timestamp unchanged.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Build and verify a scalar-produced fragment generation for one stored sequence without changing the active read policy.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT * FROM bioseq_internal.build_ready_minimizer_fragment_generation('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 'scalar');
 * ROLLBACK;
 * ```

 * @remark website.field sidecar_table: Name of the sidecar relation used to store the minimizer index.
 * @remark website.field column_attno: Attribute number of the sequence column in the source table.
 * @remark website.field source_sequence_rowno: Columnar row number of the source sequence selected for the test build.
 * @remark website.field source_epoch: Source-version epoch encoded as a fixed-width hexadecimal string.
 * @remark website.field source_num_fragments: Number of stored fragments in the source sequence.
 * @remark website.field source_total_bp: Logical length of the source sequence in bases.
 * @remark website.field selected_backend: Name of the minimizer-producing backend selected for the build.
 * @remark website.field layout: Persisted minimizer layout identifier; 5 denotes the segment-local grouped-SoA layout.
 * @remark website.field is_published: Whether the completed generation was published.
 * @remark website.field is_head_manifest_verified: Whether the fixture verified the published head and manifest against the completed generation.
 * @remark website.field generation_id: Identifier of the published minimizer generation.
 * @remark website.field source_identity: Source identity bound to the published generation, encoded as a fixed-width hexadecimal string.
 * @remark website.field source_content_checksum: Source-content checksum bound to the published generation, encoded as a fixed-width hexadecimal string.
 * @remark website.field compatibility_checksum: Checksum of the persisted storage contract, encoded as a fixed-width hexadecimal string.
 * @remark website.field manifest_target_length: Sequence length in bases recorded in the generation manifest.
 * @remark website.field kmer_size: Number of bases per k-mer recorded in the manifest.
 * @remark website.field window_size: Number of consecutive k-mers per minimizer window recorded in the manifest.
 * @remark website.field minimizer_scheme_id: Identifier of the hashing and minimizer-selection scheme recorded in the manifest.
 * @remark website.field minimizer_scheme_revision: Revision of the hashing and minimizer-selection scheme recorded in the manifest.
 * @remark website.field gpu_abi_revision: GPU data-interface revision recorded in the persisted manifest.
 * @remark website.field position_segment_span_bp: Number of bases covered by each position segment in the manifest.
 * @remark website.field position_segment_count: Number of position segments recorded in the manifest.
 * @remark website.field writer_rows: Number of encoded index rows recorded by the generation writer.
 * @remark website.field writer_batches: Number of catalog multi-insert batches performed by the generation writer.
 * @remark website.field control_count: Number of control entries recorded in the published generation manifest.
 * @remark website.field position_count: Number of minimizer positions recorded in the published generation manifest.
 * @remark website.field producer_scan_count: Total producer scan attempts, including geometry retries.
 * @remark website.field geometry_retry_count: Extra producer scans recorded when the persisted position-segment span changes between replays.
 * @remark website.field short_core_runs: Number of core collection runs whose available sequence context was too short for a minimizer window.
 * @remark website.field scalar_core_runs: Number of core collection runs handled by the scalar backend.
 * @remark website.field exact_simd_core_runs: Number of core collection runs handled by the exact SIMD backend.
 * @remark website.field bounded_nthash_core_runs: Number of core collection runs handled by the bounded ntHash SIMD backend.
 * @remark website.field vulkan_core_runs: Number of core collection runs handled by the Vulkan backend.
 * @remark website.field is_execution_accepted: Whether the fragment execution receipt accepts the selected backend, layout, work counters, and required producer scan count.
 * @remark website.field are_exact_simd_stats_available: Whether the execution receipt contains exact SIMD statistics.
 * @remark website.field are_exact_simd_stats_consistent: Whether exact SIMD statistics are present and pass consistency checks requiring nonempty vector work.
 * @remark website.field exact_simd_vector_batches: Number of vector batches recorded by the exact SIMD backend.
 * @remark website.field exact_simd_scalar_backend_branches: Number of scalar-backend branches recorded during exact SIMD execution.
 * @remark website.field are_bounded_nthash_stats_available: Whether the execution receipt contains bounded ntHash SIMD statistics.
 * @remark website.field bounded_nthash_status: Diagnostic status of bounded ntHash execution; ok denotes success, while invalid_state is the initial value when that backend did not run.
 * @remark website.field are_bounded_nthash_stats_consistent: Whether bounded ntHash statistics are present, have status ok, and pass consistency checks requiring nonempty vector work.
 * @remark website.field bounded_nthash_packed_decode_batches: Number of packed-symbol decoding batches recorded by the bounded ntHash SIMD backend.
 * @remark website.field bounded_nthash_fallback_calls: Number of fallback calls recorded by the bounded ntHash SIMD backend.
 * @remark website.field vulkan_completed_roundtrips: Number of completed Vulkan dispatch and result-download round trips.
 * @remark website.field vulkan_downloaded_minimizers: Number of minimizers downloaded from completed Vulkan work.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_ready_minimizer_fragment_generation_row>
build_ready_minimizer_fragment_generation(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string backend);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Build a stored whole-sequence minimizer index for a SQL test.
 * Publish and verify, the generation, release temporary  sort runs, and return
 * build counters. Keep the active index choice and update timestamp unchanged.

 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Build and verify a scalar-produced whole-sequence generation without changing the active read policy.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT * FROM bioseq_internal.build_ready_minimizer_sequence_generation('doc_sequences'::regclass, 'minimizers_per_sequence',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 'scalar');
 * ROLLBACK;
 * ```

 * @remark website.field sidecar_table: Name of the sidecar relation used to store the minimizer index.
 * @remark website.field column_attno: Attribute number of the sequence column in the source table.
 * @remark website.field source_sequence_rowno: Columnar row number of the source sequence selected for the test build.
 * @remark website.field source_epoch: Source-version epoch encoded as a fixed-width hexadecimal string.
 * @remark website.field source_num_fragments: Number of stored fragments in the source sequence.
 * @remark website.field source_total_bp: Logical length of the source sequence in bases.
 * @remark website.field selected_backend: Name of the minimizer-producing backend selected for the build.
 * @remark website.field layout: Persisted minimizer layout identifier; 5 denotes the segment-local grouped-SoA layout.
 * @remark website.field is_published: Whether the completed generation was published.
 * @remark website.field is_head_manifest_verified: Whether the fixture verified the published head and manifest against the completed generation.
 * @remark website.field generation_id: Identifier of the published minimizer generation.
 * @remark website.field source_identity: Source identity bound to the published generation, encoded as a fixed-width hexadecimal string.
 * @remark website.field source_content_checksum: Source-content checksum bound to the published generation, encoded as a fixed-width hexadecimal string.
 * @remark website.field compatibility_checksum: Checksum of the persisted storage contract, encoded as a fixed-width hexadecimal string.
 * @remark website.field manifest_target_length: Sequence length in bases recorded in the generation manifest.
 * @remark website.field kmer_size: Number of bases per k-mer recorded in the manifest.
 * @remark website.field window_size: Number of consecutive k-mers per minimizer window recorded in the manifest.
 * @remark website.field minimizer_scheme_id: Identifier of the hashing and minimizer-selection scheme recorded in the manifest.
 * @remark website.field minimizer_scheme_revision: Revision of the hashing and minimizer-selection scheme recorded in the manifest.
 * @remark website.field gpu_abi_revision: GPU data-interface revision recorded in the persisted manifest.
 * @remark website.field position_segment_span_bp: Number of bases covered by each position segment in the manifest.
 * @remark website.field position_segment_count: Number of position segments recorded in the manifest.
 * @remark website.field writer_rows: Number of encoded index rows recorded by the generation writer.
 * @remark website.field writer_batches: Number of catalog multi-insert batches performed by the generation writer.
 * @remark website.field control_count: Number of control entries recorded in the published generation manifest.
 * @remark website.field position_count: Number of minimizer positions recorded in the published generation manifest.
 * @remark website.field spill_run_count: Number of sorted, nonempty minimizer event runs written to temporary spill storage.
 * @remark website.field spilled_event_count: Number of minimizer events recorded in temporary spill runs.
 * @remark website.field spill_operation_epoch: Unique operation epoch assigned when the collected spill was qualified for replay.
 * @remark website.field spill_generation_token: Identity token of the temporary spill storage generation bound to the qualified spill receipt.
 * @remark website.field spill_mutation_revision: Spill storage mutation revision captured when the collected spill was qualified.
 * @remark website.field spill_qualification_scan_count: Number of scans used to verify the merged spill event count and content digest before replay.
 * @remark website.field spill_qualification_merge_count: Number of storage merge passes performed while qualifying the collected spill.
 * @remark website.field configured_core_span_bp: Configured maximum core collection span in bases.
 * @remark website.field peak_core_span_bp: Largest core collection span actually processed, in bases.
 * @remark website.field peak_core_events: Largest retained, deduplicated minimizer event count in a core collection buffer.
 * @remark website.field short_core_runs: Number of core collection runs whose available sequence context was too short for a minimizer window.
 * @remark website.field scalar_core_runs: Number of core collection runs handled by the scalar backend.
 * @remark website.field exact_simd_core_runs: Number of core collection runs handled by the exact SIMD backend.
 * @remark website.field bounded_nthash_core_runs: Number of core collection runs handled by the bounded ntHash SIMD backend.
 * @remark website.field vulkan_core_runs: Number of core collection runs handled by the Vulkan backend.
 * @remark website.field is_execution_accepted: Whether the whole-sequence execution receipt passes its spill, backend, replay, and manifest consistency checks.
 * @remark website.field are_exact_simd_stats_available: Whether the execution receipt contains exact SIMD statistics.
 * @remark website.field are_exact_simd_stats_consistent: Whether exact SIMD statistics are present and pass consistency checks requiring nonempty vector work.
 * @remark website.field exact_simd_vector_batches: Number of vector batches recorded by the exact SIMD backend.
 * @remark website.field exact_simd_scalar_backend_branches: Number of scalar-backend branches recorded during exact SIMD execution.
 * @remark website.field are_bounded_nthash_stats_available: Whether the execution receipt contains bounded ntHash SIMD statistics.
 * @remark website.field bounded_nthash_status: Diagnostic status of bounded ntHash execution; ok denotes success, while invalid_state is the initial value when that backend did not run.
 * @remark website.field are_bounded_nthash_stats_consistent: Whether bounded ntHash statistics are present, have status ok, and pass consistency checks requiring nonempty vector work.
 * @remark website.field bounded_nthash_packed_decode_batches: Number of packed-symbol decoding batches recorded by the bounded ntHash SIMD backend.
 * @remark website.field bounded_nthash_fallback_calls: Number of fallback calls recorded by the bounded ntHash SIMD backend.
 * @remark website.field vulkan_completed_roundtrips: Number of completed Vulkan dispatch and result-download round trips.
 * @remark website.field vulkan_downloaded_minimizers: Number of minimizers downloaded from completed Vulkan work.
 * @remark website.field replay_scan_count: Number of replay scans attempted while planning and encoding the generation.
 * @remark website.field replay_merge_pass_count: Number of spill-storage merge passes executed during replay.
 * @remark website.field replay_reroute_pass_count: Number of retries that repartitioned spill runs after halving the position-segment span.
 * @remark website.field replay_completed_scan_count: Number of replay scans that completed successfully.
 * @remark website.field replay_failed_scan_count: Number of replay scans that returned an error.
 * @remark website.field replay_binding_checked_failed_scan_count: Number of failed receiver scans whose spill storage binding and content were nevertheless fully checked.
 * @remark website.field replay_illegal_span_transition_count: Number of rejected replay requests with an invalid position-segment span transition.
 * @remark website.field replay_first_completed_event_count: Number of minimizer events visited by the first successful replay scan.
 * @remark website.field replay_last_completed_event_count: Number of minimizer events visited by the most recent successful replay scan.
 * @remark website.field replay_final_span_bp: Position-segment span in bases bound to the final completed replay.
 * @remark website.field replay_final_generation_token: Spill storage generation token bound to the final completed replay.
 * @remark website.field replay_final_mutation_revision: Spill storage mutation revision bound to the final completed replay.
 * @remark website.field replay_visited_event_count: Total minimizer events visited across replay scans, including scans whose receiver failed.
 * @remark website.field manifest_position_count: Minimizer position count bound to the execution receipt from the completed manifest.
 * @remark website.field manifest_position_segment_span_bp: Position-segment span in bases bound to the execution receipt from the completed manifest.
 * @remark website.field release_generation_token: Spill storage generation token captured immediately before releasing the temporary runs.
 * @remark website.field release_mutation_before: Spill storage mutation revision immediately before releasing the temporary runs.
 * @remark website.field release_mutation_after: Spill storage mutation revision immediately after releasing the temporary runs.
 * @remark website.field are_spill_resources_released: Whether cleanup left no spill groups, preserved the storage generation token, and advanced its mutation revision by one.
*/
bioseq::postgres::support::table_rows<
	bioseq::postgres::sql_rows::build_ready_minimizer_sequence_generation_row>
build_ready_minimizer_sequence_generation(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid source_table,
	std::string index_name,
	std::int64_t sequence_rowno,
	std::string backend);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Counts distinct subkeys stored for one logical index key.
 * ```sql
 * -- Count distinct physical subkeys for one sequence in the built fragment minimizer index.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq.index_distinct_sub_keys('doc_sequences'::regclass, 'minimizers_per_fragment', (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1));
 * ROLLBACK;
 * ```
*/
std::int64_t index_distinct_sub_keys(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name,
	std::int64_t query_key);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Drop every physical Bioseq sequence-index table in the current database.
 * ```sql
 * -- Drop physical Bioseq sequence-index tables in a disposable test database, then roll back the change. This affects every Bioseq table in that database.
 * BEGIN;
 * SELECT * FROM bioseq.drop_all_index_tables();
 * ROLLBACK;
 * ```
*/
void drop_all_index_tables();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Set insert-time index maintenance for every sequence column in a table
 * UPDATE_ON_INSERT enables it. Other policy strings select manual updates.
 * Reject unknown indexes and indexes that cannot be updated during insertion.

 * ```sql
 * -- Select manual index updates for a table whose fragment minimizers are built explicitly.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT * FROM bioseq.set_index_policy('doc_sequences'::regclass, 'minimizers_per_fragment', 'MANUAL');
 * ROLLBACK;
 * ```
*/
void set_index_policy(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name,
	std::string policy);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Select one index for reads of every sequence column in the table.
 * Clear the previous  choce. Indexes stored as table-wide chunks are rejected.

 * ```sql
 * -- Select the fragment minimizer index for reads of the table.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq.set_active_read_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * ROLLBACK;
 * ```
*/
void set_active_read_index(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Decode the logical row number from a USING bioseq table's ctid.
 * ```sql
 * -- Decode the logical sequence row number from the ctid returned by a Bioseq table scan.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT id, bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences;
 * ROLLBACK;
 * ```
*/
std::int64_t column_row_number(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::tid,
	})]]
	bioseq::postgres::support::tuple_identifier ctid);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Make a selected inline-sequence operation raise a PostgreSQL error.
 * Tests can choose relation lookup, tuple read, detoasting or relation close.

 * ```sql
 * -- In a disposable test session as superuser, make the next inline-sequence tuple read fail. Disconnect this test session after observing the injected failure.
 * SELECT bioseq_internal.arm_inline_sequence_pg_failure('tuple_read');
 * ```
*/
void arm_inline_sequence_pg_failure(std::string failure_point);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Insert or replace one physical index chunk for a storage test.
 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Write a reserved temporary-run subkey, as used by the storage test. Such subkeys must not be counted as ordinary data fragments.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq_internal.write_index_chunk('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1), 4611686018427387904::bigint, 0, '\x00'::bytea);
 * ROLLBACK;
 * ```
*/
void write_index_chunk(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name,
	std::int64_t query_key,
	std::int64_t sub_key,
	std::int32_t chunk_no,
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::bytea,
	})]]
	bioseq::postgres::support::bytea_value payload);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::or_replace,
})]]
/** @brief Delete an index key's manifest chunks from all shards for a storage test.
 * ```sql
 * -- Run this internal fixture in a disposable database as superuser. Remove a fixture generation manifest to test missing-index detection, then roll back the transaction.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq.update_index('doc_sequences'::regclass, 'minimizers_per_fragment');
 * SELECT * FROM bioseq_internal.delete_index_manifest('doc_sequences'::regclass, 'minimizers_per_fragment',
 *   (SELECT bioseq.column_row_number('doc_sequences'::regclass, ctid) FROM doc_sequences WHERE id = 1));
 * ROLLBACK;
 * ```
*/
void delete_index_manifest(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::pg::regclass,
	})]]
	Oid rel,
	std::string name,
	std::int64_t query_key);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Arm a sequence-reader failure  or run a named runtime test in this backend.
 * ```sql
 * -- In a disposable test session as superuser, arm a sequence-scanner failure, then clear it. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.arm_sequence_source_failure('read_before_scanner');
 * SELECT bioseq_internal.disarm_sequence_source_failure();
 * ```
*/
void arm_sequence_source_failure(std::string failure_point);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Clear this backend's armed sequence-reader failure.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.disarm_sequence_source_failure();
 * ```
*/
void disarm_sequence_source_failure();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Reset sequence-reader counters and clear the armed reader failure.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.reset_sequence_source_counters();
 * ```
*/
void reset_sequence_source_counters();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
[[bioseq_sql::result({
    .sql_type = bioseq::sql::pg::jsonb,
})]]
/** @brief Return sequence-reader creation, reuse and cleanup counters as JSON.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.sequence_source_counters();
 * ```
*/
bioseq::postgres::support::jsonb_value sequence_source_counters();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/**
 * @brief Make the next SliceProject shared-memory payload incomplete or malformed.
 * The none selector clears a pending test.

 * ```sql
 * -- In a disposable test session as superuser, clear any pending malformed shared-memory payload fixture. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.arm_slice_dsm_fixture('none');
 * ```
*/
void arm_slice_dsm_fixture(std::string mode);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Check that a worker cannot use a sequence reader owned by the backend thread.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.sequence_source_wrong_thread_probe();
 * ```
*/
bool sequence_source_wrong_thread_probe();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Check reference-count overflow, route-merge rollback and reader cleanup.
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.sequence_source_fault_contract_probe();
 * ```
*/
bool sequence_source_fault_contract_probe();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Check  that providers sharing a reldtion are distinguished by generation id
 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.sequence_source_route_alias_probe();
 * ```
*/
bool sequence_source_route_alias_probe();

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
    .volatility = bioseq::sql::volatility::stable,
    .parallel = bioseq::sql::parallel_safety::safe,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Compare all four orientations through lazy and flat, sequence readers.
 * Return true on success or SQL NULL for null input. Mismatches raise an error.

 * ```sql
 * -- Check that lazy and flat sequence readers agree in all four orientations. Run this internal diagnostic as the extension owner; it requires BIOSEQ_TEST_INDEXES and a source-backed value from a Bioseq table.
 * BEGIN;
 * SET LOCAL bioseq.index.insert_maintenance.enabled = off;
 * CREATE TABLE doc_sequences (
 *   id integer, seq bioseq.seq('dna4', 64)
 * ) USING bioseq;
 * WITH dna AS (
 *   SELECT string_agg(translate(md5(i::text),
 *     '0123456789abcdef', 'ACGTACGTACGTACGT'), '' ORDER BY i) AS s
 *   FROM generate_series(1, 64) AS g(i)
 * )
 * INSERT INTO doc_sequences
 * SELECT 1, s::bioseq.seq('dna4', 64) FROM dna
 * UNION ALL
 * SELECT 2, substring(s FROM 257 FOR 512)::bioseq.seq('dna4', 64) FROM dna;
 * SELECT bioseq_internal.sequence_facade_orientation_probe(seq)
 * FROM doc_sequences WHERE id = 1;
 * ROLLBACK;
 * ```
*/
std::optional<bool> sequence_facade_orientation_probe(
	[[bioseq_sql::param({
		.sql_type = bioseq::sql::custom_type<"bioseq.seq">,
	})]]
	std::optional<Datum> sequence);

[[bioseq_sql::function({
    .schema = bioseq::sql::fixed_schema<"bioseq_internal">,
    .create = bioseq::sql::create_mode::create,
})]]
[[bioseq_sql::revoke({
    .privileges = bioseq::sql::privileges::all,
    .from = bioseq::sql::public_role,
})]]
/** @brief Test PREPARE TRANSACTION rejection while a sequence-reader domain is live.
 * The probe raises an error, including when the expected rejection occurs.

 * ```sql
 * -- Run this internal diagnostic as the extension owner in a disposable test session. This diagnostic intentionally raises an error. Requires BIOSEQ_TEST_INDEXES.
 * SELECT bioseq_internal.sequence_source_pre_prepare_probe();
 * ```
*/
void sequence_source_pre_prepare_probe();

} // namespace bioseq::pg::sql_routines

namespace bioseq::pg::detail {

/** @brief Install executor hooks that save alignment statistics for this backend. */
void install_alignment_execution_hooks();

/** @return Borrowed latest alignment snapshot, including partial queries, or an empty optional. */
[[nodiscard]] const std::optional<
	bioseq::postgres::table::alignment_execution::alignment_execution_snapshot>&
last_alignment_execution_snapshot_ref() noexcept;

/** @return Borrowed latest  fully consumed alignment snapshot, or an empty optional. */
[[nodiscard]] const std::optional<
	bioseq::postgres::table::alignment_execution::alignment_execution_snapshot>&
last_completed_alignment_snapshot_ref() noexcept;

/** @brief Validate a snapshot's recorded execution environment and return its backend. */
[[nodiscard]] bioseq::pipeline_stats::executed_backend
validate_alignment_snapshot_environment(
	const bioseq::postgres::table::alignment_execution::alignment_execution_snapshot& snapshot);
/** @return A 16-digit lowercase hexadecimal string, including leading zeros. */
[[nodiscard]] std::string fixed_hex_u64(std::uint64_t value);

/** @return The byte value unchanged for hexadecimal formatting. */
[[nodiscard]] constexpr std::uint8_t hex_byte_value(
		const std::uint8_t byte) noexcept
{
	return byte;
}

/** @return The numeric byte value used for hexadecimal formatting. */
[[nodiscard]] constexpr std::uint8_t hex_byte_value(
		const std::byte byte) noexcept
{
	return std::to_integer<std::uint8_t>(byte);
}

template<class p_byte, std::size_t p_extent>
[[nodiscard]] std::string fixed_hex_bytes(
		const std::array<p_byte, p_extent>& bytes)
{
	constexpr char hex_digits[] = "0123456789abcdef";
	std::string encoded(p_extent * 2, '0');
	for (std::size_t byte_index = 0; byte_index < p_extent; ++byte_index) {
		const std::uint8_t value = hex_byte_value(bytes[byte_index]);
		encoded[byte_index * 2] = hex_digits[(value >> 4U) & 0x0fU];
		encoded[byte_index * 2 + 1] = hex_digits[value & 0x0fU];
	}
	return encoded;
}

} // namespace bioseq::pg::detail
