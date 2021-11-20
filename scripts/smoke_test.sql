-- Extension smoke test

DROP FUNCTION IF EXISTS "get_welcome_message";

CREATE FUNCTION get_welcome_message(integer) RETURNS text
AS 'demopgextension'
LANGUAGE C IMMUTABLE STRICT;

SELECT * FROM get_welcome_message(42);
