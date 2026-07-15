use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;

my $node = new_cache_node('cache_two_phase_commit', preload => 1);
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE prepared_rows(id integer PRIMARY KEY, value text);
	  CREATE TABLE unrelated_rows(id integer PRIMARY KEY);
	  INSERT INTO prepared_rows VALUES (1, 'one'), (2, 'two');});

sub cached_counts
{
	return $node->safe_psql(
		'postgres',
		q{SET bioseq.query_cache_optimisation_mode = 'same';
		  SELECT count(*) FROM prepared_rows;
		  SELECT count(*) FROM prepared_rows;});
}

sub exact_stats
{
	return $node->safe_psql(
		'postgres',
		'SELECT produced || ' . q{'|'}
		  . ' || completed_hits FROM bioseq_cache.stats()');
}

$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
is(cached_counts(), "2\n2", 'baseline exact result is stable');
is(exact_stats(), '1|1', 'baseline exact result produces once and hits once');

$node->safe_psql(
	'postgres',
	q{BEGIN;
	  INSERT INTO prepared_rows VALUES (3, 'committed');
	  PREPARE TRANSACTION 'cache_commit';});

is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM prepared_rows'),
	'2',
	'uncommitted prepared write is not visible');

$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
$node->safe_psql('postgres', 'INSERT INTO unrelated_rows VALUES (1)');
is(
	cached_counts(),
	"2\n2",
	'intervening unrelated commit does not expose the prepared row');
is(
	exact_stats(),
	'0|0',
	'active prepared transaction dynamically gates exact production and hits');

$node->safe_psql(
	'postgres',
	q{BEGIN;
	  INSERT INTO prepared_rows VALUES (4, 'rolled-back');
	  PREPARE TRANSACTION 'cache_rollback';});

$node->safe_psql('postgres', "COMMIT PREPARED 'cache_commit'");
$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
is(
	cached_counts(),
	"3\n3",
	'committed prepared row is visible while another GID remains prepared');
is(
	exact_stats(),
	'0|0',
	'multiple-GID gate remains closed until the last prepared xact resolves');

$node->safe_psql('postgres', "ROLLBACK PREPARED 'cache_rollback'");
$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
is(
	cached_counts(),
	"3\n3",
	'ROLLBACK PREPARED does not expose the row');
is(
	exact_stats(),
	'1|1',
	'resolving the last GID reopens production and reuse');

my ($error_ret, $error_out, $error_err) = $node->psql(
	'postgres', "COMMIT PREPARED 'missing_cache_gid'");
isnt($error_ret, 0, 'missing COMMIT PREPARED reports its SQL error');
like($error_err, qr/does not exist/, 'prepared-resolution error is preserved');
is(
	$node->safe_psql(
		'postgres',
		'SELECT invalidations > 0 FROM bioseq_cache.stats()'),
	't',
	'ambiguous COMMIT PREPARED error invalidates conservatively');
$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
is(cached_counts(), "3\n3", 'error cleanup leaves the cache usable');
is(exact_stats(), '1|1', 'error cleanup releases its transition exactly once');

$node->safe_psql(
	'postgres',
	q{BEGIN;
	  INSERT INTO prepared_rows VALUES (5, 'restart commit');
	  PREPARE TRANSACTION 'cache_restart_commit';});
$node->stop;
$node->start;
$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
is(
	cached_counts(),
	"3\n3",
	'recovered prepared transaction gates reuse after postmaster restart');
is(exact_stats(), '0|0', 'restart reconstruction keeps the dynamic gate closed');
$node->safe_psql('postgres', "COMMIT PREPARED 'cache_restart_commit'");
$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
is(cached_counts(), "4\n4", 'COMMIT PREPARED after restart exposes its row');
is(
	exact_stats(),
	'1|1',
	'dense ProcArray membership drops the resolved dummy slot');

$node->safe_psql(
	'postgres',
	q{BEGIN;
	  INSERT INTO prepared_rows VALUES (6, 'restart rollback');
	  PREPARE TRANSACTION 'cache_restart_rollback';});
$node->stop;
$node->start;
$node->safe_psql('postgres', "ROLLBACK PREPARED 'cache_restart_rollback'");
$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
is(cached_counts(), "4\n4", 'ROLLBACK PREPARED after restart hides its row');
is(exact_stats(), '1|1', 'rollback resolution also removes the recovered gate');

is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_prepared_xacts
		  WHERE database = current_database()}),
	'0',
	'no prepared transaction remains after all resolution paths');

$node->stop;
done_testing();
