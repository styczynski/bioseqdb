use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;

my $node = new_cache_node('cache_owner_liveness', preload => 1);
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE owner_rows(id integer PRIMARY KEY, value text);
	  INSERT INTO owner_rows
	  SELECT i, md5(i::text) FROM generate_series(1, 64) AS g(i);});

my $controller = $node->background_psql('postgres');
$controller->query_until(
	qr/owner-controller-armed/,
	q{SELECT bioseq_cache_test.arm_pause('after_building_publish');
	  \echo owner-controller-armed
});

my $owner = $node->background_psql('postgres');
$owner->query_until(
	qr/owner-start/,
	q{\echo owner-start
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT count(*), sum(id) FROM owner_rows;
});
ok(
	$node->poll_query_until(
		'postgres',
		q{SELECT EXISTS (SELECT 1 FROM bioseq_cache_test.entry_states()
		  WHERE state = 'BUILDING')},
		't'),
	'owner token is visible while producer is live');

my $owner_pid = $node->safe_psql(
	'postgres',
	q{SELECT producer_pid FROM bioseq_cache_test.entry_states()
	  WHERE state = 'BUILDING' ORDER BY generation LIMIT 1});
$node->safe_psql('postgres', "SELECT pg_terminate_backend($owner_pid)");
eval { $owner->quit; };
$controller->query(
	q{SELECT bioseq_cache_test.release_pause('after_building_publish');});
$controller->quit;

ok(
	$node->poll_query_until(
		'postgres',
		q{SELECT NOT EXISTS (SELECT 1 FROM bioseq_cache_test.entry_states()
		  WHERE state = 'BUILDING')},
		't'),
	'dead full owner token is reclaimed');
is(
	$node->safe_psql(
		'postgres',
		q{SET bioseq.query_cache_optimisation_mode = 'same';
		  SELECT count(*) || '|' || sum(id) FROM owner_rows}),
	'64|2080',
	'fresh producer succeeds after owner cleanup');

$node->stop;
done_testing();
