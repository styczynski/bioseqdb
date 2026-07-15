use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use PostgreSQL::Test::Utils qw(slurp_file);
use Test::More;

my $node = new_cache_node('cache_cross_database', preload => 1);
$node->start;
$node->safe_psql(
	'postgres',
	q{CREATE DATABASE cache_db_a;
	  CREATE DATABASE cache_db_b;
	  CREATE ROLE cache_namespace_b SUPERUSER LOGIN;});
bootstrap_cache($node, 'cache_db_a');
bootstrap_cache($node, 'cache_db_b');
my $primary_role_oid = $node->safe_psql(
	'cache_db_a', 'SELECT current_user::regrole::oid');

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

my ($role_before_produced, $role_before_hits) = split(
	/\|/,
	$node->safe_psql(
		'cache_db_a',
		q{SELECT produced || '|' || completed_hits
		  FROM bioseq_cache.stats()}));
is(
	$node->safe_psql(
		'cache_db_a',
		q{SET bioseq.query_cache_optimisation_mode = 'same';
		  SELECT * FROM same_oid_shape ORDER BY id;
		  SELECT * FROM same_oid_shape ORDER BY id;},
		extra_params => [ '-U', 'cache_namespace_b' ]),
	"1|cache_db_a\n1|cache_db_a",
	'second role receives the correct database-A rows twice');
my ($role_after_produced, $role_after_hits) = split(
	/\|/,
	$node->safe_psql(
		'cache_db_a',
		q{SELECT produced || '|' || completed_hits
		  FROM bioseq_cache.stats()}));
is(
	($role_after_produced - $role_before_produced) . '|'
	  . ($role_after_hits - $role_before_hits),
	'1|1',
	'second role owns a separate exact producer and subsequent hit');

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

my $role_generations_output = $node->safe_psql(
	'cache_db_a',
	"SELECT bioseq_cache_test.inject_complete_coverage("
	  . "$domain, decode(lpad(to_hex(i), 4, '0'), 'hex'), "
	  . "$provider_digest, $forced_digest) FROM generate_series(101, 165) AS g(i)",
	extra_params => [ '-U', 'cache_namespace_b' ]);
my @role_generations = split(/\r?\n/, $role_generations_output);
is(
	scalar(@role_generations),
	65,
	'second database-A role crowds the same forced digest with 65 generations');
is(
	$node->safe_psql(
		'cache_db_a',
		q{SELECT count(*) FROM bioseq_cache_test.coverage_bucket_states()
		  WHERE current_role_oid = 'cache_namespace_b'::regrole}),
	'65',
	'every crowded generation retains its second-role directory identity');
is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT count(*) FROM bioseq_cache_test.coverage_bucket_states() "
		  . "WHERE current_role_oid = $primary_role_oid"),
	'1',
	'role crowding leaves the original role-A generation intact');

my $foreign_generation = $role_generations[0];
$node->safe_psql(
	'cache_db_a',
	"SELECT bioseq_cache_test.mark_generation_evictable($foreign_generation); "
	  . 'SELECT bioseq_cache_test.request_janitor_sweep();');
is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT bioseq_cache_test.wait_for_generation_absent("
		  . "$foreign_generation, interval '10 seconds')"),
	't',
	'janitor removes the selected second-role generation');
is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT count(*) FROM bioseq_cache_test.lookup_injected_coverage($domain) "
		  . "WHERE generation = $generation_a"),
	'1',
	'generation-owned janitor cleanup does not unlink role A');
is(
	$node->safe_psql(
		'cache_db_a',
		q{SELECT count(*) FROM bioseq_cache_test.coverage_bucket_states()
		  WHERE current_role_oid = 'cache_namespace_b'::regrole}),
	'64',
	'janitor tombstones only the selected second-role generation');

$node->safe_psql('postgres', 'DROP DATABASE cache_db_b');
is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT count(*) FROM bioseq_cache_test.lookup_injected_coverage($domain) "
		  . "WHERE generation = $generation_a"),
	'1',
	'dropping database B leaves database-A role-A coverage intact');
is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT count(*) FROM bioseq_cache_test.lookup_injected_coverage($domain) "
		  . "WHERE generation = $role_generations[1]",
			extra_params => [ '-U', 'cache_namespace_b' ]),
	'1',
	'dropping database B leaves database-A role-B coverage intact');

$node->safe_psql(
	'cache_db_a',
	"SELECT bioseq_cache_test.mark_generation_evictable($generation_a); "
	  . 'SELECT bioseq_cache_test.request_janitor_sweep();',
	extra_params => [ '-U', 'cache_namespace_b' ]);
is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT bioseq_cache_test.wait_for_generation_absent("
		  . "$generation_a, interval '10 seconds')"),
	't',
	'reverse-identity janitor request removes the stored role-A generation');
is(
	$node->safe_psql(
		'cache_db_a',
		"SELECT count(*) FROM bioseq_cache_test.lookup_injected_coverage($domain) "
		  . "WHERE generation = $role_generations[1]",
		extra_params => [ '-U', 'cache_namespace_b' ]),
	'1',
	'reverse cleanup leaves an unrelated second-role generation intact');

$node->stop;
done_testing();
