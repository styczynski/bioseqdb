use strict;
use warnings FATAL => 'all';

use File::Path qw(make_path);
use File::Spec;
use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;

sub cache_files
{
	my ($store_root, $boot_nonce, $database_oid) = @_;
	my $database_dir = File::Spec->catdir(
		$store_root, $boot_nonce, $database_oid);
	return sort grep {
		-f $_ && /[0-9a-f]{64}\.[0-9a-f]{16}\.cache$/
	} glob(File::Spec->catfile($database_dir, '*.cache'));
}

sub storage_boot_nonce
{
	my ($node) = @_;
	return $node->safe_psql(
		'postgres',
		q{SELECT lpad(to_hex(
		    (bioseq_cache_test.bootstrap_status()->>'boot_nonce')::bigint),
		    16, '0')});
}

my $node = new_cache_node('cache_postmaster_restart', preload => 1);
$node->start;
bootstrap_cache($node);
$node->safe_psql(
	'postgres',
	q{CREATE TABLE restart_rows(id integer PRIMARY KEY, value text);
	  INSERT INTO restart_rows VALUES (1, 'one'), (2, 'two');
	  SELECT bioseq_cache_test.set_force_file_tier(true);
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

my $store_root = File::Spec->catdir(
	$node->data_dir, 'pg_bioseq_cache');
my $old_boot_nonce = storage_boot_nonce($node);
my $database_oid = $node->safe_psql(
	'postgres', 'SELECT oid FROM pg_database WHERE datname = current_database()');
my $old_database_dir = File::Spec->catdir(
	$store_root, $old_boot_nonce, $database_oid);
my @old_files = cache_files($store_root, $old_boot_nonce, $database_oid);
cmp_ok(scalar(@old_files), '>=', 1, 'completed entry owns a file-tier block');

make_path($old_database_dir, { mode => 0700 });
my $partial_path = File::Spec->catfile(
	$old_database_dir,
	'.stage-00000000-00000000-0000000000000000-'
	  . '0000000000000000-ffffffffffffffff.partial');
open(my $partial, '>', $partial_path) or die "open $partial_path: $!";
print {$partial} "incomplete pre-restart cache block\n";
close($partial) or die "close $partial_path: $!";
ok(-f $partial_path, 'synthetic partial file exists before shutdown');

ok($node->stop('smart'), 'postmaster accepts graceful SIGTERM shutdown');
$node->start;

is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM bioseq_cache.entries()'),
	'0',
	'boot-scoped registry is empty after restart');
ok(!-e $partial_path, 'startup purges the prior-boot partial file');
ok(
	(!grep { -e $_ } @old_files),
	'startup purges every prior-boot complete cache file');

$node->safe_psql(
	'postgres', 'SELECT bioseq_cache_test.set_force_file_tier(true)');
is(
	$node->safe_psql(
	'postgres',
	q{SET bioseq.query_cache_optimisation_mode = 'same';
	  SELECT id, value FROM restart_rows ORDER BY id;}),
	"1|one\n2|two",
	'first post-restart execution returns the complete source result');
is(
	$node->safe_psql(
		'postgres',
		q{SELECT produced || '|' || completed_hits
		  FROM bioseq_cache.stats()}),
	'1|0',
	'first post-restart execution is a fresh miss, never a stale hit');

my $new_boot_nonce = storage_boot_nonce($node);
my @new_files = cache_files($store_root, $new_boot_nonce, $database_oid);
cmp_ok(scalar(@new_files), '>=', 1, 'new boot publishes a fresh file block');
isnt(
	$new_boot_nonce,
	$old_boot_nonce,
	'graceful restart assigns a different storage boot nonce');

$node->stop;
done_testing();
