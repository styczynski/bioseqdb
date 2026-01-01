drop extension if exists bioseq cascade;
create extension bioseq;

DROP DATABASE IF EXISTS bioseq WITH FORCE;

create database bioseq;

create table x(a int) using columnar;

INSERT INTO x VALUES (23), (101);

-- EXPLAIN (ANALYZE, BUFFERS) select * from x where mmsearch(a, 'A');