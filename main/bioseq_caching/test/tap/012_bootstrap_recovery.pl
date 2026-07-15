use strict;
use warnings FATAL => 'all';

use FindBin;
use lib $FindBin::RealBin;
use BioseqCacheTest;
use Test::More;

sub start_bootstrap_node
{
	my ($name, $fault) = @_;
	my $node = new_cache_node($name, preload => 1);
	{
		local $ENV{BIOSEQ_CACHE_TEST_BOOTSTRAP_FAULT};
		if (defined $fault)
		{
			$ENV{BIOSEQ_CACHE_TEST_BOOTSTRAP_FAULT} = $fault;
		}
		else
		{
			delete $ENV{BIOSEQ_CACHE_TEST_BOOTSTRAP_FAULT};
		}
		$node->start;
	}
	bootstrap_cache($node);
	return $node;
}

sub status_sql
{
	my ($predicate) = @_;
	return qq{WITH q AS (
		SELECT bioseq_cache_test.bootstrap_status() AS s)
	  SELECT ($predicate) FROM q};
}

sub attach_twice_sql
{
	return q{WITH q AS (
		SELECT bioseq_cache_test.bootstrap_attach_twice() AS s)
	  SELECT (s->>'first_attached')::boolean
	     AND (s->>'second_attached')::boolean
	     AND (s->>'same_area')::boolean
	     AND (s->>'same_root')::boolean
	     AND (s->>'mapping_pinned')::boolean
	     AND (s->>'attempts_before')::bigint =
	         (s->>'attempts_after')::bigint
	     AND (s->>'attaches_after')::bigint -
	         (s->>'attaches_before')::bigint = 1
	  FROM q};
}

sub source_result
{
	my ($node, $table) = @_;
	return $node->safe_psql(
		'postgres',
		qq{SET bioseq.query_cache_optimisation_mode = 'same';
		   SELECT count(*), sum(id) FROM $table});
}

sub create_source_table
{
	my ($node, $table) = @_;
	$node->safe_psql(
		'postgres',
		qq{CREATE TABLE $table(id integer PRIMARY KEY, value text);
		   INSERT INTO $table VALUES
		     (10, 'ten'), (20, 'twenty'), (30, 'thirty')});
	return;
}

subtest 'one backend attaches twice without reinitializing' => sub {
	my $node = start_bootstrap_node('cache_bootstrap_idempotent');

	ok(
		$node->poll_query_until(
			'postgres',
			status_sql(q{(s->>'state')::integer = 2}),
			't'),
		'bootstrap reaches READY');
	is(
		$node->safe_psql('postgres', attach_twice_sql()),
		't',
		'repeated attachment reuses one DSA area and root');
	is(
		$node->safe_psql(
			'postgres',
			q{WITH a AS MATERIALIZED (
			    SELECT bioseq_cache_test.bootstrap_attach_twice() AS attached),
			  q AS (
			    SELECT bioseq_cache_test.bootstrap_status() AS s FROM a)
			  SELECT (s->>'state')::integer = 2
			     AND (s->>'boot_nonce')::bigint <> 0
			     AND (s->>'ready_handle_published')::boolean
			     AND (s->>'root_published')::boolean
			     AND (s->>'storage_attached')::boolean
			     AND (s->>'mapping_pinned')::boolean
			     AND (s->>'storage_metadata_in_dsa')::boolean
			     AND (s->>'reusable_wait_cvs_in_dsa')::boolean
			     AND (s->>'reusable_wait_cv_count')::integer > 0
			  FROM q}),
		't',
		'published storage metadata and wait CVs live in the pinned DSA');

	$node->stop;
	done_testing();
};

