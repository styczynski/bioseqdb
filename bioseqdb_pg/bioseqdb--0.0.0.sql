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

CREATE FUNCTION nuclseq_search_bwa(query_sequence NUCLSEQ, reference_sql CSTRING)
    RETURNS SETOF bwa_result
    AS 'MODULE_PATHNAME'
    LANGUAGE C STABLE STRICT;

CREATE FUNCTION nuclseq_multi_search_bwa(query_sql CSTRING, reference_sql CSTRING)
    RETURNS SETOF bwa_result
    AS 'MODULE_PATHNAME'
    LANGUAGE C STABLE STRICT;
