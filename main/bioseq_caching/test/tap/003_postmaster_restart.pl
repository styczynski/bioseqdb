use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;

my $node = new_cache_node('cache_postmaster_restart', preload => 1);
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE restart_rows(id integer PRIMARY KEY, value text);
	  INSERT INTO restart_rows VALUES (1, 'one'), (2, 'two');
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT * FROM restart_rows ORDER BY id;
	  SELECT * FROM restart_rows ORDER BY id;});

cmp_ok(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM bioseq_cache.entries() WHERE state = 'COMPLETE'}),
	'>=',
	1,
	'completed entry exists before restart');

$node->restart;

is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM bioseq_cache.entries()'),
	'0',
	'boot-scoped registry is empty after restart');
$node->safe_psql(
	'postgres',
	q{SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT * FROM restart_rows ORDER BY id;});
is(
	$node->safe_psql(
		'postgres', 'SELECT produced FROM bioseq_cache.stats()'),
	'1',
	'first post-restart execution is a miss/producer');

$node->stop;
done_testing();
