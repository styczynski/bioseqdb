package BioseqCacheTest;

use strict;
use warnings FATAL => 'all';

use Exporter qw(import);
use PostgreSQL::Test::Cluster;
use PostgreSQL::Test::Utils qw(slurp_file);

our @EXPORT = qw(
  bootstrap_cache
  cache_bootstrap_path
  cache_dso_path
  cache_libdir
  new_cache_node
  sql_literal
);

sub _required_env
{
	my ($name) = @_;
	die "$name is required\n"
	  unless defined($ENV{$name}) && length($ENV{$name}) > 0;
	return $ENV{$name};
}

sub cache_dso_path
{
	return _required_env('BIOSEQ_CACHE_DSO');
}

sub cache_libdir
{
	return _required_env('BIOSEQ_CACHE_LIBDIR');
}

sub cache_bootstrap_path
{
	return _required_env('BIOSEQ_CACHE_BOOTSTRAP_SQL');
}

sub sql_literal
{
	my ($value) = @_;
	$value =~ s/'/''/g;
	return "'$value'";
}

sub _conf_literal
{
	my ($value) = @_;
	die "configuration value contains newline\n" if $value =~ /[\r\n]/;
	$value =~ s/'/''/g;
	return "'$value'";
}

sub new_cache_node
{
	my ($name, %options) = @_;
	my $node = PostgreSQL::Test::Cluster->new($name . '_' . $$);
	$node->init;

	my $configuration =
	    "listen_addresses = ''\n"
	  . 'dynamic_library_path = '
	  . _conf_literal(cache_libdir() . ':$libdir') . "\n"
	  . "logging_collector = on\n"
	  . "log_min_messages = info\n"
	  . "max_worker_processes = 16\n"
	  . "max_prepared_transactions = 16\n";

	if ($options{preload})
	{
		$configuration .= "shared_preload_libraries = 'bioseq_cache'\n";
	}
	if (defined $options{mode})
	{
		$configuration .=
		  'bioseq.query_cache_optimisation_mode = '
		  . _conf_literal($options{mode}) . "\n";
	}
	$configuration .= $options{extra_conf}
	  if defined $options{extra_conf};
	$node->append_conf('postgresql.conf', $configuration);
	return $node;
}

sub bootstrap_cache
{
	my ($node, $database) = @_;
	$database //= 'postgres';
	$node->safe_psql($database, slurp_file(cache_bootstrap_path()));
	return;
}

1;
