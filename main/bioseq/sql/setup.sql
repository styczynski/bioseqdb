drop extension if exists bioseq cascade;
create extension bioseq;

DROP DATABASE IF EXISTS bioseq;

drop table if exists x CASCADE;

create database bioseq;

SET bioseq.logging = on;

create table x ( a INT, seq TEXT, c INT ) using bioseq;
select create_distributed_table('x', 'a');

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


EXPLAIN (ANALYZE, BUFFERS) select sum(a) from x where c >= 2;
