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
	  SELECT bioseq_cache.reset_stats();
	  SELECT bioseq_cache_test.fail_next_write('enospc');});

is(
	$node->safe_psql(
		'postgres',
		q{SET bioseq.query_cache_optimisation_mode = 'same';
		  SELECT count(*) || '|' || sum(id) FROM file_rows}),
	'128|8256',
	'producer returns complete source result after ENOSPC');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM bioseq_cache_test.entry_states()
		  WHERE state = 'BUILDING'}),
	'0',
	'file failure leaves no BUILDING generation');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM bioseq_cache.entries()
		  WHERE state = 'COMPLETE' AND rows BETWEEN 1 AND 127}),
	'0',
	'no partial cache result is readable');
cmp_ok(
	$node->safe_psql('postgres', 'SELECT fallbacks FROM bioseq_cache.stats()'),
	'>=',
	1,
	'failure is visible in cache statistics');

$node->stop;
done_testing();
