\set ECHO none
\set QUIET 1
\pset format unaligned
\pset tuples_only on
\o /dev/null

SELECT bioseq_cache_test.provider_init();

DO $$
BEGIN
    IF NOT bioseq_cache_test.provider_registry_owns_copy() THEN
        RAISE EXCEPTION
            'provider registry retained provider-owned table or name storage';
    END IF;
END
$$;

SELECT bioseq_cache_test.provider_shutdown();

\o
SELECT 'cache_provider_registry: ok';
