use strict;
use warnings FATAL => 'all';

use File::Basename qw(basename);
use File::Spec;
use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;
use Time::HiRes qw(sleep time);

sub cache_files
{
	my ($store_dir) = @_;
	return sort grep { -f $_ }
	  glob(File::Spec->catfile($store_dir, 'block-*.cache'));
}

sub file_boot_nonce
{
	my ($path) = @_;
	my ($nonce) = basename($path) =~ /^block-([0-9a-f]{16})-[0-9a-f]{16}\.cache$/;
	die "unexpected cache filename: $path\n" unless defined $nonce;
	return $nonce;
}

my $node = new_cache_node(
	'cache_owner_liveness',
	preload => 1,
	extra_conf => "restart_after_crash = on\n");
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE owner_rows(id integer PRIMARY KEY, value text);
	  INSERT INTO owner_rows
	  SELECT i, md5(i::text) FROM generate_series(1, 64) AS g(i);
	  SELECT bioseq_cache_test.set_force_file_tier(true);});

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
		  SELECT count(*), sum(id) FROM owner_rows}),
	'64|2080',
	'fresh producer succeeds after owner cleanup');

my $store_dir = File::Spec->catdir(
	$node->data_dir, 'bioseq_cache', 'storage-v1');
my @old_files = cache_files($store_dir);
cmp_ok(
	scalar(@old_files),
	'>=',
	1,
	'pre-crash generation owns a file named with its boot nonce');
my $old_boot_nonce = file_boot_nonce($old_files[0]);

my $reader_controller = $node->background_psql('postgres');
$reader_controller->query_until(
	qr/reader-controller-armed/,
	q{SELECT bioseq_cache_test.arm_pause('after_reader_pinned');
	  \echo reader-controller-armed
});

my $reader = $node->background_psql('postgres');
$reader->query_until(
	qr/reader-start/,
	q{\echo reader-start
	  SET application_name = 'cache_dead_reader';
	  SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT count(*), sum(id) FROM owner_rows;
});
ok(
	$node->poll_query_until(
		'postgres',
		q{SELECT EXISTS (
		    SELECT 1 FROM bioseq_cache_test.entry_states()
		    WHERE state = 'COMPLETE' AND reader_count = 1)
		  AND (SELECT reader_pins = 1
		       FROM bioseq_cache_test.storage_tier_stats())},
		't'),
	'exact and storage reader ownership is visible before backend death');

my $reader_pid = $node->safe_psql(
	'postgres',
	q{SELECT pid FROM pg_stat_activity
	  WHERE application_name = 'cache_dead_reader'});
is(kill('KILL', int($reader_pid)), 1, 'reader backend is killed without cleanup');
eval { $reader->quit; };
eval { $reader_controller->quit; };

ok(
	$node->poll_query_until('postgres', 'SELECT 1', '1'),
	'postmaster recovers after the reader backend crash');
$node->safe_psql(
	'postgres',
	q{SELECT bioseq_cache_test.set_force_file_tier(true);
	  SELECT bioseq_cache.reset_stats();});
is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM bioseq_cache.entries()'),
	'0',
	'SIGKILL recovery discards the old boot registry');
ok(
	(!grep { -e $_ } @old_files),
	'SIGKILL recovery purges every prior-boot cache file');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT reader_pins FROM bioseq_cache_test.storage_tier_stats()}),
	'0',
	'new boot starts without the killed reader pin');

is(
	$node->safe_psql(
		'postgres',
		q{SET bioseq.query_cache_optimisation_mode = 'same';
		  SELECT count(*), sum(id) FROM owner_rows}),
	'64|2080',
	'first query after SIGKILL recovery returns the complete source result');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT produced || '|' || completed_hits
		  FROM bioseq_cache.stats()}),
	'1|0',
	'first post-crash query is a fresh miss rather than a stale hit');
my @new_files = cache_files($store_dir);
cmp_ok(scalar(@new_files), '>=', 1, 'new boot publishes a fresh file block');
isnt(
	file_boot_nonce($new_files[0]),
	$old_boot_nonce,
	'SIGKILL recovery assigns a different storage boot nonce');

$node->safe_psql(
	'postgres',
	q{SELECT bioseq_cache_test.set_force_file_tier(false);
	  SELECT bioseq_cache.invalidate_all();
	  SELECT bioseq_cache_test.request_janitor_sweep();});

my $cleanup_deadline = time() + 10;
my $reader_cleanup = '';
while (time() < $cleanup_deadline)
{
	$reader_cleanup = $node->safe_psql(
		'postgres',
		q{SELECT (NOT EXISTS (
		     SELECT 1 FROM bioseq_cache_test.entry_states()))::integer
		   || '|' ||
		   (SELECT (reader_pins = 0)::integer
		    FROM bioseq_cache_test.storage_tier_stats())});
	last if $reader_cleanup eq '1|1';
	$node->safe_psql(
		'postgres', 'SELECT bioseq_cache_test.request_janitor_sweep()');
	sleep(0.05);
}
ok(
	$reader_cleanup eq '1|1',
	'dead exact and storage reader owners are reaped after invalidation');

$node->stop;
done_testing();
