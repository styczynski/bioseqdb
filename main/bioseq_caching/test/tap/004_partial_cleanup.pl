use strict;
use warnings FATAL => 'all';

use File::Path qw(make_path);
use File::Spec;
use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;
use Time::HiRes qw(sleep time);

my $node = new_cache_node('cache_partial_cleanup', preload => 1);
$node->start;
bootstrap_cache($node);

my $store_dir = File::Spec->catdir(
	$node->data_dir, 'bioseq_cache', 'storage-v1');
make_path($store_dir, { mode => 0700 });
my $partial_path = File::Spec->catfile(
	$store_dir, '.block-0000000000000000-0000000000000000-0.tmp');
open(my $partial, '>', $partial_path) or die "open $partial_path: $!";
print {$partial} "not a complete cache manifest\n";
close($partial) or die "close $partial_path: $!";
ok(-f $partial_path, 'synthetic partial file exists before restart');

$node->restart;

my $deadline = time() + 10;
while (-e $partial_path && time() < $deadline)
{
	sleep(0.05);
}
ok(!-e $partial_path, 'startup orphan cleanup removes partial file');
is(
	$node->safe_psql('postgres', 'SELECT count(*) FROM bioseq_cache.entries()'),
	'0',
	'partial file never becomes a readable registry entry');

$node->stop;
done_testing();
