use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;
use Time::HiRes qw(sleep time);

sub storage_snapshot
{
	my ($node) = @_;
	my $explain = $node->safe_psql(
		'postgres',
		q{SET bioseq.query_cache_optimisation_mode = 'same';
		  EXPLAIN (COSTS OFF)
		  SELECT id FROM entitlement_rows WHERE id <= 100 ORDER BY id;});
	my ($storage_diagnostic) =
	  $explain =~ /Cache Storage: ([^\n]+)/;
	die "Cache Storage diagnostics missing from EXPLAIN:\n$explain\n"
	  unless defined($storage_diagnostic);
	my ($hot_bytes) = $storage_diagnostic =~ /\bhot=(\d+)\/\d+/;
	my ($file_bytes) = $storage_diagnostic =~ /\bfile=(\d+)\/\d+/;
	my ($manifest_count) = $storage_diagnostic =~ /\bmanifests=(\d+)/;
	my ($waiter_count) = $storage_diagnostic =~ /\bwaiters=(\d+)/;
	die "Cache Storage diagnostics missing from EXPLAIN:\n$explain\n"
	  unless defined($hot_bytes)
	  && defined($file_bytes)
	  && defined($manifest_count)
	  && defined($waiter_count);
	return ($hot_bytes, $file_bytes, $manifest_count, $waiter_count);
}

my $node = new_cache_node('cache_waiter_entitlement_cleanup', preload => 1);
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE entitlement_rows(id integer PRIMARY KEY, value text);
	  INSERT INTO entitlement_rows
	  SELECT i, md5(i::text) FROM generate_series(1, 100) AS g(i);
	  SELECT bioseq_cache.reset_stats();});

my ($baseline_hot, $baseline_file, $baseline_manifests, $baseline_waiters) =
  storage_snapshot($node);

my $controller = $node->background_psql('postgres');
$controller->query_until(
	qr/entitlement-controller-armed/,
	q{SELECT bioseq_cache_test.arm_pause('before_terminal_publish');
	  SELECT bioseq_cache_test.arm_pause('after_waiter_registered');
	  \echo entitlement-controller-armed
});

my $producer = $node->background_psql('postgres');
$producer->query_until(
	qr/entitlement-producer-start/,
	q{\echo entitlement-producer-start
	  SET application_name = 'cache_entitlement_producer';
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SET bioseq.query_cache_ttl = 0;
	  SELECT count(*) || '|' || sum(id) FROM entitlement_rows;
});
$node->safe_psql(
	'postgres',
	q{SELECT bioseq_cache_test.wait_until_paused(
	    'before_terminal_publish')});
pass('producer reached terminal-publication pause');

my $waiter = $node->background_psql('postgres');
$waiter->query_until(
	qr/entitlement-waiter-start/,
	q{\echo entitlement-waiter-start
	  SET application_name = 'cache_entitlement_waiter';
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SET bioseq.query_cache_ttl = 0;
	  SELECT count(*) || '|' || sum(id) FROM entitlement_rows;
});
$node->safe_psql(
	'postgres',
	q{SELECT bioseq_cache_test.wait_until_paused(
	    'after_waiter_registered')});
pass('waiter registered before publication');

$controller->query(
	q{SELECT bioseq_cache_test.release_pause('before_terminal_publish');});
my $producer_output = $producer->query('');
like($producer_output, qr/100\|5050/, 'producer published the complete result');
$producer->quit;

ok(
	$node->poll_query_until(
		'postgres',
		q{SELECT count(*) = 1
		  FROM bioseq_cache_test.entry_states()
		  WHERE state = 'COMPLETE'
		    AND waiter_count = 1
		    AND reader_count = 0},
		't'),
	'published generation retains the paused waiter entitlement');

my ($entitled_hot, $entitled_file, $entitled_manifests, $entitled_waiters) =
  storage_snapshot($node);
is(
	$entitled_manifests,
	$baseline_manifests + 1,
	'waiter entitlement is attached to a published storage manifest');
is(
	$entitled_waiters,
	$baseline_waiters + 1,
	'published storage manifest owns the paused waiter token');
cmp_ok(
	$entitled_hot + $entitled_file,
	'>',
	$baseline_hot + $baseline_file,
	'published manifest owns admitted storage');

$node->safe_psql(
	'postgres', 'SELECT bioseq_cache_test.request_janitor_sweep()');
sleep(1.2);
my @pinned_snapshot = storage_snapshot($node);
is(
	$pinned_snapshot[2],
	$entitled_manifests,
	'live waiter entitlement prevents premature TTL-zero reclamation');
is(
	$pinned_snapshot[3],
	$entitled_waiters,
	'aggressive sweep preserves the live owner-bound waiter token');

my $waiter_pid = $node->safe_psql(
	'postgres',
	q{SELECT pid FROM pg_stat_activity
	  WHERE application_name = 'cache_entitlement_waiter'});
like($waiter_pid, qr/^\d+$/, 'paused waiter backend is identifiable');
is(
	$node->safe_psql(
		'postgres', "SELECT pg_terminate_backend($waiter_pid)"),
	't',
	'waiter is terminated after manifest entitlement publication');

ok(
	$node->poll_query_until(
		'postgres',
		"SELECT NOT EXISTS (SELECT 1 FROM pg_stat_activity "
		  . "WHERE pid = $waiter_pid) "
		  . "AND NOT EXISTS (SELECT 1 "
		  . "FROM bioseq_cache_test.entry_states() "
		  . "WHERE waiter_count <> 0)",
		't'),
	'terminated waiter drains its registry reference');
eval { $waiter->quit; };

$controller->query(
	q{SELECT bioseq_cache_test.release_pause('after_waiter_registered');});
$controller->quit;

my $deadline = time() + 10;
my ($reclaimed_hot, $reclaimed_file, $reclaimed_manifests,
	$reclaimed_waiters);
do
{
	$node->safe_psql(
		'postgres', 'SELECT bioseq_cache_test.request_janitor_sweep()');
	($reclaimed_hot, $reclaimed_file, $reclaimed_manifests,
		$reclaimed_waiters) =
	  storage_snapshot($node);
	last
	  if $reclaimed_hot == $baseline_hot
	  && $reclaimed_file == $baseline_file
	  && $reclaimed_manifests == $baseline_manifests
	  && $reclaimed_waiters == $baseline_waiters;
	sleep(0.05);
} while (time() < $deadline);

is(
	$reclaimed_manifests,
	$baseline_manifests,
	'janitor reclaims the terminated waiter manifest');
is($reclaimed_hot, $baseline_hot, 'janitor releases hot-tier reservation');
is($reclaimed_file, $baseline_file, 'janitor releases file-tier reservation');
is(
	$reclaimed_waiters,
	$baseline_waiters,
	'janitor leaves no owner-bound waiter token');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM bioseq_cache_test.entry_states()
		  WHERE waiter_count <> 0 OR reader_count <> 0}),
	'0',
	'no retained reference blocks later reclamation');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) || '|' || sum(id) FROM entitlement_rows}),
	'100|5050',
	'cluster remains usable after waiter termination and reclamation');

$node->stop;
done_testing();
