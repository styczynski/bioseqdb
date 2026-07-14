use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;

my $no_preload = new_cache_node('cache_no_preload', preload => 0);
$no_preload->start;

my ($ret, $out, $err) = $no_preload->psql(
	'postgres',
	q{LOAD 'bioseq_cache';
	  SET bioseq.query_cache_optimisation_mode = 'same';});
isnt($ret, 0, 'same mode is rejected without preload shared memory');
like(
	$err,
	qr/Add bioseq_cache to shared_preload_libraries and restart PostgreSQL/,
	'error includes the required restart hint');
$no_preload->stop;

my $preloaded = new_cache_node(
	'cache_preloaded_mode',
	preload => 1,
	mode => 'same');
$preloaded->start;
bootstrap_cache($preloaded);

is(
	$preloaded->safe_psql(
		'postgres', 'SHOW bioseq.query_cache_optimisation_mode'),
	'same',
	'custom-GUC placeholder replay preserves startup mode');
isnt(
	$preloaded->safe_psql('postgres', 'SELECT bioseq_cache.version()'),
	'',
	'build-tree bootstrap exposes version');
is(
	$preloaded->safe_psql(
		'postgres',
		q{SELECT has_schema_privilege('public', 'bioseq_cache', 'CREATE')}),
	'f',
	'fixed admin schema is not publicly writable');
is(
	$preloaded->safe_psql(
		'postgres',
		q{SELECT count(*)
		  FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace
		  WHERE n.nspname = 'bioseq_cache' AND c.relkind IN ('r','p')}),
	'0',
	'production schema contains no persistent metadata table');

$preloaded->safe_psql(
	'postgres',
	q{CREATE ROLE cache_monitor_granted;
	  CREATE ROLE cache_monitor_execute_only;
	  GRANT EXECUTE ON FUNCTION bioseq_cache.entries()
	    TO cache_monitor_granted, cache_monitor_execute_only;
	  GRANT USAGE ON SCHEMA bioseq_cache TO cache_monitor_granted;
	  CREATE VIEW public.cache_entries_count
	    WITH (security_invoker = true)
	    AS SELECT count(*) AS entry_count FROM bioseq_cache.entries();
	  GRANT SELECT ON public.cache_entries_count
	    TO cache_monitor_granted, cache_monitor_execute_only;});

is(
	$preloaded->safe_psql(
		'postgres',
		q{SET ROLE cache_monitor_granted;
		  SELECT entry_count FROM public.cache_entries_count;}),
	'0',
	'explicit schema and function grants pass the C diagnostics boundary');

my ($acl_ret, $acl_out, $acl_err) = $preloaded->psql(
	'postgres',
	q{SET ROLE cache_monitor_execute_only;
	  SELECT entry_count FROM public.cache_entries_count;});
isnt(
	$acl_ret,
	0,
	'stored function OID cannot bypass the C-side schema privilege check');
like(
	$acl_err,
	qr/permission denied to inspect bioseq_cache/,
	'C-side denial is distinct from parser name-resolution denial');
like(
	$acl_err,
	qr/Grant USAGE on schema bioseq_cache and EXECUTE/,
	'C-side denial explains the documented two-part grant policy');

$preloaded->stop;
done_testing();
