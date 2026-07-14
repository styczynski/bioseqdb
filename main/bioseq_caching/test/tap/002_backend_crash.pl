use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;

my $node = new_cache_node('cache_backend_crash', preload => 1);
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE crash_rows(id integer PRIMARY KEY, value text);
	  INSERT INTO crash_rows
	  SELECT i, md5(i::text) FROM generate_series(1, 100) AS g(i);
	  SELECT bioseq_cache.reset_stats();
	  SELECT bioseq_cache_test.arm_pause('after_building_publish');});

my $producer = $node->background_psql('postgres');
$producer->query_until(
	qr/cache-producer-start/,
	q{\echo cache-producer-start
	  SET application_name = 'cache_crash_producer';
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT count(*), sum(id) FROM crash_rows WHERE id <= 10;
});

ok(
	$node->poll_query_until(
		'postgres',
		q{SELECT EXISTS (
		    SELECT 1 FROM bioseq_cache_test.entry_states()
		    WHERE state = 'BUILDING')},
		't'),
	'producer reached BUILDING pause');

my $waiter = $node->background_psql('postgres');
$waiter->query_until(
	qr/cache-waiter-start/,
	q{\echo cache-waiter-start
	  SET application_name = 'cache_crash_waiter';
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT count(*), sum(id) FROM crash_rows WHERE id <= 10;
});

ok(
	$node->poll_query_until(
		'postgres',
		q{SELECT EXISTS (
		    SELECT 1 FROM bioseq_cache_test.entry_states()
		    WHERE waiter_count > 0)},
		't'),
	'waiter registered against producer generation');

my $producer_pid = $node->safe_psql(
	'postgres',
	q{SELECT producer_pid
	  FROM bioseq_cache_test.entry_states()
	  WHERE state = 'BUILDING'
	  ORDER BY generation LIMIT 1});
is(
	$node->safe_psql('postgres', "SELECT pg_terminate_backend($producer_pid)"),
	't',
	'producer backend terminated cleanly');

my $waiter_output = $waiter->query('');
like($waiter_output, qr/10\|55/, 'waiter safely retries and returns correct rows');

eval { $producer->quit; };
$waiter->quit;

is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM bioseq_cache_test.entry_states()
		  WHERE state = 'BUILDING'}),
	'0',
	'backend termination leaves no BUILDING generation');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM crash_rows WHERE id <= 10}),
	'10',
	'source remains readable after cleanup');

$node->stop;
done_testing();
