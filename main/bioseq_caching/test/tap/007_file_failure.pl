use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;

my $node = new_cache_node('cache_file_failure', preload => 1);
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE file_rows(id integer PRIMARY KEY, payload text);
	  INSERT INTO file_rows
	  SELECT i, repeat(md5(i::text), 1024)
	  FROM generate_series(1, 128) AS g(i);
	  SELECT bioseq_cache.reset_stats();});

my $controller = $node->background_psql('postgres');
$controller->query_until(
	qr/file-failure-controller-armed/,
	q{SELECT bioseq_cache_test.arm_pause('after_building_publish');
	  \echo file-failure-controller-armed
});

my $producer = $node->background_psql('postgres');
$producer->query_until(
	qr/file-failure-producer-start/,
	q{\echo file-failure-producer-start
	  SET application_name = 'cache_file_failure_producer';
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT id FROM file_rows WHERE id <= 10 ORDER BY id;
});
ok(
	$node->poll_query_until(
		'postgres',
		q{SELECT EXISTS (
		    SELECT 1 FROM bioseq_cache_test.entry_states()
		    WHERE state = 'BUILDING')},
		't'),
	'producer reaches BUILDING before the injected write failure');

my ($failed_key, $failed_generation, $failed_claim_sequence) = split(
	/\|/,
	$node->safe_psql(
	'postgres',
	q{SELECT entry_key || '|' || generation || '|' || claim_sequence
	  FROM bioseq_cache_test.entry_states()
	  WHERE state = 'BUILDING' ORDER BY generation LIMIT 1}));

my $waiter = $node->background_psql('postgres');
$waiter->query_until(
	qr/file-failure-waiter-start/,
	q{\echo file-failure-waiter-start
	  SET application_name = 'cache_file_failure_waiter';
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT id FROM file_rows WHERE id <= 10 ORDER BY id;
});
ok(
	$node->poll_query_until(
		'postgres',
		q{SELECT EXISTS (
		    SELECT 1 FROM bioseq_cache_test.entry_states()
		    WHERE state = 'BUILDING' AND waiter_count = 1)},
		't'),
	'waiter is sleeping on the producer generation before failure');

$node->safe_psql(
	'postgres', "SELECT bioseq_cache_test.fail_next_write('enospc')");
$controller->query(
	q{SELECT bioseq_cache_test.release_pause('after_building_publish');});
$controller->quit;

my $producer_output = $producer->query('');
my @producer_rows = grep { /^\d+$/ } split(/\r?\n/, $producer_output);
is_deeply(
	\@producer_rows,
	[ 1 .. 10 ],
	'producer returns the complete source result after ENOSPC');
$producer->quit;

my $waiter_output = $waiter->query('');
my @waiter_rows = grep { /^\d+$/ } split(/\r?\n/, $waiter_output);
is_deeply(
	\@waiter_rows,
	[ 1 .. 10 ],
	'woken waiter safely retries and returns the same complete rows');
$waiter->quit;

is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM bioseq_cache_test.entry_states()
		  WHERE state = 'BUILDING'}),
	'0',
	'file failure wakes waiters and leaves no BUILDING generation');
is(
	$node->safe_psql(
		'postgres',
		"SELECT state FROM bioseq_cache_test.entry_states() "
		  . "WHERE entry_key = '$failed_key' "
		  . "AND generation = $failed_generation "
		  . "AND claim_sequence = $failed_claim_sequence"),
	'FAILED',
	'failed producer token is terminal and never readable as a partial result');
cmp_ok(
	$node->safe_psql('postgres', 'SELECT inflight_waits FROM bioseq_cache.stats()'),
	'>=',
	1,
	'waiter registration is visible in cache statistics');
cmp_ok(
	$node->safe_psql('postgres', 'SELECT fallbacks FROM bioseq_cache.stats()'),
	'>=',
	1,
	'failure is visible in cache statistics');

$node->stop;
done_testing();
