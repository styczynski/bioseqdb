use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;
use Time::HiRes qw(sleep time);

my $node = new_cache_node('cache_ttl_and_file_tier', preload => 1);
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE tier_rows(id integer PRIMARY KEY, payload text NOT NULL);
	  INSERT INTO tier_rows
	  SELECT i, repeat(md5(i::text), 1024)
	  FROM generate_series(1, 64) AS g(i);
	  SELECT bioseq_cache.reset_stats();
	  SELECT bioseq_cache_test.set_force_file_tier(true);});

my $tier_query = q{SET bioseq.query_cache_optimisation_mode = 'same';
	SELECT count(*), sum(id) FROM tier_rows WHERE id <= 64};
is(
	$node->safe_psql('postgres', $tier_query),
	'64|2080',
	'forced file-tier producer returns the complete source result');
is(
	$node->safe_psql('postgres', $tier_query),
	'64|2080',
	'forced file-tier manifest replays the same nonempty result');

my $tier_stats = $node->safe_psql(
	'postgres',
	q{SELECT force_file_tier, dsa_blocks, file_blocks,
	         reserved_hot_bytes, reserved_file_bytes
	  FROM bioseq_cache_test.storage_tier_stats()});
my ($forced, $dsa_blocks, $file_blocks, $hot_bytes, $file_bytes) =
	split(/\|/, $tier_stats);
is($forced, 't', 'test-only shared override is observable');
cmp_ok($file_blocks, '>', 0, 'live ledger contains file-backed blocks');
cmp_ok($file_bytes, '>', 0, 'file-tier admission bytes are reserved');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT produced, completed_hits FROM bioseq_cache.stats()}),
	'1|1',
	'one publication and one completed hit use the forced file tier');

$node->safe_psql(
	'postgres',
	q{SELECT bioseq_cache_test.set_force_file_tier(false);
	  SELECT bioseq_cache.invalidate_all();
	  SELECT bioseq_cache_test.request_janitor_sweep();});
my $cleanup_deadline = time() + 10;
my $remaining_file_blocks = $file_blocks;
my $remaining_file_bytes = $file_bytes;
while (time() < $cleanup_deadline)
{
	my $snapshot = $node->safe_psql(
		'postgres',
		q{SELECT file_blocks, reserved_file_bytes
		  FROM bioseq_cache_test.storage_tier_stats()});
	($remaining_file_blocks, $remaining_file_bytes) = split(/\|/, $snapshot);
	last if $remaining_file_blocks == 0 && $remaining_file_bytes == 0;
	$node->safe_psql(
		'postgres', 'SELECT bioseq_cache_test.request_janitor_sweep()');
	sleep 0.05;
}
is($remaining_file_blocks, 0, 'janitor unlinks file-backed ledger blocks');
is($remaining_file_bytes, 0, 'janitor releases file admission bytes');

is(
	$node->safe_psql(
		'postgres',
		q{SET bioseq.query_cache_optimisation_mode = 'same';
		  SET bioseq.query_cache_ttl = 1;
		  SELECT min(id), max(id) FROM tier_rows}),
	'1|64',
	'short-TTL exact producer returns a nonempty result');
sleep 0.02;
$node->safe_psql(
	'postgres', 'SELECT bioseq_cache_test.request_janitor_sweep()');
is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM bioseq_cache.entries()'),
	'0',
	'expired exact registry slots converge to EMPTY diagnostics');

$node->stop;
done_testing();
