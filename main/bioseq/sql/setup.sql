drop extension if exists bioseq cascade;
create extension bioseq;

DROP DATABASE IF EXISTS bioseq;

drop table if exists x CASCADE;

create database bioseq;

SET bioseq.logging = on;
SET bioseq.enable_bioseq_index_scan TO TRUE;

create table x ( a INT PRIMARY KEY, seq TEXT, c INT ) using bioseq;
CREATE INDEX x_default_index ON x USING btree(c);

--select create_distributed_table('x', 'a', shard_count => 2);

-- select testtest();

INSERT INTO x VALUES (
    23, 'TEST1', 1
), (
    101, 'TEST2', 1
), (
    69, 'HE', 2
), (
    42, 'YO', 2
);

set bioseq.enable_column_cache to on;

--set bioseq.enable_custom_scan TO 'off';
--set enable_seqscan TO off;
--set seq_page_cost TO 10000000;

--EXPLAIN (ANALYZE, BUFFERS) select sum(a) from x where c >= 2;
-- EXPLAIN (ANALYZE, BUFFERS) select a, seq, mmsearch(seq, 'A') from x;
--select a, seq, mmsearch(seq, 'A') from x;



--EXPLAIN (ANALYZE, BUFFERS) select a from x where a > 100;
--EXPLAIN (VERBOSE) SELECT a FROM x WHERE c > 1 ORDER BY c LIMIT 2;
--EXPLAIN (VERBOSE) SELECT a, mmsearch(a, 'Y'), mmsearch(a, 'Z') from x;

EXPLAIN (ANALYZE, BUFFERS) select a, mmsearch(a, 'X') from x where c > 1;