drop extension bioseq cascade;
create extension bioseq;
create table x(a int) using mem;
create table y(a int) using mem;
INSERT INTO x VALUES (23), (101);
select a from x;
select a from x where a = 23;
select a from y;


CREATE EXTENSION IF NOT EXISTS columnar;

CREATE TABLE simple_columnar(i INT8) USING columnar;

INSERT INTO simple_columnar SELECT generate_series(1,10);

EXPLAIN (ANALYZE, BUFFERS) select * from simple_columnar where mmsearch(i, 'A');

