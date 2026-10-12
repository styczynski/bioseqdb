\echo Use "CREATE EXTENSION traceheap" to load this file. \quit

CREATE SCHEMA traceheap;

CREATE FUNCTION traceheap.handler(internal)
RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'traceheap_handler'
LANGUAGE C STRICT;

CREATE ACCESS METHOD traceheap TYPE TABLE HANDLER traceheap.handler;

CREATE FUNCTION traceheap.stats()
RETURNS jsonb
AS 'MODULE_PATHNAME', 'traceheap_stats'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

CREATE FUNCTION traceheap.reset()
RETURNS void
AS 'MODULE_PATHNAME', 'traceheap_reset'
LANGUAGE C VOLATILE PARALLEL UNSAFE;

COMMENT ON ACCESS METHOD traceheap IS
  'Heap storage with selected backend-local callback counters; educational only';