subtest 'dead prepublication initializer is cleaned and taken over once' => sub {
	my $node = start_bootstrap_node(
		'cache_bootstrap_takeover', 'pause_backend_once');
	my $initializer = $node->background_psql('postgres');
	$initializer->query_until(
		qr/bootstrap-initializer-start/,
		q{\echo bootstrap-initializer-start
		  SELECT bioseq_cache_test.bootstrap_attach_twice();
		  \echo bootstrap-initializer-finished
});

	ok(
		$node->poll_query_until(
			'postgres',
			status_sql(
				q{(s->>'state')::integer = 1
				  AND (s->>'pause_reached')::boolean
				  AND (s->>'defer_background_initializer')::boolean
				  AND (s->>'initializer_pid')::integer > 0
				  AND (s->>'initializer_nonce')::bigint > 0
				  AND (s->>'initializer_started_at')::bigint <> 0
				  AND (s->>'initializer_claim')::bigint > 0
				  AND (s->>'cleanup_handle_published')::boolean
				  AND (s->>'cleanup_handle_pinned')::integer = 1
				  AND (s->>'live_unpublished_areas')::integer = 1
				  AND NOT (s->>'ready_handle_published')::boolean
				  AND NOT (s->>'root_published')::boolean}),
			't'),
		'full owner token and pinned unpublished handle are visible');

	my $dead_token = $node->safe_psql(
		'postgres',
		q{WITH q AS (
		    SELECT bioseq_cache_test.bootstrap_status() AS s)
		  SELECT s->>'initializer_proc' || '|' ||
		         s->>'initializer_pid' || '|' ||
		         s->>'initializer_nonce' || '|' ||
		         s->>'initializer_started_at' || '|' ||
		         s->>'initializer_claim'
		  FROM q});
	my (undef, $initializer_pid) = split(/\|/, $dead_token);
	ok($initializer_pid > 0, 'paused initializer PID is captured');
	is(
		$node->safe_psql(
			'postgres',
			"SELECT pg_terminate_backend($initializer_pid)"),
		't',
		'paused initializer is terminated');
	eval { $initializer->quit; };

	ok(
		$node->poll_query_until(
			'postgres',
			status_sql(
				q{(s->>'state')::integer = 1
				  AND (s->>'live_unpublished_areas')::integer = 0
				  AND NOT (s->>'cleanup_handle_published')::boolean
				  AND (s->>'last_cleanup_handle')::integer > 0
				  AND (s->>'last_cleanup_was_pinned')::boolean
				  AND (s->>'last_cleanup_release_verified')::boolean}),
			't'),
		'exit cleanup removes the DSA pin and verifies handle release');

	is(
		$node->safe_psql(
			'postgres',
			q{WITH a AS MATERIALIZED (
			    SELECT bioseq_cache_test.bootstrap_attach_twice() AS a),
			  q AS (
			    SELECT a.a, bioseq_cache_test.bootstrap_status() AS s FROM a)
			  SELECT (a->>'first_attached')::boolean
			     AND (a->>'second_attached')::boolean
			     AND (a->>'same_area')::boolean
			     AND (a->>'same_root')::boolean
			     AND (a->>'mapping_pinned')::boolean
			     AND (a->>'attempts_before')::bigint = 1
			     AND (a->>'attempts_after')::bigint = 2
			     AND (a->>'attaches_after')::bigint -
			         (a->>'attaches_before')::bigint = 0
			     AND (s->>'state')::integer = 2
			     AND (s->>'takeovers')::bigint = 1
			     AND (s->>'takeover_used')::integer = 1
			     AND (s->>'initialization_attempts')::bigint = 2
			     AND (s->>'live_unpublished_areas')::integer = 0
			     AND (s->>'ready_handle_published')::boolean
			     AND (s->>'root_published')::boolean
			  FROM q}),
		't',
		'exactly one takeover publishes one reusable DSA root');

	my $new_token = $node->safe_psql(
		'postgres',
		q{WITH q AS (
		    SELECT bioseq_cache_test.bootstrap_status() AS s)
		  SELECT s->>'last_initializer_proc' || '|' ||
		         s->>'last_initializer_pid' || '|' ||
		         s->>'last_initializer_nonce' || '|' ||
		         s->>'last_initializer_started_at' || '|' ||
		         s->>'last_initializer_claim'
		  FROM q});
	isnt($new_token, $dead_token, 'takeover publishes a different full owner token');

	create_source_table($node, 'bootstrap_takeover_rows');
	$node->safe_psql('postgres', 'SELECT bioseq_cache.reset_stats()');
	is(
		source_result($node, 'bootstrap_takeover_rows'),
		'3|60',
		'first query after takeover returns the complete source result');
	is(
		source_result($node, 'bootstrap_takeover_rows'),
		'3|60',
		'second query after takeover returns the complete cached result');
	is(
		$node->safe_psql(
			'postgres',
			q{SELECT produced || '|' || completed_hits
			  FROM bioseq_cache.stats()}),
		'1|1',
		'cache produces once and completes one exact hit after recovery');

	$node->stop;
	done_testing();
};

