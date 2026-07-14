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
	  INSERT INTO prepared_rows VALUES (1, 'one'), (2, 'two');
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT * FROM prepared_rows ORDER BY id;
	  SELECT * FROM prepared_rows ORDER BY id;
	  BEGIN;
	  INSERT INTO prepared_rows VALUES (3, 'committed');
	  PREPARE TRANSACTION 'cache_commit';});

is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM prepared_rows'),
	'2',
	'uncommitted prepared write is not visible');
$node->safe_psql('postgres', "COMMIT PREPARED 'cache_commit'");
is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM prepared_rows'),
	'3',
	'COMMIT PREPARED makes the row visible and invalidates old cache');

$node->safe_psql(
	'postgres',
	q{SELECT * FROM prepared_rows ORDER BY id;
	  BEGIN;
	  INSERT INTO prepared_rows VALUES (4, 'rolled-back');
	  PREPARE TRANSACTION 'cache_rollback';});
$node->safe_psql('postgres', "ROLLBACK PREPARED 'cache_rollback'");
is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM prepared_rows'),
	'3',
	'ROLLBACK PREPARED does not expose the row');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT count(*) FROM pg_prepared_xacts
		  WHERE database = current_database()}),
	'0',
	'2PC owner ledger leaves no prepared transaction behind');

$node->stop;
done_testing();
