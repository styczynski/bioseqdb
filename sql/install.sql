CREATE FUNCTION get_welcome_message(integer) RETURNS text
AS '$libdir/demopgextension'
LANGUAGE C IMMUTABLE STRICT;