subtest 'every DSA allocation phase fails cleanly and fails open' => sub {
	for my $ordinal (1 .. 5)
	{
		my $node = start_bootstrap_node(
			"cache_bootstrap_alloc_$ordinal", "alloc_$ordinal");
		my $table = "bootstrap_alloc_${ordinal}_rows";
		create_source_table($node, $table);
		is(
			source_result($node, $table),
			'3|60',
			"allocation phase $ordinal fails open to source execution");
		ok(
			$node->poll_query_until(
				'postgres',
				status_sql(q{(s->>'state')::integer = 3}),
				't'),
			"allocation phase $ordinal reaches FAILED");
		is(
			$node->safe_psql(
				'postgres',
				status_sql(
					q{(s->>'failure')::integer = 34
					  AND (s->>'initialization_attempts')::bigint = 1
					  AND (s->>'cleanups')::bigint = 1
					  AND (s->>'validation_failures')::bigint = 1
					  AND (s->>'live_unpublished_areas')::integer = 0
					  AND NOT (s->>'cleanup_handle_published')::boolean
					  AND NOT (s->>'ready_handle_published')::boolean
					  AND NOT (s->>'root_published')::boolean
					  AND (s->>'last_cleanup_handle')::integer > 0
					  AND (s->>'last_cleanup_was_pinned')::boolean
					  AND (s->>'last_cleanup_release_verified')::boolean})),
			't',
			"allocation phase $ordinal releases its unpublished DSA area");
		$node->stop;
	}
	done_testing();
};

subtest 'prepublication root manifest faults release the DSA and fail open' => sub {
	for my $case (
		[ 'root_magic', 36 ],
		[ 'root_abi',   35 ])
	{
		my ($fault, $failure) = @{$case};
		my $node = start_bootstrap_node(
			"cache_bootstrap_$fault", $fault);
		my $table = "bootstrap_${fault}_rows";
		create_source_table($node, $table);
		is(
			source_result($node, $table),
			'3|60',
			"$fault fails open to source execution");
		ok(
			$node->poll_query_until(
				'postgres',
				status_sql(q{(s->>'state')::integer = 3}),
				't'),
			"$fault reaches FAILED");
		is(
			$node->safe_psql(
				'postgres',
				status_sql(
					qq{(s->>'failure')::integer = $failure
					  AND (s->>'initialization_attempts')::bigint = 1
					  AND (s->>'cleanups')::bigint = 1
					  AND (s->>'validation_failures')::bigint = 1
					  AND (s->>'live_unpublished_areas')::integer = 0
					  AND NOT (s->>'cleanup_handle_published')::boolean
					  AND NOT (s->>'ready_handle_published')::boolean
					  AND NOT (s->>'root_published')::boolean
					  AND (s->>'last_cleanup_handle')::integer > 0
					  AND (s->>'last_cleanup_was_pinned')::boolean
					  AND (s->>'last_cleanup_release_verified')::boolean})),
			't',
			"$fault rejects publication and releases the exact DSA handle");
		$node->stop;
	}
	done_testing();
};

subtest 'published immutable-root corruption is boot-fatal and fail-open' => sub {
	for my $field (qw(magic abi boot_nonce))
	{
		my $node = start_bootstrap_node("cache_bootstrap_corrupt_$field");
		ok(
			$node->poll_query_until(
				'postgres',
				status_sql(q{(s->>'state')::integer = 2}),
				't'),
			"$field corruption node reaches READY");
		is(
			$node->safe_psql(
				'postgres',
				"SELECT bioseq_cache_test.corrupt_bootstrap_manifest('$field')"),
			't',
			"published $field corruption is rejected on reattach");
		is(
			$node->safe_psql(
				'postgres',
				status_sql(
					q{(s->>'state')::integer = 3
					  AND (s->>'failure')::integer = 26
					  AND (s->>'validation_failures')::bigint = 1
					  AND (s->>'ready_handle_published')::boolean
					  AND (s->>'root_published')::boolean
					  AND NOT (s->>'storage_attached')::boolean})),
			't',
			"published $field mismatch makes the boot globally FAILED");
		my $table = "bootstrap_corrupt_${field}_rows";
		create_source_table($node, $table);
		is(
			source_result($node, $table),
			'3|60',
			"queries fail open after published $field corruption");
		$node->stop;
	}
	done_testing();
};

done_testing();
