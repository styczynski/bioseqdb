drop extension if exists bioseq cascade;
create extension bioseq;

DROP DATABASE IF EXISTS bioseq;

create database bioseq;

SET columnar.logging = on;

create table x ( a INT, seq TEXT ) using columnar;

-- INSERT INTO x VALUES (
--     23, 'TEST1'
-- ), (
--     101, 'TEST2'
-- );

-- SET parallel_setup_cost = 0;
-- SET parallel_tuple_cost = 0;
-- SET min_parallel_table_scan_size = 0;
-- SET min_parallel_index_scan_size = 0;
-- SET max_worker_processes = 6;
-- SET max_parallel_workers_per_gather = 3;
-- SET max_parallel_workers = 6;
-- SET debug_parallel_query = on;

-- EXPLAIN (ANALYZE, BUFFERS) select * from x where mmsearch(a, 'A') and a > 0;

-- --select * from x where mmsearch(a, 'A');