-- pgtam--0.0.1.sql

-- Create the SQL function
CREATE OR REPLACE FUNCTION mem_tableam_handler(internal)
RETURNS table_am_handler AS 'pgtam', 'mem_tableam_handler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD mem TYPE TABLE HANDLER mem_tableam_handler;

-- TUTAJ
CREATE FUNCTION mmsearch(el ANYELEMENT, query TEXT)
    RETURNS BOOLEAN
    AS 'MODULE_PATHNAME'
    LANGUAGE C STABLE STRICT;

-- CREATE OPERATOR pg_catalog.@@@ (
--     PROCEDURE = search_with_query_input,
--     LEFTARG = anyelement,
--     RIGHTARG = text
-- );

-- CREATE OPERATOR CLASS anyelement_mem_ops DEFAULT FOR TYPE anyelement USING mem AS
--     OPERATOR 1 pg_catalog.@@@(anyelement, text),                         /* for querying with a tantivy-compatible text query */
--     STORAGE anyelement;

-- CREATE FUNCTION mem_score()
--     RETURNS INTEGER
--     AS 'MODULE_PATHNAME'
--     LANGUAGE C STABLE STRICT;