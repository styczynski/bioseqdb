CREATE FUNCTION get_welcome_message(integer) RETURNS text
AS '$libdir/bioseq'
LANGUAGE C IMMUTABLE STRICT;