use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use PostgreSQL::Test::Utils qw(slurp_file);
use Test::More;

my $node = new_cache_node('cache_cross_database', preload => 1);
$node->start;
$node->safe_psql('postgres', 'CREATE DATABASE cache_db_a; CREATE DATABASE cache_db_b;');
bootstrap_cache($node, 'cache_db_a');
bootstrap_cache($node, 'cache_db_b');

for my $database ('cache_db_a', 'cache_db_b')
{
	$node->safe_psql(
		$database,
		q{CREATE TABLE same_oid_shape(id integer PRIMARY KEY, value text);
		  INSERT INTO same_oid_shape VALUES (1, current_database());
		  SET bioseq.query_cache_optimisation_mode = 'same';
		  SELECT * FROM same_oid_shape ORDER BY id;
		  SELECT * FROM same_oid_shape ORDER BY id;});
}

is(
	$node->safe_psql('cache_db_a', 'SELECT value FROM same_oid_shape'),
	'cache_db_a',
	'database A returns only its value');
is(
	$node->safe_psql('cache_db_b', 'SELECT value FROM same_oid_shape'),
	'cache_db_b',
	'database B never consumes database A entry');

my $domain = q{decode('444f4d41494e2d41', 'hex')};
my $provider_digest = q{decode(repeat('11', 32), 'hex')};
my $forced_digest = q{decode(repeat('aa', 32), 'hex')};
my $generation_a = $node->safe_psql(
	'cache_db_a',
	"SELECT bioseq_cache_test.inject_complete_coverage("
	  . "$domain, decode('00','hex'), $provider_digest, $forced_digest)");

$node->safe_psql(
	'cache_db_b',
	"SELECT bioseq_cache_test.inject_complete_coverage("
	  . "$domain, decode(lpad(to_hex(i), 4, '0'), 'hex'), "
	  . "$provider_digest, $forced_digest) FROM generate_series(1, 65) AS g(i)");

is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT generation FROM bioseq_cache_test.lookup_injected_coverage($domain) "
		  . "WHERE generation = $generation_a"),
	$generation_a,
	'database B crowding cannot consume database A candidate budget');
is(
	$node->safe_psql(
		'cache_db_a',
		q{SELECT bool_and(database_oid = (SELECT oid FROM pg_database
		     WHERE datname = current_database()))
		  FROM bioseq_cache_test.coverage_bucket_states()}),
	't',
	'coverage diagnostics expose only the current database namespace');

$node->safe_psql('postgres', 'DROP DATABASE cache_db_b');
is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT count(*) FROM bioseq_cache_test.lookup_injected_coverage($domain) "
		  . "WHERE generation = $generation_a"),
	'1',
	'dropping another database leaves current namespace intact');

$node->stop;
done_testing();
