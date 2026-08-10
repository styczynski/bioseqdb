--EXPLAIN (ANALYZE, BUFFERS) select a, seq from x where c >= 2;

-- select
--     a, seq,
--     mmsearch(a, 'A')+1 AS heheszki,
--     mmsearch(a, 'A') AS lol
-- from x;

-- select
--     a, seq,
--     mmsearch(a, 'A')+1 AS heheszki,
--     mmsearch(a, 'A') AS lol
-- from x; --where mmsearch(a, 'A');

-- select
--     a, seq,
--     mmsearch(a, 'A')+1 AS heheszki,
--     mmsearch(a, 'A') AS lol
-- from x;

-- select
--     a, seq,
--     mmsearch(a, 'A')+1 AS heheszki,
--     mmsearch(a, 'A') AS lol
-- from x;

-- EXPLAIN (ANALYZE, BUFFERS) select a, seq, mmsearch(a, 'A') from x; -- where mmsearch(a, 'A');

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