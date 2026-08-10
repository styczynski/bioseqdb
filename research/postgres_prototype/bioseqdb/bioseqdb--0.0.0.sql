CREATE TYPE NUCLSEQ;

CREATE FUNCTION nuclseq_in(CSTRING)
    RETURNS NUCLSEQ
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_out(NUCLSEQ)
    RETURNS CSTRING
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE nuclseq (
    internallength = VARIABLE,
    storage = EXTENDED,
	alignment = double,
    input = nuclseq_in,
    output = nuclseq_out
);

CREATE FUNCTION nuclseq_eq(NUCLSEQ, NUCLSEQ)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_ne(NUCLSEQ, NUCLSEQ)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_lt(NUCLSEQ, NUCLSEQ)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_le(NUCLSEQ, NUCLSEQ)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_gt(NUCLSEQ, NUCLSEQ)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_ge(NUCLSEQ, NUCLSEQ)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_cmp(NUCLSEQ, NUCLSEQ)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE OPERATOR = (
    LEFTARG = NUCLSEQ,
    RIGHTARG = NUCLSEQ,
    PROCEDURE = nuclseq_eq,
    COMMUTATOR = '=',
    NEGATOR = '<>',
    RESTRICT = eqsel,
    JOIN = eqjoinsel,
    HASHES, MERGES
);

CREATE OPERATOR <> (
    LEFTARG = NUCLSEQ,
    RIGHTARG = NUCLSEQ,
    PROCEDURE = nuclseq_ne,
    COMMUTATOR = '<>',
    NEGATOR = '=',
    RESTRICT = neqsel,
    JOIN = neqjoinsel
);

CREATE OPERATOR < (
    LEFTARG = NUCLSEQ,
    RIGHTARG = NUCLSEQ,
    PROCEDURE = nuclseq_lt,
    COMMUTATOR = >,
    NEGATOR = >=,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel
);

CREATE OPERATOR <= (
    LEFTARG = NUCLSEQ,
    RIGHTARG = NUCLSEQ,
    PROCEDURE = nuclseq_le,
    COMMUTATOR = >=,
    NEGATOR = >,
    RESTRICT = scalarltsel,
    JOIN = scalarltjoinsel
);

CREATE OPERATOR > (
    LEFTARG = NUCLSEQ,
    RIGHTARG = NUCLSEQ,
    PROCEDURE = nuclseq_gt,
    COMMUTATOR = <,
    NEGATOR = <=,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel
);

CREATE OPERATOR >= (
    LEFTARG = NUCLSEQ,
    RIGHTARG = NUCLSEQ,
    PROCEDURE = nuclseq_ge,
    COMMUTATOR = <=,
    NEGATOR = <,
    RESTRICT = scalargtsel,
    JOIN = scalargtjoinsel
);

CREATE OPERATOR CLASS nuclseq_btree_operators
    DEFAULT FOR TYPE NUCLSEQ
    USING btree
    AS
        OPERATOR 1 <,
        OPERATOR 2 <=,
        OPERATOR 3 =,
        OPERATOR 4 >=,
        OPERATOR 5 >,
        FUNCTION 1 nuclseq_cmp(NUCLSEQ, NUCLSEQ);

CREATE FUNCTION nuclseq_hash(NUCLSEQ)
    RETURNS INTEGER
    AS 'hashvarlena'
    LANGUAGE INTERNAL IMMUTABLE;

CREATE OPERATOR CLASS nuclseq_hash_operators
    DEFAULT FOR TYPE NUCLSEQ
    USING hash
    AS
        OPERATOR 1 =,
        FUNCTION 1 nuclseq_hash(NUCLSEQ);

CREATE FUNCTION nuclseq_len(NUCLSEQ)
    RETURNS INTEGER
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_content(NUCLSEQ, CSTRING)
    RETURNS DOUBLE PRECISION
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_complement(NUCLSEQ)
    RETURNS NUCLSEQ
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE FUNCTION nuclseq_reverse(NUCLSEQ)
    RETURNS NUCLSEQ
    AS 'MODULE_PATHNAME'
    LANGUAGE C IMMUTABLE STRICT;

CREATE TYPE bwa_options AS (
	min_seed_len INTEGER,
	max_occ INTEGER,
	match_score INTEGER,
	mismatch_penalty INTEGER,
	pen_clip3 INTEGER,
	pen_clip5 INTEGER,
	zdrop INTEGER,
	bandwidth INTEGER,
	o_del INTEGER,
	e_del INTEGER,
	o_ins INTEGER,
	e_ins INTEGER
);

CREATE FUNCTION bwa_opts(
	min_seed_len INTEGER DEFAULT 19,
	max_occ INTEGER DEFAULT NULL,
	match_score INTEGER DEFAULT 1,
	mismatch_penalty INTEGER DEFAULT 4,
	pen_clip3 INTEGER DEFAULT 5,
	pen_clip5 INTEGER DEFAULT 5,
	zdrop INTEGER DEFAULT 100,
	bandwidth INTEGER DEFAULT 100,
	o_del INTEGER DEFAULT 6,
	o_ins INTEGER DEFAULT 6,
	e_del INTEGER DEFAULT 1,
	e_ins INTEGER DEFAULT 1
) RETURNS bwa_options AS $$ 
	SELECT ROW(
		min_seed_len, max_occ, match_score, mismatch_penalty,
		pen_clip3, pen_clip5, zdrop, bandwidth,
		o_del, o_ins, e_del, e_ins
	) as opts
$$ LANGUAGE SQL IMMUTABLE;

CREATE TYPE bwa_result AS (
    ref_id BIGINT,
    ref_subseq NUCLSEQ,
    ref_match_start INTEGER,
    ref_match_end INTEGER,
    ref_match_len INTEGER,
    query_id BIGINT,
    query_subseq NUCLSEQ,
    query_match_start INTEGER,
    query_match_end INTEGER,
    query_match_len INTEGER,
    is_primary BOOLEAN,
    is_secondary BOOLEAN,
    is_reverse BOOLEAN,
    cigar TEXT,
    score INTEGER
);

CREATE FUNCTION nuclseq_search_bwa(query_sequence NUCLSEQ, reference_sql CSTRING, opts bwa_options DEFAULT bwa_opts())
    RETURNS SETOF bwa_result
    AS 'MODULE_PATHNAME'
    LANGUAGE C STABLE STRICT;

CREATE FUNCTION nuclseq_multi_search_bwa(query_sql CSTRING, reference_sql CSTRING, opts bwa_options DEFAULT bwa_opts())
    RETURNS SETOF bwa_result
    AS 'MODULE_PATHNAME'
    LANGUAGE C STABLE STRICT;
